// Compares Reno's maths functions (src/core/rmath.h) with the C library's, bit for bit.
//   rmath_check <name> [all]       a float function: every one of the 2^32 inputs (or, with a second argument "all" absent, every 16th); two-argument ones: random pairs + edge cases
//   rmath_check <name> <count>     a double function: <count> random inputs (several magnitudes) plus the edge cases
// Build (the C library's functions must really be called, not folded by the compiler):
//   g++ -std=c++17 -O2 -mfma -ffp-contract=off -fno-builtin -I<tree>/src tools/rmath_check.cpp -o rmath_check -lm
// Exit status 0 when every result is bit-identical (NaNs: any NaN equals any NaN).
#include "core/rmath.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <functional>
#include <vector>

using namespace reno::rm;

static bool sameF(float a, float b) { return f2u(a) == f2u(b); }
static bool sameD(double a, double b) { if (a != a && b != b) return true; return d2u(a) == d2u(b); }

struct F1 { const char* name; float (*mine)(float); float (*lib)(float); };
struct F2 { const char* name; float (*mine)(float, float); float (*lib)(float, float); };
struct D1 { const char* name; double (*mine)(double); double (*lib)(double); };
struct D2 { const char* name; double (*mine)(double, double); double (*lib)(double, double); };

#define LIB1(f) [](float x) -> float { volatile float v = x; return ::f(v); }
#define LIBD1(f) [](double x) -> double { volatile double v = x; return ::f(v); }
#define LIBD2(f) [](double x, double y) -> double { volatile double v = x; volatile double w = y; return ::f(v, w); }
#define LIBF2(f) [](float x, float y) -> float { volatile float v = x; volatile float w = y; return ::f(v, w); }

