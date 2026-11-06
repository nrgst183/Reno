// VEGAS's Wave (vidpcore.dll, the effect object CDXTWave), re-implemented from the arithmetic of its render function (read from the user's installed copy; checked
// against that function bit for bit, see the commit message). The six settings are the six doubles the function takes, in this order:
//   P[0] vertical waves, P[1] horizontal waves, P[2] vertical amplitude, P[3] horizontal amplitude, P[4] vertical phase, P[5] horizontal phase.
// Every output row y takes its pixels from a source row shifted sideways by sin(y / H * (P0 * 3.141) + P4) * (W * P2) (VEGAS's "pi" is the constant 3.141, not pi),
// and every pixel then goes up or down by sin(x / W * (P1 * 3.141) + P5) * (H * P3). Positions are 14-bit fixed point (the double truncated after scaling by 16384),
// there is no half-pixel offset, and the position is held to 0 .. (W - 2) and 0 .. (H - 2) so the last column and row are never the left/top of the 2x2 taken.
// The 8-bit picture is mixed with weights of 14 bits (each product shifted down 4 on its own, +0x800000, >> 24); the float picture with single precision weights.
#pragma once
#include "../core/rmath.h"
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace reno {
namespace ofx {
namespace vfwave {

inline const double kPi = 3.141;       // (the constant VEGAS multiplies the wave counts with)

// cvttsd2si: a value outside the 32-bit range gives INT_MIN
inline int cvt32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }
inline int add32(int a, int b) { return (int)((unsigned)a + (unsigned)b); }
inline int shl14(int a) { return (int)((unsigned)a << 14); }

// One 2x2 mix. 'p' is the top left pixel, 'pitch' the bytes to the next row, fx and fy 0 .. 0x3fff.
inline void mix8(uint8_t* o, const uint8_t* p, int pitch, int fx, int fy)
{
    const unsigned w00 = (unsigned)((0x4000 - fx) * (0x4000 - fy)) >> 4, w10 = (unsigned)(fx * (0x4000 - fy)) >> 4;
    const unsigned w01 = (unsigned)((0x4000 - fx) * fy) >> 4, w11 = (unsigned)(fx * fy) >> 4;
    const uint8_t* q = p + pitch;
    for (int k = 0; k < 4; ++k) o[k] = (uint8_t)((w00 * p[k] + w10 * p[4 + k] + w01 * q[k] + w11 * q[4 + k] + 0x800000u) >> 24);
}
inline void mixF(float* o, const float* p, int pitchBytes, int fx, int fy)
{
    const float fxf = (float)fx * (1.0f / 16384.0f), fyf = (float)fy * (1.0f / 16384.0f);
    const float a = 1.0f - fyf, b = 1.0f - fxf;
    const float w00 = b * a, w01 = b * fyf, w11 = fyf * fxf, w10 = a * fxf;
    const float* q = (const float*)((const uint8_t*)p + pitchBytes);
    for (int k = 0; k < 4; ++k) {
        float s = w10 * p[4 + k];
        s = s + w00 * p[k];
        s = s + w01 * q[k];
        s = s + w11 * q[4 + k];
        o[k] = s;
    }
}

// The picture (W x H, 'bpp' 4 = bytes b g r a, or 16 = floats), rows 'srcPitch' / 'dstPitch' bytes apart. W and H must be at least 2.
inline void render(uint8_t* dst, int dstPitch, int W, int H, const uint8_t* src, int srcPitch, const double* P, int bpp)
{
    const double ky = P[0] * kPi, kx = P[1] * kPi, ampX = (double)W * P[2], ampY = (double)H * P[3];
    const int limX = shl14(W - 2), limY = shl14(H - 2);
    for (int y = 0; y < H; ++y) {
        int bx = cvt32(rm::sin((double)y / (double)H * ky + P[4]) * ampX * 16384.0);
        uint8_t* d = dst + (ptrdiff_t)y * dstPitch;
        for (int x = 0; x < W; ++x, d += bpp, bx = add32(bx, 0x4000)) {
            int dy = cvt32((rm::sin((double)x / (double)W * kx + P[5]) * ampY + (double)y) * 16384.0);
            const int sx = bx < 0 ? 0 : (bx > limX ? limX : bx);
            const int sy = dy < 0 ? 0 : (dy > limY ? limY : dy);
            const int ix = sx >> 14, fx = sx & 0x3fff, iy = sy >> 14, fy = sy & 0x3fff;
            const uint8_t* p = src + (ptrdiff_t)iy * srcPitch + (ptrdiff_t)ix * bpp;
            if (bpp == 16) mixF((float*)d, (const float*)p, srcPitch, fx, fy);
            else mix8(d, p, srcPitch, fx, fy);
        }
    }
}

}  // namespace vfwave
}  // namespace ofx
}  // namespace reno
