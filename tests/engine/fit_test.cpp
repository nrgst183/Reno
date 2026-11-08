// fitImage (src/core/base.cpp): the bilinear path (SSE2 for 8-bit frames) must give the same bits as the plain per-pixel
// formula it replaced, for byte and float frames, corner aligned or not, enlarging, shrinking and fitting with bars.
//   fit_test
#include "../../src/core/base.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace reno;

namespace {
// Clears what lies around the dw x dh rectangle at (ox, oy) of 'img' (the bars of a fitted frame); the rectangle is written by the caller.
static void clearAroundRef(Image& img, int ox, int oy, int dw, int dh)
{
    const size_t px = img.pixelBytes();
    for (int y = 0; y < img.h; ++y) {
        uint8_t* r = img.row(y);
        if (y < oy || y >= oy + dh) { std::memset(r, 0, img.rowBytes); continue; }
        if (ox > 0) std::memset(r, 0, (size_t)ox * px);
        if (ox + dw < img.w) std::memset(r + (size_t)(ox + dw) * px, 0, (size_t)(img.w - ox - dw) * px);
    }
}

static ImagePtr fitReference(const Image& src, int w, int h, bool cornerAligned)
{
    if (src.w < 1 || src.h < 1) return Image::make(w, h, src.depth, true);
    ImagePtr out = Image::make(w, h, src.depth, false);
    if (!out) return out;
    if (src.w == w && src.h == h) {
        std::memcpy(out->data.get(), src.data.get(), src.byteSize());
        return out;
    }
    double s = std::min((double)w / src.w, (double)h / src.h);
    int dw = std::max(1, (int)(src.w * s + 0.5)), dh = std::max(1, (int)(src.h * s + 0.5));
    if (dw > w) dw = w;
    if (dh > h) dh = h;
    const int ox = (w - dw) / 2, oy = (h - dh) / 2;
    clearAroundRef(*out, ox, oy, dw, dh);
    const int ch = 4;
    const bool isByte = src.depth == PixelDepth::Byte;
    if (s == 1.0 && dw == src.w && dh == src.h) {
        // the frame already has the size it is fitted to.
        const size_t px = src.pixelBytes();
        for (int y = 0; y < dh; ++y) std::memcpy(out->row(oy + y) + (size_t)ox * px, src.row(y), (size_t)dw * px);
        return out;
    }
    // When shrinking by more than 2x, average a small box of source pixels so that the result does not shimmer.
    const int box = std::max(1, (int)(1.0 / s + 0.5));
    // What each output column reads (its source columns and weight) is the same on every row: worked out once here, by
    // the same arithmetic as per pixel, so the result is identical (this ran for every pixel of every frame fitted).
    std::vector<int> colX0(dw), colX1(dw);
    std::vector<float> colTx(dw);
    for (int x = 0; x < dw; ++x) {
        if (box >= 2) {
            colX0[x] = (int)(x / s);
            continue;
        }
        double fx = (x + 0.5) / s - 0.5;
        if (cornerAligned && dw > 1 && src.w > 1) fx = x * (double)(src.w - 1) / (dw - 1);
        int x0 = (int)std::floor(fx);
        colTx[x] = (float)(fx - x0);
        colX1[x] = std::min(src.w - 1, std::max(0, x0 + 1));
        colX0[x] = std::min(src.w - 1, std::max(0, x0));
    }
    parallelRows(dh, (size_t)dw * 4 * (size_t)(box * box), [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        double fy = (y + 0.5) / s - 0.5;
        if (cornerAligned && dh > 1 && src.h > 1) fy = y * (double)(src.h - 1) / (dh - 1);
        int by0 = (int)std::floor(fy);
        const float ty = (float)(fy - by0);
        const int y1 = std::min(src.h - 1, std::max(0, by0 + 1)), y0c = std::min(src.h - 1, std::max(0, by0));
        const int yBox = (int)(y / s);
        uint8_t* drow = out->row(oy + y);
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            if (box >= 2) {
                const int x0 = colX0[x];
                int cnt = 0;
                for (int by = 0; by < box; ++by) {
                    int sy = std::min(src.h - 1, yBox + by);
                    for (int bx = 0; bx < box; ++bx) {
                        int sx = std::min(src.w - 1, x0 + bx);
                        if (isByte) {
                            const uint8_t* p = src.row(sy) + sx * 4;
                            for (int k = 0; k < ch; ++k) acc[k] += p[k];
                        } else {
                            const float* p = (const float*)src.row(sy) + sx * 4;
                            for (int k = 0; k < ch; ++k) acc[k] += p[k];
                        }
                        ++cnt;
                    }
                }
                for (int k = 0; k < ch; ++k) acc[k] /= (float)cnt;
            } else {
                const int x0 = colX0[x], x1 = colX1[x];
                const float tx = colTx[x];
                for (int k = 0; k < ch; ++k) {
                    float p00, p10, p01, p11;
                    if (isByte) {
                        p00 = src.row(y0c)[x0 * 4 + k]; p10 = src.row(y0c)[x1 * 4 + k];
                        p01 = src.row(y1)[x0 * 4 + k]; p11 = src.row(y1)[x1 * 4 + k];
                    } else {
                        p00 = ((const float*)src.row(y0c))[x0 * 4 + k]; p10 = ((const float*)src.row(y0c))[x1 * 4 + k];
                        p01 = ((const float*)src.row(y1))[x0 * 4 + k]; p11 = ((const float*)src.row(y1))[x1 * 4 + k];
                    }
                    acc[k] = (p00 * (1 - tx) + p10 * tx) * (1 - ty) + (p01 * (1 - tx) + p11 * tx) * ty;
                }
            }
            if (isByte) {
                uint8_t* d = drow + (ox + x) * 4;
                for (int k = 0; k < ch; ++k) d[k] = (uint8_t)std::min(255.f, acc[k] + 0.5f);
            } else {
                float* d = (float*)drow + (ox + x) * 4;
                for (int k = 0; k < ch; ++k) d[k] = acc[k];
            }
        }
    }
    });
    return out;
}


