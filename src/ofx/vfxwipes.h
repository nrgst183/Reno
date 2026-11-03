// Barn Door, Push and Slide as VEGAS draws them with its OpenCL kernels (Vfx1.ofx, GPU acceleration on): float pictures, the moved picture read with a
// bilinear filter and the border / feather mixed in with a smoothstep. The host side (what VEGAS works out from the settings and hands to the kernel) and
// the kernels are written here from the decoded code; nothing here depends on Reno's Ctx, so a test can drive it with plain arrays.
//
// Pictures are RGBA floats. The kernels number their rows from the BOTTOM row; Pic::topFirst says the memory has the top row first (Reno's Buf), and x, y given
// to the pixel functions are the kernel's (y = 0 is the bottom row). Every number is a float, in the order the kernel does it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// the kernels are compiled without fused multiply-adds; clang's default on some CPUs would fuse them
#if defined(__clang__)
#define RENO_WIPES_NO_FMA _Pragma("clang fp contract(off)")
#else
#define RENO_WIPES_NO_FMA
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace wipes {

struct Pic {
    const float* base = nullptr;
    int w = 0, h = 0;
    bool topFirst = true;
    // read_imagef with the nearest sampler and CLK_ADDRESS_CLAMP to a transparent black border: outside the picture it is zero
    inline void texel(int x, int y, float* o) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) { o[0] = o[1] = o[2] = o[3] = 0.f; return; }
        const float* p = base + ((size_t)(topFirst ? h - 1 - y : y) * w + x) * 4;
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
    }
};

// The kernels' f0 (software path, fHWBL off): four nearest reads at the cell of the coordinate, weighted in floats. x, y are pixel indices (no half pixel).
inline void bilinear(const Pic& pic, float x, float y, float* o)
{
    RENO_WIPES_NO_FMA
    const float flx = std::floor(x), fly = std::floor(y);
    float fx = x - flx, fy = y - fly;
    const float below1 = 0.99999994f;           // (the largest float below 1: fract() never returns 1.0)
    if (fx > below1) fx = below1;
    if (fy > below1) fy = below1;
    const int ix = (int)flx, iy = (int)fly;
    const float v7 = 1.0f - fy, v8 = 1.0f - fx;
    const float w00 = v7 * v8, w10 = v7 * fx, w01 = fy * v8, w11 = fy * fx;
    float a[4], b[4], c[4], d[4];
    pic.texel(ix, iy, a);
    pic.texel(ix + 1, iy, b);
    pic.texel(ix, iy + 1, c);
    pic.texel(ix + 1, iy + 1, d);
    for (int k = 0; k < 4; ++k) {
        const float t0 = w00 * a[k], t1 = w10 * b[k], t2 = w01 * c[k], t3 = w11 * d[k];
        o[k] = ((t0 + t1) + t2) + t3;
    }
}

// The kernels' f0 with the card's hardware filter (fHWBL: read_imagef with a linear sampler at the coordinate + half a pixel): the weights are cut to 8 fractional bits (rounded half up),
// reads outside the picture are transparent black. This is what the zoom of VEGAS's pictures shows (the software bilinear above differs from it on a few of a thousand pixels).
inline void bilinearHw(const Pic& pic, float cx, float cy, float* o)
{
    RENO_WIPES_NO_FMA
    const float x = (cx + 0.5f) - 0.5f, y = (cy + 0.5f) - 0.5f;
    const float flx = std::floor(x), fly = std::floor(y);
    float fx = x - flx, fy = y - fly;
    fx = std::floor(fx * 256.0f + 0.5f) / 256.0f;
    fy = std::floor(fy * 256.0f + 0.5f) / 256.0f;
    const int x0 = (int)flx, y0 = (int)fly;
    float a[4], b[4], c[4], d[4];
    pic.texel(x0, y0, a);
    pic.texel(x0 + 1, y0, b);
    pic.texel(x0, y0 + 1, c);
    pic.texel(x0 + 1, y0 + 1, d);
    for (int k = 0; k < 4; ++k) {
        const float top = (1.0f - fx) * a[k] + fx * b[k];
        const float bot = (1.0f - fx) * c[k] + fx * d[k];
        o[k] = (1.0f - fy) * top + fy * bot;
    }
}

