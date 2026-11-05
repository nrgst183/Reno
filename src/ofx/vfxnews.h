// VEGAS Newsprint as its OpenCL kernels draw it (Vfx1.ofx, newsprintOpenCLProcessor; read from the user's own VEGAS install). A re-implementation of what that code does, in floats, header only
// (no dependencies besides vfxgauss.h) so a test program can check it against the captured pictures.
//
// The steps (all pictures RGBA floats 0..1, rows bottom first, x = red ... as OpenCL's read_imagef hands them):
//   - pitch P = max(W, H) x DotSize / 20 + 3 pixels (Px = P / pixel aspect); the grid of dots is centred on the frame (the fractions of half the left-over size are taken off), every second row
//     half a pitch over;
//   - the picture is blurred with VEGAS's recursive Gaussian (vfxgauss.h: first down the columns, then along the rows), sigma = P / sqrt(2 ln 255) in each direction (pixel aspect scales one of them);
//   - a dot takes its colour (and so its radius, 1.25 x half the pitch x (1 - brightness)) from the BLURRED picture at the dot's grid position (read as the integer pixel, edges repeated),
//     brightness .114 x + .587 y + .299 z of the three channels as they come; a pixel is covered by the ellipse: fully inside, 0 outside, and in the one pixel wide edge by a table (the area of
//     a circle's segment, 256 entries); the paper (1.0) loses (1 - colour) x coverage per channel (Color), or the coverage alone (Monochrome);
//   - each pixel sums up five dots (its own cell, the cells to the left and right, and two cells of the row above / below); Offset print draws the three channels as three prints with five-cell
//     stencils of their own and centres moved by a quarter of a radius;
//   - Dot sizes under 0.15 are faded in from the unblurred picture (Monochrome: from its grey); alpha is the picture's.
// Bytes: an 8-bit project's pictures are 8-bit textures on the card (more::oclStore12 after each kernel).
#pragma once
#include "vfxgauss.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxnews {

struct Opts {
    bool quantMid = true;       // 8-bit project: the Gaussian's halves are 8-bit textures
    bool quantFinal = true;     // ... and so is the blurred copy
};

inline float store12f(float v)
{
    if (!(v == v)) return 0.f;
    const int n = (int)std::floor(std::min(std::max(v, 0.f), 1.f) * 4096.f);
    return (float)((n * 255 + 2047) >> 12) / 255.f;
}

// the 256 entries of the edge-pixel coverage (the kernel builds them from acos / sin of the entry's position)
inline const float* coverTable()
{
    static float t[256];
    static bool done = false;
    if (!done) {
        const float PI = 3.14159265358979323846f;
        const float Q = PI * 0.5f * 0.5f;
        for (int j = 0; j < 256; j += 4) {
            float v = (float)j / 256.0f - 0.5f;
            if (j < 128) v = std::fabs(v);
            const float a = std::acos(v / 0.5f);
            const float s = std::sin(a) * 0.5f;
            float area = Q * (a / PI) - 0.5f * std::sin(a) * (s * v);
            if (j < 128) area = Q - area;
            const float r = area / Q;
            for (int k = 0; k < 4; ++k) t[j + k] = (j + k == 128) ? 0.5f : std::min(std::max(r, 0.f), 1.f);
        }
        done = true;
    }
    return t;
}

struct Host {
    float v41x, v41y, v42x, v42y, v45x, v45y;
    int par;
    float sig1, sig2;      // sigma along y (columns), along x
    double P;
};

inline Host host(int W, int H, double dot, double pa, double s88, double s90, const Opts& /*o*/)
{
    Host h;
    const double P = (double)std::max(W, H) * dot / 20.0 + 3.0;
    const double Px = P / (double)(float)pa;
    const float offy = (float)(((double)H - (double)H / P) * 0.5);
    h.par = (~(int)offy) & 1;
    const float offx = (float)(((double)W - (double)W / Px) * 0.5);
    h.v41x = (float)Px; h.v41y = (float)P;
    h.v42x = (float)(-std::fmod((double)offx, 1.0)); h.v42y = (float)(-std::fmod((double)offy, 1.0));
    h.v45x = (float)pa / (float)s88; h.v45y = (float)(1.0 / s90);
    const float ln = (float)(2.0 * std::log(1.0 / 255.0));
    const float a1 = (float)(pa * P / (s88 / s90));
    const float a2 = (float)Px;
    const float q1 = -(a1 * a1 / ln), q2 = -(a2 * a2 / ln);
    h.sig1 = std::sqrt(q1);
    h.sig2 = std::sqrt(q2);
    h.P = P;
    return h;
}

