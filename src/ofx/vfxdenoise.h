// VEGAS Denoise ("Noise Reduction", MagixCVFx.ofx), as its OpenCL kernels draw it (what VEGAS shows with GPU acceleration on): a re-implementation of what the kernels do
// (read from the user's own VEGAS install; checked against VEGAS's captured pictures), not a fit. No dependencies, so a test program can include it alone.
//
// What it does, in the order the kernels run (the picture is as the card holds it: rows bottom first, which matters for the border, see texel()):
//   1. RGB -> YUV (BT.601, floats, Y = .299 R + .587 G + .114 B, U and V centred on .5) into an 8-bit picture (the card's 8-bit store, store12()).
//   2. The three planes are cut to bytes (255 x value cut to a whole number, towards zero) into planes with a border of 13 pixels (3 for the patch + 10 for the search). The border is read
//      through a sampler with normalized coordinates, mirrored repeat, nearest, at (pixel + 1) / size: see texel() for what that gives (a shift of one pixel in some columns and rows, see the 2^-21 fixed point rule).
//   3. Each plane with a strength above 0 (Luminance for Y, Color for U and V) is denoised: every pixel is the weighted mean of the 21 x 21 pixels round it, weight table[(7 x 7 sum of
//      absolute differences) >> 6]; table[i] = (int)(19096 exp(-(i 64/49)^2 / h^2)), 0 when that is under 0.001 of 19096; the sum is whole numbers, the mean an integer division.
//   4. Back to YUV floats (byte / 255, planes not denoised keep their byte), then to RGB (R = Y + 1.13983 V, G = Y - .39465 U - .5806 V, B = Y + 2.03211 U; U and V less .5) into an 8-bit picture.
//   5. Amount (Sharpening) > 0: a 3 x 3 kernel (corners -1, edges -2, centre c, divided by c - 12; c from the amount) over the 4 channels, edges repeated; into the 8-bit result.
// Both strengths 0 (or below the float epsilon): the picture is passed on as it is (the sharpening still runs).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxdn {

// The card's write of a float to an 8-bit UNORM picture: floor(v x 4096) is taken (v cut to 0..1, NaN 0) and scaled to a byte as (m x 255 + 2047) >> 12.
// (This is a 12-bit step, not round to nearest; found by fitting VEGAS's pictures, bit exact on all captured Denoise pictures.)
inline int store12(float v)
{
    if (!(v > 0.f)) return 0;
    if (v > 1.f) v = 1.f;
    const int m = (int)std::floor(v * 4096.f);
    return (m * 255 + 2047) >> 12;
}

// The pixel the sampler gives for border-plane index i (-13 .. n + 12) of an image of n pixels along one axis. The kernel reads at coordinate (i + 1) / n with the sampler
// CLK_NORMALIZED_COORDS_TRUE | MIRRORED_REPEAT | NEAREST. What the card does with it (found by trying the float paths against every border row of the 320 x 180 captures, all of
// them reproduced, rows and columns): the coordinate is s = (i + 1) x (1 / n) in float (the reciprocal rounded to float first), it is cut down (towards minus infinity) to a
// whole number of 2^-21 (a fixed point coordinate), the texel is that times n cut down again, and the mirrored repeat is then done on the texel number (-1 - t below, 2 n - 1 - t above).
// So a texel i + 1 is read only when s happens to sit exactly on the 2^-21 grid and is not under (i + 1) / n (all exact cases, and a few more like row 57 of 180).
inline int texel(int i, int n)
{
    const int c = i + 1;
    const float r = 1.0f / (float)n;
    const float s = (float)c * r;
    const long long U = (long long)std::floor((double)s * 2097152.0);
    long long t = (U * (long long)n) >> 21;                  // arithmetic shift = floor, also below zero
    const long long p = 2LL * n;
    t %= p;
    if (t < 0) t += p;
    if (t >= n) t = p - 1 - t;
    return (int)t;
}

// The kernel's weight table for strength h: 196 entries.
inline void weightTable(double hd, int* tab)
{
    const float h = (float)hd;
    const float step = 64.f / 49.f, coef = 1.0f / (h * h);
    for (int i = 0; i < 196; ++i) {
        const float d = (float)i * step;
        float w = std::exp((-d * d) * coef);
        if (w != w) w = 1.0f;
        const int v = (int)(19096.f * w);
        tab[i] = (float)v < 0.001f * 19096.f ? 0 : v;
    }
}

typedef std::function<void(int n, const std::function<void(int, int)>&)> ParFor;

struct Params {
    double lum = 3, col = 3, amt = 0;
    bool host8 = true;          // 8-bit pictures (the card's stores); off (float project): the stores are left out (GUESS, not captured)
};

