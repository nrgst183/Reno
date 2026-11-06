// VEGAS Radial Pixelate and Spherize as VEGAS's OpenCL kernels draw them (GPU acceleration on): re-implemented from the decoded kernels (vfx1.dll, the user's own
// install), in floating point, picture rows bottom first, RGBA floats. Reads are the card's linear sampler (oclLinear4); the 8-bit conversion of the final picture is the
// host's (vegasTiesDown), and the middle pictures of Radial Pixelate are 8-bit textures when the host's frame is (oclUnormLevel; the offset 0.46 is the fit that
// matches VEGAS's captured pictures best, as for Radial Blur, see oclWriteLevel).
#pragma once
#include "../core/rmath.h"
#include "builtins_more_internal.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace more {
namespace vfxpolar {

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

// Spherize: one kernel. cx, cy: the Center as floats (0..1); sx, sy: the scalings (sy already equal to sx when Proportional is on).
inline void spherize(const std::vector<float>& src, int W, int H, float cx, float cy, float amount, float sx, float sy, std::vector<float>& out)
{
    out.assign((size_t)W * H * 4, 0.f);
    const float fw = (float)W, fh = (float)H;
    const float v28 = ((cx * 2.0f) - 0.5f) * fw;
    const float v29 = ((cy * 2.0f) - 0.5f) * fh;
    const float v30 = 1.0f / (sx + 0.0001f);
    const float v31 = 1.0f / (sy + 0.0001f);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float fx = (float)x, fy = (float)y;
            const float v33 = (fx - v28) * 2.0f / fw * v30;
            const float v34 = (fy - v29) * 2.0f / fh * v31;
            float r2 = v34 * v34 + v33 * v33;
            float* o = &out[((size_t)y * W + x) * 4];
            if (r2 >= 1.0f) {
                std::memcpy(o, &src[((size_t)y * W + x) * 4], sizeof(float) * 4);
                continue;
            }
            r2 = std::sqrt(r2);
            const float f = 1.0f - (rm::cos(r2 * 1.5707963267949f) * amount) * 0.8f;
            const float py = v29 + (fy - v29) * f;
            const float px = v28 + (fx - v28) * f;
            oclLinear4(src, W, H, px, py, o);
        }
    }
}

// Radial Pixelate, the three steps. q: the middle pictures are 8-bit textures. cx, cy: Center * 2 - 1 as floats. nx, ny: the group counts across and down (>= 1).
// type 1: groups down each column; otherwise along each row.
inline void radialPixelate(const std::vector<float>& src, int W, int H, float cx, float cy, int type, int nx, int ny, bool q, std::vector<float>& out)
{
    const float fw = (float)W, fh = (float)H;
    const float pi = 3.1415926535897932384f, two = 2.828427125f;
    std::vector<float> T((size_t)W * H * 4), U((size_t)W * H * 4);
    // 1: unroll (column = angle, row = radius)
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float xn = (float)x / fw, yn = (float)y / fh;
            const float a = (xn / 0.5f - 1.0f) * pi, r = yn * two;
            float px = r * rm::sin(a), py = r * rm::cos(a);
            px += cx; py += cy;
            px = (px / 2.0f + 0.5f) * fw; py = (py / 2.0f + 0.5f) * fh;
            float* o = &T[((size_t)y * W + x) * 4];
            oclLinear4(src, W, H, px, py, o);
            if (q) for (int k = 0; k < 4; ++k) o[k] = oclUnormLevel(o[k], 0.46f);
        }
    }
    // 2: the group averages
    if (type == 1) {
        const float g = fh / (float)ny;
        for (int x = 0; x < W; ++x) {
            float sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int y = 0; y < H; ++y) {
                ++n;
                const float* p = &T[((size_t)y * W + x) * 4];
                for (int k = 0; k < 4; ++k) sum[k] += p[k];
                if (std::fmod((float)y + 1.0f, g) < 1.0f || y == H - 1) {
                    for (int k = 0; k < 4; ++k) sum[k] /= (float)n;
                    for (int m = 0; m < n; ++m) {
                        float* o = &U[((size_t)(y - m) * W + x) * 4];
                        for (int k = 0; k < 4; ++k) o[k] = q ? oclUnormLevel(sum[k], 0.46f) : sum[k];
                    }
                    for (int k = 0; k < 4; ++k) sum[k] = 0.f;
                    n = 0;
                }
            }
        }
    } else {
        const float g = fw / (float)nx;
        for (int y = 0; y < H; ++y) {
            float sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int x = 0; x < W; ++x) {
                ++n;
                const float* p = &T[((size_t)y * W + x) * 4];
                for (int k = 0; k < 4; ++k) sum[k] += p[k];
                if (std::fmod((float)x + 1.0f, g) < 1.0f || x == W - 1) {
                    for (int k = 0; k < 4; ++k) sum[k] /= (float)n;
                    for (int m = 0; m < n; ++m) {
                        float* o = &U[((size_t)y * W + (x - m)) * 4];
                        for (int k = 0; k < 4; ++k) o[k] = q ? oclUnormLevel(sum[k], 0.46f) : sum[k];
                    }
                    for (int k = 0; k < 4; ++k) sum[k] = 0.f;
                    n = 0;
                }
            }
        }
    }
    // 3: roll up again
    out.assign((size_t)W * H * 4, 0.f);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float xn = (float)x / fw, yn = (float)y / fh;
            const float vx = (xn / 0.5f - 1.0f) - cx, vy = (yn / 0.5f - 1.0f) - cy;
            const float ang = rm::atan2(vx, vy), len = std::sqrt(vx * vx + vy * vy);
            const float u = ((ang / pi) / 2.0f + 0.5f) * fw, v = (len / two) * fh;
            oclLinear4(U, W, H, u, v, &out[((size_t)y * W + x) * 4]);
        }
    }
}

// The group counts the kernel host code makes from Strength.
inline void radialPixelateGroups(int W, int H, double strength, int& nx, int& ny)
{
    const double f = (1.0 - strength) * 0.5;
    nx = (int)(W * f + 1.0);
    ny = (int)(H * f + 1.0);
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;
}

}  // namespace vfxpolar
}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
