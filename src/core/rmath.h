// Licences: the algorithms, coefficients and tables follow ARM's optimized-routines (Copyright (c) 2017-2018 Arm Limited, MIT) and Sun's fdlibm
// (Copyright (C) 1993 Sun Microsystems, Inc.; freely granted provided the notice is preserved). The full notices are in third_party/rmath/NOTICE.txt.
// Reno's own maths functions. The effects' maths used to call the platform's libm (glibc here, the MSVC runtime on Windows) and the GPU kernels carried copies of
// those results, so a picture could differ between systems by a last bit. These functions are plain C++ (IEEE add, subtract, multiply, divide, square root and
// the explicit fused multiply-add std::fma, nothing else from the C library), so every compiler and every OS gives the same bits, and the Vulkan kernels
// (tools/gpukernels*.py) run the very same steps.
//
// The results are the ones glibc 2.39 gives on an x86-64 machine with FMA (the reference the effects were fitted and tested on): the same algorithms, evaluated in the
// same order with the same fused steps, so Linux output does not change at all. tools/rmath_check.cpp compares each function with the installed libm for every float
// (all 2^32 inputs) or, for the double functions, for 10^8 random inputs plus the edge cases; tests/engine/rmath_test.cpp repeats a sample of that in the repo.
//
// Where each algorithm comes from (written out here, not copied from a library's source files; the coefficients and tables are the published ones):
//   sinf, cosf         the single-precision sine/cosine of ARM's optimized-routines (MIT licence, Szabolcs Nagy): the argument as a double, reduction by a multiple of pi/2
//                      (a multiply-subtract for |x| < 120, the digits of 2/pi for larger), then a polynomial of degree 9/8 in x^2 evaluated in double with fused steps.
//                      The polynomial coefficients are the published ones; the digits of 2/pi are mathematics (computed, not copied).
//   atanf, atan2f      Sun's fdlibm s_atanf.c / e_atan2f.c (permissive licence): argument reduction at 7/16, 11/16, 19/16, 39/16 with a float polynomial.
//   asinf, acosf       the fdlibm layout of e_asin.c in single precision (a polynomial in (1-|x|)/2 with the square root split into high and low parts near 1); the
//                      polynomial is a degree 5 fit.
//   hypotf, hypot     hypotf: sqrt of the double sum of squares, rounded to a float. hypot: Borges' correction (arXiv:1904.09481) of sqrt(x^2 + y^2), unfused, with glibc's scaling
//                      of huge (> 2^511) and tiny (< 2^-500) arguments.
// Not covered here (still the platform's, forwarded at the end of this file): double sin cos atan asin atan2, tan, acos, cbrt, exp2, log2, log1p.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "rmath_tables.h"

// The compiler must not fuse a*b+c on its own (the fused steps are written with std::fma, the rest must round after every operation).
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif

