// Built-in stand-ins for the VEGAS Pro 18 stock video effects, with VEGAS's identifiers and parameter names, for when
// VEGAS's plug-ins are not installed. The output approximates VEGAS's. See builtins.h.
#include <mutex>
#include "builtins_impl.h"
#include "builtins_fast.h"
#include "builtins_vec.h"
#include "builtins_deriche.h"
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

// luma() of four pixels
inline V4 lumaV(V4 r, V4 g, V4 b)
{
    return v4::add(v4::add(v4::mul(v4::set(0.2126f), r), v4::mul(v4::set(0.7152f), g)), v4::mul(v4::set(0.0722f), b));
}

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
bool sepiaRows(Ctx& c, RowFn& fn)
{
    const Color col = c.color("Color");
    const double s = c.d("BlendingStrength"), falloff = c.d("BlendingFalloff");
    const int colour[3] = {(int)(col.r * 255.0), (int)(col.g * 255.0), (int)(col.b * 255.0)};
    int toGrey = 0;
    int lut[256];
    bool useLut = false;
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
    const bool greyMix = s <= 2.0 / 3.0;
    fn = [=](const float* src, float* o, int n, int) {
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

void hslAdjust(Ctx& c)
{
    const float addHue = (float)c.d("AddToHue"), sat = (float)std::max(0.0, c.d("Saturation")), lum = (float)std::max(0.0, c.d("Luminance"));
    perPixel(c, [&](float& r, float& g, float& b) {
        float h, s, l;
        rgbToHsl(clamp01(r), clamp01(g), clamp01(b), h, s, l);
        hslToRgb(h + addHue, clamp01(s * sat), clamp01(l * lum), r, g, b);
    });
}

}  // namespace

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
    auto f = [&](float v) {
        const double x = v;
        const double xin = i0 > x ? i0 : std::min(i1, x);
        const double t = xin - i0;
        double r;
        if (straight) r = t * (o1 - o0) / width;
        else r = (o1 - o0) * levelsCurve(t / width, gamma);
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
void colorBalance(Ctx& c)
{
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

// Mirror: the side of the line through Center that the angle points to (0 top, 90 left, 180 bottom, -90 right) is kept and reflected.
// Measured on VEGAS: a pixel exactly on the line (by index, not centre) is reflected too; the bilinear reflection blends into Background
// at the frame edge; for slanted lines the samples shift along the kept direction (0.4 px * kx * |ky|, 0.2 px per axis at 45 degrees).
void mirror(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double cx, cy;
    c.xy("Center", cx, cy);
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

void gaussianBlurFx(Ctx& c)
{
    c.out = c.src;
    const bool ch[4] = {c.b("R"), c.b("G"), c.b("B"), c.b("A")};
    gaussianBlur(c.out, gaussianBlurSigma(c.d("HorizontalRange"), c.out.w), gaussianBlurSigma(c.d("VerticalRange"), c.out.w), ch);
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
                    for (int k = 0; k < 4; ++k) s[k] += q[k];
                }
            }
            const double n = (double)(wx1 - wx0) * (wy1 - wy0);
            float avg[4];
            for (int k = 0; k < 4; ++k) {
                avg[k] = (float)(s[k] / n);
                if (c.levels) avg[k] = floor8(avg[k]);
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
// Chroma keyer, measured against VEGAS (exact on its pictures): d = sqrt(3)/2 * |RGB - key RGB| + (|hue diff| / 180 deg + |sat diff| + |lightness diff|) / 6 (HSL in 0..1, grey hue 0).
// Alpha is d (8 bits, truncated), the ramp starting at the low threshold; a high threshold at or below the low one is a hard cut. The blur is a Gaussian of the 8-bit mask (taps to 4 sigma,
// sigma 0.437 + 24.07 * amount pixels at height 180), truncated slightly under so 255 ends up 254. A blue key limits blue to green, a green key green to red (spill removal).
double chromaKeyDistance(double r, double g, double b, const double* key)
{
    struct Hsl { double h, s, l; };
    auto hsl = [](double r, double g, double b) {
        const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
        Hsl o;
        o.l = (mx + mn) * 0.5;
        if (d <= 0) { o.h = 0; o.s = 0; return o; }
        if (mx == r) { o.h = (g - b) / d; if (o.h < 0) o.h += 6; }
        else if (mx == g) o.h = (b - r) / d + 2;
        else o.h = (r - g) / d + 4;
        o.h *= 60;
        o.s = o.l <= 0.5 ? d / (mx + mn) : d / (2 - mx - mn);
        return o;
    };
    const Hsl p = hsl(r, g, b), k = hsl(key[0], key[1], key[2]);
    double dh = std::fabs(p.h - k.h);
    dh = std::min(dh, 360 - dh);
    const double dr = r - key[0], dg = g - key[1], db = b - key[2];
    return 0.8660254037844386 * std::sqrt(dr * dr + dg * dg + db * db) + (dh / 180 + std::fabs(p.s - k.s) + std::fabs(p.l - k.l)) / 6;
}

// The 8-bit alpha (0..255) of a distance under the thresholds.
float chromaKeyAlpha8(double d, double low, double high)
{
    double a;
    if (high <= low) a = d >= low ? 1.0 : 0.0;
    else a = clampd((d - low) / (1.0 - low), 0.0, 1.0);
    return (float)std::min(255.0, std::floor(a * 255.0 + 1e-6));
}

// 1: the key is a blue screen, 2: a green one, 0: neither
int chromaKeyDespill(const double* key) { return (key[2] > key[1] && key[2] > key[0]) ? 1 : ((key[1] > key[0] && key[1] >= key[2]) ? 2 : 0); }

// The keyer's blur sigma in pixels of this frame (0: none), and its one-sided Gaussian (taps to 4 sigma, normalised).
double chromaKeyBlurSigma(double amount, int h) { return amount > 0 ? (0.437 + 24.07 * amount) * h / 180.0 : 0.0; }
std::vector<float> chromaKeyBlurWeights(double sigma)
{
    const int r = (int)std::min(4000.0, std::ceil(sigma * 4));
    std::vector<float> k((size_t)r + 1);
    double sum = 0;
    for (int i = 0; i <= r; ++i) { k[(size_t)i] = (float)std::exp(-0.5 * i * i / (sigma * sigma)); sum += (i ? 2 : 1) * k[(size_t)i]; }
    for (auto& v : k) v = (float)(v / sum);
    return k;
}

void chromaKeyer(Ctx& c)
{
    const Color kc = c.color("color");
    const double key[3] = {kc.r, kc.g, kc.b};
    const double low = clampd(c.d("LowThreshold"), 0, 1), high = clampd(c.d("HighThreshold"), 0, 1);
    const int W = c.out.w, H = c.out.h;
    std::vector<float> mask((size_t)W * H);
    forRows(H, W, [&](int y) {
        const float* in = c.src.at(0, y);
        float* mrow = &mask[(size_t)y * W];
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(in + (size_t)x * 4, r, g, b, a);
            mrow[x] = chromaKeyAlpha8(chromaKeyDistance(clamp01(r), clamp01(g), clamp01(b), key), low, high);
        }
    });
    const double sigma = chromaKeyBlurSigma(c.d("BlurAmount"), H);
    if (sigma > 0) {
        const std::vector<float> k = chromaKeyBlurWeights(sigma);
        const int r = (int)k.size() - 1;
        std::vector<float> tmp((size_t)W * H);
        forRows(H, W * (r + 1), [&](int y) {
            const float* in = &mask[(size_t)y * W];
            float* o = &tmp[(size_t)y * W];
            for (int x = 0; x < W; ++x) {
                double s = in[x] * (double)k[0];
                for (int j = 1; j <= r; ++j) s += (in[std::max(0, x - j)] + in[std::min(W - 1, x + j)]) * (double)k[(size_t)j];
                o[x] = (float)s;
            }
        });
        forRows(H, W * (r + 1), [&](int y) {
            float* o = &mask[(size_t)y * W];
            for (int x = 0; x < W; ++x) {
                double s = tmp[(size_t)y * W + x] * (double)k[0];
                for (int j = 1; j <= r; ++j)
                    s += (tmp[(size_t)std::max(0, y - j) * W + x] + tmp[(size_t)std::min(H - 1, y + j) * W + x]) * (double)k[(size_t)j];
                o[x] = (float)std::floor(s - 0.005);          // (truncated, a little under: VEGAS's 255 blurs to 254)
            }
        });
    }
    const bool maskOnly = c.b("ShowMaskOnly");
    const int despill = chromaKeyDespill(key);
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        const float* mrow = &mask[(size_t)y * W];
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            const float m8 = std::min(255.f, std::max(0.f, mrow[x]));
            float r, g, b, a;
            unpremul(s, r, g, b, a);
            if (maskOnly) { const float v = m8 / 255.f * a; o[0] = o[1] = o[2] = o[3] = v; continue; }
            if (despill == 1) b = std::min(b, g); else if (despill == 2) g = std::min(g, r);
            // the straight colour times the alpha, truncated to 8 bits as VEGAS does
            auto q = [&](float v) { return (float)(std::floor((double)v * m8 + 1e-3) / 255.0) * a; };
            o[0] = q(r); o[1] = q(g); o[2] = q(b); o[3] = m8 / 255.f * a;
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
struct BorderSpans { int end[5]; int cov[5]; int face[5][4]; bool hasFace[5]; };
void borderSpans(BorderSpans& s, float d, float extent, const int* fa, const int* fb)
{
    s.end[0] = (int)std::floor(d); s.cov[0] = 128; std::memcpy(s.face[0], fa, sizeof(int) * 4); s.hasFace[0] = true;
    s.end[1] = (int)std::ceil(d); s.cov[1] = (int)((d - (float)s.end[0]) * 128.f); std::memcpy(s.face[1], fa, sizeof(int) * 4); s.hasFace[1] = true;
    const float e = extent - d;
    s.end[2] = (int)std::floor(e); s.cov[2] = 0; s.hasFace[2] = false;
    std::memset(s.face[2], 0, sizeof(int) * 4);
    s.end[3] = (int)std::ceil(e); s.cov[3] = (int)(((float)s.end[3] - e) * 128.f); std::memcpy(s.face[3], fb, sizeof(int) * 4); s.hasFace[3] = true;
    s.end[4] = (int)extent; s.cov[4] = 128; std::memcpy(s.face[4], fb, sizeof(int) * 4); s.hasFace[4] = true;
}
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
    const int W = c.out.w, H = c.out.h;
    const int type = c.i("Type");
    const Color col = c.color("color");
    c.out = c.src;
    if (W <= 0 || H <= 0) return;
    const float Wf = (float)W, Hf = (float)H;
    const float sz = (float)c.d("Size") * std::max(Wf, Hf);
    const float bx3 = (float)((double)sz * 0.5), by3 = sz * 0.5f;
    const float bx = bx3 < 0.f ? 0.f : std::min(Wf * 0.5f, bx3), by = by3 < 0.f ? 0.f : std::min(Hf * 0.5f, by3);
    const bool q8 = c.levels;
    // the source as bytes (or floats in 0..255), rows bottom first; channel k of the colour goes to byte k (BGRA: blue first)
    auto px = [&](int x, int y) { return c.src.at(x, H - 1 - y); };
    auto outp = [&](int x, int y) { return c.out.at(x, H - 1 - y); };
    auto v8 = [&](float v) { return q8 ? (float)(int)std::lround(std::min(std::max(v, 0.f), 1.f) * 255.f) : v * 255.f; };
    auto chOfByte = [](int k) { return k == 3 ? 3 : 2 - k; };
    const double colv[4] = {col.r, col.g, col.b, col.a};
    if (type == 2) {
        const int xi = (int)bx, xr = (int)(Wf - bx), yb = (int)by, yt = (int)(Hf - by);
        const int a8 = (int)(colv[3] * 255.0);
        const float af = (float)(a8 & 255) / 255.f;
        float cc[4];
        for (int k = 0; k < 3; ++k) {
            const int ck = (int)(colv[k] * 255.0) & 255;
            cc[k] = q8 ? (float)(int)((float)ck * af) : (float)(colv[k] * 255.0) * af;
        }
        cc[3] = (float)(a8 & 255);
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
        const int xi = (int)bx, xr = (int)(Wf - bx), yb = (int)by, yt = (int)(Hf - by);
        std::vector<int> mask((size_t)W * H, 0), tmp, out((size_t)W * H, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (x < xi || x > xr || y < yb || y > yt) mask[(size_t)y * W + x] = 255;
        const float r15 = 1.5f * bx;
        const int sigma = (int)std::sqrt(-(r15 * r15) / (2.f * std::log(1.f / 255.f)));
        const float sg = (float)sigma;
        int wl = (int)std::floor(std::sqrt(12.f * sg * sg / 3.f + 1.f));
        if (wl % 2 == 0) --wl;
        const float mI = (12.f * sg * sg - (float)(3 * wl * wl) - 4.f * 3.f * (float)wl - 3.f * 3.f) / (-4.f * (float)wl - 4.f);
        const int m = (int)std::round(mI);
        int rads[3];
        for (int i = 0; i < 3; ++i) rads[i] = ((i < m ? wl : wl + 2) - 1) / 2;
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
    int F0[4], F1[4], F2[4], F3[4];
    faceOf(nm * ly + n0 * lx + z, F0);        // bottom
    faceOf(n1 * ly + n0 * lx + z, F1);        // top
    faceOf(nm * lx + n0 * ly + z, F2);        // left
    faceOf(n1 * lx + n0 * ly + z, F3);        // right
    BorderSpans ys;
    borderSpans(ys, by, Hf, F0, F1);
    int seg = 0;
    for (int y = 0; (float)y < Hf && y < H; ++y) {
        while (seg < 4 && !(y < ys.end[seg])) ++seg;
        float d;
        if (y < ys.end[1]) d = (float)y * bx / by;
        else if (y < ys.end[3]) d = bx;
        else d = (Hf - (float)y - 1.f) * bx / by;
        BorderSpans xs;
        borderSpans(xs, d, Wf, F2, F3);
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
}

// Glow as VEGAS (read off its pictures): the mean of R, G, B is blurred by a Gaussian of sigma 50 * Glow percent + 0.3 pixels (at 320 wide);
// strength k is Intensity times that over Suppression (0..1), and the picture mixes towards Color by k. Intensity 0 leaves it alone.
void glow(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double pct = clampd(c.d("GlowPercent"), 0, 1), intensity = std::max(0.0, c.d("Intensity")), supp = clampd(c.d("Suppression"), 0, 1);
    const Color col = c.color("Color");
    if (c.levels && intensity >= 19.999 && supp < 0.999) {
        // at the very top of the Intensity range VEGAS overflows: the picture becomes the source reduced to one level (255 -> 1) where a channel is not 0
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                const float* s = c.src.at(x, y);
                float* o = c.out.at(x, y);
                for (int i = 0; i < 3; ++i) o[i] = s[i] > 0.f ? std::min(s[3], 1.f / 255.f) : 0.f;
                o[3] = s[3];
            }
        });
        return;
    }
    std::vector<float> m((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            const float l = (r + g + b) * (1.f / 3.f);
            m[(size_t)y * W + x] = l * a;
        }
    const double sigma = (pct * 50.0 + 0.3) * W / 320.0;
    const float thr = (float)supp;
    gaussianBlurPlane(m, W, H, sigma, sigma);
    const float gain = (float)intensity;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            const float k = clamp01((m[(size_t)y * W + x] - thr) * gain);
            const float ik = 1.f - k;
            o[0] = (float)col.r * k + s[0] * ik;
            o[1] = (float)col.g * k + s[1] * ik;
            o[2] = (float)col.b * k + s[2] * ik;
            o[3] = k + s[3] * ik;
        }
    });
}

// Add noise: grain added to each pixel (the same in all channels if Monochromatic), new every frame if Animate.
uint32_t hash3(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

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
    if (!((float)level > 1e-6f)) return;
    // VEGAS seeds with 1234567; animated, with rand() x 0x3023 from the C library's global sequence, which depends on how many pictures
    // were drawn before. Reno uses the frame number as the seed instead, from the sequence a fresh VEGAS draws (frame 0 = its first picture).
    uint32_t seed = 0x12d687u;
    if (animate) {
        static std::mutex mu;
        static std::vector<uint32_t> table;
        std::lock_guard<std::mutex> lock(mu);
        if (table.empty()) {
            uint32_t crt = 1;
            auto rnd = [&]() { crt = crt * 214013u + 2531011u; return (crt >> 16) & 0x7fffu; };
            for (int n = 0; n < 4096; ++n) {
                const uint32_t s0 = rnd();
                crt = s0;
                for (int k = 0; k < 750; ++k) rnd();
                const uint32_t r = rnd();
                table.push_back(r * 0x3023u);
                crt = r * 0x3023u;
                for (int k = 0; k < 750; ++k) rnd();
            }
        }
        int64_t f = (int64_t)std::floor(c.time);
        f = ((f % 4096) + 4096) % 4096;
        seed = table[(size_t)f];
    }
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
                   {rgb("Color", "Color", 0.650980, 0.650980, 0.0), dbl("BlendingStrength", "Blending strength", 0, 0, 1),
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
                         {dbl("HorizontalPixelization", "Horizontal pixelization", 0.12, 0, 1), dbl("VerticalPixelization", "Vertical pixelization", 0.12, 0, 1)},
                         pixelate));
    out.push_back(filter("linearblur", "Linear Blur", "A blur along one direction",
                         {dbl("Angle", "Angle", 0, -360, 360), dbl("Amount", "Amount", 0, 0, 1)}, linearBlur));
    out.push_back(filter("chromakeyer", "Chroma Keyer", "A colour (a blue or green screen) made transparent",
                         {rgb("color", "Color", 0.094118, 0.278431, 0.945098), dbl("LowThreshold", "Low threshold", 0, 0, 1),
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
                         {dbl("GlowPercent", "Glow percent", 0.22, 0, 1), dbl("Intensity", "Intensity", 0, 0, 20), dbl("Suppression", "Suppression", 0.3, 0, 1),
                          rgb("Color", "Color", 1, 1, 1)},
                         glow));
    out.push_back(filter("addnoise", "Add Noise", "Grain over the picture",
                         {dbl("NoiseLevel", "Noise level", 0.5, 0, 1), boolean("Monochromatic", "Monochromatic", true),
                          boolean("GaussianNoise", "Gaussian noise", false), boolean("Animate", "Animate", true)},
                         addNoise));
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
