// VEGAS's Noise Gate DirectX effect (sfppack2_x64.dll, CLSID b97c0f23-196d-11d1-b99b-00a0c9053912): its saved settings,
// presets and its processing, worked out from the DLL's own code. Reno's built-in Noise Gate uses the same settings
// and saved form, so a project's VEGAS Noise Gate loads into it and it saves back as VEGAS Noise Gate.
//
// The sound is a block gate: 20 ms of the sound are collected, the loudest sample of the block (over both channels)
// drives a small state machine (closed, opening, open, closing), and the block is output with its gain moved from the
// last block's gain towards the state's target, in a ramp or in 1/1000 steps. The output is the input of the same
// positions, complete blocks at a time, so a user of it has to delay by one block (latency()).
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegng {

const char* const kClsid = "b97c0f23-196d-11d1-b99b-00a0c9053912";
const char* const kDxUid = "dshow:{B97C0F23-196D-11D1-B99B-00A0C9053912}";

// The plug-in's own layout (what it saves, after its length and a GUID).
struct Settings {
    int32_t attack = 3;         // ms, 1..500: how long the level must stay above the threshold before the gate is open
    int32_t release = 100;      // ms, 1..5000
    int32_t threshold = -200;   // tenths of a dB, -800 (-Inf.) .. 0
};
static_assert(sizeof(Settings) == 12, "VEGAS Noise Gate's settings are 12 bytes");

// The GUID VEGAS Noise Gate writes before its settings; it reads this one and b97c0f26-196d-11d1-b99b-00a0c9053912.
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0x43, 0x8f, 0xdf, 0xa6, 0xf4, 0xba, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
    return g;
}
inline const uint8_t* persistGuidOld()
{
    static const uint8_t g[16] = {0x26, 0x0f, 0x7c, 0xb9, 0x6d, 0x19, 0xd1, 0x11, 0xb9, 0x9b, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
    return g;
}

inline void sanitize(Settings& s)
{
    s.attack = std::max(1, std::min(500, (int)s.attack));
    s.release = std::max(1, std::min(5000, (int)s.release));
    s.threshold = std::max(-800, std::min(0, (int)s.threshold));
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x20), the GUID, the settings.
inline std::string encode(const Settings& s)
{
    std::string b(4 + 16 + sizeof(Settings), '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], persistGuid(), 16);
    std::memcpy(&b[20], &s, sizeof(Settings));
    return "S:" + base64Encode(b.data(), b.size());
}

inline bool decode(const std::string& st, Settings* out)
{
    if (st.size() < 3 || st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 20 + sizeof(Settings)) return false;
    if (std::memcmp(b.data() + 4, persistGuid(), 16) != 0 && std::memcmp(b.data() + 4, persistGuidOld(), 16) != 0) return false;
    Settings s;
    std::memcpy(&s, b.data() + 20, sizeof(Settings));
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    const char* name;
    Settings s;
};

// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", {3, 100, -200}},
        {"Default all parameters", {3, 100, -200}},
        {"Strong beats only", {1, 300, -60}},
        {"Long decay time", {10, 1000, -380}},
        {"Noise gate 1", {10, 500, -600}},
        {"Noise gate 2", {1, 500, -320}},
        {"Slow attack", {300, 1000, -400}},
    };
    return list;
}

// "-20.0 dB" / "-Inf." as the page's threshold label
inline std::string thresholdText(int tenths)
{
    if (tenths <= -800) return "-Inf.";
    char b[32];
    std::snprintf(b, sizeof b, "%.1f dB", tenths / 10.0);
    return b;
}

