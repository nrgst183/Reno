// VEGAS Stereoscopic 3D Adjust (the Filters plug-in, read off VEGAS Pro 18's code and checked against it and its pictures): the matrix and the warp are in vfxstereo.h. Reno has the left view:
//   1. the matrix of the frame as it lies in memory (bottom row first): offsets, Zoom, Keystone, Rotation, the flips of the left view (bit 0 of each Flip choice), the crop (Automatic: the largest
//      corner crop that leaves no edge of the transformed frame showing; or the Crop setting);
//   2. every pixel reads the frame at the inverse matrix's point: a float bilinear mix of the four texels (continuous coordinates, nothing outside), stored as floor(v + 0.46) in 8 bits (the
//      capture goes through this warp for every setting; VEGAS's 14-bit stretch routines are not what its pictures show);
//   3. the Left floating window: black over that part of the frame's width from the left edge (whole columns checked against VEGAS's pictures; a part of a column is a GUESS).
#include "builtins_mask_internal.h"
#include "vfxstereo.h"
#include "vkfilters.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
void vegasStereo3dAdjust(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 1 || H <= 1) {
        c.out.px = c.src.px;
        return;
    }
    const double hz = clampd(c.d("Horizontal"), -1, 1), vt = clampd(c.d("Vertical"), -1, 1), zoom = clampd(c.d("Zoom"), -0.95, 10);
    const double key = clampd(c.d("Keystone"), -1, 1), rot = clampd(c.d("Rotation"), -180, 180);
    // (the choices are 0 .. 3: bit 0 flips the left view's picture, bit 1 the right view's; Reno draws the left view)
    const int fhc = std::max(0, std::min(3, c.i("FlipHorizontal"))), fvc = std::max(0, std::min(3, c.i("FlipVertical")));
    const bool autoCrop = c.b("AutoCrop");
    const double crop = clampd(c.d("Crop"), 0, 0.5);
    const double fw = clampd(c.d("FloatingWindowLeft"), 0, 0.25);
    const vfxstereo::StereoMats sm = vfxstereo::stereoMatrix(W, H, hz, vt, zoom, key, rot, fhc, fvc, autoCrop, crop);
    // the frame as it lies in memory (bottom row first) in levels (8-bit frame: whole levels)
    const bool q = c.levels;
    std::vector<float> mem((size_t)W * H * 4), res;
    for (int y = 0; y < H; ++y) {
        const float* p = c.src.at(0, H - 1 - y);
        float* o = &mem[(size_t)y * W * 4];
        for (int i = 0; i < W * 4; ++i) {
            const float v = std::min(1.f, std::max(0.f, p[i])) * 255.f;
            o[i] = q ? (float)std::nearbyint(v) : v;
        }
    }
    vfxstereo::warp(mem, W, H, sm.af, q, res);
    // the left floating window: black columns (alpha opaque) from the left edge, the part of a column over in between (GUESS: only whole columns are in VEGAS's pictures)
    const double fcols = fw * W;
    const int full = (int)fcols;
    const double part = fcols - full;
    for (int y = 0; y < H; ++y) {
        float* row = &res[(size_t)y * W * 4];
        for (int x = 0; x < full && x < W; ++x) {
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0.f;
            row[x * 4 + 3] = 255.f;
        }
        if (part > 0 && full < W) {
            float* qp = row + (size_t)full * 4;
            for (int k = 0; k < 3; ++k) qp[k] = (float)(qp[k] * (1.0 - part));
            qp[3] = (float)(qp[3] * (1.0 - part) + 255.0 * part);
            if (q) for (int k = 0; k < 4; ++k) qp[k] = (float)std::nearbyint(qp[k]);
        }
    }
    for (int y = 0; y < H; ++y) {
        const float* r = &res[(size_t)(H - 1 - y) * W * 4];
        float* o = c.out.at(0, y);
        for (int i = 0; i < W * 4; ++i) o[i] = r[i] / 255.f;
    }
}

// for the Vulkan plan (vkgeom.cpp): the same set-up as vegasStereo3dAdjust, the warp's inverse matrix and the floating window's columns
bool stereoGpu(Ctx& c, int W, int H, StereoGpu& o)
{
    if (W <= 1 || H <= 1) return false;
    const double hz = clampd(c.d("Horizontal"), -1, 1), vt = clampd(c.d("Vertical"), -1, 1), zoom = clampd(c.d("Zoom"), -0.95, 10);
    const double key = clampd(c.d("Keystone"), -1, 1), rot = clampd(c.d("Rotation"), -180, 180);
    const int fhc = std::max(0, std::min(3, c.i("FlipHorizontal"))), fvc = std::max(0, std::min(3, c.i("FlipVertical")));
    const bool autoCrop = c.b("AutoCrop");
    const double crop = clampd(c.d("Crop"), 0, 0.5);
    const double fw = clampd(c.d("FloatingWindowLeft"), 0, 0.25);
    const vfxstereo::StereoMats sm = vfxstereo::stereoMatrix(W, H, hz, vt, zoom, key, rot, fhc, fvc, autoCrop, crop);
    const vfxstereo::Mat3 Mi = vfxstereo::mInverse(sm.af);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) o.mi[i * 3 + j] = Mi.m[i][j];
    const double fcols = fw * W;
    o.full = (int)fcols;
    o.part = fcols - o.full;
    o.levels = c.levels;
    return true;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
