// Stand-ins for VEGAS Pro 18's two AI video effects (MagixAiFx.ofx), which run neural networks (OpenVINO "VEGAS Deep Learning Models") that Reno does not have.
// Each is an approximation made without a network; the descriptions say so.
//
//   de.magix:Colorization   "VEGAS Colorization": colours a black and white picture. Reno guesses colour from tone, position and detail (bright smooth areas near
//                           the top: sky; textured mid tones lower down: foliage; the rest: warm skin and earth tones), spreads it along the picture's edges
//                           (guided filter) and keeps the luminance. Parameters are the plug-in's: Preprocess, Precision, InvertColorA/B, ColorShiftA/B,
//                           ColorMagnificationA/B (A is the green-red and B the blue-yellow axis).
//   de.magix:StyleTransfer  "VEGAS Style Transfer": paints the picture in the style of 17 learned models (Model 1 .. 17; their names are not in the VEGAS files here).
//                           Reno has 17 painterly looks of its own (Kuwahara brush strokes, a colour treatment, outlines, canvas): Details sets how fine the
//                           strokes are (Low .. High; Original keeps the picture's own detail), Method how carefully they are made (Accurate or Fast).
#include "builtins_analysis_internal.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {
namespace {

// ------------------------------------------------------------------ box filters
// The mean of every pixel's (2r+1) x (2r+1) neighbourhood, edges counting only the pixels there are.
void boxMean(const std::vector<float>& in, int w, int h, int r, std::vector<float>& out)
{
    out.assign((size_t)w * h, 0.f);
    if (r <= 0) { out = in; return; }
    std::vector<float> tmp((size_t)w * h);
    std::vector<double> pre((size_t)w + 1);
    for (int y = 0; y < h; ++y) {
        const float* row = &in[(size_t)y * w];
        pre[0] = 0;
        for (int x = 0; x < w; ++x) pre[(size_t)x + 1] = pre[(size_t)x] + row[x];
        float* o = &tmp[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            const int a = std::max(0, x - r), b = std::min(w, x + r + 1);
            o[x] = (float)((pre[(size_t)b] - pre[(size_t)a]) / (b - a));
        }
    }
    std::vector<double> col((size_t)w, 0.0);
    for (int y = 0; y <= std::min(h - 1, r); ++y)
        for (int x = 0; x < w; ++x) col[(size_t)x] += tmp[(size_t)y * w + x];
    for (int y = 0; y < h; ++y) {
        if (y > 0) {
            if (y + r < h) for (int x = 0; x < w; ++x) col[(size_t)x] += tmp[(size_t)(y + r) * w + x];
            if (y - r - 1 >= 0) for (int x = 0; x < w; ++x) col[(size_t)x] -= tmp[(size_t)(y - r - 1) * w + x];
        }
        const int n = std::min(h, y + r + 1) - std::max(0, y - r);
        float* o = &out[(size_t)y * w];
        for (int x = 0; x < w; ++x) o[x] = (float)(col[(size_t)x] / n);
    }
}

// Edge-preserving smoothing of 'p' guided by 'I' (He et al.'s guided filter).
void guidedFilter(const std::vector<float>& I, const std::vector<float>& p, int w, int h, int r, float eps, std::vector<float>& out)
{
    const size_t n = (size_t)w * h;
    std::vector<float> mI, mp, mIp(n), mII(n), tmp(n);
    boxMean(I, w, h, r, mI);
    boxMean(p, w, h, r, mp);
    for (size_t i = 0; i < n; ++i) tmp[i] = I[i] * p[i];
    boxMean(tmp, w, h, r, mIp);
    for (size_t i = 0; i < n; ++i) tmp[i] = I[i] * I[i];
    boxMean(tmp, w, h, r, mII);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        const float var = mII[i] - mI[i] * mI[i];
        const float cov = mIp[i] - mI[i] * mp[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mI[i];
    }
    std::vector<float> ma, mb;
    boxMean(a, w, h, r, ma);
    boxMean(b, w, h, r, mb);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = ma[i] * I[i] + mb[i];
}

inline float sstep(float e0, float e1, float x)
{
    const float t = std::max(0.f, std::min(1.f, (x - e0) / (e1 - e0)));
    return t * t * (3.f - 2.f * t);
}

// A plane of straight luma/colour values as an array, from the frame.
struct Planes {
    int w = 0, h = 0;
    std::vector<float> r, g, b, a;
};

void splitFrame(const Buf& s, Planes& p)
{
    p.w = s.w; p.h = s.h;
    const size_t n = (size_t)s.w * s.h;
    p.r.resize(n); p.g.resize(n); p.b.resize(n); p.a.resize(n);
    forRows(s.h, s.w, [&](int y) {
        const float* q = s.at(0, y);
        for (int x = 0; x < s.w; ++x, q += 4) {
            const size_t i = (size_t)y * s.w + x;
            unpremul(q, p.r[i], p.g[i], p.b[i], p.a[i]);
        }
    });
}

inline float hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xffff) / 65535.f;
}

