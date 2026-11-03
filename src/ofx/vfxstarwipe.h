// Star Wipe as VEGAS draws it with its OpenCL kernel (Vfx1.ofx, GPU acceleration on): a star (or gear, polygon, ring, ...) with Arms points grows out of Center and shows the
// incoming picture inside it, with a border strip and a feather along its outline. The kernel (prog64) measures every pixel by how far out along its arm it is: the angle
// round the centre is folded into one arm, the point rotated onto the arm's axis, and the distance to the arm's edge worked out from Ratio (the inner radius over the outer);
// that number, scaled so the far corner is 1 and repeated Cycles times (with a seesaw fold for the Seesaw waveform), is compared with the progress.
// The host (0x18061cb20) hands the kernel the numbers as they are and one it works out itself: the biggest the distance gets along the four edges of the frame (probed on the
// CPU with the same formula, but with the Horizontal / Vertical flip switches read the other way round: the host turns the centre and the angle by the vertical switch
// where the kernel uses the horizontal one, and the other way round; the two agree when both or neither is on).
//
// Written from the decoded code. Pictures are RGBA floats; x, y given to pixel() are the kernel's (y = 0 is the bottom row), see Pic in vfxwipes.h.
// The pixel aspect (the host reads it from the destination image) is taken as 1.
#pragma once
#include "vfxwipes.h"
#include <cmath>
#include <cstdint>

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct StarWipe {
    int W = 0, H = 0;
    int cycles = 1, arms = 5;           // v22.s0, s1
    int reverse = 0;                    // s2: Direction Reverse
    int flipX = 0, flipY = 0;           // s3 (Horizontal flip), s4 (Vertical flip)
    int mirrorX = 0, mirrorY = 0;       // s5 (Horizontal mirror), s6 (Vertical mirror)
    int sawtooth = 1;                   // s7: Waveform Seesaw (the kernel's "1")
    float maxDist = 1;                  // v23: the largest distance along the frame's edges
    float p = 0, aspect = 1, ratio = 0.381f, angle = 18, centerX = 0.5f, centerYInv = 0.5f, border = 0, feather = 0;      // v24.s0 .. s7 (centerYInv = 1 - Center y)
    float col[4] = {1, 0, 1, 1};
    float asp[2] = {1, 1};              // v6

    // (the single precision functions of the Windows C runtime work in double and round once)
    static float sinf_(float v) { return (float)std::sin((double)v); }
    static float cosf_(float v) { return (float)std::cos((double)v); }
    static float atanf_(float v) { return (float)std::atan((double)v); }

    // the distance function the host probes with (0x180616570); the kernel's own (f0) differs only in dividing by a reciprocal: 'recip' says which
    static float dist(long xi, long yi, const float* cen, float v2, float v3, float v4, float v5, const float* a, bool recip)
    {
        RENO_WIPES_NO_FMA
        if (std::fabs((float)xi - cen[0]) < 0.000001f && std::fabs((float)yi - cen[1]) < 0.000001f) return 1.0f;
        float v7 = (float)xi - cen[0];
        float v8 = (float)yi - cen[1];
        v7 *= a[0];
        v8 *= a[1];
        const float v9 = v7 * v3 + v8 * v4;
        const float v10 = (-v4 * v7) + v8 * v3;
        float v11 = 0.0f;
        if (std::fabs(v9) < 0.000001f) {
            if (v10 > 0) v11 = 1.5707963f;
            if (v10 < 0) v11 = 4.71238898f;
        } else if (std::fabs(v10) < 0.000001f) {
            if (v9 > 0) v11 = 0.0f;
            if (v9 < 0) v11 = 3.1415926f;
        } else {
            v11 = atanf_(v10 / v9);
            if (v9 < 0 && v10 > 0) v11 = 3.1415926f + v11;
            else if (v9 < 0 && v10 < 0) v11 = 3.1415926f + v11;
            else if (v9 > 0 && v10 < 0) v11 = 6.2831853f + v11;
        }
        float v12 = 0.0f;
        const float v13 = v2 * 0.5f;
        if (v11 > v13 && v11 < (6.2831853f - v13)) {
            const float q = recip ? (v11 + v13) * (1.0f / v2) : (v13 + v11) / v2;
            v12 = v2 * (float)cutLong(q);
        }
        const float v14 = cosf_(v12), v15 = sinf_(v12);
        v7 = v9 * v14 + v10 * v15;
        v8 = (-v15 * v9) + v10 * v14;
        v7 = (v7 < 0) ? -v7 : v7;
        v8 = (v8 < 0) ? -v8 : v8;
        float v16 = v7 * (-v5) + v8;
        v16 = std::fmax(1.0f, v16);
        return v16;
    }

    // C's float to integer conversion; anything out of range gives the lowest value
    static long cutLong(float v)
    {
        if (!(v > -2147483648.0f && v < 2147483648.0f)) return -2147483647L - 1;
        return (long)v;
    }

    StarWipe(int w, int h, int armsN, double ratioV, double angleDeg, double featherV, double borderV, const float* colour, double centerXv, double centerYv, int cyclesN,
             int waveform, bool hFlip, bool vFlip, bool hMirror, bool vMirror, int direction, double progress)
    {
        RENO_WIPES_NO_FMA
        W = w < 1 ? 1 : w; H = h < 1 ? 1 : h;
        arms = armsN < 1 ? 1 : (armsN > 256 ? 256 : armsN);
        cycles = cyclesN < 0 ? 0 : (cyclesN > 1000 ? 1000 : cyclesN);
        auto fin = [](double v, double dflt) { return v == v ? v : dflt; };
        p = (float)fin(progress, 0.0);
        ratio = (float)fin(ratioV, 0.381);
        angle = (float)fin(angleDeg, 0.0);
        feather = (float)fin(featherV, 0.0);
        border = (float)fin(borderV, 0.0);
        const float cx = (float)fin(centerXv, 0.5), cy = (float)fin(centerYv, 0.5);
        centerX = cx;
        centerYInv = 1.0f - cy;
        reverse = direction != 0;
        flipX = hFlip; flipY = vFlip; mirrorX = hMirror; mirrorY = vMirror;
        sawtooth = waveform == 0;
        for (int k = 0; k < 3; ++k) col[k] = (float)colour[k];
        col[3] = 1.0f;

        // the host's own probe of the frame's edges
        const float v2h = 6.2831854820251465f / (float)arms;
        const float h5 = v2h * 0.5f;
        const float v5 = (sinf_(h5) * ratio) / (cosf_(h5) * ratio - 1.0f);
        float ys = centerYInv, xs = centerX, an = angle;       // the host's copies: y here is 1 - Center y
        if (flipX) { ys = 1.0f - ys; an = 360.0f - an; }       // (the host reads the horizontal switch where the kernel reads the vertical one: see above)
        if (flipY) { xs = 1.0f - xs; an = 180.0f - an; if (0.0f > an) an += 360.0f; }
        const float cen[2] = {(float)(W - 1) * xs, (float)(H - 1) * (1.0f - ys)};
        const float ar = an * 0.01745329238474369f;
        const float c3 = cosf_(ar), s4 = sinf_(ar);
        float mx = 0.0f;
        for (int y = 0; y < H; ++y) {
            float d = dist(0, y, cen, v2h, c3, s4, v5, asp, false);
            if (d > mx) mx = d;
            d = dist(W - 1, y, cen, v2h, c3, s4, v5, asp, false);
            if (d > mx) mx = d;
        }
        for (int x = 0; x < W; ++x) {
            float d = dist(x, 0, cen, v2h, c3, s4, v5, asp, false);
            if (d > mx) mx = d;
            d = dist(x, H - 1, cen, v2h, c3, s4, v5, asp, false);
            if (d > mx) mx = d;
        }
        maxDist = mx;
    }

    // the kernel's arguments: v22 (int8), v23, v24 (float8), v25 (colour), v6 (aspect pair)
    void args(int* v22, float* v23, float* v24, float* v25, float* v6) const
    {
        v22[0] = cycles; v22[1] = arms; v22[2] = reverse; v22[3] = flipX; v22[4] = flipY; v22[5] = mirrorX; v22[6] = mirrorY; v22[7] = sawtooth;
        *v23 = maxDist;
        v24[0] = p; v24[1] = aspect; v24[2] = ratio; v24[3] = angle; v24[4] = centerX; v24[5] = centerYInv; v24[6] = border; v24[7] = feather;
        for (int k = 0; k < 4; ++k) v25[k] = col[k];
        v6[0] = asp[0]; v6[1] = asp[1];
    }

    // first = the outgoing picture (v17), second = the incoming one (v18)
    void pixel(const Pic& first, const Pic& second, int xi, int yi, float* out) const
    {
        RENO_WIPES_NO_FMA
        float v48 = p;
        float v47 = centerYInv, v46 = centerX, v45 = angle;
        if (flipY) { v47 = 1.0f - v47; v45 = 360.0f - v45; }
        if (flipX) {
            v46 = 1.0f - v46;
            v45 = 180.0f - v45;
            v45 = (v45 < 0.0f) ? (v45 + 360.0f) : v45;
        }
        const float cen[2] = {(float)(W - 1) * v46, (float)(H - 1) * (1.0f - v47)};
        v45 = v45 * 0.01745329252f;
        const float v3 = cosf_(v45), v4 = sinf_(v45);
        const float v49 = sawtooth ? 1.0f : 2.0f;
        const float v2 = 6.283185307f * (1.0f / (float)arms);
        const float v13 = v2 * 0.5f;
        const float v5 = -ratio * sinf_(v13) / (1.0f - cosf_(v13) * ratio);
        long v50 = xi, v51 = yi;
        v50 = (mirrorX && (v50 > W / 2)) ? (W - v50) : v50;
        v51 = (mirrorY && (v51 < H / 2)) ? (H - v51) : v51;
        const float v52 = (float)cycles * (1.0f / maxDist);
        float v16 = dist(v50, v51, cen, v2, v3, v4, v5, asp, true);
        v16 *= v52;
        v16 = (v16 > 1.0f) ? (v16 - std::floor(v16)) : v16;
        if (!sawtooth) v16 = (v16 < 0.5f) ? v16 * 2.0f : (1.0f - v16) * 2.0f;
        v16 = reverse ? (1.0f - v16) : v16;
        v16 = clampf(v16, 0.0f, 1.0f);
        v16 = (maxDist < 1.0f) ? 1.0f : v16;
        v48 = clampf(v48, 0.0f, 1.0f);
        const bool v53 = border < 0.000001f, v54 = feather < 0.000001f;
        const float v55 = 0.125f * border, v56 = 0.125f * feather;
        float a[4], b[4];
        first.texel(xi, yi, a);
        second.texel(xi, yi, b);
        float v43[4];
        auto set = [&](const float* s) { for (int k = 0; k < 4; ++k) v43[k] = s[k]; };
        if (v53 && v54) {
            set(v16 < v48 ? b : a);
        } else if (!v53 && v54) {
            const float v57 = (-v49) * v55 + v48, v58 = v49 * v55 + v48;
            set(col);
            if (v16 < v57) set(b);
            if (v16 > v58) set(a);
        } else if (v53 && !v54) {
            const float v57 = (-v49) * v56 + v48, v58 = v49 * v56 + v48;
            const float v59 = 1.0f / (v58 - v57);
            float v60 = (v16 - v57) * v59;
            v60 = clampf(v60, 0.0f, 1.0f);
            mix4(b, a, v60, v43);
            if (v16 < v57) set(b);
            if (v16 > v58) set(a);
        } else {
            const float v61 = (-v49) * v55 + v48, v62 = v49 * v55 + v48;
            const float v63 = (-v49) * v56 + v61, v64 = v49 * v56 + v62;
            const float v65 = 1.0f / (v61 - v63), v66 = 1.0f / (v64 - v62);
            if (v16 < v63) {
                set(b);
            } else if (v16 > v64) {
                set(a);
            } else if (v63 <= v16 && v16 <= v61) {
                float v60 = (v16 - v63) * v65;
                v60 = clampf(v60, 0.0f, 1.0f);
                mix4(b, col, v60, v43);
            } else if (v61 < v16 && v16 < v62) {
                set(col);
            } else {
                float v60 = (v16 - v62) * v66;
                v60 = clampf(v60, 0.0f, 1.0f);
                mix4(col, a, v60, v43);
            }
        }
        for (int k = 0; k < 4; ++k) out[k] = v43[k];
    }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