// 21 x 21 search, 7 x 7 patch non-local means of one byte plane with border B = 13: P is (W + 26) x (H + 26) ints, out W x H.
inline void nlmPlane(const std::vector<int>& P, int W, int H, const int* tab, std::vector<int>& out, const ParFor& par)
{
    const int B = 13, EW = W + 2 * B;
    out.assign((size_t)W * H, 0);
    par(H, [&](int y0, int y1) {
        const int bh = y1 - y0, qn = bh + 6;
        std::vector<int> d((size_t)qn * (W + 6)), hs((size_t)qn * W), sum((size_t)W);
        std::vector<int64_t> num((size_t)bh * W, 0), den((size_t)bh * W, 0);
        for (int dy = -10; dy <= 10; ++dy)
            for (int dx = -10; dx <= 10; ++dx) {
                // absolute differences between the candidate patch pixels and the centre's, rows y0 - 3 .. y1 + 2, columns -3 .. W + 2
                for (int q = 0; q < qn; ++q) {
                    const int* a = &P[(size_t)(B - 3 + y0 + q) * EW + (B - 3)];
                    const int* b = &P[(size_t)(B - 3 + y0 + q + dy) * EW + (B - 3 + dx)];
                    int* dr = &d[(size_t)q * (W + 6)];
                    for (int x = 0; x < W + 6; ++x) { const int v = b[x] - a[x]; dr[x] = v < 0 ? -v : v; }
                    int* hr = &hs[(size_t)q * W];
                    for (int x = 0; x < W; ++x) hr[x] = dr[x] + dr[x + 1] + dr[x + 2] + dr[x + 3] + dr[x + 4] + dr[x + 5] + dr[x + 6];
                }
                for (int y = 0; y < bh; ++y) {
                    for (int x = 0; x < W; ++x) sum[(size_t)x] = hs[(size_t)y * W + x];
                    for (int t = 1; t < 7; ++t) {
                        const int* hr = &hs[(size_t)(y + t) * W];
                        for (int x = 0; x < W; ++x) sum[(size_t)x] += hr[x];
                    }
                    const int* cand = &P[(size_t)(B + y0 + y + dy) * EW + B + dx];
                    int64_t* nr = &num[(size_t)y * W];
                    int64_t* dn = &den[(size_t)y * W];
                    for (int x = 0; x < W; ++x) {
                        const int w = tab[std::min(sum[(size_t)x] >> 6, 195)];
                        dn[x] += w;
                        nr[x] += (int64_t)w * cand[x];
                    }
                }
            }
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < W; ++x) {
                const int64_t dd = den[(size_t)y * W + x];
                const int64_t v = dd > 0 ? num[(size_t)y * W + x] / dd : 0;
                out[(size_t)(y0 + y) * W + x] = (int)(v & 255);         // (convert_uchar of the quotient, 0..255 for a weighted mean)
            }
    });
}

