// VEGAS's Gaussian blur (see builtins_deriche.h), worked out from VEGAS's Vfx1 plug-ins: a 4th-order recursive filter after Deriche (the constants 1.680, 3.735, 1.783, 1.723,
// 0.6318, 1.997, -0.6803, -0.2598 of his 1993 report, as single-precision numbers); its causal and anti-causal halves run along each line at once, the picture's edge values taken
// to continue forever; values below 1e-17 count as 0. Every step is in doubles in VEGAS's order, so 8-bit results match to the level.
#include "builtins_deriche.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

DericheCoefs vegasDericheCoefs(double sigma)
{
    const double s = sigma > 0.6 ? sigma : 0.6;
    const double r = s * 2.50662827463;
    const double B0 = (double)-1.783f / s, C0 = (double)-0.6803f / r, A0 = (double)1.6803f / r, W1 = (double)1.997f / s;
    const double B1 = (double)-1.723f / s, A1 = (double)3.735f / r, C1 = (double)-0.2598f / r, W0 = (double)0.6318f / s;
    const double n0 = C0 + A0;
    const double sW1 = std::sin(W1), cW1 = std::cos(W1), eB1 = std::exp(B1);
    const double sW0 = std::sin(W0), cW0 = std::cos(W0), eB0 = std::exp(B0);
    const double n1 = (A1 * sW0 - (C0 + C0 + A0) * cW0) * eB0 + (C1 * sW1 - (A0 + A0 + C0) * cW1) * eB1;
    const double e01 = std::exp(B1 + B0), e00 = std::exp(B0 + B0), e11 = std::exp(B1 + B1);
    const double n2 = ((cW1 * n0 * cW0 - A1 * cW1 * sW0) - C1 * cW0 * sW1) * (e01 + e01) + e00 * C0 + e11 * A0;
    const double e001 = std::exp(B0 + B0 + B1), e011 = std::exp(B1 + B1 + B0);
    const double n3 = (A1 * sW0 - A0 * cW0) * e011 + (C1 * sW1 - C0 * cW1) * e001;
    const double d1 = -2.0 * eB1 * cW1 - 2.0 * eB0 * cW0;
    const double d2 = 4.0 * cW1 * cW0 * e01 + e11 + e00;
    const double d3 = -2.0 * cW0 * e011 - 2.0 * cW1 * e001;
    const double d4 = std::exp((B1 + B1) + (B0 + B0));
    DericheCoefs c;
    const double n[5] = {n0, n1, n2, n3, 0.0}, d[5] = {0.0, d1, d2, d3, d4};
    for (int k = 0; k < 5; ++k) { c.n[k] = n[k]; c.d[k] = d[k]; }
    c.m[0] = 0.0; c.m[1] = n1 - d1 * n0; c.m[2] = n2 - d2 * n0; c.m[3] = n3 - d3 * n0; c.m[4] = 0.0 - n0 * d4;
    const double SN = n0 + 0.0 + n1 + n2 + n3 + 0.0;
    const double SM = 0.0 + 0.0 + c.m[1] + c.m[2] + c.m[3] + c.m[4];
    const double SD = 0.0 + 0.0 + d1 + d2 + d3 + d4 + 1.0;
    double kn = 1.0, km = 1.0;
    if (SD > 1e-17 || -SD > 1e-17) { kn = SN / SD; km = SM / SD; }
    for (int k = 0; k < 5; ++k) { c.bn[k] = kn * d[k]; c.bm[k] = km * d[k]; }
    return c;
}

double vegasBlurSigma(double s)
{
    const double t = std::fabs(s) + 1.5;
    return std::sqrt(t * t / (-2.0 * std::log(1.0 / 255.0)));
}

