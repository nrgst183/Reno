// Built-in blur effects (Quick Blur, Radial Blur, Defocus, Unsharp Mask, Median, Min and Max, Chroma Blur); see builtins_more.cpp.
#include <cstring>
#include "builtins_more_internal.h"
#include "builtins_deriche.h"
#include "builtins_fast.h"
#include "builtins_vec.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ blur
void quickBlur(Ctx& c)
{
    const float k = (float)clampd(c.d("BlendAmount"), 0, 1);
    Buf b = c.src;
    gaussianBlur(b, 1.2 * frameScale(c) + c.px(0.4), 1.2 * frameScale(c) + c.px(0.4));
    blendOver(c, b, k);
}

// Radial Blur, VEGAS's CPU processor. In place in four quadrants, each from the centre pixel outwards (rows bottom first); every pixel blends with the already blurred picture at a point nearer the
// centre: Proportional a fraction 0.018 Strength^1.5 / (min(W, H) / 300) of the way in, Fixed radius (and Gaussian, treated the same) Strength^2 + 1 pixels in. The blend keeps (1 - w) of the pixel,
// w = (Strength^(300 / min(W, H)))^0.25, in 16-bit fixed point; the point is read bilinearly with 8-bit weights (missing right / upper neighbours read as opaque black). Two VEGAS quirks are kept:
// a sample within a pixel of the pixel itself reads the pixel's own source value in one corner of the 2x2, and the start of the two upper quadrants puts back the source pixel just above the centre.
void radialBlur(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    const int type = c.i("Type");
    const double st = c.d("Strength");
    if (!(st > 0)) { copyThrough(c); return; }
    const bool q8 = c.levels;
    // the picture bottom row first; 8-bit values (exact levels) or floats in 0..255
    std::vector<float> src((size_t)W * H * 4), dst;
    for (int y = 0; y < H; ++y) {
        const float* r = c.src.at(0, H - 1 - y);
        float* o = &src[(size_t)y * W * 4];
        for (int i = 0; i < W * 4; ++i) o[i] = q8 ? (float)roundNonNeg(std::min(std::max(r[i], 0.f), 1.f) * 255.f) : r[i] * 255.f;
    }
    dst = src;
    const double k = std::min(W, H) / 300.0;
    const double s0 = std::pow(st, 2.0) + 1.0, s1 = std::pow(st, 1.5) * 0.018 / k;
    const double wd = std::pow(std::pow(st, 1.0 / k), 0.25) * 65536.0;
    const long long wi = (long long)wd, iwi = 65536 - wi;
    const double wf = wd / 65536.0, iwf = 1.0 - wf;
    int cx = (int)(W * pcx), cy = (int)(H * pcy);
    if (cx == W) cx = W - 1;
    if (cy == H) cy = H - 1;
    cx = std::max(0, std::min(W - 1, cx)); cy = std::max(0, std::min(H - 1, cy));
    static const float black[4] = {0.f, 0.f, 0.f, 255.f};
    auto D = [&](int x, int y) { return &dst[((size_t)y * W + x) * 4]; };
    auto S = [&](int x, int y) { return &src[((size_t)y * W + x) * 4]; };
    for (int q = 0; q < 4; ++q) {
        if ((q == 1 || q == 3) && cx == W - 1) continue;
        if ((q == 2 || q == 3) && cy == H - 1) continue;
        const int xs = (q & 1) ? cx + 1 : cx, xstep = (q & 1) ? 1 : -1, nx = (q & 1) ? W - cx - 1 : cx + 1;
        const int ys = (q & 2) ? cy + 1 : cy, ystep = (q & 2) ? 1 : -1, ny = (q & 2) ? H - cy - 1 : cy + 1;
        {
            const int cr = (q & 2) ? cy + 1 : cy;
            if (cr < H) std::memcpy(D(cx, cr), S(cx, cr), 4 * sizeof(float));
        }
        int y = ys;
        for (int j = 0; j < ny; ++j, y += ystep) {
            const int dY = cy - y;
            int x = xs;
            for (int i = 0; i < nx; ++i, x += xstep) {
                const int dX = cx - x;
                double sx, sy;
                if (type != 0) {
                    const double d = std::sqrt((double)(dX * dX + dY * dY));
                    if (!(s0 < d)) { sx = cx; sy = cy; }
                    else { const double f = s0 / d; sx = dX * f + x; sy = dY * f + y; }
                } else { sx = dX * s1 + x; sy = dY * s1 + y; }
                const int ix = std::max(0, std::min(W - 1, (int)sx)), iy = std::max(0, std::min(H - 1, (int)sy));
                const double frx = sx - ix, fry = sy - iy;
                const float* p[4];
                p[0] = D(ix, iy);
                if (ix == W - 1 || iy == H - 1) {
                    p[3] = black;
                    p[1] = ix == W - 1 ? black : D(ix + 1, iy);
                    p[2] = iy == H - 1 ? black : D(ix, iy + 1);
                } else { p[1] = D(ix + 1, iy); p[2] = D(ix, iy + 1); p[3] = D(ix + 1, iy + 1); }
                if (std::fabs(sx - x) < 1.0 && std::fabs(sy - y) < 1.0) {
                    if (frx == 0.0 && fry == 0.0) p[0] = S(x, y);
                    else p[q] = S(x, y);
                }
                const float* o0 = S(x, y);
                float* o = D(x, y);
                if (q8) {
                    const int fx = (int)(frx * 256.0), fy = (int)(fry * 256.0);
                    for (int ch = 0; ch < 4; ++ch) {
                        const long long a = ((long long)p[0][ch] * (256 - fx) + (long long)p[1][ch] * fx) * (256 - fy);
                        const long long b = ((long long)p[2][ch] * (256 - fx) + (long long)p[3][ch] * fx) * fy;
                        const long long smp = ((a + b) >> 16) & 0xff;
                        o[ch] = (float)((smp * wi + (long long)o0[ch] * iwi) >> 16);
                    }
                } else {
                    for (int ch = 0; ch < 4; ++ch) {
                        const double smp = (p[0][ch] * (1 - frx) + p[1][ch] * frx) * (1 - fry) + (p[2][ch] * (1 - frx) + p[3][ch] * frx) * fry;
                        o[ch] = (float)(smp * wf + o0[ch] * iwf);
                    }
                }
            }
        }
    }
    for (int y = 0; y < H; ++y) {
        const float* r = &dst[(size_t)y * W * 4];
        float* o = c.out.at(0, H - 1 - y);
        for (int i = 0; i < W * 4; ++i) o[i] = r[i] * (1.f / 255.f);
    }
}

