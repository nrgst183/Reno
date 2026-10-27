// VEGAS's Dither DirectX effect (sftrkfx1_x64.dll, CLSID 260df3e1-ac77-11d2-9e93-00c04f68be44): its saved settings, presets
// and the processing, worked out from the DLL's code (same random generator, same double math, same order).
//
// It rounds the signal to a chosen word length (8..32 bit) after adding noise ("dither") and, optionally, feeding the
// rounding error back ("noise shaping"). The noise comes from a 250-word shift-register generator that the DLL fills with
// the C runtime's rand(); the DLL seeds that with a time value, so no two runs sound the same. Reno seeds it with a fixed
// number (or the one the caller gives) so renders repeat.
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegdither {

const char* const kClsid = "260df3e1-ac77-11d2-9e93-00c04f68be44";
const char* const kDxUid = "dshow:{260DF3E1-AC77-11D2-9E93-00C04F68BE44}";

// The plug-in's own layout (what it saves, after its length and a GUID): indexes into its three lists, and a "changed" flag.
struct Settings {
    int32_t depth = 2;       // 0..6: 8, 12, 16, 18, 20, 24, 32 bit
    int32_t type = 1;        // 0..5: None, Half Rectangular, Rectangular, Triangular, Highpass Triangular, Gaussian
    int32_t shape = 0;       // 0..2: Off, High-pass contour, Equal loudness contour (VEGAS's presets use 3 as well)
    int32_t flag = 0;
};
static_assert(sizeof(Settings) == 16, "VEGAS Dither's settings are 16 bytes");

inline const int* depthBits()
{
    static const int v[7] = {8, 12, 16, 18, 20, 24, 32};
    return v;
}
inline const char* depthName(int i)
{
    static const char* const n[7] = {"8 bit", "12 bit", "16 bit", "18 bit", "20 bit", "24 bit", "32 bit"};
    return n[std::max(0, std::min(6, i))];
}
inline const char* typeName(int i)
{
    static const char* const n[6] = {"None", "Half Rectangular", "Rectangular", "Triangular", "Highpass Triangular", "Gaussian"};
    return n[std::max(0, std::min(5, i))];
}
inline const char* shapeName(int i)
{
    static const char* const n[3] = {"Off", "High-pass contour", "Equal loudness contour"};
    return n[std::max(0, std::min(2, i))];
}

// The same persistent-stream GUID the Track Compressor and Track Noise Gate use (the whole DLL's "TrackFX1" id).
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0x9b, 0xf9, 0x00, 0xc4, 0x6e, 0x0c, 0x4c, 0x44, 0x8f, 0x20, 0x98, 0xdb, 0x21, 0x6b, 0xe5, 0x6e};
    return g;
}

inline void sanitize(Settings& s)
{
    s.depth = std::max(0, std::min(6, (int)s.depth));
    s.type = std::max(0, std::min(5, (int)s.type));
    s.shape = std::max(0, std::min(3, (int)s.shape));
    s.flag = 0;
}

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

// VEGAS's presets, in its order. (The last two use noise shaping index 2 and 3; 3 is outside the page's list and acts as Off.)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", {2, 1, 0, 0}},
        {"Default all parameters", {2, 1, 0, 0}},
        {"Dither for 8-bit (low sample rate)", {0, 3, 0, 0}},
        {"Dither for 8-bit", {0, 4, 1, 0}},
        {"Dither for 16-bit (low sample rate)", {2, 3, 2, 0}},
        {"Dither for 16-bit", {2, 4, 3, 0}},
    };
    return list;
}

// ------------------------------------------------------------------------------------------------ the generator
// The C runtime's rand() as Microsoft's library has it (the DLL calls it to fill the generator's table)
struct MsRand {
    uint32_t seed = 1;
    void srand(uint32_t s) { seed = s; }
    int rand()
    {
        seed = seed * 214013u + 2531011u;
        return (int)((seed >> 16) & 0x7fff);
    }
};

// 250 words, two walking indexes (0 and 103) and a table that says which index follows which (the next one, wrapping).
struct Shift250 {
    int32_t i = 0, j = 0x67;
    uint8_t next[250];
    uint32_t t[250];

    void init(uint32_t seed)
    {
        MsRand r;
        r.srand(seed);
        i = 0;
        j = 0x67;
        for (int k = 249; k >= 0; --k) {
            next[k] = (uint8_t)(k < 249 ? k + 1 : 0);
            const uint32_t a = (uint32_t)r.rand() * 4u;
            const uint32_t b = (uint32_t)r.rand() << 17;
            const uint32_t c = (uint32_t)r.rand();
            t[k] = c + (a + b);
        }
        // a diagonal of 32 bits made certain (the lower bits of word 3, 10, 17, ... masked and the next bit forced on)
        uint32_t mask = 0xffffffffu, bit = 0x80000000u;
        for (int k = 3; bit; k += 7) {
            t[k] = (t[k] & mask) | bit;
            mask >>= 1;
            bit >>= 1;
        }
    }
    // one step: t[i] ^= t[j], and both indexes move on
    uint32_t step()
    {
        t[i] ^= t[j];
        const uint32_t v = t[i];
        i = next[i];
        j = next[j];
        return v;
    }
};

