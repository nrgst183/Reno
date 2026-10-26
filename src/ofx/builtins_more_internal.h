// Shared by the topic files of the built-in effects of builtins_more.cpp: small helpers and the effect functions.
#pragma once
#include "builtins_impl.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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

inline void copyThrough(Ctx& c) { c.out.px = c.src.px; }

// out = src + (effect - src) x k
inline void blendOver(Ctx& c, const Buf& effect, float k)
{
    if (k >= 0.999f) { c.out.px = effect.px; return; }
    for (size_t i = 0; i < c.out.px.size(); ++i) c.out.px[i] = c.src.px[i] + (effect.px[i] - c.src.px[i]) * k;
}

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
inline float srgbToLin(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
inline float linToSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(std::max(0.f, v), 1.f / 2.4f) - 0.055f; }
inline void rgbToLab(float r, float g, float b, float& L, float& A, float& B)
{
    const float lr = srgbToLin(r), lg = srgbToLin(g), lb = srgbToLin(b);
    float X = (0.4124f * lr + 0.3576f * lg + 0.1805f * lb) / 0.95047f;
    float Y = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
    float Z = (0.0193f * lr + 0.1192f * lg + 0.9505f * lb) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    X = f(X); Y = f(Y); Z = f(Z);
    L = 116.f * Y - 16.f; A = 500.f * (X - Y); B = 200.f * (Y - Z);
}
inline void labToRgb(float L, float A, float B, float& r, float& g, float& b)
{
    float Y = (L + 16.f) / 116.f, X = Y + A / 500.f, Z = Y - B / 200.f;
    auto fi = [](float t) { const float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.f / 116.f) / 7.787f; };
    X = fi(X) * 0.95047f; Y = fi(Y); Z = fi(Z) * 1.08883f;
    const float lr = 3.2406f * X - 1.5372f * Y - 0.4986f * Z, lg = -0.9689f * X + 1.8758f * Y + 0.0415f * Z, lb = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
    r = linToSrgb(lr); g = linToSrgb(lg); b = linToSrgb(lb);
}

// The frame's scale against 1080 lines: sizes given for an HD frame stay in proportion at other sizes.
inline double frameScale(const Ctx& c) { return std::max(1, c.out.h) / 1080.0; }
// The same for the full-size frame (a smaller preview's size divided by its scale): for sizes in full-size pixels that are
// then scaled by hand (rounded to whole pixels at the full size first, or pixel patterns).
inline double frameScaleFull(const Ctx& c) { return std::max(1, c.out.h) / c.scale / 1080.0; }
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
