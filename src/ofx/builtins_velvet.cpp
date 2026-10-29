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
#include "../core/base.h"
#include "builtins_vec.h"
#include "builtins_cgblur.h"
#include "vkfilters.h"
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
void cgGaussianBlurPlane(float* px, int w, int h, double sigmaX, double sigmaY);  // builtins_cgblur.cpp
namespace more {
namespace {

void lineBlurAll(float* v, int count, int n, ptrdiff_t stride, ptrdiff_t lineStride, double sigma);

constexpr double kPiV = 3.14159265358979323846;
struct LineBlurCoefs { double c1, c2, c3, B; };
LineBlurCoefs lineBlurCoefs(double q);

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
        // XmlColorFloat arithmetic: c0 + (c1 - c0) * frac, every operator stores a float
        for (int k = 0; k < 4; ++k) {
            const float diff = w[(size_t)k] - v[(size_t)k];
            const float prod = (float)((double)diff * frac);
            v[(size_t)k] = v[(size_t)k] + prod;
        }
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
inline int byteOf(float v);
inline int lumaInt(int r, int g, int b) { return (19595 * r + 38469 * g + 7472 * b + 32768) >> 16; }
inline int lumaByte(const float* p) { return lumaInt(byteOf(p[0]), byteOf(p[1]), byteOf(p[2])); }
inline int alphaByte(const float* p) { return (int)clampd(std::floor(255.0 * p[3] + 0.5), 0.0, 255.0); }

inline int byteOf(float v) { return (int)clampd(std::floor((double)(v * 255.f) + 0.5), 0.0, 255.0); }

// Screen of a premultiplied byte colour over a premultiplied pixel (Composite, Screen), as CompositeNative does it on bytes: colour a + b - a x b / 255 (truncated), alpha a + b - (a x b + 127) / 255.
inline void screenOver(float* o, const uint8_t c[4])
{
    const int d[4] = {byteOf(o[0]), byteOf(o[1]), byteOf(o[2]), byteOf(o[3])};
    const int s[4] = {c[1], c[2], c[3], c[0]};
    for (int k = 0; k < 3; ++k) o[k] = (float)(d[k] + s[k] - d[k] * s[k] / 255) * (1.f / 255.f);
    o[3] = (float)(d[3] + s[3] - (d[3] * s[3] + 127) / 255) * (1.f / 255.f);
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

// EllipseMask / RectangleMask coverage 0..1 per pixel: Width x Height (fractions of the frame) about (xpos, ypos) (y from the top), rectangle
// corners rounded by corner x height / 3. Native drawing puts pixel centres on whole coordinates (half a pixel right and down of the plain
// mapping). With a blur the shape is drawn on a canvas 1.25 times the frame, blurred there (diameter blur x canvas width), and cropped.
} // namespace
static void shapeMaskImpl(bool cgBytes, int W, int H, double xpos, double ypos, double w, double h, double corner, double blur, bool ellipse, std::vector<float>& m)
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
    if (soft && cgBytes) {
        // RectangleMask / EllipseMask: the shape drawn white on a transparent byte canvas, then GaussianBlur's native (Young - van Vliet on bytes)
        std::vector<float> px((size_t)CW * CH * 4);
        for (size_t i = 0; i < (size_t)CW * CH; ++i) {
            const float b8 = (float)(int)clampd(std::floor((double)cv[i] * 255.0 + 0.5), 0.0, 255.0);
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4 + 3] = b8;
        }
        const double sigma = sigmaForDiameter(blur * CW);
        cgGaussianBlur(px.data(), CW, CH, sigma, sigma);
        for (size_t i = 0; i < (size_t)CW * CH; ++i) cv[i] = (float)cgBlurByte(px[i * 4 + 3]) * (1.f / 255.f);
    } else if (soft) {
        const double sigma = sigmaForDiameter(blur * CW);
        lineBlurAll(cv.data(), CH, CW, 1, CW, sigma);
        lineBlurAll(cv.data(), CW, CH, CW, 1, sigma);
    }
    const int ox = (CW - W) / 2, oy = (CH - H) / 2;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) m[(size_t)y * W + x] = (float)clampd(cv[(size_t)(y + oy) * CW + x + ox], 0.0, 1.0);
}

void shapeMaskVelvet(int W, int H, double xpos, double ypos, double w, double h, double corner, double blur, bool ellipse, std::vector<float>& m)
{
    shapeMaskImpl(false, W, H, xpos, ypos, w, h, corner, blur, ellipse, m);
}

