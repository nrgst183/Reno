// The four blur effects of Vfx1.ofx that run on VEGAS's OpenCL kernels when GPU acceleration is on: Gaussian Blur, Unsharp Mask, Glow and Chroma Blur. Each is the same two recursive-Gaussian
// kernels (vfxgauss.h: a causal and an anti-causal pass down every column, then the same along every row of the sum of those two pictures) followed by one small kernel of its own
// that reads the original picture and the two row pictures. A re-implementation of what the host code and the kernels do (read from the user's own VEGAS install), with no dependencies
// but vfxgauss.h, so a test program can include it alone.
//
// Pictures are W x H x 4 floats, rows BOTTOM first (the order OpenCL images are kept in). In an 8-bit project every picture a kernel writes is an 8-bit texture: 'store' is the card's
// float -> byte -> float step (more::oclStore12), or null for a float project.
//
// Host side (each effect's OpenCL processor): the two sigmas go through the Deriche coefficient builder (vfxgauss::build) as doubles; the vertical sigma serves the column kernel, the
// horizontal one the row kernel.
//   Gaussian Blur: s = (float)(max(W, H) x Range) + 1.5f (the vertical one first multiplied by the pixel aspect and divided by the render-scale ratio, both 1 here), sigma = (double)sqrtf(-(s s / (2 logf(1/255))))
//   Unsharp Mask:  s = max(W, H) x Radius + 1.5 in doubles, sigma = sqrt(-(s s / (double)(2 logf(1/255))))
//   Glow:          s = max(W, H) x clamp(GlowPercent, 0, 1) x 0.5 (no 1.5), the same double formula
//   Chroma Blur:   s = ((float)Pixels + 1.5f) x pixel aspect, the float formula as Gaussian Blur
#pragma once
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "vfxgauss.h"

