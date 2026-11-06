// Reno's own maths functions (src/core/rmath.h): the results must be the same bits on every system. Each function is run over a fixed sample of inputs (a small
// generator, the same on every machine) and the bits of the results are folded into one number that is compared with the value recorded when the functions were checked
// against glibc 2.39 for every float / 10^8 random doubles (tools/rmath_check.cpp). On a glibc 2.39 system the library's own functions are compared as well, input by input.
//   rmath_test
#include "../../src/core/rmath.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif

namespace rm = reno::rm;
using rm::f2u; using rm::u2f; using rm::d2u;

static int fails = 0;
static uint64_t gState = 88172645463325252ull;
static uint64_t next() { gState ^= gState << 13; gState ^= gState >> 7; gState ^= gState << 17; return gState; }

static void fold(uint64_t& h, uint64_t v) { h = (h ^ v) * 1099511628211ull; }
static bool libIsGlibc239()
{
#if defined(__GLIBC__)
    return std::strcmp(gnu_get_libc_version(), "2.39") == 0;
#else
    return false;
#endif
}

// float inputs: a mix of magnitudes in the ranges effects use (|x| < 130 for trig, anything for the rest)
struct Golden { const char* name; uint64_t hash; };
static const Golden kGolden[] = {{"sinf", 0xf6d82b410618f4a1ull}, {"cosf", 0x290819d2b7219d91ull}, {"atanf", 0xe502d149d4bbcc57ull}, {"asinf", 0x212f87994c08b2b2ull}, {"logf", 0x01ec76481bea2a56ull}, {"atan2f", 0x5509f5f8f06fa647ull}, {"exp", 0x365e4f557ee28634ull}, {"log", 0xc35fa7fe45c7cd9aull}, {"pow", 0xb4d85ead6c4b2e06ull},
                                {"expf", 0x27c2cb572045fc8dull}, {"powf", 0xb8a0567c00c3f818ull}, {"hypotf", 0x6e090d64450b2c7eull}, {"hypot", 0x50787df6dbc4bf2aull}};
static void checkGolden(const char* name, uint64_t h)
{
    for (const Golden& g : kGolden)
        if (std::strcmp(g.name, name) == 0) {
            if (g.hash != h) { ++fails; std::printf("FAIL %s: hash %016llx, expected %016llx\n", name, (unsigned long long)h, (unsigned long long)g.hash); }
            return;
        }
}

static float fin(int kind)
{
    const uint64_t r = next();
    switch (kind) {
    case 0: return (float)((double)(int64_t)(r >> 11) / 4503599627370496.0 - 1.0) * (r & 1 ? 130.f : 4.f);                       // trig
    case 1: return (float)((double)(r >> 40) / 16777216.0) * (r & 1 ? 2.f : 1.f) - (r & 2 ? 1.f : 0.f);                          // asin
    case 2: { uint32_t u = (uint32_t)r; return u2f(u); }                                                                            // any float
    default: return (float)((double)(r >> 11) / 9007199254740992.0) * (r & 1 ? 1000.f : 1.f);                                     // log
    }
}

#define CHECKF(name, kind, mine, lib)                                                                                      \
    {                                                                                                                      \
        gState = 88172645463325252ull;                                                                                     \
        uint64_t h = 1469598103934665603ull;                                                                               \
        int bad = 0;                                                                                                       \
        for (int i = 0; i < 200000; ++i) {                                                                                 \
            const float x = fin(kind), y2 = fin(2);                                                                        \
            const float a = mine;                                                                                          \
            if (a == a) fold(h, f2u(a)); else fold(h, 1);                                                                  \
            if (useLib) { volatile float xv = x; const float b = lib; if (!(a != a && b != b) && f2u(a) != f2u(b)) ++bad; } \
        }                                                                                                                  \
        std::printf("%-8s hash %016llx%s\n", name, (unsigned long long)h, bad ? "  DIFFERS FROM THE LIBRARY" : "");        \
        if (bad) { ++fails; std::printf("FAIL %s: %d results differ from glibc\n", name, bad); }                           \
        checkGolden(name, h);                                                                                              \
    }