// The host's smoothing pass (prog61's fifth kernel, prog62's first): weights 1 2 1 / 2 4 2 / 1 2 1 over the pixel and its eight neighbours, edges repeated, added in the
// order the kernel adds them. Returns the picture top row first, each value cut with store when given (an 8-bit frame's image).
inline void smooth3x3(const Pic& in, std::vector<float>& outv, float (*store)(float))
{
    RENO_WIPES_NO_FMA
    const int w = in.w, h = in.h;
    outv.assign((size_t)w * h * 4, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(x - 1, 0), y0 = std::max(y - 1, 0), x1 = std::min(x + 1, w - 1), y1 = std::min(y + 1, h - 1);
            float c00[4], c10[4], c01[4], c11[4], e0[4], e1[4], e2[4], e3[4], cc[4];
            in.texel(x0, y0, c00); in.texel(x1, y0, c10); in.texel(x0, y1, c01); in.texel(x1, y1, c11);
            in.texel(x0, y, e0); in.texel(x, y0, e1); in.texel(x1, y, e2); in.texel(x, y1, e3);
            in.texel(x, y, cc);
            float* o = &outv[((size_t)(in.topFirst ? h - 1 - y : y) * w + x) * 4];
            for (int k = 0; k < 4; ++k) {
                float v = c00[k] + c10[k] + c01[k] + c11[k] + 2.0f * (e0[k] + e1[k] + e2[k] + e3[k]) + 4.0f * cc[k];
                v *= 0.0625f;
                o[k] = store ? store(v) : v;
            }
        }
    }
}

inline float clampf(float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); }
// the smoothstep of the border mix: mad(-2, m, 3) * m * m
inline float smooth3(float m) { RENO_WIPES_NO_FMA return ((-2.0f * m) + 3.0f) * m * m; }
inline void mix4(const float* a, const float* b, float t, float* o)
{
    RENO_WIPES_NO_FMA
    for (int k = 0; k < 4; ++k) o[k] = a[k] + (b[k] - a[k]) * t;
}

// ------------------------------------------------------------------------------------------------------------------------------------ Barn Door
// Host (0x18059af30): the progress is a float, clamped; "In" runs it backwards and swaps the two pictures. n is the width (Orientation 0) or the height; the
// doors are half of it. Border and feather are fractions of that half. s0 .. s3 are the kernel's v21.
struct BarnDoor {
    int W = 0, H = 0;
    bool alongX = true, swapPics = false;
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    float col[4] = {0, 0, 0, 1};

    BarnDoor(int w, int h, int orientation, int direction, double progress, double borderSize, double borderFeather, const float* colour)
    {
        W = w; H = h;
        alongX = orientation <= 0;
        float p = (float)progress;
        p = 0.0f > p ? 0.0f : std::fmin(1.0f, p);
        if (direction == 0) { p = 1.0f - p; swapPics = true; }
        const float half = (float)(alongX ? W : H) * 0.5f;
        const float border = (float)borderSize * half;
        float feather = (float)borderFeather * half;
        if (1.0f > feather) feather = 0.0f;
        const float x4 = border + half;
        const float x0 = ((x4 + feather) + feather) * p;
        const float x2 = x0 - feather;
        s3 = 0.0f > x2 ? 0.0f : std::fmin(x4, x2);
        float y2 = (feather + half) - x0;
        if (y2 == 0.0f || y2 + border == 0.0f) y2 += -9.999999747378752e-06f;
        y2 += -0.5f;
        s0 = y2; s1 = border; s2 = feather;
        for (int k = 0; k < 4; ++k) col[k] = colour[k];
    }

