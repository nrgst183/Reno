// VEGAS's Rays and Glint (the "VelvetMatter" effects, managed CoreGraphics filters in ScriptPortal.MediaSoftware.TextGen.CoreGraphics.dll),
// followed step for step and checked against VEGAS snapshots with tools/effectref_compare.py. Parts living in CoreGraphics.Native.dll
// (not shipped with the effect) are written from the pictures: the line blur is a recursive (Young - van Vliet) Gaussian, the mask zoom is bilinear.
//
// Rays:  luma (or alpha) through a threshold gradient (0 below threshold - 0.05, ramp to threshold + boost at the threshold, then to
//        1 + boost + peak at white) is the mask; the mask is zoomed about the position repeatedly, each pass mixing 35% of a copy enlarged
//        by 1 + s (s grows by 1.45 + 0.25 (1 - quality) per pass, total reach amount * 4 * sqrt(2) * the larger side); the result picks a
//        colour from a gradient and is screened over the picture.
// Glint: luma through a soft threshold (0 at threshold - 0.05, full at the threshold); each of Streaks streaks (180 / n degrees apart, first
//        vertical) blurs it along its direction into its own channel (length from the two radii, as a fraction of the width), scaled by
//        1 + boost (2 boost above 0), coloured by the colour gradient and screened over the picture.
#include "builtins_more_internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <complex>
#include <cstdlib>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

namespace {

constexpr double kPiV = 3.14159265358979323846;

// ------------------------------------------------------------------ CoreGraphics' ParamLibrary pieces
// Evaluator::GetSegment: the point at or before pos and the next one, with the fraction between (clamped to the first/last point).
template <class K>
int segmentOf(const std::vector<K>& keys, double pos, int& next, double& frac)
{
    const int n = (int)keys.size();
    int idx = -1;
    next = -1;
    frac = 0;
    for (int i = 0; i < n; ++i) {
        const double k = keys[(size_t)i];
        if (k > pos) {
            if (i == 0) idx = 0;
            else {
                const double prev = keys[(size_t)i - 1];
                if (prev == k) idx = i;
                else { idx = i - 1; next = i; frac = (pos - prev) / (k - prev); }
            }
            break;
        }
    }
    if (idx < 0) idx = n - 1;
    return idx;
}

// A Gradient of colours (A R G B as floats), linear between its points.
struct Gradient {
    std::vector<double> keys;
    std::vector<std::array<float, 4>> vals;
    void add(double key, float a, float r, float g, float b)
    {
        size_t at = 0;
        while (at < keys.size() && keys[at] < key) ++at;
        keys.insert(keys.begin() + (std::ptrdiff_t)at, key);
        vals.insert(vals.begin() + (std::ptrdiff_t)at, std::array<float, 4>{a, r, g, b});
    }
    std::array<float, 4> at(double pos) const
    {
        if (keys.empty()) return {1.f, 1.f, 0.f, 0.f};
        int next;
        double frac;
        const int i = segmentOf(keys, pos, next, frac);
        std::array<float, 4> v = vals[(size_t)i];
        if (next < 0) return v;
        const std::array<float, 4>& w = vals[(size_t)next];
        for (int k = 0; k < 4; ++k) v[(size_t)k] = (float)(v[(size_t)k] + (w[(size_t)k] - v[(size_t)k]) * frac);
        return v;
    }
};

// The fixed curve both effects space their colour points with: Bezier segments through (0, 0), (0.2, 0.4), (0.6, 0.9), (1, 1).
double colourCurve(double x)
{
    struct P { double key, value, left, right; };
    static const P pts[4] = {{0.0, 0.0, 0.0, 0.0}, {0.2, 0.4, -0.15, 0.24}, {0.6, 0.9, -0.05, 0.05}, {1.0, 1.0, 0.0, 0.0}};
    std::vector<double> keys = {0.0, 0.2, 0.6, 1.0};
    int next;
    double f;
    const int i = segmentOf(keys, x, next, f);
    if (next < 0) return pts[i].value;
    const double p0 = pts[i].value, p1 = pts[i].value + pts[i].right, p2 = pts[next].value + pts[next].left, p3 = pts[next].value;
    const double u = 1 - f;
    return u * u * u * p0 + 3 * f * u * u * p1 + 3 * f * f * u * p2 + f * f * f * p3;
}

// ColorSpaceConverter(Lch -> sRGB'): Lch -> Lab -> XYZ (D65, 0..100) -> linear sRGB (matrix applied to XYZ as is, so L near 10 already gives ~1) -> sRGB curve.
void lchToSrgb(float L, float C, float H, float rgb[3])
{
    const double h = (double)H * kPiV / 180.0;
    const float a = (float)(std::cos(h) * (double)C), b = (float)(std::sin(h) * (double)C);
    const double fy = ((double)L + 16.0) / 116.0, fx = (double)a / 500.0 + fy, fz = fy - (double)b / 200.0;
    auto inv = [](double t) { const double t3 = t * t * t; return t3 > 0.008856 ? t3 : (t - 0.13793103448275862) / 7.787; };
    const float X = (float)(95.047 * inv(fx)), Y = (float)(100.0 * inv(fy)), Z = (float)(108.883 * inv(fz));
    const float m[9] = {3.241f, -1.5374f, -0.4986f, -0.9692f, 1.876f, 0.0416f, 0.0556f, -0.204f, 1.057f};
    for (int k = 0; k < 3; ++k) {
        const float v = m[k * 3] * X + m[k * 3 + 1] * Y + m[k * 3 + 2] * Z;
        rgb[k] = v > 0.0030400000978261232f ? (float)(1.055 * std::pow((double)v, 0.4166666666666667) - 0.055) : (float)(12.92 * (double)v);
    }
}

// The colour gradient: (|sweep| / 2.5) + 1 points spaced by the curve; at intensity t the colour is Lch(L(t), saturation * 10, hueEnd - sweep * t),
// premultiplied by t. Stored as 256 bytes A R G B (Gradient::GetLUT).
void colourLut(double hue, double sweep, double sat, double lTop, double lDrop, double hueEnd, uint8_t lut[256][4])
{
    const int n = (int)(std::fabs(sweep) / 2.5) + 1;
    Gradient g;
    g.add(0.0, 0, 0, 0, 0);
    for (int i = 0; i < n && i < 100000; ++i) {
        double t = colourCurve((double)(i + 1) / n);
        t = clampd(t, 0.0, 1.0);
        const float L = (float)(lTop - lDrop * (1.0 - t)) * 10.f;
        const float S = (float)(sat * 10.0);
        const float H = (float)(hueEnd - sweep * t);
        float rgb[3];
        lchToSrgb(L, S, H, rgb);
        const float a = (float)t;
        g.add(t, a, rgb[0] * a, rgb[1] * a, rgb[2] * a);
    }
    for (int i = 0; i < 256; ++i) {
        const std::array<float, 4> c = g.at(i / 255.0);
        for (int k = 0; k < 4; ++k) {
            const double v = clampd((double)c[(size_t)k] * 255.0 + 0.5, 0.0, 255.0);
            lut[i][k] = (uint8_t)v;
        }
    }
    (void)hue;
}

// GaussianBlur's pixel size -> standard deviation: its TCB spline of (deviation, radius) points, looked up backwards.
struct SigmaSpline {
    static constexpr int N = 12;
    double keys[N] = {0.0, 0.75, 6.0, 12.0, 18.0, 24.0, 30.0, 36.0, 42.0, 48.0, 51.0, 54.0};
    double vals[N] = {0.5, 3.0, 25.0, 55.0, 88.0, 127.0, 200.0, 300.0, 445.0, 730.0, 1200.0, 3000.0};
    double eval(double x) const
    {
        std::vector<double> k(keys, keys + N);
        int next;
        double s;
        const int idx = segmentOf(k, x, next, s);
        if (next < 0) return vals[idx];
        const double P1 = vals[idx], P2 = vals[next], dt = keys[next] - keys[idx];
        double prev = P1, dtp = dt, nextv = P2, dtn = dt;
        if (idx > 0) { prev = vals[idx - 1]; dtp = keys[idx] - keys[idx - 1]; }
        if (next + 1 < N) { nextv = vals[next + 1]; dtn = keys[next + 1] - keys[next]; }
        const double d1 = (0.5 * (P1 - prev) + 0.5 * (P2 - P1)) * (2 * dt / (dtp + dt));
        const double d2 = (0.5 * (P2 - P1) + 0.5 * (nextv - P2)) * (2 * dt / (dtn + dt));
        const double s2 = s * s, s3 = s2 * s;
        return (2 * s3 - 3 * s2 + 1) * P1 + (-2 * s3 + 3 * s2) * P2 + (s3 - 2 * s2 + s) * d1 + (s3 - s2) * d2;
    }
    double search(double lo, double hi, double target, int depth) const
    {
        const double mid = (lo + hi) / 2, flo = eval(lo), fmid = eval(mid), fhi = eval(hi);
        if (std::fabs(fmid - target) <= 0.001 || depth > 60) return mid;
        if (target > flo && target <= fmid) return search(lo, mid, target, depth + 1);
        if (target > fmid && target <= fhi) return search(mid, hi, target, depth + 1);
        return lo;
    }
    double reverse(double v) const
    {
        if (v == vals[N - 1]) return keys[N - 1];
        double hiv = vals[N - 1], hik = keys[N - 1];
        for (int i = N - 2; i >= 0; --i) {
            if (vals[i] == v) return keys[i];
            if (vals[i] < v && hiv > v) return search(keys[i], hik, v, 0);
            hiv = vals[i];
            hik = keys[i];
        }
        return hik;
    }
};

double sigmaForDiameter(double px)
{
    static const SigmaSpline sp;
    if (!(px > 0)) return 0;
    return sp.reverse(std::min(px, 1e7) / 2.0);
}

// ------------------------------------------------------------------ planes
struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    void resize(int W, int H) { w = W; h = H; v.assign((size_t)W * H, 0.f); }
    float& at(int x, int y) { return v[(size_t)y * w + x]; }
    float at(int x, int y) const { return v[(size_t)y * w + x]; }
    // bilinear at pixel coordinates (centres at integers), clamped to the edges
    float sample(double x, double y) const
    {
        x = clampd(x, 0.0, (double)(w - 1));
        y = clampd(y, 0.0, (double)(h - 1));
        int x0 = (int)x, y0 = (int)y;
        if (x0 > w - 2) x0 = std::max(0, w - 2);
        if (y0 > h - 2) y0 = std::max(0, h - 2);
        const float fx = (float)(x - x0), fy = (float)(y - y0);
        const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
        const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
        const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
        return a + (b - a) * fy;
    }
};

