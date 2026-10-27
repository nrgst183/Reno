// Built-in stand-in for VEGAS Pro 18's Cookie Cutter (see builtins.h): VEGAS's identifier, parameters (names, types, ranges, the 14 shapes and 2 methods)
// and presets (Vfx1.ofx.bundle's PresetPackage.xml).
//
// The picture is VEGAS's own, step by step (cookiecutterProcessor in Vfx1.ofx; builtins_cookie_vegas.h has the shape arithmetic):
//   - the frame is cut into RepeatX x RepeatY cells of (int)(size / count + 0.5) pixels; each cell holds the shape at Center (a fraction of the cell from its bottom left);
//   - Size is a fraction of the cell's diagonal, scaled by a constant per shape (1.4182 oval ... 0.8943 arrowheads); Border adds a band Border x the cell's longer side / 4
//     (also scaled by Size) on the shape's outside; the band's width is capped by the shape;
//   - every pixel gets a mask value 0 (inside), 1 (on the border band) or 2 (outside) from the least shape measure over the cells whose shape reaches it
//     (circle, diamond, square, triangles and arrowheads reach into neighbouring cells; oval and rectangle are tested inside their own cell);
//   - Feather (when its width reaches a pixel) blurs the mask with VEGAS's Gaussian (builtins_deriche.h) sized (width + 1.5) pixels;
//   - 8-bit: each pixel is then the byte-wise mix of the picture, the border Color (alpha 255) and nothing (0), with the blurred mask as weight
//     (0 .. 1: outside colour, 1 .. 2: colour to inside; no border: the picture and nothing mixed by mask / 2). Method "Cut away section" swaps picture and nothing.
#include "builtins_impl.h"
#include "builtins_vec.h"
#include "builtins_deriche.h"
#include "builtins_cookie_vegas.h"
#include "vkfilters.h"
#include "vegas8.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

// VEGAS's mix of two 8-bit pixels (bytes R, G, B, A in turn): a * (1 - t) + b * t + 0.5, cut to a whole number, in single precision.
inline void mix8(const int a[4], const int b[4], float t, int out[4])
{
    const float u = 1.0f - t;
    for (int k = 0; k < 4; ++k) {
        const float v = (float)a[k] * u + (float)b[k] * t;
        int n = (int)(v + 0.5f);
        out[k] = n < 0 ? 0 : (n > 255 ? 255 : n);
    }
}

}  // namespace

// The set-up of the processor (shared with the Vulkan plan, vkfilters.cpp)
void cookieSetup(Ctx& c, int W, int H, CookieSetup& o)
{
    using namespace cookiev;
    const int shape = std::max(0, std::min(13, c.i("Shape")));
    o.cutSection = c.i("Method") == 1;
    const double featherP = c.d("Feather"), borderP = c.d("Border"), sizeP = c.d("Size");
    const int repX = std::max(1, std::min(100, c.i("RepeatX"))), repY = std::max(1, std::min(100, c.i("RepeatY")));
    double cxp = 0.5, cyp = 0.5;
    c.xy("Center", cxp, cyp);
    const Color col = c.color("Color");

    // ---- the numbers of the processor's set-up, in single precision as VEGAS has them
    const float sx = 1.0f, sy = 1.0f;                                  // (the pixel aspect ratio: square)
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
    o.br = (float)col.r; o.bg = (float)col.g; o.bb = (float)col.b;
    o.colour[0] = (int)(o.br * 255.0f + 0.5f);
    o.colour[1] = (int)((double)col.g * 255.0 + 0.5);
    o.colour[2] = (int)(o.bb * 255.0f + 0.5f);
    o.colour[3] = 255;
}