namespace reno {
namespace rm {

inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
// what the C library returns for an invalid argument: a NaN argument comes back quiet (same payload), anything else gives the default NaN of x86 SSE (sign bit set)
inline float nanOf(float x) { return x != x ? u2f(f2u(x) | 0x400000u) : u2f(0xffc00000u); }
inline uint64_t d2u(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline double u2d(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }

namespace detail {

// 2/pi to 192+ bits: entry i is the 32 bits of the hexadecimal expansion a2f9836e4e441529fc2757d1f534ddc0db6295993c439041... that end at hex digit 2(i+1)
// (the first three entries have fewer than 32 bits). reduceLarge reads the three words at k, k+4 and k+8.
static const uint32_t kInvPio4[24] = {
    0x000000a2, 0x0000a2f9, 0x00a2f983, 0xa2f9836e, 0xf9836e4e, 0x836e4e44,
    0x6e4e4415, 0x4e441529, 0x441529fc, 0x1529fc27, 0x29fc2757, 0xfc2757d1,
    0x2757d1f5, 0x57d1f534, 0xd1f534dd, 0xf534ddc0, 0x34ddc0db, 0xddc0db62,
    0xc0db6295, 0xdb629599, 0x6295993c, 0x95993c43, 0x993c4390, 0x3c439041,
};

// the constants below are written as the bit patterns of the doubles/floats they stand for, so no compiler's decimal-to-binary conversion is involved
#define RM_D(bits) (::reno::rm::u2d(bits##ULL))
#define RM_F(bits) (::reno::rm::u2f(bits##u))

struct SinCos {
    // cos: c0 + c1 x^2 + c2 x^4 + c3 x^6 + c4 x^8, sin: x + s1 x^3 + s2 x^5 + s3 x^7 (the coefficients of the reduced argument in [-pi/4, pi/4])
    static double c0() { return 1.0; }
    static double c1() { return RM_D(0xbfdffffffd0c621c); }
    static double c2() { return RM_D(0x3fa55553e1068f19); }
    static double c3() { return RM_D(0xbf56c087e89a359d); }
    static double c4() { return RM_D(0x3ef99343027bf8c3); }
    static double s1() { return RM_D(0xbfc555545995a603); }
    static double s2() { return RM_D(0x3f81107605230bc4); }
    static double s3() { return RM_D(0xbf2994eb3774cf24); }
    static double hpi() { return RM_D(0x3ff921fb54442d18); }        // pi/2
    static double hpiInv() { return RM_D(0x41645f306dc9c883); }     // 2/pi * 2^24
};

// the polynomial of one quadrant: the sine one (odd n: false) or the cosine one (odd n: true) of the reduced argument x (x2 = x*x), with the fused steps
inline double sinfPoly(double x, double x2, bool cosine)
{
    if (!cosine) {
        const double x3 = x * x2;
        const double s1 = std::fma(x2, SinCos::s3(), SinCos::s2());
        const double x7 = x3 * x2;
        const double s = std::fma(x3, SinCos::s1(), x);
        return std::fma(x7, s1, s);
    }
    const double x4 = x2 * x2;
    const double c2 = std::fma(x2, SinCos::c4(), SinCos::c3());
    const double c1 = std::fma(x2, SinCos::c1(), SinCos::c0());
    const double x6 = x4 * x2;
    const double c = std::fma(x4, SinCos::c2(), c1);
    return std::fma(x6, c2, c);
}

// |x| < 120: n = the nearest multiple of pi/2 (found through the scaled float to int conversion), the result x - n pi/2 fused
inline double reduceFast(double x, int* np)
{
    const double r = x * SinCos::hpiInv();
    const int n = ((int32_t)r + 0x800000) >> 24;
    *np = n;
    return std::fma(-(double)n, SinCos::hpi(), x);
}

// |x| >= 120 (finite): the exact fraction of x * 2/pi from the digits of 2/pi, as a 2.62 fixed-point number
inline double reduceLarge(uint32_t xi, int* np)
{
    const uint32_t* arr = &kInvPio4[(xi >> 26) & 15];
    const int shift = (int)((xi >> 23) & 7);
    xi = (xi & 0xffffff) | 0x800000;
    xi <<= shift;
    uint64_t res0 = (uint64_t)(uint32_t)(xi * arr[0]);                       // (only its low 32 bits count)
    const uint64_t res1 = (uint64_t)xi * arr[4];
    const uint64_t res2 = (uint64_t)xi * arr[8];
    res0 = (res2 >> 32) | (res0 << 32);
    res0 += res1;
    const uint64_t n = (res0 + (1ULL << 61)) >> 62;
    res0 -= n << 62;
    const double x = (double)(int64_t)res0;
    *np = (int)n;
    return x * RM_D(0x3c1921fb54442d18);          // pi * 2^-63 (a quarter of pi/2 * 2^-61)
}

}  // namespace detail

// sin(x) as a float, like glibc 2.39's sinf
inline float sinf(float y)
{
    using namespace detail;
    double x = y;
    const uint32_t ay = f2u(y) & 0x7fffffffu;
    const uint32_t top = ay >> 20;
    if (top < 0x3f4) {                                  // |y| < 0.75
        if (top < 0x398) return y;                      // |y| < 2^-12: the argument itself
        return (float)sinfPoly(x, x * x, false);
    }
    int n;
    if (top < 0x42f) {                                  // |y| < 120
        x = reduceFast(x, &n);
        const double s = (n & 1) ? -1.0 : 1.0;
        return (float)(((n & 2) ? -1.0 : 1.0) * sinfPoly(x * s, x * x, (n & 1) != 0));
    }
    if (top < 0x7f8) {
        const uint32_t xi = f2u(y);
        const int sign = (int)(xi >> 31);
        x = reduceLarge(xi, &n);
        const int q = (n + sign) & 3;
        const double s = (q & 1) ? -1.0 : 1.0;
        return (float)(((q & 2) ? -1.0 : 1.0) * sinfPoly(x * s, x * x, (n & 1) != 0));
    }
    return nanOf(y);
}

// cos(x) as a float, like glibc 2.39's cosf
inline float cosf(float y)
{
    using namespace detail;
    double x = y;
    const uint32_t ay = f2u(y) & 0x7fffffffu;
    const uint32_t top = ay >> 20;
    if (top < 0x3f4) {
        if (top < 0x398) return 1.0f;
        return (float)sinfPoly(x, x * x, true);
    }
    int n;
    if (top < 0x42f) {
        x = reduceFast(x, &n);
        const double s = (n & 1) ? -1.0 : 1.0;
        return (float)(((n & 2) ? -1.0 : 1.0) * sinfPoly(x * s, x * x, ((n ^ 1) & 1) != 0));
    }
    if (top < 0x7f8) {
        const uint32_t xi = f2u(y);
        const int sign = (int)(xi >> 31);
        x = reduceLarge(xi, &n);
        const int q = (n + sign) & 3;
        const double s = (q & 1) ? -1.0 : 1.0;
        return (float)(((q & 2) ? -1.0 : 1.0) * sinfPoly(x * s, x * x, ((n ^ 1) & 1) != 0));
    }
    return nanOf(y);
}

// atan(x) as a float (Sun's s_atanf.c: arguments reduced to |x| < 7/16 around 0, 1/2, 1, 3/2 or infinity, an odd/even split polynomial of 11 coefficients)
inline float atanf(float x)
{
    static const uint32_t hi[4] = {0x3eed6338u, 0x3f490fdau, 0x3f7b985eu, 0x3fc90fdau};     // atan(0.5), atan(1), atan(1.5), atan(inf) high parts
    static const uint32_t lo[4] = {0x31ac3769u, 0x33222168u, 0x33140fb4u, 0x33a22168u};
    static const uint32_t at[11] = {0x3eaaaaabu, 0xbe4ccccdu, 0x3e124925u, 0xbde38e38u, 0x3dba2e6eu, 0xbd9d8795u, 0x3d886b35u, 0xbd6ef16bu, 0x3d4bda59u, 0xbd15a221u, 0x3c8569d7u};
    const uint32_t hx = f2u(x);
    const uint32_t ix = hx & 0x7fffffffu;
    int id;
    if (ix >= 0x4c000000u) {                                    // |x| >= 2^25
        if (ix > 0x7f800000u) return x + x;
        return (hx >> 31) ? -u2f(hi[3]) - u2f(lo[3]) : u2f(hi[3]) + u2f(lo[3]);
    }
    if (ix < 0x3ee00000u) {                                     // |x| < 0.4375
        if (ix < 0x31000000u) return x;                         // |x| < 2^-29
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3f980000u) {
            if (ix < 0x3f300000u) { id = 0; x = (2.0f * x - 1.0f) / (2.0f + x); }
            else { id = 1; x = (x - 1.0f) / (x + 1.0f); }
        } else {
            if (ix < 0x401c0000u) { id = 2; x = (x - 1.5f) / (1.0f + 1.5f * x); }
            else { id = 3; x = -1.0f / x; }
        }
    }
    float z = x * x;
    const float w = z * z;
    const float s1 = z * (u2f(at[0]) + w * (u2f(at[2]) + w * (u2f(at[4]) + w * (u2f(at[6]) + w * (u2f(at[8]) + w * u2f(at[10]))))));
    const float s2 = w * (u2f(at[1]) + w * (u2f(at[3]) + w * (u2f(at[5]) + w * (u2f(at[7]) + w * u2f(at[9])))));
    if (id < 0) return x - x * (s1 + s2);
    z = u2f(hi[id]) - ((x * (s1 + s2) - u2f(lo[id])) - x);
    return (hx >> 31) ? -z : z;
}

// atan2(y, x) as a float (Sun's e_atan2f.c)
inline float atan2f(float y, float x)
{
    const float tiny = 1.0e-30f, zero = 0.0f;
    const float piO4 = u2f(0x3f490fdbu), piO2 = u2f(0x3fc90fdbu), pi = u2f(0x40490fdbu), piLo = u2f(0xb3bbbd2eu);
    const uint32_t hx = f2u(x), hy = f2u(y);
    const uint32_t ix = hx & 0x7fffffffu, iy = hy & 0x7fffffffu;
    if (ix > 0x7f800000u || iy > 0x7f800000u) return x + y;
    if (hx == 0x3f800000u) return atanf(y);
    const int m = (int)(((hy >> 31) & 1u) | ((hx >> 30) & 2u));
    if (iy == 0) {
        switch (m) {
        case 0: case 1: return y;
        case 2: return pi + tiny;
        default: return -pi - tiny;
        }
    }
    if (ix == 0) return (hy >> 31) ? -piO2 - tiny : piO2 + tiny;
    if (ix == 0x7f800000u) {
        if (iy == 0x7f800000u) {
            switch (m) {
            case 0: return piO4 + tiny;
            case 1: return -piO4 - tiny;
            case 2: return 3.0f * piO4 + tiny;
            default: return -3.0f * piO4 - tiny;
            }
        }
        switch (m) {
        case 0: return zero;
        case 1: return -zero;
        case 2: return pi + tiny;
        default: return -pi - tiny;
        }
    }
    if (iy == 0x7f800000u) return (hy >> 31) ? -piO2 - tiny : piO2 + tiny;
    const int k = ((int)iy - (int)ix) >> 23;
    float z;
    if (k > 60) z = piO2 + 0.5f * piLo;
    else if ((hx >> 31) && k < -60) z = 0.0f;
    else z = atanf(std::fabs(y / x));
    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - piLo);
    default: return (z - piLo) - pi;
    }
}