// The luma byte of a premultiplied pixel, as CoreGraphics' Desaturate makes it.
inline int lumaByte(const float* p)
{
    const double v = 255.0 * (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]);
    return (int)clampd(std::floor(v + 0.5), 0.0, 255.0);
}
inline int alphaByte(const float* p) { return (int)clampd(std::floor(255.0 * p[3] + 0.5), 0.0, 255.0); }

// Screen of a premultiplied byte colour over a premultiplied pixel (Composite, Screen).
inline void screenOver(float* o, const uint8_t c[4])
{
    const float a = c[0] / 255.f, r = c[1] / 255.f, g = c[2] / 255.f, b = c[3] / 255.f;
    o[0] = o[0] + r - o[0] * r;
    o[1] = o[1] + g - o[1] * g;
    o[2] = o[2] + b - o[2] * b;
    o[3] = o[3] + a - o[3] * a;
}

// Reduce flicker: CoreGraphics' Blur3x3 (a 1 2 1 binomial each way) of a byte plane.
void blur3x3(std::vector<int>& g, int W, int H)
{
    std::vector<int> t(g.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int xl = std::max(0, x - 1), xr = std::min(W - 1, x + 1);
            t[(size_t)y * W + x] = g[(size_t)y * W + xl] + 2 * g[(size_t)y * W + x] + g[(size_t)y * W + xr];
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int yu = std::max(0, y - 1), yd = std::min(H - 1, y + 1);
            g[(size_t)y * W + x] = (t[(size_t)yu * W + x] + 2 * t[(size_t)y * W + x] + t[(size_t)yd * W + x] + 8) / 16;
        }
}

// ------------------------------------------------------------------ the masks (SimpleMask)
void lineBlur(float* v, int n, int stride, double sigma, std::vector<double>& tmp);

