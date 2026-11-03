// VEGAS's Vignette as its OpenCL kernel draws it (the float path VEGAS takes with GPU acceleration on), written from the decoded kernel and checked against it
// (see the port notes). All in 32-bit floats in the kernel's order. The kernel's rows run bottom first, so y here is counted from the bottom of the frame.
#pragma once
#include <cmath>

namespace reno {
namespace ofx {
namespace vfxvig {

struct Params {
    int w = 0, h = 0;
    float outer = 1.f, inner = 0.f, aspect = 1.f;
    float cx = 0.5f, cy = 0.5f;         // centre as fractions of the frame, y up
    float rotation = 0.f;               // degrees
    float exponent = 1.f;               // 100 / (Feather + 0.001)^2
    float par = 1.f;                    // pixel aspect ratio of the picture
    bool invert = false;
};

// The kernel's setup values that do not change from pixel to pixel
struct Setup {
    float w1, h1, centreX, centreY, c, s, asp;
};

inline Setup setup(const Params& p)
{
    Setup u;
    u.w1 = (float)(p.w - 1);
    u.h1 = (float)(p.h - 1);
    u.centreX = u.w1 * p.cx;
    u.centreY = u.h1 * (1.0f - p.cy);
    const float a = p.rotation * 0.01745329f;
    u.c = std::cos(a);
    u.s = std::sin(a);
    u.asp = (p.aspect - 1.0f) * 0.5f + 1.0f;
    return u;
}

// How far (0..1) the pixel at column x, row y (from the bottom) is mixed towards the vignette colour (before the colour's alpha)
inline float amount(const Params& p, const Setup& u, int x, int y)
{
    float w = u.w1, h = u.h1;
    float dx = (float)x - u.centreX;
    float dy = u.h1 - (float)y - u.centreY;
    if (p.par < 1.0f) { dx *= p.par; w *= p.par; }
    else { dy /= p.par; h /= p.par; }
    float rx = dx * u.c - dy * u.s;
    float ry = dx * u.s + dy * u.c;
    rx /= u.asp;
    ry *= u.asp;
    const float r = std::hypot(rx, ry);
    const float big = 0.5f * p.outer * std::hypot(w, h);
    const float inner = p.inner * big;
    const float den = (std::fabs(big - inner) < 0.000001f) ? 1.0f : big - inner;
    float t = (r - inner) / den;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    float a = 0.f;
    if (r > inner) {
        a = std::pow(t, p.exponent);
        a = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
    }
    return p.invert ? 1.0f - a : a;
}

}  // namespace vfxvig
}  // namespace ofx
}  // namespace reno
