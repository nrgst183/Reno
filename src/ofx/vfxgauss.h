// The recursive Gaussian of VEGAS's OpenCL Radial Blur (Vfx1.ofx, Type Gaussian): the coefficient builder (a Deriche-type filter in doubles, with a branch for sigmas over 100) and the
// two kernels (a causal and an anti-causal pass along the columns, written as the card runs them, in floats). A re-implementation of what that code does, read from the user's own VEGAS
// install and checked against the builder itself (to the last bit of the floats it hands the kernels). No dependencies, so a test program can include it alone.
//
// The filter along one line of n values x (all four channels at once): causal  y[i] = sum n[k] x[i-k] - sum d[k] y[i-k]  and anti-causal  z[i] = sum m[k] x[i+k] - sum d[k] z[i+k], the
// line's edge values taken to continue for ever (bn / bm are the edge terms); the result is y + z. The kernels hold the sums as a delay line of partial sums (the form below).
#pragma once
#include "../core/rmath.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxgauss {

// How the passes run their independent lines: fn(first, end) for the lines [first, end) of [0, n), in bands that may run at
// the same time ('work' says how much one line is, for deciding whether to split at all). Unset (a test program that has
// only this header) the lines run in one band on the calling thread; Reno's built-in effects set it to parallelRows
// (builtins.cpp), which takes this very signature. The results do not depend on it: every line stands alone.
using LineRunner = void (*)(int n, size_t work, const std::function<void(int first, int end)>& fn);
inline LineRunner& lineRunner()
{
    static LineRunner runner = nullptr;
    return runner;
}
inline void forLines(int n, size_t work, const std::function<void(int first, int end)>& fn)
{
    if (LineRunner run = lineRunner()) run(n, work, fn);
    else fn(0, n);
}

// The five sets of five numbers the host hands the kernels as float8 (as floats): n (causal numerator), d (feedback, d[0] = 0), m (anti-causal numerator), bn, bm (edge terms).
struct Coefs {
    double n[5], d[5], m[5], bn[5], bm[5];
};