// EllipseMask / RectangleMask coverage 0..1 per pixel: Width x Height (fractions of the frame) about (xpos, ypos) (y from the top), rectangle
// corners rounded by corner x height / 3. Native drawing puts pixel centres on whole coordinates (half a pixel right and down of the plain
// mapping). With a blur the shape is drawn on a canvas 1.25 times the frame, blurred there (diameter blur x canvas width), and cropped.
} // namespace
void shapeMaskVelvet(int W, int H, double xpos, double ypos, double w, double h, double corner, double blur, bool ellipse, std::vector<float>& m)
{
    m.assign((size_t)W * H, 0.f);
    if (W <= 0 || H <= 0) return;
    const bool soft = blur > 0;
    const int CW = soft ? (int)(W * 1.25) : W, CH = soft ? (int)(H * 1.25) : H;
    const double cx = (xpos - 0.5) * W + CW / 2.0 + 0.5, cy = (ypos - 0.5) * H + CH / 2.0 + 0.5;
    const double hw = w * W / 2, hh = h * H / 2;
    const double rr = ellipse ? 0.0 : std::min(std::min(hw, hh), corner * H / 3.0);
    std::vector<float> cv((size_t)CW * CH, 0.f);
    const int ss = 4;
    const int x0 = std::max(0, (int)std::floor(cx - hw) - 1), x1 = std::min(CW - 1, (int)std::ceil(cx + hw) + 1);
    const int y0 = std::max(0, (int)std::floor(cy - hh) - 1), y1 = std::min(CH - 1, (int)std::ceil(cy + hh) + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            int in = 0;
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) {
                    const double sx = x + (i + 0.5) / ss - cx, sy = y + (j + 0.5) / ss - cy;
                    if (ellipse) in += hw > 0 && hh > 0 && (sx * sx) / (hw * hw) + (sy * sy) / (hh * hh) <= 1.0;
                    else {
                        const double ax = std::fabs(sx), ay = std::fabs(sy);
                        if (ax > hw || ay > hh) continue;
                        const double dx = ax - (hw - rr), dy = ay - (hh - rr);
                        in += (dx <= 0 || dy <= 0 || dx * dx + dy * dy <= rr * rr);
                    }
                }
            cv[(size_t)y * CW + x] = (float)in / (ss * ss);
        }
    if (soft) {
        const double sigma = sigmaForDiameter(blur * CW);
        std::vector<double> tmp;
        for (int y = 0; y < CH; ++y) lineBlur(&cv[(size_t)y * CW], CW, 1, sigma, tmp);
        for (int x = 0; x < CW; ++x) lineBlur(&cv[(size_t)x], CH, CW, sigma, tmp);
    }
    const int ox = (CW - W) / 2, oy = (CH - H) / 2;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) m[(size_t)y * W + x] = (float)clampd(cv[(size_t)(y + oy) * CW + x + ox], 0.0, 1.0);
}
namespace {

// Mask settings for these effects: mode Disabled / Effect / Source / Show mask only, and a shape, as per-pixel coverage 0..1:
// - ellipse or rectangle: Width, Height in percent about Position, rectangle corners rounded by CornerRadius x height / 3; Softness (the
//   class's Blur) as above (1.25x canvas, blurred, cropped);
// - bar (Width percent across, Angle in turns, Position) or wipe (Progress percent along Angle, Falloff soft edge, Strength what is left
//   outside). LeadingRegion/TrailingRegion are unused.
void maskCoverage(const Ctx& c, int typeIndex, const char* const* typeNames, std::vector<float>& m, int W, int H)
{
    m.assign((size_t)W * H, 1.f);
    const char* type = typeNames[std::max(0, std::min(3, typeIndex))];
    double px, py;
    c.xy("MaskPosition", px, py);
    const double blur = std::max(0.0, c.d("MaskSoftness"));
    const double strength = clampd(c.d("MaskStrength") / 100.0, 0.0, 2.0);
    const double angle = c.d("MaskAngle") * 2 * kPiV;
    const double ca = std::cos(angle), sa = std::sin(angle);
    const double mw = std::max(0.0, c.d("MaskWidth")) / 100.0, mh = std::max(0.0, c.d("MaskHeight")) / 100.0;
    const double progress = clampd(c.d("MaskProgress") / 100.0, 0.0, 1.5), falloff = std::max(0.0, c.d("MaskFalloff"));
    const bool ellipse = std::strcmp(type, "Ellipse") == 0, rect = std::strcmp(type, "Rectangle") == 0, bar = std::strcmp(type, "Bar") == 0;
    if (ellipse || rect) {
        if (rect && (mw <= 1e-5 || mh <= 1e-5)) return;
        shapeMaskVelvet(W, H, px, 1 - py, mw, mh, rect ? std::max(0.0, c.d("MaskCornerRadius")) : 0.0, blur, ellipse, m);
        return;
    }
    const double cx = px * W, cy = (1 - py) * H;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double cover;
            if (bar) {
                const double d = std::fabs((x + 0.5 - cx) * -sa + (y + 0.5 - cy) * ca);
                const double hw = mw * std::hypot((double)W, (double)H) / 2;
                cover = d <= hw ? 1.0 : 0.0;
            } else {
                const double diag = std::fabs(W * ca) + std::fabs(H * sa);
                const double along = ((x + 0.5 - W / 2.0) * ca + (y + 0.5 - H / 2.0) * sa) / std::max(1.0, diag) + 0.5;
                const double edge = falloff * 0.5;
                cover = edge > 1e-6 ? clampd((progress - along) / edge + 0.5, 0.0, 1.0) : (along <= progress ? 1.0 : 0.0);
                cover = 1.0 - (1.0 - cover) * std::min(1.0, strength);
            }
            m[(size_t)y * W + x] = (float)cover;
        }
}

// mode 3: the mask itself, white over opaque black; returns true when that is what the effect shows.
bool maskOnly(Ctx& c, const std::vector<float>& m)
{
    const int W = c.out.w, H = c.out.h;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            const float v = m[(size_t)y * W + x];
            o[0] = o[1] = o[2] = v;
            o[3] = 1.f;
        }
    return true;
}

void multiplyBy(Buf& b, const std::vector<float>& m)
{
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            float* p = b.at(x, y);
            const float v = m[(size_t)y * b.w + x];
            for (int k = 0; k < 4; ++k) p[k] *= v;
        }
}

// ------------------------------------------------------------------ blurs along a direction (Glint)
// CoreGraphics.Native's GaussianBlurMaskNative (the one-channel blur used by DirectionalBlur and the masks) is not in the assembly; VEGAS's
// pictures match Young & van Vliet's third-order recursive Gaussian run forwards then backwards, q = the deviation GaussianBlur passes,
// each run starting from the value at its end (edges carried on outside).
void lineBlur(float* v, int n, int stride, double sigma, std::vector<double>& tmp)
{
    if (n <= 1 || !(sigma > 0.05)) return;
    const double q = sigma;
    const double b0 = 1.57825 + 2.44413 * q + 1.4281 * q * q + 0.422205 * q * q * q;
    const double b1 = 2.44413 * q + 2.85619 * q * q + 1.26661 * q * q * q;
    const double b2 = -(1.4281 * q * q + 1.26661 * q * q * q);
    const double b3 = 0.422205 * q * q * q;
    const double c1 = b1 / b0, c2 = b2 / b0, c3 = b3 / b0, B = 1 - (c1 + c2 + c3);
    tmp.resize((size_t)n);
    double p1 = v[0], p2 = p1, p3 = p1;
    for (int i = 0; i < n; ++i) {
        const double w = B * v[(size_t)i * stride] + c1 * p1 + c2 * p2 + c3 * p3;
        tmp[(size_t)i] = w;
        p3 = p2; p2 = p1; p1 = w;
    }
    p1 = p2 = p3 = tmp[(size_t)n - 1];
    for (int i = n - 1; i >= 0; --i) {
        const double w = B * tmp[(size_t)i] + c1 * p1 + c2 * p2 + c3 * p3;
        v[(size_t)i * stride] = (float)w;
        p3 = p2; p2 = p1; p1 = w;
    }
}