// asin(x) as a float (fdlibm's e_asin.c in single precision: |x| < 1/2 a polynomial in x^2; above, in t = (1-|x|)/2 with the square root split into high and low parts)
inline float asinf(float x)
{
    const float a = RM_F(0x3d2cb694), b = RM_F(0x3cc6141e), c = RM_F(0x3d3a3f25), d = RM_F(0x3d9980f2), e = RM_F(0x3e2aaae4);
    const float pio2 = RM_F(0x3fc90fdb), lo = RM_F(0xb33bbd2e), lop = RM_F(0x333bbd2e), pio4 = RM_F(0x3f490fdb);
    const uint32_t u = f2u(x), ix = u & 0x7fffffffu;
    if (ix > 0x3f800000u) return x != x ? nanOf(x) : u2f(0x7fc00000u);
    if (ix == 0x3f800000u) return pio2 * x + x * lo;
    if (ix < 0x3f000000u) {
        if (ix < 0x32000000u) return x;
        const float t0 = x * x;
        return (((((a * t0 + b) * t0 + c) * t0 + d) * t0 + e) * t0) * x + x;
    }
    const float t = (1.0f - std::fabs(x)) * 0.5f;
    const float p = (((((a * t + b) * t + c) * t + d) * t + e) * t);
    const float s = std::sqrt(t);
    float r;
    if (ix > 0x3f799999u) {
        float w1 = p * s + s;
        w1 = w1 + w1;
        r = pio2 - (lop + w1);
    } else {
        const float w = u2f(f2u(s) & 0xfffff000u);
        const float cc = (t - w * w) / (s + w);
        const float pp = (s + s) * p - (lo - (cc + cc));
        const float q = pio4 - (w + w);
        r = pio4 - (pp - q);
    }
    return (u >> 31) ? -r : r;
}