inline void transposeRGBA(const std::vector<float>& in, int W, int H, std::vector<float>& out)
{
    out.resize(in.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) out[((size_t)x * H + y) * 4 + k] = in[((size_t)y * W + x) * 4 + k];
}

// the blurred copy: columns with sigY, then rows with sigX (the card's kernels: causal and anti-causal halves into two pictures, their sum read by the next pass)
inline void blurCopy(const std::vector<float>& src, int W, int H, float sigX, float sigY, bool q, const Opts& o, std::vector<float>& outB)
{
    auto pass = [&](const std::vector<float>& in, int w, int h, float sigma, std::vector<float>& res) {
        const vfxgauss::Coefsf cf = vfxgauss::toFloats(vfxgauss::build((double)sigma));
        std::vector<float> A, B;
        // (the card's recursion fused or not changes nothing in the captured pictures; the plain form is cheaper on a CPU without fused multiply-add)
        vfxgauss::passColumns(cf, in, w, h, A, B, [](float a, float b, float c) { return a * b + c; });
        if (q && o.quantMid) { for (float& v : A) v = store12f(v); for (float& v : B) v = store12f(v); }
        res.resize(A.size());
        for (size_t i = 0; i < A.size(); ++i) res[i] = A[i] + B[i];
    };
    std::vector<float> s1, t, s2;
    pass(src, W, H, sigY, s1);
    transposeRGBA(s1, W, H, t);
    pass(t, H, W, sigX, s2);
    if (q && o.quantFinal) for (float& v : s2) v = store12f(v);
    transposeRGBA(s2, H, W, outB);
}


inline float rte(float a) { return std::nearbyint(a); }

// one tap's data
struct Tap { float p[4]; float rx, ry; bool ok; };
inline Tap tapAt(const std::vector<float>& B, int W, int H, float sx, float sy, const Host& h)
{
    Tap t;
    int xi = (int)sx, yi = (int)sy;       // (truncated)
    xi = std::min(std::max(xi, 0), W - 1); yi = std::min(std::max(yi, 0), H - 1);
    const float* q = &B[((size_t)yi * W + xi) * 4];
    for (int k = 0; k < 4; ++k) t.p[k] = q[k];
    float lum = 0.114f * q[0] + 0.587f * q[1] + 0.299f * q[2];
    const float v60 = 1.0f - lum;
    t.rx = h.v41x * h.v45x * 0.5f * v60 * 1.25f;
    t.ry = h.v41y * h.v45y * 0.5f * v60 * 1.25f;
    t.ok = t.rx > 0.00392156862745098f && t.ry > 0.00392156862745098f;
    return t;
}
inline float coverAt(float X, float Y, float cx, float cy, float rx, float ry, const Host& h)
{
    const float dx = (X - cx) * h.v45x, dy = (Y - cy) * h.v45y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d > rx + 0.5f && d > ry + 0.5f) return 0.f;
    if (d < rx - 0.5f && d < ry - 0.5f) return 1.f;
    int idx = (int)((d - rx) * 255.0f + 128.0f);
    idx = std::min(std::max(idx, 0), 255);      // (the kernel reads its table unchecked; the unequal radii of a pixel aspect other than 1 could leave it)
    return coverTable()[idx];
}
inline float cellX(float X, const Host& h, float off) { return ((rte(X / h.v41x - off - h.v42x) + off) + h.v42x) * h.v41x; }
inline float cellY(float Y, const Host& h) { return (rte(Y / h.v41y - h.v42y) + h.v42y) * h.v41y; }

