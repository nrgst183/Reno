// VEGAS Venetian Blinds, as the older (DirectX) transition draws it: a re-implementation of what its render function does (read from the user's own VEGAS
// install, sftrans1.dll, checked bit for bit against that function's profile builder and lookup), not a fit. No dependencies, so a test program can include it alone.
//
// What it does: the blinds are a one-dimensional profile of alpha bytes laid across twice the frame's diagonal. A blind's period is the frame's width divided by the
// number of blinds (in 1/16384ths of a pixel); the incoming picture opens from the middle of every period outwards, covering half the period times the progress
// on each side of the middle. The two edges of an opening are one pixel steps with the partial coverage (0..255) in the pixel they fall in. The profile is then
// blurred by VEGAS's Gaussian (builtins_deriche.h) with a size of Feather x the profile's length, when Feather is not 0. Each pixel's position on the profile is
// the distance from the frame's centre along the angle's direction (sine for the rows, cosine for the columns, in 14-bit fixed point, rows counted as the picture buffer has them, from
// the top); its alpha is the profile read between two entries with a 14-bit fraction; the picture is the outgoing one plus (incoming - outgoing) x alpha, the
// 8-bit way (+128, +itself/256, /256) or, for float pictures, in floats with alpha / 255.
#pragma once
#include <cmath>
#include <cstdint>
#include <climits>
#include <vector>
#include <algorithm>

