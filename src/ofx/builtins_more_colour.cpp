// Built-in colour effects (Saturation, Soft Contrast, White Balance, Lab Adjust, Black Restore, Broadcast Colors, Channel Blend); see builtins_more.cpp.
#include "builtins_more_internal.h"
#include "builtins_fast.h"
#include "builtins_vec.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ colour
// Saturation Adjust: VEGAS keeps the Rec.601 luma and scales the colour's distance from that grey by 1 + Amount * bump + Low * (1 - bump) below the centre + High * (1 - bump) above it;
// bump = 1 - smoothstep of the distance from the centre in half spreads; saturation is the Euclidean distance from the grey over 0.9272 (fitted).
const float kSatScale = 0.9272f;

void saturationAdjust(Ctx& c)
{
    const float amount = (float)c.d("Amount"), low = (float)c.d("Low"), high = (float)c.d("High");
    const float centre = (float)c.d("Center") * kSatScale, half = (float)std::max(0.005, c.d("Spread")) * 0.5f * kSatScale;
    perPixel(c, [&](float& r, float& g, float& b, int, int) {
        const float y = 0.299f * r + 0.587f * g + 0.114f * b;
        const float dr = r - y, dg = g - y, db = b - y;
        const float x = std::sqrt(dr * dr + dg * dg + db * db);
        const float t = clamp01(std::fabs(x - centre) / half);
        const float bump = 1.f - t * t * (3.f - 2.f * t);
        const float gain = amount * bump + (x < centre ? low : (x > centre ? high : 0.f)) * (1.f - bump);
        const float f = 1.f + gain;
        auto cut = [](float v) { return std::floor(clamp01(v) * 255.f + 1e-3f) * (1.f / 255.f); };      // VEGAS truncates to 8 bits
        r = cut(y + dr * f); g = cut(y + dg * f); b = cut(y + db * f);
    });
}

// Soft Contrast, from VEGAS's pictures (tools/effectref_compare.py): the picture is stretched (x * (1 + 0.00212 * Stretch range), clipped), then
// Soft Contrast, from VEGAS's pictures (tools/effectref_compare.py): stretch x * (1 + 0.00212 * Stretch range) (clipped), then blend with a blurred copy of the 601 luma by OVERLAY (below half 2xB,
// above 1 - 2(1-x)(1-B)) with Contrast as the amount. Diffusion blurs the luma layer (Gaussian 0.205 px per percent at 320 wide). The tint multiplies the layer per channel by 1 + 1.137 x tint x
// (T - luma(T)) (T = the correction hue at full saturation). Low / High trim bend the layer by Softlight's spline; the vignette is a RectangleMask / EllipseMask as in shapeMaskVelvet.
namespace {
inline float scOverlay(float x, float b) { return x < 0.5f ? 2.f * x * b : 1.f - 2.f * (1.f - x) * (1.f - b); }
inline float scLuma(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }
// ParamLibrary.Spline of TCB points (key, value, tension, continuity, bias) at x (Kochanek-Bartels, as Softlight's trims use it)
struct ScTcb { double key, value, t, c, b; };
double scTcb(const ScTcb* p, int n, double x)
{
    int i = n - 1, j = -1;
    double f = 0;
    for (int k = 0; k < n; ++k)
        if (p[k].key > x) {
            if (k == 0) i = 0;
            else { i = k - 1; j = k; f = (x - p[i].key) / (p[j].key - p[i].key); }
            break;
        }
    if (j < 0) return p[i].value;
    const double dt = p[j].key - p[i].key;
    const double prev = i > 0 ? p[i - 1].value : p[i].value, dtp = i > 0 ? p[i].key - p[i - 1].key : dt;
    const double next = j + 1 < n ? p[j + 1].value : p[j].value, dtn = j + 1 < n ? p[j + 1].key - p[j].key : dt;
    const ScTcb& a = p[i];
    const ScTcb& b = p[j];
    const double d1 = ((1 - a.t) * (1 + a.c) * (1 + a.b) / 2 * (a.value - prev) + (1 - a.t) * (1 - a.c) * (1 - a.b) / 2 * (b.value - a.value)) * (2 * dt / (dtp + dt));
    const double d2 = ((1 - b.t) * (1 - b.c) * (1 + b.b) / 2 * (b.value - a.value) + (1 - b.t) * (1 + b.c) * (1 - b.b) / 2 * (next - b.value)) * (2 * dt / (dtn + dt));
    const double s2 = f * f, s3 = s2 * f;
    return (2 * s3 - 3 * s2 + 1) * a.value + (-2 * s3 + 3 * s2) * b.value + (s3 - 2 * s2 + f) * d1 + (s3 - s2) * d2;
}
}  // namespace

