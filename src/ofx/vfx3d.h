// The polygon rasteriser of VEGAS's 3D transitions (3D Shuffle, 3D Blinds, 3D Cascade, 3D Fly In/Out): a re-implementation of what their draw function does for an
// 8-bit picture (vfx1.dll, read from the user's own VEGAS install and checked pixel for pixel against that function on thousands of random polygons and whole effects),
// not a fit. No dependencies but the standard library, so a test program can include it alone.
//
// What it draws: one convex polygon (up to 36 corners {x, y, z, w, u, v, e}, already in screen pixels with the perspective w kept) textured with a picture, over an 8-bit
// RGBA surface, with a coverage-weighted edge and VEGAS's two lights. The steps, in the order the code runs them:
//  - set up: 1/w, u/w and v/w per corner and the screen-space gradients of the three (from corners 0, 1 and 2);
//  - the edge table (a corner pair is dropped when it is flatter than 1/64 of a row), then for every picture row the edges that cross it, put in order by their x at the
//    row's top; when every edge runs the whole row each pair of edges is one strip, else the row is cut at every corner's y (float) and each piece is a strip;
//  - the row is swept one pixel column at a time. Every strip that reaches the column is a fragment: the strip's left and right edge are cut to the pixel's square
//    (14-bit fixed point, the cut points in doubles), the part of the pixel on the strip's inner side is marked in a 32 x 32 mask (each cut line toggles its right half-plane),
//    the marks of earlier fragments of the column are taken out, and the fragment counts as many 1/1024ths of the pixel as it has marks left; a fragment that fills the pixel
//    replaces what the column has so far;
//  - the colour of a fragment is the average of the picture over the footprint of the pixel (a summed-area table of the picture, one float per channel, read between its four
//    corners; the footprint is the step to the next pixel in u and v), the fragment's contribution coverage x colour / 1024;
//  - the pixel: the coverage-weighted colours of the column are lit (the directional light |n.(0,0,1)| and the point light of the effect, Phong with a specular power of 8 and a
//    distance falloff), then laid over what is there by the coverage (and the fade of 3D Fly In/Out).
// Single and double precision exactly as the code does it (this file wants -ffp-contract=off, which Reno's build has): the rounding tricks (adding 1.5 x 2^52 and taking the
// low half) are kept as they are. VEGAS does not stop a strip that starts left of the picture at the edge, it writes the columns before it (the one before the first lands on
// the last pixel of the row above); so does this, except that nothing is ever written outside the surface.
#pragma once
#include "../core/rmath.h"
#include <cmath>
#include <cstdint>
#include <climits>
#include <cstddef>
#include <cstring>
#include <vector>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace reno {
namespace ofx {
namespace vfx3d {

typedef int32_t i32;
typedef uint32_t u32;

// ---------------------------------------------------------------- numbers as the machine has them
// cvttsd2si / cvttss2si: the whole part, the "indefinite" integer for NaN and what does not fit
inline i32 cvtt(double x) { return (x > -2147483649.0 && x < 2147483648.0) ? (i32)x : INT_MIN; }
inline i32 cvttf(float x) { return (x > -2147483904.0f && x < 2147483648.0f) ? (i32)x : INT_MIN; }
// "x + 1.5 x 2^52": round to nearest even, then the low half of the double's bits
inline i32 low32(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return (i32)(uint32_t)b;
}
inline i32 rintMagic(double x)
{
    volatile double t = x + 4503601774854144.0;     // 2^52 + 2^31
    return (i32)((u32)low32(t) + 0x80000000u);
}
inline i32 edgeLow(double y)
{
    volatile double t = y - 0.499999985;
    volatile double u = t + 6755399441055744.0;
    return low32(u);
}
inline i32 edgeHigh(double y)
{
    volatile double t = y + 0.499999985;
    volatile double u = t + 6755399441055744.0;
    return low32(u);
}
// x / 2^s towards zero, the way the code does it (add the mask for a negative x, shift)
inline i32 divPow2(i32 v, int s) { return (i32)(((int64_t)v + ((v >> 31) & ((1 << s) - 1))) >> s); }
inline i32 wrapAdd(i32 a, i32 b) { return (i32)((u32)a + (u32)b); }
inline i32 wrapMul(i32 a, i32 b) { return (i32)((u32)a * (u32)b); }
inline int popCount32(u32 v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0f0f0f0fu;
    return (int)((v * 0x01010101u) >> 24);
}

// ---------------------------------------------------------------- data
// A polygon: corners {x, y, z, w, u, v, e} (u, v the picture position 0..1, e 1), the number of corners at the end (0x3f8 bytes in VEGAS)
struct Poly {
    float v[36][7];
    i32 pad;
    i32 n;
};

// The point light and the fixed directional light (0, 0, 1) of a draw. 'intensity' scales the point light; mode 1 uses the directional light alone, mode 2 both.
struct Lights {
    float pos[3];
    float colour[3];
    float intensity;
};

namespace detail {

struct Edge {
    float a[7], b[7];
    i32 ymin, ymax, dxdy, x0;
    float uwSl, uwSt, vwSl, vwSt, iwSt, iwSl;
};
struct Slot {
    Edge* e;
    i32 flag;
};
// A strip of one picture row: the corners (14-bit fixed point x, y) top left, bottom left, top right, bottom right, then 1/w, u/w, v/w at the top left and their steps
struct Span {
    i32 xy[8];                  // TL.x TL.y BL.x BL.y TR.x TR.y BR.x BR.y
    float uwT, uwTdy, vwT, vwTdy, iwT, iwTdy;       // at the top left (value, gradient along y)
    float uwB, vwB, iwB;        // at the bottom left
    float iwDx, uwDx, vwDx;     // along x
    i32 minx;
};
// One strip while the row is swept: its cut pixel, the running 1/w, u/w, v/w and the mask
struct Frag {
    Span* sp;
    i32 colStart, colEnd;       // the column in 14-bit fixed point
    i32 minL, maxL, minR, maxR;
    i32 rowBottom;
    i32 segCount;               // -1: the pixel is filled
    i32 segA[4][2][2];          // [segment][A or B][x or y]: the cut lines, relative to the pixel (up to two from each strip edge)
    float uw, vw, iw;           // running values
    float uw0, vw0, iw0;        // values at the top left
    float iwSt, uwSt, vwSt;     // steps per pixel
    i32 started, finished;
    i32 mask[32];
    Frag* next;
};

struct Setup {
    float iw[36], uw[36], vw[36];
    float g[6];                 // d(iw, uw, vw) / d(x, y)
};

inline void setup(Setup& S, const Poly& P, int n)
{
    const float(*v)[7] = P.v;
    float det = (v[1][0] - v[2][0]) * (v[0][1] - v[2][1]);
    const float t = (v[0][0] - v[2][0]) * (v[1][1] - v[2][1]);
    det = det - t;
    float id = 1.0f;
    if (!(1e-7 > std::fabs((double)det))) id = 1.0f / det;
    const float nid = -id;
    for (int i = 0; i < n && i < 36; ++i) {
        const float iw = 1.0f / v[i][3];
        S.iw[i] = iw;
        S.uw[i] = iw * v[i][4];
        S.vw[i] = iw * v[i][5];
    }
    if (n >= 3) {
        const float y12 = v[1][1] - v[2][1], y02 = v[0][1] - v[2][1], x02 = v[0][0] - v[2][0], x12 = v[1][0] - v[2][0];
        float d1 = S.iw[1] - S.iw[2], d0 = S.iw[0] - S.iw[2];
        S.g[0] = (y02 * d1 - y12 * d0) * id;
        S.g[1] = (x02 * d1 - x12 * d0) * nid;
        d1 = S.uw[1] - S.uw[2];
        d0 = S.uw[0] - S.uw[2];
        S.g[2] = (y02 * d1 - y12 * d0) * id;
        S.g[3] = (x02 * d1 - x12 * d0) * nid;
        d1 = S.vw[1] - S.vw[2];
        d0 = S.vw[0] - S.vw[2];
        S.g[4] = (y02 * d1 - y12 * d0) * id;
        S.g[5] = (x02 * d1 - x12 * d0) * nid;
    } else {
        for (int k = 0; k < 6; ++k) S.g[k] = 0;
    }
}

// the edge table: every corner pair taller than 1/64 row, oriented downwards
inline int buildEdges(Edge* E, int maxE, const Setup& S, const Poly& P, i32& yminOut, i32& ymaxOut)
{
    const int n = P.n;
    float ymin = P.v[0][1], ymax = P.v[0][1];
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float y = P.v[i][1];
        if (!(y > ymin)) ymin = y;
        if (!(ymax > y)) ymax = y;
        const float dy = P.v[j][1] - y;
        if (!(std::fabs((double)dy) > 0.015625)) continue;
        int A = i, B = j;
        if (dy < 0) { A = j; B = i; }
        const float* a = P.v[A];
        const float* b = P.v[B];
        Edge& R = E[cnt];
        const float dyf = b[1] - a[1];
        const double dyd = (double)dyf;
        R.ymin = edgeLow((double)a[1]);
        R.ymax = edgeHigh((double)b[1]);
        std::memcpy(R.a, a, 28);
        std::memcpy(R.b, b, 28);
        const float dxf = b[0] - a[0];
        R.dxdy = cvtt(((double)dxf * 16384.0) / dyd);
        R.iwSt = S.iw[A];
        R.uwSt = S.uw[A];
        R.vwSt = S.vw[A];
        R.iwSl = (float)((double)(S.iw[B] - R.iwSt) / dyd);
        R.uwSl = (float)((double)(S.uw[B] - R.uwSt) / dyd);
        R.vwSl = (float)((double)(S.vw[B] - R.vwSt) / dyd);
        R.x0 = cvtt((double)a[0] * 16384.0);
        ++cnt;
        if (cnt >= maxE) break;
    }
    yminOut = edgeLow((double)ymin);
    ymaxOut = edgeHigh((double)ymax);
    return cnt;
}

inline i32 edgeKey(const Edge* e, float fy) { return wrapAdd(cvttf((fy - e->a[1]) * (float)e->dxdy), e->x0); }

// Puts the edges that cross row y in order of their x at the row's top (a selection sort; on equal x the flatter slope first, the slope compared being the one of the first
// candidate only, as in the code), then marks the edges whose end or start lies inside the row. Returns 1 when one does.
inline int sortEdges(Slot* a, int n, int y)
{
    const float fy = (float)y, fy1 = (float)(y + 1);
    for (int k = 0; k < n; ++k) {
        Edge* ek = a[k].e;
        int bi = k;
        i32 bkey = edgeKey(ek, fy);
        i32 bd = ek->dxdy;
        for (int j = k + 1; j < n; ++j) {
            const i32 kk = edgeKey(a[j].e, fy), dd = a[j].e->dxdy;
            if (kk > bkey) continue;
            if (kk < bkey) { bi = j; bkey = kk; }
            else if (dd < bd) { bi = j; bd = dd; }
        }
        Edge* t = a[bi].e;
        a[bi].e = ek;
        a[k].e = t;
    }
    int any = 0;
    for (int i = 0; i < n; ++i) {
        const Edge* e = a[i].e;
        const float ya = e->a[1], yb = e->b[1];
        if (ya > fy && fy1 > ya) a[i].flag = 1;
        else if (yb > fy && fy1 > yb) a[i].flag = 2;
        else a[i].flag = 0;
        if (a[i].flag) any = 1;
    }
    return any;
}

// the row's cut heights in order, equal ones once
inline int sortUnique(float* a, int n)
{
    for (int i = 0; i < n; ++i) {
        int bi = i;
        float m = a[i];
        for (int j = i; j < n; ++j)
            if (m > a[j]) { m = a[j]; bi = j; }
        const float t = a[bi];
        a[bi] = a[i];
        a[i] = t;
    }
    int c = 1;
    if (n <= 1) return 1;
    for (int i = 1; i < n; ++i)
        if (a[i] != a[i - 1]) a[c++] = a[i];
    return c;
}

// The edges that cross the float row y, in order. The code compares the candidate's first x with the best key so far when it decides between "better" and "tie".
inline int edgesAt(Slot* out, Slot* a, int n, float fy)
{
    for (int k = 0; k < n; ++k) {
        Edge* ek = a[k].e;
        int bi = k;
        i32 bkey = edgeKey(ek, fy);
        i32 bd = ek->dxdy;
        for (int j = k + 1; j < n; ++j) {
            Edge* ej = a[j].e;
            const i32 kk = edgeKey(ej, fy), dd = ej->dxdy;
            if (kk > bkey) continue;
            const i32 old = bkey;
            bkey = kk;
            if (ej->x0 != old) bi = j;
            else if (dd < bd) { bi = j; bd = dd; }
            else bkey = old;
        }
        Edge* t = a[bi].e;
        a[bi].e = ek;
        a[k].e = t;
    }
    int c = 0;
    for (int i = 0; i < n; ++i) {
        const Edge* e = a[i].e;
        if (e->b[1] > fy && !(fy < e->a[1])) out[c++] = a[i];
    }
    return c;
}

inline void fillSpan(Span& s, const Edge* L, const Edge* R, float ytop, float ybot, const Setup& S)
{
    const double k = 0.00006103515625;
    const float t0 = ytop - L->a[1], t1 = ybot - L->a[1];
    s.xy[1] = cvtt((double)ytop * 16384.0);
    s.xy[5] = s.xy[1];
    s.xy[0] = cvtt((((double)L->dxdy * k) * (double)t0 + (double)L->a[0]) * 16384.0);
    s.xy[4] = cvtt((((double)R->dxdy * k) * (double)(ytop - R->a[1]) + (double)R->a[0]) * 16384.0);
    s.xy[3] = cvtt((double)ybot * 16384.0);
    s.xy[7] = s.xy[3];
    s.xy[2] = cvtt((((double)L->dxdy * k) * (double)t1 + (double)L->a[0]) * 16384.0);
    s.xy[6] = cvtt((((double)R->dxdy * k) * (double)(ybot - R->a[1]) + (double)R->a[0]) * 16384.0);
    s.minx = s.xy[0] < s.xy[2] ? s.xy[0] : s.xy[2];
    s.iwT = t0 * L->iwSl + L->iwSt;
    s.uwT = t0 * L->uwSl + L->uwSt;
    s.vwT = t0 * L->vwSl + L->vwSt;
    s.iwB = t1 * L->iwSl + L->iwSt;
    s.uwB = t1 * L->uwSl + L->uwSt;
    s.vwB = t1 * L->vwSl + L->vwSt;
    s.iwDx = S.g[1];
    s.uwDx = S.g[3];
    s.vwDx = S.g[5];
    s.iwTdy = S.g[0];
    s.uwTdy = S.g[2];
    s.vwTdy = S.g[4];
}

// a row with an edge that starts or ends inside it: cut at every such height and make the strips of the pieces
inline int buildSpans(Span* out, int maxSp, Slot* act, int nAct, int y, const Setup& S)
{
    float ys[96];
    int c = 0;
    for (int i = 0; i < nAct; ++i) {
        if (act[i].flag & 1) ys[c++] = act[i].e->a[1];
        if (act[i].flag & 2) ys[c++] = act[i].e->b[1];
    }
    ys[c] = (float)y;
    ys[c + 1] = (float)(y + 1);
    c += 2;
    const int m = sortUnique(ys, c);
    int cnt = 0;
    Slot list[40];
    for (int k = 0; k < m - 1 && cnt < maxSp; ++k) {
        const int nl = edgesAt(list, act, nAct, ys[k]);
        for (int i = 0; i + 1 < nl; i += 2) {
            fillSpan(out[cnt], list[i].e, list[i + 1].e, ys[k], ys[k + 1], S);
            ++cnt;
            if (cnt >= maxSp) return cnt;
        }
    }
    return cnt;
}

// ---------------------------------------------------------------- the strip against one pixel column
// The strip's left edge (TL - BL) and right edge (TR - BR) cut to the pixel [colStart, colEnd) x the row: the pieces that matter, relative to the pixel's bottom left in 14-bit
// fixed point, as up to two line segments (A to B) for the mask. segCount is the number of segments.
inline void clipPixel(const Span& sp, Frag& F)
{
    const i32 colS = F.colStart, colE = F.colEnd, bot = F.rowBottom;
    int n = 0;
    auto rel = [&](i32 y) { return y - bot + 0x4000; };
    auto setSeg = [&](int k, i32 ax, i32 ay, i32 bx, i32 by) {
        F.segA[k][0][0] = ax;
        F.segA[k][0][1] = ay;
        F.segA[k][1][0] = bx;
        F.segA[k][1][1] = by;
    };
    {
        // the left edge, p1 the lower x (a tie: the bottom point)
        const i32 xa = sp.xy[0], ya = sp.xy[1], xb = sp.xy[2], yb = sp.xy[3];
        i32 x1, y1, x2, y2;
        if (xa >= xb) { x1 = xb; y1 = yb; x2 = xa; y2 = ya; }
        else { x1 = xa; y1 = ya; x2 = xb; y2 = yb; }
        const int c1 = x1 < colS ? 1 : (x1 < colE ? 2 : 4);
        const int c2 = x2 < colS ? 8 : (x2 < colE ? 0x10 : 0x20);
        const int code = c1 | c2;
        const i32 Y0 = rel(y1), Y1 = rel(y2), X0 = x1 - colE + 0x4000, X1 = x2 - colE + 0x4000;
        if ((code & 9) == 9) {
            setSeg(0, 0, Y0, 0, Y1);
            n = 1;
        } else if ((code & 0x11) == 0x11) {
            const double dxA = (double)(colS - x1), dx = (double)(x2 - x1), dy = (double)(y2 - y1);
            const i32 yc = wrapAdd(cvtt(dy * dxA / dx), y1);
            const i32 ycr = rel(yc);
            setSeg(0, 0, ycr, 0, Y0);
            setSeg(1, 0, ycr, X1, Y1);
            n = 2;
        } else if ((code & 0x21) == 0x21) {
            const double dxA = (double)(colS - x1), dx = (double)(x2 - x1), dy = (double)(y2 - y1), dxE = (double)(colE - x1);
            const i32 yc = wrapAdd(cvtt(dxA * dy / dx), y1);
            const i32 ycr = rel(yc);
            setSeg(0, 0, ycr, 0, Y0);
            setSeg(1, 0, ycr, 0x4000, wrapAdd(cvtt(dxE * dy / dx) - bot + 0x4000, y1));
            n = 2;
        } else if ((code & 0x12) == 0x12) {
            setSeg(0, X0, Y0, X1, Y1);
            n = 1;
        } else if ((code & 2) && (code & 0x20)) {
            const double dxE = (double)(colE - x1), dx = (double)(x2 - x1), dy = (double)(y2 - y1);
            setSeg(0, X0, Y0, 0x4000, wrapAdd(cvtt(dxE * dy / dx) - bot + 0x4000, y1));
            n = 1;
        }
    }
    {
        // the right edge, p1 the lower x
        const i32 xa = sp.xy[4], ya = sp.xy[5], xb = sp.xy[6], yb = sp.xy[7];
        i32 x1, y1, x2, y2;
        if (xa < xb) { x1 = xa; y1 = ya; x2 = xb; y2 = yb; }
        else { x1 = xb; y1 = yb; x2 = xa; y2 = ya; }
        int code = x1 < colS ? 1 : (x1 < colE ? 2 : 4);
        if (x2 >= colS) code |= (x2 < colE ? 0x10 : 0x20);
        const i32 X1r = x1 - colE + 0x4000, X2r = x2 - colE + 0x4000, Y1r = rel(y1), Y2r = rel(y2);
        const int k = n;
        if ((code & 4) && (code & 0x20)) {
            // right of the pixel: nothing
        } else if ((code & 0x22) == 0x22) {
            const double dxE = (double)(colE - x1), dy = (double)(y2 - y1), dx = (double)(x2 - x1);
            setSeg(k, X1r, Y1r, 0x4000, wrapAdd(cvtt(dxE * dy / dx) - bot + 0x4000, y1));
            n = k + 1;
        } else if ((code & 0x12) == 0x12) {
            setSeg(k, X1r, Y1r, X2r, Y2r);
            n = k + 1;
        } else if ((code & 0x11) == 0x11) {
            const double dxA = (double)(colS - x1), dy = (double)(y2 - y1), dx = (double)(x2 - x1);
            const i32 yc = wrapAdd(cvtt(dxA * dy / dx), y1);
            const i32 ycr = rel(yc);
            setSeg(k, 0, ycr, X2r, Y2r);
            setSeg(k + 1, 0, ycr, 0, Y1r);
            n = k + 2;
        } else if ((code & 1) && (code & 0x20)) {
            const double dxA = (double)(colS - x1), dy = (double)(y2 - y1), dx = (double)(x2 - x1), dxE = (double)(colE - x1);
            const i32 yc = wrapAdd(cvtt(dxA * dy / dx), y1);
            const i32 ycr = rel(yc);
            setSeg(k, 0, ycr, 0x4000, wrapAdd(cvtt(dxE * dy / dx) - bot + 0x4000, y1));
            setSeg(k + 1, 0, ycr, 0, Y1r);
            n = k + 2;
        }
    }
    F.segCount = n;
}

inline void fragInit(const Span& sp, Frag& F, int y)
{
    F.minL = sp.xy[0] < sp.xy[2] ? sp.xy[0] : sp.xy[2];
    F.maxL = sp.xy[0] < sp.xy[2] ? sp.xy[2] : sp.xy[0];
    F.minR = sp.xy[4] < sp.xy[6] ? sp.xy[4] : sp.xy[6];
    F.maxR = sp.xy[4] < sp.xy[6] ? sp.xy[6] : sp.xy[4];
    F.rowBottom = (y + 1) << 14;
    F.colStart = F.minL;
    F.colEnd = (F.minL + 0x4000) & ~0x3fff;
    F.iw0 = sp.iwT;
    F.uw0 = sp.uwT;
    F.vw0 = sp.vwT;
    F.iw = sp.iwB;
    F.uw = sp.uwB;
    F.vw = sp.vwB;
    F.iwSt = sp.iwTdy;
    F.uwSt = sp.uwTdy;
    F.vwSt = sp.vwTdy;
    F.started = 0;
    F.finished = 0;
    clipPixel(sp, F);
    if (F.colEnd >= F.maxR) F.finished = 1;
}

// the next column: the running 1/w, u/w, v/w move on and the strip is cut to the new pixel
inline void fragAdvance(const Span& sp, Frag& F)
{
    if (F.finished) return;
    const i32 nxt = F.colEnd;
    const i32 tlx = sp.xy[0];
    if (tlx >= nxt) {
        F.iw = F.iwSt + F.iw;
        F.vw = F.vwSt + F.vw;
        F.uw = F.uwSt + F.uw;
    } else {
        if (F.started == 0) {
            F.iw = F.iw0;
            F.uw = F.uw0;
            F.vw = F.vw0;
            F.started = 1;
        }
        if (tlx >= F.colStart) {
            const float d = (float)(nxt - tlx) * 6.103515625e-05f;
            const float a = d * F.iwSt + F.iw;
            const float b = d * F.vwSt + F.vw;
            const float c = d * F.uwSt;
            F.iw = a;
            F.vw = b;
            F.uw = c + F.uw;
        } else {
            F.iw = F.iwSt + F.iw;
            F.vw = F.vwSt + F.vw;
            F.uw = F.uwSt + F.uw;
        }
    }
    F.colStart = nxt;
    F.colEnd = nxt + 0x4000;
    if (nxt > F.maxL && nxt + 0x4000 < F.minR) {
        const i32 ty = sp.xy[1];
        if ((ty & 0x3fff) == 0 && sp.xy[3] - ty == 0x4000) {
            F.segCount = -1;
        } else {
            const i32 rt = ty - F.rowBottom + 0x4000, rb = sp.xy[3] - F.rowBottom + 0x4000;
            F.segA[0][0][0] = 0; F.segA[0][0][1] = rt; F.segA[0][1][0] = 0; F.segA[0][1][1] = rb;
            F.segA[1][0][0] = 0x4000; F.segA[1][0][1] = rt; F.segA[1][1][0] = 0x4000; F.segA[1][1][1] = rb;
            F.segCount = 2;
        }
    } else {
        clipPixel(sp, F);
    }
    if (F.colStart >= F.maxR) F.finished = 1;
}

// The 32 x 32 coverage mask of a fragment: every segment toggles, in each of its sub-rows, the sub-columns from its x to the right.
inline void buildMask(const Frag& F, i32* M)
{
    const int n = F.segCount;
    if (n <= 0) return;
    for (int k = 0; k < n; ++k) {
        i32 c[4] = {F.segA[k][0][0], F.segA[k][0][1], F.segA[k][1][0], F.segA[k][1][1]};
        for (int q = 0; q < 4; ++q) {
            if (c[q] < 0) c[q] = 0;
            else if (c[q] > 0x4000) c[q] = 0x4000;
        }
        i32 xt, xb, yt, yb;
        if (c[3] > c[1]) { yb = c[3]; yt = c[1]; xt = c[0]; xb = c[2]; }
        else { yb = c[1]; yt = c[3]; xt = c[2]; xb = c[0]; }
        const i32 rowB = yb >> 9, rowT = yt >> 9;
        const i32 nr = rowB - rowT;
        if (k == 0)
            for (int r = 0; r < rowT; ++r) M[r] = 0;
        const i32 dxr = (nr - 1) > 0 ? (xb - xt) / (nr - 1) : 0;
        i32 x = xt;
        for (int r = rowT; r < rowB; ++r) {
            const int sh = x >> 9;
            if (sh < 32) {
                const u32 m = 0xffffffffu >> (sh & 31);
                if (k == 0) M[r] = (i32)m;
                else M[r] ^= (i32)m;
            }
            if (r == rowT) x = wrapAdd(x, cvtt((double)((rowT << 9) - yt + 0x200) * (double)dxr * 0.00006103515625));
            else x = wrapAdd(x, dxr);
        }
        if (k == 0)
            for (int r = rowB; r < 32; ++r) M[r] = 0;
    }
}

// ---------------------------------------------------------------- the picture under a pixel
// The summed-area table of an 8-bit RGBA picture: for every pixel the sum of all pixels up to and including it, four floats, rows of w x 16 bytes.
inline void buildSat(float* dst, const uint8_t* src, int w, int h, int srcStride)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = src + (ptrdiff_t)y * srcStride;
        float* d = dst + (size_t)y * w * 4;
        const float* prev = y > 0 ? dst + (size_t)(y - 1) * w * 4 : nullptr;
        float a[4] = {0, 0, 0, 0};
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                a[c] = a[c] + (float)p[x * 4 + c];
                d[x * 4 + c] = prev ? a[c] + prev[x * 4 + c] : a[c];
            }
    }
}

