// VEGAS's Portals transition (sfpagepeel.dll, the old DirectX transition the "VEGAS Portals" entry runs) as that code draws it, 8-bit frames.
//
//  * layout: a shift-register random generator (vfxrand.h, seeded with the effect's seed) makes N rectangles; the progress decides which of them are drawn and how
//    strongly (the "2N + 1 steps" counting below);
//  * each rectangle shows a part of the incoming picture (a slightly moved and shrunk rectangle of it) stretched into the rectangle, with a thin border around it;
//  * the stretch is a bilinear one in 14-bit fixed point with the edge pixels weighted by how much of them the rectangle covers; mixing is by alpha in whole levels.
// The layout, the stretch and the border were checked against the DLL's own functions (several thousand random cases, no difference), the whole picture against
// VEGAS's pictures of the default, Mondrian and further presets (no difference).
#pragma once
#include "vfxrand.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxportals {

struct Props {
    int seed = 634521;
    int squares = 6;
    double maxTransparency = 0.85;      // the most opaque a rectangle gets
    double maxOffset = 0.1;
    double maxScale = 0.05;
    double borderSize = 0.1;
    float borderColor[3] = {0.f, 0.f, 0.f};     // in the order of the frame's bytes (the first lands in byte 0 of the border's pixel)
};

struct Item {
    float r[4];         // where it is drawn: x0, y0, x1, y1 as fractions of the frame (clamped to 0..1)
    float r2[4];        // which part of the picture it shows (not clamped)
    int incoming;       // 1: the incoming picture, 0: the outgoing one
    float alpha;
    bool noBorder;
};

// the C remainder of the low 10 bits taken as a signed number (what "word % 1024" gives)
inline int signedLow10(uint32_t w)
{
    int r = (int)(w & 0x800003ffu);
    if (r < 0) r = ((r - 1) | (int)0xfffffc00) + 1;
    return r;
}

// aspect: the ratio the layout stretches its rectangles by (1 for the frames VEGAS drew: the pictures fit when it is 1)
inline std::vector<Item> layout(const Props& p, double progress, double aspect)
{
    builtin::VegasShiftRandom rng((uint32_t)p.seed);
    const int N = p.squares;
    std::vector<Item> items;
    const double x = (double)(2 * N + 1) * progress;
    const int k = (int)x;
    const double f = x - (double)k;
    const double cell = std::sqrt(1.0 / (double)N) * 1.4;
    const double inv = 1.0 / 1024.0;
    auto clamp01 = [](float v) { return v < 0.f ? 0.f : std::min(1.0f, v); };
    for (int i = 0; i < N; ++i) {
        Item it;
        it.incoming = 1;
        it.noBorder = false;
        const float cx = (float)((double)(rng.next() & 0x3ff) * inv), cy = (float)((double)(rng.next() & 0x3ff) * inv);
        float hw = (float)((double)signedLow10(rng.next()) * inv * cell + cell), hh = (float)((double)signedLow10(rng.next()) * inv * cell + cell);
        if (1.0 > aspect) hw = (float)((double)hw / aspect);
        else hh = (float)((double)hh * aspect);
        hw *= 0.5f;
        hh *= 0.5f;
        const float x0 = cx - hw, x1 = hw + cx, y0 = cy - hh, y1 = hh + cy;
        it.r[0] = clamp01(x0);
        it.r[2] = clamp01(x1);
        it.r[1] = clamp01(y0);
        it.r[3] = clamp01(y1);
        for (int j = 0; j < 4; ++j) it.r2[j] = it.r[j];
        const double jit = p.maxOffset * 0.4 * cell;
        const float a = (float)((double)signedLow10(rng.next()) * inv * jit), b = (float)((double)signedLow10(rng.next()) * inv * jit);
        it.r2[2] += a;
        it.r2[0] += a;
        it.r2[1] += b;
        it.r2[3] += b;
        const float g = (float)((double)signedLow10(rng.next()) * inv * (p.maxScale * 0.4 * cell));
        it.r2[0] += g;
        it.r2[1] += g;
        it.r2[2] -= g;
        it.r2[3] -= g;
        double al;
        if (k < N) al = i < k ? 1.0 : (i == k ? f : 0.0);
        else if (k == N) al = 1.0;
        else {
            const int m = k - N, q = N - i;
            al = q < m ? 0.0 : (q == m ? 1.0 - f : 1.0);
        }
        it.alpha = (float)(al * p.maxTransparency);
        items.push_back(it);
    }
    // the whole-frame backdrop: the outgoing picture, fading to the incoming one in the middle step, the incoming one after
    std::vector<Item> out;
    Item bg;
    for (int j = 0; j < 4; ++j) bg.r[j] = bg.r2[j] = 0.f;
    bg.r[2] = bg.r[3] = bg.r2[2] = bg.r2[3] = 1.0f;
    bg.noBorder = true;
    bg.alpha = 1.0f;
    if (k < N) {
        bg.incoming = 0;
        out.push_back(bg);
    } else if (k == N) {
        bg.incoming = 0;
        out.push_back(bg);
        Item b2 = bg;
        b2.incoming = 1;
        b2.alpha = (float)f;
        out.push_back(b2);
    } else {
        bg.incoming = 1;
        out.push_back(bg);
    }
    for (const Item& i : items) out.push_back(i);
    return out;
}