// ------------------------------------------------------------------ Colorization
void colorization(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.src.px.size() != c.out.px.size() || W < 4 || H < 4) { c.out.px = c.src.px; return; }
    const int preprocess = c.i("Preprocess"), precision = c.i("Precision");
    const bool invA = c.b("InvertColorA"), invB = c.b("InvertColorB");
    const float shiftA = (float)clampd(c.d("ColorShiftA"), -1, 1), shiftB = (float)clampd(c.d("ColorShiftB"), -1, 1);
    const float gainA = (float)clampd(c.d("ColorMagnificationA"), 0.1, 3), gainB = (float)clampd(c.d("ColorMagnificationB"), 0.1, 3);

    // the working picture: the luminance at the precision chosen
    const int sizes[4] = {96, 160, 256, 1 << 20};
    const Gray g = makeGray(c.src, sizes[std::max(0, std::min(3, precision))]);
    const int w = g.w, h = g.h;
    std::vector<float> L = g.p;
    float lo = 0.f, hi = 1.f;
    if (preprocess > 0) {
        std::vector<float> sorted = L;
        const size_t i0 = sorted.size() * 2 / 100, i1 = sorted.size() * 98 / 100;
        std::nth_element(sorted.begin(), sorted.begin() + (ptrdiff_t)i0, sorted.end());
        lo = sorted[i0];
        std::nth_element(sorted.begin(), sorted.begin() + (ptrdiff_t)i1, sorted.end());
        hi = sorted[std::min(i1, sorted.size() - 1)];
        if (hi - lo < 0.05f) { lo = 0.f; hi = 1.f; }
        for (float& v : L) v = std::max(0.f, std::min(1.f, (v - lo) / (hi - lo)));
    }
    const int rs = std::max(2, (int)std::lround(w * 0.02));
    std::vector<float> Ls;
    boxMean(L, w, h, rs, Ls);
    std::vector<float> grad((size_t)w * h, 0.f), T;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float gx = L[(size_t)y * w + std::min(w - 1, x + 1)] - L[(size_t)y * w + std::max(0, x - 1)];
            const float gy = L[(size_t)std::min(h - 1, y + 1) * w + x] - L[(size_t)std::max(0, y - 1) * w + x];
            grad[(size_t)y * w + x] = 0.5f * std::sqrt(gx * gx + gy * gy);
        }
    boxMean(grad, w, h, std::max(2, (int)std::lround(w * 0.03)), T);
    // a guess at the colour (a: green .. red, b: blue .. yellow), from the tone, the place and the detail
    std::vector<float> pa((size_t)w * h), pb((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const float yy = h > 1 ? (float)y / (h - 1) : 0.f;
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            const float l = Ls[i], t = T[i];
            const float sky = sstep(0.55f, 0.f, yy) * sstep(0.5f, 0.78f, l) * (1.f - sstep(0.012f, 0.05f, t));
            const float veg = sstep(0.15f, 0.5f, yy) * sstep(0.08f, 0.22f, l) * (1.f - sstep(0.55f, 0.8f, l)) * sstep(0.008f, 0.03f, t);
            const float rest = std::max(0.f, 1.f - sky - veg);
            float a = sky * -0.02f + veg * -0.20f + rest * 0.09f;
            float b = sky * -0.30f + veg * 0.20f + rest * 0.13f;
            const float sat = sstep(0.f, 0.14f, l) * (1.f - 0.8f * sstep(0.88f, 1.f, l));
            pa[i] = a * sat;
            pb[i] = b * sat;
        }
    }
    std::vector<float> sa, sb;
    const int rg = std::max(3, (int)std::lround(w * 0.07));
    guidedFilter(Ls, pa, w, h, rg, 0.004f, sa);
    guidedFilter(Ls, pb, w, h, rg, 0.004f, sb);
    for (size_t i = 0; i < sa.size(); ++i) {
        float a = sa[i], b = sb[i];
        if (invA) a = -a;
        if (invB) b = -b;
        sa[i] = a * gainA + shiftA * 0.25f;
        sb[i] = b * gainB + shiftB * 0.25f;
    }
    // onto the full picture: bilinear, with the picture's own luminance
    const bool enhanceOut = preprocess == 2;
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        const float fy = ((y + 0.5f) * h / H) - 0.5f;
        const int y0 = std::max(0, std::min(h - 1, (int)std::floor(fy))), y1 = std::min(h - 1, y0 + 1);
        const float ty = std::max(0.f, std::min(1.f, fy - y0));
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            float r, gg, b, al;
            unpremul(s, r, gg, b, al);
            if (al <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            const float fx = ((x + 0.5f) * w / W) - 0.5f;
            const int x0 = std::max(0, std::min(w - 1, (int)std::floor(fx))), x1 = std::min(w - 1, x0 + 1);
            const float tx = std::max(0.f, std::min(1.f, fx - x0));
            auto bil = [&](const std::vector<float>& pl) {
                return (pl[(size_t)y0 * w + x0] * (1 - tx) + pl[(size_t)y0 * w + x1] * tx) * (1 - ty) + (pl[(size_t)y1 * w + x0] * (1 - tx) + pl[(size_t)y1 * w + x1] * tx) * ty;
            };
            float yy, u, v;
            rgbToYuv(r, gg, b, yy, u, v);
            if (enhanceOut) yy = std::max(0.f, std::min(1.f, (yy - lo) / (hi - lo)));
            const float a = bil(sa), bb = bil(sb);
            float rr, g2, b2;
            yuvToRgb(yy, -bb, a, rr, g2, b2);
            o[0] = clamp01(rr) * al; o[1] = clamp01(g2) * al; o[2] = clamp01(b2) * al; o[3] = al;
        }
    });
}

