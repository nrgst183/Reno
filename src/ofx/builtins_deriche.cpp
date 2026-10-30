// VEGAS's Gaussian blur (see builtins_deriche.h), worked out from VEGAS's Vfx1 plug-ins: a 4th-order recursive filter after Deriche (the constants 1.680, 3.735, 1.783, 1.723,
// 0.6318, 1.997, -0.6803, -0.2598 of his 1993 report, as single-precision numbers); its causal and anti-causal halves run along each line at once, the picture's edge values taken
// to continue forever; values below 1e-17 count as 0. Every step is in doubles in VEGAS's order, so 8-bit results match to the level.
#include "builtins_deriche.h"
#include "builtins_vec.h"
#include "../core/cpufeat.h"
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

typedef d2::D2 D2;
inline D2 d2ld(const double* p) { return d2::ld(p); }
inline void d2st(double* p, D2 v) { d2::st(p, v); }
inline D2 d2set(double x) { return d2::set(x); }
inline D2 d2add(D2 a, D2 b) { return d2::add(a, b); }
inline D2 d2sub(D2 a, D2 b) { return d2::sub(a, b); }
inline D2 d2mul(D2 a, D2 b) { return d2::mul(a, b); }
inline D2 d2tiny(D2 a, D2 lim) { return d2::tiny(a, lim); }

const int kLanes = 16;           // four lines (rows or columns) of four channels, side by side

// The filter along 'L' values of each of the kLanes independent lines: x[i * kLanes + lane] in, yc (causal) and ya (anti-causal) out, the line's result being yc + ya.
// The first and last five values take the edge terms; after that every value is a sum of five taps in the scalar filter's order (the taps that are exactly 0 left out).
#ifdef RENO_HAVE_AVX2_CODE
RENO_AVX2_FN static inline __m256d a4tiny(__m256d a, __m256d lim, __m256d nlim)
{
    const __m256d m = _mm256_and_pd(_mm256_cmp_pd(a, lim, _CMP_LT_OS), _mm256_cmp_pd(a, nlim, _CMP_GT_OS));
    return _mm256_andnot_pd(m, a);
}
// The two long loops of dericheLanes with four doubles to a vector (the same operations in the same order; no fused multiply-add).
RENO_AVX2_FN static void dericheMainAvx2(const DericheCoefs& c, int L, const double* x, double* yc, double* ya)
{
    const int LN = kLanes;
    const __m256d n0 = _mm256_set1_pd(c.n[0]), n1 = _mm256_set1_pd(c.n[1]), n2 = _mm256_set1_pd(c.n[2]), n3 = _mm256_set1_pd(c.n[3]);
    const __m256d m1 = _mm256_set1_pd(c.m[1]), m2 = _mm256_set1_pd(c.m[2]), m3 = _mm256_set1_pd(c.m[3]), m4 = _mm256_set1_pd(c.m[4]);
    const __m256d d1 = _mm256_set1_pd(c.d[1]), d2v = _mm256_set1_pd(c.d[2]), d3 = _mm256_set1_pd(c.d[3]), d4 = _mm256_set1_pd(c.d[4]);
    const __m256d lim = _mm256_set1_pd(1e-17), nlim = _mm256_set1_pd(-1e-17);
#define tinyz(a) a4tiny((a), lim, nlim)
#define ld(q) _mm256_loadu_pd(q)
#define mul(a, b) _mm256_mul_pd((a), (b))
#define add(a, b) _mm256_add_pd((a), (b))
#define sub(a, b) _mm256_sub_pd((a), (b))
    for (int i = 5; i < L; ++i) {
        const double* xi = x + (size_t)i * LN;
        double* yi = yc + (size_t)i * LN;
        for (int p = 0; p < LN; p += 4) {
            __m256d acc = mul(ld(xi + p), n0);
            acc = add(acc, sub(mul(ld(xi + p - LN), n1), mul(d1, ld(yi + p - LN))));
            acc = add(acc, sub(mul(ld(xi + p - 2 * LN), n2), mul(d2v, ld(yi + p - 2 * LN))));
            acc = add(acc, sub(mul(ld(xi + p - 3 * LN), n3), mul(d3, ld(yi + p - 3 * LN))));
            acc = sub(acc, mul(d4, ld(yi + p - 4 * LN)));
            _mm256_storeu_pd(yi + p, tinyz(acc));
        }
    }
    for (int j = L - 6; j >= 0; --j) {
        const double* xj = x + (size_t)j * LN;
        double* yj = ya + (size_t)j * LN;
        for (int p = 0; p < LN; p += 4) {
            __m256d acc = sub(mul(ld(xj + p + LN), m1), mul(d1, ld(yj + p + LN)));
            acc = add(acc, sub(mul(ld(xj + p + 2 * LN), m2), mul(d2v, ld(yj + p + 2 * LN))));
            acc = add(acc, sub(mul(ld(xj + p + 3 * LN), m3), mul(d3, ld(yj + p + 3 * LN))));
            acc = add(acc, sub(mul(ld(xj + p + 4 * LN), m4), mul(d4, ld(yj + p + 4 * LN))));
            _mm256_storeu_pd(yj + p, tinyz(acc));
        }
    }
#undef tinyz
#undef ld
#undef mul
#undef add
#undef sub
}
#endif

