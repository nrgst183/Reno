// VEGAS Cookie Cutter as its OpenCL kernels draw it (Vfx1.ofx, cookiecutterOpenCLProcessor; read from the user's own VEGAS install): the set-up numbers of the processor, the mask
// by shape (builtins_cookie_vegas.h: the kernels' shape arithmetic), the Feather as a recursive Gaussian over the mask (the same Vfx1 coefficient builder as Radial Blur's / Newsprint's,
// vfxgauss.h, but on global float buffers and with every multiply-add fused), and the composite of picture, border colour and nothing in floats (mask 0 .. 2).
// Header only (the set-up and the mask are templates over any struct with CookieSetup's field names) so a test program can check it against the captured pictures.
#pragma once
#include "builtins_cookie_vegas.h"
#include "vfxgauss.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxcookie {

using namespace builtin::cookiev;

// The set-up of the processor, in single precision as VEGAS has it. W x H the frame; the inputs are the effect's settings. The pixel aspect ratio is square (1).
template <class S>
inline void setupCore(S& o, int W, int H, int shape, int method, double featherP, double borderP, double sizeP, int repX, int repY, double cxp, double cyp, double cr, double cg, double cb)
{
    shape = std::max(0, std::min(13, shape));
    o.cutSection = method == 1;
    repX = std::max(1, std::min(100, repX));
    repY = std::max(1, std::min(100, repY));
    const float sx = 1.0f, sy = 1.0f;
    // the per shape factors on the Size (the processor's constants: oval 1.4182, oval side 1.8583, rectangle 2.00536, diamond 1.36491, square 0.87866, triangles 1.15577, arrowheads 0.8943)
    static const uint32_t kMul[14] = {0x3f800000u, 0x3fb58794u, 0x3feddcc6u, 0x400057d1u, 0x3faeb55fu, 0x3f60efddu, 0x3f93f045u,
                                      0x3f93f045u, 0x3f93f045u, 0x3f93f045u, 0x3f64f0d8u, 0x3f64f0d8u, 0x3f64f0d8u, 0x3f64f0d8u};
    float sizeF = (float)sizeP;
    if (shape > 0) sizeF = sizeF * K(kMul[shape]);
    const int cw = std::max(1, (int)((float)W / (float)repX + 0.5f)), ch = std::max(1, (int)((float)H / (float)repY + 0.5f));
    float cwp = (float)cw * sx, chp = (float)ch * sy;
    const float ratio = chp / cwp;
    const float diag = std::sqrt(chp * chp + cwp * cwp);
    const float mx = cwp > chp ? cwp : chp;
    const float bw = (float)borderP * mx * 0.25f;
    const float fw = (float)featherP * mx * 0.25f;
    const float rOut = (bw + diag) * sizeF;
    const float m1 = rOut < bw ? rOut : bw;
    float fw2;
    if (m1 > 1.0f) fw2 = fw < m1 ? fw : m1;
    else fw2 = rOut < fw ? rOut : fw;
    const float rIn = std::max(0.0f, rOut - m1);
    float tIn = std::max(0.0f, rIn - m1);
    float tOut = rIn + m1;
    float ovalW = cwp, ovalH = chp;
    if (shape == 1 || shape == 2) {
        const float den = m1 + diag;
        tIn = tIn / den;
        tOut = tOut / den;
        tIn = tIn * tIn;
        tOut = tOut * tOut;
        if (shape == 2) { ovalW = chp; ovalH = cwp; }
    }
    o.shape = shape; o.repX = repX; o.repY = repY; o.cw = cw; o.ch = ch;
    o.noBorder = 1.0f > m1;
    o.noFeather = 1.0f > fw2;
    o.fw2 = fw2;
    o.cxCell = (float)((double)cw * cxp);
    o.cyCell = (float)((double)ch * cyp);
    o.ratio = ratio; o.tIn = tIn; o.tOut = tOut; o.rIn = rIn; o.m1 = m1; o.ovalW = ovalW; o.ovalH = ovalH;
    o.hwF = (cwp * 0.5f) * sizeF;
    o.hhF = (chp * 0.5f) * sizeF;
    o.boxHi = (o.noBorder ? tOut : tIn) * K(0x3f3504f3u);
    o.boxLo = -o.boxHi;
    o.nTout = -tOut;
    o.hx = std::max(o.hwF + m1, 0.0f);
    o.hy = std::max(o.hhF + m1, 0.0f);
    o.ix0 = std::max(o.hwF - m1, 0.0f);
    o.iy0 = std::max(o.hhF - m1, 0.0f);
    o.br = (float)cr; o.bg = (float)cg; o.bb = (float)cb;
    o.colour[0] = (int)(o.br * 255.0f + 0.5f);
    o.colour[1] = (int)((double)cg * 255.0 + 0.5);
    o.colour[2] = (int)(o.bb * 255.0f + 0.5f);
    o.colour[3] = 255;
}

