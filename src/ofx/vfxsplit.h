// Split as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on): one frame is cut into four quadrants at Center that move apart (Push), are cropped (Wipe)
// or squeezed (Squeeze) over the other frame, with a border strip and a feather around the cross that opens between them. It is the 2D transition library again: positions in
// 18.14 fixed point (1 / 16384 pixel), worked out by the geometry function 0x18060a8d0 and handed to the kernels (host 0x18060bed0) as floats, twelve kernels
// (three modes times no border / feather only / border only / both) plus a plain copy for the start of the transition.
//
// Written from the decoded code. Pictures are RGBA floats. The kernels number their rows from the BOTTOM row but flip y to count from the top before they look at the
// rectangles; here y is counted from the top throughout (Reno's own order) and a Pic is read with its row flipped back.
#pragma once
#include "vfxwipes.h"
#include <climits>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct Split {
    int W = 0, H = 0;
    int mode = 0;               // 0 Push, 1 Wipe, 2 Squeeze
    bool swapPics = false;      // Direction "In": the incoming picture is the one that is cut into quadrants
    bool onlyFirst = false;     // the start of the transition (the geometry says nothing has opened yet): the picture is copied
    bool smoothOther = false;   // Squeeze: the host smooths the picture shown behind
    bool hwFilter = true;       // the card's own bilinear filter (fHWBL) rather than the kernel's four reads
    int variant = 0;            // 0 neither, 1 feather only, 2 border only, 3 both
    float v29[2], v30[4], v31[4], v32[4], v33[4], v34[4], v35[4], v36[4], v37[4], v38[4];

    static int sat(double v)    // C's float to int, kept clear of the ends so that sums of two still fit
    {
        if (!(v == v)) return 0;
        if (v > 1073741823.0) return 1073741823;
        if (v < -1073741823.0) return -1073741823;
        return (int)v;
    }

    Split(int w, int h, int splitMode, int direction, double progress, double centerX, double centerY, double borderSize, double borderFeather, const float* colour)
    {
        W = w; H = h;
        mode = (splitMode == 1 || splitMode == 2) ? splitMode : 0;
        const float pf = (float)progress;
        swapPics = direction == 0;
        auto unit = [](double v) { return !(v == v) ? 0.0 : std::fmin(std::fmax(v, 0.0), 1.0); };
        const float cx = (float)std::fmin(std::fmax(!(centerX == centerX) ? 0.5 : centerX, -8.0), 8.0);
        const float cy = 1.0f - (float)std::fmin(std::fmax(!(centerY == centerY) ? 0.5 : centerY, -8.0), 8.0);
        int R[8][4];                // d: TL TR BL BR, then s: TL TR BL BR
        int border = 0, feather = 0;
        geometry(w, h, mode, swapPics, (double)pf, cx, cy, (float)unit(borderSize), (float)unit(borderFeather), R, border, feather);
        variant = (border != 0 ? 2 : 0) + (feather != 0 ? 1 : 0);
        const float K = 6.103515625e-05f;
        float* dst[8] = {v31, v32, v33, v34, v35, v36, v37, v38};
        for (int q = 0; q < 8; ++q)
            for (int k = 0; k < 4; ++k) dst[q][k] = (float)R[q][k] * K - 0.5f;
        v29[0] = ((float)border * K) * 0.5f;
        v29[1] = std::fmax(1.0f, (float)feather * K);
        for (int k = 0; k < 4; ++k) v30[k] = colour[k];
        // Squeeze smooths the picture behind, while the transition is under way (assuming the best quality setting: GUESS)
        const float tf = swapPics ? 1.0f - pf : pf;
        smoothOther = mode == 2 && tf > 9.999999747378752e-05f && 0.9998999834060669f > tf;
        onlyFirst = flagOnlyFirst;
    }

    bool flagOnlyFirst = false;

    // 0x18060a8d0. R[0..3]: where the quadrants are drawn (top left, top right, bottom left, bottom right), R[4..7]: the parts of the picture they show; x0 y0 x1 y1 each.
    void geometry(int w, int h, int md, bool flip, double p, float cx, float cy, float borderSize, float borderFeather, int (*R)[4], int& border, int& feather)
    {
        const int Wf = w << 14, Hf = h << 14;
        const double t0 = (0.0 > p) ? 0.0 : std::fmin(1.0, p);
        const double t = flip ? 1.0 - t0 : t0;
        const int mn = std::min(Wf, Hf), mx = std::max(Wf, Hf);
        const float fmn = (float)mn;
        border = sat((double)(fmn * borderSize * 0.5f) + 0.5);
        feather = sat((double)(fmn * borderFeather * 0.5f) + 0.5);
        const double x3 = (double)feather / (double)mx;
        const double x2 = (x3 + 1.0) * t - x3;
        int ox, oy, ob;
        if (x2 >= 0.0) {
            ox = sat((double)Wf * x2); oy = sat((double)Hf * x2); ob = sat((double)(feather + border) * x2);
        } else {
            ox = oy = ob = 0;
            feather = sat((1.0 - (-(x2 / x3))) * (double)feather);
        }
        ox = std::max(ox, 0); oy = std::max(oy, 0); ob = std::max(ob, 0);
        if (border < 0) {
            border = 0;
        } else {
            int c1 = (ox / 2 + ob) * 2 - 1; if (c1 <= 0) c1 = 0;
            int c2 = (oy / 2 + ob) * 2 - 1; if (c2 <= 0) c2 = 0;
            const int lim = c1 < c2 ? c1 : c2;
            if (border > lim) border = lim;
        }
        flagOnlyFirst = (-x3 >= x2);
        for (int q = 0; q < 8; ++q) for (int k = 0; k < 4; ++k) R[q][k] = 0;
        if (flagOnlyFirst) return;
        const int cxw = sat((double)((float)Wf * cx)), chh = sat((double)((float)Hf * cy));
        const float x7 = (float)ob;
        auto add = [](int* r, int a, int b, int c, int d) { r[0] += a; r[1] += b; r[2] += c; r[3] += d; };
        // top left
        {
            const int r13 = sat((double)((float)(-ox) * cx - x7)), eb = sat((double)((float)(-oy) * cy - x7));
            int* d = R[0]; int* s = R[4];
            s[0] = 0; s[1] = 0; s[2] = cxw; s[3] = chh;
            for (int k = 0; k < 4; ++k) d[k] = s[k];
            if (md == 0) add(d, r13, eb, r13, eb);
            else if (md == 1) { add(d, 0, 0, r13, eb); add(s, 0, 0, r13, eb); }
            else add(d, 0, 0, r13, eb);
        }
        const int es = sat((double)((1.0f - cx) * (float)ox + x7));
        const int eb = sat((double)((float)(-oy) * cy - x7));
        // top right
        {
            int* d = R[1]; int* s = R[5];
            s[0] = cxw; s[1] = 0; s[2] = Wf; s[3] = chh;
            for (int k = 0; k < 4; ++k) d[k] = s[k];
            if (md == 0) add(d, es, eb, es, eb);
            else if (md == 1) { add(d, es, 0, 0, eb); add(s, es, 0, 0, eb); }
            else add(d, es, 0, 0, eb);
        }
        // bottom left
        const int r13 = sat((double)((float)(-ox) * cx - x7));
        const int eb2 = sat((double)((1.0f - cy) * (float)oy + x7));
        {
            int* d = R[2]; int* s = R[6];
            s[0] = 0; s[1] = chh; s[2] = cxw; s[3] = Hf;
            for (int k = 0; k < 4; ++k) d[k] = s[k];
            if (md == 0) add(d, r13, eb2, r13, eb2);
            else if (md == 1) { add(d, 0, eb2, r13, 0); add(s, 0, eb2, r13, 0); }
            else add(d, 0, eb2, r13, 0);
        }
        // bottom right
        {
            int* d = R[3]; int* s = R[7];
            s[0] = cxw; s[1] = chh; s[2] = Wf; s[3] = Hf;
            for (int k = 0; k < 4; ++k) d[k] = s[k];
            if (md == 0) add(d, es, eb2, es, eb2);
            else if (md == 1) { add(d, es, eb2, 0, 0); add(s, es, eb2, 0, 0); }
            else add(d, es, eb2, 0, 0);
        }
    }

    // f3 and f4 of prog62: distance to the edge of the cross's box (f3 also inside it: to the nearest corner)
    static float dist3(float v8, float v10, float v7, float v13, float x, float y)
    {
        const float v17 = std::fmin(std::fabs(x - v8), std::fabs(x - v10));
        const float v18 = std::fmin(std::fabs(y - v7), std::fabs(y - v13));
        float v19 = std::fmin(v17, v18);
        if (v8 <= x && x <= v10 && v7 <= y && y <= v13)
            v19 = std::fmin(std::fmin(std::hypot(v8 - x, v7 - y), std::hypot(v10 - x, v13 - y)), std::fmin(std::hypot(v8 - x, v13 - y), std::hypot(v10 - x, v7 - y)));
        else if (v8 < x && x < v10) v19 = v17;
        else if (v7 < y && y < v13) v19 = v18;
        return v19;
    }
    static float dist4(float v8, float v10, float v7, float v13, float x, float y)
    {
        const float v17 = std::fmin(std::fabs(x - v8), std::fabs(x - v10));
        const float v18 = std::fmin(std::fabs(y - v7), std::fabs(y - v13));
        float v19 = std::fmin(v17, v18);
        if (v8 < x && x < v10) v19 = v17;
        else if (v7 < y && y < v13) v19 = v18;
        return v19;
    }
    static bool cross(float a, float b, float c, float d, float x, float y) { return (a < x && b > x) || (c < y && d > y); }

    void filter(const Pic& pic, float x, float y, float* o) const
    {
        if (hwFilter) bilinearHw(pic, x, y, o);
        else bilinear(pic, x, y, o);
    }

    // the quadrant picture at (x, y) (y from the top): Wipe reads the pixel itself, Push and Squeeze read the quadrant's source carried over
    void quad(const Pic& first, int xi, int yi, float v8, float v10, float v7, float v13, float* o) const
    {
        RENO_WIPES_NO_FMA
        const float x = (float)xi, y = (float)yi;
        if (mode == 1) { first.texel(xi, H - 1 - yi, o); return; }
        const float* d; const float* s;
        if (x <= v8 && y <= v7) { d = v31; s = v35; }
        else if (x >= v10 && y <= v7) { d = v32; s = v36; }
        else if (x <= v8 && y >= v13) { d = v33; s = v37; }
        else { d = v34; s = v38; }
        const float v57 = (d[0] + d[2]) * 0.5f, v58 = (d[1] + d[3]) * 0.5f, v59 = (s[0] + s[2]) * 0.5f, v60 = (s[1] + s[3]) * 0.5f;
        float xx, yy;
        if (mode == 0) {
            xx = (x - v57) + v59;
            yy = (y - v58) + v60;
        } else {
            const float v61 = s[2] - s[0], v63 = s[3] - s[1];
            const float v62 = std::fmax(d[2] - d[0], 0.0001f), v64 = std::fmax(d[3] - d[1], 0.0001f);
            const float v65 = v61 / v62, ky = v63 / v64;
            xx = v65 * (x - v57) + v59;
            yy = ky * (y - v58) + v60;
        }
        if (!(xx == xx)) xx = 0.0f;
        if (!(yy == yy)) yy = 0.0f;
        xx = clampf(xx, 0.0f, (float)W - 1.0f);
        yy = clampf(yy, 0.0f, (float)(H - 1));
        yy = (float)(H - 1) - yy;
        filter(first, xx, yy, o);
    }

    // first = the picture cut into quadrants, other = the one behind (smoothed with smooth3x3 when smoothOther); y counted from the top row
    void pixel(const Pic& first, const Pic& other, int xi, int yi, float* out) const
    {
        RENO_WIPES_NO_FMA
        if (onlyFirst) { first.texel(xi, H - 1 - yi, out); return; }
        const float x = (float)xi, y = (float)yi;
        float v[4];
        auto behind = [&](float* o) { other.texel(xi, H - 1 - yi, o); };
        if (variant == 0) {
            const float v8 = v31[2], v10 = v32[0], v7 = v31[3], v13 = v33[1];
            if (cross(v8, v10, v7, v13, x, y)) behind(v);
            else quad(first, xi, yi, v8, v10, v7, v13, v);
            for (int k = 0; k < 4; ++k) out[k] = v[k];
            return;
        }
        if (variant == 1) {
            const float v8 = v31[2], v10 = v32[0], v7 = v31[3], v13 = v33[1];
            const float v43 = (v8 + v10) * 0.5f, v44 = (v7 + v13) * 0.5f;
            const float v45 = std::fmax(1.0f, v29[1]);
            float v46 = v8 + v45, u1 = v10 - v45, v47 = v7 + v45, v1 = v13 - v45;
            float v48 = v8 - v45, U1 = v10 + v45, v49 = v7 - v45, V1 = v13 + v45;
            v46 = clampf(v46, v8, v43); u1 = clampf(u1, v43, v10); v47 = clampf(v47, v7, v44); v1 = clampf(v1, v44, v13);
            v48 = std::fmin(v48, v8); U1 = std::fmax(U1, v10); v49 = std::fmin(v49, v7); V1 = std::fmax(V1, v13);
            float v50 = v45;
            if (cross(v8, v10, v7, v13, x, y)) {
                behind(v);
                if (!cross(v46, u1, v47, v1, x, y)) v50 = dist3(v8, v10, v7, v13, x, y);
            } else {
                quad(first, xi, yi, v8, v10, v7, v13, v);
                if (cross(v48, U1, v49, V1, x, y)) v50 = dist4(v8, v10, v7, v13, x, y);
            }
            float v51 = std::fmin(v50 / v45, 1.0f);
            v51 = smooth3(v51);
            mix4(v30, v, v51, out);
            return;
        }
        const float v52 = std::fmax(1.0f, v29[0]);
        const float v8 = v31[2] + v52, v10 = v32[0] - v52, v7 = v31[3] + v52, v13 = v33[1] - v52;
        const float v43 = (v8 + v10) * 0.5f, v44 = (v7 + v13) * 0.5f;
        float v46 = v8 + v52, u1 = v10 - v52, v47 = v7 + v52, v1 = v13 - v52;
        float v48 = v8 - v52, U1 = v10 + v52, v49 = v7 - v52, V1 = v13 + v52;
        v46 = clampf(v46, v8, v43); u1 = clampf(u1, v43, v10); v47 = clampf(v47, v7, v44); v1 = clampf(v1, v44, v13);
        v48 = std::fmin(v48, v8); U1 = std::fmax(U1, v10); v49 = std::fmin(v49, v7); V1 = std::fmax(V1, v13);
        if (variant == 2) {
            if (cross(v8, v10, v7, v13, x, y)) {
                if (cross(v46, u1, v47, v1, x, y) || dist3(v48, U1, v49, V1, x, y) >= v52 + v52) behind(v);
                else for (int k = 0; k < 4; ++k) v[k] = v30[k];
            } else if (cross(v48, U1, v49, V1, x, y)) {
                for (int k = 0; k < 4; ++k) v[k] = v30[k];
            } else {
                quad(first, xi, yi, v8, v10, v7, v13, v);
            }
            for (int k = 0; k < 4; ++k) out[k] = v[k];
            return;
        }
        const float v45 = std::fmax(1.0f, v29[1]);
        float v53 = v46 + v45, r1 = u1 - v45, s0 = v47 + v45, s1 = v1 - v45;
        float v54 = v48 - v45, R1 = U1 + v45, v55 = v49 - v45, S1 = V1 + v45;
        v53 = clampf(v53, v46, v43); r1 = clampf(r1, v43, u1); s0 = clampf(s0, v47, v44); s1 = clampf(s1, v44, v1);
        v54 = std::fmin(v54, v48); R1 = std::fmax(R1, U1); v55 = std::fmin(v55, v49); S1 = std::fmax(S1, V1);
        if (cross(v53, r1, s0, s1, x, y)) {
            behind(v);
            for (int k = 0; k < 4; ++k) out[k] = v[k];
        } else if (!cross(v54, R1, v55, S1, x, y)) {
            quad(first, xi, yi, v8, v10, v7, v13, v);
            for (int k = 0; k < 4; ++k) out[k] = v[k];
        } else if (cross(v8, v10, v7, v13, x, y)) {
            const float v19 = dist3(v48, U1, v49, V1, x, y) - v52;
            if (v19 < v52) { for (int k = 0; k < 4; ++k) out[k] = v30[k]; }
            else {
                behind(v);
                float v51 = std::fmin(1.0f, (v19 - v52) / v45);
                v51 = smooth3(v51);
                mix4(v30, v, v51, out);
            }
        } else {
            const float v19 = dist4(v8, v10, v7, v13, x, y);
            if (v19 < v52) { for (int k = 0; k < 4; ++k) out[k] = v30[k]; }
            else {
                quad(first, xi, yi, v8, v10, v7, v13, v);
                float v51 = std::fmin(1.0f, (v19 - v52) / v45);
                v51 = smooth3(v51);
                mix4(v30, v, v51, out);
            }
        }
    }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