namespace {

// One line of L values: out[i] = causal[i] + anti-causal[i] (yc, ya: scratch of L each).
void dericheLine(const double* x, int L, const DericheCoefs& c, double* yc, double* ya, double* out)
{
    if (L <= 0) return;
    const double xL = x[0], xR = x[L - 1];
    double eN[5], eM[5];
    for (int k = 0; k < 5; ++k) { eN[k] = c.n[k] - c.bn[k]; eM[k] = c.m[k] - c.bm[k]; }
    for (int i = 0; i < L; ++i) {
        const int h = std::min(i, 5), j = L - 1 - i;
        double acc = 0.0, acca = 0.0;
        yc[i] = 0.0;
        ya[j] = 0.0;
        for (int k = 0; k < h; ++k) {
            acc += x[i - k] * c.n[k] - c.d[k] * yc[i - k];
            acca += x[j + k] * c.m[k] - c.d[k] * ya[j + k];
        }
        for (int k = h; k < 5; ++k) {
            acc += eN[k] * xL;
            acca += eM[k] * xR;
        }
        if (acc < 1e-17 && acc > -1e-17) acc = 0.0;
        if (acca < 1e-17 && acca > -1e-17) acca = 0.0;
        yc[i] = acc;
        ya[j] = acca;
    }
    for (int i = 0; i < L; ++i) out[i] = yc[i] + ya[i];
}

}  // namespace

void vegasDericheBlurSigma(Buf& b, double sigmaX, double sigmaY, bool levels)
{
    const int W = b.w, H = b.h;
    if (W <= 0 || H <= 0) return;
    const DericheCoefs cx = vegasDericheCoefs(sigmaX), cy = vegasDericheCoefs(sigmaY);
    std::vector<float> mid((size_t)W * H * 4);
    forRows(H, W * 40, [&](int y) {
        std::vector<double> x((size_t)W), yc((size_t)W), ya((size_t)W), o((size_t)W);
        for (int k = 0; k < 4; ++k) {
            for (int i = 0; i < W; ++i) {
                const double v = (double)b.at(i, y)[k] * 255.0;
                x[(size_t)i] = levels ? std::nearbyint(std::min(255.0, std::max(0.0, v))) : v;
            }
            dericheLine(x.data(), W, cx, yc.data(), ya.data(), o.data());
            for (int i = 0; i < W; ++i) mid[((size_t)y * W + i) * 4 + k] = (float)o[(size_t)i];
        }
    });
    // columns, from the bottom row up as VEGAS's frames lie in memory
    forRows(W, H * 40, [&](int col) {
        std::vector<double> x((size_t)H), yc((size_t)H), ya((size_t)H), o((size_t)H);
        for (int k = 0; k < 4; ++k) {
            for (int i = 0; i < H; ++i) x[(size_t)i] = (double)mid[((size_t)(H - 1 - i) * W + col) * 4 + k];
            dericheLine(x.data(), H, cy, yc.data(), ya.data(), o.data());
            for (int i = 0; i < H; ++i) {
                double v = o[(size_t)i];
                if (levels) v = std::min(255.0, std::max(0.0, std::nearbyint(v)));
                b.at(col, H - 1 - i)[k] = (float)(v / 255.0);
            }
        }
    });
}

void vegasDericheBlur(Buf& b, double sizeX, double sizeY, bool levels)
{
    vegasDericheBlurSigma(b, vegasBlurSigma(sizeX), vegasBlurSigma(sizeY), levels);
}


// ---------------------------------------------------------------------------------------------------- Linear Blur
// The shears work on a plane of levels (0..255 floats, whole numbers for 8-bit) in "shear" coordinates: VEGAS's routines shear the columns of a frame down by up to 'shear' / 16384 rows
// across its width, in 14-bit fixed point, the places outside filled from the first / last row along the shear (the blur repeats the edge along its direction), and every value
// between two rows taken by VEGAS's 8-bit mix A + (B - A) * w / 255 (w 0..254).
namespace {

struct LPlane {
    int w = 0, h = 0;
    std::vector<float> p;           // w * h * 4
    void resize(int W, int H) { w = W; h = H; p.assign((size_t)W * H * 4, 0.f); }
    float* at(int x, int y) { return &p[((size_t)y * w + x) * 4]; }
    const float* at(int x, int y) const { return &p[((size_t)y * w + x) * 4]; }
};

LPlane transposed(const LPlane& a)
{
    LPlane t;
    t.resize(a.h, a.w);
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) std::memcpy(t.at(y, x), a.at(x, y), 4 * sizeof(float));
    return t;
}
LPlane flipped(const LPlane& a)
{
    LPlane t;
    t.resize(a.w, a.h);
    for (int y = 0; y < a.h; ++y) std::memcpy(t.at(0, y), a.at(0, a.h - 1 - y), (size_t)a.w * 4 * sizeof(float));
    return t;
}