// sigma in pixels (the host gives (float)Strength x 20).
inline Coefs build(double sigma)
{
    Coefs c;
    double N[5] = {0, 0, 0, 0, 0}, D[5] = {0, 0, 0, 0, 0};
    const double s = sigma > 0.001 ? sigma : 0.001;
    double xmm6, xmm7, xmm8, xmm9, xmm10, xmm11, xmm12, xmm0, xmm1, xmm2, xmm3, xmm4, xmm5;
    if (!(s > 100.0)) {
        xmm6 = 1.475 / s;
        xmm8 = rm::sin(xmm6) * 1.021;
        xmm10 = rm::exp(-1.512 / s);
        xmm9 = rm::cos(xmm6);
        xmm12 = rm::exp(-1.556 / s);
        xmm1 = xmm9 * 3.796;
        xmm11 = xmm10 * xmm8;
        xmm6 = xmm9 * 0.8929;
        xmm1 = xmm1 * xmm10;
        xmm11 = xmm11 - xmm1;
        xmm1 = xmm12 * 0.8929;
        xmm0 = xmm10 * xmm6;
        xmm11 = xmm11 + xmm1;
        xmm11 = xmm11 + xmm0;
        xmm7 = rm::exp(-3.024 / s);
        xmm0 = rm::exp(-3.068 / s);
        xmm1 = xmm0 * xmm6;
        xmm0 = xmm0 * xmm8;
        xmm4 = -xmm12;
        xmm3 = xmm12 + xmm12;
        xmm12 = xmm12 * xmm7;
        xmm1 = xmm1 + xmm0;
        xmm2 = xmm11;
        xmm0 = xmm9;
        xmm5 = xmm7 * 1.898;
        xmm0 = xmm0 + xmm9;
        xmm3 = xmm3 * xmm9;
        xmm12 = -xmm12;
        xmm0 = xmm0 * xmm10;
        xmm5 = xmm5 - xmm1;
        xmm1 = 1.0050999999999999;   // (bits 0x3ff014e3bcd35a85, as the builder holds it: one ulp under the double nearest 1.0051)
        xmm2 = xmm2 + xmm1;
        xmm3 = xmm3 * xmm10;
        xmm4 = xmm4 - xmm0;
        xmm2 = xmm2 + xmm5;
        xmm3 = xmm3 + xmm7;
        xmm0 = xmm4 + 1.0;
        xmm2 = xmm2 + 0.0;
        xmm0 = xmm0 + xmm3;
        xmm2 = xmm2 + xmm2;
        xmm0 = xmm0 + xmm12;
        xmm2 = xmm2 / xmm0;
        xmm2 = xmm2 - xmm1;
        N[0] = xmm1 / xmm2;
        N[1] = xmm11 / xmm2;
        N[2] = xmm5 / xmm2;
        N[3] = 0.0 / xmm2;
        N[4] = 0.0;
        D[0] = 0.0;
        D[1] = xmm4;
        D[2] = xmm3;
        D[3] = xmm12;
        D[4] = 0.0;
    } else {
        xmm6 = 0.8448 / s;
        xmm10 = rm::cos(xmm6);
        xmm0 = rm::exp(-1.26 / s);
        xmm8 = 0.9629;
        xmm7 = xmm0;
        xmm9 = xmm10 * xmm8;
        xmm9 = xmm9 * xmm0;
        xmm0 = rm::sin(xmm6);
        xmm0 = xmm0 * 1.942;
        xmm10 = xmm10 + xmm10;
        xmm0 = xmm0 * xmm7;
        xmm10 = xmm10 * xmm7;
        xmm9 = xmm9 - xmm0;
        xmm0 = -2.52 / s;
        xmm9 = -xmm9;
        xmm10 = -xmm10;
        xmm0 = rm::exp(xmm0);
        xmm2 = xmm9 + xmm8;
        xmm1 = xmm10 + 1.0;
        xmm3 = xmm0;
        xmm2 = xmm2 + xmm2;
        xmm1 = xmm1 + xmm0;
        xmm0 = 0.0;
        xmm2 = xmm2 / xmm1;
        xmm2 = xmm2 - xmm8;
        N[0] = xmm8 / xmm2;
        N[1] = xmm9 / xmm2;
        N[2] = xmm0 / xmm2;
        N[3] = 0.0;
        N[4] = 0.0;
        D[0] = 0.0;
        D[1] = xmm10;
        D[2] = xmm3;
        D[3] = 0.0;
        D[4] = 0.0;
    }
    double M[5];
    M[0] = 0.0;
    M[1] = N[1] - D[1] * N[0];
    M[2] = N[2] - D[2] * N[0];
    M[3] = N[3] - D[3] * N[0];
    M[4] = N[4] - N[0] * D[4];
    double SM = M[0];
    SM = SM + 0.0;
    SM = SM + M[1];
    SM = SM + M[2];
    SM = SM + M[3];
    SM = SM + M[4];
    double SN = N[0];
    SN = SN + 0.0;
    SN = SN + N[1];
    SN = SN + N[2];
    SN = SN + N[3];
    SN = SN + N[4];
    double SD = D[0];
    SD = SD + 0.0;
    SD = SD + D[1];
    SD = SD + D[2];
    SD = SD + D[3];
    SD = SD + D[4];
    SD = SD + 1.0;
    double kn, km;
    if (1e-17 > SD || -SD > 1e-17) { kn = 1.0; km = 1.0; }
    else { kn = SN / SD; km = SM / SD; }
    for (int k = 0; k < 5; ++k) {
        c.n[k] = N[k]; c.d[k] = D[k]; c.m[k] = M[k];
        c.bn[k] = kn * D[k];
        c.bm[k] = D[k] * km;
    }
    return c;
}

// The float8 values the kernels get (the five sets cut to floats).
struct Coefsf {
    float n[5], d[5], m[5], bn[5], bm[5];
};
inline Coefsf toFloats(const Coefs& c)
{
    Coefsf f;
    for (int k = 0; k < 5; ++k) { f.n[k] = (float)c.n[k]; f.d[k] = (float)c.d[k]; f.m[k] = (float)c.m[k]; f.bn[k] = (float)c.bn[k]; f.bm[k] = (float)c.bm[k]; }
    return f;
}

