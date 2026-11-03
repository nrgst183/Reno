// Built-in colour effects (Saturation, Soft Contrast, White Balance, Lab Adjust, Black Restore, Broadcast Colors, Channel Blend); see builtins_more.cpp.
#include "builtins_more_internal.h"
#include "vegas8.h"
#include "vfxhslwb.h"
#include "vfxcolour.h"
#include <cstdio>
#include <cstring>
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

// 8-bit pictures: VEGAS's own steps (Vfx1.ofx saturationadjustProcessor<unsigned char>), in doubles on the levels as bytes: Y = G 0.587 + R 0.299 + B 0.114, the distance
// d = sqrt((Y - G)^2 + (Y - R)^2 + (Y - B)^2) / 236.437 goes through a curve of five points (0: Low, Center - Spread/2: Low, Center: Amount, Center + Spread/2: High, 1: High; cubic Hermite
// with no slopes between neighbours, the value kept as a float) giving g; with f = 1 + g and u = 1 - f each channel is f C + u Y, summed in VEGAS's order, cut to 0..alpha and
// truncated.
static double satCurve(double x, double low, double high, double amount, double centre, double spread)
{
    const double h = spread * 0.5;
    double xs[5] = {0.0, std::max(0.0, centre - h), centre, centre + h, 1.0};
    if (!(1.0 > xs[3])) xs[3] = 1.0;
    const double vs[5] = {low, low, amount, high, high};
    int j = 1;
    for (; j < 4; ++j)
        if (xs[j] > x) break;
    const int lo = j - 1, hi = j;
    double t = 0.0;
    if (xs[hi] != xs[lo]) t = (x - xs[lo]) / (xs[hi] - xs[lo]);
    const double v0 = (double)(float)vs[lo], v1 = (double)(float)vs[hi];
    const double t2 = t * t, t3 = t2 * t;
    const double h00 = ((t3 + t3) - t2 * 3.0) + 1.0, h01 = t2 * 3.0 - (t3 + t3);
    return (double)(float)(v0 * h00 + v1 * h01);
}
static void saturationAdjust8(Ctx& c)
{
    const double amount = c.d("Amount"), low = c.d("Low"), high = c.d("High"), centre = c.d("Center"), spread = c.d("Spread");
    const double div = std::sqrt(55902.447675);
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            const int bi = v8::toLevel(s[2]), gi = v8::toLevel(s[1]), ri = v8::toLevel(s[0]), ai = v8::toLevel(s[3]);
            const double R = ri, G = gi, B = bi;
            const double Y = (G * 0.587 + R * 0.299) + B * 0.114;
            const double dG = Y - G, dR = Y - R, dB = Y - B;
            const double d = std::sqrt((dG * dG + dR * dR) + dB * dB);
            const double g = satCurve(d / div, low, high, amount, centre, spread);
            const double f = g + 1.0, u = 1.0 - f;
            const double wG = u * 0.587, wR = u * 0.299, wB = u * 0.114;
            double outR = ((wR + f) * R + G * wG) + B * wB;
            double outG = ((wG + f) * G + R * wR) + B * wB;
            double outB = (wB + f) * B + (G * wG + R * wR);
            const double al = ai;
            auto fin = [al](double v) { v = al > v ? v : al; v = v > 0.0 ? v : 0.0; return (int)v; };
            o[0] = v8::fromLevel(fin(outR));
            o[1] = v8::fromLevel(fin(outG));
            o[2] = v8::fromLevel(fin(outB));
            o[3] = v8::fromLevel(ai);
        }
    });
}


// Saturation Adjust, 8-bit, for the GPU: what saturationAdjust8() above works out once per frame, as the kernel's table of doubles (each a pair of 32-bit words, high
// first, held in floats): the three inner curve positions, then for each of the four curve pieces: start, width, a flag (the piece has a width), the value at its start and end;
// and the divisor of the distance.
static float bitsAsFloat(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
static void putDouble(std::vector<float>& t, double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    t.push_back(bitsAsFloat((uint32_t)(b >> 32)));
    t.push_back(bitsAsFloat((uint32_t)b));
}
void saturationAdjust8Gpu(Ctx& c, std::vector<float>& t)
{
    const double amount = c.d("Amount"), low = c.d("Low"), high = c.d("High"), centre = c.d("Center"), spread = c.d("Spread");
    const double h = spread * 0.5;
    double xs[5] = {0.0, std::max(0.0, centre - h), centre, centre + h, 1.0};
    if (!(1.0 > xs[3])) xs[3] = 1.0;
    const double vs[5] = {low, low, amount, high, high};
    t.clear();
    for (int i = 1; i <= 3; ++i) putDouble(t, xs[i]);
    for (int j = 1; j <= 4; ++j) {
        const int lo = j - 1, hi = j;
        putDouble(t, xs[lo]);
        putDouble(t, xs[hi] - xs[lo]);
        t.push_back(bitsAsFloat(xs[hi] != xs[lo] ? 1u : 0u));
        putDouble(t, (double)(float)vs[lo]);
        putDouble(t, (double)(float)vs[hi]);
    }
    putDouble(t, std::sqrt(55902.447675));
}

// VEGAS with GPU acceleration on draws Saturation Adjust, LAB Adjust and Broadcast Colors with its OpenCL kernels (vfxcolour.h, notes: Colour_Kernels.md): float32 on the premultiplied
// values one pixel at a time, then the kernels' 12-bit store into an 8-bit frame. fn(in, out) works on one pixel's four floats.
template <class F>
static void oclPixels(Ctx& c, F fn)
{
    const int W = c.out.w, H = c.out.h;
    const bool q = c.host8;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float o[4], in[4];
            const float* sp = c.src.at(x, y);
            if (q) { vcol::readLevels(sp, in); sp = in; }
            fn(sp, o);
            float* d = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) d[k] = q ? oclStore12(o[k]) : o[k];
        }
    });
}
static bool oclUsable(const Ctx& c) { return c.openCl && c.out.w > 0 && c.out.h > 0 && c.src.w == c.out.w && c.src.h == c.out.h; }

