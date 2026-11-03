// Built-in media generators with VEGAS Pro 18's identifiers, parameters and choices (Solid Color, Color Gradient, Checkerboard,
// Noise Texture, Test Pattern, Titles & Text). See builtins.h.
#include "builtins_impl.h"
#include "builtins_more_internal.h"
#include "vfxgen.h"
#include "vkeffects.h"
#include "vkfx2.h"
#include "vfxtitles.h"
#include "vfxtitlerich.h"
#include "creditgdi.h"
#include "vfxcredit.h"
#include "vktext.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

// The generators' OpenCL kernels (vfxgen.h) give a float per channel; into an 8-bit frame it is cut to a level by the card's store (more::oclStore12), a float frame keeps it.
inline float genStore(const Ctx& c, float v) { return c.host8 ? more::oclStore12(v) : v; }
inline void genPut(const Ctx& c, float* o, const float* v)
{
    for (int k = 0; k < 4; ++k) o[k] = genStore(c, v[k]);
}

// VEGAS's generators write 8-bit pictures by truncating (0.25 -> 63, 0.5 -> 127), not by rounding.
inline float truncLevel(double v) { return (float)(std::floor(v * 255.0 + 1e-4) / 255.0); }
inline void putGen(float* p, const Color& c)
{
    const double a = c.a;
    p[0] = truncLevel(c.r * a); p[1] = truncLevel(c.g * a); p[2] = truncLevel(c.b * a); p[3] = truncLevel(a);
}

// ------------------------------------------------------------------ Solid Color
void solidColor(Ctx& c)
{
    const Color col = c.color("Color");
    float p[4];
    if (c.openCl) {
        const double rgba[4] = {col.r, col.g, col.b, col.a};
        float v[4];
        vfxgen::solidColor(rgba, v);
        genPut(c, p, v);
        for (int k = 0; k < c.out.w * c.out.h; ++k) std::memcpy(&c.out.px[(size_t)k * 4], p, sizeof p);
        return;
    }
    putGen(p, col);
    for (int k = 0; k < c.out.w * c.out.h; ++k) std::memcpy(&c.out.px[(size_t)k * 4], p, sizeof p);
}

// ------------------------------------------------------------------ Color Gradient
// VEGAS's Color Gradient is a list of colour stops (position 0..1, colour with its own alpha) laid along a shape: Linear runs from Start to End, Elliptical
// and Rectangular run outwards from Center to Radius (both in fractions of the frame; angle turns the shape). Colours and alpha are blended unpremultiplied.
// 'GradientStops' holds the stops as "position r g b a;position r g b a;..."; empty means two stops, Color1 at 0 and Color2 at 1 (VEGAS's default is white
// to black, left to right). Like VEGAS's legacy effects the position of a pixel is its index, not its centre.
struct GradStop { double pos; Color col; };

std::vector<GradStop> parseStops(const std::string& s, const Color& a, const Color& b)
{
    std::vector<GradStop> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        double n[5];
        int k = 0;
        const char* p = s.c_str() + i;
        const char* end = s.c_str() + j;
        while (k < 5) {
            char* q = nullptr;
            const double x = std::strtod(p, &q);
            if (q == p || q > end) break;
            n[k++] = x;
            p = q;
        }
        if (k == 5 && std::isfinite(n[0])) v.push_back({clampd(n[0], 0, 1), Color{n[1], n[2], n[3], n[4]}});
        i = j + 1;
    }
    if (v.empty()) v = {{0.0, a}, {1.0, b}};
    std::stable_sort(v.begin(), v.end(), [](const GradStop& x, const GradStop& y) { return x.pos < y.pos; });
    return v;
}

Color stopColor(const std::vector<GradStop>& v, double t)
{
    if (t <= v.front().pos) return v.front().col;
    if (t >= v.back().pos) return v.back().col;
    size_t k = 1;
    while (k < v.size() && v[k].pos < t) ++k;
    const GradStop& p = v[k - 1];
    const GradStop& q = v[k];
    const double d = q.pos - p.pos;
    return d <= 1e-9 ? q.col : mix(p.col, q.col, (t - p.pos) / d);
}

// the shape's numbers, worked out once per frame (colorGradient() and the Vulkan plan use these)
struct GradSetup {
    int type = 0;
    std::vector<GradStop> stops;
    double sx, sy, ex, ey, cx, cy, rx, ry, ca, sa, ddx, ddy, dd;
};
void gradSetup(Ctx& c, GradSetup& g)
{
    g.type = c.i("GradientType");
    g.stops = parseStops(c.s("GradientStops"), c.color("Color1"), c.color("Color2"));
    c.xy("Start", g.sx, g.sy);
    c.xy("End", g.ex, g.ey);
    c.xy("Center", g.cx, g.cy);
    c.xy("Radius", g.rx, g.ry);
    g.sy = 1 - g.sy; g.ey = 1 - g.ey; g.cy = 1 - g.cy; // the parameters are OFX points (y up); the rows below run from the top
    g.rx = std::max(1e-4, std::fabs(g.rx));
    g.ry = std::max(1e-4, std::fabs(g.ry));
    const double ang = c.d("AspectRatioAngle") * kPi / 180.0;
    g.ca = std::cos(ang); g.sa = std::sin(ang);
    g.ddx = g.ex - g.sx; g.ddy = g.ey - g.sy; g.dd = g.ddx * g.ddx + g.ddy * g.ddy;
}

void colorGradient(Ctx& c)
{
    GradSetup g;
    gradSetup(c, g);
    const int type = g.type;
    const std::vector<GradStop>& stops = g.stops;
    const double sx = g.sx, sy = g.sy, cx = g.cx, cy = g.cy, rx = g.rx, ry = g.ry, ca = g.ca, sa = g.sa, ddx = g.ddx, ddy = g.ddy, dd = g.dd;
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double u = (double)x / W, v = (double)y / H; // row 0 is the top of the picture
            double t;
            if (type == 0) {
                t = dd > 1e-12 ? ((u - sx) * ddx + (v - sy) * ddy) / dd : 0.0;
            } else {
                // offset from the centre in pixels, turned by the angle, then measured against the radii (in frame widths / heights)
                const double px = (u - cx) * W, py = (v - cy) * H;
                const double qx = (px * ca + py * sa) / (rx * W), qy = (-px * sa + py * ca) / (ry * H);
                t = type == 1 ? std::sqrt(qx * qx + qy * qy) : std::max(std::fabs(qx), std::fabs(qy));
            }
            put(c.out.at(x, y), stopColor(stops, clampd(t, 0, 1)));
        }
    });
}

// ------------------------------------------------------------------ Checkerboard
// Tiles of Color 1 and Color 2; tile width/height are fractions of the frame (Square: as tall as wide). Edge blending softens the change over
// that fraction of a tile (Proportional: vertical as horizontal). Grid offset moves the grid by fractions of a tile.
double tileWave(double u, double blend)
{
    const double i = std::floor(u), f = u - i;
    const double base = (((long long)i) & 1) ? 1.0 : 0.0;
    if (blend <= 1e-6) return base;
    const double hb = blend * 0.5;
    if (f < hb) return lerp(1 - base, base, 0.5 + f / blend);
    if (f > 1 - hb) return lerp(1 - base, base, 0.5 + (1 - f) / blend);
    return base;
}

// how much of the interval [u0, u1] (in tiles) lies in odd tiles: the exact coverage of a pixel across a hard edge
double oddCover(double u0, double u1)
{
    auto F = [](double u) {
        const double n = std::floor(u);
        return std::floor(n / 2) + ((((long long)n) & 1) ? (u - n) : 0.0);
    };
    return u1 > u0 ? (F(u1) - F(u0)) / (u1 - u0) : 0.0;
}

// VEGAS's 8-bit checkerboard, transliterated from the plug-in: single-precision tile maths walked pixel by pixel (x advances by 1.0 in float),
// colours cut to bytes first, edges blended in 16.16 weights. Row 0 here is the top of the picture.
static bool checkerboardFx8(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Color ca = c.color("Color1"), cb = c.color("Color2");
    auto toByte = [](double col, double al) { return (int)((float)(col * al) * 255.0f); };
    int P1[4] = {toByte(ca.r, ca.a), toByte(ca.g, ca.a), toByte(ca.b, ca.a), (int)((float)ca.a * 255.0f)};
    int P2[4] = {toByte(cb.r, cb.a), toByte(cb.g, cb.a), toByte(cb.b, cb.a), (int)((float)cb.a * 255.0f)};
    float tw = (float)c.d("TileDimensionsWidth") * (float)W;
    float th = (float)c.d("TileDimensionsHeight") * (float)H;
    if (c.b("TileDimensionsSquare")) th = tw * 1.0f;
    const float inv = 1.0f / 1.0f;
    if (inv > tw) { th = th * inv / tw; tw = inv; }
    if (1.0f > th) { tw = tw / th; th = 1.0f; }
    const float hx = std::max(0.5f, (float)c.d("EdgeBlendingHorizontal") * tw * 0.5f);
    const double bvd = c.b("EdgeBlendingProportional") ? c.d("EdgeBlendingHorizontal") : c.d("EdgeBlendingVertical");
    const float hy = std::max(0.5f, (float)bvd * th * 0.5f);
    const float kx = 32768.0f / hx, ky = 32768.0f / hy;
    const float ox = (float)c.d("GridOffsetX") * (float)W, oy = (float)c.d("GridOffsetY") * (float)H;
    // nonsense settings (not a number, huge): the plain float picture below copes with them, the tile walk would not end
    const float lim = 1e6f;
    if (!(std::fabs(tw) < lim && std::fabs(th) < lim && tw >= 1.0f && th >= 1.0f && std::fabs(ox) < lim && std::fabs(oy) < lim && hx < lim && hy < lim)) return false;
    forRows(H, W, [&](int y) {
        // memory row m = H-1-y in the plug-in; its counter is the row index from the top
        const float yc = (float)y + 0.5f - oy;
        float x = 0.5f - ox;
        int ix = (int)(x / tw);
        if ((float)ix * tw > x) --ix;
        int iy = (int)(yc / th);
        if ((float)iy * th > yc) --iy;
        float xl = (float)ix * tw + hx, xr = (float)(ix + 1) * tw - hx;
        const float yl = (float)iy * th + hy, yu = (float)(iy + 1) * th - hy;
        int C0[2][4], C1[2][4]; // [parity] colours: tile colour, neighbour colour
        int A[4], B[4];
        if (yc >= yl && yu > yc) {
            for (int k = 0; k < 4; ++k) { A[k] = P1[k]; B[k] = P2[k]; }
        } else {
            int wv;
            if (yl > yc) wv = (int)((yl - yc) * ky + 0.5f);
            else wv = (int)((yc - yu) * ky + 0.5f);
            for (int k = 0; k < 4; ++k) {
                A[k] = (P1[k] * (0x10000 - wv) + P2[k] * wv) >> 16;
                B[k] = (P2[k] * (0x10000 - wv) + P1[k] * wv) >> 16;
            }
        }
        // even tile: c0 = A, c1 = B; odd tile: c0 = B, c1 = A
        for (int k = 0; k < 4; ++k) { C0[0][k] = A[k]; C1[0][k] = B[k]; C0[1][k] = B[k]; C1[1][k] = A[k]; }
        for (int px = 0; px < W; ++px) {
            while (!(xl > x) && !(xr > x)) { ++ix; xl += tw; xr += tw; }
            const int par = (ix + iy) & 1;
            float* o = c.out.at(px, y);
            if (xl > x) {
                const int w = (int)((xl - x) * kx + 0.5f);
                for (int k = 0; k < 4; ++k)
                    o[k] = (float)((C0[par][k] * (0x10000 - w) + C1[par][k] * w) >> 16) / 255.0f;
            } else {
                for (int k = 0; k < 4; ++k) o[k] = (float)C0[par][k] / 255.0f;
            }
            x += 1.0f;
        }
    });
    return true;
}

// VEGAS with GPU acceleration on draws the checkerboard with its OpenCL kernel (vfxgen.h): float tile maths, edges mixed in float. The kernel only reads the vertical blending
// setting; the plug-in's Proportional switch copies the horizontal one into it, so here it is the horizontal setting when Proportional is on.
static void checkerboardOpenCl(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Color a = c.color("Color1"), b = c.color("Color2");
    const double col1[4] = {a.r, a.g, a.b, a.a}, col2[4] = {b.r, b.g, b.b, b.a};
    const double eh = c.d("EdgeBlendingHorizontal");
    const double ev = c.b("EdgeBlendingProportional") ? eh : c.d("EdgeBlendingVertical");
    const vfxgen::Checker k = vfxgen::checkerHost(W, H, c.d("TileDimensionsWidth"), c.d("TileDimensionsHeight"), c.b("TileDimensionsSquare"), eh, ev, c.d("GridOffsetX"),
                                                  c.d("GridOffsetY"), col1, col2, 1.0f);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float v[4];
            vfxgen::checker(k, x, H - 1 - y, v);
            genPut(c, c.out.at(x, y), v);
        }
    });
}

void checkerboard(Ctx& c)
{
    if (c.openCl && c.out.w > 0 && c.out.h > 0) { checkerboardOpenCl(c); return; }
    if (c.levels && checkerboardFx8(c)) return;
    const int W = c.out.w, H = c.out.h;
    // tiles are never smaller than a pixel
    const double tw = std::max(1.0 / std::max(1, W), c.d("TileDimensionsWidth"));
    const bool square = c.b("TileDimensionsSquare");
    const double th = std::max(1.0 / std::max(1, H), square ? tw * W / std::max(1, H) : c.d("TileDimensionsHeight"));
    const double bh = clampd(c.d("EdgeBlendingHorizontal"), 0, 1);
    const double bv = c.b("EdgeBlendingProportional") ? bh : clampd(c.d("EdgeBlendingVertical"), 0, 1);
    const Color a = c.color("Color1"), b = c.color("Color2");
    // the grid offset is a fraction of the whole frame (1 = one frame width/height), not of a tile
    const double ox = c.d("GridOffsetX"), oy = c.d("GridOffsetY");
    forRows(H, W, [&](int y) {
        const double v = (((y + 0.5) / H) - oy) / th;
        // hard edges are anti-aliased by the exact area a pixel covers; soft ones are read at the pixel centre
        const double sy = bv <= 1e-6 ? oddCover(((double)y / H - oy) / th, ((double)(y + 1) / H - oy) / th) : tileWave(v, bv);
        for (int x = 0; x < W; ++x) {
            const double u = (((x + 0.5) / W) - ox) / tw;
            const double sx = bh <= 1e-6 ? oddCover(((double)x / W - ox) / tw, ((double)(x + 1) / W - ox) / tw) : tileWave(u, bh);
            // VEGAS mixes in two 8-bit steps: Color 1 -> Color 2 by the horizontal wave (and the same with colours swapped), each truncated to a
            // level, then those two by the vertical wave (softened exclusive or)
            float* o = c.out.at(x, y);
            const double pa[4] = {a.r * a.a, a.g * a.a, a.b * a.a, a.a}, pb[4] = {b.r * b.a, b.g * b.a, b.b * b.a, b.a};
            for (int k = 0; k < 4; ++k) {
                const double m1 = std::floor(lerp(pa[k], pb[k], sx) * 255.0 + 1e-4);
                const double m2 = std::floor(lerp(pb[k], pa[k], sx) * 255.0 + 1e-4);
                o[k] = (float)(std::floor(m1 + (m2 - m1) * sy + 1e-4) / 255.0);
            }
        }
    });
}

