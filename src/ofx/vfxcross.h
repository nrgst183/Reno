// Cross Effect as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on): float pictures. The host side (what VEGAS works out from the settings and hands to the
// kernels: host 0x1805b22b0, with the Zoom branch at 0x1805b6b94, the Pixelate branch at 0x1805b4b03 and the None branch at 0x1805b726e) and the kernels are written here from the
// decoded code; nothing here depends on Reno's Ctx, so a test can drive it with plain arrays.
//
// Pictures are RGBA floats; the kernels number their rows from the BOTTOM row. Every number is a float, in the order the kernel does it.
#pragma once
#include "vfxgauss.h"
#include "vfxwipes.h"
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace crossfx {

using wipes::Pic;
using wipes::clampf;

struct Settings {
    int type = 0;                     // EffectType: 0 None, 1 Zoom, 2 Pixelate, 3 Blur
    int apply = 2;                    // ApplyTo: 0 Source (the outgoing picture), 1 Destination, 2 Both
    double progress = 0;
    double fadeRange = 1;
    double maxZoom = 1, maxX = 1, maxY = 1, maxBlur = 1;
    double srcX = 0.5, srcY = 0.5, dstX = 0.5, dstY = 0.5;       // the zoom centres, fractions of the frame (y up)
    bool bilinear = true;             // the host's sampler choice for the zoom (the kernels' f2 selector: 0 nearest, 1 bilinear)
    // The pictures between the pixelate's kernels are images of the frame's format: in an 8-bit frame every step is cut to a level (the card's store, more::oclStore12 in Reno) before the
    // next one reads it. null: float frame, nothing is cut.
    float (*store8)(float) = nullptr;
};

inline void cutLevels(std::vector<float>& v, float (*store8)(float))
{
    if (store8)
        for (float& x : v) x = store8(x);
}

// Not a number or far outside the picture: the card clamps converted coordinates; keep the integer conversions defined.
inline float safeCoord(float v)
{
    if (!(v == v)) return 0.0f;
    return std::fmin(std::fmax(v, -1.0e6f), 1.0e6f);
}

// f2 of the kernels: 0 nearest (the coordinate rounded to even), 1 bilinear f0 (no clamp of the coordinate: outside the picture the reads are transparent black).
inline void sample(const Pic& pic, int mode, float x, float y, float* o)
{
    x = safeCoord(x);
    y = safeCoord(y);
    if (mode == 0) {
        pic.texel((int)std::nearbyint(x), (int)std::nearbyint(y), o);
        return;
    }
    bilinearHw(pic, x, y, o);
}

// kernel 818 (EffectType None, and the first step of the pixelate with both pictures): mix of the two pictures with the fade worked out per pixel from the progress and the fade range
inline float fadeKernel(float progress, float fadeRange)
{
    RENO_WIPES_NO_FMA
    const float v68 = clampf(progress, 0.0f, 1.0f);
    if (fadeRange < 0.001f) return v68 > 0.5f ? 1.0f : 0.0f;
    return clampf((v68 - (1.0f - fadeRange) * 0.5f) * (1.0f / fadeRange), 0.0f, 1.0f);
}

// The fade the host works out for the zoom (kernels 819 .. 8111 get it as an argument): in doubles, cut to a float at the end.
inline float fadeHost(float pc, double fadeRange)
{
    if (fadeRange < 0.0010000000474974513) return pc > 0.5f ? 1.0f : 0.0f;
    const float t = (float)(((double)pc - (1.0 - fadeRange) * 0.5) / fadeRange);
    if (0.0f > t) return 0.0f;
    return std::fmin(1.0f, t);
}

// (The column kernel's next block length is computed with a fused multiply-add on the card, the row kernel's without: found on the block edges of VEGAS's pictures.)
// kernels 8112 (down the columns) and 8113 (along the rows): box averages of ny / nx blocks, written back over the block. The blocks are counted from the TOP row (the kernel's last row) in
// 8112 and from the left in 8113; the block lengths come from a float step.
inline void pixelateColumns(const std::vector<float>& in, std::vector<float>& out, int W, int H, int ny)
{
    RENO_WIPES_NO_FMA
    out.resize(in.size());
    for (int x = 0; x < W; ++x) {
        if (H == ny) {
            for (int y = 0; y < H; ++y)
                for (int k = 0; k < 4; ++k) out[((size_t)y * W + x) * 4 + k] = in[((size_t)y * W + x) * 4 + k];
            continue;
        }
        const float v87 = (float)H / (float)ny;
        int v88 = (int)(v87 * (float)ny - v87 * (float)(ny - 1));
        float v89[4] = {0, 0, 0, 0};
        int v90 = 0, v92 = 0;
        for (int y = 0; y < H; ++y) {
            v90++;
            const int v91 = H - 1 - y;
            const float* p = &in[((size_t)v91 * W + x) * 4];
            for (int k = 0; k < 4; ++k) v89[k] += p[k];
            if (v90 == v88 || v91 == 0) {
                for (int k = 0; k < 4; ++k) v89[k] /= (float)v90;
                for (int v93 = 0; v93 < v90; ++v93) {
                    float* q = &out[((size_t)(H - 1 - (y - v93)) * W + x) * 4];
                    for (int k = 0; k < 4; ++k) q[k] = v89[k];
                }
                for (int k = 0; k < 4; ++k) v89[k] = 0.0f;
                v90 = 0;
                v92++;
                if (ny != v92) v88 = (int)std::fma(v87, (float)(v92 + 1), -(float)y);
                else v88 = (int)v87;
            }
        }
    }
}

inline void pixelateRows(const std::vector<float>& in, std::vector<float>& out, int W, int H, int nx)
{
    RENO_WIPES_NO_FMA
    out.resize(in.size());
    for (int y = 0; y < H; ++y) {
        if (W == nx) {
            for (int x = 0; x < W; ++x)
                for (int k = 0; k < 4; ++k) out[((size_t)y * W + x) * 4 + k] = in[((size_t)y * W + x) * 4 + k];
            continue;
        }
        float v89[4] = {0, 0, 0, 0};
        int v90 = 0, v92 = 0;
        const float v95 = (float)W / (float)nx;
        int v96 = (int)v95;
        for (int x = 0; x < W; ++x) {
            v90++;
            const float* p = &in[((size_t)y * W + x) * 4];
            for (int k = 0; k < 4; ++k) v89[k] += p[k];
            if (v90 == v96 || W - 1 == x) {
                for (int k = 0; k < 4; ++k) v89[k] /= (float)v90;
                for (int v93 = 0; v93 < v90; ++v93) {
                    float* q = &out[((size_t)y * W + (x - v93)) * 4];
                    for (int k = 0; k < 4; ++k) q[k] = v89[k];
                }
                for (int k = 0; k < 4; ++k) v89[k] = 0.0f;
                v90 = 0;
                v92++;
                if (nx != v92) v96 = (int)(v95 * (float)(v92 + 1) - (float)x);
                else v96 = W - x;
            }
        }
    }
}

// The pixelate branch's block counts: nx x ny from the settings. The pixel aspect term of the host (two doubles of the frame description, 1 for square pixels) is 1 here.
inline void pixelateCounts(int W, int H, float pw, double maxX, double maxY, int& nx, int& ny)
{
    RENO_WIPES_NO_FMA
    const float q = 0.25f;
    const float fx = 1.0f - ((float)maxX * q + 0.75f) * pw;
    const float fy = 1.0f - ((float)maxY * q + 0.75f) * pw;
    // (cvttss2si of a number outside the integers gives the smallest integer; the counts are kept in 1 .. size here)
    auto cut = [](float v, int n) {
        const float s = v;
        if (!(s == s) || s < -1.0e9f) return 1;
        if (s > 1.0e9f) return n;
        return (int)s;
    };
    nx = cut((float)W * fx * 0.5f + 1.0f, W);
    ny = cut((float)H * fy * 0.5f + 1.0f, H);
    if (fx > 0.999f) nx = W;
    if (fy > 0.999f) ny = H;
    nx = std::min(std::max(nx, 1), W);
    ny = std::min(std::max(ny, 1), H);
}

// Blur (host 0x1805b22b0, the branch after the dispatch): each picture to be blurred goes through kernel 810 (a causal and an anti-causal pass down every column, the coefficients of the
// vertical sigma), kernel 811 (the same along every row of the SUM of those two pictures, the horizontal sigma: its two results are again two pictures) and kernel 812 (their sum); then kernel 818
// mixes. The Gaussian is the Deriche-type recursion of vfxgauss.h (the one Radial Blur uses). Every picture the kernels write is an image of the frame's format.
inline void blurPicture(const std::vector<float>& in, std::vector<float>& out, int W, int H, const vfxgauss::Coefsf& cx, const vfxgauss::Coefsf& cy, float (*store8)(float))
{
    auto plain = [](float a, float b, float c) { return a * b + c; };
    std::vector<float> t1, t2;
    vfxgauss::passColumns(cy, in, W, H, t1, t2, plain);
    cutLevels(t1, store8);
    cutLevels(t2, store8);
    // the row kernel reads t1 + t2; along the rows is along the columns of the transposed picture
    std::vector<float> sumT((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) sumT[((size_t)x * H + y) * 4 + k] = t1[((size_t)y * W + x) * 4 + k] + t2[((size_t)y * W + x) * 4 + k];
    std::vector<float> ra, rb;
    vfxgauss::passColumns(cx, sumT, H, W, ra, rb, plain);
    out.resize((size_t)W * H * 4);
    std::vector<float> r1((size_t)W * H * 4), r2((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) {
                r1[((size_t)y * W + x) * 4 + k] = ra[((size_t)x * H + y) * 4 + k];
                r2[((size_t)y * W + x) * 4 + k] = rb[((size_t)x * H + y) * 4 + k];
            }
    cutLevels(r1, store8);
    cutLevels(r2, store8);
    for (size_t i = 0; i < out.size(); ++i) out[i] = r1[i] + r2[i];
    cutLevels(out, store8);
}

// The horizontal and vertical sigmas of the Blur branch (pixel aspect 1): the size is a fraction of the longer side, and "falls to 1/255 at that distance" turns it into a sigma.
inline void blurSigmas(int W, int H, int apply, float p, double maxBlur, float& sx, float& sy)
{
    RENO_WIPES_NO_FMA
    float q = p;
    if (apply == 1) q = 1.0f - p;
    else if (apply == 2) q = 0.5f < p ? (1.0f - p) + (1.0f - p) : p + p;
    float sz = (float)std::max(W, H);
    sz = sz * ((float)maxBlur * q);
    sz = sz * 0.25f;
    const float ln = std::log(0.003921568859368563f);
    const float twoLn = ln + ln;
    float ax = 1.0f, ay = 1.0f;
    ax *= sz;
    ay *= sz;
    ax = ax * ax;
    ax /= twoLn;
    sx = std::sqrt(-ax);
    ay = ay * ay;
    ay /= twoLn;
    sy = std::sqrt(-ay);
}

// ------------------------------------------------------------------------------------------------------------------------------------ the transition
inline void flipRows(const float* in, std::vector<float>& out, int W, int H)
{
    out.resize((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int i = 0; i < W * 4; ++i) out[(size_t)(H - 1 - y) * W * 4 + i] = in[(size_t)y * W * 4 + i];
}

// a, b: the outgoing and incoming pictures, W x H x 4 floats; out the same. All three are in the kernels' order (the BOTTOM row first). The result is the float picture the kernels write.
inline void render(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& out, int W, int H, const Settings& s)
{
    RENO_WIPES_NO_FMA
    out.assign((size_t)W * H * 4, 0.0f);
    if (W <= 0 || H <= 0) return;
    const Pic A{a.data(), W, H, false}, B{b.data(), W, H, false};
    const int type = std::min(std::max(s.type, 0), 3), apply = std::min(std::max(s.apply, 0), 2);
    float p = (float)s.progress;
    if (!(p == p)) p = 0.0f;
    p = p < 0.0f ? 0.0f : std::fmin(1.0f, p);
    const float fr = (float)s.fadeRange;

    if (type == 0) {                                       // kernel 818
        const float t = fadeKernel(p, fr);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float va[4], vb[4];
                A.texel(x, y, va);
                B.texel(x, y, vb);
                wipes::mix4(va, vb, t, &out[((size_t)y * W + x) * 4]);
            }
        return;
    }
    if (type == 1) {                                       // kernels 819, 8110, 8111 (host 0x1805b6b94)
        const float fade = fadeHost(p, s.fadeRange);
        const float Z = (float)s.maxZoom;
        const float sA = 1.0f / std::pow(Z, p);
        const float sB = 1.0f / std::pow(Z, 1.0f - p);
        const float cs[2] = {(float)s.srcX * (float)W, (float)s.srcY * (float)H};
        const float cd[2] = {(float)s.dstX * (float)W, (float)s.dstY * (float)H};
        const int mode = s.bilinear ? 1 : 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float va[4], vb[4];
                const float fx = (float)x, fy = (float)y;
                if (apply != 1) {
                    const float qx = ((fx - cs[0]) * sA + cs[0]) * 1.0f, qy = ((fy - cs[1]) * sA + cs[1]) * 1.0f;
                    sample(A, mode, qx, qy, va);
                } else A.texel(x, y, va);
                if (apply != 0) {
                    const float qx = ((fx - cd[0]) * sB + cd[0]) * 1.0f, qy = ((fy - cd[1]) * sB + cd[1]) * 1.0f;
                    sample(B, mode, qx, qy, vb);
                } else B.texel(x, y, vb);
                wipes::mix4(va, vb, fade, &out[((size_t)y * W + x) * 4]);
            }
        return;
    }
    if (type == 2) {                                       // kernels 8112, 8113 and 818 (host 0x1805b4b03)
        float pw;
        if (apply == 0) pw = std::pow(p, 0.25f);
        else if (apply == 1) pw = std::pow(1.0f - p, 0.25f);
        else {
            float q = p;
            if (!(0.5f >= q)) q = 1.0f - q;
            q += q;
            pw = std::pow(q, 0.25f);
        }
        int nx, ny;
        pixelateCounts(W, H, pw, s.maxX, s.maxY, nx, ny);
        const float t = fadeKernel(p, fr);
        std::vector<float> m1, m2, base;
        if (apply == 2) {                                  // the mix first, then the whole picture is pixelated
            base.assign((size_t)W * H * 4, 0.0f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float va[4], vb[4];
                    A.texel(x, y, va);
                    B.texel(x, y, vb);
                    wipes::mix4(va, vb, t, &base[((size_t)y * W + x) * 4]);
                }
            cutLevels(base, s.store8);
            pixelateColumns(base, m1, W, H, ny);
            cutLevels(m1, s.store8);
            pixelateRows(m1, out, W, H, nx);
            return;
        }
        pixelateColumns(apply == 0 ? a : b, m1, W, H, ny);
        cutLevels(m1, s.store8);
        pixelateRows(m1, m2, W, H, nx);
        cutLevels(m2, s.store8);
        const Pic P{m2.data(), W, H, false};
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float va[4], vb[4];
                if (apply == 0) { P.texel(x, y, va); B.texel(x, y, vb); }
                else { A.texel(x, y, va); P.texel(x, y, vb); }
                wipes::mix4(va, vb, t, &out[((size_t)y * W + x) * 4]);
            }
        return;
    }
    // type 3: Blur
    float sx, sy;
    blurSigmas(W, H, apply, p, s.maxBlur, sx, sy);
    const vfxgauss::Coefsf cx = vfxgauss::toFloats(vfxgauss::build((double)sx)), cy = vfxgauss::toFloats(vfxgauss::build((double)sy));
    std::vector<float> ba, bb;
    if (apply != 1) blurPicture(a, ba, W, H, cx, cy, s.store8);
    if (apply != 0) blurPicture(b, bb, W, H, cx, cy, s.store8);
    const Pic PA{apply != 1 ? ba.data() : a.data(), W, H, false}, PB{apply != 0 ? bb.data() : b.data(), W, H, false};
    const float t = fadeKernel(p, fr);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float va[4], vb[4];
            PA.texel(x, y, va);
            PB.texel(x, y, vb);
            wipes::mix4(va, vb, t, &out[((size_t)y * W + x) * 4]);
        }
}

}  // namespace crossfx
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
