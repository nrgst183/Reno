// Built-in blur effects (Quick Blur, Radial Blur, Defocus, Unsharp Mask, Median, Min and Max, Chroma Blur); see builtins_more.cpp.
#include <cstring>
#include "vegas8.h"
#include "builtins_more_internal.h"
#include "vkfilters.h"
#include "builtins_deriche.h"
#include "builtins_fast.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ blur
// Quick Blur, VEGAS's CPU processor: a 3 x 3 binomial blur (1 2 1 / 2 4 2 / 1 2 1 over 16, edges repeated; whole numbers, cut by >> 4 in 8-bit) of every channel, then a mix of the
// blurred picture towards the source with weight (1 - BlendAmount): blurred + (source - blurred) * w / 256 in 8-bit (w = the weight * 255 as a byte, the quotient cut towards 0).
// A blend amount within 1e-6 of 0 gives the source.
void quickBlur(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const float blend = (float)c.d("BlendAmount");
    if (W <= 0 || H <= 0) return;
    if (std::fabs(blend) <= 9.999999974752427e-07f) { copyThrough(c); return; }
    const float wf = (float)(1.0 - (double)blend);
    const int wb = v8::roundMagic((double)(wf * 255.f)) & 255;
    c.out.ensure(W, H);
    forRows(H, W, [&](int y) {
        const int ya = y > 0 ? y - 1 : y, yb = y < H - 1 ? y + 1 : y;
        for (int x = 0; x < W; ++x) {
            const int xa = x > 0 ? x - 1 : x, xb = x < W - 1 ? x + 1 : x;
            const float* p[9] = {c.src.at(xa, ya), c.src.at(x, ya), c.src.at(xb, ya), c.src.at(xa, y), c.src.at(x, y), c.src.at(xb, y), c.src.at(xa, yb), c.src.at(x, yb), c.src.at(xb, yb)};
            static const int wt[9] = {1, 2, 1, 2, 4, 2, 1, 2, 1};
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) {
                if (c.levels) {
                    int sum = 0;
                    for (int i = 0; i < 9; ++i) sum += wt[i] * v8::toLevel(p[i][k]);
                    const int bl = sum >> 4, sv = v8::toLevel(p[4][k]);
                    int t = (sv - bl) * wb;
                    t += (t < 0) ? 255 : 0;
                    o[k] = v8::fromLevel(bl + (t >> 8));
                } else {
                    float sum = 0.f;
                    for (int i = 0; i < 9; ++i) sum += (float)wt[i] * p[i][k];
                    const float bl = sum / 16.f;
                    o[k] = bl + (p[4][k] - bl) * wf;
                }
            }
        }
    });
}

// Radial Blur, VEGAS's CPU processor. In place in four quadrants, each from the centre pixel outwards (rows bottom first); every pixel blends with the already blurred picture at a point nearer the
// centre: Proportional a fraction 0.018 Strength^1.5 / (min(W, H) / 300) of the way in, Fixed radius (and Gaussian, treated the same) Strength^2 + 1 pixels in. The blend keeps (1 - w) of the pixel,
// w = (Strength^(300 / min(W, H)))^0.25, in 16-bit fixed point; the point is read bilinearly with 8-bit weights (missing right / upper neighbours read as opaque black). Two VEGAS quirks are kept:
// a sample within a pixel of the pixel itself reads the pixel's own source value in one corner of the 2x2, and the start of the two upper quadrants puts back the source pixel just above the centre.
void radialBlurSetup(Ctx& c, int W, int H, RbSetup& g)
{
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    g.type = c.i("Type");
    const double st = c.d("Strength");
    g.pass = W <= 0 || H <= 0 || !(st > 0);
    if (g.pass) return;
    g.q8 = c.levels;
    const double k = std::min(W, H) / 300.0;
    g.s0 = std::pow(st, 2.0) + 1.0; g.s1 = std::pow(st, 1.5) * 0.018 / k;
    const double wd = std::pow(std::pow(st, 1.0 / k), 0.25) * 65536.0;
    g.wi = (long long)wd; g.iwi = 65536 - g.wi;
    g.wf = wd / 65536.0; g.iwf = 1.0 - g.wf;
    int cx = (int)(W * pcx), cy = (int)(H * pcy);
    if (cx == W) cx = W - 1;
    if (cy == H) cy = H - 1;
    g.cx = std::max(0, std::min(W - 1, cx)); g.cy = std::max(0, std::min(H - 1, cy));
}