// log(x) as a float (ARM optimized-routines logf, MIT licence: x = 2^k z with z in [0.69, 1.38), a 16 entry table of 1/c and log(c) for the c nearest z, then log(z c^-1) by a
// degree 3 polynomial in r = z/c - 1, all in double with fused steps). The 16 pairs of the table and the three coefficients are the published ones.
inline float logf(float x)
{
    static const uint64_t tab[16][2] = {
        {0x3ff661ec79f8f3beULL, 0xbfd57bf7808caadeULL}, {0x3ff571ed4aaf883dULL, 0xbfd2bef0a7c06ddbULL},
        {0x3ff49539f0f010b0ULL, 0xbfd01eae7f513a67ULL}, {0x3ff3c995b0b80385ULL, 0xbfcb31d8a68224e9ULL},
        {0x3ff30d190c8864a5ULL, 0xbfc6574f0ac07758ULL}, {0x3ff25e227b0b8ea0ULL, 0xbfc1aa2bc79c8100ULL},
        {0x3ff1bb4a4a1a343fULL, 0xbfba4e76ce8c0e5eULL}, {0x3ff12358f08ae5baULL, 0xbfb1973c5a611cccULL},
        {0x3ff0953f419900a7ULL, 0xbfa252f438e10c1eULL}, {0x3ff0000000000000ULL, 0x0000000000000000ULL},
        {0x3fee608cfd9a47acULL, 0x3faaa5aa5df25984ULL}, {0x3feca4b31f026aa0ULL, 0x3fbc5e53aa362eb4ULL},
        {0x3feb2036576afce6ULL, 0x3fc526e57720db08ULL}, {0x3fe9c2d163a1aa2dULL, 0x3fcbc2860d224770ULL},
        {0x3fe886e6037841edULL, 0x3fd1058bc8a07ee1ULL}, {0x3fe767dcf5534862ULL, 0x3fd4043057b6ee09ULL}};
    const double ln2 = RM_D(0x3fe62e42fefa39ef), a0 = RM_D(0xbfd00ea348b88334), a1 = RM_D(0x3fd5575b0be00b6a), a2 = RM_D(0xbfdffffef20a4123);
    uint32_t ix = f2u(x);
    if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u) {                     // zero, subnormal, negative, infinity, NaN
        if (ix * 2 == 0) return -INFINITY;
        if (ix == 0x7f800000u) return x;
        if ((ix & 0x80000000u) || ix * 2 >= 0xff000000u) return nanOf(x);
        ix = f2u(x * 8388608.0f);                                               // subnormal: scale by 2^23
        ix -= 23u << 23;
    }
    const uint32_t tmp = ix - 0x3f330000u;
    const int i = (int)((tmp >> 19) % 16u);
    const int k = (int32_t)tmp >> 23;
    const uint32_t iz = ix - (tmp & (0x1ffu << 23));
    const double invc = u2d(tab[i][0]), logc = u2d(tab[i][1]);
    const double z = (double)u2f(iz);
    const double r = std::fma(z, invc, -1.0);
    const double y0 = std::fma((double)k, ln2, logc);
    const double r2 = r * r;
    double y = std::fma(a1, r, a2);
    y = std::fma(a0, r2, y);
    y = std::fma(y, r2, y0 + r);
    return (float)y;
}

// expf(x) (ARM optimized-routines: x = (k + r) ln2/32, a table of 2^(k/32), a cubic in r, all in double with fused steps)
inline float expf(float x)
{
    using namespace tables;
    const uint32_t ux = f2u(x);
    const uint32_t abstop = (ux >> 20) & 0x7ffu;
    if (abstop >= 0x42bu) {                                   // |x| >= 88 or NaN
        if (ux == 0xff800000u) return 0.0f;
        if (abstop >= 0x7f8u) return x + x;
        if (x > u2f(0x42b17217u)) return INFINITY;
        if (x < u2f(0xc2cff1b4u)) return 0.0f;
    }
    const double invln2N = u2d(kExp2fHead[5]), shift = u2d(kExp2fHead[4]);
    const double c0 = u2d(kExp2fHead[6]), c1 = u2d(kExp2fHead[7]), c2 = u2d(kExp2fHead[8]);
    const double xd = (double)x;
    double z = invln2N * xd;
    double kd = z + shift;
    const uint64_t ki = d2u(kd);
    kd -= shift;
    double r = z - kd;
    uint64_t t = kExp2fTab[ki % 32];
    t += ki << 47;
    const double s = u2d(t);
    z = std::fma(c0, r, c1);
    const double r2 = r * r;
    double y = std::fma(c2, r, 1.0);
    y = std::fma(z, r2, y);
    y = y * s;
    return (float)y;
}

namespace detail {
inline int checkintf(uint32_t iy)
{
    const int e = (int)(iy >> 23 & 0xff);
    if (e < 0x7f) return 0;
    if (e > 0x7f + 23) return 2;
    if (iy & ((1u << (0x7f + 23 - e)) - 1)) return 0;
    if (iy & (1u << (0x7f + 23 - e))) return 1;
    return 2;
}
inline bool zeroinfnanf(uint32_t i) { return 2u * i - 1u >= 2u * 0x7f800000u - 1u; }
}  // namespace detail

