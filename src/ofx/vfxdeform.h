// VEGAS's Deform (the effect object CDeform in sftrans1.dll), re-implemented from the arithmetic of its functions (read from the user's installed copy; checked against
// those functions bit for bit, see the commit message). Everything is in doubles and positions are pixel INDICES (no half pixel), as the original has it.
//
// The ten settings it takes, in the order of its own list (P[i]): 0 ShearHorizontal, 1 ShearVertical, 2 SqueezeTop, 3 SqueezeBottom, 4 SqueezeLeft, 5 SqueezeRight, 6 CurveTop,
// 7 CurveBottom, 8 CurveLeft, 9 CurveRight, all scaled by Amount; "top" and "left" are rows / columns 0 of the picture handed to the function.
//
// The picture is moved in five steps (map(), in this order): curve the rows (the top row and the bottom row each move along y by a parabola over the width, the rows between
// are spread between the two), curve the columns the same way, scale y about the middle by a factor that changes linearly across x (left and right squeeze), scale x about
// the middle by a factor that changes linearly down y (top and bottom squeeze), and shear (x += y * a, then y += x * b with the new x). The picture the effect makes is the
// bounding box of eight mapped points (the corners and the middles of the edges) plus a pixel on each side; with Center image it is moved so that the mapped middle of the
// frame lands on the middle of the frame. Every pixel of that box asks unmap() where it comes from; unmap() is the steps undone in reverse order, by arithmetic that is
// not the exact inverse (that is how VEGAS does it, and so the picture is, too).
//
// The pixel is made from the source at three levels (quality 0.85, 0.6, 0 from the host's render quality): the average of samples taken along the two diagonals of the
// footprint (the quadrilateral through the mid points to the neighbouring output pixels' source points, one sample per pixel step), a bilinear sample, or the nearest pixel.
#pragma once
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace reno {
namespace ofx {
namespace vfdeform {

struct Geom {
    // the frame and the settings
    double amount = 0, quality = 0;
    double ww = 0, hh = 0, w2 = 0, h2 = 0;       // frame width / height and half of them
    double sheX = 0, sheY = 0;                   // shear factors (x += y * sheX; y += x * sheY)
    double sqX0 = 0, sqX1 = 0, sqXk = 0;         // x scale at the top row, at the bottom row, and its slope per row
    double sqY0 = 0, sqY1 = 0, sqYk = 0;         // y scale at the left column, at the right column, and its slope per column
    double cuY0 = 0, cuY1 = 0, cuY0b = 0, cuY1b = 0;   // curve of the top and bottom rows (and the copies the original keeps)
    double cuX0 = 0, cuX1 = 0, cuX0b = 0, cuX1b = 0;
    bool fShear = false, fSqX = false, fSqY = false, fCuY = false, fCuX = false;
    // the result
    double midX = 0, midY = 0;                   // the mapped middle of the frame
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;       // box of the mapped points, then as whole pixels
    double offX = 0, offY = 0;                   // the source position of the box's pixel (0, 0)
    int outW = 0, outH = 0;                      // the box in pixels
    int boxX0 = 0, boxY0 = 0, boxX1 = 0, boxY1 = 0;      // the box in the frame's pixels (after Center image)
};

inline bool nz(double v) { return !(v == 0.0); }      // (a NaN counts as not zero)

// 0x1801e3a70, for the frame (0, 0, W, H). P: see the top.
inline void setup(Geom& g, int W, int H, const double* P, double amount, double quality)
{
    g = Geom();
    g.amount = amount;
    g.quality = quality;
    const double Ww = (double)W, Hh = (double)H, w2 = Ww * 0.5, h2 = Hh * 0.5;
    g.ww = Ww; g.hh = Hh; g.w2 = w2; g.h2 = h2;
    double a13 = Ww * P[0], a14 = Hh * P[1];
    a13 = a13 * amount;
    a14 = a14 * amount;
    g.sheX = a13 / (Hh - 1.0);
    g.sheY = a14 / (Ww - 1.0);
    g.cuY0 = amount * P[6] * h2;
    g.cuY1 = amount * P[7] * h2;
    g.cuX0 = -(amount * P[8] * w2);
    g.cuX1 = -(amount * P[9] * w2);
    const double k1 = (h2 * h2) / (h2 * h2), k2 = (w2 * w2) / (w2 * w2);
    g.cuX0b = k1 * g.cuX0;
    g.cuX1b = k1 * g.cuX1;
    g.cuY1b = k2 * g.cuY1;
    g.cuY0b = k2 * g.cuY0;
    g.sqY0 = (1.0 - amount * P[4]) * h2;
    g.sqY1 = h2 * (1.0 - amount * P[5]);
    g.sqYk = (g.sqY1 - g.sqY0) / Ww;
    g.sqX0 = (1.0 - amount * P[2]) * w2;
    g.sqX1 = w2 * (1.0 - amount * P[3]);
    g.sqXk = (g.sqX1 - g.sqX0) / Hh;
    if (amount != 0.0) {
        g.fShear = nz(a13) || nz(a14);
        g.fSqX = nz(P[2]) || nz(P[3]);
        g.fSqY = nz(P[4]) || nz(P[5]);
        g.fCuY = nz(P[6]) || nz(P[7]);
        g.fCuX = nz(P[8]) || nz(P[9]);
    }
}

// 0x1801e6820: the picture's position (x, y) to where it is drawn.
inline void map(const Geom& g, double& xr, double& yr)
{
    double x = xr, y = yr;
    if (g.fCuY) {
        const double t = (x - g.w2) * (x - g.w2) / (g.w2 * g.w2);
        const double q = y / g.hh;
        const double a = t * g.cuY0 - g.cuY0b;
        double b = t * g.cuY1 - g.cuY1b;
        b = b + g.hh;
        b = b - a;
        y = b * q + a;
    }
    if (g.fCuX) {
        const double t = (y - g.h2) * (y - g.h2) / (g.h2 * g.h2);
        const double q = x / g.ww;
        const double a = t * g.cuX0 - g.cuX0b;
        double b = t * g.cuX1 - g.cuX1b;
        b = b + g.ww;
        b = b - a;
        x = b * q + a;
    }
    if (g.fSqY) {
        const double s = x * g.sqYk + g.sqY0;
        y = s * (y - g.h2) / g.h2 + g.h2;
    }
    if (g.fSqX) {
        const double s = y * g.sqXk + g.sqX0;
        x = s * (x - g.w2) / g.w2 + g.w2;
    }
    if (g.fShear) {
        x = x + y * g.sheX;
        y = y + x * g.sheY;
    }
    xr = x; yr = y;
}

// 0x1801e69b0: where the drawn position (xr, yr) comes from. Not the exact inverse of map(); where it gives up (a factor of 0) it sets only the one value, as the original does.
inline void unmap(const Geom& g, double& xr, double& yr)
{
    double x = xr, y = yr;
    if (g.fShear) {
        const double xs = x - y * g.sheX;
        const double ys = y - x * g.sheY;
        x = xs; y = ys;
    }
    if (g.fSqX) {
        const double s = y * g.sqXk + g.sqX0;
        if (s == 0.0) { xr = -1.0; return; }
        x = (x - g.w2) * g.w2 / s + g.w2;
    }
    if (g.fSqY) {
        const double s = x * g.sqYk + g.sqY0;
        if (s == 0.0) { yr = -1.0; return; }
        y = (y - g.h2) * g.h2 / s + g.h2;
    }
    if (g.fCuX) {
        double t = (y - g.h2) * (y - g.h2) / (g.h2 * g.h2);
        double b = t * g.cuX1 - g.cuX1b, a = t * g.cuX0 - g.cuX0b;
        b = b + g.ww;
        if (a == b) { xr = a > x ? -1.0 : g.ww + 1.0; return; }
        x = (x - a) / (b - a) * g.ww;
    }
    if (g.fCuY) {
        double t = (x - g.w2) * (x - g.w2) / (g.w2 * g.w2);
        double b = t * g.cuY1 - g.cuY1b, a = t * g.cuY0 - g.cuY0b;
        b = b + g.hh;
        if (a == b) { yr = a > y ? -1.0 : g.hh + 1.0; return; }
        y = (y - a) / (b - a) * g.hh;
    }
    xr = x; yr = y;
}

// round to nearest even by the add of 1.5 * 2^52 and the low 32 bits of the sum (what the original does; the result is garbage for |v| >= 2^31 as there)
inline int rnd32(double v)
{
    volatile double t = v + 6755399441055744.0;
    uint64_t b;
    const double tt = t;
    std::memcpy(&b, &tt, 8);
    return (int)(uint32_t)b;
}
inline int cvt32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }

// The box the picture is drawn in: 0x1801e3a70's end (the eight mapped points) and 0x1801e4140 (the pixels, Center image). centre: Center image.
inline void bounds(Geom& g, int W, int H, bool centre)
{
    g.midX = (double)(W + 0) * 0.5;
    g.midY = (double)(H + 0) * 0.5;
    map(g, g.midX, g.midY);
    double mnx = g.midX, mxx = g.midX, mny = g.midY, mxy = g.midY;
    const double X0 = 0, X1 = (double)W, Y0 = 0, Y1 = (double)H, XM = (double)(0 + W) * 0.5, YM = (double)(H + 0) * 0.5;
    const double pts[8][2] = {{X0, Y0}, {X0, Y1}, {X1, Y1}, {X1, Y0}, {X0, YM}, {X1, YM}, {XM, Y0}, {XM, Y1}};
    for (int i = 0; i < 8; ++i) {
        double x = pts[i][0], y = pts[i][1];
        map(g, x, y);
        if (mnx > x) mnx = x; else if (x > mxx) mxx = x;
        if (mny > y) mny = y; else if (y > mxy) mxy = y;
    }
    const double c1 = 0.499999985;
    const int bx0 = rnd32(mnx - c1) - 1, bx1 = rnd32(mxx + c1) + 1, by0 = rnd32(mny - c1) - 1, by1 = rnd32(mxy + c1) + 1;
    g.offX = (double)bx0;
    g.offY = (double)by0;
    long long ox0 = bx0, oy0 = by0, ox1 = bx1, oy1 = by1;
    if (centre) {
        const double sx = (double)(W + 0) * 0.5 - g.midX, sy = (double)(H + 0) * 0.5 - g.midY;
        const int ix = cvt32(sx), iy = cvt32(sy);
        ox0 += ix; ox1 += ix; oy0 += iy; oy1 += iy;
        g.offX = ((double)ix - sx) + g.offX;
        g.offY = ((double)iy - sy) + g.offY;
    }
    // (a box beyond what an int holds can only come from absurd settings: it is made empty)
    const long long lim = 1LL << 30;
    if (ox0 < -lim || ox0 > lim || ox1 < -lim || ox1 > lim || oy0 < -lim || oy0 > lim || oy1 < -lim || oy1 > lim) { ox0 = ox1 = oy0 = oy1 = 0; }
    g.boxX0 = (int)ox0; g.boxY0 = (int)oy0; g.boxX1 = (int)ox1; g.boxY1 = (int)oy1;
    g.outW = (int)std::min<long long>(ox1 - ox0, lim);
    g.outH = (int)std::min<long long>(oy1 - oy0, lim);
}

// ---- the samplers (0x1801e5590 / 0x1801e5980 and the footprint sampler 0x1801e5e10 / 0x1801e6310); C is uint8_t (4 bytes a pixel) or float (4 floats)
template <class C> inline C toChan(double v);
template <> inline uint8_t toChan<uint8_t>(double v) { return (uint8_t)(int)v; }
template <> inline float toChan<float>(double v) { return (float)v; }

template <class C>
struct Src {
    const C* p; int w, h; ptrdiff_t stride;    // stride in C units
    const C* bg;
    const C* at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? bg : p + (ptrdiff_t)y * stride + (ptrdiff_t)x * 4; }
};

// bilinear, the pixel INDEX being the pixel's position; outside (or beyond one pixel beyond) the background
template <class C>
inline void bilinear(const Src<C>& s, double x, double y, C* o)
{
    const int ix = rnd32(x - 0.499999985), iy = rnd32(y - 0.499999985);
    if (-1.0 > (double)ix || ix >= s.w || -1.0 > (double)iy || iy >= s.h) { std::memcpy(o, s.bg, 4 * sizeof(C)); return; }
    const double fx = x - (double)ix, fy = y - (double)iy;
    const double wx0 = 1.0 - fx, wy0 = 1.0 - fy;
    const C* p00 = s.at(ix, iy);
    const C* p10 = s.at(ix + 1, iy);
    const C* p01 = s.at(ix, iy + 1);
    const C* p11 = s.at(ix + 1, iy + 1);
    for (int k = 0; k < 4; ++k) {
        const double a = ((double)p00[k] * wx0 + (double)p10[k] * fx) * wy0;
        const double b = ((double)p01[k] * wx0 + (double)p11[k] * fx) * fy;
        o[k] = toChan<C>(a + b);
    }
}

// the footprint: A..D are the four points of the pixel's footprint (see render()); the samples are taken along A-C one per pixel in x and along B-D one per pixel in y
template <class C>
inline void footprint(const Src<C>& s, const double* A, const double* B, const double* Cc, const double* D, C* o)
{
    const double lx = 5.0 * s.w, ly = 5.0 * s.h;
    const double dx1 = Cc[0] - A[0], dy1 = Cc[1] - A[1], dx2 = D[0] - B[0], dy2 = D[1] - B[1];
    if (std::fabs(dx1) > lx || std::fabs(dy1) > ly || std::fabs(dx2) > lx || std::fabs(dy2) > ly) { std::memcpy(o, s.bg, 4 * sizeof(C)); return; }
    double sum[4] = {0, 0, 0, 0}, cnt = 0;
    const int limN = 5 * (s.w > s.h ? s.w : s.h) + 8;       // (a safeguard against positions so large that adding 1 changes nothing; the original would not end)
    const double W = (double)s.w, H = (double)s.h;
    auto take = [&](double x, double y, bool in) {
        C px[4];
        if (in) bilinear(s, x, y, px); else std::memcpy(px, s.bg, sizeof px);
        for (int k = 0; k < 4; ++k) sum[k] += (double)px[k];
        cnt += 1.0;
    };
    if (Cc[0] != A[0]) {
        const double slope = dy1 / dx1;
        if (!(Cc[0] < A[0])) {
            double x = A[0];
            int guard = 0;
            do {
                bool in = false;
                double y = 0;
                if (!(x < 0.0) && W > x) {
                    y = (x - A[0]) * slope + A[1];
                    in = !(y < 0.0) && H > y;
                }
                take(x, y, in);
                x += 1.0;
            } while (Cc[0] >= x && ++guard <= limN);
        }
    }
    if (B[1] != D[1]) {
        const double slope = dx2 / dy2;
        if (!(D[1] < B[1])) {
            double y = B[1];
            int guard = 0;
            do {
                bool in = false;
                double x = 0;
                if (!(y < 0.0) && H > y) {
                    x = (y - B[1]) * slope + B[0];
                    in = !(x < 0.0) && W > x;
                }
                take(x, y, in);
                y += 1.0;
            } while (D[1] >= y && ++guard <= limN);
        }
    }
    if (cnt == 0.0) { std::memcpy(o, s.bg, 4 * sizeof(C)); return; }
    for (int k = 0; k < 4; ++k) o[k] = toChan<C>(sum[k] / cnt);
}

// 0x1801e4790: the box's pixels. 'out' is outW x outH x 4 channels. src is the frame (sw x sh, the pixels 'stride' channels apart per row), bg the background pixel.
template <class C>
inline void render(const Geom& g, const C* src, int sw, int sh, ptrdiff_t stride, const C* bg, C* out)
{
    const int Wb = g.outW, Hb = g.outH;
    if (Wb <= 0 || Hb <= 0) return;
    const Src<C> s{src, sw, sh, stride, bg};
    const double q = g.quality;
    const bool quad = !(q < 0.85), lin = !(0.6 > q);
    std::vector<double> pts;
    if (quad) pts.resize((size_t)Wb * Hb * 2);
    for (int y = 0; y < Hb; ++y) {
        for (int x = 0; x < Wb; ++x) {
            double px = (double)x + g.offX, py = (double)y + g.offY;
            unmap(g, px, py);
            C* o = out + ((size_t)y * Wb + x) * 4;
            if (quad) { pts[((size_t)y * Wb + x) * 2] = px; pts[((size_t)y * Wb + x) * 2 + 1] = py; continue; }
            if (!(px < 0.0) && (double)sw > px && !(py < 0.0) && (double)sh > py) {
                if (lin) bilinear(s, px, py, o);
                else std::memcpy(o, src + (ptrdiff_t)(int)py * stride + (ptrdiff_t)(int)px * 4, 4 * sizeof(C));
            } else std::memcpy(o, bg, 4 * sizeof(C));
        }
    }
    if (!quad) return;
    for (int y = 0; y < Hb; ++y) {
        for (int x = 0; x < Wb; ++x) {
            const double* P = &pts[((size_t)y * Wb + x) * 2];
            double A[2] = {P[0], P[1]}, B[2] = {P[0], P[1]}, Cc[2] = {P[0], P[1]}, D[2] = {P[0], P[1]};
            if (x != 0) { A[0] = (P[0] + P[-2]) * 0.5; A[1] = (P[1] + P[-1]) * 0.5; }
            if (y != 0) { B[0] = (P[0] + P[-2 * Wb]) * 0.5; B[1] = (P[1] + P[-2 * Wb + 1]) * 0.5; }
            if (x != Wb - 1) { Cc[0] = (P[0] + P[2]) * 0.5; Cc[1] = (P[1] + P[3]) * 0.5; }
            if (y != Hb - 1) { D[0] = (P[0] + P[2 * Wb]) * 0.5; D[1] = (P[1] + P[2 * Wb + 1]) * 0.5; }
            if (A[0] > Cc[0]) std::swap(A[0], Cc[0]);
            if (B[1] > D[1]) std::swap(B[1], D[1]);
            if (A[1] > Cc[1]) std::swap(A[1], Cc[1]);
            if (B[0] > D[0]) std::swap(B[0], D[0]);
            C* o = out + ((size_t)y * Wb + x) * 4;
            bool quadPath = false;
            if (g.amount != 0.0) {
                const double t1 = (Cc[1] - A[1]) * (Cc[0] - A[0]);
                if (!(t1 == 0.0)) quadPath = true;
                else {
                    const double t2 = (D[1] - B[1]) * (D[0] - B[0]);
                    if (!(t2 == 0.0)) quadPath = true;
                }
            }
            if (quadPath) { footprint(s, A, B, Cc, D, o); continue; }
            if (!(P[0] < 0.0) && !((double)sw < P[0]) && !(P[1] < 0.0) && !((double)sh < P[1])) bilinear(s, P[0], P[1], o);
            else std::memcpy(o, bg, 4 * sizeof(C));
        }
    }
}

// The whole effect on a picture: sets up, finds the box, draws it and puts it in the frame (outside the box: the background). dst: W x H, 'dstStride' channels per row.
// False when the box is too big to make (the frame is then left to the caller).
template <class C>
inline bool run(const C* src, int W, int H, ptrdiff_t srcStride, const double* P, double amount, double quality, bool centre, const C* bg, C* dst, ptrdiff_t dstStride)
{
    Geom g;
    setup(g, W, H, P, amount, quality);
    bounds(g, W, H, centre);
    if (g.outW < 0 || g.outH < 0 || (double)g.outW * (double)g.outH > 67108864.0 || (double)g.outW * (double)g.outH > 8.0 * W * H + 4096.0) return false;
    std::vector<C> box((size_t)g.outW * g.outH * 4);
    render<C>(g, src, W, H, srcStride, bg, box.data());
    for (int y = 0; y < H; ++y) {
        C* d = dst + (ptrdiff_t)y * dstStride;
        for (int x = 0; x < W; ++x, d += 4) {
            if (x >= g.boxX0 && x < g.boxX1 && y >= g.boxY0 && y < g.boxY1) std::memcpy(d, &box[((size_t)(y - g.boxY0) * g.outW + (size_t)(x - g.boxX0)) * 4], 4 * sizeof(C));
            else std::memcpy(d, bg, 4 * sizeof(C));
        }
    }
    return true;
}

}  // namespace vfdeform
}  // namespace ofx
}  // namespace reno