// The sample point of a pixel (VEGAS's): the same in the CPU effect, the dependency walk below and (in doubles, with the same operations) the GPU kernel.
static inline void rbPoint(const RbSetup& g, int x, int y, double& sx, double& sy)
{
    const int dX = g.cx - x, dY = g.cy - y;
    if (g.type != 0) {
        const double d = std::sqrt((double)(dX * dX + dY * dY));
        if (!(g.s0 < d)) { sx = g.cx; sy = g.cy; }
        else { const double f = g.s0 / d; sx = dX * f + x; sy = dY * f + y; }
    } else { sx = dX * g.s1 + x; sy = dY * g.s1 + y; }
}

// The order the in-place pass can run in on a GPU: every pixel (and the quadrants' restore of the centre pixel) gets the number of the step after everything it reads or overwrites,
// replaying the CPU's loops and its reads of the (already blurred) picture. Pixels of one step do not touch each other. rec: x | y << 14 | quadrant << 28 | restore << 30, by step;
// starts[k]: where step k + 1 begins. False when the frame is too big or there are more than 'maxSteps'.
bool radialBlurSteps(const RbSetup& g, int W, int H, std::vector<uint32_t>& rec, std::vector<uint32_t>& starts, int maxSteps)
{
    if (W > 16383 || H > 16383 || W <= 0 || H <= 0) return false;
    const size_t N = (size_t)W * H;
    std::vector<int32_t> wdp(N, 0), rdp(N, 0);
    std::vector<uint32_t> words;
    std::vector<int32_t> depth;
    words.reserve(N + 8); depth.reserve(N + 8);
    int maxd = 0;
    auto emit = [&](uint32_t word, size_t w, const size_t* rd, int nr) {
        int d = std::max(rdp[w], wdp[w]);
        for (int i = 0; i < nr; ++i) d = std::max(d, wdp[rd[i]]);
        d += 1;
        for (int i = 0; i < nr; ++i) rdp[rd[i]] = std::max(rdp[rd[i]], d);
        wdp[w] = d;
        words.push_back(word);
        depth.push_back(d);
        if (d > maxd) maxd = d;
    };
    for (int q = 0; q < 4; ++q) {
        if ((q == 1 || q == 3) && g.cx == W - 1) continue;
        if ((q == 2 || q == 3) && g.cy == H - 1) continue;
        const int xs = (q & 1) ? g.cx + 1 : g.cx, xstep = (q & 1) ? 1 : -1, nx = (q & 1) ? W - g.cx - 1 : g.cx + 1;
        const int ys = (q & 2) ? g.cy + 1 : g.cy, ystep = (q & 2) ? 1 : -1, ny = (q & 2) ? H - g.cy - 1 : g.cy + 1;
        {
            const int cr = (q & 2) ? g.cy + 1 : g.cy;
            if (cr < H) emit((uint32_t)g.cx | ((uint32_t)cr << 14) | (1u << 30), (size_t)cr * W + g.cx, nullptr, 0);
        }
        int y = ys;
        for (int j = 0; j < ny; ++j, y += ystep) {
            int x = xs;
            for (int i = 0; i < nx; ++i, x += xstep) {
                double sx, sy;
                rbPoint(g, x, y, sx, sy);
                const int ix = std::max(0, std::min(W - 1, (int)sx)), iy = std::max(0, std::min(H - 1, (int)sy));
                const double frx = sx - ix, fry = sy - iy;
                // which of the four taps read the blurred picture (-1: black or the source)
                long long tap[4];
                tap[0] = (long long)iy * W + ix;
                if (ix == W - 1 || iy == H - 1) {
                    tap[3] = -1;
                    tap[1] = ix == W - 1 ? -1 : (long long)iy * W + ix + 1;
                    tap[2] = iy == H - 1 ? -1 : (long long)(iy + 1) * W + ix;
                } else { tap[1] = (long long)iy * W + ix + 1; tap[2] = (long long)(iy + 1) * W + ix; tap[3] = (long long)(iy + 1) * W + ix + 1; }
                if (std::fabs(sx - x) < 1.0 && std::fabs(sy - y) < 1.0) {
                    if (frx == 0.0 && fry == 0.0) tap[0] = -1;
                    else tap[q] = -1;
                }
                size_t rd[4];
                int nr = 0;
                for (int k = 0; k < 4; ++k) if (tap[k] >= 0) rd[nr++] = (size_t)tap[k];
                emit((uint32_t)x | ((uint32_t)y << 14) | ((uint32_t)q << 28), (size_t)y * W + x, rd, nr);
                if (maxd > maxSteps) return false;
            }
        }
    }
    starts.assign((size_t)maxd + 1, 0);
    for (int32_t d : depth) ++starts[(size_t)d];       // (starts[d]: the count of step d, shifted below)
    // starts[d] -> where step d begins; step d runs from starts[d - 1] to starts[d]
    std::vector<uint32_t> begin((size_t)maxd + 2, 0);
    for (int d = 1; d <= maxd; ++d) begin[(size_t)d + 1] = begin[(size_t)d] + starts[(size_t)d];
    rec.assign(words.size(), 0);
    std::vector<uint32_t> fill(begin);
    for (size_t i = 0; i < words.size(); ++i) rec[fill[(size_t)depth[i]]++] = words[i];
    starts.assign(begin.begin() + 1, begin.end());      // step k + 1 begins at starts[k] (k = 0 .. maxd - 1), the last entry is the end
    return true;
}