// VEGAS's mix of two levels with a weight of w / 255 (w 0..254): (B - A) * w + 128, plus itself / 256, / 256, added to A, kept in 0..255
inline void mix8(const float* a, const float* b, int w8, bool levels, float* o)
{
    float r[4];
    for (int k = 0; k < 4; ++k) {
        if (levels) {
            const int A = (int)a[k], t = ((int)b[k] - A) * w8 + 128;
            const int v = A + ((t + (t >> 8)) >> 8);
            r[k] = (float)std::min(255, std::max(0, v));
        } else {
            r[k] = std::min(255.f, std::max(0.f, a[k] + (b[k] - a[k]) * (float)w8 / 255.f));
        }
    }
    std::memcpy(o, r, sizeof r);
}

inline int ceilShear(int shear) { return shear > 0 ? (shear + 16383) / 16384 : 0; }

// The frame sheared into a plane ceil(shear / 16384) rows taller (VEGAS's 0x1803ee5b0)
LPlane shearIn(const LPlane& src, int shear, bool levels)
{
    const int H = src.h, W = src.w, bufH = H + ceilShear(shear);
    LPlane dst;
    dst.resize(W, bufH);
    const int inc = W > 1 ? (int)((unsigned)shear / (unsigned)(W - 1)) : 0;
    if (inc == 0) {
        for (int k = 0; k < bufH; ++k) std::memcpy(dst.at(0, k), src.at(0, std::min(k, H - 1)), (size_t)W * 4 * sizeof(float));
        return dst;
    }
    // the edge values along the shear: the last column's top rows from the first row, the first column's bottom rows from the last row
    const int S0 = shear >> 14;
    for (int r = 0; r <= S0; ++r) {
        const int X = (int)((double)((r << 14) + 0x2000) * 16384.0 / (double)inc);
        const int Xi = X >> 14, c0 = std::min(Xi, W - 1), c1 = std::min(c0 + 1, W - 1), R = std::max(0, W - 1 - Xi);
        const int w8 = ((X & 0x3fff) * 255) >> 14;
        if (bufH - 1 - r >= 0) mix8(src.at(R, H - 1), src.at(R > 0 ? R - 1 : R, H - 1), w8, levels, dst.at(0, bufH - 1 - r));
        if (r < bufH) mix8(src.at(c0, 0), src.at(c1, 0), w8, levels, dst.at(W - 1, r));
    }
    int acc = 0;
    for (int x = 0; x < W; ++x, acc += inc) {
        const int r12 = acc >> 14, w8 = ((acc & 0x3fff) * 255) >> 14;
        if (r12 >= bufH) break;
        if (x != W - 1)
            for (int k = 0; k < r12; ++k) std::memcpy(dst.at(x, k), dst.at(W - 1, k), 4 * sizeof(float));
        mix8(src.at(x, 0), dst.at(W - 1, r12), w8, levels, dst.at(x, r12));
        for (int k = 1; k < H && r12 + k < bufH; ++k) mix8(src.at(x, k), src.at(x, k - 1), w8, levels, dst.at(x, r12 + k));
        if (r12 + H < bufH) mix8(dst.at(0, r12 + H), src.at(x, H - 1), w8, levels, dst.at(x, r12 + H));
        if (x != 0)
            for (int k = r12 + H + 1; k < bufH; ++k) std::memcpy(dst.at(x, k), dst.at(0, k), 4 * sizeof(float));
    }
    return dst;
}