void dericheLanes(const DericheCoefs& c, int L, const double* x, double* yc, double* ya)
{
    if (L <= 0) return;
    double eN[5], eM[5];
    for (int k = 0; k < 5; ++k) { eN[k] = c.n[k] - c.bn[k]; eM[k] = c.m[k] - c.bm[k]; }
    const double tiny = 1e-17;
    const int LN = kLanes;
    for (int l = 0; l < LN; ++l) {
        const double xL = x[l], xR = x[(size_t)(L - 1) * LN + l];
        for (int i = 0; i < L && i < 5; ++i) {
            yc[(size_t)i * LN + l] = 0.0;
            double acc = 0.0;
            for (int k = 0; k < i; ++k) acc += x[(size_t)(i - k) * LN + l] * c.n[k] - c.d[k] * yc[(size_t)(i - k) * LN + l];
            for (int k = i; k < 5; ++k) acc += eN[k] * xL;
            if (acc < tiny && acc > -tiny) acc = 0.0;
            yc[(size_t)i * LN + l] = acc;
        }
        for (int i = 0; i < L && i < 5; ++i) {
            const int j = L - 1 - i;
            ya[(size_t)j * LN + l] = 0.0;
            double acca = 0.0;
            for (int k = 0; k < i; ++k) acca += x[(size_t)(j + k) * LN + l] * c.m[k] - c.d[k] * ya[(size_t)(j + k) * LN + l];
            for (int k = i; k < 5; ++k) acca += eM[k] * xR;
            if (acca < tiny && acca > -tiny) acca = 0.0;
            ya[(size_t)j * LN + l] = acca;
        }
    }
#ifdef RENO_HAVE_AVX2_CODE
    if (cpuHasAvx2()) { dericheMainAvx2(c, L, x, yc, ya); return; }
#endif
    const D2 n0 = d2set(c.n[0]), n1 = d2set(c.n[1]), n2 = d2set(c.n[2]), n3 = d2set(c.n[3]);
    const D2 m1 = d2set(c.m[1]), m2 = d2set(c.m[2]), m3 = d2set(c.m[3]), m4 = d2set(c.m[4]);
    const D2 d1 = d2set(c.d[1]), d2 = d2set(c.d[2]), d3 = d2set(c.d[3]), d4 = d2set(c.d[4]);
    const D2 lim = d2set(tiny);
    // (every lane has its own recurrence: all of them step together, so the pairs do not wait for one another)
    for (int i = 5; i < L; ++i) {
        const double* xi = x + (size_t)i * LN;
        double* yi = yc + (size_t)i * LN;
        for (int p = 0; p < LN; p += 2) {
            D2 acc = d2mul(d2ld(xi + p), n0);
            acc = d2add(acc, d2sub(d2mul(d2ld(xi + p - LN), n1), d2mul(d1, d2ld(yi + p - LN))));
            acc = d2add(acc, d2sub(d2mul(d2ld(xi + p - 2 * LN), n2), d2mul(d2, d2ld(yi + p - 2 * LN))));
            acc = d2add(acc, d2sub(d2mul(d2ld(xi + p - 3 * LN), n3), d2mul(d3, d2ld(yi + p - 3 * LN))));
            acc = d2sub(acc, d2mul(d4, d2ld(yi + p - 4 * LN)));
            d2st(yi + p, d2tiny(acc, lim));
        }
    }
    for (int j = L - 6; j >= 0; --j) {
        const double* xj = x + (size_t)j * LN;
        double* yj = ya + (size_t)j * LN;
        for (int p = 0; p < LN; p += 2) {
            D2 acc = d2sub(d2mul(d2ld(xj + p + LN), m1), d2mul(d1, d2ld(yj + p + LN)));
            acc = d2add(acc, d2sub(d2mul(d2ld(xj + p + 2 * LN), m2), d2mul(d2, d2ld(yj + p + 2 * LN))));
            acc = d2add(acc, d2sub(d2mul(d2ld(xj + p + 3 * LN), m3), d2mul(d3, d2ld(yj + p + 3 * LN))));
            acc = d2add(acc, d2sub(d2mul(d2ld(xj + p + 4 * LN), m4), d2mul(d4, d2ld(yj + p + 4 * LN))));
            d2st(yj + p, d2tiny(acc, lim));
        }
    }
}