// the table read between its four corners; (x, y) in 14-bit fixed point (the sum up to the pixel before)
inline void satLookup(float out[4], const float* sat, int stride, i32 x, i32 y)
{
    static const float zero[4] = {0, 0, 0, 0};
    const i32 ey = y + ((y >> 31) & 0x3fff);
    const i32 fy = (ey & 0x3fff) - ((y >> 31) & 0x3fff);
    const i32 yi = ey >> 14;
    const i32 ex = x + ((x >> 31) & 0x3fff);
    const i32 fx = (ex & 0x3fff) - ((x >> 31) & 0x3fff);
    const i32 xi = ex >> 14;
    const float fyf = (float)fy * 6.103515625e-05f, fxf = (float)fx * 6.103515625e-05f;
    const int xm = xi - 1, ym = yi - 1;
    const size_t rs = (size_t)stride / 4;       // floats per row
    const float* r9 = sat + (ptrdiff_t)ym * (ptrdiff_t)rs + (ptrdiff_t)xm * 4;
    const float* r10 = sat + (ptrdiff_t)ym * (ptrdiff_t)rs + (ptrdiff_t)xi * 4;
    const float* rax = sat + (ptrdiff_t)yi * (ptrdiff_t)rs + (ptrdiff_t)xm * 4;
    const float* rcx = sat + (ptrdiff_t)yi * (ptrdiff_t)rs + (ptrdiff_t)xi * 4;
    if (ym < 0) { r9 = zero; r10 = zero; }
    if (xm < 0) { r9 = zero; rax = zero; }
    float w6 = 1.0f - fyf, w5 = 1.0f - fxf;
    const float w4 = w5 * w6;
    w5 = w5 * fyf;
    const float w7 = fyf * fxf;
    w6 = w6 * fxf;
    for (int c = 0; c < 4; ++c) {
        float a = w6 * r10[c];
        const float b = w4 * r9[c];
        a = a + b;
        const float d = w5 * rax[c];
        a = a + d;
        const float e = w7 * rcx[c];
        a = a + e;
        out[c] = a;
    }
}