// ------------------------------------------------------------------ Style Transfer
struct Look {
    const char* name;
    float brush;
    float sat, contrast;
    int levels;         // 0: no posterising
    float edge;         // outline strength
    float canvas;       // canvas / paper grain
    float hue;
    float shadow[3], high[3];
    float dots;
};

const Look kLooks[17] = {
    {"Impressionism",       0.010f, 1.15f, 1.00f, 0, 0.00f, 0.05f,   0.f, {0.00f, 0.00f, 0.03f}, {0.03f, 0.02f, 0.00f}, 0.f},
    {"Post-impressionism",  0.013f, 1.45f, 1.20f, 0, 0.10f, 0.07f,  -8.f, {0.00f, 0.00f, 0.06f}, {0.05f, 0.03f, -0.03f}, 0.f},
    {"Pointillism",         0.006f, 1.35f, 1.10f, 0, 0.00f, 0.05f,   0.f, {0.00f, 0.00f, 0.02f}, {0.02f, 0.02f, 0.00f}, 1.f},
    {"Fauvism",             0.010f, 1.90f, 1.25f, 9, 0.00f, 0.03f,  12.f, {0.00f, -0.02f, 0.06f}, {0.06f, 0.00f, -0.04f}, 0.f},
    {"Cubism",              0.020f, 0.55f, 1.15f, 6, 0.55f, 0.04f,  20.f, {0.02f, 0.01f, 0.00f}, {0.05f, 0.04f, 0.00f}, 0.f},
    {"Abstract (Kandinsky)",0.016f, 1.70f, 1.20f, 5, 0.75f, 0.03f, -20.f, {0.00f, 0.00f, 0.08f}, {0.06f, 0.04f, -0.03f}, 0.f},
    {"Expressionism",       0.014f, 1.50f, 1.35f, 0, 0.20f, 0.06f,  25.f, {0.00f, 0.02f, 0.10f}, {0.08f, 0.03f, -0.05f}, 0.f},
    {"Pop art",             0.004f, 2.10f, 1.40f, 4, 0.95f, 0.00f,   0.f, {0.00f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.00f}, 0.f},
    {"Ukiyo-e",             0.008f, 0.75f, 1.05f, 6, 0.85f, 0.06f, -15.f, {0.00f, 0.03f, 0.09f}, {0.07f, 0.05f, 0.00f}, 0.f},
    {"Watercolour",         0.016f, 0.85f, 0.85f, 0, 0.08f, 0.10f,   0.f, {0.04f, 0.04f, 0.06f}, {0.05f, 0.05f, 0.05f}, 0.f},
    {"Charcoal",            0.007f, 0.00f, 1.50f, 0, 0.70f, 0.14f,   0.f, {0.00f, 0.00f, 0.00f}, {0.04f, 0.04f, 0.03f}, 0.f},
    {"Oil painting",        0.008f, 1.15f, 1.12f, 0, 0.00f, 0.05f,   0.f, {0.00f, 0.00f, 0.02f}, {0.02f, 0.01f, 0.00f}, 0.f},
    {"Stained glass",       0.022f, 1.60f, 1.20f, 5, 0.90f, 0.02f,   0.f, {0.00f, 0.00f, 0.05f}, {0.04f, 0.03f, 0.00f}, 0.f},
    {"Sepia ink",           0.006f, 0.00f, 1.30f, 0, 0.65f, 0.09f,   0.f, {0.10f, 0.05f, 0.00f}, {0.12f, 0.08f, 0.02f}, 0.f},
    {"Vintage poster",      0.009f, 1.10f, 1.25f, 5, 0.30f, 0.12f,  10.f, {0.08f, 0.03f, 0.00f}, {0.10f, 0.07f, 0.01f}, 0.f},
    {"Neon night",          0.010f, 1.70f, 1.45f, 0, 0.25f, 0.02f, -30.f, {0.04f, 0.00f, 0.14f}, {0.10f, 0.00f, 0.08f}, 0.f},
    {"Pastel dream",        0.015f, 0.70f, 0.80f, 0, 0.05f, 0.06f,   0.f, {0.10f, 0.08f, 0.12f}, {0.06f, 0.05f, 0.04f}, 0.f},
};

