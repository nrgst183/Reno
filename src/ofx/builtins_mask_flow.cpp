// Built-in stand-in for VEGAS Pro 18's Warp Flow transition (de.magix:WarpFlowTransition): "sophisticated optical flow technology to add new intermediary frames that smoothly
// morph the last frame of the first event into the first frame of the second" (the manual, on Smart Split). Not VEGAS's maths (unpublished): the motion between the outgoing and
// incoming pictures is found with a coarse-to-fine Horn-Schunck optical flow (on a reduced copy), and each frame is made by moving the outgoing picture part of the way along
// the flow, the incoming picture back the rest of the way, and blending by progress. Where the pictures have no common motion (a scene change) it looks like a dissolve with some smearing.
#include "builtins_mask_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[(size_t)std::min(h - 1, std::max(0, y)) * w + std::min(w - 1, std::max(0, x))]; }
    float sample(double x, double y) const        // bilinear, pixel centres at +0.5, edges clamped
    {
        x = clampd(x - 0.5, 0, w - 1);
        y = clampd(y - 0.5, 0, h - 1);
        const int x0 = (int)x, y0 = (int)y;
        const float fx = (float)(x - x0), fy = (float)(y - y0);
        const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
        const float* r0 = &v[(size_t)y0 * w];
        const float* r1 = &v[(size_t)y1 * w];
        return (r0[x0] * (1 - fx) + r0[x1] * fx) * (1 - fy) + (r1[x0] * (1 - fx) + r1[x1] * fx) * fy;
    }
};

// The picture's brightness, reduced (averaging whole pixels) to w x h.
Plane reducedLuma(const Buf& b, int w, int h)
{
    Plane p;
    p.w = w;
    p.h = h;
    p.v.assign((size_t)w * h, 0.f);
    std::vector<float> weight((size_t)w * h, 0.f);
    forRows(h, w, [&](int y) {
        const int y0 = (int)((long long)y * b.h / h), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * b.h / h));
        for (int x = 0; x < w; ++x) {
            const int x0 = (int)((long long)x * b.w / w), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * b.w / w));
            double s = 0;
            int n = 0;
            for (int yy = y0; yy < y1 && yy < b.h; ++yy) {
                const float* px = b.at(x0, yy);
                for (int xx = x0; xx < x1 && xx < b.w; ++xx, px += 4, ++n) s += luma(px[0], px[1], px[2]);
            }
            p.v[(size_t)y * w + x] = n ? (float)(s / n) : 0.f;
        }
    });
    return p;
}

Plane halve(const Plane& a)
{
    Plane p;
    p.w = std::max(1, a.w / 2);
    p.h = std::max(1, a.h / 2);
    p.v.assign((size_t)p.w * p.h, 0.f);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            p.v[(size_t)y * p.w + x] = 0.25f * (a.at(2 * x, 2 * y) + a.at(2 * x + 1, 2 * y) + a.at(2 * x, 2 * y + 1) + a.at(2 * x + 1, 2 * y + 1));
    return p;
}

struct Flow {
    Plane u, v;
    Plane conf;
};