// the average of the picture over the box starting at (X0, Y0) of extent (adx, ady), 14-bit fixed point
inline void boxFilter(i32 out[4], const float* sat, int sw, int sh, int stride, i32 X0, i32 Y0, i32 adx, i32 ady)
{
    i32 dx = adx > 0x4000 ? adx : 0x4000, dy = ady > 0x4000 ? ady : 0x4000;
    if (dx > 0x4000) dx = wrapAdd(wrapMul(dx, 3), -0x8000);
    if (dy > 0x4000) dy = wrapAdd(wrapMul(dy, 3), -0x8000);
    const i32 x1 = wrapAdd(X0, dx), y1 = wrapAdd(Y0, dy);
    const i32 xMax = (sw << 14) - 0x4000, yMax = (sh << 14) - 0x4000;
    const i32 x0c = X0 < 0 ? 0 : (X0 > xMax ? xMax : X0);
    i32 y0c = 0;
    if (!(Y0 < 0)) {
        y0c = Y0;
        if (y0c > yMax) y0c = yMax;
    }
    i32 x1c = x0c + 1;
    if (x1 >= x1c) {
        x1c = x1;
        if (x1 > (sw << 14) - 1) x1c = (sw << 14) - 1;
    }
    i32 y1c = y0c + 1;
    if (y1 >= y1c) {
        y1c = y1;
        if (y1 > (sh << 14) - 1) y1c = (sh << 14) - 1;
    }
    float f11[4], f10[4], f01[4], f00[4];
    satLookup(f11, sat, stride, x1c, y1c);
    satLookup(f10, sat, stride, x1c, y0c);
    satLookup(f01, sat, stride, x0c, y1c);
    satLookup(f00, sat, stride, x0c, y0c);
    float a = (float)(y1c - y0c) * 6.103515625e-05f;
    const float b = (float)(x1c - x0c) * 6.103515625e-05f;
    a = a * b;
    const float inv = 1.0f / a;
    for (int c = 0; c < 4; ++c) {
        float s = f11[c] - f10[c];
        s = s - f01[c];
        s = s + f00[c];
        const float v = s * inv;
        float r;
        if (0.0f <= v) r = (255.0f < v) ? 255.0f : v;
        else r = 0.0f;
        out[c] = cvttf(r);
    }
}