void softContrast(Ctx& c)
{
    const double stretch = c.d("EffectStretchRange"), contrast = c.d("EffectContrast") / 100.0, diffusion = c.d("EffectDiffusion");
    const double tint = c.d("EffectTint") / 100.0, hue = c.d("EffectCorrectionHue");
    const int ext = c.i("VignetteExteriorEffect");
    const int W = c.out.w, H = c.out.h;
    const float gain = (float)(1.0 + 0.00212 * std::max(0.0, stretch));
    const float amount = (float)clampd(contrast, 0.0, 2.0);
    std::vector<float> lum((size_t)W * H);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            lum[(size_t)y * W + x] = scLuma(clamp01(r * gain), clamp01(g * gain), clamp01(b * gain));
        }
    });
    const double sigma = diffusion * 0.2033 * W / 320.0;
    if (sigma > 0.3) gaussianBlurPlane(lum, W, H, sigma, sigma);
    const double lowTrim = std::max(0.0, c.d("EffectLowTrim") / 100.0), highTrim = std::max(0.0, c.d("EffectHighTrim") / 100.0);
    if (lowTrim > 0 || highTrim > 0) {
        const ScTcb pts[3] = {{0.0, lowTrim * 0.25, 0, -1, 0}, {0.5, 0.5, 0, 0, 0}, {1.0, 1.0 - highTrim * 0.25, 0, -1, 0}};
        float tl[256];
        for (int i = 0; i < 256; ++i) tl[i] = (float)(clampd(std::floor(scTcb(pts, 3, i / 255.0) * 255.0 + 0.5), 0.0, 255.0) / 255.0);
        for (float& v : lum) {
            const float q = clamp01(v) * 255.f;
            const int i0 = std::min(254, (int)q);
            v = tl[i0] + (tl[i0 + 1] - tl[i0]) * (q - (float)i0);
        }
    }
    float tm[3] = {1.f, 1.f, 1.f}, toff = 0.f;
    if (tint > 0.0) {
        float tr, tg, tb;
        hslToRgb((float)(hue / 360.0 - std::floor(hue / 360.0)), 1.f, 0.5f, tr, tg, tb);
        const float ty = scLuma(tr, tg, tb), k = (float)(1.137 * tint);
        tm[0] = 1.f + k * (tr - ty); tm[1] = 1.f + k * (tg - ty); tm[2] = 1.f + k * (tb - ty);
        toff = (float)(-0.0133 * tint / 0.3263);
    }
    std::vector<float> vmask;
    const double vs = clampd(c.d("VignetteStrength") / 100.0, 0.0, 1.0);
    const bool ellipse = c.i("VignetteShape") == 1;
    double cx = 0.5, cy = 0.5;
    c.xy("VignettePosition", cx, cy);
    if (ext != 0)
        shapeMaskVelvet(W, H, cx, 1.0 - cy, std::max(0.0, c.d("VignetteWidth") / 100.0), std::max(0.0, c.d("VignetteHeight") / 100.0),
                        ellipse ? 0.0 : std::max(0.0, c.d("VignetteCornerRadius") / 100.0), std::max(0.0, c.d("VignetteSoftness") / 100.0), ellipse, vmask);
    Buf soft;
    if (ext == 1) {
        soft = c.src;
        const double bx = c.d("VignetteHorizontalBlur") * 0.05 * W, by = c.d("VignetteVerticalBlur") * 0.05 * H;
        gaussianBlur(soft, bx, by);
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            float* o = c.out.at(x, y);
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            const float B = lum[(size_t)y * W + x];
            float v[3] = {clamp01(r * gain), clamp01(g * gain), clamp01(b * gain)};
            for (int k = 0; k < 3; ++k) {
                const float bc = B * tm[k] + toff;
                v[k] = clamp01(v[k] + amount * (scOverlay(v[k], bc) - v[k]));
            }
            if (ext != 0) {
                const float m = (float)(std::floor((double)vmask[(size_t)y * W + x] * 255.0 + 0.5) / 255.0);
                const float k = (float)(vs * (1.0 - (double)m));
                if (ext == 2) { for (int q = 0; q < 3; ++q) v[q] *= (1.f - k); }
                else if (ext == 3) { for (int q = 0; q < 3; ++q) v[q] += (1.f - v[q]) * k; }
                else if (ext == 4) a *= (1.f - k);
                else if (ext == 1) {
                    const float* sp = soft.at(x, y);
                    float sr, sg2, sb, sa;
                    unpremul(sp, sr, sg2, sb, sa);
                    v[0] += (sr - v[0]) * k; v[1] += (sg2 - v[1]) * k; v[2] += (sb - v[2]) * k;
                }
            }
            premul(o, clamp01(v[0] - 0.0017f), clamp01(v[1] - 0.0017f), clamp01(v[2] - 0.0017f), a);
        }
    });
}

