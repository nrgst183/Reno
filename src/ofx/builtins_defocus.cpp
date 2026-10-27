// Defocus (FilterLibrary.Defocus of VEGAS's CoreGraphics), step for step as the DLL does it (worked out from its IL and its native FFTImage routines, and checked against VEGAS's
// pictures by tools/effectref_compare.py):
//   - the frame's 8-bit values (all four channels) are "bloomed": a value above the threshold th = Threshold x 255 becomes v + (v - th) x g, g = (Strength x 255 - th) / (255 - th)
//     (single precision, as the native row loader does it; only for a threshold below 1);
//   - the iris is a picture T x T of bytes: a disc (an ellipse of the VEGAS drawing code) or a regular polygon, filled with a radial gradient (the "Disc curvature"), 200 for a flat one;
//   - the frame is cut into tiles of T x T pixels that overlap by 'ov' (FFTImage.GetBestTileSize / IterateThroughTiles), each tile is convolved with the iris by FFT and divided by the
//     iris's sum, and the middle of each tile (T - ov pixels square) is taken, rounded to bytes;
//   - beyond the frame the native code takes the pixel of the last column for columns left of it and the first row for rows above or below it (what VEGAS's pictures show).
// The convolution is done in double precision (the native FFT is single precision): the result differs from VEGAS's only where a sum lies almost exactly between two bytes.
#include "builtins_fast.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {

namespace {

const double kPiD = 3.14159265358979323846;

// Where the iris's disc lies and how big it is, fitted to VEGAS's pictures: the drawing code (GDI+, no smoothing) fills the pixels whose centre is inside the shape, which comes out as a
// circle of the (rounded) radius less one pixel, a little smaller (the curve is flattened to a polygon), centred a pixel right of the tile's middle.
const double kCx = 0.98, kCy = -0.03;

int pow2GreaterThan(double v)
{
    for (int p = 2; p < 16; ++p)
        if ((double)(1 << p) >= v) return 1 << p;
    return 1 << 15;
}

double tileCost(int T, int ov, int W, int H)
{
    long n = 0;
    for (int y = -ov; y < H; y += T - ov)
        for (int x = -ov; x < W; x += T - ov) ++n;
    return (double)n * T * T / ((double)W * H);
}

// FFTImage.GetBestTileSize and the overlap of IterateThroughTiles for a radius in pixels.
void bestTile(double r, int W, int H, int& T, int& ov)
{
    const double d = r * 2;
    ov = (int)std::ceil(d + 16);
    ov += ov & 1;
    T = std::max(64, pow2GreaterThan(d + ov / 2.0 + 16));
    double c = tileCost(T, ov, W, H);
    int t = T;
    while (t < 1024) {
        const double c2 = tileCost(t * 2, ov, W, H);
        if (c2 < c) { t *= 2; c = c2; T = t; }
        else if (c2 == c) t *= 2;
        else break;
    }
}

// An in-place radix-2 FFT of n values with the given twiddles (w[k] = exp(-+2 pi i k / n), k < n / 2).
struct Twiddles {
    int n = 0;
    std::vector<std::complex<double>> w;
    explicit Twiddles(int N, bool inverse) : n(N), w((size_t)N / 2)
    {
        for (int k = 0; k < N / 2; ++k) {
            const double a = 2 * kPiD * k / N * (inverse ? 1 : -1);
            w[(size_t)k] = std::complex<double>(std::cos(a), std::sin(a));
        }
    }
};

void fft1(std::complex<double>* a, const Twiddles& tw)
{
    const int n = tw.n;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; ++k) {
                const std::complex<double> t = a[i + k + half] * tw.w[(size_t)k * step];
                a[i + k + half] = a[i + k] - t;
                a[i + k] += t;
            }
    }
}

// The 2-D transform of a T x T tile (rows, then columns).
void fft2(std::complex<double>* a, int T, const Twiddles& tw, std::vector<std::complex<double>>& col)
{
    for (int y = 0; y < T; ++y) fft1(a + (size_t)y * T, tw);
    col.resize((size_t)T * 8);
    for (int xb = 0; xb < T; xb += 8) {
        const int nb = std::min(8, T - xb);
        for (int y = 0; y < T; ++y)
            for (int k = 0; k < nb; ++k) col[(size_t)k * T + y] = a[(size_t)y * T + xb + k];
        for (int k = 0; k < nb; ++k) fft1(&col[(size_t)k * T], tw);
        for (int y = 0; y < T; ++y)
            for (int k = 0; k < nb; ++k) a[(size_t)y * T + xb + k] = col[(size_t)k * T + y];
    }
}

}  // namespace