// VEGAS's 17 styles in its order (the paintings and patterns its models were trained on, as its Style list names them) and the look of Reno's approximation
// each gets (an index into kLooks); the look follows the kind of picture, not the painting's strokes.
const char* const kStyleNames[17] = {"Self Portrait (Picasso)", "Night Alley Walk (Afremov)", "The Great Wave of Kanagawa (Hokusai)", "Fruit (Hunt)", "Light (Afremov)",
    "Small Worlds 1 (Kandinsky)", "Floral pattern", "Sunrise Flowers (Lambert)", "Abstract Painting (Picasso)", "Black and White (KLH)", "The Starry Night (van Gogh)",
    "The Pandorica Opens (van Gogh)", "The Weeping Woman (Picasso)", "Bark pattern", "Font pattern", "Leaf pattern", "Rick and Morty"};
const int kStyleLook[17] = {4, 15, 8, 3, 6, 5, 16, 0, 14, 10, 1, 11, 12, 13, 2, 9, 7};

// The Kuwahara filter: every pixel takes the mean of the smoothest of the four windows round it (edges stay sharp, flat areas become brush strokes). 'soft': windows are
// blended by smoothness (Accurate) instead of taking the smoothest alone (Fast).
void kuwahara(Planes& p, int radius, bool soft)
{
    const int w = p.w, h = p.h;
    if (radius < 1) return;
    const size_t n = (size_t)w * h;
    const int hh = std::max(1, radius / 2);
    const int off = hh;
    std::vector<float> L(n), L2(n);
    for (size_t i = 0; i < n; ++i) { L[i] = 0.299f * p.r[i] + 0.587f * p.g[i] + 0.114f * p.b[i]; L2[i] = L[i] * L[i]; }
    std::vector<float> mR, mG, mB, mL, mL2;
    boxMean(p.r, w, h, hh, mR);
    boxMean(p.g, w, h, hh, mG);
    boxMean(p.b, w, h, hh, mB);
    boxMean(L, w, h, hh, mL);
    boxMean(L2, w, h, hh, mL2);
    std::vector<float> nr(n), ng(n), nb(n);
    parallelRows(h, (size_t)w * 40, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float best = 1e30f;
                int bi = 0;
                size_t idx[4];
                float var[4];
                for (int k = 0; k < 4; ++k) {
                    const int sx = std::max(0, std::min(w - 1, x + ((k & 1) ? off : -off)));
                    const int sy = std::max(0, std::min(h - 1, y + ((k & 2) ? off : -off)));
                    idx[k] = (size_t)sy * w + sx;
                    var[k] = std::max(0.f, mL2[idx[k]] - mL[idx[k]] * mL[idx[k]]);
                    if (var[k] < best) { best = var[k]; bi = k; }
                }
                const size_t o = (size_t)y * w + x;
                if (!soft) {
                    nr[o] = mR[idx[bi]]; ng[o] = mG[idx[bi]]; nb[o] = mB[idx[bi]];
                } else {
                    float sw = 0, r = 0, g = 0, b = 0;
                    for (int k = 0; k < 4; ++k) {
                        float q = 1.f / (var[k] * 400.f + 0.02f);
                        q = q * q;
                        sw += q; r += q * mR[idx[k]]; g += q * mG[idx[k]]; b += q * mB[idx[k]];
                    }
                    nr[o] = r / sw; ng[o] = g / sw; nb[o] = b / sw;
                }
            }
    });
    p.r.swap(nr); p.g.swap(ng); p.b.swap(nb);
}

