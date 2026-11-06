// Built-in creative effects (Black Bar Fill, Crop, Picture in Picture, film looks, Vignette, TV Simulator, Timecode, Deform...); see builtins_more.cpp.
#include "../core/rmath.h"
#include "vfxpipcrop.h"
#include "vfxlayerdim.h"
#include "vktext.h"
#include "vkfilters.h"
#include "vkfx2.h"
#include <cstring>
#include <cstdlib>
#include "builtins_more_internal.h"
#include "builtins_fast.h"
#include "builtins_deriche.h"
#include "builtins_cgblur.h"
#include "vegas8.h"
#include "vfxrand.h"
#include "vfxdeform.h"
#include "vfxfilm.h"
#include <map>
#include <memory>
#include <mutex>
#include "vfxtv.h"
#include "vfxtimecode.h"
#include "vfxpolar.h"
#include "vfxvignette.h"
#include "builtins_vec.h"
#include "../core/base.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ creative
// Black Bar Fill: the frame (scaled) over a blurred, enlarged copy of itself, with a soft shadow. Measured on VEGAS: positions are pixel INDICES, not centres (source index = (x - location*W)/scale + W/2,
// rows from the bottom); a picture exists only where that index lies inside the frame (hard edge); the shadow is the blurred foreground mask (sigma 9.4 px per unit of Shadow Blur at 320 wide) and darkens
// the colour only; a resampled or blurred picture comes out slightly darker with alpha 254 (8-bit).
void bbfSetup(Ctx& c, int W, int H, BbfSetup& o)
{
    c.xy("Foreground Location", o.fx, o.fy);
    c.xy("Background Location", o.bx, o.by);
    o.fs = std::max(0.01, c.d("Foreground Scale")); o.bs = std::max(0.01, c.d("Background Scale"));
    auto isWhole = [&](double z, double cx, double cy) {
        const double sx = (0 - cx * W) / z + W * 0.5;
        const double sj = ((H - 1 - 0) - cy * H) / z + H * 0.5;
        const double sy = (H - 1) - sj;
        return std::fabs(z - 1) < 1e-9 && std::fabs(sx - std::floor(sx + 0.5)) < 1e-6 && std::fabs(sy - std::floor(sy + 0.5)) < 1e-6;
    };
    o.blur = clampd(c.d("Background Blur"), 0, 1) * 0.3213 * W;
    o.bgWhole = isWhole(o.bs, o.bx, o.by);
    o.fgWhole = isWhole(o.fs, o.fx, o.fy);
    o.bgBlurred = o.blur > c.px(0.05);
    o.bf = (c.levels && o.blur < 24) ? 1 : softBlurFactor(o.blur);
    o.shadow = clampd(c.d("Foreground Shadow Blur"), 0, 1);
    o.sigma = o.shadow * 0.029375 * W;
    o.sf = (c.levels && o.sigma < 12) ? 1 : softBlurFactor(o.sigma);
}

// Black Bar Fill on 8-bit pictures, step for step as VEGAS's processor does it (bytes, memory order: the bottom row first):
//  1. the background: the frame through a 3x3 matrix in single precision (scale, then the location), bilinear with weights of 1/1024 cut to whole numbers (so the picture comes out a hair
//     dark), nothing where the position lies outside (-0.4, size - 0.6); 2. blurred by the Gaussian of the Vfx1 plug-ins (size = the larger side * Background Blur); 3. the foreground the same way;
//  4. its shadow: the foreground's alpha, blurred in floats (size = Shadow Blur * 0.6 * the larger side / 8), black; 5. the foreground over the shadow ("over" in floats, straight colours),
//  rounded to bytes; 6. that over the blurred background with weights alpha / 255 (the background's own alpha weighs too, so a 254 background loses a level).
static void bbfTable(float* t)
{
    const float k = 0.003921568859368563f;
    for (int j = 0; j < 128; ++j) {
        t[j] = (float)j * k;
        t[255 - j] = 1.0f - t[j];
    }
}
static void bbfMatrix(float* m, int W, int H, double lx, double ly, double scale)
{
    const float sx = (float)(1.0 / scale), sy = (float)(1.0 / scale);
    const float negW = -(float)((double)W * lx), negH = -(float)((double)H * ly);
    const float hw = (float)W * 0.5f, hh = (float)H * 0.5f;
    const float C = 1.f, S = 0.f, T = 0.f, a = 1.f, b = 1.f;
    const float m0 = (a * sx) * C;
    const float m1 = -((b * sx) * S);
    const float m2 = ((negH * m1) + (negW * m0)) + hw;
    const float m3 = ((C * T) + S) * (sy * a);
    const float m4 = (C - S * T) * (sy * b);
    const float m5 = ((negH * m4) + (negW * m3)) + hh;
    m[0] = m0; m[1] = m1; m[2] = m2; m[3] = m3; m[4] = m4; m[5] = m5; m[6] = 0.f; m[7] = 0.f; m[8] = 1.f;
}
static void bbfSample(const std::vector<uint8_t>& s, std::vector<uint8_t>& o, int W, int H, const float* m)
{
    o.assign((size_t)W * H * 4, 0);
    const float lo = -0.4f, hiX = (float)W - 0.6f, hiY = (float)H - 0.6f;
    forRows(H, W * 16, [&](int j) {
        const float yf = (float)j;
        const float x9 = yf * m[1] + m[2], x7 = yf * m[4] + m[5], x5 = yf * m[7] + m[8];
        for (int i = 0; i < W; ++i) {
            const float xf = (float)i;
            const float den = xf * m[6] + x5;
            const float inv = 1.0f / den;
            const float xp = (xf * m[0] + x9) * inv, yp = (xf * m[3] + x7) * inv;
            if (!(xp > lo) || !(xp < hiX) || !(yp > lo) || !(yp < hiY)) continue;
            const int ix = (int)xp, iy = (int)yp;
            float fx = xp - (float)ix, fy = yp - (float)iy;
            if (!(fx > 0.f)) fx = 0.f;
            if (!(fy > 0.f)) fy = 0.f;
            const float omx = 1.0f - fx, omy = 1.0f - fy;
            const int w00 = (int)((omx * omy) * 1024.0f), w01 = (int)((omx * fy) * 1024.0f), w10 = (int)((omy * fx) * 1024.0f), w11 = (int)((fy * fx) * 1024.0f);
            const bool lastX = ix == W - 1, lastY = iy == H - 1;
            const size_t idx = ((size_t)iy * W + ix) * 4;
            for (int k = 0; k < 4; ++k) {
                const int p00 = s[idx + k];
                const int p10 = lastX ? p00 : s[idx + 4 + k];
                const int p01 = lastY ? p00 : s[idx + (size_t)W * 4 + k];
                const int p11 = (lastX || lastY) ? p01 : s[idx + ((size_t)W + 1) * 4 + k];
                o[((size_t)j * W + i) * 4 + k] = (uint8_t)((p11 * w11 + p01 * w01 + p10 * w10 + p00 * w00) >> 10);
            }
        }
    });
}
void bbf8Setup(Ctx& c, int W, int H, Bbf8Setup& o)
{
    double fx, fy, bx, by;
    c.xy("Foreground Location", fx, fy);
    c.xy("Background Location", bx, by);
    const double fs = c.d("Foreground Scale"), bs = c.d("Background Scale");
    const double sb = c.d("Foreground Shadow Blur"), bb = c.d("Background Blur");
    bbfTable(o.tab);
    bbfMatrix(o.mBg, W, H, bx, by, bs);
    bbfMatrix(o.mFg, W, H, fx, fy, fs);
    o.bsize = (double)std::max(W, H) * bb;
    o.s8 = (float)sb * 0.6f;
    o.shadowSize = (double)((o.s8 * 1.0f) * (float)std::max(W, H)) * 0.125;
}

