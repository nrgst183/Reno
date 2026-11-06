// VEGAS's Swirl and Pinch/Punch (sftrans1.dll, the effect objects CSwirl and CPinch), re-implemented from the arithmetic of their render loops (read from the user's
// installed copy). Both work inside an ellipse round a centre pixel and copy every other pixel unchanged:
//   centre pixel   = (int)((CenterX + 0.5) * W), (int)((CenterY + 0.5) * H)   (truncated: the centre is a whole pixel; CenterX / CenterY here are 0..1 across / down)
//   ellipse        = half axes W * (ScaleX + 0.0001) / 2 and H * (ScaleY + 0.0001) / 2 pixels; d = ((x - cx) / rx, (y - cy) / ry), outside when d.d >= 1
//   Swirl          = the source point is the pixel turned by Amount * 15 * (1 - |d|)^2 radians (x' = dx cos + dy sin, y' = dy cos - dx sin, in ellipse units)
//   Pinch/Punch    = the source point is (pixel - centre) * (1 - Amount * 0.4 * (1 + cos(3.1415926 * |d|))) from the centre
// Source positions are in whole-pixel coordinates (no half-pixel offset), the floor of one is taken as floor(v + 1.5e-8) by the magic-number trick, and rows / columns
// are clamped to the picture. The 8-bit picture (bytes B, G, R, A) is mixed with 8-bit weights, the float picture (4 floats) with single precision weights:
//   Swirl 8-bit : weights = (alpha * wx * wy) >> 8 per corner (they carry the corner's alpha); alpha = (sum + 128) >> 8; colour = (sum of colour * weight + 0x8000) >> 16;
//                 sum 0 gives a transparent black pixel. Swirl float: the same with float weights (alpha times bilinear weight), colour = their sum, alpha = sum of weights.
//   Pinch 8-bit : plain bilinear, ((c00 * (256 - fx) + c01 * fx) * (256 - fy) + (c10 * (256 - fx) + c11 * fx) * fy) >> 16 for all four bytes. Float: plain bilinear.
#pragma once
#include "../core/rmath.h"
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace reno {
namespace ofx {
namespace vfswirl {

// cvttsd2si (32 bit): a value outside the range gives INT_MIN
inline int cvt32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }
// floor of a source position the way the DLL takes it: round-to-nearest-even of (v - 0.499999985) with the 2^52 + 2^51 trick
inline int floorMagic(double v)
{
    double d = (v - 0.499999985) + 6755399441055744.0;
    int64_t bits;
    std::memcpy(&bits, &d, 8);
    return (int)(int32_t)(bits & 0xffffffff);
}
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Area {
    int W = 0, H = 0;
    int cxPix = 0, cyPix = 0;      // centre pixel
    double sxInv = 1, syInv = 1;   // 1 / (Scale + 0.0001)
    double k13 = 0;                // 2 * sxInv / W
    double rx = 0, ry = 0;         // half axes in pixels
    double amount = 0;
    double cyN = 0;                // per row: 2 / H * syInv is applied in 'rowD'
};

// cx, cy: 0..1 across and down (the DLL holds them minus 0.5 and adds the 0.5 back)
inline Area setup(int W, int H, double amount, double cx, double cy, double scaleX, double scaleY)
{
    Area a;
    a.W = W;
    a.H = H;
    a.amount = amount;
    const double cxF = (cx - 0.5) + 0.5, cyF = (cy - 0.5) + 0.5;
    a.cxPix = cvt32(cxF * (double)W);
    a.cyPix = cvt32(cyF * (double)H);
    a.sxInv = 1.0 / (scaleX + 0.0001);
    a.syInv = 1.0 / (scaleY + 0.0001);
    a.k13 = (a.sxInv + a.sxInv) / (double)W;
    a.rx = ((double)W / a.sxInv) * 0.5;
    a.ry = ((double)H / a.syInv) * 0.5;
    return a;
}
// the row's normalised y (and its square is what the pixel loop adds to dx * dx)
inline double rowD(const Area& a, int y) { return (((double)(y - a.cyPix) + (double)(y - a.cyPix)) / (double)a.H) * a.syInv; }

struct Src8 {
    const uint8_t* p;
    ptrdiff_t pitch;   // bytes
    const uint8_t* px(int x, int y) const { return p + (size_t)y * (size_t)pitch + (size_t)x * 4; }
};

// ---------------------------------------------------------------- Swirl 8-bit
inline void swirl8(uint8_t* dst, ptrdiff_t dstPitch, const uint8_t* src, ptrdiff_t srcPitch, const Area& a)
{
    const int W = a.W, H = a.H;
    if (W <= 0 || H <= 0) return;
    const double amt15 = a.amount * 15.0;
    const Src8 s{src, srcPitch};
    for (int y = 0; y < H; ++y) {
        uint8_t* o = dst + (size_t)y * (size_t)dstPitch;
        const double dy = rowD(a, y), dy2 = dy * dy;
        for (int x = 0; x < W; ++x, o += 4) {
            const double dx = (double)(x - a.cxPix) * a.k13;
            const double d2 = dx * dx + dy2;
            if (!(d2 < 1.0)) { std::memcpy(o, s.px(x, y), 4); continue; }
            const double r = std::sqrt(d2);
            const double one = 1.0 - r;
            const double t = (one * amt15) * one;
            const double ca = rm::cos(t), sa = rm::sin(t);
            const double ysrc = (ca * dy - sa * dx) * a.ry + (double)a.cyPix;
            const double xsrc = (ca * dx + sa * dy) * a.rx + (double)a.cxPix;
            const int iy = floorMagic(ysrc);
            const int fy = cvt32((ysrc - (double)iy) * 256.0);
            const int ix = floorMagic(xsrc);
            const int fx = cvt32((xsrc - (double)ix) * 256.0);
            const int y0 = clampi(iy, 0, H - 1), y1 = clampi(iy + 1, 0, H - 1);
            const int x0 = clampi(ix, 0, W - 1), x1 = clampi(ix + 1, 0, W - 1);
            const uint8_t* p00 = s.px(x0, y0);
            const uint8_t* p01 = s.px(x1, y0);
            const uint8_t* p10 = s.px(x0, y1);
            const uint8_t* p11 = s.px(x1, y1);
            const unsigned a00 = ((unsigned)p00[3] * (unsigned)(256 - fx) * (unsigned)(256 - fy)) >> 8;
            const unsigned a01 = ((unsigned)p01[3] * (unsigned)fx * (unsigned)(256 - fy)) >> 8;
            const unsigned a10 = ((unsigned)p10[3] * (unsigned)(256 - fx) * (unsigned)fy) >> 8;
            const unsigned a11 = ((unsigned)p11[3] * (unsigned)fx * (unsigned)fy) >> 8;
            const unsigned sum = a11 + a10 + a01 + a00;
            if (sum == 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[3] = (uint8_t)((sum + 0x80) >> 8);
            for (int k = 0; k < 3; ++k)
                o[k] = (uint8_t)((p01[k] * a01 + p10[k] * a10 + p00[k] * a00 + (p11[k] * a11 + 0x8000u)) >> 16);
        }
    }
}

// ---------------------------------------------------------------- Swirl float
inline void swirlF(float* dst, ptrdiff_t dstPitch, const float* src, ptrdiff_t srcPitch, const Area& a)
{
    const int W = a.W, H = a.H;
    if (W <= 0 || H <= 0) return;
    const double amt15 = a.amount * 15.0;
    auto px = [&](int x, int y) { return (const float*)((const uint8_t*)src + (size_t)y * (size_t)srcPitch) + (size_t)x * 4; };
    for (int y = 0; y < H; ++y) {
        float* o = (float*)((uint8_t*)dst + (size_t)y * (size_t)dstPitch);
        const double dy = rowD(a, y), dy2 = dy * dy;
        for (int x = 0; x < W; ++x, o += 4) {
            const double dx = (double)(x - a.cxPix) * a.k13;
            const double d2 = dx * dx + dy2;
            if (!(d2 < 1.0)) { std::memcpy(o, px(x, y), 16); continue; }
            const double r = std::sqrt(d2);
            const double one = 1.0 - r;
            const double t = (one * amt15) * one;
            const double ca = rm::cos(t), sa = rm::sin(t);
            const double ysrc = (ca * dy - sa * dx) * a.ry + (double)a.cyPix;
            const double xsrc = (ca * dx + sa * dy) * a.rx + (double)a.cxPix;
            const int iy = floorMagic(ysrc);
            const float fy = (float)(ysrc - (double)iy), omfy = 1.0f - fy;
            const int ix = floorMagic(xsrc);
            const float fx = (float)(xsrc - (double)ix), omfx = 1.0f - fx;
            const int y0 = clampi(iy, 0, H - 1), y1 = clampi(iy + 1, 0, H - 1);
            const int x0 = clampi(ix, 0, W - 1), x1 = clampi(ix + 1, 0, W - 1);
            const float* p00 = px(x0, y0);
            const float* p01 = px(x1, y0);
            const float* p10 = px(x0, y1);
            const float* p11 = px(x1, y1);
            const float w00 = (omfx * p00[3]) * omfy;
            const float w10 = (omfx * p10[3]) * fy;
            const float w01 = (fx * p01[3]) * omfy;
            const float w11 = (fx * p11[3]) * fy;
            const float sum = ((w01 + w00) + w10) + w11;
            if (!(sum > 0.0f)) { o[0] = o[1] = o[2] = o[3] = 0.0f; continue; }
            o[3] = sum;
            for (int k = 0; k < 3; ++k) o[k] = ((w00 * p00[k] + w01 * p01[k]) + w10 * p10[k]) + w11 * p11[k];
        }
    }
}

// ---------------------------------------------------------------- Pinch/Punch
inline double pinchK(const Area& a, double r)
{
    const double c = rm::cos(r * 3.1415926);
    return 1.0 - ((c + 1.0) * a.amount) * 0.4;
}

inline void pinch8(uint8_t* dst, ptrdiff_t dstPitch, const uint8_t* src, ptrdiff_t srcPitch, const Area& a)
{
    const int W = a.W, H = a.H;
    if (W <= 0 || H <= 0) return;
    const Src8 s{src, srcPitch};
    for (int y = 0; y < H; ++y) {
        uint8_t* o = dst + (size_t)y * (size_t)dstPitch;
        const double dy = rowD(a, y), dy2 = dy * dy;
        const double yrel = (double)(y - a.cyPix);
        for (int x = 0; x < W; ++x, o += 4) {
            const double xrel = (double)(x - a.cxPix);
            const double dx = xrel * a.k13;
            const double d2 = dx * dx + dy2;
            if (!(d2 < 1.0)) { std::memcpy(o, s.px(x, y), 4); continue; }
            const double k = pinchK(a, std::sqrt(d2));
            const double ysrc = (double)a.cyPix + yrel * k;
            const double xsrc = (double)a.cxPix + xrel * k;
            const int iy = floorMagic(ysrc);
            const int fy = cvt32((ysrc - (double)iy) * 256.0);
            const int ix = floorMagic(xsrc);
            const int fx = cvt32((xsrc - (double)ix) * 256.0);
            const int y0 = clampi(iy, 0, H - 1), y1 = clampi(iy + 1, 0, H - 1);
            const int x0 = clampi(ix, 0, W - 1), x1 = clampi(ix + 1, 0, W - 1);
            const uint8_t* p00 = s.px(x0, y0);
            const uint8_t* p01 = s.px(x1, y0);
            const uint8_t* p10 = s.px(x0, y1);
            const uint8_t* p11 = s.px(x1, y1);
            for (int c = 0; c < 4; ++c) {
                const unsigned top = (unsigned)p00[c] * (unsigned)(256 - fx) + (unsigned)p01[c] * (unsigned)fx;
                const unsigned bot = (unsigned)p10[c] * (unsigned)(256 - fx) + (unsigned)p11[c] * (unsigned)fx;
                o[c] = (uint8_t)((top * (unsigned)(256 - fy) + bot * (unsigned)fy) >> 16);
            }
        }
    }
}

inline void pinchF(float* dst, ptrdiff_t dstPitch, const float* src, ptrdiff_t srcPitch, const Area& a)
{
    const int W = a.W, H = a.H;
    if (W <= 0 || H <= 0) return;
    auto px = [&](int x, int y) { return (const float*)((const uint8_t*)src + (size_t)y * (size_t)srcPitch) + (size_t)x * 4; };
    for (int y = 0; y < H; ++y) {
        float* o = (float*)((uint8_t*)dst + (size_t)y * (size_t)dstPitch);
        const double dy = rowD(a, y), dy2 = dy * dy;
        const double yrel = (double)(y - a.cyPix);
        for (int x = 0; x < W; ++x, o += 4) {
            const double xrel = (double)(x - a.cxPix);
            const double dx = xrel * a.k13;
            const double d2 = dx * dx + dy2;
            if (!(d2 < 1.0)) { std::memcpy(o, px(x, y), 16); continue; }
            const double k = pinchK(a, std::sqrt(d2));
            const double ysrc = (double)a.cyPix + yrel * k;
            const double xsrc = (double)a.cxPix + xrel * k;
            const int iy = floorMagic(ysrc);
            const float fy = (float)(ysrc - (double)iy), omfy = 1.0f - fy;
            const int ix = floorMagic(xsrc);
            const float fx = (float)(xsrc - (double)ix), omfx = 1.0f - fx;
            const int y0 = clampi(iy, 0, H - 1), y1 = clampi(iy + 1, 0, H - 1);
            const int x0 = clampi(ix, 0, W - 1), x1 = clampi(ix + 1, 0, W - 1);
            const float* p00 = px(x0, y0);
            const float* p01 = px(x1, y0);
            const float* p10 = px(x0, y1);
            const float* p11 = px(x1, y1);
            for (int c = 0; c < 4; ++c) {
                const float top = omfx * p00[c] + fx * p01[c];
                const float bot = omfx * p10[c] + fx * p11[c];
                o[c] = top * omfy + bot * fy;
            }
        }
    }
}

}  // namespace vfswirl
}  // namespace ofx
}  // namespace reno
