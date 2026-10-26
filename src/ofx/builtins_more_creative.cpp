// Built-in creative effects (Black Bar Fill, Crop, Picture in Picture, film looks, Vignette, TV Simulator, Timecode, Deform...); see builtins_more.cpp.
#include <cstring>
#include "builtins_more_internal.h"
#include "builtins_fast.h"
#include "builtins_vec.h"
#include "../core/base.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ creative
// Black Bar Fill: the frame (scaled) over a blurred, enlarged copy of itself, with a soft shadow. Measured on VEGAS: positions are pixel INDICES, not centres (source index = (x - location*W)/scale + W/2,
// rows from the bottom); a picture exists only where that index lies inside the frame (hard edge); the shadow is the blurred foreground mask (sigma 9.4 px per unit of Shadow Blur at 320 wide) and darkens
// the colour only; a resampled or blurred picture comes out slightly darker with alpha 254 (8-bit).
void blackBarFill(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double fx, fy, bx, by;
    c.xy("Foreground Location", fx, fy);
    c.xy("Background Location", bx, by);
    const double fs = std::max(0.01, c.d("Foreground Scale")), bs = std::max(0.01, c.d("Background Scale"));
    auto srcPos = [&](int x, int y, double cx, double cy, double z, double& sx, double& sy) {
        sx = (x - cx * W) / z + W * 0.5;
        const double sj = ((H - 1 - y) - cy * H) / z + H * 0.5;
        sy = (H - 1) - sj;
    };
    const double eps = 0.397;
    auto inside = [&](double sx, double sy) { return sx >= -eps && sx <= W - 1 + eps && sy >= -eps && sy <= H - 1 + eps; };
    auto isWhole = [&](double z, double cx, double cy) {
        double sx, sy;
        srcPos(0, 0, cx, cy, z, sx, sy);
        return std::fabs(z - 1) < 1e-9 && std::fabs(sx - std::floor(sx + 0.5)) < 1e-6 && std::fabs(sy - std::floor(sy + 0.5)) < 1e-6;
    };
    // bilinear on the index grid; nothing outside the frame
    auto fetch = [&](double sx, double sy, float* o) {
        sx = clampd(sx, 0, W - 1);
        sy = clampd(sy, 0, H - 1);
        const int x0 = std::min((int)sx, W - 1), y0 = std::min((int)sy, H - 1), x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
        const float u = (float)(sx - x0), v = (float)(sy - y0);
        const float *a = c.src.at(x0, y0), *b = c.src.at(x1, y0), *cc = c.src.at(x0, y1), *d = c.src.at(x1, y1);
        for (int i = 0; i < 4; ++i) o[i] = (a[i] * (1 - u) + b[i] * u) * (1 - v) + (cc[i] * (1 - u) + d[i] * u) * v;
    };
    Buf bg;
    bg.resize(W, H);
    const double blur = clampd(c.d("Background Blur"), 0, 1) * 0.3213 * W;
    const bool bgWhole = isWhole(bs, bx, by);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            double sx, sy;
            srcPos(x, y, bx, by, bs, sx, sy);
            float* o = bg.at(x, y);
            if (inside(sx, sy)) fetch(sx, sy, o); else std::memset(o, 0, sizeof(float) * 4);
        }
    });
    const bool bgBlurred = blur > c.px(0.05);
    if (bgBlurred) {
        const int bf = (c.levels && blur < 24) ? 1 : softBlurFactor(blur);
        if (bf > 1) {
            const int lw = (W + bf - 1) / bf, lh = (H + bf - 1) / bf;
            Buf low;
            low.ensure(lw, lh);
            for (int j = 0; j < lh; ++j)
                for (int i = 0; i < lw; ++i) std::memcpy(low.at(i, j), bg.at(std::min(W - 1, i * bf + bf / 2), std::min(H - 1, j * bf + bf / 2)), sizeof(float) * 4);
            gaussianBlur(low, blur / bf, blur / bf);
            upsampleBilinear(low.px, lw, lh, 4, bf, bg.px, W, H);
        } else gaussianBlur(bg, blur, blur);
    }
    if (c.levels && (bgBlurred || !bgWhole)) {          // what VEGAS's resampling/blur does to 8-bit levels
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                float* o = bg.at(x, y);
                for (int i = 0; i < 3; ++i) {
                    const double v = o[i] * 255.0;
                    o[i] = (float)(bgBlurred ? std::max(0.0, std::floor(v * (254.0 / 255.0) - 0.5 + 1e-4)) : std::floor(std::ceil(v - 1e-3) * (127.0 / 128.0) + 1e-6)) / 255.f;
                }
                o[3] = (float)std::max(0.0, std::ceil(o[3] * 255.0 - 1e-3) - (o[3] > 0 ? 1 : 0)) / 255.f;
            }
        });
    }
    const bool fgWhole = isWhole(fs, fx, fy);
    std::vector<unsigned char> fm((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double sx, sy;
            srcPos(x, y, fx, fy, fs, sx, sy);
            fm[(size_t)y * W + x] = inside(sx, sy) ? 1 : 0;
        }
    const double shadow = clampd(c.d("Foreground Shadow Blur"), 0, 1);
    std::vector<float> sh;
    if (shadow > 0.001) {
        const double sigma = shadow * 0.029375 * W;
        const int sf = (c.levels && sigma < 12) ? 1 : softBlurFactor(sigma);
        const int lw = (W + sf - 1) / sf, lh = (H + sf - 1) / sf;
        std::vector<float> low((size_t)lw * lh);
        for (int j = 0; j < lh; ++j)
            for (int i = 0; i < lw; ++i) low[(size_t)j * lw + i] = (float)fm[(size_t)std::min(H - 1, j * sf + sf / 2) * W + std::min(W - 1, i * sf + sf / 2)];
        gaussianBlurPlane(low, lw, lh, sigma / sf, sigma / sf);
        if (sf > 1) upsampleBilinear(low, lw, lh, 1, sf, sh, W, H); else sh.swap(low);
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            std::memcpy(o, bg.at(x, y), sizeof(float) * 4);
            if (!sh.empty()) {
                const float k = clamp01(sh[(size_t)y * W + x]);
                float v[3] = {o[0] * (1 - k), o[1] * (1 - k), o[2] * (1 - k)};
                for (int i = 0; i < 3; ++i) o[i] = c.levels ? std::floor(v[i] * 255.f + 1e-3f) / 255.f : v[i];
            }
            if (!fm[(size_t)y * W + x]) continue;
            float p[4];
            double sx, sy;
            srcPos(x, y, fx, fy, fs, sx, sy);
            fetch(sx, sy, p);
            if (c.levels && !fgWhole)
                for (int i = 0; i < 3; ++i) p[i] = (float)std::floor(std::ceil(p[i] * 255.0 - 1e-3) * (127.0 / 128.0) + 1e-6) / 255.f;
            for (int i = 0; i < 4; ++i) o[i] = p[i] + o[i] * (1 - p[3]);
        }
    });
}