// Back (VEGAS's 0x1803ef0a0): H rows, each column read 'acc' / 16384 rows further down (acc grows by shear / width a column).
// wrap: one past the last row is the next column's first value (VEGAS's column-wise copy reads on in memory).
LPlane shearOut(const LPlane& buf, int shear, int H, bool levels, bool wrap)
{
    const int W = buf.w, bufH = buf.h;
    LPlane dst;
    dst.resize(W, H);
    const int inc = W > 0 ? (int)((unsigned)shear / (unsigned)W) : 0;
    if (inc == 0) {
        for (int k = 0; k < H; ++k) std::memcpy(dst.at(0, k), buf.at(0, std::min(k, bufH - 1)), (size_t)W * 4 * sizeof(float));
        return dst;
    }
    int acc = 0;
    for (int x = 0; x < W; ++x, acc += inc) {
        const int s = acc >> 14, w8 = ((acc & 0x3fff) * 255) >> 14;
        for (int i = 0; i < H; ++i) {
            const int a = std::min(s + i, bufH - 1), j = s + i + 1;
            const float* B = j < bufH ? buf.at(x, j) : (wrap && x > 0 ? buf.at(x - 1, 0) : buf.at(x, bufH - 1));
            mix8(buf.at(x, a), B, w8, levels, dst.at(x, i));
        }
    }
    return dst;
}

}  // namespace

void vegasLinearBlur(const Buf& src, Buf& out, double angleDeg, double amount, bool levels)
{
    const int W = src.w, H = src.h;
    out.ensure(W, H);
    if (!(amount > 1e-6) || W < 2 || H < 2) { out.px = src.px; return; }
    const double pi = 3.141592653589793;
    double a = std::fmod(angleDeg, 360.0);         // (VEGAS's own angles go 0..360)
    if (a < 0) a += 360.0;
    if (!(a >= 0.0 && a < 360.0)) a = 0.0;
    const double x7 = (a >= 180.0 ? a - 180.0 : a) * pi / 180.0;      // (VEGAS's own folding: only one turn is expected)
    double x6 = 360.0 - a;
    if (x6 >= 180.0) x6 -= 180.0;
    double th = x6 * pi / 180.0;
    const double cth = std::cos(th);
    if (std::fabs(cth) > 1e-6) {
        th = std::atan2(std::sin(th), cth);
        if (th < 0.0) th += 2.0 * pi;
    }
    const double L = (double)std::max(W, H) * amount;
    // the frame as levels, top row first (VEGAS's frames lie bottom row first: 'flip' below turns to and from that)
    LPlane pic;
    pic.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* s = src.at(x, y);
            float* d = pic.at(x, y);
            for (int k = 0; k < 4; ++k) {
                const double v = (double)s[k] * 255.0;
                d[k] = levels ? (float)std::nearbyint(std::min(255.0, std::max(0.0, v))) : (float)v;
            }
        }
    int branch, shear;
    double sx, sy;
    if (th < pi / 4) { branch = 1; shear = (int)(std::tan(th) * W * 16384.0); sx = std::cos(x7) * L; sy = 0; }
    else if (th < pi / 2) { branch = 2; shear = (int)(std::tan(pi / 2 - th) * H * 16384.0); sx = 0; sy = std::cos(pi / 2 - x7) * L; }
    else if (th < 3 * pi / 4) { branch = 3; shear = (int)(std::tan(th - pi / 2) * H * 16384.0); sx = 0; sy = std::cos(x7 - pi / 2) * L; }
    else { branch = 4; shear = (int)(std::tan(pi - th) * W * 16384.0); sx = std::cos(pi - x7) * L; sy = 0; }
    shear = std::max(0, shear);
    const bool memoryOrder = branch <= 2, across = branch == 2 || branch == 3;
    LPlane f = memoryOrder ? flipped(pic) : pic;
    if (across) f = transposed(f);
    LPlane buf = shearIn(f, shear, levels);
    if (across) buf = transposed(buf);
    if (memoryOrder) buf = flipped(buf);
    // blurred as a frame (top row first)
    Buf b;
    b.ensure(buf.w, buf.h);
    for (size_t i = 0; i < buf.p.size(); ++i) b.px[i] = (float)(buf.p[i] / 255.0);
    vegasDericheBlur(b, sx, sy, levels);
    for (size_t i = 0; i < buf.p.size(); ++i) buf.p[i] = levels ? (float)std::nearbyint(b.px[i] * 255.0) : (float)(b.px[i] * 255.0);
    if (memoryOrder) buf = flipped(buf);
    if (across) buf = transposed(buf);
    LPlane o = shearOut(buf, shear, across ? W : H, levels, across);
    if (across) o = transposed(o);
    if (memoryOrder) o = flipped(o);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) out.at(x, y)[k] = (float)(o.at(x, y)[k] / 255.0);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