// White Balance, rebuilt from VEGAS's code. Works on 8-bit bytes in two steps:
// White Balance, rebuilt from VEGAS's code, on 8-bit bytes. 1. A 1024-entry table indexed by luma x 4 (rounded) gives the luma (x Brightness, from 0.25 steps, held to 0..255) and the shift of U, V
// (BT.601, 0..255 scale) = cos / sin of Angle x Amount^2 x 127.5; each pixel goes to Y, U, V, is shifted and goes back (R from 1.1402509 V, G from 0.3947313 U + 0.5808092 V, B from 2.0325203 U), rounded.
// 2. With HasData ("Equalize Histogram") each channel is stretched by its own histogram: lo = level below which 0.5% of pixels lie, hi = highest level with at most 99.5% below;
// new = (old - lo x 255/256) x 256 / (hi - lo), held to 0..255, taken from the frame being drawn.
// VEGAS draws from a hidden Angle, Amount, Brightness and HasData (Temperature, Tint and the neutral colour are dialog settings it converts). Reno has no Angle, so wbAngle derives it from temperature
// and tint with a model fitted to two VEGAS pictures (mired scale: warmer or bluer than 6500 K moves U, the tint moves V); the default temperature gives 0. Older Reno values (-1..1) mean the default.
static const double kWbDefaultKelvin = 6500.0;
static double wbAngle(double kelvin, double tint)
{
    if (!(kelvin >= 100.0)) kelvin = kWbDefaultKelvin;
    const double dm = 1e6 / clampd(kelvin, 1700.0, 15000.0) - 1e6 / kWbDefaultKelvin;
    const double su = dm, sv = -0.26067 * dm + 0.68791 * clampd(tint, -100.0, 100.0);
    if (su == 0.0 && sv == 0.0) return 0.0;
    return std::atan2(sv, su) * 180.0 / 3.141592653589793;
}
// same rounding as VEGAS (a float plus 1.5e-8 to the nearest whole number)
static inline int wbRound(float v) { return (int)std::floor((double)v + 1.5e-8 + 0.5); }
static inline float wbClampTo255(float v) { return v < 0.f ? 0.f : (v > 255.f ? 255.f : v); }