// Crop (OFX): the part of the frame in a rotated box is kept, outside is transparent. Anchor: which edge or corner of the box stays put when
// X and Y crop change (the box is X Crop x Y Crop of the frame centred on Location).
void cropFx(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double lx, ly;
    c.xy("Location", lx, ly);
    const double xs = clampd(c.d("XScale"), 0, 1), ys = clampd(c.d("YScale"), 0, 1), ang = c.d("Angle") * kPi / 180.0;
    const double op = clampd(c.d("Opacity"), 0, 1);
    const double cs = std::cos(ang), sn = std::sin(ang);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double u = x + 0.5 - lx * W, v = y + 0.5 - (1 - ly) * H;
            const double ru = u * cs + v * sn, rv = -u * sn + v * cs;
            float* o = c.out.at(x, y);
            if (std::fabs(ru) <= xs * W * 0.5 && std::fabs(rv) <= ys * H * 0.5) {
                const float* s = c.src.at(x, y);
                for (int k = 0; k < 4; ++k) o[k] = s[k] * (float)op;
            } else std::memset(o, 0, sizeof(float) * 4);
        }
    });
}

// Picture in Picture: the source scaled (and turned) into the frame at Location, over transparent.
void pictureInPicture(Ctx& c)
{
    // VEGAS places the picture by pixel INDICES (like Black Bar Fill); it exists only where the index lies inside the frame. Fixed Shape keeps
    // Scale in X and Y equal (whichever moved from its default wins); Free Form puts the corners on the four corner points.
    const int W = c.out.w, H = c.out.h;
    double lx, ly;
    c.xy("Location", lx, ly);
    const int mode = c.i("KeepProportions");
    double sx = std::max(0.01, c.d("Scale")), sy = std::max(0.01, c.d("DistortionScaleY"));
    if (mode == 0) { if (std::fabs(sx - 0.5) < 1e-9 && std::fabs(sy - 0.5) > 1e-9) sx = sy; else sy = sx; }
    const double ang = c.d("Angle") * kPi / 180.0, op = clampd(c.d("Opacity"), 0, 1);
    const int anchor = c.i("Anchor");
    const double ax = (anchor % 3) * 0.5, ay = 1.0 - (anchor / 3) * 0.5;          // the anchor inside the picture, from the bottom left
    const double cs = std::cos(ang), sn = std::sin(ang);
    const double shear = mode == 1 ? c.d("DistortionShear") : 0.0;
    double hm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (mode == 2) {
        double q[4][2];
        c.xy("CornerBL", q[0][0], q[0][1]);
        c.xy("CornerBR", q[1][0], q[1][1]);
        c.xy("CornerTR", q[2][0], q[2][1]);
        c.xy("CornerTL", q[3][0], q[3][1]);
        const double dx1 = q[1][0] - q[2][0], dx2 = q[3][0] - q[2][0], dx3 = q[0][0] - q[1][0] + q[2][0] - q[3][0];
        const double dy1 = q[1][1] - q[2][1], dy2 = q[3][1] - q[2][1], dy3 = q[0][1] - q[1][1] + q[2][1] - q[3][1];
        double g = 0, h = 0;
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(dx3) > 1e-12 || std::fabs(dy3) > 1e-12) {
            if (std::fabs(den) > 1e-12) { g = (dx3 * dy2 - dx2 * dy3) / den; h = (dx1 * dy3 - dx3 * dy1) / den; }
        }
        const double m[9] = {q[1][0] - q[0][0] + g * q[1][0], q[3][0] - q[0][0] + h * q[3][0], q[0][0],
                             q[1][1] - q[0][1] + g * q[1][1], q[3][1] - q[0][1] + h * q[3][1], q[0][1], g, h, 1};
        const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
        if (std::fabs(det) > 1e-12) {
            hm[0] = (m[4] * m[8] - m[5] * m[7]) / det; hm[1] = (m[2] * m[7] - m[1] * m[8]) / det; hm[2] = (m[1] * m[5] - m[2] * m[4]) / det;
            hm[3] = (m[5] * m[6] - m[3] * m[8]) / det; hm[4] = (m[0] * m[8] - m[2] * m[6]) / det; hm[5] = (m[2] * m[3] - m[0] * m[5]) / det;
            hm[6] = (m[3] * m[7] - m[4] * m[6]) / det; hm[7] = (m[1] * m[6] - m[0] * m[7]) / det; hm[8] = (m[0] * m[4] - m[1] * m[3]) / det;
        } else hm[0] = hm[4] = 0;
    }
    const double eps = 0.397;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double j = H - 1 - y;                                   // row from the bottom
            double px, pj;                                                // source position, index units, row from the bottom
            if (mode == 2) {
                const double X = (double)x / W, Y = j / H;
                const double w = hm[6] * X + hm[7] * Y + hm[8];
                const double u = (hm[0] * X + hm[1] * Y + hm[2]) / (std::fabs(w) < 1e-12 ? 1e-12 : w), v = (hm[3] * X + hm[4] * Y + hm[5]) / (std::fabs(w) < 1e-12 ? 1e-12 : w);
                px = u * W;
                pj = v * H;
            } else {
                const double u = x - lx * W, v = j - ly * H;
                double ru = u * cs - v * sn, rv = u * sn + v * cs;
                ru = (ru - shear * rv * 0.5) / sx;
                rv /= sy;
                px = ru + ax * W;
                pj = rv + ay * H;
            }
            float* o = c.out.at(x, y);
            const double py = (H - 1) - pj;
            if (!(px >= -eps && px <= W - 1 + eps && py >= -eps && py <= H - 1 + eps)) { std::memset(o, 0, sizeof(float) * 4); continue; }
            const double cx = clampd(px, 0, W - 1), cy = clampd(py, 0, H - 1);
            const int x0 = std::min((int)cx, W - 1), y0 = std::min((int)cy, H - 1), x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const float fu = (float)(cx - x0), fv = (float)(cy - y0);
            const float *a = c.src.at(x0, y0), *b = c.src.at(x1, y0), *cc = c.src.at(x0, y1), *d = c.src.at(x1, y1);
            const bool exact = std::fabs(px - std::floor(px + 0.5)) < 1e-7 && std::fabs(py - std::floor(py + 0.5)) < 1e-7;
            for (int k = 0; k < 4; ++k) {
                float v = ((a[k] * (1 - fu) + b[k] * fu) * (1 - fv) + (cc[k] * (1 - fu) + d[k] * fu) * fv);
                if (c.levels) {
                    // 8-bit VEGAS: a really interpolated value is rounded UP and one taken off, then the opacity floors
                    if (!exact) v = std::max(0.f, std::ceil(v * 255.f - 1e-3f) - 1.f) / 255.f;
                    v = floor8(v * (float)op);
                } else v *= (float)op;
                o[k] = v;
            }
        }
    });
}