void styleTransfer(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.src.px.size() != c.out.px.size() || W < 8 || H < 8) { c.out.px = c.src.px; return; }
    const Look& look = kLooks[kStyleLook[std::max(0, std::min(16, c.i("Style")))]];
    const int details = std::max(0, std::min(3, c.i("Details")));
    const bool accurate = c.i("Method") == 0;
    static const float detailScale[4] = {1.7f, 1.0f, 0.55f, 0.4f};
    Planes p;
    splitFrame(c.src, p);
    const Planes orig = p;
    const int rad = (int)std::lround(look.brush * W * detailScale[details]);
    if (rad >= 1) {
        kuwahara(p, std::min(rad, 40), accurate);
        if (accurate && rad >= 3) kuwahara(p, std::max(1, rad / 2), true);
    }
    const size_t n = (size_t)W * H;
    std::vector<float> edge;
    if (look.edge > 0.f) {
        std::vector<float> L(n);
        for (size_t i = 0; i < n; ++i) L[i] = 0.299f * orig.r[i] + 0.587f * orig.g[i] + 0.114f * orig.b[i];
        gaussianBlurPlane(L, W, H, std::max(0.8, c.px(1.2)), std::max(0.8, c.px(1.2)));
        edge.assign(n, 0.f);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                const size_t i = (size_t)y * W + x;
                const float gx = (L[i + 1 - W] + 2 * L[i + 1] + L[i + 1 + W]) - (L[i - 1 - W] + 2 * L[i - 1] + L[i - 1 + W]);
                const float gy = (L[i + W - 1] + 2 * L[i + W] + L[i + W + 1]) - (L[i - W - 1] + 2 * L[i - W] + L[i - W + 1]);
                edge[i] = sstep(0.18f, 0.55f, std::sqrt(gx * gx + gy * gy));
            }
    }
    const double hueRad = look.hue * kPi / 180.0;
    const float ch = (float)std::cos(hueRad), sh = (float)std::sin(hueRad);
    const float blendOrig = details == 3 ? 0.35f : 0.f;
    const double period = std::max(2.0, 4.0 * c.scale * (W / 640.0));
    forRows(H, W, [&](int y) {
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x, o += 4) {
            const size_t i = (size_t)y * W + x;
            const float a = p.a[i];
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            float yy, u, v;
            rgbToYuv(p.r[i], p.g[i], p.b[i], yy, u, v);
            yy = (yy - 0.5f) * look.contrast + 0.5f;
            float u2 = (u * ch - v * sh) * look.sat, v2 = (u * sh + v * ch) * look.sat;
            float r, g, b;
            yuvToRgb(yy, u2, v2, r, g, b);
            const float t = clamp01(yy);
            r += look.shadow[0] * (1 - t) + look.high[0] * t;
            g += look.shadow[1] * (1 - t) + look.high[1] * t;
            b += look.shadow[2] * (1 - t) + look.high[2] * t;
            if (look.levels > 0) {
                const float nl = (float)look.levels;
                auto post = [&](float vv) {
                    vv = clamp01(vv) * nl;
                    const float fl = std::floor(vv);
                    return (fl + sstep(0.35f, 0.65f, vv - fl)) / nl;
                };
                r = post(r); g = post(g); b = post(b);
            }
            if (look.dots > 0.f) {
                const double cx = std::fmod((double)x / (period * 1.5), 1.0) - 0.5, cy = std::fmod((double)y / (period * 1.5), 1.0) - 0.5;
                const float inDot = sstep(0.5f, 0.3f, (float)std::sqrt(cx * cx + cy * cy) + (1.f - clamp01(luma(r, g, b))) * 0.0f);
                const float k = look.dots * (0.35f + 0.65f * inDot);
                r = r * k + 0.95f * (1 - k) * 1.0f; g = g * k + 0.93f * (1 - k); b = b * k + 0.88f * (1 - k);
            }
            if (!edge.empty()) {
                const float line = edge[i] * look.edge;
                const float f = 1.f - 0.9f * line;
                r *= f; g *= f; b *= f;
            }
            if (look.canvas > 0.f) {
                const float weave = 0.5f + 0.25f * (float)(std::sin(2 * kPi * x / period) * std::sin(2 * kPi * y / period)) + 0.25f * (hash2(x / 2, y / 2) - 0.5f) * 2.f;
                const float f = 1.f + look.canvas * (weave - 0.5f) * 1.2f;
                r *= f; g *= f; b *= f;
            }
            if (blendOrig > 0.f) {
                r = r * (1 - blendOrig) + orig.r[i] * blendOrig;
                g = g * (1 - blendOrig) + orig.g[i] * blendOrig;
                b = b * (1 - blendOrig) + orig.b[i] * blendOrig;
            }
            o[0] = clamp01(r) * a; o[1] = clamp01(g) * a; o[2] = clamp01(b) * a; o[3] = a;
        }
    });
}

}  // namespace

