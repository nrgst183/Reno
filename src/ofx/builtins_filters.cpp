// Built-in stand-ins for the VEGAS Pro 18 stock video effects, with VEGAS's identifiers and parameter names, for when
// VEGAS's plug-ins are not installed. The output approximates VEGAS's. See builtins.h.
#include <mutex>
#include "builtins_impl.h"
#include "builtins_vec.h"
#include "builtins_deriche.h"
#include "vegas8.h"
#include "vkfilters.h"
#include <algorithm>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

// Runs fn(r, g, b) on every pixel's straight colour (alpha kept), from src into out.
template <class F>
void perPixel(Ctx& c, F fn)
{
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            if (a <= 0.f) { std::memset(c.out.at(x, y), 0, sizeof(float) * 4); continue; }
            fn(r, g, b);
            premul(c.out.at(x, y), r, g, b, a);
        }
    });
}

// ------------------------------------------------------------------ colour
// The colour effects below are row functions, four pixels at a time (see Def::rows); each channel does the same arithmetic as the plain version.
typedef v4::V4 V4;

// Invert: on 8-bit levels, each level moves towards its inverse (255 - level) by floor(|difference| * amount) + 1 levels (never past it),
// so even amount 0 shifts a level by one.
bool invertRows(Ctx& c, RowFn& fn)
{
    if (c.levels) {
        // 8-bit: VEGAS's own steps (Vfx1.ofx invertProcessor<unsigned char>): an opaque pixel gets 255 - level; otherwise the colour is taken to straight with an
        // integer division (c 255 / a, at most 255), inverted and put back as (level a) >> 8. The result is mixed with the picture by the weight
        // w = round((1 - amount) 256), each channel: P + trunc((S - P) w / 256), P being the inverted picture, S the original.
        const float amount = (float)clampd(c.d("BlendAmount"), 0, 1);
        const float wf = (float)(1.0 - (double)amount) * 256.f;
        const int w = std::min(255, v8::roundMagic((double)wf));      // (256 comes out as 255: the weight is a byte, saturated)
        fn = [w](const float* s, float* o, int n, int) {
            for (int x = 0; x < n; ++x, s += 4, o += 4) {
                const int a = v8::toLevel(s[3]);
                int v[3] = {v8::toLevel(s[0]), v8::toLevel(s[1]), v8::toLevel(s[2])};
                int pr[3];
                if (a == 255) {
                    for (int i = 0; i < 3; ++i) pr[i] = 255 - v[i];
                } else {
                    for (int i = 0; i < 3; ++i) {
                        const int t = a == 0 ? 0 : std::min(255, v[i] * 255 / a);
                        pr[i] = ((255 - t) * a) >> 8;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    const int prod = (v[i] - pr[i]) * w;
                    o[i] = v8::fromLevel((pr[i] + ((prod + (prod < 0 ? 255 : 0)) >> 8)) & 255);
                }
                o[3] = v8::fromLevel(a);
            }
        };
        return true;
    }
    const float k = (float)clampd(c.d("BlendAmount"), 0, 1);
    fn = [k](const float* s, float* o, int n, int) {
        for (int x = 0; x < n; ++x) {
            const float* p = s + (size_t)x * 4;
            float* q = o + (size_t)x * 4;
            const float a = p[3];
            if (!(a > 0.f)) { q[0] = q[1] = q[2] = q[3] = 0.f; continue; }
            for (int i = 0; i < 3; ++i) {
                const float lv = std::floor((a > 1e-6f ? p[i] / a : 0.f) * 255.f + 0.5f);
                const float d = 255.f - 2.f * lv;
                const float ad = std::fabs(d);
                const float step = std::min(ad, std::floor(ad * k + 1e-4f) + 1.f);
                q[i] = (lv + (d < 0 ? -step : step)) / 255.f * a;
            }
            q[3] = a;
        }
    };
    return true;
}
void invert(Ctx& c) { runRows(c, invertRows); }

// Black and White: on 8-bit levels, the grey is floor(sum / 3); each level moves towards it by ceil(|difference| * amount) levels.
// Amount reads slightly high at 25% (65/256, measured), exact at 50% and 75%.
bool blackAndWhiteRows(Ctx& c, RowFn& fn)
{
    float k = (float)clampd(c.d("BlendAmount"), 0, 1);
    if (k > 0.f && k < 0.375f) k += 1.f / 256.f;
    fn = [k](const float* s, float* o, int n, int) {
        for (int x = 0; x < n; ++x) {
            const float* p = s + (size_t)x * 4;
            float* q = o + (size_t)x * 4;
            const float a = p[3];
            if (!(a > 0.f)) { q[0] = q[1] = q[2] = q[3] = 0.f; continue; }
            float lv[3];
            for (int i = 0; i < 3; ++i) lv[i] = std::floor((a > 1e-6f ? p[i] / a : 0.f) * 255.f + 0.5f);
            const float grey = std::floor((lv[0] + lv[1] + lv[2]) / 3.f);
            for (int i = 0; i < 3; ++i) {
                const float d = grey - lv[i];
                const float step = std::ceil(std::fabs(d) * k - 1e-5f);
                const float out = lv[i] + (d < 0 ? -step : step);
                q[i] = out / 255.f * a;
            }
            q[3] = a;
        }
    };
    return true;
}
void blackAndWhite(Ctx& c) { runRows(c, blackAndWhiteRows); }

// Sepia on 8-bit levels of the premultiplied picture (alpha untouched). Grey = floor(sum / 3); a mix is c + (target - c) * w / 256 truncated.
// Strength s: up to 1/3 each level goes towards the grey by w = 1.5 * s * 255; up to 2/3 the same, then towards the colour by a weight
// from a table of the grey; above that the table level is (s - 1/3) * 1.5 capped at 1 and the grey itself is mixed towards the colour.
// The colour's levels are its components times 255, truncated.
// (the integer settings of the above, shared with the Vulkan plan: vkfilters.cpp)
void sepiaSettings(Ctx& c, SepiaSettings& o)
{
    const Color col = c.color("Color");
    const double s = c.d("BlendingStrength"), falloff = c.d("BlendingFalloff");
    for (int i = 0; i < 3; ++i) o.colour[i] = (int)((i == 0 ? col.r : i == 1 ? col.g : col.b) * 255.0);
    int& toGrey = o.toGrey;
    int* lut = o.lut;
    bool& useLut = o.useLut;
    toGrey = 0;
    useLut = false;
    if (s <= 1.0 / 3.0) {
        toGrey = (int)(s * 1.5 * 255.0) & 0xff;
    } else {
        const double a = 1.5 * s, v = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
        double lvl = (s - 1.0 / 3.0) * 1.5;
        if (s <= 2.0 / 3.0) {
            toGrey = (int)(v * 255.0);
        } else {
            lvl = lvl < 0.0 ? 0.0 : (lvl > 1.0 ? 1.0 : lvl);
        }
        const int level = (int)(lvl * 255.0) & 0xff;
        double acc = 0.0;
        for (int i = 0; i < 256; ++i) {
            const double t = (double)level + acc;
            lut[i] = (int)(t < 0.0 ? 0.0 : (t > 255.0 ? 255.0 : t));
            acc += -falloff;
        }
        useLut = true;
    }
    o.greyMix = s <= 2.0 / 3.0;
}
bool sepiaRows(Ctx& c, RowFn& fn)
{
    SepiaSettings st;
    sepiaSettings(c, st);
    fn = [st](const float* src, float* o, int n, int) {
        const int toGrey = st.toGrey;
        const bool useLut = st.useLut, greyMix = st.greyMix;
        const int* colour = st.colour;
        const int* lut = st.lut;
        auto lv = [](float v) { const float t = v * 255.f + 0.5f; return t < 0.f ? 0 : (t > 255.f ? 255 : (int)t); };
        for (int x = 0; x < n; ++x) {
            const float* p = src + (size_t)x * 4;
            float* q = o + (size_t)x * 4;
            int v[3] = {lv(p[0]), lv(p[1]), lv(p[2])};
            const int grey = (v[0] + v[1] + v[2]) / 3;
            for (int i = 0; i < 3; ++i) {
                int r = v[i];
                if (greyMix) r = r + ((grey - r) * toGrey) / 256;
                else r = grey;
                if (useLut) r = r + ((colour[i] - r) * lut[grey]) / 256;
                q[i] = (float)(r & 0xff) / 255.f;
            }
            q[3] = p[3];
        }
    };
    return true;
}
void sepia(Ctx& c) { runRows(c, sepiaRows); }

// Automatic Contrast: brightness and contrast (about 0.5) that stretch the frame's brightness to Min..Max after cutting Percent of the
// histogram (half at each end). A 256-bin luma histogram accumulates every frame given (Current Clip adds a few frames).
struct LumaHistogram {
    double bins[256] = {};
    double total = 0;
    void add(const Buf& b)
    {
        for (size_t i = 0; i + 3 < b.px.size(); i += 4) {
            const float a = b.px[i + 3];
            if (!(a > 1e-4f)) continue;
            const float l = luma(b.px[i] / a, b.px[i + 1] / a, b.px[i + 2] / a);
            bins[(int)(clamp01(l) * 255.f + 0.5f)] += 1;
            total += 1;
        }
    }
    bool range(double percent, double& lo, double& hi) const
    {
        if (total < 1) return false;
        const double cut = clampd(percent, 0, 49) * 0.01 * 0.5 * total;
        double acc = 0;
        int a = 0, b = 255;
        for (; a < 255; ++a) { acc += bins[a]; if (acc > cut) break; }
        acc = 0;
        for (; b > 0; --b) { acc += bins[b]; if (acc > cut) break; }
        lo = a / 255.0;
        hi = b / 255.0;
        return true;
    }
};

// Settings mapping lo..hi to outMin..outMax with the effect's formula out = (v + brightness - 0.5) * f + 0.5,
// f = 1 / (1 - contrast) for positive contrast, 1 + contrast for negative.
bool autoContrast(double lo, double hi, double outMin, double outMax, double& brightness, double& contrast)
{
    if (!(hi - lo > 0.004) || !(outMax - outMin > 0.004)) return false;
    const double f = (outMax - outMin) / (hi - lo);
    contrast = f >= 1 ? 1.0 - 1.0 / f : f - 1.0;
    const double ff = contrast >= 0 ? 1.0 / std::max(0.01, 1.0 - contrast) : 1.0 + contrast;
    brightness = clampd((outMin - 0.5) / ff + 0.5 - lo, -1, 1);
    contrast = clampd(contrast, -1, 1);
    return true;
}

// The analysis modes change nothing at render time ("Set for each Frame independently" leaves the frame unchanged at default
// Brightness and Contrast); the settings come only from "Calculate Brightness & Contrast".
void brightnessContrastSettings(Ctx& c, float& bright, float& contrast, float& center)
{
    bright = (float)c.d("Brightness");
    contrast = (float)clampd(c.d("Contrast"), -1, 1);
    center = (float)c.d("ContrastCenter");
}

// VEGAS limits the value to 0..1 and truncates to 1/255 steps (no rounding); Contrast 1 is an exact threshold at the centre.
RowFn brightnessContrastRow(float bright, float contrast, float center)
{
    const float f = contrast >= 0 ? 1.f / std::max(1e-6f, 1.f - contrast) : 1.f + contrast;
    return [=](const float* s, float* o, int n, int) {
        for (int x = 0; x < n; ++x) {
            const float* p = s + (size_t)x * 4;
            float* q = o + (size_t)x * 4;
            const float a = p[3];
            if (!(a > 0.f)) { q[0] = q[1] = q[2] = q[3] = 0.f; continue; }
            for (int k = 0; k < 3; ++k) {
                float v = ((a > 1e-6f ? p[k] / a : 0.f) + bright - center) * f + center;
                v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                q[k] = std::floor(v * 255.f + 1e-3f) * (1.f / 255.f) * a;
            }
            q[3] = a;
        }
    };
}

bool brightnessContrastRows(Ctx& c, RowFn& fn)
{
    float bright, contrast, center;
    brightnessContrastSettings(c, bright, contrast, center);
    fn = brightnessContrastRow(bright, contrast, center);
    return true;
}

void brightnessContrast(Ctx& c)
{
    float bright, contrast, center;
    brightnessContrastSettings(c, bright, contrast, center);
    runRowFn(c, brightnessContrastRow(bright, contrast, center));
}

// "Calculate Brightness & Contrast": computes Brightness, Contrast and Contrast center from the current frame, or (Current Clip) from
// frames spread over the clip.
void brightnessContrastChanged(Change& ch)
{
    if (ch.param != "DoAnalysis") return;
    LumaHistogram h;
    Buf frame;
    if (ch.i("ReferenceMode") == 1) {
        double first, last;
        ch.clipRange(first, last);
        if (last > first + 1) {
            const int n = 24;
            for (int k = 0; k < n; ++k)
                if (ch.source(std::floor(first + (last - first - 1) * k / (n - 1) + 0.5), frame)) h.add(frame);
        }
    }
    if (h.total < 1 && ch.source(ch.time, frame)) h.add(frame);
    double lo, hi, B, C;
    if (!h.range(ch.d("Percent"), lo, hi) || !autoContrast(lo, hi, ch.d("Min"), ch.d("Max"), B, C)) return;
    ch.setD("Brightness", B);
    ch.setD("Contrast", C);
    ch.setD("ContrastCenter", 0.5);
}

// Border: the two colour settings kept the same.
void borderChanged(Change& ch)
{
    if (ch.param == "color_rgb") {
        Color c = ch.color("color");
        const Color n = ch.color("color_rgb");
        c.r = n.r; c.g = n.g; c.b = n.b;
        ch.setColor("color", c);
    } else if (ch.param == "color") {
        Color c = ch.color("color");
        c.a = 1;
        ch.setColor("color_rgb", c);
    }
}

void rgbToHsl(float r, float g, float b, float& h, float& s, float& l)
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

float hue2rgb(float p, float q, float t)
{
    t -= std::floor(t);
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}

void hslToRgb(float h, float s, float l, float& r, float& g, float& b)
{
    if (s <= 0) { r = g = b = l; return; }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue2rgb(p, q, h + 1.f / 3);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.f / 3);
}