// ------------------------------------------------------------------ Noise Texture
// VEGAS's gradient noise: Perlin's improved noise in 3D (x, y, progress as z) with a fixed permutation and 256 unit gradients (copied from the plug-in), summed over the layers, each doubling the
// frequency and weighing 'Grain' times the previous (last fractional layer by its fraction; layers under 0.01 left out); doubles cut to 8-bit levels, exact. Styles differ in how the sum becomes
// the 0..1 mix: 0 and 1 directly (1 folds each layer to its absolute value), 2 to 5 a smooth wave ("marble"; 5 without folding is "contour"), 6 another wave ("rings").
const unsigned char kNtPerm[256] = {
    209, 59, 152, 243, 49, 141, 92, 83, 155, 106, 77, 35, 164, 111, 206, 68,
    80, 91, 60, 159, 153, 56, 110, 157, 247, 126, 3, 44, 32, 98, 71, 79,
    118, 163, 224, 74, 36, 236, 66, 133, 40, 104, 37, 140, 234, 88, 75, 240,
    120, 214, 144, 150, 165, 197, 11, 238, 115, 69, 86, 251, 183, 82, 57, 239,
    207, 43, 142, 117, 33, 114, 84, 87, 180, 127, 48, 89, 189, 223, 215, 54,
    53, 73, 5, 24, 38, 174, 172, 194, 248, 12, 108, 147, 97, 138, 25, 219,
    137, 129, 233, 101, 119, 124, 195, 186, 130, 19, 131, 241, 212, 232, 217, 218,
    14, 16, 151, 20, 55, 156, 9, 158, 143, 63, 26, 46, 136, 210, 148, 182,
    171, 149, 202, 160, 30, 96, 177, 178, 205, 29, 116, 103, 222, 135, 52, 185,
    184, 225, 61, 17, 252, 90, 23, 64, 179, 72, 216, 250, 204, 169, 162, 213,
    176, 105, 191, 4, 181, 188, 139, 27, 208, 161, 229, 246, 123, 132, 2, 6,
    13, 227, 51, 220, 199, 196, 134, 102, 62, 107, 93, 70, 28, 198, 78, 201,
    21, 95, 146, 235, 109, 244, 42, 125, 193, 168, 41, 187, 122, 31, 7, 175,
    99, 18, 22, 145, 94, 65, 81, 34, 249, 245, 47, 154, 230, 192, 221, 203,
    231, 237, 8, 67, 128, 253, 242, 113, 121, 112, 15, 85, 45, 50, 254, 58,
    190, 166, 0, 1, 255, 100, 76, 211, 167, 226, 39, 10, 170, 228, 173, 200,
};
const float kNtGrad[256][3] = {
    {-0.632021844f, -0.6313169f, 0.449430019f},
    {0.954781651f, -0.147500291f, -0.258138657f},
    {0.353760034f, 0.29955107f, -0.886071622f},
    {-0.472004771f, 0.186954647f, 0.861544788f},
    {0.825473964f, 0.434834719f, -0.359877139f},
    {0.756674886f, -0.383278221f, 0.529661179f},
    {0.572191834f, -0.494837999f, 0.654012144f},
    {0.180834562f, -0.406724811f, 0.895474017f},
    {-0.514184892f, 0.35044539f, -0.782816589f},
    {-0.25449267f, 0.685079396f, -0.682568431f},
    {-0.945111632f, 0.00959634036f, 0.326606691f},
    {0.238931537f, -0.129385144f, -0.962377846f},
    {0.0390419215f, -0.970326364f, -0.238626271f},
    {0.537169456f, -0.123363823f, 0.83440423f},
    {-0.523458719f, -0.210350543f, -0.825677693f},
    {-0.674951375f, 0.161260083f, 0.720024884f},
    {0.76430124f, 0.602141023f, -0.2308027f},
    {0.0482929684f, 0.677604616f, -0.733839095f},
    {0.910870016f, 0.162033543f, 0.379553556f},
    {0.352846593f, 0.400435567f, 0.845665812f},
    {-0.0847442597f, -0.545654058f, 0.833714664f},
    {-0.29420504f, -0.930896401f, -0.216506734f},
    {0.01492415f, -0.934082389f, 0.356745481f},
    {0.44505623f, 0.894269884f, 0.0469726995f},
    {0.147731811f, -0.904640257f, -0.399751693f},
    {-0.969118059f, -0.142977074f, -0.200917035f},
    {-0.631905496f, 0.681078613f, -0.369901836f},
    {0.41505608f, 0.907163441f, 0.0691590309f},
    {0.743749201f, -0.462011307f, -0.483096749f},
    {0.357444078f, 0.763350487f, 0.538079679f},
    {0.559143007f, -0.17122969f, 0.811196327f},
    {0.184999004f, -0.532037914f, 0.826263309f},
    {0.374915779f, -0.514591873f, 0.771124721f},
    {0.335779399f, -0.913899362f, 0.228123218f},
    {-0.434410691f, 0.74835664f, 0.501247942f},
    {0.0321342014f, 0.746676803f, 0.664410412f},
    {0.904275715f, 0.423433036f, 0.0546805598f},
    {0.54545939f, 0.535597742f, 0.644677579f},
    {0.862437487f, -0.167532355f, 0.477634192f},
    {-0.847685218f, 0.279369891f, 0.450979143f},
    {-0.260036379f, -0.18358016f, -0.94798702f},
    {-0.7492221f, -0.589094162f, -0.302711517f},
    {-0.141397998f, 0.989916623f, 0.00847536977f},
    {0.171842396f, -0.0104292696f, -0.985069215f},
    {-0.412385255f, 0.563717544f, -0.715654135f},
    {-0.640807867f, -0.0373211391f, -0.766793668f},
    {-0.227398083f, -0.0550100096f, 0.972246885f},
    {-0.486526698f, 0.168237016f, 0.857314527f},
    {-0.6273036f, 0.26115793f, 0.733680189f},
    {0.104019716f, -0.702412844f, 0.704127848f},
    {0.311345071f, 0.392836839f, 0.865299642f},
    {0.594492197f, 0.451181322f, -0.665593266f},
    {-0.7867468f, 0.302277625f, 0.538198531f},
    {0.867355764f, -0.462026954f, 0.185000181f},
    {0.522615075f, -0.314318866f, 0.792513192f},
    {0.670457065f, -0.041927889f, 0.740762651f},
    {0.793827176f, 0.456744671f, -0.401525706f},
    {0.764932275f, 0.0134084905f, 0.643971026f},
    {0.94707191f, 0.145256609f, 0.286278367f},
    {0.985738695f, 0.0305222496f, 0.1654917f},
    {0.228059441f, -0.301938504f, 0.925646782f},
    {0.857159495f, 0.0802579075f, 0.508759379f},
    {0.849859774f, -0.376360774f, 0.368905067f},
    {0.630335271f, 0.762257338f, 0.147109404f},
    {0.759676337f, 0.628785789f, -0.165892646f},
    {0.36033392f, -0.659636378f, -0.659575045f},
    {-0.499917805f, 0.0709266067f, 0.86316371f},
    {-0.874369264f, 0.483862847f, 0.0368120298f},
    {0.893995047f, 0.444875896f, -0.0534634702f},
    {-0.0775214732f, -0.696994662f, -0.712873638f},
    {-0.304014415f, 0.946924806f, -0.104444697f},
    {0.291053534f, -0.954101384f, 0.0705574229f},
    {-0.44104141f, -0.344178289f, 0.828868985f},
    {-0.465939313f, -0.851628423f, -0.240061596f},
    {0.607420802f, -0.149186835f, 0.780245602f},
    {0.342999518f, -0.189209029f, -0.920082211f},
    {0.618270576f, -0.766479015f, -0.173929706f},
    {0.826916993f, 0.558918476f, 0.061794281f},
    {0.956870496f, 0.261228621f, -0.127116323f},
    {-0.492487609f, -0.198158786f, 0.84746027f},
    {0.350317061f, 0.857484818f, -0.376826018f},
    {0.000688460015f, 0.998100758f, -0.0615991689f},
    {0.715239763f, 0.270071f, 0.644587934f},
    {-0.0374345593f, 0.894938052f, 0.444617301f},
    {0.430314451f, -0.788734734f, 0.439006954f},
    {0.457574517f, 0.777794659f, -0.430884033f},
    {-0.283702523f, -0.956883252f, 0.0623477884f},
    {0.0125136003f, -0.966774702f, -0.25532344f},
    {0.381114572f, 0.789853871f, -0.480502427f},
    {-0.879241884f, -0.204854131f, -0.430079639f},
    {0.186066225f, -0.196513712f, 0.962684691f},
    {0.00578350993f, -0.858432651f, 0.512893558f},
    {-0.0455651805f, -0.276799738f, 0.959846735f},
    {0.168110088f, -0.422939032f, 0.890427768f},
    {0.791633487f, 0.2763578f, -0.544924557f},
    {-0.120776422f, 0.958845675f, 0.256959379f},
    {-0.115582488f, 0.0356028005f, 0.992659688f},
    {0.415114552f, -0.607368767f, -0.677335322f},
    {-0.501600087f, 0.764224768f, 0.405410618f},
    {0.917584538f, 0.0799244195f, -0.389423549f},
    {0.924654543f, 0.230255276f, -0.303309292f},
    {-0.534012854f, 0.00584636023f, -0.845456183f},
    {0.334698617f, 0.934495389f, -0.121224277f},
    {0.105373412f, -0.854051113f, 0.509404659f},
    {0.0811083466f, -0.995676875f, -0.0452658497f},
    {0.686470628f, -0.679825902f, 0.258059651f},
    {-0.401877582f, 0.107448317f, -0.909367561f},
    {-0.617365718f, -0.17219387f, -0.767599404f},
    {0.0408716016f, 0.159160227f, -0.986406446f},
    {-0.335335255f, 0.892566681f, -0.301454961f},
    {-0.369363368f, 0.879991055f, 0.298641145f},
    {-0.268170029f, 0.215565354f, 0.93894434f},
    {0.234693244f, -0.403113842f, 0.884544194f},
    {0.670546472f, 0.551739812f, -0.495934129f},
    {0.853802681f, 0.5204795f, 0.01104852f},
    {0.67592895f, -0.468275011f, -0.569068074f},
    {0.524117351f, -0.745441198f, 0.41184774f},
    {0.145945415f, 0.62643671f, 0.765687287f},
    {0.528777063f, -0.464192837f, -0.71057719f},
    {-0.729022443f, 0.0357923694f, -0.683553338f},
    {-0.976433992f, 0.184612229f, 0.111781299f},
    {0.924077094f, -0.338425726f, 0.177621782f},
    {-0.487800121f, -0.0513456203f, 0.871443987f},
    {-0.672495484f, -0.0840662792f, 0.73531121f},
    {-0.945778191f, 0.0572668016f, 0.319725364f},
    {0.837264776f, 0.197199985f, 0.50999999f},
    {0.49635464f, 0.8610515f, -0.110554643f},
    {-0.405636907f, -0.229653075f, 0.88471365f},
    {-0.823903739f, 0.382124037f, 0.418525696f},
    {-0.192206055f, 0.454568028f, -0.869726837f},
    {-0.704855025f, -0.292509913f, 0.646233261f},
    {-0.304066628f, -0.631276608f, -0.71346575f},
    {0.667836368f, -0.669048071f, -0.326143235f},
    {0.888780713f, 0.324475527f, -0.323704213f},
    {-0.076699771f, 0.91109401f, -0.404999614f},
    {0.606342852f, -0.0118827801f, 0.795114577f},
    {-0.428599954f, 0.468034595f, 0.772816658f},
    {-0.304361165f, 0.887989998f, -0.344729006f},
    {-0.934200644f, -0.197730586f, 0.296937168f},
    {-0.729938984f, 0.676512659f, -0.0975681394f},
    {-0.796298444f, -0.445507258f, 0.409184575f},
    {0.275760531f, 0.10296724f, -0.95569551f},
    {0.780700862f, 0.0920220986f, -0.618092299f},
    {-0.0725622922f, 0.198201835f, 0.97747159f},
    {0.955705583f, -0.246177047f, 0.161319166f},
    {0.125384614f, -0.700275958f, 0.702774704f},
    {0.541255414f, 0.6702106f, 0.507799566f},
    {0.762872219f, 0.221342236f, 0.607481241f},
    {-0.396637142f, -0.513441324f, -0.760958016f},
    {0.0826625973f, 0.732429266f, 0.675806403f},
    {0.0738804266f, 0.270366013f, -0.959918618f},
    {0.462454081f, -0.180588216f, 0.868057609f},
    {-0.630181313f, 0.184952125f, 0.754098237f},
    {-0.232619256f, 0.951409936f, 0.201760575f},
    {0.0603179485f, -0.997192621f, 0.0443698987f},
    {-0.242930859f, 0.165536955f, -0.955814838f},
    {-0.675700545f, -0.427481234f, 0.60057348f},
    {0.559653103f, -0.571622491f, 0.600030005f},
    {-0.420353651f, 0.8397578f, -0.343670905f},
    {-0.326143295f, -0.106156088f, 0.939340889f},
    {-0.914475501f, 0.049554389f, -0.401595563f},
    {-0.98488903f, 0.166598573f, 0.0473133884f},
    {-0.448724627f, -0.183433264f, -0.874641836f},
    {0.740719974f, 0.613282263f, 0.274260312f},
    {0.764341712f, 0.528626561f, -0.369236469f},
    {0.365992099f, 0.255655348f, -0.894812942f},
    {-0.464309782f, 0.0414738916f, 0.884701312f},
    {-0.0377787091f, -0.427899152f, -0.903036594f},
    {0.317592472f, -0.948078036f, 0.0168290194f},
    {0.197550356f, -0.735577345f, 0.647996843f},
    {-0.440842748f, -0.76814425f, 0.464340627f},
    {0.778120637f, 0.541816115f, -0.317747891f},
    {0.800476611f, 0.410247684f, 0.436959803f},
    {0.0599538982f, 0.863843858f, -0.50017941f},
    {0.550501168f, 0.635104239f, -0.541840494f},
    {0.447680801f, 0.443350673f, -0.776544869f},
    {-0.0296798106f, 0.555365622f, -0.831076562f},
    {-0.430254817f, 0.122667469f, -0.894334078f},
    {0.578779817f, -0.649321198f, 0.493351698f},
    {0.0513922907f, 0.69785434f, 0.714393556f},
    {-0.189053297f, 0.734385133f, -0.651872218f},
    {-0.982912719f, 0.00511948997f, 0.184001267f},
    {0.212485507f, 0.550620258f, 0.807259142f},
    {-0.164192602f, -0.594946861f, 0.786815703f},
    {0.411994994f, -0.781010926f, 0.469342113f},
    {0.397296935f, -0.252945602f, -0.882141531f},
    {-0.162458748f, -0.983092546f, -0.0844761133f},
    {0.0434848405f, -0.0790772736f, 0.995919585f},
    {0.980742455f, 0.167313159f, 0.100749582f},
    {-0.255049318f, -0.214634016f, -0.942805469f},
    {0.416761547f, 0.526148975f, -0.741267145f},
    {0.243968189f, 0.390909046f, 0.887507558f},
    {0.0845535621f, 0.971838892f, 0.219954103f},
    {0.482301503f, -0.515166223f, 0.708511889f},
    {-0.51476717f, -0.847400486f, 0.130104572f},
    {-0.276735663f, 0.829997361f, -0.484274358f},
    {0.465685278f, 0.148032546f, 0.872481287f},
    {-0.899321079f, 0.30203706f, -0.316220313f},
    {-0.0942182094f, 0.678336322f, -0.728685558f},
    {0.142340273f, -0.283733249f, 0.948279798f},
    {0.796658158f, -0.222710222f, 0.561903834f},
    {0.29420808f, 0.924680889f, -0.241674915f},
    {-0.939554989f, -0.291558444f, -0.179527104f},
    {-0.941163063f, -0.264440119f, 0.210436404f},
    {-0.869844973f, -0.299776405f, -0.391795784f},
    {0.844697714f, 0.367443413f, 0.389192909f},
    {0.660534918f, 0.167161331f, 0.731949925f},
    {-0.725935042f, 0.687546194f, 0.0172714796f},
    {-0.379518479f, 0.900496483f, 0.212301269f},
    {-0.663716972f, 0.705189347f, 0.249374866f},
    {0.481121778f, 0.662659526f, 0.573937476f},
    {0.842597604f, -0.202597126f, 0.498982728f},
    {0.911723018f, -0.400140733f, -0.0929980427f},
    {-0.678896785f, -0.661218941f, -0.319200218f},
    {-0.307136983f, -0.0264386795f, -0.951298058f},
    {-0.502329588f, -0.1042215f, -0.858372092f},
    {0.779143929f, -0.237307519f, -0.580189526f},
    {-0.0558312498f, 0.959195971f, -0.277175009f},
    {-0.374109924f, -0.30202055f, -0.876826882f},
    {0.921355963f, -0.320382982f, -0.220131636f},
    {0.740400374f, 0.634150565f, 0.22284621f},
    {-0.147570908f, 0.873220384f, 0.464444786f},
    {-0.983156681f, 0.12215741f, -0.135942921f},
    {0.655451179f, -0.661592245f, 0.364251941f},
    {-0.232852355f, 0.853592217f, 0.466004372f},
    {0.722922742f, 0.687986553f, -0.0636973083f},
    {0.0225846004f, 0.267963767f, -0.96316421f},
    {0.247242481f, 0.838057816f, -0.486343801f},
    {0.691602349f, -0.355716467f, -0.628611207f},
    {-0.562794268f, 0.703157246f, 0.434548646f},
    {0.895489156f, 0.31194663f, -0.317472309f},
    {0.267260879f, -0.777879953f, 0.568748176f},
    {-0.433485091f, 0.882578313f, 0.182060584f},
    {0.857562184f, -0.513672888f, 0.0269661006f},
    {0.766079724f, -0.364076525f, -0.529688656f},
    {-0.0497050807f, -0.936199844f, 0.347935766f},
    {0.757798254f, -0.506513536f, 0.411322057f},
    {-0.70532161f, 0.653744221f, 0.274116337f},
    {-0.224687144f, -0.592211545f, 0.773822427f},
    {0.906426668f, 0.401639611f, -0.130676165f},
    {0.0132026104f, 0.144351721f, 0.989438295f},
    {0.902996957f, 0.346173793f, 0.254480332f},
    {0.102231763f, -0.994726598f, -0.00823246967f},
    {0.783261061f, -0.612913966f, 0.104107819f},
    {-0.15579018f, -0.702284396f, -0.694641054f},
    {-0.10738679f, -0.576350093f, -0.81011641f},
    {0.959814548f, -0.159317777f, -0.231027797f},
    {-0.116715349f, 0.078335233f, 0.990071356f},
    {-0.991574645f, 0.0949250534f, 0.0881422907f},
    {0.274067521f, -0.01922171f, 0.961518407f},
    {0.138985708f, 0.976548493f, 0.164426148f},
    {0.409180582f, -0.912011504f, -0.0283960402f},
    {0.0600317903f, -0.507165253f, 0.859755516f},
    {-0.0720754489f, -0.075561069f, 0.994532943f},
    {-0.753437877f, -0.375127822f, 0.540009499f},
    {-0.643426239f, -0.192163929f, -0.74099648f},
};