// Scratch of the calling thread for one tile of lines.
struct DericheScratch {
    std::vector<double> x, yc, ya;
    void size(int L)
    {
        const size_t n = (size_t)L * kLanes;
        if (x.size() < n) { x.resize(n); yc.resize(n); ya.resize(n); }
    }
};

// Four floats of a pixel as doubles in levels (v * 255, whole levels for 8-bit pictures).
inline void packPixel(const float* p, double* o, bool levels)
{
#ifdef RENO_PIXROWS_SSE2
    const __m128 f = _mm_loadu_ps(p);
    __m128d a = _mm_mul_pd(_mm_cvtps_pd(f), _mm_set1_pd(255.0));
    __m128d b = _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(f, f)), _mm_set1_pd(255.0));
    if (levels) {
        a = _mm_cvtepi32_pd(_mm_cvtpd_epi32(_mm_min_pd(_mm_max_pd(a, _mm_setzero_pd()), _mm_set1_pd(255.0))));
        b = _mm_cvtepi32_pd(_mm_cvtpd_epi32(_mm_min_pd(_mm_max_pd(b, _mm_setzero_pd()), _mm_set1_pd(255.0))));
    }
    _mm_storeu_pd(o, a);
    _mm_storeu_pd(o + 2, b);
#else
    for (int k = 0; k < 4; ++k) {
        const double v = (double)p[k] * 255.0;
        o[k] = levels ? std::nearbyint(std::min(255.0, std::max(0.0, v))) : v;
    }
#endif
}

// (yc + ya) of four values to four floats, rounded to whole levels first when 'levels', then divided by 255 when 'div'.
inline void unpackPixel(const double* a, const double* z, float* o, bool levels, bool div)
{
#ifdef RENO_PIXROWS_SSE2
    __m128d s0 = _mm_add_pd(_mm_loadu_pd(a), _mm_loadu_pd(z)), s1 = _mm_add_pd(_mm_loadu_pd(a + 2), _mm_loadu_pd(z + 2));
    if (levels) {
        s0 = _mm_cvtepi32_pd(_mm_cvtpd_epi32(_mm_min_pd(_mm_max_pd(s0, _mm_setzero_pd()), _mm_set1_pd(255.0))));
        s1 = _mm_cvtepi32_pd(_mm_cvtpd_epi32(_mm_min_pd(_mm_max_pd(s1, _mm_setzero_pd()), _mm_set1_pd(255.0))));
    }
    if (div) { s0 = _mm_div_pd(s0, _mm_set1_pd(255.0)); s1 = _mm_div_pd(s1, _mm_set1_pd(255.0)); }
    _mm_storeu_ps(o, _mm_movelh_ps(_mm_cvtpd_ps(s0), _mm_cvtpd_ps(s1)));
#else
    for (int k = 0; k < 4; ++k) {
        double v = a[k] + z[k];
        if (levels) v = std::nearbyint(std::min(255.0, std::max(0.0, v)));
        o[k] = (float)(div ? v / 255.0 : v);
    }
#endif
}

}  // namespace