// ---- the stretch ----
struct IRect { int x0, y0, x1, y1; };      // 14-bit fixed point

inline int div14(int v) { return (v + ((v >> 31) & 0x3fff)) >> 14; }

inline bool intersect(IRect& o, const IRect& a, const IRect& b)
{
    o.x0 = std::max(a.x0, b.x0);
    o.y0 = std::max(a.y0, b.y0);
    o.x1 = std::min(a.x1, b.x1);
    o.y1 = std::min(a.y1, b.y1);
    if (o.x0 >= o.x1 || o.y0 >= o.y1) {
        o = IRect{0, 0, 0, 0};
        return false;
    }
    return true;
}

struct Plan {
    IRect D, S;
    int xstep, ystep;
};

// Clips the two rectangles to the pictures and works out the steps through the source per destination pixel.
inline bool planStretch(Plan& c, int dW, int dH, int sW, int sH, IRect DR, IRect SR)
{
    const IRect sb = {0, 0, sW << 14, sH << 14}, db = {0, 0, dW << 14, dH << 14};
    IRect SRc;
    intersect(SRc, SR, sb);
    if (SRc.x1 - SRc.x0 < 0x4000 || SRc.y1 - SRc.y0 < 0x4000) return false;
    if (DR.x1 - DR.x0 < 0x400 || DR.y1 - DR.y0 < 0x400) return false;
    const int ratioX = (int)((double)(SRc.x1 - SRc.x0) * 16384.0 / (double)(DR.x1 - DR.x0));
    const int ratioY = (int)((double)(SRc.y1 - SRc.y0) * 16384.0 / (double)(DR.y1 - DR.y0));
    IRect DRc;
    intersect(DRc, DR, db);
    if (DRc.x1 - DRc.x0 < 0x400 || DRc.y1 - DRc.y0 < 0x400) return false;
    const int sw = SRc.x1 - SRc.x0, sh = SRc.y1 - SRc.y0;
    IRect sd;
    sd.x0 = DR.x0;
    sd.y0 = DR.y0;
    sd.x1 = (int)((double)sw * 16384.0 / (double)ratioX) + DR.x0;
    sd.y1 = (int)((double)sh * 16384.0 / (double)ratioY) + DR.y0;
    IRect E;
    intersect(E, sd, DRc);
    const double C = 1.0 / 16384.0;
    IRect S2;
    S2.x0 = (int)((double)(E.x0 - DR.x0) * (double)ratioX * C) + SRc.x0;
    S2.y0 = (int)((double)(E.y0 - DR.y0) * (double)ratioY * C) + SRc.y0;
    S2.x1 = (int)((double)(E.x1 - DR.x0) * (double)ratioX * C) + SRc.x0;
    S2.y1 = (int)((double)(E.y1 - DR.y0) * (double)ratioY * C) + SRc.y0;
    IRect S3;
    intersect(S3, S2, SRc);
    c.D = DRc;
    c.S = S3;
    const int cw = DRc.x1 - DRc.x0;
    if (cw < 0x8000) c.xstep = 0;
    else {
        const int v = (int)((double)((S3.x1 - S3.x0) - 0x4000) * 16384.0 / (double)(cw - 0x4000));
        c.xstep = v <= 0 ? 0 : v;
    }
    const int ch = DRc.y1 - DRc.y0;
    if (ch < 0x8000) c.ystep = 0;
    else {
        const int v = (int)((double)((S3.y1 - S3.y0) - 0x4000) * 16384.0 / (double)(ch - 0x4000));
        c.ystep = v <= 0 ? 0 : v;
    }
    return true;
}

