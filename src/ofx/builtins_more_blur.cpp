// Built-in blur effects (Quick Blur, Radial Blur, Defocus, Unsharp Mask, Median, Min and Max, Chroma Blur); see builtins_more.cpp.
#include <cstring>
#include "vegas8.h"
#include "builtins_more_internal.h"
#include "vkfilters.h"
#include "builtins_deriche.h"
#include "vkblur.h"
#include "vfxgauss.h"
#include "vfxblurs.h"
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

// Radial Blur as VEGAS's OpenCL kernels draw it (GPU acceleration on, read from the decoded kernels): the picture is unwrapped to polar form (columns = angle, rows = radius, 2.828427125 = 2 sqrt 2 frame
// radii; the Center sets where the radius starts), then every column is blurred along the radius and the result wrapped back. Proportional / Fixed radius run outwards as a one-pole recursion
// (row y = row(y) + (row(y - back) - row(y)) * w, with back = y - round(y * s1) for Proportional and y - round(s0) for Fixed, s0 = Strength^2 + 1, s1 = 0.018 Strength^1.5 / (H / 300),
// w = (Strength^(300 / H))^0.25). Type Gaussian runs the recursive Gaussian of vfxgauss.h along the radius (sigma = (float)Strength x 20 polar rows: a causal and an anti-causal pass into two
// pictures, then their sum). The taps are read as the card's texture unit does (oclLinear4: weights cut to 1/256, pixel coordinates without the half). In an 8-bit project every picture the kernels
// write (the polar one, the two Gaussian halves and their sum, the recursion's result, the result) is an 8-bit texture with the card's 12-bit store (oclStore12: all 13 captured Radial Blur
// pictures within one level, 99.9% exact; Gaussian 99.8%).
void radialBlurOclParams(Ctx& c, int H, RbOclParams& r)
{
    const double Hd = H;
    const double st = c.d("Strength"), k = Hd / 300.0;
    r.s0 = (float)(std::pow(st, 2.0) + 1.0);
    r.s1 = (float)(std::pow(st, 1.5) * 0.018 / k);
    r.w = (float)std::pow(std::pow(st, 1.0 / k), 0.25);
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    r.v56x = (float)(2 * pcx - 1);
    r.v56y = (float)(2 * pcy - 1);
    r.sigma = (float)st * 20.0f;       // (Gaussian: a float multiply, then the builder's double)
    r.cf = vfxgauss::toFloats(vfxgauss::build((double)r.sigma));
}
static void radialBlurOpenCl(Ctx& c, const RbSetup& g, int W, int H)
{
    const bool q = c.host8;
    const float pi = 3.14159274f, two = 2.828427125f;
    RbOclParams rp;
    radialBlurOclParams(c, H, rp);
    const float s0 = rp.s0, s1 = rp.s1, w = rp.w, v56x = rp.v56x, v56y = rp.v56y;
    std::vector<float> src((size_t)W * H * 4), P((size_t)W * H * 4), out((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) std::memcpy(&src[(size_t)y * W * 4], c.src.at(0, H - 1 - y), (size_t)W * 4 * sizeof(float));
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float xn = (float)x / (float)W, yn = (float)y / (float)H;
            const float a = (xn / 0.5f - 1.f) * pi, r = yn * two;
            float px = r * std::sin(a) + v56x, py = r * std::cos(a) + v56y;
            px = (px / 2.f + 0.5f) * (float)W; py = (py / 2.f + 0.5f) * (float)H;
            float* o = &P[((size_t)y * W + x) * 4];
            oclLinear4(src, W, H, px, py, o);
            if (q) for (int ch = 0; ch < 4; ++ch) o[ch] = oclStore12(o[ch]);
        }
    });
    std::vector<float> buf;
    if (g.type == 2) {
        // Gaussian: the five coefficient sets as floats, sigma (float)Strength x 20 (a float multiply, then the builder's double)
        const vfxgauss::Coefsf cf = rp.cf;
        std::vector<float> A, B;
        vfxgauss::passColumns(cf, P, W, H, A, B, [](float a, float b, float cc) { return a * b + cc; });
        if (q) { for (float& v : A) v = oclStore12(v); for (float& v : B) v = oclStore12(v); }
        buf.resize(A.size());
        for (size_t i = 0; i < buf.size(); ++i) { const float sum = A[i] + B[i]; buf[i] = q ? oclStore12(sum) : sum; }
    } else {
        buf = P;
        for (int y = 0; y < H; ++y) {
            int back;
            if (g.type == 1) back = y <= s0 ? 0 : y - (int)std::nearbyint((double)((float)y * (s0 / (float)y)));
            else back = y - (int)std::nearbyint((double)((float)y * s1));
            back = std::max(0, std::min(y, back));
            const float* p = &P[(size_t)y * W * 4];
            const float* bk = &buf[(size_t)back * W * 4];
            float* o = &buf[(size_t)y * W * 4];
            for (int i = 0; i < W * 4; ++i) o[i] = p[i] + (bk[i] - p[i]) * w;
        }
        if (q) for (float& v : buf) v = oclStore12(v);
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float xn = (float)x / (float)W, yn = (float)y / (float)H;
            const float vx = (xn / 0.5f - 1.f) - v56x, vy = (yn / 0.5f - 1.f) - v56y;
            const float a2 = std::atan2(vx, vy), r2 = std::sqrt(vx * vx + vy * vy);
            const float u = ((a2 / pi) / 2.f + 0.5f) * (float)W, vv = (r2 / two) * (float)H;
            float* o = &out[((size_t)y * W + x) * 4];
            oclLinear4(buf, W, H, u, vv, o);
            if (q) for (int ch = 0; ch < 4; ++ch) o[ch] = oclStore12(o[ch]);
        }
    });
    c.out.ensure(W, H);
    for (int y = 0; y < H; ++y) std::memcpy(c.out.at(0, H - 1 - y), &out[(size_t)y * W * 4], (size_t)W * 4 * sizeof(float));
}