// VEGAS's HSL Adjust on 8-bit bytes (SfDibHSLAdjust and its pixel helpers, Vfx1), all in doubles in VEGAS's order. A pixel's premultiplied bytes /255 (x 0.00392156862745098) go
// to H, S, L by the Foley-van Dam routine (hue via (max - c) / (max - min) per channel, / 6); Hue = H + AddToHue (less 1 when over 1), Saturation x Saturation, Lightness x Luminance
// (both kept in 0..1). Back to bytes (truncated, no rounding): two pixels at a time VEGAS uses its SSE routine (m2 = L S + L for L <= 0.5, else (S - L S) + L; the hue sector from
// H x 6, a hue of 1 counting as 0), a last odd pixel of a row the scalar one ((S + 1) L ... and v = ((m2 - m1) / m2) m2 f). Colour bytes are then cut down to the alpha byte.
struct HslD { double h, s, l, a; };

HslD hslFromBytes(const unsigned char* bgra)
{
    const double k = 0.00392156862745098;
    const double b = (double)bgra[0] * k, g = (double)bgra[1] * k, r = (double)bgra[2] * k, a = (double)bgra[3] * k;
    HslD o = {0.0, 0.0, 0.0, a};
    double mx = std::max(r, g);
    if (!(mx > b)) mx = b;
    double mn = std::min(r, g);
    if (!(b > mn)) mn = b;
    const double sum = mn + mx;
    const double L = sum * 0.5;
    o.l = L;
    if (!(0.0 < L)) return o;
    const double d = mx - mn;
    if (!(d > 0.0)) return o;
    double den = sum;
    if (!(0.5 >= L)) den = (2.0 - mx) - mn;
    o.s = d / den;
    const double inv = 1.0 / d;
    const double rc = (mx - r) * inv, gc = (mx - g) * inv, bc = (mx - b) * inv;
    double h;
    if (mx == r) h = (g == mn) ? bc + 5.0 : 1.0 - gc;
    else if (mx == g) h = (b == mn) ? rc + 1.0 : 3.0 - bc;
    else h = (r == mn) ? gc + 3.0 : 5.0 - rc;
    o.h = h * 0.16666666666666666;
    return o;
}

// The two-pixel (SSE) conversion of one pixel: bytes B G R in out[0..2], alpha byte in out[3].
void hslToBytesPair(const HslD& p, unsigned char* out)
{
    const double L = p.l, S = p.s;
    const double LS = L * S;
    const double m2 = (L <= 0.5) ? (LS + L) : ((S - LS) + L);
    out[0] = out[1] = out[2] = 0;
    out[3] = (unsigned char)(int)(p.a * 255.0);
    if (!(0.0 < m2)) return;
    const double m1 = (L + L) - m2;
    double h6 = p.h * 6.0;
    if (h6 == 6.0) h6 = 0.0;
    const int sector = (int)h6;
    const double f = h6 - (double)sector;
    const double v = f * (m2 - m1);
    const double q = m2 - v, t = v + m1;
    const int M = (int)(m2 * 255.0), P = (int)(m1 * 255.0), T = (int)(t * 255.0), Q = (int)(q * 255.0);
    const bool even = (sector & 1) == 0;
    const int X9 = even ? M : Q, X7 = even ? T : M;
    const int s2 = sector >> 1;
    const int G = s2 == 0 ? X7 : (s2 == 1 ? X9 : P);
    const int R = s2 == 0 ? X9 : (s2 == 1 ? P : X7);
    const int B = s2 == 0 ? P : (s2 == 1 ? X7 : X9);
    out[0] = (unsigned char)B;
    out[1] = (unsigned char)G;
    out[2] = (unsigned char)R;
}

// The scalar conversion (a row's odd last pixel).
void hslToBytesScalar(const HslD& p, unsigned char* out)
{
    const double L = p.l, S = p.s;
    const double m2 = (L <= 0.5) ? (S + 1.0) * L : (S + L) - S * L;
    int al = (int)(p.a * 255.0);
    al = al < 0 ? 0 : (al > 255 ? 255 : al);
    out[3] = (unsigned char)al;
    if (!(0.0 < m2)) { out[0] = out[1] = out[2] = 0; return; }
    const double h6 = p.h * 6.0;
    const double m1 = (L + L) - m2;
    const int sec = (int)h6;
    const double f = h6 - (double)sec;
    const double v = (((m2 - m1) / m2) * m2) * f;
    const double q = m2 - v, t = v + m1;
    if (sec < 0 || sec > 6) return;
    double r, g, b;
    switch (sec) {
    case 1: r = q; g = m2; b = m1; break;
    case 2: r = m1; g = m2; b = t; break;
    case 3: r = m1; g = q; b = m2; break;
    case 4: r = t; g = m1; b = m2; break;
    case 5: r = m2; g = m1; b = q; break;
    default: r = m2; g = t; b = m1; break;
    }
    out[2] = (unsigned char)(int)(r * 255.0);
    out[1] = (unsigned char)(int)(g * 255.0);
    out[0] = (unsigned char)(int)(b * 255.0);
}

void hslAdjust(Ctx& c)
{
    const double addHue = c.d("AddToHue"), sat = c.d("Saturation"), lum = c.d("Luminance");
    if (c.levels) {
        const int W = c.out.w, H = c.out.h;
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                const float* s = c.src.at(x, y);
                unsigned char in[4] = {(unsigned char)v8::toLevel(s[2]), (unsigned char)v8::toLevel(s[1]), (unsigned char)v8::toLevel(s[0]), (unsigned char)v8::toLevel(s[3])};
                HslD p = hslFromBytes(in);
                p.h = p.h + addHue;
                if (p.h > 1.0) p.h = p.h - 1.0;
                double ns = p.s * sat;
                p.s = 0.0 > ns ? 0.0 : std::min(1.0, ns);
                double nl = p.l * lum;
                p.l = 0.0 > nl ? 0.0 : std::min(1.0, nl);
                unsigned char o[4];
                const bool tail = (W & 1) && x == W - 1;
                if (tail) hslToBytesScalar(p, o); else hslToBytesPair(p, o);
                for (int k = 0; k < 3; ++k) if (o[k] >= o[3]) o[k] = o[3];
                float* d = c.out.at(x, y);
                d[0] = v8::fromLevel(o[2]);
                d[1] = v8::fromLevel(o[1]);
                d[2] = v8::fromLevel(o[0]);
                d[3] = v8::fromLevel(o[3]);
            }
        });
        return;
    }
    const float addH = (float)addHue, sa = (float)std::max(0.0, sat), lu = (float)std::max(0.0, lum);
    perPixel(c, [&](float& r, float& g, float& b) {
        float h, s, l;
        rgbToHsl(clamp01(r), clamp01(g), clamp01(b), h, s, l);
        hslToRgb(h + addH, clamp01(s * sa), clamp01(l * lu), r, g, b);
    });
}

}  // namespace

void sepiaSettingsGpu(Ctx& c, SepiaSettings& o) { sepiaSettings(c, o); }

// Levels' gamma curve (VEGAS's curve builder for G = 1 / Gamma, read from the plug-in): a power with a straight piece near 0, where
// k0 = 0.0381838 G^0.9418 starts the power and the slope below is G^-1.8836. For G < 1 the power is (1 + a) t^G - a, for G > 1 it is
// ((t + a) / (1 + a))^G, with a chosen to join the straight piece at k0 (a = 0.337 at Gamma 5). t and the result are 0..1.
double levelsCurve(double t, double gamma)
{
    const double G = 1.0 / gamma;
    if (std::fabs(gamma - 1.0) < 1e-6) return t;
    const double k0 = std::pow(1.0 / G, -0.9418051973077521) * 0.03818376618407356;
    const double slope = std::pow(1.0 / G, 1.8836103946155043);
    if (G < 1.0) {
        const double t1 = std::pow(k0, G);
        const double a = (slope * k0 - t1) / (t1 - 1.0);
        return t >= k0 ? std::pow(t, G) * (a + 1.0) - a : t * slope;
    }
    const double u = slope * k0, w = std::pow(u, 1.0 / G);
    const double a = (k0 - w) / (w - 1.0), inv = 1.0 / (a + 1.0);
    return t >= k0 ? std::pow((a + t) * inv, G) : t * slope;
}

