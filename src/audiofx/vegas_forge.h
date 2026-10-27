// VEGAS's Volume and Pan DirectX effects (sffrgpnv_x64.dll): their saved settings, presets and the processing, worked out
// from the DLL. Each keeps one float; the DLL's own parameter descriptor says Volume is 0..2 (1 = neutral) and Pan -1..1
// (0 = neutral). Reno's built-in versions use the same settings and saved form, so a project's VEGAS Volume / Pan loads into
// them and they save back as VEGAS's.
#pragma once
#include "../core/base.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegforge {

// ------------------------------------------------------------------------------------------ saved form
// IPersistStream::Save of both effects: u32 total length (0x18), a 16-byte GUID, the float.
struct Kind {
    const char* clsid;       // the DirectX plug-in's CLSID
    const char* dxUid;
    uint8_t guid[16];        // the GUID in the saved bytes
    float lo, hi, neutral;   // the parameter descriptor
};

inline const Kind& volume()
{
    static const Kind k = {"ee38ca88-d78e-4bfb-b05e-577892730c83", "dshow:{EE38CA88-D78E-4BFB-B05E-577892730C83}",
                           {0x9c, 0x6b, 0x95, 0xc6, 0x70, 0x44, 0xb2, 0x45, 0x8a, 0x7c, 0xe4, 0xf9, 0x8a, 0x39, 0x43, 0xb9}, 0.f, 2.f, 1.f};
    return k;
}
inline const Kind& pan()
{
    static const Kind k = {"eb6213db-08ff-4510-9f8d-3058b0ece4c6", "dshow:{EB6213DB-08FF-4510-9F8D-3058B0ECE4C6}",
                           {0x11, 0x7e, 0x80, 0x83, 0xae, 0xaf, 0xf7, 0x48, 0x8b, 0xd0, 0xff, 0x1f, 0x64, 0x5a, 0xec, 0x16}, -1.f, 1.f, 0.f};
    return k;
}

// Volume's own limit for what can be typed in (the DLL clamps to 0..2000); Pan's is its descriptor range.
inline float limit(const Kind& k, float v)
{
    if (!std::isfinite(v)) return k.neutral;
    const float hi = &k == &volume() ? 2000.f : k.hi;
    return v < k.lo ? k.lo : (v > hi ? hi : v);
}

inline std::string encode(const Kind& k, float v)
{
    std::string b(24, '\0');
    const uint32_t len = 24;
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], k.guid, 16);
    std::memcpy(&b[20], &v, 4);
    return "S:" + base64Encode(b.data(), b.size());
}

inline bool decode(const Kind& k, const std::string& st, float* out)
{
    if (st.size() < 3 || st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 24 || std::memcmp(b.data() + 4, k.guid, 16) != 0) return false;
    float v;
    std::memcpy(&v, b.data() + 20, 4);
    *out = limit(k, v);
    return true;
}

// ------------------------------------------------------------------------------------------ presets
// Both effects list "(Default)" and "Default all parameters": the neutral value.
inline const char* presetName(int i) { return i == 0 ? "(Default)" : "Default all parameters"; }

// ------------------------------------------------------------------------------------------ processing
// Volume: every sample times the gain, in float (the DLL's mulss).
inline void applyVolume(float* l, float* r, int n, float gain)
{
    for (int k = 0; k < n; ++k) {
        l[k] = gain * l[k];
        r[k] = gain * r[k];
    }
}

// Pan (stereo): moves signal from one channel to the other, in double, as the DLL does. Below 0 the left channel is
// turned down by (1 + p) and what is taken goes to the right; from 0 up the right is turned down by (1 - p) and goes to
// the left.
inline void applyPan(float* l, float* r, int n, float p)
{
    if (p < 0.f) {
        const double a = (double)p + 1.0, b = (double)(-p);
        for (int k = 0; k < n; ++k) {
            const double L = (double)l[k], R = (double)r[k];
            const double nl = L * a, nr = L * b + R;
            l[k] = (float)nl;
            r[k] = (float)nr;
        }
    } else {
        const double c = 1.0 - (double)p, d = (double)p;
        for (int k = 0; k < n; ++k) {
            const double L = (double)l[k], R = (double)r[k];
            const double nr = R * c, nl = R * d + L;
            l[k] = (float)nl;
            r[k] = (float)nr;
        }
    }
}

}  // namespace vegforge
}  // namespace reno