void addAiEffects(std::vector<Def>& out)
{
    {
        std::vector<PDef> ps;
        // (VEGAS's one-entry choice that carries its "install the AI bundle" message: nothing to choose, so not shown)
        ps.push_back(choice("Localization", "", 0, {"This plugin requires additional data. Please install the VEGAS AI bundle, which you can download at www.vegascreativesoftware.com."}));
        ps.back().secret = true;
        ps.push_back(choice("Preprocess", "Luminance enhancement", 0, {"None", "Estimation only (do not apply to output)", "Apply to output"}));
        ps.push_back(choice("Precision", "Precision", 0, {"Low", "Medium", "High", "Match Input Size"}));
        ps.push_back(boolean("InvertColorA", "Invert Green and Red", false));
        ps.push_back(boolean("InvertColorB", "Invert Blue and Yellow", false));
        ps.push_back(dbl("ColorShiftA", "Shift between Green and Red", 0.0, -1, 1));
        ps.push_back(dbl("ColorShiftB", "Shift between Blue and Yellow", 0.0, -1, 1));
        ps.push_back(dbl("ColorMagnificationA", "Gain on Green and Red", 1.0, 0.1, 3));
        ps.push_back(dbl("ColorMagnificationB", "Gain on Blue and Yellow", 1.0, 0.1, 3));
        ps.push_back(custom("ModelNotFound", "Model not found", ""));
        ps.back().secret = true;
        out.push_back(make("de.magix:Colorization", "Colorization",
                           "Colours a black and white picture. VEGAS uses an AI model for this; Reno guesses sky, foliage and warm tones from the picture's tones, place and detail and spreads them along its edges - plausible, not the AI's result",
                           ps, colorization));
    }
    {
        std::vector<PDef> ps;
        std::vector<std::string> styles(kStyleNames, kStyleNames + 17);
        ps.push_back(choice("Model not found", "", 0, {"This plugin requires additional data. Please install the VEGAS AI bundle, which you can download at www.vegascreativesoftware.com."}));
        ps.back().secret = true;
        ps.push_back(choice("Style", "Style", 0, styles));
        ps.push_back(dbl("Mean Red", "Mean Red", 103.94, 0, 255));
        ps.push_back(dbl("Mean Green", "Mean Green", 116.78, 0, 255));
        ps.push_back(dbl("Mean Blue", "Mean Blue", 123.68, 0, 255));
        ps.push_back(choice("Details", "Details", 0, {"Low", "Medium", "High", "Original"}));
        ps.push_back(choice("Method", "Method", 0, {"Accurate", "Fast"}));
        // Custom-reference settings: VEGAS lets the user's own picture stand in for the 17 styles. Reno's looks are not made from a reference, so these are kept
        // (they round-trip) but unused and hidden. The image setting is VEGAS's OfxParamTypeImage (declared when the host has the VEGAS extensions); DetailSwitch's default and range are a guess.
        ps.push_back(intp("DetailSwitch", "Detail Level", 0, 0, 3));
        ps.push_back(choice("CustomSwitch", "Style source", 0, {"Pre-defined Reference", "Custom Reference"}));
        ps.push_back(imageParam("StyleRefImg", "Reference Style"));
        for (size_t k = 2; k <= 4; ++k) ps[k].secret = true;                // (the colour means of the network's input: nothing to set)
        for (size_t k = 7; k < ps.size(); ++k) ps[k].secret = true;
        out.push_back(make("de.magix:StyleTransfer", "Style Transfer",
                           "Paints the picture in the style of a painting. VEGAS uses 17 AI models for this; Reno has 17 painterly looks of its own (brush strokes, colour treatment, outlines, canvas) - not the same styles",
                           ps, styleTransfer));
    }
}

}  // namespace analysis
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
