// Built-in stand-in for VEGAS Pro 18's Cookie Cutter (see builtins.h): VEGAS's identifier, parameters (names, types, ranges, the 14 shapes and 2 methods)
// and presets (Vfx1.ofx's PresetPackage.xml).
//
// Measured against VEGAS's own effect (Vfx1.ofx run through Reno's host under Wine, tests/vegas/vegofx_wine.sh). In pixels (x right, y up from the bottom row;
// a pixel is taken at its corner, as VEGAS does):
//   - the frame is cut into RepeatX x RepeatY cells (0: no shape); each cell gets the shape at Center (0..1 of the cell, from its bottom left). The shapes of
//     all cells together make the cut; circle, oval and rectangle are cut off at their cell's edges, the other shapes reach into neighbouring cells;
//   - Size: the shape's "radius" r = Size x the cell's diagonal. Circle: radius r. Oval: an ellipse through the corners of Rectangle (semi-axes
//     r x sqrt 2 x width / diagonal, ... x height / diagonal). Oval side: turned on its side and larger (semi-axes 1.861 r x height / diagonal, 1.861 r x
//     width / diagonal). Rectangle: the cell's shape with its corners on the circle. Diamond: half-diagonal 1.3665 r. Square: half-side 0.8786 r.
//     Triangles: equilateral round the circle (apex 2 r from the centre). Arrowheads: apex 2 r out, wings 2 r to the sides and back, notch r back;
//   - Border (0..1, in Color): a band T = Border x the cell's longer side / 2 wide. Its outside is the shape grown by T x Size / 2 in radius (Rectangle: T / 2
//     each way; ovals: not grown), its inside the shape shrunk by T across the band (in radius: T circle, 1.138 T square, 0.732 T diamond, 0.866 T
//     triangles, 1.12 T arrowheads, 0.68 T Oval, 0.515 T Oval side; Rectangle: T / 2 each way);
//   - Feather (0..2): Gaussian blur of the cut's edges, sigma 0.0783 x Feather x the cell's longer side, at most 0.305 r (with a border: 0.305 of the
//     inside's radius, and 0.18 T);
//   - Method "Cut away all but section" leaves only the shape (and border); "Cut away section" cuts the shape out of the frame (border stays).
// The frame inside keeps its alpha; the border is the colour, opaque. Shapes and border are within a pixel or so of VEGAS's, the feather close
// (tests/vegas/vegcookie_test --compare).
#include "builtins_impl.h"
#include "builtins_vec.h"
#include "../core/base.h"
#include <algorithm>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

// The 14 shapes, as VEGAS lists them.
enum Shape { Circle, Oval, OvalSide, Rectangle, Diamond, Square, Tri, TriRight, TriDown, TriLeft, Arrow, ArrowRight, ArrowDown, ArrowLeft, ShapeCount };

struct Pt { double x, y; };

bool inPolygon(const Pt* v, int n, double x, double y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((v[i].y > y) != (v[j].y > y)) {
            const double xc = v[j].x + (y - v[j].y) * (v[i].x - v[j].x) / (v[i].y - v[j].y);
            if (std::fabs(x - xc) <= 1e-6) return true;      // (a pixel exactly on an edge is inside)
            if (x < xc) in = !in;
        }
    }
    return in;
}