void filmGrain(Ctx& c)
{
    const double amount0 = clampd(c.d("Amount"), 0, 1), gran = clampd(c.d("Granularity"), 0, 1), chroma = clampd(c.d("Chormaticity"), 0, 1);
    const double cellFull = (double)std::max(1, (int)std::lround(1 + gran * 3 * std::max(1.0, frameScaleFull(c) * 2)));
    const double cellHere = cellFull * c.scale;
    const int cell = c.scale >= 0.999 ? std::max(1, (int)std::lround(1 + gran * 3 * std::max(1.0, frameScale(c) * 2))) : std::max(1, (int)std::lround(cellHere));
    const double weaken = c.scale >= 0.999 ? 1.0 : std::min(1.0, cellHere);
    const double amount = amount0 * weaken;
    const uint32_t seed = (uint32_t)std::lround(c.time) * 2654435761u;
    auto rnd = [&](uint32_t x, uint32_t y, uint32_t k) {
        uint32_t h = x * 374761393u + y * 668265263u + k * 2246822519u + seed;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 32767.5f - 1.f;
    };
    perPixel(c, [&](float& r, float& g, float& b, int x, int y) {
        const uint32_t gx = (uint32_t)(x / cell), gy = (uint32_t)(y / cell);
        const float n0 = rnd(gx, gy, 0) * (float)amount * 0.35f;
        r += n0 + rnd(gx, gy, 1) * (float)(amount * chroma) * 0.2f;
        g += n0 + rnd(gx, gy, 2) * (float)(amount * chroma) * 0.2f;
        b += n0 + rnd(gx, gy, 3) * (float)(amount * chroma) * 0.2f;
        r = clamp01(r); g = clamp01(g); b = clamp01(b);
    });
}

// Film Effects: old film - grey (or tinted), grain, dust and scratches.
void filmEffects(Ctx& c)
{
    const double grain = clampd(c.d("Grain"), 0, 1), tint = clampd(c.d("Tint"), 0, 1);
    const Color col = c.color("Color");
    const bool grey = c.b("ConvertToGreyscale"), particles = c.b("EnableEnableParticles");
    const int type = c.i("EnableParticlesType");
    const double amount = clampd(c.d("EnableParticlesAmount"), 0, 1);
    const uint32_t frame = (uint32_t)std::lround(c.time);
    auto rnd = [&](uint32_t a, uint32_t b, uint32_t k) {
        uint32_t h = a * 374761393u + b * 668265263u + k * 2246822519u + frame * 3266489917u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 65535.f;
    };
    const float grainK = (float)c.scale;
    auto cellOf = [&](int full) { return (uint32_t)std::max(1, (int)std::lround(full * c.scale)); };
    const uint32_t c2 = cellOf(2), c3 = cellOf(3), c4 = cellOf(4), c40 = cellOf(40);
    perPixel(c, [&](float& r, float& g, float& b, int x, int y) {
        if (grey) {
            const float l = luma(r, g, b);
            r = l + ((float)col.r - 0.5f) * (float)tint; g = l + ((float)col.g - 0.5f) * (float)tint; b = l + ((float)col.b - 0.5f) * (float)tint;
        }
        const float n = (rnd((uint32_t)x, (uint32_t)y, 7) - 0.5f) * (float)grain * 0.5f * grainK;
        r += n; g += n; b += n;
        if (particles) {
            if (type == 4 && rnd((uint32_t)x / c3, 0, 11) < amount * 0.01) { r += 0.35f; g += 0.35f; b += 0.35f; }            // Scratches
            else if (type == 0 && rnd((uint32_t)x / c2, (uint32_t)y / c2, 13) > 1.0 - amount * 0.0015) { r *= 0.2f; g *= 0.2f; b *= 0.2f; }   // Dust
            else if (type == 1) { const float f = 1.f + (rnd(0, 0, 17) - 0.5f) * (float)amount * 0.3f; r *= f; g *= f; b *= f; }      // Flicker
            else if (type == 2 && rnd((uint32_t)x / c4, (uint32_t)y / c40, 19) > 1.0 - amount * 0.0006) { r *= 0.1f; g *= 0.1f; b *= 0.1f; }  // Hair
        }
        r = clamp01(r); g = clamp01(g); b = clamp01(b);
    });
}

