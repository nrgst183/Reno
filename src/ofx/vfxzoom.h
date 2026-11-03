// Zoom as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on): the incoming picture grows out of a point (Zoom In) or the outgoing one shrinks into it
// (Zoom Out) inside a rectangle, with a border strip and a feather around the rectangle. It is the 2D transition library again (Slide, Spiral, Split): positions in 18.14
// fixed point (1 / 16384 pixel), eight kernels (prog68): no border / feather only / border only / both, each with the card's bilinear read (best quality) or a nearest
// read (lower quality), and a 3x3 smoothing kernel run over the growing picture first when it is drawn at half size or less.
// The host is 0x180657a00 and its geometry 0x1806566e0.
//
// Written from the decoded code. Pictures are RGBA floats; x, y given to pixel() are the kernel's (y = 0 is the bottom row), see Pic in vfxwipes.h.
#pragma once
#include "vfxspiral.h"
#include <climits>
#include <cstdint>

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct Zoom {
    int W = 0, H = 0;
    bool swapPics = false;      // Direction "Out": the outgoing picture is the one that is shown in the rectangle
    bool best = true;           // best render quality (the host's quality is 3 or more): the bilinear kernels and the smoothing pass
    bool hwFilter = true;       // the card's own bilinear filter (fHWBL) rather than the kernel's four reads
    int variant = 0;            // 0 neither, 1 feather only, 2 border only, 3 both
    int kernel = 0;             // the kernel the host enqueues (0..3 best quality, 5..8 the nearest-read ones)
    bool smoothMoved = false;   // the host smooths the growing picture first
    int border = 0, feather = 0;
    float v31[4], v32[4], v33[4], v34[4], v35[2], v36[2], v37[2], v38[2], v39[4], v40[4];

    // C's double to int conversion (cvttsd2si): anything out of range, and NaN, gives the lowest int
    static int cvt(double v)
    {
        if (!(v > -2147483649.0 && v < 2147483648.0)) return INT_MIN;
        return (int)v;
    }
    static int wrap(int64_t v) { return (int)(int32_t)(uint32_t)(uint64_t)v; }

    // 0x1806566e0. P: centre x, centre y (the picture's rows counted from the bottom), border size, feather; t: how far the transition is.
    // R: where the growing picture is, R1: the rectangle the border is drawn around (R, or R grown while the picture is held at full size).
    static void geometry(int w, int h, const float* P, double t, int* R, int* R1, int& border, int& feather)
    {
        const int W14 = wrap((int64_t)w << 14), H14 = wrap((int64_t)h << 14);
        R[0] = 0; R[1] = 0; R[2] = W14; R[3] = H14;
        double tc = 0.0;
        if (!(0.0 > t)) tc = (1.0 > t || !(t == t)) ? t : 1.0;             // (minsd: a NaN is kept)
        const int mx = wrap((int64_t)(H14 <= W14 ? w : h) << 14);
        const float mxf = (float)mx;
        border = cvt((double)(mxf * P[2]) + 0.5);
        feather = cvt((double)(mxf * P[3]) + 0.5);
        const int ex = wrap(0x8000 - (int64_t)feather);
        if ((border != 0 || feather != 0) && ex > 0) {
            feather = 0x8000;
            border = wrap((int64_t)border - ex);
            if (border < 0) border = 0;
        }
        const int eb0 = cvt((double)wrap((int64_t)mx + 2 * (int64_t)feather) * tc + 0.5);
        double s = 1.0;
        if (mx != 0 && eb0 < mx) s = (double)eb0 / (double)mx;
        const int X0 = wrap((int64_t)cvt((double)W14 * (double)P[0] + 0.5));
        const int Y0 = wrap((int64_t)cvt((double)H14 * (double)P[1] + 0.5));
        const int r0 = wrap((int64_t)cvt((double)wrap(0 - (int64_t)X0) * s + 0.5) + X0);
        const int r1 = wrap((int64_t)cvt((double)wrap(0 - (int64_t)Y0) * s + 0.5) + Y0);
        const int r2 = wrap((int64_t)cvt((double)wrap((int64_t)W14 - X0) * s + 0.5) + X0);
        const int r3 = wrap((int64_t)cvt((double)wrap((int64_t)H14 - Y0) * s + 0.5) + Y0);
        R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3;
        if (eb0 < mx) {
            for (int k = 0; k < 4; ++k) R1[k] = R[k];
        } else {
            const int e = wrap((int64_t)eb0 - mx) >> 1;
            R1[0] = wrap((int64_t)r0 - e); R1[1] = wrap((int64_t)r1 - e); R1[2] = wrap((int64_t)r2 + e); R1[3] = wrap((int64_t)r3 + e);
        }
        border = cvt((double)(mxf * P[2]) * s + 0.5);
        feather = cvt((double)(mxf * P[3]) * s + 0.5);
    }

    Zoom(int w, int h, int direction, double progress, double centerX, double centerY, double borderSize, double borderFeather, const float* colour, bool bestQuality = true)
    {
        RENO_WIPES_NO_FMA
        W = w; H = h;
        best = bestQuality;
        auto num = [](double v, double lo, double hi, double dflt) { return !(v == v) ? dflt : std::fmin(std::fmax(v, lo), hi); };
        const float pf = (float)num(progress, 0.0, 1.0, 0.0);
        double t = (double)pf;
        if (direction == 1) { t = 1.0 - t; swapPics = true; }
        const float P[4] = {(float)num(centerX, -8.0, 8.0, 0.5), (float)num(centerY, -8.0, 8.0, 0.5), (float)num(borderSize, 0.0, 1.0, 0.0), (float)num(borderFeather, 0.0, 1.0, 0.0)};
        int R[4], R1[4];
        geometry(w, h, P, t, R, R1, border, feather);
        variant = (border != 0 ? 2 : 0) + (feather != 0 ? 1 : 0);
        kernel = variant + (best ? 0 : 5);
        const float K = 6.103515625e-05f;
        const float borderF = (float)border * K, featherF = (float)feather * K;
        float A[4], M[4];
        for (int k = 0; k < 4; ++k) { A[k] = (float)R1[k] * K; M[k] = (float)R[k] * K; }
        if (R[0] == 1) M[0] = -9.999999747378752e-05f;
        if (R[1] == 1) M[1] = -9.999999747378752e-05f;
        const float bf = (borderF > 9.99999993922529e-09f) ? borderF : 9.99999993922529e-09f;
        const float ff = (1.0f > featherF) ? 1.0f : featherF;
        const float h5 = 0.5f;
        for (int k = 0; k < 4; ++k) v31[k] = A[k] - h5;
        v32[0] = A[0] - bf; v32[1] = A[1] - bf; v32[2] = A[2] + bf; v32[3] = A[3] + bf;
        v33[0] = v31[0] + ff; v33[1] = v31[1] + ff; v33[2] = v31[2] - ff; v33[3] = v31[3] - ff;
        v34[0] = v32[0] - ff; v34[1] = v32[1] - ff; v34[2] = v32[2] + ff; v34[3] = v32[3] + ff;
        v35[0] = bf; v35[1] = ff;
        v36[0] = (A[2] + A[0]) * h5 - h5; v36[1] = (A[3] + A[1]) * h5 - h5;
        v37[0] = (float)(w - 1) * h5; v37[1] = (float)(h - 1) * h5;
        const float d0 = M[2] - M[0], d1 = M[3] - M[1];
        v38[0] = (float)w / ((1.0f > d0) ? 1.0f : d0);
        v38[1] = (float)h / ((1.0f > d1) ? 1.0f : d1);
        for (int k = 0; k < 4; ++k) { v39[k] = colour[k]; v40[k] = M[k]; }
        smoothMoved = best && (Spiral::cut((double)v38[0]) > 1 || Spiral::cut((double)v38[1]) > 1);
    }

    // the kernels' arguments 4 .. 13 after the three pictures and the size, for a test
    void filter(const Pic& pic, float x, float y, float* o) const
    {
        if (hwFilter) bilinearHw(pic, x, y, o);
        else bilinear(pic, x, y, o);
    }

    void moved(const Pic& pic, float x, float y, float* o) const
    {
        RENO_WIPES_NO_FMA
        float px = v38[0] * (x - v36[0]) + v37[0];
        float py = v38[1] * (y - v36[1]) + v37[1];
        if (!(px == px)) px = 0.0f;
        if (!(py == py)) py = 0.0f;
        px = clampf(px, 0.0f, (float)W - 1.0f);
        py = clampf(py, 0.0f, (float)H - 1.0f);
        if (best) filter(pic, px, py, o);
        else pic.texel((int)std::floor(px), (int)std::floor(py), o);       // (read_imagef with the nearest sampler at the coordinate)
    }

    // first = the picture outside the rectangle, mover = the picture inside it (smoothed with smooth3x3 when smoothMoved)
    void pixel(const Pic& first, const Pic& mover, int xi, int yi, float* out) const
    {
        RENO_WIPES_NO_FMA
        const float x = (float)xi, y = (float)yi;
        const int k = variant;                                              // (the best-quality and nearest-read kernels differ only in the read)
        float v[4];
        if (k == 0) {
            if (Spiral::out4(v32, x, y)) first.texel(xi, yi, v);
            else moved(mover, x, y, v);
            for (int c = 0; c < 4; ++c) out[c] = v[c];
            return;
        }
        if (k == 2) {
            for (int c = 0; c < 4; ++c) v[c] = v39[c];
            if (Spiral::farOut(v32, x, y, v31, v35[0])) first.texel(xi, yi, v);
            else if (Spiral::in3(v40, x, y)) moved(mover, x, y, v);
            for (int c = 0; c < 4; ++c) out[c] = v[c];
            return;
        }
        const float ffv = v35[1];
        float v22 = ffv;
        for (int c = 0; c < 4; ++c) v[c] = v39[c];
        if (k == 1) {
            if (Spiral::out4(v32, x, y)) {
                first.texel(xi, yi, v);
                if (Spiral::in3(v34, x, y)) v22 = Spiral::edgeDistance(v31, x, y);
            } else if (Spiral::in3(v40, x, y)) {
                moved(mover, x, y, v);
                if (Spiral::out4(v33, x, y)) v22 = Spiral::edgeDistance(v31, x, y);
            }
        } else {
            if (Spiral::farOut(v32, x, y, v31, v35[0])) {
                first.texel(xi, yi, v);
                if (Spiral::in3(v34, x, y)) v22 = Spiral::edgeDistance(v31, x, y) - v35[0];
            } else if (Spiral::in3(v40, x, y)) {
                moved(mover, x, y, v);
                if (Spiral::out4(v33, x, y)) v22 = Spiral::edgeDistance(v31, x, y);
            }
        }
        v22 = clampf(v22, 0.0f, ffv) / ffv;
        v22 = smooth3(v22);
        mix4(v39, v, v22, out);
    }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