void vegasDericheBlurSigma(Buf& b, double sigmaX, double sigmaY, bool levels, bool rawOut)
{
    const int W = b.w, H = b.h;
    if (W <= 0 || H <= 0) return;
    const DericheCoefs cx = vegasDericheCoefs(sigmaX), cy = vegasDericheCoefs(sigmaY);
    std::vector<float> mid((size_t)W * H * 4);
    const int rowTiles = (H + 3) / 4, colTiles = (W + 3) / 4;
    forRows(rowTiles, W * 160, [&](int t) {
        thread_local DericheScratch s;
        s.size(W);
        const int y0 = t * 4;
        for (int i = 0; i < W; ++i)
            for (int r = 0; r < 4; ++r) {
                double* xo = &s.x[(size_t)i * kLanes + r * 4];
                if (y0 + r >= H) { xo[0] = xo[1] = xo[2] = xo[3] = 0.0; continue; }
                packPixel(b.at(i, y0 + r), xo, levels);
            }
        dericheLanes(cx, W, s.x.data(), s.yc.data(), s.ya.data());
        for (int r = 0; r < 4 && y0 + r < H; ++r)
            for (int i = 0; i < W; ++i) {
                const double* a = &s.yc[(size_t)i * kLanes + r * 4];
                const double* z = &s.ya[(size_t)i * kLanes + r * 4];
                unpackPixel(a, z, &mid[((size_t)(y0 + r) * W + i) * 4], false, false);
            }
    });
    // columns, from the bottom row up as VEGAS's frames lie in memory
    forRows(colTiles, H * 160, [&](int t) {
        thread_local DericheScratch s;
        s.size(H);
        const int c0 = t * 4;
        for (int i = 0; i < H; ++i) {
            const float* row = &mid[((size_t)(H - 1 - i) * W + c0) * 4];
            double* xo = &s.x[(size_t)i * kLanes];
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 4; ++k) xo[c * 4 + k] = c0 + c < W ? (double)row[c * 4 + k] : 0.0;
        }
        dericheLanes(cy, H, s.x.data(), s.yc.data(), s.ya.data());
        for (int i = 0; i < H; ++i)
            for (int c = 0; c < 4 && c0 + c < W; ++c) {
                const double* a = &s.yc[(size_t)i * kLanes + c * 4];
                const double* z = &s.ya[(size_t)i * kLanes + c * 4];
                unpackPixel(a, z, b.at(c0 + c, H - 1 - i), levels && !rawOut, !rawOut);
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

bool vegasLinearBlurParams(int W, int H, double angleDeg, double amount, LinearBlurParams& r)
{
    if (!(amount > 1e-6) || W < 2 || H < 2) return false;
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
    int branch, shear;
    double sx, sy;
    if (th < pi / 4) { branch = 1; shear = (int)(std::tan(th) * W * 16384.0); sx = std::cos(x7) * L; sy = 0; }
    else if (th < pi / 2) { branch = 2; shear = (int)(std::tan(pi / 2 - th) * H * 16384.0); sx = 0; sy = std::cos(pi / 2 - x7) * L; }
    else if (th < 3 * pi / 4) { branch = 3; shear = (int)(std::tan(th - pi / 2) * H * 16384.0); sx = 0; sy = std::cos(x7 - pi / 2) * L; }
    else { branch = 4; shear = (int)(std::tan(pi - th) * W * 16384.0); sx = std::cos(pi - x7) * L; sy = 0; }
    r.branch = branch;
    r.shear = std::max(0, shear);
    r.sx = sx;
    r.sy = sy;
    return true;
}

void vegasLinearBlur(const Buf& src, Buf& out, double angleDeg, double amount, bool levels)
{
    const int W = src.w, H = src.h;
    out.ensure(W, H);
    LinearBlurParams lp;
    if (!vegasLinearBlurParams(W, H, angleDeg, amount, lp)) { out.px = src.px; return; }
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
    const int branch = lp.branch, shear = lp.shear;
    const double sx = lp.sx, sy = lp.sy;
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