// Vignette: the frame's edges darkened (or coloured, or made transparent) through Colour's alpha. Shape found from VEGAS's pictures: the distance
// from the centre in an ellipse with half axes Outer x half the diagonal x (1 + Aspect) / 2 across and 2 / (1 + Aspect) up, turned by Rotation,
// is 1 at the outer radius; Inner starts the ramp ((d - inner) / (1 - inner)); strength is that ramp to the power (10 / Feather)^2.
void vignette(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double cx, cy;
    c.xy("Center", cx, cy);
    const double outer = std::max(1e-6, c.d("Outer")), inner = clampd(c.d("Inner"), 0, 0.999999);
    const double aspect = std::max(0.0, c.d("Aspect")), rot = c.d("Rotation") * kPi / 180.0, feather = std::max(0.01, c.d("Feather"));
    const Color col = c.color("Color");
    const bool invert = c.b("Invert");
    const double cs = std::cos(rot), sn = std::sin(rot), px0 = cx * W, py0 = (1 - cy) * H;
    const double half = 0.5 * std::sqrt((double)W * W + (double)H * H) * outer, g = std::max(1e-3, (1.0 + aspect) * 0.5);
    const double ix = 1.0 / (half * g), iy = g / half, expo = (10.0 / feather) * (10.0 / feather);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double u = x + 0.5 - px0, v = y + 0.5 - py0;
            const double ru = (u * cs + v * sn) * ix, rv = (-u * sn + v * cs) * iy;
            const double d = std::sqrt(ru * ru + rv * rv);
            const double t = (d - inner) / (1.0 - inner);
            double m = t <= 0.0 ? 0.0 : std::min(1.0, std::pow(t, expo));
            if (invert) m = 1 - m;
            const float k = (float)(m * col.a);
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            for (int i = 0; i < 3; ++i) o[i] = s[i] * (1 - k) + (float)(i == 0 ? col.r : i == 1 ? col.g : col.b) * k * s[3];
            o[3] = s[3];
        }
    });
}

// TV Simulator: scan lines, an aperture grill, line and vertical sync trouble, phosphor glow and static.
void tvSimulator(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double zoom = 1.0 + clampd(c.d("DetailZoom"), 0, 1) * 1.5, grill = clampd(c.d("ApertureGrill"), 0, 1), inter = clampd(c.d("Interlacing"), 0, 1);
    const double lineSync = clampd(c.d("LineSync"), 0, 1), vSync = clampd(c.d("VerticalSync"), 0, 1), scan = clampd(c.d("ScanPhasing"), 0, 1);
    const double phos = clampd(c.d("Phosphorescence"), 0, 1), stat = clampd(c.d("Static"), 0, 1);
    const double t = c.time;
    auto rnd = [&](uint32_t a, uint32_t b) {
        uint32_t h = a * 374761393u + b * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 65535.f;
    };
    Buf glow = c.src;
    if (phos > 0.01) gaussianBlur(glow, 2.0 * frameScale(c) + c.px(1), 2.0 * frameScale(c) + c.px(1));
    forRows(H, W, [&](int y) {
        const double yFull = y / c.scale;
        const double wob = (1.0 - lineSync) * 0.03 * W * std::sin(yFull * 0.07 + t * 0.9) * (rnd((uint32_t)yFull, (uint32_t)t) > 0.8 ? 3 : 1);
        for (int x = 0; x < W; ++x) {
            const double roll = (1.0 - vSync) * H * std::fmod(t * 0.07, 1.0);
            double sy = std::fmod(y + roll, (double)H);
            const double zx = (x + 0.5 - W * 0.5) / zoom + W * 0.5 + wob, zy = (sy + 0.5 - H * 0.5) / zoom + H * 0.5;
            float p[4], g[4];
            c.src.sampleClamped(zx, zy, p);
            glow.sampleClamped(zx, zy, g);
            float* o = c.out.at(x, y);
            const float k = (float)phos * 0.5f;
            for (int i = 0; i < 3; ++i) o[i] = p[i] + (g[i] * 1.1f - p[i]) * k;
            o[2] += (float)phos * 0.04f * g[3];
            const int col = x % 3;
            if (c.scale >= 0.999) {
                for (int i = 0; i < 3; ++i) o[i] *= 1.f - (float)grill * 0.5f * (col == i ? 0.f : 1.f);
                if (grill > 0) for (int i = 0; i < 3; ++i) o[i] *= 1.f + (float)grill * 0.6f * (col == i ? 1.f : 0.f);
            } else {
                const float own = 1.f + (float)grill * 0.6f, other = 1.f - (float)grill * 0.5f;
                const float mean = grill > 0 ? (own + 2.f * other) / 3.f : 1.f;
                for (int i = 0; i < 3; ++i) o[i] *= mean;
            }
            const float line = c.scale >= 0.999 ? 1.f - (float)inter * 0.5f * (y % 2 ? 1.f : 0.f) : 1.f - (float)inter * 0.25f;
            const float band = 1.f - (float)scan * 0.25f * (0.5f + 0.5f * (float)std::sin(yFull * 0.02 - t * 0.4));
            for (int i = 0; i < 3; ++i) o[i] *= line * band;
            if (stat > 0) { const float n = (rnd((uint32_t)x, (uint32_t)(y + t * 7)) - 0.5f) * (float)stat * 0.8f * (float)c.scale; for (int i = 0; i < 3; ++i) o[i] += n * g[3]; }
            o[3] = p[3];
            for (int i = 0; i < 3; ++i) o[i] = std::min(std::max(o[i], 0.f), o[3]);
        }
    });
}