// pow(x, y) as a float (ARM optimized-routines powf: log2 of x in double by a 16 entry table and a degree 4 polynomial, multiplied by y, then 2^ of it by the expf method)
inline float powf(float x, float y)
{
    using namespace detail;
    using namespace tables;
    uint32_t signBias = 0;
    uint32_t ix = f2u(x), iy = f2u(y);
    if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u || zeroinfnanf(iy)) {
        if (zeroinfnanf(iy)) {
            if (2 * iy == 0) return ((ix & 0x7fc00000u) == 0x7f800000u && (ix << 9)) ? x + y : 1.0f;
            if (ix == 0x3f800000u) return ((iy & 0x7fc00000u) == 0x7f800000u && (iy << 9)) ? x + y : 1.0f;
            if (2 * ix > 2u * 0x7f800000u || 2 * iy > 2u * 0x7f800000u) return x + y;
            if (2 * ix == 2 * 0x3f800000u) return 1.0f;
            if ((2 * ix < 2 * 0x3f800000u) == !(iy & 0x80000000u)) return 0.0f;
            return y * y;
        }
        if (zeroinfnanf(ix)) {
            float x2 = x * x;
            if ((ix & 0x80000000u) && checkintf(iy) == 1) { x2 = -x2; }
            return (iy & 0x80000000u) ? 1 / x2 : x2;
        }
        if (ix & 0x80000000u) {
            const int yint = checkintf(iy);
            if (yint == 0) return nanOf(x);
            if (yint == 1) signBias = 0x800u << 5;
            ix &= 0x7fffffffu;
        }
        if (ix < 0x00800000u) {
            ix = f2u(x * 8388608.0f);
            ix &= 0x7fffffffu;
            ix -= 23u << 23;
        }
    }
    // log2(x)
    const uint32_t tmp = ix - 0x3f330000u;
    const int i = (int)((tmp >> 19) % 16u);
    const uint32_t top = tmp & 0xff800000u;
    const uint32_t iz = ix - top;
    const int k = (int32_t)top >> 23;
    const double invc = u2d(kPowfTab[2 * i]), logc = u2d(kPowfTab[2 * i + 1]);
    const double zz = (double)u2f(iz);
    const double r = std::fma(zz, invc, -1.0);
    const double y0 = logc + (double)k;
    const double r2 = r * r;
    const double A0 = u2d(kPowfPoly[0]), A1 = u2d(kPowfPoly[1]), A2 = u2d(kPowfPoly[2]), A3 = u2d(kPowfPoly[3]), A4 = u2d(kPowfPoly[4]);
    double yy = std::fma(A0, r, A1);
    const double p = std::fma(A2, r, A3);
    const double r4 = r2 * r2;
    double q = std::fma(A4, r, y0);
    q = std::fma(p, r2, q);
    yy = std::fma(yy, r4, q);
    const double logx = yy;
    const double ylogx = (double)y * logx;
    if ((d2u(ylogx) >> 47 & 0xffff) >= d2u(126.0) >> 47) {
        if (ylogx > u2d(0x405fffffffd1d571ULL)) return signBias ? -INFINITY : INFINITY;
        if (ylogx <= -150.0) return signBias ? -0.0f : 0.0f;
    }
    // 2^ylogx
    const double shift = u2d(kExp2fHead[0]);
    const double c0 = u2d(kExp2fHead[1]), c1 = u2d(kExp2fHead[2]), c2 = u2d(kExp2fHead[3]);
    double kd = ylogx + shift;
    const uint64_t ki = d2u(kd);
    kd -= shift;
    const double rr = ylogx - kd;
    uint64_t t = kExp2fTab[ki % 32];
    const uint64_t ski = ki + signBias;
    t += ski << 47;
    const double s = u2d(t);
    double z = std::fma(c0, rr, c1);
    const double rr2 = rr * rr;
    double res = std::fma(c2, rr, 1.0);
    res = std::fma(z, rr2, res);
    res = res * s;
    return (float)res;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- double exp, log, pow
// The double exp, log and pow of ARM's optimized-routines (MIT licence, Szabolcs Nagy): table driven (128 subintervals), a short polynomial, error below 0.52 ulp. glibc 2.39
// uses these same algorithms, so the results are its results; the FMA build of the library is what this machine runs, so the steps it fuses are fused here (std::fma).
namespace detail {

inline double nanOfD(double x) { return x != x ? u2d(d2u(x) | 0x8000000000000ULL) : u2d(0xfff8000000000000ULL); }
inline uint32_t top12(double x) { return (uint32_t)(d2u(x) >> 52); }
inline double dh(uint64_t bits) { return u2d(bits); }

// the results of exp whose scale does not fit: large (k > 0) and in the subnormal range (k < 0); tmp is the fraction, sbits the scale's bits, ki the table index word
inline double expSpecial(double tmp, uint64_t sbits, uint64_t ki)
{
    double scale, y;
    if ((ki & 0x80000000u) == 0) {
        sbits -= 1009ULL << 52;
        scale = u2d(sbits);
        return dh(0x7f00000000000000ULL) * std::fma(scale, tmp, scale);              // 2^1009 * ...
    }
    sbits += 1022ULL << 52;
    scale = u2d(sbits);
    const double st = scale * tmp;                          // (used twice, so the library does not fuse it)
    y = scale + st;
    if (std::fabs(y) < 1.0) {
        double one = 1.0;
        if (y < 0.0) one = -1.0;
        double lo = scale - y + st;
        const double hi = one + y;
        lo = one - hi + y + lo;
        y = (hi + lo) - one;
        if (y == 0) y = u2d(sbits & 0x8000000000000000ULL);
    }
    return dh(0x0010000000000000ULL) * y;                                                // 2^-1022 * ...
}

// exp(x + xtail) times -1 when signBias is set (pow's negative results): the common core
inline double expCore(double x, double xtail, uint64_t signBias)
{
    using namespace tables;
    const double invln2N = dh(kExpHead[0]), shift = dh(kExpHead[1]), negln2hiN = dh(kExpHead[2]), negln2loN = dh(kExpHead[3]);
    const double c2 = dh(kExpHead[4]), c3 = dh(kExpHead[5]), c4 = dh(kExpHead[6]), c5 = dh(kExpHead[7]);
    uint32_t abstop = top12(x) & 0x7ff;
    if (abstop - 0x3c9u >= 0x408u - 0x3c9u) {                // |x| < 2^-54, |x| >= 512 (or NaN)
        if (abstop - 0x3c9u >= 0x80000000u) {
            const double one = 1.0 + x;
            return signBias ? -one : one;
        }
        if (abstop >= 0x409u) {                              // |x| >= 1024
            if (d2u(x) >> 63) return signBias ? -0.0 : 0.0;
            return signBias ? -INFINITY : INFINITY;
        }
        abstop = 0;
    }
    const double z = invln2N * x;
    double kd = z + shift;
    const uint64_t ki = d2u(kd);
    kd -= shift;
    double r = std::fma(kd, negln2loN, std::fma(kd, negln2hiN, x));
    r += xtail;
    const uint64_t idx = 2 * (ki % 128);
    const uint64_t top = (ki + signBias) << 45;
    const double tail = dh(kExpTab[idx]);
    const uint64_t sbits = kExpTab[idx + 1] + top;
    const double r2 = r * r;
    const double tmp = std::fma(r2 * r2, std::fma(r, c5, c4), std::fma(r2, std::fma(r, c3, c2), tail + r));
    if (abstop == 0) return expSpecial(tmp, sbits, ki);
    const double scale = u2d(sbits);
    return std::fma(scale, tmp, scale);
}

}  // namespace detail