// A shape of one cell: is the point (x, y), relative to the shape's centre in pixels (y up), inside it at "radius" r? 'grow' is added to Rectangle's half-sides
// (its border works in pixels) and 'axes' are the cell's width / diagonal and height / diagonal (for shapes that take the cell's shape).
struct ShapeTest {
    int shape = Circle;
    double aw = 0.7071, ah = 0.7071;
    bool inside(double x, double y, double r, double rectGrow) const
    {
        if (r <= 0 && !(shape == Rectangle && rectGrow > 0)) return false;
        const double ax = std::fabs(x), ay = std::fabs(y);
        switch (shape) {
        case Circle: return x * x + y * y < r * r;      // (a pixel exactly on the edge is outside, as in VEGAS)
        case Oval: case OvalSide: {
            const double k = shape == Oval ? std::sqrt(2.0) : 1.861;
            const double a = shape == Oval ? k * r * aw : k * r * ah, b = shape == Oval ? k * r * ah : k * r * aw;
            if (a <= 0 || b <= 0) return false;
            return (x / a) * (x / a) + (y / b) * (y / b) < 1.0;
        }
        case Rectangle: return ax <= r * aw + rectGrow + 1e-6 && ay <= r * ah + rectGrow + 1e-6;      // (the edge pixel counts: the 1e-6 is float noise)
        case Diamond: return ax + ay <= 1.3665 * r + 1e-6;
        case Square: return std::max(ax, ay) <= 0.8786 * r + 1e-6;
        default: break;
        }
        const double s3 = std::sqrt(3.0);
        Pt v[4];
        int n = 3;
        switch (shape) {
        case Tri: v[0] = {0, 2 * r}; v[1] = {s3 * r, -r}; v[2] = {-s3 * r, -r}; break;
        case TriRight: v[0] = {2 * r, 0}; v[1] = {-r, -s3 * r}; v[2] = {-r, s3 * r}; break;
        case TriDown: v[0] = {0, -2 * r}; v[1] = {-s3 * r, r}; v[2] = {s3 * r, r}; break;
        case TriLeft: v[0] = {-2 * r, 0}; v[1] = {r, s3 * r}; v[2] = {r, -s3 * r}; break;
        case Arrow: n = 4; v[0] = {0, 2 * r}; v[1] = {2 * r, -2 * r}; v[2] = {0, -r}; v[3] = {-2 * r, -2 * r}; break;
        case ArrowRight: n = 4; v[0] = {2 * r, 0}; v[1] = {-2 * r, -2 * r}; v[2] = {-r, 0}; v[3] = {-2 * r, 2 * r}; break;
        case ArrowDown: n = 4; v[0] = {0, -2 * r}; v[1] = {-2 * r, 2 * r}; v[2] = {0, r}; v[3] = {2 * r, 2 * r}; break;
        case ArrowLeft: n = 4; v[0] = {-2 * r, 0}; v[1] = {2 * r, 2 * r}; v[2] = {r, 0}; v[3] = {2 * r, -2 * r}; break;
        default: return false;
        }
        return inPolygon(v, n, x, y);
    }
    double reach(double r, double rectGrow) const
    {
        switch (shape) {
        case Circle: return r;
        case Oval: return std::sqrt(2.0) * r * std::max(aw, ah);
        case OvalSide: return 1.861 * r * std::max(aw, ah);
        case Rectangle: return r + rectGrow * 1.5;
        case Diamond: return 1.3665 * r;
        case Square: return 0.8786 * r * 1.5;
        default: return 2.0 * r * 1.5;
        }
    }
};

// How much the inside of the border is shrunk, in radius, per pixel of border (see the top of the file).
double innerShrink(int shape)
{
    switch (shape) {
    case Circle: return 1.0;
    case Square: return 1.138;
    case Diamond: return 0.732;
    case Tri: case TriRight: case TriDown: case TriLeft: return 0.866;
    case Arrow: case ArrowRight: case ArrowDown: case ArrowLeft: return 1.12;
    default: return 1.0;
    }
}

