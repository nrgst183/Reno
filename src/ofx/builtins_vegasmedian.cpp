// VEGAS's Median (see builtins_deriche.h), from its CPU processor in Vfx1.ofx: a weighted histogram of each channel's 8-bit values over the window (weights in units of 16384 per whole
// pixel), target rank T = 8192 + (total - 16384) * Offset, output the mean value over ranks T - 8192 .. T + 8192, rounded. The window's whole-weight runs slide along the row (one pixel in,
// one out per window row) and the few part-weight pixels at its rim are added per pixel and taken out again.
#include "builtins_deriche.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

namespace {

// The quarter window (rows |dy| 0..ky-1, columns |dx| 0..kx-1) of 14-bit weights.
std::vector<int> medianKernel(float rx, float ry, bool round, int& kx, int& ky)
{
    std::vector<int> q;
    if (!round) {
        const int ix = (int)rx, iy = (int)ry;
        const float fx = rx - (float)ix, fy = ry - (float)iy;
        kx = ix + 2; ky = iy + 2;
        q.assign((size_t)kx * ky, 16384);
        for (int i = 0; i < kx; ++i) q[(size_t)(ky - 1) * kx + i] = (int)(fy * 16384.f);
        for (int j = 0; j < ky; ++j) q[(size_t)j * kx + kx - 1] = (int)(fx * 16384.f);
        q[(size_t)(ky - 1) * kx + kx - 1] = (int)((float)((double)fx * (double)fy) * 16384.f);
        return q;
    }
    const double a = (double)rx + 0.5, b = (double)ry + 0.5;
    kx = (int)(a + 1.0) + 1; ky = (int)(b + 1.0) + 1;
    q.assign((size_t)kx * ky, 0);
    const int steps = 512;
    for (int j = 0; j < ky; ++j)
        for (int i = 0; i < kx; ++i) {
            const double x0 = i - 0.5, x1 = i + 0.5, y0 = j - 0.5, y1 = j + 0.5;
            double area = 0;
            for (int s = 0; s < steps; ++s) {
                const double x = x0 + (s + 0.5) * (x1 - x0) / steps;
                if (std::fabs(x) >= a) continue;
                const double h = b * std::sqrt(1.0 - (x / a) * (x / a));
                const double lo = std::max(y0, -h), hi = std::min(y1, h);
                if (hi > lo) area += (hi - lo) / steps;
            }
            q[(size_t)j * kx + i] = (int)((float)area * 16384.f);
        }
    return q;
}

}  // namespace

void vegasMedian(const Buf& src, Buf& out, double rxd, double ryd, bool round, double offset)
{
    const int W = src.w, H = src.h;
    out.ensure(W, H);
    if (W <= 0 || H <= 0) return;
    const float rx = (float)std::max(0.0, rxd), ry = (float)std::max(0.0, ryd);
    int kx = 1, ky = 1;
    const std::vector<int> q = medianKernel(rx, ry, round, kx, ky);
    std::vector<int> run((size_t)ky, -1);
    struct Cell { int dx, dy, w; };
    std::vector<Cell> rim;
    for (int j = 0; j < ky; ++j) {
        int h = -1;
        while (h + 1 < kx && q[(size_t)j * kx + h + 1] == 16384) ++h;
        run[(size_t)j] = h;
        for (int i = h + 1; i < kx; ++i) {
            const int w = q[(size_t)j * kx + i];
            if (w <= 0) continue;
            for (int sy = (j ? -1 : 1); sy <= 1; sy += 2)
                for (int sx = (i ? -1 : 1); sx <= 1; sx += 2) rim.push_back(Cell{sx * i, sy * j, w});
        }
    }
    const int r12 = (int)((float)std::min(1.0, std::max(0.0, offset)) * 16384.f);
    std::vector<uint8_t> lv((size_t)W * H * 4);
    for (size_t i = 0; i < lv.size(); ++i) lv[i] = (uint8_t)(int)std::nearbyint(std::min(1.f, std::max(0.f, src.px[i])) * 255.f);
    forRows(H, W * 40, [&](int y) {
        for (int ch = 0; ch < 4; ++ch) {
            int hist[256], block[16];
            std::fill(hist, hist + 256, 0);
            std::fill(block, block + 16, 0);
            long long total = 0;
            auto add = [&](int xx, int yy, int w) {
                if (xx < 0 || xx >= W || yy < 0 || yy >= H) return;
                const int v = lv[((size_t)yy * W + xx) * 4 + ch];
                hist[v] += w; block[v >> 4] += w; total += w;
            };
            for (int dy = -(ky - 1); dy <= ky - 1; ++dy) {
                const int h = run[(size_t)std::abs(dy)];
                for (int dx = -h; dx <= h; ++dx) add(dx, y + dy, 16384);
            }
            float* orow = out.at(0, y);
            for (int x = 0; x < W; ++x) {
                if (x > 0)
                    for (int dy = -(ky - 1); dy <= ky - 1; ++dy) {
                        const int h = run[(size_t)std::abs(dy)];
                        if (h < 0) continue;
                        add(x - 1 - h, y + dy, -16384);
                        add(x + h, y + dy, 16384);
                    }
                for (const Cell& cl : rim) add(x + cl.dx, y + cl.dy, cl.w);
                const int T = 8192 - (int)((double)(total - 16384) * (double)r12 * -6.103515625e-05);
                const int lo = T - 8192, hi = T + 8192;
                long long acc = 0;
                int start = 0, v = 256;
                for (int bk = 0; bk < 16; ++bk) {
                    if (start + block[bk] <= lo) { start += block[bk]; continue; }
                    v = bk * 16;
                    break;
                }
                if (start + 0 <= hi)
                    for (; v < 256; ++v) {
                        const int c = hist[v];
                        if (c <= 0) continue;
                        const int end = start + c;
                        const int a = std::max(start, lo), b = std::min(end, hi);
                        if (b > a) acc += (long long)v * (b - a);
                        start = end;
                        if (end >= hi) break;
                    }
                for (const Cell& cl : rim) add(x + cl.dx, y + cl.dy, -cl.w);
                const int o = (int)((acc + 0x2000) >> 14);
                orow[(size_t)x * 4 + ch] = (float)std::min(255, std::max(0, o)) / 255.f;
            }
        }
    });
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