// levelsCurve() with what it works out from the gamma alone (four pow calls) done once: the same values, so the same results.
struct LevelsCurveFixed {
    double G = 1.0, k0 = 0, slope = 0, t1 = 0, a = 0, inv = 0;
    bool identity = true;
    explicit LevelsCurveFixed(double gamma)
    {
        G = 1.0 / gamma;
        if (std::fabs(gamma - 1.0) < 1e-6) return;
        identity = false;
        k0 = std::pow(1.0 / G, -0.9418051973077521) * 0.03818376618407356;
        slope = std::pow(1.0 / G, 1.8836103946155043);
        if (G < 1.0) {
            t1 = std::pow(k0, G);
            a = (slope * k0 - t1) / (t1 - 1.0);
        } else {
            const double u = slope * k0, w = std::pow(u, 1.0 / G);
            a = (k0 - w) / (w - 1.0);
            inv = 1.0 / (a + 1.0);
        }
    }
    double operator()(double t) const
    {
        if (identity) return t;
        if (G < 1.0) return t >= k0 ? std::pow(t, G) * (a + 1.0) - a : t * slope;
        return t >= k0 ? std::pow((a + t) * inv, G) : t * slope;
    }
};

void levelsGammaTable(double g, float* lut)
{
    g = clampd(g, 0.2, 10);
    for (int i = 0; i <= 4096; ++i) {
        const double r = levelsCurve(i / 4096.0, g);
        lut[i] = (float)(r < 0 ? 0 : (r > 1 ? 1 : r));
    }
}

namespace {

// Levels. VEGAS uses a 256-entry table: input clipped to Input Start..End (at least 1/255 apart), scaled to 0..1, gamma-curved, mapped to
// Output Start..End and rounded. Reno computes the same on the actual value. Channel 4 applies the table to colour and alpha.
void levels(Ctx& c)
{
    const int ch = c.i("Channel");
    const double i0 = c.d("InputStart"), o0 = c.d("OutputStart"), o1 = c.d("OutputEnd"), gamma = c.d("Gamma");
    double i1 = c.d("InputEnd"), width = i1 - i0;
    if (std::fabs(width) < 0.003921568859368563) { i1 = i0 + 0.003921568859368563; width = 0.003921568859368563; }
    const bool straight = std::fabs(gamma - 1.0) < 1e-6;
    const LevelsCurveFixed curve(gamma);
    auto f = [&](float v) {
        const double x = v;
        const double xin = i0 > x ? i0 : std::min(i1, x);
        const double t = xin - i0;
        double r;
        if (straight) r = t * (o1 - o0) / width;
        else r = (o1 - o0) * curve(t / width);
        return (float)(r + o0);
    };
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            if (ch == 0 || ch == 1 || ch == 4) r = f(r);
            if (ch == 0 || ch == 2 || ch == 4) g = f(g);
            if (ch == 0 || ch == 3 || ch == 4) b = f(b);
            if (ch == 4) a = clamp01(f(a));
            premul(c.out.at(x, y), r, g, b, a);
        }
    });
}

// Colour Balance: red, green and blue pushed in highlights, midtones or shadows (weighted by pixel brightness), optionally keeping brightness.
// 8-bit pictures: VEGAS's byte routine (Vfx1, colorbalanceProcessor<uchar,4>). Groups of four pixels along a row take the SIMD code: weight from the
// byte sum times 1/765, each push the weight times Amount * 255 cut toward zero (with Preserve luminosity, half the push of each of the other two colours
// taken off), the result capped by the alpha. The last (width mod 4) pixels of a row take the scalar code: weight from sum / 3 (whole) / 255, rounding with
// VEGAS's magic-number round, no alpha cap.
static void colorBalance8(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double pr = (double)(float)c.d("Red"), pg = (double)(float)c.d("Green"), pb = (double)(float)c.d("Blue");
    const int type = c.i("Type");
    const bool keep = c.b("PreserveLuminosity");
    uint64_t invBits = 0x3f556ac015c06a15ull;
    double inv765;
    std::memcpy(&inv765, &invBits, 8);
    auto weight = [&](double L) {
        if (type == 2) return 1.0 - L * L;
        if (type == 1) { const double x = L - 0.5; return 1.0 - x * x * 4.0; }
        const double y = 1.0 - L;
        return 1.0 - y * y;
    };
    const int simdEnd = (W / 4) * 4;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* q = c.src.at(x, y);
            int r = v8::toLevel(q[0]), g = v8::toLevel(q[1]), b = v8::toLevel(q[2]);
            const int a = v8::toLevel(q[3]);
            int dr, dg, db;
            if (x < simdEnd) {
                const double w = weight((double)(b + g + r) * inv765);
                const int tr = (int)(w * (pr * 255.0)), tg = (int)(w * (pg * 255.0)), tb = (int)(w * (pb * 255.0));
                if (keep) {
                    const int hr = (int)(w * (pr * 127.5)), hg = (int)(w * (pg * 127.5)), hb = (int)(w * (pb * 127.5));
                    dr = tr - hb - hg; dg = tg - hb - hr; db = tb - hg - hr;
                } else { dr = tr; dg = tg; db = tb; }
                r = std::min(std::min(255, std::max(0, r + dr)), a);
                g = std::min(std::min(255, std::max(0, g + dg)), a);
                b = std::min(std::min(255, std::max(0, b + db)), a);
            } else {
                const double w = weight((double)((unsigned)(b + g + r) / 3u) / 255.0);
                const double tr = pr * w * 255.0, tg = pg * w * 255.0, tb = pb * w * 255.0;
                if (keep) {
                    const int rr = v8::roundMagic(tr), rg = v8::roundMagic(tg), rb = v8::roundMagic(tb);
                    const int qr = v8::roundMagic(tr * 0.5), qg = v8::roundMagic(tg * 0.5), qb = v8::roundMagic(tb * 0.5);
                    dr = rr - qb - qg; dg = rg - qr - qb; db = rb - qr - qg;
                } else { dr = v8::roundMagic(tr); dg = v8::roundMagic(tg); db = v8::roundMagic(tb); }
                r = std::min(255, std::max(0, r + dr));
                g = std::min(255, std::max(0, g + dg));
                b = std::min(255, std::max(0, b + db));
            }
            float* o = c.out.at(x, y);
            o[0] = v8::fromLevel(r); o[1] = v8::fromLevel(g); o[2] = v8::fromLevel(b); o[3] = v8::fromLevel(a);
        }
    });
}

void colorBalance(Ctx& c)
{
    if (c.levels) { colorBalance8(c); return; }
    const float dr = (float)c.d("Red"), dg = (float)c.d("Green"), db = (float)c.d("Blue");
    const int type = c.i("Type");
    const bool keep = c.b("PreserveLuminosity");
    // VEGAS: level = plain mean of the three channels; each push is Amount * weight, truncated to whole 8-bit levels toward zero
    // (with keep brightness, half the push is first taken off the other two channels).
    perPixel(c, [&](float& r, float& g, float& b) {
        const float m = clamp01((r + g + b) * (1.f / 3.f));
        const float w = type == 0 ? 1 - (1 - m) * (1 - m) : (type == 1 ? 4 * m * (1 - m) : 1 - m * m);
        const float tr = dr * w, tg = dg * w, tb = db * w;
        float sr = tr, sg = tg, sb = tb;
        if (keep) { sr = tr - 0.5f * (tg + tb); sg = tg - 0.5f * (tr + tb); sb = tb - 0.5f * (tr + tg); }
        auto cut = [](float v) { return std::trunc(v * 255.f + (v < 0 ? -1e-3f : 1e-3f)) * (1.f / 255.f); };
        r = clamp01(r + cut(sr)); g = clamp01(g + cut(sg)); b = clamp01(b + cut(sb));
    });
}

void threshold(Ctx& c)
{
    const float t = (float)c.d("Threshold");
    perPixel(c, [&](float& r, float& g, float& b) {
        const float v = luma(r, g, b) >= t - 1e-5f ? 1.f : 0.f;
        r = g = b = v;
    });
}

// ------------------------------------------------------------------ space
// Bilinear sample with outside taps taking the colour bg: Mirror's edges blend into the background this way (as VEGAS's do).
static void sampleOnBg(const Buf& b, double x, double y, const float* bg, float out[4])
{
    if (!std::isfinite(x) || !std::isfinite(y)) { std::memcpy(out, bg, 4 * sizeof(float)); return; }
    const double xs = x - 0.5, ys = y - 0.5;
    const double x0f = std::floor(xs), y0f = std::floor(ys);
    if (x0f < -2.0 || y0f < -2.0 || x0f > b.w + 1.0 || y0f > b.h + 1.0) { std::memcpy(out, bg, 4 * sizeof(float)); return; }
    const int x0 = (int)x0f, y0 = (int)y0f;
    const float fx = (float)(xs - x0f), fy = (float)(ys - y0f);
    const float ix = 1 - fx, iy = 1 - fy;
    const float wt[4] = {ix * iy, fx * iy, ix * fy, fx * fy};
    const int tx[4] = {x0, x0 + 1, x0, x0 + 1}, ty[4] = {y0, y0, y0 + 1, y0 + 1};
    const float* p[4];
    for (int t = 0; t < 4; ++t) p[t] = (tx[t] < 0 || ty[t] < 0 || tx[t] >= b.w || ty[t] >= b.h) ? bg : b.at(tx[t], ty[t]);
    for (int k = 0; k < 4; ++k) {
        float v = 0.f;
        v += p[0][k] * wt[0];
        v += p[1][k] * wt[1];
        v += p[2][k] * wt[2];
        v += p[3][k] * wt[3];
        out[k] = v;
    }
}