namespace reno {
namespace ofx {
namespace vfxvb {

// cvttsd2si: whole number towards zero, the "indefinite" integer for what does not fit
inline int cvt32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }
inline int add32(int a, int b) { return (int)((unsigned)a + (unsigned)b); }      // (the machine's wrapping add)
inline int sub32(int a, int b) { return (int)((unsigned)a - (unsigned)b); }
// (x + (x < 0 ? 16383 : 0)) >> 14: x / 16384 towards zero
inline int div14(int x) { return (int)(((int64_t)x + ((x >> 31) & 0x3fff)) >> 14); }

struct Params {
    double blinds = 1, angleDeg = 0, feather = 0;
    double progress = 0;                 // 0..1
    double aspect = 1;                   // (the pixel aspect VEGAS multiplies the cosine by)
};

// What the setup works out from the frame's size and the settings.
struct Setup {
    int w = 0, h = 0;
    int n = 0;                           // entries in the profile
    int centre = 0;                      // the profile's middle in 1/16384ths
    int sinI = 0, cosI = 0;              // 14-bit fixed point, the cosine with the aspect
    int progress14 = 0, feather14 = 0;
    double blurSize = 0;                 // size for the Gaussian (builtins_deriche.h: vegasDericheBlur(.., size, 0, ..)); only when feather14 > 0 and progress14 != 0
    bool blur = false;
};

inline Setup setup(const Params& p, int W, int H)
{
    Setup s;
    s.w = W;
    s.h = H;
    const double rad = p.angleDeg * 3.141592653589793 / 180.0;
    s.sinI = cvt32(std::sin(rad) * 16384.0);
    s.cosI = cvt32(std::cos(rad) * p.aspect * 16384.0);
    const double d2 = std::pow((double)H, 2.0) + std::pow((double)W, 2.0);
    const int L = cvt32(std::sqrt(d2) * 2.0 * 16384.0) & (int)0xffffc000;
    s.n = div14(L);
    s.centre = (L - (L >> 31)) >> 1;
    s.progress14 = cvt32((double)(float)p.progress * 16384.0);
    s.feather14 = cvt32(p.feather * 16384.0);
    const double F = p.feather < 0 ? 0.0 : std::min(1.0, p.feather);
    s.blurSize = (double)std::max(1, s.n) * F;
    s.blur = s.feather14 > 0;
    return s;
}

// The profile before the blur: alpha bytes, s.n of them (VEGAS's table builder). All 0 for no progress.
inline std::vector<uint8_t> profile(const Params& p, const Setup& s)
{
    const int n = std::max(0, s.n);
    std::vector<uint8_t> t((size_t)n, 0);
    if (s.progress14 == 0 || n <= 0) return t;
    const int A1 = cvt32((double)s.w * 16384.0);
    const int B1 = cvt32(p.blinds * 16384.0);
    const int P = cvt32((double)A1 * 16384.0 / (double)B1);          // one blind's length
    const int half = (P - (P >> 31)) >> 1;
    int E = cvt32((double)half * (double)s.progress14 * 6.103515625e-05);    // where the opening's edges lie
    int E2 = sub32(P, E);
    int sE = div14(E), sE2 = div14(E2);
    int i = 0, flag = 1;
    int hi = (n - ((n) >> 31)) >> 1;                 // upper half is written forwards from the middle, the lower half mirrored
    int lo = ((n - 1) - ((n - 1) >> 31)) >> 1;
    for (; hi < n && lo >= 0; ++hi, --lo, ++i) {
        int cl;
        if (i == sE) {
            if (sE2 == sE) {
                if (E2 < E) {
                    const int v = (((sub32(E, E2)) & 0x3fff) * 255 + 0x2000) >> 14;
                    const unsigned tt = (unsigned)((v & 0xff) * 255 + 128);
                    const int r = (int)((tt + (tt >> 8)) >> 8);
                    cl = r < 0 ? 0 : std::min(r, 255);
                } else {
                    const int v = (((sub32(E2, E)) & 0x3fff) * 255 + 0x2000) >> 14;
                    const int e = 0x80 - (v & 0xff) * 255;
                    int r = ((e >> 8) + e) >> 8;
                    r += 255;
                    cl = r < 0 ? 0 : std::min(r, 255);
                }
                E2 = add32(E2, P);
                sE2 = div14(E2);
            } else {
                flag = 0;
                cl = ((((E & 0x3fff) * 255) + 0x2000) >> 14) & 0xff;
            }
            t[(size_t)hi] = (uint8_t)cl;
            E = add32(E, P);
            sE = div14(E);
        } else if (i == sE2) {
            const int v = (((E2 & 0x3fff) * 255) + 0x2000) >> 14;
            flag = 1;
            E2 = add32(E2, P);
            t[(size_t)hi] = (uint8_t)~(uint8_t)v;
            sE2 = div14(E2);
        } else {
            t[(size_t)hi] = flag ? 0xff : 0x00;
        }
        t[(size_t)lo] = t[(size_t)hi];
    }
    return t;
}

// The alpha at position u (1/16384ths along the profile), between two entries, as VEGAS's lookup reads it.
inline int alphaAt(const std::vector<uint8_t>& t, int u)
{
    const int idx = div14(u);
    const int f = (((u & 0x3fff) * 255)) >> 14 & 0xff;
    const int n = (int)t.size();
    const int v0 = (idx >= 0 && idx < n) ? t[(size_t)idx] : 0, v1 = (idx + 1 >= 0 && idx + 1 < n) ? t[(size_t)idx + 1] : 0;
    const int d = (v1 - v0) * f + 128;
    const int r = (((d >> 8) + d) >> 8) + v0;
    return r < 0 ? 0 : std::min(r, 255);
}

// Position of the pixel in row y (counted from the top of the buffer) and column x: the profile's index in 1/16384ths.
inline int rowBase(const Setup& s, int y)
{
    const int edi = add32(-(s.h << 13), y * 0x4000);
    return sub32(s.centre, cvt32((double)edi * (double)s.sinI * -6.103515625e-05));
}
inline int positionAt(const Setup& s, int base, int x)
{
    const int r14 = add32(-(s.w << 13), x * 0x4000);
    return sub32(base, cvt32((double)r14 * (double)s.cosI * -6.103515625e-05));
}

// 8-bit mix of one channel: a + ((b - a) * alpha + 128, plus itself / 256, / 256), kept in 0..255
inline int mix8(int a, int b, int alpha)
{
    const int d = (b - a) * alpha + 128;
    const int r = (((d >> 8) + d) >> 8) + a;
    return r < 0 ? 0 : std::min(r, 255);
}
// Float mix: b - a times alpha / 255 (alpha as a float times the float 1/255), plus a
inline float mixF(float a, float b, int alpha)
{
    const float w = (float)alpha * 0.003921568859368563f;
    return (b - a) * w + a;
}

}  // namespace vfxvb
}  // namespace ofx
}  // namespace reno
