// More built-in stand-ins for VEGAS Pro 18 effects (see builtins.h): Color Corrector (Secondary), Color Corrector, Convolution Kernel,
// Spherize and Newsprint, with VEGAS's identifiers and parameter names.
//   - colour wheels and hue limits use vectorscope angles (atan2(V, U), U = 0.492 (B - Y), V = 0.877 (R - Y), Rec. 601 luma): red 103.5
//     degrees, green 240.7, blue 347.1, as in VEGAS's presets; a wheel point is (angle, amount 0..1);
//   - luminance limits are 0..255, saturation limits the vectorscope distance x 255 (pure red is 162, VEGAS's "all of it" upper limit);
//   - Offset is in 0..255 steps (a guess: VEGAS's range is not known).
#include "builtins_impl.h"
#include "builtins_vec.h"
#include "vegas8.h"
#include "vkeffects.h"
#include <cmath>
#include "../core/base.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace reno {
namespace ofx {
namespace builtin {
void vegasNewsprint(Ctx& c);   // builtins_newsprint.cpp: VEGAS's Newsprint halftone

// The 8-bit Convolution Kernel's weights, offset and alpha mode (shared with the Vulkan plan)
void convolutionKernel8Setup(Ctx& c, float* A, float& off, bool& alphaPlain)
{
    float w[9];
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) w[r * 3 + col] = (float)c.d(strfmt("Row%dColumn%d", r + 1, col + 1).c_str());
    float rot = (float)c.d("Rotation");
    if (0.f > rot) rot += (float)(std::abs((int)(rot / 360.f)) * 360 + 360);
    if (rot == 0.f) {
        A[0] = w[6]; A[1] = w[7]; A[2] = w[8]; A[3] = w[3]; A[4] = w[4]; A[5] = w[5]; A[6] = w[0]; A[7] = w[1]; A[8] = w[2];
    } else {
        const float ring[8] = {w[6], w[7], w[8], w[5], w[2], w[1], w[0], w[3]};
        const int idx = (int)(rot / 45.f);
        const float frac = (rot - (float)(45 * idx)) / 45.f, inv = 1.f - frac;
        auto tap = [&](int k) { return frac * ring[(idx + k + 1) & 7] + inv * ring[(idx + k) & 7]; };
        A[0] = tap(0); A[1] = tap(1); A[2] = tap(2); A[3] = tap(-1); A[4] = w[4]; A[5] = tap(3); A[6] = tap(-2); A[7] = tap(-3); A[8] = tap(-4);
    }
    // (a Scale outside its range 0..4 is no usable factor: VEGAS's picture comes out normalised, as the reference "every setting at its smallest" shows)
    const double scaleIn = c.d("Scale");
    const bool norm = c.b("AutoNormalize") || !(scaleIn >= 0.0 && scaleIn <= 4.0);
    float scale = (float)scaleIn;
    if (norm) {
        float sum = A[0] + 0.f;
        for (int i = 1; i < 9; ++i) sum = sum + A[i];
        if (sum >= -9.999999747378752e-05f && 9.999999747378752e-05f >= sum) scale = 1.f;
        else {
            scale = (float)(1.0 / (double)sum);
            if (!(-100.f > scale)) scale = std::min(100.f, scale);
            else scale = -100.f;
        }
    }
    for (int i = 0; i < 9; ++i) A[i] = A[i] * scale;
    float sumW = A[0] + 0.f;
    for (int i = 1; i < 9; ++i) sumW = sumW + A[i];
    alphaPlain = 1.f > sumW;                          // (sum under 1: the alpha is the right neighbour's, not weighted)
    off = (float)(c.d("Offset") * 255.0);
}

namespace {

const char* const kGroup = "Reno Built-in";

// Rec. 601 Y and the vectorscope's U, V of a straight colour, and back.
inline void toYuv(float r, float g, float b, float& y, float& u, float& v)
{
    y = 0.299f * r + 0.587f * g + 0.114f * b;
    u = 0.492f * (b - y);
    v = 0.877f * (r - y);
}

// ------------------------------------------------------------------ Color Corrector (Secondary)
// Measured on VEGAS's reference renders (tools/vegasref_compare.py). Works in Rec. 601 Y, U = 0.492 (B - Y), V = 0.877 (R - Y) of the straight colour: Chrominance adds 0.5 x amount^2 at its
// angle to (U, V); Rotate hue turns (U, V); Saturation scales them; Gain, Gamma and Offset act on Y only (Y x gain, gamma curve, + offset / 255, limited to 0..1); the colour is rebuilt
// from (Y, U, V) without limiting. The mask is the product of the enabled limits; a limit is 1 inside [low, high] and falls over 'smooth' outside (secondaryFall, nearly smoothstep squared);
// luminance is Y x 255, saturation the vectorscope distance x 255. Result = source + mask x (changed - source), limited to 0..1. Gamma curve ((Y + c) / (1 + c)) ^ a with c and a read from
// VEGAS at gamma 0, 0.85, 1, 1.25 and 2 and interpolated.
typedef v4::V4 V4;

// The fall-off of a limit: u = 1 - (distance outside the window) / smooth, 0..1.
inline float secondaryFall(float u)
{
    if (!(u > 0.f)) return 0.f;
    if (u >= 1.f) return 1.f;
    static const float q[7] = {0.00758003534f, -0.0561729743f, 1.58064343f, -7.07356105f, 20.7903083f, -20.3405720f, 6.09177422f};
    float r = q[6];
    for (int i = 5; i >= 0; --i) r = r * u + q[i];
    return r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
}
// How much of a range a value is in (secondaryFall outside the window).
inline float secondaryRange(float x, float lo, float hi, float smooth)
{
    if (hi < lo) std::swap(lo, hi);
    if (x >= lo && x <= hi) return 1.f;
    const float d = x < lo ? lo - x : x - hi;
    return smooth > 1e-6f ? secondaryFall(1.f - d / smooth) : 0.f;
}
// The gamma curve's c and a for a Gamma setting.
inline void secondaryGamma(double g, float& cOut, float& aOut)
{
    static const double gn[5] = {0, 0.85, 1, 1.25, 2}, gc[5] = {0.1708, 0.0171, 0, -0.0099, -0.0186}, ga[5] = {2.996, 1.2948, 1, 0.8057, 0.2713};
    g = clampd(g, 0, 2);
    int i = 0;
    while (i < 3 && g > gn[i + 1]) ++i;
    const double t = (g - gn[i]) / (gn[i + 1] - gn[i]);
    cOut = (float)(gc[i] + (gc[i + 1] - gc[i]) * t);
    aOut = (float)(ga[i] + (ga[i + 1] - ga[i]) * t);
}

// The row function (see Def::rows): the plain per-pixel code of this effect.
bool colorCorrectorSecondary8(Ctx& c, RowFn& fn);
bool colorCorrectorSecondaryRows(Ctx& c, RowFn& fn)
{
    if (c.levels && colorCorrectorSecondary8(c, fn)) return true;
    double ca, cm;
    c.xy("Chrominance", ca, cm);
    const double cam = clampd(cm, -2, 2), ct = (cam < 0 ? -cam * cam : cam * cam) * 0.5;
    const float cu = (float)(std::cos(ca * kPi / 180.0) * ct), cv = (float)(std::sin(ca * kPi / 180.0) * ct);
    const double rot = c.d("RotateHue") * kPi / 180.0;
    const float cr = (float)std::cos(rot), sr = (float)std::sin(rot);
    const float sat = (float)clampd(c.d("Saturation"), 0, 100);
    float gc, ga;
    secondaryGamma(c.d("Gamma"), gc, ga);
    const bool gammaOn = std::fabs(ga - 1.f) > 1e-6f || std::fabs(gc) > 1e-6f;
    const float gain = (float)clampd(c.d("Gain"), -100, 100);
    const float offset = (float)(clampd(c.d("Offset"), -1000, 1000) / 255.0);
    const float alpha = (float)clampd(c.d("Alpha"), 0, 1);
    const bool showMask = c.b("ShowMask"), invert = c.b("InvertMask");
    const bool useL = c.b("EnableLimitLuminance"), useS = c.b("EnableLimitSaturation"), useH = c.b("EnableLimitHue");
    const float l0 = (float)c.d("LimitLuminanceLow"), l1 = (float)c.d("LimitLuminanceHigh");
    const float ls = (float)std::max(0.0, c.d("LimitLuminanceSmooth"));
    const float s0 = (float)c.d("LimitSaturationLow"), s1 = (float)c.d("LimitSaturationHigh");
    const float ss = (float)std::max(0.0, c.d("LimitSaturationSmooth"));
    const float hc = (float)c.d("LimitHueCenter"), hw = (float)std::max(0.0, c.d("LimitHueWidth")), hs = (float)std::max(0.0, c.d("LimitHueSmooth"));
    const bool hueLimit = useH && hw < 360.f;
    fn = [=](const float* src, float* out, int n, int) {
        for (int x = 0; x < n; ++x) {
            const float* ps = src + (size_t)x * 4;
            float* po = out + (size_t)x * 4;
            const float a = ps[3];
            if (!(a > 0.f)) { po[0] = po[1] = po[2] = po[3] = 0.f; continue; }
            const bool has = a > 1e-6f;
            const float r = has ? ps[0] / a : 0.f, g = has ? ps[1] / a : 0.f, b = has ? ps[2] / a : 0.f;
            const float Y = 0.299f * r + 0.587f * g + 0.114f * b;
            const float U = 0.492f * (b - Y), V = 0.877f * (r - Y);
            float m = 1.f;
            if (useL) m *= secondaryRange(Y * 255.f, l0, l1, ls);
            if (useS) m *= secondaryRange(std::sqrt(U * U + V * V) * 255.f, s0, s1, ss);
            if (hueLimit && m != 0.f) {
                // (a colour with no chroma has hue 0: its rounding noise must not decide, and the GPU kernel does the same)
                const float hue = std::max(std::fabs(U), std::fabs(V)) < 1e-5f ? 0.f : std::atan2(V, U) * (float)(180.0 / kPi);
                float d = std::fabs(std::fmod(hue - hc, 360.f));
                if (d > 180.f) d = 360.f - d;
                const float dd = d - hw * 0.5f;
                m *= dd <= 0.f ? 1.f : (hs > 1e-6f ? secondaryFall(1.f - dd / hs) : 0.f);
            }
            if (invert) m = 1.f - m;
            if (showMask) { const float v = m * a; po[0] = po[1] = po[2] = po[3] = v; continue; }
            if (!(m > 0.f)) { std::memcpy(po, ps, sizeof(float) * 4); continue; }
            const float ut = U + cu, vt = V + cv;
            const float u2 = (ut * cr - vt * sr) * sat, v2 = (ut * sr + vt * cr) * sat;
            float Yp = Y * gain;
            if (Yp < 0.f) Yp = 0.f;
            if (gammaOn) {
                const float base = (Yp + gc) / (1.f + gc);
                Yp = base > 0.f ? std::pow(base, ga) : 0.f;
            }
            Yp += offset;
            Yp = Yp < 0.f ? 0.f : (Yp > 1.f ? 1.f : Yp);
            const float nr = Yp + v2 / 0.877f, nb = Yp + u2 / 0.492f;
            const float ng = (Yp - 0.299f * nr - 0.114f * nb) / 0.587f;
            const float oA = a * (1.f + (alpha - 1.f) * m);
            po[0] = clamp01(r + (nr - r) * m) * oA;
            po[1] = clamp01(g + (ng - g) * m) * oA;
            po[2] = clamp01(b + (nb - b) * m) * oA;
            po[3] = oA;
        }
    };
    return true;
}
void colorCorrectorSecondary(Ctx& c) { runRows(c, colorCorrectorSecondaryRows); }

// "Select effect range": Reno takes the colours of the middle of the current frame (a quarter of its width and height, non-transparent pixels)
// and limits luminance, saturation and hue to where 90 per cent of them lie (a little wider), switching the three limits on.
void secondaryChanged(Change& ch)
{
    if (ch.param != "SelectEffectRange") return;
    Buf f;
    if (!ch.source(ch.time, f) || f.w < 2 || f.h < 2) return;
    std::vector<float> ys, ss;
    double hx = 0, hy = 0;
    std::vector<float> hues;
    const int x0 = f.w * 3 / 8, x1 = std::max(x0 + 1, f.w * 5 / 8), y0 = f.h * 3 / 8, y1 = std::max(y0 + 1, f.h * 5 / 8);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float r, g, b, a;
            unpremul(f.at(x, y), r, g, b, a);
            if (a < 0.5f) continue;
            float Y, U, V;
            toYuv(r, g, b, Y, U, V);
            const float sat = std::sqrt(U * U + V * V) * 255.f;
            ys.push_back(Y * 255.f);
            ss.push_back(sat);
            if (sat > 2.f) {            // (a grey has no hue)
                const float hue = std::atan2(V, U);
                hx += std::cos(hue) * sat; hy += std::sin(hue) * sat;
                hues.push_back(hue);
            }
        }
    if (ys.size() < 4) return;
    auto quantile = [](std::vector<float>& v, double q) {
        std::sort(v.begin(), v.end());
        return v[(size_t)clampd(q * (double)(v.size() - 1) + 0.5, 0, (double)(v.size() - 1))];
    };
    const double lowY = quantile(ys, 0.05), highY = quantile(ys, 0.95), lowS = quantile(ss, 0.05), highS = quantile(ss, 0.95);
    ch.setD("LimitLuminanceLow", clampd(lowY - 6, 0, 255));
    ch.setD("LimitLuminanceHigh", clampd(highY + 6, 0, 255));
    ch.setD("LimitSaturationLow", clampd(lowS - 4, 0, 162));
    ch.setD("LimitSaturationHigh", clampd(highS + 4, 0, 162));
    if (hues.size() >= 4 && (hx != 0 || hy != 0)) {
        const double centre = std::atan2(hy, hx);
        std::vector<float> dev;
        for (float h : hues) {
            double d = std::fabs(std::fmod(h - centre, 2 * kPi));
            if (d > kPi) d = 2 * kPi - d;
            dev.push_back((float)d);
        }
        const double half = quantile(dev, 0.9) * 180.0 / kPi;
        double deg = centre * 180.0 / kPi;
        if (deg < 0) deg += 360.0;
        ch.setD("LimitHueCenter", deg);
        ch.setD("LimitHueWidth", clampd(half * 2 + 10, 10, 360));
        ch.setI("EnableLimitHue", 1);
    } else {
        ch.setI("EnableLimitHue", 0);
    }
    ch.setI("EnableLimitLuminance", 1);
    ch.setI("EnableLimitSaturation", 1);
}

// ---- 8-bit pictures: VEGAS's own steps (Vfx1.ofx colorcorrectorProcessor<unsigned char>), copied operation by operation. A table of 1024 entries over
// Y x 4 (Y in levels, rounded to the nearest) holds the gain/gamma/offset curve (y), the tint of the three wheels (a: added to V, b: added to U, in levels,
// each wheel a cos/sin pair x weight x 127.5, summed, kept inside a circle of radius 162) and the saturation (s). Each pixel: Y, U = (B - Y) 0.492, V = (R - Y) 0.877
// in floats; U, V get the table's tint and are scaled by the saturation; R, G, B are rebuilt from the curve's Y' (or Y when there is none) and cut to 0..alpha.
struct CcEntry { float a, b, y, s; };
struct CcCurve { bool lin; double g, A, B, C, P; };
CcCurve ccCurve(double g)
{
    CcCurve s = {};
    s.g = g;
    const double ig = 1.0 / g;
    const double A = std::pow(ig, -0.9418051973077521) * 0.03818376618407356;
    s.A = A;
    if (1.0 > g) {
        const double pa = std::pow(A, g);
        s.B = (std::pow(ig, 1.8836103946155043) * A - pa) / (pa - 1.0);
        s.lin = true;
    } else if (g > 1.0) {
        const double q = std::pow(std::pow(ig, 1.8836103946155043) * A, ig);
        s.B = (A - q) / (q - 1.0);
        s.C = 1.0 / (s.B + 1.0);
        s.lin = false;
    } else {
        s.A = 100.0;
        s.B = 0.0;
        s.lin = true;
    }
    s.P = std::pow(ig, 1.8836103946155043);
    return s;
}
// The curve's value at x (0..1) for 'lin' curves (gamma < 1 after the remap) and the other kind.
double ccCurveAt(const CcCurve& s, double x)
{
    double v;
    if (s.lin) {
        if (x >= s.A) {
            v = std::pow(x, s.g);
            v = v * (s.B + 1.0) - s.B;
        } else if (x > s.A * -1.0) {
            v = x * s.P;
        } else {
            const double v0 = std::pow(x * -1.0, s.g) * (s.B + 1.0);
            v = s.B - v0;
        }
    } else {
        if (x >= s.A) v = std::pow((s.B + x) * s.C, s.g);
        else if (x > s.A * -1.0) v = x * s.P;
        else v = std::pow((x - s.B) * (s.C * -1.0), s.g) * -1.0;
    }
    return v;
}
struct CcTable {
    std::vector<CcEntry> e;
    std::vector<int> d;         // wheels only: 4 ints per entry (blue, green, red, 255)
    int flags = 0;
    bool lum = false;           // Color Corrector (Secondary): the table follows a luminance mask, so every pixel looks its entry up
};
// The Y' curve of gain/gamma/offset (the 1024 entries' y): the curve of VEGAS, or Y itself.
void ccYFill(std::vector<CcEntry>& e, bool curve, double offset, double gamma, double gain, bool delta = false)
{
    if (curve) {
        double tg = 2.0 - gamma;
        if (tg > 1.0) tg = (tg - 1.0) + (tg - 1.0) + 1.0;
        else if (1.0 > tg) tg = 1.0 - (1.0 - tg) * 0.8;
        const CcCurve cur = ccCurve(tg);
        for (int i = 0; i < 1024; ++i) {
            const double x = (double)i * 0.25 / 255.0;
            double v = ccCurveAt(cur, x);
            v = v * 255.0;
            v = v * gain + offset;
            v = (0.0 > v) ? 0.0 : (255.0 < v ? 255.0 : v);
            e[i].y = (float)(delta ? v - (double)i * 0.25 : v);
        }
    } else {
        for (int i = 0; i < 1024; ++i) e[i].y = (float)i * 0.25f;
    }
}
// The wheels-only case: the table's tints as integers (blue, green, red, 255), as VEGAS adds them to the bytes.
void ccMakeD(CcTable& t)
{
    t.d.assign(4096, 0);
    for (int i = 0; i < 1024; ++i) {
        const double a = t.e[i].a, b = t.e[i].b;
        t.d[4 * i + 0] = v8::roundMagic(b * 2.032520294189453);
        t.d[4 * i + 1] = v8::roundMagic(0.0 - (b * 0.394731342792511 + a * 0.5808091759681702));
        t.d[4 * i + 2] = v8::roundMagic(a * 1.1402509212493896);
        t.d[4 * i + 3] = 255;
    }
}
void ccBuild(CcTable& t, const double P[10])
{
    int flags = 0;
    if (P[6] != 0.0 || P[7] != 1.0 || P[8] != 1.0) flags |= 1;
    if (P[9] != 1.0) flags |= 2;
    if (P[1] != 0.0 || P[3] != 0.0 || P[5] != 0.0) flags |= 4;
    t.flags = flags;
    t.e.assign(1024, CcEntry{0.f, 0.f, 0.f, (float)P[9]});
    ccYFill(t.e, (flags & 1) != 0, P[6], P[7], P[8]);
    if (flags & 4) {
        const double k[3] = {-9.518927955800229e-05, -0.00026399289035467123, -9.518927955800229e-05};
        const double centre[3] = {0.0, 127.5, 255.0};
        for (int w = 0; w < 3; ++w) {
            const double rad = P[2 * w] * 3.141592653589793 / 180.0;
            const double c = std::cos(rad), sn = std::sin(rad);
            const double amt2 = P[2 * w + 1] * P[2 * w + 1];
            for (int i = 0; i < 1024; ++i) {
                double dy = (double)t.e[i].y - centre[w];
                dy = dy * dy;
                dy = dy * k[w];
                const double wt = std::exp(dy) * amt2;
                const float o1 = (float)((c * wt) * 127.5), o2 = (float)((sn * wt) * 127.5);
                if (w == 0) { t.e[i].b = o1; t.e[i].a = o2; }
                else { t.e[i].b = o1 + t.e[i].b; t.e[i].a = o2 + t.e[i].a; }
            }
        }
        for (int i = 0; i < 1024; ++i) {
            float* f[2] = {&t.e[i].a, &t.e[i].b};
            const float a = *f[0], b = *f[1];
            const float n2 = a * a + b * b;
            if (!(26244.0 > (double)n2)) {
                const double s = 162.0 / std::sqrt((double)n2);
                *f[1] = (float)((double)b * s);
                *f[0] = (float)((double)a * s);
            }
        }
        if ((flags & 3) == 0) ccMakeD(t);
    }
}
// One row of 8-bit pixels (float 0..1, premultiplied as the host keeps them; VEGAS works on the bytes as they are).
void ccRow8(const CcTable& t, const float* src, float* out, int n)
{
    const int flags = t.flags;
    for (int x = 0; x < n; ++x, src += 4, out += 4) {
        const int bb = v8::toLevel(src[2]), gg = v8::toLevel(src[1]), rr = v8::toLevel(src[0]), aa = v8::toLevel(src[3]);
        int ob = bb, og = gg, orr = rr;
        if (flags == 4) {
            float Y = (float)gg * 0.587f;
            Y = Y + (float)rr * 0.299f;
            Y = Y + (float)bb * 0.114f;
            Y = Y * 4.f;
            const int idx = v8::roundMagic((double)Y);
            const int* d = &t.d[4 * idx];
            int r2 = rr + d[2], g2 = gg + d[1], b2 = bb + d[0];
            r2 = r2 < 0 ? 0 : (r2 > aa ? aa : r2);
            g2 = g2 < 0 ? 0 : (g2 > aa ? aa : g2);
            b2 = b2 < 0 ? 0 : (b2 > aa ? aa : b2);
            orr = r2; og = g2; ob = b2;
        } else if (flags != 0) {
            float Y = (float)gg * 0.587f;
            Y = Y + (float)rr * 0.299f;
            Y = Y + (float)bb * 0.114f;
            float U = (float)bb - Y, V = (float)rr - Y;
            U = U * 0.492f;
            V = V * 0.877f;
            int idx = 0;
            const bool useIdx = (flags & 5) != 0 || t.lum;
            if (useIdx) idx = v8::roundMagic((double)(Y * 4.f));
            const CcEntry& e = t.e[useIdx ? idx : 0];
            float Yp = (flags & 1) ? e.y : Y;
            if (flags & 4) { U = U + e.b; V = V + e.a; }
            if (flags & 2) { U = U * e.s; V = V * e.s; }
            const float fa = (float)aa;
            float nb = U * 2.032520294189453f;
            nb = nb + Yp;
            float ng = Yp - U * 0.394731342792511f;
            ng = ng - V * 0.5808091759681702f;
            float nr = V * 1.1402509212493896f;
            nr = nr + Yp;
            nr = (0.f > nr) ? 0.f : (fa < nr ? fa : nr);
            ng = (0.f > ng) ? 0.f : (fa < ng ? fa : ng);
            nb = (0.f > nb) ? 0.f : (fa < nb ? fa : nb);
            orr = v8::roundMagic((double)nr) & 255;
            og = v8::roundMagic((double)ng) & 255;
            ob = v8::roundMagic((double)nb) & 255;
        }
        out[0] = v8::fromLevel(orr);
        out[1] = v8::fromLevel(og);
        out[2] = v8::fromLevel(ob);
        out[3] = v8::fromLevel(aa);
    }
}


// ---- Color Corrector (Secondary), 8-bit: Vfx1.ofx's secondary processor. The settings become a flag word (1 gain/gamma/offset, 2 saturation, 4 chrominance, 8 alpha, 0x10 rotate hue,
// 0x20/0x40/0x80 a luminance / saturation / hue limit); with only 1, 2, 4 and the luminance limit the primary corrector's machinery runs on a table whose entries follow the mask.
struct CcsSet {
    double ang = 0, amt = 0, rot = 0, sat = 1, gamma = 1, gain = 1, off = 0, alpha = 1;
    bool show = false, invert = false, enL = false, enS = false, enH = false;
    double l0 = 0, l1 = 255, ls = 0, s0 = 0, s1 = 162, ss = 0, hc = 0, hw = 360, hs = 0;
};
int ccsFlags(CcsSet& p, bool& inv)
{
    int fl = 0;
    if (p.enL) {
        if (p.invert || 255.0 > p.l1 - p.l0) fl |= 0x20;
        else { p.l0 = 0; p.l1 = 255; }
    } else { p.l0 = 0; p.l1 = 255; }
    if (p.enS) {
        if (p.invert || 162.0 > p.s1 - p.s0) fl |= 0x40;
        else { p.s0 = 0; p.s1 = 162; }
    } else { p.s0 = 0; p.s1 = 162; }
    if (p.enH && (p.invert || 360.0 > p.hw)) fl |= 0x80;
    else p.hw = 360;
    inv = fl != 0 && p.invert;
    if (p.off != 0.0 || p.gamma != 1.0 || p.gain != 1.0) fl |= 1;
    if (p.sat != 1.0) fl |= 2;
    if (p.amt != 0.0) fl |= 4;
    if (p.alpha != 1.0) fl |= 8;
    if (p.rot != 0.0 && p.rot != 360.0) fl |= 0x10;
    if (((fl & 0x20) && p.l0 == p.l1 && p.ls == 0.0) || ((fl & 0x40) && p.s0 == p.s1 && p.ss == 0.0) || ((fl & 0x80) && p.hw == 0.0 && p.hs == 0.0))
        fl &= inv ? 0x1f : 0;
    return fl;
}
// A limit's window over a range 0..cap: 1 inside [lo, hi], falling off as exp(-d^2 / (2 (0.3 s)^2)) over 'smooth' s outside (cut to 0 below exp(-15)).
struct CcsWindow {
    double lo, hi, sl, sh, k1 = 0, k2 = 0;
    CcsWindow(double L0, double L1, double smooth, double cap)
    {
        lo = (0.0 > L0) ? 0.0 : ((cap < L0) ? cap : L0);
        hi = (lo > L1) ? lo : ((cap < L1) ? cap : L1);
        const double room = cap - hi;
        sl = (lo < smooth) ? lo : smooth;
        sh = (room < smooth) ? room : smooth;
        if (sl > 0) { const double q = sl * 0.3; k1 = -1.0 / ((q + q) * q); }
        if (sh > 0) { const double q = sh * 0.3; k2 = -1.0 / ((q + q) * q); }
    }
    float at(double x) const
    {
        if (lo > x && sl != 0.0) {
            double q = lo - x;
            q = q * q;
            q = q * k1;
            return (-15.0 > q) ? 0.f : (float)std::exp(q);
        }
        if (x > hi && sh != 0.0) {
            double q = hi - x;
            q = q * q;
            q = q * k2;
            return (-15.0 > q) ? 0.f : (float)std::exp(q);
        }
        return (x < lo || hi < x) ? 0.f : 1.f;
    }
};
// The table of the luminance-only case ('fl' has none of 8, 0x10, 0x40, 0x80).
void ccsBuildA(CcTable& t, const CcsSet& p, int fl)
{
    t.flags = fl & 0x1f;
    t.lum = (fl & 0x20) != 0;
    const double rad = p.ang * 3.141592653589793 / 180.0;
    const double cs = std::cos(rad), sn = std::sin(rad), amt2 = p.amt * p.amt;
    const float U = (float)((cs * amt2) * 127.5), V = (float)((sn * amt2) * 127.5);
    t.e.assign(1024, CcEntry{V, U, 0.f, (float)p.sat});
    ccYFill(t.e, (fl & 1) != 0, p.off, p.gamma, p.gain);
    if (t.lum) {
        const CcsWindow win(p.l0, p.l1, p.ls, 255.0);
        for (int i = 0; i < 1024; ++i) {
            const double x = (double)i * 0.25;
            double m = (double)win.at(x);
            if (p.invert) m = 1.0 - m;
            CcEntry& e = t.e[i];
            const double y = e.y, a = e.a, b = e.b;
            e.y = (float)((y - x) * m + x);
            e.b = (float)(b * m);
            e.a = (float)(a * m);
            e.s = (float)((p.sat - 1.0) * m + 1.0);
        }
    }
    if ((fl & 3) == 0) ccMakeD(t);
}

// The general case: three mask tables (luminance by Y x 4, saturation by the chroma radius x 4, hue by the colour's direction), each pixel's weight their product (or its complement).
struct Ccs8 {
    int fl = 0;
    bool show = false, inv = false;
    std::vector<float> lum, dy, sat, hue;
    float tintV = 0, tintU = 0, satF = 1, alphaF = 1, rot[4] = {1, 0, 0, 1};
};
void ccsBuildGeneral(Ccs8& t, const CcsSet& p, int fl, bool inv)
{
    t.fl = fl;
    t.show = p.show;
    t.inv = inv;
    const double rad = p.ang * 3.141592653589793 / 180.0;
    const double cs = std::cos(rad), sn = std::sin(rad), amt2 = p.amt * p.amt;
    t.tintU = (float)((cs * amt2) * 127.5);
    t.tintV = (float)((sn * amt2) * 127.5);
    t.satF = (float)p.sat;
    t.alphaF = (float)p.alpha;
    if (fl & 0x18) {
        const double r = p.rot * 3.141592653589793 / 180.0;
        const float c = (float)std::cos(r), s = (float)std::sin(r);
        t.rot[0] = c; t.rot[1] = -s; t.rot[2] = s; t.rot[3] = c;
    }
    // the Y' curve as a difference to Y, and the luminance mask
    std::vector<CcEntry> e(1024);
    ccYFill(e, true, p.off, p.gamma, p.gain, true);
    t.dy.resize(1024);
    t.lum.resize(1024);
    const CcsWindow wl(p.l0, p.l1, p.ls, 256.0);
    for (int i = 0; i < 1024; ++i) {
        t.dy[i] = e[i].y;
        t.lum[i] = wl.at((double)i * 0.25);
    }
    t.sat.resize(648);
    const CcsWindow ws(p.s0, p.s1, p.ss, 162.0);
    for (int i = 0; i < 648; ++i) t.sat[i] = ws.at((double)i * 0.25);
    // hue: 175 rows (the ratio of the smaller to the larger chroma axis, in 240ths) x 8 (octants)
    t.hue.resize(1400);
    const double hw = std::min(p.hw * 0.5, 180.0);
    const double lo = 180.0 - hw, hi = hw + 180.0, shift = 180.0 - p.hc;
    const double sm = (lo < p.hs) ? lo : p.hs;
    double k = 0;
    if (sm > 0) { const double q = sm * 0.3; k = -1.0 / ((q + q) * q); }
    for (int i = 0; i < 175; ++i) {
        const float a = std::asin((float)i / 240.f) * 57.29578018188477f;
        const float b = 90.f - a;
        const float ang[8] = {a, 180.f - a, 360.f - a, a + 180.f, b, 180.f - b, 360.f - b, b + 180.f};
        for (int j = 0; j < 8; ++j) {
            double v = (double)ang[j] + shift;
            if (0.0 > v) v += 360.0;
            else if (v > 360.0) v += -360.0;
            float m;
            if (lo > v && sm != 0.0) {
                double q = lo - v;
                q = q * q;
                q = q * k;
                m = (-15.0 > q) ? 0.f : (float)std::exp(q);
            } else if (v > hi && sm != 0.0) {
                double q = hi - v;
                q = q * q;
                q = q * k;
                m = (-15.0 > q) ? 0.f : (float)std::exp(q);
            } else {
                m = (v < lo || hi < v) ? 0.f : 1.f;
            }
            t.hue[i * 8 + j] = m;
        }
    }
}
void ccsRowGeneral(const Ccs8& t, const float* src, float* out, int n)
{
    const float winv = t.inv ? 1.f : 0.f;
    for (int x = 0; x < n; ++x, src += 4, out += 4) {
        const int bb = v8::toLevel(src[2]), gg = v8::toLevel(src[1]), rr = v8::toLevel(src[0]), aa = v8::toLevel(src[3]);
        int ob = bb, og = gg, orr = rr, oa = aa;
        const float fb = (float)bb, fg = (float)gg, fr = (float)rr;
        float Y = fr * 0.299f;
        Y = Y + fg * 0.587f;
        Y = Y + fb * 0.114f;
        float U = (fb - Y) * 0.492f, V = (fr - Y) * 0.877f;
        const float rad = std::sqrt(V * V + U * U);
        int row = 0, oct = 0;
        if (rad > 0.f) {
            const float aU = std::fabs(U), aV = std::fabs(V);
            float mn = aV;
            if (aV > aU) { oct = 4; mn = aU; }
            uint32_t ub, vb;
            std::memcpy(&ub, &U, 4);
            std::memcpy(&vb, &V, 4);
            oct |= (int)(((vb & ~(1u << 30)) | (ub >> 1)) >> 30);
            row = v8::roundMagic((double)((mn / rad) * 240.f));
        }
        int iy = v8::roundMagic((double)(Y * 4.f)), ir = v8::roundMagic((double)(rad * 4.f));
        iy = iy < 0 ? 0 : (iy > 1023 ? 1023 : iy);
        ir = ir < 0 ? 0 : (ir > 647 ? 647 : ir);
        row = row < 0 ? 0 : (row > 174 ? 174 : row);
        float mk = t.lum[iy] * t.sat[ir];
        mk = mk * t.hue[row * 8 + oct];
        if (t.show) {
            float v = winv - mk;
            v = v * (t.inv ? 255.f : -255.f);
            v = (0.f > v) ? 0.f : (255.f < v ? 255.f : v);
            const int q = v8::roundMagic((double)v) & 255;
            ob = og = orr = oa = q;
        } else {
            const float w = std::fabs(mk - winv);
            if (w > 0.f) {
                float Ut = U + t.tintU, Vt = V + t.tintV;
                const float Yp = Y + t.dy[iy];
                const float omw = 1.f - w;
                float nb, ng, nr;
                if ((t.fl & 0x18) == 0) {
                    Ut = Ut * t.satF;
                    Vt = Vt * t.satF;
                    nb = Ut * 2.032520294189453f;
                    nb = nb + Yp;
                    ng = Yp - Ut * 0.394731342792511f;
                    ng = ng - Vt * 0.5808091759681702f;
                    nr = Vt * 1.1402509212493896f;
                    nr = nr + Yp;
                    nb = nb * w; nr = nr * w; ng = ng * w;
                    nr = nr + omw * fr;
                    ng = ng + omw * fg;
                    nb = nb + omw * fb;
                    const float fa = (float)aa;
                    nr = (0.f > nr) ? 0.f : (fa < nr ? fa : nr);
                    ng = (0.f > ng) ? 0.f : (fa < ng ? fa : ng);
                    nb = (0.f > nb) ? 0.f : (fa < nb ? fa : nb);
                } else {
                    const float M0 = t.satF * t.rot[0], M1 = t.satF * t.rot[1], M2 = t.satF * t.rot[2], M3 = t.satF * t.rot[3];
                    const float wa = w * t.alphaF;
                    float nu = M0 * Ut;
                    nu = nu + M1 * Vt;
                    float nv = M3 * Vt;
                    nv = nv + M2 * Ut;
                    float na = omw * (float)aa;
                    na = na + wa * (float)aa;
                    na = (0.f > na) ? 0.f : (255.f < na ? 255.f : na);
                    nb = nu * 2.032520294189453f;
                    nb = nb + Yp;
                    ng = Yp - nu * 0.394731342792511f;
                    ng = ng - nv * 0.5808091759681702f;
                    nr = nv * 1.1402509212493896f;
                    nr = nr + Yp;
                    nb = nb * wa; nr = nr * wa; ng = ng * wa;
                    nr = nr + omw * fr;
                    ng = ng + omw * fg;
                    nb = nb + omw * fb;
                    nr = (0.f > nr) ? 0.f : (na < nr ? na : nr);
                    ng = (0.f > ng) ? 0.f : (na < ng ? na : ng);
                    nb = (0.f > nb) ? 0.f : (na < nb ? na : nb);
                    oa = v8::roundMagic((double)na) & 255;
                }
                orr = v8::roundMagic((double)nr) & 255;
                og = v8::roundMagic((double)ng) & 255;
                ob = v8::roundMagic((double)nb) & 255;
            }
        }
        out[0] = v8::fromLevel(orr);
        out[1] = v8::fromLevel(og);
        out[2] = v8::fromLevel(ob);
        out[3] = v8::fromLevel(oa);
    }
}

void ccsRead(Ctx& c, CcsSet& p)
{
    c.xy("Chrominance", p.ang, p.amt);
    p.rot = c.d("RotateHue");
    p.sat = c.d("Saturation");
    p.gamma = c.d("Gamma");
    p.gain = c.d("Gain");
    p.off = c.d("Offset");
    p.alpha = c.d("Alpha");
    p.show = c.b("ShowMask");
    p.invert = c.b("InvertMask");
    p.enL = c.b("EnableLimitLuminance");
    p.enS = c.b("EnableLimitSaturation");
    p.enH = c.b("EnableLimitHue");
    p.l0 = c.d("LimitLuminanceLow"); p.l1 = c.d("LimitLuminanceHigh"); p.ls = c.d("LimitLuminanceSmooth");
    p.s0 = c.d("LimitSaturationLow"); p.s1 = c.d("LimitSaturationHigh"); p.ss = c.d("LimitSaturationSmooth");
    p.hc = c.d("LimitHueCenter"); p.hw = c.d("LimitHueWidth"); p.hs = c.d("LimitHueSmooth");
}

bool colorCorrectorSecondary8(Ctx& c, RowFn& fn)
{
    CcsSet p;
    ccsRead(c, p);
    bool inv = false;
    const int fl = ccsFlags(p, inv);
    if (!(fl & 0xd8) && !p.show) {
        auto table = std::make_shared<CcTable>();
        ccsBuildA(*table, p, fl);
        fn = [table](const float* src, float* out, int n, int) { ccRow8(*table, src, out, n); };
        return true;
    }
    if ((fl & 0x1f) == 0 && !p.show) {   // (limits and nothing to apply: the picture goes through as it is)
        auto none = std::make_shared<CcTable>();
        fn = [none](const float* src, float* out, int n, int) { ccRow8(*none, src, out, n); };
        return true;
    }
    auto t = std::make_shared<Ccs8>();
    ccsBuildGeneral(*t, p, fl, inv);
    fn = [t](const float* src, float* out, int n, int) { ccsRowGeneral(*t, src, out, n); };
    return true;
}


// ------------------------------------------------------------------ Color Corrector
// Measured on VEGAS: like the secondary corrector, works on Rec. 601 Y, U, V of the straight colour. Gain, gamma and offset change Y alone
// (see secondaryGamma); then each wheel adds its tint to (U, V) weighted by the NEW Y with a Gaussian - Low exp(-(Y / 0.402)^2), Mid
// exp(-((Y - 0.5) / 0.2414)^2), High exp(-((Y - 1) / 0.402)^2) - the tint being amount^2 x 0.5 at the wheel's angle; Saturation scales the sum last.
bool colorCorrectorRows(Ctx& c, RowFn& fn)
{
    if (c.levels) {
        // (the wheels' point is (angle in degrees, amount); the settings go to VEGAS's code as they are)
        double P[10];
        const char* names[3] = {"Low", "Mid", "High"};
        for (int k = 0; k < 3; ++k) c.xy(names[k], P[2 * k], P[2 * k + 1]);
        P[6] = c.d("Offset");
        P[7] = c.d("Gamma");
        P[8] = c.d("Gain");
        P[9] = c.d("Saturation");
        auto table = std::make_shared<CcTable>();
        ccBuild(*table, P);
        fn = [table](const float* src, float* out, int n, int) { ccRow8(*table, src, out, n); };
        return true;
    }
    float du[3], dv[3];
    const char* names[3] = {"Low", "Mid", "High"};
    for (int k = 0; k < 3; ++k) {
        double a, m;
        c.xy(names[k], a, m);
        const double am = clampd(m, -2, 2), t = (am < 0 ? -am * am : am * am) * 0.5, an = a * kPi / 180.0;
        du[k] = (float)(std::cos(an) * t);
        dv[k] = (float)(std::sin(an) * t);
    }
    const float sat = (float)clampd(c.d("Saturation"), 0, 100);
    float gc, ga;
    secondaryGamma(c.d("Gamma"), gc, ga);
    const bool gammaOn = std::fabs(ga - 1.f) > 1e-6f || std::fabs(gc) > 1e-6f;
    const float gain = (float)clampd(c.d("Gain"), -100, 100);
    const float offset = (float)(clampd(c.d("Offset"), -1000, 1000) / 255.0);
    fn = [=](const float* src, float* out, int n, int) {
        for (int x = 0; x < n; ++x) {
            const float* ps = src + (size_t)x * 4;
            float* po = out + (size_t)x * 4;
            const float a = ps[3];
            if (!(a > 0.f)) { po[0] = po[1] = po[2] = po[3] = 0.f; continue; }
            const bool has = a > 1e-6f;
            const float r = has ? ps[0] / a : 0.f, g = has ? ps[1] / a : 0.f, b = has ? ps[2] / a : 0.f;
            const float Y = 0.299f * r + 0.587f * g + 0.114f * b;
            const float U = 0.492f * (b - Y), V = 0.877f * (r - Y);
            float Yp = Y * gain;
            if (Yp < 0.f) Yp = 0.f;
            if (gammaOn) {
                const float base = (Yp + gc) / (1.f + gc);
                Yp = base > 0.f ? std::pow(base, ga) : 0.f;
            }
            Yp += offset;
            Yp = Yp < 0.f ? 0.f : (Yp > 1.f ? 1.f : Yp);
            const float e0 = Yp / 0.402f, e1 = (Yp - 0.5f) / 0.2414f, e2 = (Yp - 1.f) / 0.402f;
            const float wl = std::exp(-e0 * e0), wm = std::exp(-e1 * e1), wh = std::exp(-e2 * e2);
            const float u2 = (U + du[0] * wl + du[1] * wm + du[2] * wh) * sat, v2 = (V + dv[0] * wl + dv[1] * wm + dv[2] * wh) * sat;
            const float nr = Yp + v2 / 0.877f, nb = Yp + u2 / 0.492f;
            const float ng = (Yp - 0.299f * nr - 0.114f * nb) / 0.587f;
            po[0] = clamp01(nr) * a;
            po[1] = clamp01(ng) * a;
            po[2] = clamp01(nb) * a;
            po[3] = a;
        }
    };
    return true;
}
void colorCorrector(Ctx& c) { runRows(c, colorCorrectorRows); }

// ------------------------------------------------------------------ Convolution Kernel
// A 3 x 3 kernel over each pixel's neighbours (read at places turned by Rotation, in degrees), divided by the kernel sum when Auto normalize
// is on and the sum is not 0, else multiplied by Scale, then Offset added (0..1). Colour only; alpha is kept.
static void convolutionKernelRaw(Ctx& c)
{
    double k[3][3];
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) k[r][col] = clampd(c.d(strfmt("Row%dColumn%d", r + 1, col + 1).c_str()), -1e4, 1e4);
    double sum = 0;
    for (auto& row : k) for (double v : row) sum += v;
    // VEGAS: a Scale outside 0..4 is no usable factor and the picture comes out normalised (reference "every setting at its smallest":
    // all weights -100, Scale -100, Auto normalize off gives the plain 3 x 3 average)
    const double scaleIn = c.d("Scale");
    const bool scaleBad = !(scaleIn >= 0.0 && scaleIn <= 4.0);
    const bool norm = (c.b("AutoNormalize") || scaleBad) && std::fabs(sum) > 1e-9;
    // (normalising: each weight is divided by the sum first, as VEGAS does; then no factor is left)
    const float mul = (float)(norm ? 1.0 : clampd(c.d("Scale"), -1e4, 1e4));
    const float off = (float)(clampd(c.d("Offset"), -100, 100) * 255.0);      // (in levels)
    const double rot = c.d("Rotation") * kPi / 180.0;
    const double cr = std::cos(rot), sr = std::sin(rot);
    const bool turned = std::fabs(rot) > 1e-9;
    const bool between = c.scale < 0.999;
    // the straight (unpremultiplied) frame, so dark edges of soft alpha do not bleed in
    const int W = c.out.w, H = c.out.h;
    const bool levels = c.levels;
    Buf s;
    s.ensure(W, H);
    std::vector<char> rowBad((size_t)H, 0);
    forRows(H, W, [&](int y) {
        const float* in = c.src.at(0, y);
        float* o = s.at(0, y);
        int x = 0;
#ifdef RENO_PIXROWS_SSE2
        const V4 tiny = v4::set(1e-6f), zero = v4::zero();
        for (; x + 4 <= W; x += 4) {
            V4 r, g, b, a;
            v4::load4(in + (size_t)x * 4, r, g, b, a);
            const V4 has = v4::gt(a, tiny);
            v4::store4(o + (size_t)x * 4, v4::vand(has, v4::div(r, a)), v4::vand(has, v4::div(g, a)), v4::vand(has, v4::div(b, a)), a);
        }
        (void)zero;
#endif
        for (; x < W; ++x) {
            const float* q = in + (size_t)x * 4;
            float* d = o + (size_t)x * 4;
            const float a = q[3];
            for (int ch = 0; ch < 3; ++ch) d[ch] = a > 1e-6f ? q[ch] / a : 0.f;
            d[3] = a;
        }
        rowBad[(size_t)y] = pix::allFinite(o, (size_t)W * 4) ? 0 : 1;
        // VEGAS works on levels (0..255) in float and truncates to whole levels: a value within float noise of a whole level becomes exactly that level
        for (int i = 0; i < W; ++i)
            for (int ch = 0; ch < 3; ++ch) {
                const float t = o[(size_t)i * 4 + ch] * 255.f, r = std::floor(t + 0.5f);
                o[(size_t)i * 4 + ch] = (levels && std::fabs(t - r) < 2e-3f) ? r : t;
            }
    });
    // the taps that count (a zero tap adds nothing), in the plain loop's order
    struct Tap { float kv; int dx, dy; double fdx, fdy; };
    std::vector<Tap> taps;
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            if (k[r][col] != 0) taps.push_back(Tap{norm ? (float)k[r][col] / (float)sum : (float)k[r][col], col - 1, r - 1, (double)(col - 1), (double)(r - 1)});
    auto plainPixel = [&](int x, int y) {
        float acc[3] = {0, 0, 0};
        for (const Tap& tp : taps) {
            float t[4];
            if (between) s.sampleClamped(x + 0.5 + (tp.fdx * cr - tp.fdy * sr) * c.scale, y + 0.5 + (tp.fdx * sr + tp.fdy * cr) * c.scale, t);
            else if (turned) s.sampleClamped(x + 0.5 + tp.fdx * cr - tp.fdy * sr, y + 0.5 + tp.fdx * sr + tp.fdy * cr, t);
            else {
                const float* q = s.at(std::min(W - 1, std::max(0, x + tp.dx)), std::min(H - 1, std::max(0, y + tp.dy)));
                t[0] = q[0]; t[1] = q[1]; t[2] = q[2];
            }
            for (int ch = 0; ch < 3; ++ch) acc[ch] += tp.kv * t[ch];
        }
        const float a = s.at(x, y)[3];
        premul(c.out.at(x, y), acc[0] * mul + off, acc[1] * mul + off, acc[2] * mul + off, a);
    };
    // Where a tap reads for a pixel: the sample position is a column part plus a row part, so the start pixel and next-pixel weight along each
    // axis are computed once per column and once per row and tap. Same arithmetic as Buf::sampleClamped (far-edge or odd places take the plain way: 'ok' false).
    struct Axis { int i0 = 0; float f = 0.f; bool ok = false; };
    const int nt = (int)taps.size();
    auto axisOf = [](double pos, int n, Axis& a) {
        const double cp = pos < 0.5 ? 0.5 : (pos > n - 0.5 ? n - 0.5 : pos);
        const double ps = cp - 0.5;
        a.ok = ps >= 0.0 && ps < (double)(n - 1);
        if (a.ok) { a.i0 = (int)ps; a.f = (float)(ps - a.i0); }
    };
    auto tapX = [&](const Tap& tp, int x, Axis& a) {
        if (between) axisOf(x + 0.5 + (tp.fdx * cr - tp.fdy * sr) * c.scale, W, a);
        else if (turned) axisOf(x + 0.5 + tp.fdx * cr - tp.fdy * sr, W, a);
        else { a.i0 = std::min(W - 1, std::max(0, x + tp.dx)); a.f = 0.f; a.ok = true; }
    };
    auto tapY = [&](const Tap& tp, int y, Axis& a) {
        if (between) axisOf(y + 0.5 + (tp.fdx * sr + tp.fdy * cr) * c.scale, H, a);
        else if (turned) axisOf(y + 0.5 + tp.fdx * sr + tp.fdy * cr, H, a);
        else { a.i0 = std::min(H - 1, std::max(0, y + tp.dy)); a.f = 0.f; a.ok = true; }
    };
    // the shortcuts below drop terms whose weight is exactly 0: the same sum for finite values; frames with infinities or NaNs take the plain way
    bool finite = true;
    for (char bad : rowBad) if (bad) finite = false;
    if (!finite || nt == 0 || nt > 9) {
        forRows(H, W, [&](int y) { for (int x = 0; x < W; ++x) plainPixel(x, y); });
        return;
    }
    std::vector<Axis> cols((size_t)nt * W);
    std::vector<char> colOk((size_t)W, 1), runOk((size_t)W + 1, 0);
    for (int t = 0; t < nt; ++t)
        for (int x = 0; x < W; ++x) {
            Axis& a = cols[(size_t)t * W + x];
            tapX(taps[(size_t)t], x, a);
            if (!a.ok) colOk[(size_t)x] = 0;
        }
    {
        const int xm = W / 2;
        if (colOk[(size_t)xm])
            for (int x = 0; x < W; ++x) {
                bool same = colOk[(size_t)x] != 0;
                for (int t = 0; t < nt && same; ++t) {
                    const Axis& a = cols[(size_t)t * W + x];
                    const Axis& r = cols[(size_t)t * W + xm];
                    same = a.f == r.f && a.i0 - x == r.i0 - xm;
                }
                runOk[(size_t)x] = same ? 1 : 0;
            }
    }
    const V4 vmul = v4::set(mul), voff = v4::set(off);
    float alphaBits[4];
    {
        const uint32_t m[4] = {0, 0, 0, 0xFFFFFFFFu};
        std::memcpy(alphaBits, m, sizeof m);
    }
    const V4 alphaLane = v4::ld(alphaBits);
    parallelRows(H, (size_t)W * 8, [&](int first, int end) {
        std::vector<float> accRow((size_t)W * 4);
        for (int y = first; y < end; ++y) {
            Axis rowAx[9];
            bool rowOk = true;
            for (int t = 0; t < nt; ++t) {
                tapY(taps[(size_t)t], y, rowAx[t]);
                if (!rowAx[t].ok) rowOk = false;
            }
            if (!rowOk) {
                for (int x = 0; x < W; ++x) plainPixel(x, y);
                continue;
            }
            const float* base = s.px.data();
            float* orow = c.out.at(0, y);
            const float* srow = s.at(0, y);
            auto tablePixel = [&](int x) {
                V4 acc = v4::zero();
                for (int t = 0; t < nt; ++t) {
                    const Axis& ax = cols[(size_t)t * W + x];
                    const Axis& ay = rowAx[t];
                    const float* p = base + ((size_t)ay.i0 * W + ax.i0) * 4;
                    const float ix = 1 - ax.f, iy = 1 - ay.f;
                    const float* p01 = p + (size_t)W * 4;
                    V4 v = v4::mul(v4::ld(p), v4::set(ix * iy));
                    if (ax.f != 0.f) v = v4::add(v, v4::mul(v4::ld(p + 4), v4::set(ax.f * iy)));
                    if (ay.f != 0.f) {
                        v = v4::add(v, v4::mul(v4::ld(p01), v4::set(ix * ay.f)));
                        if (ax.f != 0.f) v = v4::add(v, v4::mul(v4::ld(p01 + 4), v4::set(ax.f * ay.f)));
                    }
                    acc = v4::add(acc, v4::mul(v4::set(taps[(size_t)t].kv), v));
                }
                const V4 res = v4::mul(v4::add(v4::mul(acc, vmul), voff), v4::set(srow[(size_t)x * 4 + 3]));
                v4::st(orow + (size_t)x * 4, v4::blend(alphaLane, v4::ld(srow + (size_t)x * 4), res));
            };
            int x = 0;
            while (x < W) {
                if (!runOk[(size_t)x]) {
                    if (colOk[(size_t)x]) tablePixel(x); else plainPixel(x, y);
                    ++x;
                    continue;
                }
                int xe = x;
                while (runOk[(size_t)xe]) ++xe;
                const int n = xe - x;
                float* acc = accRow.data();
                std::memset(acc, 0, (size_t)n * 4 * sizeof(float));
                const int xm = W / 2;
                for (int t = 0; t < nt; ++t) {
                    const Axis& ar = cols[(size_t)t * W + xm];
                    const Axis& ay = rowAx[t];
                    const int d = ar.i0 - xm;
                    const float* p = base + ((size_t)ay.i0 * W + (size_t)(x + d)) * 4;
                    const float* p01 = p + (size_t)W * 4;
                    const V4 vkv = v4::set(taps[(size_t)t].kv);
                    const float fx = ar.f, fy = ay.f, ix = 1 - fx, iy = 1 - fy;
                    if (fx == 0.f && fy == 0.f) {
                        for (int i = 0; i < n; ++i) v4::st(acc + (size_t)i * 4, v4::add(v4::ld(acc + (size_t)i * 4), v4::mul(vkv, v4::ld(p + (size_t)i * 4))));
                    } else if (fx == 0.f) {
                        const V4 w0 = v4::set(ix * iy), w2 = v4::set(ix * fy);
                        for (int i = 0; i < n; ++i) {
                            const V4 v = v4::add(v4::mul(v4::ld(p + (size_t)i * 4), w0), v4::mul(v4::ld(p01 + (size_t)i * 4), w2));
                            v4::st(acc + (size_t)i * 4, v4::add(v4::ld(acc + (size_t)i * 4), v4::mul(vkv, v)));
                        }
                    } else if (fy == 0.f) {
                        const V4 w0 = v4::set(ix * iy), w1 = v4::set(fx * iy);
                        for (int i = 0; i < n; ++i) {
                            const V4 v = v4::add(v4::mul(v4::ld(p + (size_t)i * 4), w0), v4::mul(v4::ld(p + (size_t)i * 4 + 4), w1));
                            v4::st(acc + (size_t)i * 4, v4::add(v4::ld(acc + (size_t)i * 4), v4::mul(vkv, v)));
                        }
                    } else {
                        const V4 w0 = v4::set(ix * iy), w1 = v4::set(fx * iy), w2 = v4::set(ix * fy), w3 = v4::set(fx * fy);
                        for (int i = 0; i < n; ++i) {
                            V4 v = v4::add(v4::mul(v4::ld(p + (size_t)i * 4), w0), v4::mul(v4::ld(p + (size_t)i * 4 + 4), w1));
                            v = v4::add(v, v4::mul(v4::ld(p01 + (size_t)i * 4), w2));
                            v = v4::add(v, v4::mul(v4::ld(p01 + (size_t)i * 4 + 4), w3));
                            v4::st(acc + (size_t)i * 4, v4::add(v4::ld(acc + (size_t)i * 4), v4::mul(vkv, v)));
                        }
                    }
                }
                for (int i = 0; i < n; ++i) {
                    const float* sp = srow + (size_t)(x + i) * 4;
                    const V4 res = v4::mul(v4::add(v4::mul(v4::ld(acc + (size_t)i * 4), vmul), voff), v4::set(sp[3]));
                    v4::st(orow + (size_t)(x + i) * 4, v4::blend(alphaLane, v4::ld(sp), res));
                }
                x = xe;
            }
        }
    });
}