// Defocus: a disc (or polygon) iris with bright colours blooming. Sizes from VEGAS's pictures at 320x180: iris radius 3.27 x Radius - 1.27 pixels, growing with the frame's height;
// the bloom gain of channels above the threshold is 1.10 x Bloom strength - 1.98 more at its brightest.
void defocus(Ctx& c)
{
    const double radius = std::max(0.0, 3.2714 * c.d("Radius") - 1.2651) * c.out.h / 180.0;
    if (radius < 0.5) { copyThrough(c); return; }
    const int shape = c.i("IrisShape");
    const double turn = c.d("Orientation") * 2 * kPi;
    const double curvature = clampd(c.d("DiscCurvature") / 100.0, -1, 1);   // < 0: a ring
    const double thresh = clampd(c.d("BloomThreshold") / 100.0, 0, 2), gainA = std::max(0.0, 1.1018 * c.d("BloomStrength") - 1.9834);
    const int sides = shape == 0 ? 0 : shape + 4;
    defocusFast(c.src, c.out, radius, sides, turn, curvature, thresh, gainA);
}

// Unsharp Mask, VEGAS's CPU processor. The picture is blurred by VEGAS's Gaussian (Deriche filter, builtins_deriche.cpp; sigma (s + 1.5) / sqrt(2 ln 255), s = Radius * the larger
// side, as Gaussian Blur); every channel (alpha too) of the 8-bit premultiplied picture becomes v = source + (source - blur) * Amount, kept in 0..255 and truncated, and is taken where it
// differs from the blur by more than (int)(Threshold * 255 + 0.5) levels (the source elsewhere). Radius 0 or threshold 255 levels changes nothing.
double unsharpSigma(double radius, int W, int H, bool& none)
{
    const double r = radius < 0 ? 0.0 : std::min(1.0, radius);
    const double s = (double)std::max(W, H) * r;
    none = std::fabs((float)s) <= 1e-6f || std::fabs((float)radius) <= 1e-6f;
    return std::max(0.6, (s + 1.5) / std::sqrt(-2.0 * std::log(1.0 / 255.0)));
}
void unsharpMask(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double amount = c.d("Amount");
    const int thr8 = (int)(c.d("Threshold") * 255.0 + 0.5);
    bool none = false;
    const double sigma = unsharpSigma(c.d("Radius"), W, H, none);
    if (none || (thr8 & 255) == 255) { copyThrough(c); return; }
    Buf blur = c.src;
    vegasDericheBlurSigma(blur, sigma, sigma, c.levels);
    const bool q8 = c.levels;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s = c.src.at(x, y);
            const float* bl = blur.at(x, y);
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) {
                const double sv = q8 ? std::nearbyint(std::min(std::max(s[k], 0.f), 1.f) * 255.0) : s[k] * 255.0;
                const double bv = q8 ? std::nearbyint(std::min(std::max(bl[k], 0.f), 1.f) * 255.0) : bl[k] * 255.0;
                double v = std::min(std::max(sv + (sv - bv) * amount, 0.0), 255.0);
                if (q8) v = (double)(int)v;
                o[k] = (float)((std::fabs(v - bv) > thr8 ? v : sv) / 255.0);
            }
        }
    });
}