void whiteBalance(Ctx& c)
{
    const double amount = clampd(c.d("AmountOfCorrection"), 0, 1), bright = c.d("Brightness");
    const bool hasData = c.b("HasData");
    const int W = c.out.w, H = c.out.h;
    const bool shift = amount != 0.0 || bright != 1.0;
    if (!shift && !hasData) { copyThrough(c); return; }
    std::vector<float> tab((size_t)1024 * 3);
    {
        const double ang = wbAngle(c.d("ColorTemperature"), c.d("ColorTint")) * 3.141592653589793 / 180.0;
        const double s2 = amount * amount;
        const float su = (float)(std::cos(ang) * s2 * 127.5), sv = (float)(std::sin(ang) * s2 * 127.5);
        for (int i = 0; i < 1024; ++i) {
            double y = ((double)i * 0.25 / 255.0) * 255.0 * bright;
            y = y < 0.0 ? 0.0 : (y > 255.0 ? 255.0 : y);
            tab[(size_t)i * 3] = sv;
            tab[(size_t)i * 3 + 1] = su;
            tab[(size_t)i * 3 + 2] = (float)y;
        }
    }
    std::vector<unsigned char> by((size_t)W * H * 3);       // R, G, B bytes (straight)
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            unsigned char* o = &by[((size_t)y * W + x) * 3];
            if (a <= 0.f) { o[0] = o[1] = o[2] = 0; continue; }
            const float R = (float)wbRound(clamp01(r) * 255.f), G = (float)wbRound(clamp01(g) * 255.f), B = (float)wbRound(clamp01(b) * 255.f);
            if (!shift) { o[0] = (unsigned char)R; o[1] = (unsigned char)G; o[2] = (unsigned char)B; continue; }
            const float Y = G * 0.587f + R * 0.299f + B * 0.114f;
            const float* t = &tab[(size_t)wbRound(Y * 4.f) * 3];
            const float U = (B - Y) * 0.492f + t[1], V = (R - Y) * 0.877f + t[0], Yp = t[2];
            const float R2 = V * 1.1402509f + Yp, B2 = U * 2.0325203f + Yp, G2 = Yp - U * 0.39473134f - V * 0.5808092f;
            o[0] = (unsigned char)wbRound(wbClampTo255(R2));
            o[1] = (unsigned char)wbRound(wbClampTo255(G2));
            o[2] = (unsigned char)wbRound(wbClampTo255(B2));
        }
    });
    float gain[3] = {1.f, 1.f, 1.f}, off[3] = {0.f, 0.f, 0.f};
    if (hasData) {
        const float tail = 0.009999999776482582f * 0.5f;
        for (int k = 0; k < 3; ++k) {
            float cum[256] = {};
            for (size_t i = 0; i < (size_t)W * H; ++i) {
                if (c.src.px[i * 4 + 3] <= 0.f) continue;
                cum[by[i * 3 + (size_t)k]] += 1.f;
            }
            for (int i = 1; i < 256; ++i) cum[i] += cum[i - 1];
            if (cum[255] <= 0.f) continue;
            const float sc = (float)(1.0 / (double)cum[255]);
            for (int i = 0; i < 256; ++i) cum[i] *= sc;
            int lo = 0;
            while (lo < 255 && tail > cum[lo]) ++lo;
            const float up = 1.f - tail;
            int hi = 255;
            while (hi > 0 && cum[hi] > up) --hi;
            off[k] = (float)(-lo) * 0.00390625f;
            gain[k] = hi == lo ? 1.f : 256.f / (float)(hi - lo);
        }
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* sp = c.src.at(x, y);
            float* o = c.out.at(x, y);
            if (sp[3] <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            const unsigned char* q = &by[((size_t)y * W + x) * 3];
            float v[3];
            for (int k = 0; k < 3; ++k) {
                float f = (float)q[k];
                if (hasData) f = wbClampTo255((f + off[k] * 255.f) * gain[k]);
                v[k] = hasData ? (float)(int)(f + 0.5f) : f;
            }
            premul(o, v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, sp[3]);
        }
    });
}

