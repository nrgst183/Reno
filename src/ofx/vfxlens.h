// VEGAS Lens Correction's picture, as read off its MagixCV plug-in (compatibility notes: Lens_Correction.md). Shared by the CPU effect and its GPU plan.
// Single precision, rows counted from the bottom (the OpenCL orientation): the distortion centre is an INDEX (W * x, H * y, no half pixel), a point at distance
// r (in half shorter sides) comes from f = (k3 u^3 + k2 u^2 + k1 u + 1 - k1 - k2 - k3) / Scale times as far, with k1 = RadialDistParam3, k2 = RadialDistParam2,
// k3 = RadialDistParam1 (the user interface order is reversed). The source is read bilinearly on the index grid; a neighbour outside the frame counts as
// transparent black (it is not repeated), the weights are the card's: fractions rounded to 1/256, and a place outside -1 < m < W (-1 < m < H) is transparent.
#pragma once
#include <algorithm>
#include <cmath>

namespace reno {
namespace ofx {
namespace builtin {

struct LensMapSetup {
    float cxp = 0, cyp = 0;     // centre in pixels (bottom-up y)
    float s = 0;                // 2 / min(W, H)
    float k1 = 0, k2 = 0, k3 = 0, d = 1;
    float invScale = 1;
};

inline LensMapSetup lensMapMake(int W, int H, double cx, double cy, double p1, double p2, double p3, double scale)
{
    LensMapSetup o;
    o.cxp = (float)W * (float)cx;
    o.cyp = (float)H * (float)cy;
    o.s = 2.0f / (float)std::min(W, H);
    o.k1 = (float)p3; o.k2 = (float)p2; o.k3 = (float)p1;
    o.d = 1.0f - o.k1 - o.k2 - o.k3;
    o.invScale = (float)(1.0 / scale);
    return o;
}

// Where the output pixel (x, y) (y counted from the bottom) reads from; false when that is outside the frame (then transparent). mx, my: index coordinates, y from the bottom.
inline bool lensMapPlace(const LensMapSetup& m, int W, int H, int x, int yb, float& mx, float& my)
{
    const float dx = (float)x - m.cxp, dy = (float)yb - m.cyp;
    const float u = std::sqrt(dx * dx + dy * dy) * m.s;
    float f = (((m.k3 * u + m.k2) * u + m.k1) * u + m.d);
    f = f * m.invScale;
    mx = dx * f + m.cxp;
    my = dy * f + m.cyp;
    return mx > -1.f && mx < (float)W && my > -1.f && my < (float)H;
}

// The bilinear read at (mx, my) of a picture whose pixel (x, yb) (y from the bottom) is get(x, yb, float* rgba); outside the frame reads zero. o: 4 floats.
template <class Get>
inline void lensMapRead(int W, int H, float mx, float my, Get get, float* o)
{
    const float fx0 = std::floor(mx), fy0 = std::floor(my);
    const int x0 = (int)fx0, y0 = (int)fy0;
    float fx = mx - fx0, fy = my - fy0;
    { fx = std::floor(fx * 256.f + 0.5f) / 256.f; fy = std::floor(fy * 256.f + 0.5f) / 256.f; }
    float v[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int xx = x0 + i, yy = y0 + j;
            if (xx >= 0 && xx < W && yy >= 0 && yy < H) get(xx, yy, v[j * 2 + i]);
        }
    for (int k = 0; k < 4; ++k) {
        const float t0 = v[0][k] + (v[1][k] - v[0][k]) * fx, t1 = v[2][k] + (v[3][k] - v[2][k]) * fx;
        o[k] = t0 + (t1 - t0) * fy;
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