// The two passes of kernel k8778a1c20 along the 'h' rows (y) of every column x of a W x H picture of 4 floats a pixel, in = src; causal result a, anti-causal result b (both W x H x 4).
// The state registers are float4s: v11..v15 hold the numerator's partial sums, v16..v19 the feedback's; the first two samples of each pass are special (the edge terms).
// fma: the card fuses a multiply and the add after it (a x b + c with one rounding) when true.
template <class Fma>
inline void passColumns(const Coefsf& cf, const std::vector<float>& in, int W, int H, std::vector<float>& a, std::vector<float>& b, Fma fma)
{
    a.assign((size_t)W * H * 4, 0.f);
    b.assign((size_t)W * H * 4, 0.f);
    auto at = [&](std::vector<float>& v, int x, int y) { return &v[((size_t)y * W + x) * 4]; };
    auto atc = [&](const std::vector<float>& v, int x, int y) { return &v[((size_t)y * W + x) * 4]; };
    // (each column x is filtered on its own: the columns are shared out by forLines)
    forLines(W, (size_t)H * 64, [&](int firstColumn, int endColumn) {
    for (int x = firstColumn; x < endColumn; ++x) {
        for (int pass = 0; pass < 2; ++pass) {
            // pass 0: causal, y = 0 .. H-1 (kernel branch x < W); pass 1: anti-causal, y = H-1 .. 0 (branch W <= x < 2 W)
            const float* nn = pass == 0 ? cf.n : cf.m;
            const float* ee = pass == 0 ? cf.bn : cf.bm;
            const float* d = cf.d;
            std::vector<float>& out = pass == 0 ? a : b;
            auto yOf = [&](int i) { return pass == 0 ? i : H - 1 - i; };
            for (int ch = 0; ch < 4; ++ch) {
                float v11, v12, v13, v14, v15, v16, v17, v18, v19, v22;
                float x0 = atc(in, x, yOf(0))[ch];
                if (pass == 0) {
                    v15 = -ee[4] * x0;
                    v14 = fma(nn[3] - ee[3], x0, v15);
                    v13 = fma(nn[2] - ee[2], x0, v14);
                    v12 = fma(nn[1] - ee[1], x0, v13);
                    v11 = fma(nn[0] - ee[0], x0, v12);
                } else {
                    v15 = (nn[4] - ee[4]) * x0;
                    v14 = fma(nn[3] - ee[3], x0, v15);
                    v13 = fma(nn[2] - ee[2], x0, v14);
                    v12 = fma(nn[1] - ee[1], x0, v13);
                    v11 = fma(nn[0] - ee[0], x0, v12);
                }
                v22 = v11;
                at(out, x, yOf(0))[ch] = v22;
                if (H < 2) continue;
                float x1 = atc(in, x, yOf(1))[ch];
                v16 = 0.0f;
                v17 = v22 * d[0];
                v18 = 0.0f;
                v19 = 0.0f;
                v11 = fma(x1, nn[0], v12);
                v12 = fma(x1, nn[1], v13);
                v13 = fma(x1, nn[2], v14);
                v14 = fma(x1, nn[3], v15);
                v15 = pass == 0 ? 0.0f : x1 * nn[4];
                v22 = v11;
                v22 -= v16;
                at(out, x, yOf(1))[ch] = v22;
                for (int i = 2; i < H; ++i) {
                    const float xi = atc(in, x, yOf(i))[ch];
                    v16 = fma(v22, d[1], v17);
                    v17 = fma(v22, d[2], v18);
                    v18 = fma(v22, d[3], v19);
                    v19 = v22 * d[4];
                    v11 = fma(xi, nn[0], v12);
                    v12 = fma(xi, nn[1], v13);
                    v13 = fma(xi, nn[2], v14);
                    v14 = fma(xi, nn[3], v15);
                    v15 = pass == 0 ? 0.0f : xi * nn[4];
                    v22 = v11;
                    v22 -= v16;
                    at(out, x, yOf(i))[ch] = v22;
                }
            }
        }
    }
    });
}

}  // namespace vfxgauss
}  // namespace ofx
}  // namespace reno