// Radial Pixelate: VEGAS's CPU processor (rows bottom first, 8-bit). The picture is unrolled about the centre into a polar picture of the same size
// (column = angle from -pi, row = radius out to 2 sqrt 2 in half frames), read at the nearest pixel below, averaged down in whole-pixel blocks
// (sums truncated) to (1 - Strength) / 2 * width + 1 blocks across (Radial) or as many down (Circular), spread back and rolled up again.
// Strength 0 changes nothing.
void radialPixelate(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    const double st = c.d("Strength");
    const int type = c.i("Type");
    if (W <= 0 || H <= 0 || std::fabs((float)st) <= 1e-6f) { copyThrough(c); return; }
    const bool q8 = c.levels;
    const double k = (1.0 - st) * 0.5;
    int gw = (int)(W * k + 1.0), gh = (int)(H * k + 1.0);
    if (gw > W) gw = W;
    if (gh > H) gh = H;
    if (type == 0) gh = H; else gw = W;
    gw = std::max(1, gw); gh = std::max(1, gh);
    const float c0 = (float)(pcx * 2.0 - 1.0), c1 = (float)(pcy * 2.0 - 1.0);
    const float Wf = (float)W, Hf = (float)H;
    // VEGAS's rows (bottom first) of the source; values in levels
    auto srcAt = [&](int x, int y) { return c.src.at(x, H - 1 - y); };
    std::vector<float> pol((size_t)W * H * 4);
    forRows(H, W, [&](int j) {
        const float rr = ((float)j / Hf) * 2.8284270763397217f;
        for (int i = 0; i < W; ++i) {
            const float a = (float)((double)(((float)i / Wf) * 2.f - 1.f) * 3.141592653589793);
            float px = (float)std::sin((double)a) * rr, py = (float)std::cos((double)a) * rr;      // correctly rounded, as VEGAS's sinf/cosf
            px = ((px + c0) * 0.5f + 0.5f) * Wf; py = ((py + c1) * 0.5f + 0.5f) * Hf;
            if (px < 0.f) px = 0.f;
            if (!(px < Wf)) px = (float)(W - 1);
            if (py < 0.f) py = 0.f;
            if (!(py < Hf)) py = (float)(H - 1);
            const float* p = srcAt((int)std::floor(px), (int)std::floor(py));
            float* o = &pol[((size_t)j * W + i) * 4];
            for (int ch = 0; ch < 4; ++ch) o[ch] = q8 ? (float)(int)std::lround(std::min(std::max(p[ch], 0.f), 1.f) * 255.f) : p[ch] * 255.f;
        }
    });
    const int sx = (int)((double)W / gw * 16384.0), sy = (int)((double)H / gh * 16384.0);
    const int bw = std::max(1, sx / 16384), bh = std::max(1, sy / 16384);
    std::vector<float> small((size_t)gw * gh * 4);
    const double inv = 1.0 / ((double)bw * bh);
    for (int gy = 0; gy < gh; ++gy) {
        const int y0 = (int)(((long long)gy * sy) / 16384);
        for (int gx = 0; gx < gw; ++gx) {
            const int x0 = (int)(((long long)gx * sx) / 16384);
            double sum[4] = {0, 0, 0, 0};
            for (int yy = y0; yy < std::min(H, y0 + bh); ++yy)
                for (int xx = x0; xx < std::min(W, x0 + bw); ++xx) {
                    const float* p = &pol[((size_t)yy * W + xx) * 4];
                    for (int ch = 0; ch < 4; ++ch) sum[ch] += p[ch];
                }
            float* o = &small[((size_t)gy * gw + gx) * 4];
            for (int ch = 0; ch < 4; ++ch) {
                const double v = sum[ch] * inv;
                // VEGAS hands the level on as level / 255 and back as a truncated level * 255, so a few levels come back one less
                o[ch] = q8 ? (float)std::min(255, (int)(((double)(int)v * 0.00392156862745098) * 255.0)) : (float)v;
            }
        }
    }
    const int tx = (int)((double)W / gw * 16384.0), ty = (int)((double)H / gh * 16384.0);
    for (int gy = 0; gy < gh; ++gy) {
        const int y0 = (int)(((long long)gy * ty) / 16384), y1 = gy == gh - 1 ? H : (int)(((long long)gy * ty + ty) / 16384);
        for (int gx = 0; gx < gw; ++gx) {
            const int x0 = (int)(((long long)gx * tx) / 16384), x1 = gx == gw - 1 ? W : (int)(((long long)gx * tx + tx) / 16384);
            const float* col = &small[((size_t)gy * gw + gx) * 4];
            for (int yy = std::max(0, y0); yy < std::min(H, y1); ++yy)
                for (int xx = std::max(0, x0); xx < std::min(W, x1); ++xx) std::memcpy(&pol[((size_t)yy * W + xx) * 4], col, sizeof(float) * 4);
        }
    }
    forRows(H, W, [&](int j) {
        const float Y = (((float)j / Hf) * 2.f - 1.f) - c1;
        for (int i = 0; i < W; ++i) {
            const float X = (((float)i / Wf) * 2.f - 1.f) - c0;
            const float r = std::sqrt(X * X + Y * Y);
            float ri = (r / 2.8284270763397217f) * Hf;
            const float ang = (float)std::atan2((double)X, (double)Y);
            float u = (float)((double)ang / 3.141592653589793 * 0.5 + 0.5) * Wf;
            u = std::max(0.f, u);
            if (!(u < Wf)) u = (float)(W - 1);
            ri = std::max(0.f, ri);
            if (!(ri < Hf)) ri = (float)(H - 1);
            const float* p = &pol[((size_t)(int)std::floor(ri) * W + (int)std::floor(u)) * 4];
            float* o = c.out.at(i, H - 1 - j);
            for (int ch = 0; ch < 4; ++ch) o[ch] = p[ch] / 255.f;
        }
    });
}

// Mask Generator: a grey mask made from the frame's luminance, a colour channel or its alpha.
void maskGenerator(Ctx& c)
{
    // VEGAS: Luminance is the plain average of R,G,B; Invert comes BEFORE the level mapping; a degenerate in-range (Low in >= High in) is
    // a hard step; the mask is written to all four channels (alpha included).
    const int type = c.i("Type");
    const bool invert = c.b("Invert");
    const float li = (float)c.d("LowIn"), hi = (float)c.d("HighIn"), lo = (float)c.d("LowOut"), ho = (float)c.d("HighOut");
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            float v = type == 0 ? (r + g + b) * (1.f / 3.f) : type == 1 ? r : type == 2 ? g : type == 3 ? b : a;
            if (c.levels) v = std::floor(v * 255.f + 0.5f) / 255.f;
            if (invert) v = 1.f - v;
            float t = hi > li ? clamp01((v - li) / (hi - li)) : (v > li ? 1.f : 0.f);
            v = lo + (ho - lo) * t;
            float* o = c.out.at(x, y);
            o[0] = o[1] = o[2] = o[3] = clamp01(v);
        }
    });
}

// Timecode: the frame's time burnt in, white digits in a black box. VEGAS uses a Windows font; here the digits are a small bold shape (3 x 5
// cells smoothed up). Box, place and size follow VEGAS (measured at 320 x 180, scaled with the frame): text 9 px high at Height 1, digit advance 7,
// colon or point 3.5, box adds 3.5 each side and sits 29 / 28 px from the sides, 16 from top and bottom.
const char* const kDigits[12] = {
    "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
    "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111", "000010000010000", "000000000000010"};

