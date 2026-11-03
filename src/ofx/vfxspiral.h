// Spiral as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on): the incoming picture, shrunk by Zoom, is carried along a spiral over the
// outgoing one. It shares the 2D transition library's set-up with Slide (positions in 18.14 fixed point, four kernels for no border / feather only / border only /
// both) but works out the picture's place its own way (host 0x180605650, geometry 0x180603f30): the move is the angle's cosine and sine times the distances
// to travel, not one distance along a direction. When the picture is drawn at half its size or smaller the host first smooths it with a 3x3 kernel.
//
// Written from the decoded code. Pictures are RGBA floats; x, y given to the pixel functions are the kernel's (y = 0 is the bottom row), see Pic in vfxwipes.h.
#pragma once
#include "vfxwipes.h"
#include <climits>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct Spiral {
    int W = 0, H = 0;
    bool swapPics = false;      // Direction "Out": the outgoing picture is the one that moves
    int variant = 0;            // 0 none, 1 feather only, 2 border only, 3 both
    bool smoothMoved = false;   // the host smooths the moving picture first
    bool hwFilter = true;       // the card's own bilinear filter (fHWBL) rather than the kernel's four reads
    float v31[4], v32[4], v33[4], v34[4], v35[2], v36[2], v37[2], v38[2], v39[4];

    // C's float to int conversion for any value: out of range and NaN give the lowest int (what cvttss2si does), which the callers then treat as "nothing".
    static int cut(double v)
    {
        if (!(v > -2147483648.0 && v < 2147483648.0)) return INT_MIN;
        return (int)v;
    }
    static int sat(double v)    // the same, but kept clear of the ends so that sums of two still fit
    {
        if (!(v == v)) return 0;
        if (v > 1073741823.0) return 1073741823;
        if (v < -1073741823.0) return -1073741823;
        return (int)v;
    }

    // orientation 0 / 1 / 2 / 3 (Left, Top, Right, Bottom) -> 0 / 270 / 180 / 90 degrees: the kernels' rows run bottom up
    static double orientDegrees(int o)
    {
        switch (o) { case 1: return 270.0; case 2: return 180.0; case 3: return 90.0; default: return 0.0; }
    }

    Spiral(int w, int h, double turns, double zoomPercent, int orientation, int motion, int direction, double progress, double borderSize, double borderFeather,
           const float* colour)
    {
        W = w; H = h;
        const double p = (double)(float)progress;
        if (!(turns == turns)) turns = 0.0;
        turns = std::fmin(std::fmax(turns, -1000.0), 1000.0);
        const double ang = (double)(motion != 0 ? 360 : -360) * (p * turns) + orientDegrees(orientation);
        double t = p;
        if (direction != 0) { t = 1.0 - p; swapPics = true; }
        if (!(zoomPercent == zoomPercent)) zoomPercent = 100.0;
        const double zoom = std::fmin(std::fmax(zoomPercent * 0.01, -100.0), 100.0);
        auto unit = [](double v) { return !(v == v) ? 0.0 : std::fmin(std::fmax(v, 0.0), 1.0); };
        int R[4], R1[4], border = 0, feather = 0;
        geometry(w, h, ang, (float)unit(borderSize), (float)unit(borderFeather), zoom, t, R, R1, border, feather);
        variant = (border != 0 ? 2 : 0) + (feather != 0 ? 1 : 0);
        const float K = 6.103515625e-05f;
        float a[4], m[4];                       // a: the rectangle the border and feather are measured from; m: where the moving picture is
        for (int k = 0; k < 4; ++k) { a[k] = (float)R1[k] * K; m[k] = (float)R[k] * K; }
        const float bf = (float)border * K, ff = (float)feather * K;
        const float h5 = 0.5f;
        // keeps the scale below from dividing by nothing: a picture one 16384th wide, or starting at the frame's right edge, is nudged
        if (R[2] == 1) m[2] = -9.999999747378752e-05f;
        if (cut((double)m[0]) == w) m[0] = (float)w + 9.999999747378752e-05f;
        for (int k = 0; k < 2; ++k) {
            v31[k] = a[k] - h5;           v31[k + 2] = a[k + 2] - h5;
            v32[k] = a[k] - bf;           v32[k + 2] = a[k + 2] + bf;
            v33[k] = (a[k] - h5) + ff;    v33[k + 2] = (a[k + 2] - h5) - ff;
            v34[k] = (a[k] - bf) - ff;    v34[k + 2] = (a[k + 2] + bf) + ff;
        }
        v35[0] = bf; v35[1] = ff;
        v36[0] = (m[2] + m[0]) * h5 - h5; v36[1] = (m[3] + m[1]) * h5 - h5;
        v37[0] = (float)w * h5; v37[1] = (float)h * h5;
        v38[0] = (float)w / (m[2] - m[0]); v38[1] = (float)h / (m[3] - m[1]);
        for (int k = 0; k < 4; ++k) v39[k] = colour[k];
        const int sx = cut((double)v38[0]), sy = cut((double)v38[1]);
        smoothMoved = sx > 1 || sy > 1;         // (assuming the best quality setting: GUESS)
    }

    // 0x180603f30. R: where the moving picture is, R1: the rectangle the border is drawn around (R, or R grown by the feather while the picture waits outside).
    static void geometry(int w, int h, double angleDeg, float borderSize, float borderFeather, double zoom, double t, int* R, int* R1, int& border, int& feather)
    {
        const int Wf = w << 14, Hf = h << 14;
        const double tc = (0.0 <= t) ? std::fmin(1.0, t) : 0.0;
        const int mn = std::min(Wf, Hf), mx = std::max(Wf, Hf);
        const float fmn = (float)mn;
        border = sat((double)(fmn * borderSize) + 0.5);
        feather = sat((double)(fmn * borderFeather) + 0.5);
        const int ex = 0x8000 - feather;
        if ((border != 0 || feather != 0) && ex > 0) {
            feather = 0x8000;
            border -= ex;
            if (border < 0) border = 0;
        }
        const double x1 = (double)feather / (double)mx;
        const double s = (x1 + 1.0) * (1.0 - tc) - x1;
        const double z = (zoom - 1.0) * s + 1.0;
        R[0] = 0; R[1] = 0; R[2] = Wf; R[3] = Hf;
        if (s >= 0.0) {
            R[2] = sat((double)Wf * z); R[3] = sat((double)Hf * z);
            const double a = angleDeg * 0.017453292519943;
            const double x7 = ((double)(-(Wf / 2)) - ((double)Wf * z * 0.5 + (double)border + (double)feather)) * s;
            const double y7 = ((double)(-(Hf / 2)) - ((double)Hf * z * 0.5 + (double)border + (double)feather)) * s;
            const int bx = sat(std::cos(a) * x7 + 0.5), by = sat(std::sin(a) * y7 + 0.5);
            const int dx = sat((1.0 - z) * (double)Wf * 0.5 + 0.5) + bx, dy = sat((1.0 - z) * (double)Hf * 0.5 + 0.5) + by;
            R[0] += dx; R[2] += dx; R[1] += dy; R[3] += dy;
            for (int i = 0; i < 4; ++i) R1[i] = R[i];
        } else {
            for (int i = 0; i < 4; ++i) R1[i] = R[i];
            const int e = sat(-(s / x1) * (double)feather);
            R1[0] -= e; R1[2] += e; R1[1] -= e; R1[3] += e;
        }
    }

    // the library's tests (prog61 f3, f4): strictly inside, and its complement
    static bool in3(const float* r, float x, float y) { return r[0] < x && x < r[2] && r[1] < y && y < r[3]; }
    static bool out4(const float* r, float x, float y) { return !in3(r, x, y); }
    // f5: distance from the rectangle's edge, from inside or outside
    static float edgeDistance(const float* r, float x, float y)
    {
        const float d20 = std::fmin(std::fabs(x - r[0]), std::fabs(x - r[2]));
        const float d21 = std::fmin(std::fabs(y - r[1]), std::fabs(y - r[3]));
        if (in3(r, x, y)) return std::fmin(d20, d21);
        if (r[0] < x && x < r[2]) return d21;
        if (r[1] < y && y < r[3]) return d20;
        return std::hypot(d20, d21);
    }
    // f6: outside the outer rectangle, or outside the inner one by more than the border
    static bool farOut(const float* outer, float x, float y, const float* inner, float bw)
    {
        bool r = out4(outer, x, y);
        if (!r && !in3(inner, x, y)) r = edgeDistance(inner, x, y) > bw;
        return r;
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
        if (hwFilter) bilinearHw(pic, px, py, o);
        else bilinear(pic, px, py, o);
    }

    // first = the picture behind (the outgoing one, or the incoming one with Direction Out), mover = the moving picture (smoothed with smooth() when smoothMoved)
    void pixel(const Pic& first, const Pic& mover, int xi, int yi, float* out) const
    {
        RENO_WIPES_NO_FMA
        const float x = (float)xi, y = (float)yi;
        float v[4];
        if (variant == 0) {
            if (out4(v31, x, y)) first.texel(xi, yi, v);
            else moved(mover, x, y, v);
            for (int k = 0; k < 4; ++k) out[k] = v[k];
            return;
        }
        if (variant == 2) {
            for (int k = 0; k < 4; ++k) v[k] = v39[k];
            if (farOut(v32, x, y, v31, v35[0])) first.texel(xi, yi, v);
            else if (out4(v31, x, y)) moved(mover, x, y, v);
            for (int k = 0; k < 4; ++k) out[k] = v[k];
            return;
        }
        const float v42 = v35[1];
        float v22 = v42;
        if (variant == 1) {
            if (out4(v31, x, y)) {
                first.texel(xi, yi, v);
                if (in3(v34, x, y)) v22 = edgeDistance(v31, x, y);
            } else {
                moved(mover, x, y, v);
                if (out4(v33, x, y)) v22 = edgeDistance(v31, x, y);
            }
        } else {
            for (int k = 0; k < 4; ++k) v[k] = v39[k];
            if (farOut(v32, x, y, v31, v35[0])) {
                first.texel(xi, yi, v);
                if (in3(v34, x, y)) v22 = edgeDistance(v31, x, y) - v35[0];
            } else if (in3(v31, x, y)) {
                moved(mover, x, y, v);
                if (out4(v33, x, y)) v22 = edgeDistance(v31, x, y);
            }
        }
        v22 = clampf(v22, 0.0f, v42) / v42;
        v22 = smooth3(v22);
        mix4(v39, v, v22, out);
    }

    // the host's smoothing pass (see smooth3x3 in vfxwipes.h)
    static void smooth(const Pic& in, std::vector<float>& outv, float (*store)(float)) { smooth3x3(in, outv, store); }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