// A shape's test along one row of pixels: what ShapeTest::inside works out from the row alone is computed once, so each pixel needs a few comparisons (same arithmetic, same answer).
struct RowShape {
    int kind = 0;
    double rr = 0, yy = 0, a = 1, lim = 0, ay = 0;
    bool yok = false;
    double xc[4];
    int nx = 0;
    void prepare(const ShapeTest& st, double dy, double r, double rectGrow)
    {
        kind = 0;
        nx = 0;
        if (r <= 0 && !(st.shape == Rectangle && rectGrow > 0)) return;
        ay = std::fabs(dy);
        switch (st.shape) {
        case Circle: kind = 1; rr = r * r; yy = dy * dy; return;
        case Oval: case OvalSide: {
            const double k = st.shape == Oval ? std::sqrt(2.0) : 1.861;
            const double aa = st.shape == Oval ? k * r * st.aw : k * r * st.ah, bb = st.shape == Oval ? k * r * st.ah : k * r * st.aw;
            if (aa <= 0 || bb <= 0) return;
            kind = 2; a = aa; yy = (dy / bb) * (dy / bb);
            return;
        }
        case Rectangle: kind = 3; lim = r * st.aw + rectGrow; yok = ay <= r * st.ah + rectGrow + 1e-6; return;
        case Diamond: kind = 4; lim = 1.3665 * r; return;
        case Square: kind = 5; lim = 0.8786 * r; return;
        default: break;
        }
        const double s3 = std::sqrt(3.0);
        Pt v[4];
        int n = 3;
        switch (st.shape) {
        case Tri: v[0] = {0, 2 * r}; v[1] = {s3 * r, -r}; v[2] = {-s3 * r, -r}; break;
        case TriRight: v[0] = {2 * r, 0}; v[1] = {-r, -s3 * r}; v[2] = {-r, s3 * r}; break;
        case TriDown: v[0] = {0, -2 * r}; v[1] = {-s3 * r, r}; v[2] = {s3 * r, r}; break;
        case TriLeft: v[0] = {-2 * r, 0}; v[1] = {r, s3 * r}; v[2] = {r, -s3 * r}; break;
        case Arrow: n = 4; v[0] = {0, 2 * r}; v[1] = {2 * r, -2 * r}; v[2] = {0, -r}; v[3] = {-2 * r, -2 * r}; break;
        case ArrowRight: n = 4; v[0] = {2 * r, 0}; v[1] = {-2 * r, -2 * r}; v[2] = {-r, 0}; v[3] = {-2 * r, 2 * r}; break;
        case ArrowDown: n = 4; v[0] = {0, -2 * r}; v[1] = {-2 * r, 2 * r}; v[2] = {0, r}; v[3] = {2 * r, 2 * r}; break;
        case ArrowLeft: n = 4; v[0] = {-2 * r, 0}; v[1] = {2 * r, 2 * r}; v[2] = {r, 0}; v[3] = {2 * r, -2 * r}; break;
        default: return;
        }
        kind = 6;
        for (int i = 0, j = n - 1; i < n; j = i++)
            if ((v[i].y > dy) != (v[j].y > dy)) xc[nx++] = v[j].x + (dy - v[j].y) * (v[i].x - v[j].x) / (v[i].y - v[j].y);
    }
    bool at(double dx) const
    {
        switch (kind) {
        case 1: return dx * dx + yy < rr;
        case 2: return (dx / a) * (dx / a) + yy < 1.0;
        case 3: return yok && std::fabs(dx) <= lim + 1e-6;
        case 4: return std::fabs(dx) + ay <= lim + 1e-6;
        case 5: return std::max(std::fabs(dx), ay) <= lim + 1e-6;
        case 6: {
            bool in = false;
            for (int i = 0; i < nx; ++i)
                {
                    if (std::fabs(dx - xc[i]) <= 1e-6) return true;
                    if (dx < xc[i]) in = !in;
                }
            return in;
        }
        default: return false;
        }
    }
};