// VEGAS cuts the result to whole levels (truncates it): done on the straight colour, over the picture the plain code made.
// 8-bit pictures: VEGAS's own byte routine (Vfx1, convolutionkernelProcessor<uchar,4>), step for step in float. The kernel is a ring of eight
// weights round the centre; Rotation turns the ring by interpolating between neighbouring weights (45 degrees a step), the weights are scaled
// (Scale, or 1 / sum with Auto normalize), the 3 x 3 sums run over the stored bytes (alpha too; colour is then capped by the alpha) and are cut.
static void convolutionKernel8(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    float A[9], off;
    bool alphaPlain;
    convolutionKernel8Setup(c, A, off, alphaPlain);
    // the picture in whole levels (premultiplied, as stored)
    std::vector<uint8_t> in((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* q = c.src.at(x, y);
            for (int k = 0; k < 4; ++k) in[((size_t)y * W + x) * 4 + k] = (uint8_t)v8::toLevel(q[k]);
        }
    auto clampv = [](float v) { return 0.f > v ? 0.f : std::min(255.f, v); };
    forRows(H, W, [&](int y) {
        // rows of the image as VEGAS reads them: the bottom row of the kernel (weights 0..2) is the image row below
        const uint8_t* rows[3];
        for (int j = 0; j < 3; ++j) rows[j] = &in[(size_t)std::min(H - 1, std::max(0, y + 1 - j)) * W * 4];
        for (int x = 0; x < W; ++x) {
            float acc[4] = {0.f, 0.f, 0.f, 0.f};            // r, g, b, a
            for (int j = 0; j < 3; ++j)
                for (int t = 0; t < 3; ++t) {
                    const uint8_t* q = rows[j] + (size_t)std::min(W - 1, std::max(0, x + t - 1)) * 4;
                    const float wt = A[j * 3 + t];
                    for (int k = 0; k < 3; ++k) acc[k] = acc[k] + (float)q[k] * wt;
                    if (!alphaPlain) acc[3] = acc[3] + (float)q[3] * wt;
                }
            if (alphaPlain) acc[3] = (float)rows[1][(size_t)std::min(W - 1, x + 1) * 4 + 3];
            const float a = clampv(acc[3]);
            float* o = c.out.at(x, y);
            for (int k = 0; k < 3; ++k) {
                const float v = std::min(clampv(acc[k] + off), a);
                o[k] = v8::fromLevel((int)v);
            }
            o[3] = v8::fromLevel((int)a);
        }
    });
}

void convolutionKernel(Ctx& c)
{
    if (c.levels) { convolutionKernel8(c); return; }
    convolutionKernelRaw(c);
    const bool levels = c.levels;
    forRows(c.out.h, c.out.w, [&](int y) {
        float* o = c.out.at(0, y);
        for (int x = 0; x < c.out.w; ++x, o += 4) {
            const float a = o[3];
            if (!(a > 1e-6f)) continue;
            for (int k = 0; k < 3; ++k) {
                const float v = o[k] / a;
                o[k] = (levels ? (!(v >= 0.f) ? 0.f : std::floor(std::min(v, 255.f))) : v) / 255.f * a;
            }
        }
    });
}

// ------------------------------------------------------------------ Spherize
// Inside an ellipse round Center (Scaling: size as a fraction of the frame) the frame bulges (Amount > 0) or pinches (Amount < 0). VEGAS's CPU processor: pixel indices, rows bottom first, centre
// (int)((2 Center - 0.5) * size); a pixel at (dx, dy) is inside when r^2 = (2 dx / (W (Horizontal + 0.0001)))^2 + (2 dy / (H (Vertical + 0.0001)))^2 < 1 and is read from centre + (dx, dy) *
// (1 - 0.8 Amount cos(r pi / 2)), bilinearly with 8-bit weights (edges clamped). Amount 0 or a scaling of 0 is the identity.
void spherize(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    const double amount = c.d("Amount"), sh = c.d("ScalingHorizontal"), sv = c.d("ScalingVertical");
    if (W <= 0 || H <= 0 || std::fabs((float)amount) <= 1e-6f || std::fabs((float)sh) <= 1e-6f || std::fabs((float)sv) <= 1e-6f) { c.out.px = c.src.px; return; }
    const int cxp = (int)((pcx * 2 - 0.5) * W), cyp = (int)((pcy * 2 - 0.5) * H);
    const double ky = 1.0 / (sv + 0.0001), kx = 1.0 / (sh + 0.0001) * 2.0 / W;
    const bool q8 = c.levels;
    forRows(H, W, [&](int yt) {
        const int j = H - 1 - yt;                                   // VEGAS's row (bottom first)
        const int dy = j - cyp;
        const double yy0 = 2.0 * dy / H * ky, yy = yy0 * yy0;
        for (int i = 0; i < W; ++i) {
            const int dx = i - cxp;
            const double xx = dx * kx, r2 = xx * xx + yy;
            float* o = c.out.at(i, yt);
            if (r2 >= 1.0) { std::memcpy(o, c.src.at(i, yt), sizeof(float) * 4); continue; }
            const double f = 1.0 - std::cos(std::sqrt(r2) * 3.1415926 * 0.5) * amount * 0.8;
            const double sy = f * dy + cyp, sx = f * dx + cxp;
            const int iy = (int)std::nearbyint(sy - 0.499999985), ix = (int)std::nearbyint(sx - 0.499999985);
            const int y0 = std::min(std::max(iy, 0), H - 1), y1 = std::min(std::max(iy + 1, 0), H - 1);
            const int x0 = std::min(std::max(ix, 0), W - 1), x1 = std::min(std::max(ix + 1, 0), W - 1);
            const float* p00 = c.src.at(x0, H - 1 - y0);
            const float* p10 = c.src.at(x1, H - 1 - y0);
            const float* p01 = c.src.at(x0, H - 1 - y1);
            const float* p11 = c.src.at(x1, H - 1 - y1);
            if (q8) {
                const int fy = (int)((sy - iy) * 256.0), fx = (int)((sx - ix) * 256.0);
                for (int k = 0; k < 4; ++k) {
                    auto v8 = [](float v) { return roundNonNeg(std::min(std::max(v, 0.f), 1.f) * 255.f); };
                    const int a = (v8(p00[k]) * (256 - fx) + v8(p10[k]) * fx) * (256 - fy) + (v8(p01[k]) * (256 - fx) + v8(p11[k]) * fx) * fy;
                    o[k] = (float)(a >> 16) * (1.f / 255.f);
                }
            } else {
                const double fy = sy - iy, fx = sx - ix;
                for (int k = 0; k < 4; ++k)
                    o[k] = (float)((p00[k] * (1 - fx) + p10[k] * fx) * (1 - fy) + (p01[k] * (1 - fx) + p11[k] * fx) * fy);
            }
        }
    });
}

Def filter(const char* id, const char* label, const char* what, std::vector<PDef> params, void (*fn)(Ctx&))
{
    Def d;
    d.id = std::string("com.vegascreativesoftware:") + id;
    d.label = label;
    d.group = kGroup;
    d.description = std::string(what) + " (Reno's own, standing in for VEGAS " + label + ").";
    d.params = std::move(params);
    d.render = fn;
    return d;
}

// A colour wheel point (angle in degrees, amount): not a position in the frame.
PDef wheelPoint(const std::string& name, const std::string& label)
{
    PDef p = point(name, label, 0, 0);
    p.normalised = false;
    p.min = -360; p.max = 360; p.dmin = 0; p.dmax = 360;
    return p;
}

}  // namespace