// src: RGBA floats 0..1, W x H, rows bottom first. dst: the same. 'src' is taken as the 8-bit picture the card holds when host8 (rounded to the level).
inline void run(const float* src, int W, int H, const Params& pr, float* dst, const ParFor& parIn = ParFor())
{
    const ParFor par = parIn ? parIn : ParFor([](int n, const std::function<void(int, int)>& f) { f(0, n); });
    const double eps = 1.1920929e-07;
    const size_t n = (size_t)W * H;
    // the input image (read_imagef of an 8-bit UNORM picture: byte / 255)
    std::vector<float> in(n * 4);
    for (size_t i = 0; i < n * 4; ++i) {
        float v = src[i];
        if (!(v > 0.f)) v = 0.f;
        if (v > 1.f) v = 1.f;
        in[i] = pr.host8 ? (float)(int)(v * 255.f + 0.5f) / 255.f : v;
    }
    std::vector<float> work;                       // the picture after the denoising (RGBA floats; 8-bit levels when host8)
    if (pr.lum <= eps && pr.col <= eps) {
        work = in;
    } else {
        const int B = 13, EW = W + 2 * B, EH = H + 2 * B;
        std::vector<int> mx(EW), my(EH);
        for (int i = 0; i < EW; ++i) mx[i] = texel(i - B, W);
        for (int i = 0; i < EH; ++i) my[i] = texel(i - B, H);
        // 1 + 2: YUV into an 8-bit picture, then the planes (as bytes: 255 x value towards zero)
        std::vector<float> yuv(n * 4);
        for (size_t i = 0; i < n; ++i) {
            const float r = in[i * 4], g = in[i * 4 + 1], b = in[i * 4 + 2];
            float y = 0.299f * r;
            y = y + 0.587f * g;
            y = y + 0.114f * b;
            float u = -0.14713f * r;
            u = u + -0.28886f * g;
            u = u + 0.436f * b;
            u = u + 0.5f;
            float v = 0.615f * r;
            v = v + -0.51499f * g;
            v = v + -0.10001f * b;
            v = v + 0.5f;
            const float c[4] = {y, u, v, in[i * 4 + 3]};
            for (int k = 0; k < 4; ++k) yuv[i * 4 + k] = pr.host8 ? (float)store12(c[k]) / 255.f : c[k];
        }
        auto byteOf = [](float v) { const float t = 255.f * v; return t <= 0.f ? 0 : (t >= 255.f ? 255 : (int)t); };
        std::vector<int> pl[3];
        const double hs[3] = {pr.lum, pr.col, pr.col};
        std::vector<int> res[3];
        int tab[3][196];
        for (int k = 0; k < 3; ++k) {
            const bool on = hs[k] > eps;
            if (on) {
                pl[k].resize((size_t)EW * EH);
                for (int j = 0; j < EH; ++j)
                    for (int i = 0; i < EW; ++i) pl[k][(size_t)j * EW + i] = byteOf(yuv[((size_t)my[j] * W + mx[i]) * 4 + k]);
                weightTable(hs[k], tab[k]);
                nlmPlane(pl[k], W, H, tab[k], res[k], par);
            } else {
                // not denoised: the plane as it is (border left out)
                res[k].resize(n);
                for (int j = 0; j < H; ++j)
                    for (int i = 0; i < W; ++i) res[k][(size_t)j * W + i] = byteOf(yuv[((size_t)my[j + B] * W + mx[i + B]) * 4 + k]);
            }
        }
        // 3: merge (byte / 255.0 in a double, to a float), alpha plane from the untouched pixel's own alpha
        work.resize(n * 4);
        par((int)H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    const size_t i = (size_t)y * W + x;
                    const float Y = (float)((double)res[0][i] / 255.0);
                    float U = (float)((double)res[1][i] / 255.0), V = (float)((double)res[2][i] / 255.0);
                    const int a8 = byteOf(yuv[i * 4 + 3]);
                    const float A = (float)((double)a8 / 255.0);
                    U = U - 0.5f;
                    V = V - 0.5f;
                    float R = 1.0f * Y;
                    R = R + 0.0f * U;
                    R = R + 1.13983f * V;
                    float G = 1.0f * Y;
                    G = G + -0.39465f * U;
                    G = G + -0.58060f * V;
                    float Bv = 1.0f * Y;
                    Bv = Bv + 2.03211f * U;
                    Bv = Bv + 0.0f * V;
                    const float c[4] = {R, G, Bv, A};
                    for (int k = 0; k < 4; ++k) work[i * 4 + k] = pr.host8 ? (float)store12(c[k]) / 255.f : c[k];
                }
        });
    }
    // 5: sharpening (the kernel needs the amount to be at least 0.001)
    const float a = (float)pr.amt;
    if (pr.amt > eps && a >= 0.001f) {
        float c0 = 29.0f;
        if (a >= 0.2500f) c0 = 29.0f - a * 16.0f;
        else if (a >= 0.1250f) c0 = (-152.0f) * a + 63.0f;
        else if (a >= 0.0630f) c0 = (-269.230769f) * a + 88.961538f;
        else c0 = (-451.612903f) * a + 100.451613f;
        const float m1 = -1.0f, m2 = -2.0f, dv = c0 - 12.0f;
        par(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int ya = std::max(0, y - 1), yb = std::min(H - 1, y + 1);
                for (int x = 0; x < W; ++x) {
                    const int xa = std::max(0, x - 1), xb = std::min(W - 1, x + 1);
                    const float* p00 = &work[((size_t)ya * W + xa) * 4];
                    const float* p01 = &work[((size_t)y * W + xa) * 4];
                    const float* p02 = &work[((size_t)yb * W + xa) * 4];
                    const float* p10 = &work[((size_t)ya * W + x) * 4];
                    const float* p11 = &work[((size_t)y * W + x) * 4];
                    const float* p12 = &work[((size_t)yb * W + x) * 4];
                    const float* p20 = &work[((size_t)ya * W + xb) * 4];
                    const float* p21 = &work[((size_t)y * W + xb) * 4];
                    const float* p22 = &work[((size_t)yb * W + xb) * 4];
                    for (int k = 0; k < 4; ++k) {
                        float s = m1 * p00[k];
                        s = s + m2 * p01[k];
                        s = s + m1 * p02[k];
                        s = s + m2 * p10[k];
                        s = s + c0 * p11[k];
                        s = s + m2 * p12[k];
                        s = s + m1 * p20[k];
                        s = s + m2 * p21[k];
                        s = s + m1 * p22[k];
                        s = s / dv;
                        dst[((size_t)y * W + x) * 4 + k] = pr.host8 ? (float)store12(s) / 255.f : s;
                    }
                }
            }
        });
    } else {
        for (size_t i = 0; i < n * 4; ++i) dst[i] = work[i];
    }
}

}  // namespace vfxdn
}  // namespace ofx
}  // namespace reno