// Mirror in 8-bit for slanted lines, as VEGAS does it (float arithmetic on bytes): every row finds the column where the line crosses it; the kept side
// is copied, the other side takes the bilinear sample (taps outside the frame are the background) of the point reflected over the line.
static void mirror8General(Ctx& c, float theta, double cx, double cy)
{
    const int W = c.out.w, H = c.out.h;
    const float sx = (float)c.scale, sy = (float)c.scale;
    const float cxp = (float)(cx * (double)(W - 1)), cyp = (float)((1.0 - cy) * (double)(H - 1));
    const float u = (theta > 0.f ? 3.14159274101257f : -3.14159274101257f) - theta;
    const float t = (float)std::tan((double)u);
    const float isx = 1.0f / sx, isy = 1.0f / sy;
    const float cxs = cxp * isx, cys = cyp * isy;
    const float m1t = -1.0f / t;
    const float tcx = t * cxs, den = t - m1t;
    const Color bgc = c.color("Background");
    uint32_t bg = 0;
    {
        const double v[3] = {bgc.r, bgc.g, bgc.b};
        for (int k = 0; k < 3; ++k) {
            const int b = (int)((float)v[k] * 255.0f + 0.5f);
            bg |= (uint32_t)(uint8_t)b << (8 * k);
        }
    }
    std::vector<uint32_t> sp((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* q = c.src.at(x, y);
            sp[(size_t)y * W + x] = (uint32_t)v8::toLevel(q[0]) | ((uint32_t)v8::toLevel(q[1]) << 8) | ((uint32_t)v8::toLevel(q[2]) << 16) | ((uint32_t)v8::toLevel(q[3]) << 24);
        }
    auto tap = [&](int x, int y) -> uint32_t { return (x < 0 || y < 0 || x >= W || y >= H) ? bg : sp[(size_t)y * W + x]; };
    forRows(H, W, [&](int j) {
        const float Yj = (float)j * isy;
        const float xs = (((Yj - cys) / t) + cxs) * sx;
        const int e = (int)xs + 1;
        int mirLo = 0, mirHi = 0;          // mirrored columns [mirLo, mirHi)
        if (u > 0.f) { mirLo = std::min(e, W); if (mirLo < 0) mirLo = 0; mirHi = W; }
        else { mirLo = 0; mirHi = std::min(e, W); }
        for (int x = 0; x < W; ++x) {
            uint32_t px;
            if (x < mirLo || x >= mirHi) px = sp[(size_t)j * W + x];
            else {
                const float xu = (float)x * isx;
                const float P = (((Yj - xu * m1t) - cys) + tcx) / den;
                float srcX = ((P + P) - xu) * sx;
                float srcY = ((((P - cxs) * t + cys) + ((P - cxs) * t + cys)) - Yj) * sy;
                const int x0 = (int)std::floor(srcX), y0 = (int)std::floor(srcY);
                if (x0 < -1 || x0 >= W || y0 < -1 || y0 >= H) px = bg;
                else {
                    const float fx = srcX - (float)x0, fy = srcY - (float)y0;
                    const float ix = 1.0f - fx, iy = 1.0f - fy;
                    const uint32_t tl = tap(x0, y0), tr = tap(x0 + 1, y0), bl = tap(x0, y0 + 1), br = tap(x0 + 1, y0 + 1);
                    px = 0;
                    for (int k = 0; k < 4; ++k) {
                        const int sh = 8 * k;
                        const float a = (float)((tl >> sh) & 255), b = (float)((tr >> sh) & 255), cc = (float)((bl >> sh) & 255), d = (float)((br >> sh) & 255);
                        const float r = (a * ix + b * fx) * iy + (cc * ix + d * fx) * fy;
                        px |= (uint32_t)(uint8_t)(int)r << sh;
                    }
                }
            }
            float* o = c.out.at(x, j);
            for (int k = 0; k < 4; ++k) o[k] = v8::fromLevel((int)((px >> (8 * k)) & 255));
        }
    });
}

// Mirror: the side of the line through Center that the angle points to (0 top, 90 left, 180 bottom, -90 right) is kept and reflected.
// Measured on VEGAS: a pixel exactly on the line (by index, not centre) is reflected too; the bilinear reflection blends into Background
// at the frame edge; for slanted lines the samples shift along the kept direction (0.4 px * kx * |ky|, 0.2 px per axis at 45 degrees).
void mirror(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double cx, cy;
    c.xy("Center", cx, cy);
    if (c.levels) {
        const float th = (float)std::fmod(c.d("Angle") * 0.01745329252, 6.28318530718);
        static const float bands[8][2] = {{-0.008726646f, 0.008726646f}, {3.1328659f, 3.1503193f}, {-3.1503193f, -3.1328659f}, {1.5620697f, 1.5795230f},
                                          {-4.7211156f, -4.7036624f}, {4.7036624f, 4.7211156f}, {-1.5795230f, -1.5620697f}, {0.f, -1.f}};
        bool axis = false;
        for (int i = 0; i < 7; ++i) axis = axis || (th >= bands[i][0] && th <= bands[i][1]);
        if (!axis && std::isfinite(th)) { mirror8General(c, th, cx, cy); return; }
    }
    const double a = c.d("Angle") * kPi / 180.0;
    const double kx = -std::sin(a), ky = -std::cos(a);
    const double px0 = cx * W, py0 = (1 - cy) * H;
    const double ex = 0.4 * c.scale * kx * std::fabs(ky), ey = 0.4 * c.scale * ky * std::fabs(kx);
    const bool straight = std::fabs(kx) < 1e-9 || std::fabs(ky) < 1e-9;
    const double tieShift = 0.5 * (kx + ky);
    const Color bg = c.color("Background");
    const bool numbers = pix::allFinite(c.src.px.data(), c.src.px.size());
    std::vector<double> colTerm((size_t)W);
    for (int x = 0; x < W; ++x) colTerm[(size_t)x] = ((x + 0.5) - px0) * kx;
    float bgPix[4];
    put(bgPix, bg);
    forRows(H, W, [&](int y) {
        const double py0y = y + 0.5, rowTerm = (py0y - py0) * ky;
        for (int x = 0; x < W; ++x) {
            double px = x + 0.5, py = py0y;
            const double d = colTerm[(size_t)x] + rowTerm;
            float* o = c.out.at(x, y);
            if (d - tieShift <= 1e-6) {
                px -= 2 * d * kx; py -= 2 * d * ky;
                if (straight) {
                    if (px < -0.5 - 1e-6 || py < -0.5 - 1e-6 || px > W + 0.5 + 1e-6 || py > H + 0.5 + 1e-6) std::memcpy(o, bgPix, sizeof bgPix);
                    else c.src.sampleClamped(px, py, o);
                } else
                    sampleOnBg(c.src, px + ex, py + ey, bgPix, o);
            } else if (numbers) {
                const float* q = c.src.at(x, y);
                for (int k = 0; k < 4; ++k) o[k] = q[k] + 0.f;
            } else
                c.src.sampleClamped(px, py, o);
        }
    });
}

// VEGAS's Gaussian Blur: its Deriche Gaussian (builtins_deriche.cpp) of size Range x the frame's larger side along / across; channels switched off keep the source.
void gaussianBlurFx(Ctx& c)
{
    const bool ch[4] = {c.b("R"), c.b("G"), c.b("B"), c.b("A")};
    const double big = (double)std::max(c.out.w, c.out.h);
    Buf b = c.src;
    vegasDericheBlur(b, c.d("HorizontalRange") * big, c.d("VerticalRange") * big, c.levels);
    c.out = c.src;
    for (size_t i = 0; i + 3 < b.px.size(); i += 4)
        for (int k = 0; k < 4; ++k)
            if (ch[k]) c.out.px[i + k] = b.px[i + k];
}

void sharpen(Ctx& c)
{
    // VEGAS's Sharpen is a 3 x 3 convolution, weights -1 -2 -1 / -2 C -2 / -1 -2 -1 divided by their sum, C = 13 + 16 * (1 - Amount) (fitted):
    // pixel + (12 pixel - weighted neighbours) / (17 - 16 Amount). Amount 0 leaves the picture alone; edges repeat the border pixels.
    const double amount = clampd(c.d("Amount"), 0, 1);
    const int W = c.out.w, H = c.out.h;
    const float t = amount <= 0 ? 0.f : (float)(1.0 / (17.0 - 16.0 * amount));
    const bool levels = c.levels;
    forRows(H, W, [&](int y) {
        const float* rm = c.src.at(0, std::max(0, y - 1));
        const float* r0 = c.src.at(0, y);
        const float* rp = c.src.at(0, std::min(H - 1, y + 1));
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x) {
            const int xl = std::max(0, x - 1) * 4, xc = x * 4, xr = std::min(W - 1, x + 1) * 4;
            const float al = r0[xc + 3];
            for (int k = 0; k < 3; ++k) {
                const float corners = rm[xl + k] + rm[xr + k] + rp[xl + k] + rp[xr + k];
                const float sides = rm[xc + k] + rp[xc + k] + r0[xl + k] + r0[xr + k];
                const float v = r0[xc + k];
                const float r = v + (12.f * v - corners - 2.f * sides) * t;
                o[xc + k] = std::min(al, levels ? floor8(r) : std::max(0.f, r));
            }
            o[xc + 3] = al;
        }
    });
}

// Pixelate as VEGAS: per axis, cells of 2 / (1 - p) pixels counted from the bottom row (pixelateAxis); a cell shows the truncated average
// of the K x K window at its start, K = cell size truncated; the largest setting is one cell.
void pixelate(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    std::vector<int> sx, sy;
    int kx = 1, ky = 1;
    pixelateAxis(W, clampd(c.d("HorizontalPixelization"), 0, 1), c.scale, sx, kx);
    pixelateAxis(H, clampd(c.d("VerticalPixelization"), 0, 1), c.scale, sy, ky);
    const int nx = (int)sx.size() - 1, ny = (int)sy.size() - 1;
    // nothing set on either axis: VEGAS copies the picture bytes
    const bool copyOnly = std::fabs((float)c.d("HorizontalPixelization")) <= 1e-6f && std::fabs((float)c.d("VerticalPixelization")) <= 1e-6f;
    forRows(ny, W * 8, [&](int cy) {
        // the cell's rows counted from the bottom: yb in [sy[cy], sy[cy + 1]); its window yb in [sy[cy], sy[cy] + ky)
        const int yb0 = sy[(size_t)cy], yb1 = sy[(size_t)cy + 1];
        const int wy0 = yb0, wy1 = std::min(H, yb0 + ky);
        for (int cx = 0; cx < nx; ++cx) {
            const int x0 = sx[(size_t)cx], x1 = sx[(size_t)cx + 1];
            const int wx0 = x0, wx1 = std::min(W, x0 + kx);
            double s[4] = {0, 0, 0, 0};
            for (int yb = wy0; yb < wy1; ++yb) {
                const int y = H - 1 - yb;
                for (int x = wx0; x < wx1; ++x) {
                    const float* q = c.src.at(x, y);
                    for (int k = 0; k < 4; ++k) s[k] += c.levels ? (double)v8::toLevel(q[k]) : (double)q[k];
                }
            }
            const double n = (double)(wx1 - wx0) * (wy1 - wy0);
            float avg[4];
            for (int k = 0; k < 4; ++k) {
                if (c.levels) {
                    // VEGAS: the window's byte sum times 1 / area (double), cut; then the cell's byte goes back through a double (b / 255) and the
                    // fill multiplies by 255 and cuts: sometimes one level lower. Nothing set on either axis: the bytes are copied.
                    int b = (int)(s[k] * (1.0 / n));
                    if (!copyOnly) b = (int)std::min(255.0, ((double)b * 0.00392156862745098) * 255.0);
                    avg[k] = v8::fromLevel(b);
                } else avg[k] = (float)(s[k] / n);
            }
            for (int yb = yb0; yb < yb1; ++yb) {
                const int y = H - 1 - yb;
                for (int x = x0; x < x1; ++x) std::memcpy(c.out.at(x, y), avg, sizeof avg);
            }
        }
    });
}