// The table of a CcTable for the kernel "colorcorrector8": 1024 entries of (a, b, y, s), then the 4096 integers of the wheels-only case.
static void ccTableGpu(const CcTable& t, std::vector<float>& tab)
{
    tab.assign(4096 + 4096, 0.f);
    for (int i = 0; i < 1024; ++i) {
        tab[4 * (size_t)i + 0] = t.e[(size_t)i].a;
        tab[4 * (size_t)i + 1] = t.e[(size_t)i].b;
        tab[4 * (size_t)i + 2] = t.e[(size_t)i].y;
        tab[4 * (size_t)i + 3] = t.e[(size_t)i].s;
    }
    if (!t.d.empty())
        for (size_t i = 0; i < 4096; ++i) tab[4096 + i] = (float)t.d[i];
}

// Color Corrector, 8-bit, for the GPU: the CPU's tables (ccBuild) as floats and the flags.
int colorCorrector8Gpu(Ctx& c, std::vector<float>& tab)
{
    double P[10];
    const char* names[3] = {"Low", "Mid", "High"};
    for (int k = 0; k < 3; ++k) c.xy(names[k], P[2 * k], P[2 * k + 1]);
    P[6] = c.d("Offset");
    P[7] = c.d("Gamma");
    P[8] = c.d("Gain");
    P[9] = c.d("Saturation");
    CcTable t;
    ccBuild(t, P);
    ccTableGpu(t, tab);
    return t.flags;
}