// the text of a format; the time is in frames of the clip (c.time) at c.fps
std::string timecodeText(const Ctx& c, int fmt)
{
    const double fps = c.fps > 0 ? c.fps : 30.0;
    const double secs = c.time / fps;
    const long long total = (long long)std::floor(secs + 1e-9);
    const double frac = secs - (double)total;
    char buf[64];
    auto hms = [&](const char* tail, int v) {
        std::snprintf(buf, sizeof buf, tail, total / 3600, total / 60 % 60, total % 60, v);
    };
    switch (fmt) {
    case 0: hms("%02lld:%02lld:%02lld.%03d", (int)std::floor(frac * 1000 + 1e-6)); break;
    case 1: std::snprintf(buf, sizeof buf, "%.3f", secs); break;
    case 2: hms("%02lld:%02lld:%02lld.%02d", (int)std::floor(frac * fps + 1e-6)); break;
    case 3: std::snprintf(buf, sizeof buf, "%.3f", c.time); break;
    default: {
        const double rate = fmt == 6 ? 23.976 : fmt == 7 ? 24.0 : fmt == 8 ? 25.0 : fmt == 9 || fmt == 10 ? 29.97 : fmt == 11 ? 30.0 : fps;
        const long long fr = (long long)std::floor(secs * rate + 1e-6);
        const long long ri = (long long)std::lround(rate);
        const long long ss = fr / ri;
        std::snprintf(buf, sizeof buf, fmt == 10 ? "%02lld:%02lld:%02lld;%02lld" : "%02lld:%02lld:%02lld:%02lld", ss / 3600, ss / 60 % 60, ss % 60, fr % ri);
    }
    }
    return buf;
}

// how much wider the text gets with the Width setting (measured at Width 0.5, 1, 1.25 and 2); a wider font is only made up to a limit
// that grows with the height (1.58 at Height 1, 1.94 at Height 2)
double timecodeWidthRatio(double w, double h)
{
    static const double xs[4] = {0.0, 0.5, 1.0, 1.25}, ys[4] = {0.0, 0.49, 1.0, 1.18};
    w = clampd(w, 0.0, 2.0);
    double r;
    if (w <= 1.25) {
        r = ys[3];
        for (int i = 0; i < 3; ++i)
            if (w <= xs[i + 1]) { r = ys[i] + (ys[i + 1] - ys[i]) * (w - xs[i]) / (xs[i + 1] - xs[i]); break; }
    } else {
        r = 1.18 + (w - 1.25) * 0.9;
    }
    return std::min(r, 1.58 + 0.36 * (h - 1.0));
}

void timecodeFx(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    copyThrough(c);
    const std::string s = timecodeText(c, c.i("TimecodeFormat"));
    const double u = W / 320.0;                                    // one reference pixel
    const double hs = std::max(0.05, c.d("Height")), wr = timecodeWidthRatio(c.d("Width"), hs);
    const double digitW = 7.0 * u * wr, sepW = 3.5 * u * wr, glyphH = 9.0 * u * hs, pad = 3.5 * u * wr;
    double textW = 0;
    for (char ch : s) textW += (ch >= '0' && ch <= '9') ? digitW : sepW;
    const double boxW = textW + 2 * pad, boxH = 12.5 * u * hs;
    const int halign = c.i("HorizontalAlignment"), valign = c.i("VerticalAlignment");
    const double bx = halign == 0 ? 29.0 * u : halign == 1 ? (W - boxW) * 0.5 : W - 28.0 * u - boxW;
    const double by = valign == 0 ? 16.0 * u : valign == 1 ? (H - boxH) * 0.5 : H - 16.0 * u - boxH;
    const int X0 = std::max(0, (int)std::lround(bx)), X1 = std::min(W, (int)std::lround(bx + boxW));
    const int Y0 = std::max(0, (int)std::lround(by)), Y1 = std::min(H, (int)std::lround(by + boxH));
    for (int y = Y0; y < Y1; ++y)
        for (int x = X0; x < X1; ++x) { float* o = c.out.at(x, y); o[0] = o[1] = o[2] = 0.f; o[3] = 1.f; }
    const double gy0 = by + (boxH - glyphH) * 0.5 + 0.5 * u * hs;
    double pen = bx + pad;
    for (char ch : s) {
        const bool digit = ch >= '0' && ch <= '9';
        const double adv = digit ? digitW : sepW;
        const char* shape = kDigits[digit ? ch - '0' : (ch == '.' ? 11 : 10)];
        const double gw = adv * 0.82, gx0 = pen + adv * 0.09;
        if (ch != ' ') {
            const int xa = std::max(0, (int)std::floor(gx0)), xb = std::min(W, (int)std::ceil(gx0 + gw));
            const int ya = std::max(0, (int)std::floor(gy0)), yb = std::min(H, (int)std::ceil(gy0 + glyphH));
            for (int y = ya; y < yb; ++y)
                for (int x = xa; x < xb; ++x) {
                    const double cx = (x + 0.5 - gx0) / gw * 3.0 - 0.5, cy = (y + 0.5 - gy0) / glyphH * 5.0 - 0.5;
                    if (cx < -0.5 || cx > 2.5 || cy < -0.5 || cy > 4.5) continue;
                    double cov = 0;
                    const int ix = (int)std::floor(cx), iy = (int)std::floor(cy);
                    const double fx = cx - ix, fy = cy - iy;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int gx = std::min(2, std::max(0, ix + dx)), gy = std::min(4, std::max(0, iy + dy));
                            const double wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                            cov += wgt * (shape[gy * 3 + gx] == '1' ? 1.0 : 0.0);
                        }
                    cov = std::min(1.0, cov * 1.15);
                    float* o = c.out.at(x, y);
                    for (int k = 0; k < 3; ++k) o[k] = std::max(o[k], (float)cov);
                }
        }
        pen += adv;
    }
}

