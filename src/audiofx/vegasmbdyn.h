// VEGAS's Multi-Band Dynamics DirectX effect (sfppack2_x64.dll, CLSID 026d0aa0-9bb9-11d0-aebc-00a0c9053912): its saved
// settings, presets and its processing, worked out from the DLL's own code. Reno's built-in Multi-Band Dynamics uses the
// same settings and saved form, so a project's VEGAS Multi-Band Dynamics loads into it and it saves back as VEGAS
// Multi-Band Dynamics.
//
// The sound is cut into 20 ms blocks (the same block engine as Graphic Dynamics: a block is collected, processed and
// handed out a block late). Up to four bands work one after the other on the block: a band filters the block (a low
// shelf, a band-pass or a high shelf, found by the same SfSig routines as the Paragraphic EQ), the loudest sample of the
// filtered block is the band's level, and the gain a compressor makes of that level (the amount of compression, the
// threshold, the attack and release steps from one block to the next) is applied to the band: the filtered sound times
// (1 - gain) is taken off the block. A band's own gain (a boost or a cut at its frequency) comes after.
#pragma once
#include "../core/base.h"
#include "vegassig.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegmb {

const char* const kClsid = "026d0aa0-9bb9-11d0-aebc-00a0c9053912";
const char* const kDxUid = "dshow:{026D0AA0-9BB9-11D0-AEBC-00A0C9053912}";

const int kBands = 4;
const int kInts = 53;

// The plug-in's own layout (what it saves, after its length and a GUID): 0xd4 bytes, 53 integers. Levels and gains are in
// tenths of a dB, the times in tenths of a ms, the amount in tenths (of ":1"), the width in tenths of an octave.
struct Settings {
    int32_t outGain = 0;                                // 0: -250..250
    int32_t threshold[kBands] = {0, 0, 0, 0};           // 1..4: -600..0
    int32_t type[kBands] = {1, 1, 1, 1};                // 5..8: 0 low shelf, 1 band, 2 high shelf
    int32_t amount[kBands] = {10, 10, 10, 10};          // 9..12: 10 (1.0:1) .. 500 (50:1)
    int32_t attack[kBands] = {10, 10, 10, 10};          // 13..16: 0..5000
    int32_t release[kBands] = {1000, 1000, 1000, 1000}; // 17..20: 0..50000
    int32_t gain[kBands] = {0, 0, 0, 0};                // 21..24: -250..250
    int32_t width[kBands] = {10, 10, 10, 10};           // 25..28: 3..30
    int32_t freq[kBands] = {200, 1000, 4000, 10000};    // 29..32: 50..15000 Hz
    int32_t sync = 1;                                   // 33: "Sync channels"
    int32_t bypass[kBands] = {0, 1, 1, 1};              // 34..37: band bypass
    int32_t extra[10] = {128, 128, 128, 128, 16, 16, 16, 16, 1, 0};  // 38..47: what the plug-in's window keeps (kept as it is)
    int32_t solo[kBands] = {0, 0, 0, 0};                // 48..51
    int32_t bypassAll = 0;                              // 52

