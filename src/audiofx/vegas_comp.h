// VEGAS's Track Compressor DirectX effect (sftrkfx1_x64.dll, CLSID 23c9f225-40ec-11d2-9d36-00c04f8edc1e): its saved
// settings, presets and the processing, worked out from the DLL's code (same math, same order, same state).
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegcomp {

const char* const kClsid = "23c9f225-40ec-11d2-9d36-00c04f8edc1e";
const char* const kDxUid = "dshow:{23C9F225-40EC-11D2-9D36-00C04F8EDC1E}";

// The plug-in's own layout (what it saves, after its length and a GUID).
struct Settings {
    float inputGain = 0.f;      // dB, -60..25
    float outputGain = 0.f;     // dB, -60..25
    float threshold = 0.f;      // dB, -60..0
    float amount = 1.f;         // x:1, 1..50
    float attack = 15.f;        // ms, 0..500
    float release = 250.f;      // ms, 0..5000
    int32_t autoGain = 1;       // "Auto gain compensation"
    int32_t smooth = 0;         // "Smooth saturation"
    int32_t unused = 0;
    int32_t bypass = 0;
};
static_assert(sizeof(Settings) == 40, "VEGAS Track Compressor's settings are 40 bytes");

inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0x9b, 0xf9, 0x00, 0xc4, 0x6e, 0x0c, 0x4c, 0x44, 0x8f, 0x20, 0x98, 0xdb, 0x21, 0x6b, 0xe5, 0x6e};
    return g;
}

inline float clampF(float v, float lo, float hi)
{
    if (!(v >= lo)) return lo;
    return v > hi ? hi : v;
}

inline void sanitize(Settings& s)
{
    s.inputGain = clampF(s.inputGain, -60.f, 25.f);
    s.outputGain = clampF(s.outputGain, -60.f, 25.f);
    s.threshold = clampF(s.threshold, -60.f, 0.f);
    s.amount = clampF(s.amount, 1.f, 50.f);
    s.attack = clampF(s.attack, 0.f, 500.f);
    s.release = clampF(s.release, 0.f, 5000.f);
    s.autoGain = s.autoGain ? 1 : 0;
    s.smooth = s.smooth ? 1 : 0;
    s.bypass = s.bypass ? 1 : 0;
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x3c), the GUID, the settings.
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

inline Settings make(float thr, float amount, float attack, float release, int autoGain, int smooth)
{
    Settings s;
    s.threshold = thr;
    s.amount = amount;
    s.attack = attack;
    s.release = release;
    s.autoGain = autoGain;
    s.smooth = smooth;
    return s;
}

// VEGAS's own presets, in its order (names from the DLL's string table)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", make(0.f, 1.f, 15.f, 250.f, 1, 0)},
        {"Default all parameters", make(0.f, 1.f, 15.f, 250.f, 1, 0)},
        {"Soft Limiter", make(-6.f, 25.f, 0.f, 250.f, 0, 1)},
        {"1.5:1 compression starting at -24 dB", make(-24.f, 1.5f, 15.f, 250.f, 0, 1)},
        {"2:1 compression starting at -18 dB", make(-18.f, 2.f, 15.f, 250.f, 1, 0)},
        {"3:1 compression starting at -15 dB", make(-15.f, 3.f, 15.f, 250.f, 1, 1)},
        {"6:1 compression starting at -12 dB", make(-12.f, 6.f, 15.f, 250.f, 1, 1)},
        {"10:1 compression starting at -9 dB", make(-9.f, 10.f, 15.f, 250.f, 1, 1)},
        {"Limit levels to -6 dB (hard limiter)", make(-6.f, 50.f, 0.f, 250.f, 0, 0)},
    };
    return list;
}