// 16.16 fixed point and round-to-nearest as VEGAS gets from adding 2^52+2^51 and reading the low bits
inline int32_t ntRound(double v)
{
    v += 6755399441055744.0;
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return (int32_t)(uint32_t)bits;
}
const double kNtHalf = 0.499999985;

// One layer of noise for one row: adds 'count' values with x step dx to out[]. The row start uses full interpolation; inside a cell the two x-end
// values move by their slopes (as VEGAS does; its rounding is part of the exact result).
void ntLayer(double x, double y, double z, double dx, int count, bool fold, double amp, double* out)
{
    amp *= 1.5151515151515151;
    const double X = x + 1000000.0, Y = y + 1000000.0, Z = z + 1000000.25;
    int32_t xi = ntRound(X - kNtHalf);
    const int32_t yi = ntRound(Y - kNtHalf), zi = ntRound(Z - kNtHalf);
    double xf = X - (double)xi;
    const double yf = Y - (double)yi, zf = Z - (double)zi;
    const int y0 = yi & 255, y1 = (yi + 1) & 255, z0 = zi & 255, z1 = (zi + 1) & 255;
    const int ha = kNtPerm[(kNtPerm[z0] + y0) & 255], hb = kNtPerm[(kNtPerm[z0] + y1) & 255];
    const int hc = kNtPerm[(kNtPerm[z1] + y0) & 255], hd = kNtPerm[(kNtPerm[z1] + y1) & 255];
    const double fy = ((yf * 6.0 - 15.0) * yf + 10.0) * ((yf * yf) * yf);
    const double fz = ((zf * 6.0 - 15.0) * zf + 10.0) * ((zf * zf) * zf);
    const double yf1 = yf - 1.0, zf1 = zf - 1.0;
    double l0 = 0, l1 = 0, s0 = 0, s1 = 0;
    auto cell = [&]() {
        const int x0 = xi & 255, x1 = (xi + 1) & 255;
        const int g000 = kNtPerm[(ha + x0) & 255], g100 = kNtPerm[(ha + x1) & 255], g010 = kNtPerm[(hb + x0) & 255], g110 = kNtPerm[(hb + x1) & 255];
        const int g001 = kNtPerm[(hc + x0) & 255], g101 = kNtPerm[(hc + x1) & 255], g011 = kNtPerm[(hd + x0) & 255], g111 = kNtPerm[(hd + x1) & 255];
        const double xf1 = xf - 1.0;
        auto dot = [](int g, double a, double b, double c) { const float* v = kNtGrad[g]; return ((double)v[1] * b + (double)v[0] * a) + (double)v[2] * c; };
        const double d000 = dot(g000, xf, yf, zf), d100 = dot(g100, xf1, yf, zf), d010 = dot(g010, xf, yf1, zf), d110 = dot(g110, xf1, yf1, zf);
        const double d001 = dot(g001, xf, yf, zf1), d101 = dot(g101, xf1, yf, zf1), d011 = dot(g011, xf, yf1, zf1), d111 = dot(g111, xf1, yf1, zf1);
        const double a00 = (d001 - d000) * fz + d000, a10 = (d101 - d100) * fz + d100;
        const double a01 = (d011 - d010) * fz + d010, a11 = (d111 - d110) * fz + d110;
        l0 = (a01 - a00) * fy + a00;
        l1 = (a11 - a10) * fy + a10;
        auto gx = [](int g) { return (double)kNtGrad[g][0]; };
        const double p00 = (gx(g001) - gx(g000)) * fz + gx(g000), p01 = (gx(g011) - gx(g010)) * fz + gx(g010);
        const double p10 = (gx(g101) - gx(g100)) * fz + gx(g100), p11 = (gx(g111) - gx(g110)) * fz + gx(g110);
        s0 = ((p01 - p00) * fy + p00) * dx;
        s1 = ((p11 - p10) * fy + p10) * dx;
    };
    cell();
    for (int i = 0; i < count; ++i) {
        const double f = ((xf * 6.0 - 15.0) * xf + 10.0) * ((xf * xf) * xf);
        double v = ((l1 - l0) * f + l0) * amp;
        if (fold) v = std::fabs(v);
        out[i] = v + out[i];
        xf += dx;
        if (xf < 1.0) {
            l0 += s0;
            l1 += s1;
        } else {
            const int32_t n = ntRound(xf - kNtHalf);
            xi += n;
            xf -= (double)n;
            cell();
        }
    }
}

// With GPU acceleration on VEGAS draws the texture with two OpenCL kernels (vfxgen.h): float noise layers, then the style's wave and the mix of the two colours.
static void noiseTextureOpenCl(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Color c1 = c.color("Color1"), c2 = c.color("Color2");
    const double col1[4] = {c1.r, c1.g, c1.b, c1.a}, col2[4] = {c2.r, c2.g, c2.b, c2.a};
    const vfxgen::Noise k = vfxgen::noiseHost(W, H, c.i("NoiseStyle"), c.b("Swapcolors"), c.d("FrequencyX"), c.d("FrequencyY"), c.d("NumberOfLayers"), c.d("OffsetX"),
                                              c.d("OffsetY"), c.d("OffsetprogressinDegrees"), c.d("NoiseParametersGrainAmplitudeFalloff"), c.d("NoiseParametersMin"),
                                              c.d("NoiseParametersMax"), c.d("NoiseParametersBias"), c.d("NoiseParametersAmplitude"), col1, col2);
    const vfxgen::NoiseLayers L = vfxgen::noiseLayers(k);
    forRows(H, W * 16, [&](int y) {
        std::vector<float> row;
        const int yk = H - 1 - y;
        vfxgen::noiseRow(k, kNtPerm, kNtGrad, yk, row);
        for (int x = 0; x < W; ++x) {
            float v[4];
            vfxgen::noiseColour(k, vfxgen::noiseMixOf(k, L, row, x, yk), v);
            genPut(c, c.out.at(x, y), v);
        }
    });
}

void noiseTexture(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.openCl && W > 0 && H > 0) { noiseTextureOpenCl(c); return; }
    const int style = (int)clampd((double)c.i("NoiseStyle"), 0, 6);
    const double layers = c.d("NumberOfLayers");
    const Color c1 = c.color("Color1"), c2 = c.color("Color2");
    const bool swap = c.b("Swapcolors");
    const double fx = c.d("FrequencyX"), fy = c.d("FrequencyY");
    const double ox = c.d("OffsetX"), oy = c.d("OffsetY");
    const double z0 = c.d("OffsetprogressinDegrees");
    const double lo = c.d("NoiseParametersMin"), hi = c.d("NoiseParametersMax");
    const double bias = c.d("NoiseParametersBias"), amp = c.d("NoiseParametersAmplitude");
    const double falloff = c.d("NoiseParametersGrainAmplitudeFalloff");

    struct Style { int mode; bool fold; float p[5]; };
    static const Style kStyles[7] = {{0, false, {}},
                                     {0, true, {}},
                                     {1, true, {0.9f, 0.15f, 0.4f, 1.2f, 0.1f}},
                                     {1, true, {0.05f, 0.9f, 1.3f, 0.f, 0.f}},
                                     {1, true, {0.6f, 0.f, 0.2f, 0.5f, 0.1f}},
                                     {1, false, {1.0f, 0.f, 1.5f, 0.5f, 0.05f}},
                                     {2, false, {4.0f, 0, 0, 0, 0}}};
    const Style& st = kStyles[style];

    // the two colours in the float premultiplied form VEGAS mixes, in its B, G, R, A order (colour A is the one at mix 0)
    float k1[4] = {(float)(c1.a * c1.b), (float)(c1.a * c1.g), (float)(c1.a * c1.r), (float)c1.a};
    float k2[4] = {(float)(c2.a * c2.b), (float)(c2.a * c2.g), (float)(c2.a * c2.r), (float)c2.a};
    const float* ca = swap ? k2 : k1;
    const float* cb = swap ? k1 : k2;

    double w[16] = {1.0};
    const int nl = (int)clampd((double)ntRound(layers - kNtHalf), 0, 14);
    const double frac = layers - (double)nl;
    for (int i = 1; i <= nl; ++i) w[i] = falloff * w[i - 1];
    w[nl] *= frac;
    int used = 0;
    double sumW = 0;
    while (used <= nl && w[used] > 0.01) sumW += w[used++];

    double k, b;
    if (lo < hi) {
        k = amp / (hi - lo) * 65536.0;
        b = bias * 65536.0 - lo * k + 0.5;
    } else {
        k = 0;
        b = (((lo + hi) * 0.5) * amp + bias) * 65536.0 + 0.5;
    }
    if (st.mode == 0) {
        double s = sumW;
        if (!st.fold) {
            b = k * 0.5 + b;
            s = sumW + sumW;
        }
        k /= s;
    } else if (st.mode == 2) {
        k *= 0.5;
    }

    const double stepX = (1.0 / (double)(float)W) * fx, stepY = (1.0 / (double)(float)W) * fy;
    forRows(H, W, [&](int y) {
        std::vector<double> sum((size_t)W, 0.0);
        double xx = (ox + 0.0) * fx, yy = ((double)((float)y / (float)W) + oy) * fy, zz = z0, dd = stepX;
        for (int o = 0; o < used; ++o) {
            ntLayer(xx, yy, zz, dd, W, st.fold, w[o], sum.data());
            xx += xx; yy += yy; zz += zz; dd += dd;
        }
        double base = 0, p0dx = 0, p2s = 0;
        if (st.mode == 1) {
            const double p0 = st.p[0], p1 = st.p[1], p2 = st.p[2], p3 = st.p[3], p4 = st.p[4];
            p2s = p2 * sumW;
            p0dx = p0 * stepX;
            double t = -(((double)W * oy + (double)y) * (-stepY) * p1);
            if (p3 > 0) {
                double v = -(((oy - 1000.0) * (double)W + (double)y) * (-stepY) * p3);
                const double fr = v - (double)ntRound(v - kNtHalf);
                const double g = fr < 0.5 ? 1.0 - (fr * 4.0 - 1.0) * (fr * 4.0 - 1.0) : (fr * 4.0 - 3.0) * (fr * 4.0 - 3.0) - 1.0;
                t += p4 * g;
            }
            base = (double)W * ox * p0dx + t;
        }
        for (int x = 0; x < W; ++x) {
            const double n = sum[(size_t)x];
            double v;
            if (st.mode == 0) {
                v = k * n + b;
            } else if (st.mode == 1) {
                const double t = p2s * n + (double)x * p0dx + base;
                const double fr = t - (double)ntRound(t - kNtHalf);
                const double g = 1.0 - (fr + fr - 1.0) * (fr + fr - 1.0);
                v = g * k + b;
            } else {
                const double t = (n + 2.0) * ((double)st.p[0] * sumW);
                const double fr = t - (double)ntRound(t - kNtHalf);
                const double g = fr < 0.5 ? 2.0 - (fr * 4.0 - 1.0) * (fr * 4.0 - 1.0) : (fr * 4.0 - 3.0) * (fr * 4.0 - 3.0);
                v = g * k + b;
            }
            int32_t q = ntRound(v + 1.5e-8);
            if ((q | 0xffff) != 0xffff) q = q < 0 ? 0 : 0x10000;
            const double mix = (double)q * 1.52587890625e-05;
            float* o = c.out.at(x, y);
            // VEGAS keeps the bytes in B, G, R, A order; the mode 0 rounds to nearest, the waves truncate
            for (int ch = 0; ch < 4; ++ch) {
                double m = ((double)cb[ch] * mix + (double)ca[ch] * (1.0 - mix)) * 255.0;
                if (st.mode == 0) m += 0.5;
                const int level = (int)m;
                o[ch == 3 ? 3 : 2 - ch] = (float)((double)level / 255.0);
            }
        }
    });
}