// Color Corrector (Secondary), 8-bit, for the GPU (colorCorrectorSecondary8 above): mode 0 runs on the primary corrector's kernel with the table (flags bit 0x20: the luminance
// mask picks the entry), mode 1 is the general case with the mask tables (lum 1024, dy 1024, sat 648, hue 1400).
void ccs8Gpu(Ctx& c, Ccs8Gpu& o)
{
    CcsSet p;
    ccsRead(c, p);
    bool inv = false;
    const int fl = ccsFlags(p, inv);
    if (!(fl & 0xd8) && !p.show) {
        CcTable t;
        ccsBuildA(t, p, fl);
        o.mode = 0;
        o.flags = t.flags | (t.lum ? 0x20 : 0);
        ccTableGpu(t, o.tab);
        return;
    }
    if ((fl & 0x1f) == 0 && !p.show) {
        o.mode = 0;
        o.flags = 0;
        o.tab.assign(8192, 0.f);
        return;
    }
    Ccs8 t;
    ccsBuildGeneral(t, p, fl, inv);
    o.mode = 1;
    o.flags = t.fl;
    o.show = t.show;
    o.inv = t.inv;
    o.tintU = t.tintU; o.tintV = t.tintV; o.satF = t.satF; o.alphaF = t.alphaF;
    for (int k = 0; k < 4; ++k) o.rot[k] = t.rot[k];
    o.tab.clear();
    o.tab.insert(o.tab.end(), t.lum.begin(), t.lum.end());
    o.tab.insert(o.tab.end(), t.dy.begin(), t.dy.end());
    o.tab.insert(o.tab.end(), t.sat.begin(), t.sat.end());
    o.tab.insert(o.tab.end(), t.hue.begin(), t.hue.end());
}