// Layer Dimensionality: a shadow, glow and emboss on frames with transparency.
void layerDimensionality(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const int shadowType = c.i("ShadowType"), glowType = c.i("GlowType");
    std::vector<float> alpha((size_t)W * H);
    for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = c.src.px[i * 4 + 3];
    c.out.px = c.src.px;
    // Foreground Opacity thins the frame itself (VEGAS: 0 gives a fully transparent picture)
    const float fgOp = (float)clampd(c.d("ForegroundOpacity"), 0, 1);
    if (fgOp < 1.f) for (auto& v : c.out.px) v *= fgOp;
    if (shadowType != 0) {
        const double off = c.d("ShadowHeight") * W * 0.15, blur = c.d("ShadowBlur") * W * 0.04 + c.px(0.3);
        std::vector<float> sh((size_t)W * H, 0.f);
        const int ox = (int)std::lround(off * 0.7), oy = (int)std::lround(off * 0.7);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                // (VEGAS repeats the frame's edge: an opaque frame's shadow covers the whole frame)
                const int sx = std::min(W - 1, std::max(0, x - ox)), sy = std::min(H - 1, std::max(0, y - oy));
                sh[(size_t)y * W + x] = alpha[(size_t)sy * W + sx];
            }
        gaussianBlurPlane(sh, W, H, blur, blur);
        // under the frame: where the frame is not opaque (the shadow's alpha put under the frame's)
        const float amount = (float)clampd(c.d("ShadowAmount"), 0, 1);
        for (size_t i = 0; i < sh.size(); ++i) {
            float* o = &c.out.px[i * 4];
            const float k = sh[i] * amount * (1 - o[3]);
            if (fgOp < 1.f && o[3] + k > 0.f) {
                // VEGAS with the foreground thinned: the thinned colour weighted by the foreground cover once more and divided by the
                // cover of the whole (its "mid" picture: colour src / 3 for Opacity 0.5 over a half shadow)
                const float af = o[3], aout = af + k;
                for (int q = 0; q < 3; ++q) o[q] = o[q] * af / aout;
            }
            o[3] += k;
        }
    }
    if (glowType != 0) {
        const Color gc = c.color("GlowColor");
        const double amt = clampd(c.d("GlowAmount"), 0, 1), rad = amt * W * 0.03 + c.px(0.5);
        std::vector<float> g = alpha;
        gaussianBlurPlane(g, W, H, rad, rad);
        const bool inner = glowType == 2 || glowType == 1;
        const float cr = (float)gc.r, cg = (float)gc.g, cb = (float)gc.b;
        for (size_t i = 0; i < g.size(); ++i) {
            const float a = inner ? std::max(0.f, g[i] - alpha[i]) : std::max(0.f, alpha[i] - g[i]) * 2;
            if (a <= 0.f) continue;
            float* o = &c.out.px[i * 4];
            o[0] = cr * a + o[0] * (1 - a); o[1] = cg * a + o[1] * (1 - a); o[2] = cb * a + o[2] * (1 - a); o[3] = a + o[3] * (1 - a);
        }
    }
    const double fg = c.d("ForegroundAmount");
    if (std::fabs(fg) > 0.001) {
        const double lx = c.d("LightIntensity") * 0.5 + 0.1;
        double Lx, Ly, Lz;
        c.xyz("LightLocation", Lx, Ly, Lz);
        const double mx = std::max(std::fabs(Lx), std::fabs(Ly));
        const double ox = mx > 1e-6 ? Lx / mx : -1.0, oy = mx > 1e-6 ? -Ly / mx : -1.0;      // (the frame's y runs down)
        const double heightGain = 3.0 / clampd(Lz, 0.3, 30.0);
        std::vector<float> sm = alpha;
        const double sigma = c.px(1.5 + c.d("EmbossRoundness") * 4);
        gaussianBlurPlane(sm, W, H, sigma, sigma);
        struct Axis { int i0, i1; float t; };
        auto axisOf = [](double pos, int n) {
            pos = clampd(pos, 0, n - 1);
            const int i0 = (int)pos;
            return Axis{i0, std::min(n - 1, i0 + 1), (float)(pos - i0)};
        };
        std::vector<Axis> colA((size_t)W), colB((size_t)W), rowA((size_t)H), rowB((size_t)H);
        for (int x = 0; x < W; ++x) { colA[(size_t)x] = axisOf(x + ox, W); colB[(size_t)x] = axisOf(x - ox, W); }
        for (int y = 0; y < H; ++y) { rowA[(size_t)y] = axisOf(y + oy, H); rowB[(size_t)y] = axisOf(y - oy, H); }
        const float gain = (float)(fg * lx * 3 * c.scale * heightGain);
        auto at = [&](const Axis& ax, const Axis& ay) {
            const float* r0 = &sm[(size_t)ay.i0 * W];
            const float* r1 = &sm[(size_t)ay.i1 * W];
            const float a = r0[ax.i0] * (1 - ax.t) + r0[ax.i1] * ax.t, b = r1[ax.i0] * (1 - ax.t) + r1[ax.i1] * ax.t;
            return a * (1 - ay.t) + b * ay.t;
        };
        if (H > 2)
            parallelRows(H - 2, (size_t)W * 8, [&](int first, int end) {
                for (int y = 1 + first; y < 1 + end; ++y)
                    for (int x = 1; x < W - 1; ++x) {
                        const float slope = (at(colA[(size_t)x], rowA[(size_t)y]) - at(colB[(size_t)x], rowB[(size_t)y])) * gain;
                        float* o = c.out.at(x, y);
                        for (int k = 0; k < 3; ++k) o[k] = std::min(std::max(o[k] + slope * o[3], 0.f), o[3]);
                    }
            });
    }
}

// Deform: squeeze, shear and curve the frame's edges.
void deform(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double amount = c.d("Amount"), sl = c.d("SqueezeLeft") * amount, sr = c.d("SqueezeRight") * amount, st = c.d("SqueezeTop") * amount, sb = c.d("SqueezeBottom") * amount;
    const double sh = (c.d("ShearHorizontal") - 1.0) * amount, sv = c.d("ShearVertical") * amount;
    const double cl = c.d("CurveLeft") * amount, cr = c.d("CurveRight") * amount, ct = c.d("CurveTop") * amount, cb = c.d("CurveBottom") * amount;
    const Color bgc = c.color("Background");
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            double u = (x + 0.5) / W, v = (y + 0.5) / H;
            const double sq = (1 - v) * st + v * sb;                     // horizontal squeeze by row
            const double sqv = (1 - u) * sl + u * sr;
            double su = (u - 0.5) / std::max(0.05, 1 - sq) + 0.5, sv2 = (v - 0.5) / std::max(0.05, 1 - sqv) + 0.5;
            su += sh * (v - 0.5) * 1.0; sv2 += sv * (u - 0.5);
            su += (ct * (1 - v) + cb * v) * 0.5 * std::sin(kPi * v) * 0.5;
            sv2 += (cl * (1 - u) + cr * u) * 0.5 * std::sin(kPi * u) * 0.5;
            float* o = c.out.at(x, y);
            if (su < 0 || su > 1 || sv2 < 0 || sv2 > 1) { put(o, bgc); continue; }
            c.src.sample(su * W, sv2 * H, o);
        }
    });
}