    // the kernel (prog49): from = the outgoing picture, to = the incoming one
    void pixel(const Pic& from, const Pic& to, int x, int y, float* out) const
    {
        RENO_WIPES_NO_FMA
        const Pic& i0 = swapPics ? to : from;
        const Pic& i1 = swapPics ? from : to;
        const float nf = (float)(alongX ? W : H);
        float fx = (float)x, fy = (float)y;
        float& pos = alongX ? fx : fy;
        const float v29 = s0, v30 = s0 + s1, v31 = s0 - s2, v32 = v30 + s2;
        const float v33 = 0.5f * nf + 0.0001f;
        const float v36 = nf - v30, v37 = v36 + s1, v38 = v36 - s2, v39 = v37 + s2;
        const float v42 = 0.0f < s2 ? 1.0f / s2 : 1.0f;
        float v40[4] = {col[0], col[1], col[2], col[3]};
        float v41 = 1.0f;
        const float maxX = (float)W - 1.0f, maxY = (float)H - 1.0f;
        auto fetch = [&](const Pic& pic) {
            fx = clampf(fx, 0.0f, maxX);
            fy = clampf(fy, 0.0f, maxY);
            bilinear(pic, fx, fy, v40);
        };
        if (pos < v33) {
            if (pos <= v29 || pos > v30) {
                if (pos <= v29) {
                    v41 = (pos > v31) ? (pos - v31) * v42 : 0.0f;
                    pos += s3;
                    fetch(i0);
                } else {
                    v41 = (pos < v32) ? (v32 - pos) * v42 : 0.0f;
                    fetch(i1);
                }
            }
        } else {
            if (pos <= v36 || pos > v37) {
                if (pos <= v36) {
                    v41 = (pos > v38) ? (pos - v38) * v42 : 0.0f;
                    fetch(i1);
                } else {
                    v41 = (pos < v39) ? (v39 - pos) * v42 : 0.0f;
                    pos -= s3;
                    fetch(i0);
                }
            }
        }
        v41 = smooth3(v41);
        mix4(v40, col, v41, out);
    }
};

// ------------------------------------------------------------------------------------------------------------------------------------ Push
// Host (0x1805fa330): the progress is a float, clamped; direction 0 up, 1 down, 2 left, 3 right. L is the height (up, down) or the width. Four kernels (prog59),
// one per direction; the host picks them 0 -> 1, 1 -> 0, 2 -> 2, 3 -> 3 (found on VEGAS's pictures).
struct Push {
    int W = 0, H = 0, dir = 0, kernel = 1;
    float L = 0, pushOff = 0;
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    float col[4] = {0, 0, 0, 1};

    Push(int w, int h, int direction, double progress, double borderSize, double borderFeather, bool pushOffPrevious, const float* colour)
    {
        W = w; H = h;
        dir = direction < 0 ? 0 : (direction > 3 ? 3 : direction);
        static const int kmap[4] = {1, 0, 2, 3};
        kernel = kmap[dir];
        float p = (float)progress;
        p = 0.0f > p ? 0.0f : std::fmin(1.0f, p);
        L = (float)(dir > 1 ? W : H);
        const float border = (float)borderSize * L;
        float feather = (float)borderFeather * L;
        if (1.0f > feather) feather = 0.0f;
        const float x10 = L + border;
        const float x0 = ((x10 + feather) + feather) * p;
        const float x1 = x0 - feather;
        s3 = 0.0f > x1 ? 0.0f : std::fmin(x10, x1);
        float a = (dir == 0 || dir == 3) ? x1 : (L + feather) - x0;
        s0 = a + -0.5f;
        s1 = border; s2 = feather;
        pushOff = pushOffPrevious ? 2.0f : 0.0f;
        for (int k = 0; k < 4; ++k) col[k] = colour[k];
    }