// Mixing modes: Lerp (the picture by its alpha) and Over (a premultiplied pixel over what is there: the border).
enum class Mix { Lerp, Over };

// One stretch as the drawing does it, worked out but not drawn (for the GPU, which draws the list of them in order, every pixel on its own): what stretch() takes from
// planStretch and its row / column counters. A pixel (x, y) of [px0, px1) x [py0, py1) takes the source row syf (the first row: sy0; the last: none, the source's last row;
// the others syStart + (y - py0 - 1) * ystep) and column sxf (the first: sx0; the others sx0 + step1 + (x - px0 - 1) * xstep), scaled by the coverage of its row / column.
struct Op {
    int mix;                    // 0: Lerp, 1: Over
    int src;                    // 0: the outgoing picture, 1: the incoming one, 2: a single pixel (cpx)
    int px0, px1, py0, py1;
    int firstRowCov, lastRowCov, firstColCov, lastColCov;
    int sx0, sy0, xstep, ystep, step1, syStart;
    int sw1, sh1;
    int alpha14;
    uint8_t cpx[4];
};

inline void mixPixel(Mix mode, uint8_t* d, const uint8_t* s, int a)
{
    if (mode == Mix::Over) {
        if (a == 0x4000) {
            const int r9 = 255 - s[3];
            for (int i = 0; i < 4; ++i) {
                int t = d[i] * r9 + 128;
                t = (t + (t >> 8)) >> 8;
                t += s[i];
                d[i] = (uint8_t)(t < 0 ? 0 : (t > 255 ? 255 : t));
            }
            return;
        }
        int m = (a * 255 + 0x2000 + (((a * 255 + 0x2000) >> 31) & 0x3fff)) >> 14;
        m &= 255;
        const int sa = s[3];
        const int r9 = 255 - (((sa * m + 0x7f) / 255) & 255);
        for (int i = 0; i < 3; ++i) {
            const int sc = std::min((int)s[i], sa);
            const int t1 = (sc * m + 0x7f) / 255, t2 = (d[i] * r9 + 0x7f) / 255;
            d[i] = (uint8_t)(t1 + t2);
        }
        const int t1 = (sa * m + 0x7f) / 255, t2 = (d[3] * r9 + 0x7f) / 255;
        d[3] = (uint8_t)(t1 + t2);
        return;
    }
    if (a == 0x4000) {
        for (int i = 0; i < 4; ++i) d[i] = s[i];
        return;
    }
    const int a8 = (short)((a * 255 + ((a * 255 >> 31) & 0x3fff)) >> 14);
    for (int i = 0; i < 4; ++i) {
        const int df = (int)s[i] - (int)d[i];
        int p = df * a8;
        p += (p < 0) ? 255 : 0;
        d[i] = (uint8_t)(d[i] + (p >> 8));
    }
}

inline void bilinear(uint8_t* out, const uint8_t* p00, const uint8_t* p10, const uint8_t* p01, const uint8_t* p11, int fx, int fy)
{
    const int w00 = div14((0x4000 - fx) * (0x4000 - fy)), w10 = div14((0x4000 - fy) * fx), w01 = div14((0x4000 - fx) * fy), w11 = div14(fy * fx);
    for (int i = 0; i < 4; ++i) out[i] = (uint8_t)div14(p11[i] * w11 + p01[i] * w01 + p10[i] * w10 + p00[i] * w00);
}

