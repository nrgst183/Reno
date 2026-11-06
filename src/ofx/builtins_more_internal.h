// Shared by the topic files of the built-in effects of builtins_more.cpp: small helpers and the effect functions.
#pragma once
#include "../core/rmath.h"
#include "builtins_impl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ helpers
template <class F>
inline void perPixel(Ctx& c, F fn)
{
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            if (a <= 0.f) { std::memset(c.out.at(x, y), 0, sizeof(float) * 4); continue; }
            fn(r, g, b, x, y);
            premul(c.out.at(x, y), r, g, b, a);
        }
    });
}

// The reads and writes of VEGAS's OpenCL kernels, shared by the ported kernels (one rule, as the GPU does it). img: rows bottom first, RGBA floats, size W x H.
// A CLK_FILTER_LINEAR read at pixel coordinates (px, py) (the kernels add no half pixel): the card's texture unit weights the four neighbours (edges repeated) with the fractions rounded to
// 1/256 (NVIDIA hardware filtering, 8-bit fractional weights), blended as a + (b - a) * f along x, then along y.
inline void oclLinear4(const std::vector<float>& img, int W, int H, float px, float py, float* o)
{
    const float flx = std::floor(px), fly = std::floor(py);
    float fx = px - flx, fy = py - fly;
    fx = std::floor(fx * 256.f + 0.5f) / 256.f;
    fy = std::floor(fy * 256.f + 0.5f) / 256.f;
    const int x0 = (int)std::max(-2.0, std::min((double)W + 1, (double)flx)), y0 = (int)std::max(-2.0, std::min((double)H + 1, (double)fly));
    const int xa = std::max(0, std::min(W - 1, x0)), xb = std::max(0, std::min(W - 1, x0 + 1));
    const int ya = std::max(0, std::min(H - 1, y0)), yb = std::max(0, std::min(H - 1, y0 + 1));
    const float* a = &img[((size_t)ya * W + xa) * 4];
    const float* b = &img[((size_t)ya * W + xb) * 4];
    const float* cc = &img[((size_t)yb * W + xa) * 4];
    const float* d = &img[((size_t)yb * W + xb) * 4];
    for (int k = 0; k < 4; ++k) {
        const float t0 = a[k] + (b[k] - a[k]) * fx, t1 = cc[k] + (d[k] - cc[k]) * fx;
        o[k] = t0 + (t1 - t0) * fy;
    }
}
// write_imagef to an 8-bit UNORM picture kept in the kernel (the middle pictures): v kept in 0..1 and rounded to the nearest level.
inline float oclUnormLevel(float v, float add) { return std::floor(std::min(std::max(v, 0.f), 1.f) * 255.f + add) / 255.f; }
// The 8-bit store of a kernel's float result (write_imagef): the 12-bit step oclStore12 below, which every capture checked fits best (the older fitted floor(v * 255 + 0.46)
// was close but off by a level on a few percent of values). Kept as a name for the ports that call it.
inline float oclStore12(float v);
inline float oclWriteLevel(float v) { return oclStore12(v); }
// The 8-bit store of the kernels of Levels and Convolution Kernel (checked bit for bit on their captures; the same rule as Add Noise): the value kept in 0..1 is cut to 12 bits and
// scaled to a byte with the half down, byte = ((int)floor(v * 4096) * 255 + 2047) >> 12 (not a number: 0). Returns the level as a float level / 255.
inline float oclStore12(float v)
{
    if (!(v == v)) return 0.f;
    const int n = (int)std::floor(std::min(std::max(v, 0.f), 1.f) * 4096.f);
    return (float)((n * 255 + 2047) >> 12) / 255.f;
}


// A picture the way the OpenCL kernels hold it (rows bottom first, RGBA floats) and back (the Buf keeps the top row first).
inline void oclRowsOf(const Buf& b, std::vector<float>& v)
{
    v.resize((size_t)b.w * b.h * 4);
    for (int y = 0; y < b.h; ++y) std::memcpy(&v[(size_t)y * b.w * 4], b.at(0, b.h - 1 - y), (size_t)b.w * 4 * sizeof(float));
}
inline void oclRowsTo(const std::vector<float>& v, Buf& b, int W, int H)
{
    b.ensure(W, H);
    for (int y = 0; y < H; ++y) std::memcpy(b.at(0, H - 1 - y), &v[(size_t)y * W * 4], (size_t)W * 4 * sizeof(float));
}
// A setting for a kernel argument: a number that is not a number (a hand-edited nonsense value) counts as 0, and the value is held to lo .. hi.
inline double oclSetting(double v, double lo, double hi)
{
    if (!(v == v)) return 0.0;
    return std::min(std::max(v, lo), hi);
}