// Per column / row: the cell the pixel lies in (-1: before the first centre) and its distance from the centre across the cell.
struct MaskGeo {
    std::vector<int> xi, yi;
    std::vector<float> xa, ya;
    TileParams tp;
};
template <class S>
inline void maskPrepare(const S& su, int W, int H, MaskGeo& g)
{
    g.tp.shape = su.shape; g.tp.repX = su.repX; g.tp.repY = su.repY; g.tp.cw = su.cw; g.tp.ch = su.ch;
    g.tp.cx0 = su.cxCell; g.tp.cy0 = su.cyCell; g.tp.ratio = su.ratio; g.tp.sx = 1.0f; g.tp.sy = 1.0f;
    const int cw = su.cw, ch = su.ch;
    g.xi.assign((size_t)W, 0); g.yi.assign((size_t)H, 0); g.xa.assign((size_t)W, 0.f); g.ya.assign((size_t)H, 0.f);
    for (int x = 0; x < W; ++x) {
        const float xf = (float)x;
        if (su.cxCell <= xf) { const int q = (int)(xf - su.cxCell) / cw; g.xi[(size_t)x] = std::min(q, su.repX - 1); }
        else g.xi[(size_t)x] = -1;
        g.xa[(size_t)x] = std::fabs((float)(x % cw) - su.cxCell);
    }
    for (int y = 0; y < H; ++y) {
        const float yf = (float)y;
        if (su.cyCell <= yf) { const int q = (int)(yf - su.cyCell) / ch; g.yi[(size_t)y] = std::min(q, su.repY - 1); }
        else g.yi[(size_t)y] = -1;
        g.ya[(size_t)y] = std::fabs((float)(y % ch) - su.cyCell);
    }
}

// One row of the mask (row y counted from the bottom): 0 inside the shape, 1 on its border band, 2 outside.
template <class S>
inline void maskRow(const S& su, const MaskGeo& g, int W, int y, float* mrow, bool kernel = false)
{
    const int shape = su.shape;
    const float ay = g.ya[(size_t)y];
    const int iy = g.yi[(size_t)y];
    for (int x = 0; x < W; ++x) {
        const float ax = g.xa[(size_t)x];
        const int ix = g.xi[(size_t)x];
        float m;
        if (shape == 3) {
            const float hx = std::max(su.hwF + su.m1, 0.0f), hy = std::max(su.hhF + su.m1, 0.0f);
            const float ix0 = std::max(su.hwF - su.m1, 0.0f);
            if (ix0 > ax && std::max(su.hhF - su.m1, 0.0f) > ay) m = 0.0f;
            else if (ax > hx) m = 2.0f;
            else if (ay > hy) m = 2.0f;
            else m = 1.0f;
        } else if (shape == 1 || shape == 2) {
            const float dx = (ax / su.ovalW) * (ax / su.ovalW), dy = (ay / su.ovalH) * (ay / su.ovalH);
            const float s2 = dy + dx;
            if (su.tIn > s2) m = 0.0f;
            else if (s2 > su.tOut) m = 2.0f;
            else m = 1.0f;
        } else if (shape == 0 && kernel) {
            // the kernel's thresholds as they are (0 under the inner radius, 2 over the outer, 1 on the band, a band of no width included); the box of pixels that are surely inside is cut from the inner radius
            const float bh = su.tIn * 0.7f;
            if (ax >= -bh && bh >= ax && ay >= -bh && bh > ay) m = 0.0f;
            else if (su.nTout > ax || ax > su.tOut || su.nTout > ay || ay > su.tOut) m = 2.0f;
            else {
                const float d = tileDist(x, y, ix, iy, g.tp);
                m = su.tIn > d ? 0.0f : (d > su.tOut ? 2.0f : 1.0f);
            }
        } else if (shape == 0) {
            if (ax >= su.boxLo && su.boxHi >= ax && ay >= su.boxLo && su.boxHi > ay) m = 0.0f;
            else if (su.nTout > ax || ax > su.tOut || su.nTout > ay || ay > su.tOut) m = 2.0f;
            else {
                const float d = tileDist(x, y, ix, iy, g.tp);
                if (su.noBorder) m = su.rIn > d ? 0.0f : 2.0f;
                else if (su.tIn > d) m = 0.0f;
                else if (d > su.tOut) m = 2.0f;
                else m = 1.0f;
            }
        } else {
            const float d = tileDist(x, y, ix, iy, g.tp);
            if (su.tIn > d) m = 0.0f;
            else if (d > su.tOut) m = 2.0f;
            else m = 1.0f;
        }
        mrow[x] = m;
    }
}