// The five noise generators, as the DLL's functions (each returns noise already times 'scale')
inline double noiseRect(Shift250& g, double scale)
{
    const int32_t v = (int32_t)g.step();
    return ((double)v * 4.656612873077393e-10) * scale;
}
inline double noiseTri(Shift250& g, double scale)
{
    const double c = 4.656612873077393e-10;
    const int32_t v1 = (int32_t)g.step();
    const double a = (double)v1 * c;
    const int32_t v2 = (int32_t)g.step();
    return ((double)v2 * c + a) * scale;
}
inline double noiseHigh(Shift250& g, double scale, double prevTimes099, double* prev)
{
    const int32_t v = (int32_t)g.step();
    const double x = (double)v * 4.656612873077393e-10;
    *prev = x;
    return (x - prevTimes099) * scale;
}
inline double noiseGauss(Shift250& g, double scale)
{
    double s, b;
    for (;;) {
        const double a = ((double)(uint16_t)g.step() / 65535.0) * 2.0 - 1.0;
        b = ((double)(uint16_t)g.step() / 65535.0) * 2.0 - 1.0;
        const double a2 = a * a;
        s = b * b + a2;
        if (s >= 1.0) continue;
        if (s == 0.0) continue;
        break;
    }
    double r = std::log(s);
    r = r * -2.0;
    r = r / s;
    r = std::sqrt(r);
    return r * (b * scale);
}

// ------------------------------------------------------------------------------------------------ the effect
struct Dither {
    Shift250 gen;
    int type = 1, mode = 0;
    double scale = 0, pow2 = 0, inv = 0;
    double blk[32];                  // [0] the high-pass noise's last value, [1+c] and [7+c] the shaping errors of channel c

    // The DLL's init of its generator (what runs when the effect starts): bits from the depth list, noise type, shaping mode.
    void prepare(const Settings& st, uint32_t seed)
    {
        Settings s = st;
        sanitize(s);
        const int bits = depthBits()[s.depth];
        type = s.type;
        mode = s.shape < 3 ? s.shape : 0;
        std::memset(blk, 0, sizeof blk);
        gen.init(seed);
        pow2 = (double)(int32_t)(1u << (bits - 1));      // (for 32 bit this is -2^31: the DLL's int shift)
        inv = 1.0 / pow2;
        switch (type) {
        case 0: scale = 0.0; break;
        case 1: scale = 0.5; break;
        case 2: scale = 1.0; break;
        default: scale = 0.5; break;
        }
        if (mode == 0) scale = scale / pow2;
    }

    // Planar buffers, one channel after the other (the DLL goes through all of one channel's samples, then the next, so the
    // generator's numbers are shared out in that order).
    void process(float* const* ch, int nch, int frames)
    {
        nch = std::max(0, std::min(6, nch));
        for (int c = 0; c < nch; ++c) channel(ch[c], c, frames);
    }

private:
    double noise()
    {
        switch (type) {
        case 1:
        case 2: return noiseRect(gen, scale);
        case 3: return noiseTri(gen, scale);
        case 4: {
            const double prev = blk[0] * 0.99;
            return noiseHigh(gen, scale, prev, &blk[0]);
        }
        case 5: return noiseGauss(gen, scale);
        default: return 0.0;
        }
    }
    static int32_t lowWord(double d)
    {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        return (int32_t)(uint32_t)b;
    }
    static double killDenormal(double d)
    {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        if ((uint32_t)((b >> 32) & 0x7ff00000u) < 0x3df00000u) return 0.0;
        return d;
    }
    void channel(float* x, int c, int frames)
    {
        const double magic = 6755399441055744.0;
        if (mode == 0) {
            for (int k = 0; k < frames; ++k) {
                const double n = noise();
                double v = (double)x[k] + n;
                v = v * pow2;
                v = v + magic;
                const int32_t q = lowWord(v);
                x[k] = (float)((double)q * inv);
            }
        } else if (mode == 1) {
            double e = blk[1 + c];
            for (int k = 0; k < frames; ++k) {
                const double n = noise();
                const double xs = (double)x[k] * pow2 - e;
                double v = xs + n;
                v = v + magic;
                const float f = (float)lowWord(v);
                e = (double)f - xs;
                x[k] = (float)((double)f * inv);
            }
            blk[1 + c] = killDenormal(e);
        } else {
            const double k2 = mode == 2 ? 0.75 : 0.5;
            double e1 = blk[1 + c], e2 = blk[7 + c];
            for (int k = 0; k < frames; ++k) {
                const double n = noise();
                const double xs = ((double)x[k] * pow2 - e1) + e2;
                e2 = e1 * k2;
                double v = xs + n;
                v = v + magic;
                const float f = (float)lowWord(v);
                e1 = (double)f - xs;
                x[k] = (float)((double)f * inv);
            }
            blk[1 + c] = killDenormal(e1);
            blk[7 + c] = killDenormal(e2);
        }
    }
};

}  // namespace vegdither
}  // namespace reno