DefocusIris defocusIris(double radiusPx, int sides, double turn, double curvature)
{
    DefocusIris k;
    const double rho = std::floor(radiusPx + 0.5) - 1.0;                   // the drawn shape is two pixels narrower than the rounded diameter
    if (!(rho >= 0.75)) { k.half = 0; k.w.assign(1, 200); return k; }      // (nothing left to draw: the frame is not blurred)
    const double R = sides >= 3 ? rho : rho - (rho < 14.0 ? 0.12 : 0.05);      // (smaller discs lose a little more: fitted to VEGAS's pictures of radius 10 and 17)
    k.half = (int)std::ceil(rho) + 1;
    const int n = 2 * k.half + 1;
    k.w.assign((size_t)n * n, 0);
    // the gradient: 255 stops at i / 254 of the curve (v0 + (1 - v0) t^3), each rounded to a byte; the middle of the iris is the last stop
    const double v0 = 1.0 + curvature * (curvature >= 0 ? 0.25 : 0.4);
    double stop[255];
    for (int i = 0; i < 255; ++i) {
        const double t = i / 254.0;
        stop[i] = std::min(255.0, std::max(0.0, std::nearbyint(200.0 * (v0 + (1.0 - v0) * t * t * t))));
    }
    const double a0 = -kPiD / 2 + turn, seg = sides >= 3 ? 2 * kPiD / sides : 0.0;
    for (int v = -k.half; v <= k.half; ++v)
        for (int u = -k.half; u <= k.half; ++u) {
            const double px = u + 0.5 - kCx, py = v + 0.5 - kCy, d = std::sqrt(px * px + py * py);
            double lim = R;
            if (sides >= 3) {
                double rel = std::fmod(std::atan2(py, px) - a0, seg);
                if (rel < 0) rel += seg;
                lim = R * std::cos(kPiD / sides) / std::cos(rel - kPiD / sides);
            }
            if (!(d <= lim)) continue;
            const double t = (1.0 - std::min(1.0, d / R)) * 254.0;
            const int i = std::min(253, (int)t);
            const double f = t - i;
            const double val = std::nearbyint(stop[i] + (stop[i + 1] - stop[i]) * f);
            k.w[(size_t)(v + k.half) * n + (size_t)(u + k.half)] = (int)std::min(255.0, std::max(0.0, val));
        }
    return k;
}