// DirectionalBlur of one plane: angle in degrees (0: vertical), blur diameter in pixels.
void directionalBlur(std::vector<float>& g, int W, int H, double angleDeg, double diameter)
{
    double a = std::fmod(angleDeg, 360.0);
    if (a < 0) a += 360.0;
    if (a > 90) a = a < 270 ? a - 180 : a - 360;
    a = clampd(a, -90, 90);
    const double sigma = sigmaForDiameter(diameter);
    if (!(sigma > 0)) return;
    std::vector<double> tmp;
    if (std::fabs(a) <= 0.1) {
        for (int x = 0; x < W; ++x) lineBlur(&g[(size_t)x], H, W, sigma, tmp);
        return;
    }
    if (std::fabs(std::fabs(a) - 90) <= 0.1) {
        for (int y = 0; y < H; ++y) lineBlur(&g[(size_t)y * W], W, 1, sigma, tmp);
        return;
    }
    const int q = std::max(0, std::min(3, (int)(a + 90) / 45));
    const bool cols = q == 0 || q == 3;
    const double t = cols ? std::tan((q == 0 ? a + 90 : 90 - a) * kPiV / 180) : std::tan((q == 1 ? -a : a) * kPiV / 180);
    const int L = cols ? W : H, M = cols ? H : W;
    const int sign = (q == 0 || q == 1) ? 1 : -1;
    const int M2 = (int)std::ceil(M + t * L);
    if (M2 > 200000 || (size_t)M2 * (size_t)L > (size_t)400000000) return;
    std::vector<float> sh((size_t)M2 * L, 0.f);
    auto offOf = [&](int i) { return sign > 0 ? i * t : (L - 1 - i) * t; };
    auto src = [&](int i, int j) -> float { return cols ? g[(size_t)j * W + i] : g[(size_t)i * W + j]; };
    for (int i = 0; i < L; ++i) {
        const double off = offOf(i);
        for (int r = 0; r < M2; ++r) {
            const double pos = clampd(r - off, 0.0, (double)(M - 1));
            int j0 = (int)pos;
            if (j0 > M - 2) j0 = std::max(0, M - 2);
            const int j1 = std::min(M - 1, j0 + 1);
            const float f = (float)(pos - j0);
            sh[(size_t)r * L + i] = src(i, j0) + (src(i, j1) - src(i, j0)) * f;
        }
    }
    for (int r = 0; r < M2; ++r) lineBlur(&sh[(size_t)r * L], L, 1, sigma, tmp);
    for (int i = 0; i < L; ++i) {
        const double off = offOf(i);
        for (int j = 0; j < M; ++j) {
            const double pos = clampd(j + off, 0.0, (double)(M2 - 1));
            int r0 = (int)pos;
            if (r0 > M2 - 2) r0 = std::max(0, M2 - 2);
            const int r1 = std::min(M2 - 1, r0 + 1);
            const float f = (float)(pos - r0);
            const float v = sh[(size_t)r0 * L + i] + (sh[(size_t)r1 * L + i] - sh[(size_t)r0 * L + i]) * f;
            if (cols) g[(size_t)j * W + i] = v; else g[(size_t)i * W + j] = v;
        }
    }
}

// Rays' noise texture (GenerateNoiseMask, 512 x 512, uniform in lo..hi from seed 12335). VEGAS's generator is native code Reno lacks;
// this is a plain LCG, so streaks look alike but differ.
const std::vector<float>& noiseTexture(double lo, double hi)
{
    static thread_local std::vector<float> tex;
    static thread_local double tlo = -1, thi = -1;
    if (tex.empty() || tlo != lo || thi != hi) {
        tex.resize(512 * 512);
        uint32_t x = 12335u;
        for (float& v : tex) {
            x = x * 214013u + 2531011u;
            v = (float)(lo + (hi - lo) * ((x >> 16) & 0x7FFF) / 32767.0);
        }
        tlo = lo;
        thi = hi;
    }
    return tex;
}


// ------------------------------------------------------------------ Fill Light's pieces
// CoreGraphics' ColorMatrixClass: a 5 x 5 float matrix multiplied by colour rows (R G B A 1); Multiply(m) makes this = this x m.
struct ColorMatrix {
    float m[5][5];
    ColorMatrix() { for (int i = 0; i < 5; ++i) for (int j = 0; j < 5; ++j) m[i][j] = i == j ? 1.f : 0.f; }
    void multiply(const ColorMatrix& a)
    {
        float r[5][5];
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) {
                float acc = 0.f;
                for (int k = 0; k < 5; ++k) acc += m[i][k] * a.m[k][j];
                r[i][j] = acc;
            }
        std::memcpy(m, r, sizeof m);
    }
    void transform(float r, float g, float b, float out[4]) const
    {
        out[1] = r * m[0][0] + g * m[1][0] + b * m[2][0];
        out[2] = r * m[0][1] + g * m[1][1] + b * m[2][1];
        out[3] = r * m[0][2] + g * m[1][2] + b * m[2][2];
        out[0] = r * m[0][3] + g * m[1][3] + b * m[2][3];
    }
    void scale(float a, float r, float g, float b)
    {
        ColorMatrix o;
        o.m[0][0] = r; o.m[1][1] = g; o.m[2][2] = b; o.m[3][3] = a;
        multiply(o);
    }
    void saturation(float v)
    {
        const float lr = 0.299f, lg = 0.587f, lb = 0.114f;
        ColorMatrix o;
        o.m[0][0] = (1.f - v) * lr + v; o.m[0][1] = (1.f - v) * lr; o.m[0][2] = (1.f - v) * lr;
        o.m[1][0] = (1.f - v) * lg; o.m[1][1] = (1.f - v) * lg + v; o.m[1][2] = (1.f - v) * lg;
        o.m[2][0] = (1.f - v) * lb; o.m[2][1] = (1.f - v) * lb; o.m[2][2] = (1.f - v) * lb + v;
        multiply(o);
    }
    static void rot(ColorMatrix& t, int axis, float s, float c)
    {
        ColorMatrix o;
        if (axis == 0) { o.m[1][1] = c; o.m[1][2] = -s; o.m[2][1] = s; o.m[2][2] = c; }
        else if (axis == 1) { o.m[0][0] = c; o.m[0][2] = s; o.m[2][0] = -s; o.m[2][2] = c; }
        else { o.m[0][0] = c; o.m[0][1] = -s; o.m[1][0] = s; o.m[1][1] = c; }
        t.multiply(o);
    }
    static void shearBlue(ColorMatrix& t, float a, float b)
    {
        ColorMatrix o;
        o.m[0][2] = a; o.m[1][2] = b;
        t.multiply(o);
    }
    // HueRotate (Paul Haeberli's rotation about the grey axis, optionally keeping luminance)
    void hueRotate(float rad, bool keepLuma)
    {
        rad = (float)(6.283185307179586 - std::fmod((double)rad, 6.283185307179586));
        ColorMatrix t;
        const float r2 = (float)std::sqrt(2.0), r3 = (float)std::sqrt(3.0);
        const float sn = (float)std::sin((double)rad), cs = (float)std::cos((double)rad);
        const float v6 = 1.f / r2, v7 = 1.f / r2;
        rot(t, 0, -v6, v7);
        const float v8 = -1.f / r3, v9 = r2 / r3;
        rot(t, 1, -v8, v9);
        float sa = 0.f, sb = 0.f;
        if (keepLuma) {
            float c[4];
            t.transform(0.299f, 0.587f, 0.114f, c);
            sa = c[1] / c[3];
            sb = c[2] / c[3];
            shearBlue(t, sa, sb);
        }
        rot(t, 2, sn, cs);
        if (keepLuma) shearBlue(t, -sa, -sb);
        rot(t, 1, v8, v9);
        rot(t, 0, v6, v7);
        multiply(t);
    }
    void colorize(float hue, float sat)
    {
        scale(1.f, 3.3444817066192627f, 0.f, 0.f);
        hueRotate((float)((double)hue * 3.141592653589793 * 2.0), true);
        saturation(sat);
    }
};