    bool operator==(const Settings& o) const { return std::memcmp(this, &o, sizeof(Settings)) == 0; }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
static_assert(sizeof(Settings) == 0xd4, "VEGAS Multi-Band Dynamics's settings are 0xd4 bytes");

// The GUID VEGAS Multi-Band Dynamics writes before its settings (length 0xe8, the same one as Graphic Dynamics); it also
// reads the older form {026d0aa3-9bb9-11d0-aebc-00a0c9053912} with length 0xe4 (the settings without the last field).
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0xa2, 0x33, 0x71, 0x5f, 0x34, 0x9a, 0x2e, 0x4e, 0xa7, 0xbd, 0xb6, 0x2a, 0x4a, 0x9d, 0xd5, 0xf5};
    return g;
}
inline const uint8_t* persistGuidOld()
{
    static const uint8_t g[16] = {0xa3, 0x0a, 0x6d, 0x02, 0xb9, 0x9b, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
    return g;
}

inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The ranges of the plug-in's parameters (its setters keep them; its loading does not)
inline void sanitize(Settings& s)
{
    s.outGain = clampi(s.outGain, -250, 250);
    for (int b = 0; b < kBands; ++b) {
        s.threshold[b] = clampi(s.threshold[b], -600, 0);
        s.type[b] = clampi(s.type[b], 0, 2);
        s.amount[b] = clampi(s.amount[b], 10, 500);
        s.attack[b] = clampi(s.attack[b], 0, 5000);
        s.release[b] = clampi(s.release[b], 0, 50000);
        s.gain[b] = clampi(s.gain[b], -250, 250);
        s.width[b] = clampi(s.width[b], 3, 30);
        s.freq[b] = clampi(s.freq[b], 50, 15000);
        s.bypass[b] = s.bypass[b] ? 1 : 0;
        s.solo[b] = s.solo[b] ? 1 : 0;
    }
    s.sync = s.sync ? 1 : 0;
    s.bypassAll = s.bypassAll ? 1 : 0;
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0xe8), the GUID, the settings
inline std::string encode(const Settings& in)
{
    Settings s = in;
    sanitize(s);
    std::string b(4 + 16 + sizeof(Settings), '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], persistGuid(), 16);
    std::memcpy(&b[20], &s, sizeof(Settings));
    return "S:" + base64Encode(b.data(), b.size());
}

inline bool decode(const std::string& st, Settings* out)
{
    if (st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 20) return false;
    uint32_t len;
    std::memcpy(&len, b.data(), 4);
    Settings s;
    if (std::memcmp(b.data() + 4, persistGuid(), 16) == 0 && len == 20 + sizeof(Settings) && b.size() >= len) {
        std::memcpy(static_cast<void*>(&s), b.data() + 20, sizeof(Settings));
    } else if (std::memcmp(b.data() + 4, persistGuidOld(), 16) == 0 && len == 20 + sizeof(Settings) - 4 && b.size() >= len) {
        std::memcpy(static_cast<void*>(&s), b.data() + 20, sizeof(Settings) - 4);
        s.bypassAll = 0;
    } else {
        return false;
    }
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    std::string name;
    Settings s;
};

// VEGAS's own presets, in its order (as the plug-in has them: 53 integers each)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = [] {
        struct Raw {
            const char* name;
            int32_t v[kInts];
        };
        static const Raw raw[] = {
            {"(Default)", {0, 0, 0, 0, 0, 1, 1, 1, 1, 10, 10, 10, 10, 10, 10, 10, 10, 1000, 1000, 1000, 1000, 0, 0, 0, 0, 10, 10, 10, 10, 200, 1000, 4000, 10000, 1, 0, 1, 1, 1, 128, 128, 128, 128, 16, 16, 16, 16, 1, 0, 0, 0, 0, 0, 0}},
            {"Default all parameters", {0, 0, 0, 0, 0, 1, 1, 1, 1, 10, 10, 10, 10, 10, 10, 10, 10, 1000, 1000, 1000, 1000, 0, 0, 0, 0, 10, 10, 10, 10, 200, 1000, 4000, 10000, 1, 0, 1, 1, 1, 128, 128, 128, 128, 16, 16, 16, 16, 1, 0, 0, 0, 0, 0, 0}},
            {"Anti-DTMF talk-off", {0, -250, -250, -250, -250, 1, 1, 1, 1, 300, 300, 300, 300, 10, 10, 10, 10, 1000, 1000, 1000, 1000, 0, 0, 0, 0, 7, 7, 7, 7, 1209, 1336, 1477, 1633, 1, 0, 0, 0, 0, 128, 128, 128, 128, 16, 16, 16, 16, 1, 0, 0, 0, 0, 0, 0}},
            {"Reduce loud plosives", {0, -150, 0, 0, 0, 0, 0, 0, 0, 320, 0, 0, 0, 10, 0, 0, 0, 1000, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 500, 0, 0, 0, 1, 0, 1, 1, 1, 128, 128, 128, 128, 16, 16, 16, 16, 1, 0, 0, 0, 0, 0, 0}},
            {"Reduce loud sibilants (de-esser)", {0, -300, 0, 0, 0, 1, 0, 0, 0, 250, 0, 0, 0, 10, 0, 0, 0, 1000, 0, 0, 0, 20, 0, 0, 0, 10, 0, 0, 0, 5000, 0, 0, 0, 1, 0, 1, 1, 1, 128, 128, 128, 128, 16, 16, 16, 16, 1, 0, 0, 0, 0, 0, 0}},
            {"Reduce plosives and sibilants", {0, -150, -320, 0, 0, 0, 1, 0, 0, 400, 200, 0, 0, 10, 10, 0, 0, 1000, 1000, 0, 0, 0, 20, 0, 0, 20, 10, 0, 0, 600, 5000, 0, 0, 1, 0, 0, 1, 1, 128, 128, 128, 128, 16, 16, 16, 16, 1, 0, 0, 0, 0, 0, 0}},
        };
        std::vector<Preset> v;
        for (const Raw& r : raw) {
            Preset p;
            p.name = r.name;
            std::memcpy(static_cast<void*>(&p.s), r.v, sizeof p.s);
            v.push_back(p);
        }
        return v;
    }();
    return list;
}