static void shapeMaskCgBytes(int W, int H, double xpos, double ypos, double w, double h, double corner, double blur, bool ellipse, std::vector<float>& m)
{
    shapeMaskImpl(true, W, H, xpos, ypos, w, h, corner, blur, ellipse, m);
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
// A third-order recursive blur along lines, eight at once (line l starts at v + l * lineStride, its values 'stride' apart), each line's sums in the same order, so the same results;
// lines are independent, so the recursion's waiting is shared out between them.
LineBlurCoefs lineBlurCoefs(double q)
{
    const double b0 = 1.57825 + 2.44413 * q + 1.4281 * q * q + 0.422205 * q * q * q;
    const double b1 = 2.44413 * q + 2.85619 * q * q + 1.26661 * q * q * q;
    const double b2 = -(1.4281 * q * q + 1.26661 * q * q * q);
    const double b3 = 0.422205 * q * q * q;
    LineBlurCoefs r;
    r.c1 = b1 / b0;
    r.c2 = b2 / b0;
    r.c3 = b3 / b0;
    r.B = 1 - (r.c1 + r.c2 + r.c3);
    return r;
}

void lineBlurLanes(float* v, int n, ptrdiff_t stride, ptrdiff_t lineStride, int lines, double sigma, std::vector<double>& tmp)
{
    if (n <= 1 || !(sigma > 0.05)) return;
    const LineBlurCoefs lc = lineBlurCoefs(sigma);
    const double c1 = lc.c1, c2 = lc.c2, c3 = lc.c3, B = lc.B;
    tmp.resize((size_t)n * 8);
    const d2::D2 kB = d2::set(B), k1 = d2::set(c1), k2 = d2::set(c2), k3 = d2::set(c3);
    d2::D2 p1[4], p2[4], p3[4];
    double x[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int l = 0; l < 8; ++l) x[l] = l < lines ? (double)v[(ptrdiff_t)l * lineStride] : 0.0;
    for (int g = 0; g < 4; ++g) p1[g] = p2[g] = p3[g] = d2::ld(x + 2 * g);
    for (int i = 0; i < n; ++i) {
        for (int l = 0; l < lines; ++l) x[l] = (double)v[(ptrdiff_t)l * lineStride + i * stride];
        for (int g = 0; g < 4; ++g) {
            const d2::D2 w = d2::add(d2::add(d2::add(d2::mul(kB, d2::ld(x + 2 * g)), d2::mul(k1, p1[g])), d2::mul(k2, p2[g])), d2::mul(k3, p3[g]));
            d2::st(&tmp[(size_t)i * 8 + 2 * g], w);
            p3[g] = p2[g]; p2[g] = p1[g]; p1[g] = w;
        }
    }
    for (int g = 0; g < 4; ++g) p1[g] = p2[g] = p3[g] = d2::ld(&tmp[(size_t)(n - 1) * 8 + 2 * g]);
    for (int i = n - 1; i >= 0; --i) {
        double o[8];
        for (int g = 0; g < 4; ++g) {
            const d2::D2 w = d2::add(d2::add(d2::add(d2::mul(kB, d2::ld(&tmp[(size_t)i * 8 + 2 * g])), d2::mul(k1, p1[g])), d2::mul(k2, p2[g])), d2::mul(k3, p3[g]));
            d2::st(o + 2 * g, w);
            p3[g] = p2[g]; p2[g] = p1[g]; p1[g] = w;
        }
        for (int l = 0; l < lines; ++l) v[(ptrdiff_t)l * lineStride + i * stride] = (float)o[l];
    }
}

// lineBlur of 'count' lines in parallel: line k starts at v + k * lineStride.
void lineBlurAll(float* v, int count, int n, ptrdiff_t stride, ptrdiff_t lineStride, double sigma)
{
    if (n <= 1 || !(sigma > 0.05)) return;
    forRows((count + 7) / 8, n * 64, [&](int gi) {
        std::vector<double> tmp;
        const int first = gi * 8;
        lineBlurLanes(v + (ptrdiff_t)first * lineStride, n, stride, lineStride, std::min(8, count - first), sigma, tmp);
    });
}

// VEGAS's DirectionalBlur on one grey plane of bytes (top row first), as FilterLibrary.DirectionalBlur does it with CoreGraphics.Native: the angle is brought to -90..90 and the
// picture (its rows in Reno's order: the angle's sense matches VEGAS's output so) is sheared (ShearNative: every line moves by |line x t| pixels, 12 bit fixed-point weights of the two neighbours, +0x800 >> 12, edge
// pixels repeated; columns for the 0th and 3rd 45 degree sector, rows for the others), blurred across the shear by the Gaussian (always both directions, the other at least 0.5)
// with bytes in between, and sheared back by -t. Exactly an axis, only the blur.
// The per-line numbers of a shear (line k moves by |k t| pixels: whole pixels and 12 bit weight), shared with the GPU plan
inline void shearLineOf(int k, double t, int& ip, int& fr)
{
    const double off = std::fabs((double)k * t);
    ip = (int)std::floor(off);
    fr = (int)((off - (double)ip) * 4096.0);
}

void shearLines(const uint8_t* src, int lines, int len, ptrdiff_t lineStep, ptrdiff_t posStep, uint8_t* dst, int dlen, ptrdiff_t dLineStep, ptrdiff_t dPosStep, double t, bool rev)
{
    const bool side1 = !(t >= 0);
    forRows(lines, (size_t)dlen * 4, [&](int k) {
        int ip, fr;
        shearLineOf(k, t, ip, fr);
        const uint8_t* s = src + (ptrdiff_t)k * lineStep;
        uint8_t* d = dst + (ptrdiff_t)k * dLineStep;
        auto at = [&](int i) -> int {
            i = i < 0 ? 0 : (i > len - 1 ? len - 1 : i);
            return s[(ptrdiff_t)(rev ? len - 1 - i : i) * posStep];
        };
        for (int x = 0; x < dlen; ++x) {
            const int xr = rev ? dlen - 1 - x : x;
            int a, b;
            if (!side1) { a = xr - ip; b = a - 1; }
            else { a = xr + ip; b = a + 1; }
            d[(ptrdiff_t)x * dPosStep] = (uint8_t)((at(a) * (4096 - fr) + at(b) * fr + 2048) >> 12);
        }
    });
}

// What directionalBlur() works out from the angle and the size: mode 0 nothing at all, 1 / 2 the plane blurred in place (1: rows by the minimal blur, columns by sigma; 2 the other
// way round), 3 sheared by t (columns when cols), blurred across the shear and sheared back.
void directionalGeom(int W, int H, double angleDeg, double diameter, DbGeom& o)
{
    o = DbGeom();
    double a = std::fmod(angleDeg, 360.0);
    if (a < 0) a += 360.0;
    if (a > 90) a = a < 270 ? a - 180 : a - 360;
    a = clampd(a, -90, 90);
    o.sigma = sigmaForDiameter(diameter);
    if (!(o.sigma > 0) || W <= 0 || H <= 0) return;
    if (std::fabs(a) <= 0.1) { o.mode = 1; return; }
    if (std::fabs(std::fabs(a) - 90) <= 0.1) { o.mode = 2; return; }
    const int q = std::max(0, std::min(3, (int)(a + 90) / 45));
    o.cols = q == 0 || q == 3;
    double tn;
    if (q == 0) tn = std::tan((a + 90.0) * kPiV / 180.0);
    else if (q == 1) tn = std::tan(-a * kPiV / 180.0);
    else if (q == 2) tn = std::tan(a * kPiV / 180.0);
    else tn = std::tan((90.0 - a) * kPiV / 180.0);
    const double dim = o.cols ? (double)W : (double)H;
    o.t = (tn * dim) / dim;
    o.rev = q <= 1;
    o.SW = o.cols ? W : (int)std::ceil((double)W + tn * H);
    o.SH = o.cols ? (int)std::ceil((double)H + tn * W) : H;
    if ((size_t)o.SW * (size_t)o.SH > (size_t)400000000) return;
    o.mode = 3;
}

void directionalBlur(std::vector<uint8_t>& g, int W, int H, double angleDeg, double diameter, float scale, bool blurIt)
{
    DbGeom d;
    directionalGeom(W, H, angleDeg, diameter, d);
    if (d.mode == 0) return;
    const double sigma = d.sigma;
    std::vector<uint8_t>& nat = g;
    auto blurBytes = [&](std::vector<uint8_t>& p, int w, int h, double sx, double sy) {
        if (!blurIt) return;
        std::vector<float> f(p.begin(), p.end());
        cgGaussianBlurPlane(f.data(), w, h, sx, sy);
        for (size_t i = 0; i < f.size(); ++i) p[i] = (uint8_t)cgBlurByte(f[i] * scale);
    };
    if (d.mode == 1) blurBytes(nat, W, H, 0.0, sigma);
    else if (d.mode == 2) blurBytes(nat, W, H, sigma, 0.0);
    else {
        const bool cols = d.cols, rev = d.rev;
        const double t = d.t;
        const int SW = d.SW, SH = d.SH;
        std::vector<uint8_t> sh((size_t)SW * SH);
        if (cols) shearLines(nat.data(), W, H, 1, W, sh.data(), SH, 1, SW, t, rev);
        else shearLines(nat.data(), H, W, W, 1, sh.data(), SW, SW, 1, t, rev);
        if (cols) blurBytes(sh, SW, SH, sigma, 0.0);
        else blurBytes(sh, SW, SH, 0.0, sigma);
        if (cols) shearLines(sh.data(), W, SH, 1, SW, nat.data(), H, 1, W, -t, rev);
        else shearLines(sh.data(), H, SW, SW, 1, nat.data(), W, W, 1, -t, rev);
    }
}

// Rays' noise texture: CoreGraphics.Native GenerateNoiseMask (512 x 512, seed 12335), read off its machine code. A table of 1024 floats lo + i / 1023 x (hi - lo) is indexed per
// pixel, in rows, by bits 16..25 of the MSVC rand state (state = state x 214013 + 2531011, kept as a signed 32-bit number).
const std::vector<float>& noiseTexture(double lo, double hi)
{
    static thread_local std::vector<float> tex;
    static thread_local double tlo = -1, thi = -1;
    if (tex.empty() || tlo != lo || thi != hi) {
        tex.resize(512 * 512);
        const float flo = (float)lo, span = (float)hi - (float)lo;
        float table[1024];
        for (int i = 0; i < 1024; ++i) table[i] = (float)i / 1023.f * span + flo;
        uint32_t x = 12335u;
        for (float& v : tex) {
            x = x * 214013u + 2531011u;
            v = table[(int32_t)x >> 16 & 0x3ff];
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



// ------------------------------------------------------------------ Starburst's pieces
// The twiddle factors of an FFT of n (a power of two): for each stage (len 2, 4 .. n) len / 2 values, w[k + 1] = w[k] * wl as the butterflies take them.
struct FftTwiddles {
    std::vector<double> w;                   // re, im pairs, the stages one after the other
    FftTwiddles(int n, bool inverse)
    {
        for (int len = 2; len <= n; len <<= 1) {
            const double ang = 2 * kPiV / len * (inverse ? 1 : -1);
            const double lr = std::cos(ang), li = std::sin(ang);
            double wr = 1, wi = 0;
            for (int k = 0; k < len / 2; ++k) {
                w.push_back(wr);
                w.push_back(wi);
                const double nr = wr * lr - wi * li, ni = wr * li + wi * lr;
                wr = nr;
                wi = ni;
            }
        }
    }
};

// An in-place radix-2 FFT of n (a power of two) complex values; inverse unscaled.
void fft1(std::complex<double>* a, int n, const FftTwiddles& tw)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    double* d = reinterpret_cast<double*>(a);
    const double* stage = tw.w.data();
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2;
        for (int i = 0; i < n; i += len) {
            double* u = d + (size_t)i * 2;
            double* v = u + (size_t)half * 2;
            for (int k = 0; k < half; ++k) {
                const double wr = stage[2 * k], wi = stage[2 * k + 1];
                const double tr = v[2 * k] * wr - v[2 * k + 1] * wi, ti = v[2 * k] * wi + v[2 * k + 1] * wr;
                const double ur = u[2 * k], ui = u[2 * k + 1];
                v[2 * k] = ur - tr;
                v[2 * k + 1] = ui - ti;
                u[2 * k] = ur + tr;
                u[2 * k + 1] = ui + ti;
            }
        }
        stage += (size_t)half * 2;
    }
}
// The 2-D transform, rows then columns. Rows 'zeroRow' marks hold only zeros (their transform is zero: left as they are); only the first 'cols' columns are wanted.
void fft2(std::vector<std::complex<double>>& a, int w, int h, bool inverse, const std::vector<char>* zeroRow, int cols)
{
    const FftTwiddles tw(w, inverse), th(h, inverse);
    forRows(h, w, [&](int y) {
        if (zeroRow && (*zeroRow)[(size_t)y]) return;
        fft1(&a[(size_t)y * w], w, tw);
    });
    forRows((std::min(w, cols) + 7) / 8, h, [&](int xb) {
        std::vector<std::complex<double>> col((size_t)h * 8);
        for (int y = 0; y < h; ++y)
            for (int k = 0; k < 8; ++k) col[(size_t)k * h + y] = a[(size_t)y * w + xb * 8 + k];
        for (int k = 0; k < 8; ++k) fft1(&col[(size_t)k * h], h, th);
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
// What Rays works out from the settings alone (shared by the CPU code and the GPU plan): false when the effect shows the mask (mode 3). The noise stretch and every blur pass are
// index and weight tables per axis (11 bit fixed point steps of the native scaler).
bool raysSetup(Ctx& c, int W, int H, RaysSetup& o)
{
    static const char* const kRaysMasks[4] = {"Wipe", "Rectangle", "Bar", "Ellipse"};
    o.maskMode = c.i("MaskMaskMode");
    o.mask.clear();
    if (o.maskMode >= 1 && o.maskMode <= 3) maskCoverage(c, c.i("MaskMaskType"), kRaysMasks, o.mask, W, H);
    if (o.maskMode == 3) return false;
    o.alpha = c.b("EffectKeyAlphaChannel");
    o.effectOnly = c.b("EffectEffectOnly");
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
    for (int i = 0; i < 256; ++i) o.tlut[i] = tg.at(i / 255.0)[3];
    o.flicker = c.b("EffectReduceFlicker");

    o.noise = noise > 0;
    if (o.noise) {
        const double nlo = 1 - noise * 2, nhi = 1 + noise * 2;
        o.tex = noiseTexture((float)nlo, (float)nhi);
        // ScaleMaskNative (mode 2): the 512 x 512 texture stretched over a square of side 512 x max(W/512, H/512) at the frame's corner, cut to the frame, with 11 bit fixed-point
        // steps (srcSpan - 1) / (dstSpan - 1) and bilinear weights of 11 bits; multiplied into the mask
        auto axis = [&](int dn, std::vector<int>& i0, std::vector<float>& fr) {
            const float R = 512.f * (float)std::max(W / 512.f, H / 512.f);
            const float sw = (float)dn < R ? 512.f * (float)dn / R : 512.f;
            const int step = dn > 1 ? (int)(2048.f * (sw - 1.f) / (float)(dn - 1)) : 0;
            i0.resize((size_t)dn);
            fr.resize((size_t)dn);
            for (int k = 0; k < dn; ++k) {
                const long long f = (long long)k * step;
                i0[(size_t)k] = (int)(f >> 11);
                fr[(size_t)k] = (float)(f & 2047) / 2048.f;
            }
        };
        axis(W, o.nxi, o.nxf);
        axis(H, o.nyi, o.nyf);
    }

    o.passes.clear();
    if (amount > 0) {
        // Rays_RadialBlurMask: passes of ScaleMaskNative (mode 1, weight 0.35) that blend the mask with a copy of itself zoomed by (1 + s x aspect, 1 + s) about the centre; s grows by k per pass.
        // The native scaler walks the source in 11 bit fixed point: start = -rect origin x srcSpan / rect span, step = (srcSpan - 1) / (dstSpan - 1), bilinear weights of 11 bits
        const float cx = (float)(px * W), cy = (float)((1 - py) * H);
        const double k = 1.45 + 0.25 * (1.0 - quality);
        const double L = std::max(W, H) * amount * 4.0 * std::sqrt(2.0);
        const int n = L > 1 ? (int)std::nearbyint(std::log(L) / std::log(k)) : 0;
        double s = amount * 4.0 / std::pow(k, n);
        const int passes = std::min(n, steps);
        auto axis = [](int dn, float c0, float scale, std::vector<int>& i0, std::vector<float>& fr) {
            const float r0 = c0 - scale * c0, rw = scale * (float)dn;
            const float start = r0 < 0.f ? (0.f - r0) * (float)dn / rw : 0.f;
            const float span = (float)dn * (float)dn / rw;
            const int stepv = dn > 1 ? (int)(2048.f * (span - 1.f) / (float)(dn - 1)) : 0;
            const long long st = (long long)(start * 2048.f);
            i0.resize((size_t)dn);
            fr.resize((size_t)dn);
            for (int q = 0; q < dn; ++q) {
                const long long f = st + (long long)q * stepv;
                i0[(size_t)q] = (int)(f >> 11);
                fr[(size_t)q] = (float)(f & 2047) / 2048.f;
            }
        };
        for (int i = 0; i < passes; ++i) {
            const float sx = (float)(1.0 + s * aspect), sy = (float)(1.0 + s);
            if (sx > 1e-6f && sy > 1e-6f) {
                o.passes.emplace_back();
                RaysSetup::Pass& ps = o.passes.back();
                axis(W, cx, sx, ps.xi, ps.xf);
                axis(H, cy, sy, ps.yi, ps.yf);
            }
            s *= k;
        }
    }
    const double hue = c.d("EffectHue"), sweep = c.d("EffectHueSweep"), sat = c.d("EffectSaturation") / 100.0;
    colourLut(hue, sweep, sat, 1.1, 0.25, hue + sweep, o.clut);
    return true;
}

void raysVelvet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    RaysSetup su;
    if (!raysSetup(c, W, H, su)) { maskOnly(c, su.mask); return; }
    const int maskMode = su.maskMode;
    const std::vector<float>& mask = su.mask;
    Buf srcCopy;
    const Buf* src = &c.src;
    if (maskMode == 2) { srcCopy = c.src; multiplyBy(srcCopy, mask); src = &srcCopy; }
    const bool alpha = su.alpha, effectOnly = su.effectOnly;
    const float* tlut = su.tlut;

    std::vector<int> g((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = src->at(x, y);
            int v = alpha ? alphaByte(p) : lumaByte(p);
            if (maskMode == 1) v = (int)std::floor(v * mask[(size_t)y * W + x] + 0.5);
            g[(size_t)y * W + x] = v;
        }
    if (su.flicker) blur3x3(g, W, H);
    Plane m;
    m.resize(W, H);
    for (size_t i = 0; i < g.size(); ++i) m.v[i] = tlut[std::max(0, std::min(255, g[i]))];

    if (su.noise) {
        Plane np;
        np.w = np.h = 512;
        np.v = su.tex;
        const std::vector<int>& xi = su.nxi;
        const std::vector<int>& yi = su.nyi;
        const std::vector<float>& xf = su.nxf;
        const std::vector<float>& yf = su.nyf;
        forRows(H, W, [&](int y) {
            const int y0 = std::min(511, yi[(size_t)y]), y1 = std::min(511, yi[(size_t)y] + 1);
            const float fy = yf[(size_t)y];
            const float* r0 = &np.v[(size_t)y0 * 512];
            const float* r1 = &np.v[(size_t)y1 * 512];
            for (int x = 0; x < W; ++x) {
                const int x0 = std::min(511, xi[(size_t)x]), x1 = std::min(511, xi[(size_t)x] + 1);
                const float fx = xf[(size_t)x];
                const float a = r0[x0] * (1.f - fx) + r0[x1] * fx;
                const float b = r1[x0] * (1.f - fx) + r1[x1] * fx;
                m.at(x, y) *= a * (1.f - fy) + b * fy;
            }
        });
    }

    if (!su.passes.empty()) {
        Plane next;
        next.resize(W, H);
        for (const RaysSetup::Pass& ps : su.passes) {
            const std::vector<int>& xi = ps.xi;
            const std::vector<int>& yi = ps.yi;
            const std::vector<float>& xf = ps.xf;
            const std::vector<float>& yf = ps.yf;
            forRows(H, W, [&](int y) {
                const int y0 = std::min(H - 1, yi[(size_t)y]), y1 = std::min(H - 1, yi[(size_t)y] + 1);
                const float fy = yf[(size_t)y];
                const float* r0 = &m.v[(size_t)y0 * W];
                const float* r1 = &m.v[(size_t)y1 * W];
                const float* own = &m.v[(size_t)y * W];
                float* o = &next.v[(size_t)y * W];
                for (int x = 0; x < W; ++x) {
                    const int x0 = std::min(W - 1, xi[(size_t)x]), x1 = std::min(W - 1, xi[(size_t)x] + 1);
                    const float fx = xf[(size_t)x];
                    const float a = r0[x0] * (1.f - fx) + r0[x1] * fx;
                    const float b = r1[x0] * (1.f - fx) + r1[x1] * fx;
                    const float z = a * (1.f - fy) + b * fy;
                    o[x] = own[x] * (1.f - 0.35f) + z * 0.35f;
                }
            });
            m.v.swap(next.v);
        }
    }

    const uint8_t (*clut)[4] = su.clut;
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
// What Glint works out from the settings alone (shared by the CPU code and the GPU plan): false when the effect shows the mask (mode 3)
bool glintSetup(Ctx& c, int W, int H, GlintSetup& o)
{
    static const char* const kGlintMasks[4] = {"Rectangle", "Bar", "Wipe", "Ellipse"};
    o.maskMode = c.i("MaskMaskMode");
    o.mask.clear();
    if (o.maskMode >= 1 && o.maskMode <= 3) maskCoverage(c, c.i("MaskMaskType"), kGlintMasks, o.mask, W, H);
    if (o.maskMode == 3) return false;
    o.effectOnly = c.b("EffectEffectOnly");
    const double threshold = c.d("EffectThreshold") / 100.0, boost = c.d("EffectBoost") / 100.0;
    const double dx = c.d("EffectHorizontalRadius") / 100.0, dy = c.d("EffectVerticalRadius") / 100.0;
    const double hue = c.d("EffectHue"), sweep = c.d("EffectHueSweep"), sat = c.d("EffectSaturation") / 100.0;
    const double rotation = c.d("EffectOrientation");
    o.n = std::max(0, std::min(64, c.i("EffectStreaks")));
    Gradient tg;
    tg.add(threshold - 0.05, 0, 0, 0, 0);
    tg.add(threshold, 1, 1, 1, 1);
    for (int i = 0; i < 256; ++i) o.tlut[i] = (int)clampd((double)tg.at(i / 255.0)[3] * 255.0 + 0.5, 0.0, 255.0);
    o.flicker = c.b("EffectReduceFlicker");
    const double bs = boost > 0 ? boost * 2.0 : boost;
    o.scale = (float)(1.0 + bs);
    double v16 = 0.3;
    if (boost > 0) v16 -= v16 * boost / 2.0;
    colourLut(hue, sweep, sat, 0.9, 0.15, hue + sweep * (1.0 - v16), o.clut);
    o.streaks.assign((size_t)o.n, DbGeom());
    for (int i = 0; i < o.n; ++i) {
        const double ang = i * 180.0 / o.n;
        const double a = ang * kPiV / 180.0;
        const double rx = -std::sin(a) * dy, ry = std::cos(a) * dy;
        const double sxf = dy > 0 ? dx / dy : 0.0;
        const double blur = std::hypot(rx * sxf, ry);
        directionalGeom(W, H, ang + rotation, blur * W, o.streaks[(size_t)i]);
    }
    return true;
}

// the (whole pixels, 12 bit weight) of every line of a shear by t, as floats pairs
void shearParamsGpu(int lines, double t, std::vector<float>& tab)
{
    tab.resize((size_t)lines * 2);
    for (int k = 0; k < lines; ++k) {
        int ip, fr;
        shearLineOf(k, t, ip, fr);
        tab[(size_t)k * 2] = (float)ip;
        tab[(size_t)k * 2 + 1] = (float)fr;
    }
}

void glintVelvet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    GlintSetup su;
    if (!glintSetup(c, W, H, su)) { maskOnly(c, su.mask); return; }
    const int maskMode = su.maskMode;
    const std::vector<float>& mask = su.mask;
    Buf srcCopy;
    const Buf* src = &c.src;
    if (maskMode == 2) { srcCopy = c.src; multiplyBy(srcCopy, mask); src = &srcCopy; }
    const bool effectOnly = su.effectOnly;
    const int n = su.n;
    const int* tlut = su.tlut;
    const double dx = c.d("EffectHorizontalRadius") / 100.0, dy = c.d("EffectVerticalRadius") / 100.0, rotation = c.d("EffectOrientation");
    std::vector<int> g((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int v = lumaByte(src->at(x, y));
            if (maskMode == 1) v = (int)std::floor(v * mask[(size_t)y * W + x] + 0.5);
            g[(size_t)y * W + x] = v;
        }
    if (su.flicker) blur3x3(g, W, H);
    const float scale = su.scale;
    const uint8_t (*clut)[4] = su.clut;

    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            const float* s0 = src->at(x, y);
            if (effectOnly) o[0] = o[1] = o[2] = o[3] = 0.f;
            else for (int q = 0; q < 4; ++q) o[q] = s0[q];
        }
    });
    if (n <= 0) return;
    // FilterLibrary.Glint blurs the picture's channels one after the other: streak i is DirectionalBlur(angle i x 180 / n + rotation) on channel i, but the sheared picture has all its
    // channels in it, so every other channel also goes through that streak's shear and its shear back (and loses a little on the way).
    std::vector<uint8_t> first((size_t)W * H);
    for (size_t k = 0; k < first.size(); ++k) first[k] = (uint8_t)tlut[std::max(0, std::min(255, g[k]))];
    std::vector<std::vector<uint8_t>> streaks((size_t)n, first);
    for (int i = 0; i < n; ++i) {
        const double ang = i * 180.0 / n;
        const double a = ang * kPiV / 180.0;
        const double rx = -std::sin(a) * dy, ry = std::cos(a) * dy;
        const double sxf = dy > 0 ? dx / dy : 0.0;
        const double blur = std::hypot(rx * sxf, ry);
        parallelTasks(n, [&](int j) { directionalBlur(streaks[(size_t)j], W, H, ang + rotation, blur * W, scale, j == i); });
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            for (int i = 0; i < n; ++i) screenOver(o, clut[streaks[(size_t)i][(size_t)y * W + x]]);
        }
    });
}


// ================================================================== Fill Light
// FilterLibrary.FillLight, as CoreGraphics.Native does it: the picture goes to bytes (float x 255 in float, +0.5, floor), Desaturate makes the luma ((19595 r + 38469 g + 7472 b + 32768) >> 16), ChannelMap looks it up in a table
// of 256 colours - a Bezier curve that is 0 at black, 0.3 at 0.25 + 0.08 bias and 0 from 0.85, as grey coloured by Colorize(hue, 0.1 x correction), sampled at 128 points into a Gradient - GaussianBlur(Softness) of the
// bytes (the Young - van Vliet blur of builtins_cgblur, deviation from the diameter Softness x width) and the mask, back in floats (m / 255), is ADDED to the float picture at Strength x 0.5, without a limit.
// Hue in degrees, Strength / Bias / Tint (= Correction) / Softness in percent. Not exact: about 4 % of the pixels are a level off, where the mask climbs in steps (VEGAS may blur on the GPU).
// What Fill Light works out from the settings alone (shared by the CPU code and the GPU plan): copy true when the picture goes through unchanged, else the byte table of the
// colour made from the luma, the blur (cgGaussianBlur's coefficients, blur false for none) and the opacity of the Add.
bool fillLightSetup(Ctx& c, int W, FillLightSetup& o)
{
    const double strength = c.d("Strength") / 100.0, bias = c.d("Bias") / 100.0, correction = c.d("Tint") / 100.0;
    const double softness = c.d("Softness") / 100.0, hue = c.d("Hue") / 360.0;
    o.copy = true;
    o.blur = false;
    if (!(strength > 0)) return true;
    std::vector<BezierPoint> curve = {{0.0, 0.0, 0.0, 0.0}, {0.25 + bias * 0.08, 0.3, 0.0, 0.0}, {0.85, 0.0, 0.0, 0.0}};
    std::sort(curve.begin(), curve.end(), [](const BezierPoint& a, const BezierPoint& b) { return a.key < b.key; });
    ColorMatrix cm;
    cm.colorize((float)hue, (float)(0.1 * correction));
    Gradient g;
    for (int i = 0; i < 128; ++i) {
        const double x = i / 127.0;
        const float v = (float)bezierCurve(curve, x);
        float oc[4];
        cm.transform(v, v, v, oc);
        g.add(x, oc[0], oc[1], oc[2], oc[3]);
    }
    for (int i = 0; i < 256; ++i) {
        const std::array<float, 4> col = g.at(i / 255.0);
        for (int k = 0; k < 4; ++k) o.lut[i][k] = (uint8_t)clampd((double)col[(size_t)k] * 255.0 + 0.5, 0.0, 255.0);
    }
    const double diameter = softness * W;
    if (diameter > 0) {
        const double sigma = sigmaForDiameter(diameter);
        o.blur = true;
        o.sigma = sigma;
    }
    // The mask, blurred in bytes, goes back to floats (m / 255) and CompositeOp 3 (Add) at the opacity adds it to the (float) picture without a limit: colour d + m x op, alpha d + m x op x (1 - d)
    const double op = clampd(strength * 0.5, 0.0, 1.0);
    if (op < 0.00392156862745098) return true;
    o.fop = (float)op;
    o.copy = false;
    return true;
}

void fillLightVelvet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    c.out.px = c.src.px;
    FillLightSetup su;
    fillLightSetup(c, W, su);
    if (su.copy) return;
    std::vector<float> mask((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = c.src.at(x, y);
            const int r = byteOf(p[0]), gg = byteOf(p[1]), b = byteOf(p[2]);
            const int l = lumaInt(r, gg, b);
            float* m = &mask[((size_t)y * W + x) * 4];
            m[0] = su.lut[l][1]; m[1] = su.lut[l][2]; m[2] = su.lut[l][3]; m[3] = su.lut[l][0];
        }
    if (su.blur) {
        cgGaussianBlur(mask.data(), W, H, su.sigma, su.sigma);
        for (float& v : mask) v = (float)cgBlurByte(v);
    }
    const float fop = su.fop;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s0 = c.src.at(x, y);
            float* o = c.out.at(x, y);
            const float* m = &mask[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = s0[k] + m[k] * (1.f / 255.f) * fop;
            o[3] = s0[3] + m[3] * (1.f / 255.f) * fop * (1.f - s0[3]);
        }
    });
}