// ParamLibrary.Curve of Bezier points (key, value, left control, right control) at x.
struct BezierPoint { double key, value, left, right; };
double bezierCurve(const std::vector<BezierPoint>& pts, double x)
{
    std::vector<double> keys;
    for (const BezierPoint& p : pts) keys.push_back(p.key);
    int next;
    double f;
    const int i = segmentOf(keys, x, next, f);
    if (next < 0) return pts[(size_t)i].value;
    const double p0 = pts[(size_t)i].value, p1 = p0 + pts[(size_t)i].right, p3 = pts[(size_t)next].value, p2 = p3 + pts[(size_t)next].left;
    const double u = 1 - f;
    return u * u * u * p0 + 3 * f * u * u * p1 + 3 * f * f * u * p2 + f * f * f * p3;
}

// Gaussian blur of float planes (one weight per pixel, cut at 3 deviations, edges carried on), in place.
void firGaussian(std::vector<float>& v, int W, int H, int planes, double sigma)
{
    if (!(sigma > 0.05) || W <= 0 || H <= 0) return;
    const int r = std::max(1, std::min(4000, (int)std::ceil(sigma * 3.0)));
    std::vector<double> k((size_t)(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += (k[(size_t)(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma)));
    for (double& w : k) w /= sum;
    std::vector<float> tmp(v.size());
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x)
            for (int p = 0; p < planes; ++p) {
                double acc = 0;
                for (int j = -r; j <= r; ++j) acc += k[(size_t)(j + r)] * v[((size_t)y * W + std::max(0, std::min(W - 1, x + j))) * planes + p];
                tmp[((size_t)y * W + x) * planes + p] = (float)acc;
            }
    });
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x)
            for (int p = 0; p < planes; ++p) {
                double acc = 0;
                for (int j = -r; j <= r; ++j) acc += k[(size_t)(j + r)] * tmp[((size_t)std::max(0, std::min(H - 1, y + j)) * W + x) * planes + p];
                v[((size_t)y * W + x) * planes + p] = (float)acc;
            }
    });
}

inline int byteOf(float v) { return (int)clampd(std::floor((double)v * 255.0 + 0.5), 0.0, 255.0); }


// ------------------------------------------------------------------ Starburst's pieces
// An in-place radix-2 FFT of n (a power of two) complex values with a stride; inverse unscaled.
void fft1(std::complex<double>* a, int n, int stride, bool inverse)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[(size_t)i * stride], a[(size_t)j * stride]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPiV / len * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1, 0);
            for (int k = 0; k < len / 2; ++k) {
                std::complex<double>& u = a[(size_t)(i + k) * stride];
                std::complex<double>& v = a[(size_t)(i + k + len / 2) * stride];
                const std::complex<double> t = v * w;
                v = u - t;
                u = u + t;
                w *= wl;
            }
        }
    }
}
void fft2(std::vector<std::complex<double>>& a, int w, int h, bool inverse)
{
    forRows(h, w, [&](int y) { fft1(&a[(size_t)y * w], w, 1, inverse); });
    forRows(w / 8, h, [&](int xb) {
        std::vector<std::complex<double>> col((size_t)h * 8);
        for (int y = 0; y < h; ++y)
            for (int k = 0; k < 8; ++k) col[(size_t)k * h + y] = a[(size_t)y * w + xb * 8 + k];
        for (int k = 0; k < 8; ++k) fft1(&col[(size_t)k * h], h, 1, inverse);
        for (int y = 0; y < h; ++y)
            for (int k = 0; k < 8; ++k) a[(size_t)y * w + xb * 8 + k] = col[(size_t)k * h + y];
    });
}

// System.Random as .NET Framework has it (Knuth's subtractive generator), for StarShape's Variance.
struct DotNetRandom {
    int seedArray[56];
    int inext = 0, inextp = 21;
    explicit DotNetRandom(int seed)
    {
        const int MBIG = 2147483647, MSEED = 161803398;
        const int subtraction = seed == (-2147483647 - 1) ? 2147483647 : std::abs(seed);
        int mj = MSEED - subtraction;
        seedArray[55] = mj;
        int mk = 1;
        for (int i = 1; i < 55; ++i) {
            const int ii = (21 * i) % 55;
            seedArray[ii] = mk;
            mk = mj - mk;
            if (mk < 0) mk += MBIG;
            mj = seedArray[ii];
        }
        for (int k = 1; k < 5; ++k)
            for (int i = 1; i < 56; ++i) {
                seedArray[i] -= seedArray[1 + (i + 30) % 55];
                if (seedArray[i] < 0) seedArray[i] += MBIG;
            }
    }
    double nextDouble()
    {
        if (++inext >= 56) inext = 1;
        if (++inextp >= 56) inextp = 1;
        int r = seedArray[inext] - seedArray[inextp];
        if (r == 2147483647) --r;
        if (r < 0) r += 2147483647;
        seedArray[inext] = r;
        return r * (1.0 / 2147483647);
    }
};

// Coverage (0..1, 4 x 4 samples a pixel, even-odd) of a polygon, added x weight into a w x h plane.
void addPolygon(std::vector<float>& plane, int w, int h, const std::vector<std::array<double, 2>>& pts, float weight)
{
    if (pts.size() < 3) return;
    double ymin = 1e300, ymax = -1e300;
    for (const auto& p : pts) { ymin = std::min(ymin, p[1]); ymax = std::max(ymax, p[1]); }
    const int ss = 4;
    const int r0 = std::max(0, (int)std::floor(ymin * ss)), r1 = std::min(h * ss - 1, (int)std::ceil(ymax * ss));
    std::vector<double> xs;
    std::vector<float> row((size_t)w);
    for (int py = r0 / ss; py <= r1 / ss && py < h; ++py) {
        std::fill(row.begin(), row.end(), 0.f);
        bool any = false;
        for (int sr = 0; sr < ss; ++sr) {
            const double sy = py + (sr + 0.5) / ss;
            xs.clear();
            const size_t n = pts.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& a = pts[i];
                const auto& b = pts[(i + 1) % n];
                if ((a[1] > sy) != (b[1] > sy)) xs.push_back(a[0] + (sy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                const int k0 = std::max(0, (int)std::ceil(xs[i] * ss - 0.5)), k1 = std::min(w * ss - 1, (int)std::ceil(xs[i + 1] * ss - 0.5) - 1);
                for (int k = k0; k <= k1; ++k) { row[(size_t)(k / ss)] += 1.f; any = true; }
            }
        }
        if (any)
            for (int x = 0; x < w; ++x) plane[(size_t)py * w + x] += row[(size_t)x] / (ss * ss) * weight;
    }
}

// StarShape.Load's outline in unit coordinates (centre at 0.5): outer points at radius 0.499, notches at 0.499 x inner, Variance moving each
// outer point but the first by a System.Random draw.
std::vector<std::array<double, 2>> starOutline(int n, double inner, double variance, int seed)
{
    std::vector<std::array<double, 2>> out;
    if (n < 2) n = 2;
    auto rot = [](double x, double y, double deg, double& ox, double& oy) {
        const double a = deg / 180.0 * kPiV;
        const float sn = (float)std::sin(a), cs = (float)std::cos(a);
        ox = (double)((float)(cs * x) - (float)(sn * y));
        oy = (double)((float)(sn * x) + (float)(cs * y));
    };
    const double v1y = -0.49900001287460327;
    double v2x, v2y;
    rot(0.0, (double)(float)(-0.49900001287460327f * (float)inner), (double)(180.0f / (float)n), v2x, v2y);
    out.push_back({0.5, 0.5 + v1y});
    out.push_back({0.5 + v2x, 0.5 + v2y});
    DotNetRandom rnd(seed);
    for (int i = 0; i < n - 1; ++i) {
        const double d = (double)((float)(i + 1) * 360.0f / (float)n);
        double py = v1y;
        if (variance > 0) py = (double)(float)(v1y + variance * ((rnd.nextDouble() - 0.5) * 2.0) * v1y);
        double ax, ay, bx, by;
        rot(0.0, py, d, ax, ay);
        rot(v2x, v2y, d, bx, by);
        out.push_back({0.5 + ax, 0.5 + ay});
        out.push_back({0.5 + bx, 0.5 + by});
    }
    return out;
}

}  // namespace