// -------------------------------------------------------------------------------------------------------- the sound
// cvttsd2si: truncation; out of range gives the "integer indefinite" value
inline int toInt(double x)
{
    if (!(x > -2147483649.0 && x < 2147483648.0)) return (int)0x80000000u;
    return (int)x;
}

// the plug-in's way of keeping a recursive filter's state from going denormal: below 2^-32 it is zero
inline double flush(double v)
{
    uint64_t b;
    std::memcpy(&b, &v, 8);
    if ((uint32_t)((b >> 32) & 0x7ff00000u) < 0x3df00000u) b = 0;
    std::memcpy(&v, &b, 8);
    return v;
}

class Dsp {
public:
    // SetMediaType: the format (float, 1 or 2 channels) and a new start
    void setFormat(int rate, int channels)
    {
        rate_ = rate > 0 ? rate : 44100;
        chn_ = std::max(1, std::min(2, channels));
        for (int c = 0; c < 2; ++c) ch_[c] = Ch();
        blk_ = 0;
        fill_ = 0;
        pend_ = false;
        outIdx_ = 0;
        started_ = false;
        restart_ = false;
        inited_ = false;
        dirty_ = false;
        if (have_) {
            init(true);
            inited_ = true;
        }
    }
    int block() const { return blk_ > 0 ? blk_ : std::max(1, toInt((double)rate_ * 0.02)); }
    int latency() const { return block(); }
    int channels() const { return chn_; }

    // the settings the sound should use from the next call on (a change makes the next call work them out again)
    void setSettings(const Settings& s)
    {
        if (!have_ || s != set_) {
            set_ = s;
            dirty_ = true;
        }
        have_ = true;
    }
    // a jump in time (a discontinuity): the next call starts the effect again
    void restart() { restart_ = true; }
    bool bypassAll() const { return bypassAll_; }

    // One call of the plug-in's Process: it takes the frames of 'in' (interleaved) until a block is complete, hands the
    // finished block out into 'out' (room for 'cap' frames) and stops when the finished sound that did not fit is still
    // waiting or the input is used up. 'consumed' and 'delivered' say how far it came. Call it again with the rest of the
    // input, as a host does.
    void process(const float* in, int inN, float* out, int cap, int* consumed, int* delivered)
    {
        *consumed = 0;
        *delivered = 0;
        if (!have_) return;
        const bool mode0 = restart_;
        // (a change of the settings, or a jump in time after the first call, makes the call work its state out again)
        if (dirty_ || !inited_ || (mode0 && started_)) {
            init(mode0);
            inited_ = true;
        }
        started_ = true;
        dirty_ = false;
        restart_ = false;
        if (bypassAll_) {                       // the sound goes through as it is
            const int n = std::min(inN, cap);
            std::copy(in, in + (size_t)n * chn_, out);
            *consumed = n;
            *delivered = n;
            return;
        }
        if (inN == 0 && !pend_ && fill_ == 0) return;
        int used = 0, given = 0;
        for (;;) {
            if (pend_) {
                while (given < cap && outIdx_ < blk_) {
                    for (int c = 0; c < chn_; ++c) {
                        const double g = rd<double>(ch_[c], 8);
                        out[(size_t)given * chn_ + c] = (float)((double)ch_[c].b18[(size_t)outIdx_] * g);
                    }
                    ++outIdx_;
                    ++given;
                }
                if (outIdx_ != blk_) break;     // what did not fit waits for the next call
                outIdx_ = 0;
                fill_ = 0;
                pend_ = false;
            }
            while (used < inN && fill_ < blk_) {
                for (int c = 0; c < chn_; ++c) ch_[c].b18[(size_t)fill_] = in[(size_t)used * chn_ + c];
                ++fill_;
                ++used;
            }
            if (fill_ != blk_) break;
            apply();
            pend_ = true;
        }
        *consumed = used;
        *delivered = given;
    }
    // the finished sound still waiting for room (frames)
    int pending() const { return pend_ ? blk_ - outIdx_ : 0; }
    // a copy of the first frames of it (without taking them: the next call still gives them), for a host that needs the
    // sound before the plug-in's call would have handed it over; returns how many frames there were
    int peekPending(float* out, int frames) const
    {
        const int n = std::min(frames, pending());
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < chn_; ++c)
                out[(size_t)i * chn_ + c] = (float)((double)ch_[c].b18[(size_t)(outIdx_ + i)] * rd<double>(ch_[c], 8));
        return n;
    }

    // a value of the plug-in's state (tests): band 'k' of channel 'c', the field at offset 'off' of the band's record
    double stateDouble(int c, int k, int off) const { return rd<double>(ch_[c], 0x200 * k + off); }