void radialBlur(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    RbSetup g;
    radialBlurSetup(c, W, H, g);
    if (g.pass) { copyThrough(c); return; }
    if (c.openCl) { radialBlurOpenCl(c, g, W, H); return; }
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
// bloom threshold Bloom threshold / 100 and the strength Bloom strength; Iris shape 0 is a circle, 1 .. 4 a polygon of 5 .. 8 sides; Orientation is in degrees (as for
// Starburst, which VEGAS stores the same way); Disc curvature / 100 shapes the gradient of the iris (below 0 the rim is dimmer).
void defocus(Ctx& c)
{
    // (the slider stops at 11.2; a hand-edited setting is held to the frame's own size, or the iris table would need gigabytes)
    const double pct = std::min(100.0, std::max(0.0, c.d("Radius")));
    const double radius = pct / 100.0 * (double)std::max(c.out.w, c.out.h);
    const int shape = std::max(0, std::min(4, c.i("IrisShape")));
    const double orient = c.d("Orientation");                                  // degrees
    const double curvature = clampd(c.d("DiscCurvature") / 100.0, -1.0, 1.0);
    const double thresh = clampd(c.d("BloomThreshold") / 100.0, 0, 2), strength = std::max(0.0, c.d("BloomStrength"));
    if (c.openCl && c.out.w > 0 && c.out.h > 0 && c.src.px.size() == c.out.px.size()) {
        // VEGAS's OpenCL path (GPU acceleration on): the radius in pixels is max(width x pixel aspect, height) x Radius / 100 (pixel aspect 1 here)
        defocusRenderGpu(c.src, c.out, radius, shape, orient, curvature, thresh, strength, c.b("ReduceFlicker"), c.host8);
        return;
    }
    if (radius < 0.5) { copyThrough(c); return; }
    const double turn = orient * kPi / 180.0;
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
    if (c.openCl && W > 0 && H > 0 && c.src.px.size() == (size_t)W * H * 4) {
        // VEGAS's OpenCL path (GPU acceleration on): the recursive Gaussian of vfxblurs.h with sigma from larger side x Radius + 1.5, then one kernel on every channel (alpha too):
        // v = s + Amount (s - blur), taken where |v - blur| > Threshold (floats, no rounding of the amounts), else the source; 8-bit frames get the 12-bit store. Captured: 25 pictures,
        // 99.5% of the values exact (the rest are ties of the threshold test at Radius 1); the older model was 0.16 levels off on average and 70 at worst.
        std::vector<float> in, out;
        oclRowsOf(c.src, in);
        vfxblurs::unsharpMask(in, W, H, oclSetting(c.d("Radius"), 0, 1), (float)oclSetting(c.d("Amount"), -1e6, 1e6), (float)oclSetting(c.d("Threshold"), -1e6, 1e6),
                              c.host8 ? oclStore12 : nullptr, out);
        oclRowsTo(out, c.out, W, H);
        return;
    }
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
    // the window's taps in the order the weights are added (Min and Max walk the rows bottom first, Median top first), those with weight 0 left out
    struct Tap { int dx, dy; float w; };
    std::vector<Tap> taps;
    for (int step = 0; step < 2 * k.ny - 1; ++step) {
        const int dy = topFirst ? step - (k.ny - 1) : (k.ny - 1) - step;
        const int iy = dy < 0 ? -dy : dy;
        for (int dxs = 0; dxs < 2 * k.nx - 1; ++dxs) {
            const int dx = colsDesc ? (k.nx - 1) - dxs : dxs - (k.nx - 1);
            const float w = k.at(dx < 0 ? -dx : dx, iy);
            if (w == 0.f) continue;
            taps.push_back(Tap{dx, dy, w});
        }
    }
    // the picture as levels once (B, G, R, A per pixel), not once per tap
    std::vector<unsigned char> lv((size_t)W * H * 4);
    forRows(H, W * 4, [&](int y) {
        const float* p = c.src.at(0, y);
        unsigned char* d = &lv[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, p += 4, d += 4)
            for (int b = 0; b < 4; ++b) d[b] = (unsigned char)v8::toLevel(p[kOrder[b]]);
    });
    forRows(H, W * (int)std::min<size_t>(taps.size() + 64, 4096), [&](int y) {
        std::vector<float> hist(4 * 256, 0.f);       // (all zero between pixels: only the levels a pixel touched are cleared again)
        for (int x = 0; x < W; ++x) {
            int lo8[4] = {255, 255, 255, 255}, hi8[4] = {0, 0, 0, 0};
            for (const Tap& t : taps) {
                const int xx = x + t.dx, yy = y + t.dy;
                if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
                const unsigned char* q = &lv[((size_t)yy * W + xx) * 4];
                for (int b = 0; b < 4; ++b) {
                    const int v = q[b];
                    hist[(size_t)b * 256 + (size_t)v] += t.w;
                    if (v < lo8[b]) lo8[b] = v;
                    if (v > hi8[b]) hi8[b] = v;
                }
            }
            // (the levels outside the touched range hold 0, which adds nothing to the total and is skipped below)
            float total = 0.f;
            for (int i = lo8[0]; i <= hi8[0]; ++i) total += hist[(size_t)i];
            const float t = ((total - 1.f) * rank) + 0.5f;
            const float lo = t - 0.5f, hi = 0.5f + t;
            float* o = c.out.at(x, y);
            for (int b = 0; b < 4; ++b) {
                float prev = 0.f, acc = 0.f;
                for (int v = lo8[b]; v <= hi8[b]; ++v) {
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
                if (lo8[b] <= hi8[b]) std::fill(hist.begin() + (size_t)b * 256 + lo8[b], hist.begin() + (size_t)b * 256 + hi8[b] + 1, 0.f);
            }
        }
    });
}
static void minMax8(Ctx& c, bool maximum) { rankFilter8(c, maximum ? 1.f : 0.f, false, false); }

// Median and Min and Max as VEGAS's OpenCL kernels draw them (one pass per channel, R G B A alike; Min and Max read their rank, 0 or 1, as an interval of one unit of weight, 'interval'). The window is the weighted quadrant kernel of the 8-bit code above, its weights turned into whole
// numbers ((int)(weight * 20000)); a pixel's histogram holds those weights at the level (round to nearest of value * 255) of every tap inside the picture, and the answer is the level
// where the running total reaches (int)(total * rank) (rank = Offset kept in 0.001 .. 0.999; below one half the level whose total goes past it, from one half the first that reaches it).
// Along a row the histogram is not rebuilt: the kernel starts with the whole window at the first pixel and then adds the column of weights entering and takes away the column leaving, with
// the differences between neighbouring weights (also cut to whole numbers), so the same steps are made here (their roundings stay in the sums). Taps outside the picture count 0.
// All of it runs on a picture half the size (every second pixel kept, the window's reach halved, kOclReachScale), which is read back to full size with the bicubic read (VEGAS's kernels do the
// scaling both ways before and after).
struct MedianEnt { int dx, dy, z; };
// The window of the OpenCL Median / Min and Max as whole-number weights: every tap of the full window (for the first pixel of a row) and the column of differences added and taken away from one pixel to the next
static void medianOclEntries(const MinMaxKernel& k, std::vector<MedianEnt>& full, std::vector<MedianEnt>& edge)
{
    for (int dy = 1 - k.ny; dy < k.ny; ++dy)
        for (int dx = 1 - k.nx; dx < k.nx; ++dx) {
            const float w = k.at(std::abs(dx), std::abs(dy));
            if ((double)w > 1e-17) full.push_back(MedianEnt{dx, dy, (int)(w * 20000.f)});
        }
    for (int dy = 0; dy < k.ny; ++dy)
        for (int dx = 0; dx < k.nx; ++dx) {
            const float w = dx + 1 < k.nx ? k.at(dx, dy) - k.at(dx + 1, dy) : k.at(dx, dy);
            if ((double)w > 1e-17) edge.push_back(MedianEnt{dx, dy, (int)(w * 20000.f)});
        }
}
static void medianOpenCl(Ctx& c, float rank, float rxf, float ryf, bool circle, bool interval)
{
    const int W = c.out.w, H = c.out.h;
    const int Wh = W / 2, Hh = H / 2;
    c.out.ensure(W, H);
    if (Wh < 1 || Hh < 1) { copyThrough(c); return; }
    const MinMaxKernel k = circle ? minMaxCircleKernel(rxf, ryf) : minMaxRectKernel(rxf, ryf);
    std::vector<MedianEnt> full, edge;
    medianOclEntries(k, full, edge);
    typedef MedianEnt Ent;
    // the half-size picture (every second pixel, counted from the bottom row), as levels (R, G, B, A)
    std::vector<unsigned char> lv((size_t)Wh * Hh * 4);
    for (int y = 0; y < Hh; ++y) {
        const float* row = c.src.at(0, H - 1 - 2 * y);
        unsigned char* d = &lv[(size_t)y * Wh * 4];
        for (int x = 0; x < Wh; ++x)
            for (int ch = 0; ch < 4; ++ch) d[x * 4 + ch] = (unsigned char)std::nearbyint(std::min(std::max(row[2 * x * 4 + ch] * 255.f, 0.f), 255.f));
    }
    std::vector<float> half((size_t)Wh * Hh * 4, 0.f);
    forRows(Hh, Wh * (int)std::min<size_t>(edge.size() + 64, 4096), [&](int y) {
        for (int ch = 0; ch < 4; ++ch) {
            std::vector<int> hist(256, 0);
            auto tap = [&](int xx, int yy, int z, int sign) {
                if (xx < 0 || xx >= Wh || yy < 0 || yy >= Hh) return;
                hist[lv[((size_t)yy * Wh + xx) * 4 + ch]] += sign * z;
            };
            for (const Ent& e : full) tap(e.dx, y + e.dy, e.z, 1);
            for (int x = 0; x < Wh; ++x) {
                if (x > 0)
                    for (const Ent& e : edge) {
                        tap(x + e.dx, y + e.dy, e.z, 1);
                        tap(x - e.dx - 1, y + e.dy, e.z, -1);
                        if (e.dy != 0) {
                            tap(x - e.dx - 1, y - e.dy, e.z, -1);
                            tap(x + e.dx, y - e.dy, e.z, 1);
                        }
                    }
                int cum[256], run = 0;
                for (int b = 0; b < 256; ++b) { run += hist[b]; cum[b] = run; }
                int pick = 0;
                if (!interval) {
                    const int t = (int)((float)cum[255] * rank);
                    for (int b = 0; b < 256; ++b) {
                        const bool hit = rank < 0.5f ? (t < cum[b] && cum[b] - hist[b] <= t) : (t <= cum[b] && cum[b] - hist[b] < t);
                        if (hit) { pick = b; break; }
                    }
                } else {
                    // Min and Max: the mean level of the one unit of weight that starts at rank * (total - 1) (the weights taken as pixels: / 20000), summed as the kernel's 64 lanes do
                    const float sc = 20000.f;
                    const float tot = (float)cum[255] / sc;
                    const float mid = rank * (tot - 1.f) + 0.5f, hi = mid + 0.5f, lo = mid - 0.5f;
                    float lane[64];
                    for (int l = 0; l < 64; ++l) {
                        float acc = 0.f;
                        for (int j = 0; j < 4; ++j) {
                            const int b = l * 4 + j;
                            const float prev = b > 0 ? (float)cum[b - 1] / sc : 0.f, cur = (float)cum[b] / sc, h = (float)hist[b] / sc;
                            float term = 0.f;
                            if (prev < hi && cur >= lo) {
                                if (prev >= lo) term = cur > hi ? (hi - prev) * (float)b : h * (float)b;
                                else term = cur > hi ? (float)b : (cur - lo) * (float)b;
                            }
                            acc = j == 0 ? term : acc + term;
                        }
                        lane[l] = acc;
                    }
                    float grp[16], quad[4];
                    for (int g = 0; g < 16; ++g) grp[g] = (lane[4 * g + 2] + lane[4 * g + 3]) + (lane[4 * g] + lane[4 * g + 1]);
                    for (int g = 0; g < 4; ++g) quad[g] = (grp[4 * g + 2] + grp[4 * g + 3]) + (grp[4 * g] + grp[4 * g + 1]);
                    pick = (int)((quad[2] + quad[3]) + (quad[0] + quad[1]));
                    pick = std::max(0, std::min(255, pick));
                }
                half[((size_t)y * Wh + x) * 4 + ch] = (float)pick / 255.f;
            }
        }
    });
    // back to full size: the bicubic read at half the pixel coordinates, written as 8-bit levels
    forRows(H, W, [&](int yt) {
        const int y = H - 1 - yt;
        float* o = c.out.at(0, yt);
        for (int x = 0; x < W; ++x) {
            float v[4];
            oclBicubic4(half, Wh, Hh, (float)x / 2.f, (float)y / 2.f, v);
            for (int ch = 0; ch < 4; ++ch) o[x * 4 + ch] = oclWriteLevel(v[ch]);
        }
    });
}
static const float kOclReachScale = 0.5f;
static bool medianOpenClApplies(Ctx& c, float& rx, float& ry)
{
    auto clamp01 = [](double v) { return v < 0 ? 0.0 : std::min(1.0, v); };
    const float big = (float)std::max(c.out.w, c.out.h);
    rx = (float)(clamp01(c.d("HorizontalRange")) * kOclReachScale * big * 0.05f);
    ry = (float)(clamp01(c.d("VerticalRange")) * kOclReachScale * big * 0.05f);
    return c.openCl && c.out.w > 0 && c.out.h > 0;
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
// What the Vulkan plan of the OpenCL Median / Min and Max needs: the weights (table: the full window's taps then the edge's, three numbers each: dx, dy, weight), the half-size picture's
// size and the rank (see medianOpenCl). False when the frame does not take the OpenCL path (or is too small for the half-size picture).
bool medianOclGpuSetup(Ctx& c, bool minMax, int W, int H, MedianOclGpu& g)
{
    if (!c.openCl || W <= 0 || H <= 0) return false;
    auto clamp01 = [](double v) { return v < 0 ? 0.0 : std::min(1.0, v); };
    const float big = (float)std::max(W, H);
    const float rx = (float)(clamp01(c.d("HorizontalRange")) * kOclReachScale * big * 0.05f);
    const float ry = (float)(clamp01(c.d("VerticalRange")) * kOclReachScale * big * 0.05f);
    g.wh = W / 2;
    g.hh = H / 2;
    if (g.wh < 1 || g.hh < 1) return false;
    g.interval = minMax;
    g.rank = minMax ? (c.i("Operation") != 0 ? 1.f : 0.f) : (float)clampd(c.d("Offset"), 0.001, 0.999);
    const bool circle = c.b("CircularAreaProcessing");
    const MinMaxKernel k = circle ? minMaxCircleKernel(rx, ry) : minMaxRectKernel(rx, ry);
    std::vector<MedianEnt> full, edge;
    medianOclEntries(k, full, edge);
    g.nfull = (int)full.size();
    g.nedge = (int)edge.size();
    g.tab.clear();
    for (const MedianEnt& e : full) { g.tab.push_back((float)e.dx); g.tab.push_back((float)e.dy); g.tab.push_back((float)e.z); }
    for (const MedianEnt& e : edge) { g.tab.push_back((float)e.dx); g.tab.push_back((float)e.dy); g.tab.push_back((float)e.z); }
    return true;
}
void median(Ctx& c)
{
    float rx, ry;
    if (medianOpenClApplies(c, rx, ry)) {
        medianOpenCl(c, (float)clampd(c.d("Offset"), 0.001, 0.999), rx, ry, c.b("CircularAreaProcessing"), false);
        return;
    }
    neighbourhood(c, true, false);
}
void minAndMax(Ctx& c)
{
    float rx, ry;
    if (medianOpenClApplies(c, rx, ry)) {
        medianOpenCl(c, c.i("Operation") != 0 ? 1.f : 0.f, rx, ry, c.b("CircularAreaProcessing"), true);
        return;
    }
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

// Chroma Blur in 8 bits, as VEGAS draws it in a project (fitted to its own pictures: Reno Effect Capture, 16 cases on two inputs, mean 0.4 to 0.7 levels off; the picture the
// stand-alone plug-in gives when another host runs it is a different one, so this is NOT VEGAS's decoded code): luma Y = 0.299 R + 0.587 G + 0.114 B is kept; the colour differences
// Cb = (B - Y) / 1.772 and Cr = (R - Y) / 1.402 are blurred with a true Gaussian of sigma 0.44 + 0.3 p pixels (p the Horizontal / Vertical pixels setting, so even 0 blurs a little),
// edge pixels repeated; Cr is held to +-0.406 before the blur and the result is rounded to whole levels. The Cr limit is explained: the plug-in's analog V (0.615 R - 0.515 G - 0.1 B) held
// to a signed byte (+-127 levels) is Cr +-0.405, and it gives VEGAS's pure red (221, 17, 0); the Gaussian itself is only fitted (the decoded 3-box code cannot give sigma 0.46 at 0 pixels,
// so VEGAS's host runs another path of the plug-in than Reno's host does; which one is not known).
static void gaussTaps(double sigma, std::vector<float>& k)
{
    const int n = std::max(1, (int)std::ceil(4.5 * sigma));
    k.assign((size_t)(2 * n + 1), 0.f);
    double sum = 0;
    for (int i = -n; i <= n; ++i) sum += std::exp(-(double)i * i / (2.0 * sigma * sigma));
    for (int i = -n; i <= n; ++i) k[(size_t)(i + n)] = (float)(std::exp(-(double)i * i / (2.0 * sigma * sigma)) / sum);
}
static void chromaBlur8(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const size_t N = (size_t)W * H;
    const double kr = 0.299, kb = 0.114, kg = 1.0 - kr - kb;
    std::vector<float> Y(N), cb(N), cr(N);
    std::vector<unsigned char> alpha(N);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const float* p = c.src.at(i, j);
            const double r = v8::toLevel(p[0]) / 255.0, g = v8::toLevel(p[1]) / 255.0, b = v8::toLevel(p[2]) / 255.0;
            const size_t k = (size_t)j * W + i;
            const double y = kr * r + kg * g + kb * b;
            Y[k] = (float)y;
            cb[k] = (float)((b - y) / (2.0 * (1.0 - kb)));
            cr[k] = (float)std::min(0.406, std::max(-0.406, (r - y) / (2.0 * (1.0 - kr))));
            alpha[k] = (unsigned char)v8::toLevel(p[3]);
        }
    std::vector<float> kx, ky;
    gaussTaps(0.44 + 0.3 * c.px(clampd(c.d("HorizontalPixels"), 0, 10)), kx);
    gaussTaps(0.44 + 0.3 * c.px(clampd(c.d("VerticalPixels"), 0, 10)), ky);
    convolvePlane(cb, W, H, kx, true); convolvePlane(cr, W, H, kx, true);
    convolvePlane(cb, W, H, ky, false); convolvePlane(cr, W, H, ky, false);
    auto level = [](double v) { return (int)std::lround(std::min(255.0, std::max(0.0, v * 255.0))); };
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const size_t k = (size_t)j * W + i;
            const double r = Y[k] + 2.0 * (1.0 - kr) * cr[k], b = Y[k] + 2.0 * (1.0 - kb) * cb[k], g = (Y[k] - kr * r - kb * b) / kg;
            float* o = c.out.at(i, j);
            o[0] = v8::fromLevel(level(r));
            o[1] = v8::fromLevel(level(g));
            o[2] = v8::fromLevel(level(b));
            o[3] = v8::fromLevel(alpha[k]);
        }
}

void chromaBlur(Ctx& c)
{
    if (c.openCl && c.out.w > 0 && c.out.h > 0 && c.src.px.size() == (size_t)c.out.w * c.out.h * 4) {
        // VEGAS's OpenCL path (GPU acceleration on), from the decoded kernels: the picture becomes Y U V (V held in 0..1 by the 8-bit texture), U and V are blurred by the recursive Gaussian
        // of vfxblurs.h (sigma from Pixels + 1.5, vertical and horizontal separately), Y and alpha stay, and the picture is turned back. Captured: 20 pictures, 99.999% of the values
        // exact and none more than 2 levels off; the model below (fitted to the 8-bit pictures) was 0.36 levels off on average.
        std::vector<float> in, out;
        oclRowsOf(c.src, in);
        vfxblurs::chromaBlur(in, c.out.w, c.out.h, oclSetting(c.d("HorizontalPixels"), 0, 10), oclSetting(c.d("VerticalPixels"), 0, 10), c.host8 ? oclStore12 : nullptr, out);
        oclRowsTo(out, c.out, c.out.w, c.out.h);
        return;
    }
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