// ------------------------------------------------------------------ Test Pattern
// Test Pattern: VEGAS's nine patterns in broadcast 8-bit levels (black 7, white 235, 75% bars 180, 16/25 pluge steps). The seven bars are
// floor(width/7) wide, the last taking the rest; the "porch" of framed patterns is the middle 80% of the frame (ends included, rows from the
// bottom); the Gradient uses the frame's own coordinates. Matches VEGAS Pro 18 at 320x180.
void testPattern(Ctx& c)
{
    Buf& o = c.out;
    const int type = c.i("TestPatternType");
    const int W = o.w, H = o.h;
    if (c.openCl && W > 0 && H > 0) {
        // VEGAS with GPU acceleration on: the OpenCL kernel (vfxgen.h), levels over 255 in float, then the card's 8-bit store
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                float v[4];
                vfxgen::testPattern(type, W, H, x, H - 1 - y, v);
                genPut(c, o.at(x, y), v);
            }
        });
        return;
    }
    struct Rgb { int r, g, b; };
    const Rgb bars[7] = {{180, 180, 180}, {16, 180, 180}, {180, 180, 16}, {16, 180, 16}, {180, 16, 180}, {16, 16, 180}, {180, 16, 16}};
    const Rgb castle[7] = {{235, 7, 7}, {16, 16, 16}, {180, 7, 180}, {16, 16, 16}, {180, 180, 7}, {16, 16, 16}, {180, 180, 180}};
    const Rgb black = {7, 7, 7};
    const int stepsLvl[4] = {70, 125, 180, 235};            // the porch's four steps, darkest at the bottom
    const int barW = std::max(1, W / 7);
    auto setPx = [&](int x, int y, Rgb v) {
        float* p = o.at(x, y);
        p[0] = (float)(v.r / 255.0); p[1] = (float)(v.g / 255.0); p[2] = (float)(v.b / 255.0); p[3] = 1.f;
    };
    for (int y = 0; y < H; ++y) {
        const int yb = H - 1 - y;
        const bool rowIn = yb * 10 >= H && yb * 10 <= 9 * H;
        // which of the four steps this row belongs to (counted from the bottom of the porch area)
        int stepRow = 0;
        if (rowIn) stepRow = std::min(3, std::max(0, (int)std::ceil(4.0 * (yb - 0.1 * H) / (0.8 * H)) - 1));
        for (int x = 0; x < W; ++x) {
            const bool colIn = x * 10 >= W && x * 10 <= 9 * W;
            const int bar = std::min(6, x / barW);
            Rgb v = black;
            switch (type) {
            case 0: case 1:
                v = bars[bar];
                if (type == 1 && bar == 0) v = {235, 235, 235};
                break;
            case 2: {
                const double fx = x * 255.0 / W;
                v = {(int)std::floor((y + 1) * 255.0 / H), (int)std::floor(fx), (int)std::floor(255.0 - fx)};
                break;
            }
            case 3:                 // Pluge and porches
                if (rowIn) {
                    if (x * 5 >= W && x * 10 < W * 3) v = {16, 16, 16};
                    else if (x * 10 >= W * 3 && x * 10 < W * 4) v = {25, 25, 25};
                    else if (x * 10 >= W * 4 && x * 2 < W) v = {16, 16, 16};
                    else if (x * 2 >= W && colIn) v = {stepsLvl[stepRow], stepsLvl[stepRow], stepsLvl[stepRow]};
                }
                break;
            case 4:                 // Ramp
                if (rowIn && colIn) {
                    const int g = (int)std::floor(25.0 + (x - 0.1 * W) * 204.0 / (0.8 * W) + 0.5);
                    v = {g, g, g};
                }
                break;
            case 5: case 6: {       // SMPTE bars (NTSC / PAL)
                if (y * 3 < 2 * H) {
                    v = bars[bar];
                    if (type == 6 && bar == 0) v = {235, 235, 235};
                } else if (y * 4 < 3 * H) {
                    v = castle[bar];
                } else {
                    const int xl = barW * 5;
                    if (x >= xl) {
                        const int u = barW / 3;
                        if (x < xl + u) v = {7, 7, 7};
                        else if (x < xl + 2 * u) v = {16, 16, 16};
                        else if (x < xl + 3 * u) v = {25, 25, 25};
                        else v = {16, 16, 16};
                    } else if (type == 6) {
                        v = {16, 16, 16};
                    } else if (x < W / 6) v = {113, 40, 7};                // -I
                    else if (x < W * 2 / 6) v = {235, 235, 235};
                    else if (x < W * 3 / 6) v = {165, 7, 70};              // +Q
                    else v = {16, 16, 16};
                }
                break;
            }
            case 7: case 8:
                if (rowIn && colIn) {
                    const int k = std::min(3, (int)std::floor((x - 0.1 * W) * 4.0 / (0.8 * W)));
                    const int l = stepsLvl[3 - k];
                    v = type == 7 ? Rgb{l, l, l} : Rgb{7, l, l};
                }
                break;
            default:
                break;
            }
            setPx(x, y, v);
        }
    }
}

// ------------------------------------------------------------------ Titles & Text
// Text (VEGAS keeps rich text; plain text uses the Font settings) at Location (anchor of the block, y from the bottom), scaled, with outline, shadow and background. A 48 point font is an eighth of
// the frame height at Scale 1 (VEGAS's exact factor is unknown). VEGAS's Animation list is kept by name in the hidden AnimationName string ("_Bounce", as in its presets and projects) and played as
// Reno's own version: the block moved, scaled, squeezed and faded over the first second or so (Scroll and Scroll Left over ten seconds, Earthquake, Rough Day and Menace throughout);
// Title01 .. Title25 fade in.
const char* const kTextAnimations[] = {"None", "Action Flip", "Bounce", "Coming at You", "Double Flash Glow", "Drop Split", "Dropping Words",
                                       "Earthquake", "Fall Down", "Float and Pop", "Fly In", "Fly in from Right", "Jump", "Menace", "Popup",
                                       "Rolling Glow and Enlarge", "Rough Day", "Scroll", "Scroll Left", "Slide", "Slide Down", "Slide Left",
                                       "Slide Right", "Slide Up", "Speedy", "Twist In"};
const int kTextAnimationCount = (int)(sizeof kTextAnimations / sizeof kTextAnimations[0]);

// The animation's name without VEGAS's leading underscore, spaces for underscores ("_Fly_in_from_Right" -> "fly in from right").
std::string animationKey(const std::string& raw)
{
    std::string s;
    for (char ch : raw) s += ch == '_' ? ' ' : (char)std::tolower((unsigned char)ch);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Where the text block is at time t (seconds from the media start): moved by (dx, dy) pixels, scaled by (sx, sy) about its middle, its opacity.
// 'bx, by, bw, bh': the block at rest (pixels, from the top left), W x H the frame.
struct TextMotion { double dx = 0, dy = 0, sx = 1, sy = 1, opacity = 1; };
TextMotion textMotion(const std::string& anim, double t, int W, int H, double bx, double by, double bw, double bh)
{
    TextMotion m;
    const std::string a = animationKey(anim);
    if (a.empty() || a == "none") return m;
    const double D = 1.0;
    const double u = clampd(t / D, 0, 1);
    const double out = 1 - (1 - u) * (1 - u) * (1 - u);
    auto hash = [](double v) { const double x = std::sin(v * 12.9898) * 43758.5453; return x - std::floor(x); };
    const double frame = std::floor(t * 30.0);
    if (a == "fly in") m.dx = -(bx + bw + 4) * (1 - out);
    else if (a == "fly in from right") m.dx = (W - bx + 4) * (1 - out);
    else if (a == "slide" || a == "slide right") { m.dx = -0.25 * W * (1 - out); m.opacity = u; }
    else if (a == "slide left") { m.dx = 0.25 * W * (1 - out); m.opacity = u; }
    else if (a == "slide up") { m.dy = 0.25 * H * (1 - out); m.opacity = u; }
    else if (a == "slide down") { m.dy = -0.25 * H * (1 - out); m.opacity = u; }
    else if (a == "speedy") { const double f = clampd(t / 0.4, 0, 1); m.dx = -(bx + bw + 4) * (1 - f) * (1 - f); }
    else if (a == "fall down" || a == "drop split" || a == "dropping words") m.dy = -(by + bh + 4) * (1 - u * u) * (u < 1 ? 1 : 0);
    else if (a == "bounce") {
        const double fall = by + bh + 4;
        if (u < 0.4) m.dy = -fall * (1 - (u / 0.4) * (u / 0.4));
        else {
            const double b = (u - 0.4) / 0.6 * 3.0;             // three bounces
            const int k = std::min(2, (int)b);
            const double ph = b - k;
            m.dy = -fall * 0.25 / (1 << (2 * k)) * 4 * ph * (1 - ph);
        }
    } else if (a == "jump") m.dy = -0.15 * H * 4 * u * (1 - u);
    else if (a == "popup") {
        // grows from nothing past its size and back (an overshoot)
        const double k = 1.70158, v = u - 1;
        m.sx = m.sy = std::max(0.0, 1 + v * v * ((k + 1) * v + k));
    } else if (a == "coming at you") { m.sx = m.sy = 0.1 + 0.9 * out; m.opacity = u; }
    else if (a == "twist in" || a == "action flip") { m.sx = (a == "action flip" ? -1 + 2 * out : out); m.opacity = std::min(1.0, 2 * u); }
    else if (a == "float and pop") {
        const double f = clampd(t / 1.5, 0, 1);
        m.dy = 0.1 * H * (1 - f);
        m.opacity = std::min(1.0, 2 * f);
        if (f >= 1) m.sx = m.sy = 1 + 0.15 * std::max(0.0, 1 - (t - 1.5) / 0.25);
    } else if (a == "double flash glow") m.opacity = t < 0.6 ? (std::fmod(t, 0.3) < 0.15 ? 1.0 : 0.25) : 1.0;
    else if (a == "rolling glow and enlarge") { m.sx = m.sy = 0.8 + 0.3 * clampd(t / 2.0, 0, 1); m.opacity = std::min(1.0, 2 * u); }
    else if (a == "earthquake") { m.dx = (hash(frame) - 0.5) * 0.04 * W; m.dy = (hash(frame + 0.37) - 0.5) * 0.04 * H; }
    else if (a == "rough day") { m.dx = (hash(frame) - 0.5) * 0.012 * W; m.dy = (hash(frame + 0.71) - 0.5) * 0.012 * H; }
    else if (a == "menace") m.sx = m.sy = 1 + 0.08 * (0.5 - 0.5 * std::cos(t * kPi));
    else if (a == "scroll") {
        const double f = clampd(t / 10.0, 0, 1);
        m.dy = (H - by) + 4 - f * (H + bh + 8);
    } else if (a == "scroll left") {
        const double f = clampd(t / 10.0, 0, 1);
        m.dx = (W - bx) + 4 - f * (W + bw + 8);
    } else m.opacity = clampd(t / 0.5, 0, 1);                   // VEGAS's numbered titles and anything else: a fade in
    return m;
}

// The text itself is laid out and drawn as VEGAS's TextGen does (vfxtitles.h); this only gathers the settings, plays Reno's own animation over the block and puts the
// premultiplied layer (shadow under it) over the background.
inline uint8_t cutByte(double v) { return (uint8_t)std::max(0.0, std::min(255.0, std::floor(v * 255.0))); }   // VEGAS's ConvertColor: (int)(c * 255)

// The earlier bitmap version, kept for the moving frames: VEGAS moves its text letter by letter (a cascade of delays inside each animation), which Reno does not
// follow, and its guessed whole-block motion was closer to those pictures when drawn this way.
void titlesAndTextMoving(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Color bg = c.color("Background");
    if (bg.a > 0) {
        float p[4];
        put(p, bg);
        for (int k = 0; k < W * H; ++k) std::memcpy(&c.out.px[(size_t)k * 4], p, sizeof p);
    }
    const RichText rt = parseRichText(c.s("Text"));
    if (rt.text.empty() || W <= 0 || H <= 0) return;
    TextRequest req;
    req.text = rt.text;
    req.font = !rt.font.empty() ? rt.font : c.s("FontName");
    if (req.font.empty()) req.font = "Arial";
    const double points = rt.points > 0 ? rt.points : std::max(1.0, c.d("FontSize"));
    const double scale = std::max(0.0, c.d("Scale"));
    req.pixelSize = points * H / 400.0 * scale;
    if (req.pixelSize < c.px(0.5)) return;
    req.pixelSize = std::min(req.pixelSize, (double)H * 4);
    req.bold = rt.bold || c.b("Bold");
    req.italic = rt.italic || c.b("Italic");
    req.underline = rt.underline;
    req.tracking = c.d("Tracking");
    req.lineSpacing = std::max(0.05, c.d("LineSpacing"));
    const int anchor = (int)clampd(c.i("Alignment"), 0, 8);
    req.align = anchor % 3;
    TextMask m;
    if (!rasterizeText(req, m)) return;

    double lx, ly;
    c.xy("Location", lx, ly);
    const double ax = lx * W, ay = (1 - ly) * H;
    const double bx = ax - (anchor % 3) * 0.5 * m.w, by = ay - (anchor / 3) * 0.5 * m.h;
    const int X0 = (int)std::lround(bx), Y0 = (int)std::lround(by);

    std::string anim;
    const int pick = c.i("collection");
    if (pick > 0 && pick < kTextAnimationCount) anim = kTextAnimations[pick];
    else anim = c.s("AnimationName");
    const double secs = c.fps > 0 ? c.time / c.fps : c.time / 30.0;
    const TextMotion mo = textMotion(anim, secs, W, H, X0, Y0, m.w, m.h);

    std::vector<float> cover((size_t)W * H, 0.f);
    const bool still = mo.dx == 0 && mo.dy == 0 && mo.sx == 1 && mo.sy == 1;
    if (still) {
        for (int y = 0; y < m.h; ++y) {
            const int fy = Y0 + y;
            if (fy < 0 || fy >= H) continue;
            for (int x = 0; x < m.w; ++x) {
                const int fx = X0 + x;
                if (fx < 0 || fx >= W) continue;
                cover[(size_t)fy * W + fx] = m.a[(size_t)y * m.w + x] / 255.f;
            }
        }
    } else if (std::fabs(mo.sx) > 1e-4 && std::fabs(mo.sy) > 1e-4) {
        const double cx = X0 + m.w * 0.5 + mo.dx, cy = Y0 + m.h * 0.5 + mo.dy;
        const double hw = std::fabs(mo.sx) * m.w * 0.5 + 1, hh = std::fabs(mo.sy) * m.h * 0.5 + 1;
        const int x0 = std::max(0, (int)std::floor(cx - hw)), x1 = std::min(W - 1, (int)std::ceil(cx + hw));
        const int y0 = std::max(0, (int)std::floor(cy - hh)), y1 = std::min(H - 1, (int)std::ceil(cy + hh));
        auto at = [&](int x, int y) -> float { return (x < 0 || y < 0 || x >= m.w || y >= m.h) ? 0.f : m.a[(size_t)y * m.w + x] / 255.f; };
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double u = (x + 0.5 - cx) / mo.sx + m.w * 0.5 - 0.5, v = (y + 0.5 - cy) / mo.sy + m.h * 0.5 - 0.5;
                const int iu = (int)std::floor(u), iv = (int)std::floor(v);
                const float fu = (float)(u - iu), fv = (float)(v - iv);
                cover[(size_t)y * W + x] = (at(iu, iv) * (1 - fu) + at(iu + 1, iv) * fu) * (1 - fv) + (at(iu, iv + 1) * (1 - fu) + at(iu + 1, iv + 1) * fu) * fv;
            }
    }
    if (mo.opacity < 1) {
        const float op = (float)clampd(mo.opacity, 0, 1);
        for (float& v : cover) v *= op;
    }
    const double ow = std::max(0.0, c.d("OutlineWidth")) * H / 720.0;
    std::vector<float> outline;
    if (ow > c.px(0.25)) {
        outline = cover;
        std::vector<float> tmp((size_t)W * H);
        const int r = (int)std::ceil(ow);
        // a square max filter, across and down, then the corners rounded off by a light blur
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float v = 0;
                for (int k = std::max(0, x - r); k <= std::min(W - 1, x + r); ++k) v = std::max(v, outline[(size_t)y * W + k]);
                tmp[(size_t)y * W + x] = v;
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float v = 0;
                for (int k = std::max(0, y - r); k <= std::min(H - 1, y + r); ++k) v = std::max(v, tmp[(size_t)k * W + x]);
                outline[(size_t)y * W + x] = v;
            }
        gaussianBlurPlane(outline, W, H, ow * 0.25, ow * 0.25);
    }
    if (c.b("ShadowEnable")) {
        const std::vector<float>& shape = outline.empty() ? cover : outline;
        const int dx = (int)std::lround(c.d("ShadowOffsetX") * H), dy = (int)std::lround(c.d("ShadowOffsetY") * H);
        std::vector<float> sh((size_t)W * H, 0.f);
        for (int y = 0; y < H; ++y) {
            const int sy = y - dy;
            if (sy < 0 || sy >= H) continue;
            for (int x = 0; x < W; ++x) {
                const int sx = x - dx;
                if (sx >= 0 && sx < W) sh[(size_t)y * W + x] = shape[(size_t)sy * W + sx];
            }
        }
        const double blur = std::max(0.0, c.d("ShadowBlur")) * H * 0.1;
        gaussianBlurPlane(sh, W, H, blur, blur);
        fillThrough(c.out, sh, c.color("ShadowColor"));
    }
    if (!outline.empty()) fillThrough(c.out, outline, c.color("OutlineColor"));
    fillThrough(c.out, cover, c.color("TextColor"));
}