int main()
{
    const bool useLib = libIsGlibc239();
    std::printf("rmath_test (library comparison %s)\n", useLib ? "on: glibc 2.39" : "off");
    CHECKF("sinf", 0, rm::sinf(x), ::sinf(xv))
    CHECKF("cosf", 0, rm::cosf(x), ::cosf(xv))
    CHECKF("atanf", 2, rm::atanf(x), ::atanf(xv))
    CHECKF("asinf", 1, rm::asinf(x), ::asinf(xv))
    CHECKF("logf", 3, rm::logf(x), ::logf(xv))
    CHECKF("atan2f", 2, rm::atan2f(x, y2), ::atan2f(xv, y2))
    CHECKF("expf", 0, rm::expf(x), ::expf(xv))
    CHECKF("powf", 3, rm::powf(x, y2 * 0.001f), ::powf(xv, y2 * 0.001f))
    CHECKF("hypotf", 2, rm::hypotf(x, y2), ::hypotf(xv, y2))
    // double functions
    gState = 1234567ull;
    uint64_t he = 1469598103934665603ull, hl = he, hp = he, hh = he;
    int bad = 0;
    for (int i = 0; i < 200000; ++i) {
        const double x = (double)(int64_t)(next() >> 11) / 9007199254740992.0 * (i & 1 ? 700.0 : 8.0);
        const double y = (double)(int64_t)(next() >> 11) / 9007199254740992.0 * (i & 2 ? 6.0 : 300.0);
        const double ax = std::fabs(x) + 1e-3;
        const double e = rm::exp(x), l = rm::log(ax), p = rm::pow(ax, y * 0.01);
        // hypot: magnitudes near each other and far apart, and the very small and very large ones
        volatile double ty = y * 0.001;            // (volatile: the input must not depend on a compiler fusing the multiply-add)
        const double hx = x * (i % 7 == 0 ? 1e-160 : (i % 7 == 1 ? 1e150 : 1.0)), hy = (i % 3 ? x * (1.0 + ty) : y);
        const double h = rm::hypot(hx, hy);
        fold(he, d2u(e)); fold(hl, d2u(l)); fold(hp, d2u(p)); fold(hh, d2u(h));
        if (useLib) {
            volatile double xv = x, axv = ax, yv = y * 0.01, hxv = hx, hyv = hy;
            if (d2u(e) != d2u(::exp(xv)) || d2u(l) != d2u(::log(axv)) || d2u(p) != d2u(::pow(axv, yv)) || d2u(h) != d2u(::hypot(hxv, hyv))) ++bad;
        }
    }
    std::printf("exp      hash %016llx\nlog      hash %016llx\npow      hash %016llx\nhypot    hash %016llx\n", (unsigned long long)he, (unsigned long long)hl, (unsigned long long)hp, (unsigned long long)hh);
    checkGolden("exp", he); checkGolden("log", hl); checkGolden("pow", hp); checkGolden("hypot", hh);
    if (bad) { ++fails; std::printf("FAIL double functions differ from glibc: %d\n", bad); }
    // special values
    if (!(std::isinf(rm::exp(1000.0)) && rm::exp(-1000.0) == 0.0 && std::isnan(rm::log(-1.0)) && rm::log(0.0) == -INFINITY && rm::pow(2.0, 10.0) == 1024.0 && rm::pow(-8.0, 1.0 / 3.0) != rm::pow(-8.0, 1.0 / 3.0) && rm::pow(-2.0, 3.0) == -8.0 && rm::pow(0.0, 0.0) == 1.0)) {
        ++fails;
        std::printf("FAIL special values of exp/log/pow\n");
    }
    if (!(rm::sinf(0.f) == 0.f && rm::cosf(0.f) == 1.f && std::isnan(rm::sinf(INFINITY)) && std::isnan(rm::asinf(1.5f)) && rm::atan2f(0.f, -1.f) > 3.14f && std::isinf(-rm::logf(0.f)) && std::isnan(rm::logf(-1.f)))) {
        ++fails;
        std::printf("FAIL special values of the float functions\n");
    }
    std::printf(fails ? "rmath_test: %d FAILED\n" : "rmath_test: all passed\n", fails);
    return fails ? 1 : 0;
}
