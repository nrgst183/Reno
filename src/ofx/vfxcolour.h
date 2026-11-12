// Invert, Black and White, Sepia, Color Balance, Saturation Adjust, LAB Adjust, Broadcast Colors and Chroma Keyer as VEGAS's OpenCL kernels (Vfx1.ofx, programs 27, 05, 41, 14, 40, 28, 09, 13)
// draw them with GPU acceleration on: float32 arithmetic on the premultiplied pixel values as the picture stores them, one pixel at a time, nothing cut to whole levels between the steps;
// the store into an 8-bit frame is more::oclStore12 (done by the caller). Re-implemented from the kernels' arithmetic (notes: vegas_code/Colour_Kernels.md).
// No Reno types in here, so the standalone tests can include it.
#pragma once
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vcol {

inline float mixf(float x, float y, float a) { return x + (y - x) * a; }
// level / 255 for each level, made by the compiler: readLevels runs on every channel of every pixel, and the table is
// the same division without its cost (and without floor(), a library call on plain x86-64).
struct ByteLevels { float level[256]; };
constexpr ByteLevels makeByteLevels()
{
    ByteLevels t{};
    for (int n = 0; n < 256; ++n) t.level[n] = (float)n / 255.0f;
    return t;
}
inline constexpr ByteLevels kByteLevels = makeByteLevels();

// floor(v * 255 + 0.5) / 255: for 0 <= x < 256 the cast is the floor and the table holds the division's result.
inline float readLevel(float v)
{
    const float x = v * 255.0f + 0.5f;
    if (x >= 0.0f && x < 256.0f) return kByteLevels.level[(int)x];
    return std::floor(x) / 255.0f;
}
// An 8-bit frame is read by the kernels as level / 255 (a division); Reno's loader makes its floats as level * (1 / 255), which is a unit in the last place off for half of the levels.
inline void readLevels(const float* p, float* q)
{
    for (int k = 0; k < 4; ++k) q[k] = readLevel(p[k]);
}
inline float clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

// ---------------------------------------------------------------------------------------------------------------------------------- Brightness and Contrast
// Two steps of the kernel (Vfx1.ofx program 08), on the premultiplied pixel: the brightness is added to the straight colour when it is not (nearly) zero, then the contrast scales the
// straight colour about the centre. Nothing is cut to 0..1 between the steps or at the end (the store does that). A pixel with no alpha becomes all zero.
// bright / contrast / centre = Brightness / Contrast / ContrastCenter as floats.
inline void brightContrastPx(float bright, float contrast, float centre, const float* p, float* o)
{
    float c[4] = {p[0], p[1], p[2], p[3]};
    if (std::fabs(bright) > 0.000001f) {
        if (c[3] > 0.000001f) {
            if (c[3] < 1.0f) {
                const float a = c[3];
                for (int k = 0; k < 3; ++k) c[k] = (c[k] / a + bright) * a;
            } else {
                for (int k = 0; k < 3; ++k) c[k] = c[k] + bright;
                c[3] = 1.0f;
            }
        } else {
            c[0] = c[1] = c[2] = c[3] = 0.0f;
        }
    }
    const float f = (contrast > 0.0f) ? (1.0f / (1.0f + 0.000001f - contrast)) : (contrast + 1.0f);
    if (c[3] > 0.000001f) {
        if (c[3] < 1.0f) {
            const float a = c[3];
            for (int k = 0; k < 3; ++k) o[k] = ((c[k] / a - centre) * f + centre) * a;
            o[3] = a;
        } else {
            for (int k = 0; k < 3; ++k) o[k] = (c[k] - centre) * f + centre;
            o[3] = 1.0f;
        }
    } else {
        o[0] = o[1] = o[2] = o[3] = 0.0f;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------- Color Corrector
// The kernel of Vfx1.ofx program 15, on the premultiplied pixel, in floats (the card's native_powr / native_exp / native_cos / native_sin are done with the plain float functions).
// The straight colour goes to Y, U = 0.492 (B - Y), V = 0.877 (R - Y); gain / gamma / offset change Y alone (a curve with a straight piece round zero); each wheel then adds a tint to
// (U, V), a Gaussian of the NEW Y at 0 / 0.5 / 1 times amount squared times 0.5 in the wheel's direction; the saturation scales the sum; R, G, B are rebuilt and multiplied by the alpha.
struct CcKernel {
    bool gcurve = false, wheels = false, sat = false;   // gain / gamma / offset, tints, saturation
    float ang[3] = {0, 0, 0}, amt[3] = {0, 0, 0};       // the wheels (degrees, amount)
    float offset = 0, gain = 1, satv = 1;               // offset in 0..1 units
    float g = 1, A = 100, B = 0, P = 1, C = 1;          // the gamma curve (power, knee, offset, slope, scale)
};
inline float ccCurveF(const CcKernel& s, float x)
{
    if (s.g <= 1.0f) {
        if (x >= s.A) return (1.0f + s.B) * reno::rm::pow(x, s.g) - s.B;
        if (x > -1.0f * s.A) return x * s.P;
        return ((-1.0f) - s.B) * reno::rm::pow((-1.0f) * x, s.g) + s.B;
    }
    if (x >= s.A) return reno::rm::pow((x + s.B) * s.C, s.g);
    if (x > -1.0f * s.A) return x * s.P;
    return (-1.0f) * reno::rm::pow((x - s.B) * ((-1.0f) * s.C), s.g);
}
inline void ccKernelPx(const CcKernel& s, const float* p, float* o)
{
    float a = p[3];
    if (a > 1.0f) a = 1.0f;
    if (!(a > 0.000001f)) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    const float r = p[0] / a, g = p[1] / a, b = p[2] / a;
    float Y = (0.299f * r) + (0.587f * g) + (0.114f * b);
    float U = (0.492f) * (b - Y);
    float V = (0.877f) * (r - Y);
    const float pi = 3.14159265358979323846f;
    float ty = Y, tu = 0.0f, tv = 0.0f, tw = s.satv;
    if (s.gcurve || s.wheels) {
        ty = ccCurveF(s, Y);
        ty *= s.gain;
        ty += s.offset;
        if (s.wheels) {
            const float cen[3] = {0.0f, 0.5f, 1.0f};
            const float sg[3] = {0.283107266761f, 0.17f, 0.283107266761f};
            for (int w = 0; w < 3; ++w) {
                const float cs = reno::rm::cos(s.ang[w] * pi / 180.0f), sn = reno::rm::sin(s.ang[w] * pi / 180.0f);
                const float a2 = s.amt[w] * s.amt[w];
                const float k = 0.5f / (sg[w] * sg[w]);
                const float x = ty - cen[w];
                const float wt = a2 * reno::rm::exp(-x * x * k);
                const float du = cs * wt * 0.5f, dv = sn * wt * 0.5f;
                if (w == 0) { tu = du; tv = dv; } else { tu += du; tv += dv; }
            }
        }
    }
    Y = ty;
    U += tu;
    V += tv;
    if (s.sat) { U *= tw; V *= tw; }
    float R = Y + ((1.0f / 0.877f) * V);
    float G = Y + ((-0.114f / (0.587f * 0.492f)) * U) + ((-0.299f / (0.587f * 0.877f)) * V);
    float B = Y + ((1.0f / 0.492f) * U);
    o[0] = R * a; o[1] = G * a; o[2] = B * a; o[3] = a;
}

// ---------------------------------------------------------------------------------------------------------------------------------- Color Corrector (Secondary)
// The kernels of Vfx1.ofx program 16, on the premultiplied pixel, in floats. Three kernels: the primary-like one (mode 1: gain / gamma / offset, saturation, a chrominance tint and an optional
// luminance window), the general one (mode 2: windows on luminance, chroma and hue, a rotation of the colour, an alpha) and the mask display (mode 3).
struct CcsWin {
    float lo = 0, hi = 0, sl = 0, sh = 0, k1 = 0, k2 = 0;
    void make(float lo0, float hi0, float smooth, float cap, bool sameSmooth = false)
    {
        lo = std::min(std::max(lo0, 0.0f), cap);
        hi = std::min(std::max(hi0, lo), cap);
        sl = std::min(lo, smooth);
        sh = sameSmooth ? sl : std::min(cap - hi, smooth);
        k1 = k2 = 0.0f;
        if (0.0f < sl) { float v = 0.3f * sl; k1 = -1.0f / (2.0f * v * v); }
        if (0.0f < sh) { float v = 0.3f * sh; k2 = -1.0f / (2.0f * v * v); }
    }
    float at(float y) const
    {
        if (y < lo && 0.0f != sl) { const float d = lo - y; const float a = d * d * k1; return (-15.0f > a) ? 0.0f : reno::rm::exp(a); }
        if (y > hi && 0.0f != sh) { const float d = hi - y; const float a = d * d * k2; return (-15.0f > a) ? 0.0f : reno::rm::exp(a); }
        if (y >= lo && y <= hi) return 1.0f;
        return 0.0f;
    }
};
struct CcsKernel {
    int mode = 1;                       // 1 primary-like, 2 general, 3 mask display
    bool inv = false, satOn = false, lumOn = false;
    float ang = 0, amt = 0, tintU = 0, tintV = 0;       // the chrominance tint (angle in degrees, amount; the tint of mode 2 from the host)
    float satv = 1, alphaF = 1, gain = 1, offset = 0;
    float g = 1, A = 100, B = 0, P = 1, C = 1;          // the gamma curve (as CcKernel)
    CcsWin lum, sat, hue;
    float hueShift = 0;                                  // 180 - the hue centre
    float M[4] = {1, 0, 0, 1};                           // saturation times the rotation
};
inline float ccsCurve(const CcsKernel& s, float x)
{
    CcKernel k;
    k.g = s.g; k.A = s.A; k.B = s.B; k.P = s.P; k.C = s.C;
    return ccCurveF(k, x);
}
inline void ccsPx(const CcsKernel& s, const float* p, float* o)
{
    float a = p[3];
    if (a > 1.0f) a = 1.0f;
    if (!(a > 0.000001f)) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    float r = p[0], g = p[1], b = p[2];
    if (a < 1.0f) { r /= a; g /= a; b /= a; }
    float y = (0.299f * r) + (0.587f * g) + (0.114f * b);
    float U = (0.492f) * (b - y);
    float V = (0.877f) * (r - y);
    const float pi = 3.1415926535897932384f;
    if (s.mode == 1) {
        float ty = y, tu, tv, tw = s.satv;
        {
            const float cs = reno::rm::cos(s.ang * pi / 180.0f), sn = reno::rm::sin(s.ang * pi / 180.0f);
            const float a2 = s.amt * s.amt;
            tu = cs * a2 * 0.5f;
            tv = sn * a2 * 0.5f;
        }
        ty = ccsCurve(s, y);
        ty *= s.gain;
        ty += s.offset;
        if (s.lumOn) {
            const float yy = y * 255.0f;
            float m = s.lum.at(yy);
            if (s.inv) m = 1.0f - m;
            ty = (float)(y + (m * ((float)ty - y)));
            tu = (float)(m * tu);
            tv = (float)(m * tv);
            tw = (float)(1.0f + (m * (tw - 1.0f)));
        }
        U += tu;
        V += tv;
        if (s.satOn) { U *= tw; V *= tw; }
        const float R = ty + ((1.0f / 0.877f) * V);
        const float G = ty + ((-0.114f / (0.587f * 0.492f)) * U) + ((-0.299f / (0.587f * 0.877f)) * V);
        const float Bl = ty + ((1.0f / 0.492f) * U);
        o[0] = R * a; o[1] = G * a; o[2] = Bl * a; o[3] = a;
        return;
    }
    // the masks (the kernels' f2)
    float chroma = std::sqrt((U * U) + (V * V));
    float ratio = 0.0f;
    int oct = 0;
    if (0.0f < chroma) {
        const float aU = std::fabs(U), aV = std::fabs(V);
        if (aU < aV) { ratio = aU; oct = 4; } else { ratio = aV; }
        ratio = ratio / chroma;
        if (V < 0.0f) oct += 2;
        if (U < 0.0f) oct += 1;
    }
    const float lumM = s.lum.at(y * 255.0f);
    const float satM = s.sat.at(chroma * 255.0f);
    float ang[8];
    {
        const float aa = reno::rm::asin(ratio) * (180.0f / pi);
        const float bb = 90.0f - aa;
        ang[0] = aa; ang[4] = bb; ang[1] = 180.0f - aa; ang[5] = 180.0f - bb; ang[2] = 360.0f - aa; ang[6] = 360.0f - bb; ang[3] = 180.0f + aa; ang[7] = 180.0f + bb;
    }
    float h = ang[oct] + s.hueShift;
    if (0.0f > h) h += 360.0f;
    else if (360.0f < h) h -= 360.0f;
    const float hueM = s.hue.at(h);
    float dy = 0.0f;
    if (0.0f < lumM * satM * hueM) {
        float v = ccsCurve(s, y);
        v *= s.gain;
        v += s.offset;
        dy = v - y;
    }
    const float m = lumM * satM * hueM;
    const float inv = s.inv ? 1.0f : 0.0f;
    if (s.mode == 3) {
        float v = (inv - m) * (s.inv ? 1.0f : -1.0f);
        v = std::min(std::max(v, 0.0f), 1.0f);
        o[0] = o[1] = o[2] = o[3] = v;
        return;
    }
    float w = std::fabs(m - inv);
    if (0.0f < w) {
        const float w1 = 1.0f - w;
        y += dy;
        float u2 = U + s.tintU, v2 = V + s.tintV;
        const float nu = s.M[0] * u2 + s.M[1] * v2;
        const float nv = s.M[2] * u2 + s.M[3] * v2;
        float na = a;
        const float R = y + ((1.0f / 0.877f) * nv);
        const float G = y + ((-0.114f / (0.587f * 0.492f)) * nu) + ((-0.299f / (0.587f * 0.877f)) * nv);
        const float Bl = y + ((1.0f / 0.492f) * nu);
        w *= s.alphaF;
        na = na * w + a * w1;
        const float nr = R * w + r * w1, ng = G * w + g * w1, nb = Bl * w + b * w1;
        na = std::min(std::max(na, 0.0f), 1.0f);
        o[0] = nr * a; o[1] = ng * a; o[2] = nb * a; o[3] = na;
    } else {
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------- Invert
// t = BlendAmount (as a float). The colour is divided by the alpha, turned to 1 - c, multiplied by the alpha again and mixed with the picture by t.
inline void invertPx(float t, const float* p, float* o)
{
    const float w = p[3];
    for (int k = 0; k < 3; ++k) {
        const float st = (w < 0.000001f) ? 0.0f : (p[k] / w);
        const float v = (1.0f - st) * w;
        o[k] = mixf(p[k], v, t);
    }
    o[3] = w;
}

// ---------------------------------------------------------------------------------------------------------------------------------- Black and White
// k = float(1.0 - BlendAmount) (the host hands the kernel the share of the original picture): the kernel's mix goes from the grey (k = 0) to the picture (k = 1).
// The grey is the plain mean of the three values; the alpha is copied.
inline void blackWhitePx(float k, const float* p, float* o)
{
    if (std::fabs(1.0f - k) > 0.000001f) {
        const float g = (p[0] + p[1] + p[2]) / 3.0f;
        for (int i = 0; i < 3; ++i) o[i] = mixf(g, p[i], k);
        o[3] = p[3];
    } else {
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------- Sepia
// col = the Color parameter, strength / falloff = BlendingStrength / BlendingFalloff, all as floats. Up to a third of the strength the picture moves towards its grey; up to two thirds the
// same, then towards the colour by a weight that falls with the grey; above that the grey itself moves towards the colour.
struct SepiaSettings { float col[3]; float strength, falloff; };
inline void sepiaPx(const SepiaSettings& s, const float* p, float* o)
{
    const float a = p[3];
    const float v4 = s.strength;
    const float v10 = s.falloff * -1.0f;
    if (!(a > 0.000001f)) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    float c[3] = {p[0], p[1], p[2]};
    const bool straight = a < 1.0f;
    if (straight) for (int k = 0; k < 3; ++k) c[k] /= a;
    float r[3];
    if (v4 <= (1.0f / 3.0f)) {
        const float v11 = v4 * 1.5f;
        const float g = (c[0] + c[1] + c[2]) / 3.0f;
        for (int k = 0; k < 3; ++k) r[k] = mixf(c[k], g, v11);
    } else if (v4 <= (2.0f / 3.0f)) {
        const float v11 = v4 * 1.5f;
        const float v13 = (v4 - (1.0f / 3.0f)) * 1.5f;
        float g = (c[0] + c[1] + c[2]) / 3.0f;
        for (int k = 0; k < 3; ++k) r[k] = mixf(c[k], g, v11);
        g = clamp01(g);
        const float v14 = clamp01((g * v10) + v13);
        for (int k = 0; k < 3; ++k) r[k] = mixf(r[k], s.col[k], v14);
    } else {
        const float v13 = (v4 - 1.0f / 3.0f) * 1.5f;
        const float g = (c[0] + c[1] + c[2]) / 3.0f;
        const float g2 = clamp01(g);
        const float v14 = clamp01((g2 * v10) + v13);
        for (int k = 0; k < 3; ++k) r[k] = mixf(g, s.col[k], v14);
    }
    if (straight) for (int k = 0; k < 3; ++k) r[k] *= a;
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
    o[3] = straight ? a : 1.0f;
}

// ---------------------------------------------------------------------------------------------------------------------------------- Color Balance
// r, g, b = the three sliders; type = the Type choice (2: a weight of 1 - grey^2, 1: 1 - (2 (grey - 0.5))^2, else 1 - (1 - grey)^2); preserve = PreserveLuminosity (exactly 1).
// The weight (from the plain mean of the three values) times the slider is added; with PreserveLuminosity half of the other two sliders' share is taken off. The alpha is untouched.
struct BalanceSettings { float r, g, b; int type, preserve; };
inline void colorBalancePx(const BalanceSettings& s, const float* p, float* o)
{
    const float m = (p[0] + p[1] + p[2]) / 3.0f;
    float w;
    if (s.type == 2) w = 1.0f - (m) * (m);
    else if (s.type == 1) w = 1.0f - (m - 0.5f) * 2.0f * (m - 0.5f) * 2.0f;
    else w = 1.0f - (1.0f - m) * (1.0f - m);
    float a, b, c;
    if (s.preserve == 1) {
        a = (w * s.r) - (w * s.b / 2.0f) - (w * s.g / 2.0f);
        b = (w * s.g) - (w * s.b / 2.0f) - (w * s.r / 2.0f);
        c = (w * s.b) - (w * s.g / 2.0f) - (w * s.r / 2.0f);
    } else {
        a = w * s.r; b = w * s.g; c = w * s.b;
    }
    o[0] = p[0] + a;
    o[1] = p[1] + b;
    o[2] = p[2] + c;
    o[3] = p[3];
}

// ---------------------------------------------------------------------------------------------------------------------------------- Saturation Adjust
// amount, centre, spread, low, high = the five sliders (floats). A cubic Hermite curve over the colour's distance from its Rec.601 grey (scaled by 1 / 0.9272) gives the change of
// saturation: low below centre - spread / 2, amount at the centre, high above centre + spread / 2. The picture is mixed from its grey by 1 + that. The alpha is copied.
struct SatSettings { float amount, centre, spread, low, high; };
inline float satCurve(float x, float lo, float hi, float am, float ce, float sp)
{
    const float k0 = 0.0f;
    const float k1 = std::max(0.0f, ce - sp / 2.0f);
    const float k2 = ce;
    const float k3 = std::min(ce + sp / 2.0f, 1.0f);
    const float k4 = 1.0f;
    float a, b, t;
    if (x < k1) { a = lo; b = lo; t = (k1 == k0) ? 0.0f : ((x - k0) / (k1 - k0)); }
    else if (x < k2) { a = lo; b = am; t = (k2 == k1) ? 0.0f : ((x - k1) / (k2 - k1)); }
    else if (x < k3) { a = am; b = hi; t = (k3 == k2) ? 0.0f : ((x - k2) / (k3 - k2)); }
    else { a = hi; b = hi; t = (k4 == k3) ? 0.0f : ((x - k3) / (k4 - k3)); }
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * a + (-2.0f * t3 + 3.0f * t2) * b;
}
inline void saturationPx(const SatSettings& s, const float* p, float* o)
{
    const float y = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    const float q = (y - p[0]) * (y - p[0]) + (y - p[1]) * (y - p[1]) + (y - p[2]) * (y - p[2]);
    const float dist = std::sqrt(q);
    const float f = satCurve(dist / 0.92720386107910491f, s.low, s.high, s.amount, s.centre, s.spread) + 1.0f;
    const float w[3] = {(1.0f - f) * 0.299f, (1.0f - f) * 0.587f, (1.0f - f) * 0.114f};
    o[0] = (w[0] + f) * p[0] + w[1] * p[1] + w[2] * p[2];
    o[1] = w[0] * p[0] + (w[1] + f) * p[1] + w[2] * p[2];
    o[2] = w[0] * p[0] + w[1] * p[1] + (w[2] + f) * p[2];
    o[3] = p[3];
}

// ---------------------------------------------------------------------------------------------------------------------------------- LAB Adjust
// lightness = Lightness, a / b = Channela / Channelb times 100, invert = InvertLuminance, desat = 1 - Desaturate (all floats). sRGB (D65) to CIE L*a*b* and back with the old 0.008856 / 7.787
// steps, L turned to 95.580666 - L first when asked, L times Lightness, a and b moved and scaled. A partly transparent pixel is divided by its alpha first; the kernel multiplies
// only the red result by the alpha (three times) and leaves green and blue straight; an opaque pixel is used as it is.
struct LabSettings { float lightness, a, b; int invert; float desat; };
inline void labFromRgb(const float* c, float* lab)
{
    float v[3];
    for (int k = 0; k < 3; ++k) v[k] = (c[k] > 0.04045f) ? reno::rm::pow((c[k] + 0.055f) / 1.055f, 2.4f) : c[k] / 12.92f;
    float x = v[0] * 0.4124f + v[1] * 0.3576f + v[2] * 0.1805f;
    float y = v[0] * 0.2126f + v[1] * 0.7152f + v[2] * 0.0722f;
    float z = v[0] * 0.0193f + v[1] * 0.1192f + v[2] * 0.9505f;
    x = x / 0.95047f;
    z = z / 1.08883f;
    x = x > 0.008856f ? reno::rm::cbrt(x) : (7.787f * x) + (16.0f / 116.0f);
    y = y > 0.008856f ? reno::rm::cbrt(y) : (7.787f * y) + (16.0f / 116.0f);
    z = z > 0.008856f ? reno::rm::cbrt(z) : (7.787f * z) + (16.0f / 116.0f);
    lab[0] = 116.0f * y - 16.0f;
    lab[1] = 500.0f * (x - y);
    lab[2] = 200.0f * (y - z);
}
inline void rgbFromLab(const float* lab, float* c)
{
    float y = (lab[0] + 16.0f) / 116.0f;
    float x = lab[1] / 500.0f + y;
    float z = y - lab[2] / 200.0f;
    const float cx = x * x * x, cy = y * y * y, cz = z * z * z;
    x = cx > 0.008856f ? cx : (x - 16.0f / 116.0f) / 7.787f;
    y = cy > 0.008856f ? cy : (y - 16.0f / 116.0f) / 7.787f;
    z = cz > 0.008856f ? cz : (z - 16.0f / 116.0f) / 7.787f;
    x *= 0.95047f;
    z *= 1.08883f;
    float v[3];
    v[0] = x * 3.2406f + y * -1.5372f + z * -0.4986f;
    v[1] = x * -0.9689f + y * 1.8758f + z * 0.0415f;
    v[2] = x * 0.0557f + y * -0.2040f + z * 1.0570f;
    for (int k = 0; k < 3; ++k) c[k] = (v[k] > 0.0031308f) ? (1.055f * reno::rm::pow(v[k], 1.0f / 2.4f) - 0.055f) : (12.92f * v[k]);
}
inline void labPx(const LabSettings& s, const float* p, float* o)
{
    const float a = p[3];
    if (!(a > 0.000001f)) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    float c[3] = {p[0], p[1], p[2]};
    const bool part = a < 1.0f;
    if (part) for (int k = 0; k < 3; ++k) c[k] /= a;
    float lab[3];
    labFromRgb(c, lab);
    if (s.invert) lab[0] = 95.580666f - lab[0];
    lab[0] *= s.lightness;
    lab[1] += s.a;
    lab[2] += s.b;
    lab[1] *= s.desat;
    lab[2] *= s.desat;
    rgbFromLab(lab, o);
    if (part) { o[0] *= a; o[0] *= a; o[0] *= a; }
    o[3] = part ? a : 1.0f;
}

// ---------------------------------------------------------------------------------------------------------------------------------- Broadcast Colors
// The ten sliders and two switches as the host hands them to the kernel (doubles to floats, the switches as they are). Everything is in a 0..255 video-level scale: luma Y' with the
// 7.5 IRE set-up added, chroma (U, V) scaled the same, composite = Y' + 2 |chroma| style limits. Every limit has a soft knee: a cubic through the limit that starts earlier by
// (range * smoothness).
struct BroadcastSettings {
    float lumaMin, lumaMinSmooth, lumaMax, lumaMaxSmooth, chromaMax, chromaSmooth, compMin, compMinSmooth, compMax, compMaxSmooth;
    int setup75, studio;
};
namespace bc_detail {
inline void cubicFit(float v1, float v2, float v3, float v4, float v5, float* c3, float* c2)
{
    *c3 = ((v4 - v1 - v2 * v3) - (v5 - v2) * (v3 / 2.0f)) / (v3 * v3 * v3 / -2.0f);
    *c2 = ((v4 - v1 - v2 * v3) - (v5 - v2) * (v3 / 3.0f)) / (v3 * v3 / 3.0f);
}
}
// The frame-wide numbers the kernel works out for every pixel again (they do not depend on it).
struct BroadcastFrame {
    float v18, v19, v21, v22, v23, v72;
    float lumaLo0, lumaLo1, lumaHi0, lumaHi1;   // v34, v35, v38, v37 of the luma curve
    float lumaKneeLo, lumaKneeHi;                // v24, v25 (the limits themselves)
    float lumaMid;                               // v39
    float chromaStart, chromaEnd, chromaLimit;   // v38, v37, v48 of the chroma curve
    float chromaCub3, chromaCub2;
    float v79, v47;
    float lumaLoCub3, lumaLoCub2, lumaHiCub3, lumaHiCub2;
    float compLoStart, compLoEnd, compHiStart, compHiEnd;
    float compLoCub3, compLoCub2, compHiCub3, compHiCub2;
    float v80, v81;
};
inline void broadcastPrepare(const BroadcastSettings& s, BroadcastFrame& f)
{
    using namespace bc_detail;
    f.v18 = s.setup75 ? 7.5f : 0.0f;
    float v19 = 16.0f, v20 = 235.0f;
    if (0 == s.studio) { v19 = 0.0f; v20 = 255.0f; }
    f.v19 = v19;
    f.v21 = v20 - v19;
    f.v22 = f.v21 / ((100.0f - f.v18));
    f.v23 = ((100.0f - f.v18)) / f.v21;
    f.v72 = f.v23 * f.v23;       // (the kernel's main function calls (100 - setup) / range v22, the helper functions call it v23)
    // ---- luma curve (kernel function f1)
    const float v24 = s.lumaMin, v25 = s.lumaMax, v12 = s.lumaMinSmooth, v13 = s.lumaMaxSmooth;
    float v34 = v24, v35 = v24;
    float v37 = v25, v38 = v25;
    const float v39 = (v24 + v25) / 2.0f;
    f.lumaMid = v39;
    const float v41 = 0.299f * (0.0f - v19) * f.v23 + 0.587f * (0.0f - v19) * f.v23 + 0.114f * (0.0f - v19) * f.v23 + f.v18;
    const float v42 = 0.299f * (255.0f - v19) * f.v23 + 0.587f * (255.0f - v19) * f.v23 + 0.114f * (255.0f - v19) * f.v23 + f.v18;
    if (v41 + 2.0f < v24 && 0.0f < v12) {
        float v40 = (v24 - v41);
        float v27 = 1.0f;
        if (v24 - v41 > v39 - v24) v27 = (v39 - v24) / (v24 - v41);
        v35 += v40 * v27 * v12;
        if (v35 == v24) {
            v34 = v24;
        } else {
            v34 -= v40;
            v34 = std::fmax(v34, (-8.0f));
            v35 = std::fmin(v35, v39);
            if ((v35 - v34) / 3.0f >= v35 - v24) v34 = v35 - ((v35 - v24) * 3.0f);
        }
    }
    if (v25 + 2.0f < v42 && 0.0f < v13) {
        float v40 = v42 - v25;
        float v27 = 1.0f;
        if (v42 - v25 > v25 - v39) v27 = (v25 - v39) / (v42 - v25);
        v38 -= v40 * v27 * v13;
        if (v38 == v25) {
            v37 = v25;
        } else {
            v37 += v40;
            v38 = std::fmax(v38, v39);
            v37 = std::fmin(v37, (110.0f));
            if ((v37 - v38) / 3.0f >= v25 - v38) v37 = v38 + ((v25 - v38) * 3.0f);
        }
    }
    f.lumaLo0 = v34; f.lumaLo1 = v35; f.lumaHi0 = v38; f.lumaHi1 = v37;
    f.lumaKneeLo = v24; f.lumaKneeHi = v25;
    f.lumaLoCub3 = f.lumaLoCub2 = f.lumaHiCub3 = f.lumaHiCub2 = 0.0f;
    if (v34 < v35) cubicFit(v24, 0.0f, v35 - v34, v35, 1.0f, &f.lumaLoCub3, &f.lumaLoCub2);
    if (v38 < v37) cubicFit(v25, 0.0f, v38 - v37, v38, 1.0f, &f.lumaHiCub3, &f.lumaHiCub2);
    // ---- chroma curve (kernel function f2, with the pixel's chroma left out): v38 / v37 / v48
    {
        const float v43 = s.chromaMax, v44 = s.chromaSmooth;
        const float v48 = v43;
        float c37 = v48, c38 = v48;
        const float a49 = -0.147f * 255.0f;
        const float a50 = 0.615f * 255.0f;
        float v51 = a49 * a49 + a50 * a50;
        v51 = std::sqrt(v51) * 2.0f * f.v23;
        if (1.0f <= v48 && v48 + 2.0f < v51 && 0.0f < v44) {
            float v40 = v51 - v48;
            float v27 = 1.0f;
            if (v51 - v48 > v48 - 1.0f) v27 = (v48 - 1.0f) / (v51 - v48);
            c38 -= v40 * v27 * v44;
            if (c38 == v48) {
                c37 = v48;
            } else {
                c37 += v40;
                c38 = std::fmax(c38, 1.0f);
                c37 = std::fmin(c37, (148.0f));
                if ((c37 - c38) / 3.0f >= v48 - c38) c37 = c38 + ((v48 - c38) * 3.0f);
            }
        }
        f.chromaStart = c38; f.chromaEnd = c37; f.chromaLimit = v48;
        f.chromaCub3 = f.chromaCub2 = 0.0f;
        if (c38 < c37) cubicFit(v48, 0.0f, c38 - c37, c38, 1.0f, &f.chromaCub3, &f.chromaCub2);
    }
    // ---- composite curves (kernel function f3 with the pixel's value left out): a start v38, end v37 per side; the first side rounds its reduction up to a whole number
    {
        float a36 = 100.0f, a37 = a36, a38 = a36;
        if (0.0f < s.compMinSmooth) {
            const float v40 = (120.0f) - a36;
            a38 -= (float)std::ceil(v40 * s.compMinSmooth);
            if (a38 == a36) {
                a37 = a36;
            } else {
                a37 += v40;
                a38 = std::fmax(a38, 1.0f);
                if ((a37 - a38) / 3.0f >= (100.0f) - a38) a37 = a38 + (((100.0f) - a38) * 3.0f);
            }
        }
        f.compLoStart = a38; f.compLoEnd = a37;
        f.compLoCub3 = f.compLoCub2 = 0.0f;
        if (a38 < a37) cubicFit(100.0f, 0.0f, a38 - a37, a38, 1.0f, &f.compLoCub3, &f.compLoCub2);
        f.v80 = ((float)a38 / (100.0f)) * ((float)a38 / (100.0f));
        a36 = 100.0f; a37 = a36; a38 = a36;
        if (0.0f < s.compMaxSmooth) {
            const float v40 = (120.0f) - a36;
            a38 -= v40 * s.compMaxSmooth;
            if (a38 == a36) {
                a37 = a36;
            } else {
                a37 += v40;
                a38 = std::fmax(a38, 1.0f);
                if ((a37 - a38) / 3.0f >= (100.0f) - a38) a37 = a38 + (((100.0f) - a38) * 3.0f);
            }
        }
        f.compHiStart = a38; f.compHiEnd = a37;
        f.compHiCub3 = f.compHiCub2 = 0.0f;
        if (a38 < a37) cubicFit(100.0f, 0.0f, a38 - a37, a38, 1.0f, &f.compHiCub3, &f.compHiCub2);
        f.v81 = ((float)a38 / (100.0f)) * ((float)a38 / (100.0f));
    }
    f.v79 = (f.chromaStart * f.chromaStart) * 0.25f;
    f.v47 = f.chromaStart * 0.5f;
}

// The factor the chroma curve (kernel f2) gives a chroma magnitude v45 (0 inside the limit's start, a cubic fall to the limit, then limit / v45).
inline float broadcastChromaFactor(const BroadcastFrame& f, float v45)
{
    if (v45 > 0.0f && v45 < f.chromaStart) return 1.0f;
    if (v45 >= f.chromaStart && v45 < f.chromaEnd) {
        const float x = v45 - f.chromaEnd;
        const float x2 = x * x, x3 = x2 * x;
        return (f.chromaCub3 * x3 + f.chromaCub2 * x2 + f.chromaLimit) / v45;
    }
    if (v45 >= f.chromaEnd) return f.chromaLimit / v45;
    return 0.0f;
}
// The composite curves (kernel f3) for the value v54: out[0]/out[1] the factor for the min / max side.
inline float broadcastCompFactor(float start, float end, float c3, float c2, float v54)
{
    if (v54 < start) return 1.0f;
    if (v54 >= start && v54 < end) {
        const float x = v54 - end;
        const float x2 = x * x, x3 = x2 * x;
        return (c3 * x3 + c2 * x2 + (100.0f)) / v54;
    }
    return (100.0f) / v54;
}

inline void broadcastPx(const BroadcastSettings& s, const BroadcastFrame& f, const float* p, float* o)
{
    float w = p[3];
    float px[4] = {p[0], p[1], p[2], p[3]};
    if (w > 1.0f) { w = 1.0f; px[3] = 1.0f; }
    if (!(w > 0.000001f)) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    float v82 = px[0] * 255.0f, v83 = px[1] * 255.0f, v7 = px[2] * 255.0f;
    if (w < 1.0f) { v82 /= w; v83 /= w; v7 /= w; }
    int v16 = 0;
    float v49 = (-0.147f * v82 - 0.289f * v83 + 0.436f * v7);
    float v50 = (0.615f * v82 - 0.515f * v83 - 0.100f * v7);
    if (v49 > -1.0f * 0.000001f && v49 < 0.000001f) v49 = 0.0f;
    if (v50 > -1.0f * 0.000001f && v50 < 0.000001f) v50 = 0.0f;
    // ---- the luma (kernel f1): from the premultiplied values
    float v15, yv;
    {
        const float v30 = (px[0] * 255.0f - f.v19) * f.v23;
        const float v31 = (px[1] * 255.0f - f.v19) * f.v23;
        const float v32 = (px[2] * 255.0f - f.v19) * f.v23;
        v15 = 0.299f * v30 + 0.587f * v31 + 0.114f * v32 + f.v18;
        v16 = 1;
        const float v24 = f.lumaKneeLo, v25 = f.lumaKneeHi;
        if (v15 < f.lumaLo0) {
            v15 = v24;
        } else if (v15 >= f.lumaLo0 && v15 < f.lumaLo1) {
            const float x = v15 - f.lumaLo0;
            const float x2 = x * x, x3 = x2 * x;
            v15 = f.lumaLoCub3 * x3 + f.lumaLoCub2 * x2 + v24;
        } else if (v15 >= f.lumaLo1 && v15 < f.lumaHi0) {
            v16 = 0;
        } else if (v15 >= f.lumaHi0 && v15 < f.lumaHi1) {
            const float x = v15 - f.lumaHi1;
            const float x2 = x * x, x3 = x2 * x;
            v15 = f.lumaHiCub3 * x3 + f.lumaHiCub2 * x2 + v25;
        } else if (v15 >= f.lumaHi1) {
            v15 = v25;
        }
        yv = (v15 - f.v18) * f.v22 + f.v19;
    }
    const float v45 = (std::fabs(v49) + std::fabs(v50)) * f.v23;
    float v84 = s.compMax - v15;
    float v85 = v15 - s.compMin;
    if (v16) {
        v84 = s.compMax - v15;
        v85 = v15 - s.compMin;
    } else if (v45 < (v84 + 0.000001f) && v45 < (v85 + 0.000001f) && v45 < (f.v47 + 0.000001f)) {
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
        return;
    }
    float v86 = v84;
    float v87 = f.v81;
    bool sideMax = true;
    float v89 = (v49 * v49 + v50 * v50) * f.v72;
    if (v85 < v84) { v86 = v85; v87 = f.v80; sideMax = false; }
    const float v90 = v86 * v86 * v87;
    float comp[2] = {0.0f, 0.0f};          // v55 (min side), v57 (max side): set by the composite curve when it is asked
    if (v89 > f.v79) {
        const float e = 2.0f * std::sqrt(v89);
        const float v46 = broadcastChromaFactor(f, e);
        v49 *= v46;
        v50 *= v46;
        v89 *= v46 * v46;
        v16 = 1;
    }
    if (v89 > v90) {
        const float e = std::sqrt(v89);
        float v27 = v86 / e;
        if (v27 > 100.0f / 120.0f) {
            const float v54 = 100.0f / v27;
            comp[0] = broadcastCompFactor(f.compLoStart, f.compLoEnd, f.compLoCub3, f.compLoCub2, v54);
            comp[1] = broadcastCompFactor(f.compHiStart, f.compHiEnd, f.compHiCub3, f.compHiCub2, v54);
            v27 = sideMax ? comp[1] : comp[0];
        }
        v49 *= v27;
        v50 *= v27;
        v16 = 1;
    }
    if (v16) {
        yv /= 255.0f;
        v49 /= 255.0f;
        v50 /= 255.0f;
        v82 = (float)((yv + 1.140f * v50));
        v83 = (float)((yv - 0.394f * v49 - 0.581f * v50));
        v7 = (float)((yv + 2.032f * v49));
        o[0] = v82 * w;
        o[1] = v83 * w;
        o[2] = v7 * w;
        o[3] = w;
    } else {
        o[0] = px[0]; o[1] = px[1]; o[2] = px[2]; o[3] = px[3];
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------- Chroma Keyer
// The key colour (the Color parameter, floats, in the order the kernel reads the picture's channels), the two thresholds, the despill choice (2: the key is a blue screen, 1: a green one,
// 0: neither) and ShowMaskOnly. The kernel compares each pixel's HSL and RGB distance to the key and ramps the alpha from the low to the high threshold; with a blur the alpha
// plane goes through Deriche's recursive Gaussian down the columns and then along the rows (single precision, VEGAS's coefficient builder, see keyerBlurPlane).
struct KeyerSettings { float key[3]; float low, high; int despill; bool showMask; };
inline void keyerHsl(const float* c, float* o)
{
    float mx = (c[0] > c[1]) ? c[0] : c[1];
    mx = (mx > c[2]) ? mx : c[2];
    float mn = (c[0] < c[1]) ? c[0] : c[1];
    mn = (mn < c[2]) ? mn : c[2];
    o[0] = o[1] = 0.0f;
    o[2] = (mn + mx) * 0.5f;
    if (o[2] <= 0) { o[1] = 0; o[0] = 0; return; }
    o[1] = mx - mn;
    if (o[1] > 0.0f) {
        const float d = (o[2] <= 0.5f) ? (mn + mx) : (2.0f - mx - mn);
        o[1] /= d;
    } else {
        o[0] = 0;
        return;
    }
    const float inv = 1.0f / (mx - mn);
    const float r = (mx - c[0]) * inv, g = (mx - c[1]) * inv, b = (mx - c[2]) * inv;
    const float v8[3] = {1.0f + r, 3.0f + g, 5.0f + b};
    const float v9[3] = {5.0f - r, 1.0f - g, 3.0f - b};
    if (c[0] == mx) o[0] = (c[1] == mn) ? v8[2] : v9[1];
    else if (c[1] == mx) o[0] = (c[2] == mn) ? v8[0] : v9[2];
    else o[0] = (c[0] == mn) ? v8[1] : v9[0];
    o[0] *= 0.16666667f;
}
// The alpha the key leaves of one pixel (kernel function f1): px = the pixel as stored (premultiplied), keyHsl = the key's HSL, invRange = 1 / (high - low) or 0.
inline float keyerAlpha(const KeyerSettings& s, const float* keyHslv, float invRange, const float* px)
{
    float c[3] = {px[0], px[1], px[2]};
    const float a = px[3];
    if (1.0f != a && 0.0f != a) { c[0] /= a; c[1] /= a; c[2] /= a; }
    float h[3];
    keyerHsl(c, h);
    float dh = std::fabs(h[0] - keyHslv[0]);
    const float ds = std::fabs(h[1] - keyHslv[1]);
    const float dl = std::fabs(h[2] - keyHslv[2]);
    if (dh > 0.5f) dh = 1.0f - dh;
    dh *= 2;
    float d = (dh + ds + dl) * (1.0f / 3.0f);
    const float dx = c[0] - s.key[0], dy = c[1] - s.key[1], dz = c[2] - s.key[2];
    const float dist = std::sqrt(std::fma(dx, dx, std::fma(dy, dy, dz * dz)));       // (dot() and mad() are fused multiply-adds on the GPU)
    d = std::fma(dist, 1.732050807568877f, d);
    d *= 0.5f;
    float k;
    if (d < s.low) k = 0.0f;
    else if (d + 0.000001f > s.high) k = 1.0f;
    else k = clamp01((d - s.low) * invRange);
    return (a < k) ? a : k;
}
inline float keyerInvRange(const KeyerSettings& s)
{
    return (std::fabs(s.high - s.low) < 0.000001f) ? 0.0f : 1.0f / (s.high - s.low);
}
// The end of both kernel paths: px = the pixel as stored, alpha = the keyed (and blurred) alpha, clampAll = the blur path's extra clamp of the pixel to 0..1.
inline void keyerFinish(const KeyerSettings& s, const float* px, float alpha, bool clampAll, float* o)
{
    float v[4] = {px[0], px[1], px[2], alpha};
    if (clampAll) for (int k = 0; k < 4; ++k) v[k] = clamp01(v[k]);
    if (v[3] >= 0.00392f && v[3] <= 1.0f) {
        if (s.despill == 2 && v[2] > v[1]) v[2] = v[1];
        else if (s.despill == 1 && v[1] > v[0]) v[1] = v[0];
    }
    if (s.showMask) {
        o[0] = o[1] = o[2] = o[3] = v[3];
    } else {
        o[0] = v[0] * v[3]; o[1] = v[1] * v[3]; o[2] = v[2] * v[3]; o[3] = v[3];
    }
}

// Deriche's recursive Gaussian, the way the keyer's two blur kernels run it along one line of L values (the line starts at the first memory row / column of VEGAS's picture, i.e. the
// bottom row): two passes, causal and anti-causal, written to two arrays whose sum is the blurred line. VEGAS keeps these arrays in images of the frame's own format: in an 8-bit
// project each value goes through the 8-bit store (store, the 12-bit step), in a float project it stays as it is (store = null). A..E = the coefficient vectors n, d, m and the edge terms (vfxgauss / builtins_filters).
inline void keyerBlurLine(const float* A, const float* B, const float* C, const float* D, const float* E, const float* in, int L, float* fwd, float* bwd, float (*store)(float))
{
    auto Q = [store](float v) { return store ? store(v) : v; };
    if (L < 2) { if (L == 1) { fwd[0] = bwd[0] = 0.0f; } return; }
    {
        float w = in[0];
        float v42 = -D[4] * w;
        float v41 = v42 + (A[3] - D[3]) * w;
        float v40 = v41 + (A[2] - D[2]) * w;
        float v39 = v40 + (A[1] - D[1]) * w;
        float v38 = v39 + (A[0] - D[0]) * w;
        float v49 = v38;
        fwd[0] = Q(v49);
        w = in[1];
        float v43 = 0.0f, v44 = v49 * B[0], v45 = 0.0f, v46 = 0.0f;
        v38 = v39 + w * A[0];
        v39 = v40 + w * A[1];
        v40 = v41 + w * A[2];
        v41 = v42 + w * A[3];
        v42 = 0.0f;
        v49 = v38;
        v49 -= v43;
        fwd[1] = Q(v49);
        for (int x = 2; x < L; ++x) {
            w = in[x];
            v43 = v44 + v49 * B[1];
            v44 = v45 + v49 * B[2];
            v45 = v46 + v49 * B[3];
            v46 = v49 * B[4];
            v38 = v39 + w * A[0];
            v39 = v40 + w * A[1];
            v40 = v41 + w * A[2];
            v41 = v42 + w * A[3];
            v49 = v38;
            v49 -= v43;
            fwd[x] = Q(v49);
        }
    }
    {
        float w = in[L - 1];
        float v42 = (C[4] - E[4]) * w;
        float v41 = v42 + (C[3] - E[3]) * w;
        float v40 = v41 + (C[2] - E[2]) * w;
        float v39 = v40 + (C[1] - E[1]) * w;
        float v38 = v39 + (C[0] - E[0]) * w;
        float v49 = v38;
        bwd[L - 1] = Q(v49);
        w = in[L - 2];
        float v43 = 0.0f, v44 = v49 * B[0], v45 = 0.0f, v46 = 0.0f;
        v38 = v39 + w * C[0];
        v39 = v40 + w * C[1];
        v40 = v41 + w * C[2];
        v41 = v42 + w * C[3];
        v42 = w * C[4];
        v49 = v38;
        v49 -= v43;
        bwd[L - 2] = Q(v49);
        for (int x = 2; x < L; ++x) {
            w = in[L - 1 - x];
            v43 = v44 + v49 * B[1];
            v44 = v45 + v49 * B[2];
            v45 = v46 + v49 * B[3];
            v46 = v49 * B[4];
            v38 = v39 + w * C[0];
            v39 = v40 + w * C[1];
            v40 = v41 + w * C[2];
            v41 = v42 + w * C[3];
            v42 = w * C[4];
            v49 = v38;
            v49 -= v43;
            bwd[L - 1 - x] = Q(v49);
        }
    }
}
// The blur of columns [x0, x1) of an alpha plane (W x H floats), in place. Each column stands alone.
inline void keyerBlurColumns(const float* A, const float* B, const float* C, const float* D, const float* E, int W, int H, std::vector<float>& plane, float (*store)(float),
                             int x0, int x1)
{
    std::vector<float> col((size_t)H), f((size_t)H), b((size_t)H);
    for (int x = x0; x < x1; ++x) {
        for (int y = 0; y < H; ++y) col[(size_t)y] = plane[(size_t)y * W + x];
        keyerBlurLine(A, B, C, D, E, col.data(), H, f.data(), b.data(), store);
        for (int y = 0; y < H; ++y) plane[(size_t)y * W + x] = f[(size_t)y] + b[(size_t)y];
    }
}
// The blur of rows [y0, y1) of an alpha plane (W x H floats), in place. Each row stands alone.
inline void keyerBlurRows(const float* A, const float* B, const float* C, const float* D, const float* E, int W, std::vector<float>& plane, float (*store)(float),
                          int y0, int y1)
{
    std::vector<float> rf((size_t)W), rb((size_t)W);
    for (int y = y0; y < y1; ++y) {
        float* row = &plane[(size_t)y * W];
        keyerBlurLine(A, B, C, D, E, row, W, rf.data(), rb.data(), store);
        for (int x = 0; x < W; ++x) row[x] = rf[(size_t)x] + rb[(size_t)x];
    }
}
// The whole blur of an alpha plane (W x H floats, row 0 = VEGAS's first memory row): down the columns, then along the rows; the plane holds the blurred alpha afterwards.
// (Reno's Chroma Keyer shares the columns and then the rows out between the processors; the result is the same.)
inline void keyerBlurPlane(const float* A, const float* B, const float* C, const float* D, const float* E, int W, int H, std::vector<float>& plane, float (*store)(float))
{
    keyerBlurColumns(A, B, C, D, E, W, H, plane, store, 0, W);
    keyerBlurRows(A, B, C, D, E, W, plane, store, 0, H);
}

}  // namespace vcol