ImagePtr randomImage(int w, int h, PixelDepth d, std::mt19937& g)
{
    ImagePtr im = Image::make(w, h, d, false);
    for (int y = 0; y < h; ++y) {
        if (d == PixelDepth::Byte) { uint8_t* r = im->row(y); for (int i = 0; i < w * 4; ++i) r[i] = (uint8_t)g(); }
        else { float* r = (float*)im->row(y); for (int i = 0; i < w * 4; ++i) r[i] = (g() % 1000) / 700.f - 0.2f; }
    }
    return im;
}
}  // namespace

int main()
{
    std::mt19937 g(5);
    int bad = 0, n = 0;
    const int dims[][4] = {{1280, 720, 1920, 1080}, {640, 480, 1920, 1080}, {1920, 1080, 1280, 720}, {3840, 2160, 1920, 1080}, {100, 100, 333, 77},
                           {7, 5, 40, 40}, {1, 1, 9, 9}, {1000, 300, 500, 300}, {300, 1000, 500, 300}, {2, 2, 1920, 1080}, {1920, 1080, 1919, 1079}};
    for (auto& dm : dims)
        for (int d = 0; d < 2; ++d)
            for (int c = 0; c < 2; ++c) {
                ImagePtr src = randomImage(dm[0], dm[1], d ? PixelDepth::Float : PixelDepth::Byte, g);
                ImagePtr a = fitReference(*src, dm[2], dm[3], c != 0), b = fitImage(*src, dm[2], dm[3], c != 0);
                ++n;
                const bool same = a && b && a->byteSize() == b->byteSize() && !std::memcmp(a->data.get(), b->data.get(), a->byteSize());
                if (!same) { ++bad; std::printf("FAIL %dx%d -> %dx%d %s corner %d\n", dm[0], dm[1], dm[2], dm[3], d ? "float" : "byte", c); }
            }
    std::printf("%d cases compared, %d differ\n", n, bad);
    ImagePtr src = randomImage(1280, 720, PixelDepth::Byte, g);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) fitReference(*src, 1920, 1080, false);
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) fitImage(*src, 1920, 1080, false);
    auto t2 = std::chrono::steady_clock::now();
    std::printf("720p -> 1080p: reference %.2f ms, fitImage %.2f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count() / 20, std::chrono::duration<double, std::milli>(t2 - t1).count() / 20);
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