void secondaryGammaCurve(double gamma, float& c, float& a) { secondaryGamma(gamma, c, a); }

void addFilters2(std::vector<Def>& out)
{
    {
        Def d = filter("colorcorrectorsecondary", "Color Corrector (Secondary)", "A range of colours (by brightness, saturation and hue) recoloured or made transparent",
                       {wheelPoint("Chrominance", "Chrominance"), dbl("RotateHue", "Rotate hue", 0, -3600, 3600), dbl("Saturation", "Saturation", 1, 0, 10),
                        dbl("Gamma", "Gamma", 1, 0.01, 10), dbl("Gain", "Gain", 1, 0, 10), dbl("Offset", "Offset", 0, -255, 255), dbl("Alpha", "Alpha", 1, 0, 1),
                        button("SelectEffectRange", "Select effect range"), boolean("ShowMask", "Show mask", false), boolean("InvertMask", "Invert mask", false),
                        group("LimitLuminance", "Limit luminance"), boolean("EnableLimitLuminance", "Enable", true, "LimitLuminance"),
                        dbl("LimitLuminanceLow", "Low", 0, 0, 255, "LimitLuminance"), dbl("LimitLuminanceHigh", "High", 255, 0, 255, "LimitLuminance"),
                        dbl("LimitLuminanceSmooth", "Smooth", 0, 0, 255, "LimitLuminance"),
                        group("LimitSaturation", "Limit saturation"), boolean("EnableLimitSaturation", "Enable", true, "LimitSaturation"),
                        dbl("LimitSaturationLow", "Low", 0, 0, 162, "LimitSaturation"), dbl("LimitSaturationHigh", "High", 162, 0, 162, "LimitSaturation"),
                        dbl("LimitSaturationSmooth", "Smooth", 0, 0, 162, "LimitSaturation"),
                        group("LimitHue", "Limit hue"), boolean("EnableLimitHue", "Enable", true, "LimitHue"),
                        dbl("LimitHueCenter", "Center", 180, 0, 360, "LimitHue"), dbl("LimitHueWidth", "Width", 360, 0, 360, "LimitHue"),
                        dbl("LimitHueSmooth", "Smooth", 0, 0, 360, "LimitHue")},
                       colorCorrectorSecondary);
        for (auto& p : d.params) {
            if (p.name == "RotateHue") { p.dmin = -360; p.dmax = 360; }
            if (p.name == "Saturation" || p.name == "Gamma" || p.name == "Gain") p.dmax = 4;
            if (p.name == "Gamma") p.min = 0;   // (VEGAS accepts 0: its curve then is the steepest)
        }
        d.changed = secondaryChanged;
        d.rows = colorCorrectorSecondaryRows;
        out.push_back(d);
    }
    {
        Def d = filter("colorcorrector", "Color Corrector", "Shadows, midtones and highlights pushed towards colours; saturation, gain, gamma and offset",
                       {wheelPoint("Low", "Low"), wheelPoint("Mid", "Mid"), wheelPoint("High", "High"), dbl("Saturation", "Saturation", 1, 0, 10),
                        dbl("Gamma", "Gamma", 1, 0, 10), dbl("Gain", "Gain", 1, 0, 10), dbl("Offset", "Offset", 0, -255, 255)},
                       colorCorrector);
        for (auto& p : d.params) if (p.name == "Saturation" || p.name == "Gamma" || p.name == "Gain") p.dmax = 4;
        d.rows = colorCorrectorRows;
        out.push_back(d);
    }
    {
        std::vector<PDef> ps;
        for (int r = 1; r <= 3; ++r) {
            const std::string g = strfmt("Row%d", r);
            ps.push_back(group(g, strfmt("Row %d", r)));
            for (int col = 1; col <= 3; ++col) {
                PDef p = dbl(strfmt("Row%dColumn%d", r, col), strfmt("Column %d", col), (r == 2 && col == 2) ? 1 : 0, -1000, 1000, g);
                p.dmin = -20; p.dmax = 20;
                ps.push_back(p);
            }
        }
        ps.push_back(dbl("Rotation", "Rotation", 0, -360, 360));
        ps.push_back(dbl("Scale", "Scale", 1, -100, 100));
        ps.back().dmin = 0; ps.back().dmax = 4;
        ps.push_back(dbl("Offset", "Offset", 0, -1, 1));
        ps.push_back(boolean("AutoNormalize", "Auto normalize", true));
        out.push_back(filter("convolutionkernel", "Convolution Kernel", "Each pixel made from its neighbours by a 3 x 3 kernel (blur, sharpen, emboss, edges)", ps,
                             convolutionKernel));
    }
    {
        Def d = filter("spherize", "Spherize", "The picture bulged out or pinched in",
                       {point("Center", "Center", 0.5, 0.5), dbl("Amount", "Amount", 1, -1, 1), group("Scaling", "Scaling"),
                        dbl("ScalingHorizontal", "Horizontal", 1, 0, 10, "Scaling"), dbl("ScalingVertical", "Vertical", 1, 0, 10, "Scaling"),
                        boolean("ScalingProportional", "Proportional", true, "Scaling")},
                       spherize);
        for (auto& p : d.params) if (p.name == "ScalingHorizontal" || p.name == "ScalingVertical") p.dmax = 2;
        out.push_back(d);
    }
    out.push_back(filter("newsprint", "Newsprint", "The picture as printed dots",
                         {dbl("DotSize", "Dot size", 0, 0, 1), choice("Type", "Type", 0, {"Color", "Monochrome", "Offset color"})}, vegasNewsprint));
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
