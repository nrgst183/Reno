// More built-in stand-ins for VEGAS Pro 18 effects (see builtins.h): Color Corrector (Secondary), Color Corrector, Convolution Kernel,
// Spherize and Newsprint, with VEGAS's identifiers and parameter names.
//   - colour wheels and hue limits use vectorscope angles (atan2(V, U), U = 0.492 (B - Y), V = 0.877 (R - Y), Rec. 601 luma): red 103.5
//     degrees, green 240.7, blue 347.1, as in VEGAS's presets; a wheel point is (angle, amount 0..1);
//   - luminance limits are 0..255, saturation limits the vectorscope distance x 255 (pure red is 162, VEGAS's "all of it" upper limit);
//   - Offset is in 0..255 steps (a guess: VEGAS's range is not known).
#include "builtins_impl.h"
#include "builtins_vec.h"
#include <cmath>
#include "../core/base.h"
#include <algorithm>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
void vegasNewsprint(Ctx& c);   // builtins_newsprint.cpp: VEGAS's Newsprint halftone

namespace {

const char* const kGroup = "Reno Built-in";

// Rec. 601 Y and the vectorscope's U, V of a straight colour, and back.
inline void toYuv(float r, float g, float b, float& y, float& u, float& v)
{
    y = 0.299f * r + 0.587f * g + 0.114f * b;
    u = 0.492f * (b - y);
    v = 0.877f * (r - y);
}
// How much of a range a value is in: 1 between lo and hi, falling to 0 over 'smooth' outside them.
inline float inRange(float x, float lo, float hi, float smooth)
{
    if (hi < lo) std::swap(lo, hi);
    if (x >= lo && x <= hi) return 1.f;
    const float d = x < lo ? lo - x : x - hi;
    return smooth > 1e-6f ? std::max(0.f, 1.f - d / smooth) : 0.f;
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
bool colorCorrectorSecondaryRows(Ctx& c, RowFn& fn)
{
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

// ------------------------------------------------------------------ Color Corrector
// Measured on VEGAS: like the secondary corrector, works on Rec. 601 Y, U, V of the straight colour. Gain, gamma and offset change Y alone
// (see secondaryGamma); then each wheel adds its tint to (U, V) weighted by the NEW Y with a Gaussian - Low exp(-(Y / 0.402)^2), Mid
// exp(-((Y - 0.5) / 0.2414)^2), High exp(-((Y - 1) / 0.402)^2) - the tint being amount^2 x 0.5 at the wheel's angle; Saturation scales the sum last.
bool colorCorrectorRows(Ctx& c, RowFn& fn)
{
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
void convolutionKernel(Ctx& c)
{
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
        }
        d.changed = secondaryChanged;
        d.rows = colorCorrectorSecondaryRows;
        out.push_back(d);
    }
    {
        Def d = filter("colorcorrector", "Color Corrector", "Shadows, midtones and highlights pushed towards colours; saturation, gain, gamma and offset",
                       {wheelPoint("Low", "Low"), wheelPoint("Mid", "Mid"), wheelPoint("High", "High"), dbl("Saturation", "Saturation", 1, 0, 10),
                        dbl("Gamma", "Gamma", 1, 0.01, 10), dbl("Gain", "Gain", 1, 0, 10), dbl("Offset", "Offset", 0, -255, 255)},
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
