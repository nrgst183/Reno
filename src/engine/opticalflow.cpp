#include "opticalflow.h"
#include <algorithm>
#include <cmath>

namespace reno {
namespace {

struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[(size_t)std::min(h - 1, std::max(0, y)) * w + std::min(w - 1, std::max(0, x))]; }
};

// Brightness of a frame reduced by 'f' (box average); row 0 is the top.
Plane lumaOf(const Image& im, int f)
{
    Plane p;
    p.w = std::max(1, im.w / f);
    p.h = std::max(1, im.h / f);
    p.v.assign((size_t)p.w * p.h, 0.f);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            double sum = 0;
            int n = 0;
            for (int j = 0; j < f; ++j)
                for (int i = 0; i < f; ++i) {
                    const int sx = std::min(im.w - 1, x * f + i), sy = std::min(im.h - 1, y * f + j);
                    const uint8_t* row = im.row(im.h - 1 - sy);
                    if (im.depth == PixelDepth::Byte) { const uint8_t* q = row + sx * 4; sum += 0.114 * q[0] + 0.587 * q[1] + 0.299 * q[2]; }
                    else { const float* q = (const float*)row + sx * 4; sum += (0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2]) * 255.0; }
                    ++n;
                }
            p.v[(size_t)y * p.w + x] = (float)(sum / n);
        }
    return p;
}

Plane half(const Plane& s)
{
    Plane p;
    p.w = std::max(1, s.w / 2);
    p.h = std::max(1, s.h / 2);
    p.v.resize((size_t)p.w * p.h);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            p.v[(size_t)y * p.w + x] = 0.25f * (s.at(2 * x, 2 * y) + s.at(2 * x + 1, 2 * y) + s.at(2 * x, 2 * y + 1) + s.at(2 * x + 1, 2 * y + 1));
    return p;
}

const int kBlock = 8, kStep = 4;

struct Grid {
    int gw = 0, gh = 0;
    std::vector<float> dx, dy;
};

// For each block (every kStep pixels, kBlock wide) of 'a' the offset in 'b' with the smallest difference, searched
// around the guess from the coarser level (twice its flow).
Grid match(const Plane& a, const Plane& b, const Grid* coarse, int radius)
{
    Grid g;
    g.gw = std::max(1, (a.w - kBlock) / kStep + 1);
    g.gh = std::max(1, (a.h - kBlock) / kStep + 1);
    g.dx.assign((size_t)g.gw * g.gh, 0.f);
    g.dy.assign((size_t)g.gw * g.gh, 0.f);
    for (int gy = 0; gy < g.gh; ++gy)
        for (int gx = 0; gx < g.gw; ++gx) {
            float gdx = 0, gdy = 0;
            if (coarse) {
                const int cx = std::min(coarse->gw - 1, gx / 2), cy = std::min(coarse->gh - 1, gy / 2);
                gdx = coarse->dx[(size_t)cy * coarse->gw + cx] * 2;
                gdy = coarse->dy[(size_t)cy * coarse->gw + cx] * 2;
            }
            const int bx = gx * kStep, by = gy * kStep;
            const int ox = (int)std::lround(gdx), oy = (int)std::lround(gdy);
            float best = 1e30f;
            int bdx = ox, bdy = oy;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int tx = ox + dx, ty = oy + dy;
                    float sad = 0;
                    for (int j = 0; j < kBlock && sad < best; j += 1)
                        for (int i = 0; i < kBlock; i += 1) sad += std::fabs(a.at(bx + i, by + j) - b.at(bx + i + tx, by + j + ty));
                    sad += (std::abs(tx) + std::abs(ty)) * 0.05f;
                    if (sad < best) { best = sad; bdx = tx; bdy = ty; }
                }
            g.dx[(size_t)gy * g.gw + gx] = (float)bdx;
            g.dy[(size_t)gy * g.gw + gx] = (float)bdy;
        }
    // a median over the neighbours takes out the odd wrong block
    Grid s = g;
    for (int gy = 0; gy < g.gh; ++gy)
        for (int gx = 0; gx < g.gw; ++gx) {
            float xs[9], ys[9];
            int n = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int x = std::min(g.gw - 1, std::max(0, gx + i)), y = std::min(g.gh - 1, std::max(0, gy + j));
                    xs[n] = g.dx[(size_t)y * g.gw + x]; ys[n] = g.dy[(size_t)y * g.gw + x]; ++n;
                }
            std::nth_element(xs, xs + 4, xs + 9);
            std::nth_element(ys, ys + 4, ys + 9);
            s.dx[(size_t)gy * g.gw + gx] = xs[4];
            s.dy[(size_t)gy * g.gw + gx] = ys[4];
        }
    return s;
}