// What titlesAndText works out before the picture's own passes: the text layer as the glyph rasteriser leaves it (premultiplied bytes), and the passes that follow it
// (node blur, glow, drop shadow, the composite over the background). The CPU applies them in titleApply(); the Vulkan plan (vktext.cpp) does the same on the card.
enum TitleKind { TitleBackground, TitleMoving, TitleLayer };
struct TitlePrep {
    vfxtt::Layer L;
    bool blur = false;
    double blurSx = 0, blurSy = 0;
    bool glow = false, glowInner = false;
    double glowAmount = 0, glowColor[4] = {0, 0, 0, 0};
    bool shadow = false;
    double shadowColor[4] = {0, 0, 0, 0}, shadowBlur = 0, shadowDx = 0, shadowDy = 0;
    double opacity = 1;
};

TitleKind titleBuild(Ctx& c, TitlePrep& job)
{
    const int W = c.frameW, H = c.frameH;       // (= the picture's size; the Vulkan plan has no c.out)
    const RichText rt = parseRichText(c.s("Text"));
    if (rt.text.empty() || W <= 0 || H <= 0) return TitleBackground;
    vfxtt::Style S;
    S.text = rt.text;
    S.font = !rt.font.empty() ? rt.font : c.s("FontName");
    if (S.font.empty()) S.font = "Verdana";
    S.points = rt.points > 0 ? rt.points : std::max(1.0, c.d("FontSize"));
    S.bold = rt.bold || c.b("Bold");
    S.italic = rt.italic || c.b("Italic");
    S.lineAlign = rt.align;
    S.scale = std::max(0.0, c.d("Scale"));
    S.tracking = clampd(c.d("Tracking"), -20, 20);   // canvas units per character, grown by Scale with everything else
    S.lineSpacing = c.d("LineSpacing");
    S.anchor = (int)clampd(c.i("Alignment"), 0, 8);
    c.xy("Location", S.locX, S.locY);
    S.outline = std::max(0.0, c.d("OutlineWidth"));
    const Color tc = c.color("TextColor"), oc = c.color("OutlineColor");
    S.fill[0] = cutByte(tc.r); S.fill[1] = cutByte(tc.g); S.fill[2] = cutByte(tc.b); S.fill[3] = cutByte(tc.a);
    S.stroke[0] = cutByte(oc.r); S.stroke[1] = cutByte(oc.g); S.stroke[2] = cutByte(oc.b); S.stroke[3] = cutByte(oc.a);
    std::shared_ptr<TextFace> face = vfxtt::face(S.font, S.bold, S.italic);
    if (!face) return TitleMoving;                              // (no glyph source installed: the older bitmap path, through the plain text rasteriser)

    // The characters' own formatting (sizes, bold, italic, font changes inside the text) and the animation engine (the animation's data is read from VEGAS's assembly).
    {
        std::vector<vfxtt::CpFmt> fmt;
        bool mixed = false;
        if (!rt.spans.empty()) {
            std::vector<size_t> cpAt(rt.text.size() + 1, 0);
            size_t n = 0;
            for (size_t i = 0; i < rt.text.size(); ++i) {
                if ((((unsigned char)rt.text[i]) & 0xC0) != 0x80) ++n;
                cpAt[i + 1] = n;
            }
            fmt.assign(n, vfxtt::CpFmt());
            for (const RichText::Span& sp : rt.spans) {
                const size_t a = std::min(sp.start, rt.text.size()), b = std::min(sp.start + sp.len, rt.text.size());
                for (size_t k = cpAt[a]; k < cpAt[b] && k < fmt.size(); ++k) {
                    vfxtt::CpFmt& f = fmt[k];
                    f.points = sp.points > 0 ? sp.points : S.points;
                    f.bold = sp.bold;
                    f.italic = sp.italic;
                    f.font = sp.font;
                    if (f.points != S.points || sp.bold != rt.bold || sp.italic != rt.italic || (!sp.font.empty() && sp.font != S.font)) mixed = true;
                }
            }
        }
        const std::string akey = animationKey(c.s("AnimationName"));
        std::shared_ptr<const vfxta::Item> item;
        if (!akey.empty() && akey != "none") item = vfxta::findItem(c.s("AnimationName"));
        if (item && item->usesPath) item.reset();      // (text along a path is not played: the older motion stands in)
        vfxtt::Rich R;
        if (item) S.anchor = 3 + S.anchor % 3;     // (an animation keeps its text in the middle of its row: the picture of the capture; the column is the setting's)
        if ((item || mixed) && vfxtt::placeRich(S, fmt, W, H, R) && R.glyphs.size() <= 3000) {
            vfxta::State st;
            if (item) {
                // the progress over the event (what the animations' own 0..1 range is laid over)
                const double p = c.duration > 0 ? c.time / c.duration : 0.0;
                vfxta::evaluate(*item, R.shape, clampd(p, 0, 1), st, 0);
                std::vector<char> hid = vfxtt::hiddenChars(R, st);
                {
                    bool any = false;
                    for (char h : hid) any = any || h;
                    if (any) {                                        // (a block with undrawn characters sits by the ink of the others)
                        vfxtt::Rich R2;
                        if (vfxtt::placeRich(S, fmt, W, H, R2, &hid) && R2.shape.lines.size() == R.shape.lines.size()) R = R2;
                    }
                }
                if (st.node.trackSet && std::fabs(st.node.track) < 100) {        // an animated tracking changes the layout: lay the text out again with it
                    vfxtt::Style S2 = S;
                    S2.tracking = clampd(S.tracking + st.node.track, -20, 20);
                    vfxtt::Rich R2;
                    if (vfxtt::placeRich(S2, fmt, W, H, R2, &hid) && R2.shape.lines.size() == R.shape.lines.size()) R = R2;
                }
            } else vfxtt::identityState(R.shape, st);
            const vfxtt::AnimOut ao = vfxtt::drawRich(S, R, st, W, H, job.L);
            const vfxta::Sub& ns = st.node;
            if (ns.blurOn && (ns.hblur > 0 || ns.vblur > 0)) {
                job.blur = true;
                job.blurSx = ns.hblur > 0 ? builtin::more::cgSigmaForDiameter(clampd(ns.hblur, 0, 10) * vfxtt::kBlurRef * W) : 0.0;
                job.blurSy = ns.vblur > 0 ? builtin::more::cgSigmaForDiameter(clampd(ns.vblur, 0, 10) * vfxtt::kBlurRef * H) : 0.0;
            }
            if (ns.glowOn && ns.glow > 0) {
                job.glow = true;
                job.glowAmount = ns.glow;
                for (int k = 0; k < 4; ++k) job.glowColor[k] = ns.glowColor[k];
                job.glowInner = ns.glowInner > 0;
            }
            if (c.b("ShadowEnable")) {
                const Color sc = c.color("ShadowColor");
                job.shadow = true;
                job.shadowColor[0] = sc.r; job.shadowColor[1] = sc.g; job.shadowColor[2] = sc.b; job.shadowColor[3] = sc.a;
                job.shadowBlur = c.d("ShadowBlur") / 10.0;
                job.shadowDx = c.d("ShadowOffsetX") / 10.0;
                job.shadowDy = c.d("ShadowOffsetY") / 10.0;
            }
            job.opacity = (double)(float)clampd(ao.opacity, 0, 1);
            return TitleLayer;
        }
    }

    std::string anim;
    const int pick = c.i("collection");
    // (VEGAS's own pictures with the Animation list set to any entry are the plain still text, so the list picks nothing here; only the hidden AnimationName plays.)
    (void)pick;
    anim = c.s("AnimationName");
    const double secs = c.fps > 0 ? c.time / c.fps : c.time / 30.0;
    vfxtt::Move mv;
    TextMotion mo;
    {
        const std::string key = animationKey(anim);
        if (!key.empty() && key != "none") {
            vfxtt::Placed rest;
            if (!vfxtt::place(*face, S, W, H, vfxtt::Move(), rest)) return TitleBackground;
            mo = textMotion(anim, secs, W, H, rest.bx, rest.by, rest.bw, rest.bh);
            mv.dx = mo.dx; mv.dy = mo.dy; mv.sx = mo.sx; mv.sy = mo.sy;
            if (mo.dx != 0 || mo.dy != 0 || mo.sx != 1 || mo.sy != 1 || mo.opacity != 1) return TitleMoving;
        }
    }
    if (!vfxtt::draw(*face, S, W, H, mv, job.L)) return TitleBackground;
    if (c.b("ShadowEnable")) {
        const Color sc = c.color("ShadowColor");
        job.shadow = true;
        job.shadowColor[0] = sc.r; job.shadowColor[1] = sc.g; job.shadowColor[2] = sc.b; job.shadowColor[3] = sc.a;
        job.shadowBlur = c.d("ShadowBlur") / 10.0;
        job.shadowDx = c.d("ShadowOffsetX") / 10.0;
        job.shadowDy = c.d("ShadowOffsetY") / 10.0;
    }
    job.opacity = (double)(float)clampd(mo.opacity, 0, 1);
    return TitleLayer;
}

// The passes after the glyphs, on the CPU: the node blur, the glow, the drop shadow, then the layer over the background in c.out.
void titleApply(Ctx& c, TitlePrep& j)
{
    const int W = c.out.w, H = c.out.h;
    vfxtt::Layer& L = j.L;
    if (j.blur) vfxtt::blurLayer(L, j.blurSx, j.blurSy);
    if (j.glow) vfxtt::glowLayer(L, j.glowAmount, j.glowColor, j.glowInner);
    if (j.shadow) vfxtt::dropShadow(L, j.shadowColor, j.shadowBlur, j.shadowDx, j.shadowDy);
    const float op = (float)j.opacity;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t* s = &L.px[((size_t)y * W + x) * 4];
            if (!s[3]) continue;
            float* d = c.out.at(x, y);
            const float a = s[3] / 255.f * op, ia = 1.f - a;
            d[0] = s[2] / 255.f * op + d[0] * ia;
            d[1] = s[1] / 255.f * op + d[1] * ia;
            d[2] = s[0] / 255.f * op + d[2] * ia;
            d[3] = a + d[3] * ia;
        }
}

void titlesAndText(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Color bg = c.color("Background");
    if (bg.a > 0) {
        float p[4];
        putGen(p, bg);
        for (int k = 0; k < W * H; ++k) std::memcpy(&c.out.px[(size_t)k * 4], p, sizeof p);
    }
    TitlePrep job;
    const TitleKind kind = titleBuild(c, job);
    if (kind == TitleMoving) titlesAndTextMoving(c);
    else if (kind == TitleLayer) titleApply(c, job);
}

// For the Vulkan plan (vktext.cpp): the same preparation; the glyphs stay on the CPU, the layer goes up as a table. False: the CPU draws the whole picture (the moving older path).
bool titlesAndTextGpuImpl(Ctx& c, TitleGpu& g)
{
    TitlePrep job;
    const TitleKind kind = titleBuild(c, job);
    if (kind == TitleMoving) return false;
    const Color bg = c.color("Background");
    g.bg[0] = g.bg[1] = g.bg[2] = g.bg[3] = 0.f;
    if (bg.a > 0) putGen(g.bg, bg);
    g.layer = kind == TitleLayer;
    if (!g.layer) return true;
    g.w = job.L.w; g.h = job.L.h;
    g.bytes.swap(job.L.px);
    g.blur = job.blur; g.blurSx = job.blurSx; g.blurSy = job.blurSy;
    g.glow = job.glow; g.glowInner = job.glowInner; g.glowAmount = job.glowAmount;
    for (int k = 0; k < 4; ++k) { g.glowColor[k] = job.glowColor[k]; g.shadowColor[k] = job.shadowColor[k]; }
    g.shadow = job.shadow; g.shadowBlur = job.shadowBlur; g.shadowDx = job.shadowDx; g.shadowDy = job.shadowDy;
    g.opacity = job.opacity;
    return true;
}

// ------------------------------------------------------------------ Credit Roll
// VEGAS's Credit Roll, ported from its code (sftrans1.dll, notes/vegas_code/Credit_Roll.md; the arithmetic is in vfxcredit.h). Five text styles (title, sub-item, the
// two columns and the leader between them) each with a font, size, colour, alignment and the space before / after a line in frame heights; the lines are laid
// out under one another (a line is as high as its style's font, GDI's ascent + descent times the frame height / 480), and
//  - scrolling: the block starts just below the frame and moves up by (frame height + block height) x progress (backwards: down) - progress is the time over
//    the roll's duration (VEGAS's 10 seconds of generated media);
//  - timed: the lines are split into pages, the page the progress is at is shown at 'TimedY' with the in effect over the first part of its time and the out effect
//    over the last (fade, scroll in from a side, zoom, wipe).
// Only the glyphs differ from VEGAS where it is not Windows: its text engine draws with GDI (done as VEGAS does on Windows, creditgdi.h), Reno's text rasteriser
// stands in elsewhere. The leader between two columns is laid out as VEGAS does (its string repeated enough times to reach across the gap from the middle of
// the gap, clipped to the gap), the zoom resamples with VEGAS's own cubic and smoothing (vfxcredit.h, bit for bit), and the zoom / wipe box is made of the rectangles
// of the text VEGAS's engine reports (the text's cell with 2 pixels round it). A last line without a line end is not a line, as in VEGAS.
struct CreditStyle {
    std::string face = "Arial";
    int size = 20;
    bool bold = false, italic = false;
    float col[4] = {1.f, 1.f, 1.f, 1.f};
    int align = 2;
    double before = 0, after = 0.05;
};
struct CreditCfg {
    std::string text, leader;
    bool timed = false, back = false;
    double dur = 10, left = 0.1, right = 0.9, timedY = 0.3, gap = 0.05;
    int page = 0, inFx = 0, outFx = 0;
    CreditStyle st[5];
    Color bg;
};

CreditCfg creditConfig(const Ctx& c)
{
    CreditCfg g;
    g.text = c.s("Text");
    g.leader = c.s("Leader");
    g.timed = c.i("Mode") != 0;
    g.back = c.b("Backward");
    auto num = [&](const char* name, double lo, double hi, double def) {
        const double v = c.d(name);
        return v == v ? clampd(v, lo, hi) : def;       // (a NaN gets the default)
    };
    g.dur = num("Duration", 0.01, 1e6, 10);
    g.left = num("Left", 0, 1, 0.1);
    g.right = num("Right", 0, 1, 0.9);
    g.gap = num("ColumnGap", 0, 1, 0.05);
    g.timedY = num("TimedY", 0, 1, 0.3);
    g.page = std::max(0, std::min(3, c.i("PageMode")));
    g.inFx = std::max(0, std::min(15, c.i("InEffect")));
    g.outFx = std::max(0, std::min(15, c.i("OutEffect")));
    g.bg = c.color("Background");
    for (int s = 0; s < 5; ++s) {
        const std::string p = std::string("S") + (char)('0' + s);
        CreditStyle& t = g.st[s];
        t.face = c.s((p + "Font").c_str());
        if (t.face.empty()) t.face = "Arial";
        t.size = (int)num((p + "Size").c_str(), 1, 1000, 20);
        t.bold = c.b((p + "Bold").c_str());
        t.italic = c.b((p + "Italic").c_str());
        const Color k = c.color((p + "Color").c_str());
        t.col[0] = (float)k.r; t.col[1] = (float)k.g; t.col[2] = (float)k.b; t.col[3] = (float)k.a;
        t.align = std::max(0, std::min(2, c.i((p + "Align").c_str())));
        t.before = num((p + "Before").c_str(), 0, 4, 0);
        t.after = num((p + "After").c_str(), 0, 4, 0.05);
    }
    return g;
}

