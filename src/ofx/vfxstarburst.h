// Starburst with VEGAS's OpenCL kernels (Vfx1.ofx, the Starburst processor: the picture is boosted above the threshold, convolved with a star by FFT and added back at Strength).
// What is exact: the order of the steps and the sums (boost, additive gain, the 8-bit store) and the host's star generator (0x180560f40): the polygons it draws, their float
// arithmetic, the colour table, the conversion of the drawn bitmap to the bytes handed to the kernel and the scale 1 / sum. What VEGAS draws with GDI+ (a path gradient brush over each
// polygon) is drawn by Windows' own GDI+ in Reno on Windows (gdipflat.h: the same calls in the same order) and by the model below elsewhere.
#pragma once
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "gdipflat.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace more {
namespace vfxstar {

struct Pt { float x, y; };

// One polygon of the generator: the vertices as the generator computes them in float (the first one repeated at the end, as the path is built from them), the colour at the middle
// (the surround is black), and the point counts of its FillPolygon calls (the burst with 27 spikes is filled twice: with all 54 vertices, then with the first 16).
struct Shape {
    std::vector<Pt> pts;
    uint32_t centre = 0;
    std::vector<int> fills;
    int translations = 1;           // how often the origin was moved to the middle of the bitmap (the last burst moves it a second time: it reuses the graphics of the one before)
    bool fresh = true;
};

// Everything the generator does before it draws: the bitmap is n x n (n = 2S + 1) with the origin moved to its middle (n / 2, between pixels S and S + 1).
struct Plan {
    int S = 0, n = 0;
    Shape star;
    bool bursts = false;
    Shape burst1, burst2;
    float w1 = 0.f, w2 = 0.f;       // how much of each burst is mixed into the star
};

// The generator's float arithmetic and constants (all read from Vfx1.ofx).
inline Plan makePlan(int points, double orientationDeg, double radiusPx, double centerBurst)
{
    Plan P;
    if (!(radiusPx >= 0) || !std::isfinite(radiusPx)) radiusPx = 0;
    if (radiusPx > 30000.0) radiusPx = 30000.0;
    if (!std::isfinite(orientationDeg)) orientationDeg = 0;
    if (!std::isfinite(centerBurst)) centerBurst = 0;
    if (points > 1000000) points = 1000000;
    const int S = (int)(radiusPx * 0.5860000252723694);                           // cvttsd2si of radius x 0.586
    P.S = S;
    P.n = 2 * S + 1;
    if (points < 2) points = 2;
    const int m = std::min(points, 6);
    const float R = (float)((double)(float)S * 2.0);                              // the tips are at 2S, twice as far as the bitmap reaches
    const float twoPi = 6.28318548f, toRad = 0.0174532924f;
    const float start = std::fmod((float)orientationDeg + 270.f, 360.f) * toRad;   // rows count downwards: 270 degrees points up
    const float step = twoPi / (float)points;
    P.star.centre = 0xFF404040u;
    for (int i = 0; i < m; ++i) {
        float a = (float)i * step + start;
        P.star.pts.push_back({rm::cos(a) * R, rm::sin(a) * R});
        a += step * 0.5f;
        P.star.pts.push_back({rm::cos(a) * 0.75f, rm::sin(a) * 0.75f});
    }
    P.star.pts.push_back(P.star.pts[0]);
    P.star.fills.push_back(2 * m);
    if (centerBurst > 0) {
        P.bursts = true;
        // the spike lengths are a table in the DLL (System.Random values), 8 for the first burst and 27 for the second
        static const float t1[8] = {0.0f, -0.6764289140701294f, 0.7472606897354126f, 0.2130359560251236f, -0.2235686033964157f, -0.9010393619537354f, -0.6725515723228455f, -0.7372854948043823f};
        static const float t2[27] = {0.0f, -0.8665060997009277f, -0.8596809506416321f, 0.5495302677154541f,
                                     0.02227853052318096f, 0.5949811339378357f, 0.6546165943145752f, -0.6680824160575867f,
                                     0.4722612500190735f, -0.4795672595500946f, 0.012009701691567898f, -0.5394255518913269f,
                                     -0.22571910917758942f, -0.5680387616157532f, -0.958378255367279f, -0.7303327918052673f,
                                     0.48166346549987793f, -0.6098178625106812f, 0.7740791440010071f, 0.7222012281417847f,
                                     -0.2822698652744293f, -0.792595386505127f, 0.05916610360145569f, -0.28248995542526245f,
                                     0.4150109589099884f, 0.16094966232776642f, 0.2410966455936432f};
        const float cb = (float)centerBurst;
        P.w1 = cb * 0.25f;
        P.w2 = cb * 0.2f;
        const float len1 = R * 0.2f, len2 = R * 0.4f;
        const float pi4 = 0.7853981852531433f, pi8 = 0.39269909262657166f, a27 = 0.2327105700969696f, h27 = 0.1163552850484848f;
        P.burst1.centre = 0xFFFFFFFFu;
        for (int i = 0; i < 8; ++i) {
            const float len = (t1[i] * 0.3f + 1.0f) * len1;
            float a = (float)i * pi4 + start;
            P.burst1.pts.push_back({rm::cos(a) * len, rm::sin(a) * len});
            a += pi8;
            P.burst1.pts.push_back({rm::cos(a) * 7.5f, rm::sin(a) * 7.5f});
        }
        P.burst1.pts.push_back(P.burst1.pts[0]);
        P.burst1.fills.push_back(16);
        P.burst2.centre = 0xFFFFFFFFu;
        P.burst2.translations = 2;
        P.burst2.fresh = false;
        for (int i = 0; i < 27; ++i) {
            const float len = (t2[i] * 0.75f + 1.0f) * len2;
            float a = (float)i * a27 + start;
            P.burst2.pts.push_back({rm::cos(a) * len, rm::sin(a) * len});
            a += h27;
            P.burst2.pts.push_back({rm::cos(a) * 3.75f, rm::sin(a) * 3.75f});
        }
        P.burst2.pts.push_back(P.burst2.pts[0]);
        P.burst2.fills.push_back(54);
        P.burst2.fills.push_back(16);
    }
    return P;
}

// What the generator hands to the kernel: n x n bytes (rows downwards) and the scale 1 / sum.
struct Kernel {
    int n = 0;
    std::vector<uint8_t> bytes;
    float scale = 1.f;
};

// The generator's table of byte -> float (filled from both ends, so the values below 128 are 1 - (255 - i) / 255).
inline const float* byteTable()
{
    static float tbl[256];
    static bool filled = false;
    if (!filled) {
        const float inv = 0.00392156886f;
        for (int i = 0; i < 256; ++i) {
            tbl[i] = (float)i * inv;
            tbl[255 - i] = 1.0f - (float)i * inv;
        }
        filled = true;
    }
    return tbl;
}

// The model of GDI+'s path gradient fill of one polygon (centre colour at the origin, black on the outline, straight in each triangle of the fan from the origin to an outline edge, no
// anti-aliasing): the blue byte of every pixel in 'img' (n x n) that the polygon covers is set. What was found by fitting VEGAS's pictures: the pixel at (x, y) lies at the point (x, y) (not
// its middle) about the origin, which is at n / 2 (twice that for the last burst), and the corners are held to 1/16 pixel (the fixed point GDI+ draws in), which is what makes the star
// of Points 4 at Orientation 0 (four spikes along the axes, thinner than a pixel row) draw nothing.
inline void drawModel(std::vector<uint8_t>& img, int n, const Shape& sh)
{
    const int m = (int)sh.pts.size() - 1;
    if (m < 3) return;
    const double cx = n * 0.5 * sh.translations, cy = n * 0.5 * sh.translations;
    const double centre = (double)(sh.centre & 255u);
    std::vector<double> ax((size_t)m), ay((size_t)m), bx((size_t)m), by((size_t)m), det((size_t)m);
    auto fixed = [&](float v) { return std::floor(((double)v + cx) * 16.0 + 0.5) / 16.0 - cx; };
    for (int i = 0; i < m; ++i) {
        ax[(size_t)i] = fixed(sh.pts[(size_t)i].x); ay[(size_t)i] = fixed(sh.pts[(size_t)i].y);
        bx[(size_t)i] = fixed(sh.pts[(size_t)i + 1].x); by[(size_t)i] = fixed(sh.pts[(size_t)i + 1].y);
        det[(size_t)i] = ax[(size_t)i] * by[(size_t)i] - ay[(size_t)i] * bx[(size_t)i];
    }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double qx = x - cx, qy = y - cy;
            for (int i = 0; i < m; ++i) {
                if (det[(size_t)i] == 0.0) continue;
                const double u = (qx * by[(size_t)i] - qy * bx[(size_t)i]) / det[(size_t)i];
                const double v = (ax[(size_t)i] * qy - ay[(size_t)i] * qx) / det[(size_t)i];
                if (u >= 0 && v >= 0 && u + v <= 1) {
                    img[(size_t)y * n + x] = (uint8_t)std::floor(centre * (1.0 - (u + v)) + 0.5);
                    break;
                }
            }
        }
}