// The buttons: "Select neutral color" takes the frame's average colour as the one that should be grey; the correction is an angle (direction in U, V)
// and an amount (size Amount^2 x 127.5), and temperature and tint are set to the pair giving that angle. "Equalize Histogram" switches equalising
// on, "Undo Equalization" off.
void whiteBalanceChanged(Change& ch)
{
    if (ch.param == "EnhanceHistogram") { ch.setI("HasData", 1); return; }
    if (ch.param == "ClearData") { ch.setI("HasData", 0); return; }
    if (ch.param != "SelectWhiteColor") return;
    Buf f;
    if (!ch.source(ch.time, f)) return;
    double R = 0, G = 0, B = 0, n = 0;
    for (size_t i = 0; i + 3 < f.px.size(); i += 4) {
        const float a = f.px[i + 3];
        if (!(a > 0.5f)) continue;
        R += f.px[i] / a; G += f.px[i + 1] / a; B += f.px[i + 2] / a;
        n += 1;
    }
    if (n < 1) return;
    R = R / n * 255.0; G = G / n * 255.0; B = B / n * 255.0;
    const double y = 0.299 * R + 0.587 * G + 0.114 * B, u = 0.492 * (B - y), v = 0.877 * (R - y);
    const double mag = std::sqrt(u * u + v * v);
    if (mag < 0.5) {
        ch.setD("ColorTemperature", kWbDefaultKelvin); ch.setD("ColorTint", 0.0); ch.setD("AmountOfCorrection", 0.0);
        return;
    }
    const double ca = -u / mag, sa = -v / mag;
    const double k = std::fabs(sa + 0.26067 * ca);
    double lam = 100.0;
    if (k > 1e-9) lam = std::min(lam, 68.79 / k);
    if (std::fabs(ca) > 1e-9) lam = std::min(lam, 87.0 / std::fabs(ca));
    const double dm = lam * ca, tint = clampd((lam * sa + 0.26067 * dm) / 0.68791, -100.0, 100.0);
    ch.setD("ColorTemperature", clampd(1e6 / (1e6 / kWbDefaultKelvin + dm), 1700.0, 15000.0));
    ch.setD("ColorTint", tint);
    ch.setD("AmountOfCorrection", clampd(std::sqrt(mag / 127.5), 0.0, 1.0));
}

// LAB Adjust, measured against VEGAS: CIE L*a*b* (sRGB, D65). Invert Luminance turns L into 95.58 - L first (may go below zero, darker than black),
// then Lightness multiplies L (above 100 not clipped: the colour clips per channel). Channel a / b move the colour by 100 per unit, Desaturate scales
// them down afterwards. The 8-bit result is truncated.
void labAdjust(Ctx& c)
{
    const double light = c.d("Lightness"), da = c.d("Channela"), db = c.d("Channelb"), des = clampd(c.d("Desaturate"), 0, 1);
    const bool invert = c.b("InvertLuminance");
    const double wx = 0.95047, wz = 1.08883, e = 216.0 / 24389.0, kk = 24389.0 / 27.0;
    perPixel(c, [&](float& r, float& g, float& b, int, int) {
        auto lin = [](double v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        const double lr = lin(r), lg = lin(g), lb = lin(b);
        const double X = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) / wx;
        const double Y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
        const double Z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) / wz;
        auto f = [&](double v) { return v > e ? std::cbrt(v) : (kk * v + 16.0) / 116.0; };
        const double fx = f(X), fy = f(Y), fz = f(Z);
        double L = 116.0 * fy - 16.0, A = 500.0 * (fx - fy), B = 200.0 * (fy - fz);
        if (invert) L = 95.58 - L;
        L *= light;
        A = (A + da * 100.0) * (1 - des);
        B = (B + db * 100.0) * (1 - des);
        const double gy = (L + 16.0) / 116.0, gx = gy + A / 500.0, gz = gy - B / 200.0;
        auto fi = [&](double v) { const double v3 = v * v * v; return v3 > e ? v3 : (116.0 * v - 16.0) / kk; };
        const double x2 = fi(gx) * wx, y2 = fi(gy), z2 = fi(gz) * wz;
        const double cr = 3.24045484 * x2 - 1.53713885 * y2 - 0.49853155 * z2;
        const double cg = -0.96926639 * x2 + 1.87601093 * y2 + 0.04155608 * z2;
        const double cb = 0.05564342 * x2 - 0.20402585 * y2 + 1.05722516 * z2;
        auto enc = [](double v) {
            v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
            v = v < 0 ? 0 : (v > 1 ? 1 : v);
            return (float)(std::floor(v * 255.0 + 1e-3) / 255.0);
        };
        r = enc(cr); g = enc(cg); b = enc(cb);
    });
}