void radialBlur(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    RbSetup g;
    radialBlurSetup(c, W, H, g);
    if (g.pass) { copyThrough(c); return; }
    const bool q8 = g.q8;
    // the picture bottom row first; 8-bit values (exact levels) or floats in 0..255
    std::vector<float> src((size_t)W * H * 4), dst;
    for (int y = 0; y < H; ++y) {
        const float* r = c.src.at(0, H - 1 - y);
        float* o = &src[(size_t)y * W * 4];
        for (int i = 0; i < W * 4; ++i) o[i] = q8 ? (float)roundNonNeg(std::min(std::max(r[i], 0.f), 1.f) * 255.f) : r[i] * 255.f;
    }
    dst = src;
    const long long wi = g.wi, iwi = g.iwi;
    const double wf = g.wf, iwf = g.iwf;
    const int cx = g.cx, cy = g.cy;
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
            int x = xs;
            for (int i = 0; i < nx; ++i, x += xstep) {
                double sx, sy;
                rbPoint(g, x, y, sx, sy);
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

// Defocus, as VEGAS's FilterLibrary.Defocus does it (builtins_defocus.cpp): the iris radius is Radius / 100 of the larger side of the frame (ApertureRadius, a fraction of the frame), the
// bloom threshold Bloom threshold / 100 and the strength Bloom strength; Iris shape 0 is a circle, 1 .. 4 a polygon of 5 .. 8 sides; Orientation is a fraction of a turn (assumed, as for
// Starburst); Disc curvature / 100 shapes the gradient of the iris (below 0 the rim is dimmer).
void defocus(Ctx& c)
{
    // (the slider stops at 11.2; a hand-edited setting is held to the frame's own size, or the iris table would need gigabytes)
    const double radius = std::min(100.0, std::max(0.0, c.d("Radius"))) / 100.0 * (double)std::max(c.out.w, c.out.h);
    if (radius < 0.5) { copyThrough(c); return; }
    const int shape = c.i("IrisShape");
    const double turn = c.d("Orientation") * 2 * kPi;
    const double curvature = clampd(c.d("DiscCurvature") / 100.0, -2.01, 1.0);
    const double thresh = clampd(c.d("BloomThreshold") / 100.0, 0, 2), strength = std::max(0.0, c.d("BloomStrength"));
    const int sides = shape == 0 ? 0 : shape + 4;
    defocusRender(c.src, c.out, radius, sides, turn, curvature, thresh, strength, c.b("ReduceFlicker"));
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

// Min and Max in 8 bits, as VEGAS's SSE code does it: every channel (premultiplied bytes B, G, R, A) of each pixel gets a weighted histogram of its window
// (weights 1 inside, the fractions of the radii on the rim; pixels outside the picture are left out); with W the weights' sum (added bin by bin
// over the first channel), the answer is the weighted mean of the values in the rank interval [lo, lo + 1] with lo = (W - 1) * Operation, cut to whole levels
// (Operation 0 = minimum, 1 = maximum: with a rim weight under one the lowest value is blended with the next).
struct MinMaxKernel {
    int nx = 0, ny = 0;            // quadrant size: the window reaches nx - 1 and ny - 1 pixels
    std::vector<float> q;          // [iy * nx + ix]
    float at(int ix, int iy) const { return q[(size_t)iy * nx + ix]; }
};
static MinMaxKernel minMaxRectKernel(float rx, float ry)
{
    MinMaxKernel k;
    const int cx = (int)rx + 1, cy = (int)ry + 1;
    k.nx = cx + 1;
    k.ny = cy + 1;
    k.q.assign((size_t)k.nx * k.ny, 0.f);
    double ipart;
    const double fx = std::modf((double)rx, &ipart), fy = std::modf((double)ry, &ipart);
    for (int y = 0; y < cy; ++y)
        for (int x = 0; x < cx; ++x) k.q[(size_t)y * k.nx + x] = 1.f;
    for (int x = 0; x < cx; ++x) k.q[(size_t)cy * k.nx + x] = (float)fy;
    for (int y = 0; y < cy; ++y) k.q[(size_t)y * k.nx + cx] = (float)fx;
    k.q[(size_t)cy * k.nx + cx] = (float)(fy * fx);
    return k;
}
static MinMaxKernel minMaxCircleKernel(float rxf, float ryf)
{
    // The window is the ellipse with semi-axes rx + 0.5, ry + 0.5; every cell of a quadrant holds the area of the ellipse inside it, from the closed form
    // of VEGAS (its operations in order, so that the same cases turn into NaN: those cells take their left neighbour, as in VEGAS).
    MinMaxKernel k;
    const double A = (double)rxf + 0.5, B = (double)ryf + 0.5;
    k.nx = (int)(A + 1.0) + 1;
    k.ny = (int)(B + 1.0) + 1;
    k.q.assign((size_t)k.nx * k.ny, 0.f);
    const double A2 = A * A, B2 = B * B;
    const double q = -(B2 / A2), twoQ = q + q, d4q = q * 4.0;
    const double t0 = (B2 * 4.0) * q;
    const double s = std::sqrt(-t0);
    const double c = (-1.0 / ((d4q / t0) + (d4q / t0))) / std::sqrt(-q);
    auto F = [&](double x, double& asinPart) {
        // (h * (2 q x)) / (4 q) + asin((2 q x) / s) * c, with h = sqrt(q x x + B2)
        const double h = std::sqrt((q * x) * x + B2);
        const double tw = twoQ * x;
        const double t = (h * tw) / d4q;
        asinPart = std::asin(tw / s) * c;
        return t + asinPart;
    };
    for (int j = 0; j < k.ny; ++j)
        for (int i = 0; i < k.nx; ++i) {
            double val;
            if (i == 0 && j == 0) { k.q[0] = 1.f; continue; }
            const double im = (double)i - 0.5, ip = (double)i + 0.5, jm = (double)j - 0.5, jp = (double)j + 0.5;
            const double h2p = -(((ip * ip) / A2 - 1.0) * B2);
            const double hp = h2p >= 0 ? std::sqrt(h2p) : -1.0;
            if (!(hp < jp)) { k.q[(size_t)j * k.nx + i] = 1.f; continue; }
            const double imc = 0.0 > im ? 0.0 : im, jmc = 0.0 > jm ? 0.0 : jm;
            const double h2l = -(((imc * imc) / A2 - 1.0) * B2);
            const double hl = h2l >= 0 ? std::sqrt(h2l) : -1.0;
            if (!(jmc < hl)) { k.q[(size_t)j * k.nx + i] = 0.f; continue; }
            bool esi = false, bot = false, top = false;
            double xbot = -1.0, xtop = -1.0;
            if (jm <= hp && hp <= jp && hp > 0) esi = true;
            {
                const double w2 = -(((jm * jm) / B2 - 1.0) * A2);
                const double xb = (w2 >= 0 && jm >= 0) ? std::sqrt(w2) : -1.0;
                if (xb >= im && ip >= xb && xb > 0) { bot = true; xbot = xb; }
                const double w2t = -(((jp * jp) / B2 - 1.0) * A2);
                const double xt = (w2t >= 0 && jp >= 0) ? std::sqrt(w2t) : -1.0;
                if (xt >= im && ip >= xt && xt > 0) { top = true; xtop = xt; }
            }
            double xU = ip, xL = im;
            if (i == 0) {
                xL = 0.0;
                if (!(B > jm)) val = 0.0;
                else {
                    if (bot) xU = xbot;
                    if (top) xL = xtop;
                    double a1, a2;
                    const double fu = F(xU, a1), fl = F(xL, a2);
                    val = (((fu - fl) - ((xU - xL) * jm)) + (xL + 0.0));
                    val += val;
                }
            } else if (j == 0) {
                if (!(A > im)) val = 0.0;
                else {
                    if (!esi) xU = A;
                    if (top) xL = xtop;
                    double a1, a2;
                    const double fu = F(xU, a1), fl = F(xL, a2);
                    val = (fu - fl) + (((xL - im) * 0.5) + 0.0);
                    val += val;
                }
            } else {
                if (bot) xU = xbot;
                if (top) xL = xtop;
                double a1, a2;
                const double fu = F(xU, a1), fl = F(xL, a2);
                val = ((fu - fl) - ((xU - xL) * jm)) + ((xL - im) + 0.0);
            }
            k.q[(size_t)j * k.nx + i] = (float)val;
        }
    for (size_t n = 0; n < k.q.size(); ++n) {
        if (std::isnan((double)k.q[n]) && n > 0) k.q[n] = k.q[n - 1];
        if ((double)k.q[n] < 1e-17) k.q[n] = 0.f;
    }
    return k;
}
// rank 0 / 1 = Min / Max (rows walked bottom first); Median passes its Offset and walks the rows top first.
static MinMaxKernel rank8Kernel(Ctx& c, int W, int H)
{
    auto clamp01 = [](double v) { return v < 0 ? 0.0 : std::min(1.0, v); };
    const float big = (float)std::max(W, H);
    const float rx0 = ((float)clamp01(c.d("HorizontalRange")) * big) * 0.05f;
    const float ry0 = ((float)clamp01(c.d("VerticalRange")) * big) * 0.05f;
    const float rx = rx0, ry = ry0;
    return c.b("CircularAreaProcessing") ? minMaxCircleKernel(rx, ry) : minMaxRectKernel(rx, ry);
}
// The window of the 8-bit rank filter for the Vulkan plan: the quadrant's weights (nx x ny floats)
void rank8GpuKernel(Ctx& c, int W, int H, std::vector<float>& q, int& nx, int& ny)
{
    const MinMaxKernel k = rank8Kernel(c, W, H);
    q = k.q; nx = k.nx; ny = k.ny;
}
// Does the exact 8-bit rank filter apply to this frame (else the plain code below runs)?
bool rank8Applies(int W, int H)
{
    const double reach = 0.05 * std::max(W, H) + 2.0;
    return (double)W * H * (2 * reach + 1) * (2 * reach + 1) < 6e9;
}
static void rankFilter8(Ctx& c, float rank, bool topFirst, bool colsDesc)
{
    const int W = c.out.w, H = c.out.h;
    const MinMaxKernel k = rank8Kernel(c, W, H);
    static const int kOrder[4] = {2, 1, 0, 3};   // byte 0..3 = B, G, R, A = Buf channels 2, 1, 0, 3
    forRows(H, W * k.nx * k.ny, [&](int y) {
        std::vector<float> hist(4 * 256);
        std::vector<unsigned char> px;
        for (int x = 0; x < W; ++x) {
            std::fill(hist.begin(), hist.end(), 0.f);
            for (int step = 0; step < 2 * k.ny - 1; ++step) {   // Min and Max walk the rows bottom first, Median top first
                const int dy = topFirst ? step - (k.ny - 1) : (k.ny - 1) - step;
                const int yy = y + dy;
                if (yy < 0 || yy >= H) continue;
                const int iy = dy < 0 ? -dy : dy;
                for (int dxs = 0; dxs < 2 * k.nx - 1; ++dxs) {
                    const int dx = colsDesc ? (k.nx - 1) - dxs : dxs - (k.nx - 1);
                    const int xx = x + dx;
                    if (xx < 0 || xx >= W) continue;
                    const float w = k.at(dx < 0 ? -dx : dx, iy);
                    if (w == 0.f) continue;
                    const float* p = c.src.at(xx, yy);
                    for (int b = 0; b < 4; ++b) hist[(size_t)b * 256 + (size_t)v8::toLevel(p[kOrder[b]])] += w;
                }
            }
            float total = 0.f;
            for (int i = 0; i < 256; ++i) total += hist[(size_t)i];
            const float t = ((total - 1.f) * rank) + 0.5f;
            const float lo = t - 0.5f, hi = 0.5f + t;
            float* o = c.out.at(x, y);
            for (int b = 0; b < 4; ++b) {
                float prev = 0.f, acc = 0.f;
                for (int v = 0; v < 256; ++v) {
                    const float w = hist[(size_t)b * 256 + (size_t)v];
                    if (!(w > 0.f)) continue;
                    const float cur = w + prev;
                    if (!(hi <= prev) && lo <= cur) {
                        float ov;
                        if (hi < cur) ov = lo <= prev ? hi - prev : 1.f;
                        else ov = lo <= prev ? w : cur - lo;
                        acc += ov * (float)v;
                    }
                    prev = cur;
                }
                int r = (int)acc;
                o[kOrder[b]] = v8::fromLevel(r < 0 ? 0 : (r > 255 ? 255 : r));
            }
        }
    });
}

static void minMax8(Ctx& c, bool maximum) { rankFilter8(c, maximum ? 1.f : 0.f, false, false); }

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
        // in 8 bits VEGAS's SSE float code: the Min and Max histogram with the rank (total - 1) * Offset (frames whose windows are too large for it use the integer code)
        const double reach = 0.05 * big + 2.0;
        if (c.levels && (double)W * H * (2 * reach + 1) * (2 * reach + 1) < 6e9) { rankFilter8(c, (float)clampd(c.d("Offset"), 0, 1), false, false); return; }
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
void minAndMax(Ctx& c)
{
    // the exact 8-bit path costs the window's area per pixel: very large windows on large frames use the plain filter below
    const double reach = 0.05 * std::max(c.out.w, c.out.h) + 2.0;
    if (c.levels && (double)c.out.w * c.out.h * (2 * reach + 1) * (2 * reach + 1) < 6e9) { minMax8(c, c.i("Operation") == 1); return; }
    neighbourhood(c, false, c.i("Operation") == 1);
}

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

// Chroma Blur in 8 bits, as VEGAS's code does it (float arithmetic on the premultiplied bytes B, G, R, A): Y = 0.299 b0 + 0.587 b1 + 0.114 b2, U = 0.436 b2 - (0.28886 b1 + 0.14713 b0),
// V = 0.615 b0 - 0.51499 b1 - 0.10001 b2; U and V go through three box blurs across and three down (box widths for a Gaussian of sigma = radius, running sums in float, edge pixels repeated),
// radius across = (int)(Horizontal + 1.5), down = (int)(Vertical + 1.5); back with 1.13983 / 0.39465, 0.5806 / 2.03211 and cut to whole levels.
static void boxesForGauss(float radius, int out[3])
{
    const float s = radius;
    float x7 = s * 12.0f;
    x7 = x7 * s;
    const int n = 3;
    int wl = (int)std::floor(std::sqrt(x7 / 3.0f + 1.0f));
    if (!(wl & 1)) wl -= 1;
    const float t1 = (float)(wl * wl * n);
    const float t2 = 12.0f * (float)wl;
    const float t3 = 9.0f;
    const float den = (float)wl * -4.0f - 4.0f;
    float m = ((x7 - t1) - t2) - t3;
    m = m / den;
    const int mi = (int)std::round(m);
    for (int i = 0; i < n; ++i) out[i] = (i < mi) ? wl : wl + 2;
}
static void boxPassH(const std::vector<float>& in, std::vector<float>& out, int W, int H, int r)
{
    const float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < H; ++y) {
        const float* a = &in[(size_t)y * W];
        float* o = &out[(size_t)y * W];
        const float fv = a[0], lv = a[W - 1];
        float val = (float)(r + 1) * fv;
        for (int j = 0; j < r; ++j) val += a[j];
        int ri = r, li = 0, ti = 0;
        for (int j = 0; j <= r; ++j) { val += a[ri++] - fv; o[ti++] = val * inv; }
        for (; ti < W - r; ++ti) { val += a[ri++] - a[li++]; o[ti] = val * inv; }
        for (; ti < W; ++ti) { val += lv - a[li++]; o[ti] = val * inv; }
    }
}
// VEGAS walks the picture bottom row first, so the running sums down the picture start at the bottom.
static void boxPassV(const std::vector<float>& in, std::vector<float>& out, int W, int H, int r)
{
    const float inv = 1.0f / (float)(2 * r + 1);
    auto at = [&](int x, int k) -> size_t { return (size_t)(H - 1 - k) * W + x; };
    for (int x = 0; x < W; ++x) {
        const float fv = in[at(x, 0)], lv = in[at(x, H - 1)];
        float val = (float)(r + 1) * fv;
        for (int j = 0; j < r; ++j) val += in[at(x, j)];
        int ri = r, li = 0, ti = 0;
        for (int j = 0; j <= r; ++j) { val += in[at(x, ri++)] - fv; out[at(x, ti++)] = val * inv; }
        for (; ti < H - r; ++ti) { val += in[at(x, ri++)] - in[at(x, li++)]; out[at(x, ti)] = val * inv; }
        for (; ti < H; ++ti) { val += lv - in[at(x, li++)]; out[at(x, ti)] = val * inv; }
    }
}
// the box widths of the three passes down, and the radius of the last pass across (shared with the Vulkan plan)
void chromaBlur8Widths(Ctx& c, int& rh, int bv[3])
{
    rh = (int)(c.px(clampd(c.d("HorizontalPixels"), 0, 10)) + 1.5);
    const int rv = (int)(c.px(clampd(c.d("VerticalPixels"), 0, 10)) + 1.5);
    boxesForGauss((float)rv, bv);
}
static void chromaBlur8(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const size_t N = (size_t)W * H;
    std::vector<float> Y(N), U(N), V(N), tmp(N);
    std::vector<unsigned char> alpha(N);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const float* p = c.src.at(i, j);
            const float b0 = (float)v8::toLevel(p[2]), b1 = (float)v8::toLevel(p[1]), b2 = (float)v8::toLevel(p[0]);
            const size_t k = (size_t)j * W + i;
            Y[k] = (b0 * 0.299f + b1 * 0.587f) + b2 * 0.114f;
            U[k] = b2 * 0.436f - (b1 * 0.28886f + b0 * 0.14713f);
            V[k] = (b0 * 0.615f - b1 * 0.51499f) - b2 * 0.10001f;
            alpha[k] = (unsigned char)v8::toLevel(p[3]);
        }
    // across: three passes of widths 1, 1, 2 rh + 1; down: three passes with the widths of a Gaussian of sigma rv
    int rh, bv[3];
    chromaBlur8Widths(c, rh, bv);
    const int bh[3] = {1, 1, 2 * rh + 1};
    for (std::vector<float>* pl : {&U, &V}) {
        for (int k = 0; k < 3; ++k) { boxPassH(*pl, tmp, W, H, (bh[k] - 1) / 2); pl->swap(tmp); }
        for (int k = 0; k < 3; ++k) { boxPassV(*pl, tmp, W, H, (bv[k] - 1) / 2); pl->swap(tmp); }
    }
    auto cut = [](float v) { return v8::fromLevel((int)(0.f > v ? 0.f : std::min(255.0f, v))); };
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const size_t k = (size_t)j * W + i;
            const float y = Y[k], u = U[k], v = V[k];
            const float v0 = (u * 0.0f + y) + v * 1.13983f;
            const float v1 = (y - u * 0.39465f) - v * 0.5806f;
            const float v2 = (u * 2.03211f + y) + v * 0.0f;
            float* o = c.out.at(i, j);
            o[2] = cut(v0);
            o[1] = cut(v1);
            o[0] = cut(v2);
            o[3] = v8::fromLevel(alpha[k]);
        }
}

void chromaBlur(Ctx& c)
{
    if (c.levels) { chromaBlur8(c); return; }
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