// One row y of the picture: src the picture, B its blurred copy (both W x H RGBA floats, rows bottom first), o4 the row's W pixels. type 0 Color, 1 Mono, 2 Offset; q8: an 8-bit project's store.
inline void drawRow(const std::vector<float>& src, const std::vector<float>& B, int W, int H, const Host& h, double dot, int type, bool q8, int y, float* o4)
{
    for (int x = 0; x < W; ++x) {
        const float X = (float)x, Y = (float)y;
        float acc[3] = {1.f, 1.f, 1.f};
        const float row = rte(Y / h.v41y - h.v42y);
        const int base = (((int)row + h.par) & 1) ? 1 : 0;
        float p58 = base ? 0.5f : 0.f;
        auto flip = [](float v) { return v > 0.f ? 0.f : 0.5f; };
        if (type != 2) {
            const bool mono = type == 1;
            auto tap = [&](float sx, float sy) {
                const Tap t = tapAt(B, W, H, sx, sy, h);
                if (!t.ok) return;
                const float c = coverAt(X, Y, sx, sy, t.rx, t.ry, h);
                for (int k = 0; k < 3; ++k) acc[k] -= mono ? c : (1.f - t.p[k]) * c;
            };
            // the cell's dot, its left and right neighbours, then (the other parity: half a cell over) the dots of the row below and the row above
            float vx = cellX(X, h, p58), vy = cellY(Y, h);
            tap(vx, vy);
            vx = vx - h.v41x; tap(vx, vy);
            vx = vx + 2.0f * h.v41x; tap(vx, vy);
            p58 = flip(p58);
            vx = cellX(X, h, p58); vy = cellY(Y, h) - h.v41y; tap(vx, vy);
            vy = vy + 2.0f * h.v41y; tap(vx, vy);
        } else {
            // three prints, one per channel (blue, green, red in turn), four dots each; the step says whether the cell parity flips (it stays flipped for the later steps) and the cell offset
            struct Step { int fl, dx, dy; };
            static const Step zs[4] = {{0, 0, 0}, {0, 1, 0}, {1, 0, -1}, {0, 1, -1}};
            static const Step ys[4] = {{0, 0, 0}, {1, 0, -1}, {1, -1, 0}, {1, -1, -1}};
            static const Step xs[4] = {{0, 0, 0}, {1, 0, 1}, {1, 1, 0}, {0, -1, 0}};
            for (int chn = 0; chn < 3; ++chn) {
                const int ch = chn == 0 ? 2 : (chn == 1 ? 1 : 0);
                const Step* seq = chn == 0 ? zs : (chn == 1 ? ys : xs);
                float pp = base ? 0.5f : 0.f;
                for (int s = 0; s < 4; ++s) {
                    if (seq[s].fl) pp = flip(pp);
                    float sx = cellX(X, h, pp), sy = cellY(Y, h);
                    if (seq[s].dx) sx = sx + (float)seq[s].dx * h.v41x;
                    if (seq[s].dy) sy = sy + (float)seq[s].dy * h.v41y;
                    const Tap t = tapAt(B, W, H, sx, sy, h);
                    if (!t.ok) continue;
                    float cx, cy;       // the print's centre moves a quarter of the radius: blue up and left, green up and right, red down
                    if (ch == 2) { cx = sx - t.rx / 4.0f; cy = sy + t.ry / 4.0f; }
                    else if (ch == 1) { cx = sx + t.rx / 4.0f; cy = sy + t.ry / 4.0f; }
                    else { cx = sx; cy = sy - t.ry / 4.0f; }
                    const float c = coverAt(X, Y, cx, cy, t.rx, t.ry, h);
                    acc[ch] -= (1.f - t.p[ch]) * c;
                }
            }
        }
        const float* s = &src[((size_t)y * W + x) * 4];
        if (dot < 0.15) {
            // small dots fade in from the unblurred picture (Monochrome: its grey); mix(a, b, t) = a + (b - a) t
            const float a = 1.0f - (float)dot / 0.15f;
            if (type == 1) {
                const float l = 0.114f * s[2] + 0.587f * s[1] + 0.299f * s[0];
                for (int k = 0; k < 3; ++k) acc[k] = acc[k] + (l - acc[k]) * a;
            } else {
                for (int k = 0; k < 3; ++k) acc[k] = acc[k] + (s[k] - acc[k]) * a;
            }
        }
        float* q = o4 + (size_t)x * 4;
        for (int k = 0; k < 3; ++k) q[k] = q8 ? store12f(acc[k]) : acc[k];
        q[3] = s[3];
    }
}

// The whole picture. src, out: W x H RGBA floats, rows bottom first. type 0 Color, 1 Mono, 2 Offset.
inline void render(const std::vector<float>& src, int W, int H, double dot, int type, double pa, double s88, double s90, bool q8, const Opts& o, std::vector<float>& out)
{
    const Host h = host(W, H, dot, pa, s88, s90, o);
    std::vector<float> B;
    blurCopy(src, W, H, h.sig2, h.sig1, q8, o, B);
    out.assign((size_t)W * H * 4, 0.f);
    for (int y = 0; y < H; ++y) drawRow(src, B, W, H, h, dot, type, q8, y, &out[(size_t)y * W * 4]);
}

}  // namespace vfxnews
}  // namespace ofx
}  // namespace reno
