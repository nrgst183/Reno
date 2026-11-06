// VEGAS's Linear Blur as its OpenCL kernels draw it (Vfx1.ofx, GPU acceleration on): the picture is turned by the angle into a larger canvas (so the blur direction lies along the canvas
// columns), blurred along the columns by the recursive Gaussian of vfxgauss.h (a causal and an anti-causal pass, then their sum) and turned back. Every picture the kernels write in between is
// an 8-bit texture in an 8-bit project. Read from the kernels and the host code of the user's own VEGAS install; re-implemented here. No dependencies beyond vfxgauss.h, so a test program can
// include it alone. The reads and the stores are passed in (the shared OpenCL rules of builtins_more_internal.h), the rest is here.
#pragma once
#include "../core/rmath.h"
#include "vfxgauss.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxlinblur {

struct Geometry {
    float thf = 0.f;            // the angle the kernels turn by (radians, as a float)
    int Wc = 0, Hc = 0;         // the enlarged canvas
    double sigma = 0;           // of the Gaussian along the canvas columns
};

// Angle in degrees (0..360), amount 0..1, W x H the frame (pixel aspect 1, render scale 1: the host's other terms are 1 then).
inline Geometry geometry(int W, int H, double angle, double amount)
{
    const double pi = 3.141592653589793;
    Geometry g;
    double th = ((angle >= 180.0) ? (angle - 270.0) : (angle - 90.0)) * pi / 180.0;
    if (std::fabs(rm::cos(th)) > 1e-6) {
        th = rm::atan2(rm::sin(th), rm::cos(th));
        if (th < 0) th += 2 * pi;
    }
    g.thf = (float)th;
    const double ss = std::fabs(rm::sin(th)), cc = std::fabs(rm::cos(th));
    g.Wc = (int)std::ceil((double)H * ss + (double)W * cc);
    g.Hc = (int)std::ceil((double)H * cc + (double)W * ss);
    const double L = (double)std::max(W, H) * amount;
    const double t = L + 1.5;
    const float lf = rm::log(1.0f / 255.0f);
    const float lf2 = lf + lf;
    g.sigma = std::sqrt(-(t * t / (double)lf2));
    return g;
}

// Everything the three kernels get from the host, worked out in one place (shared with the Vulkan plan, vkblur.cpp). state 0: the kernels run; 1: the frame passes through (only the final
// write: amount 0); -1: the canvas is too small or too big to be drawn this way (the caller draws the CPU version).
struct Setup {
    int state = 1;
    Geometry g;
    float s1 = 0, c1 = 0, cx = 0, cy = 0, offx = 0, offy = 0;       // frame -> canvas
    float s2 = 0, c2 = 0, cx2 = 0, cy2 = 0;                         // canvas -> frame
    vfxgauss::Coefsf cf;
};
inline Setup prepare(int W, int H, double angle, double amount)
{
    Setup s;
    if (!(amount > 0.0) || W < 1 || H < 1) { s.state = 1; return s; }
    if (!(angle >= 0.0)) angle = 0.0;
    if (angle > 360.0) angle = 360.0;
    if (amount > 1.0) amount = 1.0;
    s.g = geometry(W, H, angle, amount);
    if (s.g.Wc < 2 || s.g.Hc < 2 || (double)s.g.Wc * s.g.Hc > 1.0e8) { s.state = -1; return s; }
    const int Wc = s.g.Wc, Hc = s.g.Hc;
    const float nth = -s.g.thf;
    s.s1 = rm::sin(nth); s.c1 = rm::cos(nth);
    s.cx = (float)Wc * 0.5f; s.cy = (float)Hc * 0.5f;
    s.offx = (float)std::nearbyint((double)(Wc - W)) / 2.0f; s.offy = (float)std::nearbyint((double)(Hc - H)) / 2.0f;
    s.s2 = rm::sin(s.g.thf); s.c2 = rm::cos(s.g.thf);
    s.cx2 = (float)W * 0.5f; s.cy2 = (float)H * 0.5f;
    s.cf = vfxgauss::toFloats(vfxgauss::build(s.g.sigma));
    s.state = 0;
    return s;
}

// mem: the frame, rows bottom first, RGBA floats 0..1. out: the same layout. samp(img, w, h, px, py, o): the kernels' bilinear read; store(v): the write of an 8-bit texture
// (q off: floats are kept as they are, only the last write is clamped to 0..1). Returns false (out untouched) when the frame is left as it is (amount 0 or too small a canvas).
template <class Samp, class Store>
inline bool run(const std::vector<float>& mem, int W, int H, double angle, double amount, bool q, Samp samp, Store store, std::vector<float>& out)
{
    out.assign((size_t)W * H * 4, 0.f);
    const Setup su = prepare(W, H, angle, amount);
    if (su.state == 1) {
        for (size_t i = 0; i < out.size(); ++i) out[i] = store(mem[i]);
        return true;
    }
    if (su.state < 0) return false;
    const Geometry g = su.g;
    const int Wc = g.Wc, Hc = g.Hc;
    const float s1 = su.s1, c1 = su.c1, cx = su.cx, cy = su.cy, offx = su.offx, offy = su.offy;
    // pass 1: the frame turned into the canvas
    std::vector<float> I3((size_t)Wc * Hc * 4);
    for (int y = 0; y < Hc; ++y)
        for (int x = 0; x < Wc; ++x) {
            const float u = ((float)x - cx) * c1 + ((float)y - cy) * s1 + cx;
            const float v = (-(((float)x - cx) * s1)) + ((float)y - cy) * c1 + cy;
            float* o = &I3[((size_t)y * Wc + x) * 4];
            samp(mem, W, H, u - offx, v - offy, o);
            if (q) for (int k = 0; k < 4; ++k) o[k] = store(o[k]);
        }
    // pass 2: the Gaussian along the canvas columns (causal and anti-causal into two pictures, then their sum)
    const vfxgauss::Coefsf cf = su.cf;
    std::vector<float> A, B;
    vfxgauss::passColumns(cf, I3, Wc, Hc, A, B, [](float a, float b, float cc2) { return a * b + cc2; });
    if (q) { for (float& v : A) v = store(v); for (float& v : B) v = store(v); }
    for (size_t i = 0; i < A.size(); ++i) { const float s = A[i] + B[i]; I3[i] = q ? store(s) : s; }
    // pass 3: back out of the canvas
    const float s2 = su.s2, c2 = su.c2, cx2 = su.cx2, cy2 = su.cy2;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float u = ((float)x - cx2) * c2 + ((float)y - cy2) * s2 + cx2;
            const float v = (-(((float)x - cx2) * s2)) + ((float)y - cy2) * c2 + cy2;
            float* o = &out[((size_t)y * W + x) * 4];
            samp(I3, Wc, Hc, u + offx, v + offy, o);
            for (int k = 0; k < 4; ++k) o[k] = q ? store(o[k]) : std::min(std::max(o[k], 0.f), 1.f);
        }
    return true;
}

}  // namespace vfxlinblur
}  // namespace ofx
}  // namespace reno