// Black Restore: every pixel whose brightest channel is at or below the threshold becomes plain black (hard threshold: at 0.5 levels up to 127 go,
// 128 stays; at 1 the whole picture is black).
void blackRestore(Ctx& c)
{
    const float thr = (float)clampd(c.d("Threshold"), 0, 1) + 1e-6f;
    perPixel(c, [&](float& r, float& g, float& b, int, int) {
        if (std::max(r, std::max(g, b)) <= thr) { r = 0; g = 0; b = 0; }
    });
}

// Broadcast Colors, measured against VEGAS (keeps the picture within broadcast limits; every limit starts far enough out to change nothing). A level
// Broadcast Colors, measured against VEGAS (every limit starts far enough out to change nothing). L IRE is L * 2.37/255 - 1.75/255 with Studio RGB on, L * 2.55/255 + 1/255 off. Luma (Rec. 601 Y)
// outside Min..Max moves to the limit by adding the same amount to R, G, B (no clipping in between); chroma, the length of (U, V) = (0.492 (B - Y), 0.877 (R - Y)), is held to Max/100 * 0.465; the
// composite Y +- |(U, V)| must stay within CompositeMin..CompositeMax (chroma scaled down, Y stays). Smoothness is a soft knee: luma and chroma limits bend the range between the signal's extreme
// and the limit from (1 + smoothness) times their distance along a power curve meeting the line with slope 1; composite limits use a quadratic knee 0.2 * smoothness * |limit - Y| wide.
// 7.5 IRE setup changes nothing.
struct BroadcastLimits { float ymin, ymax, symin, symax, clim, sc, cmin, cmax, scmin, scmax; };
BroadcastLimits broadcastLimits(Ctx& c)
{
    const bool studio = c.b("StudioRGB16To135");
    const float k1 = studio ? 2.37f / 255.f : 2.55f / 255.f, k0 = studio ? -1.75f / 255.f : 1.f / 255.f;
    auto ire = [&](const char* n) { return (float)c.d(n) * k1 + k0; };
    auto sm = [&](const char* n) { return (float)clampd(c.d(n), 0, 1); };
    BroadcastLimits l;
    l.ymin = ire("LumaMin"); l.ymax = ire("LumaMax");
    l.symin = sm("LumaMinSmoothness"); l.symax = sm("LumaMaxSmoothness");
    l.clim = std::max(0.f, (float)c.d("ChromaMax") / 100.f * 0.465f); l.sc = sm("ChromaSmoothness");
    l.cmin = ire("CompositeMin"); l.cmax = ire("CompositeMax");
    l.scmin = sm("CompositeMinSmoothness"); l.scmax = sm("CompositeMaxSmoothness");
    return l;
}
// The largest |(U, V)| of an RGB colour (pure red or cyan).
const float kBroadcastChromaTop = 0.63214f;
// A soft lower limit: x <= 0 gives L, x >= T gives x, a power curve between with slope 1 at T (T <= L: a hard limit).
inline float broadcastSoftLow(float x, float L, float T)
{
    if (T <= L + 1e-7f) return std::max(x, L);
    if (x >= T) return x;
    return L + (T - L) * std::pow(std::min(1.f, std::max(0.f, x / T)), T / (T - L));
}
void broadcastColors(Ctx& c)
{
    const BroadcastLimits L = broadcastLimits(c);
    perPixel(c, [&](float& r, float& g, float& b, int, int) {
        const float y0 = 0.299f * r + 0.587f * g + 0.114f * b;
        float y = y0;
        y = L.ymin > 0 ? broadcastSoftLow(y, L.ymin, L.ymin * (1 + L.symin)) : std::max(y, L.ymin);
        y = L.ymax < 1 ? 1 - broadcastSoftLow(1 - y, 1 - L.ymax, (1 - L.ymax) * (1 + L.symax)) : std::min(y, L.ymax);
        const float d = y - y0;
        float cb = b + d - y, cr = r + d - y;
        const float uv = std::sqrt((0.492f * cb) * (0.492f * cb) + (0.877f * cr) * (0.877f * cr));
        const float top = kBroadcastChromaTop;
        const float uv1 = L.clim < top ? top - broadcastSoftLow(top - uv, top - L.clim, (top - L.clim) * (1 + L.sc)) : std::min(uv, L.clim);
        float f = uv > 1e-9f ? uv1 / uv : 1.f;
        const float uv2 = uv * f;
        if (uv2 > 1e-9f) {
            float hi = y + uv2, lo = y - uv2;
            const float wh = 0.2f * L.scmax * std::max(0.f, L.cmax - y), wl = 0.2f * L.scmin * std::max(0.f, y - L.cmin);
            if (wh > 1e-9f) { if (hi >= L.cmax + wh) hi = L.cmax; else if (hi > L.cmax - wh) hi = L.cmax - (L.cmax + wh - hi) * (L.cmax + wh - hi) / (4 * wh); }
            else hi = std::min(hi, L.cmax);
            if (wl > 1e-9f) { if (lo <= L.cmin - wl) lo = L.cmin; else if (lo < L.cmin + wl) lo = L.cmin + (lo - L.cmin + wl) * (lo - L.cmin + wl) / (4 * wl); }
            else lo = std::max(lo, L.cmin);
            const float s = std::min(1.f, std::max(0.f, std::min((hi - y) / uv2, (y - lo) / uv2)));
            f *= s;
        }
        cb *= f; cr *= f;
        const float r2 = y + cr, b2 = y + cb;
        r = clamp01(r2); b = clamp01(b2); g = clamp01((y - 0.299f * r2 - 0.114f * b2) / 0.587f);
    });
}