// exp(x), like glibc 2.39's
inline double expd(double x)
{
    using namespace detail;
    const uint32_t abstop = top12(x) & 0x7ff;
    if (abstop >= 0x409u) {                                  // |x| >= 1024, inf, NaN
        if (d2u(x) == d2u(-INFINITY)) return 0.0;
        if (abstop >= 0x7ffu) return 1.0 + x;
    }
    return detail::expCore(x, 0.0, 0);
}

// log(x), like glibc 2.39's
inline double logd(double x)
{
    using namespace detail;
    using namespace tables;
    const double ln2hi = dh(kLogHead[0]), ln2lo = dh(kLogHead[1]);
    double a[5], b[11];
    for (int i = 0; i < 5; ++i) a[i] = dh(kLogHead[2 + i]);
    for (int i = 0; i < 11; ++i) b[i] = dh(kLogHead[7 + i]);
    uint64_t ix = d2u(x);
    const uint32_t top = (uint32_t)(ix >> 48);
    const uint64_t LO = 0x3fee000000000000ULL, HI = 0x3ff1090000000000ULL;
    if (ix - LO < HI - LO) {                                 // x in [1 - 2^-4, 1 + 0x1.09p-4): a polynomial in r = x - 1
        if (ix == 0x3ff0000000000000ULL) return 0;
        const double r = x - 1.0;
        const double r2 = r * r, r3 = r * r2;
        const double i2 = std::fma(r3, b[10], std::fma(r2, b[9], std::fma(r, b[8], b[7])));
        const double i1 = std::fma(r3, i2, std::fma(r2, b[6], std::fma(r, b[5], b[4])));
        const double yp = std::fma(r3, i1, std::fma(r2, b[3], std::fma(r, b[2], b[1])));
        double w = r * 134217728.0;
        const double rhi = r + w - w;
        const double rlo = r - rhi;
        w = rhi * rhi * b[0];
        const double hi = r + w;
        double lo = r - hi + w;
        lo = std::fma(b[0] * rlo, rhi + r, lo);
        double y = std::fma(r3, yp, lo);                    // (r3 * yp + lo fused)
        y += hi;
        return y;
    }
    if (top - 0x0010u >= 0x7ff0u - 0x0010u) {               // zero, subnormal, negative, infinity, NaN
        if (ix * 2 == 0) return -INFINITY;
        if (ix == 0x7ff0000000000000ULL) return x;
        if ((top & 0x8000u) || (top & 0x7ff0u) == 0x7ff0u) return nanOfD(x);
        ix = d2u(x * 4503599627370496.0);                    // subnormal: scale by 2^52
        ix -= 52ULL << 52;
    }
    const uint64_t tmp = ix - 0x3fe6000000000000ULL;
    const int i = (int)((tmp >> 45) % 128);
    const int64_t k = (int64_t)tmp >> 52;
    const uint64_t iz = ix - (tmp & (0xfffULL << 52));
    const double invc = dh(kLogTab[2 * i]), logc = dh(kLogTab[2 * i + 1]);
    const double z = u2d(iz);
    const double r = std::fma(z, invc, -1.0);
    const double kd = (double)k;
    const double w = std::fma(kd, ln2hi, logc);
    const double hi = w + r;
    const double lo = std::fma(kd, ln2lo, w - hi + r);
    const double r2 = r * r;
    const double poly = std::fma(r2, std::fma(r, a[4], a[3]), std::fma(r, a[2], a[1]));
    return std::fma(r * r2, poly, std::fma(r2, a[0], lo)) + hi;
}