static void blackBarFill8(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double fx, fy, bx, by;
    c.xy("Foreground Location", fx, fy);
    c.xy("Background Location", bx, by);
    const double fs = c.d("Foreground Scale"), bs = c.d("Background Scale");
    const double sb = c.d("Foreground Shadow Blur"), bb = c.d("Background Blur");
    float tab[256];
    bbfTable(tab);
    // the frame's levels, bottom row first
    std::vector<uint8_t> src((size_t)W * H * 4);
    forRows(H, W * 4, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* q = c.src.at(x, y);
            for (int k = 0; k < 4; ++k) src[((size_t)(H - 1 - y) * W + x) * 4 + k] = (uint8_t)v8::toLevel(q[k]);
        }
    });
    float m[9];
    std::vector<uint8_t> bg, fg;
    bbfMatrix(m, W, H, bx, by, bs);
    bbfSample(src, bg, W, H, m);
    const double bsize = (double)std::max(W, H) * bb;
    if (bsize != 0.0) {
        Buf t;
        t.resize(W, H);
        forRows(H, W * 4, [&](int y) { for (size_t i = (size_t)y * W * 4; i < (size_t)(y + 1) * W * 4; ++i) t.px[i] = v8::fromLevel(bg[i]); });
        vegasDericheBlur(t, bsize, bsize, true);
        forRows(H, W * 4, [&](int y) { for (size_t i = (size_t)y * W * 4; i < (size_t)(y + 1) * W * 4; ++i) bg[i] = (uint8_t)v8::toLevel(t.px[i]); });
    }
    bbfMatrix(m, W, H, fx, fy, fs);
    bbfSample(src, fg, W, H, m);
    // the shadow: alpha only
    const float s8 = (float)sb * 0.6f;
    Buf pl;
    pl.resize(W, H);
    forRows(H, W * 4, [&](int y) {
        for (int i = y * W; i < (y + 1) * W; ++i) {
            float* o = &pl.px[(size_t)i * 4];
            o[0] = o[1] = o[2] = 0.f;
            o[3] = tab[fg[(size_t)i * 4 + 3]] * 1.0f;
        }
    });
    if (s8 > 0.f) {
        const double size = (double)((s8 * 1.0f) * (float)std::max(W, H)) * 0.125;
        vegasDericheBlur(pl, size, size, false);
    }
    forRows(H, W * 24, [&](int y) {
      for (int i = y * W; i < (y + 1) * W; ++i) {
        const float Fa = pl.px[(size_t)i * 4 + 3], Fc[3] = {pl.px[(size_t)i * 4], pl.px[(size_t)i * 4 + 1], pl.px[(size_t)i * 4 + 2]};
        const uint8_t* f = &fg[(size_t)i * 4];
        const float x0 = 1.0f * Fa, x1 = 1.0f * tab[f[3]];
        float x4 = 1.0f - x1;
        float x2 = x0 * 1.0f;
        const float x3 = x1 * 1.0f;
        x2 = x2 * x4;
        x4 = x4 * x0;
        x4 = x4 + x1;
        float col[4];
        for (int k = 0; k < 3; ++k) {
            col[k] = (x3 * tab[f[k]]) + (x2 * Fc[k]);
            if (x4 > 9.999999747378752e-05f) col[k] = col[k] / x4;
        }
        col[3] = x4;
        uint8_t lay[4];
        for (int k = 0; k < 4; ++k) {
            int v = (int)(col[k] * 255.0f + 0.5f);
            lay[k] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        // the layer over the background
        uint8_t* d = &bg[(size_t)i * 4];
        const float ta = tab[lay[3]];
        const float kk = (1.0f - ta) * tab[d[3]];
        for (int k = 0; k < 3; ++k) {
            int v = (int)(((float)d[k] * kk) + ((float)lay[k] * ta));
            d[k] = (uint8_t)(v > 255 ? 255 : v);
        }
        d[3] = std::max(lay[3], d[3]);
      }
    });
    forRows(H, W * 4, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t* d = &bg[((size_t)(H - 1 - y) * W + x) * 4];
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) o[k] = v8::fromLevel(d[k]);
        }
    });
}

// A float sample through a Black Bar Fill matrix: the place (xp, yp) is an index into the frame (rows from the bottom); a pixel counts when xp > lo and xp < size + hi, and the neighbour past the last column
// or row is transparent black (it is not repeated), so the picture's outer pixel fades over the one step before the bound. o: premultiplied RGBA.
static bool bbfReadF(const Buf& src, int W, int H, const float* m, int i, int j, float* o)
{
    const float xf = (float)i, yf = (float)j;
    const float x9 = yf * m[1] + m[2], x7 = yf * m[4] + m[5], x5 = yf * m[7] + m[8];
    const float inv = 1.0f / (xf * m[6] + x5);
    const float xp = (xf * m[0] + x9) * inv, yp = (xf * m[3] + x7) * inv;
    o[0] = o[1] = o[2] = o[3] = 0.f;
    if (!(xp > -0.4f) || !(xp < (float)W) || !(yp > -0.4f) || !(yp < (float)H)) return false;
    const float flx = std::floor(xp), fly = std::floor(yp);
    const int x0 = (int)flx, y0 = (int)fly;
    const float fx = xp - flx, fy = yp - fly;
    float v[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    for (int q = 0; q < 4; ++q) {
        const int xx = x0 + (q & 1), yy = y0 + (q >> 1);
        if (xx >= 0 && xx < W && yy >= 0 && yy < H) std::memcpy(v[q], src.at(xx, H - 1 - yy), sizeof(float) * 4);
    }
    for (int k = 0; k < 4; ++k) {
        const float t0 = v[0][k] + (v[1][k] - v[0][k]) * fx, t1 = v[2][k] + (v[3][k] - v[2][k]) * fx;
        o[k] = t0 + (t1 - t0) * fy;
    }
    return true;
}

void blackBarFill(Ctx& c)
{
    if (c.levels) { blackBarFill8(c); return; }
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double fx, fy, bx, by;
    c.xy("Foreground Location", fx, fy);
    c.xy("Background Location", bx, by);
    const double fs = c.d("Foreground Scale"), bs = c.d("Background Scale");
    const double sb = c.d("Foreground Shadow Blur"), bb = c.d("Background Blur");
    float mFg[9], mBg[9];
    bbfMatrix(mBg, W, H, bx, by, bs);
    bbfMatrix(mFg, W, H, fx, fy, fs);
    // VEGAS reads the straight (not premultiplied) pictures channel by channel, so a faded edge pixel is (w * colour, w) and the 'over' below weighs the colour by w once more
    Buf st;
    st.ensure(W, H);
    forRows(H, W * 4, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* p = c.src.at(x, y);
            float* o = st.at(x, y);
            const float inv = p[3] > 0.f ? 1.f / p[3] : 0.f;
            o[0] = p[0] * inv; o[1] = p[1] * inv; o[2] = p[2] * inv; o[3] = p[3];
        }
    });
    Buf bg, pl;
    bg.resize(W, H);
    pl.resize(W, H);
    std::vector<float> fg((size_t)W * H * 4);
    forRows(H, W * 16, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float* o = bg.at(x, y);
            bbfReadF(st, W, H, mBg, x, H - 1 - y, o);
            for (int k = 0; k < 3; ++k) o[k] *= o[3];                  // (premultiplied from here)
            float* f = &fg[((size_t)y * W + x) * 4];
            bbfReadF(st, W, H, mFg, x, H - 1 - y, f);
            float* s = pl.at(x, y);
            s[0] = s[1] = s[2] = 0.f;
            s[3] = f[3];
        }
    });
    // both blurs are the OpenCL recursive Gaussian: a size in pixels (larger side * Background Blur; Shadow Blur * 0.6 * larger side / 8) is a deviation of size / 6 for it,
    // which gives the Gaussian of the 8-bit code ((size + 1.5) / sqrt(2 ln 255)); the shadow is blurred even when Shadow Blur is 0 (a spread of a pixel)
    const double bsize = (double)std::max(W, H) * bb;
    cgOclBlur(bg.px.data(), W, H, bsize / 6.0, bsize / 6.0);
    const float s8 = (float)sb * 0.6f;
    const double shadowSize = (double)((s8 * 1.0f) * (float)std::max(W, H)) * 0.125;
    cgOclBlur(pl.px.data(), W, H, shadowSize / 6.0, shadowSize / 6.0);
    const bool store8 = c.host8;
    forRows(H, W * 8, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* f = &fg[((size_t)y * W + x) * 4];
            const float sa = std::min(1.f, std::max(0.f, pl.at(x, y)[3]));
            const float* b = bg.at(x, y);
            float* o = c.out.at(x, y);
            // the foreground over its black shadow, that over the background (the layer's colour is the foreground's weighed by its alpha)
            const float la = f[3] + (1.f - f[3]) * sa;
            for (int k = 0; k < 3; ++k) o[k] = f[k] * f[3] + (1.f - la) * b[k];
            o[3] = la + (1.f - la) * b[3];
            if (store8) for (int k = 0; k < 4; ++k) o[k] = oclStore12(o[k]);
        }
    });
}

// Crop (OFX): the part of the frame in a rotated box is kept, outside is transparent. Anchor: which edge or corner of the box stays put when
// X and Y crop change (the box is X Crop x Y Crop of the frame centred on Location).
void cropFx(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.openCl && W > 0 && H > 0) {
        // VEGAS's OpenCL Crop (vfxpipcrop.h): integer pixel positions through the float matrix, inclusive bounds, the opacity in floats, the 8-bit store rounding (ties down)
        double lx, ly;
        c.xy("Location", lx, ly);
        const vfxpc::CropSetup su = vfxpc::cropSetup(W, H, lx, ly, c.d("XScale"), c.d("YScale"), c.d("Angle"), c.d("Opacity"));
        forRows(H, W, [&](int y) {
            const int j = H - 1 - y;                                      // the kernel's row, from the bottom
            for (int x = 0; x < W; ++x) {
                float* o = c.out.at(x, y);
                if (!vfxpc::cropInside(su, x, j)) { std::memset(o, 0, sizeof(float) * 4); continue; }
                const float* s = c.src.at(x, y);
                for (int k = 0; k < 4; ++k) { const float v = s[k] * su.opacity; o[k] = c.host8 ? oclWriteLevel(v) : v; }
            }
        });
        return;
    }
    double lx, ly;
    c.xy("Location", lx, ly);
    const double xs = clampd(c.d("XScale"), 0, 1), ys = clampd(c.d("YScale"), 0, 1), ang = c.d("Angle") * kPi / 180.0;
    const double op = clampd(c.d("Opacity"), 0, 1);
    const double cs = rm::cos(ang), sn = rm::sin(ang);
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
void pipSetup(Ctx& c, int W, int H, PipSetup& o)
{
    // VEGAS places the picture by pixel INDICES (like Black Bar Fill); it exists only where the index lies inside the frame. Fixed Shape keeps
    // Scale in X and Y equal (Y is ignored); Free Form puts the corners on the four corner points.
    double lx, ly;
    c.xy("Location", lx, ly);
    const int mode = c.i("KeepProportions");
    double sx = std::max(0.01, c.d("Scale")), sy = std::max(0.01, c.d("DistortionScaleY"));
    if (mode == 0) sy = sx;      // (VEGAS's captured pictures: in Fixed Shape the Distortion Scale Y setting changes nothing)
    const double ang = c.d("Angle") * kPi / 180.0, op = clampd(c.d("Opacity"), 0, 1);
    const int anchor = c.i("Anchor");
    const double ax = (anchor % 3) * 0.5, ay = 1.0 - (anchor / 3) * 0.5;          // the anchor inside the picture, from the bottom left
    const double cs = rm::cos(ang), sn = rm::sin(ang);
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
    o.mode = mode; o.sx = sx; o.sy = sy; o.cs = cs; o.sn = sn; o.shear = shear; o.op = op;
    o.lxW = lx * W; o.lyH = ly * H; o.axW = ax * W; o.ayH = ay * H;
    for (int i = 0; i < 9; ++i) o.hm[i] = hm[i];
    // The sampling matrix (u, v) = ((M0 x + M1 j + M2) / w, (M3 x + M4 j + M5) / w), w = M6 x + M7 j + M8, with x the column and j the row from the bottom (VEGAS's memory
    // order), in floats as VEGAS's renderer takes them.
        {
        double m[9];
        if (mode == 2) {
            m[0] = hm[0]; m[1] = hm[1] * W / H; m[2] = hm[2] * W; m[3] = hm[3] * H / W; m[4] = hm[4]; m[5] = hm[5] * H;
            m[6] = hm[6] / W; m[7] = hm[7] / H; m[8] = hm[8];
        } else {
            // Vfx1's matrix builder (0x18066b120 -> 0x18066af80), step for step in floats: the turn is fmod(Angle * pi / 180, 2 pi) cut to a float, cos and sin of that
            // as floats; the scale factors are 1 / Scale (cut to floats); the shear sits in the second row.
            const float a = (float)std::fmod(c.d("Angle") * 0.017453292519943295, 6.283185307179586);
            const float fcs = (float)rm::cos((double)a), fsn = (float)rm::sin((double)a);
            const float P0 = (float)(1.0 / sx), P1 = (float)(1.0 / (mode == 0 ? sx : sy)), C = mode == 0 ? 0.f : (float)(shear * 0.5);
            const float t3 = P0 * fcs, t2 = P0 * fsn, pa = t3, pb = -t2;
            const float da = (fcs * C + fsn) * P1, db = (fcs - fsn * C) * P1;
            const float B0 = (float)(-lx * W), B1 = (float)(-ly * H);
            m[0] = pa; m[1] = pb; m[2] = (B1 * pb + B0 * pa) + (float)(ax * W);
            m[3] = da; m[4] = db; m[5] = (B1 * db + B0 * da) + (float)(ay * H);
            m[6] = 0; m[7] = 0; m[8] = 1;
        }
        for (int i = 0; i < 9; ++i) o.M[i] = (float)m[i];
    }
}