void defocusRender(const Buf& src, Buf& out, double radiusPx, int sides, double turn, double curvature, double thresh, double strength, bool reduceFlicker)
{
    const int W = src.w, H = src.h;
    out.ensure(W, H);
    if (W <= 0 || H <= 0) return;
    const DefocusIris iris = defocusIris(radiusPx, sides, turn, curvature);
    double total = 0;
    for (int w : iris.w) total += w;
    if (!(total > 0)) { out.px = src.px; return; }

    // the frame's bytes (blurred by Blur3x3 for Reduce flicker: the 3 x 3 weights 1 2 1 / 2 4 2 / 1 2 1 over 16, truncated, edges carried on), with the bloom
    std::vector<uint8_t> by((size_t)W * H * 4);
    parallelRows(H, (size_t)W * 8, [&](int y0, int y1) {
        for (int i = y0 * W * 4; i < y1 * W * 4; ++i) by[(size_t)i] = (uint8_t)std::floor(std::min(1.f, std::max(0.f, src.px[(size_t)i])) * 255.f + 0.5f);
    });
    if (reduceFlicker) {
        std::vector<uint8_t> bb(by.size());
        parallelRows(H, (size_t)W * 40, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int ya = std::max(0, y - 1), yb = std::min(H - 1, y + 1);
                for (int x = 0; x < W; ++x) {
                    const int xa = std::max(0, x - 1), xb = std::min(W - 1, x + 1);
                    for (int k = 0; k < 4; ++k) {
                        const int s = by[((size_t)ya * W + xa) * 4 + k] + 2 * by[((size_t)ya * W + x) * 4 + k] + by[((size_t)ya * W + xb) * 4 + k] +
                                      2 * (by[((size_t)y * W + xa) * 4 + k] + 2 * by[((size_t)y * W + x) * 4 + k] + by[((size_t)y * W + xb) * 4 + k]) +
                                      by[((size_t)yb * W + xa) * 4 + k] + 2 * by[((size_t)yb * W + x) * 4 + k] + by[((size_t)yb * W + xb) * 4 + k];
                        bb[((size_t)y * W + x) * 4 + k] = (uint8_t)(s >> 4);
                    }
                }
            }
        });
        by.swap(bb);
    }
    std::vector<float> bl((size_t)W * H * 4);
    const bool bloom = thresh < 1.0;
    const float th = (float)(thresh * 255.0);
    const float gain = bloom ? (float)((strength * 255.0 - (double)th) / (double)(255.0f - th)) : 0.f;
    parallelRows(H, (size_t)W * 8, [&](int y0, int y1) {
        for (int i = y0 * W * 4; i < y1 * W * 4; ++i) {
            float v = (float)by[(size_t)i];
            if (bloom && v > th) v = v + (v - th) * gain;
            bl[(size_t)i] = v;
        }
    });

    int T, ov;
    bestTile(radiusPx, W, H, T, ov);
    // tiles must be big enough for the iris: (a safeguard; the tile size follows the radius)
    while (T - ov < 8 || ov / 2 < iris.half + 1) { ov += 2; if (T - ov < 16) T *= 2; }
    const int step = T - ov;
    std::vector<int> xs, ys;
    for (int x = -ov; x < W; x += step) xs.push_back(x);
    for (int y = -ov; y < H; y += step) ys.push_back(y);
    const Twiddles fw(T, false), inv(T, true);

    // the iris's transform: its offset (u, v) sits at the circular position (u, v) mod T, so that the product is the convolution out(x) = sum in(x - u) w(u)
    std::vector<std::complex<double>> kf((size_t)T * T);
    {
        const int n = 2 * iris.half + 1;
        for (int v = -iris.half; v <= iris.half; ++v)
            for (int u = -iris.half; u <= iris.half; ++u)
                kf[(size_t)((v + T) % T) * T + (size_t)((u + T) % T)] = (double)iris.w[(size_t)(v + iris.half) * n + (size_t)(u + iris.half)];
        std::vector<std::complex<double>> col;
        fft2(kf.data(), T, fw, col);
    }
    const double scale = 1.0 / ((double)T * T * total);

    const int nt = (int)(xs.size() * ys.size());
    parallelRows(nt, (size_t)T * T * 60, [&](int t0, int t1) {
        std::vector<std::complex<double>> pl[2];
        pl[0].resize((size_t)T * T);
        pl[1].resize((size_t)T * T);
        std::vector<std::complex<double>> col;
        for (int ti = t0; ti < t1; ++ti) {
            const int x0 = xs[(size_t)ti % xs.size()], y0 = ys[(size_t)ti / xs.size()];
            for (int y = 0; y < T; ++y) {
                const int yy = y0 + y;
                const int sy = (yy < 0 || yy >= H) ? 0 : yy;
                for (int x = 0; x < T; ++x) {
                    const int xx = x0 + x;
                    const int sx = (xx < 0 || xx >= W) ? W - 1 : xx;
                    const float* p = &bl[((size_t)sy * W + (size_t)sx) * 4];
                    pl[0][(size_t)y * T + x] = std::complex<double>(p[0], p[1]);
                    pl[1][(size_t)y * T + x] = std::complex<double>(p[2], p[3]);
                }
            }
            for (int q = 0; q < 2; ++q) {
                fft2(pl[q].data(), T, fw, col);
                for (size_t i = 0; i < (size_t)T * T; ++i) pl[q][i] *= kf[i];
                fft2(pl[q].data(), T, inv, col);
            }
            const int xa = std::max(0, x0 + ov / 2), xb = std::min(W, x0 + T - ov / 2);
            const int ya = std::max(0, y0 + ov / 2), yb = std::min(H, y0 + T - ov / 2);
            for (int y = ya; y < yb; ++y)
                for (int x = xa; x < xb; ++x) {
                    const size_t i = (size_t)(y - y0) * T + (size_t)(x - x0);
                    const double v[4] = {pl[0][i].real(), pl[0][i].imag(), pl[1][i].real(), pl[1][i].imag()};
                    float* o = &out.px[((size_t)y * W + (size_t)x) * 4];
                    for (int k = 0; k < 4; ++k) o[k] = (float)std::floor(std::min(255.0, std::max(0.0, v[k] * scale)) + 0.5) * (1.f / 255.f);
                }
        }
    });
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float* o = &out.px[i * 4];
        for (int k = 0; k < 3; ++k) o[k] = std::min(o[k], o[3]);
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