namespace detail {
// 0: not an integer, 1: odd, 2: even
inline int checkint(uint64_t iy)
{
    const int e = (int)(iy >> 52 & 0x7ff);
    if (e < 0x3ff) return 0;
    if (e > 0x3ff + 52) return 2;
    if (iy & ((1ULL << (0x3ff + 52 - e)) - 1)) return 0;
    if (iy & (1ULL << (0x3ff + 52 - e))) return 1;
    return 2;
}
inline bool zeroinfnan(uint64_t i) { return 2 * i - 1 >= 2 * 0x7ff0000000000000ULL - 1; }

// log(x) as a high part and a tail (about 68 bits), for pow
inline double powLog(uint64_t ix, double* tail)
{
    using namespace tables;
    const double ln2hi = dh(kPowHead[0]), ln2lo = dh(kPowHead[1]);
    double A[7];
    for (int j = 0; j < 7; ++j) A[j] = dh(kPowHead[2 + j]);
    const uint64_t tmp = ix - 0x3fe6955500000000ULL;
    const int i = (int)((tmp >> 45) % 128);
    const int64_t k = (int64_t)tmp >> 52;
    const uint64_t iz = ix - (tmp & (0xfffULL << 52));
    const double z = u2d(iz);
    const double kd = (double)k;
    const double invc = dh(kPowTab[4 * i]), logc = dh(kPowTab[4 * i + 2]), logctail = dh(kPowTab[4 * i + 3]);
    const double r = std::fma(z, invc, -1.0);
    const double t1 = std::fma(kd, ln2hi, logc);
    const double t2 = t1 + r;
    const double lo1 = std::fma(kd, ln2lo, logctail);
    const double lo2 = t1 - t2 + r;
    const double ar = A[0] * r;
    const double ar2 = r * ar;
    const double ar3 = r * ar2;
    const double hi = t2 + ar2;
    const double lo3 = std::fma(ar, r, -ar2);
    const double lo4 = t2 - hi + ar2;
    const double in = std::fma(ar2, std::fma(r, A[6], A[5]), std::fma(r, A[4], A[3]));
    const double pm = std::fma(ar2, in, std::fma(r, A[2], A[1]));
    const double lo = std::fma(ar3, pm, lo1 + lo2 + lo3 + lo4);          // (ar3 * pm added to the sum, fused)
    const double y = hi + lo;
    *tail = hi - y + lo;
    return y;
}
}  // namespace detail