// One level of the pyramid: improves 'flow' (already the size of the pictures) so that B moved by it matches A.
void refine(const Plane& A, const Plane& B, Flow& f, int warps, int iters, float alpha)
{
    const int w = A.w, h = A.h;
    const size_t N = (size_t)w * h;
    std::vector<float> Ix(N), Iy(N), It(N), du(N, 0.f), dv(N, 0.f), du2(N), dv2(N);
    for (int wi = 0; wi < warps; ++wi) {
        std::vector<float> Bw(N);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) Bw[(size_t)y * w + x] = B.sample(x + 0.5 + f.u.v[(size_t)y * w + x], y + 0.5 + f.v.v[(size_t)y * w + x]);
        Plane Bp;
        Bp.w = w; Bp.h = h; Bp.v = Bw;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t k = (size_t)y * w + x;
                const float gxA = 0.5f * (A.at(x + 1, y) - A.at(x - 1, y)), gyA = 0.5f * (A.at(x, y + 1) - A.at(x, y - 1));
                const float gxB = 0.5f * (Bp.at(x + 1, y) - Bp.at(x - 1, y)), gyB = 0.5f * (Bp.at(x, y + 1) - Bp.at(x, y - 1));
                Ix[k] = 0.5f * (gxA + gxB);
                Iy[k] = 0.5f * (gyA + gyB);
                It[k] = Bw[k] - A.v[k];
            }
        std::fill(du.begin(), du.end(), 0.f);
        std::fill(dv.begin(), dv.end(), 0.f);
        for (int it = 0; it < iters; ++it) {
            for (int y = 0; y < h; ++y) {
                const int ya = std::max(0, y - 1), yb = std::min(h - 1, y + 1);
                for (int x = 0; x < w; ++x) {
                    const int xa = std::max(0, x - 1), xb = std::min(w - 1, x + 1);
                    const size_t k = (size_t)y * w + x;
                    auto tu = [&](int xx, int yy) { const size_t j = (size_t)yy * w + xx; return f.u.v[j] + du[j]; };
                    auto tv = [&](int xx, int yy) { const size_t j = (size_t)yy * w + xx; return f.v.v[j] + dv[j]; };
                    const float ub = 0.25f * (tu(xa, y) + tu(xb, y) + tu(x, ya) + tu(x, yb));
                    const float vb = 0.25f * (tv(xa, y) + tv(xb, y) + tv(x, ya) + tv(x, yb));
                    const float num = Ix[k] * (ub - f.u.v[k]) + Iy[k] * (vb - f.v.v[k]) + It[k];
                    const float den = alpha + Ix[k] * Ix[k] + Iy[k] * Iy[k];
                    du2[k] = ub - f.u.v[k] - Ix[k] * num / den;
                    dv2[k] = vb - f.v.v[k] - Iy[k] * num / den;
                }
            }
            du.swap(du2);
            dv.swap(dv2);
        }
        for (size_t k = 0; k < N; ++k) {
            f.u.v[k] += du[k];
            f.v.v[k] += dv[k];
        }
    }
}

Plane upsampleFlowPlane(const Plane& p, int w, int h, float mul)
{
    Plane o;
    o.w = w;
    o.h = h;
    o.v.resize((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) o.v[(size_t)y * w + x] = p.sample((x + 0.5) * p.w / w, (y + 0.5) * p.h / h) * mul;
    return o;
}

// The flow from picture A to picture B on a grid of about 'longSide' pixels along its longer side (the pictures need not be that size).
// In the pixels of that grid.
Flow computeFlow(const Buf& a, const Buf& b, int longSide, int iters, float alpha, int& fw, int& fh)
{
    const double k = (double)longSide / std::max(a.w, a.h);
    fw = std::max(8, (int)std::lround(a.w * std::min(1.0, k)));
    fh = std::max(8, (int)std::lround(a.h * std::min(1.0, k)));
    std::vector<Plane> pa, pb;
    pa.push_back(reducedLuma(a, fw, fh));
    pb.push_back(reducedLuma(b, fw, fh));
    while (std::min(pa.back().w, pa.back().h) > 24) {
        pa.push_back(halve(pa.back()));
        pb.push_back(halve(pb.back()));
    }
    Flow f;
    const int top = (int)pa.size() - 1;
    f.u.w = pa[(size_t)top].w; f.u.h = pa[(size_t)top].h; f.u.v.assign((size_t)f.u.w * f.u.h, 0.f);
    f.v = f.u;
    for (int lv = top; lv >= 0; --lv) {
        const Plane& A = pa[(size_t)lv];
        const Plane& B = pb[(size_t)lv];
        if (f.u.w != A.w || f.u.h != A.h) {
            f.u = upsampleFlowPlane(f.u, A.w, A.h, (float)A.w / f.u.w);
            f.v = upsampleFlowPlane(f.v, A.w, A.h, (float)A.h / f.v.h);
        }
        refine(A, B, f, 3, iters, alpha);
    }
    // where the flow does not bring the pictures together (unrelated pictures, something covered or uncovered) it is not trusted: the leftover difference, averaged over a few pixels, takes the movement away there
    {
        const Plane& A = pa[0];
        const Plane& B = pb[0];
        Plane res;
        res.w = A.w; res.h = A.h; res.v.assign((size_t)A.w * A.h, 0.f);
        for (int y = 0; y < A.h; ++y)
            for (int x = 0; x < A.w; ++x) {
                const size_t k = (size_t)y * A.w + x;
                res.v[k] = std::fabs(B.sample(x + 0.5 + f.u.v[k], y + 0.5 + f.v.v[k]) - A.v[k]);
            }
        f.conf.w = A.w; f.conf.h = A.h; f.conf.v.assign((size_t)A.w * A.h, 1.f);
        for (int y = 0; y < A.h; ++y)
            for (int x = 0; x < A.w; ++x) {
                float sum = 0;
                int n = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i, ++n) sum += res.at(x + i, y + j);
                const float r = sum / n;
                f.conf.v[(size_t)y * A.w + x] = 1.f - (float)smooth01((r - 0.06) / 0.14);
            }
    }
    return f;
}