// The bicubic read of the OpenCL kernels (Catmull-Rom, a = -0.5, nearest reads with the edges repeated) at float pixel coordinates (px, py) of an RGBA picture W x H, rows bottom first.
inline void oclBicubic4(const std::vector<float>& img, int W, int H, float px, float py, float* o)
{
    const float fl_x = std::floor(px), fl_y = std::floor(py);
    const float tx = px - fl_x, ty = py - fl_y;
    const int ix = (int)std::max(-4.0, std::min((double)W + 4, (double)fl_x)), iy = (int)std::max(-4.0, std::min((double)H + 4, (double)fl_y));
    auto cubic = [](float t, float* w) {
        const float a = -0.5f;
        const float u = t + 1.f, v = 2.f - t, s = 1.f - t;
        w[0] = ((a * u - 5.f * a) * u + 8.f * a) * u - 4.f * a;
        w[1] = ((a + 2.f) * t - (a + 3.f)) * t * t + 1.f;
        w[2] = ((a + 2.f) * s - (a + 3.f)) * s * s + 1.f;
        w[3] = ((a * v - 5.f * a) * v + 8.f * a) * v - 4.f * a;
    };
    float wx[4], wy[4];
    cubic(tx, wx);
    cubic(ty, wy);
    for (int k = 0; k < 4; ++k) o[k] = 0.f;
    for (int j = 0; j < 4; ++j) {
        const int yy = std::max(0, std::min(H - 1, iy - 1 + j));
        for (int i = 0; i < 4; ++i) {
            const int xx = std::max(0, std::min(W - 1, ix - 1 + i));
            const float* p = &img[((size_t)yy * W + xx) * 4];
            const float w = wy[j] * wx[i];
            for (int k = 0; k < 4; ++k) o[k] += w * p[k];
        }
    }
}

inline void copyThrough(Ctx& c) { c.out.px = c.src.px; }

inline void rgbToHsl(float r, float g, float b, float& h, float& s, float& l)
{
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    l = (mx + mn) * 0.5f;
    if (mx - mn < 1e-6f) { h = s = 0; return; }
    const float d = mx - mn;
    s = l > 0.5f ? d / std::max(1e-6f, 2.f - mx - mn) : d / std::max(1e-6f, mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / d + 2.f;
    else h = (r - g) / d + 4.f;
    h /= 6.f;
}
inline float hue2rgb(float p, float q, float t)
{
    t -= std::floor(t);
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}
inline void hslToRgb(float h, float s, float l, float& r, float& g, float& b)
{
    if (s <= 0) { r = g = b = l; return; }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue2rgb(p, q, h + 1.f / 3);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.f / 3);
}

// sRGB <-> CIE Lab (D65), L 0..100
inline float srgbToLin(float v) { return v <= 0.04045f ? v / 12.92f : rm::pow((v + 0.055f) / 1.055f, 2.4f); }
inline float linToSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * rm::pow(std::max(0.f, v), 1.f / 2.4f) - 0.055f; }
inline void rgbToLab(float r, float g, float b, float& L, float& A, float& B)
{
    const float lr = srgbToLin(r), lg = srgbToLin(g), lb = srgbToLin(b);
    float X = (0.4124f * lr + 0.3576f * lg + 0.1805f * lb) / 0.95047f;
    float Y = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
    float Z = (0.0193f * lr + 0.1192f * lg + 0.9505f * lb) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? rm::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    X = f(X); Y = f(Y); Z = f(Z);
    L = 116.f * Y - 16.f; A = 500.f * (X - Y); B = 200.f * (Y - Z);
}

// The frame's scale against 1080 lines: sizes given for an HD frame stay in proportion at other sizes.
inline double frameScale(const Ctx& c) { return std::max(1, c.out.h) / 1080.0; }
// ------------------------------------------------------------------ the effects (each in the file of its topic)
// builtins_more_blur.cpp
void quickBlur(Ctx& c);
void radialBlur(Ctx& c);
void defocus(Ctx& c);
void unsharpMask(Ctx& c);
void median(Ctx& c);
// Unsharp Mask's blur (VEGAS's Gaussian for its Radius); none: VEGAS leaves the picture as it is
double unsharpSigma(double radius, int W, int H, bool& none);
void minAndMax(Ctx& c);
void chromaBlur(Ctx& c);
// builtins_more_colour.cpp
void saturationAdjust(Ctx& c);
void softContrast(Ctx& c);
void whiteBalance(Ctx& c);
void whiteBalanceChanged(Change& ch);
void whiteBalanceCreated(Change& ch);
void labAdjust(Ctx& c);
void blackRestore(Ctx& c);
void broadcastColors(Ctx& c);
void channelBlend(Ctx& c);
// builtins_more_creative.cpp
void blackBarFill(Ctx& c);
void cropFx(Ctx& c);
void pictureInPicture(Ctx& c);
void filmGrain(Ctx& c);
void filmEffects(Ctx& c);
void vignette(Ctx& c);
void tvSimulator(Ctx& c);
void radialPixelate(Ctx& c);
void maskGenerator(Ctx& c);
void timecodeFx(Ctx& c);
void layerDimensionality(Ctx& c);
void deform(Ctx& c);
void bumpMap(Ctx& c);
// builtins_more_light.cpp
void lensFlare(Ctx& c);
void lightRays(Ctx& c);
void gradientMap(Ctx& c);
// builtins_velvet.cpp: VEGAS's managed Rays and Glint, as its CoreGraphics filters do them
void raysVelvet(Ctx& c);
void glintVelvet(Ctx& c);
void fillLightVelvet(Ctx& c);
void starburstVelvet(Ctx& c);
// builtins_velvet.cpp: EllipseMask / RectangleMask's soft shape, coverage 0..1 per pixel (see there)
void shapeMaskVelvet(int W, int H, double xpos, double ypos, double w, double h, double corner, double blur, bool ellipse, std::vector<float>& m);

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