// The colour of a fragment: the footprint of the pixel is the step to the next pixel along the diagonal in (u, v); returns w = 1 / (1/w)
inline float fragColour(i32 out[4], const Span& sp, const Frag& F, const float* sat, int sw, int sh, int stride)
{
    const float invw = F.iw, uw = F.uw, vw = F.vw;
    const float w = 1.0f / invw;
    float iw2 = invw + F.iwSt;
    iw2 = iw2 + sp.iwDx;
    const float w2 = 1.0f / iw2;
    float u2 = uw + F.uwSt;
    u2 = u2 + sp.uwDx;
    u2 = u2 * w2;
    float v2 = vw + F.vwSt;
    v2 = v2 + sp.vwDx;
    v2 = v2 * w2;
    const float u0 = uw * w, v0 = vw * w;
    const i32 X0 = (i32)((u32)low32((double)u0 * 16384.0 * (double)sw + 4503601774854144.0) + 0x80000000u);
    const i32 Y0 = (i32)((u32)low32((double)v0 * 16384.0 * (double)sh + 4503601774854144.0) + 0x80000000u);
    const i32 X1r = low32((double)u2 * 16384.0 * (double)sw + 4503601774854144.0);
    const i32 Y1r = low32((double)v2 * 16384.0 * (double)sh + 4503601774854144.0);
    const i32 dY = (i32)((u32)Y1r - (u32)Y0 - 0x80000000u);
    const i32 dX = (i32)((u32)X1r - (u32)X0 - 0x80000000u);
    const i32 adx = dX < 0 ? wrapMul(dX, -1) : dX, ady = dY < 0 ? wrapMul(dY, -1) : dY;
    boxFilter(out, sat, sw, sh, stride, X0, Y0, adx, ady);
    return w;
}