// Stretches the rectangle SR of the source picture (4 bytes a pixel) into the rectangle DR of the destination, both in 14-bit fixed point pixels.
// 'rec' (with the source's number 'srcId'): the stretch is not drawn but put in the list, as an Op.
inline bool stretch(Mix mode, uint8_t* dst, int dW, int dH, int dpitch, const uint8_t* src, int sW, int sH, int spitch, IRect DR, IRect SR, float alpha,
                    std::vector<Op>* rec = nullptr, int srcId = 0)
{
    Plan c;
    if (!planStretch(c, dW, dH, sW, sH, DR, SR)) return false;
    const IRect& D = c.D;
    const IRect& S = c.S;
    const int px0 = D.x0 >> 14, px1 = ((D.x1 - 1) >> 14) + 1, py0 = D.y0 >> 14, py1 = ((D.y1 - 1) >> 14) + 1;
    const int sw1 = ((S.x1 - 1) >> 14) + 1, sh1 = ((S.y1 - 1) >> 14) + 1;
    const int rows = py1 - py0, cols = px1 - px0;
    const int firstRowCov = rows >= 2 ? ((py0 + 1) << 14) - D.y0 : D.y1 - D.y0;
    const int lastRowCov = D.y1 - (py1 << 14) + 0x4000;
    const int firstColCov = cols >= 2 ? ((px0 + 1) << 14) - D.x0 : D.x1 - D.x0;
    const int lastColCov = D.x1 - (px1 << 14) + 0x4000;
    const int alpha14 = (int)(alpha * 16384.0f);
    const double C = 1.0 / 16384.0;
    if (rec) {
        Op o;
        std::memset(&o, 0, sizeof(o));
        o.mix = mode == Mix::Over ? 1 : 0;
        o.src = srcId;
        o.px0 = px0; o.px1 = px1; o.py0 = py0; o.py1 = py1;
        o.firstRowCov = firstRowCov; o.lastRowCov = lastRowCov; o.firstColCov = firstColCov; o.lastColCov = lastColCov;
        o.sx0 = S.x0; o.sy0 = S.y0; o.xstep = c.xstep; o.ystep = c.ystep;
        o.step1 = (int)((double)c.xstep * (double)firstColCov * C);
        o.syStart = S.y0 + (int)((double)c.ystep * (double)firstRowCov * C);
        o.sw1 = sw1; o.sh1 = sh1;
        o.alpha14 = alpha14;
        if (srcId == 2) std::memcpy(o.cpx, src, 4);
        rec->push_back(o);
        return true;
    }
    auto doRow = [&](int row, int syf, bool lastRow, int rowCov) {
        int iy = syf >> 14;
        const int fy = lastRow ? 0 : (syf & 0x3fff);
        if (lastRow) iy = sh1 - 1;
        const uint8_t* r0 = src + (size_t)iy * spitch;
        const uint8_t* r1 = (iy == sh1 - 1) ? r0 : r0 + spitch;
        uint8_t* d = dst + (size_t)row * dpitch + (size_t)px0 * 4;
        int sxf = S.x0;
        for (int x = px0; x < px1; ++x, d += 4) {
            int ix = sxf >> 14;
            const int fx = sxf & 0x3fff;
            int flag = (ix != sw1 - 1);
            int a = alpha14;
            if (rowCov >= 0) a = (int)((double)a * (double)rowCov * C);
            if (x == px0) a = (int)((double)a * (double)firstColCov * C);
            else if (x == px1 - 1) {
                a = (int)((double)a * (double)lastColCov * C);
                ix = sw1 - 1;
                flag = 0;
            }
            uint8_t o[4];
            bilinear(o, r0 + ix * 4, r0 + (ix + flag) * 4, r1 + ix * 4, r1 + (ix + flag) * 4, fx, fy);
            mixPixel(mode, d, o, a);
            if (x == px0) sxf += (int)((double)c.xstep * (double)firstColCov * C);
            else sxf += c.xstep;
        }
    };
    doRow(py0, S.y0, false, firstRowCov);
    if (rows >= 2) {
        const int syStart = S.y0 + (int)((double)c.ystep * (double)firstRowCov * C);
        for (int r = py0 + 1; r <= py1 - 2; ++r) doRow(r, syStart + (r - py0 - 1) * c.ystep, false, -1);
        doRow(py1 - 1, 0, true, lastRowCov);
    }
    return true;
}

// ---- the border: three rings of four one-pixel strips stretched (over) around the rectangle ----
inline int ceilPx(int v)
{
    const int a = (((v - 1) | 0x3fff) + 1) + 0x2000;
    return (a + ((a >> 31) & 0x3fff)) >> 14;
}
inline int floorPx(int v)
{
    const int a = (v & ~0x3fff) + 0x2000;
    return (a + ((a >> 31) & 0x3fff)) >> 14;
}
inline int ceilFix(int v) { return ((v - 1) | 0x3fff) + 1; }
inline uint8_t roundEven(float p) { return (uint8_t)(long long)std::nearbyint((double)p + 1.5e-8); }