// The processing, in the plug-in's own order of operations and its own arithmetic (double inside, float samples).
class Dsp {
public:
    // The format is set once (the plug-in's set-media-type): rate and channel count (1..2 here; the DLL takes more).
    void setFormat(double rate, int channels)
    {
        rate_ = (uint32_t)(rate + 0.5);
        ch_ = std::max(1, std::min(kMaxCh, channels));
        block_ = (int)((double)rate_ * 0.02);
        cap_ = (int)((double)rate_ * 0.03);
        hold_ = (int)((double)rate_ * 0.004);
        if (block_ < 1) block_ = 1;
        if (cap_ < block_) cap_ = block_;
        for (int c = 0; c < kMaxCh; ++c) {
            in_[c].assign((size_t)cap_, 0.f);
            out_[c].assign((size_t)cap_, 0.f);
        }
        prepare();
        for (double& p : peak_) p = 0;
        state_ = 0;
        fill_ = outPos_ = 0;
        pending_ = false;
        gain_ = 0;
        c428_ = c42c_ = 0;
        ready_ = true;
    }
    void setSettings(const Settings& s)
    {
        Settings t = s;
        sanitize(t);
        if (std::memcmp(&t, &set_, sizeof t) != 0 || !prepared_) {
            set_ = t;
            dirty_ = true;
        }
    }
    int block() const { return block_; }
    int latency() const { return block_; }

    // The start of a stream, or after a jump in time (a discontinuity): the settings are taken again and the gate
    // starts closed. The peaks of a block that was being collected stay, as in the plug-in.
    void restart()
    {
        prepare();
        fill_ = 0;
        outPos_ = 0;
        pending_ = false;
        gain_ = 0;
        state_ = 0;
        cap_ = std::max(cap_, block_);
        curBlock_ = block_;
    }

    // Takes the settings that were changed since the last call (the plug-in does this at each buffer it gets).
    void applyChanges()
    {
        if (dirty_ || !prepared_) prepare();
    }

    // One frame in. Returns true once a block is complete and processed: its samples are then in out(c, 0..n-1),
    // to be taken with outFrame(); the next push starts the next block.
    bool push(const float* frame)
    {
        if (!ready_) return false;
        for (int c = 0; c < ch_; ++c) {
            const double x = (double)frame[c];
            in_[c][(size_t)fill_] = (float)x;
            if (x > peak_[c]) peak_[c] = x;
            else if (-x > peak_[c]) peak_[c] = -x;
        }
        ++fill_;
        if (fill_ >= block_) {
            processBlock();
            return true;
        }
        return false;
    }
    int blockFrames() const { return outN_; }
    float outAt(int c, int i) const { return out_[c][(size_t)i]; }

private:
    static constexpr int kMaxCh = 2;
    uint32_t rate_ = 48000;
    int ch_ = 2;
    int block_ = 960, cap_ = 1440, hold_ = 192, curBlock_ = 960;
    int attackS_ = 0, releaseS_ = 0;
    double thrA_ = 0, thrB_ = 0;
    double coefA_ = 0, coefB_ = 0, coefC_ = 0;
    double gain_ = 0;
    int state_ = 0, c428_ = 0, c42c_ = 0;
    int fill_ = 0, outPos_ = 0, outN_ = 0;
    bool pending_ = false, ready_ = false, prepared_ = false, dirty_ = false;
    Settings set_;
    std::vector<float> in_[kMaxCh], out_[kMaxCh];
    double peak_[kMaxCh] = {0, 0};

    // the plug-in's prepare: times in samples, threshold levels and the three smoothing steps
    void prepare()
    {
        attackS_ = (int)((double)set_.attack / 1000.0 * (double)rate_);
        releaseS_ = (int)((double)set_.release / 1000.0 * (double)rate_);
        if (set_.threshold == -800) thrA_ = bitsToDouble(0x3effffd845847726ull);
        else thrA_ = std::pow(10.0, (double)set_.threshold / 200.0);
        thrB_ = thrA_ * 0.85;
        double pk = 0;
        for (int c = 0; c < ch_; ++c)
            if (peak_[c] > pk) pk = peak_[c];
        if ((thrA_ >= pk && pk >= thrB_) || (pk >= thrA_ && thrB_ >= pk)) state_ = 0;
        coefA_ = 1.0 - std::pow(0.1, 1.0 / ((double)set_.release / 20.0));
        coefB_ = 1.0 - std::pow(0.1, 1.0 / ((double)set_.attack / 20.0));
        coefC_ = 1.0 - std::pow(0.1, 1.3333333333333333);
        prepared_ = true;
        dirty_ = false;
    }
    static double bitsToDouble(uint64_t b)
    {
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    }