namespace {

void cookieCutter(Ctx& c)
{
    using namespace cookiev;
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    CookieSetup su;
    cookieSetup(c, W, H, su);
    const int shape = su.shape, repX = su.repX, repY = su.repY, cw = su.cw, ch = su.ch;
    const bool cutSection = su.cutSection, noBorder = su.noBorder, noFeather = su.noFeather;
    const float sx = 1.0f, sy = 1.0f, cxCell = su.cxCell, cyCell = su.cyCell, ratio = su.ratio, tIn = su.tIn, tOut = su.tOut, rIn = su.rIn, m1 = su.m1, fw2 = su.fw2;
    const float ovalW = su.ovalW, ovalH = su.ovalH, hwF = su.hwF, hhF = su.hhF, boxHi = su.boxHi, boxLo = su.boxLo, nTout = su.nTout;
    const Color col = c.color("Color");

    TileParams tp;
    tp.shape = shape; tp.repX = repX; tp.repY = repY; tp.cw = cw; tp.ch = ch;
    tp.cx0 = cxCell; tp.cy0 = cyCell; tp.ratio = ratio; tp.sx = sx; tp.sy = sy;

    // per column / row: the cell the pixel lies in (-1: before the first centre) and its distance from the centre across the cell (x mod the cell's width ...)
    std::vector<int> xi((size_t)W), yi((size_t)H);
    std::vector<float> xa((size_t)W), ya((size_t)H);
    for (int x = 0; x < W; ++x) {
        const float xf = (float)x;
        if (cxCell <= xf) { const int q = (int)(xf - cxCell) / cw; xi[(size_t)x] = std::min(q, repX - 1); }
        else xi[(size_t)x] = -1;
        xa[(size_t)x] = std::fabs((float)(x % cw) - cxCell) * sx;
    }
    for (int y = 0; y < H; ++y) {
        const float yf = (float)y;
        if (cyCell <= yf) { const int q = (int)(yf - cyCell) / ch; yi[(size_t)y] = std::min(q, repY - 1); }
        else yi[(size_t)y] = -1;
        ya[(size_t)y] = std::fabs((float)(y % ch) - cyCell) * sy;
    }

    // ---- the mask, bottom row first as VEGAS has the frame
    std::vector<float> mask((size_t)W * H, 2.0f);
    forRows(H, W, [&](int y) {
        float* mrow = &mask[(size_t)y * W];
        const float ay = ya[(size_t)y];
        const int iy = yi[(size_t)y];
        for (int x = 0; x < W; ++x) {
            const float ax = xa[(size_t)x];
            const int ix = xi[(size_t)x];
            float m;
            if (shape == 3) {
                const float hx = std::max(hwF + m1, 0.0f), hy = std::max(hhF + m1, 0.0f);
                const float ix0 = std::max(hwF - m1, 0.0f);
                if (ix0 > ax && std::max(hhF - m1, 0.0f) > ay) m = 0.0f;
                else if (ax > hx) m = 2.0f;
                else if (ay > hy) m = 2.0f;
                else m = 1.0f;
            } else if (shape == 1 || shape == 2) {
                const float dx = (ax / ovalW) * (ax / ovalW), dy = (ay / ovalH) * (ay / ovalH);
                const float s2 = dy + dx;
                if (tIn > s2) m = 0.0f;
                else if (s2 > tOut) m = 2.0f;
                else m = 1.0f;
            } else if (shape == 0) {
                if (ax >= boxLo && boxHi >= ax && ay >= boxLo && boxHi > ay) m = 0.0f;
                else if (nTout > ax || ax > tOut || nTout > ay || ay > tOut) m = 2.0f;
                else {
                    const float d = tileDist(x, y, ix, iy, tp);
                    if (noBorder) m = rIn > d ? 0.0f : 2.0f;
                    else if (tIn > d) m = 0.0f;
                    else if (d > tOut) m = 2.0f;
                    else m = 1.0f;
                }
            } else {
                const float d = tileDist(x, y, ix, iy, tp);
                if (tIn > d) m = 0.0f;
                else if (d > tOut) m = 2.0f;
                else m = 1.0f;
            }
            mrow[x] = m;
        }
    });

    // ---- the feather: VEGAS's Gaussian over the mask (Reno's pictures hold the top row first)
    if (!noFeather) {
        Buf mb;
        mb.resize(W, H);
        for (int y = 0; y < H; ++y) {
            const float* mrow = &mask[(size_t)(H - 1 - y) * W];
            for (int x = 0; x < W; ++x) mb.at(x, y)[0] = mrow[x];
        }
        const float s = fw2 + 1.5f;
        vegasDericheBlur(mb, (double)(s * sx), (double)(s * sy), false);
        for (int y = 0; y < H; ++y) {
            float* mrow = &mask[(size_t)(H - 1 - y) * W];
            for (int x = 0; x < W; ++x) mrow[x] = mb.at(x, y)[0];
        }
    }

    // ---- the picture
    const float br = su.br, bg = su.bg, bb = su.bb;
    const int* colour = su.colour;
    (void)col;
    forRows(H, W, [&](int y) {
        const float* mrow = &mask[(size_t)(H - 1 - y) * W];
        for (int x = 0; x < W; ++x) {
            const float m = mrow[x];
            const float* sp = c.src.at(x, y);
            float* op = c.out.at(x, y);
            if (c.levels) {
                int s[4], zero[4] = {0, 0, 0, 0}, r[4];
                for (int k = 0; k < 4; ++k) s[k] = v8::toLevel(sp[k]);
                const int* A = cutSection ? s : zero;      // what a mask of 2 (outside) shows
                const int* B = cutSection ? zero : s;      // what a mask of 0 (inside) shows
                if (noBorder) {
                    if (K(0x3727c5acu) > m) std::memcpy(r, B, sizeof r);
                    else if (m > K(0x3fffffacu)) std::memcpy(r, A, sizeof r);
                    else mix8(B, A, m * 0.5f, r);
                } else {
                    if (K(0x3727c5acu) > m) std::memcpy(r, B, sizeof r);
                    else if (m > K(0x3fffffacu)) std::memcpy(r, A, sizeof r);
                    else if (1.0f >= m) mix8(B, colour, m, r);
                    else mix8(colour, A, m - 1.0f, r);
                }
                for (int k = 0; k < 4; ++k) op[k] = v8::fromLevel(r[k]);
            } else {
                float cf[4] = {br, bg, bb, 1.f}, zero[4] = {0, 0, 0, 0}, r[4];
                const float* A = cutSection ? sp : zero;
                const float* B = cutSection ? zero : sp;
                auto mixf = [](const float* a, const float* b, float t, float* o) { for (int k = 0; k < 4; ++k) o[k] = (1.0f - t) * a[k] + t * b[k]; };
                if (noBorder) {
                    if (K(0x3727c5acu) > m) std::memcpy(r, B, sizeof r);
                    else if (m > K(0x3fffffacu)) std::memcpy(r, A, sizeof r);
                    else mixf(B, A, m * 0.5f, r);
                } else {
                    if (K(0x3727c5acu) > m) std::memcpy(r, B, sizeof r);
                    else if (m > K(0x3fffffacu)) std::memcpy(r, A, sizeof r);
                    else if (1.0f >= m) mixf(B, cf, m, r);
                    else mixf(cf, A, m - 1.0f, r);
                }
                for (int k = 0; k < 4; ++k) op[k] = r[k];
            }
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
    d.colourAboveAlpha = true;      // (VEGAS writes the mixed bytes as they come)
    out.push_back(d);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