// Median / Min and Max: over a (square or round) neighbourhood of each pixel.
void neighbourhood(Ctx& c, bool median, bool maximum)
{
    const int W = c.out.w, H = c.out.h;
    const double hr = clampd(c.d("HorizontalRange"), 0, 1), vr = clampd(c.d("VerticalRange"), 0, 1);
    // VEGAS: the window reaches 16 * range pixels each way (square: the radius rounded; round: the pixels inside the radius plus half a pixel)
    const double rxf = c.px(hr * 16), ryf = c.px(vr * 16);
    const int RX = (int)std::lround(rxf), RY = (int)std::lround(ryf);
    const bool round = c.b("CircularAreaProcessing");
    const float rankFrac = median ? (float)(round ? clampd(c.d("Offset"), 0, 1) : 0.25 + 0.5 * clampd(c.d("Offset"), 0, 1)) : 0.5f;
    if (median) {
        // VEGAS's own (builtins_vegasmedian.cpp): the reach is the range * the larger side * 0.05 (float), weighted at the rim
        const float big = (float)std::max(W, H);
        vegasMedian(c.src, c.out, (double)((float)hr * big * 0.05f), (double)((float)vr * big * 0.05f), round, clampd(c.d("Offset"), 0, 1));
        return;
    }
    if (RX == 0 && RY == 0) { copyThrough(c); return; }
    // sliding windows / histograms; the plain loop below is for the frames the histograms cannot take
    if (!median) { extremeFilter(c.src, c.out, RX, RY, round, maximum, rxf + 0.5, ryf + 0.5); return; }
    if (medianFilter(c.src, c.out, RX, RY, round, rankFrac)) return;
    forRows(H, W, [&](int y) {
        std::vector<float> vals[3];
        for (int x = 0; x < W; ++x) {
            for (auto& v : vals) v.clear();
            float alpha = c.src.at(x, y)[3];
            float aMax = 0, aMin = 1;
            for (int j = -RY; j <= RY; ++j)
                for (int i = -RX; i <= RX; ++i) {
                    if (round) {
                        const double u = RX ? (double)i / RX : 0, v = RY ? (double)j / RY : 0;
                        if (u * u + v * v > (median ? 1.2 : 1.0625) + 1e-9) continue;
                    }
                    const int xx = std::min(W - 1, std::max(0, x + i)), yy = std::min(H - 1, std::max(0, y + j));
                    const float* p = c.src.at(xx, yy);
                    for (int k = 0; k < 3; ++k) vals[k].push_back(p[k]);
                    aMax = std::max(aMax, p[3]); aMin = std::min(aMin, p[3]);
                }
            float* o = c.out.at(x, y);
            for (int k = 0; k < 3; ++k) {
                auto& v = vals[k];
                if (v.empty()) { o[k] = c.src.at(x, y)[k]; continue; }
                if (median) { const size_t rk = (size_t)std::floor(rankFrac * (float)(v.size() - 1) + 0.5f); std::nth_element(v.begin(), v.begin() + (long)rk, v.end()); o[k] = v[rk]; }
                else o[k] = maximum ? *std::max_element(v.begin(), v.end()) : *std::min_element(v.begin(), v.end());
            }
            o[3] = median ? alpha : (maximum ? aMax : aMin);
            for (int k = 0; k < 3; ++k) o[k] = std::min(std::max(o[k], 0.f), o[3]);
        }
    });
}
void median(Ctx& c) { neighbourhood(c, true, false); }
void minAndMax(Ctx& c) { neighbourhood(c, false, c.i("Operation") == 1); }