float chan(const Image& im, double x, double y, int k)       // bilinear, clamped
{
    x -= 0.5; y -= 0.5;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    auto at = [&](int xx, int yy) {
        xx = std::min(im.w - 1, std::max(0, xx));
        yy = std::min(im.h - 1, std::max(0, yy));
        const uint8_t* row = im.row(im.h - 1 - yy);
        return im.depth == PixelDepth::Byte ? (float)row[xx * 4 + k] : ((const float*)row)[xx * 4 + k];
    };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

}  // namespace

FlowField estimateFlow(const Image& a, const Image& b)
{
    FlowField out;
    if (a.w != b.w || a.h != b.h || a.w < 2 || a.h < 2) return out;
    // work on a reduced frame (about 200 pixels across at the finest level), three levels
    int f = 1;
    while (std::max(a.w, a.h) / f > 256) f *= 2;
    Plane pa[3], pb[3];
    pa[0] = lumaOf(a, f);
    pb[0] = lumaOf(b, f);
    for (int k = 1; k < 3; ++k) { pa[k] = half(pa[k - 1]); pb[k] = half(pb[k - 1]); }
    Grid g;
    bool have = false;
    for (int k = 2; k >= 0; --k) {
        if (pa[k].w < kBlock || pa[k].h < kBlock) continue;
        g = match(pa[k], pb[k], have ? &g : nullptr, have ? 2 : 5);
        have = true;
    }
    if (!have) return out;
    out.gw = g.gw; out.gh = g.gh;
    out.step = kStep * f;
    out.dx = g.dx; out.dy = g.dy;
    for (float& v : out.dx) v *= (float)f;
    for (float& v : out.dy) v *= (float)f;
    return out;
}

bool interpolateOpticalFlow(Image& dst, const Image& a, const Image& b, float t)
{
    if (dst.w != a.w || dst.h != a.h || dst.w != b.w || dst.h != b.h || dst.depth != a.depth || dst.depth != b.depth) return false;
    t = std::min(1.f, std::max(0.f, t));
    const FlowField fl = estimateFlow(a, b);
    if (fl.gw == 0) { blendImages(dst, a, b, t); return true; }
    // the flow at a pixel: bilinear between the block centres
    const double cx0 = kBlock * 0.5 * (fl.step / (double)kStep), cy0 = cx0;
    auto flowAt = [&](double x, double y, float& fx, float& fy) {
        double gx = (x - cx0) / fl.step, gy = (y - cy0) / fl.step;
        gx = std::min((double)fl.gw - 1, std::max(0.0, gx));
        gy = std::min((double)fl.gh - 1, std::max(0.0, gy));
        const int x0 = (int)gx, y0 = (int)gy, x1 = std::min(fl.gw - 1, x0 + 1), y1 = std::min(fl.gh - 1, y0 + 1);
        const float u = (float)(gx - x0), v = (float)(gy - y0);
        auto m = [&](const std::vector<float>& s) {
            return (s[(size_t)y0 * fl.gw + x0] * (1 - u) + s[(size_t)y0 * fl.gw + x1] * u) * (1 - v) + (s[(size_t)y1 * fl.gw + x0] * (1 - u) + s[(size_t)y1 * fl.gw + x1] * u) * v;
        };
        fx = m(fl.dx);
        fy = m(fl.dy);
    };
    parallelRows(dst.h, (size_t)dst.w * 16, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int ty = dst.h - 1 - y;
            uint8_t* drow = dst.row(y);
            for (int x = 0; x < dst.w; ++x) {
                float fx, fy;
                flowAt(x + 0.5, ty + 0.5, fx, fy);
                for (int k = 0; k < 4; ++k) {
                    const float va = chan(a, x + 0.5 - t * fx, ty + 0.5 - t * fy, k);
                    const float vb = chan(b, x + 0.5 + (1 - t) * fx, ty + 0.5 + (1 - t) * fy, k);
                    const float v = va * (1 - t) + vb * t;
                    if (dst.depth == PixelDepth::Byte) drow[x * 4 + k] = (uint8_t)std::min(255.f, std::max(0.f, v + 0.5f));
                    else ((float*)drow)[x * 4 + k] = v;
                }
            }
        }
    });
    return true;
}

}  // namespace reno