// ---------------------------------------------------------------- the light
inline float dot3f(const float* a, const float* b)
{
    float r = a[1] * b[1];
    const float q = a[0] * b[0];
    const float z = a[2] * b[2];
    r = r + q;
    r = r + z;
    return r;
}
inline void norm3(float* v)
{
    const float x = v[0], y = v[1], z = v[2];
    float l = x * x;
    float t = y * y;
    l = l + t;
    t = z * z;
    l = l + t;
    const float len = (float)std::sqrt((double)l);
    v[0] = x / len;
    v[1] = y / len;
    v[2] = z / len;
}

// The pixel: light the column's colours, then lay them over the surface by the coverage 'cov' (1024 = all of the pixel) and the fade (fadeFix 16384 = none).
inline void blendPixel(uint8_t* dp, i32* sum, int cov, float x, float y, float s, const float* nrm, const Lights& L, int mode, int fadeFix, double W, double H, double asp)
{
    const int r11 = cov < 1024 ? cov : 1024;
    if (mode != 0) {
        const float s0 = (float)sum[0], s1 = (float)sum[1], s2 = (float)sum[2], s3 = (float)sum[3];
        float a0 = 0, a1 = 0, a2 = 0;
        for (int li = 0; li < mode; ++li) {
            if (li == 0) {
                // the directional light (0, 0, 1)
                static const float dir[3] = {0.0f, 0.0f, 1.0f};
                float d = dot3f(nrm, dir);
                if (0 > d) d = -d;
                const float v0 = s0 * d, v1 = s1 * d, v2 = s2 * d;
                a1 = a1 + v1;
                a2 = a2 + v2;
                a0 = a0 + v0;
            } else {
                const float dy = L.pos[1] - y;
                const float W2 = (float)(W * W), H2 = (float)(H * H);
                float V[3];
                V[0] = (float)(((W * 0.5) - (double)x) * asp);
                V[1] = (float)((H * 0.5) - (double)y);
                V[2] = 0.0f - s;
                float Lv[3];
                {
                    const float t = L.pos[0] - x;
                    Lv[0] = (float)((double)t * asp);
                }
                Lv[1] = dy;
                Lv[2] = L.pos[2] - s;
                float t8 = dy * dy;
                float t3 = Lv[2] * Lv[2];
                float t2 = Lv[0] * Lv[0];
                t8 = t8 / H2;
                t3 = t3 / W2;
                t2 = t2 / W2;
                float d2 = t8 + t2;
                d2 = d2 + t3;
                float diff, spec, att;
                if (9.99999975e-06f > d2) {
                    norm3(V);
                    diff = 0;
                    spec = 0;
                    att = 0;
                } else {
                    norm3(V);
                    norm3(Lv);
                    const float dist = (float)std::sqrt((double)d2);
                    const float dd = dot3f(nrm, Lv);
                    float Rv[3];
                    if (0 > dd) {
                        Rv[0] = -(((nrm[0] + nrm[0]) * dd) - Lv[0]);
                        Rv[1] = -(((nrm[1] + nrm[1]) * dd) - Lv[1]);
                        Rv[2] = -(((nrm[2] + nrm[2]) * dd) - Lv[2]);
                        diff = -dd;
                        const double q = (double)dot3f(V, Rv);
                        spec = (q <= -0.001) ? (float)rm::pow(q, 8.0) : 0.0f;
                    } else {
                        Rv[0] = ((nrm[0] + nrm[0]) * dd) - Lv[0];
                        Rv[1] = ((nrm[1] + nrm[1]) * dd) - Lv[1];
                        Rv[2] = ((nrm[2] + nrm[2]) * dd) - Lv[2];
                        diff = dd;
                        const double q = (double)dot3f(V, Rv);
                        spec = (0.001 <= q) ? (float)rm::pow(q, 8.0) : 0.0f;
                    }
                    float den = dist * 0.0f;
                    den = den + 0.0f;
                    den = den + d2;
                    att = 1.0f;
                    if (!(1.0f > den)) {
                        att = 1.0f / den;
                        if (!(att < 1.0f)) att = 1.0f;
                    }
                }
                const float a = att * L.intensity;
                const float t0 = a * L.colour[0], t1 = a * L.colour[1], t2b = a * L.colour[2];
                const float v0 = ((((s0 * 0.8f) / 255.0f) * diff + spec) * t0) * 255.0f;
                const float v1 = ((((s1 * 0.8f) / 255.0f) * diff + spec) * t1) * 255.0f;
                const float v2 = ((((s2 * 0.8f) / 255.0f) * diff + spec) * t2b) * 255.0f;
                a1 = a1 + v1;
                a2 = a2 + v2;
                a0 = a0 + v0;
            }
        }
        if (!(a0 < s3)) a0 = s3;
        if (!(a1 < s3)) a1 = s3;
        if (!(a2 < s3)) a2 = s3;
        sum[0] = rintMagic((double)a0);
        sum[1] = rintMagic((double)a1);
        sum[2] = rintMagic((double)a2);
    }
    const int FULL = 0x400;
    if (r11 == FULL && fadeFix == 0x4000) {
        for (int c = 0; c < 4; ++c) dp[c] = (uint8_t)sum[c];
        return;
    }
    if (r11 <= 0) return;
    const i32 inv = FULL - r11;
    const i32 dstA = dp[3];
    const i32 Af = (i32)((u32)sum[3] << 10) / r11;
    const i32 Am = Af < 255 ? Af : 255;
    if (fadeFix == 0x4000) {
        i32 e = (Am - dstA) * r11;
        e = (i32)((u32)e << 4);
        const i32 q = divPow2(e, 14);
        const i32 newA = q + dstA;
        for (int c = 0; c < 3; ++c) {
            const i32 v = divPow2((i32)dp[c] * inv, 10) + sum[c];
            dp[c] = (uint8_t)(v < newA ? v : newA);
        }
        dp[3] = (uint8_t)newA;
    } else {
        const i32 e = Am - dstA;
        const i32 G = cvtt((double)(r11 << 4) * (double)fadeFix * -0.00006103515625);
        const i32 t = divPow2(wrapMul(G, e), 14);
        const i32 newA = dstA - t;
        for (int c = 0; c < 3; ++c) {
            const i32 ec = dp[c];
            i32 v = divPow2(ec * inv, 10) - ec + sum[c];
            v = divPow2(wrapMul(v, fadeFix), 14) + ec;
            dp[c] = (uint8_t)(v < newA ? v : newA);
        }
        dp[3] = (uint8_t)newA;
    }
}