namespace reno {
namespace ofx {
namespace vfxblurs {

// the little kernels after the recursions come out the same fused or not (checked), so the compiler is told not to fuse them behind our back
#if defined(__clang__)
#define RENO_BLUR_NO_FMA _Pragma("clang fp contract(off)")
#else
#define RENO_BLUR_NO_FMA
#endif

typedef float (*StoreFn)(float);

// 2 logf(1/255) as the host computes it (logf of the float 0x3b808081, doubled)
inline float twoLn()
{
    const float ln = rm::log(0.003921568859368563f);
    return ln + ln;
}
// sigma of the float formula (Gaussian Blur, Chroma Blur): s is the float already carrying its 1.5
inline double sigmaFloat(float s)
{
    float t = s * s;
    t = t / twoLn();
    t = -t;
    return (double)std::sqrt(t);
}
// sigma of the double formula (Unsharp Mask, Glow): x is the double already carrying its 1.5 where it has one
inline double sigmaDouble(double x)
{
    double t = x * x;
    t = t / (double)twoLn();
    t = -t;
    return std::sqrt(t);
}

// The card fuses a multiply and the add after it (one rounding) in the recursions; the captures of the four effects fit that better than two roundings (Gaussian Blur 98.0% of the values
// exact against 97.8%, Glow and Unsharp Mask likewise). The little kernels after them (mix, unsharp, the colour matrices) came out the same either way.
inline float fusedMad(float a, float b, float c) { return std::fmaf(a, b, c); }

inline void applyStore(std::vector<float>& v, StoreFn store)
{
    if (!store) return;
    for (float& x : v) x = store(x);
}

// The two row pictures r1, r2 (each stored) of the picture 'in' (W x H, rows bottom first): kernel 0 along the columns with the coefficients cy (two stored pictures), kernel 1 along the
// rows of their SUM with cx (two stored pictures). The result of the effect's own kernel is a function of in, r1 + r2.
template <class Fma>
inline void blurHalves(const std::vector<float>& in, int W, int H, const vfxgauss::Coefsf& cx, const vfxgauss::Coefsf& cy, StoreFn store, std::vector<float>& r1, std::vector<float>& r2, Fma fma)
{
    std::vector<float> t1, t2;
    vfxgauss::passColumns(cy, in, W, H, t1, t2, fma);
    applyStore(t1, store);
    applyStore(t2, store);
    // the row kernel reads t1 + t2; along the rows is along the columns of the transposed picture
    std::vector<float> sumT((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) sumT[((size_t)x * H + y) * 4 + k] = t1[((size_t)y * W + x) * 4 + k] + t2[((size_t)y * W + x) * 4 + k];
    std::vector<float> ra, rb;
    vfxgauss::passColumns(cx, sumT, H, W, ra, rb, fma);
    r1.resize((size_t)W * H * 4);
    r2.resize((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) {
                r1[((size_t)y * W + x) * 4 + k] = ra[((size_t)x * H + y) * 4 + k];
                r2[((size_t)y * W + x) * 4 + k] = rb[((size_t)x * H + y) * 4 + k];
            }
    applyStore(r1, store);
    applyStore(r2, store);
}

// ---------------------------------------------------------------------------------------------------- Gaussian Blur
// Kernel 2 (all four channels on) or kernel 3 (channels off keep the source): out = blur (r1 + r2) where the channel is on, else the source; then the card's store.
inline float gaussSigmaV(double range, int W, int H)
{
    // (the host: max(W, H) x Range x pixel aspect / render-scale ratio, as a float, + 1.5f; aspect and ratio are 1)
    return (float)((double)std::max(W, H) * range) + 1.5f;
}
// The coefficients of the two recursions (cy: down the columns, cx: along the rows), shared with the Vulkan plans (vkblur.cpp)
struct GaussSetup { vfxgauss::Coefsf cx, cy; };
inline GaussSetup gaussianSetup(int W, int H, double rangeH, double rangeV)
{
    const float sv = gaussSigmaV(rangeV, W, H), sh = (float)((double)std::max(W, H) * rangeH + 1.5);
    GaussSetup g;
    g.cy = vfxgauss::toFloats(vfxgauss::build(sigmaFloat(sv)));
    g.cx = vfxgauss::toFloats(vfxgauss::build(sigmaFloat(sh)));
    return g;
}
inline void gaussianBlur(const std::vector<float>& src, int W, int H, double rangeH, double rangeV, const bool ch[4], StoreFn store, std::vector<float>& out)
{
    const GaussSetup gs = gaussianSetup(W, H, rangeH, rangeV);
    const vfxgauss::Coefsf cy = gs.cy, cx = gs.cx;
    std::vector<float> r1, r2;
    blurHalves(src, W, H, cx, cy, store, r1, r2, fusedMad);
    out.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        const int k = (int)(i & 3);
        float v = ch[k] ? r1[i] + r2[i] : src[i];
        out[i] = store ? store(v) : v;
    }
}

// ---------------------------------------------------------------------------------------------------- Unsharp Mask
// Kernel 8: b = r1 + r2; v = s + Amount x (s - b); out = |v - b| > Threshold ? v : s (per channel, alpha too); then the store.
inline GaussSetup unsharpSetup(int W, int H, double radius)
{
    const double big = (double)std::max(W, H);
    const double sv = big * radius + 1.5, sh = big * radius / 1.0 + 1.5;
    GaussSetup g;
    g.cy = vfxgauss::toFloats(vfxgauss::build(sigmaDouble(sv)));
    g.cx = vfxgauss::toFloats(vfxgauss::build(sigmaDouble(sh)));
    return g;
}
inline void unsharpMask(const std::vector<float>& src, int W, int H, double radius, float amount, float threshold, StoreFn store, std::vector<float>& out)
{
    RENO_BLUR_NO_FMA
    const GaussSetup gs = unsharpSetup(W, H, radius);
    const vfxgauss::Coefsf cy = gs.cy, cx = gs.cx;
    std::vector<float> r1, r2;
    blurHalves(src, W, H, cx, cy, store, r1, r2, fusedMad);
    out.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        const float s = src[i];
        const float b = r1[i] + r2[i];
        const float v = s + amount * (s - b);
        const float o = (std::fabs(v - b) > threshold) ? v : s;
        out[i] = store ? store(o) : o;
    }
}

// ---------------------------------------------------------------------------------------------------- Glow
// Kernel 8: b = r1 + r2; mask = clamp(((b.r + b.g + b.b) / 3 - Suppression) x Intensity, 0, 1); out = mix(s, Colour, mask) (Colour = r, g, b, 1); then the store.
inline GaussSetup glowSetup(int W, int H, double percent)
{
    double g = percent;
    if (!(g > 0)) g = 0;           // (the host: 0 > g gives 0, else min(1, g))
    else g = std::min(1.0, g);
    const double sg = (double)std::max(W, H) * g * 0.5;
    GaussSetup r;
    r.cy = vfxgauss::toFloats(vfxgauss::build(sigmaDouble(sg)));
    r.cx = vfxgauss::toFloats(vfxgauss::build(sigmaDouble(sg)));
    return r;
}
inline void glow(const std::vector<float>& src, int W, int H, double percent, float intensity, float suppression, const float colour[3], StoreFn store, std::vector<float>& out)
{
    RENO_BLUR_NO_FMA
    const GaussSetup gs = glowSetup(W, H, percent);
    const vfxgauss::Coefsf cy = gs.cy, cx = gs.cx;
    std::vector<float> r1, r2;
    blurHalves(src, W, H, cx, cy, store, r1, r2, fusedMad);
    out.resize(src.size());
    const float col[4] = {colour[0], colour[1], colour[2], 1.0f};
    for (size_t i = 0; i < src.size(); i += 4) {
        const float b0 = r1[i] + r2[i], b1 = r1[i + 1] + r2[i + 1], b2 = r1[i + 2] + r2[i + 2];
        const float avg = (b0 + b1 + b2) / 3.0f;
        float m = (avg - suppression) * intensity;
        m = std::min(std::max(m, 0.0f), 1.0f);
        for (int k = 0; k < 4; ++k) {
            const float s = src[i + k];
            const float o = s + (col[k] - s) * m;
            out[i + k] = store ? store(o) : o;
        }
    }
}

// ---------------------------------------------------------------------------------------------------- Chroma Blur
// Kernel 38 turns the picture into Y U V (U, V with 0.5 added; stored, so V above 1 is held at 1), the two Gaussian kernels blur it, kernel 33 takes U and V from the blur and Y and alpha from the
// converted picture (stored), kernel 39 turns it back (stored).
inline GaussSetup chromaSetup(double horizontalPixels, double verticalPixels)
{
    const float sv = (float)verticalPixels + 1.5f, sh = (float)horizontalPixels + 1.5f;
    GaussSetup g;
    g.cy = vfxgauss::toFloats(vfxgauss::build(sigmaFloat(sv)));
    g.cx = vfxgauss::toFloats(vfxgauss::build(sigmaFloat(sh)));
    return g;
}
inline void chromaBlur(const std::vector<float>& src, int W, int H, double horizontalPixels, double verticalPixels, StoreFn store, std::vector<float>& out)
{
    RENO_BLUR_NO_FMA
    (void)W; (void)H;
    const GaussSetup gs = chromaSetup(horizontalPixels, verticalPixels);
    const vfxgauss::Coefsf cy = gs.cy, cx = gs.cx;
    std::vector<float> yuv(src.size());
    for (size_t i = 0; i < src.size(); i += 4) {
        const float r = src[i], g = src[i + 1], b = src[i + 2];
        float y = 0.299f * r + 0.587f * g + 0.114f * b;
        float u = -0.14713f * r + -0.28886f * g + 0.436f * b + 0.5f;
        float v = 0.615f * r + -0.51499f * g + -0.10001f * b + 0.5f;
        yuv[i] = y; yuv[i + 1] = u; yuv[i + 2] = v; yuv[i + 3] = src[i + 3];
    }
    applyStore(yuv, store);
    std::vector<float> r1, r2;
    blurHalves(yuv, W, H, cx, cy, store, r1, r2, fusedMad);
    std::vector<float> mid(src.size());
    for (size_t i = 0; i < src.size(); i += 4) {
        mid[i] = yuv[i];
        mid[i + 1] = r1[i + 1] + r2[i + 1];
        mid[i + 2] = r1[i + 2] + r2[i + 2];
        mid[i + 3] = yuv[i + 3];
    }
    applyStore(mid, store);
    out.resize(src.size());
    for (size_t i = 0; i < src.size(); i += 4) {
        const float y = mid[i];
        const float u = mid[i + 1] - 0.5f, v = mid[i + 2] - 0.5f;
        out[i] = 1.0f * y + 0.0f * u + 1.13983f * v;
        out[i + 1] = 1.0f * y + -0.39465f * u + -0.58060f * v;
        out[i + 2] = 1.0f * y + 2.03211f * u + 0.0f * v;
        out[i + 3] = mid[i + 3];
    }
    applyStore(out, store);
}

}  // namespace vfxblurs
}  // namespace ofx
}  // namespace reno
