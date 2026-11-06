// VEGAS Page Peel, Page Loop and Page Roll as VEGAS draws them with GPU acceleration on: the float OpenCL kernels of the three transitions (Vfx1.ofx), re-implemented
// from reading them (not fitted; checked against the captured pictures and against the kernels run on the CPU, see tests/engine/vfxpage_test.cpp). No dependencies, so a
// test program can include it alone.
//
// What they do: every output pixel is averaged over 2 x 2 samples placed 0.333 and 0.666 into the pixel (rows counted from the bottom, as the kernel's pictures are). A sample's
// offset from the frame's centre is turned by the transition's angle into (a, b): a along the direction of travel, b across it. The page is drawn as a few layers on top of the
// picture underneath; each layer maps (a, b) to a position (u, v) on the page, an opacity and an amount of the light colour, the position is turned back into frame
// coordinates, the page is read there with the kernel's own bilinear read (a 2 x 2 blend at pixel coordinates, nothing outside the frame is transparent black), the light is
// mixed in and the result mixed over what is below.
//   Peel: two layers - the page (flat part, then the curl over the fold) and its back, seen through the page (the incoming picture is underneath).
//   Loop: three layers - the page lying flat and rising on a ramp, the near half of the loop, the far half.
//   Roll: four layers - the page, the roll's back, its inner turn (0.9 of the radius) and its front.
// The settings the host hands the kernels (the setup of the CPU processors in float: positions, opacity as whole levels for Peel and Roll, the light colour) are in Setup structs.
#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include "../core/rmath.h"