void cookieCutter(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    const int shape = std::max(0, std::min((int)ShapeCount - 1, c.i("Shape")));
    const bool cutSection = c.i("Method") == 1;
    const double feather = clampd(c.d("Feather"), 0, 2);
    const double border = clampd(c.d("Border"), 0, 1);
    const int rx = std::max(0, std::min(100, c.i("RepeatX"))), ry = std::max(0, std::min(100, c.i("RepeatY")));
    const double size = clampd(c.d("Size"), 0, 10);
    double cx = 0.5, cy = 0.5;
    c.xy("Center", cx, cy);
    const Color col = c.color("Color");

    // the inside of the cut (I) and the inside of its border's outer edge (O), per pixel, top row first
    const size_t N = (size_t)W * H;
    // (without a border the inside is the same as the outer edge's inside, at every pixel: I is then not kept at all)
    std::vector<float> O(N, 0.f), I;
    bool haveBorder = false;
    double sigma = 0;
    if (rx > 0 && ry > 0) {
        const double cw = (double)W / rx, ch = (double)H / ry;
        const double diag = std::hypot(cw, ch), longer = std::max(cw, ch);
        ShapeTest st;
        st.shape = shape;
        st.aw = cw / diag;
        st.ah = ch / diag;
        const double r = size * diag;
        const double T = border * longer / 2;
        haveBorder = T > 0;
        if (haveBorder) I.assign(N, 0.f);
        double rO = r, rI = r, growO = 0, growI = 0;
        if (T > 0) {
            if (shape == Rectangle) { growO = T / 2; growI = -T / 2; }
            else if (shape == Oval || shape == OvalSide) {
                // the ovals: not grown; the inside shrunk in radius (and so in both axes alike)
                rI = r - (shape == Oval ? 0.68 : 0.515) * T;
            } else {
                rO = r * (1 + T / (2 * diag));
                rI = rO - innerShrink(shape) * T;
            }
        }
        // VEGAS's blur grows with Feather up to 0.305 of the radius (of the border's inside, with a border) and 0.18 of the border's width
        if (feather > 0) {
            sigma = std::min(0.0783 * feather * longer + c.px(0.7), 0.305 * std::max(0.0, T > 0 && shape != Rectangle ? rI : r));
            if (T > 0) sigma = std::min(sigma, 0.18 * T);
        }
        const double reach = st.reach(std::max(rO, r), growO) + 2;
        // the cells' ranges of columns and rows (same for every cell of a column / row), then the shapes drawn row by row (a row is one thread's: no two write the same pixel)
        const bool own = shape == Circle || shape == Oval || shape == OvalSide || shape == Rectangle;
        struct Span { int lo, hi; double pos; };
        std::vector<Span> xsp((size_t)rx), ysp((size_t)ry);
        for (int kx = 0; kx < rx; ++kx) {
            const double px = (kx + cx) * cw;
            const int cx0 = own ? (int)std::ceil(kx * cw - 1e-9) : 0, cx1 = own ? std::min(W, (int)std::ceil((kx + 1) * cw - 1e-9)) - 1 : W - 1;
            xsp[(size_t)kx] = Span{std::max(cx0, (int)std::floor(px - reach)), std::min(cx1, (int)std::ceil(px + reach)), px};
        }
        for (int ky = 0; ky < ry; ++ky) {
            const double py = (ky + cy) * ch;
            const int cj0 = own ? (int)std::ceil(ky * ch - 1e-9) : 0, cj1 = own ? std::min(H, (int)std::ceil((ky + 1) * ch - 1e-9)) - 1 : H - 1;
            ysp[(size_t)ky] = Span{std::max(cj0, (int)std::floor(py - reach)), std::min(cj1, (int)std::ceil(py + reach)), py};
        }
        const double shift = c.shift();
        forRows(H, W, [&](int y) {
            const int j = H - 1 - y;                                      // (y up, from the bottom row)
            float* Orow = &O[(size_t)y * W];
            float* Irow = haveBorder ? &I[(size_t)y * W] : nullptr;
            RowShape sO, sI;
            for (int ky = 0; ky < ry; ++ky) {
                const Span& sy = ysp[(size_t)ky];
                if (j < sy.lo || j > sy.hi) continue;
                const double dy = j + shift - sy.pos;
                sO.prepare(st, dy, rO, growO);
                if (T > 0) sI.prepare(st, dy, rI, growI);
                for (int kx = 0; kx < rx; ++kx) {
                    const Span& sx = xsp[(size_t)kx];
                    for (int x = sx.lo; x <= sx.hi; ++x) {
                        const double dx = x + shift - sx.pos;
                        if (Orow[x] < 1.f && sO.at(dx)) Orow[x] = 1.f;
                        if (haveBorder && Irow[x] < 1.f && sI.at(dx)) Irow[x] = 1.f;
                    }
                }
            }
        });
        if (T > 0)
            for (size_t k = 0; k < N; ++k) I[k] = std::min(I[k], O[k]);
    }
    if (sigma > 0) {
        gaussianBlurPlane(O, W, H, sigma, sigma);
        if (haveBorder) gaussianBlurPlane(I, W, H, sigma, sigma);
    }
    const float br = (float)clampd(col.r, 0, 1), bg = (float)clampd(col.g, 0, 1), bb = (float)clampd(col.b, 0, 1);
    const float colour[4] = {br, bg, bb, 1.f};
    const v4::V4 vcol = v4::ld(colour);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const size_t k = (size_t)y * W + x;
            const float o = clamp01(O[k]);
            if (!haveBorder) {
                const float keep = cutSection ? 1.f - o : o;
                v4::st(c.out.at(x, y), v4::add(v4::mul(v4::ld(c.src.at(x, y)), v4::set(keep)), v4::zero()));
                continue;
            }
            const float in = std::min(o, clamp01(I[k]));
            const float band = o - in;
            const float keep = cutSection ? 1.f - o : in;
            v4::st(c.out.at(x, y), v4::add(v4::mul(v4::ld(c.src.at(x, y)), v4::set(keep)), v4::mul(vcol, v4::set(band))));
        }
    });
}

}  // namespace

