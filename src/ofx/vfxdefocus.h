// Defocus's iris picture as VEGAS's OpenCL path builds it (Vfx1.ofx, host function 0x1803af280, iris builder 0x18039e710): a bitmap that the plug-in draws with GDI+ (a path
// gradient brush over a circle or a regular polygon, sharp edges), of which the middle 2 * half square of bytes is the convolution mask. On Windows Reno makes the same GDI+ calls
// (gdipflat.h: 32bppARGB bitmap, default smoothing and pixel offset, origin at the middle, path gradient brush from the path with the 255-entry preset blend, FillEllipseI /
// FillPolygon), so the bytes are VEGAS's. Elsewhere GDI+'s fill is re-done from what it is known to do (the shape is flattened to a polygon whose corners are held to 1/16 pixel as
// GDI+ does, the gradient runs from the polygon's mean point out to its edge in the triangles of a fan, the pixels whose position lies inside are filled, the position of pixel k
// being k) - the bytes agree with VEGAS's pictures to within a level on most pixels, not on all (GDI+'s own rounding inside its gradient is not known).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "gdipflat.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace vfxdf {

// The 255 grey levels of the gradient (the plug-in evaluates a Bezier-like blend in double in this order and truncates).
inline void lut(double curv, int out[255])
{
    const double k = (0.0 <= curv) ? 0.25 : 0.4;
    const double p = k * curv + 1.0;
    for (int i = 0; i < 255; ++i) {
        const double t = i / 254.0;
        double x2 = 1.0 - t, x4 = t * t, x1 = x2 * x2, x0 = t * 3.0, x3 = x1 * x2;
        x0 = x0 * x1; x1 = x4 * 3.0; x3 = x3 * p; x0 = x0 * p; x1 = x1 * x2; x4 = x4 * t;
        x3 = x3 + x0; x1 = x1 * p; x3 = x3 + x1; x3 = x3 + x4; x3 = x3 * 200.0;
        const int v = (int)x3;
        out[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
}

// The number of straight pieces GDI+ makes of a circle of radius r (four Beziers, each halved until the control points are within 1/4 pixel of the chord).
inline int circlePieces(double r)
{
    const double kPi = 3.14159265358979323846;
    for (int k = 0; k < 8; ++k) {
        const double quarter = (kPi / 2) / (double)(1 << k);
        const double sag = r * (1.0 - std::cos(quarter / 2));
        if (sag * 4.0 / 3.0 <= 0.25) return 4 << k;
    }
    return 4 << 8;
}

// The mask: 2 * half squared bytes (row by row, rows in the picture's bottom-up order), half returned. radPx: the iris radius in pixels (Radius / 100 x the larger side), shape 0 circle,
// else 1 .. 4 for 5 .. 8 sides, orientDeg in degrees, sx and sy the polygon's axis scales (pixel aspect), curv -1 .. 1.
inline void maskBytes(double radPx, double curv, double orientDeg, int shape, double sx, double sy, std::vector<uint8_t>& mask, int& halfOut, bool gdi = true)
{
    const double kPi = 3.14159265358979323846;
    const float r = (float)(int)(radPx * 0.95);
    const int half = (int)(r + 0.5f);
    const int S = 2 * half + 1;
    halfOut = half;
    mask.assign((size_t)(2 * half) * (size_t)(2 * half), 0);
    if (half < 1) return;
    int L[255];
    lut(curv, L);
    std::vector<double> vx, vy;
    std::vector<more::gdip::PointF> poly;           // the polygon's corners as the plug-in computes them in float, the first one repeated at the end
    if (shape > 0) {
        const int n = std::min(8, shape + 4);
        const float twoPi = 6.28318548f;
        const float step = twoPi / (float)n;
        float a = std::fmod((float)orientDeg * 0.0174532924f + (1.57079637f - step), twoPi);
        const float rx = r * (float)sx, ry = r * (float)sy;
        for (int i = 0; i < n; ++i) {
            poly.push_back({std::cos(a) * rx, std::sin(a) * ry});
            a = std::fmod(a + step, twoPi);
        }
        poly.push_back(poly[0]);
    }
    if (gdi) {
        uint32_t colors[255];
        float positions[255];
        for (int i = 0; i < 255; ++i) {
            colors[i] = 0xFF000000u | ((uint32_t)L[i] * 0x010101u);
            positions[i] = (float)((double)i / 254.0);
        }
        std::vector<uint8_t> bmp;
        if (more::gdip::drawIris(S, shape <= 0, r, poly, shape > 0 ? (int)poly.size() - 1 : 0, colors, positions, 255, bmp) && bmp.size() == (size_t)S * (size_t)S) {
            // the plug-in keeps the blue byte of the pixels (1 .. 2 half) in both directions, dropping the first row and column
            for (int row = 1; row <= 2 * half; ++row)
                for (int col = 1; col <= 2 * half; ++col)
                    mask[(size_t)(row - 1) * (size_t)(2 * half) + (size_t)(col - 1)] = bmp[(size_t)row * (size_t)S + (size_t)col];
            return;
        }
    }
    if (shape <= 0) {
        const int n = circlePieces((double)r);
        for (int i = 0; i < n; ++i) { vx.push_back(std::cos(2 * kPi * i / n) * (double)r); vy.push_back(std::sin(2 * kPi * i / n) * (double)r); }
    } else {
        for (size_t i = 0; i + 1 < poly.size(); ++i) { vx.push_back((double)poly[i].x); vy.push_back((double)poly[i].y); }
    }
    for (size_t i = 0; i < vx.size(); ++i) {                                   // GDI+ holds a corner to 1/16 pixel
        vx[i] = std::floor(vx[i] * 16.0 + 0.5) / 16.0;
        vy[i] = std::floor(vy[i] * 16.0 + 0.5) / 16.0;
    }
    const int n = (int)vx.size();
    vx.push_back(vx[0]);
    vy.push_back(vy[0]);
    double cx = 0, cy = 0;
    for (int i = 0; i <= n; ++i) { cx += vx[(size_t)i]; cy += vy[(size_t)i]; }
    cx /= (n + 1);
    cy /= (n + 1);
    std::vector<double> ax((size_t)n), ay((size_t)n), bx((size_t)n), by((size_t)n), det((size_t)n);
    for (int i = 0; i < n; ++i) {
        ax[(size_t)i] = vx[(size_t)i] - cx; ay[(size_t)i] = vy[(size_t)i] - cy;
        bx[(size_t)i] = vx[(size_t)i + 1] - cx; by[(size_t)i] = vy[(size_t)i + 1] - cy;
        det[(size_t)i] = ax[(size_t)i] * by[(size_t)i] - ay[(size_t)i] * bx[(size_t)i];
    }
    for (int row = 1; row <= 2 * half; ++row)
        for (int col = 1; col <= 2 * half; ++col) {
            const double px = col - S / 2.0, py = row - S / 2.0;
            const double qx = px - cx, qy = py - cy;
            double t = -1.0;
            for (int i = 0; i < n; ++i) {
                if (det[(size_t)i] == 0.0) continue;
                const double u = (qx * by[(size_t)i] - qy * bx[(size_t)i]) / det[(size_t)i];
                const double v = (ax[(size_t)i] * qy - ay[(size_t)i] * qx) / det[(size_t)i];
                if (u >= 0 && v >= 0 && u + v <= 1) { t = 1 - (u + v); break; }
            }
            if (t < 0) continue;
            const double pos = std::min(1.0, std::max(0.0, t)) * 254.0;
            const int i0 = std::min(253, (int)std::floor(pos));
            const double fr = pos - i0;
            const double val = L[i0] * (1 - fr) + L[i0 + 1] * fr;
            mask[(size_t)(row - 1) * (size_t)(2 * half) + (size_t)(col - 1)] = (uint8_t)std::floor(val + 0.5);
        }
}

}  // namespace vfxdf
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
