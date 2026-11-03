// Squeeze as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on): the incoming picture is squeezed into a band that grows from the chosen side (or from
// the middle for Lateral and Vertical) over the outgoing picture, which is squeezed into what is left when Squeeze previous image is on, with a border strip and a feather
// between them. It is the Push family again: the host (0x180611cb0) works out the same s0 / s3 numbers as Push's from the length along the direction (the middle ones count
// half of it), and hands them to one of six kernels (prog63), one per Start. The kernel handles are created in the order of the names k...f81, f80, f82, f83, f84, f85
// (and f86 for the smoothing pass), so Start 0 Top, 1 Bottom, 2 Left, 3 Right, 4 Lateral, 5 Vertical use the kernels 0, 1, 3, 2, 5, 4.
// With best render quality, a picture that is squeezed to half its size or less is first smoothed along the squeeze (kernel 6, a 1 2 1 / 5 tap filter on a float image).
//
// Written from the decoded code. Pictures are RGBA floats; x, y given to pixel() are the kernel's (y = 0 is the bottom row), see Pic in vfxwipes.h.
#pragma once
#include "vfxwipes.h"
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct Squeeze {
    int W = 0, H = 0, start = 0, kernel = 0;
    bool vertical = true;       // the squeeze runs along the picture's height (Top, Bottom, Lateral)
    bool best = true;           // best render quality: the squeezed pictures are smoothed first
    bool hwFilter = true;       // the card's own bilinear filter (fHWBL) rather than the kernel's four reads
    bool smoothSecond = false, smoothFirst = false;     // the incoming / the outgoing picture is smoothed first
    int mode2[2] = {1, 1}, mode1[2] = {1, 1};           // the smoothing kernel's two mode numbers for the incoming / the outgoing picture
    float S = 0, flag = 0, p = 0;                       // v21 = (S, 2 when Squeeze previous image, p, 0)
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;               // v22
    float col[4] = {0, 0, 0, 1};

    static int clampN(float r)      // the number of smoothing steps asked for, cut to 1 .. 8
    {
        int n = (r >= -2147483648.0f && r < 2147483648.0f) ? (int)r : (int)0x80000000;
        if (n < 1) return 1;
        return n > 8 ? 8 : n;
    }

    Squeeze(int w, int h, int startSide, double progress, bool squeezePrevious, double borderSize, double borderFeather, const float* colour, bool bestQuality = true)
    {
        RENO_WIPES_NO_FMA
        W = w; H = h;
        best = bestQuality;
        start = startSide < 0 ? 0 : (startSide > 5 ? 5 : startSide);
        vertical = start == 0 || start == 1 || start == 4;
        const float pf = (float)progress;
        p = (0.0f > pf) ? 0.0f : std::fmin(1.0f, pf);
        const float L = (float)(vertical ? H : W);
        S = L * (start >= 4 ? 0.5f : 1.0f);
        const float border = (float)borderSize * S;
        float feather = (float)borderFeather * S;
        if (1.0f > feather) feather = 0.0f;
        const float x10 = border + S;
        const float x0 = ((x10 + feather) + feather) * p;
        const float x1 = x0 - feather;
        s3 = (0.0f > x1) ? 0.0f : std::fmin(x10, x1);
        static const int kmap[6] = {0, 1, 3, 2, 5, 4};
        kernel = kmap[start];
        const float a = (start == 1 || start == 2) ? x1 : (feather + S) - x0;
        s0 = a - 0.5f;
        s1 = border; s2 = feather;
        flag = squeezePrevious ? 2.0f : 0.0f;
        for (int k = 0; k < 4; ++k) col[k] = colour[k];
        if (best && s3 > 0.0f && S > s3) {
            const float r0 = S / s3, r1 = S / (S - s3);
            if (r0 > 2.0f) {
                smoothSecond = true;
                const int n = clampN(r0);
                mode2[0] = vertical ? 1 : n; mode2[1] = vertical ? n : 1;
            }
            if (squeezePrevious && r1 > 2.0f) {
                smoothFirst = true;
                const int n = clampN(r1);
                mode1[0] = vertical ? 1 : n; mode1[1] = vertical ? n : 1;
            }
        }
    }

    // the host's arguments: v20 = (width, height), v21 = (S, flag, p, 0), v22 = (s0, border, feather, s3), v23 = colour
    void args(float* v21, float* v22, float* v23) const
    {
        v21[0] = S; v21[1] = flag; v21[2] = p; v21[3] = 0.0f;
        v22[0] = s0; v22[1] = s1; v22[2] = s2; v22[3] = s3;
        for (int k = 0; k < 4; ++k) v23[k] = col[k];
    }

    // kernel 6, the smoothing pass: m0 == 1 vertical 1 2 1, else m1 == 1 five taps along the row, else 3 x 3. Returns the picture with the same row order as 'in' (bottom row first
    // when in.topFirst is false).
    static void smooth(const Pic& in, int m0, int m1, std::vector<float>& outv)
    {
        RENO_WIPES_NO_FMA
        const int w = in.w, h = in.h;
        outv.assign((size_t)w * h * 4, 0.0f);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int v8 = std::max(x - 1, 0), v7 = std::max(y - 1, 0), v10 = std::min(x + 1, w - 1), v13 = std::min(y + 1, h - 1);
                float t[9][4];
                float r[4];
                if (m0 == 1) {
                    float a[4], b[4], c[4];
                    in.texel(x, v7, a); in.texel(x, v13, b); in.texel(x, y, c);
                    for (int k = 0; k < 4; ++k) r[k] = 4.0f * (a[k] + b[k]) + 8.0f * c[k];
                } else if (m1 == 1) {
                    const int xx = std::max(x - 2, v8), v49 = std::min(x + 2, w - 1);
                    float a[4], b[4], c[4], d[4], e[4];
                    in.texel(xx, y, a); in.texel(v49, y, b); in.texel(v8, y, c); in.texel(v10, y, d); in.texel(x, y, e);
                    for (int k = 0; k < 4; ++k) r[k] = 2.0f * (a[k] + b[k]) + 3.5f * (c[k] + d[k]) + 5.0f * e[k];
                } else {
                    in.texel(v8, v7, t[0]); in.texel(v10, v7, t[1]); in.texel(v8, v13, t[2]); in.texel(v10, v13, t[3]);
                    in.texel(v8, y, t[4]); in.texel(x, v7, t[5]); in.texel(v10, y, t[6]); in.texel(x, v13, t[7]); in.texel(x, y, t[8]);
                    for (int k = 0; k < 4; ++k) r[k] = t[0][k] + t[1][k] + t[2][k] + t[3][k] + 2.0f * (t[4][k] + t[5][k] + t[6][k] + t[7][k]) + 4.0f * t[8][k];
                }
                float* o = &outv[((size_t)(in.topFirst ? h - 1 - y : y) * w + x) * 4];
                for (int k = 0; k < 4; ++k) o[k] = r[k] * 0.0625f;
            }
        }
    }

    void filter(const Pic& pic, float x, float y, float* o) const
    {
        if (hwFilter) bilinearHw(pic, x, y, o);
        else bilinear(pic, x, y, o);
    }

    // first = the outgoing picture (kernel input 0), second = the incoming one (input 1); each smoothed first when smoothFirst / smoothSecond says so
    void pixel(const Pic& first, const Pic& second, int xi, int yi, float* out) const
    {
        RENO_WIPES_NO_FMA
        float x = (float)xi, y = (float)yi;
        const float v34 = (float)W, v35 = (float)H;
        const float fl = flag;
        const float rc = 0.0f < s2 ? 1.0f / s2 : 0.0f;                 // (native_recip, used only where 0 < s2)
        const bool alongY = (kernel == 0 || kernel == 1 || kernel == 5);
        const bool mid = kernel >= 4;
        float& pos = alongY ? y : x;
        const float len = alongY ? v35 : v34;
        const float other = alongY ? x : y;
        (void)other;
        float v36[4] = {col[0], col[1], col[2], col[3]};
        float v37 = 1.0f;
        auto fetch = [&](const Pic& pic) {
            x = clampf(x, 0.0f, v34 - 1.0f);
            y = clampf(y, 0.0f, v35 - 1.0f);
            filter(pic, x, y, v36);
        };
        // the kernels differ in which test is "<=" and which "<", which picture is first, and where the "squeeze previous" factor applies
        if (!mid) {
            // k0 / k2: the incoming picture (second) on the far side; k1 / k3: the other way round
            const bool flipped = (kernel == 1 || kernel == 3);       // k1, k3: s0 is where the incoming band ends
            float v30, v31, v32, v33;
            if (!flipped) { v30 = s0; v31 = s0 + s1; } else { v31 = s0; v30 = s0 - s1; }
            v32 = (!flipped ? s0 : v30) - s2;
            v33 = v31 + s2;
            // k3 uses < and >= where the others use <= and >
            const bool strict = kernel == 3;
            const bool low = strict ? (pos < v30) : (pos <= v30);
            const bool high = strict ? (pos >= v31) : (pos > v31);
            if (low || high) {
                if (low) {
                    v37 = (pos > v32 && 0.0f < s2) ? (pos - v32) * rc : 0.0f;
                    v30 = clampf(v30, 0.0001f, len);
                    if (kernel == 0 || kernel == 2) pos = (kernel == 0) ? ((1.0f < fl) ? pos * len / v30 : pos) : pos * ((1.0f < fl) ? len / v30 : 1.0f);
                    else pos *= len / v30;
                    fetch((kernel == 0 || kernel == 2) ? first : second);
                } else {
                    v37 = (pos < v33 && 0.0f < s2) ? (v33 - pos) * rc : 0.0f;
                    v31 = clampf(v31, 0.0f, len - 0.0001f);
                    if (kernel == 0 || kernel == 2) pos = (pos - v31) * len / (len - v31);
                    else pos = (1.0f < fl) ? (pos - v31) * len / (len - v31) : pos;
                    fetch((kernel == 0 || kernel == 2) ? second : first);
                }
            }
        } else {
            const float v30 = s0, v31 = s0 + s1, v32 = s0 - s2, v33 = v31 + s2;
            const float v39 = 0.5f * len + 0.0001f;
            const float v40 = len - v31, v41 = v40 + s1, v42 = v40 - s2, v43 = v41 + s2;
            // k5 (along y) tests "<" where k4 (along x) tests "<=" in the inner branches
            const bool strict = alongY;
            if (pos < v39) {
                if ((pos <= v30) || (pos > v31)) {
                    if (strict ? (pos < v30) : (pos <= v30)) {
                        v37 = (pos > v32 && 0.0f < s2) ? (pos - v32) * rc : 0.0f;
                        const float c = clampf(v30, 0.0001f, v39);
                        pos *= (1.0f < fl) ? v39 / c : 1.0f;
                        fetch(first);
                    } else {
                        v37 = (pos < v33 && 0.0f < s2) ? (v33 - pos) * rc : 0.0f;
                        const float c = clampf(v31, 0.0f, v39 - 0.0001f);
                        pos = (pos - c) * v39 / (v39 - c);
                        fetch(second);
                    }
                }
            } else {
                if ((pos <= v40) || (pos > v41)) {
                    if (strict ? (pos < v40) : (pos <= v40)) {
                        v37 = (pos > v42 && 0.0f < s2) ? (pos - v42) * rc : 0.0f;
                        const float c = clampf(v40, v39 + 0.0001f, len);
                        pos = v39 + (pos - v39) * v39 / (c - v39);
                        fetch(second);
                    } else {
                        v37 = (pos < v43 && 0.0f < s2) ? (v43 - pos) * rc : 0.0f;
                        const float c = clampf(v41, v39, len - 0.0001f);
                        pos = (1.0f < fl) ? v39 + (pos - c) * v39 / (len - c) : pos;
                        fetch(first);
                    }
                }
            }
        }
        v37 = smooth3(v37);
        mix4(v36, col, v37, out);
    }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