// Picture in Picture as VEGAS's OpenCL kernel draws it (vfxpipcrop.h): the matrix from the builder (Free Form: the 4-point solver, and when that cannot be solved the
// parametric matrix), the position tested 0 <= u < W, 0 <= v < H, the card's bilinear read (1/256 weights, transparent beyond the picture), times the opacity.
vfxpc::PipSetup8 pipOpenClSetup(Ctx& c, int W, int H)
{
    double lx, ly;
    c.xy("Location", lx, ly);
    const int mode = std::max(0, std::min(2, c.i("KeepProportions")));
    const double scale = std::max(0.01, c.d("Scale")), scaleY = std::max(0.01, c.d("DistortionScaleY"));
    vfxpc::PipSetup8 su;
    bool done = false;
    if (mode == 2) {
        double cr[4][2];
        c.xy("CornerTL", cr[0][0], cr[0][1]);
        c.xy("CornerTR", cr[1][0], cr[1][1]);
        c.xy("CornerBL", cr[2][0], cr[2][1]);
        c.xy("CornerBR", cr[3][0], cr[3][1]);
        done = vfxpc::pipSetupFreeForm(W, H, cr, clampd(c.d("Opacity"), 0, 1), su);
    }
    if (!done) su = vfxpc::pipSetupAffine(W, H, mode, lx, ly, scale, scaleY, c.d("DistortionShear"), c.d("Angle"), clampd(c.d("Opacity"), 0, 1));
    return su;
}
void pipOpenClGpu(Ctx& c, int W, int H, float M[9], float& opacity)       // (for the Vulkan plan, vkfilters.cpp)
{
    const vfxpc::PipSetup8 su = pipOpenClSetup(c, W, H);
    for (int i = 0; i < 9; ++i) M[i] = su.M[i];
    opacity = su.opacity;
}
static void pictureInPictureOpenCl(Ctx& c, int W, int H)
{
    const vfxpc::PipSetup8 su = pipOpenClSetup(c, W, H);
    std::vector<float> src((size_t)W * H * 4);                           // rows from the bottom
    for (int y = 0; y < H; ++y) std::memcpy(&src[(size_t)y * W * 4], c.src.at(0, H - 1 - y), (size_t)W * 4 * sizeof(float));
    const float* M = su.M;
    forRows(H, W * 4, [&](int y) {
        const int j = H - 1 - y;
        const float fy = (float)j;
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            const float fx = (float)x;
            const float w = vfxpc::dot3(M[6], M[7], M[8], fx, fy);
            const float u = vfxpc::dot3(M[0], M[1], M[2], fx, fy) / w, v = vfxpc::dot3(M[3], M[4], M[5], fx, fy) / w;
            if (!(u >= 0.f && u < (float)W && v >= 0.f && v < (float)H)) { std::memset(o, 0, sizeof(float) * 4); continue; }
            float p[4];
            vfxpc::linearRead(src, W, H, u, v, false, p);
            for (int k = 0; k < 4; ++k) { const float val = p[k] * su.opacity; o[k] = c.host8 ? oclWriteLevel(val) : val; }
        }
    });
}