// pow(x, y), like glibc 2.39's
inline double powd(double x, double y)
{
    using namespace detail;
    uint64_t signBias = 0;
    uint64_t ix = d2u(x);
    const uint64_t iy = d2u(y);
    uint32_t topx = top12(x);
    const uint32_t topy = top12(y);
    if (topx - 0x001u >= 0x7ffu - 0x001u || (topy & 0x7ffu) - 0x3beu >= 0x43eu - 0x3beu) {
        if (zeroinfnan(iy)) {
            if (2 * iy == 0) return (ix & 0x7ff8000000000000ULL) == 0x7ff0000000000000ULL && (ix << 13) ? x + y : 1.0;
            if (ix == 0x3ff0000000000000ULL) return ((iy & 0x7ff8000000000000ULL) == 0x7ff0000000000000ULL && (iy << 13)) ? x + y : 1.0;
            if (2 * ix > 2 * 0x7ff0000000000000ULL || 2 * iy > 2 * 0x7ff0000000000000ULL) return x + y;
            if (2 * ix == 2 * 0x3ff0000000000000ULL) return 1.0;
            if ((2 * ix < 2 * 0x3ff0000000000000ULL) == !(iy >> 63)) return 0.0;
            return y * y;
        }
        if (zeroinfnan(ix)) {
            double x2 = x * x;
            if ((ix >> 63) && checkint(iy) == 1) x2 = -x2;
            if (2 * ix == 0 && (iy >> 63)) return (ix >> 63) && checkint(iy) == 1 ? -INFINITY : INFINITY;
            return (iy >> 63) ? 1 / x2 : x2;
        }
        if (ix >> 63) {
            const int yint = checkint(iy);
            if (yint == 0) return nanOfD(x);
            if (yint == 1) signBias = 0x800ULL << 7;
            ix &= 0x7fffffffffffffffULL;
            topx &= 0x7ff;
        }
        if ((topy & 0x7ffu) - 0x3beu >= 0x43eu - 0x3beu) {
            if (ix == 0x3ff0000000000000ULL) return 1.0;
            if ((topy & 0x7ffu) < 0x3beu) return ix > 0x3ff0000000000000ULL ? 1.0 + y : 1.0 - y;
            return (ix > 0x3ff0000000000000ULL) == (topy < 0x800u) ? INFINITY : 0.0;
        }
        if (topx == 0) {
            ix = d2u(x * 4503599627370496.0);
            ix &= 0x7fffffffffffffffULL;
            ix -= 52ULL << 52;
        }
    }
    double lo;
    const double hi = powLog(ix, &lo);
    const double ehi = y * hi;
    const double elo = std::fma(y, lo, std::fma(y, hi, -ehi));
    return expCore(ehi, elo, signBias);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------- hypot
// hypotf: the square root of the sum of the two squares in double (each square of a float is exact in a double, the sum rounds once), rounded to a float - this is what
// glibc 2.39 gives for every pair (checked against the library on 10^9 random pairs and all special values).
inline float hypotf(float x, float y)
{
    const uint32_t ix = f2u(x) & 0x7fffffffu, iy = f2u(y) & 0x7fffffffu;
    if (ix == 0x7f800000u || iy == 0x7f800000u) return u2f(0x7f800000u);          // an infinity wins over a NaN
    if (ix > 0x7f800000u || iy > 0x7f800000u) return x + y;                       // a NaN (quiet)
    const double dx = (double)x, dy = (double)y;
    return (float)std::sqrt(dx * dx + dy * dy);
}
namespace detail {
// Borges' correction of the square root of the sum of squares ("An improved algorithm for hypot(a,b)", 2019): h = sqrt(x^2 + y^2) is corrected by the error of its
// square worked out from exact differences (no fused multiply-add, so every system gives the same bits). x >= y >= 0, both neither huge nor tiny.
inline double hypotKernel(double x, double y)
{
    double h = std::sqrt(x * x + y * y);
    double t1, t2;
    if (h <= 2.0 * y) {
        const double delta = h - y;
        t1 = x * (2.0 * delta - x);
        t2 = (delta - 2.0 * (x - y)) * delta;
    } else {
        const double delta = h - x;
        t1 = 2.0 * delta * (x - 2.0 * y);
        t2 = (4.0 * delta - y) * y + delta * delta;
    }
    h -= (t1 + t2) / (2.0 * h);
    return h;
}
}  // namespace detail
inline double hypotd(double x, double y)
{
    const uint64_t ux = d2u(x) & 0x7fffffffffffffffULL, uy = d2u(y) & 0x7fffffffffffffffULL;
    if (ux >= 0x7ff0000000000000ULL || uy >= 0x7ff0000000000000ULL) {
        if (ux == 0x7ff0000000000000ULL || uy == 0x7ff0000000000000ULL) return u2d(0x7ff0000000000000ULL);
        return x + y;
    }
    double ax = u2d(ux), ay = u2d(uy);
    if (ax < ay) { const double t = ax; ax = ay; ay = t; }
    if (ay == 0.0) return ax;
    const double kScale = u2d(0x1a70000000000000ULL);      // 2^-600
    const double kLarge = u2d(0x5fe0000000000000ULL);      // 2^511
    const double kTiny = u2d(0x20b0000000000000ULL);       // 2^-500
    const double kEps = u2d(0x3c90000000000000ULL);        // 2^-54
    if (ax > kLarge) {
        if (ay <= ax * kEps) return ax + ay;
        return detail::hypotKernel(ax * kScale, ay * kScale) / kScale;
    }
    if (ay < kTiny) {
        if (ax >= ay / kEps) return ax + ay;
        return detail::hypotKernel(ax / kScale, ay / kScale) * kScale;
    }
    if (ay <= ax * kEps) return ax + ay;
    return detail::hypotKernel(ax, ay);
}
inline float hypot(float x, float y) { return hypotf(x, y); }
inline double hypot(double x, double y) { return hypotd(x, y); }
template <class A, class B, class = typename std::enable_if<std::is_arithmetic<A>::value && std::is_arithmetic<B>::value && !(std::is_same<A, float>::value && std::is_same<B, float>::value) && !(std::is_same<A, double>::value && std::is_same<B, double>::value)>::type>
inline double hypot(A x, B y) { return hypotd((double)x, (double)y); }

// pow, exp and log with the C++ library's argument rules (<cmath>): two floats (one float) give the float function and a float result; two doubles the double function;
// anything else (an integer, a float with a double) is worked out in double. This is what lets the effects' std::pow(...) calls change to rm::pow(...) with the same meaning.
inline float pow(float x, float y) { return powf(x, y); }
inline double pow(double x, double y) { return powd(x, y); }
template <class A, class B, class = typename std::enable_if<std::is_arithmetic<A>::value && std::is_arithmetic<B>::value && !(std::is_same<A, float>::value && std::is_same<B, float>::value) && !(std::is_same<A, double>::value && std::is_same<B, double>::value)>::type>
inline double pow(A x, B y) { return powd((double)x, (double)y); }
inline float exp(float x) { return expf(x); }
inline double exp(double x) { return expd(x); }
template <class A, class = typename std::enable_if<std::is_integral<A>::value>::type> inline double exp(A x) { return expd((double)x); }
inline float log(float x) { return logf(x); }
inline double log(double x) { return logd(x); }
template <class A, class = typename std::enable_if<std::is_integral<A>::value>::type> inline double log(A x) { return logd((double)x); }

// sin, cos, atan, asin and atan2 with the C++ library's argument rules. Floats use the functions above (bit-identical everywhere). DOUBLES STILL GO TO THE PLATFORM'S C
// LIBRARY: glibc's double sin/cos/atan/asin/atan2 are IBM's Accurate Mathematical Library (LGPL, long tables), for which no permissive source gives the same bits, so
// they are not reproduced here yet (rm::sin(double) etc. are the single place to change when they are).
#define RM_TRIG1(name, fl)                                                                                             \
    inline float name(float x) { return fl(x); }                                                                       \
    inline double name(double x) { return std::name(x); }                                                              \
    template <class A, class = typename std::enable_if<std::is_integral<A>::value>::type> inline double name(A x) { return std::name((double)x); }
RM_TRIG1(sin, sinf)
RM_TRIG1(cos, cosf)
RM_TRIG1(atan, atanf)
RM_TRIG1(asin, asinf)
#undef RM_TRIG1
inline float atan2(float y, float x) { return atan2f(y, x); }
inline double atan2(double y, double x) { return std::atan2(y, x); }
template <class A, class B, class = typename std::enable_if<std::is_arithmetic<A>::value && std::is_arithmetic<B>::value && !(std::is_same<A, float>::value && std::is_same<B, float>::value) && !(std::is_same<A, double>::value && std::is_same<B, double>::value)>::type>
inline double atan2(A y, B x) { return std::atan2((double)y, (double)x); }

// The rest of the maths library that effects use. NOT reproduced yet (glibc's cbrt, tan and acos are LGPL code, log1p/exp2/log2 are compiled differently on every system):
// these forward to the platform's library. They exist so that the effects have no direct libm call left: rm::tan, rm::acos, rm::cbrt, rm::exp2, rm::log2 and rm::log1p
// are the single places to change when an exact copy is written. Argument rules as <cmath>'s (float in, float out; double or integer in, double out).
#define RM_LIBM1(name)                                                                                                 \
    inline float name(float x) { return std::name(x); }                                                                \
    inline double name(double x) { return std::name(x); }                                                              \
    template <class A, class = typename std::enable_if<std::is_integral<A>::value>::type> inline double name(A x) { return std::name((double)x); }
RM_LIBM1(tan)
RM_LIBM1(acos)
RM_LIBM1(cbrt)
RM_LIBM1(exp2)
RM_LIBM1(log2)
RM_LIBM1(log1p)
#undef RM_LIBM1

#undef RM_D
#undef RM_F

}  // namespace rm
}  // namespace reno

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