// ================================================================== Rays
void raysVelvet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    static const char* const kRaysMasks[4] = {"Wipe", "Rectangle", "Bar", "Ellipse"};
    const int maskMode = c.i("MaskMaskMode");
    std::vector<float> mask;
    if (maskMode >= 1 && maskMode <= 3) maskCoverage(c, c.i("MaskMaskType"), kRaysMasks, mask, W, H);
    if (maskMode == 3) { maskOnly(c, mask); return; }
    Buf srcCopy;
    const Buf* src = &c.src;
    if (maskMode == 2) { srcCopy = c.src; multiplyBy(srcCopy, mask); src = &srcCopy; }
    const bool alpha = c.b("EffectKeyAlphaChannel"), effectOnly = c.b("EffectEffectOnly");
    const double threshold = std::min(c.d("EffectThreshold") / 100.0, 0.996078431372549);
    const double amount = c.d("EffectAmount") / 100.0, aspect = c.d("EffectAspect"), boost = c.d("EffectBoost") / 100.0;
    const double peak = c.d("EffectPeak") / 100.0, noise = c.d("EffectNoise") / 100.0, quality = clampd(c.d("EffectQuality"), 0.0, 1.0);
    double px, py;
    c.xy("EffectPosition", px, py);
    const int steps = 20;                                                   // the managed effect's Steps (not an OpenFX setting)

    Gradient tg;
    const double lo = std::max(threshold - 0.05, 0.0);
    if (lo < threshold) tg.add(lo, 0, 0, 0, 0);
    const float v8 = (float)(255.0 * (threshold + boost)), v9 = (float)(255.0 * (1.0 + boost + peak));
    tg.add(threshold, v8, v8, v8, v8);
    tg.add(1.0, v9, v9, v9, v9);
    float tlut[256];
    for (int i = 0; i < 256; ++i) tlut[i] = tg.at(i / 255.0)[3];

    std::vector<int> g((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = src->at(x, y);
            int v = alpha ? alphaByte(p) : lumaByte(p);
            if (maskMode == 1) v = (int)std::floor(v * mask[(size_t)y * W + x] + 0.5);
            g[(size_t)y * W + x] = v;
        }
    if (c.b("EffectReduceFlicker")) blur3x3(g, W, H);
    Plane m;
    m.resize(W, H);
    for (size_t i = 0; i < g.size(); ++i) m.v[i] = tlut[std::max(0, std::min(255, g[i]))];

    if (noise > 0) {
        const double nlo = 1 - noise * 2, nhi = 1 + noise * 2;
        const std::vector<float>& tex = noiseTexture((float)nlo, (float)nhi);
        Plane np;
        np.w = np.h = 512;
        np.v = tex;
        const double sc = std::max(W / 512.0, H / 512.0);
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) m.at(x, y) *= np.sample((x + 0.5) / sc - 0.5, (y + 0.5) / sc - 0.5);
        });
    }

    if (amount > 0) {
        const float cx = (float)(px * W), cy = (float)((1 - py) * H);
        const double k = 1.45 + 0.25 * (1.0 - quality);
        const double L = std::max(W, H) * amount * 4.0 * std::sqrt(2.0);
        const int n = L > 1 ? (int)std::lround(std::log(L) / std::log(k)) : 0;
        double s = amount * 4.0 / std::pow(k, n);
        const int passes = std::min(n, steps);
        Plane prev;
        for (int i = 0; i < passes; ++i) {
            prev = m;
            const double sx = (float)(1.0 + s * aspect), sy = (float)(1.0 + s);
            if (sx > 1e-6 && sy > 1e-6)
                forRows(H, W, [&](int y) {
                    const double qy = cy + (y + 0.5 - cy) / sy - 0.5;
                    for (int x = 0; x < W; ++x) {
                        const double qx = cx + (x + 0.5 - cx) / sx - 0.5;
                        const float z = prev.sample(qx, qy);
                        float& d = m.at(x, y);
                        d = d + (z - d) * 0.35f;
                    }
                });
            s *= k;
        }
    }

    uint8_t clut[256][4];
    const double hue = c.d("EffectHue"), sweep = c.d("EffectHueSweep"), sat = c.d("EffectSaturation") / 100.0;
    colourLut(hue, sweep, sat, 1.1, 0.25, hue + sweep, clut);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s0 = c.src.at(x, y);
            float* o = c.out.at(x, y);
            if (effectOnly) o[0] = o[1] = o[2] = o[3] = 0.f;
            else for (int q = 0; q < 4; ++q) o[q] = (maskMode == 2 ? src->at(x, y)[q] : s0[q]);
            const int mb = (int)clampd(std::floor(m.at(x, y) + 0.5f), 0.0, 255.0);
            screenOver(o, clut[mb]);
        }
    });
}