void hueToRgb(double h, float& r, float& g, float& b)
{
    h = h - std::floor(h);
    const double s = h * 6;
    const int i = (int)s;
    const float f = (float)(s - i);
    switch (i % 6) {
    case 0: r = 1; g = f; b = 0; break;
    case 1: r = 1 - f; g = 1; b = 0; break;
    case 2: r = 0; g = 1; b = f; break;
    case 3: r = 0; g = 1 - f; b = 1; break;
    case 4: r = f; g = 0; b = 1; break;
    default: r = 1; g = 0; b = 1 - f; break;
    }
}

void warpFlowWith(Ctx& c, int quality, double smooth, double strength, bool show)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    const double t = clampd(c.progress, 0, 1);
    if (t <= 0) { c.out.px = c.src.px; return; }
    if (t >= 1 && !show) { c.out.px = c.src2.px; return; }
    quality = std::max(0, std::min(2, quality));
    const int sides[3] = {128, 256, 480};
    const int iterCount[3] = {20, 30, 40};
    const float alpha = (float)(0.002 + clampd(smooth, 0, 1) * 0.006);
    int fw = 0, fh = 0;
    const Flow f = computeFlow(c.src, c.src2, sides[quality], iterCount[quality], alpha, fw, fh);
    const double sx = (double)W / fw, sy = (double)H / fh;
    strength = clampd(strength, 0, 2);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double gx = (x + 0.5) / sx, gy = (y + 0.5) / sy;
            const double trust = f.conf.sample(gx, gy) * strength;
            const double dx = f.u.sample(gx, gy) * sx * trust, dy = f.v.sample(gx, gy) * sy * trust;
            float* o = c.out.at(x, y);
            if (show) {
                const double mag = std::hypot(dx / W, dy / H) * 8;
                float r, g, b;
                hueToRgb(std::atan2(dy, dx) / (2 * kPi) + 0.5, r, g, b);
                const float m = (float)std::min(1.0, mag);
                o[0] = r * m; o[1] = g * m; o[2] = b * m; o[3] = 1.f;
                continue;
            }
            float pa[4], pb[4];
            c.src.sampleClamped(x + 0.5 - t * dx, y + 0.5 - t * dy, pa);
            c.src2.sampleClamped(x + 0.5 + (1 - t) * dx, y + 0.5 + (1 - t) * dy, pb);
            for (int k = 0; k < 4; ++k) o[k] = (float)(pa[k] * (1 - t) + pb[k] * t);
        }
    });
}

// VEGAS's Warp Flow has no settings: Reno's medium quality, middling smoothness and full strength.
void warpFlow(Ctx& c) { warpFlowWith(c, 1, 0.5, 1.0, false); }

}  // namespace

void warpFlowForTest(Ctx& c, int quality, double smoothness, double strength, bool showField) { warpFlowWith(c, quality, smoothness, strength, showField); }

void addMaskFlow(std::vector<Def>& out)
{
    Def d;
    d.id = "de.magix:WarpFlowTransition";
    d.label = "VEGAS Warp Flow";
    d.group = kGroup;
    d.description = "Morphs the outgoing picture into the incoming one along the motion found between them (optical flow) instead of "
                    "dissolving. Works best with no camera movement.";
    d.transition = true;
    // (no parameters: VEGAS's has none)
    d.render = warpFlow;
    out.push_back(d);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
