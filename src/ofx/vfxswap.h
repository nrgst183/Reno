// Swap as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on): the two pictures slide past each other along the chosen direction, the outgoing one
// in front for the first half and the incoming one in front for the second, with a border strip and a feather where the front picture ends. It is the Push family again:
// the host (0x180652840) works the same numbers out as Push's (float progress, border and feather as fractions of the length along the direction, a feather under one
// pixel dropped, a start s0 and a shift s3) and hands them to one of four kernels (prog67), one per direction. The kernel handles are created in the order of
// the names k...f11, f10, f12, f13, so directions 0 up, 1 down, 2 left, 3 right use kernels 1, 0, 2, 3.
//
// Written from the decoded code. Pictures are RGBA floats; x, y given to pixel() are the kernel's (y = 0 is the bottom row), see Pic in vfxwipes.h.
#pragma once
#include "vfxwipes.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct Swap {
    int W = 0, H = 0, dir = 0, kernel = 1;
    bool hwFilter = true;       // the card's own bilinear filter (fHWBL) rather than the kernel's four reads
    float L = 0, p = 0;         // v20.s0 and v20.s2
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    float col[4] = {0, 0, 0, 1};

    Swap(int w, int h, int direction, double progress, double borderSize, double borderFeather, const float* colour)
    {
        RENO_WIPES_NO_FMA
        W = w; H = h;
        dir = direction < 0 ? 0 : (direction > 3 ? 3 : direction);
        static const int kmap[4] = {1, 0, 2, 3};
        kernel = kmap[dir];
        const float pf = (float)progress;
        p = (0.0f > pf) ? 0.0f : std::fmin(1.0f, pf);
        L = (float)(dir > 1 ? W : H);
        const float border = (float)borderSize * L;
        float feather = (float)borderFeather * L;
        if (1.0f > feather) feather = 0.0f;
        const float x10 = L + border;
        const float x0 = ((x10 + feather) + feather) * p;
        const float x1 = x0 - feather;
        s3 = (0.0f > x1) ? 0.0f : std::fmin(x10, x1);
        const float a = (dir == 0 || dir == 3) ? x1 : (L + feather) - x0;
        s0 = a - 0.5f;
        s1 = border; s2 = feather;
        for (int k = 0; k < 4; ++k) col[k] = colour[k];
    }

    // the host's arguments for the kernel: v20 = (L, width, p, height), v21 = (s0, s1, s2, s3), v22 = colour
    void args(float* v20, float* v21, float* v22) const
    {
        v20[0] = L; v20[1] = (float)W; v20[2] = p; v20[3] = (float)H;
        v21[0] = s0; v21[1] = s1; v21[2] = s2; v21[3] = s3;
        for (int k = 0; k < 4; ++k) v22[k] = col[k];
    }

    void filter(const Pic& pic, float x, float y, float* o) const
    {
        if (hwFilter) bilinearHw(pic, x, y, o);
        else bilinear(pic, x, y, o);
    }

    void pixel(const Pic& from, const Pic& to, int x, int y, float* out) const
    {
        RENO_WIPES_NO_FMA
        float fx = (float)x, fy = (float)y;
        float& pos = (kernel < 2) ? fy : fx;
        const float first = (0.5f > p) ? s3 : (-s3 + (L + s1));          // the shift of the front picture, and (negated) of the one behind it
        const float second = (0.5f > p) ? -s3 : (s3 - (L + s1));
        const float v42 = 0.0f < s2 ? 1.0f / s2 : 0.0f;                     // (the kernel divides only where 0 < s2)
        float v33[4];
        float v34 = 0.0f;
        if (kernel == 0 || kernel == 2) {
            const float v29 = s0, v30 = s0 + s1, v31 = s0 - s2, v32 = v30 + s2;
            if (pos < v29) {
                v34 = (pos > v31 && 0.0f < s2) ? (pos - v31) * v42 : 0.0f;
                pos += first;
                filter(from, fx, fy, v33);
            } else if (pos > v30) {
                v34 = (pos < v32 && 0.0f < s2) ? (v32 - pos) * v42 : 0.0f;
                pos += second;
                filter(to, fx, fy, v33);
            } else {
                v34 = 1.0f;
                for (int k = 0; k < 4; ++k) v33[k] = col[k];
            }
        } else {
            const float v30 = s0, v29 = s0 - s1, v31 = v29 - s2, v32 = v30 + s2;
            if (pos < v29) {
                v34 = (pos > v31 && 0.0f < s2) ? (pos - v31) * v42 : 0.0f;
                pos += first;
                filter(to, fx, fy, v33);
            } else if (pos > v30) {
                v34 = (pos < v32 && 0.0f < s2) ? (v32 - pos) * v42 : 0.0f;
                pos += second;
                filter(from, fx, fy, v33);
            } else {
                v34 = 1.0f;
                for (int k = 0; k < 4; ++k) v33[k] = col[k];
            }
        }
        v34 = smooth3(v34);
        mix4(v33, col, v34, out);
    }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