// Linear blur: see vegasLinearBlur() (builtins_deriche.cpp). linearBlurTaps is an older approximation kept for callers.
}  // namespace
void linearBlurTaps(double len, double dx, double dy, int& n, double& sx, double& sy, std::vector<float>& wt)
{
    const int R = (int)len;
    const int stride = std::max(1, (int)std::ceil(R / 128.0));
    n = R / stride;
    const double sg = 0.32 * len;
    wt.assign((size_t)n + 1, 0.f);
    double sum = 0;
    for (int k = 0; k <= n; ++k) {
        const double w = std::exp(-0.5 * (double)k * stride * k * stride / (sg * sg));
        wt[(size_t)k] = (float)w;
        sum += (k ? 2 : 1) * w;
    }
    for (auto& w : wt) w = (float)(w / sum);
    sx = dx * stride; sy = dy * stride;
}
namespace {

void linearBlur(Ctx& c)
{
    // VEGAS's own (builtins_deriche.cpp): a shear, its Deriche Gaussian along the rows / columns, the shear back
    vegasLinearBlur(c.src, c.out, c.d("Angle"), c.d("Amount"), c.levels);
}

// ------------------------------------------------------------------ keying, edges, light
// Chroma keyer from VEGAS's own code: d = (sqrt(3) * |RGB - key RGB| + (2 * hue diff + |sat diff| + |lightness diff|) / 3) / 2 in floats (HSL in 0..1, hue diff folded to 0..0.5), the
// alpha byte from the ramp low..high (truncated), the blur (below) of the alpha, then a blue key limits blue to green, a green key green to red (spill removal), colours times alpha.
// 1: the key is a blue screen, 2: a green one, 0: neither
int chromaKeyDespill(const double* key) { return (key[2] > key[1] && key[2] > key[0]) ? 1 : ((key[1] > key[0] && key[1] >= key[2]) ? 2 : 0); }

// The keyer's blur (VEGAS's own, in single precision): the alpha bytes go through Deriche's recursive Gaussian along the rows (sigma sx) into a float plane (clipped to 0..255), then
// along its columns (sigma sy); the answer is cut to a whole level. size = the larger side * BlurAmount * 0.25 + 1.5 and sigma = size / sqrt(2 ln 255), all in floats; a sigma of 0.6
// or less means no blur.
struct KeyerCoefs { float A[5], B[5], C[5], D[5], E[5]; };
// VEGAS's coefficient generator for this blur (a second / third order recursive Gaussian, constants and order of operations as in Vfx1.ofx): n (causal), d (feedback), m (anti-causal),
// bn / bm (the edge terms) in doubles.
static void keyerCoefsD(double sigma, double* n, double* d, double* m, double* bn, double* bm)
{
    const double s = std::max(sigma, 0.001);
    for (int i = 0; i < 5; ++i) n[i] = d[i] = m[i] = bn[i] = bm[i] = 0.0;
    if (!(s > 100.0)) {
        const double a = 1.475 / s;
        const double sn = std::sin(a) * 1.021;
        const double e1 = std::exp(-1.512 / s);
        const double co = std::cos(a);
        const double e2 = std::exp(-1.556 / s);
        double t1 = co * 3.796;
        double x11 = e1 * sn;
        const double t6 = co * 0.8929;
        t1 = t1 * e1;
        x11 = x11 - t1;
        const double t1b = e2 * 0.8929;
        const double t0 = e1 * t6;
        x11 = x11 + t1b;
        x11 = x11 + t0;
        const double e3 = std::exp(-3.024 / s);
        const double e4 = std::exp(-3.068 / s);
        const double u0 = e4 * sn;
        const double u1 = e4 * t6;
        double x3 = e2 + e2;
        double x4 = -e2;
        double x12 = e2 * e3;
        const double w1 = u1 + u0;
        double x5 = e3 * 1.898;
        double x0 = co + co;
        x3 = x3 * co;
        x12 = -x12;
        x0 = x0 * e1;
        x5 = x5 - w1;
        double x2 = x11 + 1.0051;
        x3 = x3 * e1;
        x4 = x4 - x0;
        x2 = x2 + x5;
        x3 = x3 + e3;
        double y0 = x4 + 1.0;
        x2 = x2 + 0.0;
        y0 = y0 + x3;
        x2 = x2 + x2;
        y0 = y0 + x12;
        x2 = x2 / y0;
        x2 = x2 - 1.0051;
        n[0] = 1.0051 / x2; n[1] = x11 / x2; n[2] = x5 / x2; n[3] = 0.0 / x2; n[4] = 0.0;
        d[0] = 0.0; d[1] = x4; d[2] = x3; d[3] = x12; d[4] = 0.0;
    } else {
        const double a = 0.8448 / s;
        const double co = std::cos(a);
        const double e = std::exp(-1.26 / s);
        double x9 = co * 0.9629;
        x9 = x9 * e;
        double x0 = std::sin(a) * 1.942;
        x0 = x0 * e;
        double x10 = co + co;
        x10 = x10 * e;
        x9 = x9 - x0;
        const double e2 = std::exp(-2.52 / s);
        x9 = -x9;
        x10 = -x10;
        double x2 = x9 + 0.9629;
        double x1 = x10 + 1.0;
        x2 = x2 + x2;
        x1 = x1 + e2;
        x2 = x2 / x1;
        x2 = x2 - 0.9629;
        n[0] = 0.9629 / x2; n[1] = x9 / x2; n[2] = 0.0 / x2; n[3] = 0.0; n[4] = 0.0;
        d[0] = 0.0; d[1] = x10; d[2] = e2; d[3] = 0.0; d[4] = 0.0;
    }
    m[0] = 0.0;
    m[1] = n[1] - d[1] * n[0];
    m[2] = n[2] - d[2] * n[0];
    m[3] = n[3] - d[3] * n[0];
    m[4] = n[4] - n[0] * d[4];
    double sm = m[0] + 0.0, sn = n[0] + 0.0, sd = d[0] + 0.0;
    sm += m[1]; sn += n[1]; sd += d[1];
    sm += m[2]; sn += n[2]; sd += d[2];
    sm += m[3]; sn += n[3]; sd += d[3];
    sm += m[4]; sn += n[4]; sd += d[4];
    sd += 1.0;
    double kn = 1.0, km = 1.0;
    if (!(1e-17 > sd) && !(-sd > 1e-17)) { kn = sn / sd; km = sm / sd; }
    for (int i = 0; i < 5; ++i) { bn[i] = kn * d[i]; bm[i] = d[i] * km; }
}
KeyerCoefs keyerCoefs(float sigma)
{
    double n[5], d[5], m[5], bn[5], bm[5];
    keyerCoefsD((double)sigma, n, d, m, bn, bm);
    KeyerCoefs k;
    for (int i = 0; i < 5; ++i) { k.A[i] = (float)n[i]; k.B[i] = (float)d[i]; k.C[i] = (float)m[i]; k.D[i] = (float)bn[i]; k.E[i] = (float)bm[i]; }
    return k;
}
// One line of L values: out[i] = clip(causal[i] + anti-causal[i], 0, 255), each in floats in VEGAS's order (taps from the current value back, then the edge terms).
void keyerLine(const KeyerCoefs& k, int L, const float* in, float* out, std::vector<float>& yc, std::vector<float>& yb)
{
    yc.assign((size_t)L, 0.f);
    yb.assign((size_t)L, 0.f);
    const float xL = in[0], xR = in[L - 1];
    for (int i = 0; i < L; ++i) {
        const int cnt = i < 5 ? i : 5;
        float acc = 0.f, accb = 0.f;
        yc[(size_t)i] = 0.f;
        yb[(size_t)i] = 0.f;
        for (int j = 0; j < cnt; ++j) {
            const float t1 = in[i - j] * k.A[j];
            const float t2 = k.B[j] * yc[(size_t)(i - j)];
            acc += t1 - t2;
            const float u1 = in[L - 1 - (i - j)] * k.C[j];
            const float u2 = k.B[j] * yb[(size_t)(i - j)];
            accb += u1 - u2;
        }
        for (int j = cnt; j < 5; ++j) { acc += (k.A[j] - k.D[j]) * xL; accb += (k.C[j] - k.E[j]) * xR; }
        if (!((double)acc > 1e-17) && !((double)(-acc) > 1e-17)) acc = 0.f;
        if (!((double)accb > 1e-17) && !((double)(-accb) > 1e-17)) accb = 0.f;
        yc[(size_t)i] = acc;
        yb[(size_t)i] = accb;
    }
    for (int i = 0; i < L; ++i) {
        const float v = yb[(size_t)(L - 1 - i)] + yc[(size_t)i];
        out[i] = 0.f > v ? 0.f : std::min(255.f, v);
    }
}

// VEGAS's RGB to HSL (all 0..1; doubles, hue from the largest channel's neighbours).
static void keyerHsl(double r, double g, double b, double& h, double& sat, double& l)
{
    double mx = std::max(r, g);
    if (!(mx > b)) mx = b;
    double mn = std::min(r, g);
    if (!(b > mn)) mn = b;
    const double sum = mn + mx;
    l = sum * 0.5;
    if (!(0.0 < l)) { sat = 0.0; h = 0.0; return; }
    const double d = mx - mn;
    sat = d;
    if (!(d > 0.0)) { sat = d; h = 0.0; return; }
    double den = sum;
    if (!(0.5 >= l)) { den = 2.0; den = den - mx; den = den - mn; }
    sat = d / den;
    const double inv = 1.0 / d;
    const double rc = (mx - r) * inv, gc = (mx - g) * inv, bc = (mx - b) * inv;
    double hh;
    if (r == mx) hh = (g == mn) ? bc + 5.0 : 1.0 - gc;
    else if (g == mx) hh = (b == mn) ? rc + 1.0 : 3.0 - bc;
    else hh = (r == mn) ? gc + 3.0 : 5.0 - rc;
    h = hh * 0.16666666666666666;
}

}  // namespace
// The set-up numbers of chromaKeyer() for the Vulkan plan (vkfilters.cpp), computed by the same code as the CPU path
void chromaKeyerGpuSetup(Ctx& c, int W, int H, ChromaKeyerSetup& o)
{
    const Color kc = c.color("color");
    const double key[3] = {kc.r, kc.g, kc.b};
    const double low = clampd(c.d("LowThreshold"), 0, 1), high = clampd(c.d("HighThreshold"), 0, 1);
    keyerHsl(key[0], key[1], key[2], o.kh, o.ks, o.kl);
    const float lowF = (float)low, highF = (float)(high <= low ? low : 1.0);
    o.lowF = lowF;
    o.hiF = highF - 9.99999997e-07f;
    o.krF = (float)key[0]; o.kgF = (float)key[1]; o.kbF = (float)key[2];
    o.invRange = 0.f;
    { const float diff = highF - lowF; const float ad = (float)std::fabs((double)diff); if (!(1e-6f > ad)) o.invRange = 1.0f / diff; }
    const float amount = (float)clampd(c.d("BlurAmount"), 0, 1);
    const float size = std::max((float)W, (float)H) * amount * 0.25f + 1.5f;
    const float ln2 = std::log(0.003921568859368563f) + std::log(0.003921568859368563f);
    const float sigma = std::sqrt(-((size * size) / ln2));
    o.blur = sigma > 0.6f;
    if (o.blur) {
        const KeyerCoefs k = keyerCoefs(sigma);
        for (int i = 0; i < 5; ++i) { o.coefs[i] = k.A[i]; o.coefs[5 + i] = k.B[i]; o.coefs[10 + i] = k.C[i]; o.coefs[15 + i] = k.D[i]; o.coefs[20 + i] = k.E[i]; }
    }
    o.maskOnly = c.b("ShowMaskOnly");
    o.despill = chromaKeyDespill(key);
}
// Border's set-up numbers and the bevel's per-row spans, shared with the Vulkan plan
static void borderSpans(BorderSpans& s, float d, float extent, const int* fa, const int* fb)
{
    s.end[0] = (int)std::floor(d); s.cov[0] = 128; std::memcpy(s.face[0], fa, sizeof(int) * 4); s.hasFace[0] = true;
    s.end[1] = (int)std::ceil(d); s.cov[1] = (int)((d - (float)s.end[0]) * 128.f); std::memcpy(s.face[1], fa, sizeof(int) * 4); s.hasFace[1] = true;
    const float e = extent - d;
    s.end[2] = (int)std::floor(e); s.cov[2] = 0; s.hasFace[2] = false;
    std::memset(s.face[2], 0, sizeof(int) * 4);
    s.end[3] = (int)std::ceil(e); s.cov[3] = (int)(((float)s.end[3] - e) * 128.f); std::memcpy(s.face[3], fb, sizeof(int) * 4); s.hasFace[3] = true;
    s.end[4] = (int)extent; s.cov[4] = 128; std::memcpy(s.face[4], fb, sizeof(int) * 4); s.hasFace[4] = true;
}