void saturationAdjust(Ctx& c)
{
    if (oclUsable(c)) {
        vcol::SatSettings st;
        st.amount = (float)c.d("Amount"); st.centre = (float)c.d("Center"); st.spread = (float)c.d("Spread"); st.low = (float)c.d("Low"); st.high = (float)c.d("High");
        oclPixels(c, [st](const float* p, float* o) { vcol::saturationPx(st, p, o); });
        return;
    }
    if (c.levels) { saturationAdjust8(c); return; }
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
// White Balance, rebuilt from VEGAS's code (the CPU path, on 8-bit bytes; the OpenCL path VEGAS used for the captures follows). 1. A 1024-entry table indexed by luma x 4 (rounded) gives the luma
// (x Brightness, from 0.25 steps, not held to 255) and the shift of U, V (BT.601, 0..255 scale) = cos / sin of Angle x Amount^2 x 127.5; each pixel goes to Y, U, V, is shifted and goes back (R from
// 1.1402509 V, G from 0.3947313 U + 0.5808092 V, B from 2.0325203 U), rounded.
// 2. With HasData ("Equalize Histogram") each channel is stretched by its own histogram: lo = level below which 0.5% of pixels lie, hi = highest level with at most 99.5% below;
// new = (old - lo x 255/256) x 256 / (hi - lo), held to 0..255, taken from the frame being drawn.
// VEGAS keeps White Balance's state in hidden settings: Angle and AmountOfCorrection (what it draws from), and, in the effect itself, the neutral colour and the temperature and tint it
// last worked from and a flag. Its changedParam (Vfx1.ofx 0x180588220) keeps them in step, the same on every change:
//  - SelectWhiteColor changed (compared with the colour it knows, by more than 1/255): the colour's temperature and tint are put into ColorTemperature and ColorTint, the correction that turns
//    the colour grey into Angle (when it differs by more than 0.1) and, if the flag is clear, into AmountOfCorrection (when it differs by more than 0.001);
//  - ColorTemperature or ColorTint changed: the values it knows are brought up to date (when the setting differs from them by more than 0.99), the colour of that temperature and tint is worked
//    out and, if it is not the colour it knows (1/255), put into SelectWhiteColor, with Angle and (flag clear) AmountOfCorrection from it;
//  - HasData changed: the flag is cleared.
// The flag is set when the effect is made; VEGAS clears it right away (it reports every setting changed once, HasData last). VEGAS puts a script's or a preset's settings in first and reports
// the changed ones afterwards in the order SelectWhiteColor, ColorTemperature, ColorTint, ...: so a later temperature or tint overwrites an Amount set in the same go, and a new colour puts
// the temperature and tint (and with them the correction) back to what it stands for. A made effect's temperature and tint are those of its default grey, 5998.98 K and tint 9.99
// (SelectWhiteColor is reported changed against nothing at the start); that is where the 'tint 10' of a temperature set alone comes from (captures 006-009, 013-016 and the (Default) preset 001 are
// exact, tools/wb_state_sim.py in the notes).
// Reno keeps the colour in WhiteColor (SelectWhiteColor is a button here) and the rest in Angle and WbState ("r g b kelvin tint flag").
static const double kWbDefaultKelvin = 6000.0;      // the describe default (VEGAS's preset (Default) puts it back)
static const double kWbByte = 0.003921568859368563;  // 1/255 as a float, the colour compare's limit

struct WbHidden {
    double col[3] = {0.5, 0.5, 0.5};
    double kelvin = 5998.9845462479225, tint = 9.991051797908726;
    double lum = 0.5;                   // VEGAS's +0xd8: the Y (0.3439664498 R + 0.7281660966 G - 0.0721325464 B) of the last neutral colour; a temperature's colour is made this bright
    int flag = 0;                       // VEGAS's byte at +0x120: 0 once HasData has been reported changed (Amount then follows the colour, temperature and tint)
};

static std::string wbFormat(const WbHidden& h)
{
    char buf[200];
    std::snprintf(buf, sizeof buf, "%.17g %.17g %.17g %.17g %.17g %d %.17g", h.col[0], h.col[1], h.col[2], h.kelvin, h.tint, h.flag, h.lum);
    return buf;
}
static bool wbParse(const std::string& t, WbHidden& h)
{
    WbHidden r;
    double v[5], lum = 0.0;
    int f = 0;
    const int got = std::sscanf(t.c_str(), "%lf %lf %lf %lf %lf %d %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &f, &lum);
    if (got < 6) return false;
    for (double x : v) if (!std::isfinite(x)) return false;
    for (int k = 0; k < 3; ++k) r.col[k] = v[k];
    // (a state saved before the luminance was kept: the colour's own)
    r.lum = (got >= 7 && std::isfinite(lum)) ? lum : 0.3439664498 * v[0] + 0.7281660966 * v[1] - 0.0721325464 * v[2];
    r.kelvin = v[3]; r.tint = v[4]; r.flag = f ? 1 : 0;
    h = r;
    return true;
}

// Angle and amount for drawing: the hidden Angle and AmountOfCorrection. A project saved before they were kept (its temperature or tint is not the one the state knows) gets the angle from them.
static void wbDrawSettings(Ctx& c, double& angle, double& amount)
{
    amount = clampd(c.d("AmountOfCorrection"), 0, 1);
    angle = c.d("Angle");
    WbHidden h;
    const bool known = wbParse(c.s("WbState"), h);
    const double k = c.d("ColorTemperature"), t = c.d("ColorTint");
    if (!known || std::fabs(k - h.kelvin) > 0.99 * 4 || std::fabs(t - h.tint) > 0.99 * 4) {
        const double kk = clampd(k >= 100.0 ? k : kWbDefaultKelvin, 1700.0, 15000.0);
        double tt = clampd(t, -100.0, 100.0);
        if (kk == kWbDefaultKelvin && tt == 0.0) angle = 90.0;
        else {
            if (tt == 0.0) tt = vhw::kWbNeutralTint;            // (the tint of VEGAS's default grey)
            double rgb[3], a, m;
            vhw::tempTintToRgb(kk, tt, rgb);
            vhw::rgbToAngleAmount(rgb, a, m);
            angle = a;
        }
    }
    if (!std::isfinite(angle)) angle = 90.0;
}
// same rounding as VEGAS (a float plus 1.5e-8 to the nearest whole number)
static inline int wbRound(float v) { return (int)std::floor((double)v + 1.5e-8 + 0.5); }
static inline float wbClampTo255(float v) { return v < 0.f ? 0.f : (v > 255.f ? 255.f : v); }

// The settings of White Balance as both the CPU and the Vulkan plan use them: the 1024 x (V shift, U shift, Y) table, whether the colours shift, whether the histogram stretches.
void whiteBalanceSettings(Ctx& c, std::vector<float>& tab, bool& shift, bool& hasData)
{
    const double amount = clampd(c.d("AmountOfCorrection"), 0, 1), bright = c.d("Brightness");
    hasData = c.b("HasData");
    shift = amount != 0.0 || bright != 1.0;
    tab.clear();
    if (!shift) return;
    tab.resize((size_t)1024 * 3);
    {
        double angDeg, amt;
        wbDrawSettings(c, angDeg, amt);
        const double ang = angDeg * 3.141592653589793 / 180.0;
        const double s2 = amount * amount;
        const float su = (float)(std::cos(ang) * s2 * 127.5), sv = (float)(std::sin(ang) * s2 * 127.5);
        for (int i = 0; i < 1024; ++i) {
            double y = ((double)i * 0.25 / 255.0) * 255.0 * bright;
            y = y < 0.0 ? 0.0 : y;      // not held to 255: with Brightness 2 the luma goes above it and only the finished R, G, B are clamped (captured picture)
            tab[(size_t)i * 3] = sv;
            tab[(size_t)i * 3 + 1] = su;
            tab[(size_t)i * 3 + 2] = (float)y;
        }
    }
}

// VEGAS with GPU acceleration on draws White Balance with its OpenCL kernels (vfxhslwb.h, prog47): the shift pass in float on the straight colour; with HasData its picture is an 8-bit
// one, whose truncated bytes are counted into 256-bin histograms per channel, and a second pass stretches that picture. The result goes into an 8-bit frame by the 12-bit store.
static void whiteBalanceOpenCl(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double amount = clampd(c.d("AmountOfCorrection"), 0, 1), bright = c.d("Brightness");
    const bool hasData = c.b("HasData");
    vhw::WbShift sh;
    double angDeg, amtDraw;
    wbDrawSettings(c, angDeg, amtDraw);
    sh.angle = (float)angDeg;
    sh.amount = (float)amount;
    sh.bright = (float)bright;
    const bool q8 = c.host8;
    vhw::WbStretch st;
    for (int k = 0; k < 3; ++k) { st.gain[k] = 1.f; st.off[k] = 0.f; }
    std::vector<float> mid;                         // the shift pass's picture (only kept when HasData needs a second pass)
    if (hasData) mid.resize((size_t)W * H * 4);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float o[4];
            vhw::wbPixel(sh, false, st, c.src.at(x, y), o);
            if (hasData) {
                float* m = &mid[((size_t)y * W + x) * 4];
                for (int k = 0; k < 4; ++k) m[k] = q8 ? more::oclStore12(o[k]) : o[k];       // (an 8-bit picture in an 8-bit frame)
            } else {
                float* d = c.out.at(x, y);
                for (int k = 0; k < 4; ++k) d[k] = q8 ? more::oclStore12(o[k]) : o[k];
            }
        }
    });
    if (!hasData) return;
    const bool shifted = amount != 0.0 || bright != 1.0;
    const size_t n = std::min((size_t)vhw::wbHistPixels(shifted), (size_t)W * H);
    std::vector<int> counts((size_t)3 * 256, 0);
    for (size_t i = 0; i < n; ++i) {                // the kernel's order: rows from the bottom
        const int j = (int)(i / (size_t)W), x = (int)(i % (size_t)W);
        const float* m = &mid[((size_t)(H - 1 - j) * W + x) * 4];
        for (int k = 0; k < 3; ++k) counts[(size_t)k * 256 + (size_t)vhw::wbPlaneByte(m[k])]++;
    }
    for (int k = 0; k < 3; ++k) vhw::wbStretchChannel(&counts[(size_t)k * 256], st.gain[k], st.off[k]);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float o[4];
            vhw::wbStretchPixel(st, &mid[((size_t)y * W + x) * 4], o);
            float* d = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) d[k] = q8 ? more::oclStore12(o[k]) : o[k];
            d[3] = d[2];                            // VEGAS's stretch pass writes the stretched blue into the alpha as well (every HasData capture has alpha == blue, colour bytes as they are)
        }
    });
}