// ---- the feather: the card's recursive Gaussian over the mask, one float a pixel, in global buffers: columns (down y) with the sigma of y, the two halves added and the picture
// transposed, then the same with the sigma of x, and transposed back. The recursion is the one of vfxgauss.h (the first two samples of a pass are special) with every multiply-add fused.
inline void passColumns1(const vfxgauss::Coefsf& cf, const std::vector<float>& in, int W, int H, std::vector<float>& a, std::vector<float>& b)
{
    a.assign((size_t)W * H, 0.f);
    b.assign((size_t)W * H, 0.f);
    for (int x = 0; x < W; ++x) {
        for (int pass = 0; pass < 2; ++pass) {
            const float* nn = pass == 0 ? cf.n : cf.m;
            const float* ee = pass == 0 ? cf.bn : cf.bm;
            const float* d = cf.d;
            std::vector<float>& out = pass == 0 ? a : b;
            auto at = [&](int i) { return pass == 0 ? i : H - 1 - i; };
            float v11, v12, v13, v14, v15, v16, v17, v18, v19, v22;
            const float x0 = in[(size_t)at(0) * W + x];
            if (pass == 0) v15 = -ee[4] * x0;
            else v15 = (nn[4] - ee[4]) * x0;
            v14 = std::fmaf(nn[3] - ee[3], x0, v15);
            v13 = std::fmaf(nn[2] - ee[2], x0, v14);
            v12 = std::fmaf(nn[1] - ee[1], x0, v13);
            v11 = std::fmaf(nn[0] - ee[0], x0, v12);
            v22 = v11;
            out[(size_t)at(0) * W + x] = v22;
            if (H < 2) continue;
            const float x1 = in[(size_t)at(1) * W + x];
            v16 = 0.0f;
            v17 = v22 * d[0];
            v18 = 0.0f;
            v19 = 0.0f;
            v11 = std::fmaf(x1, nn[0], v12);
            v12 = std::fmaf(x1, nn[1], v13);
            v13 = std::fmaf(x1, nn[2], v14);
            v14 = std::fmaf(x1, nn[3], v15);
            v15 = pass == 0 ? 0.0f : x1 * nn[4];
            v22 = v11;
            v22 -= v16;
            out[(size_t)at(1) * W + x] = v22;
            for (int i = 2; i < H; ++i) {
                const float xi = in[(size_t)at(i) * W + x];
                v16 = std::fmaf(v22, d[1], v17);
                v17 = std::fmaf(v22, d[2], v18);
                v18 = std::fmaf(v22, d[3], v19);
                v19 = v22 * d[4];
                v11 = std::fmaf(xi, nn[0], v12);
                v12 = std::fmaf(xi, nn[1], v13);
                v13 = std::fmaf(xi, nn[2], v14);
                v14 = std::fmaf(xi, nn[3], v15);
                v15 = pass == 0 ? 0.0f : xi * nn[4];
                v22 = v11;
                v22 -= v16;
                out[(size_t)at(i) * W + x] = v22;
            }
        }
    }
}

// The sigmas of the feather (width Fe in pixels, pixel aspect scales sx / sy): sqrt(((Fe + 1.5) s)^2 / c) with c = -2 ln(1/255) (as the card's host computes them, in floats).
inline void featherSigmas(float fe, float sx, float sy, float& sigx, float& sigy)
{
    const float c = -2.0f * std::log(0.00392156886f);
    const float ax = (fe + 1.5f) * sx, ay = (fe + 1.5f) * sy;
    sigx = std::sqrt((ax * ax) / c);
    sigy = std::sqrt((ay * ay) / c);
}

// mask: W x H floats, rows bottom first, blurred in place.
inline void feather(std::vector<float>& mask, int W, int H, float sigx, float sigy)
{
    std::vector<float> a, b, t((size_t)W * H);
    vfxgauss::Coefsf cf = vfxgauss::toFloats(vfxgauss::build((double)sigy));
    passColumns1(cf, mask, W, H, a, b);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) t[(size_t)x * H + y] = a[(size_t)y * W + x] + b[(size_t)y * W + x];
    cf = vfxgauss::toFloats(vfxgauss::build((double)sigx));
    passColumns1(cf, t, H, W, a, b);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) mask[(size_t)y * W + x] = a[(size_t)x * H + y] + b[(size_t)x * H + y];
}

// ---- the composite of one pixel. s: the picture's pixel (RGBA floats), m the (feathered) mask. cutSection (Method 1) shows the picture where the mask is 2 (outside the shape) and nothing where
// it is 0; Method 0 the other way. Without a border band the mask runs between "nothing" and the picture; with one, 0 .. 1 is the first one to the border colour and 1 .. 2 the colour to the
// other. A mix is fused: mix(a, b, t) = fma(b - a, t, a).
inline float mixf(float a, float b, float t) { return std::fmaf(b - a, t, a); }
inline void composite(const float* s, float m, bool cutSection, bool noBorder, const float* col, float* r)
{
    static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    const float* A = cutSection ? s : zero;      // what a mask of 2 shows
    const float* B = cutSection ? zero : s;      // what a mask of 0 shows
    for (int k = 0; k < 4; ++k) {
        if (m < K(0x3727c5acu)) r[k] = B[k];
        else if (m > K(0x3fffffacu)) r[k] = A[k];
        else if (noBorder) r[k] = mixf(B[k], A[k], m * 0.5f);
        else if (1.0f >= m) r[k] = mixf(B[k], col[k], m);
        else r[k] = mixf(col[k], A[k], m - 1.0f);
    }
}

}  // namespace vfxcookie
}  // namespace ofx
}  // namespace reno