void borderGeometry(Ctx& c, int W, int H, BorderGeo& g)
{
    g.W = W; g.H = H;
    g.type = c.i("Type");
    const Color col = c.color("color");
    if (W <= 0 || H <= 0) return;
    const float Wf = (float)W, Hf = (float)H;
    g.Wf = Wf; g.Hf = Hf;
    const float sz = (float)c.d("Size") * std::max(Wf, Hf);
    const float bx3 = (float)((double)sz * 0.5), by3 = sz * 0.5f;
    const float bx = bx3 < 0.f ? 0.f : std::min(Wf * 0.5f, bx3), by = by3 < 0.f ? 0.f : std::min(Hf * 0.5f, by3);
    g.bx = bx; g.by = by;
    const bool q8 = c.levels;
    const double colv[4] = {col.r, col.g, col.b, col.a};
    g.xi = (int)bx; g.xr = (int)(Wf - bx); g.yb = (int)by; g.yt = (int)(Hf - by);
    if (g.type == 2) {
        const int a8 = (int)(colv[3] * 255.0);
        const float af = (float)(a8 & 255) / 255.f;
        for (int k = 0; k < 3; ++k) {
            const int ck = (int)(colv[k] * 255.0) & 255;
            g.cc[k] = q8 ? (float)(int)((float)ck * af) : (float)(colv[k] * 255.0) * af;
        }
        g.cc[3] = (float)(a8 & 255);
    } else if (g.type == 0) {
        const float r15 = 1.5f * bx;
        const int sigma = (int)std::sqrt(-(r15 * r15) / (2.f * std::log(1.f / 255.f)));
        const float sg = (float)sigma;
        int wl = (int)std::floor(std::sqrt(12.f * sg * sg / 3.f + 1.f));
        if (wl % 2 == 0) --wl;
        const float mI = (12.f * sg * sg - (float)(3 * wl * wl) - 4.f * 3.f * (float)wl - 3.f * 3.f) / (-4.f * (float)wl - 4.f);
        const int m = (int)std::round(mI);
        for (int i = 0; i < 3; ++i) g.rads[i] = ((i < m ? wl : wl + 2) - 1) / 2;
    } else {
        const double ang = c.d("Angle") * 0.017453292519943295;
        float lx = (float)std::sin(ang), ly = -(float)std::cos(ang);
        const float s2 = std::sqrt(2.f);
        const float n0 = 0.f / s2, nm = -1.f / s2, n1 = 1.f / s2;
        const float ln = std::sqrt(lx * lx + ly * ly + 2.25f);
        lx /= ln; ly /= ln;
        const float lz = 1.5f / ln, z = n1 * lz;
        auto faceOf = [&](float dot, int* f) {
            for (int k = 0; k < 3; ++k) f[k] = (int)(colv[k] * (double)dot * 256.0 * 1.7999999523162842 + 0.5);
            f[3] = 256;
        };
        faceOf(nm * ly + n0 * lx + z, g.F[0]);        // bottom
        faceOf(n1 * ly + n0 * lx + z, g.F[1]);        // top
        faceOf(nm * lx + n0 * ly + z, g.F[2]);        // left
        faceOf(n1 * lx + n0 * ly + z, g.F[3]);        // right
        borderSpans(g.ys, by, Hf, g.F[0], g.F[1]);
    }
}

// The bevel's spans along the row y (counted from the bottom): where each face starts and how much of the pixel it covers
void borderBevelRow(const BorderGeo& g, int y, BorderSpans& xs)
{
    const BorderSpans& ys = g.ys;
    int seg = 0;
    while (seg < 4 && !(y < ys.end[seg])) ++seg;
    float d;
    if (y < ys.end[1]) d = (float)y * g.bx / g.by;
    else if (y < ys.end[3]) d = g.bx;
    else d = (g.Hf - (float)y - 1.f) * g.bx / g.by;
    borderSpans(xs, d, g.Wf, g.F[2], g.F[3]);
    const int cy = ys.cov[seg];
    if (cy != 0)
        for (int k = 0; k < 5; ++k) {
            if (xs.cov[k] == 128) continue;
            if (xs.cov[k] == 0) { xs.cov[k] = cy; std::memcpy(xs.face[k], ys.face[seg], sizeof(int) * 4); xs.hasFace[k] = true; }
            else {
                const float w4 = (float)xs.cov[k] * 0.0078125f, w3 = 1.f - w4;
                for (int ch = 0; ch < 3; ++ch) xs.face[k][ch] = (int)((float)ys.face[seg][ch] * w3 + (float)xs.face[k][ch] * w4);
                xs.face[k][3] = 256;
            }
        }
}

namespace {

void chromaKeyer(Ctx& c)
{
    const Color kc = c.color("color");
    const double key[3] = {kc.r, kc.g, kc.b};
    const double low = clampd(c.d("LowThreshold"), 0, 1), high = clampd(c.d("HighThreshold"), 0, 1);
    const int W = c.out.w, H = c.out.h;
    std::vector<float> mask((size_t)W * H);
    double kh, ks, kl;
    keyerHsl(key[0], key[1], key[2], kh, ks, kl);
    // (measured: a high threshold above the low one makes the ramp run to 1, at or below it the alpha is a hard cut at the low one)
    const float lowF = (float)low, highF = (float)(high <= low ? low : 1.0), krF = (float)key[0], kgF = (float)key[1], kbF = (float)key[2];
    float invRange = 0.f;
    { const float diff = highF - lowF; const float ad = (float)std::fabs((double)diff); if (!(1e-6f > ad)) invRange = 1.0f / diff; }
    double val[256], inv[256];
    for (int i = 0; i < 256; ++i) { val[i] = (double)i * 0.00392156862745098; inv[i] = i ? 255.0 / (double)i : 0.0; }
    forRows(H, W, [&](int y) {
        const float* in = c.src.at(0, y);
        float* mrow = &mask[(size_t)y * W];
        for (int x = 0; x < W; ++x) {
            const float* px = in + (size_t)x * 4;
            const int a = v8::toLevel(px[3]);
            const int br = v8::toLevel(px[0]), bg = v8::toLevel(px[1]), bb = v8::toLevel(px[2]);
            double r, g, b;
            if (a >= 1 && a <= 254) { r = inv[a] * val[br]; g = inv[a] * val[bg]; b = inv[a] * val[bb]; }
            else { r = val[br]; g = val[bg]; b = val[bb]; }
            double h, sat, l;
            keyerHsl(r, g, b, h, sat, l);
            float dh = (float)std::fabs(h - kh);
            const float ds = (float)std::fabs(sat - ks), dl = (float)std::fabs(l - kl);
            if (dh > 0.5f) dh = 1.0f - dh;
            const float dg = kgF - (float)g, dr = krF - (float)r, db = kbF - (float)b;
            const float dist = std::sqrt((dr * dr + dg * dg) + db * db);
            float hs = dh + dh;
            hs += ds;
            hs += dl;
            hs /= 3.0f;
            float d = dist * 1.7320508f + hs;
            d *= 0.5f;
            int m;
            if (lowF > d) m = 0;
            else if (d > highF - 9.99999997e-07f) m = 255;
            else {
                float v = (d - lowF) * invRange;
                v *= 255.0f;
                v = 0.f > v ? 0.f : std::min(255.f, v);
                m = (int)v;
            }
            mrow[x] = (float)std::min(m, a);                                          // (the alpha byte also holds the picture's own alpha)
        }
    });
    // the blur
    const float amount = (float)clampd(c.d("BlurAmount"), 0, 1);
    const float size = std::max((float)W, (float)H) * amount * 0.25f + 1.5f;
    const float ln2 = std::log(0.003921568859368563f) + std::log(0.003921568859368563f);
    const float sigma = std::sqrt(-((size * size) / ln2));
    if (sigma > 0.6f) {
        const KeyerCoefs k = keyerCoefs(sigma);
        std::vector<float> plane((size_t)W * H);
        forRows(H, W * 40, [&](int y) {
            std::vector<float> yc, yb;
            keyerLine(k, W, &mask[(size_t)y * W], &plane[(size_t)y * W], yc, yb);
        });
        // columns, a few at a time through a scratch line
        std::vector<float> col((size_t)H), res((size_t)H), yc, yb;
        for (int x = 0; x < W; ++x) {
            for (int y = 0; y < H; ++y) col[(size_t)y] = plane[(size_t)(H - 1 - y) * W + x];     // (VEGAS's rows run bottom first)
            keyerLine(k, H, col.data(), res.data(), yc, yb);
            for (int y = 0; y < H; ++y) mask[(size_t)(H - 1 - y) * W + x] = (float)(int)res[(size_t)y];
        }
    }
    const bool maskOnly = c.b("ShowMaskOnly");
    const int despill = chromaKeyDespill(key);
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        const float* mrow = &mask[(size_t)y * W];
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            const int a8 = (int)mrow[x];
            int p[3] = {v8::toLevel(s[0]), v8::toLevel(s[1]), v8::toLevel(s[2])};     // R, G, B bytes as the source holds them (premultiplied)
            if (maskOnly) { const float v = v8::fromLevel(a8); o[0] = o[1] = o[2] = o[3] = v; continue; }
            if (a8 >= 1) { if (despill == 1) p[2] = std::min(p[2], p[1]); else if (despill == 2) p[1] = std::min(p[1], p[0]); }
            const double f = (double)a8 * 0.00392156862745098;
            for (int i = 0; i < 3; ++i) o[i] = v8::fromLevel((int)((double)p[i] * f));
            o[3] = v8::fromLevel(a8);
        }
    });
}