namespace reno {
namespace ofx {
namespace vfxpage {

struct Img {
    const float* p = nullptr;     // RGBA floats, rows bottom first
    int w = 0, h = 0;
};

// the card's float to int (NaN gives 0, out of range saturates)
inline int cvtInt(float v)
{
    if (!(v == v)) return 0;
    if (v >= 2147483520.f) return INT_MAX;
    if (v <= -2147483648.f) return INT_MIN;
    return (int)v;
}

inline int cvtIntD(double v)
{
    if (!(v == v)) return 0;
    if (v >= 2147483647.0) return INT_MAX;
    if (v <= -2147483648.0) return INT_MIN;
    return (int)v;
}

// a texel; outside the picture reads are transparent black (CLK_ADDRESS_CLAMP)
inline void texel(const Img& im, int x, int y, float* o)
{
    if (x < 0 || y < 0 || x >= im.w || y >= im.h) { o[0] = o[1] = o[2] = o[3] = 0.f; return; }
    const float* q = im.p + ((size_t)y * im.w + x) * 4;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[3];
}

// the kernel's own bilinear read (f0): the four neighbours of (px, py) as the coordinates stand, weights from the fraction, summed in the kernel's order
inline void bilinear(const Img& im, float px, float py, float* o)
{
    const float flx = std::floor(px), fly = std::floor(py);
    const float fx = std::min(px - flx, 0.99999994f), fy = std::min(py - fly, 0.99999994f);
    const int x0 = cvtInt(flx), y0 = cvtInt(fly);
    const float v7 = 1.0f - fy, v8 = 1.0f - fx;
    float t00[4], t10[4], t01[4], t11[4];
    texel(im, x0, y0, t00);
    texel(im, x0 + 1, y0, t10);
    texel(im, x0, y0 + 1, t01);
    texel(im, x0 + 1, y0 + 1, t11);
    const float w00 = v7 * v8, w10 = v7 * fx, w01 = fy * v8, w11 = fy * fx;
    for (int k = 0; k < 4; ++k) o[k] = w00 * t00[k] + w10 * t10[k] + w01 * t01[k] + w11 * t11[k];
}

// what one drawn layer says about a sample: where on the page, the opacity, the light amount and its colour
struct LayerHit {
    float u = 0, v = 0;
    float alpha = 1.f;
    float light = 0.f;
    const float* lightCol = nullptr;
};

// The frame's geometry for the kernel: the angle's sine and cosine, the rows' scale (1 here) and the half sizes.
struct Geo {
    int w = 0, h = 0;
    float sn = 0, cs = 1, scale = 1;
    float w2 = 0, h2 = 0, iscale = 1;
};

inline Geo makeGeo(int w, int h, float sn, float cs, float scale)
{
    Geo g;
    g.w = w; g.h = h; g.sn = sn; g.cs = cs; g.scale = scale;
    g.w2 = (float)w * 0.5f; g.h2 = (float)h * 0.5f;
    g.iscale = 1.0f / scale;
    return g;
}

// (a, b) of the sample at pixel position (sx, sy)
inline void sampleAB(const Geo& g, float sx, float sy, float& a, float& b)
{
    const float yy = (sy - g.h2) * g.scale, xx = sx - g.w2;
    a = xx * g.cs + yy * g.sn;
    b = xx * -g.sn + yy * g.cs;
}

// A layer's page position back to the frame, the page read there and mixed into col (RGBA).
inline void drawLayer(const Geo& g, const Img& paper, const LayerHit& t, float* col)
{
    const float xb = t.u * g.cs + t.v * -g.sn;
    const float yb = t.u * g.sn + t.v * g.cs;
    float fy = yb * g.iscale + g.h2;
    float fx = xb + g.w2;
    const int iy = cvtInt(fy), ix = cvtInt(fx);
    if ((unsigned)ix < (unsigned)g.w && (unsigned)iy < (unsigned)g.h) {
        fx = std::fmin(std::fmax(fx, 0.5f), (float)g.w - 0.5f);       // (OpenCL's clamp: a NaN becomes the lower bound)
        fy = std::fmin(std::fmax(fy, 0.5f), (float)g.h - 0.5f);
        float s[4];
        bilinear(paper, fx, fy, s);
        if (t.light > 0.0f)
            for (int k = 0; k < 4; ++k) s[k] = s[k] + (t.lightCol[k] - s[k]) * t.light;
        for (int k = 0; k < 4; ++k) col[k] = col[k] + (s[k] - col[k]) * t.alpha;
    }
}

inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

// ------------------------------------------------------------------ Page Peel

struct PeelSetup {
    float sn = 0, cs = 1, scale = 1;
    float slide = 0;                // c3c
    float flatEnd = 0;              // c44: where the fold starts
    float c48 = 0, c4c = 0;         // 2 c44 + pi R, pi R + c44
    float R = 0, iR = 0;            // fold radius and its inverse
    float edge = 0;                 // c54: end of the fold
    float persp = 0;                // Perspective x 0.125
    float opacity = 0;              // whole levels / 255
    float inv = 0;                  // (255 - levels) / 255
    float lightBase = 200.f / 255.f;
    float lightScale = 0;           // 200 x 4 / pi / 255
    float lightCol[4] = {1, 1, 1, 1};
};

// A sample (a, b) of Page Peel: col is what lies below (the incoming picture); paper is the outgoing one.
inline void peelSample(const Geo& g, const PeelSetup& s, const Img& paper, float a, float b, float* col)
{
    const float a1 = a - s.slide;
    const float kPi = 3.14159265359f;
    LayerHit t;
    t.lightCol = s.lightCol;
    // layer 0: the page's front
    if (a1 <= s.edge) {
        t.light = 0.f;
        if (a1 < s.flatEnd) {
            t.u = a1;
            t.v = b;
            t.alpha = 1.0f;
        } else {
            const float th = rm::asinf((a1 - s.flatEnd) * s.iR);
            const float cs = rm::cosf(th);
            t.alpha = s.opacity + (s.inv * (kPi - th)) * 0.318309886184f;
            const float u = s.R * th + s.flatEnd;
            const float sc = s.persp * (cs - 1.0f) + 1.0f;
            t.u = u * sc;
            t.v = b * sc;
        }
        drawLayer(g, paper, t, col);
    }
    // layer 1: its back
    if (a1 <= s.edge) {
        float sc;
        float u;
        if (a1 < s.flatEnd) {
            t.light = 0.f;
            u = -a1 + s.c48;
            t.alpha = s.opacity;
            sc = s.persp * -2.0f + 1.0f;
        } else {
            const float th = rm::asinf((a1 - s.flatEnd) * s.iR);
            const float cs = rm::cosf(th);
            t.light = s.lightBase - std::fabs(th - 0.785398163397f) * s.lightScale;
            t.alpha = s.opacity + (s.inv * th) * 0.318309886184f;
            u = -s.R * th + s.c4c;
            sc = s.persp * -(1.0f + cs) + 1.0f;
        }
        t.u = u * sc;
        t.v = b * sc;
        drawLayer(g, paper, t, col);
    }
}

// ------------------------------------------------------------------ Page Loop

struct LoopSetup {
    float sn = 0, cs = 1, scale = 1;
    float p = 0;                    // progress
    float x0 = 0;                   // where the loop sits (c1c)
    float R = 0;                    // loop radius
    float circ = 0;                 // its circumference
    float ramp = 0;                 // length of the ramp
    float shift = 0;                // c3c
    float lo = 0, hi = 0;           // x0 -/+ R
    float alphaFlat = 0;            // opacity of the paper at the flat end (0..1)
    float alphaEnd = 1.f;
    float persp = 0;
    float lightScale = 200.f / 255.f;
    float lightCol[4] = {1, 1, 1, 1};
};

// A sample of Page Loop: col is what lies below (the outgoing picture); paper is the incoming one.
inline void loopSample(const Geo& g, const LoopSetup& s, const Img& paper, float a, float b, float* col)
{
    const float kPi = 3.14159265359f;
    const float R = s.R < 0.001f ? 0.001f : s.R;
    const float iR = 1.0f / R, iRamp = 1.0f / s.ramp;
    LayerHit t;
    t.lightCol = s.lightCol;
    // layer 0: the paper lying flat and on the ramp
    {
        t.u = a < s.x0 ? a - s.shift : a + s.circ - s.shift;
        t.v = b;
        t.alpha = s.alphaFlat;
        t.light = 0.f;
        if (1.0f > s.p && a >= s.x0) {
            const float th = rm::asinf((a - s.x0) * iRamp);
            const float gm = 1.0f - (1.0f - rm::cosf(th)) * s.persp;
            t.u = gm * ((s.x0 + s.circ) + (th * s.ramp - s.shift));
            t.v = gm * b;
            t.alpha = mixf(s.alphaFlat, s.alphaEnd, th * 0.636619772368f);
            t.light = s.lightScale * (1.0f - std::fabs((th - 0.785398163397f) * 1.273239544735f));
        }
        drawLayer(g, paper, t, col);
    }
    if (a > s.lo && a < s.hi) {
        const bool neg = a < s.x0;
        const float d = neg ? s.x0 - a : a - s.x0;
        const float th = rm::asinf(d * iR);
        const float cs = rm::cosf(th);
        const float lightAmount = s.lightScale * (1.0f - std::fabs((th - 0.785398163397f) * 1.273239544735f));
        t.alpha = mixf(s.alphaFlat, s.alphaEnd, th * 0.636619772368f);
        // layer 1: the near half
        {
            const float gm = (cs - 1.0f) * s.persp + 1.0f;
            const float w = neg ? 6.283185307180f - th : th;
            t.u = gm * (s.x0 + (w * R - s.shift));
            t.v = gm * b;
            t.light = neg ? 0.0f : lightAmount;
            drawLayer(g, paper, t, col);
        }
        // layer 2: the far half
        {
            const float w = neg ? kPi + th : kPi - th;
            const float gm = -(1.0f + cs) * s.persp + 1.0f;
            t.u = gm * (s.x0 + (w * R - s.shift));
            t.v = gm * b;
            t.light = neg ? lightAmount : 0.0f;
            drawLayer(g, paper, t, col);
        }
    }
}

// ------------------------------------------------------------------ Page Roll

struct RollSetup {
    float sn = 0, cs = 1, scale = 1;
    float slide = 0;
    float R = 0, R2 = 0, iR = 0, iR2 = 0;
    float D = 0;                    // where the roll starts
    float c7c = 0;                  // pi R + D
    float edge = 0;                 // D + R
    float persp = 0;
    float opacity = 0;              // whole levels / 255
    float inv = 0;                  // (255 - levels) / 255
    float lightBase = 200.f / 255.f;
    float lightScale = 0;           // 200 x 4 / pi / 255
    float lightCol[4] = {1, 1, 1, 1};
};

// A sample of Page Roll: col is the incoming picture; paper the outgoing one.
inline void rollSample(const Geo& g, const RollSetup& s, const Img& paper, float a, float b, float* col)
{
    const float kPi = 3.14159265359f;
    const float a1 = a - s.slide;
    static const float black[4] = {0.f, 0.f, 0.f, 1.f};
    LayerHit t;
    t.lightCol = black;
    // layer 0: the page and the first turn
    if (a1 < s.edge) {
        t.light = 0.f;
        if (a1 < s.D) {
            t.u = a1;
            t.v = b;
            t.alpha = 1.0f;
        } else {
            const float th = rm::asinf((a1 - s.D) * s.iR);
            t.alpha = s.inv * ((kPi - th) * 0.3183098862f) + s.opacity;
            const float u = s.R * th + s.D;
            const float sc = (rm::cosf(th) - 1.0f) * s.persp + 1.0f;
            t.u = u * sc;
            t.v = b * sc;
        }
        drawLayer(g, paper, t, col);
    }
    // layer 1: the roll's back
    if (a1 >= s.D - s.R && a1 < s.D + s.R2) {
        const bool neg = a1 < s.D;
        const float d = neg ? s.D - a1 : a1 - s.D;
        const float th = rm::asinf(d * (neg ? s.iR : s.iR2));
        const float w = neg ? kPi - th : th + 6.2831853072f;
        const float base = neg ? s.c7c : s.D;
        const float u = s.R * w + base;
        const float sc = (rm::cosf(th) - 1.0f) * s.persp + 1.0f;
        t.light = 0.f;
        t.alpha = s.inv * ((kPi - th) * 0.3183098862f) + s.opacity;
        t.u = u * sc;
        t.v = b * sc;
        drawLayer(g, paper, t, col);
    }
    // layers 2 and 3: the inner turn (0.9 R) and the front, shaded dark on the shadow side and lit on the other
    for (int layer = 2; layer <= 3; ++layer) {
        const float Rl = layer == 2 ? s.R2 : s.R;
        const float iRl = layer == 2 ? s.iR2 : s.iR;
        const float hi = layer == 2 ? s.D + s.R2 : s.edge;
        if (!(a1 >= s.D - Rl && a1 < hi)) continue;
        const bool neg = a1 < s.D;
        const float d = neg ? s.D - a1 : a1 - s.D;
        const float th = rm::asinf(d * iRl);
        t.lightCol = neg ? black : s.lightCol;
        const float v80 = neg ? (1.5707963268f - th) * 0.5f : std::fabs(th - 0.7853981634f);
        const float v81 = neg ? th : -th;
        t.light = s.lightScale * -v80 + s.lightBase;
        const float u = layer == 2 ? Rl * (3.0f * 3.1415926536f + v81) + s.D : Rl * v81 + s.c7c;
        const float sc = s.persp * -(1.0f + rm::cosf(th)) + 1.0f;
        t.alpha = s.inv * (th * 0.3183098862f) + s.opacity;
        t.u = u * sc;
        t.v = b * sc;
        drawLayer(g, paper, t, col);
    }
}

// ------------------------------------------------------------------ the host's setup (the CPU processors' setup in float, the same numbers the kernels are handed)

// sinf / cosf as VEGAS's runtime gives them: the double result rounded to float, but results within a thousandth of a float step of the halfway point go to the larger magnitude
// (sinf of 210 degrees is -0.5, not -0.49999997).
inline float roundLikeCrt(double t)
{
    const float f = (float)t;
    if (!std::isfinite(f) || (double)f == t) return f;
    const float o = std::nextafter(f, t > (double)f ? HUGE_VALF : -HUGE_VALF);
    const double mid = 0.5 * ((double)f + (double)o);
    if (std::fabs(t - mid) < 1e-3 * std::fabs((double)o - (double)f)) return std::fabs(o) > std::fabs(f) ? o : f;
    return f;
}
inline float crtSin(float a) { return roundLikeCrt(rm::sin((double)a)); }
inline float crtCos(float a) { return roundLikeCrt(rm::cos((double)a)); }

// The direction of travel: (360 - angle) degrees, turned into 0..2 pi when it points to the right (the pixel aspect, square here, enters there in VEGAS)
inline float travelAngle(float a)
{
    const float cs = crtCos(a);
    if (cs > 1e-6f) {
        float r = rm::atan2f(crtSin(a), cs);
        if (r < 0) r += 6.2831854820251465f;
        return r;
    }
    return a;
}

// How far the frame reaches along the direction of travel (W cos + H sin, the angle folded into 0..90 degrees), worked out as each processor does it.
inline float spanLoop(float ang, float W, float H)
{
    const float diag = std::sqrt(W * W + H * H);
    const int ic = cvtInt(ang * 572.9578247070312f);
    const int m = ic - (ic / 1800) * 1800;
    float phi = (float)m * 0.001745329238474369f;
    if (phi > 1.5707963705062866f) phi = 3.1415927410125732f - phi;
    phi -= rm::atan2f(H, W);
    return rm::cosf(phi) * diag;
}
inline float spanPeel(float ang, float W, float H)
{
    const double kPi = 3.14159265358979323846;
    const float diag = std::sqrt(W * W + H * H);
    const int ic = cvtIntD((double)(ang * 1800.f) / kPi);
    const int m = ic - (ic / 1800) * 1800;
    float phi = (float)(m * kPi / 1800.0);
    if (phi > 1.5707963705062866f) phi = (float)(kPi - (double)phi);
    phi -= rm::atan2f(H, W);
    return diag * rm::cosf(phi);
}
inline float spanRoll(float ang, float W, float H)
{
    const float PIf = 3.1415927410125732f;
    const float diag = std::sqrt(W * W + H * H);
    const int ic = cvtInt((ang * 1800.f) / PIf);
    const int m = ic - (ic / 1800) * 1800;
    float phi = ((float)m * PIf) / 1800.f;
    if (phi > 1.5707963705062866f) phi = PIf - phi;
    phi -= rm::atan2f(H, W);
    return diag * rm::cosf(phi);
}

inline void lightRgba(const double* rgb, float* o)
{
    o[0] = (float)rgb[0]; o[1] = (float)rgb[1]; o[2] = (float)rgb[2]; o[3] = 1.f;
}

// Page Peel's settings: angle in degrees, progress 0..1, fold radius, slide amount, opacity, perspective (all 0..1), light colour RGB.
inline PeelSetup peelSetup(int w, int h, double progress, double angleDeg, double fold, double slideAmount, double opacity, double perspective, const double* rgb)
{
    PeelSetup s;
    const float ang = travelAngle((360.f - (float)angleDeg) * 0.01745329238474369f);
    const float W = (float)w, Hh = (float)h;
    const float span = spanPeel(ang, W, Hh);
    const float p = (float)progress, FR = (float)fold, sl = (float)slideAmount;
    const float mn = std::min(W, Hh), R = mn * FR;
    float slide = (p - FR * 1.5707963705062866f) * sl * mn;
    if (0.f > slide) slide = 0.f;
    const float x3 = span * 0.5f - (R + span) * p - slide;
    const float piR = (float)((double)R * 3.14159265358979323846);
    const int op = cvtInt((float)opacity * 255.f) & 255, k = (255 - op) & 255;
    s.sn = crtSin(ang); s.cs = crtCos(ang);
    s.slide = slide; s.flatEnd = x3; s.c48 = (x3 + x3) + piR; s.c4c = piR + x3; s.R = R; s.iR = R > 0.f ? 1.f / R : 1e14f; s.edge = x3 + R;
    s.persp = (float)perspective * 0.125f;
    s.opacity = (float)op / 255.f; s.inv = (float)k / 255.f;
    s.lightScale = 200.f * 1.2732394933700562f / 255.f;
    lightRgba(rgb, s.lightCol);
    return s;
}

// Page Loop's: angle (degrees), LoopPosition, LoopRadius, PaperOpacity, Perspective.
inline LoopSetup loopSetup(int w, int h, double progress, double angleDeg, double loopPosition, double loopRadius, double paperOpacity, double perspective, const double* rgb)
{
    LoopSetup s;
    const float ang = travelAngle((float)((360.0 - angleDeg) * 0.01745329238474369));
    const float span = spanLoop(ang, (float)w, (float)h);
    const float p = (float)progress, L = (float)loopPosition, O = (float)paperOpacity, Rr = (float)loopRadius;
    const float x0 = (((1.f - L) * (p * p) + L) - 0.5f) * span;
    const float half = span * 0.5f;
    const float Rset = half * Rr;
    const float circ = Rset * 6.2831854820251465f;
    const float ramp = (half - x0) / rm::sinf(1.3962634f);
    const float op = ((1.f - O) * (p * p) + O) * 255.f;
    s.sn = crtSin(ang); s.cs = crtCos(ang);
    s.p = p; s.x0 = x0; s.R = Rset; s.circ = circ; s.ramp = ramp;
    s.shift = (((half + x0) + circ) + ramp * 1.3962634f) * (1.f - p);
    s.lo = x0 - Rset; s.hi = x0 + Rset;
    s.alphaFlat = op / 255.f;
    s.persp = (float)perspective * 0.125f;
    lightRgba(rgb, s.lightCol);
    return s;
}

// Page Roll's: angle (degrees), fold radius, slide amount, opacity, perspective.
inline RollSetup rollSetup(int w, int h, double progress, double angleDeg, double fold, double slideAmount, double opacity, double perspective, const double* rgb)
{
    RollSetup s;
    const float PIf = 3.1415927410125732f;
    const float ang = travelAngle((float)((360.0 - angleDeg) * 0.01745329238474369));
    const float W = (float)w, Hh = (float)h;
    const float span = spanRoll(ang, W, Hh);
    const float p = (float)progress, FR = (float)fold, sl = (float)slideAmount;
    const float mn = std::min(W, Hh), R = mn * FR, R2 = R * 0.9f;
    float slide = (p - FR * 1.5707963705062866f) * sl * mn;
    if (0.f > slide) slide = 0.f;
    const float D = span * 0.5f - (R + span) * p - slide;
    const int op = cvtInt((float)(opacity * 255.0)) & 255;
    s.sn = crtSin(ang); s.cs = crtCos(ang);
    s.slide = slide; s.R = R; s.R2 = R2; s.iR = R > 0.f ? 1.f / R : 1e14f; s.iR2 = R2 > 0.f ? 1.f / R2 : 1e14f;
    s.D = D; s.c7c = R * PIf + D; s.edge = D + R;
    s.persp = (float)perspective * 0.125f;
    s.opacity = (float)op / 255.f; s.inv = (float)((255 - op) & 255) / 255.f;
    s.lightScale = 200.f * 1.2732394933700562f / 255.f;
    lightRgba(rgb, s.lightCol);
    return s;
}

// ------------------------------------------------------------------ the frame

// The 2 x 2 samples of every pixel; fn(geo, x, y, col) draws one sample into col. below: what lies under the page (read at the pixel, as the kernels do). out: RGBA floats, rows bottom first.
template <class SampleFn>
inline void renderFrame(const Geo& g, const Img& below, float* out, int rowBegin, int rowEnd, SampleFn fn)
{
    static const float offs[4][2] = {{0.333f, 0.333f}, {0.666f, 0.333f}, {0.333f, 0.666f}, {0.666f, 0.666f}};
    for (int y = rowBegin; y < rowEnd; ++y) {
        for (int x = 0; x < g.w; ++x) {
            float base[4];
            texel(below, x, y, base);
            float acc[4] = {0.f, 0.f, 0.f, 0.f};
            for (int s = 0; s < 4; ++s) {
                float col[4] = {base[0], base[1], base[2], base[3]};
                float a, b;
                sampleAB(g, (float)x + offs[s][0], (float)y + offs[s][1], a, b);
                fn(a, b, col);
                for (int k = 0; k < 4; ++k) acc[k] += col[k];
            }
            float* o = out + ((size_t)y * g.w + x) * 4;
            for (int k = 0; k < 4; ++k) o[k] = acc[k] * 0.25f;
        }
    }
}

}  // namespace vfxpage
}  // namespace ofx
}  // namespace reno
