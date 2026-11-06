// Flash as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on; host at 0x1805cf6e0, kernels prog53). One picture is shown for the whole transition: the outgoing one up
// to the middle, the incoming one after it. A flash is added to it: tint x (brightness of the picture, blurred first when a diffusion is set) x S + a^2, where a is twice the distance of the
// progress from the end of its half and S = 1 - (1 - a)^2. The blur is the Deriche-type Gaussian of vfxgauss.h (a column pass, then a row pass over the sum of the two column results), every
// picture the kernels write being an image of the frame's format, so cut to levels in an 8-bit frame. Pictures are RGBA floats, the BOTTOM row first (the kernels' order).
// Nothing here depends on Reno's Ctx, so a test can drive it with plain arrays.
#pragma once
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "vfxgauss.h"

#if defined(__clang__)
#define RENO_FLASH_NO_FMA _Pragma("clang fp contract(off)")
#else
#define RENO_FLASH_NO_FMA
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace flashk {

struct Settings {
    float progress = 0.f;            // the transition's progress (a float, as the host holds it)
    double diffX = 0.0, diffY = 0.0; // HorizontalDiffusion, VerticalDiffusion
    double tint[3] = {0, 0, 0};      // Tint (r, g, b)
    float (*store8)(float) = nullptr;// the 8-bit store of a kernel's float result (null: a float frame)
};

struct Host {
    bool useTo = false;              // the incoming picture is the one shown
    float a2 = 0.f, S = 0.f;         // a^2 and S as the host works them out
    float sigmaX = 0.f, sigmaY = 0.f;
    bool blur = false;
};

// The host's numbers (pixel aspect 1).
inline Host host(int W, int H, const Settings& s)
{
    RENO_FLASH_NO_FMA
    Host h;
    float p = s.progress;
    if (!(0.5f > p)) { p = 1.0f - p; h.useTo = true; }
    const float a = p + p;
    h.a2 = a * a;
    const float om = 1.0f - a;
    h.S = 1.0f - om * om;
    const float hd = (float)s.diffX, vd = (float)s.diffY;
    const int dim = (int)std::max((float)W * 1.0f, (float)H * 1.0f);
    const float dimf = (float)dim;
    float sx = hd * 0.25f;
    sx = sx * dimf;
    sx = sx * h.S;
    sx = sx / 1.0f;
    sx = sx * 1.0f;
    float sy = vd * 0.25f;
    sy = sy * dimf;
    sy = sy * h.S;
    sy = sy * 1.0f;
    const float ln = rm::log(0.003921568859368563f);
    const float twoLn = ln + ln;
    sx = sx * sx;
    sx /= twoLn;
    h.sigmaX = std::sqrt(-sx);
    sy = sy * sy;
    sy /= twoLn;
    h.sigmaY = std::sqrt(-sy);
    h.blur = hd > 0.0f || vd > 0.0f;
    return h;
}

inline void cut(std::vector<float>& v, float (*store8)(float))
{
    if (store8)
        for (float& x : v) x = store8(x);
}

// The two kernels of the blur: the column pass of the vertical sigma (two pictures), the row pass over their sum (two pictures). r1 + r2 is the blurred picture.
inline void blurParts(const std::vector<float>& in, int W, int H, float sigmaX, float sigmaY, float (*store8)(float), std::vector<float>& r1, std::vector<float>& r2)
{
    auto plain = [](float a, float b, float c) { return a * b + c; };
    const vfxgauss::Coefsf cy = vfxgauss::toFloats(vfxgauss::build((double)sigmaY));
    const vfxgauss::Coefsf cx = vfxgauss::toFloats(vfxgauss::build((double)sigmaX));
    std::vector<float> t1, t2;
    vfxgauss::passColumns(cy, in, W, H, t1, t2, plain);
    cut(t1, store8);
    cut(t2, store8);
    std::vector<float> sumT((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) sumT[((size_t)x * H + y) * 4 + k] = t1[((size_t)y * W + x) * 4 + k] + t2[((size_t)y * W + x) * 4 + k];
    std::vector<float> ra, rb;
    vfxgauss::passColumns(cx, sumT, H, W, ra, rb, plain);
    r1.resize((size_t)W * H * 4);
    r2.resize((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k) {
                r1[((size_t)y * W + x) * 4 + k] = ra[((size_t)x * H + y) * 4 + k];
                r2[((size_t)y * W + x) * 4 + k] = rb[((size_t)x * H + y) * 4 + k];
            }
    cut(r1, store8);
    cut(r2, store8);
}

// The last kernel for one pixel: lum is the (blurred) picture's pixel to take the brightness of (the sum of the two blur pictures, kept in 0..1), src the shown picture's pixel.
inline void pixel(const Host& h, const double* tint, const float* lum, const float* src, float* o)
{
    RENO_FLASH_NO_FMA
    float v50[4];
    for (int k = 0; k < 4; ++k) v50[k] = std::fmin(std::fmax(lum[k], 0.0f), 1.0f);
    float l = v50[0] * 0.299f;
    l = l + v50[1] * 0.587f;
    l = l + v50[2] * 0.114f;
    l = l + v50[3] * 0.0f;
    l *= h.S;
    float v56[4];
    const float t[3] = {(float)tint[0], (float)tint[1], (float)tint[2]};
    for (int k = 0; k < 3; ++k) v56[k] = t[k] * l + h.a2;
    v56[3] = 1.0f * l + h.a2;
    for (int k = 0; k < 4; ++k) o[k] = v56[k] + src[k];
    o[3] = std::fmin(1.0f, v56[3] + src[3]);
}

// from, to: the two pictures; out: the result (all W x H x 4 floats, bottom row first).
inline void render(const std::vector<float>& from, const std::vector<float>& to, std::vector<float>& out, int W, int H, const Settings& s)
{
    out.assign((size_t)W * H * 4, 0.0f);
    if (W <= 0 || H <= 0) return;
    const Host h = host(W, H, s);
    const std::vector<float>& src = h.useTo ? to : from;
    std::vector<float> r1, r2;
    if (h.blur) blurParts(src, W, H, h.sigmaX, h.sigmaY, s.store8, r1, r2);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float lum[4];
        if (h.blur) for (int k = 0; k < 4; ++k) lum[k] = r1[i * 4 + k] + r2[i * 4 + k];
        else for (int k = 0; k < 4; ++k) lum[k] = src[i * 4 + k];
        float o[4];
        pixel(h, s.tint, lum, &src[i * 4], o);
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = s.store8 ? s.store8(o[k]) : o[k];
    }
}

}  // namespace flashk
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