// one piece of text as a coverage mask; (ox, oy) is where its cell starts in it
struct CreditRun {
    int w = 0, h = 0, ox = 0, oy = 0;
    std::vector<uint8_t> a;
    double adv = 0;
};

bool creditRun(const std::string& txt, const CreditStyle& s, double H, CreditRun& r)
{
    r = CreditRun();
    if (txt.empty()) return false;
    const double scale = H / 480.0;
    CreditGdiRun g;
    if (creditGdiRun(txt, s.face, s.size, s.bold, s.italic, scale, g)) {
        r.w = g.w; r.h = g.h; r.ox = g.ox; r.oy = g.oy; r.a.swap(g.a); r.adv = g.adv;
        return r.w > 0 && r.h > 0;
    }
    // elsewhere: Reno's text rasteriser at the size GDI would be given (lfHeight -MulDiv(points, 96, 72)) times the frame height / 480
    const int design = std::max(1, (s.size * 96 + 36) / 72);
    TextRequest rq;
    rq.text = txt;
    rq.font = s.face;
    rq.pixelSize = std::min(4000.0, std::max(1.0, design * scale));
    rq.bold = s.bold;
    rq.italic = s.italic;
    rq.align = 1;
    const int pad = (int)std::ceil(std::min(4000.0, std::max(1.0, rq.pixelSize)) * 0.15);       // (the margin the text rasteriser leaves round the line)
    TextMask m;
    if (!rasterizeText(rq, m) || m.w <= 2 * pad || m.h <= 2 * pad) return false;
    r.w = m.w; r.h = m.h; r.ox = r.oy = pad; r.a.swap(m.a);
    r.adv = m.w - 2 * pad;
    return true;
}

// the height of a line of this style (it does not depend on the text)
double creditHeight(const CreditStyle& s, double H)
{
    double h = 0;
    if (creditGdiHeight(s.face, s.size, s.bold, s.italic, H / 480.0, h)) return h;
    CreditRun r;
    if (creditRun("Hg", s, H, r)) return (double)(r.h - 2 * r.oy);
    return std::max(1, (s.size * 96 + 36) / 72) * (H / 480.0) * 1.15;
}

// A pixel rectangle (right and bottom excluded) and Windows' IntersectRect on it: nothing in common gives an empty (all zero) rectangle.
struct CRect {
    int l = 0, t = 0, r = 0, b = 0;
    bool empty() const { return r <= l || b <= t; }
};
CRect cIntersect(const CRect& a, const CRect& c)
{
    CRect o;
    o.l = std::max(a.l, c.l); o.t = std::max(a.t, c.t); o.r = std::min(a.r, c.r); o.b = std::min(a.b, c.b);
    return o.empty() ? CRect() : o;
}

// The Vulkan plan (vktext.cpp) plays the same code with a recorder set: the text blits are noted, not drawn, and the card draws them in the same order.
struct CreditBlit {
    int x0 = 0, y0 = 0, w = 0, h = 0;               // the mask's place in the frame (its first pixel)
    int l = 0, t = 0, r = 0, b = 0;                 // the clip (right and bottom excluded)
    float col[4] = {0, 0, 0, 0};
    std::vector<uint8_t> a;                         // coverage, w x h
};
struct CreditPost {
    int kind = 0;                                   // 0 none, 1 zoom, 2 wipe
    vfxcredit::Box box;
    double t = 0;                                   // zoom: the shrink; wipe: the edge's place
    bool vertical = false, clearFirst = false;
};
struct CreditJob {
    std::vector<CreditBlit> blits;
    CreditPost post;
    float bg[4] = {0, 0, 0, 0};
};
thread_local CreditJob* gCreditJob = nullptr;

// text colour over what is in the frame (straight colour; coverage 0..255 times the colour's alpha); only inside 'clip' (VEGAS copies the text's picture to the
// frame only where its clip rectangle and the frame overlap)
void creditBlit(Buf& out, const CreditRun& r, double x, double y, const float col[4], const CRect& clip)
{
    const int X0 = (int)std::lround(x) - r.ox, Y0 = (int)std::lround(y) - r.oy;
    if (gCreditJob) {
        CreditBlit b;
        b.x0 = X0; b.y0 = Y0; b.w = r.w; b.h = r.h;
        b.l = clip.l; b.t = clip.t; b.r = clip.r; b.b = clip.b;
        for (int k = 0; k < 4; ++k) b.col[k] = col[k];
        b.a = r.a;
        gCreditJob->blits.push_back(std::move(b));
        return;
    }
    for (int yy = 0; yy < r.h; ++yy) {
        const int fy = Y0 + yy;
        if (fy < 0 || fy >= out.h || fy < clip.t || fy >= clip.b) continue;
        for (int xx = 0; xx < r.w; ++xx) {
            const int fx = X0 + xx;
            if (fx < 0 || fx >= out.w || fx < clip.l || fx >= clip.r) continue;
            const float v = r.a[(size_t)yy * r.w + xx] * (1.f / 255.f) * col[3];
            if (v <= 0.f) continue;
            float* d = out.at(fx, fy);
            const float k = 1.f - v;
            d[0] = col[0] * v + d[0] * k;
            d[1] = col[1] * v + d[1] * k;
            d[2] = col[2] * v + d[2] * k;
            d[3] = v + d[3] * k;
        }
    }
}

// The rectangle VEGAS's text engine gives a line of text (0x18024cc30): the text's cell with 2 pixels round it, left at the placed position
CRect creditCell(const CreditRun& r, double x, double top, double cellH)
{
    CRect c;
    c.l = (int)std::lround(x) - 2;
    c.r = c.l + (int)std::lround(r.adv) + 4;
    c.t = (int)std::lround(top) - 2;
    c.b = c.t + (int)std::lround(cellH) + 4;
    return c;
}

// Draws 'r' inside the overlap of its cell, the clip and the frame; 'drawn' is what the text engine reports back (see below). False if nothing is drawn.
bool creditDraw(Buf& out, const CreditRun& r, double x, double top, const CRect& cell, const CRect& clip, const float col[4], CRect& drawn)
{
    const CRect frame = {0, 0, out.w, out.h};
    const CRect dr = cIntersect(cIntersect(cell, clip), frame);
    if (dr.empty()) return false;
    drawn = cIntersect(cell, frame);                    // (what the engine reports is the cell on the frame: the zoom and wipe boxes in the captures start 2 pixels above the text)
    creditBlit(out, r, x, top, col, dr);
    return true;
}

// One piece of text as VEGAS's line functions draw it (0x1801f6850 and the two columns of 0x1801f6af0): the clip is the margins' width and the line's height
// (top cut to a whole number, bottom rounded up); for left-aligned text it starts at the text, for right-aligned text it ends at the text; the text is drawn
// where its cell and the clip overlap; the rectangle the effect gets back (the zoom and wipe boxes are made of them) is the cell on the frame. Returns false if nothing is drawn.
bool creditPiece(Buf& out, const CreditRun& r, double x, double top, double cellH, int align, int clipL, int clipR, const float col[4], CRect& drawn)
{
    const CRect cell = creditCell(r, x, top, cellH);
    CRect clip;
    clip.l = clipL; clip.r = clipR; clip.t = (int)top; clip.b = (int)std::ceil(top + cellH - 1e-8);
    if (align == 0) clip.l = cell.l;
    else if (align == 1) clip.r = cell.r;
    return creditDraw(out, r, x, top, cell, clip, col, drawn);
}

// One line with its cell's top at 'top' (dx: the whole line moved sideways by the scroll-in / out effects, clipL / clipR: the margins as moved by them); the
// rectangles of the text drawn are added to 'box'. h: the height of each of the five styles.
void creditLine(Buf& out, const vfxcredit::Line& ln, const CreditCfg& cfg, float cols[5][4], const double hs[5], double top, double dx, int Lpx, int Rpx, int clipL,
                int clipR, vfxcredit::Box* box)
{
    const double H = out.h;
    const int cen = (Lpx + Rpx) / 2;
    auto xOf = [](const CreditRun& r, int align, double ref) { return align == 0 ? ref : align == 1 ? ref - r.adv : ref - r.adv * 0.5; };
    auto add = [&](const CRect& d) { if (box) box->add(d.l, d.t, d.r, d.b); };
    if (ln.type != 2) {
        const int s = ln.type == 0 ? 0 : 1;
        CreditRun r;
        if (!creditRun(ln.a, cfg.st[s], H, r)) return;
        const int al = cfg.st[s].align;
        const double x = xOf(r, al, al == 0 ? Lpx : al == 1 ? Rpx : cen) + dx;
        CRect d;
        if (creditPiece(out, r, x, top, hs[s], al, clipL, clipR, cols[s], d)) add(d);
        return;
    }
    // two columns: the left one against the left margin / the middle (less half the gap) / the middle of its half, the right one the same way; a leader between them
    const double cellH = std::max(hs[2], std::max(hs[3], hs[4]));
    const double halfGap = cfg.gap * out.w * 0.5;
    const int gapPx = (int)(cfg.gap * 0.5 * out.w);                      // (the clip uses it as a whole number)
    const int clipCen = (clipL + clipR) / 2;
    CreditRun a, b;
    const bool ha = creditRun(ln.a, cfg.st[2], H, a), hb = creditRun(ln.b, cfg.st[4], H, b);
    double xa = 0, xb = 0;
    CRect da, db;
    bool okA = false, okB = false;
    if (ha) {
        const int al = cfg.st[2].align;
        xa = xOf(a, al, al == 0 ? (double)Lpx : al == 1 ? cen - halfGap : (double)((Lpx + cen) / 2) - halfGap * 0.5) + dx;
        okA = creditPiece(out, a, xa, top, cellH, al, clipL, clipR, cols[2], da);
        if (okA) add(da);
    }
    if (hb) {
        const int al = cfg.st[4].align;
        xb = xOf(b, al, al == 0 ? cen + halfGap : al == 1 ? (double)Rpx : (double)((cen + Rpx) / 2) + halfGap * 0.5) + dx;
        // the right column may not reach into the left one: its clip starts a gap after the left text's drawn edge (or half a gap right of the middle)
        const int edge = okA ? da.r : clipR;
        const int c0 = std::min(edge + 2 * gapPx, clipCen + gapPx);
        CRect cell = creditCell(b, xb, top, cellH), clip;
        clip.l = c0; clip.r = clipR; clip.t = (int)top; clip.b = (int)std::ceil(top + cellH - 1e-8);
        if (al == 0) clip.l = cell.l;
        else if (al == 1) clip.r = cell.r;
        CRect x = cIntersect(clip, cell);
        x.r = std::max(x.r, edge);
        if (creditDraw(out, b, xb, top, cell, x, cols[4], db)) { okB = true; add(db); }
    }
    if (ha && hb && !cfg.leader.empty() && okA && okB) {
        // the leader: its string, as many times as it takes to reach across the gap from where it is placed (the middle of the gap between the two texts), is laid
        // out as one line and drawn inside the gap (0x18024d900 with its repeat count)
        CreditRun u;
        CRect gap;
        gap.l = da.r; gap.r = db.l; gap.t = (int)top; gap.b = (int)std::ceil(top + cellH - 1e-8);
        const CRect col = {clipL, (int)top, clipR, gap.b};
        gap = cIntersect(gap, col);
        if (!gap.empty() && creditRun(cfg.leader, cfg.st[3], H, u) && u.adv > 0.5) {
            const int al = cfg.st[3].align;
            const double mid = (gap.l + gap.r) * 0.5;
            // the width of one repeat (on Windows GDI's own whole number of pixels; the other text rasteriser rounds a single string up, so eight are measured)
            CreditRun u8;
            std::string eight;
            for (int k = 0; k < 8; ++k) eight += cfg.leader;
            const double one = creditRun(eight, cfg.st[3], H, u8) ? u8.adv / 8.0 : u.adv;
            const int w1 = std::max(1, (int)std::lround(one));
            double ax = mid;                                              // where the one-string layout sits: left edge, centre or right edge at the middle
            const double left1 = al == 0 ? ax : al == 1 ? ax - one : ax - one * 0.5;
            const int c0 = (int)std::lround(left1), c1 = c0 + w1;        // the content of the one-string layout
            int n;
            if (al == 0) n = std::max(0, gap.r - c0) / w1 + 1;
            else if (al == 1) n = std::max(0, c1 - gap.l) / w1 + 1;
            else {
                const double cc = c0 + w1 * 0.5;
                const int rr = std::max(0, gap.r - (int)std::floor(cc + 1.5e-8)), ll = std::max(0, (int)std::ceil(cc - 1.5e-8) - gap.l);
                n = 2 * std::max(rr / w1, ll / w1) + 2;
            }
            n = std::max(1, std::min(n, 2000));
            std::string many;
            for (int k = 0; k < n; ++k) many += cfg.leader;
            CreditRun m;
            if (creditRun(many, cfg.st[3], H, m)) {
                const double x = xOf(m, al, mid);
                const CRect cell = creditCell(m, x, top, cellH);
                CRect clip = cIntersect(gap, cell), dl;
                if (creditDraw(out, m, x, top, cell, clip, cols[3], dl)) add(dl);
            }
        }
    }
}