// The generator's conversion: bytes -> floats through the table, the bursts mixed in, then bytes again (the float cut down) and the sum of the floats.
inline void finish(Kernel& K, const Plan& P, const std::vector<uint8_t>& star, const std::vector<uint8_t>& b1, const std::vector<uint8_t>& b2)
{
    const float* tbl = byteTable();
    const size_t cnt = (size_t)P.n * P.n;
    std::vector<float> f(cnt);
    for (size_t i = 0; i < cnt; ++i) f[i] = tbl[star[i]];
    if (P.bursts) {
        const float w[2] = {P.w1, P.w2};
        const std::vector<uint8_t>* b[2] = {&b1, &b2};
        for (int k = 0; k < 2; ++k) {
            const float one = 1.0f - w[k];
            for (size_t i = 0; i < cnt; ++i) f[i] = w[k] * tbl[(*b[k])[i]] + one * f[i];
        }
    }
    K.n = P.n;
    K.bytes.assign(cnt, 0);
    float sum = 0.f;
    for (size_t i = 0; i < cnt; ++i) {
        const int v = (int)(f[i] * 255.0f);
        K.bytes[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        sum += f[i];
    }
    K.scale = sum > 1.0e-7f ? 1.0f / sum : 1.0f;
}

// The star kernel as the host makes it. points 2 .. 6, orientationDeg, radiusPx = Radius x the longer side, centerBurst 0 .. 1. 'gdi' true asks for the real GDI+ where Reno has it.
// False when there is nothing to draw (no kernel).
inline bool makeKernel(Kernel& K, int points, double orientationDeg, double radiusPx, double centerBurst, bool gdi = true)
{
    const Plan P = makePlan(points, orientationDeg, radiusPx, centerBurst);
    K = Kernel();
    if (P.n < 1 || (double)P.n * P.n > 1.0e8) return false;
    const size_t cnt = (size_t)P.n * P.n;
    std::vector<uint8_t> star(cnt, 0), b1, b2;
    bool drawn = false;
    if (gdi) {
        std::vector<const Shape*> shapes;
        shapes.push_back(&P.star);
        if (P.bursts) { shapes.push_back(&P.burst1); shapes.push_back(&P.burst2); }
        std::vector<gdip::Shape> in;
        for (const Shape* s : shapes) {
            gdip::Shape g;
            g.pts.resize(s->pts.size());
            for (size_t i = 0; i < s->pts.size(); ++i) { g.pts[i].x = s->pts[i].x; g.pts[i].y = s->pts[i].y; }
            g.centre = s->centre;
            g.fills = s->fills;
            g.translations = s->translations;
            g.freshGraphics = s->fresh;
            in.push_back(g);
        }
        std::vector<std::vector<uint8_t> > out;
        if (gdip::drawGradientShapes(P.n, in, out) && out.size() == shapes.size()) {
            star = out[0];
            if (P.bursts) { b1 = out[1]; b2 = out[2]; }
            drawn = true;
        }
    }
    if (!drawn) {
        drawModel(star, P.n, P.star);
        if (P.bursts) {
            b1.assign(cnt, 0);
            b2.assign(cnt, 0);
            drawModel(b1, P.n, P.burst1);
            drawModel(b2, P.n, P.burst2);
        }
    }
    finish(K, P, star, b1, b2);
    return true;
}

}  // namespace vfxstar
}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