// ================================================================== Glint
void glintVelvet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    static const char* const kGlintMasks[4] = {"Rectangle", "Bar", "Wipe", "Ellipse"};
    const int maskMode = c.i("MaskMaskMode");
    std::vector<float> mask;
    if (maskMode >= 1 && maskMode <= 3) maskCoverage(c, c.i("MaskMaskType"), kGlintMasks, mask, W, H);
    if (maskMode == 3) { maskOnly(c, mask); return; }
    Buf srcCopy;
    const Buf* src = &c.src;
    if (maskMode == 2) { srcCopy = c.src; multiplyBy(srcCopy, mask); src = &srcCopy; }
    const bool effectOnly = c.b("EffectEffectOnly");
    const double threshold = c.d("EffectThreshold") / 100.0, boost = c.d("EffectBoost") / 100.0;
    const double dx = c.d("EffectHorizontalRadius") / 100.0, dy = c.d("EffectVerticalRadius") / 100.0;
    const double hue = c.d("EffectHue"), sweep = c.d("EffectHueSweep"), sat = c.d("EffectSaturation") / 100.0;
    const double rotation = c.d("EffectOrientation");
    const int n = std::max(0, std::min(64, c.i("EffectStreaks")));

    Gradient tg;
    tg.add(threshold - 0.05, 0, 0, 0, 0);
    tg.add(threshold, 1, 1, 1, 1);
    int tlut[256];
    for (int i = 0; i < 256; ++i) tlut[i] = (int)clampd((double)tg.at(i / 255.0)[3] * 255.0 + 0.5, 0.0, 255.0);
    std::vector<int> g((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int v = lumaByte(src->at(x, y));
            if (maskMode == 1) v = (int)std::floor(v * mask[(size_t)y * W + x] + 0.5);
            g[(size_t)y * W + x] = v;
        }
    if (c.b("EffectReduceFlicker")) blur3x3(g, W, H);

    const double bs = boost > 0 ? boost * 2.0 : boost;
    const float scale = (float)(1.0 + bs);
    uint8_t clut[256][4];
    double v16 = 0.3;
    if (boost > 0) v16 -= v16 * boost / 2.0;
    colourLut(hue, sweep, sat, 0.9, 0.15, hue + sweep * (1.0 - v16), clut);

    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            const float* s0 = src->at(x, y);
            if (effectOnly) o[0] = o[1] = o[2] = o[3] = 0.f;
            else for (int q = 0; q < 4; ++q) o[q] = s0[q];
        }
    });
    if (n <= 0) return;
    std::vector<std::vector<uint8_t>> streaks((size_t)n);
    auto one = [&](int i) {
        const double ang = i * 180.0 / n;
        const double a = ang * kPiV / 180.0;
        const double rx = -std::sin(a) * dy, ry = std::cos(a) * dy;
        const double sxf = dy > 0 ? dx / dy : 0.0;
        const double blur = std::hypot(rx * sxf, ry);
        std::vector<float> p((size_t)W * H);
        for (size_t k = 0; k < p.size(); ++k) p[k] = (float)tlut[std::max(0, std::min(255, g[k]))];
        directionalBlur(p, W, H, ang + rotation, blur * W);
        std::vector<uint8_t>& out = streaks[(size_t)i];
        out.resize(p.size());
        for (size_t k = 0; k < p.size(); ++k) out[k] = (uint8_t)clampd(std::floor(p[k] * scale + 0.5f), 0.0, 255.0);
    };
    for (int i = 0; i < n; ++i) one(i);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            for (int i = 0; i < n; ++i) screenOver(o, clut[streaks[(size_t)i][(size_t)y * W + x]]);
        }
    });
}


// ================================================================== Fill Light
// FilterLibrary.FillLight: the luma (Desaturate) through a lookup table - a Bezier curve that is 0 at black, 0.3 at 0.25 + 0.08 bias and 0 from 0.85, as grey coloured by Colorize(hue, 0.1 x correction),
// sampled at 128 points into a Gradient, read as 256 bytes - softened by GaussianBlur(softness) and ADDED at strength x 0.5 (bytes, sum truncated). Hue in degrees (class turns x 360), Strength / Bias / Tint
// (= Correction) in percent, Softness = class value x 500 (fraction of the width). The native blur's deviation, read off VEGAS's pictures: 0.45 + 0.3016 x the diameter in pixels, cut at 3 deviations.
void fillLightVelvet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    const double strength = c.d("Strength") / 100.0, bias = c.d("Bias") / 100.0, correction = c.d("Tint") / 100.0;
    const double softness = c.d("Softness") / 500.0, hue = c.d("Hue") / 360.0;
    c.out.px = c.src.px;
    if (!(strength > 0)) return;
    std::vector<BezierPoint> curve = {{0.0, 0.0, 0.0, 0.0}, {0.25 + bias * 0.08, 0.3, 0.0, 0.0}, {0.85, 0.0, 0.0, 0.0}};
    std::sort(curve.begin(), curve.end(), [](const BezierPoint& a, const BezierPoint& b) { return a.key < b.key; });
    ColorMatrix cm;
    cm.colorize((float)hue, (float)(0.1 * correction));
    Gradient g;
    for (int i = 0; i < 128; ++i) {
        const double x = i / 127.0;
        const float v = (float)bezierCurve(curve, x);
        float o[4];
        cm.transform(v, v, v, o);
        g.add(x, o[0], o[1], o[2], o[3]);
    }
    uint8_t lut[256][4];
    for (int i = 0; i < 256; ++i) {
        const std::array<float, 4> col = g.at(i / 255.0);
        for (int k = 0; k < 4; ++k) lut[i][k] = (uint8_t)clampd((double)col[(size_t)k] * 255.0 + 0.5, 0.0, 255.0);
    }
    std::vector<float> mask((size_t)W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = c.src.at(x, y);
            const int r = byteOf(p[0]), gg = byteOf(p[1]), b = byteOf(p[2]);
            const int l = (int)clampd(std::floor(0.299 * r + 0.587 * gg + 0.114 * b + 0.5), 0.0, 255.0);
            float* m = &mask[((size_t)y * W + x) * 3];
            m[0] = lut[l][1]; m[1] = lut[l][2]; m[2] = lut[l][3];
        }
    const double diameter = softness * W;
    if (diameter > 0) {
        firGaussian(mask, W, H, 3, 0.45 + 0.3016 * diameter);
        for (float& v : mask) v = (float)clampd(std::floor(v + 0.5), 0.0, 255.0);
    }
    const double op = clampd(strength * 0.5, 0.0, 1.0);
    if (op < 0.00392156862745098) return;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s0 = c.src.at(x, y);
            float* o = c.out.at(x, y);
            const int ab = byteOf(s0[3]);
            const float* m = &mask[((size_t)y * W + x) * 3];
            for (int k = 0; k < 3; ++k) {
                const double v = std::floor(byteOf(s0[k]) + m[k] * op);
                o[k] = (float)(std::min(v, (double)ab) / 255.0);
            }
        }
    });
}