// Channel Blend: each output channel is a weighted sum of the input channels. VEGAS's plug-in numbers channels in memory order (blue first), so its
// "RED" group makes the picture's blue, "BLUE" the red, and the "R" and "B" weights weigh the input's blue and red. With Normalize the sum is
// divided by the sum of the weights' sizes (the offset counting as a weight on a constant 1); weights are not limited to 0..1.
void channelBlend(Ctx& c)
{
    float w[4][5];
    const char* names[4] = {"BLUE", "GREEN", "RED", "ALPHA"};
    const char* parts[5] = {" weight B", " weight G", " weight R", " weight A", " weight Offset"};
    const bool normalise = c.b("Normalize");
    for (int o = 0; o < 4; ++o)
        for (int i = 0; i < 5; ++i) w[o][i] = (float)c.d((std::string(names[o]) + parts[i]).c_str());
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* in = c.src.at(x, y);                  // premultiplied as it lies (VEGAS blends it so)
            float* dst = c.out.at(x, y);
            float out[4];
            for (int o = 0; o < 4; ++o) {
                float sum = 0, ws = std::fabs(w[o][4]);
                for (int i = 0; i < 4; ++i) { sum += w[o][i] * in[i]; ws += std::fabs(w[o][i]); }
                sum += w[o][4];
                if (normalise) sum = ws > 1e-6f ? sum / ws : 0.f;
                out[o] = clamp01(sum);
            }
            for (int o = 0; o < 4; ++o) dst[o] = out[o];
        }
    });
}

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