// Chroma Blur: only the colour is blurred. VEGAS's: luma with weights (0.114, 0.587, 0.299) on (R, G, B) (the usual ones with red and blue swapped); the differences B - Y and R - Y
// are blurred across with a box of radius p + 1 and down with a triangle of half width 2.4 p + 1 (pixels, kept at the picture's edges); the result is cut to whole levels.
}  // namespace more
std::vector<float> chromaBlurKernel(bool triangle, double r)
{
    const int n = (int)std::ceil(r + (triangle ? 1.0 : 0.0));
    std::vector<float> k((size_t)(2 * n + 1), 0.f);
    double sum = 0;
    for (int i = -n; i <= n; ++i) {
        const double w = triangle ? std::max(0.0, 1.0 - std::fabs((double)i) / (r + 1.0)) : std::min(1.0, std::max(0.0, r + 1.0 - std::fabs((double)i)) );
        k[(size_t)(i + n)] = (float)w; sum += w;
    }
    for (auto& v : k) v = (float)(v / sum);
    return k;
}

namespace more {
static void convolvePlane(std::vector<float>& plane, int W, int H, const std::vector<float>& k, bool alongX)
{
    const int n = (int)k.size() / 2;
    std::vector<float> res(plane.size());
    forRows(H, W * (int)k.size(), [&](int y) {
        for (int x = 0; x < W; ++x) {
            double acc = 0;
            for (int i = -n; i <= n; ++i) {
                const int xx = alongX ? std::min(W - 1, std::max(0, x + i)) : x, yy = alongX ? y : std::min(H - 1, std::max(0, y + i));
                acc += (double)k[(size_t)(i + n)] * plane[(size_t)yy * W + xx];
            }
            res[(size_t)y * W + x] = (float)acc;
        }
    });
    plane.swap(res);
}

void chromaBlur(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const float wr = 0.114f, wg = 0.587f, wb = 0.299f;
    std::vector<float> y((size_t)W * H), cb((size_t)W * H), cr((size_t)W * H);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            float r, g, b, a;
            unpremul(c.src.at(i, j), r, g, b, a);
            const size_t k = (size_t)j * W + i;
            y[k] = wr * r + wg * g + wb * b;
            cb[k] = b - y[k]; cr[k] = r - y[k];
        }
    const std::vector<float> kx = chromaBlurKernel(false, c.px(clampd(c.d("HorizontalPixels"), 0, 10) + 1.0)),
                             ky = chromaBlurKernel(true, c.px(2.4 * clampd(c.d("VerticalPixels"), 0, 10) + 1.0));
    convolvePlane(cb, W, H, kx, true); convolvePlane(cr, W, H, kx, true);
    convolvePlane(cb, W, H, ky, false); convolvePlane(cr, W, H, ky, false);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const float a = c.src.at(i, j)[3];
            const size_t k = (size_t)j * W + i;
            const float r = y[k] + cr[k], b = y[k] + cb[k], g = (y[k] - wr * r - wb * b) / wg;
            if (c.levels) premul(c.out.at(i, j), floor8(r), floor8(g), floor8(b), a);
            else premul(c.out.at(i, j), clamp01(r), clamp01(g), clamp01(b), a);
        }
}

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