private:
    // a channel's state, in the plug-in's own layout: 4 header bytes of bands and so on, then the four bands' records
    // (0x200 bytes apart) of filter states and coefficients (see init); the buffers hold the block being worked on
    struct Ch {
        alignas(16) uint8_t raw[0x840];
        std::vector<float> b18, b20;
        Ch() { std::memset(raw, 0, sizeof raw); }
    };
    Settings set_;
    bool have_ = false, dirty_ = false, inited_ = false, started_ = false, restart_ = false, bypassAll_ = false;
    int rate_ = 44100, chn_ = 2, blk_ = 0, fill_ = 0, outIdx_ = 0;
    bool pend_ = false;
    int solo_[kBands] = {0, 0, 0, 0};
    Ch ch_[2];

    template <class T>
    static T rd(const Ch& c, int off)
    {
        T v;
        std::memcpy(&v, c.raw + off, sizeof v);
        return v;
    }
    template <class T>
    static void wr(Ch& c, int off, T v)
    {
        std::memcpy(c.raw + off, &v, sizeof v);
    }
    static double* stage(Ch& c, int off) { return reinterpret_cast<double*>(c.raw + off); }

    // The settings made into the state (SfMbdyn 0x18002cb70 and 0x18002ee80). 'reset' is a start (a new format or a
    // discontinuity): the filters, the gains and the block being collected start again.
    void init(bool reset)
    {
        const Settings& s = set_;
        bypassAll_ = s.bypassAll != 0;
        for (int c = 0; c < chn_; ++c) initCh(ch_[c], reset);
        for (int k = 0; k < kBands; ++k) solo_[k] = s.solo[k];
        if (reset) {
            fill_ = 0;
            pend_ = false;
            outIdx_ = 0;
        }
    }

    void initCh(Ch& c, bool reset)
    {
        const Settings& s = set_;
        const double rate = (double)(uint32_t)rate_;
        if (reset) {
            blk_ = toInt(rate * 0.02);
            wr<int32_t>(c, 0x14, blk_);
            c.b18.assign((size_t)std::max(blk_, 1), 0.f);
            c.b20.assign((size_t)std::max(blk_, 1), 0.f);
            for (int k = 0; k < kBands; ++k) {
                std::memset(c.raw + 0x200 * k + 0x80, 0, 0x40);
                std::memset(c.raw + 0x200 * k + 0x100, 0, 0x40);
            }
        }
        wr<int32_t>(c, 0, kBands);
        for (int k = 0; k < kBands; ++k) wr<int32_t>(c, 0x200 * k + 0x40, s.bypass[k]);
        wr<double>(c, 8, std::pow(10.0, (double)s.outGain / 200.0));
        wr<int32_t>(c, 0x10, s.sync);
        const double minGain = reset ? 1.0 : 0.0;       // (the plug-in hands its flag on as the minimum gain of its shelves)
        const int rs = reset ? 1 : 0;
        for (int k = 0; k < kBands; ++k) {
            if (s.bypass[k] != 0) continue;
            const int R = 0x200 * k;
            const double attack = (double)s.attack[k] / 10000.0, release = (double)s.release[k] / 10000.0;
            wr<int32_t>(c, R + 0x1f8, toInt(rate * attack));
            wr<int32_t>(c, R + 0x218, toInt(rate * release));
            wr<double>(c, R + 0x1d8, (double)s.amount[k] / 10.0);
            double thr = std::pow(10.0, (double)s.threshold[k] / 200.0);
            if (s.attack[k] == -600) thr = 4.656612875245797e-10;   // (the plug-in tests the attack here, not the threshold)
            wr<double>(c, R + 0x1e0, thr);
            wr<double>(c, R + 0x200, thr * 0.85);
            if (release > 0.02) wr<double>(c, R + 0x208, 1.0 - std::pow(0.1, 0.02 / release));
            else wr<double>(c, R + 0x208, 1.0);
            if (attack > 0.02) wr<double>(c, R + 0x1e8, 1.0 - std::pow(0.1, 0.02 / attack));
            else wr<double>(c, R + 0x1e8, 1.0);
            wr<double>(c, R + 0x210, 1.0 - rd<double>(c, R + 0x208));
            wr<double>(c, R + 0x1f0, 1.0 - rd<double>(c, R + 0x1e8));
            if (reset) {
                wr<int32_t>(c, R + 0x1c0, 0);
                wr<double>(c, R + 0x1d0, 1.0);
            }
            wr<int64_t>(c, R + 0x1c8, 0);
            wr<int32_t>(c, R + 0x1c4, s.type[k]);
            wr<int32_t>(c, R + 0x140, 0);
            double p[3];
            p[0] = (double)s.freq[k] / rate;
            p[1] = -25.0;
            const double g = (double)s.gain[k] / 10.0;
            switch (s.type[k]) {
            case 0:
                p[2] = 0.1;
                vegsig::lowShelfCut(stage(c, R + 0x80), p, minGain, 1);
                vegsig::lowShelfCut(stage(c, R + 0x100), p, minGain, 1);
                p[1] = g;
                if (g > 0.0) {
                    vegsig::lowShelfBoost(stage(c, R + 0x180), p, rs);
                    wr<int32_t>(c, R + 0x140, 1);
                } else if (0.0 > g) {
                    vegsig::lowShelfCut(stage(c, R + 0x180), p, minGain, 1);
                    wr<int32_t>(c, R + 0x140, -1);
                }
                break;
            case 1:
                p[2] = 10.0 / (double)s.width[k];
                vegsig::bandPass(stage(c, R + 0x48), p, rs);
                vegsig::bandPass(stage(c, R + 0xc0), p, rs);
                p[1] = g;
                vegsig::bandPass(stage(c, R + 0x148), p, rs);
                if (g > 0.0) wr<int32_t>(c, R + 0x140, 1);
                else if (0.0 > g) wr<int32_t>(c, R + 0x140, -1);
                break;
            case 2:
                p[2] = 0.3;
                vegsig::hiShelfCut(stage(c, R + 0x80), p, minGain, 1);
                vegsig::hiShelfCut(stage(c, R + 0x100), p, minGain, 1);
                p[1] = g;
                if (g > 0.0) {
                    vegsig::hiShelfBoost(stage(c, R + 0x180), p, rs);
                    wr<int32_t>(c, R + 0x140, 1);
                } else if (0.0 > g) {
                    vegsig::hiShelfCut(stage(c, R + 0x180), p, minGain, 1);
                    wr<int32_t>(c, R + 0x140, -1);
                }
                break;
            default:
                break;
            }
        }
    }

    // The block through its bands (SfMbdyn 0x1800371f0 for two channels, 0x180036760 for one)
    void apply()
    {
        int soloAny = 0;
        for (int k = 0; k < kBands; ++k) soloAny |= solo_[k];
        for (int k = 0; k < kBands; ++k) {
            const int R = 0x200 * k;
            const bool want = soloAny == 0 ? true : solo_[k] != 0;
            if (rd<int32_t>(ch_[0], R + 0x40) != 0 || !want) continue;
            const int type = rd<int32_t>(ch_[0], R + 0x1c4);
            // the band's part of the sound, and its loudest sample
            for (int c = 0; c < chn_; ++c) {
                Ch& h = ch_[c];
                wr<int64_t>(h, R + 0x1c8, 0);
                double peak = 0;
                for (int i = 0; i < blk_; ++i) {
                    const double y = firstStage(h, R, type, (double)h.b18[(size_t)i]);
                    if (y > peak) {
                        peak = y;
                    } else {
                        const double ny = -y;
                        if (ny > peak) peak = ny;
                    }
                    h.b20[(size_t)i] = (float)y;
                }
                wr<double>(h, R + 0x1c8, peak);
            }
            if (chn_ == 2 && rd<int32_t>(ch_[0], 0x10) != 0) {
                // the channels move together: the louder of the two decides
                const double p1 = rd<double>(ch_[1], R + 0x1c8);
                if (p1 > rd<double>(ch_[0], R + 0x1c8)) wr<double>(ch_[0], R + 0x1c8, p1);
                follow(ch_[0], R);
                applyGain(ch_[0], R, &ch_[1]);
                if (rd<int32_t>(ch_[0], R + 0x140) != 0) {
                    gainStage(ch_[0], R, type);
                    gainStage(ch_[1], R, type);
                }
            } else {
                for (int c = 0; c < chn_; ++c) {
                    follow(ch_[c], R);
                    applyGain(ch_[c], R, nullptr);
                    if (rd<int32_t>(ch_[c], R + 0x140) != 0) gainStage(ch_[c], R, type);
                }
            }
        }
    }

    // a sample through the band's first filter (the part of the sound the band works on)
    static double firstStage(Ch& h, int R, int type, double x)
    {
        double* st = reinterpret_cast<double*>(h.raw + R);
        switch (type) {
        case 0: {
            const double s0 = st[0x80 / 8];
            double v = x * st[0xa0 / 8];
            const double t = st[0x98 / 8] * st[0x88 / 8];
            st[0x88 / 8] = s0;
            v = v + s0 * st[0x90 / 8];
            v = v - t;
            v = flush(v);
            st[0x80 / 8] = v;
            return v;
        }
        case 1: {
            const double s0 = st[0x48 / 8];
            double v = x * st[0x68 / 8];
            const double t = st[0x60 / 8] * st[0x50 / 8];
            st[0x50 / 8] = s0;
            const double u = s0 * st[0x58 / 8];
            v = v - t;
            v = v + u;
            const double raw = v;
            st[0x48 / 8] = flush(v);
            return (s0 - raw) * st[0x70 / 8];
        }
        case 2: {
            const double s0 = st[0x80 / 8], s1 = st[0x88 / 8];
            double v = x * st[0xa8 / 8];
            st[0x88 / 8] = s0;
            const double a = s0 * st[0x90 / 8];
            const double b = s1 * st[0x98 / 8];
            const double twice = s0 * 2.0;
            v = v + a;
            v = v - b;
            v = flush(v);
            st[0x80 / 8] = v;
            double y = s1 + v;
            y = y - twice;
            return y;
        }
        default:
            return x;
        }
    }

    // the band is "above" its threshold (the attack takes over) once its level reaches it, and goes back to release when
    // the level falls under 85 percent of it
    static void follow(Ch& h, int R)
    {
        const double peak = rd<double>(h, R + 0x1c8);
        if (peak >= rd<double>(h, R + 0x1e0)) wr<int32_t>(h, R + 0x1c0, 2);
        else if (rd<double>(h, R + 0x200) > peak) wr<int32_t>(h, R + 0x1c0, 0);
    }

    // SfMbdyn 0x18002fe50: the gain the band's level asks for, stepped from the gain it had (one pole), and the block
    // gets the band's part taken off by (1 - gain), the gain ramped over the block. 'other' is the second channel when
    // the channels are synced: it is given the same gain.
    void applyGain(Ch& h, int R, Ch* other)
    {
        const int frames = blk_;
        if (frames == 0) return;
        const double thr = rd<double>(h, R + 0x1e0), peak = rd<double>(h, R + 0x1c8), ratio = rd<double>(h, R + 0x1d8);
        double x;
        if (thr == 0.0 || peak == 0.0 || ratio == 0.0) {
            x = 1.0;
        } else {
            double t = std::log10(peak / thr);
            t = t / ratio;
            const double v = std::pow(10.0, t);
            x = v * thr;
            x = x / peak;
            if (0.0 > x) x = 0.0;
            else if (x > 1.0) x = 1.0;
        }
        if (ratio == 1.0) x = 1.0;
        if (ratio > 30.0) x = x * x;
        if (ratio > 40.0) x = x * x;
        if (ratio == 50.0 && 0.5 > x) x = 0.0;
        const double cur = rd<double>(h, R + 0x1d0);
        double target;
        int smp;
        if (rd<int32_t>(h, R + 0x1c0) != 0) {
            const double a = x * rd<double>(h, R + 0x1e8);
            smp = rd<int32_t>(h, R + 0x1f8);
            target = cur * rd<double>(h, R + 0x1f0) + a;
        } else {
            const double a = x * rd<double>(h, R + 0x208);
            smp = rd<int32_t>(h, R + 0x218);
            target = cur * rd<double>(h, R + 0x210) + a;
        }
        double step;
        if (smp > frames) {
            step = (target - cur) / (double)frames;
        } else if (smp != 0) {
            step = (target - cur) / (double)smp;
        } else {
            wr<double>(h, R + 0x1d0, target);
            step = 0.0;
        }
        for (int i = 0; i < frames; ++i) {
            double g = rd<double>(h, R + 0x1d0);
            h.b18[(size_t)i] = h.b18[(size_t)i] - (float)((double)h.b20[(size_t)i] * (1.0 - g));
            if (other) {
                g = rd<double>(h, R + 0x1d0);
                other->b18[(size_t)i] = other->b18[(size_t)i] - (float)((double)other->b20[(size_t)i] * (1.0 - g));
            }
            g = rd<double>(h, R + 0x1d0);
            if ((step > 0.0 && target > g) || (0.0 > step && g > target)) wr<double>(h, R + 0x1d0, g + step);
        }
        wr<double>(h, R + 0x1d0, target);
    }

    // SfMbdyn 0x1800340d0: the band's own gain, a shelf or peak filter of the whole block
    void gainStage(Ch& h, int R, int type)
    {
        double* st = reinterpret_cast<double*>(h.raw + R);
        const int frames = blk_;
        switch (type) {
        case 0: {
            const bool boost = rd<int32_t>(h, R + 0x140) > 0;
            for (int i = 0; i < frames; ++i) {
                const double x = (double)h.b18[(size_t)i];
                const double s0 = st[0x180 / 8];
                const double t = st[0x198 / 8] * st[0x188 / 8];
                double v = x * st[0x1a0 / 8];
                const double a = s0 * st[0x190 / 8];
                st[0x188 / 8] = s0;
                const double xo = x * st[0x1b0 / 8];
                v = v + a;
                v = v - t;
                v = flush(v);
                st[0x180 / 8] = v;
                double y;
                if (boost) y = v * st[0x1b8 / 8] + xo;
                else y = (x - v) * st[0x1b8 / 8] + xo;
                h.b18[(size_t)i] = (float)y;
            }
            break;
        }
        case 1:
            for (int i = 0; i < frames; ++i) {
                const double x = (double)h.b18[(size_t)i];
                const double s0 = st[0x148 / 8];
                const double t = st[0x160 / 8] * st[0x150 / 8];
                const double u = s0 * st[0x158 / 8];
                double v = x * st[0x168 / 8];
                st[0x150 / 8] = s0;
                v = v - t;
                v = v + u;
                const double raw = v;
                st[0x148 / 8] = flush(v);
                double d = raw - s0;
                d = d * st[0x170 / 8];
                h.b18[(size_t)i] = (float)(x + d);
            }
            break;
        case 2:
            for (int i = 0; i < frames; ++i) {
                const double x = (double)h.b18[(size_t)i];
                const double s0 = st[0x180 / 8], s1 = st[0x188 / 8];
                const double a = s0 * st[0x190 / 8];
                const double b = s1 * st[0x198 / 8];
                double v = x * st[0x1a8 / 8];
                st[0x188 / 8] = s0;
                const double xo = x * st[0x1b0 / 8];
                v = v + a;
                const double twice = s0 * 2.0;
                v = v - b;
                v = flush(v);
                double y = s1 + v;
                st[0x180 / 8] = v;
                y = y - twice;
                y = y * st[0x1b8 / 8];
                y = y + xo;
                h.b18[(size_t)i] = (float)y;
            }
            break;
        default:
            break;
        }
    }
};

}  // namespace vegmb
}  // namespace reno