// ================================================================== Soft Contrast
// FilterLibrary.Softlight (8-bit path, exact against VEGAS): 1. every colour channel through a table min(255, floor(i x (1 + 0.2 Stretch) + 0.5)); 2. a grey copy (byte luma) blurred like Fill Light's mask
// (GaussianBlur of Diffusion x width pixels); 3. the grey through a table made from the Low / High trim spline; 4. the Overlay composite (CompositeNative op 5) of the grey over the picture at Contrast
// (opacity rounded to a byte); 5. the Vignette (mask cut to a byte, MaskedFadeNative). Tint: the grey through HslAdjust's colour matrix. Exact for the default and the Moderate / Heavy presets; with a Vignette the
// mask's soft edge is still a level or two off (the mask's own native blur / drawing is not rebuilt).
namespace {
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

// What Soft Contrast works out from the settings alone: the tables and the vignette mask (shared by the CPU code and the GPU plan).
bool softContrastSetup(Ctx& c, int W, int H, ScSetup& o)
{
    const double stretch = c.d("EffectStretchRange") / 100.0, strength = c.d("EffectContrast") / 100.0, diffusion = c.d("EffectDiffusion") / 100.0;
    const double tint = c.d("EffectTint") / 100.0, hue = c.d("EffectCorrectionHue");
    o.ext = c.i("VignetteExteriorEffect");
    for (int i = 0; i < 256; ++i) o.stl[i] = (int)std::min(255.0, std::floor((double)i * (1.0 + stretch * 0.2) + 0.5));
    o.blur = diffusion > 0;
    o.sigma = o.blur ? sigmaForDiameter(diffusion * 0.5 * W) : 0.0;
    const double lowTrim = std::max(0.0, c.d("EffectLowTrim") / 100.0), highTrim = std::max(0.0, c.d("EffectHighTrim") / 100.0);
    o.trim = lowTrim > 0 || highTrim > 0;
    if (o.trim) {
        const ScTcb pts[3] = {{0.0, lowTrim * 0.25, 0, -1, 0}, {0.5, 0.5, 0, 0, 0}, {1.0, 1.0 - highTrim * 0.25, 0, -1, 0}};
        for (int i = 0; i < 256; ++i) o.tl[i] = (int)clampd(std::floor(scTcb(pts, 3, i / 255.0) * 255.0 + 0.5), 0.0, 255.0);
    }
    // HslAdjust (mode 1, "tint") draws the grey through a GDI+ colour matrix: Saturation(0), then Colorize(TintHue, TintSaturation x 0.299)
    o.tint = tint > 0.0;
    if (o.tint) {
        ColorMatrix cm;
        cm.saturation(0.f);
        cm.colorize((float)(hue / 360.0), (float)(tint * 0.299));
        for (int g = 0; g < 256; ++g) {
            const float v = (float)g * (1.f / 255.f);
            float oc[4];
            cm.transform(v, v, v, oc);
            for (int k = 0; k < 3; ++k) o.tl3[g][k] = (int)clampd(std::floor((double)oc[1 + k] * 255.0 + 0.5), 0.0, 255.0);
        }
    }
    o.oq = (int)(clampd(strength, 0.0, 1.0) * 255.0 + 0.5);
    const double vs = clampd(c.d("VignetteStrength") / 100.0, 0.0, 1.0);
    o.vscale = vs < 1.0;
    o.vq = (int)(vs * 255.0 + 0.5);
    const bool ellipse = c.i("VignetteShape") == 1;
    double cx = 0.5, cy = 0.5;
    c.xy("VignettePosition", cx, cy);
    o.vmask.clear();
    if (o.ext != 0)
        shapeMaskCgBytes(W, H, cx, 1.0 - cy, std::max(0.0, c.d("VignetteWidth") / 100.0), std::max(0.0, c.d("VignetteHeight") / 100.0),
                        ellipse ? 0.0 : std::max(0.0, c.d("VignetteCornerRadius") / 100.0), std::max(0.0, c.d("VignetteSoftness") / 100.0), ellipse, o.vmask);
    return true;
}

void softContrast(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    ScSetup su;
    softContrastSetup(c, W, H, su);
    const int ext = su.ext;
    const bool tintOn = su.tint;
    const int oq = su.oq;
    const std::vector<float>& vmask = su.vmask;
    const int* stl = su.stl;
    std::vector<float> st((size_t)W * H * 4), gray((size_t)W * H * 4, 0.f);
    std::vector<float> alpha((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            const size_t i = (size_t)y * W + x;
            alpha[i] = a;
            const int R = stl[byteOf(clamp01(r))], G = stl[byteOf(clamp01(g))], B = stl[byteOf(clamp01(b))];
            st[i * 4] = (float)R; st[i * 4 + 1] = (float)G; st[i * 4 + 2] = (float)B; st[i * 4 + 3] = 255.f;
            const float l = (float)lumaInt(R, G, B);
            gray[i * 4] = gray[i * 4 + 1] = gray[i * 4 + 2] = l; gray[i * 4 + 3] = 255.f;
        }
    if (su.blur) {
        cgGaussianBlur(gray.data(), W, H, su.sigma, su.sigma);
        for (float& v : gray) v = (float)cgBlurByte(v);
    }
    if (su.trim) {
        for (size_t i = 0; i < (size_t)W * H; ++i)
            for (int k = 0; k < 3; ++k) gray[i * 4 + k] = (float)su.tl[(int)gray[i * 4 + k]];
    }
    const int (*tl3)[3] = su.tl3;
    const bool vscale = su.vscale;
    const int vq = su.vq;
    Buf soft;
    if (ext == 1) {
        soft = c.src;
        const double bx = c.d("VignetteHorizontalBlur") * 0.05 * W, by = c.d("VignetteVerticalBlur") * 0.05 * H;
        gaussianBlur(soft, bx, by);
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            float* o = c.out.at(x, y);
            float a = alpha[i];
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            int v[3];
            for (int k = 0; k < 3; ++k) {
                const int d = (int)st[i * 4 + k];
                const int gv = (int)gray[i * 4 + k];
                const int s = tintOn ? tl3[gv][k] : gv;
                const int ov = d < 128 ? (2 * d * s) / 255 : 255 - (2 * (255 - d) * (255 - s)) / 255;
                v[k] = (d * (255 - oq) + ov * oq) / 255;
            }
            if (ext != 0) {
                // Vignette: the mask (coverage cut to a byte, 255 - that), scaled by Strength through a table, fades the picture towards the exterior colour (MaskedFadeNative: (e M + d (255 - M)) / 255)
                int M = 255 - (int)std::floor((double)vmask[i] * 255.0);
                M = (int)clampd((double)M, 0.0, 255.0);
                if (vscale) M = (int)std::floor((double)M * (double)vq / 255.0 + 0.5);
                int e[4] = {0, 0, 0, 0};
                if (ext == 1) {
                    float sr, sg2, sb, sa;
                    unpremul(soft.at(x, y), sr, sg2, sb, sa);
                    e[0] = byteOf(clamp01(sr)); e[1] = byteOf(clamp01(sg2)); e[2] = byteOf(clamp01(sb)); e[3] = 255;
                } else if (ext == 3) e[0] = e[1] = e[2] = e[3] = 255;
                else if (ext == 2) e[3] = 255;
                for (int q = 0; q < 3; ++q) v[q] = (e[q] * M + v[q] * (255 - M)) / 255;
                if (ext == 4) a = (float)((e[3] * M + byteOf(a) * (255 - M)) / 255) * (1.f / 255.f);
            }
            premul(o, v[0] * (1.f / 255.f), v[1] * (1.f / 255.f), v[2] * (1.f / 255.f), a);
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
        lineBlurAll(cv.data(), CH, CW, 1, CW, sigma);
        lineBlurAll(cv.data(), CW, CH, CW, 1, sigma);
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
    std::vector<char> zeroK((size_t)Q, 1), zeroImg((size_t)Q, 1);
    for (int y = 0; y < KS; ++y) zeroK[(size_t)(((y - R) % Q + Q) % Q)] = 0;
    for (int y = 0; y < H; ++y) zeroImg[(size_t)y] = 0;
    fft2(K, P, Q, false, &zeroK, P);
    fft2(A, P, Q, false, &zeroImg, P);
    fft2(B, P, Q, false, &zeroImg, P);
    forRows(Q, P, [&](int y) {
        for (size_t i = (size_t)y * P, e = i + P; i < e; ++i) {
            const double kr = K[i].real(), ki = K[i].imag();
            const double ar = A[i].real(), ai = A[i].imag(), br = B[i].real(), bi = B[i].imag();
            A[i] = std::complex<double>(ar * kr - ai * ki, ar * ki + ai * kr);
            B[i] = std::complex<double>(br * kr - bi * ki, br * ki + bi * kr);
        }
    });
    fft2(A, P, Q, true, nullptr, W);
    fft2(B, P, Q, true, nullptr, W);
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