    void pixel(const Pic& from, const Pic& to, int x, int y, float* out) const
    {
        RENO_WIPES_NO_FMA
        float fx = (float)x, fy = (float)y;
        const bool alongY = kernel < 2;
        float& pos = alongY ? fy : fx;
        const float maxX = (float)W - 1.0f, maxY = (float)H - 1.0f;
        const float v42 = 0.0f < s2 ? 1.0f / s2 : 0.0f;       // (the divisions are guarded by 0 < s2 in the kernel)
        float v36[4] = {col[0], col[1], col[2], col[3]};
        float v37 = 0.0f;
        auto fetch = [&](const Pic& pic) {
            fx = clampf(fx, 0.0f, maxX);
            fy = clampf(fy, 0.0f, maxY);
            bilinear(pic, fx, fy, v36);
        };
        if (kernel == 0 || kernel == 2) {
            const float v33 = s0 + s1, v34 = s0 - s2, v35 = v33 + s2;
            if (pos < s0) {
                v37 = (pos > v34 && 0.0f < s2) ? (pos - v34) * v42 : 0.0f;
                pos += (1.0f < pushOff) ? s3 : 0.0f;
                fetch(from);
            } else if (pos > v33) {
                v37 = (pos < v35 && 0.0f < s2) ? (v35 - pos) * v42 : 0.0f;
                pos += (s3 - L) - s1;
                fetch(to);
            } else {
                v37 = 1.0f;
            }
        } else {
            const float v33 = s0, v32 = s0 - s1, v34 = v32 - s2, v35 = v33 + s2;
            const bool atOrAfter = kernel == 3 ? pos >= v33 : pos > v33;
            if (pos < v32) {
                v37 = (pos > v34 && 0.0f < s2) ? (pos - v34) * v42 : 0.0f;
                pos += (L - s3) + s1;
                fetch(to);
            } else if (atOrAfter) {
                v37 = (pos < v35 && 0.0f < s2) ? (v35 - pos) * v42 : 0.0f;
                pos -= (1.0f < pushOff) ? s3 : 0.0f;
                fetch(from);
            } else {
                v37 = 1.0f;
            }
        }
        v37 = smooth3(v37);
        mix4(v36, col, v37, out);
    }
};

// ------------------------------------------------------------------------------------------------------------------------------------ Slide
// Host (0x1805ff4e0 with the geometry of 0x1805fdef0, the 2D transition library Slide shares with Spiral and Split): positions in 18.14 fixed point (1 / 16384 pixel),
// the moved picture's rectangle B and the rectangle A the border and feather are measured from; the kernel gets them as floats (int x 2^-14) with the angle
// turned to y-up (360 - Angle). Four kernels (prog60): no border / no feather, feather only, border only, both.
struct Slide {
    int W = 0, H = 0;
    bool swapPics = false;
    int variant = 0;
    float v31[4], v32[4], v33[4], v34[4], v35[2], v36[2], v37[2], v38[2], v39[4], v40[4];

    Slide(int w, int h, double angle, int direction, double progress, double borderSize, double borderFeather, const float* colour)
    {
        W = w; H = h;
        const double ang = (double)(360.0f - (float)angle);
        const double pf = (double)(float)progress;
        double t = pf;
        if (direction != 0) { t = 1.0 - pf; swapPics = true; }
        int A[4], B[4], border = 0, feather = 0;
        geometry(w, h, ang, (float)borderSize, (float)borderFeather, 1.0, t, A, B, border, feather);
        variant = (border != 0 ? 2 : 0) + (feather != 0 ? 1 : 0);
        const float K = 6.103515625e-05f;
        float Af[4], Bf[4];
        for (int k = 0; k < 4; ++k) { Af[k] = (float)A[k] * K; Bf[k] = (float)B[k] * K; }
        const float bf = (float)border * K, ff = (float)feather * K;
        const float h5 = 0.5f;
        for (int k = 0; k < 2; ++k) {
            v31[k] = Af[k] - h5;           v31[k + 2] = Af[k + 2] - h5;
            v32[k] = Af[k] - bf;           v32[k + 2] = Af[k + 2] + bf;
            v33[k] = (Af[k] - h5) + ff;    v33[k + 2] = (Af[k + 2] - h5) - ff;
            v34[k] = (Af[k] - bf) - ff;    v34[k + 2] = (Af[k + 2] + bf) + ff;
        }
        v35[0] = bf; v35[1] = ff;
        v36[0] = (Af[2] + Af[0]) * h5 - h5; v36[1] = (Af[3] + Af[1]) * h5 - h5;
        v37[0] = (float)w * h5; v37[1] = (float)h * h5;
        v38[0] = (float)w / (Bf[2] - Bf[0]); v38[1] = (float)h / (Bf[3] - Bf[1]);
        for (int k = 0; k < 4; ++k) { v39[k] = colour[k]; v40[k] = Bf[k]; }
    }

