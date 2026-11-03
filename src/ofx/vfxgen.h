// VEGAS's four media generators with OpenCL kernels (Solid Color, Checkerboard, Test Pattern, Noise Texture) as the GPU-accelerated VEGAS draws them: a
// re-implementation of the kernels' float arithmetic and of the arguments the plug-in's OpenCL processors hand them (read from the user's own VEGAS
// install, Vfx1.ofx; notes: vegas_code/Generators_ocl.md), checked bit for bit against the decoded kernels run on the CPU. No dependencies, so a test
// program can include it. Every function gives the kernel's float result for one pixel (before the 8-bit store, which is more::oclStore12 in Reno).
//
// Rows: the kernels' image row 0 is the bottom of the picture; the functions take the row counted from the BOTTOM (yk) like the kernels do, Reno's
// buffers count from the top (yk = H - 1 - y). All arithmetic is float with no fused multiply-add (mad is a * b + c), as the kernel shim does it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace reno {
namespace ofx {
namespace vfxgen {

// float -> whole number towards zero (convert_int), with the machine's "indefinite" value for what does not fit
inline int cvtInt(float v) { return (v > -2147483648.0f && v < 2147483648.0f) ? (int)v : (int)0x80000000u; }
// convert_uint of a float (what the card gives: below 0 and not a number 0, above the range all ones)
inline unsigned cvtUint(float v)
{
    if (!(v > 0.f)) return 0u;
    if (v >= 4294967296.0f) return 0xffffffffu;
    return (unsigned)v;
}
inline float clampf(float v, float a, float b) { return std::fmin(std::fmax(v, a), b); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

// ------------------------------------------------------------------ Solid Color
// kernel arguments: the colour as floats (red, green, blue, alpha), the picture's size; the kernel writes (r a, g a, b a, a)
inline void solidColor(const double rgba[4], float out[4])
{
    const float r = (float)rgba[0], g = (float)rgba[1], b = (float)rgba[2], a = (float)rgba[3];
    out[0] = r * a;
    out[1] = g * a;
    out[2] = b * a;
    out[3] = a;
}

// ------------------------------------------------------------------ Test Pattern
// kernel arguments: the pattern (the choice's index, 0 to 8), the picture's size. The values are broadcast levels over 255 (235 white, 180 75% bars,
// 16 black or pluge, 7 below black, 25 above), x and the row from the bottom are the pixel's indices.
namespace tp {
const float one = 1.0f, white = (float)(235.0f / 255.0f), bar = (float)(180.0f / 255.0f), l125 = (float)(125.0f / 255.0f), l70 = (float)(70.0f / 255.0f),
            blk = (float)(16.0f / 255.0f), sub = (float)(7.0f / 255.0f), plu = (float)(25.0f / 255.0f), iq = (float)(113.0f / 255.0f), iq2 = (float)(40.0f / 255.0f);
struct C { float r, g, b, a; };
inline C c3(float r, float g, float b) { return C{r, g, b, one}; }
// the picture inside a frame of black (7) a tenth of the picture wide and high
inline C frame(C c, float x, float y, int w, int h)
{
    const float fx = (float)w / 10.0f, fy = (float)h / 10.0f;
    if (x < fx) return c3(sub, sub, sub);
    if (x > (float)w - fx) return c3(sub, sub, sub);
    if (y < fy) return c3(sub, sub, sub);
    if (y > (float)h - fy) return c3(sub, sub, sub);
    return c;
}
}   // namespace tp

inline void testPattern(int type, int w, int h, int x, int yk, float out[4])
{
    using namespace tp;
    const float fx = (float)x, fy = (float)yk;
    C v = {0.f, 0.f, 0.f, 1.f};      // (a pattern number the kernel does not know: black)
    // the colour bars: white, yellow, cyan, green, magenta, red, blue at 75% (the PAL ones start with a 100% white)
    auto colourBars = [&](bool pal) {
        const int u = w / 7;
        const float hi = pal ? white : bar;
        if (fx < (float)u) return c3(hi, hi, hi);
        if (fx < (float)(2 * u)) return c3(bar, bar, blk);
        if (fx < (float)(3 * u)) return c3(blk, bar, bar);
        if (fx < (float)(4 * u)) return c3(blk, bar, blk);
        if (fx < (float)(5 * u)) return c3(bar, blk, bar);
        if (fx < (float)(6 * u)) return c3(bar, blk, blk);
        return c3(blk, blk, bar);
    };
    // the strip under the SMPTE bars' bars: blue, black, magenta, black, cyan, black, white
    auto reverse = [&]() {
        const int u = w / 7;
        if (fx < (float)u) return c3(sub, sub, white);
        if (fx < (float)(2 * u)) return c3(blk, blk, blk);
        if (fx < (float)(3 * u)) return c3(bar, sub, bar);
        if (fx < (float)(4 * u)) return c3(blk, blk, blk);
        if (fx < (float)(5 * u)) return c3(sub, bar, bar);
        if (fx < (float)(6 * u)) return c3(blk, blk, blk);
        return c3(bar, bar, bar);
    };
    auto pluge = [&](bool ntsc) {
        const int u = w / 21;
        if (ntsc) {
            if (fx < ((float)w / 6.0f)) return c3(sub, iq2, iq);
            if (fx < ((float)w / 3.0f)) return c3(white, white, white);
            if (fx < ((float)w / 2.0f)) return c3(l70, sub, 0.65f);
        }
        if (fx < (float)(15 * u)) return c3(blk, blk, blk);
        if (fx < (float)(16 * u)) return c3(sub, sub, sub);
        if (fx < (float)(17 * u)) return c3(blk, blk, blk);
        if (fx < (float)(18 * u)) return c3(plu, plu, plu);
        return c3(blk, blk, blk);
    };
    auto border = [&](C c) { return frame(c, fx, fy, w, h); };
    switch (type) {
    case 0: v = colourBars(false); break;
    case 1: v = colourBars(true); break;
    case 2: {
        const float ix = 1.0f / (float)w, iy = 1.0f / (float)h;
        v.r = 1.0f - (fx * ix);
        v.g = 0.0f + (fx * ix);
        v.b = 1.0f - (fy * iy);
        v.a = 1.0f;
        break;
    }
    case 3: {
        if (fx < (float)w * 0.2f) v = c3(sub, sub, sub);
        else if (fx < (float)w * 0.3f) v = c3(blk, blk, blk);
        else if (fx < (float)w * 0.4f) v = c3(plu, plu, plu);
        else if (fx < (float)w * 0.5f) v = c3(blk, blk, blk);
        else if (fy > (float)h * 0.7f) v = c3(white, white, white);
        else if (fy > (float)h * 0.5f) v = c3(bar, bar, bar);
        else if (fy > (float)h * 0.3f) v = c3(l125, l125, l125);
        else v = c3(l70, l70, l70);
        v = border(v);
        break;
    }
    case 4: {
        const float i = 1.0f / (float)w;
        const float g = fx * i;
        v = border(C{g, g, g, 1.0f});
        break;
    }
    case 5:
        if (yk < h / 4) v = pluge(true);
        else if (yk < h / 3) v = reverse();
        else v = colourBars(false);
        break;
    case 6:
        if (yk < h / 4) v = pluge(false);
        else if (yk < h / 3) v = reverse();
        else v = colourBars(true);
        break;
    case 7:
        if (fx < (float)w * 0.3f) v = c3(one, one, one);
        else if (fx < (float)w * 0.5f) v = c3(bar, bar, bar);
        else if (fx < (float)w * 0.7f) v = c3(l125, l125, l125);
        else v = c3(l70, l70, l70);
        v = border(v);
        break;
    case 8:
        if (fx < (float)w * 0.3f) v = c3(one, one, sub);
        else if (fx < (float)w * 0.5f) v = c3(bar, bar, sub);
        else if (fx < (float)w * 0.7f) v = c3(l125, l125, sub);
        else v = c3(l70, l70, sub);
        v = border(v);
        break;
    default: break;
    }
    out[0] = v.r; out[1] = v.g; out[2] = v.b; out[3] = v.a;
}

// ------------------------------------------------------------------ Checkerboard
// What the plug-in hands the kernel (all float): tile = (w, h) of a tile in pixels, hh = half the width of the blended edge (at least half a pixel),
// inv = 0.5 / hh, off = the grid offset in pixels; c2 = Color 2 (the first, "background") and c1 = Color 1, both premultiplied.
struct Checker {
    int w = 0, h = 0;
    float tile[2] = {1, 1}, hh[2] = {0.5f, 0.5f}, inv[2] = {1, 1}, off[2] = {0, 0};
    float c2[4] = {0, 0, 0, 0}, c1[4] = {0, 0, 0, 0};
};
// tileW, tileH: the tile settings (fractions of the frame), square, edgeH, edgeV: the blending settings, offX, offY: the grid offset settings (fractions
// of the frame), col1 and col2 (red, green, blue, alpha), par: the pixel aspect ratio of the picture
inline Checker checkerHost(int w, int h, double tileW, double tileH, bool square, double edgeH, double edgeV, double offX, double offY, const double col1[4], const double col2[4],
                           float par = 1.0f)
{
    Checker k;
    k.w = w;
    k.h = h;
    const float inv = 1.0f / par;
    float tw = (float)tileW * (float)w;
    float th = (float)tileH * (float)h;
    if (square) th = tw * par;
    if (inv > tw) {
        th = th * inv;
        th = th / tw;
        tw = inv;
    }
    if (1.0f > th) {
        tw = tw / th;
        th = 1.0f;
    }
    float hx = (float)edgeH * tw;
    hx = hx * 0.5f;
    hx = (0.5f > hx) ? 0.5f : hx;           // (maxss: not a number gives the second operand)
    float hy = (float)edgeV * th;
    hy = hy * 0.5f;
    hy = (0.5f > hy) ? 0.5f : hy;
    k.tile[0] = tw; k.tile[1] = th;
    k.hh[0] = hx; k.hh[1] = hy;
    k.inv[0] = 0.5f / hx; k.inv[1] = 0.5f / hy;
    k.off[0] = (float)w * (float)offX;
    k.off[1] = (float)h * (float)offY;
    for (int i = 0; i < 3; ++i) {
        k.c1[i] = (float)(col1[i] * col1[3]);
        k.c2[i] = (float)(col2[i] * col2[3]);
    }
    k.c1[3] = (float)col1[3];
    k.c2[3] = (float)col2[3];
    return k;
}
// the pixel x of the row yk (from the bottom)
inline void checker(const Checker& k, int x, int yk, float out[4])
{
    const float px = (float)x + 0.5f - k.off[0];
    const float py = (float)(k.h - 1 - yk) + 0.5f - k.off[1];
    const float rx = 1.0f / k.tile[0], ry = 1.0f / k.tile[1];
    int ix = cvtInt(px * rx), iy = cvtInt(py * ry);
    ix -= (px < (float)ix * k.tile[0]) ? 1 : 0;
    iy -= (py < (float)iy * k.tile[1]) ? 1 : 0;
    const float fix = (float)ix, fiy = (float)iy;
    float x0 = fix * k.tile[0] + k.hh[0];
    const float y0 = fiy * k.tile[1] + k.hh[1];
    float x1 = (fix + 1.0f) * k.tile[0] + (-k.hh[0]);
    const float y1 = (fiy + 1.0f) * k.tile[1] + (-k.hh[1]);
    float bg[4], fg[4];
    std::memcpy(bg, k.c2, sizeof bg);
    std::memcpy(fg, k.c1, sizeof fg);
    float colA[4], colB[4];     // the colours of the tile with an odd / even index sum: "on" (this tile's) and "off" (the neighbour's)
    float result[4] = {bg[0], bg[1], bg[2], bg[3]};
    float vA[4], vB[4];
    if (y0 <= py && py < y1) {
        std::memcpy(vA, bg, sizeof vA);     // odd tiles show the background colour, even ones the foreground
        std::memcpy(vB, fg, sizeof vB);
    } else {
        const float t = (py < y0) ? (y0 - py) * k.inv[1] : (py - y1) * k.inv[1];
        for (int i = 0; i < 4; ++i) {
            vB[i] = mixf(fg[i], bg[i], t);      // (even tile's colour as the edge softens)
            vA[i] = mixf(bg[i], fg[i], t);
        }
    }
    int ixx = ix;
    for (int pass = 0; pass < 2; ++pass) {
        const bool odd = ((ixx + iy) & 1) != 0;
        for (int i = 0; i < 4; ++i) {
            colA[i] = odd ? vA[i] : vB[i];
            colB[i] = odd ? vB[i] : vA[i];
        }
        if (px < x0) {
            const float t = (x0 - px) * k.inv[0];
            for (int i = 0; i < 4; ++i) result[i] = mixf(colA[i], colB[i], t);
            break;
        }
        if (px < x1) {
            for (int i = 0; i < 4; ++i) result[i] = colA[i];
            break;
        }
        ixx += 1;
        x0 += k.tile[0];
        x1 += k.tile[0];
    }
    for (int i = 0; i < 4; ++i) out[i] = result[i];
}

// ------------------------------------------------------------------ Noise Texture
// What the plug-in hands the kernels (float): the picture's size, the style (0 to 6, else 0), the swap flag, the frequency (x, y), the number of
// layers, the offset (x, y), the progress (the third axis), the grain falloff, min, max, bias, amplitude and the two premultiplied colours.
struct Noise {
    int w = 0, h = 0, style = 0;
    bool swap = false;
    float freq[2] = {1, 1}, layers = 1, off[2] = {0, 0}, z = 0, falloff = 0.5f, lo = 0, hi = 1, bias = 0, amp = 1;
    float c1[4] = {0, 0, 0, 0}, c2[4] = {0, 0, 0, 0};
};
inline Noise noiseHost(int w, int h, int style, bool swap, double fx, double fy, double layers, double ox, double oy, double z, double falloff, double lo, double hi,
                       double bias, double amp, const double col1[4], const double col2[4])
{
    Noise k;
    k.w = w;
    k.h = h;
    k.style = (style >= 0 && style < 7) ? style : 0;
    k.swap = swap;
    k.freq[0] = (float)fx; k.freq[1] = (float)fy;
    // (the settings run from 1 to 5 layers and the kernel's tables hold five: anything else is brought into 0 to 5)
    float l = (float)layers;
    if (!(l >= 0.f)) l = 0.f;
    if (l > 5.f) l = 5.f;
    k.layers = l;
    k.off[0] = (float)ox; k.off[1] = (float)oy;
    k.z = (float)z;
    k.falloff = (float)falloff;
    k.lo = (float)lo; k.hi = (float)hi; k.bias = (float)bias; k.amp = (float)amp;
    for (int i = 0; i < 3; ++i) {
        k.c1[i] = (float)(col1[i] * col1[3]);
        k.c2[i] = (float)(col2[i] * col2[3]);
    }
    k.c1[3] = (float)col1[3];
    k.c2[3] = (float)col2[3];
    return k;
}

// the numbers both kernels work out the same way
struct NoiseLayers {
    float weight[6];
    int count;
    float sum;
};
inline NoiseLayers noiseLayers(const Noise& k)
{
    NoiseLayers r;
    for (int i = 0; i < 6; ++i) r.weight[i] = 0.f;
    r.weight[0] = 1.0f;
    unsigned n = cvtUint(std::floor(k.layers)) + 1u;
    if (n > 6u) n = 6u;
    const float frac = k.layers - (float)(n - 1u);
    for (unsigned i = 1; i < n; ++i) r.weight[i] = r.weight[i - 1] * k.falloff;
    r.weight[n - 1] *= frac;
    unsigned used = 0;
    float sum = 0.0f;
    while (used < n && r.weight[used] > 0.01f) {
        sum += r.weight[used];
        ++used;
    }
    r.count = (int)used;
    r.sum = sum;
    return r;
}
inline bool noiseFolds(int style) { return style == 1 || style == 2 || style == 3 || style == 4; }

// One row's layers (kernel 1): row yk from the bottom; out[layer][x] for the layers noiseLayers counts. perm: the 256 permutation bytes, grad: the 256
// gradients (x, y, z).
inline void noiseRow(const Noise& k, const unsigned char* perm, const float (*grad)[3], int yk, std::vector<float>& out)
{
    const int W = k.w, H = k.h;
    const NoiseLayers L = noiseLayers(k);
    out.assign((size_t)std::max(1, L.count) * (size_t)std::max(1, W), 0.f);
    if (W <= 0 || L.count <= 0) return;
    short p[512];
    for (int i = 0; i < 256; ++i) p[i] = p[i + 256] = (short)perm[i];
    const bool fold = noiseFolds(k.style);
    const float step = 1.0f / (float)W;
    // the position of the row's first pixel and the step along x
    float pos[4];
    pos[0] = k.off[0] * k.freq[0];
    pos[1] = ((float)(H - 1 - yk) * step + k.off[1]) * k.freq[1];
    pos[2] = k.z;
    pos[3] = step * k.freq[0];
    for (int layer = 0; layer < L.count; ++layer) {
        const float scale = std::pow(2.0f, (float)layer);
        float v48[4] = {pos[0] * scale, pos[1] * scale, pos[2] * scale, pos[3] * scale};
        const float s = v48[3];
        v48[0] += 10000.0f;
        v48[1] += 10000.0f;
        v48[2] += (10000.0f + 0.25f);
        v48[3] += 10000.0f;
        int cell[4];
        float fr[4];
        for (int i = 0; i < 4; ++i) {
            cell[i] = (int)std::floor(v48[i]);
            fr[i] = v48[i] - (float)cell[i];
            cell[i] += 990000;
        }
        float fade[4];
        for (int i = 0; i < 4; ++i) {
            const float a = fr[i] * 6.0f + (-15.0f);
            const float b = fr[i] * a + 10.0f;
            fade[i] = fr[i] * fr[i] * fr[i] * b;
        }
        int a0[4], a1[4];
        for (int i = 0; i < 4; ++i) {
            a0[i] = cell[i] & 255;
            a1[i] = (cell[i] + 1) & 255;
        }
        int v41[4];
        v41[0] = p[p[a0[2]] + a0[1]];
        v41[1] = p[p[a0[2]] + a1[1]];
        v41[2] = p[p[a1[2]] + a0[1]];
        v41[3] = p[p[a1[2]] + a1[1]];
        float lo2[2] = {0, 0}, hi2[2] = {0, 0};
        auto cellValues = [&]() {
            int g[8];
            for (int i = 0; i < 4; ++i) {
                g[2 * i] = p[v41[i] + a0[0]];
                g[2 * i + 1] = p[v41[i] + a1[0]];
            }
            // fr[3] is 0 here, as the kernel sets it
            auto dot3 = [&](int gi, float dx, float dy, float dz) {
                const float* v = grad[gi];
                return v[0] * dx + v[1] * dy + v[2] * dz + 0.0f * 0.0f;
            };
            float d[8];
            const float f0 = fr[0], f1 = fr[1], f2 = fr[2];
            d[0] = dot3(g[0], f0, f1, f2);
            d[1] = dot3(g[1], f0 - 1.0f, f1, f2);
            d[2] = dot3(g[2], f0, f1 - 1.0f, f2);
            d[3] = dot3(g[3], f0 - 1.0f, f1 - 1.0f, f2);
            d[4] = dot3(g[4], f0, f1, f2 - 1.0f);
            d[5] = dot3(g[5], f0 - 1.0f, f1, f2 - 1.0f);
            d[6] = dot3(g[6], f0, f1 - 1.0f, f2 - 1.0f);
            d[7] = dot3(g[7], f0 - 1.0f, f1 - 1.0f, f2 - 1.0f);
            float m1[4] = {d[0], d[2], d[1], d[3]};
            float m2[4] = {d[4], d[6], d[5], d[7]};
            float m3[4];
            for (int i = 0; i < 4; ++i) m3[i] = mixf(m1[i], m2[i], fade[2]);
            float gx1[4] = {grad[g[0]][0], grad[g[2]][0], grad[g[1]][0], grad[g[3]][0]};
            float gx2[4] = {grad[g[4]][0], grad[g[6]][0], grad[g[5]][0], grad[g[7]][0]};
            float m4[4];
            for (int i = 0; i < 4; ++i) m4[i] = mixf(gx1[i], gx2[i], fade[2]);
            const float e1[4] = {m3[0], m3[2], m4[0], m4[2]};
            const float e2[4] = {m3[1], m3[3], m4[1], m4[3]};
            float m5[4];
            for (int i = 0; i < 4; ++i) m5[i] = mixf(e1[i], e2[i], fade[1]);
            m5[2] *= s;
            m5[3] *= s;
            lo2[0] = m5[0]; lo2[1] = m5[1];
            hi2[0] = m5[2]; hi2[1] = m5[3];
        };
        cellValues();
        const float w = 1.515151515f * L.weight[layer];
        float* o = &out[(size_t)layer * (size_t)W];
        float fx = fr[0];
        for (int x = 0; x < W; ++x) {
            const float a = fx * 6.0f + (-15.0f);
            const float b = fx * a + 10.0f;
            const float fadeX = fx * fx * fx * b;
            float v = mixf(lo2[0], lo2[1], fadeX) * w;
            v = fold ? std::fabs(v) : v;
            o[x] = v;
            fx += s;
            if (fx >= 1.0f) {
                const int n = (int)cvtUint(std::floor(fx));
                cell[0] += n;
                fx -= (float)n;
                fr[0] = fx;
                a0[0] = cell[0] & 255;
                a1[0] = (cell[0] + 1) & 255;
                cellValues();
            } else {
                lo2[0] += hi2[0];
                lo2[1] += hi2[1];
            }
        }
    }
}

// One pixel (kernel 2) from the layers of its row (rows[layer * w + x] as noiseRow made them), its mix value 0..1 between the two colours.
inline float noiseMixOf(const Noise& k, const NoiseLayers& L, const std::vector<float>& rows, int x, int yk)
{
    const int W = k.w, H = k.h;
    const bool fold = noiseFolds(k.style);
    static const float T[7][5] = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0.9f, 0.15f, 0.4f, 1.2f, 0.1f}, {0.05f, 0.9f, 1.3f, 0.0f, 0.0f},
                                  {0.6f, 0.0f, 0.2f, 0.5f, 0.1f}, {1.0f, 0.0f, 1.5f, 0.5f, 0.05f}, {4.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
    const float lo = k.lo, hi = k.hi, bias = k.bias, amp = k.amp;
    float v44 = (hi <= lo) ? 0.0f : amp * (1.0f / (hi - lo)) * 65536.0f;
    float v45 = (hi <= lo) ? ((lo + hi) / 2.0f * amp + bias) * 65536.0f + 0.5f : -(lo * v44) + bias * 65536.0f + 0.5f;
    const float step = 1.0f / (float)W;
    const float v23x = step * k.freq[0], v23y = -step * k.freq[1];
    float sum = 0.0f;
    if (L.count > 0) {
        sum = rows[(size_t)x];
        for (int i = 1; i < L.count; ++i) sum += rows[(size_t)i * (size_t)W + (size_t)x];
    }
    unsigned v65;
    const int mode = (k.style < 2) ? 0 : (k.style < 6 ? 1 : 2);
    if (mode == 0) {
        float v47;
        if (fold) {
            v47 = L.sum;
        } else {
            v45 = v44 * 0.5f + v45;
            v47 = L.sum + L.sum;
        }
        v44 *= (1.0f / v47);
        v65 = cvtUint(sum * v44 + v45 + 0.5f);
    } else if (mode == 2) {
        float v67 = T[k.style][0];
        v44 /= 2.0f;
        v67 *= L.sum;
        float v68 = (sum + 2.0f) * v67;
        v68 = v68 - (float)(int)std::floor(v68);
        if (v68 < 0.5f) {
            v68 = v68 * 4.0f + (-1.0f);
            v68 = v68 * (-v68) + 2.0f;
        } else {
            v68 = v68 * 4.0f + (-3.0f);
            v68 = v68 * v68;
        }
        v65 = cvtUint(v68 * v44 + v45);
    } else {
        const float* t = T[k.style];
        const float v69 = L.sum * t[2];
        const float v70 = v23x * t[0];
        float v71 = -((float)W * k.off[1] + (float)H - 1.0f - (float)yk) * v23y * t[1];
        if (t[3] > 0.0f) {
            float v68 = -((float)W * (k.off[1] - 1000.0f) + (float)H - 1.0f - (float)yk) * v23y * t[3];
            v68 = v68 - (float)(int)std::floor(v68);
            if (v68 < 0.5f) {
                v68 = v68 * 4.0f + (-1.0f);
                v68 = v68 * (-v68) + 1.0f;
            } else {
                v68 = v68 * 4.0f + (-3.0f);
                v68 = v68 * v68 + (-1.0f);
            }
            v71 += v68 * t[4];
        }
        const float v72 = ((float)W * k.off[0]) * v70 + v71;
        float v68 = (sum * v69 + (float)x * v70) + v72;
        const int fl = (int)std::floor(v68);
        v68 = 2.0f * (v68 - (float)fl) + (-1.0f);
        v68 = v68 * (-v68) + 1.0f;
        v65 = cvtUint(v68 * v44 + v45);
    }
    return clampf((float)v65 * 0.0000152587890625f, 0.0f, 1.0f);
}
// the colour of the pixel for the mix value t (the kernel's final mix of the two colours, swapped when asked)
inline void noiseColour(const Noise& k, float t, float out[4])
{
    const float* a = k.swap ? k.c2 : k.c1;
    const float* b = k.swap ? k.c1 : k.c2;
    for (int i = 0; i < 4; ++i) out[i] = mixf(a[i], b[i], t);
}

}   // namespace vfxgen
}   // namespace ofx
}   // namespace reno