static F1 f1s[] = {
    {"sinf", reno::rm::sinf, LIB1(sinf)},
    {"cosf", reno::rm::cosf, LIB1(cosf)},
    {"atanf", reno::rm::atanf, LIB1(atanf)},
    {"asinf", reno::rm::asinf, LIB1(asinf)},
    {"logf", reno::rm::logf, LIB1(logf)},
    {"expf", reno::rm::expf, LIB1(expf)},
};
static F2 f2s[] = {
    {"atan2f", reno::rm::atan2f, LIBF2(atan2f)},
    {"powf", reno::rm::powf, LIBF2(powf)},
    {"hypotf", reno::rm::hypotf, LIBF2(hypotf)},
};
static D1 d1s[] = {
    {"exp", reno::rm::expd, LIBD1(exp)},
    {"log", reno::rm::logd, LIBD1(log)},
};
static D2 d2s[] = {
    {"pow", reno::rm::powd, LIBD2(pow)},
    {"hypot", reno::rm::hypotd, LIBD2(hypot)},
};

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: rmath_check <function> [count | step]\n"); return 2; }
    const std::string name = argv[1];
    long long bad = 0, n = 0;
    for (const F1& f : f1s) if (name == f.name) {
        const uint64_t step = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;     // 1: all inputs
        for (uint64_t u = 0; u < 0x100000000ull; u += step) {
            const float x = u2f((uint32_t)u);
            const float a = f.mine(x), b = f.lib(x);
            ++n;
            if (!sameF(a, b)) { if (bad < 10) std::printf("%s(%08x = %.9g): mine %08x (%.9g) lib %08x (%.9g)\n", f.name, (unsigned)u, x, f2u(a), a, f2u(b), b); ++bad; }
        }
        std::printf("%s: %lld inputs, %lld differ\n", f.name, n, bad);
        return bad ? 1 : 0;
    }
    for (const F2& f : f2s) if (name == f.name) {
        const long long count = argc > 2 ? std::atoll(argv[2]) : 200000000ll;
        std::mt19937_64 rng(12345);
        std::vector<float> edge = {0.f, -0.f, 1.f, -1.f, INFINITY, -INFINITY, NAN, 1e-45f, -1e-45f, 3e38f, -3e38f, 1e-30f, 0.5f, 2.f};
        for (long long i = 0; i < count; ++i) {
            float x, y;
            if (i < (long long)(edge.size() * edge.size())) { x = edge[i / edge.size()]; y = edge[i % edge.size()]; }
            else {
                // random bit patterns, half of them with exponents kept near each other
                x = u2f((uint32_t)rng()); y = u2f((uint32_t)rng());
                if (i & 1) y = u2f((f2u(x) & 0x80000000u) ^ ((uint32_t)rng() & 0x80000000u) | (((f2u(x) >> 23 & 0xff) + (rng() % 9) - 4) & 0xff) << 23 | ((uint32_t)rng() & 0x7fffff));
            }
            if (name == "powf" && i % 3 == 0) { y = (float)((rng() >> 11) / 9007199254740992.0 * (i % 6 ? 16.0 : 1.0)); x = (float)((rng() >> 11) / 9007199254740992.0 * 16.0 - 8.0); }      // the ranges effects use
            const float a = f.mine(y, x), b = f.lib(y, x);
            ++n;
            if (!sameF(a, b)) { if (bad < 10) std::printf("%s(%.9g, %.9g): mine %08x lib %08x\n", f.name, y, x, f2u(a), f2u(b)); ++bad; }
        }
        std::printf("%s: %lld inputs, %lld differ\n", f.name, n, bad);
        return bad ? 1 : 0;
    }
    for (const D1& f : d1s) if (name == f.name) {
        const long long count = argc > 2 ? std::atoll(argv[2]) : 100000000ll;
        std::mt19937_64 rng(777);
        std::vector<double> edge = {0.0, -0.0, 1.0, -1.0, INFINITY, -INFINITY, NAN, 4.9e-324, -4.9e-324, 2.2250738585072014e-308, 1e308, -1e308, 0.5, 2.0, 710.0, -745.0, 1e-300};
        for (long long i = 0; i < count; ++i) {
            double x;
            if (i < (long long)edge.size()) x = edge[i];
            else switch (i % 4) {
                case 0: x = u2d(rng()); break;                                                       // any bit pattern
                case 1: x = (double)(int64_t)(rng() >> 11) / 9007199254740992.0 * 8.0 - 4.0; break;   // -4..4
                case 2: x = (double)(int64_t)(rng() >> 11) / 9007199254740992.0 * 2000.0 - 1000.0; break;
                default: x = (double)(rng() >> 11) / 9007199254740992.0; break;                      // 0..1
            }
            const double a = f.mine(x), b = f.lib(x);
            ++n;
            if (!sameD(a, b)) { if (bad < 10) std::printf("%s(%.17g = %016llx): mine %016llx lib %016llx\n", f.name, x, (unsigned long long)d2u(x), (unsigned long long)d2u(a), (unsigned long long)d2u(b)); ++bad; }
        }
        std::printf("%s: %lld inputs, %lld differ\n", f.name, n, bad);
        return bad ? 1 : 0;
    }
    for (const D2& f : d2s) if (name == f.name) {
        const long long count = argc > 2 ? std::atoll(argv[2]) : 100000000ll;
        std::mt19937_64 rng(4242);
        std::vector<double> edge = {0.0, -0.0, 1.0, -1.0, INFINITY, -INFINITY, NAN, 4.9e-324, 2.2250738585072014e-308, 1e308, 0.5, 2.0, 3.0, -2.0, 10.0};
        for (long long i = 0; i < count; ++i) {
            double x, y;
            if (i < (long long)(edge.size() * edge.size())) { x = edge[i / edge.size()]; y = edge[i % edge.size()]; }
            else {
                auto pick = [&](int mode) { switch (mode) { case 0: return u2d(rng()); case 1: return (double)(int64_t)(rng() >> 11) / 9007199254740992.0 * 8.0 - 4.0; case 2: return (double)(int64_t)(rng() >> 11) / 9007199254740992.0 * 200.0 - 100.0; default: return (double)(rng() >> 11) / 9007199254740992.0 * 4.0; } };
                x = pick((int)(i % 4)); y = pick((int)((i / 4) % 4));
                if (name == "hypot" && i % 3 == 0) y = x * ((double)(rng() >> 11) / 9007199254740992.0 * 4.0 + 0.05);      // magnitudes near each other (the hard case)
            }
            const double a = f.mine(x, y), b = f.lib(x, y);
            ++n;
            if (!sameD(a, b)) { if (bad < 10) std::printf("%s(%.17g, %.17g): mine %016llx lib %016llx\n", f.name, x, y, (unsigned long long)d2u(a), (unsigned long long)d2u(b)); ++bad; }
        }
        std::printf("%s: %lld inputs, %lld differ\n", f.name, n, bad);
        return bad ? 1 : 0;
    }
    std::printf("unknown function %s\n", name.c_str());
    return 2;
}