// ================================================================== Starburst
// FilterLibrary.Starburst: the picture (masked by a soft rounded rectangle with Soften edges, 3 x 3 blurred with Reduce flicker) has what is above the threshold scaled by the brilliance, convolved (FFTImage, tile by
// tile) with a star of radius Radius x the larger side: Points thin spikes of grey 128 narrowing to nothing from 0.75 pixels at the middle, turned by Orientation (turns), squeezed by Aspect; Center burst adds two denser
// bursts (8 and 27 uneven spikes at 0.2 and 0.4 of the size, System.Random lengths) with Add. The result, cut to 0..1, is screened over the picture at Strength. The native half (threshold, FFT scaling, star drawing) is
// written from VEGAS's pictures, which are not repeatable (its tiles read uncleared memory; two runs differ by 16 levels on average), so tiles here are padded with nothing.
void starburstVelvet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    const double threshold = c.d("Threshold") / 100.0, brilliance = c.d("Brilliance") / 100.0, strength = c.d("Strength") / 100.0;
    const double radius = c.d("Radius") / 100.0, centerburst = c.d("CenterBurst") / 100.0, aspect = std::max(0.01, c.d("Aspect"));
    const double orientation = c.d("Orientation") / 360.0, softedges = c.d("SoftenEdges") / 100.0;
    const int points = (int)(std::max(2.0, c.d("Points")) + 0.5);
    const bool effectOnly = c.b("EffectOnly");
    // the star's weight in the convolution: read off VEGAS's pictures (FFT scaling is native)
    const double gainK = 0.15;
    if (effectOnly) std::fill(c.out.px.begin(), c.out.px.end(), 0.f);
    else c.out.px = c.src.px;
    if (!(strength >= 0.00392156862745098)) return;
    std::vector<float> img((size_t)W * H * 4);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (float)byteOf(c.src.px[i]);
    if (softedges > 0) {
        const double v2 = 1.0 - 0.1 * softedges;
        double mw, mh;
        if (W > H) { mw = v2; mh = 1.0 - W * (1.0 - v2) / H; }
        else { mh = v2; mw = 1.0 - H * (1.0 - v2) / W; }
        const double blur = 0.5 * softedges, corner = 0.2 * softedges;
        const int CW = (int)(W * 1.25), CH = (int)(H * 1.25);
        std::vector<float> cv((size_t)CW * CH, 0.f);
        const double cx = CW / 2.0, cy = CH / 2.0, hw = mw * W / 2, hh = mh * H / 2;
        const double rr = std::min(std::min(hw, hh), corner * H / 3.0);
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                int in = 0;
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i) {
                        const double ax = std::fabs(x + (i + 0.5) / 4 - cx), ay = std::fabs(y + (j + 0.5) / 4 - cy);
                        if (ax > hw || ay > hh) continue;
                        const double dx = ax - (hw - rr), dy = ay - (hh - rr);
                        in += (dx <= 0 || dy <= 0 || dx * dx + dy * dy <= rr * rr);
                    }
                cv[(size_t)y * CW + x] = in / 16.f;
            }
        const double sigma = sigmaForDiameter(blur * CW);
        std::vector<double> tmp;
        for (int y = 0; y < CH; ++y) lineBlur(&cv[(size_t)y * CW], CW, 1, sigma, tmp);
        for (int x = 0; x < CW; ++x) lineBlur(&cv[(size_t)x], CH, CW, sigma, tmp);
        const int ox = (CW - W) / 2, oy = (CH - H) / 2;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double m = clampd(cv[(size_t)(y + oy) * CW + x + ox], 0.0, 1.0);
                for (int k = 0; k < 4; ++k) {
                    float& v = img[((size_t)y * W + x) * 4 + k];
                    v = (float)std::floor(v * m + 0.5);
                }
            }
    }
    if (c.b("ReduceFlicker"))
        for (int k = 0; k < 4; ++k) {
            std::vector<int> g((size_t)W * H);
            for (size_t i = 0; i < g.size(); ++i) g[i] = (int)img[i * 4 + k];
            blur3x3(g, W, H);
            for (size_t i = 0; i < g.size(); ++i) img[i * 4 + k] = (float)g[i];
        }
    for (float& v : img) v = (float)(std::max(0.0, v / 255.0 - threshold) * brilliance);
    const double rpx = radius * std::max((double)W, (double)H);
    const int R = (int)std::ceil(rpx * std::max(1.0, std::max(1.0 / aspect, aspect))) + 2;
    int P = 8, Q = 8;
    while (P < W + 2 * R + 2) P <<= 1;
    while (Q < H + 2 * R + 2) Q <<= 1;
    if ((size_t)P * Q > (size_t)64 * 1024 * 1024) return;
    double sx = 1.0, sy = 1.0;
    if (aspect < 1) sx = aspect; else sy = 1.0 / aspect;
    const double rot = orientation * 360.0 / 180.0 * kPiV;
    const double cr = std::cos(rot), sr = std::sin(rot);
    auto toKernel = [&](const std::vector<std::array<double, 2>>& unit, double scale) {
        std::vector<std::array<double, 2>> o;
        for (const auto& p : unit) {
            const double x = (p[0] - 0.5) * 2 * rpx * sx * scale, y = (p[1] - 0.5) * 2 * rpx * sy * scale;
            o.push_back({x * cr - y * sr + R, x * sr + y * cr + R});
        }
        return o;
    };
    const int KS = 2 * R + 1;
    std::vector<float> kern((size_t)KS * KS, 0.f);
    addPolygon(kern, KS, KS, toKernel(starOutline(points, 0.75 / rpx, 0.0, 0), 1.0), 128.f / 255.f);
    if (centerburst > 0) {
        static const double table[2][5] = {{0.2, 0.3, 0.25, 43341.0, 8.0}, {0.4, 0.75, 0.2, 12345.0, 27.0}};
        for (int b = 0; b < 2; ++b) {
            const double sc = table[b][0];
            std::vector<float> one((size_t)KS * KS, 0.f);
            addPolygon(one, KS, KS, toKernel(starOutline((int)std::lround(table[b][4]), 0.75 / rpx / sc * 2.0, table[b][1], (int)std::lround(table[b][3])), sc), 1.f);
            const float op = (float)clampd(table[b][2] * centerburst, 0.0, 1.0);
            for (size_t i = 0; i < kern.size(); ++i) kern[i] = std::min(1.f, kern[i] + one[i] * op);
        }
    }
    std::vector<std::complex<double>> K((size_t)P * Q), A((size_t)P * Q), B((size_t)P * Q);
    for (int y = 0; y < KS; ++y)
        for (int x = 0; x < KS; ++x) {
            const int ky = ((y - R) % Q + Q) % Q, kx = ((x - R) % P + P) % P;
            K[(size_t)ky * P + kx] += kern[(size_t)y * KS + x] * gainK;
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* v = &img[((size_t)y * W + x) * 4];
            A[(size_t)y * P + x] = std::complex<double>(v[0], v[1]);
            B[(size_t)y * P + x] = std::complex<double>(v[2], v[3]);
        }
    fft2(K, P, Q, false);
    fft2(A, P, Q, false);
    fft2(B, P, Q, false);
    for (size_t i = 0; i < K.size(); ++i) { A[i] *= K[i]; B[i] *= K[i]; }
    fft2(A, P, Q, true);
    fft2(B, P, Q, true);
    const double norm = 1.0 / ((double)P * Q);
    const double op = clampd(strength, 0.0, 1.0);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const std::complex<double> a = A[(size_t)y * P + x] * norm, b = B[(size_t)y * P + x] * norm;
            const double cv[4] = {a.real(), a.imag(), b.real(), b.imag()};
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) {
                const double s = std::floor(clampd(cv[k], 0.0, 1.0) * 255.0 + 0.5) / 255.0 * op;
                o[k] = (float)(o[k] + s - o[k] * s);
            }
        }
    });
}

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