inline double bits(uint64_t b)
{
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
inline int32_t fbits(float f)
{
    int32_t i;
    std::memcpy(&i, &f, 4);
    return i;
}
inline float bitsf(int32_t i)
{
    float f;
    std::memcpy(&f, &i, 4);
    return f;
}
inline uint32_t dhi(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return (uint32_t)(b >> 32);
}

// The compressor as the DLL runs it on a stereo stream. The loudest of the two samples of a frame, boosted by a small
// term that follows rises, is compared (as a float's bits) with the threshold; above it the target gain is
// out * ((1 - b) * in + b * T / peak) (a "soft" compression law), below it in * out. The target is reached with separate
// attack and release one-pole coefficients, optionally through a second smoothing stage ("smooth saturation").
struct Comp {
    // prepared values (the DLL's names are its offsets in the core object)
    double inGain = 0, outGain = 0;            // 1d8, 1e0
    double attCoef = 0, relCoef = 0;           // 1b8, 1c0
    double g = 0;                              // 198, the gain applied
    double c200 = 0, c208 = 0, c210 = 0, c218 = 0, c220 = 1.0;
    double k228 = 0, k230 = 0, f238 = 0;       // boost filter
    double c240 = 0, c248 = 0, f250 = 0;       // smoothing stage
    double c258 = 0, c260 = 0;                 // 1 - attack / release coefficient
    int32_t envBits = 0;                       // 268, the held peak (float bits)
    int32_t thrBits = 0;                       // 26c, T / in (float bits)
    int smooth = 0;                            // 1fc
    int neutral = 0;                           // 1f8, ratio 1:1 and no gain: the DLL leaves the sound alone
    int bypass = 0;                            // 160

    static double powd(double a, double b) { return std::pow(a, b); }

    // The DLL's prepare; 'keep' keeps the running gain (a change of settings in the middle of a stream).
    void prepare(const Settings& s, double sampleRate, bool keep)
    {
        const int64_t rate = (int64_t)(uint32_t)(int)sampleRate;
        bypass = s.bypass;
        double in = 0, out = 0;
        if (s.inputGain > -60.0f) in = powd(10.0, (double)s.inputGain / 20.0);
        if (s.outputGain > -60.0f) out = powd(10.0, (double)s.outputGain / 20.0);
        double ex = -3.0;
        if (s.threshold > -60.0f) ex = (double)s.threshold / 20.0;
        double T = powd(10.0, ex);
        if (s.threshold > -3.0f) {
            double t = (-3.0 - (double)s.threshold) / 20.0;
            t = powd(10.0, t);
            const double out2 = out / t;
            in = in * t;
            out = out2;
            T = t * T;
        }
        double W, B;
        if (500.0f > s.amount) {
            const double r = (double)s.amount;
            const double e = 1.0 - 1.0 / r;
            W = powd(T, e);
            B = (1.0 - W) / (1.0 - T);
        } else {
            W = T;
            B = 1.0;
        }
        neutral = ((double)s.amount == 1.0 && s.inputGain == 0.f && s.outputGain == 0.f) ? 1 : 0;
        if (s.autoGain) {
            double x = std::log10(W);
            x = x * 0.7;
            const double p = powd(10.0, x);
            out = (1.0 / p) * out;
        }
        // attack and release as one-pole coefficients
        auto nSamples = [&](float ms) {
            const float prod = (float)rate * ms;
            return (int)((double)prod / 1000.0);
        };
        int na = nSamples(s.attack), nr = nSamples(s.release);
        attCoef = na > 0 ? powd(0.1, 1.0 / (double)na) : 0.0;
        relCoef = nr > 0 ? powd(0.1, 1.0 / (double)nr) : 0.0;
        if (!keep) g = out;
        if (in == 0.0) {
            out = 0.0;
            in = bits(0x3e112e0be826d695ull);
        }
        c220 = 1.0;
        envBits = 0;
        const double TinvIn = T / in;
        c200 = B * out * T;
        c208 = (1.0 - B) * out * in;
        c210 = TinvIn;
        c218 = out * in;
        thrBits = fbits((float)TinvIn);
        c258 = 1.0 - attCoef;
        c260 = 1.0 - relCoef;
        smooth = s.smooth;
        if (s.smooth) {
            na = (int)((double)s.attack / 10000.0 * (double)rate);
            nr = (int)((double)s.release / 10000.0 * (double)rate);
            attCoef = na > 0 ? powd(0.1, 1.0 / (double)na) : 0.0;
            relCoef = nr > 0 ? powd(0.1, 1.0 / (double)nr) : 0.0;
            c260 = 1.0 - relCoef;
            c258 = 1.0 - attCoef;
        } else if (s.attack == 0.f) {
            na = (int)((double)rate * 0.001);
            attCoef = powd(0.1, 1.0 / (double)na);
            c258 = 1.0 - attCoef;
        }
        if (!keep) {
            c240 = bits(0x3f8cc565c1295a8bull);
            c248 = bits(0x3fef19d4d1f6b52cull);
            f250 = 0.5;
        }
        f238 = 0.0;
        k228 = bits((uint64_t)(-0x40101d135b73f5f4ll));
        k230 = bits(0x3fefc5d949181472ull);
    }

    // One stereo frame, in place.
    void frame(float* x)
    {
        int32_t a, b;
        std::memcpy(&a, &x[0], 4);
        std::memcpy(&b, &x[1], 4);
        a &= 0x7fffffff;
        b &= 0x7fffffff;
        int32_t r = a > b ? a : b;
        if ((dhi(f238) & 0x7ff00000u) < 0x3df00000u) f238 = 0.0;
        const double s0 = f238;
        float pk = bitsf(r);
        const double t = (double)pk * k228 + s0 * k230;
        f238 = t;
        const float d = (float)(t - s0);
        if (d > 0.0f) {
            pk = pk + d;
            r = fbits(pk);
        }
        double target;
        if (r > envBits) {
            envBits = r;
            if (r > thrBits) {
                const float e = bitsf(envBits);
                const double c88 = c200 / (double)e;
                envBits = fbits(e * 0.9990000128746033f);
                c220 = c88;
                target = c208 + c88;
            } else {
                target = c218;
            }
        } else if (envBits > thrBits) {
            const double c88 = 1.001001001001001 * c220;
            const float e = 0.9990000128746033f * bitsf(envBits);
            c220 = c88;
            target = c88 + c208;
            envBits = fbits(e);
        } else {
            target = c218;
        }
        double gg = g;
        if (gg > target) gg = gg * attCoef + target * c258;
        else gg = gg * relCoef + target * c260;
        g = gg;
        if (smooth) {
            if ((dhi(f250) & 0x7ff00000u) < 0x3df00000u) f250 = 0.0;
            const double s2 = f250;
            const double x1 = c240 * g + s2 * c248;
            const double y = s2 + x1;
            f250 = x1;
            g = y;
        }
        x[0] = (float)g * x[0];
        x[1] = (float)g * x[1];
    }

    // a block of stereo frames from two channel buffers (the DLL works on interleaved frames)
    void process(float* l, float* r, int n)
    {
        if (neutral || bypass) return;
        for (int k = 0; k < n; ++k) {
            float f[2] = {l[k], r[k]};
            frame(f);
            l[k] = f[0];
            r[k] = f[1];
        }
    }
};

}  // namespace vegcomp
}  // namespace reno