// ---------------------------------------------------------------- a picture row
struct DrawCtx {
    const float* sat;
    int sw, sh, sstride;        // the table: picture size, bytes per table row
    const float* nrm;
    const Lights* lights;
    int mode;
    double asp, fade;
    uint8_t* base;              // the surface (nothing is written before it)
    Frag* pool;                 // the fragment records (see Scratch)
};

inline void renderRow(uint8_t* dstRow, int w, int h, int y, Span* sp, int nsp, const DrawCtx& C)
{
    const i32 fadeFix = cvtt(C.fade * 16384.0);
    i32 x = sp[0].minx >> 14;
    for (int i = 1; i < nsp; ++i) {
        const i32 m = sp[i].minx >> 14;
        if (m < x) x = m;
    }
    i32 bnd = (sp[0].minx + 0x4000) & ~0x3fff;
    if (x >= w) return;
    Frag* frags = C.pool;
    int nextSp = 0, nalloc = 0;
    Frag* head = nullptr;
    for (;;) {
        while (nextSp < nsp && sp[nextSp].minx < bnd && nalloc < 40) {
            Frag* A = &frags[nalloc++];
            // (the mask of the record is not cleared: a strip that no edge cuts through the pixel leaves it as it was, as in VEGAS)
            i32 keep[32];
            std::memcpy(keep, A->mask, sizeof keep);
            std::memset(A, 0, sizeof(Frag));
            std::memcpy(A->mask, keep, sizeof keep);
            A->next = nullptr;
            A->sp = &sp[nextSp];
            if (!head) head = A;
            else {
                Frag* t = head;
                while (t->next) t = t->next;
                t->next = A;
            }
            fragInit(sp[nextSp], *A, y);
            ++nextSp;
        }
        i32 sum[4] = {0, 0, 0, 0};
        float acc = 0.0f;
        i32 tot = 0;
        i32 M[32];
        bool minit = false;
        Frag** link = &head;
        Frag* A = head;
        while (A) {
            i32 cov;
            float accx;
            i32 col[4];
            float ret;
            if (A->segCount == -1) {
                ret = fragColour(sum, *A->sp, *A, C.sat, C.sw, C.sh, C.sstride);
                cov = 1024;
                accx = ret * 1024.0f;
            } else {
                if (!minit) {
                    std::memset(M, 0, sizeof M);
                    minit = true;
                }
                i32* mask = A->mask;
                buildMask(*A, mask);
                for (int i = 0; i < 32; ++i) {
                    mask[i] &= ~M[i];
                    M[i] |= mask[i];
                }
                cov = 0;
                for (int i = 0; i < 32; ++i) cov += popCount32((u32)mask[i]);
                ret = fragColour(col, *A->sp, *A, C.sat, C.sw, C.sh, C.sstride);
                for (int c = 0; c < 4; ++c) sum[c] += divPow2(cov * col[c], 10);
                accx = ret * (float)cov;
            }
            acc = accx + acc;
            tot += cov;
            fragAdvance(*A->sp, *A);
            if (A->finished == 0) {
                link = &A->next;
                A = A->next;
            } else {
                A = A->next;
                *link = A;
            }
        }
        if (tot != 0) {
            float s = (float)w * acc;
            s = s * (float)w;
            s = s / (float)tot;
            uint8_t* dp = dstRow + (ptrdiff_t)x * 4;
            if ((dstRow - C.base) + (ptrdiff_t)x * 4 >= 0) blendPixel(dp, sum, tot, (float)x, (float)y, s, C.nrm, *C.lights, C.mode, fadeFix, (double)w, (double)h, C.asp);
        }
        ++x;
        bnd += 0x4000;
        if (x >= w) break;
        if (nextSp == nsp && head == nullptr) break;
    }
}

}  // namespace detail