void addFiltersCookie(std::vector<Def>& out)
{
    Def d;
    d.id = "com.vegascreativesoftware:cookiecutter";
    d.label = "Cookie Cutter";
    d.group = kGroup;
    d.description = "The picture cut to a shape, or a shape cut out of it (Reno's own, standing in for VEGAS Cookie Cutter).";
    std::vector<PDef> ps = {
        rgb("Color", "Color", 0, 0, 0),
        choice("Shape", "Shape", 0, {"Circle", "Oval", "Oval side", "Rectangle", "Diamond", "Square", "Triangle", "Triangle right",
                                     "Triangle upside down", "Triangle left", "Arrowhead", "Arrowhead right", "Arrowhead upside down",
                                     "Arrowhead left"}),
        choice("Method", "Method", 0, {"Cut away all but section", "Cut away section"}),
        dbl("Feather", "Feather", 0, 0, 2),
        dbl("Border", "Border", 0, 0, 1),
        intp("RepeatX", "Repeat X", 1, 1, 100),
        intp("RepeatY", "Repeat Y", 1, 1, 100),
        dbl("Size", "Size", 0.25, 0, 1),
        point("Center", "Center", 0.5, 0.5),
        dbl("StereoscopicDepth", "Stereoscopic 3D depth", 0, -0.1, 0.1),
    };
    // the ranges VEGAS's effect declares: Repeat 1..100 shown 1..10, Center 0..1, the depth -0.5..0.5 shown -0.1..0.1
    for (auto& p : ps) {
        if (p.name == "RepeatX" || p.name == "RepeatY") { p.dmin = 1; p.dmax = 10; }
        if (p.name == "Center") { p.min = 0; p.max = 1; }
        if (p.name == "StereoscopicDepth") { p.min = -0.5; p.max = 0.5; p.dmin = -0.1; p.dmax = 0.1; }
    }
    d.params = ps;
    d.render = cookieCutter;
    out.push_back(d);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
