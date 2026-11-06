// Iris as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on; host 0x1805db720, one kernel for each family of shapes). Every pixel gets its
// distance from the nearest iris centre in the shape's own measure (an ellipse, a rectangle / diamond, a triangle, or the arrowheads), the distance is divided by
// its largest value on the frame's four edges, and the result is compared with the progress: inside shows the incoming picture, outside the outgoing one, with a
// feathered edge and an optional border band between. Repeat X / Y tile the frame into whole-pixel cells that each hold one iris.
//
// Written from the decoded code. Pictures are RGBA floats; x, y given to the pixel functions are the kernel's (y = 0 is the bottom row), see Pic in vfxwipes.h.
#pragma once
#include "vfxwipes.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include "../core/rmath.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct Iris {
    int W = 0, H = 0;
    int shape = 0;              // 0 .. 13 (Circle .. Arrowhead)
    int nx = 1, ny = 1;         // Repeat X / Y, 1 .. 10
    int cellW = 1, cellH = 1;   // whole-pixel cell size
    bool swapPics = false;      // Direction "Iris out": the pictures change places and the progress runs backwards
    float threshold = 0.f;      // the progress as the kernel compares it (1 - progress when swapped)
    float maxDist = 1.f;        // the largest distance on the frame's edges (at least 1)
    float feather = 0.f, border = 0.f;
    float cxScale = 0.5f, cyKeep = 0.5f;    // Center x, and 1 - (1 - Center y): how the kernel places the first iris inside the first cell
    float ratio = 1.f;          // cell height over cell width (the ellipse / rectangle stretch)
    float asp[2] = {1.f, 1.f};  // pixel aspect as a stretch of the two axes
    float col[4] = {0.f, 0.f, 0.f, 1.f};
    float ox = 0.f, oy = 0.f;   // where the first iris is, in pixels from the frame's left / bottom

    // C's float to int conversion for any value: out of range and NaN give the lowest int (what cvttss2si does)
    static int cut(double v)
    {
        if (!(v > -2147483648.0 && v < 2147483648.0)) return INT_MIN;
        return (int)v;
    }
    static int mulSat(int a, int b)     // a product of two ints that never overflows
    {
        const long long p = (long long)a * (long long)b;
        return p > 1073741823LL ? 1073741823 : p < -1073741823LL ? -1073741823 : (int)p;
    }
    static double finite(double v, double lo, double hi, double dflt)
    {
        if (!(v == v)) return dflt;
        return std::fmin(std::fmax(v, lo), hi);
    }

    // ---------------------------------------------------------------------------------------------------------------- the shapes' measures
    // vector from the iris centre in the stretched frame
    inline void rel(int x, int y, float cx, float cy, float& u, float& w) const
    {
        RENO_WIPES_NO_FMA
        u = (float)x - cx;
        w = (float)y - cy;
        u *= asp[0];
        w *= asp[1];
    }
    // distance to the line through two points, scaled: |(c - a) . (b - p)| style product the triangles and arrowheads use
    static float lineProduct(float a1, float a2, float a3, float a4, float u, float w)
    {
        RENO_WIPES_NO_FMA
        const float p0 = a3 - a1, p1 = u - a1, q0 = a2 - w, q1 = a4 - a2;
        return std::fabs(p0 * q0 + p1 * q1);
    }
    // Circle (0), Ellipse across (1: x stretched by the ratio) and Ellipse up (2)
    float ellipse(int x, int y, float cx, float cy) const
    {
        RENO_WIPES_NO_FMA
        float u, w;
        rel(x, y, cx, cy, u, w);
        if (shape == 1) u *= ratio;
        else if (shape == 2) w *= ratio;
        return rm::hypot(u, w);
    }
    // Rectangle (3), Diamond (4), Square (5)
    float boxy(int x, int y, float cx, float cy) const
    {
        RENO_WIPES_NO_FMA
        float u, w;
        rel(x, y, cx, cy, u, w);
        u = std::fabs(u);
        w = std::fabs(w);
        if (shape == 3) return std::fmax(w, ratio * u);
        if (shape == 4) return u + w;
        return std::fmax(u, w);
    }
    // the four triangles (6 .. 9)
    float triangle(int x, int y, float cx, float cy) const
    {
        RENO_WIPES_NO_FMA
        float u, w;
        rel(x, y, cx, cy, u, w);
        float a1, a2, a3, a4;
        if (shape == 6) {
            const float s = 0.57735027f * u;
            if ((u <= 0.0f) & (w > s)) { a1 = 0.0f; a2 = 0.66666667f; a3 = -0.57735027f; a4 = -0.33333333f; }
            else if ((w <= s) & (w < -s)) { a1 = -0.57735027f; a2 = -0.33333333f; a3 = 0.57735027f; a4 = -0.33333333f; }
            else { a1 = 0.57735027f; a2 = -0.33333333f; a3 = 0.0f; a4 = 0.66666667f; }
        } else if (shape == 7) {
            const float s = 1.73205081f * u;
            if ((w > -s) & (w >= 0.0f)) { a1 = 0.66666667f; a2 = 0.0f; a3 = -0.33333333f; a4 = 0.57735027f; }
            else if ((w <= -s) & (w > s)) { a1 = -0.33333333f; a2 = 0.57735027f; a3 = -0.33333333f; a4 = -0.57735027f; }
            else { a1 = -0.33333333f; a2 = -0.57735027f; a3 = 0.66666667f; a4 = 0.0f; }
        } else if (shape == 8) {
            const float s = 0.57735027f * u;
            if ((u >= 0.0f) & (w < s)) { a1 = 0.0f; a2 = -0.66666667f; a3 = 0.57735027f; a4 = 0.33333333f; }
            else if ((u < 0.0f) & (w <= -s)) { a1 = -0.57735027f; a2 = 0.33333333f; a3 = 0.0f; a4 = -0.66666667f; }
            else { a1 = 0.57735027f; a2 = 0.33333333f; a3 = -0.57735027f; a4 = 0.33333333f; }
        } else {
            const float s = 1.73205081f * u;
            if ((w > 0.0f) & (w >= s)) { a1 = 0.33333333f; a2 = 0.57735027f; a3 = -0.66666667f; a4 = 0.0f; }
            else if ((w <= 0.0f) & (w < -s)) { a1 = 0.33333333f; a2 = -0.57735027f; a3 = -0.66666667f; a4 = 0.0f; }
            else { a1 = 0.33333333f; a2 = -0.57735027f; a3 = 0.33333333f; a4 = 0.57735027f; }
        }
        return lineProduct(a1, a2, a3, a4, u, w);
    }
    // the arrowheads: kind 10 .. 13 (Arrowhead right / down / left / up in the order of the Shape list). The centre is not in the middle of the head, so
    // neighbouring cells' irises reach into this one and the lattice below asks for them by kind.
    float arrow(int kind, int x, int y, float cx, float cy) const
    {
        RENO_WIPES_NO_FMA
        float u, w;
        rel(x, y, cx, cy, u, w);
        float a1, a2, a3, a4, k;
        const float longSide = 1.11803399f, shortSide = 0.55901699f;
        if (kind == 10) {
            if ((u >= 0.0f) & (w < -u)) { a1 = 0.0f; a2 = -0.4f; a3 = 0.8f; a4 = -0.8f; k = longSide; }
            else if ((u > 0.0f) & (w >= -u)) { a1 = 0.8f; a2 = -0.8f; a3 = 0.0f; a4 = 0.8f; k = shortSide; }
            else if ((u <= 0.0f) & (w > u)) { a1 = 0.0f; a2 = 0.8f; a3 = -0.8f; a4 = -0.8f; k = shortSide; }
            else { a1 = -0.8f; a2 = -0.8f; a3 = 0.0f; a4 = -0.4f; k = longSide; }
        } else if (kind == 11) {
            if ((w >= 0.0f) & (w > -u)) { a1 = 0.8f; a2 = 0.0f; a3 = -0.8f; a4 = 0.8f; k = shortSide; }
            else if ((w > 0.0f) & (w <= -u)) { a1 = -0.8f; a2 = 0.8f; a3 = -0.4f; a4 = 0.0f; k = longSide; }
            else if ((w <= 0.0f) & (w > u)) { a1 = -0.4f; a2 = 0.0f; a3 = -0.8f; a4 = -0.8f; k = longSide; }
            else { a1 = -0.8f; a2 = -0.8f; a3 = 0.8f; a4 = 0.0f; k = shortSide; }
        } else if (kind == 12) {
            if ((u > 0.0f) & (w >= u)) { a1 = 0.8f; a2 = 0.8f; a3 = 0.0f; a4 = 0.4f; k = longSide; }
            else if ((u <= 0.0f) & (w > -u)) { a1 = 0.0f; a2 = 0.4f; a3 = -0.8f; a4 = 0.8f; k = longSide; }
            else if ((u < 0.0f) & (w <= -u)) { a1 = -0.8f; a2 = 0.8f; a3 = 0.0f; a4 = -0.8f; k = shortSide; }
            else { a1 = 0.0f; a2 = -0.8f; a3 = 0.8f; a4 = 0.8f; k = shortSide; }
        } else {
            if ((w >= 0.0f) & (w < u)) { a1 = 0.4f; a2 = 0.0f; a3 = 0.8f; a4 = 0.8f; k = longSide; }
            else if ((w > 0.0f) & (w >= u)) { a1 = 0.8f; a2 = 0.8f; a3 = -0.8f; a4 = 0.0f; k = shortSide; }
            else if ((w <= 0.0f) & (w < -u)) { a1 = -0.8f; a2 = 0.0f; a3 = 0.8f; a4 = -0.8f; k = shortSide; }
            else { a1 = 0.8f; a2 = -0.8f; a3 = 0.4f; a4 = 0.0f; k = longSide; }
        }
        return lineProduct(a1, a2, a3, a4, u, w) * k;
    }

    // ---------------------------------------------------------------------------------------------------------------- the cells
    // which cell a pixel is in along one axis: -1 before the first iris, else the cell counted from it, kept below the last
    static int cellIndex(int pos, float origin, int size, int count)
    {
        RENO_WIPES_NO_FMA
        if ((float)pos < origin) return -1;
        const int whole = cut((double)((float)pos - origin));
        int i = cut((double)((float)whole * (1.0f / (float)size)));
        return std::min(i, count - 1);
    }

    // shapes 0 .. 8: the nearest of the (up to four) irises around the pixel's cell, one function for the measure
    template <class F>
    float nearestOfCells(int x, int y, F measure) const
    {
        RENO_WIPES_NO_FMA
        const int ix = cellIndex(x, ox, cellW, nx), iy = cellIndex(y, oy, cellH, ny);
        float r;
        if (ix == -1 && iy == -1) {
            r = measure(ox, oy);
        } else if (ix == -1) {
            const int by = mulSat(iy, cellH);
            r = measure(ox, oy + by);
            if (iy + 1 < ny) r = std::fmin(measure(ox, oy + by + cellH), r);
        } else if (iy == -1) {
            const int bx = mulSat(ix, cellW);
            r = measure(ox + bx, oy);
            if (ix + 1 < nx) r = std::fmin(measure(ox + bx + cellW, oy), r);
        } else {
            const int bx = mulSat(ix, cellW), by = mulSat(iy, cellH);
            r = measure(ox + bx, oy + by);
            if ((ix + 1 < nx) & (iy + 1 < ny)) r = std::fmin(measure(ox + bx + cellW, oy + by + cellH), r);
            if (ix + 1 < nx) r = std::fmin(measure(ox + bx + cellW, oy + by), r);
            if (iy + 1 < ny) r = std::fmin(measure(ox + bx, oy + by + cellH), r);
        }
        return r;
    }

    // shapes 10 .. 13: the same three cells, plus the irises of further cells that an arrowhead's long tail can still reach (which ones depends on the shape, and
    // on whether the pixel lies before the first iris on an axis)
    float arrowCells(int x, int y) const
    {
        RENO_WIPES_NO_FMA
        const int ix = cellIndex(x, ox, cellW, nx), iy = cellIndex(y, oy, cellH, ny);
        const float X0 = ox, Y0 = oy;
        const int cw = cellW, ch = cellH;
        float r = 1.0f;
        auto at = [&](int kind, float cx, float cy) { return arrow(kind, x, y, cx, cy); };
        auto take = [&](int kind, float cx, float cy) { r = std::fmin(at(kind, cx, cy), r); };
        const int s = shape;
        if (ix == -1 && iy == -1) {
            r = at(s, X0, Y0);
            if (s == 10 || s == 11) {
                if ((1 < nx) & (1 < ny)) take(s, X0 + cw, Y0 + ch);
                if (1 < nx) take(s, X0 + cw, Y0);
                if (1 < ny) take(s, X0, Y0 + ch);
            }
        } else if (ix == -1) {
            const int by = mulSat(iy, ch);
            r = at(s, X0, Y0 + by);
            if (iy + 1 < ny) take(s, X0, Y0 + by + ch);
            if (s == 11) {
                if (1 < nx) {
                    take(11, X0 + cw, Y0 + by);
                    if (0 < iy) take(11, X0 + cw, Y0 + by - ch);
                    if (iy + 1 < ny) take(11, X0 + cw, Y0 + by + ch);
                    if (iy + 2 < ny) take(11, X0 + cw, Y0 + by + ch + ch);
                }
                if (0 < iy) take(11, X0, Y0 + by - ch);
                if (iy + 2 < ny) take(11, X0 + cw, Y0 + by + ch + ch);
            } else if (s == 10) {
                if ((1 < nx) & (iy + 1 < ny)) take(10, X0 + cw, Y0 + by + ch);
                if ((1 < nx) & (iy + 2 < ny)) take(10, X0 + cw, Y0 + by + ch + ch);
                if (iy + 2 < ny) take(10, X0, Y0 + by + ch + ch);
            } else if (s == 12) {
                if ((1 < nx) & (0 < iy)) take(12, X0 + cw, Y0 + by - ch);
                if (0 < iy) take(12, X0, Y0 + by - ch);
                if (1 < nx) take(12, X0 + cw, Y0 + by);
            }
        } else if (iy == -1) {
            const int bx = mulSat(ix, cw);
            r = at(s, X0 + bx, Y0);
            if (ix + 1 < nx) take(s, X0 + bx + cw, Y0);
            if (s == 10) {
                if (1 < ny) {
                    take(10, X0 + bx, Y0 + ch);
                    if (ix + 1 < nx) take(10, X0 + bx + cw, Y0 + ch);
                    if (ix + 2 < nx) take(10, X0 + bx + cw + cw, Y0 + ch);
                    if (0 < ix) take(10, X0 + bx - cw, Y0 + ch);
                }
                if (0 < ix) take(10, X0 + bx - cw, Y0);
                if (ix + 2 < nx) take(10, X0 + bx + cw + cw, Y0);
            } else if (s == 13) {
                if ((1 < ny) & (0 < ix)) take(13, X0 + bx - cw, Y0 + ch);
                if (1 < ny) take(13, X0 + bx, Y0 + ch);
                if (0 < ix) take(13, X0 + bx - cw, Y0);
            } else if (s == 11) {
                if ((1 < ny) & (ix + 2 < nx)) take(11, X0 + bx + cw + cw, Y0 + ch);
                if ((1 < ny) & (ix + 1 < nx)) take(11, X0 + bx + cw, Y0 + ch);
                if (ix + 2 < nx) take(11, X0 + bx + cw + cw, Y0);
            }
        } else {
            const int bx = mulSat(ix, cw), by = mulSat(iy, ch);
            r = at(s, X0 + bx, Y0 + by);
            if ((ix + 1 < nx) & (iy + 1 < ny)) take(s, X0 + bx + cw, Y0 + by + ch);
            if (ix + 1 < nx) take(s, X0 + bx + cw, Y0 + by);
            if (iy + 1 < ny) take(s, X0 + bx, Y0 + by + ch);
            if (s == 11) {
                if (ix + 1 < nx) {
                    if (iy + 2 < ny) take(11, X0 + bx + cw, Y0 + by + ch + ch);
                    if (0 < iy) take(11, X0 + bx + cw, Y0 + by - ch);
                }
                if (ix + 2 < nx) {
                    take(11, X0 + bx + cw + cw, Y0 + by);
                    if (0 < iy) take(11, X0 + bx + cw + cw, Y0 + by - ch);
                    if (iy + 1 < ny) take(11, X0 + bx + cw + cw, Y0 + by + ch);
                    if (iy + 2 < ny) take(11, X0 + bx + cw + cw, Y0 + by + ch + ch);
                }
            } else if (s == 13) {
                if (iy + 2 < ny) take(13, X0 + bx, Y0 + by + ch + ch);
                if (0 < iy) take(13, X0 + bx, Y0 + by - ch);
                if (0 < ix) {
                    take(13, X0 + bx - cw, Y0 + by);
                    if (iy + 1 < ny) take(13, X0 + bx - cw, Y0 + by + ch);
                    if (iy + 2 < ny) take(13, X0 + bx - cw, Y0 + by + ch + ch);
                    if (0 < iy) take(13, X0 + bx - cw, Y0 + by - ch);
                }
            } else if (s == 10) {
                if (iy + 1 < ny) {
                    if (ix + 2 < nx) take(10, X0 + bx + cw + cw, Y0 + by + ch);
                    if (0 < ix) take(10, X0 + bx - cw, Y0 + by + ch);
                }
                if (iy + 2 < ny) {
                    take(10, X0 + bx, Y0 + by + ch + ch);
                    if (ix + 2 < nx) take(10, X0 + bx + cw + cw, Y0 + by + ch + ch);
                    if (ix + 1 < nx) take(10, X0 + bx + cw, Y0 + by + ch + ch);
                    if (0 < ix) take(10, X0 + bx - cw, Y0 + by + ch + ch);
                }
            } else if (s == 12) {
                if (ix + 2 < nx) take(12, X0 + bx + cw + cw, Y0 + by);
                if (0 < ix) take(12, X0 + bx - cw, Y0 + by);
                if (0 < iy) {
                    take(12, X0 + bx, Y0 + by - ch);
                    if (ix + 1 < nx) take(12, X0 + bx + cw, Y0 + by - ch);
                    if (ix + 2 < nx) take(12, X0 + bx + cw + cw, Y0 + by - ch);
                    if (0 < ix) take(12, X0 + bx - cw, Y0 + by - ch);
                }
            }
        }
        return r;
    }

    // the kernel's distance for the pixel (x, y), y from the bottom
    float distance(int x, int y) const
    {
        if (shape <= 2) return nearestOfCells(x, y, [&](float cx, float cy) { return ellipse(x, y, cx, cy); });
        if (shape <= 5) return nearestOfCells(x, y, [&](float cx, float cy) { return boxy(x, y, cx, cy); });
        if (shape <= 9) return nearestOfCells(x, y, [&](float cx, float cy) { return triangle(x, y, cx, cy); });
        return arrowCells(x, y);
    }

    // the host's normalisation: the largest distance over the frame's four edges, never below 1. The host runs its own distance routine over the edge rows 0 and H-1
    // and columns 0 and W-1, counting the rows from the top and with the first iris (1 - Center y) of a cell from the top: the same arithmetic as the kernel's,
    // fed the rows the other way up.
    float edgeMax(double centerX, double centerY) const
    {
        Iris top = *this;
        top.ox = (float)((double)cellW * centerX);
        top.oy = (float)((1.0 - centerY) * (double)cellH);
        float m = 1.0f;
        for (int x = 0; x < W; ++x) { m = std::fmax(m, top.distance(x, 0)); m = std::fmax(m, top.distance(x, H - 1)); }
        for (int y = 0; y < H; ++y) { m = std::fmax(m, top.distance(0, y)); m = std::fmax(m, top.distance(W - 1, y)); }
        return m;
    }

    // With one iris whose centre lies on the frame's edge the largest distance the host finds is larger than the frame's own edges give: the shape is not symmetric about
    // the centre and the host's scan also counts the points mirrored about it (a pixel less far), e.g. a triangle on the top edge reaches 262.96 where the frame itself
    // reaches 207.46. This is a rule found from the captured pictures (the host's edge loops read from code do not give it), applied to the same edge points; 1 when
    // there is nothing to add.
    float mirroredMax(double centerX, double centerY) const
    {
        float m = 1.0f;
        if (nx != 1 || ny != 1) return m;
        const double lo = 0.001, hi = 0.999;
        const int twiceX = cut((double)(2.0f * ox)), twiceY = cut((double)(2.0f * oy));
        auto mirror = [&](int xi, int yi, bool alongY) {
            const int ex = twiceX - xi, ey = twiceY - yi;
            const float d0 = distance(xi, yi);
            if (distance(ex, ey) == d0) return;
            m = std::fmax(m, distance(alongY ? xi : ex - 1, alongY ? ey - 1 : yi));
        };
        if (centerY <= lo || centerY >= hi) for (int x = 0; x < W; ++x) { mirror(x, 0, true); mirror(x, H - 1, true); }
        if (centerX <= lo || centerX >= hi) for (int y = 0; y < H; ++y) { mirror(0, y, false); mirror(W - 1, y, false); }
        return m;
    }

    Iris(int w, int h, int shapeIn, int repeatX, int repeatY, double progress, int direction, double centerX, double centerY, double featherIn, double borderIn,
         const float* colour, double pixelAspect = 1.0)
    {
        W = std::max(w, 1); H = std::max(h, 1);
        shape = std::min(13, std::max(0, shapeIn));
        nx = std::min(10, std::max(1, repeatX));
        ny = std::min(10, std::max(1, repeatY));
        const float p = (float)finite(progress, 0.0, 1.0, 0.0);
        threshold = p;
        if (direction == 0) { threshold = 1.0f - p; swapPics = true; }
        const float pa = (float)finite(pixelAspect, 1e-3, 1e3, 1.0);
        if (pa < 1.0f) { asp[0] = pa; asp[1] = 1.0f; } else { asp[0] = 1.0f; asp[1] = 1.0f / pa; }
        // (a frame too small for ten cells would give a cell of no pixels; the host would divide by it)
        cellW = std::max(1, cut((double)((float)W / (float)nx + 0.5f)));
        cellH = std::max(1, cut((double)((float)H / (float)ny + 0.5f)));
        ratio = (float)cellH / (float)cellW;
        // the kernel reads Center, Feather and Border as the host stores them (the host's own clamped copies are not passed to it)
        const double cx = finite(centerX, -1e6, 1e6, 0.5), cy = finite(centerY, -1e6, 1e6, 0.5);
        cxScale = (float)cx;
        cyKeep = 1.0f - (float)cy;
        feather = (float)finite(featherIn, -1e6, 1e6, 0.0);
        border = (float)finite(borderIn, -1e6, 1e6, 0.0);
        {
            RENO_WIPES_NO_FMA
            ox = (float)cellW * cxScale;
            oy = (float)cellH * (1.0f - cyKeep);
        }
        for (int k = 0; k < 4; ++k) col[k] = colour ? colour[k] : (k == 3 ? 1.0f : 0.0f);
        col[3] = 1.0f;
        maxDist = edgeMax(cx, cy);
    }

    // first = the picture the kernel gets first (outside the iris), second = inside
    void pixel(const Pic& first, const Pic& second, int x, int y, float* out) const
    {
        RENO_WIPES_NO_FMA
        float a[4], b[4], v[4];
        first.texel(x, y, a);
        second.texel(x, y, b);
        const float d = distance(x, y);
        const float t = d * (1.0f / maxDist);
        // the feather's width: at least 1 percent of a cell count, then a twentieth of that
        float fw = feather;
        if (fw < 0.01f) fw = 0.01f * (float)nx * (float)ny;
        fw *= 0.05f;
        const float bw = border * 0.05f;
        const bool noFeather = fw < 0.000001f, noBorder = bw < 0.000001f;
        const float th = threshold;
        if (noBorder && noFeather) {
            const float* s = (t < th) ? b : a;
            for (int k = 0; k < 4; ++k) out[k] = s[k];
            return;
        }
        if (!noBorder && noFeather) {
            const float lo = clampf(th - bw, 0.0f, th), hi = clampf(th + bw, th, 1.0f);
            for (int k = 0; k < 4; ++k) v[k] = col[k];
            if (t < lo) for (int k = 0; k < 4; ++k) v[k] = b[k];
            if (t > hi) for (int k = 0; k < 4; ++k) v[k] = a[k];
        } else if (noBorder) {
            const float lo = clampf(th - fw, 0.0f, th), hi = clampf(th + fw, th, 1.0f);
            const float inv = 1.0f / (hi - lo);
            float m = (t - lo) * inv;
            m = clampf(m, 0.0f, 0.999f);
            mix4(b, a, m, v);
            if (t < lo) for (int k = 0; k < 4; ++k) v[k] = b[k];
            if (t > hi) for (int k = 0; k < 4; ++k) v[k] = a[k];
        } else {
            float in0 = th - bw, in1 = th + bw;
            in0 = clampf(in0, 0.0f, th);
            in1 = clampf(in1, th, 1.0f);
            float o0 = in0 - fw, o1 = in1 + fw;
            o0 = clampf(o0, 0.0f, in0);
            o1 = clampf(o1, in1, 1.0f);
            const float r0 = 1.0f / (in0 - o0), r1 = 1.0f / (o1 - in1);
            if (t < o0) {
                for (int k = 0; k < 4; ++k) v[k] = b[k];
            } else if (t > o1) {
                for (int k = 0; k < 4; ++k) v[k] = a[k];
            } else if ((o0 <= t) & (t <= in0)) {
                float m = (t - o0) * r0;
                m = clampf(m, 0.0f, 0.999f);
                mix4(b, col, m, v);
            } else if ((in0 < t) & (t < in1)) {
                for (int k = 0; k < 4; ++k) v[k] = col[k];
            } else {
                float m = (t - in1) * r1;
                m = clampf(m, 0.0f, 0.999f);
                mix4(col, a, m, v);
            }
        }
        for (int k = 0; k < 4; ++k) out[k] = v[k];
    }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