void pictureInPicture(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.openCl && W > 0 && H > 0) { pictureInPictureOpenCl(c, W, H); return; }
    PipSetup su;
    pipSetup(c, W, H, su);
    const int mode = su.mode;
    const double sx = su.sx, sy = su.sy, cs = su.cs, sn = su.sn, shear = su.shear, op = su.op;
    const double* hm = su.hm;
    const float* M = su.M;
    if (c.levels) {
        // VEGAS's 8-bit renderer: the position in floats, inside where -0.4 < u < W - 0.6 (and the same for v), bilinear taps in 1/1024 (each weight cut to a whole number), the sum
        // shifted down by 10 and cut to a byte, then times the opacity in floats and cut.
        const float opF = (float)op;
        std::vector<unsigned char> sb((size_t)W * H * 4);                     // the source bytes, rows from the bottom
        forRows(H, W, [&](int y) {
            const float* s = c.src.at(0, y);
            unsigned char* d = &sb[(size_t)(H - 1 - y) * W * 4];
            for (int i = 0; i < W * 4; ++i) d[i] = (unsigned char)v8::toLevel(s[i]);
        });
        forRows(H, W * 8, [&](int y) {
            const int j = H - 1 - y;
            const float yf = (float)j;
            const float a9 = yf * M[1] + M[2], a7 = yf * M[4] + M[5], a5 = yf * M[7] + M[8];
            float* o = c.out.at(0, y);
            for (int x = 0; x < W; ++x, o += 4) {
                const float xf = (float)x;
                const float inv = 1.0f / (xf * M[6] + a5);
                const float u = (xf * M[0] + a9) * inv, v = (xf * M[3] + a7) * inv;
                if (!(u > -0.4f) || !((float)W - 0.6f > u) || !(v > -0.4f) || !((float)H - 0.6f > v)) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
                const int ix = (int)u, iy = (int)v;
                const float fx = std::max(0.f, u - (float)ix), fy = std::max(0.f, v - (float)iy);
                const float omx = 1.0f - fx;
                const int w1 = (int)(((1.0f - fy) * fx) * 1024.0f), w2 = (int)((omx * fy) * 1024.0f), w3 = (int)((fy * fx) * 1024.0f), w0 = (int)((omx * (1.0f - fy)) * 1024.0f);
                const bool edgeX = ix == W - 1, edgeY = iy == H - 1;
                const unsigned char* p00 = &sb[((size_t)iy * W + ix) * 4];
                for (int k = 0; k < 4; ++k) {
                    const int a = p00[k];
                    const int b = edgeX ? a : p00[4 + k];
                    const int cc = edgeY ? a : p00[(size_t)W * 4 + k];
                    const int dd = (!edgeX && !edgeY) ? p00[(size_t)W * 4 + 4 + k] : cc;
                    const int sum = dd * w3 + cc * w2 + b * w1 + a * w0;
                    const int byte = (sum >> 10) & 0xff;
                    o[k] = v8::fromLevel((int)((float)byte * opF));
                }
            }
        });
        return;
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
                const double u = x - su.lxW, v = j - su.lyH;
                double ru = u * cs - v * sn, rv = u * sn + v * cs;
                ru = (ru - shear * rv * 0.5) / sx;
                rv /= sy;
                px = ru + su.axW;
                pj = rv + su.ayH;
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

// Film Grain, as VEGAS's own code does it (vfx1.dll, read, not fitted): a 64 x 64 table of three planes of noise is made from the shift-register generator
// (VegasShiftRandom) seeded with (int)(grain size x 1000) + the event time stamp (Ctx::legacySeedTime): each value is a Gaussian number from the polar method
// (v x sqrt(-2 ln(s) / s) of a point u, v inside the unit circle, u and v the low 16 bits of two words) put into 0..1 as (g + 5.28) / 10.56. The grain size is
// max(1, (2 x Granularity + 1) x frame height / 240) pixels. Down each row the table is read bilinearly, one table cell per grain size, wrapping at 64, and the
// same down the rows; every row starts again at cell 0. With Chormaticity above 0 the colours take planes 0 (first colour; in memory order blue), 1 and 2 as
// c x plane + (1 - c) x plane 0; without it all three take plane 0. The picture is multiplied by 1.3 x noise and mixed in by Amount: 8-bit with the weight
// (int)(Amount x 255) and the noisy value cut to 0..alpha and the integer mix ((d x w + 128) + that >> 8) >> 8; float with the float arithmetic as it comes.
// the numbers of one frame (filmGrain() and the Vulkan plan use these)
void filmGrainSetup(Ctx& c, int W, int H, FilmGrainSetup& g)
{
    g.amount = clampd(c.d("Amount"), 0, 1);
    g.gran = clampd(c.d("Granularity"), 0, 1);
    g.chroma = clampd(c.d("Chormaticity"), 0, 1);
    // the grain's size is worked from the project's frame, the cells are drawn at this frame's scale
    const double frameH = c.frameH > 0 ? (double)c.frameH : (double)H / c.scale;
    const double grain = std::max(1.0, (g.gran * 2 + 1) * frameH / 240.0);
    VegasShiftRandom rng((uint32_t)((int)(grain * 1000.0) + c.legacySeedTime()));
    g.table.assign(12288, 0.0);
    std::vector<double>& table = g.table;
    for (int k = 0; k < 12288;) {
        const double u = (double)(rng.next() & 0xffffu) / 65535.0 * 2 - 1.0;
        const double v = (double)(rng.next() & 0xffffu) / 65535.0 * 2 - 1.0;
        const double q = u * u + v * v;
        if (q >= 1.0 || q == 0.0) continue;
        table[(size_t)k++] = (std::sqrt(rm::log(q) * -2.0 / q) * v + 5.28) / 10.56;
    }
    g.stepX = 1.0 / (grain * c.scale);
    g.stepY = 1.0 / (grain * c.scale);
    // the table rows and the fraction between them at each picture row
    g.rowA.assign((size_t)H, 0); g.rowB.assign((size_t)H, 0);
    g.frac.assign((size_t)H, 0.0);
    {
        int r0 = 0, r1 = 1;
        double fy = 0;
        for (int y = 0; y < H; ++y) {
            g.rowA[(size_t)y] = r0; g.rowB[(size_t)y] = r1; g.frac[(size_t)y] = fy;
            fy += g.stepY;
            if (fy >= 1.0) { r0 = r1; fy -= 1.0; r1 = (r1 + 1) & 63; }
        }
    }
    // (every row starts again at cell 0: the columns' cells and fractions are the same in each)
    g.colA.assign((size_t)W, 0); g.colB.assign((size_t)W, 0);
    g.colFrac.assign((size_t)W, 0.0);
    {
        int c0 = 0, c1 = 1;
        double fx = 0;
        for (int x = 0; x < W; ++x) {
            g.colA[(size_t)x] = c0; g.colB[(size_t)x] = c1; g.colFrac[(size_t)x] = fx;
            fx += g.stepX;
            if (fx >= 1.0) { c0 = c1; fx -= 1.0; c1 = (c1 + 1) & 63; }
        }
    }
    g.wt = (int)(g.amount * 255.0) & 0xff;
}

void filmGrain(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    FilmGrainSetup g;
    filmGrainSetup(c, W, H, g);
    const double amount = g.amount, chroma = g.chroma, stepX = g.stepX;
    const std::vector<double>& table = g.table;
    const std::vector<int>& rowA = g.rowA;
    const std::vector<int>& rowB = g.rowB;
    const std::vector<double>& frac = g.frac;
    const int wt = g.wt;
    const float amountF = (float)amount;
    const bool colour = chroma > 0;
    forRows(H, W, [&](int y) {
        const int r0 = rowA[(size_t)y] * 64, r1 = rowB[(size_t)y] * 64;
        const double fy = frac[(size_t)y];
        int c0 = 0, c1 = 1;
        double fx = 0;
        for (int x = 0; x < W; ++x) {
            double n[3] = {0, 0, 0};
            for (int p = 0; p < (colour ? 3 : 1); ++p) {
                const double* t = &table[(size_t)p * 4096];
                const double a = (1 - fx) * t[r0 + c0] + fx * t[r0 + c1];
                const double b = (1 - fx) * t[r1 + c0] + fx * t[r1 + c1];
                n[p] = a * (1 - fy) + b * fy;
            }
            double m[3];
            if (colour) { m[0] = n[0]; m[1] = chroma * n[1] + (1 - chroma) * n[0]; m[2] = chroma * n[2] + (1 - chroma) * n[0]; }
            else m[0] = m[1] = m[2] = n[0];
            fx += stepX;
            if (fx >= 1.0) { c0 = c1; fx -= 1.0; c1 = (c1 + 1) & 63; }
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            if (c.levels) {
                const int al = v8::toLevel(s[3]);
                for (int k = 0; k < 3; ++k) {           // (Reno's red is the last of VEGAS's three: blue first)
                    const int sv = v8::toLevel(s[k]);
                    const int t = (int)std::lrint(m[2 - k] * 1.3 * (double)sv);
                    const int nv = (t < 0 ? 0 : (t > al ? al : t)) & 255;
                    const int e = (nv - sv) * wt + 128;
                    const int r = sv + (((e >> 8) + e) >> 8);
                    o[k] = v8::fromLevel(r < 0 ? 0 : (r > 255 ? 255 : r));
                }
                o[3] = v8::fromLevel(al);
            } else {
                for (int k = 0; k < 3; ++k) {
                    const float nm = (float)m[2 - k] * 1.3f;
                    const float noisy = s[k] * nm;
                    o[k] = (noisy - s[k]) * amountF + s[k];
                }
                o[3] = s[3];
            }
        }
    });
}

// Film Effects (VEGAS's DirectX film transform): grey, grain, tint, then the particles. Pieces in vfxfilm.h, each checked against the DLL. The particle simulation keeps
// its random generator (seeded 123456 for each effect) and its particles from call to call, as the DLL's effect object does, so a picture depends on the pictures drawn before
// it: Reno keeps the state of each effect (FilmCache). VEGAS's captures show that when the effect is asked for a picture that does not follow the last one (the first, or a jump)
// it draws one more picture first, unseen, and then the one asked for (bit for bit on all 22 captures with something on); a picture that follows the last one is just the next
// call, and the same picture again is drawn again from the state it started from.
namespace {
struct FilmKey {
    vfxfilm::Params p;
    int W = 0, H = 0;
    double dt = 0;
    bool same(const FilmKey& o) const
    {
        if (W != o.W || H != o.H || dt != o.dt || p.grain != o.p.grain || p.grey != o.p.grey || p.tint != o.p.tint) return false;
        for (int k = 0; k < 3; ++k)
            if (p.tintBgr[k] != o.p.tintBgr[k]) return false;
        for (int k = 0; k < 7; ++k)
            if (p.g[k].a != o.p.g[k].a || p.g[k].b != o.p.g[k].b || p.g[k].c != o.p.g[k].c || p.g[k].enable != o.p.g[k].enable) return false;
        return true;
    }
};
struct FilmCache {
    std::mutex mu;
    FilmKey key;
    bool valid = false;
    long last = -1000000;                   // the picture the state 'cur' has just drawn
    bool lastJumped = false;                // (and whether that one had the unseen picture first)
    vfxfilm::Sim cur;
    vfxfilm::Sim before;                    // the state that picture started from
};
std::mutex gFilmMutex;
std::map<const void*, std::shared_ptr<FilmCache>>& filmCaches()
{
    static std::map<const void*, std::shared_ptr<FilmCache>> m;
    return m;
}
std::shared_ptr<FilmCache> filmCacheFor(const void* inst)
{
    std::lock_guard<std::mutex> lk(gFilmMutex);
    auto& m = filmCaches();
    auto it = m.find(inst);
    if (it != m.end()) return it->second;
    if (m.size() > 32) m.erase(m.begin());
    auto s = std::make_shared<FilmCache>();
    m[inst] = s;
    return s;
}
}  // namespace

// One frame of the effect on 'live' (the picture's bytes or floats, or Rec to write the work down for the Vulkan plan): the key, the state kept for the effect and the frames
// (the unseen one first after a jump).
static void filmRun(Ctx& c, int W, int H, vfxfilm::Surf& live)
{
    const Color col = c.color("Color");
    FilmKey key;
    key.W = W; key.H = H;
    key.dt = 1.0 / (c.fps > 1e-6 && c.fps < 1e6 ? c.fps : 30.0);
    key.p = vfxfilm::paramsFor(c.d("Grain"), c.d("Tint"), col.r, col.g, col.b, c.b("ConvertToGreyscale"), c.b("EnableEnableParticles"), c.i("EnableParticlesType"),
                               c.d("EnableParticlesAmount"));
    // the frame number since the event's start (nonsense times count as the first frame)
    long n = std::isfinite(c.time) ? std::lround(c.time) : 0;
    n = std::max(0L, std::min(n, 100000000L));
    vfxfilm::Surf dry;
    dry.W = W; dry.H = H; dry.bytes = true;
    std::shared_ptr<FilmCache> cp = filmCacheFor(c.instance);
    FilmCache& fc = *cp;
    std::lock_guard<std::mutex> lk(fc.mu);
    if (!fc.valid || !fc.key.same(key)) {
        fc.key = key; fc.valid = true; fc.last = -1000000; fc.lastJumped = false; fc.cur = vfxfilm::Sim(); fc.before = vfxfilm::Sim();
    }
    bool jumped;
    if (n == fc.last) {                 // the same picture again: from the state it started from
        fc.cur = fc.before;
        jumped = fc.lastJumped;
    } else {
        fc.before = fc.cur;
        jumped = n != fc.last + 1;
    }
    if (jumped) vfxfilm::frame(fc.cur, key.p, dry, key.dt);
    vfxfilm::frame(fc.cur, key.p, live, key.dt);
    fc.last = n;
    fc.lastJumped = jumped;
}

// The Vulkan plan's part (vkfx2.cpp): the same frame with the picture work written down in 'rec' (the state stays here on the CPU).
void filmEffectsGpu(Ctx& c, int W, int H, vfxfilm::Rec& rec, bool& bytes)
{
    bytes = c.levels;
    vfxfilm::Surf live;
    live.W = W; live.H = H; live.bytes = bytes;
    live.rec = &rec;
    if (W > 0 && H > 0) filmRun(c, W, H, live);
}

void filmEffects(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    // the picture as VEGAS's bytes (blue first); a float picture is the same bytes' worth of values, premultiplied
    const bool bytes = c.levels;
    std::vector<uint8_t> b8;
    std::vector<float> bf;
    const size_t npx = (size_t)W * (size_t)H;
    if (bytes) {
        b8.resize(npx * 4);
        for (size_t i = 0; i < npx; ++i) {
            const float* s = &c.src.px[i * 4];
            uint8_t* d = &b8[i * 4];
            d[0] = (uint8_t)v8::toLevel(s[2]); d[1] = (uint8_t)v8::toLevel(s[1]); d[2] = (uint8_t)v8::toLevel(s[0]); d[3] = (uint8_t)v8::toLevel(s[3]);
        }
    } else {
        bf.resize(npx * 4);
        for (size_t i = 0; i < npx; ++i) {
            const float* s = &c.src.px[i * 4];
            float* d = &bf[i * 4];
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    }
    vfxfilm::Surf live;
    live.W = W; live.H = H; live.bytes = bytes;
    live.b = bytes ? b8.data() : nullptr;
    live.f = bytes ? nullptr : bf.data();
    filmRun(c, W, H, live);
    for (size_t i = 0; i < npx; ++i) {
        float* o = &c.out.px[i * 4];
        if (bytes) {
            const uint8_t* d = &b8[i * 4];
            o[0] = v8::fromLevel(d[2]); o[1] = v8::fromLevel(d[1]); o[2] = v8::fromLevel(d[0]); o[3] = v8::fromLevel(d[3]);
        } else {
            const float* d = &bf[i * 4];
            o[0] = d[2]; o[1] = d[1]; o[2] = d[0]; o[3] = d[3];
        }
    }
}

// Vignette, VEGAS's CPU processor (floats in VEGAS's order). With W, H the frame (pixel aspect 1), R = sqrt(W^2 + H^2) x Outer / 2 and Ri = R x Inner, a pixel at (u, v) from the
// centre (whole pixels, no half) is turned by Rotation, stretched by g = (Aspect + 1) / 2 across and 1 / g up; d = its distance; the strength is ((d - Ri) / (R - Ri) clamped to 0..1)
// ^ (100 / (Feather + 0.001)^2) (0 inside Ri; Invert: 1 minus it); it mixes the pixel's bytes (premultiplied, alpha too) towards Color's bytes (unpremultiplied, (int)(c x 255 + 0.5))
// by w = Color alpha x strength: (int)(src x (1 - w) + color x w + 0.5) kept in 0..255.
void vignetteSetup(Ctx& c, int W, int H, VignetteSetup& o)
{
    double cx, cy;
    c.xy("Center", cx, cy);
    const Color col = c.color("Color");
    o.invert = c.b("Invert");
    const float diag = std::sqrt((float)W * (float)W + (float)H * (float)H);
    float rOuter = (float)((double)diag * (c.d("Outer") * 0.5));
    o.rInner = (float)((double)rOuter * c.d("Inner"));
    o.ramp = rOuter - o.rInner;
    if (1e-6f > o.ramp) o.ramp = 1.f;
    const float rot = (float)(c.d("Rotation") * 0.01745329052209854);
    o.cs = rm::cos(rot); o.sn = rm::sin(rot);
    o.g = (float)((c.d("Aspect") - 1.0) * 0.5 + 1.0);
    const float fe = (float)c.d("Feather") + 0.001f;
    o.expo = 100.f / (fe * fe);
    o.cxp = (float)((double)W * cx); o.cyp = (float)((double)H * cy);
    o.cb[0] = (int)((float)col.r * 255.f + 0.5f);
    o.cb[1] = (int)(col.g * 255.0 + 0.5);
    o.cb[2] = (int)((float)col.b * 255.f + 0.5f);
    o.cb[3] = 0;
    o.ca = (float)col.a;
    o.cf[0] = (float)col.r; o.cf[1] = (float)col.g; o.cf[2] = (float)col.b;
}

// With GPU acceleration on VEGAS draws the Vignette with its OpenCL kernel (vfxvignette.h, ported from the decoded kernel): float, the centre and radius measured from the
// (W - 1) x (H - 1) frame, the rows counted from the bottom, the colour mixed into the premultiplied pixel and the alpha kept. The 8-bit frame gets the float result rounded to
// the nearest level with exact halves down (vegasTiesDown, vegasDrawsFloat lists the effect).
static void vignetteOclParams(Ctx& c, int W, int H, vfxvig::Params& p, float cc[3], float& ca)
{
    double cx, cy;
    c.xy("Center", cx, cy);
    const Color col = c.color("Color");
    p.w = W; p.h = H;
    p.outer = (float)c.d("Outer"); p.inner = (float)c.d("Inner"); p.aspect = (float)c.d("Aspect");
    p.cx = (float)cx; p.cy = (float)cy;
    p.rotation = (float)c.d("Rotation");
    const float fe = (float)c.d("Feather") + 0.001f;
    p.exponent = 100.f / (fe * fe);
    p.invert = c.b("Invert");
    cc[0] = (float)col.r; cc[1] = (float)col.g; cc[2] = (float)col.b;
    ca = (float)col.a;
}
// the kernel's per-frame numbers for the Vulkan plan (vkfilters.cpp): the setup, the outer radius' inner part and the divisor, as the CPU works them out
void vignetteOclGpu(Ctx& c, int W, int H, float out[14], bool& invert)
{
    vfxvig::Params p;
    float cc[3], ca;
    vignetteOclParams(c, W, H, p, cc, ca);
    const vfxvig::Setup u = vfxvig::setup(p);
    const float big = 0.5f * p.outer * rm::hypot(u.w1, u.h1 / p.par);
    const float inner = p.inner * big;
    const float den = (std::fabs(big - inner) < 0.000001f) ? 1.0f : big - inner;
    const float v[14] = {u.h1, u.centreX, u.centreY, u.c, u.s, u.asp, inner, den, p.exponent, ca, cc[0], cc[1], cc[2], 0.f};
    for (int i = 0; i < 14; ++i) out[i] = v[i];
    invert = p.invert;
}
static void vignetteOpenCl(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    vfxvig::Params p;
    float cc[3], ca;
    vignetteOclParams(c, W, H, p, cc, ca);
    const vfxvig::Setup u = vfxvig::setup(p);
    forRows(H, W, [&](int yt) {
        const int y = H - 1 - yt;
        for (int x = 0; x < W; ++x) {
            const float t = vfxvig::amount(p, u, x, y) * ca;
            const float* s = c.src.at(x, yt);
            float* o = c.out.at(x, yt);
            for (int k = 0; k < 3; ++k) o[k] = s[k] + (cc[k] - s[k]) * t;
            o[3] = s[3];
        }
    });
}

void vignette(Ctx& c)
{
    if (c.openCl && c.out.w > 0 && c.out.h > 0) { vignetteOpenCl(c); return; }
    const int W = c.out.w, H = c.out.h;
    VignetteSetup su;
    vignetteSetup(c, W, H, su);
    const Color col = c.color("Color");
    const bool invert = su.invert;
    const float rInner = su.rInner, ramp = su.ramp, cs = su.cs, sn = su.sn, g = su.g, expo = su.expo, cxp = su.cxp, cyp = su.cyp, ca = su.ca;
    const int* cb = su.cb;
    forRows(H, W, [&](int y) {
        const float v = ((float)(H - 1 - y) - cyp);
        const float v14 = v * cs, v15 = -(v * sn);
        for (int x = 0; x < W; ++x) {
            const float u = (float)x - cxp;
            float xr = u * sn + v14, yr = u * cs + v15;
            xr = xr * g;
            yr = yr / g;
            const float d = std::sqrt(xr * xr + yr * yr);
            float t = (d - rInner) / ramp;
            t = t < 0.f ? 0.f : std::min(1.f, t);
            float m = 0.f;
            if (d > rInner) {
                const float pw = rm::pow(t, expo);
                m = pw < 0.f ? 0.f : std::min(1.f, pw);
            }
            if (invert) m = 1.f - m;
            const float w = ca * m;
            const float iw = 1.f - w;
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            if (c.levels) {
                for (int k = 0; k < 4; ++k) {
                    const int sv = v8::toLevel(s[k]), cv = k == 3 ? sv : cb[k];
                    int r = (int)((float)sv * iw + (float)cv * w + 0.5f);
                    r = r < 0 ? 0 : std::min(255, r);
                    o[k] = v8::fromLevel(r);
                }
            } else {
                const float cc[3] = {(float)col.r, (float)col.g, (float)col.b};
                for (int k = 0; k < 3; ++k) o[k] = s[k] * iw + cc[k] * w;
                o[3] = s[3];
            }
        }
    });
}

// TV Simulator, from VEGAS's own code (vfx1.dll, read and checked bit for bit against its render function and parts; see vfxtv.h). The settings go to the render
// function as the property page keeps them: Detail zoom 0..1 is the page's 1..4 magnification, the others are used as they are. The picture is VEGAS's 8-bit one
// (blue, green, red, alpha bytes, premultiplied, rows from the top) and the time stamp the event's 100 ns ticks. A float picture (the project is 32-bit) goes through the
// same function on the 8-bit levels and comes back as such (the float render function of vfx1.dll is not ported).
static void tvSettings(Ctx& c, double P[8])
{
    P[0] = 1.0 + clampd(c.d("DetailZoom"), 0, 1) * 3.0;
    P[1] = clampd(c.d("ApertureGrill"), 0, 1);
    P[2] = clampd(c.d("Interlacing"), 0, 1);
    P[3] = clampd(c.d("LineSync"), 0, 1);
    P[4] = clampd(c.d("VerticalSync"), 0, 1);
    P[5] = clampd(c.d("ScanPhasing"), 0, 1);
    P[6] = clampd(c.d("Phosphorescence"), 0, 1);
    P[7] = clampd(c.d("Static"), 0, 1);
}

// The per-row numbers of the render for the Vulkan plan (vkfx2.cpp): the per-pixel part runs on the card.
void tvSimulatorGpu(Ctx& c, int W, int H, vftv::Rows8& o)
{
    double P[8];
    tvSettings(c, P);
    vftv::prepare8(W, H, P, c.legacySeedTime64(), 1.0, vftv::defaultNoiseTable(), o);
}

void tvSimulator(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double P[8];
    tvSettings(c, P);
    std::vector<uint8_t> in((size_t)W * H * 4), out((size_t)W * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        const float* s = c.src.at(0, y);
        uint8_t* d = &in[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, s += 4, d += 4) {
            d[0] = (uint8_t)v8::toLevel(s[2]);
            d[1] = (uint8_t)v8::toLevel(s[1]);
            d[2] = (uint8_t)v8::toLevel(s[0]);
            d[3] = (uint8_t)v8::toLevel(s[3]);
        }
    }
    vftv::render8(out.data(), W * 4, W, H, in.data(), W * 4, P, c.legacySeedTime64(), 1.0, vftv::defaultNoiseTable());
    for (int y = 0; y < H; ++y) {
        float* o = c.out.at(0, y);
        const uint8_t* s = &out[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, o += 4, s += 4) {
            o[0] = v8::fromLevel(s[2]);
            o[1] = v8::fromLevel(s[1]);
            o[2] = v8::fromLevel(s[0]);
            o[3] = v8::fromLevel(s[3]);
        }
    }
}

// Radial Pixelate: VEGAS's CPU processor (rows bottom first, 8-bit). The picture is unrolled about the centre into a polar picture of the same size
// (column = angle from -pi, row = radius out to 2 sqrt 2 in half frames), read at the nearest pixel below, averaged down in whole-pixel blocks
// (sums truncated) to (1 - Strength) / 2 * width + 1 blocks across (Radial) or as many down (Circular), spread back and rolled up again.
// Strength 0 changes nothing.
void rpixSetup(Ctx& c, int W, int H, RpixSetup& o)
{
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    const double st = c.d("Strength");
    const int type = c.i("Type");
    o.pass = W <= 0 || H <= 0 || std::fabs((float)st) <= 1e-6f;
    if (o.pass) return;
    o.q8 = c.levels;
    const double k = (1.0 - st) * 0.5;
    int gw = (int)(W * k + 1.0), gh = (int)(H * k + 1.0);
    if (gw > W) gw = W;
    if (gh > H) gh = H;
    if (type == 0) gh = H; else gw = W;
    o.gw = std::max(1, gw); o.gh = std::max(1, gh);
    o.c0 = (float)(pcx * 2.0 - 1.0); o.c1 = (float)(pcy * 2.0 - 1.0);
    o.sx = (int)((double)W / o.gw * 16384.0); o.sy = (int)((double)H / o.gh * 16384.0);
    o.bw = std::max(1, o.sx / 16384); o.bh = std::max(1, o.sy / 16384);
    o.inv = 1.0 / ((double)o.bw * o.bh);
}

void radialPixelate(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.openCl) {
        // VEGAS's OpenCL kernels (vfxpolar.h): unroll, average in 1-D groups, roll up; Strength 0 (or less) copies the picture
        double pcx, pcy;
        c.xy("Center", pcx, pcy);
        const double st = c.d("Strength");
        if (W <= 0 || H <= 0 || !(st > 0.0)) { copyThrough(c); return; }
        int nx, ny;
        vfxpolar::radialPixelateGroups(W, H, st, nx, ny);
        std::vector<float> src((size_t)W * H * 4), out;
        for (int y = 0; y < H; ++y) std::memcpy(&src[(size_t)y * W * 4], c.src.at(0, H - 1 - y), (size_t)W * 4 * sizeof(float));
        vfxpolar::radialPixelate(src, W, H, (float)(pcx * 2.0 - 1.0), (float)(pcy * 2.0 - 1.0), c.i("Type"), nx, ny, c.host8, out);
        c.out.ensure(W, H);
        for (int y = 0; y < H; ++y) std::memcpy(c.out.at(0, H - 1 - y), &out[(size_t)y * W * 4], (size_t)W * 4 * sizeof(float));
        return;
    }
    RpixSetup su;
    rpixSetup(c, W, H, su);
    if (su.pass) { copyThrough(c); return; }
    const bool q8 = su.q8;
    const int gw = su.gw, gh = su.gh;
    const float c0 = su.c0, c1 = su.c1;
    const float Wf = (float)W, Hf = (float)H;
    // VEGAS's rows (bottom first) of the source; values in levels
    auto srcAt = [&](int x, int y) { return c.src.at(x, H - 1 - y); };
    std::vector<float> pol((size_t)W * H * 4);
    forRows(H, W, [&](int j) {
        const float rr = ((float)j / Hf) * 2.8284270763397217f;
        for (int i = 0; i < W; ++i) {
            const float a = (float)((double)(((float)i / Wf) * 2.f - 1.f) * 3.141592653589793);
            float px = (float)rm::sin((double)a) * rr, py = (float)rm::cos((double)a) * rr;      // correctly rounded, as VEGAS's sinf/cosf
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
    const int sx = su.sx, sy = su.sy, bw = su.bw, bh = su.bh;
    std::vector<float> smallPic((size_t)gw * gh * 4);
    const double inv = su.inv;
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
            float* o = &smallPic[((size_t)gy * gw + gx) * 4];
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
            const float* col = &smallPic[((size_t)gy * gw + gx) * 4];
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
            const float ang = (float)rm::atan2((double)X, (double)Y);
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

// Timecode: the time burnt in as grey text in an opaque box (VEGAS's OpenCL kernel writes (g, g, g, 1) where its bitmap is, the picture elsewhere). The string, the box
// and its place are VEGAS's own arithmetic (vfxtimecode.h, read from Vfx1.ofx). The glyph bitmaps and metrics are GDI's GetGlyphOutline on Windows, as in VEGAS; elsewhere
// Reno's own font model (vfxtc::modelFont), so the box is only exact where the model's metrics are (the plain size).
#ifdef _WIN32
// The 16 glyphs "0123456789;:.,+-" from GDI as VEGAS asks for them: Arial, weight 700, GRAY8 with the Width / Height settings as the glyph matrix.
static bool timecodeGdiFont(int cell, double sx, double sy, vfxtc::Font& F)
{
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf = {};
    lf.lfHeight = cell;
    lf.lfWeight = 700;
    lf.lfCharSet = 1;
    lf.lfOutPrecision = 7;
    static const wchar_t face[] = L"Arial";
    for (int i = 0; i < 6; ++i) lf.lfFaceName[i] = face[i];
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    // (VEGAS keeps Width and Height as floats and splits the float, widened to double, into the whole part and the fraction x 65536, both cut: the same here)
    auto fixedOf = [](double v, FIXED& f) {
        double ip = 0;
        const double fr = std::modf((double)(float)v, &ip);
        f.value = (short)ip;
        f.fract = (WORD)(int)(fr * 65536.0);
    };
    MAT2 m = {};
    fixedOf(sx, m.eM11);
    fixedOf(sy, m.eM22);
    bool ok = font != nullptr;
    static const wchar_t chars[17] = L"0123456789;:.,+-";
    for (int i = 0; i < 16 && ok; ++i) {
        GLYPHMETRICS gm = {};
        const DWORD size = GetGlyphOutlineW(dc, chars[i], GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
        if (size == GDI_ERROR) { ok = false; break; }
        vfxtc::Glyph& g = F.slot[i];
        g.bw = (int)gm.gmBlackBoxX; g.bh = (int)gm.gmBlackBoxY;
        g.ox = gm.gmptGlyphOrigin.x; g.oy = gm.gmptGlyphOrigin.y;
        g.adv = gm.gmCellIncX;
        g.g.assign((size_t)std::max(0, g.bw * g.bh), 0);
        if (size == 0 || g.bw <= 0 || g.bh <= 0) continue;
        std::vector<unsigned char> raw(size);
        if (GetGlyphOutlineW(dc, chars[i], GGO_GRAY8_BITMAP, &gm, size, raw.data(), &m) == GDI_ERROR) { ok = false; break; }
        const int stride = (g.bw + 3) & ~3;                    // (rows are padded to 4 bytes)
        for (int y = 0; y < g.bh; ++y)
            for (int x = 0; x < g.bw; ++x) {
                const size_t at = (size_t)y * stride + x;
                const int v = at < raw.size() ? (int)raw[at] * 4 : 0;
                g.g[(size_t)y * g.bw + x] = (unsigned char)std::min(255, v);
            }
    }
    if (old) SelectObject(dc, old);
    if (font) DeleteObject(font);
    DeleteDC(dc);
    return ok;
}
#endif

// Timecode's text box for this frame: the string, the font (GDI on Windows) and the layout (false: nothing to draw, the picture goes through as it is)
static bool timecodeBox(Ctx& c, int W, int H, vfxtc::Layout& L)
{
    if (W <= 0 || H <= 0) return false;
    const double fps = c.fps > 0 && c.fps < 10000 ? c.fps : 30.0;
    // the time since the start of the clip in 100 ns ticks, snapped to the frame grid (all of that is in vfxtc::text)
    const long long ticks = vegasTicks(c.time, fps);
    const int fmt = std::max(0, std::min(11, c.i("TimecodeFormat")));
    const std::string s = vfxtc::text(fmt, ticks, fps);
    const double sx = clampd(c.d("Width"), 0.05, 8.0), sy = clampd(c.d("Height"), 0.05, 8.0);
    const int cell = std::max(1, std::min(W, H) / 12);                // the font's cell height
    vfxtc::Font font;
    bool have = false;
#ifdef _WIN32
    have = timecodeGdiFont(cell, sx, sy, font);
#endif
    if (!have) font = vfxtc::modelFont(cell, sx, sy);
    L = vfxtc::layout(font, s, W, H, c.i("HorizontalAlignment"), c.i("VerticalAlignment"));
    return L.ok;
}

bool timecodeGpuLayout(Ctx& c, vfxtc::Layout& L) { return timecodeBox(c, c.frameW, c.frameH, L); }

void timecodeFx(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    copyThrough(c);
    vfxtc::Layout L;
    if (!timecodeBox(c, W, H, L)) return;
    // the kernel: inside the bitmap's rectangle (one row and column in from its origin) the output is (g, g, g, 1) with g the bitmap's byte / 255
    const int yTop = std::max(0, H - L.oy - L.hb), yBot = std::min(H - 1, H - L.oy - 2);
    const int xLeft = std::max(0, L.ox + 1), xRight = std::min(W - 1, L.ox + L.wb - 1);
    for (int y = yTop; y <= yBot; ++y)
        for (int x = xLeft; x <= xRight; ++x) {
            const int v = vfxtc::boxValue(L, H, x, y);
            if (v < 0) continue;
            float* o = c.out.at(x, y);
            o[0] = o[1] = o[2] = (float)v / 255.f;
            o[3] = 1.f;
        }
}

// The settings Layer Dimensionality works from (shared with the GPU plan, so the host-side maths is the same)
void ldSetup(Ctx& c, int W, int H, LdSetup& o)
{
    o.shadowType = c.i("ShadowType"); o.glowType = c.i("GlowType");
    // Foreground Opacity thins the frame itself (VEGAS: 0 gives a fully transparent picture)
    o.fgOp = (float)clampd(c.d("ForegroundOpacity"), 0, 1);
    o.amount = (float)clampd(c.d("ShadowAmount"), 0, 1);
    if (o.shadowType == 2) {
        // (VEGAS does this in single precision: the light's direction, the offset as a part of the frame, then 14-bit fixed point)
        double Lx, Ly, Lz;
        c.xyz("LightLocation", Lx, Ly, Lz);
        const float fx = (float)Lx, fy = (float)Ly, fz = (float)Lz;
        const float ln = std::sqrt(fy * fy + fx * fx + fz * fz);
        float nx = 0.f, ny = 0.f, nz = 1.f;
        if (ln > 1.0000000116860974e-07f) { const float inv = 1.f / ln; nx = fx * inv; ny = fy * inv; nz = fz * inv; }
        float ox = 0.f, oy = 0.f;
        if (std::fabs((double)nz) > 9.999999747378752e-05) { const float k = (float)c.d("ShadowHeight") * 0.1f / nz; ox = -(nx * k); oy = -(ny * k); }
        // (the offset in 1/16384 pixels, cut towards zero; y counted from the bottom as VEGAS stores it)
        o.sx = (double)(int)((float)(W << 14) * ox) / 16384.0;
        o.sy = -(double)(int)((float)(H << 14) * oy) / 16384.0;
        o.bsz = (double)((float)c.d("ShadowBlur") * 0.6f * (float)std::max(W, H)) * 0.125;
    } else if (o.shadowType != 0) {
        const double off = c.d("ShadowHeight") * W * 0.15;
        o.blur = c.d("ShadowBlur") * W * 0.04 + c.px(0.3);
        o.ox = o.oy = (int)std::lround(off * 0.7);
    }
    if (o.glowType != 0) {
        const Color gc = c.color("GlowColor");
        o.gc[0] = (float)gc.r; o.gc[1] = (float)gc.g; o.gc[2] = (float)gc.b;
        o.glowRad = clampd(c.d("GlowAmount"), 0, 1) * W * 0.03 + c.px(0.5);
    }
    const double fg = c.d("ForegroundAmount");
    o.emboss = std::fabs(fg) > 0.001;
    if (o.emboss) {
        const double lx = c.d("LightIntensity") * 0.5 + 0.1;
        double Lx, Ly, Lz;
        c.xyz("LightLocation", Lx, Ly, Lz);
        const double mx = std::max(std::fabs(Lx), std::fabs(Ly));
        o.eox = mx > 1e-6 ? Lx / mx : -1.0; o.eoy = mx > 1e-6 ? -Ly / mx : -1.0;      // (the frame's y runs down)
        const double heightGain = 3.0 / clampd(Lz, 0.3, 30.0);
        o.sigma = c.px(1.5 + c.d("EmbossRoundness") * 4);
        o.gain = (float)(fg * lx * 3 * c.scale * heightGain);
    }
}

// Layer Dimensionality as VEGAS's OpenCL path draws it (vfxlayerdim.h): float maths on the frame, rows bottom first, then the 8-bit store of the host.
vfxld::Settings ldOclSettings(Ctx& c)
{
    auto fin = [](double v, double lo, double hi, double dflt) { return (v == v && v >= lo && v <= hi) ? v : (v == v ? (v < lo ? lo : hi) : dflt); };
    const int shadowType = c.i("ShadowType"), glowType = c.i("GlowType");
    const Color g1 = c.color("GlowColor"), g2 = c.color("GlowColor2");
    const double gc1[3] = {fin(g1.r, -1e6, 1e6, 0), fin(g1.g, -1e6, 1e6, 0), fin(g1.b, -1e6, 1e6, 0)};
    const double gc2[3] = {fin(g2.r, -1e6, 1e6, 0), fin(g2.g, -1e6, 1e6, 0), fin(g2.b, -1e6, 1e6, 0)};
    double Lx = 0, Ly = 0, Lz = 1;
    c.xyz("LightLocation", Lx, Ly, Lz);
    const double ll[3] = {fin(Lx, -1e6, 1e6, 0), fin(Ly, -1e6, 1e6, 0), fin(Lz, -1e6, 1e6, 1)};
    return vfxld::makeSettings(
        (shadowType == 1 || shadowType == 2) ? shadowType : 0, (glowType >= 1 && glowType <= 4) ? glowType : 0,
        fin(c.d("ShadowHeight"), -1e6, 1e6, 0), fin(c.d("ShadowBlur"), -1e6, 1e6, 0), fin(c.d("ShadowAmount"), -1e6, 1e6, 0), gc1, gc2,
        fin(c.d("GlowAmount"), -1e6, 1e6, 0), fin(c.d("ForegroundAmount"), -1e6, 1e6, 0), fin(c.d("BackgroundAmount"), -1e6, 1e6, 0),
        fin(c.d("EmbossRoundness"), -1e6, 1e6, 0), ll, fin(c.d("LightIntensity"), -1e6, 1e6, 0), fin(c.d("LightAmbience"), -1e6, 1e6, 0),
        fin(c.d("Shininess"), -1e6, 1e6, 0), fin(c.d("ForegroundOpacity"), -1e6, 1e6, 1), 1.0);
}

void layerDimensionalityOpenCl(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const vfxld::Settings st = ldOclSettings(c);
    const size_t row = (size_t)W * 4;
    std::vector<float> in((size_t)W * H * 4), res((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) std::memcpy(&in[(size_t)(H - 1 - y) * row], &c.src.px[(size_t)y * row], row * sizeof(float));
    vfxld::render(in.data(), W, H, st, res.data());
    for (int y = 0; y < H; ++y) {
        const float* r = &res[(size_t)(H - 1 - y) * row];
        float* o = c.out.at(0, y);
        for (size_t k = 0; k < row; ++k) o[k] = c.host8 ? oclStore12(r[k]) : r[k];
    }
}

// Layer Dimensionality: a shadow, glow and emboss on frames with transparency.
void layerDimensionality(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.openCl && W > 0 && H > 0) { layerDimensionalityOpenCl(c); return; }
    LdSetup su;
    ldSetup(c, W, H, su);
    const int shadowType = su.shadowType, glowType = su.glowType;
    std::vector<float> alpha((size_t)W * H);
    for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = c.src.px[i * 4 + 3];
    c.out.px = c.src.px;
    const float fgOp = su.fgOp;
    if (fgOp < 1.f) for (auto& v : c.out.px) v *= fgOp;
    if (shadowType == 2) {
        // Inner shadow, as VEGAS builds it: the frame's alpha shifted by a sub-pixel offset along the light (zero outside), blurred by the edge-replicating Deriche
        // Gaussian, black weighted by (1 - blurred) * alpha * amount.
        const double sx = su.sx, sy = su.sy, bsz = su.bsz;
        Buf pl; pl.resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double px = x - sx, py = y - sy;
                const int x0 = (int)std::floor(px), y0 = (int)std::floor(py);
                const double fx = px - x0, fy = py - y0;
                auto g = [&](int xx, int yy) { return (xx < 0 || yy < 0 || xx >= W || yy >= H) ? 0.0 : (double)alpha[(size_t)yy * W + xx]; };
                const float v = (float)((g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy);
                float* o = pl.at(x, y);
                o[0] = o[1] = o[2] = o[3] = v;
            }
        vegasDericheBlur(pl, bsz, bsz, false);
        const float amount = su.amount;
        for (int i = 0; i < W * H; ++i) {
            float* o = &c.out.px[(size_t)i * 4];
            const float v = std::min(1.f, std::max(0.f, (1.f - pl.px[(size_t)i * 4 + 3]) * amount * alpha[(size_t)i]));
            for (int q = 0; q < 3; ++q) o[q] *= (1.f - v);
            o[3] = v + o[3] * (1.f - v);
        }
    } else if (shadowType != 0) {
        const double blur = su.blur;
        std::vector<float> sh((size_t)W * H, 0.f);
        const int ox = su.ox, oy = su.oy;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                // (VEGAS repeats the frame's edge: an opaque frame's shadow covers the whole frame)
                const int sx = std::min(W - 1, std::max(0, x - ox)), sy = std::min(H - 1, std::max(0, y - oy));
                sh[(size_t)y * W + x] = alpha[(size_t)sy * W + sx];
            }
        gaussianBlurPlane(sh, W, H, blur, blur);
        // under the frame: where the frame is not opaque (the shadow's alpha put under the frame's)
        const float amount = su.amount;
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
        const double rad = su.glowRad;
        std::vector<float> g = alpha;
        gaussianBlurPlane(g, W, H, rad, rad);
        const bool inner = glowType == 2 || glowType == 1;
        const float cr = su.gc[0], cg = su.gc[1], cb = su.gc[2];
        for (size_t i = 0; i < g.size(); ++i) {
            const float a = inner ? std::max(0.f, g[i] - alpha[i]) : std::max(0.f, alpha[i] - g[i]) * 2;
            if (a <= 0.f) continue;
            float* o = &c.out.px[i * 4];
            o[0] = cr * a + o[0] * (1 - a); o[1] = cg * a + o[1] * (1 - a); o[2] = cb * a + o[2] * (1 - a); o[3] = a + o[3] * (1 - a);
        }
    }
    if (su.emboss) {
        const double ox = su.eox, oy = su.eoy;
        std::vector<float> sm = alpha;
        const double sigma = su.sigma;
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
        const float gain = su.gain;
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

// Deform: squeeze, shear and curve the frame's edges. VEGAS's own code (sftrans1.dll, CDeform; see vfxdeform.h): the picture's eight edge points are mapped (curves, squeezes,
// shear), the result is the box around them (Center image puts the middle of the frame back in the middle), every pixel of the box asks the approximate inverse where it comes
// from, and takes the average of samples along the two diagonals of its footprint. An 8-bit frame goes through the 8-bit sampler, a float frame through the float one; what is
// outside the picture is the Background.
// The settings of Deform, shared by the CPU code and the Vulkan plan (vkgeom.cpp).
struct DeformSettings {
    double P[10] = {0};
    double amount = 0, quality = 0.85;
    bool centre = false;
    double br = 0, bg = 0, bb = 0, ba = 0;      // the background, premultiplied
};
void deformSettings(Ctx& c, DeformSettings& d)
{
    auto setting = [&](const char* name) { const double v = c.d(name); return std::isfinite(v) ? clampd(v, 0.0, 1.0) : 0.0; };
    // the order of VEGAS's list: shear (horizontal, vertical), squeeze (top, bottom, left, right), curve (top, bottom, left, right)
    const double P[10] = {setting("ShearHorizontal"), setting("ShearVertical"), setting("SqueezeTop"), setting("SqueezeBottom"), setting("SqueezeLeft"), setting("SqueezeRight"),
                          setting("CurveTop"), setting("CurveBottom"), setting("CurveLeft"), setting("CurveRight")};
    for (int i = 0; i < 10; ++i) d.P[i] = P[i];
    d.amount = setting("Amount");
    d.quality = 0.85;        // (the host's best render quality: the footprint average; its lower ones are a bilinear and a nearest pixel)
#ifdef RENO_DEFORM_QUALITY_TEST
    // (a build for the GPU check only: the other two qualities, which no host setting reaches)
    if (const char* q = std::getenv("RENO_DEFORM_QUALITY")) d.quality = std::atof(q);
#endif
    d.centre = c.b("CenterImage");
    const Color bgc = c.color("Background");
    const auto fix = [](double v) { return std::isfinite(v) ? clampd(v, 0.0, 1.0) : 0.0; };
    d.ba = fix(bgc.a); d.br = fix(bgc.r) * d.ba; d.bg = fix(bgc.g) * d.ba; d.bb = fix(bgc.b) * d.ba;
}

void deform(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    if (W < 2 || H < 2) { c.out = c.src; return; }      // (the original divides by W - 1 and H - 1)
    DeformSettings ds;
    deformSettings(c, ds);
    const double* P = ds.P;
    const double amount = ds.amount, quality = ds.quality;
    const bool centre = ds.centre;
    const double br = ds.br, bg = ds.bg, bb = ds.bb, ba = ds.ba;
    if (!(c.host8 || c.levels)) {
        const float bgf[4] = {(float)br, (float)bg, (float)bb, (float)ba};
        if (!vfdeform::run<float>(c.src.px.data(), W, H, (ptrdiff_t)W * 4, P, amount, quality, centre, bgf, c.out.px.data(), (ptrdiff_t)W * 4))
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) std::memcpy(c.out.at(x, y), bgf, sizeof bgf);
        return;
    }
    // the 8-bit frame as VEGAS holds it: bytes blue, green, red, alpha
    std::vector<uint8_t> in((size_t)W * H * 4), out((size_t)W * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        const float* s = c.src.at(0, y);
        uint8_t* d = &in[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, s += 4, d += 4) {
            d[0] = (uint8_t)v8::toLevel(s[2]);
            d[1] = (uint8_t)v8::toLevel(s[1]);
            d[2] = (uint8_t)v8::toLevel(s[0]);
            d[3] = (uint8_t)v8::toLevel(s[3]);
        }
    }
    const uint8_t bg8[4] = {(uint8_t)v8::toLevel((float)bb), (uint8_t)v8::toLevel((float)bg), (uint8_t)v8::toLevel((float)br), (uint8_t)v8::toLevel((float)ba)};
    if (!vfdeform::run<uint8_t>(in.data(), W, H, (ptrdiff_t)W * 4, P, amount, quality, centre, bg8, out.data(), (ptrdiff_t)W * 4))
        for (size_t i = 0; i < out.size(); i += 4) std::memcpy(&out[i], bg8, 4);
    for (int y = 0; y < H; ++y) {
        float* o = c.out.at(0, y);
        const uint8_t* s = &out[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, o += 4, s += 4) {
            o[0] = v8::fromLevel(s[2]);
            o[1] = v8::fromLevel(s[1]);
            o[2] = v8::fromLevel(s[0]);
            o[3] = v8::fromLevel(s[3]);
        }
    }
}

// Bump Map: VEGAS's plug-in step for step (its OpenCL kernel and host code; the light's pixel position is measured from the bottom row, as the kernel's rows are). The normal is a 3x3 Prewitt gradient of the chosen channel (premultiplied 0..1) times 255 * BumpHeight^2 / 16 * max(W, H) / 640
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
    const float cosCut = rm::cos((float)c.d("Focus") * 0.7853981852531433f);
    const float invCut = 1.f / std::max(1.f - cosCut, 1e-5f);
    const float unit = 2.f / mx, cx = W * 0.5f, cy = H * 0.5f;
    auto hAt = [&](int x, int y) { return c.src.at(x, y)[chan]; };
    const bool store8 = c.host8;     // an 8-bit frame gets the kernel's write_imagef store
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
                const float px = ((float)x - cx) * unit, py = (cy - (float)(H - 1 - y)) * unit;   // (the kernel's row index counts from the bottom: y_kernel = H - 1 - y)
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
            const float spec = rm::pow(std::max(rz, 0.01f), power);
            const float k = std::max(0.f, i8 * (kd * ndl + ks * spec) * att + amb2);
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            o[0] = s[0] * k; o[1] = s[1] * k; o[2] = s[2] * k; o[3] = s[3];
            if (store8) for (int q = 0; q < 4; ++q) o[q] = oclStore12(o[q]);
        }
    });
}

void bumpMap(Ctx& c) { bumpMapRender(c, "LocationSrc", "LocationDst"); }

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno

namespace reno {
namespace ofx {
namespace builtin {
// for the Vulkan plan (vkgeom.cpp): the same set-up as deform(): the geometry's doubles (as vfdeform holds them), the box and the background
bool deformGpu(Ctx& c, int W, int H, DeformGpu& o)
{
    if (W < 2 || H < 2) return false;
    more::DeformSettings ds;
    more::deformSettings(c, ds);
    vfdeform::Geom g;
    vfdeform::setup(g, W, H, ds.P, ds.amount, ds.quality);
    vfdeform::bounds(g, W, H, ds.centre);
    const double dd[20] = {g.ww, g.hh, g.w2, g.h2, g.sheX, g.sheY, g.sqX0, g.sqXk, g.sqY0, g.sqYk, g.cuY0, g.cuY1, g.cuY0b, g.cuY1b, g.cuX0, g.cuX1, g.cuX0b, g.cuX1b, g.offX, g.offY};
    o.tab.clear();
    for (int i = 0; i < 20; ++i) gpuPushDouble(o.tab, dd[i]);
    o.flags = (g.fShear ? 1u : 0u) | (g.fSqX ? 2u : 0u) | (g.fSqY ? 4u : 0u) | (g.fCuY ? 8u : 0u) | (g.fCuX ? 16u : 0u) | (g.amount != 0.0 ? 32u : 0u);
    o.mode = !(g.quality < 0.85) ? 2 : (!(0.6 > g.quality) ? 1 : 0);
    o.bx0 = g.boxX0; o.by0 = g.boxY0; o.ow = g.outW; o.oh = g.outH;
    // (vfdeform::run gives up on a box too big to make: the frame is then all background)
    if (g.outW < 0 || g.outH < 0 || (double)g.outW * (double)g.outH > 67108864.0 || (double)g.outW * (double)g.outH > 8.0 * W * H + 4096.0) o.ow = o.oh = 0;
    o.q8 = c.host8 || c.levels;
    const double bgd[4] = {ds.br, ds.bg, ds.bb, ds.ba};
    for (int k = 0; k < 4; ++k) o.bg[k] = o.q8 ? (float)v8::toLevel((float)bgd[k]) : (float)bgd[k];
    return true;
}
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
