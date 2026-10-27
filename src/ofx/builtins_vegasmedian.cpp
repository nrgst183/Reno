// VEGAS's Median (see builtins_deriche.h), from its CPU processor in Vfx1.ofx: a weighted histogram of each channel's 8-bit values over the window (weights in units of 16384 per whole
// pixel), target rank T = 8192 + (total - 16384) * Offset, output the mean value over ranks T - 8192 .. T + 8192, rounded. The window's whole-weight runs slide along the row (one pixel in,
// one out per window row) and the few part-weight pixels at its rim are added per pixel and taken out again.
#include "builtins_deriche.h"
#include "vkfilters.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>
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

// The sliding histogram of one frame for counts of type N (int where the window's total weight fits, else long long).
template <class N>
static void medianRun(const std::vector<uint16_t>& planes, Buf& out, int W, int H, int PW, int PH, int padX, int padY,
                      const std::vector<std::pair<ptrdiff_t, int>>& cells, const std::vector<std::pair<ptrdiff_t, int>>& steps, long long whole, int r12)
{
    forRows(H, W * 40, [&](int y) {
        for (int ch = 0; ch < 4; ++ch) {
            const uint16_t* at = &planes[(size_t)ch * PW * PH + (size_t)(y + padY) * PW + padX];       // the pixel (0, y)
            N hist[257], block[17];
            std::fill(hist, hist + 257, 0);
            std::fill(block, block + 17, 0);
            for (const auto& c : cells) {
                const int v = at[c.first];
                hist[v] += c.second; block[v >> 4] += c.second;
            }
            float* orow = out.at(0, y);
            for (int x = 0; x < W; ++x) {
                const long long total = whole - hist[256];
                const long long T = 8192 - (long long)((double)(total - 16384) * (double)r12 * -6.103515625e-05);
                const long long lo = T - 8192, hi = T + 8192;
                long long acc = 0, start = 0;
                int v = 256;
                for (int bk = 0; bk < 16; ++bk) {
                    if (start + block[bk] <= lo) { start += block[bk]; continue; }
                    v = bk * 16;
                    break;
                }
                if (start <= hi)
                    for (; v < 256; ++v) {
                        const long long c = hist[v];
                        if (c <= 0) continue;
                        const long long end = start + c;
                        const long long a = std::max(start, lo), b = std::min(end, hi);
                        if (b > a) acc += (long long)v * (b - a);
                        start = end;
                        if (end >= hi) break;
                    }
                const long long o = (acc + 0x2000) >> 14;
                orow[(size_t)x * 4 + ch] = (float)std::min<long long>(255, std::max<long long>(0, o)) / 255.f;
                if (x + 1 == W) break;
                const uint16_t* p = at + x;
                for (const auto& s : steps) {
                    const int v2 = p[s.first];
                    hist[v2] += s.second; block[v2 >> 4] += s.second;
                }
            }
        }
    });
}

void medianIntSetup(int W, int H, double rxd, double ryd, bool round, double offset, MedianSetup& m)
{
    const float rx = (float)std::max(0.0, rxd), ry = (float)std::max(0.0, ryd);
    int kx = 1, ky = 1;
    const std::vector<int> q = medianKernel(rx, ry, round, kx, ky);
    auto weight = [&](int i, int j) { return std::abs(i) < kx && std::abs(j) < ky ? q[(size_t)std::abs(j) * kx + std::abs(i)] : 0; };
    m.kx = kx; m.ky = ky;
    m.cells.clear(); m.steps.clear();
    long long whole = 0;
    for (int j = -(ky - 1); j <= ky - 1; ++j) {
        for (int t = -(kx - 1); t <= kx; ++t) {
            const int dw = weight(t - 1, j) - weight(t, j);
            if (dw) { m.steps.push_back(j); m.steps.push_back(t); m.steps.push_back(dw); }
        }
        for (int i = -(kx - 1); i <= kx - 1; ++i)
            if (weight(i, j) > 0) { m.cells.push_back(j); m.cells.push_back(i); m.cells.push_back(weight(i, j)); whole += weight(i, j); }
    }
    m.whole = whole;
    m.r12 = (int)((float)std::min(1.0, std::max(0.0, offset)) * 16384.f);
    (void)W; (void)H;
}

void vegasMedian(const Buf& src, Buf& out, double rxd, double ryd, bool round, double offset)
{
    const int W = src.w, H = src.h;
    out.ensure(W, H);
    if (W <= 0 || H <= 0) return;
    const float rx = (float)std::max(0.0, rxd), ry = (float)std::max(0.0, ryd);
    int kx = 1, ky = 1;
    const std::vector<int> q = medianKernel(rx, ry, round, kx, ky);
    auto weight = [&](int i, int j) { return std::abs(i) < kx && std::abs(j) < ky ? q[(size_t)std::abs(j) * kx + std::abs(i)] : 0; };
    const int padX = kx + 1, padY = ky, PW = W + 2 * padX, PH = H + 2 * padY;
    // The window's cells (offset in the padded plane, weight) and what changes in the histogram when it moves one pixel right:
    // the cell at offset t (-(kx-1) .. kx) from the old centre gains weight(t - 1) - weight(t).
    std::vector<std::pair<ptrdiff_t, int>> cells, steps;
    long long whole = 0;
    for (int j = -(ky - 1); j <= ky - 1; ++j) {
        for (int t = -(kx - 1); t <= kx; ++t) {
            const int dw = weight(t - 1, j) - weight(t, j);
            if (dw) steps.push_back(std::make_pair((ptrdiff_t)j * PW + t, dw));
        }
        for (int i = -(kx - 1); i <= kx - 1; ++i)
            if (weight(i, j) > 0) { cells.push_back(std::make_pair((ptrdiff_t)j * PW + i, weight(i, j))); whole += weight(i, j); }
    }
    const int r12 = (int)((float)std::min(1.0, std::max(0.0, offset)) * 16384.f);
    // the levels of every channel in planes with a rim of value 256 (outside the picture: counted apart, left out of the total)
    std::vector<uint16_t> planes((size_t)4 * PW * PH, 256);
    forRows(H, W * 4, [&](int y) {
        for (int ch = 0; ch < 4; ++ch) {
            uint16_t* d = &planes[(size_t)ch * PW * PH + (size_t)(y + padY) * PW + padX];
            const float* s = src.at(0, y) + ch;
            for (int x = 0; x < W; ++x) d[x] = (uint16_t)(int)std::nearbyint(std::min(1.f, std::max(0.f, s[(size_t)x * 4])) * 255.f);
        }
    });
    if (whole < (1ll << 30)) medianRun<int>(planes, out, W, H, PW, PH, padX, padY, cells, steps, whole, r12);
    else medianRun<long long>(planes, out, W, H, PW, PH, padX, padY, cells, steps, whole, r12);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