    // 0x1805fdef0. Returns the rectangles A (border and feather are measured from it) and B (where the moved picture is) in 1/16384 pixels, the border width and the feather.
    static void geometry(int w, int h, double angleDeg, float borderSize, float borderFeather, double zoom, double t, int* A, int* B, int& border, int& feather)
    {
        const int Wf = w << 14, Hf = h << 14;
        const double tc = (0.0 <= t) ? std::fmin(1.0, t) : 0.0;
        const int mn = std::min(Wf, Hf), mx = std::max(w, h) << 14;
        const float fmn = (float)mn;
        const float pb = fmn * borderSize, pf = fmn * borderFeather;
        border = (int)((double)pb + 0.5);
        feather = (int)((double)pf + 0.5);
        const int ex = 0x8000 - feather;
        if ((border != 0 || feather != 0) && ex > 0) {
            feather = 0x8000;
            border -= ex;
            if (border < 0) border = 0;
        }
        const double x1 = (double)feather / (double)mx;
        const double s = (x1 + 1.0) * (1.0 - tc) - x1;
        const double z = (zoom - 1.0) * s + 1.0;
        const double a = angleDeg * 0.017453292519943;
        const double sn = std::sin(a), cs = std::cos(a);
        const double k = s / std::fmax(std::fabs(sn), std::fabs(cs));
        B[0] = 0; B[1] = 0; B[2] = Wf; B[3] = Hf;
        if (!(k < 0.0)) {
            B[2] = (int)((double)Wf * z) + B[0];
            B[3] = (int)((double)Hf * z) + B[1];
            const double x7 = ((double)(-(Wf / 2)) - ((double)Wf * z * 0.5 + (double)border + (double)feather)) * k;
            const double y7 = ((double)(-(Hf / 2)) - ((double)Hf * z * 0.5 + (double)border + (double)feather)) * k;
            const int bx = (int)(x7 * cs + 0.5), by = (int)(y7 * sn + 0.5);
            const int dx = (int)((1.0 - z) * (double)Wf * 0.5 + 0.5) + bx, dy = (int)((1.0 - z) * (double)Hf * 0.5 + 0.5) + by;
            B[0] += dx; B[2] += dx; B[1] += dy; B[3] += dy;
            for (int i = 0; i < 4; ++i) A[i] = B[i];
        } else {
            for (int i = 0; i < 4; ++i) A[i] = B[i];
            const int e = (int)(-(s / x1 * (double)feather));
            A[0] -= e; A[2] += e; A[1] -= e; A[3] += e;
        }
    }