// Bump Map: VEGAS's plug-in step for step (its OpenCL kernel and host code). The normal is a 3x3 Prewitt gradient of the chosen channel (premultiplied 0..1) times 255 * BumpHeight^2 / 16 * max(W, H) / 640
// (negative when black is high). Positions: the larger side spans -1..1, y down; the light's place is Source with y flipped. Directional: from the direction of Source; Omnidirectional: a point at Source,
// falling off as 2 / distance; Spotlight: that point aimed along Source - Destination, cone cos(Focus * pi / 4) with a soft edge. Phong (viewer straight on): k = Intensity * 8 * ((0.6 - 0.2 Shininess) N.L +
// 0.5 Shininess max(R.V, 0.01)^(4 + 8 Shininess)) * attenuation + 2 Ambience, floored at 0, multiplies the colour (diffuse not clamped: slopes facing away darken below ambient). Alpha is kept.
void bumpMapRender(Ctx& c, const char* srcName, const char* dstName)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double psx, psy, psz, pdx, pdy;
    c.xyz(srcName, psx, psy, psz);
    c.xy(dstName, pdx, pdy);
    const int type = std::max(0, std::min(2, c.i("Type")));
    const int chan = std::max(0, std::min(3, c.i("BumpChannel")));
    const float sx = (float)psx, sy = (float)-psy, sz = (float)psz;
    float dx = (float)pdx, dy = (float)-pdy;
    if (type != 1) dx = dy = 0.f;
    float ax = sx - dx, ay = sy - dy, az = sz;
    {
        const float l = std::sqrt(ax * ax + ay * ay + az * az);
        if (l > 1e-7f) { ax /= l; ay /= l; az /= l; }
        else { ax = 0.f; ay = 0.f; az = 1.f; }
    }
    const float mx = (float)std::max(W, H);
    const double bh = c.d("BumpHeight");
    const float hs = (float)((double)((float)(bh * bh) * 0.0625f) * ((double)mx * 0.0015625)) * (c.b("WhiteIsHigh") ? 1.f : -1.f) * 255.f;
    const float amb2 = (float)c.d("Ambience") * 2.f, i8 = (float)c.d("Intensity") * 8.f;
    const float shin = (float)c.d("Shininess");
    const float kd = 0.6f - shin * 0.2f, ks = shin * 0.5f, power = shin * 8.f + 4.f;
    const float cosCut = std::cos((float)c.d("Focus") * 0.7853981852531433f);
    const float invCut = 1.f / std::max(1.f - cosCut, 1e-5f);
    const float unit = 2.f / mx, cx = W * 0.5f, cy = H * 0.5f;
    auto hAt = [&](int x, int y) { return c.src.at(x, y)[chan]; };
    forRows(H, W, [&](int y) {
        // VEGAS's rows go bottom first: its "y + 1" is the row above
        const int up = std::max(0, y - 1), dn = std::min(H - 1, y + 1);
        for (int x = 0; x < W; ++x) {
            float gx, gy;
            if (W == 1) { gx = 0.f; gy = 3.f * (hAt(x, up) - hAt(x, dn)); }
            else if (x == 0) {
                gx = (hAt(0, dn) - hAt(1, dn)) + (hAt(0, y) - hAt(1, y)) + (hAt(0, up) - hAt(1, up));
                gy = (hAt(0, up) - hAt(0, dn)) + 2.f * (hAt(1, up) - hAt(1, dn));
            } else if (x == W - 1) {
                gx = (hAt(x - 1, dn) - hAt(x, dn)) + (hAt(x - 1, y) - hAt(x, y)) + (hAt(x - 1, up) - hAt(x, up));
                gy = (hAt(x - 1, up) - hAt(x - 1, dn)) + 2.f * (hAt(x, up) - hAt(x, dn));
            } else {
                gx = (hAt(x - 1, dn) - hAt(x + 1, dn)) + (hAt(x - 1, y) - hAt(x + 1, y)) + (hAt(x - 1, up) - hAt(x + 1, up));
                gy = (hAt(x - 1, up) - hAt(x - 1, dn)) + (hAt(x + 1, up) - hAt(x + 1, dn)) + (hAt(x, up) - hAt(x, dn));
            }
            float nx = gx * hs, ny = gy * hs, nz = 1.f;
            const float nl = std::sqrt(nx * nx + ny * ny + 1.f);
            nx /= nl; ny /= nl; nz /= nl;
            float lx = ax, ly = ay, lz = az, att = 1.f;
            if (type != 0) {
                const float px = (x - cx) * unit, py = (y - cy) * unit;
                lx = sx - px; ly = sy - py; lz = sz;
                float d = std::sqrt(lx * lx + ly * ly + lz * lz), inv;
                if (d > 0.f) { inv = 1.f / d; lx *= inv; ly *= inv; lz *= inv; }
                else { inv = 100.f; lx = 0.f; ly = 0.f; lz = 1.f; }
                att = 1.f;
                if (type == 1) {
                    float a = (lx * ax + ly * ay + lz * az - cosCut) * invCut;
                    a = std::min(std::max(a, 0.f), 1.f);
                    att = a * a * 2.f;
                }
                att *= inv * 2.f;
            }
            const float ndl = nx * lx + ny * ly + nz * lz;
            const float rz = nz * 2.f * ndl - lz;
            const float spec = std::pow(std::max(rz, 0.01f), power);
            const float k = std::max(0.f, i8 * (kd * ndl + ks * spec) * att + amb2);
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            o[0] = s[0] * k; o[1] = s[1] * k; o[2] = s[2] * k; o[3] = s[3];
        }
    });
}

void bumpMap(Ctx& c) { bumpMapRender(c, "LocationSrc", "LocationDst"); }

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