// The fragment records of the rows being swept. VEGAS keeps them between rows and polygons, and a pixel that no strip edge cuts keeps the mask its record had before; so the
// records live as long as one picture's polygons (make one Scratch for all the polygons of a frame, drawn in order).
struct Scratch {
    detail::Frag frag[40];
    Scratch() { std::memset(this, 0, sizeof *this); }
};

// Draws the polygon over an 8-bit RGBA surface (w x h, rows 'dstStride' bytes apart) textured with the picture whose summed-area table is 'sat' (buildSat, rows sw x 4 floats).
// nrm: the polygon's unit normal; mode 1: directional light only, 2: with the point light; asp: the pixel aspect; fade: 1 = opaque (3D Fly In/Out fades the picture in or out).
// Returns false when the polygon has fewer than two usable edges (nothing drawn).
inline bool drawPolygon(Scratch& scratch, uint8_t* dst, int w, int h, int dstStride, const float* sat, int sw, int sh, const Poly& poly, const float nrm[3], const Lights& lights,
                        int mode, double asp, double fade)
{
    if (!dst || !sat || w < 1 || h < 1 || sw < 1 || sh < 1 || poly.n < 3 || poly.n > 36) return false;
    detail::Setup S;
    std::memset(&S, 0, sizeof S);
    detail::setup(S, poly, poly.n);
    detail::Edge E[40];
    i32 ymin, ymax;
    const int ne = detail::buildEdges(E, 36, S, poly, ymin, ymax);
    if (ne < 2) return false;
    detail::DrawCtx C = {sat, sw, sh, sw * 16, nrm, &lights, mode, asp, fade, dst, scratch.frag};
    const int y0 = ymin < 0 ? 0 : ymin;
    const int y1 = ymax < h ? ymax : h;
    detail::Slot slots[40];
    std::memset(slots, 0, sizeof slots);
    detail::Span spans[40];
    for (int y = y0; y < y1; ++y) {
        int cnt = 0;
        for (int k = 0; k < ne; ++k)
            if (E[k].ymin <= y && E[k].ymax > y) slots[cnt++].e = &E[k];
        if (cnt < 2) continue;
        const int partial = detail::sortEdges(slots, cnt, y);
        uint8_t* row = dst + (ptrdiff_t)y * dstStride;
        if (!partial) {
            // every edge runs the whole row: one strip for each pair of edges
            const float fy = (float)y, fy1 = (float)(y + 1);
            const int np = ((cnt - 1) >> 1) + 1;
            for (int q = 0; q < np; ++q) {
                const detail::Edge* L = slots[2 * q].e;
                const detail::Edge* R = slots[2 * q + 1].e;
                if (!L || !R) continue;
                detail::Span sp;
                std::memset(&sp, 0, sizeof sp);
                detail::fillSpan(sp, L, R, fy, fy1, S);
                detail::renderRow(row, w, h, y, &sp, 1, C);
            }
        } else {
            const int ns = detail::buildSpans(spans, 36, slots, cnt, y, S);
            if (ns > 0) detail::renderRow(row, w, h, y, spans, ns, C);
        }
    }
    return true;
}

}  // namespace vfx3d
}  // namespace ofx
}  // namespace reno