// The whole effect. 'out' is the picture (c.out); with a job it is only a size (w, h) and the blits and the last pass are recorded in the job instead of drawn.
void creditPlay(Ctx& c, Buf& out, CreditJob* job)
{
    const int W = out.w, H = out.h;
    if (W <= 0 || H <= 0) return;
    const CreditCfg cfg = creditConfig(c);
    float bgp[4];
    putGen(bgp, cfg.bg);                                // (VEGAS fills with the premultiplied 8-bit colour)
    if (job) std::memcpy(job->bg, bgp, sizeof bgp);
    else for (int k = 0; k < W * H; ++k) std::memcpy(&out.px[(size_t)k * 4], bgp, sizeof bgp);
    const std::vector<vfxcredit::Line> lines = vfxcredit::parse(cfg.text);
    if (lines.empty()) return;
    double h[5], before[3], after[3];
    for (int s = 0; s < 5; ++s) h[s] = creditHeight(cfg.st[s], H);
    for (int s = 0; s < 3; ++s) { before[s] = cfg.st[s].before; after[s] = cfg.st[s].after; }
    const double secs = c.fps > 0 ? c.time / c.fps : c.time / 30.0;
    const double p = clampd(secs / std::max(0.01, cfg.dur), 0, 1);
    const int Lpx = (int)(W * cfg.left + 0.5), Rpx = (int)(W * cfg.right + 0.5);
    float base[5][4];
    for (int s = 0; s < 5; ++s) std::memcpy(base[s], cfg.st[s].col, sizeof base[s]);
    std::vector<vfxcredit::Placed> pl;

    if (!cfg.timed) {
        const double total = vfxcredit::layout(lines, nullptr, 0, h, before, after, H, (double)H, false, pl);
        const double off = total * (cfg.back ? 1.0 - p : p);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!(pl[i].y0 < off + H && pl[i].y1 > off)) continue;
            creditLine(out, lines[i], cfg, base, h, pl[i].y0 - off, 0, Lpx, Rpx, Lpx, Rpx, nullptr);
        }
        return;
    }

    std::vector<int> pg;
    const int N = vfxcredit::pagesOf(lines, cfg.page, pg);
    const double xx = N * p;
    const int cur = (int)xx;
    const double frac = xx - cur;
    if (cur >= N) return;
    if (cfg.page == 2) vfxcredit::fixTitle(lines, pg, cur);
    const double pageEnd = vfxcredit::layout(lines, &pg, cur, h, before, after, H, 0.0, true, pl);
    int yOff = (int)(H * cfg.timedY + 0.5);
    const int pageH = (int)pageEnd;
    const int id = frac <= 0.5 ? cfg.inFx : cfg.outFx;
    const int kind = vfxcredit::fxKind(id);
    double t = 0;
    bool in = true;
    const bool act = kind != vfxcredit::FxNone && vfxcredit::phase(vfxcredit::fxDuration(id), frac, t, in);
    const bool post = vfxcredit::fxIsPost(id);
    double dx = 0;
    int clipShift = 0;                                  // the margins move by a whole number of pixels with the scroll effects, the text by the exact amount
    float cols[5][4];
    std::memcpy(cols, base, sizeof cols);
    if (act && !post) {
        const double s = (kind == vfxcredit::FxFade) ? (in ? t : 1.0 - t) : (in ? 1.0 - t : t);
        if (kind == vfxcredit::FxFade) {
            const float bgs[4] = {(float)cfg.bg.r, (float)cfg.bg.g, (float)cfg.bg.b, (float)cfg.bg.a};
            for (int k = 0; k < 5; ++k) vfxcredit::mixColour(bgs, base[k], s, cols[k]);
        } else if (kind == vfxcredit::FxLeft) { dx = -(double)W * s; clipShift = -(int)(W * s); }
        else if (kind == vfxcredit::FxRight) { dx = (double)W * s; clipShift = (int)(W * s); }
        else if (kind == vfxcredit::FxUp) yOff -= (int)((pageH + yOff) * s);
        else if (kind == vfxcredit::FxDown) yOff += (int)((H - yOff) * s);
    }
    vfxcredit::Box box;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (pg[i] != cur) continue;
        creditLine(out, lines[i], cfg, cols, h, (double)yOff + pl[i].y0, dx, Lpx, Rpx, Lpx + clipShift, Rpx + clipShift, &box);
    }
    box.clip(W, H);                                     // (the engine only reports what it drew on the frame)
    if (act && post && !box.empty()) {
        if (job) {
            CreditPost& cp = job->post;
            cp.box = box;
            if (kind == vfxcredit::FxZoom) { cp.kind = 1; cp.t = in ? 1.0 - t : t; }
            else if (kind == vfxcredit::FxWipeHR) { cp.kind = 2; cp.vertical = false; cp.clearFirst = in; cp.t = 1.0 - t; }
            else if (kind == vfxcredit::FxWipeH) { cp.kind = 2; cp.vertical = false; cp.clearFirst = !in; cp.t = t; }
            else if (kind == vfxcredit::FxWipeVR) { cp.kind = 2; cp.vertical = true; cp.clearFirst = in; cp.t = 1.0 - t; }
            else if (kind == vfxcredit::FxWipeV) { cp.kind = 2; cp.vertical = true; cp.clearFirst = !in; cp.t = t; }
            return;
        }
        float* px = out.px.data();
        if (kind == vfxcredit::FxZoom) vfxcredit::zoomBox(px, W, H, box, in ? 1.0 - t : t, bgp);
        else if (kind == vfxcredit::FxWipeHR) vfxcredit::wipe(px, W, H, box, false, in, 1.0 - t, bgp);
        else if (kind == vfxcredit::FxWipeH) vfxcredit::wipe(px, W, H, box, false, !in, t, bgp);
        else if (kind == vfxcredit::FxWipeVR) vfxcredit::wipe(px, W, H, box, true, in, 1.0 - t, bgp);
        else if (kind == vfxcredit::FxWipeV) vfxcredit::wipe(px, W, H, box, true, !in, t, bgp);
    }
}

void creditRoll(Ctx& c) { creditPlay(c, c.out, nullptr); }

// For the Vulkan plan: the text blits in the order the CPU draws them, and the last pass (zoom / wipe), recorded by playing the effect without a picture.
bool creditRollGpuImpl(Ctx& c, CreditGpu& g)
{
    Buf size;
    size.w = c.frameW; size.h = c.frameH;
    if (size.w <= 0 || size.h <= 0) return false;
    CreditJob job;
    gCreditJob = &job;
    try { creditPlay(c, size, &job); } catch (...) { gCreditJob = nullptr; throw; }
    gCreditJob = nullptr;
    for (int k = 0; k < 4; ++k) g.bg[k] = job.bg[k];
    for (CreditBlit& b : job.blits) {
        CreditGpu::Blit o;
        o.x0 = b.x0; o.y0 = b.y0; o.w = b.w; o.h = b.h; o.l = b.l; o.t = b.t; o.r = b.r; o.b = b.b;
        for (int k = 0; k < 4; ++k) o.col[k] = b.col[k];
        o.a.swap(b.a);
        g.blits.push_back(std::move(o));
    }
    g.postKind = job.post.kind;
    g.bx0 = job.post.box.x0; g.by0 = job.post.box.y0; g.bx1 = job.post.box.x1; g.by1 = job.post.box.y1;
    g.t = job.post.t; g.vertical = job.post.vertical; g.clearFirst = job.post.clearFirst;
    return true;
}

}  // namespace

bool titlesAndTextGpu(Ctx& c, TitleGpu& g) { return titlesAndTextGpuImpl(c, g); }
bool creditRollGpu(Ctx& c, CreditGpu& g) { return creditRollGpuImpl(c, g); }
// Color Gradient's numbers for the Vulkan plan (vkfx2.cpp): the shape and the stops (position, r, g, b, a each) as doubles.
void colorGradientGpu(Ctx& c, ColorGradientGpu& o)
{
    GradSetup g;
    gradSetup(c, g);
    o.type = g.type;
    o.stops.clear();
    for (const GradStop& s : g.stops) {
        o.stops.push_back(s.pos);
        o.stops.push_back(s.col.r); o.stops.push_back(s.col.g); o.stops.push_back(s.col.b); o.stops.push_back(s.col.a);
    }
    o.v[0] = g.sx; o.v[1] = g.sy; o.v[2] = g.ddx; o.v[3] = g.ddy; o.v[4] = g.dd; o.v[5] = g.cx; o.v[6] = g.cy; o.v[7] = g.rx; o.v[8] = g.ry; o.v[9] = g.ca; o.v[10] = g.sa;
}

void addGenerators(std::vector<Def>& out)
{
    {
        Def d;
        d.id = "com.vegascreativesoftware:solidcolor";
        d.label = "Solid Color";
        d.group = kGroup;
        d.generator = true;
        d.description = "A frame of one colour (Reno's own, standing in for VEGAS Solid Color).";
        d.params = {rgba("Color", "Color", 0, 0, 0, 1)};
        d.render = solidColor;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:colorgradient";
        d.label = "Color Gradient";
        d.group = kGroup;
        d.generator = true;
        d.description = "A blend between colour stops along a line, an ellipse or a rectangle (Reno's own, standing in for VEGAS Color Gradient).";
        d.params = {choice("GradientType", "Gradient type", 0, {"Linear", "Elliptical", "Rectangular"}),
                    rgba("Color1", "Color 1", 1, 1, 1, 1), rgba("Color2", "Color 2", 0, 0, 0, 1),
                    text("GradientStops", "Gradient stops (position r g b a; ...)", "", false),
                    point("Start", "Start (linear)", 0.0, 0.5), point("End", "End (linear)", 1.0, 0.5),
                    point("Center", "Center", 0.5, 0.5), point("Radius", "Radius (elliptical, rectangular)", 0.5, 0.5),
                    dbl("AspectRatioAngle", "Aspect ratio angle", 0, -360, 360)};
        d.render = colorGradient;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:checkerboard";
        d.label = "Checkerboard";
        d.group = kGroup;
        d.generator = true;
        d.description = "Tiles of two colours (Reno's own, standing in for VEGAS Checkerboard).";
        d.params = {group("TileDimensions", "Tile dimensions"),
                    dbl("TileDimensionsWidth", "Width", 0.1, 0.001, 1, "TileDimensions"),
                    dbl("TileDimensionsHeight", "Height", 0.1, 0.001, 1, "TileDimensions"),
                    boolean("TileDimensionsSquare", "Square", true, "TileDimensions"),
                    group("EdgeBlending", "Edge blending"),
                    dbl("EdgeBlendingHorizontal", "Horizontal", 0, 0, 1, "EdgeBlending"),
                    dbl("EdgeBlendingVertical", "Vertical", 0, 0, 1, "EdgeBlending"),
                    boolean("EdgeBlendingProportional", "Proportional", true, "EdgeBlending"),
                    rgba("Color1", "Color 1", 0, 0, 0, 1), rgba("Color2", "Color 2", 1, 1, 1, 1),
                    group("GridOffset", "Grid offset"),
                    dbl("GridOffsetX", "X", 0, -1, 1, "GridOffset"), dbl("GridOffsetY", "Y", 0, -1, 1, "GridOffset")};
        d.render = checkerboard;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:noisetexture";
        d.label = "Noise Texture";
        d.group = kGroup;
        d.generator = true;
        d.description = "Clouds, marble, wood and other textures made of noise (Reno's own, standing in for VEGAS Noise Texture).";
        d.params = {choice("NoiseStyle", "Noise style", 0, {"Fractal - smooth", "Fractal - turbulent", "Marble - light grain", "Marble - heavy grain",
                                                          "Jagged lines", "Contours", "Rings"}),
                    dbl("NumberOfLayers", "Number of layers", 1, 1, 10), boolean("Swapcolors", "Swap colors", false),
                    rgba("Color1", "Color 1", 0, 0, 0, 1), rgba("Color2", "Color 2", 1, 1, 1, 1),
                    group("Frequency", "Frequency"),
                    dbl("FrequencyX", "X", 10, 0, 100, "Frequency"), dbl("FrequencyY", "Y", 10, 0, 100, "Frequency"),
                    group("Offset", "Offset"),
                    dbl("OffsetX", "X", 0, -100, 100, "Offset"), dbl("OffsetY", "Y", 0, -100, 100, "Offset"),
                    dbl("OffsetprogressinDegrees", "Progress (in degrees)", 0, -3600, 3600, "Offset"),
                    group("NoiseParameters", "Noise parameters"),
                    dbl("NoiseParametersMin", "Min", 0, 0, 1, "NoiseParameters"), dbl("NoiseParametersMax", "Max", 1, 0, 1, "NoiseParameters"),
                    dbl("NoiseParametersBias", "Bias", 0, -1, 1, "NoiseParameters"),
                    dbl("NoiseParametersAmplitude", "Amplitude", 1, 0, 10, "NoiseParameters"),
                    dbl("NoiseParametersGrainAmplitudeFalloff", "Grain (amplitude falloff)", 0.5, 0, 1, "NoiseParameters")};
        d.params[6].dmax = d.params[7].dmax = 40;
        d.params[11].dmin = 0; d.params[11].dmax = 360;
        d.render = noiseTexture;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:testpattern";
        d.label = "Test Pattern";
        d.group = kGroup;
        d.generator = true;
        d.description = "Colour bars and other test patterns (Reno's own, standing in for VEGAS Test Pattern).";
        d.params = {choice("TestPatternType", "Test pattern type", 0, {"Color bars (NTSC)", "Color bars (PAL)", "Gradient", "Pluge and porches", "Ramp",
                                                                      "SMPTE bars (NTSC)", "SMPTE bars (PAL)", "White porches", "Yellow porches"})};
        d.render = testPattern;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:creditroll";
        d.label = "Credit Roll";
        d.group = kGroup;
        d.generator = true;
        d.description = "Credits for a title roll (VEGAS's Credit Roll): scrolling up or down, or timed pages with in / out effects (fade, scroll, zoom, wipe). A line starting with a space is a title, a tab splits two columns, quotes keep spaces, ~ is a quote.";
        PDef t = text("Text", "Credits text", vfxcredit::defaultText(), true);
        t.hint = "A line starting with a space is a title; a tab splits two columns; a line in quotes keeps its spaces (a ~ inside is a quote)";
        const std::vector<std::string> fx = {"None", "Fade", "Fade (short)", "From / to the left", "From / to the right", "From / to the top", "From / to the bottom",
                                              "From / to the left (slow)", "From / to the right (slow)", "From / to the top (slow)", "From / to the bottom (slow)",
                                              "Zoom", "Wipe right to left", "Wipe left to right", "Wipe bottom to top", "Wipe top to bottom"};
        d.params = {t, choice("Mode", "Mode", 0, {"Scrolling", "Timed"}), boolean("Backward", "Scroll backward (down)", false),
                    dbl("Duration", "Duration (seconds)", 10, 0.01, 3600), dbl("Left", "Left margin", 0.1, 0, 1), dbl("Right", "Right margin", 0.9, 0, 1),
                    dbl("ColumnGap", "Gap between columns", 0.05, 0, 1), dbl("TimedY", "Timed: top of the text (frame heights)", 0.3, 0, 1),
                    choice("PageMode", "Timed: pages", 0, {"A page for each title", "A page for each line", "A title with each of its items", "A page for each title or sub-item"}),
                    choice("InEffect", "Timed: in effect", 0, fx), choice("OutEffect", "Timed: out effect", 0, fx), text("Leader", "Leader between columns", "", false),
                    rgba("Background", "Background color", 0, 0, 0, 1)};
        {
            int n = 0;
            const vfxcredit::Preset* first = vfxcredit::presets(n);
            static const char* const names[5] = {"Title", "Sub-item", "Left column", "Leader", "Right column"};
            for (int s = 0; s < 5; ++s) {
                const std::string p = std::string("S") + (char)('0' + s), nm = names[s];
                const vfxcredit::StyleRec& r = first[0].st[s];
                d.params.push_back(text(p + "Font", nm + ": font", r.face, false));
                d.params.push_back(dbl(p + "Size", nm + ": size (points)", r.size, 1, 1000));
                d.params.push_back(boolean(p + "Bold", nm + ": bold", r.bold != 0));
                d.params.push_back(boolean(p + "Italic", nm + ": italic", r.italic != 0));
                d.params.push_back(rgba(p + "Color", nm + ": color", r.col[0], r.col[1], r.col[2], r.col[3]));
                d.params.push_back(choice(p + "Align", nm + ": alignment", r.align, {"Left", "Right", "Center"}));
                d.params.push_back(dbl(p + "Before", nm + ": space before (frame heights)", r.before, 0, 4));
                d.params.push_back(dbl(p + "After", nm + ": space after (frame heights)", r.after, 0, 4));
            }
        }
        d.render = creditRoll;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:titlesandtext";
        d.label = "Titles & Text";
        d.group = kGroup;
        d.generator = true;
        d.description = "Text over the picture (Reno's own, standing in for VEGAS Titles & Text; its animations are Reno's versions of VEGAS's).";
        PDef t = text("Text", "Text", "Sample Text", true);
        t.hint = "Plain text, or VEGAS's rich text (its font, size, bold and italic are then used)";
        PDef loc = point("Location", "Location", 0.5, 0.5);
        d.params = {t, dbl("Scale", "Scale", 1, 0, 10), loc, rgba("TextColor", "Text color", 1, 1, 1, 1),
                    choice("Alignment", "Anchor Point", 4, {"Top Left", "Top Center", "Top Right", "Center Left", "Center", "Center Right",
                                                            "Bottom Left", "Bottom Center", "Bottom Right"}),
                    group("FontGroup", "Font"),
                    text("FontName", "Font", "Verdana", false), dbl("FontSize", "Size (points)", 48, 1, 1000), boolean("Bold", "Bold", false),
                    boolean("Italic", "Italic", false),
                    group("AdvancedGroup", "Advanced"),
                    rgba("Background", "Background", 0, 0, 0, 0, "AdvancedGroup"), dbl("Tracking", "Tracking", 0, -20, 20, "AdvancedGroup"),
                    dbl("LineSpacing", "Line spacing", 1, 0.1, 5, "AdvancedGroup"),
                    group("OutlineGroup", "Outline"),
                    dbl("OutlineWidth", "Outline width", 0, 0, 50, "OutlineGroup"), rgba("OutlineColor", "Outline color", 1, 1, 1, 1, "OutlineGroup"),
                    group("ShadowGroup", "Shadow"),
                    boolean("ShadowEnable", "Shadow enable", false, "ShadowGroup"), rgba("ShadowColor", "Shadow color", 0, 0, 0, 1, "ShadowGroup"),
                    dbl("ShadowOffsetX", "Shadow offset X", 0.2, -1, 1, "ShadowGroup"), dbl("ShadowOffsetY", "Shadow offset Y", 0.2, -1, 1, "ShadowGroup"),
                    dbl("ShadowBlur", "Shadow blur", 0.4, 0, 2, "ShadowGroup")};
        // VEGAS's Animation list, and the hidden name it keeps the animation by
        {
            std::vector<std::string> names(kTextAnimations, kTextAnimations + kTextAnimationCount);
            PDef an = choice("collection", "Animation", 0, names);
            an.hint = "How the text comes in. None plays the animation a VEGAS preset or project named, if any";
            PDef keep = text("AnimationName", "Animation name", "", false);
            keep.secret = true;
            d.params.insert(d.params.begin() + 5, an);
            d.params.push_back(keep);
        }
        for (const char* n : {"FontName", "FontSize", "Bold", "Italic"})
            for (auto& p : d.params) if (p.name == n) p.parent = "FontGroup";
        for (auto& p : d.params) {
            if (p.name == "Scale") p.dmax = 4;
            if (p.name == "FontSize") p.dmax = 200;
            if (p.name == "OutlineWidth") p.dmax = 20;
            if (p.name == "ShadowOffsetX" || p.name == "ShadowOffsetY") { p.dmin = -0.2; p.dmax = 0.2; }
        }
        d.render = titlesAndText;
        out.push_back(d);
    }
}