void whiteBalance(Ctx& c)
{
    if (c.openCl && c.out.w > 0 && c.out.h > 0) { whiteBalanceOpenCl(c); return; }
    std::vector<float> tab;
    bool shift, hasData;
    whiteBalanceSettings(c, tab, shift, hasData);
    const int W = c.out.w, H = c.out.h;
    if (!shift && !hasData) { copyThrough(c); return; }
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

// The settings VEGAS's changedParam keeps in step (see above). 'h' is the effect's own memory of the colour, temperature and tint.
static void wbSetAngleAmount(const Change& ch, const WbHidden& h, double angle, double amount)
{
    if (std::fabs(ch.d("Angle") - angle) > 0.1) ch.setD("Angle", clampd(angle, 0.0, 360.0));
    if (h.flag == 0 && std::fabs(ch.d("AmountOfCorrection") - amount) > 0.001) ch.setD("AmountOfCorrection", clampd(amount, 0.0, 1.0));
}
static void wbTempTintChanged(Change& ch, WbHidden& h);
static void wbColourChanged(Change& ch, WbHidden& h)
{
    double c[3];
    {
        const Color col = ch.color("WhiteColor");
        c[0] = col.r; c[1] = col.g; c[2] = col.b;
    }
    for (int k = 0; k < 3; ++k) if (!std::isfinite(c[k])) c[k] = 0.0;
    bool differs = false;
    for (int k = 0; k < 3; ++k) if (std::fabs(c[k] - h.col[k]) > kWbByte) differs = true;
    if (!differs) return;
    for (int k = 0; k < 3; ++k) h.col[k] = c[k];
    h.lum = 0.3439664498 * c[0] + 0.7281660966 * c[1] - 0.0721325464 * c[2];
    double kel, tint;
    if (vhw::rgbToTempTint(c, kel, tint)) { h.kelvin = kel; h.tint = tint; }       // (black has none: the old ones stay)
    ch.setD("ColorTemperature", clampd(h.kelvin, 1700.0, 15000.0));
    ch.setD("ColorTint", clampd(h.tint, -100.0, 100.0));
    double ang, amt;
    vhw::rgbToAngleAmount(c, ang, amt);
    wbSetAngleAmount(ch, h, ang, amt);
    // the temperature and tint it just put in are changed settings too: VEGAS reports them after the colour (ColorTemperature, then ColorTint)
    wbTempTintChanged(ch, h);
    wbTempTintChanged(ch, h);
}
static void wbTempTintChanged(Change& ch, WbHidden& h)
{
    const double k = ch.d("ColorTemperature"), t = ch.d("ColorTint");
    if (!std::isfinite(k) || !std::isfinite(t)) return;
    // (VEGAS keeps the value it put into a setting even when the setting's range is narrower: the colour of 1666.67 K, set as 1700 K, still works from 1666.67)
    if (std::fabs(t - clampd(h.tint, -100.0, 100.0)) > 0.99) h.tint = t;
    if (std::fabs(k - clampd(h.kelvin, 1700.0, 15000.0)) > 0.99) h.kelvin = k;
    double rgb[3];
    vhw::tempTintToRgb(clampd(h.kelvin, 100.0, 1.0e6), clampd(h.tint, -1000.0, 1000.0), rgb, h.lum);
    bool differs = false;
    for (int j = 0; j < 3; ++j) if (!(std::fabs(rgb[j] - h.col[j]) <= kWbByte)) differs = true;
    if (!differs) return;
    ch.setColor("WhiteColor", Color{clampd(rgb[0], 0, 1), clampd(rgb[1], 0, 1), clampd(rgb[2], 0, 1), 1.0});
    double ang, amt;
    vhw::rgbToAngleAmount(rgb, ang, amt);
    if (h.flag == 0 && std::fabs(ch.d("AmountOfCorrection") - amt) > 0.001) ch.setD("AmountOfCorrection", clampd(amt, 0.0, 1.0));
    if (std::fabs(ch.d("Angle") - ang) > 0.1) ch.setD("Angle", clampd(ang, 0.0, 360.0));
}

// A new effect has been through VEGAS's first reports: its neutral grey (0.5) was reported changed, which put the grey's temperature and tint into the settings, and HasData cleared the flag.
void whiteBalanceCreated(Change& ch)
{
    WbHidden h;
    ch.setS("WbState", wbFormat(h));
    ch.setD("ColorTemperature", h.kelvin);
    ch.setD("ColorTint", h.tint);
    ch.setD("Angle", 90.0);
}

// Reported changes (VEGAS: changedParam). The buttons: "Select neutral color" takes the frame's average colour as the one that should be grey (what picking it with VEGAS's dropper does);
// "Equalize Histogram" switches equalising on, "Undo Equalization" off.
void whiteBalanceChanged(Change& ch)
{
    WbHidden h;
    if (!wbParse(ch.s("WbState"), h)) h = WbHidden();      // (nothing kept, as after a preset that reset it: the state of a made effect)
    bool state = true;
    if (ch.param == "EnhanceHistogram") { ch.setI("HasData", 1); h.flag = 0; }
    else if (ch.param == "ClearData") { ch.setI("HasData", 0); h.flag = 0; }
    else if (ch.param == "HasData") h.flag = 0;
    else if (ch.param == "WhiteColor") wbColourChanged(ch, h);
    else if (ch.param == "ColorTemperature" || ch.param == "ColorTint") wbTempTintChanged(ch, h);
    else if (ch.param == "SelectWhiteColor") {
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
        ch.setColor("WhiteColor", Color{clampd(R / n, 0, 1), clampd(G / n, 0, 1), clampd(B / n, 0, 1), 1.0});
        wbColourChanged(ch, h);
    } else state = false;
    if (state) ch.setS("WbState", wbFormat(h));
}

// LAB Adjust's sRGB steps. Decoding: the 256 values of an 8-bit picture are looked up (any other value is worked out). Encoding: the result is a whole level, so the 255 values
// of the linear input where the level steps up are found once (by halving, with the formula itself) and the level is looked up among them.
double srgbToLinear(double v)
{
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
float labEncodeSlow(double v)
{
    v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    return (float)(std::floor(v * 255.0 + 1e-3) / 255.0);
}
struct LabTables {
    float key[256];
    double val[256];
    double step[255];                        // step[k - 1]: the smallest input giving level k or more
    LabTables()
    {
        for (int k = 0; k < 256; ++k) {
            key[k] = (float)k * (1.f / 255.f);
            val[k] = srgbToLinear((double)key[k]);
        }
        for (int k = 1; k <= 255; ++k) {
            uint64_t lo = 0, hi = 0;                           // the doubles from 0 to 2 in the order of their bits
            const double two = 2.0;
            std::memcpy(&hi, &two, 8);
            while (lo < hi) {
                const uint64_t mid = lo + (hi - lo) / 2;
                double m;
                std::memcpy(&m, &mid, 8);
                if ((int)std::lround(labEncodeSlow(m) * 255.0) >= k) hi = mid; else lo = mid + 1;
            }
            std::memcpy(&step[k - 1], &lo, 8);
        }
    }
};
const LabTables& labTables()
{
    static const LabTables t;
    return t;
}
// (the helper below is for the Vulkan plan)
// The 256 linear values of the levels, then the 255 steps of the encoding, as doubles (each the bits of its high and low halves held in two floats).
void labAdjustGpuTables(std::vector<float>& t)
{
    const LabTables& lt = labTables();
    auto put2 = [&](double v) {
        uint32_t w[2];
        uint64_t b;
        std::memcpy(&b, &v, 8);
        w[0] = (uint32_t)(b >> 32);
        w[1] = (uint32_t)b;
        float f[2];
        std::memcpy(f, w, 8);
        t.push_back(f[0]);
        t.push_back(f[1]);
    };
    t.clear();
    for (int k = 0; k < 256; ++k) put2(lt.val[k]);
    for (int k = 0; k < 255; ++k) put2(lt.step[k]);
}

// The linear value of each of the 256 levels, as the 8-bit routine takes them (doubles as two floats each)
void labAdjust8GpuTable(std::vector<float>& t)
{
    t.clear();
    for (int k = 0; k < 256; ++k) {
        const double v = (double)k * 0.00392156862745098;
        const double l = v > 0.04045 ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
        uint64_t b;
        std::memcpy(&b, &l, 8);
        const uint32_t w[2] = {(uint32_t)(b >> 32), (uint32_t)b};
        float f[2];
        std::memcpy(f, w, 8);
        t.push_back(f[0]);
        t.push_back(f[1]);
    }
}

// LAB Adjust, measured against VEGAS: CIE L*a*b* (sRGB, D65). Invert Luminance turns L into 95.58 - L first (may go below zero, darker than black),
// then Lightness multiplies L (above 100 not clipped: the colour clips per channel). Channel a / b move the colour by 100 per unit, Desaturate scales
// them down afterwards. The 8-bit result is truncated.
// 8-bit pictures: VEGAS's byte routine (Vfx1, labadjustProcessor<uchar,4>) step for step in double: sRGB bytes (x 1/255) to CIE L*a*b* by the old
// constants (0.008856, 7.787, 16/116, D65 white 0.95047 / 1.08883, matrix 0.4124 ...), the settings applied, back with the matrix 3.2406 ... and
// the sRGB curve (clamped), cut to whole levels (truncated); colour is then capped by the alpha.
static void labAdjust8(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double light = c.d("Lightness");
    const double chA = c.d("Channela") * 100.0, chB = c.d("Channelb") * 100.0;
    const double keepSat = 1.0 - c.d("Desaturate");
    const bool invert = c.b("InvertLuminance");
    const double k255 = 0.00392156862745098;
    auto lin = [&](double v) { return v > 0.04045 ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92; };
    auto fwd = [&](double t) {
        if (t > 0.008856) {
            if (2.220446049250313e-16 > std::fabs(t)) return 0.0;
            return t > 0.0 ? std::pow(t, 0.3333333333333333) : -std::pow(-t, 0.3333333333333333);
        }
        return t * 7.7870001792907715 + 0.13793103448275862;
    };
    auto inv = [&](double f) {
        const double cube = f * f * f;
        return cube > 0.008856 ? cube : (f - 0.13793103448275862) / 7.787;
    };
    auto enc = [&](double v) { return v > 0.0031308 ? std::pow(v, 0.4166666666666667) * 1.055 - 0.055 : v * 12.92; };
    auto back = [&](double v) {
        const double e = enc(v);
        if (0.0 > e) return 0.0;
        if (e > 1.0) return 1.0;
        return e;
    };
    // lin() of the 256 levels, once
    double linTab[256];
    for (int i = 0; i < 256; ++i) linTab[i] = lin((double)i * k255);
    // The last step, (int)(back(v) * 255.0), is a staircase in v (back() rises with v): thrTab[n - 1] is the smallest v that gives n or more. A v well away from a
    // step takes its level from the table; one within 1e-12 of a step (or not a plain number) is worked out as before, so the result is the same either way.
    double thrTab[255];
    for (int n = 1; n <= 255; ++n) {
        double lo = 0.0, hi = 2.0;                  // (back(lo) * 255 < n <= back(hi) * 255: 0 gives 0, 2 is clamped to 1 and gives 255)
        for (int it = 0; it < 120; ++it) {
            const double mid = lo + (hi - lo) * 0.5;
            if (mid <= lo || mid >= hi) break;
            if ((int)(back(mid) * 255.0) >= n) hi = mid; else lo = mid;
        }
        thrTab[n - 1] = hi;
    }
    auto outLevel = [&](double v) {
        if (v > 0.0 && v < 2.0) {
            const int k = (int)(std::upper_bound(thrTab, thrTab + 255, v) - thrTab);      // the steps at or below v
            if (!((k > 0 && v - thrTab[k - 1] < 1e-12) || (k < 255 && thrTab[k] - v < 1e-12))) return k;
        }
        return (int)(back(v) * 255.0);
    };
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* q = c.src.at(x, y);
            const int r8 = v8::toLevel(q[0]), g8 = v8::toLevel(q[1]), b8 = v8::toLevel(q[2]), a8 = v8::toLevel(q[3]);
            const double rl = linTab[r8], gl = linTab[g8], bl = linTab[b8];
            const double X = ((rl * 0.4124 + gl * 0.3576) + bl * 0.1805) / 0.95047;
            const double Y = (gl * 0.7152 + rl * 0.2126) + bl * 0.0722;
            const double Z = ((rl * 0.0193 + gl * 0.1192) + bl * 0.9505) / 1.08883;
            const double fx = fwd(X), fy = fwd(Y), fz = fwd(Z);
            double L = fy * 116.0 - 16.0, A = (fx - fy) * 500.0, B = (fy - fz) * 200.0;
            L = (invert ? 95.5806655883789 - L : L) * light;
            A = (A + chA) * keepSat;
            B = (B + chB) * keepSat;
            const double gy = (L + 16.0) / 116.0, gx = A / 500.0 + gy, gz = gy - B / 200.0;
            const double xr = inv(gx) * 0.950469970703125, yr = inv(gy), zr = inv(gz) * 1.0888299942016602;
            const double cr = (xr * 3.2406 - yr * 1.5372) - zr * 0.4986;
            const double cg = (yr * 1.8758 - xr * 0.9689) + zr * 0.0415;
            const double cb = (xr * 0.0557 - yr * 0.204) + zr * 1.057;
            const int R = std::min(outLevel(cr), a8), G = std::min(outLevel(cg), a8), Bl = std::min(outLevel(cb), a8);
            float* o = c.out.at(x, y);
            o[0] = v8::fromLevel(R); o[1] = v8::fromLevel(G); o[2] = v8::fromLevel(Bl); o[3] = v8::fromLevel(a8);
        }
    });
}

