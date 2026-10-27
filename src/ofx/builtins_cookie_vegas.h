// VEGAS's Cookie Cutter shape arithmetic (Vfx1.ofx, cookiecutterProcessor), transcribed step by step: every operation is a single-precision float one in VEGAS's order, so the cut
// comes out pixel for pixel the same. Header only (builtins_cookie.cpp includes it; tests/tools can check it against the plug-in's own routines).
//
//  - cookieShapeDist  : the distance-like measure of one shape (the plug-in's function at 0x180390f30): a pixel's offset from a shape's centre, scaled, run through the shape's
//                       piecewise-linear (triangles, arrowheads) or norm (circle, diamond, square, ...) formula;
//  - cookieTileDist   : the least of those over the cells whose shape can reach the pixel (0x180391770). Which neighbours count depends on the shape (the arrowheads and
//                       triangles stick out of their cell), the cell the pixel lies in (-1: left of / below the first centre) and how many cells there are.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace cookiev {

inline float K(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// the plug-in's float constants (read from its data section)
struct Consts {
    float c333 = K(0x3eaaaaab), c559 = K(0x3f0f1bbd), c577 = K(0x3f13cd3a), c667 = K(0x3f2aaaab), c1118 = K(0x3f8f1bbd), c1154 = K(0x3f93cd3a);
    float c16 = K(0x3fcccccd), c04 = K(0x3ecccccd), c08 = K(0x3f4ccccd), c1732 = K(0x3fddb3d7);
    float m333 = K(0xbeaaaaab), m04 = K(0xbecccccd), m577 = K(0xbf13cd3a), m667 = K(0xbf2aaaab), m08 = K(0xbf4ccccd), m1154 = K(0xbf93cd3a), m16 = K(0xbfcccccd);
};
inline const Consts& C()
{
    static const Consts c;
    return c;
}

// One shape's measure for the offset (x - cx, y - cy) in pixels. K is the cell's height / width ratio (after the pixel aspect), sx / sy the pixel aspect scales.
inline float shapeDist(int x, int y, float cx, float cy, int shape, float Kr, float sx, float sy)
{
    const Consts& c = C();
    float X = (float)x - cx;
    float Y = (float)y - cy;
    X = X * sx;
    Y = Y * sy;
    float r;
    switch (shape) {
    case 0: return std::sqrt(Y * Y + X * X);
    case 1: X = X * Kr; return std::sqrt(Y * Y + X * X);
    case 2: Y = Y * Kr; return std::sqrt(Y * Y + X * X);
    case 3: {
        const float a = std::fabs(Y), b = std::fabs(X) * Kr;
        return a > b ? a : b;
    }
    case 4: return std::fabs(Y) + std::fabs(X);
    case 5: {
        const float a = std::fabs(X), b = std::fabs(Y);
        return a > b ? a : b;
    }
    case 6: {  // triangle, apex up
        float x0 = X * c.c577;
        if (!(0.f < X) && Y > x0) {
            float t0 = c.c667 - Y;
            float t2 = (0.f - X) * -1.f;
            t0 = t0 * c.m577;
            return std::fabs(t0 - t2);
        }
        if (!(x0 < Y)) {
            x0 = -x0;
            if (x0 > Y) {
                float t1 = c.m333 - Y;
                float t0 = c.m577 - X;
                t1 = t1 * c.c1154;
                t0 = t0 * 0.f;
                return std::fabs(t1 - t0);
            }
        }
        float t0 = c.m333 - Y;
        t0 = t0 * c.m577;
        const float t1 = c.c577 - X;
        return std::fabs(t0 - t1);
    }
    case 7: {  // triangle, apex right
        const float x0 = X * c.c1732;
        const float x1 = -x0;
        if (Y > x1 && !(Y < 0.f)) {
            float t0 = c.c667 - X;
            float t2 = (0.f - Y) * -1.f;
            t0 = t0 * c.c577;
            return std::fabs(t2 - t0);
        }
        if (!(x1 < Y) && Y > x0) {
            float t1 = c.c577 - Y;
            float t0 = c.m333 - X;
            t1 = t1 * 0.f;
            t0 = t0 * c.m1154;
            return std::fabs(t1 - t0);
        }
        float t0 = c.m333 - X;
        float t1 = c.m577 - Y;
        t0 = t0 * c.c577;
        return std::fabs(t1 - t0);
    }
    case 8: {  // triangle, apex down
        float x0 = X * c.c577;
        if (!(X < 0.f) && x0 > Y) {
            float t0 = c.m667 - Y;
            t0 = t0 * c.c577;
            const float t1 = 0.f - X;
            return std::fabs(t0 - t1);
        }
        if (0.f > X) {
            x0 = -x0;
            if (!(x0 < Y)) {
                float t1 = c.c333 - Y;
                float t0 = c.m577 - X;
                t1 = t1 * c.c577;
                t0 = t0 * -1.f;
                return std::fabs(t1 - t0);
            }
        }
        float t0 = c.c333;
        float t2 = c.c577 - X;
        t0 = t0 - Y;
        t2 = t2 * 0.f;
        t0 = t0 * c.m1154;
        return std::fabs(t0 - t2);
    }
    case 9: {  // triangle, apex left
        const float x0 = X * c.c1732;
        if (Y > 0.f && !(Y < x0)) {
            float t1 = c.c577 - Y;
            float t0 = c.c333 - X;
            t1 = t1 * -1.f;
            t0 = t0 * c.m577;
            return std::fabs(t1 - t0);
        }
        if (!(0.f < Y)) {
            const float nx = -x0;
            if (nx > Y) {
                float t1 = c.m577 - Y;
                float t0 = c.c333 - X;
                t1 = t1 * -1.f;
                t0 = t0 * c.c577;
                return std::fabs(t1 - t0);
            }
        }
        float t1 = c.m577 - Y;
        float t0 = c.c333 - X;
        t1 = t1 * 0.f;
        t0 = t0 * c.c1154;
        return std::fabs(t1 - t0);
    }
    case 10: {  // arrowhead, apex up
        if (!(X < 0.f)) {
            const float nx = -X;
            if (nx > Y) {
                float t0 = c.m04;
                float t2 = 0.f - X;
                float t1 = t0 - Y;
                t2 = t2 * t0;
                t1 = t1 * c.c08;
                t1 = t1 - t2;
                return std::fabs(t1) * c.c1118;
            }
            if (X > 0.f && !(Y < nx)) {
                const float t0 = c.m08;
                float t1 = c.c08;
                float t2 = t0 - Y;
                t1 = t1 - X;
                t2 = t2 * t0;
                t1 = t1 * c.c16;
                r = std::fabs(t2 - t1);
                return r * c.c559;
            }
        }
        if (!(0.f < X) && Y > X) {
            float t0 = c.c08;
            float t2 = 0.f - X;
            t0 = t0 - Y;
            t2 = t2 * c.m16;
            t0 = t0 * c.m08;
            t0 = t0 - t2;
            return std::fabs(t0) * c.c559;
        }
        float t0 = c.m08;
        float t1 = t0;
        t0 = t0 - X;
        t1 = t1 - Y;
        t0 = t0 * c.c04;
        t1 = t1 * c.c08;
        t1 = t1 - t0;
        return std::fabs(t1) * c.c1118;
    }
    case 11: {  // arrowhead, apex right
        if (!(Y < 0.f)) {
            const float nx = -X;
            if (Y > nx) {
                const float t0 = c.c08;
                float t2 = 0.f - Y;
                t2 = t2 * c.m16;
                float t1 = t0 - X;
                t1 = t1 * t0;
                t2 = t2 - t1;
                return std::fabs(t2) * c.c559;
            }
            if (Y > 0.f && !(nx < Y)) {
                float t2 = c.c08 - Y;
                const float t0 = c.m08;
                t2 = t2 * c.c04;
                float t1 = t0 - X;
                t1 = t1 * t0;
                t2 = t2 - t1;
                return std::fabs(t2) * c.c1118;
            }
        }
        if (!(0.f < Y) && Y > X) {
            float t0 = c.m04;
            float t2 = 0.f - Y;
            t2 = t2 * t0;
            t0 = t0 - X;
            t0 = t0 * c.m08;
            t2 = t2 - t0;
            return std::fabs(t2) * c.c1118;
        }
        float t0 = c.m08;
        float t1 = t0;
        t0 = t0 - X;
        t1 = t1 - Y;
        t0 = t0 * c.c08;
        t1 = t1 * c.c16;
        t1 = t1 - t0;
        return std::fabs(t1) * c.c559;
    }
    case 12: {  // arrowhead, apex down
        if (X > 0.f && !(Y < X)) {
            float t0 = c.c08;
            float t1 = t0;
            t0 = t0 - X;
            t1 = t1 - Y;
            t0 = t0 * c.m04;
            t1 = t1 * c.m08;
            t1 = t1 - t0;
            return std::fabs(t1) * c.c1118;
        }
        if (!(0.f < X)) {
            const float nx = -X;
            if (!(Y <= nx)) {
                float t0 = c.c04;
                float t2 = 0.f - X;
                float t1 = t0 - Y;
                t2 = t2 * t0;
                t1 = t1 * c.m08;
                t1 = t1 - t2;
                return std::fabs(t1) * c.c1118;
            }
            if (!(0.f <= X) && !(nx < Y)) {
                const float t0 = c.c08;
                float t1 = c.m08;
                float t2 = t0 - Y;
                t1 = t1 - X;
                t2 = t2 * t0;
                t1 = t1 * c.m16;
                return std::fabs(t2 - t1) * c.c559;
            }
        }
        float t0 = c.m08;
        float t2 = 0.f - X;
        t0 = t0 - Y;
        t2 = t2 * c.c16;
        t0 = t0 * c.c08;
        t0 = t0 - t2;
        return std::fabs(t0) * c.c559;
    }
    default: {  // 13: arrowhead, apex left
        if (!(Y < 0.f)) {
            if (X > Y) {
                float t0 = c.c04;
                float t2 = 0.f - Y;
                t2 = t2 * t0;
                t0 = t0 - X;
                t0 = t0 * c.c08;
                t2 = t2 - t0;
                return std::fabs(t2) * c.c1118;
            }
            if (Y > 0.f && !(Y < X)) {
                float t0 = c.c08;
                float t1 = t0;
                t0 = t0 - X;
                t1 = t1 - Y;
                t0 = t0 * c.m08;
                t1 = t1 * c.m16;
                t1 = t1 - t0;
                return std::fabs(t1) * c.c559;
            }
        }
        if (!(0.f < Y)) {
            const float nx = -X;
            if (nx > Y) {
                const float t0 = c.m08;
                float t2 = 0.f - Y;
                t2 = t2 * c.c16;
                float t1 = t0 - X;
                t1 = t1 * t0;
                t2 = t2 - t1;
                return std::fabs(t2) * c.c559;
            }
        }
        float t2 = c.m08;
        const float t0 = c.c08;
        t2 = t2 - Y;
        t2 = t2 * c.m04;
        float t1 = t0 - X;
        t1 = t1 * t0;
        t2 = t2 - t1;
        return std::fabs(t2) * c.c1118;
    }
    }
}

struct TileParams {
    int shape = 0, repX = 1, repY = 1;
    int cw = 1, ch = 1;          // the cell's size in whole pixels
    float cx0 = 0, cy0 = 0;      // the first cell's centre (cell width x Center.x ...)
    float ratio = 1, sx = 1, sy = 1;
};

// The least shapeDist over the cells that can reach pixel (x, y); ix / iy are the pixel's cell (-1: before the first centre), as VEGAS finds them.
inline float tileDist(int x, int y, int ix, int iy, const TileParams& p)
{
    const int s = p.shape;
    const float cw = (float)p.cw, ch = (float)p.ch;
    float best = 0;
    bool have = false;
    auto put = [&](float cxx, float cyy) {
        const float d = shapeDist(x, y, cxx, cyy, s, p.ratio, p.sx, p.sy);
        if (!have || d < best) best = d;      // (minss: the first one stays unless the new one is smaller)
        have = true;
    };
    if (ix == -1 && iy == -1) {
        const float X0 = p.cx0, Y0 = p.cy0;
        put(X0, Y0);
        if (s == 10 || s == 11) {
            if (p.repX > 1) {
                if (p.repY > 1) put(cw + X0, ch + Y0);
                put(cw + X0, Y0);
            }
            if (p.repY > 1) put(X0, ch + Y0);
        }
        return best;
    }
    if (ix == -1) {   // iy >= 0
        const int j = iy;
        float Y = (float)(p.ch * j) + p.cy0;
        const float X0 = p.cx0;
        put(X0, Y);
        if (j + 1 < p.repY) put(X0, ch + Y);
        if (s == 11) {
            if (p.repX > 1) {
                const float X1 = cw + X0;
                put(X1, Y);
                if (j > 0) put(X1, Y - ch);
                if (j + 1 < p.repY) put(X1, ch + Y);
                if (j + 2 < p.repY) put(X1, (Y + ch) + ch);
            }
            if (j > 0) put(X0, Y - ch);
            if (j + 2 < p.repY) put(cw + X0, (Y + ch) + ch);
        } else if (s == 10) {
            if (p.repX > 1) {
                if (j + 1 < p.repY) put(cw + X0, ch + Y);
                if (j + 2 < p.repY) put(cw + X0, (Y + ch) + ch);
            }
            if (j + 2 < p.repY) put(X0, (Y + ch) + ch);
        } else if (s == 12) {
            if (p.repX > 1 && j > 0) put(cw + X0, Y - ch);
            if (j > 0) put(X0, Y - ch);
            if (p.repX > 1) put(cw + X0, Y);
        }
        return best;
    }
    if (iy == -1) {   // ix >= 0
        const int i = ix;
        const float X = (float)(p.cw * i) + p.cx0;
        const float Y0 = p.cy0;
        put(X, Y0);
        if (i + 1 < p.repX) put(cw + X, Y0);
        if (s == 10) {
            if (p.repY > 1) {
                const float Y1 = ch + Y0;
                put(X, Y1);
                if (i + 1 < p.repX) put(cw + X, Y1);
                if (i + 2 < p.repX) put((X + cw) + cw, Y1);
                if (i > 0) put(X - cw, Y1);
            }
            if (i > 0) put(X - cw, Y0);
            if (i + 2 < p.repX) put((X + cw) + cw, Y0);
        } else if (s == 13) {
            if (p.repY > 1) {
                if (i > 0) put(X - cw, ch + Y0);
                put(X, ch + Y0);
            }
            if (i > 0) put(X - cw, Y0);
        } else if (s == 11) {
            if (p.repY > 1) {
                if (i + 2 < p.repX) put((X + cw) + cw, ch + Y0);
                if (i + 1 < p.repX) put(cw + X, ch + Y0);
            }
            if (i + 2 < p.repX) put((X + cw) + cw, Y0);
        }
        return best;
    }
    // inside the grid
    const int i = ix, j = iy;
    float X = (float)(p.cw * i) + p.cx0;
    float Y = (float)(p.ch * j) + p.cy0;
    put(X, Y);
    if (i + 1 < p.repX) {
        if (j + 1 < p.repY) put(cw + X, ch + Y);
        put(cw + X, Y);
    }
    if (j + 1 < p.repY) put(X, ch + Y);
    if (s == 11) {
        if (i + 1 < p.repX) {
            if (j + 2 < p.repY) put(cw + X, (Y + ch) + ch);
            if (j > 0) put(cw + X, Y - ch);
        }
        if (i + 2 < p.repX) {
            X = (X + cw) + cw;
            put(X, Y);
            if (j > 0) put(X, Y - ch);
            if (j + 1 < p.repY) put(X, ch + Y);
            if (j + 2 < p.repY) put(X, (Y + ch) + ch);
        }
    } else if (s == 13) {
        if (j + 2 < p.repY) put(X, (Y + ch) + ch);
        if (j > 0) put(X, Y - ch);
        if (i > 0) {
            X = X - cw;
            put(X, Y);
            if (j + 1 < p.repY) put(X, ch + Y);
            if (j + 2 < p.repY) put(X, (Y + ch) + ch);
            if (j > 0) put(X, Y - ch);
        }
    } else if (s == 10) {
        if (j + 1 < p.repY) {
            if (i + 2 < p.repX) put((X + cw) + cw, ch + Y);
            if (i > 0) put(X - cw, ch + Y);
        }
        if (j + 2 < p.repY) {
            Y = (Y + ch) + ch;
            put(X, Y);
            if (i + 2 < p.repX) put((X + cw) + cw, Y);
            if (i + 1 < p.repX) put(cw + X, Y);
            if (i > 0) put(X - cw, Y);
        }
    } else if (s == 12) {
        if (i + 2 < p.repX) put((X + cw) + cw, Y);
        if (i > 0) put(X - cw, Y);
        if (j > 0) {
            Y = Y - ch;
            put(X, Y);
            if (i + 1 < p.repX) put(cw + X, Y);
            if (i + 2 < p.repX) put((X + cw) + cw, Y);
            if (i > 0) put(X - cw, Y);
        }
    }
    return best;
}

}  // namespace cookiev
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