// ---- for the GPU (vkeffects.cpp): the numbers the CPU code above works out in double precision, so the kernels only mix them.
// Solid Color's premultiplied levels.
void solidColorGpu(Ctx& c, float p[4])
{
    if (c.openCl) {         // (the OpenCL kernel's colour, through the card's store into an 8-bit frame: a constant, so the host works it out)
        const Color col = c.color("Color");
        const double rgba[4] = {col.r, col.g, col.b, col.a};
        float v[4];
        vfxgen::solidColor(rgba, v);
        genPut(c, p, v);
        return;
    }
    putGen(p, c.color("Color"));
}

// Noise Texture as VEGAS's OpenCL kernels draw it (noiseTextureOpenCl): the arguments of the row kernel and the mixing kernel (false: nothing to draw on the card)
bool noiseTextureOclGpu(Ctx& c, int W, int H, NoiseGpu& o, std::vector<float>& tab)
{
    if (!c.openCl || W <= 0 || H <= 0) return false;
    const Color c1 = c.color("Color1"), c2 = c.color("Color2");
    const double col1[4] = {c1.r, c1.g, c1.b, c1.a}, col2[4] = {c2.r, c2.g, c2.b, c2.a};
    const vfxgen::Noise k = vfxgen::noiseHost(W, H, c.i("NoiseStyle"), c.b("Swapcolors"), c.d("FrequencyX"), c.d("FrequencyY"), c.d("NumberOfLayers"), c.d("OffsetX"),
                                              c.d("OffsetY"), c.d("OffsetprogressinDegrees"), c.d("NoiseParametersGrainAmplitudeFalloff"), c.d("NoiseParametersMin"),
                                              c.d("NoiseParametersMax"), c.d("NoiseParametersBias"), c.d("NoiseParametersAmplitude"), col1, col2);
    const vfxgen::NoiseLayers L = vfxgen::noiseLayers(k);
    o.style = k.style;
    o.fold = vfxgen::noiseFolds(k.style);
    o.count = L.count;
    for (int i = 0; i < 6; ++i) o.weight[i] = L.weight[i];
    o.sum = L.sum;
    for (int i = 0; i < 2; ++i) { o.freq[i] = k.freq[i]; o.off[i] = k.off[i]; }
    o.z = k.z;
    o.lo = k.lo; o.hi = k.hi; o.bias = k.bias; o.amp = k.amp;
    for (int i = 0; i < 5; ++i) o.t[i] = vfxgen::noiseStyleTable()[k.style][i];
    const float* ca = k.swap ? k.c2 : k.c1;
    const float* cb = k.swap ? k.c1 : k.c2;
    for (int i = 0; i < 4; ++i) { o.a[i] = ca[i]; o.b[i] = cb[i]; }
    tab.clear();
    for (int i = 0; i < 256; ++i) tab.push_back((float)kNtPerm[i]);
    for (int i = 0; i < 256; ++i) for (int j = 0; j < 3; ++j) tab.push_back(kNtGrad[i][j]);
    return true;
}

// Checkerboard as VEGAS's OpenCL kernel draws it (checkerboardOpenCl): what the kernel gets, tile (2), half edge (2), 0.5 / half edge (2), offset (2), Color 2 (4), Color 1 (4)
void checkerboardOclGpu(Ctx& c, int W, int H, float out[16])
{
    const Color a = c.color("Color1"), b = c.color("Color2");
    const double col1[4] = {a.r, a.g, a.b, a.a}, col2[4] = {b.r, b.g, b.b, b.a};
    const double eh = c.d("EdgeBlendingHorizontal");
    const double ev = c.b("EdgeBlendingProportional") ? eh : c.d("EdgeBlendingVertical");
    const vfxgen::Checker k = vfxgen::checkerHost(W, H, c.d("TileDimensionsWidth"), c.d("TileDimensionsHeight"), c.b("TileDimensionsSquare"), eh, ev, c.d("GridOffsetX"),
                                                  c.d("GridOffsetY"), col1, col2, 1.0f);
    for (int i = 0; i < 2; ++i) { out[i] = k.tile[i]; out[2 + i] = k.hh[i]; out[4 + i] = k.inv[i]; out[6 + i] = k.off[i]; }
    for (int i = 0; i < 4; ++i) { out[8 + i] = k.c2[i]; out[12 + i] = k.c1[i]; }
}

// Checkerboard: the horizontal wave of each column, then the vertical wave of each row (as checkerboard() works them out), and the two premultiplied colours.
void checkerboardGpu(Ctx& c, int W, int H, std::vector<float>& tab, float pa[4], float pb[4])
{
    const double tw = std::max(1.0 / std::max(1, W), c.d("TileDimensionsWidth"));
    const bool square = c.b("TileDimensionsSquare");
    const double th = std::max(1.0 / std::max(1, H), square ? tw * W / std::max(1, H) : c.d("TileDimensionsHeight"));
    const double bh = clampd(c.d("EdgeBlendingHorizontal"), 0, 1);
    const double bv = c.b("EdgeBlendingProportional") ? bh : clampd(c.d("EdgeBlendingVertical"), 0, 1);
    const Color a = c.color("Color1"), b = c.color("Color2");
    const double ox = c.d("GridOffsetX"), oy = c.d("GridOffsetY");
    tab.assign((size_t)W + (size_t)H, 0.f);
    for (int x = 0; x < W; ++x) {
        const double u = (((x + 0.5) / W) - ox) / tw;
        tab[(size_t)x] = (float)(bh <= 1e-6 ? oddCover(((double)x / W - ox) / tw, ((double)(x + 1) / W - ox) / tw) : tileWave(u, bh));
    }
    for (int y = 0; y < H; ++y) {
        const double v = (((y + 0.5) / H) - oy) / th;
        tab[(size_t)W + (size_t)y] = (float)(bv <= 1e-6 ? oddCover(((double)y / H - oy) / th, ((double)(y + 1) / H - oy) / th) : tileWave(v, bv));
    }
    const double da[4] = {a.r * a.a, a.g * a.a, a.b * a.a, a.a}, db[4] = {b.r * b.a, b.g * b.a, b.b * b.a, b.a};
    for (int k = 0; k < 4; ++k) { pa[k] = (float)da[k]; pb[k] = (float)db[k]; }
}

// Checkerboard in doubles: the horizontal wave of every column and the vertical wave of every row, and the premultiplied colours (as checkerboard() works them out).
void checkerboardGpuD(Ctx& c, int W, int H, std::vector<double>& sx, std::vector<double>& sy, double pa[4], double pb[4])
{
    const double tw = std::max(1.0 / std::max(1, W), c.d("TileDimensionsWidth"));
    const bool square = c.b("TileDimensionsSquare");
    const double th = std::max(1.0 / std::max(1, H), square ? tw * W / std::max(1, H) : c.d("TileDimensionsHeight"));
    const double bh = clampd(c.d("EdgeBlendingHorizontal"), 0, 1);
    const double bv = c.b("EdgeBlendingProportional") ? bh : clampd(c.d("EdgeBlendingVertical"), 0, 1);
    const Color a = c.color("Color1"), b = c.color("Color2");
    const double ox = c.d("GridOffsetX"), oy = c.d("GridOffsetY");
    sx.assign((size_t)W, 0.0);
    sy.assign((size_t)H, 0.0);
    for (int x = 0; x < W; ++x) {
        const double u = (((x + 0.5) / W) - ox) / tw;
        sx[(size_t)x] = bh <= 1e-6 ? oddCover(((double)x / W - ox) / tw, ((double)(x + 1) / W - ox) / tw) : tileWave(u, bh);
    }
    for (int y = 0; y < H; ++y) {
        const double v = (((y + 0.5) / H) - oy) / th;
        sy[(size_t)y] = bv <= 1e-6 ? oddCover(((double)y / H - oy) / th, ((double)(y + 1) / H - oy) / th) : tileWave(v, bv);
    }
    const double da[4] = {a.r * a.a, a.g * a.a, a.b * a.a, a.a}, db[4] = {b.r * b.a, b.g * b.a, b.b * b.a, b.a};
    for (int k = 0; k < 4; ++k) { pa[k] = da[k]; pb[k] = db[k]; }
}

// Checkerboard on 8-bit pictures (checkerboardFx8 above): what its tile walk gives each column (parity of the tile, the blend weight or -1) and each row (parity
// of the tile row, the two colours A and B), as ints for the kernel; false where checkerboardFx8 gives up (the float picture is made then).
bool checkerboard8Gpu(Ctx& c, int W, int H, std::vector<int>& t)
{
    const Color ca = c.color("Color1"), cb = c.color("Color2");
    auto toByte = [](double col, double al) { return (int)((float)(col * al) * 255.0f); };
    int P1[4] = {toByte(ca.r, ca.a), toByte(ca.g, ca.a), toByte(ca.b, ca.a), (int)((float)ca.a * 255.0f)};
    int P2[4] = {toByte(cb.r, cb.a), toByte(cb.g, cb.a), toByte(cb.b, cb.a), (int)((float)cb.a * 255.0f)};
    float tw = (float)c.d("TileDimensionsWidth") * (float)W;
    float th = (float)c.d("TileDimensionsHeight") * (float)H;
    if (c.b("TileDimensionsSquare")) th = tw * 1.0f;
    const float inv = 1.0f / 1.0f;
    if (inv > tw) { th = th * inv / tw; tw = inv; }
    if (1.0f > th) { tw = tw / th; th = 1.0f; }
    const float hx = std::max(0.5f, (float)c.d("EdgeBlendingHorizontal") * tw * 0.5f);
    const double bvd = c.b("EdgeBlendingProportional") ? c.d("EdgeBlendingHorizontal") : c.d("EdgeBlendingVertical");
    const float hy = std::max(0.5f, (float)bvd * th * 0.5f);
    const float kx = 32768.0f / hx, ky = 32768.0f / hy;
    const float ox = (float)c.d("GridOffsetX") * (float)W, oy = (float)c.d("GridOffsetY") * (float)H;
    const float lim = 1e6f;
    if (!(std::fabs(tw) < lim && std::fabs(th) < lim && tw >= 1.0f && th >= 1.0f && std::fabs(ox) < lim && std::fabs(oy) < lim && hx < lim && hy < lim)) return false;
    t.clear();
    {
        float x = 0.5f - ox;
        int ix = (int)(x / tw);
        if ((float)ix * tw > x) --ix;
        float xl = (float)ix * tw + hx, xr = (float)(ix + 1) * tw - hx;
        for (int px = 0; px < W; ++px) {
            while (!(xl > x) && !(xr > x)) { ++ix; xl += tw; xr += tw; }
            t.push_back(ix & 1);
            t.push_back(xl > x ? (int)((xl - x) * kx + 0.5f) : -1);
            x += 1.0f;
        }
    }
    for (int y = 0; y < H; ++y) {
        const float yc = (float)y + 0.5f - oy;
        int iy = (int)(yc / th);
        if ((float)iy * th > yc) --iy;
        const float yl = (float)iy * th + hy, yu = (float)(iy + 1) * th - hy;
        int A[4], B[4];
        if (yc >= yl && yu > yc) {
            for (int k = 0; k < 4; ++k) { A[k] = P1[k]; B[k] = P2[k]; }
        } else {
            int wv;
            if (yl > yc) wv = (int)((yl - yc) * ky + 0.5f);
            else wv = (int)((yc - yu) * ky + 0.5f);
            for (int k = 0; k < 4; ++k) {
                A[k] = (P1[k] * (0x10000 - wv) + P2[k] * wv) >> 16;
                B[k] = (P2[k] * (0x10000 - wv) + P1[k] * wv) >> 16;
            }
        }
        t.push_back(iy & 1);
        for (int k = 0; k < 4; ++k) t.push_back(A[k]);
        for (int k = 0; k < 4; ++k) t.push_back(B[k]);
    }
    return true;
}

// Test Pattern: the type, the bar width and the table testpattern's kernel reads (see there).
void testPatternGpu(Ctx& c, int W, int H, int& type, int& barW, std::vector<float>& tab)
{
    type = c.i("TestPatternType");
    barW = std::max(1, W / 7);
    static const int bars[7][3] = {{180, 180, 180}, {16, 180, 180}, {180, 180, 16}, {16, 180, 16}, {180, 16, 180}, {16, 16, 180}, {180, 16, 16}};
    static const int castle[7][3] = {{235, 7, 7}, {16, 16, 16}, {180, 7, 180}, {16, 16, 16}, {180, 180, 7}, {16, 16, 16}, {180, 180, 180}};
    tab.clear();
    for (const auto& r : bars) for (int v : r) tab.push_back((float)v);
    for (const auto& r : castle) for (int v : r) tab.push_back((float)v);
    for (int x = 0; x < W; ++x) {
        const bool colIn = x * 10 >= W && x * 10 <= 9 * W;
        tab.push_back(colIn ? 1.f : 0.f);
        tab.push_back((float)std::min(6, x / barW));
        tab.push_back((float)(int)std::floor(25.0 + (x - 0.1 * W) * 204.0 / (0.8 * W) + 0.5));
        tab.push_back((float)std::max(0, std::min(3, (int)std::floor((x - 0.1 * W) * 4.0 / (0.8 * W)))));
    }
    for (int y = 0; y < H; ++y) {
        const int yb = H - 1 - y;
        const bool rowIn = yb * 10 >= H && yb * 10 <= 9 * H;
        int stepRow = 0;
        if (rowIn) stepRow = std::min(3, std::max(0, (int)std::ceil(4.0 * (yb - 0.1 * H) / (0.8 * H)) - 1));
        tab.push_back(rowIn ? 1.f : 0.f);
        tab.push_back((float)stepRow);
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
