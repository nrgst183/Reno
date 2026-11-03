// Lens Flare as VEGAS's OpenCL kernels draw it (Vfx1.ofx, the draw kernels of its Lens Flare processor; read off the kernel text and the host function that lays the elements out).
// VEGAS launches one kernel per row of the lens' element table, one after the other, each reading the picture the one before wrote (an 8-bit picture: every write is cut to a level by
// the card's store). A pixel only ever depends on its own earlier value, so Reno does all the elements in turn on one picture, in place.
//   - the element's box and centre come from the light position (Light Y negated: the kernels count rows from the bottom), the element's place along the line through the middle of the
//     frame, Size, the element's size and Perspective (a scale of 2 ^ ((place - 1) sin(-Perspective pi / 2)));
//   - u, v run from +1 to -1 over the box (so the middle of the element is at 0, 0); d is the length of (u, v); the element's type turns them into a strength (rings, discs, hexagons,
//     the rays of the sunburst), which times the colour (Tint of the way from the element's own colour to the light's, red and blue swapped in the table's order) and Intensity x the
//     element's strength is added to the pixel, with Blend taken of it;
//   - the sums are single precision in the kernels' order; the sunburst's ray lengths are a table of 64 values the host fills (Reno: the ray table in builtins_lensflare.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {
namespace vfxflare {

// One row of a lens' table: the kernel (0 disc, 1 ring, 2 thin ring, 3 hexagon with a ramp, 4 hexagon, 5 sunburst rays), its strength, size, place and colour (c0 = blue's share).
struct Row { int type; float strength, size, place, c0, c1, c2; };

struct Params {
    float lightR = 1.f, lightG = 1.f, lightB = 1.f;
    float lx = 0.5f, ly = -0.5f;                // the Light position as the setting gives it (up is positive)
    float tint = 0.f, intensity = 0.f, blend = 1.f, size = 1.f, perspective = 0.f;
    float pixelAspect = 1.f;
};

struct Hex {
    float vx[6], vy[6], n0[6], n1[6];           // the vertices and the unit edge normals (dy, dx) / length
    Hex()
    {
        static const float x[6] = {0.6f, -0.6f, -1.0f, -0.6f, 0.6f, 1.0f}, y[6] = {-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f};
        for (int i = 0; i < 6; ++i) { vx[i] = x[i]; vy[i] = y[i]; }
        for (int i = 0; i < 6; ++i) {
            const int j = (i + 1) % 6;
            const float dy = vy[j] - vy[i], dx = vx[i] - vx[j];
            const float l = std::sqrt(dy * dy + dx * dx);
            n0[i] = dy / l;
            n1[i] = dx / l;
        }
    }
    // the smallest of the six signed distances to the edges: negative outside
    float inside(float u, float v) const
    {
        float m = (u - vx[0]) * n0[0] + (v - vy[0]) * n1[0];
        for (int i = 1; i < 6; ++i) {
            const float s = (u - vx[i]) * n0[i] + (v - vy[i]) * n1[i];
            m = s < m ? s : m;
        }
        return m;
    }
};

inline float clamp01f(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// The strength of the element's shape at (u, v), d from its middle, as the kernels work it out (their sums, in their order). Returns false when the pixel gets nothing (the kernel's w = 0).
// col: the three colour parts in the order the kernel's arguments have them.
inline bool shape(int type, float u, float v, float d, float strength, const float* col, const Hex& hex, const float* rays, float* o)
{
    switch (type) {
    case 0: {                                           // disc, falling from the middle
        if (!(d < 1.0f)) return false;
        const float s = 1.0f - d;
        o[0] = col[0] * s * strength; o[1] = col[1] * s * strength; o[2] = col[2] * s * strength;
        return true;
    }
    case 1: {                                           // ring with a soft inner side
        if (!(d < 1.0f)) return false;
        float s;
        if (d > 0.8f) s = (1.0f - d) * 5.f;
        else if (d > 0.75f) s = 0.75f + ((d - 0.75f) * 5.f);
        else s = d;
        o[0] = col[0] * s * strength; o[1] = col[1] * s * strength; o[2] = col[2] * s * strength;
        return true;
    }
    case 2: {                                           // thin ring
        if (!(d < 1.0f && d > 0.85f)) return false;
        const float t = 1.0f - d;
        const float s = t < 0.1f ? t * 10.f : (.15f - t) * 20.f;
        o[0] = col[0] * s * strength; o[1] = col[1] * s * strength; o[2] = col[2] * s * strength;
        return true;
    }
    case 3: {                                           // hexagon, ramp up at the edge and down to the middle
        const float m = hex.inside(u, v);
        if (!(m >= 0.0f)) return false;
        float s;
        if (m < 0.2f) s = clamp01f(m / 0.2f);
        else s = clamp01f((0.8f - (m - 0.2f)) / 0.8f);
        o[0] = col[0] * s * strength; o[1] = col[1] * s * strength; o[2] = col[2] * s * strength;
        return true;
    }
    case 4: {                                           // hexagon, squared distance to the edge
        const float m = hex.inside(u, v);
        if (!(m >= 0.0f)) return false;
        const float s = m * m;
        o[0] = col[0] * s * strength; o[1] = col[1] * s * strength; o[2] = col[2] * s * strength;
        return true;
    }
    default: {                                          // rays round the circle
        if (!(d < 1.0f)) return false;
        const float pi = 3.14159265358979323846f;
        const float ang = (float)(std::atan2((double)v, (double)u));        // (the double function rounded to a float: the same on every C library)
        const float a = ang + pi;
        const float p = a / (2 * pi) * 3;
        const float pv = p * 60.f;
        const float fr = pv - std::trunc(pv);
        int i = (int)pv;
        if (i < 0) i = 0;
        const float w = rays[i % 64] * (1 - fr) + rays[(i + 1) % 64] * fr;
        const float lim = 1.0f * w;
        float q;                                        // the kernel's ramp helper: 0 up to 0, 1 from lim
        if (d <= 0.0f) q = 0.f;
        else if (d >= lim) q = 1.f;
        else q = (d - 0.0f) / (lim - 0.0f);
        float s = 1.0f - q;
        s = s * s * s;
        if (d < 0.3f) s = 1 + ((s - 1) * (d / 0.3f));
        o[0] = col[0] * strength * s; o[1] = col[1] * strength * s; o[2] = col[2] * strength * s;
        return true;
    }
    }
}

// One element's box and colour as the draw works them out (the same numbers the GPU plan hands its kernel): the pixels x in [xa, xb), y in [ya, yb) are drawn.
struct Elem { int type; float strength, ox, oy, R, Rx; int xa, xb, ya, yb; float col[3]; };

// The elements that draw something, in order (an element whose box is not a sane size is left out, as the draw skips it).
inline void prepare(std::vector<Elem>& out, int W, int H, const Row* rows, int nrows, const Params& P)
{
    out.clear();
    const float lx = P.lx, ly = -P.ly;
    const float mn = (float)(W < H ? W : H);
    const float sinP = (float)std::sin((double)(-P.perspective) * 1.5707963267948966);
    const float pa = P.pixelAspect > 0.f ? P.pixelAspect : 1.f;
    const float Wf = (float)W, Hf = (float)H;
    const float lcol[3] = {P.lightR, P.lightG, P.lightB};
    const float tint = P.tint;
    auto sane = [](float v) { return std::isfinite(v) && std::fabs(v) < 1.0e8f; };
    for (int n = 0; n < nrows; ++n) {
        const Row& e = rows[n];
        const float xoff = e.place * (-lx) + lx, yoff = e.place * (-ly) + ly;
        const float sc = (float)std::pow(2.0, (double)((e.place - 1.0f) * sinP));
        const float cx = Wf * xoff * 0.5f + Wf * 0.5f, cy = Hf * yoff * 0.5f + Hf * 0.5f;
        const float R = sc * P.size * e.size * mn;
        const float Rx = R / pa;
        const float left = cx - Rx, top = cy - R, rightF = cx + Rx, botF = R + cy;
        if (!sane(left) || !sane(top) || !sane(rightF) || !sane(botF) || !(R > 0.f) || !(Rx > 0.f)) continue;
        const int ix0 = (int)left, iy0 = (int)top, ix1 = (int)rightF, iy1 = (int)botF;
        const float fx = left - (float)ix0, fy = top - (float)iy0;
        Elem el;
        el.type = e.type;
        el.ox = (float)ix0 + fx;
        el.oy = (float)iy0 + fy;         // the box's origin with its fraction
        el.R = R;
        el.Rx = Rx;
        // the colour: Tint of the way from the element's own to the light's; the table is in blue, green, red order
        el.col[0] = (1.0f - tint) * e.c2 + tint * lcol[0];
        el.col[1] = tint * lcol[1] + (1.0f - tint) * e.c1;
        el.col[2] = tint * lcol[2] + (1.0f - tint) * e.c0;
        el.strength = P.intensity * e.strength;
        // (the kernel runs over the whole picture and keeps the pixels with ix0 <= x < ix1 and iy0 <= H - row < iy1; here only those)
        el.xa = ix0 > 0 ? ix0 : 0;
        el.xb = ix1 < W ? ix1 : W;
        el.ya = iy0 - 1;                                 // (top-down row y has v64 = y + 1)
        el.yb = iy1 - 1;
        if (el.ya < 0) el.ya = 0;
        if (el.yb > H) el.yb = H;
        out.push_back(el);
    }
}

// Draws the lens' elements onto 'img' (W x H, RGBA floats premultiplied, top row first), in place. 'rows'/'nrows': the lens' table; 'rays': 64 ray lengths; 'store': the 8-bit store the
// kernels' writes go through (nullptr: floats are kept).
inline void render(std::vector<float>& img, int W, int H, const Row* rows, int nrows, const Params& P, const float* rays, float (*store)(float))
{
    if (W < 1 || H < 1 || img.size() < (size_t)W * H * 4) return;
    static const Hex hex;
    if (store && nrows > 0)
        for (size_t i = 0; i < (size_t)W * H * 4; ++i) img[i] = store(img[i]);         // (the first launch writes every pixel)
    const float blend = P.blend, iblend = 1.0f - blend;
    std::vector<Elem> els;
    prepare(els, W, H, rows, nrows, P);
    for (const Elem& el : els) {
        const float R = el.R, Rx = el.Rx, ox = el.ox, oy = el.oy, strength = el.strength;
        const float* col = el.col;
        for (int y = el.ya; y < el.yb; ++y) {
            const float v64 = (float)(y + 1);
            const float v = 1 - ((v64 - oy) / R);
            for (int x = el.xa; x < el.xb; ++x) {
                const float u = 1 - (((float)x - ox) / Rx);
                const float d = (float)std::sqrt((double)u * (double)u + (double)v * (double)v);        // (hypot, correctly rounded)
                float eff[3];
                if (!shape(el.type, u, v, d, strength, col, hex, rays, eff)) continue;
                float* p = &img[((size_t)y * W + x) * 4];
                float c[4] = {p[0], p[1], p[2], p[3]};
                // the blend helper: out = c (1 - Blend) + (effect + c) Blend, on the colour with the alpha divided out
                if (c[3] > 0.000001f) {
                    if (c[3] < 1.0f) {
                        const float a = c[3];
                        for (int k = 0; k < 3; ++k) {
                            float t = c[k] / a;
                            t = t * iblend + (eff[k] + t) * blend;
                            c[k] = t * a;
                        }
                    } else {
                        c[3] = 1.0f;
                        for (int k = 0; k < 3; ++k) c[k] = c[k] * iblend + (eff[k] + c[k]) * blend;
                    }
                } else {
                    c[0] = c[1] = c[2] = c[3] = 0.f;
                }
                for (int k = 0; k < 4; ++k) p[k] = store ? store(c[k]) : c[k];
            }
        }
    }
}

}  // namespace vfxflare
}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