// Border: VEGAS's CPU processor (rows bottom first, 8-bit BGRA; colour channels in its parameter order, so red lands in the blue byte). The frame is bx = Size * max(W, H) / 2 pixels
// wide at the sides (at most half the width), by the same at top and bottom.
//  Solid: pixels with x < (int)bx, x > (int)(W - bx), y < (int)by or y > (int)(H - by) get the colour premultiplied by its alpha (truncated).
//  Blurred: a 255 mask in the frame goes through three box blurs approximating a Gaussian of sigma (int)(1.5 bx / sqrt(2 ln 255)); each pixel is multiplied by 1 - mask / 255.
//  Beveled: four pyramid faces lit from Angle; face factor (256 = 1) = (int)(colour * (n . l) * 256 * 1.8 + 0.5), n the face normal (0, -+1, 1) / sqrt 2 or (-+1, 0, 1) / sqrt 2,
//   l = (sin a, -cos a, 1.5) normalised; diagonals are spans with 1/128 coverage; a face pixel is source * factor / 256, not above the source alpha, and its alpha byte stays 0
//   (VEGAS writes only the colour bytes; see Def::colourAboveAlpha).
namespace {
// Kutskir's box blurs (rounded running sums) on a byte plane
void borderBoxH(const std::vector<int>& scl, std::vector<int>& tcl, int w, int h, int r)
{
    const float iarr = 1.f / (float)(r + r + 1);
    auto rnd = [&](int v) { return (int)std::floor((float)v * iarr + 0.5f); };
    for (int i = 0; i < h; ++i) {
        const int* row = &scl[(size_t)i * w];
        int* out = &tcl[(size_t)i * w];
        const int fv = row[0], lv = row[w - 1];
        int val = (r + 1) * fv;
        for (int j = 0; j < r; ++j) val += row[std::min(j, w - 1)];
        int li = 0, ri = r, ti = 0;
        for (int j = 0; j <= r && ti < w; ++j) { val += row[std::min(ri, w - 1)] - fv; ++ri; out[ti++] = rnd(val); }
        for (int j = r + 1; j < w - r; ++j) { val += row[ri++] - row[li++]; out[ti++] = rnd(val); }
        for (int j = std::max(w - r, r + 1); j < w; ++j) { val += lv - row[li++]; out[ti++] = rnd(val); }
    }
}
void borderBoxT(const std::vector<int>& scl, std::vector<int>& tcl, int w, int h, int r)
{
    std::vector<int> a((size_t)w * h), b((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) a[(size_t)x * h + y] = scl[(size_t)y * w + x];
    borderBoxH(a, b, h, w, r);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) tcl[(size_t)y * w + x] = b[(size_t)x * h + y];
}
void borderBoxBlur(std::vector<int>& scl, std::vector<int>& tcl, int w, int h, int r)
{
    tcl = scl;
    borderBoxH(tcl, scl, w, h, r);
    borderBoxT(scl, tcl, w, h, r);
}
}  // namespace

void border(Ctx& c)
{
    BorderGeo g;
    borderGeometry(c, c.out.w, c.out.h, g);
    const int W = g.W, H = g.H, type = g.type;
    const float Hf = g.Hf, bx = g.bx;
    c.out = c.src;
    if (W <= 0 || H <= 0) return;
    const bool q8 = c.levels;
    // the source as bytes (or floats in 0..255), rows bottom first; channel k of the colour goes to byte k (BGRA: blue first)
    auto px = [&](int x, int y) { return c.src.at(x, H - 1 - y); };
    auto outp = [&](int x, int y) { return c.out.at(x, H - 1 - y); };
    auto v8 = [&](float v) { return q8 ? (float)roundNonNeg(std::min(std::max(v, 0.f), 1.f) * 255.f) : v * 255.f; };
    auto chOfByte = [](int k) { return k == 3 ? 3 : 2 - k; };
    if (type == 2) {
        const int xi = g.xi, xr = g.xr, yb = g.yb, yt = g.yt;
        const float* cc = g.cc;
        forRows(H, W, [&](int yt0) {
            const int y = H - 1 - yt0;
            for (int x = 0; x < W; ++x) {
                if (!(x < xi || x > xr || y < yb || y > yt)) continue;
                float* o = c.out.at(x, yt0);
                for (int k = 0; k < 4; ++k) o[chOfByte(k)] = cc[k] / 255.f;
            }
        });
        return;
    }
    if (type == 0) {
        const int xi = g.xi, xr = g.xr, yb = g.yb, yt = g.yt;
        std::vector<int> mask((size_t)W * H, 0), tmp, out((size_t)W * H, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (x < xi || x > xr || y < yb || y > yt) mask[(size_t)y * W + x] = 255;
        const int* rads = g.rads;
        // (radii past the frame are kept in by the edge reads: VEGAS's frames never need that at its sizes)
        tmp = mask;
        borderBoxBlur(tmp, out, W, H, rads[0]);
        borderBoxBlur(out, tmp, W, H, rads[1]);
        borderBoxBlur(tmp, out, W, H, rads[2]);
        forRows(H, W, [&](int yt0) {
            const int y = H - 1 - yt0;
            for (int x = 0; x < W; ++x) {
                const float k = 1.f - (float)out[(size_t)y * W + x] / 255.f;
                const float* s = c.src.at(x, yt0);
                float* o = c.out.at(x, yt0);
                for (int ch = 0; ch < 4; ++ch) o[ch] = q8 ? (float)(int)(v8(s[ch]) * k) / 255.f : s[ch] * k;
            }
        });
        return;
    }
    c.out.px.assign(c.out.px.size(), 0.f);
    for (int y = 0; (float)y < Hf && y < H; ++y) {
        BorderSpans xs;
        borderBevelRow(g, y, xs);
        int x = 0;
        for (int k = 0; k < 5; ++k)
            for (; x < xs.end[k] && x < W; ++x) {
                const float* s = px(x, y);
                float* o = outp(x, y);
                if (xs.cov[k] == 0) { std::memcpy(o, s, sizeof(float) * 4); continue; }
                const float al = v8(s[3]);
                for (int kb = 0; kb < 3; ++kb) {
                    const int ch = chOfByte(kb);
                    float v;
                    if (q8) { const int iv = (int)v8(s[ch]) * xs.face[k][kb]; v = (float)(iv / 256); }
                    else v = v8(s[ch]) * (float)xs.face[k][kb] / 256.f;
                    o[ch] = std::min(v, al) / 255.f;
                }
                o[3] = 0.f;
            }
    }
    (void)bx;
}

// Glow as VEGAS (SfDibGlow and its processor, Vfx1): the picture is blurred by VEGAS's Gaussian (builtins_deriche.cpp, 8-bit input, float output) of size
// larger side x Glow percent x 0.5; the mean of the blurred R, G, B, less Suppression and times Intensity, clamped to 0..1, is the weight w of Color: in 8-bit
// w = (int)(255 x that) and every byte (alpha too) becomes d + ((color - d) * w, cut towards 0 after / 256), color = (int)(255 x Color).
void glow(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double pct = clampd(c.d("GlowPercent"), 0, 1);
    const float intensity = (float)c.d("Intensity"), supp = (float)clampd(c.d("Suppression"), 0, 1);
    const Color col = c.color("Color");
    const double size = (double)std::max(W, H) * pct * 0.5;
    Buf bl = c.src;
    vegasDericheBlurSigma(bl, vegasBlurSigma(size), vegasBlurSigma(size), c.levels, c.levels);   // (8-bit: floats of 0..255)
    const double cc[4] = {col.r, col.g, col.b, col.a};
    int cb[4];
    for (int k = 0; k < 4; ++k) cb[k] = (int)(cc[k] * 255.0);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s = c.src.at(x, y);
            const float* m = bl.at(x, y);
            float* o = c.out.at(x, y);
            if (c.levels) {
                float f = m[0] + m[1] + m[2];
                f = f / 3.f;
                f = f / 255.f;
                f = (f - supp) * intensity;
                f = f < 0.f ? 0.f : std::min(1.f, f);
                f = f * 255.f;
                f = f < 0.f ? 0.f : std::min(255.f, f);
                const int w = (int)f;
                for (int k = 0; k < 4; ++k) {
                    const int d = v8::toLevel(s[k]);
                    int t = (cb[k] - d) * w;
                    t += t < 0 ? 255 : 0;
                    o[k] = v8::fromLevel((d + (t >> 8)) & 255);
                }
            } else {
                float f = m[0] + m[1] + m[2];
                f = f / 3.f;
                f = (f - supp) * intensity;
                const float w = f < 0.f ? 0.f : std::min(1.f, f);
                for (int k = 0; k < 4; ++k) o[k] = ((float)cc[k] - s[k]) * w + s[k];
            }
        }
    });
}

// Add noise: grain added to each pixel (the same in all channels if Monochromatic), new every frame if Animate.
// VEGAS's grain comes from an R250 shift-register generator (w[i] ^= w[i + 103]) filled from the Microsoft C rand()
// (x = x * 214013 + 2531011, bits 16..30) after srand(seed), with 32 bits fixed up for full rank.
struct NoiseR250 {
    uint32_t w[250];
    unsigned char next[250];
    int i, j;
    uint32_t crt;
    int crtRand() { crt = crt * 214013u + 2531011u; return (int)((crt >> 16) & 0x7fff); }
    explicit NoiseR250(uint32_t seed)
    {
        crt = seed;
        i = 0; j = 103;
        for (int k = 249; k >= 0; --k) {
            next[k] = (unsigned char)(k < 249 ? k + 1 : 0);
            const uint32_t r1 = (uint32_t)crtRand(), r2 = (uint32_t)crtRand(), r3 = (uint32_t)crtRand();
            w[k] = r1 * 4u + (r2 << 17) + r3;
        }
        uint32_t mask = 0xffffffffu, msb = 0x80000000u;
        for (int k = 0; k < 32; ++k) {
            const int at = 3 + 7 * k;
            w[at] = (w[at] & mask) | msb;
            mask >>= 1; msb >>= 1;
        }
    }
    uint32_t step()
    {
        w[i] ^= w[j];
        const uint32_t v = w[i];
        i = next[i]; j = next[j];
        return v;
    }
    double gauss()
    {
        double u, v, s;
        do {
            u = (double)(step() & 0xffffu) / 65535.0 * 2.0 - 1.0;
            v = (double)(step() & 0xffffu) / 65535.0 * 2.0 - 1.0;
            s = v * v + u * u;
        } while (s >= 1.0 || s == 0.0);
        return std::sqrt(-2.0 * std::log(s) / s) * v;
    }
};