    void processBlock()
    {
        const int n = fill_;
        double pk = 0;
        for (int c = 0; c < ch_; ++c)
            if (peak_[c] > pk) pk = peak_[c];
        switch (state_) {
        case 0:
            if (pk >= thrA_) {
                state_ = 2;
                c428_ = n;
            }
            break;
        case 2:
            c428_ += n;
            if (thrB_ >= pk) state_ = 0;
            else if (c428_ >= attackS_) state_ = 3;
            break;
        case 3:
            if (thrB_ >= pk) {
                state_ = 4;
                c42c_ = n;
            }
            break;
        case 4:
            c42c_ += n;
            if (pk >= thrA_) state_ = 3;
            else if (c42c_ >= hold_) state_ = 0;
            break;
        default:
            break;
        }
        for (int c = 0; c < ch_; ++c) apply(in_[c].data(), out_[c].data(), n);
        outN_ = n;
        for (double& p : peak_) p = 0;
        fill_ = 0;
    }

    // The gain stage for one channel of a block (the float version): the gain moves from the last gain towards the
    // state's target (1, or 0 when closed), by a ramp over the attack or release time and then steady. The gain is one
    // value for the whole filter: the second channel starts from where the first one ended, as in the plug-in.
    void apply(const float* in, float* out, int n)
    {
        double g;
        int ramp;
        switch (state_) {
        case 0:
            g = coefA_ * 0.0 + (1.0 - coefA_) * gain_;
            ramp = releaseS_;
            break;
        case 2:
            g = (1.0 - coefB_) * gain_ + coefB_ * 1.0;
            ramp = attackS_;
            break;
        case 4:
            g = (1.0 - coefA_) * gain_ + coefA_ * 1.0;
            ramp = releaseS_;
            break;
        default:
            g = (1.0 - coefC_) * gain_ + coefC_ * 1.0;
            ramp = attackS_;
            break;
        }
        if (g > 1.0) g = 1.0;
        const double g0 = gain_;
        gain_ = g;
        double g1 = g;
        if (0.0002 > g1) {
            g1 = 0.0;
            gain_ = 0.0;
        }
        rampBlock(in, out, n, g0, g1, ramp);
    }

    static void rampBlock(const float* in, float* out, int n, double g0, double g1, int r12)
    {
        const int step = (g1 > g0) ? 1 : -1;
        const int fixedGain = (int)(g1 * 1000.0);
        const double diff = g0 - g1;
        auto steady = [&](int from) {
            for (int i = from; i < n; ++i) out[i] = (float)((double)in[i] * (double)fixedGain) / 1000.0f;
        };
        if (0.0005 > std::fabs(diff)) {
            steady(0);
            return;
        }
        const int len = r12 > n ? n : r12;
        const double span = g1 - g0;
        const int D = toInt((double)len / std::fabs(span));
        const bool slope = r12 > n ? D < 0 : D < 1;
        if (slope) {
            const double inc = len ? span / (double)len : 0.0;
            double cur = g0;
            for (int i = 0; i < len; ++i) {
                out[i] = (float)((double)in[i] * cur);
                cur += inc;
            }
        } else {
            int v = toInt((double)D * g0);
            const float fd = (float)D;
            for (int i = 0; i < len; ++i) {
                out[i] = ((float)v * in[i]) / fd;
                v += step;
            }
        }
        if (len < n) steady(len);
    }
    // cvttsd2si: truncation; out of range gives the "integer indefinite" value
    static int toInt(double x)
    {
        if (!(x > -2147483649.0 && x < 2147483648.0)) return (int)0x80000000u;
        return (int)x;
    }
};

}  // namespace vegng
}  // namespace reno