    static bool inside(const float* r, float x, float y) { return r[0] <= x && x <= r[2] && r[1] <= y && y <= r[3]; }
    static bool outside(const float* r, float x, float y) { return r[0] >= x || x >= r[2] || r[1] >= y || y >= r[3]; }
    // distance from the rectangle's edge (f5)
    static float edgeDistance(const float* r, float x, float y)
    {
        const float d20 = std::fmin(std::fabs(x - r[0]), std::fabs(x - r[2]));
        const float d21 = std::fmin(std::fabs(y - r[1]), std::fabs(y - r[3]));
        float d;
        if (inside(r, x, y)) d = std::fmin(d20, d21);
        else if (r[0] < x && x < r[2]) d = d21;
        else if (r[1] < y && y < r[3]) d = d20;
        else d = std::hypot(d20, d21);
        return d;
    }
    // f6: outside the outer rectangle, or outside the inner one by more than the border
    static bool farOut(const float* outer, float x, float y, const float* inner, float bw)
    {
        bool r = outside(outer, x, y);
        if (!r && !inside(inner, x, y)) r = edgeDistance(inner, x, y) > bw;
        return r;
    }

    // the moved picture read at the pixel's place carried back: (p - v36) * v38 + v37, clamped to the picture
    void moved(const Pic& pic, float x, float y, float* o) const
    {
        RENO_WIPES_NO_FMA
        float px = (x - v36[0]) * v38[0] + v37[0];
        float py = (y - v36[1]) * v38[1] + v37[1];
        px = clampf(px, 0.0f, (float)W - 1.0f);
        py = clampf(py, 0.0f, (float)H - 1.0f);
        bilinear(pic, px, py, o);
    }

    // from = the outgoing picture, to = the incoming one. A pixel the kernel leaves unset (inside A but not inside the picture rectangle B: the half pixel between
    // A's edge and B's, kernels without a border colour to start from) shows, on VEGAS's card, the red, blue and alpha of the moved picture at the pixel itself with
    // green zero (seen on the BorderFeather 0.5 picture; the same is assumed for the kernel with neither border nor feather: GUESS).
    void pixel(const Pic& from, const Pic& to, int xi, int yi, float* out) const
    {
        RENO_WIPES_NO_FMA
        const Pic& i0 = swapPics ? to : from;
        const Pic& i1 = swapPics ? from : to;
        const float x = (float)xi, y = (float)yi;
        float v[4];
        auto unset = [&]() {
            float t[4];
            i1.texel(xi, yi, t);
            v[0] = t[0]; v[1] = 0.0f; v[2] = t[2]; v[3] = t[3];
        };
        if (variant == 0) {
            if (outside(v31, x, y)) i0.texel(xi, yi, v);
            else if (inside(v40, x, y)) moved(i1, x, y, v);
            else unset();
            for (int k = 0; k < 4; ++k) out[k] = v[k];
            return;
        }
        if (variant == 2) {
            for (int k = 0; k < 4; ++k) v[k] = v39[k];
            if (farOut(v32, x, y, v31, v35[0])) i0.texel(xi, yi, v);
            else if (outside(v40, x, y)) moved(i1, x, y, v);
            for (int k = 0; k < 4; ++k) out[k] = v[k];
            return;
        }
        const float v43 = v35[1];
        float v22 = v43;
        if (variant == 1) {
            bool set = true;
            if (outside(v31, x, y)) {
                i0.texel(xi, yi, v);
                if (inside(v34, x, y)) v22 = edgeDistance(v31, x, y);
            } else if (inside(v40, x, y)) {
                moved(i1, x, y, v);
                if (outside(v33, x, y)) v22 = edgeDistance(v31, x, y);
            } else {
                set = false;
                unset();
            }
            (void)set;
        } else {
            for (int k = 0; k < 4; ++k) v[k] = v39[k];
            if (farOut(v32, x, y, v31, v35[0])) {
                i0.texel(xi, yi, v);
                if (inside(v34, x, y)) v22 = edgeDistance(v31, x, y) - v35[0];
            } else if (inside(v40, x, y)) {
                moved(i1, x, y, v);
                if (outside(v33, x, y)) v22 = edgeDistance(v31, x, y);
            }
        }
        v22 = clampf(v22, 0.0f, v43) / v43;
        v22 = smooth3(v22);
        mix4(v39, v, v22, out);
    }
};

}  // namespace wipes
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