void addNoise(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double level = clampd(c.d("NoiseLevel"), 0, 1) * c.scale;
    const bool mono = c.b("Monochromatic"), gauss = c.b("GaussianNoise"), animate = c.b("Animate");
    c.out.px = c.src.px;
    // VEGAS draws from the C library's global rand() state (one per thread, first value of a fresh process: srand(1)), in this order on
    // every picture: x0 = rand(); srand(x0); 750 rand() (the generator's first fill); then, if the level is above 1e-6, either
    // x1 = rand() (Animate: seed = x1 * 0x3023) or the fixed 1234567, and srand(seed) with 750 more rand() for the generator that is used.
    // So the grain of a picture depends on the pictures drawn before it, like in VEGAS (checked against its reference for 24 pictures in a
    // row: the harness's sequence of tests gives exactly VEGAS's seeds). One state for all threads here; draws are serialised.
    static std::mutex mu;
    static uint32_t crtState = 1;
    uint32_t seed = 0x12d687u;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto rnd = [&]() { crtState = crtState * 214013u + 2531011u; return (crtState >> 16) & 0x7fffu; };
        const uint32_t x0 = rnd();
        crtState = x0;
        for (int k = 0; k < 750; ++k) rnd();
        if (!((float)clampd(c.d("NoiseLevel"), 0, 1) > 1e-6f)) return;
        if (animate) seed = rnd() * 0x3023u;
        crtState = seed;
        for (int k = 0; k < 750; ++k) rnd();
    }
    if (!((float)level > 1e-6f)) return;
    NoiseR250 gen(seed);
    const int K = (int)(level * 16384.0);
    auto sample = [&]() {
        if (gauss) return (int)((gen.gauss() - 0.5) * level * 255.0);
        int x = ((int)(gen.step() & 0xffu) - 0x7f) * K;
        if (x < 0) x += 0x3fff;
        return x >> 14;
    };
    // the draws follow the picture's memory order: bottom row first (so the order does not depend on how Reno stores it),
    // and the bytes of the premultiplied pixel: blue, green, red (the draws go red, green, blue)
    for (int y = H - 1; y >= 0; --y)
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            const int ab = (int)std::floor(clamp01(o[3]) * 255.f + 0.5f);
            int d[3];
            d[0] = sample();
            d[1] = mono ? d[0] : sample();
            d[2] = mono ? d[0] : sample();
            for (int k = 0; k < 3; ++k) {
                int v = (int)std::floor(clamp01(o[k]) * 255.f + 0.5f) + d[k];
                v = v < 0 ? 0 : (v > ab ? ab : v);
                o[k] = (float)v / 255.f;
            }
        }
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

}  // namespace

void addFilters(std::vector<Def>& out)
{
    {
        // groups as in VEGAS (Automatic Contrast holds Input and Output); settings from its resource file; Min / Max / Percent ranges are guessed
        Def d = filter("brightnessandcontrast", "Brightness and Contrast", "Brighter or darker, more or less contrast",
                       {dbl("Brightness", "Brightness", 0, -1, 1), dbl("Contrast", "Contrast", 0, -1, 1), dbl("ContrastCenter", "Contrast center", 0.5, 0, 1),
                        group("Group", "Automatic Contrast"),
                        choice("ReferenceMode", "Analysis Mode", 0, {"Current Frame", "Current Clip", "Set for each Frame independently"}, "Group"),
                        group("GroupInput", "Input"), dbl("Percent", "Histogram Cut-Off", 1, 0, 25, "GroupInput"),
                        group("GroupOutput", "Output"), dbl("Min", "Minimum Brightness", 0, 0, 1, "GroupOutput"), dbl("Max", "Maximum Brightness", 1, 0, 1, "GroupOutput"),
                        button("DoAnalysis", "Calculate Brightness & Contrast", "Group")},
                       brightnessContrast);
        for (auto& p : d.params) {
            if (p.name == "GroupInput" || p.name == "GroupOutput") p.parent = "Group";
            if (p.name == "Brightness") p.hint = "Brighntess manipulation.";
            if (p.name == "Contrast") p.hint = "Contrast manipulation.";
            if (p.name == "ContrastCenter") p.hint = "Center of Contrast manipulation.";
            if (p.name == "ReferenceMode") p.hint = "The given brightness level refers to ....";
            if (p.name == "Min") p.hint = "Resulting lowest brightness.";
            if (p.name == "Max") p.hint = "Resulting highest brightness.";
            if (p.name == "Percent") p.hint = "Percentage of input histogram to cut-off.";
            if (p.name == "DoAnalysis") p.hint = "Offline calculation.";
        }
        d.changed = brightnessContrastChanged;
        d.rows = brightnessContrastRows;
        out.push_back(d);
    }
    {
        Def d = filter("invert", "Invert", "The colours turned to their opposites", {dbl("BlendAmount", "Blend amount", 1, 0, 1)}, invert);
        d.rows = invertRows;
        out.push_back(d);
        d = filter("blackandwhite", "Black and White", "The picture in greys", {dbl("BlendAmount", "Blend amount", 1, 0, 1)}, blackAndWhite);
        d.rows = blackAndWhiteRows;
        out.push_back(d);
        d = filter("sepia", "Sepia", "Greys tinted like an old photograph",
                   {rgb("Color", "Color", 166.0 / 255.0, 166.0 / 255.0, 0.0), dbl("BlendingStrength", "Blending strength", 0, 0, 1),
                    dbl("BlendingFalloff", "Blending falloff", 0, 0, 1)},
                   sepia);
        d.rows = sepiaRows;
        out.push_back(d);
    }
    out.push_back(filter("hsladjust", "HSL Adjust", "Hue turned, saturation and lightness scaled",
                         {dbl("AddToHue", "Add to hue", 0, 0, 1), dbl("Saturation", "Saturation", 1, 0, 5), dbl("Luminance", "Luminance", 1, 0, 5)}, hslAdjust));
    {
        Def d = filter("levels", "Levels", "Input and output levels and gamma",
                       {choice("Channel", "Channel", 0, {"All", "Red", "Green", "Blue", "Alpha"}), dbl("InputStart", "Input start", 0, -1, 2),
                        dbl("InputEnd", "Input end", 1, -1, 2), dbl("OutputStart", "Output start", 0, -1, 2), dbl("OutputEnd", "Output end", 1, -1, 2),
                        dbl("Gamma", "Gamma", 1, 0.01, 10)},
                       levels);
        for (auto& p : d.params) if (p.type == PT::Double && p.name != "Gamma") { p.dmin = 0; p.dmax = 1; }
        d.params.back().dmax = 4;
        out.push_back(d);
    }
    out.push_back(filter("colorbalance", "Color Balance", "Red, green and blue pushed in the highlights, midtones or shadows",
                         {dbl("Red", "Red", 0, -1, 1), dbl("Green", "Green", 0, -1, 1), dbl("Blue", "Blue", 0, -1, 1),
                          choice("Type", "Type", 1, {"Highlights", "Midtones", "Shadow"}), boolean("PreserveLuminosity", "Preserve luminosity", true)},
                         colorBalance));
    out.push_back(filter("threshold", "Threshold", "Black or white by brightness", {dbl("Threshold", "Threshold", 0.5, 0, 1)}, threshold));
    out.push_back(filter("mirror", "Mirror", "One side reflected over the other",
                         {dbl("Angle", "Angle", 0, -360, 360), point("Center", "Center", 0.5, 0.5), rgba("Background", "Background", 0, 0, 0, 0)}, mirror));
    out.push_back(filter("gaussianblur", "Gaussian Blur", "A soft blur",
                         {dbl("HorizontalRange", "Horizontal range", 0.063, 0, 1), dbl("VerticalRange", "Vertical range", 0.063, 0, 1),
                          group("Channels", "Channels"), boolean("R", "Red", true, "Channels"), boolean("G", "Green", true, "Channels"),
                          boolean("B", "Blue", true, "Channels"), boolean("A", "Alpha", true, "Channels")},
                         gaussianBlurFx));
    out.push_back(filter("sharpen", "Sharpen", "Edges made crisper", {dbl("Amount", "Amount", 0.5, 0, 1)}, sharpen));
    out.push_back(filter("pixelate", "Pixelate", "The picture in blocks",
                         {dbl("HorizontalPixelization", "Horizontal pixelization", 0, 0, 1), dbl("VerticalPixelization", "Vertical pixelization", 0, 0, 1)},
                         pixelate));
    out.push_back(filter("linearblur", "Linear Blur", "A blur along one direction",
                         {dbl("Angle", "Angle", 0, -360, 360), dbl("Amount", "Amount", 0, 0, 1)}, linearBlur));
    out.push_back(filter("chromakeyer", "Chroma Keyer", "A colour (a blue or green screen) made transparent",
                         {rgb("color", "Color", 24.0 / 255.0, 71.0 / 255.0, 241.0 / 255.0), dbl("LowThreshold", "Low threshold", 0, 0, 1),
                          dbl("HighThreshold", "High threshold", 0.4, 0, 1), dbl("BlurAmount", "Blur amount", 0, 0, 1),
                          boolean("ShowMaskOnly", "Show mask only", false)},
                         chromaKeyer));
    {
        // VEGAS has the colour twice: "color" (with alpha) and "color_rgb" (without). Reno draws with "color" and keeps both the same.
        Def d = filter("border", "Border", "A soft edge, a bevel or a frame of colour",
                       {choice("Type", "Type", 0, {"Blurred", "Beveled", "Solid"}), dbl("Size", "Size", 0, 0, 1), dbl("Angle", "Angle", 0, 0, 360),
                        rgba("color", "Color", 1, 1, 1, 1), rgb("color_rgb", "Color", 1, 1, 1)},
                       border);
        d.changed = borderChanged;
        d.colourAboveAlpha = true;
        out.push_back(d);
    }
    out.push_back(filter("glow", "Glow", "Bright parts glowing",
                         {dbl("GlowPercent", "Glow percent", 0, 0, 1), dbl("Intensity", "Intensity", 0, 0, 20), dbl("Suppression", "Suppression", 0, 0, 1),
                          rgb("Color", "Color", 0, 0, 0)},
                         glow));
    out.push_back(filter("addnoise", "Add Noise", "Grain over the picture",
                         {dbl("NoiseLevel", "Noise level", 0.5, 0, 1), boolean("Monochromatic", "Monochromatic", true),
                          boolean("GaussianNoise", "Gaussian noise", false), boolean("Animate", "Animate", true)},
                         addNoise));
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