void labAdjust(Ctx& c)
{
    if (oclUsable(c)) {
        vcol::LabSettings st;
        st.lightness = (float)c.d("Lightness");
        st.a = (float)(c.d("Channela") * 100.0);
        st.b = (float)(c.d("Channelb") * 100.0);
        st.invert = c.b("InvertLuminance") ? 1 : 0;
        st.desat = (float)(1.0 - c.d("Desaturate"));
        oclPixels(c, [st](const float* p, float* o) { vcol::labPx(st, p, o); });
        return;
    }
    if (c.levels) { labAdjust8(c); return; }
    const double light = c.d("Lightness"), da = c.d("Channela"), db = c.d("Channelb"), des = clampd(c.d("Desaturate"), 0, 1);
    const bool invert = c.b("InvertLuminance");
    const double wx = 0.95047, wz = 1.08883, e = 216.0 / 24389.0, kk = 24389.0 / 27.0;
    const LabTables& labTab = labTables();
    perPixel(c, [&](float& r, float& g, float& b, int, int) {
        auto lin = [&](float f) {
            if (f >= 0.f && f <= 1.f) {
                const int k = (int)(f * 255.f + 0.5f);
                if (k <= 255 && labTab.key[k] == f) return labTab.val[k];
            }
            return srgbToLinear(f);
        };
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
        auto enc = [&](double v) {
            if (!(v == v)) return labEncodeSlow(v);
            return (float)((double)(int)(std::upper_bound(labTab.step, labTab.step + 255, v) - labTab.step) / 255.0);
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

// VEGAS's 8-bit Broadcast Colors, transcribed from its own code (tables of doubles built from the settings, then a per-pixel pass over the bytes). Luma Y = 0.299 R + 0.587 G + 0.114 B and
// U, V from the BT.601 matrix are worked in a scale where full range is 0..200 (so "IRE x 2"); the luma limits become two tables over the rounded Y (the value, or a marker that Y is
// left alone, and the byte level the luma goes back to), the chroma limit a table of ratios over the rounded chroma length, the composite limits two more over the closeness to the limit.
// Every rounding is the magic-number one (x + 1.5e-8, then + 2^52 + 2^51, low word), every table value a double.
struct BcTables {
    double r[256], g[256], b[256];
    double q0[16 + 220], q1[16 + 220];          // from index -16
    double tc[296], td[241], te[241];          // (the composite tables are read one past their end by VEGAS: zero there)
    double v1, v2, v3;                          // the first chroma-knee index, and the first composite-knee indexes / 200
};
inline int bcNint(double x)                     // round to nearest even through the 2^52 + 2^51 trick, no nudge
{
    double t = x + 6755399441055744.0;
    int64_t bits;
    std::memcpy(&bits, &t, 8);
    return (int)(int32_t)(bits & 0xffffffff);
}
// the cubic that leaves the level `from` flat at u = 0 and joins the line of slope 1 at u = n having risen `rise`
inline void bcCubic(double rise, double n, double& A, double& B)
{
    double x6 = rise - n * 0.0;
    double x3 = n, x2 = n * n, x1 = n;
    x1 = x1 * x2;
    double x0 = x3 * 0.5;
    double x8 = x6 - x0;
    x3 = x3 / 3.0;
    x1 = x1 * -0.5;
    x2 = x2 / 3.0;
    x6 = x6 - x3;
    x8 = x8 / x1;
    x6 = x6 / x2;
    A = x6; B = x8;
}
// the same read from the top end: n counted negative as VEGAS does
inline void bcCubicTop(double start, double top, double nneg, double& A, double& B)
{
    double x3 = nneg;
    double x6 = start - top;
    double x2 = x3 * x3;
    double x0 = x3 * 0.0;
    double x1 = x3 * x2;
    x6 = x6 - x0;
    x0 = x3 * 0.5;
    double x8 = x6;
    x3 = x3 / 3.0;
    x1 = x1 * -0.5;
    x8 = x8 - x0;
    x2 = x2 / 3.0;
    x6 = x6 - x3;
    A = x6 / x2; B = x8 / x1;
}
void bcBuild(const double S[10], bool setup, bool studio, BcTables& T)
{
    const double c5o = setup ? 7.5 : 0.0, c4 = studio ? 16.0 : 0.0;
    double c0 = (studio ? 235.0 : 255.0) - c4;
    double c2 = 100.0 - c5o;
    const double hi = S[2] + S[2], lo = S[0] + S[0], c5 = c5o + c5o;
    c2 = c2 + c2;
    const double scale1 = c0 / c2, scale2 = c2 / c0;
    for (int i = 0; i < 256; ++i) {
        const double x = ((double)i - c4) * scale2;
        T.r[i] = x * 0.299; T.g[i] = x * 0.587; T.b[i] = x * 0.114 + c5;
    }
    const double ymin = (T.r[0] + T.g[0]) + T.b[0], ymax = (T.r[255] + T.g[255]) + T.b[255];
    int iLo = bcNint(lo + 1.5e-8), iHi = bcNint(hi + 1.5e-8);
    int edi = iLo, esi = iLo, r11 = iHi, ebx = iHi;
    int sum = iLo + iHi;
    const int mid = (sum - (sum >> 31)) >> 1;
    if (lo > ymin + 2.0 && S[1] > 0.0) {
        const double d = lo - ymin;
        const int n1 = bcNint(d + 1.5e-8);
        const double t = (double)mid - lo;
        double ratio = 1.0;
        if (d > t) ratio = t / d;
        const int m = bcNint((double)n1 * ratio * S[1] + 1.5e-8);
        esi = m + edi;
        if (esi != edi) {
            int e = edi - n1;
            edi = e > -16 ? e : -16;
            int e2 = mid;
            if (esi < mid) e2 = esi;
            esi = e2;
            const double x0 = (double)edi;
            double x1 = (double)esi;
            const double x2 = x1 - lo;
            x1 = (x1 - x0) / 3.0;
            if (!(x1 < x2)) edi = esi - bcNint(x2 * 3.0 - 0.499999985);
        }
    }
    if (ymax > hi + 2.0 && S[3] > 0.0) {
        const double d2 = ymax - hi;
        const int n2 = bcNint(d2 + 1.5e-8);
        const double t2 = hi - (double)mid;
        double ratio = 1.0;
        if (d2 > t2) ratio = t2 / d2;
        const int m2 = bcNint((double)n2 * ratio * S[3] + 1.5e-8);
        ebx = ebx - m2;
        if (ebx != r11) {
            const int eax = n2 + r11;
            int ecx = mid;
            if (ebx > ecx) ecx = ebx;
            int nr = 220;
            if (eax < 220) nr = eax;
            ebx = ecx; r11 = nr;
            const double x1 = (double)ebx;
            double x0 = (double)r11;
            const double x2 = hi - x1;
            x0 = (x0 - x1) / 3.0;
            if (!(x0 < x2)) r11 = bcNint(x2 * 3.0 - 0.499999985) + ebx;
        }
    }
    auto clampi = [](int v, int a, int b) { return v < a ? a : (v > b ? b : v); };
    edi = clampi(edi, -16, 220); esi = clampi(esi, -16, 220); ebx = clampi(ebx, -16, 220); r11 = clampi(r11, -16, 220);
    double* q0 = T.q0 + 16; double* q1 = T.q1 + 16;
    const double c4h = c4 + 0.5;                // the byte level the luma goes back to, kept half a level up
    auto back = [&](double v) { return (v - c5) * scale1 + c4h; };
    int e = -16;
    for (; e < edi; ++e) { q0[e] = lo; q1[e] = back(lo); }
    double A = 0, B = 0;
    if (edi < esi) {
        const double n = (double)(esi - edi);
        bcCubic((double)esi - lo, n, A, B);
        for (; e < esi; ++e) {
            const double u = (double)(e - edi);
            const double u2 = u * u;
            double v = (u2 * u) * B;
            v = v + u2 * A;
            v = v + lo;
            q0[e] = v; q1[e] = back(v);
        }
    }
    for (; e < ebx; ++e) { q0[e] = -3454.0; q1[e] = ((double)e - c5) * scale1 + c4h; }
    if (ebx < r11) {
        const double nneg = (double)(ebx - r11);
        bcCubicTop((double)ebx, hi, nneg, A, B);
        for (; e < r11; ++e) {
            const double w = (double)(e - r11);
            const double w2 = w * w;
            double v = (w2 * w) * B;
            v = v + w2 * A;
            v = v + hi;
            q0[e] = v; q1[e] = back(v);
        }
    }
    for (; e < 220; ++e) { q0[e] = hi; q1[e] = back(hi); }

    // the chroma limit
    const double c = S[4] + S[4];
    const int iC = bcNint(c + 1.5e-8);
    int cs = iC, ce = iC;
    const double cmax = (std::sqrt(25999.205849999995) + std::sqrt(25999.205849999995)) * scale2;
    if (c >= 1.0 && cmax > c + 2.0 && S[5] > 0.0) {
        const double d = cmax - c, cm1 = c - 1.0;
        const int n = bcNint(d + 1.5e-8);
        double ratio = 1.0;
        if (d > cm1) ratio = cm1 / d;
        const int m = bcNint((double)n * ratio * S[5] + 1.5e-8);
        const int es = iC - m;
        if (es != iC) {
            const int ecx = n + iC;
            int eax = 1;
            if (es > eax) eax = es;
            int eb = 296;
            if (ecx < eb) eb = ecx;
            cs = eax; ce = eb;
            const double x1 = (double)cs;
            double x0 = (double)ce;
            const double x2 = c - x1;
            x0 = (x0 - x1) / 3.0;
            if (!(x0 < x2)) ce = bcNint(x2 * 3.0 - 0.499999985) + cs;
        } else cs = es;
    }
    T.v1 = (double)cs;
    cs = clampi(cs, 0, 296); ce = clampi(ce, 0, 296);
    T.tc[0] = 0.0;
    int k = 1;
    if (cs > k) { for (int i = 1; i < cs; ++i) T.tc[i] = 1.0; k = cs; }
    A = 0; B = 0;
    if (cs < ce) bcCubicTop((double)cs, c, (double)(cs - ce), A, B);
    for (; k < ce; ++k) {
        const double w = (double)(k - ce);
        const double w2 = w * w;
        double v = (w2 * w) * B;
        v = v + w2 * A;
        v = v + c;
        T.tc[k] = v / (double)k;
    }
    for (; k < 296; ++k) T.tc[k] = c / (double)k;

    // the composite limits (two tables of the same shape)
    auto composite = [&](double smooth, double* t, double& slot) {
        int start = 200, end = 200;
        if (smooth > 0.0) {
            const int m = bcNint(smooth * 40.0 + 1.5e-8);
            start = 200 - m;
            if (start != 200) {
                int ecx = 1;
                end = 240;
                if (start > ecx) ecx = start;
                start = ecx;
                const int rest = 240 - start;
                const int third = rest / 3;
                if (!(third < 200 - start)) { end = 300 - start; end += end; }
            }
        }
        slot = (double)start / 200.0;
        t[0] = 0.0;
        start = clampi(start, 0, 240); end = clampi(end, 0, 240);
        int j = 1;
        if (start > j) { for (int i = 1; i < start; ++i) t[i] = 1.0; j = start; }
        double a = 0, b = 0;
        if (start < end) bcCubicTop((double)start, 200.0, (double)(start - end), a, b);
        for (; j < end; ++j) {
            const double w = (double)(j - end);
            const double w2 = w * w;
            double v = (w2 * w) * b;
            v = v + w2 * a;
            v = v + 200.0;
            t[j] = v / (double)j;
        }
        for (; j < 240; ++j) t[j] = 200.0 / (double)j;
        t[240] = 0.0;
    };
    composite(S[7], T.td, T.v2);
    composite(S[9], T.te, T.v3);
}
static void bcSetup8(Ctx& c, BcTables& T, double* sc)
{
    double S[10] = {c.d("LumaMin"), clampd(c.d("LumaMinSmoothness"), 0, 1), c.d("LumaMax"), clampd(c.d("LumaMaxSmoothness"), 0, 1), c.d("ChromaMax"), clampd(c.d("ChromaSmoothness"), 0, 1),
                    c.d("CompositeMin"), clampd(c.d("CompositeMinSmoothness"), 0, 1), c.d("CompositeMax"), clampd(c.d("CompositeMaxSmoothness"), 0, 1)};
    // (guessed from VEGAS's pictures: the 7.5 IRE setup stays applied while Studio RGB is on; only with Studio RGB off does the setting count)
    for (double& v : S) { if (!(v == v)) v = 0; v = std::max(-1e4, std::min(1e4, v)); }
    // Vfx1 reads the two flags straight into the table builder (checked in the PE: no OR on the way), and so does this.
    const bool studio = c.b("StudioRGB16To135"), setup = c.b("75IREsetup");
    bcBuild(S, setup, studio, T);
    const double q = (100.0 - (setup ? 7.5 : 0.0)) / (studio ? 219.0 : 255.0);
    const double Q = q + q, Q2 = Q * Q;
    const double cmin = S[6] + S[6], cmax = S[8] + S[8];
    const double PA = T.v1 * 0.5, PB = (T.v1 * T.v1) * 0.25, P2 = T.v2 * T.v2, P3 = T.v3 * T.v3;
    sc[0] = Q; sc[1] = Q2; sc[2] = cmin; sc[3] = cmax; sc[4] = PA; sc[5] = PB; sc[6] = P2; sc[7] = P3;
}
void broadcastColors8(Ctx& c)
{
    BcTables T;
    double sc[8];
    bcSetup8(c, T, sc);
    const double Q = sc[0], Q2 = sc[1], cmin = sc[2], cmax = sc[3], PA = sc[4], PB = sc[5], P2 = sc[6], P3 = sc[7];
    const double* q0 = T.q0 + 16; const double* q1 = T.q1 + 16;
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* sp = c.src.at(x, y);
            float* op = c.out.at(x, y);
            // the picture is stored r, g, b, a; VEGAS's bytes run b, g, r, a
            const int R = v8::toLevel(sp[0]), G = v8::toLevel(sp[1]), Bv = v8::toLevel(sp[2]), A = v8::toLevel(sp[3]);
            op[0] = sp[0]; op[1] = sp[1]; op[2] = sp[2]; op[3] = sp[3];
            double Y = T.g[G];
            Y = Y + T.r[R];
            Y = Y + T.b[Bv];
            double x9 = (double)R, x3 = (double)Bv, x2 = (double)G;
            double x1 = x2 * 0.289;
            x2 = x2 * 0.515;
            double x0 = x9 * 0.147;
            x9 = x9 * 0.615;
            double x7 = (double)Bv;
            x3 = x3 * 0.1;
            x7 = x7 * 0.436;
            x9 = x9 - x2;
            x9 = x9 - x3;
            x1 = x1 + x0;
            x7 = x7 - x1;
            x1 = std::fabs(x9) + std::fabs(x7);
            x1 = x1 * Q;
            int idx = bcNint(Y);
            idx = idx < -16 ? -16 : (idx > 219 ? 219 : idx);
            double x6 = cmax - Y;
            double x4 = Y - cmin;
            const double v0 = q0[idx];
            bool changed = false;
            if (v0 == -3454.0) {
                if (x6 > x1 && x4 > x1 && PA > x1) continue;
            } else {
                x4 = v0; x6 = cmax - v0; changed = true; x4 = x4 - cmin;
            }
            double K = P3; const double* tab = T.te;
            if (x6 > x4) { K = P2; x6 = x4; tab = T.td; }
            double x8 = x9 * x9;
            x0 = x7 * x7;
            x8 = x8 + x0;
            x8 = x8 * Q2;
            const double Ql = q1[idx];
            double x10 = x6 * x6;
            x10 = x10 * K;
            if (x8 > PB) {
                const double s = std::sqrt(x8);
                int i2 = bcNint((s + s) + 1.5e-8);
                i2 = i2 < 0 ? 0 : (i2 > 295 ? 295 : i2);
                double f = T.tc[i2];
                changed = true;
                x7 = x7 * f; x9 = x9 * f;
                f = f * f;
                x8 = x8 * f;
            }
            if (x8 > x10) {
                const double s = std::sqrt(x8);
                x6 = x6 / s;
                if (x6 > 0.8333333333333334) {
                    const double t = 200.0 / x6;
                    int i3 = bcNint(t - 0.499999985);
                    i3 = i3 < 0 ? 0 : (i3 > 240 ? 240 : i3);
                    x6 = tab[i3];
                }
                x7 = x7 * x6; x9 = x9 * x6;
            } else if (!changed) continue;
            double rr = x9 * 1.14;
            rr = rr + Ql; rr = rr + 1.5e-8;
            double gg = Ql;
            const double u394 = x7 * 0.394;
            x9 = x9 * 0.581;
            double bb = x7 * 2.032;
            bb = bb + Ql; bb = bb + 1.5e-8;
            gg = gg - u394; gg = gg - x9; gg = gg + 1.5e-8;
            int ri = bcNint(rr), gi = bcNint(gg), bi = bcNint(bb);
            ri = ri < 0 ? 0 : (ri > A ? A : ri);
            gi = gi < 0 ? 0 : (gi > A ? A : gi);
            bi = bi < 0 ? 0 : (bi > A ? A : bi);
            op[0] = v8::fromLevel(ri); op[1] = v8::fromLevel(gi); op[2] = v8::fromLevel(bi);
        }
    });
}
// The tables and constants of the 8-bit pass as doubles (two floats each): 8 constants (Q, Q2, cmin, cmax, PA, PB, P2, P3), then r, g, b (256 each), q0, q1 (236 each, from level -16),
// tc (296), td, te (241 each)
void broadcastColors8GpuTable(Ctx& c, std::vector<float>& t)
{
    BcTables T;
    double sc[8];
    bcSetup8(c, T, sc);
    t.clear();
    auto put = [&](double v) {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        const uint32_t w[2] = {(uint32_t)(b >> 32), (uint32_t)b};
        float f[2];
        std::memcpy(f, w, 8);
        t.push_back(f[0]);
        t.push_back(f[1]);
    };
    for (int i = 0; i < 8; ++i) put(sc[i]);
    for (int i = 0; i < 256; ++i) put(T.r[i]);
    for (int i = 0; i < 256; ++i) put(T.g[i]);
    for (int i = 0; i < 256; ++i) put(T.b[i]);
    for (int i = 0; i < 236; ++i) put(T.q0[i]);
    for (int i = 0; i < 236; ++i) put(T.q1[i]);
    for (int i = 0; i < 296; ++i) put(T.tc[i]);
    for (int i = 0; i < 241; ++i) put(T.td[i]);
    for (int i = 0; i < 241; ++i) put(T.te[i]);
}
void broadcastColors(Ctx& c)
{
    if (oclUsable(c)) {
        vcol::BroadcastSettings st;
        st.lumaMin = (float)c.d("LumaMin"); st.lumaMinSmooth = (float)c.d("LumaMinSmoothness");
        st.lumaMax = (float)c.d("LumaMax"); st.lumaMaxSmooth = (float)c.d("LumaMaxSmoothness");
        st.chromaMax = (float)c.d("ChromaMax"); st.chromaSmooth = (float)c.d("ChromaSmoothness");
        st.compMin = (float)c.d("CompositeMin"); st.compMinSmooth = (float)c.d("CompositeMinSmoothness");
        st.compMax = (float)c.d("CompositeMax"); st.compMaxSmooth = (float)c.d("CompositeMaxSmoothness");
        st.setup75 = c.b("75IREsetup") ? 1 : 0;
        st.studio = c.b("StudioRGB16To135") ? 1 : 0;
        vcol::BroadcastFrame fr;
        vcol::broadcastPrepare(st, fr);
        oclPixels(c, [st, fr](const float* p, float* o) { vcol::broadcastPx(st, fr, p, o); });
        return;
    }
    if (c.levels) { broadcastColors8(c); return; }
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
    // 8-bit frames lie in memory blue first, float ones red first, and VEGAS's plug-in numbers the channels in memory order
    const char* names[4] = {"BLUE", "GREEN", "RED", "ALPHA"};
    const char* namesF[4] = {"RED", "GREEN", "BLUE", "ALPHA"};
    const char* parts[5] = {" weight B", " weight G", " weight R", " weight A", " weight Offset"};
    const char* partsF[5] = {" weight R", " weight G", " weight B", " weight A", " weight Offset"};
    const bool normalise = c.b("Normalize");
    for (int o = 0; o < 4; ++o)
        for (int i = 0; i < 5; ++i) w[o][i] = (float)c.d((std::string(c.levels ? names[o] : namesF[o]) + (c.levels ? parts[i] : partsF[i])).c_str());
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
                if (c.levels) {
                    // VEGAS works on the bytes: the weighted sum (the offset counting 255 per unit) over the weights' sizes, cut to whole levels
                    float bs = w[o][4] * 255.f;
                    for (int i = 0; i < 4; ++i) bs += w[o][i] * std::floor(in[i] * 255.f + 0.5f);
                    if (normalise) bs = ws > 1e-6f ? bs / ws : 0.f;
                    const float t = std::floor(bs);
                    out[o] = std::min(255.f, std::max(0.f, t)) / 255.f;
                    continue;
                }
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