inline void strips(uint8_t* dst, int W, int H, int pitch, IRect A, IRect B, const uint8_t px[4], bool shift, std::vector<Op>* rec = nullptr)
{
    if (shift) {
        A = IRect{A.x0 << 14, A.y0 << 14, A.x1 << 14, A.y1 << 14};
        B = IRect{B.x0 << 14, B.y0 << 14, B.x1 << 14, B.y1 << 14};
    }
    const IRect s[4] = {{B.x0, B.y0, B.x1, A.y0}, {B.x0, A.y1, B.x1, B.y1}, {B.x0, A.y0, A.x0, A.y1}, {A.x1, A.y0, B.x1, A.y1}};
    for (int i = 0; i < 4; ++i) stretch(Mix::Over, dst, W, H, pitch, px, 1, 1, 0, s[i], IRect{0, 0, 1 << 14, 1 << 14}, 1.0f, rec, 2);
}

// r: the rectangle (14-bit fixed point), bx / by: the border's width / height in pixels, col: the colour and the alpha
inline void border(uint8_t* dst, int W, int H, int pitch, IRect r, float bx, float by, const float col[4], std::vector<Op>* rec = nullptr)
{
    const int ix = (int)(bx * 16384.0f), iy = (int)(by * 16384.0f);
    const IRect O = {r.x0 - ix, r.y0 - iy, r.x1 + ix, r.y1 + iy};
    uint8_t px[4];
    px[0] = roundEven((col[0] * col[3]) * 255.0f);
    px[1] = roundEven((col[1] * col[3]) * 255.0f);
    px[2] = roundEven((col[2] * col[3]) * 255.0f);
    px[3] = roundEven(col[3] * 255.0f);
    const IRect R1 = {ceilPx(O.x0), ceilPx(O.y0), floorPx(O.x1), floorPx(O.y1)};
    const IRect R2 = {floorPx(r.x0), floorPx(r.y0), ceilPx(r.x1), ceilPx(r.y1)};
    strips(dst, W, H, pitch, R2, R1, px, true, rec);
    const IRect A2 = {ceilFix(O.x0), ceilFix(O.y0), O.x1 & ~0x3fff, O.y1 & ~0x3fff};
    strips(dst, W, H, pitch, A2, O, px, false, rec);
    const IRect B3 = {r.x0 & ~0x3fff, r.y0 & ~0x3fff, ceilFix(r.x1), ceilFix(r.y1)};
    strips(dst, W, H, pitch, r, B3, px, false, rec);
}

// The whole picture. outgoing / incoming / out: W * H * 4 bytes, top row first (the same order for all three; the border colour is in that order too).
// With 'rec' nothing is drawn (the three pictures may be null): the stretches are put in the list in drawing order.
inline void render(const Props& p, double progress, double aspect, const uint8_t* outgoing, const uint8_t* incoming, int W, int H, uint8_t* out, std::vector<Op>* rec = nullptr)
{
    if (!rec) std::fill(out, out + (size_t)W * H * 4, (uint8_t)0);
    if (p.squares < 0 || W < 1 || H < 1) {
        if (!rec) std::copy(outgoing, outgoing + (size_t)W * H * 4, out);
        return;
    }
    const std::vector<Item> items = layout(p, progress, aspect);
    const float by = (float)((double)std::max(W, H) * p.borderSize * 0.05);
    const float bx = (float)((double)by / aspect);
    auto fix = [](float v, int D) { return (int)(((float)D * v) * 16384.0f); };
    for (const Item& it : items) {
        if (!(it.alpha > 0)) continue;
        const IRect R = {fix(it.r[0], W), fix(it.r[1], H), fix(it.r[2], W), fix(it.r[3], H)};
        const IRect R2 = {fix(it.r2[0], W), fix(it.r2[1], H), fix(it.r2[2], W), fix(it.r2[3], H)};
        if (!it.noBorder && bx != 0 && by != 0) {
            const float col[4] = {p.borderColor[0], p.borderColor[1], p.borderColor[2], it.alpha};
            border(out, W, H, W * 4, R, bx, by, col, rec);
        }
        stretch(Mix::Lerp, out, W, H, W * 4, it.incoming ? incoming : outgoing, W, H, W * 4, R, R2, it.alpha, rec, it.incoming ? 1 : 0);
    }
}

}
}
}
