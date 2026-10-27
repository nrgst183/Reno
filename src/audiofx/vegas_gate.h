// VEGAS's Track Noise Gate DirectX effect (sftrkfx1_x64.dll, CLSID 869419dd-501f-11d3-8cdc-00c04f6b8e4c): its saved
// settings, presets and the processing, worked out from the DLL's code (same double math, same order, same state).
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace veggate {

const char* const kClsid = "869419dd-501f-11d3-8cdc-00c04f6b8e4c";
const char* const kDxUid = "dshow:{869419DD-501F-11D3-8CDC-00C04F6B8E4C}";
const int kInf = -800;           // threshold in tenths of a dB; -80.0 dB is "-Inf." (the gate is off)

// The plug-in's own layout (what it saves, after its length and a GUID).
struct Settings {
    int32_t attack = 3;          // ms, 0..500
    int32_t release = 100;       // ms, 1..5000
    int32_t threshold = kInf;    // tenths of a dB, -800..0
    int32_t bypass = 0;
};
static_assert(sizeof(Settings) == 16, "VEGAS Track Noise Gate's settings are 16 bytes");

inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0x9b, 0xf9, 0x00, 0xc4, 0x6e, 0x0c, 0x4c, 0x44, 0x8f, 0x20, 0x98, 0xdb, 0x21, 0x6b, 0xe5, 0x6e};
    return g;
}

inline void sanitize(Settings& s)
{
    s.attack = std::max(0, std::min(500, (int)s.attack));
    s.release = std::max(1, std::min(5000, (int)s.release));
    s.threshold = std::max(kInf, std::min(0, (int)s.threshold));
    s.bypass = s.bypass ? 1 : 0;
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x24), the GUID, the settings.
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
    if (b.size() < 20 + sizeof(Settings) || std::memcmp(b.data() + 4, persistGuid(), 16) != 0) return false;
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

// VEGAS's own presets, in its order (names from the DLL's string table)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", {3, 100, kInf, 0}},
        {"Default all parameters", {3, 100, kInf, 0}},
        {"Strong beats only", {1, 300, -60, 0}},
        {"Long decay time", {10, 1000, -380, 0}},
        {"Noise gate 1", {10, 500, -600, 0}},
        {"Noise gate 2", {1, 500, -320, 0}},
        {"Slow attack", {300, 1000, -400, 0}},
    };
    return list;
}

inline double bits(uint64_t b)
{
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

// The gate as the DLL runs it. Per frame (all channels): the loudest sample, boosted by a small high-pass-like term, is
// held as a peak for 20 ms; a five-state machine (closed, open, attack, release, hold) turns the peak and the threshold
// into a target gain; two smoothing stages shape it; the gain multiplies every sample in the frame.
struct Gate {
    // the DLL's state, in its offsets (relative to the state block)
    int attackSamples = 0, releaseSamples = 0, holdSamples = 0;   // +0, +4, +8
    double thr = 0, thrLow = 0;                                   // +0x10, +0x18
    int counter = 0, holdCounter = 0;                             // +0x20, +0x24
    double gain = 0;                                              // +0x28
    double peak = 0;                                              // +0x30
    int state = 3;                                                // +0x38
    int windowSamples = 0, windowPos = 0;                         // +0x3c, +0x40
    int off = 0;                                                  // +0x44: threshold is -Inf., the gate does nothing
    double k48 = 0, k50 = 0, f58 = 0, k60 = 0, k68 = 0, f70 = 0;
    int bypass = 0;                                               // core +0x118

    // The DLL's prepare: settings -> coefficients; 'keep' keeps the running state (a change of settings in the middle of
    // a stream), otherwise the state is started over.
    void prepare(const Settings& s, double sampleRate, bool keep)
    {
        bypass = s.bypass;
        if (s.threshold == kInf) thr = bits(0x3effffd845847726ull);
        else thr = std::pow(10.0, (double)s.threshold / 200.0);
        thrLow = thr * 0.85;
        const int rate = (int)sampleRate;
        attackSamples = (int)((double)s.attack / 1000.0 * (double)rate);
        releaseSamples = (int)((double)s.release / 1000.0 * (double)rate);
        off = s.threshold <= kInf ? 1 : 0;
        if (!keep) {
            double x = std::pow(thr, 0.9);
            x = std::log10(x);
            x = x * 20.0;
            x = x / 20.0;
            x = x * 0.7;
            const double p = std::pow(10.0, x);
            k60 = bits(0x3f8cc565c1295a8bull);
            f58 = 0;
            k68 = bits(0x3fef19d4d1f6b52cull);
            counter = 0;
            holdCounter = 0;
            gain = 1.0 / p;
            k48 = bits(0xbfefee87a5f61e2bull);
            k50 = bits(0x3fefdd0f4bec3cafull);
            f70 = 0.5;
            state = 3;
        }
    }
    // what the DLL does once when the effect starts (after the first prepare)
    void start(double sampleRate)
    {
        const int rate = (int)sampleRate;
        windowSamples = std::max(1, (int)((double)rate * 0.02));
        windowPos = 0;
        holdSamples = (int)((double)rate * 0.004);
    }

    // one frame of 'n' channels
    void frame(float* x, int n)
    {
        if (off) return;
        if (windowPos % windowSamples == 0) {
            peak = 0.0;
            windowPos = 0;
        }
        ++windowPos;
        double m = 0.0;
        for (int c = 0; c < n; ++c) {
            const double a = std::fabs((double)x[c]);
            if (a > m) m = a;
        }
        if ((uint32_t)(((uint64_t)bitsOf(f58) >> 32) & 0x7ff00000u) < 0x3df00000u) f58 = 0.0;
        const double old58 = f58;
        const double t = m * k48;
        double nw = old58 * k50 + t;
        f58 = nw;
        double up = nw - old58;
        if (!(up > 0.0)) up = 0.0;
        m = m + up;
        if (m > peak) peak = m;
        switch (state) {
        case 0:
            if (releaseSamples > counter) {
                const int c = counter;
                ++counter;
                gain = (double)(releaseSamples - c) / (double)releaseSamples;
                if (peak >= thr) state = 2;       // (the counter keeps running)
            } else {
                gain = 0.0;
                state = 3;
                counter = 0;
            }
            break;
        case 1:
            gain = 1.0;
            if (thrLow > peak) state = 4;
            break;
        case 2:
            if (attackSamples > counter) {
                const int c = counter;
                ++counter;
                gain = (double)c / (double)attackSamples;
            } else {
                counter = 0;
                gain = 1.0;
                state = peak >= thr ? 1 : 4;
            }
            break;
        case 3:
            gain = 0.0;
            if (peak >= thr) { state = 2; counter = 0; }
            break;
        case 4:
            ++holdCounter;
            gain = 1.0;
            if (holdSamples < holdCounter) {
                holdCounter = 0;
                state = 0;
            }
            if (peak >= thrLow) {
                holdCounter = 0;
                state = 4;
            }
            break;
        default:
            break;
        }
        if ((uint32_t)(((uint64_t)bitsOf(f70) >> 32) & 0x7ff00000u) < 0x3df00000u) f70 = 0.0;
        const double y1 = f70;
        const double x1 = k60 * gain + y1 * k68;
        const double y2 = y1 + x1;
        f70 = x1;
        gain = y2;
        const float g = (float)gain;
        for (int c = 0; c < n; ++c) x[c] = g * x[c];
    }

private:
    static uint64_t bitsOf(double d)
    {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        return b;
    }
};

}  // namespace veggate
}  // namespace reno
