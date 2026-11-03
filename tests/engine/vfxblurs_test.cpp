// VEGAS Gaussian Blur, Unsharp Mask, Glow and Chroma Blur as its OpenCL kernels draw them (src/ofx/vfxblurs.h). Needs nothing but that header:
//
//   g++ -std=c++17 -O1 -o vfxblurs_test tests/engine/vfxblurs_test.cpp && ./vfxblurs_test
//
// The pinned facts are properties of the kernels (checked against the captured 320 x 180 pictures when the port was made): a flat picture stays flat through every blur, Unsharp Mask with
// Amount 0 and Glow with Intensity 0 give the picture back, switched-off channels keep the source, Chroma Blur keeps grey and the luma, and odd sizes do not crash.
#include "../../src/ofx/vfxblurs.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno::ofx;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}
static float store12(float v)
{
    if (!(v == v)) return 0.f;
    const int n = (int)std::floor(std::min(std::max(v, 0.f), 1.f) * 4096.f);
    return (float)((n * 255 + 2047) >> 12) / 255.f;
}
static std::vector<float> flat(int W, int H, float r, float g, float b)
{
    std::vector<float> v((size_t)W * H * 4);
    for (size_t i = 0; i < v.size(); i += 4) { v[i] = r; v[i + 1] = g; v[i + 2] = b; v[i + 3] = 1.f; }
    return v;
}
static std::vector<float> stripes(int W, int H)
{
    std::vector<float> v((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = &v[((size_t)y * W + x) * 4];
            const float lv = (float)(((x / 4) % 2) ? 200 : 40) / 255.f;
            p[0] = lv; p[1] = (float)((y * 7) % 256) / 255.f; p[2] = (float)((x * 3 + y) % 256) / 255.f; p[3] = 1.f;
        }
    return v;
}
static float maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    float m = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
    return a.size() == b.size() ? m : 1e9f;
}

int main()
{
    const int W = 64, H = 36;
    std::vector<float> out;
    const bool all[4] = {true, true, true, true};
    // a flat picture is flat through the blur (to a level: the recursion's edge terms make it exact in the middle of the range)
    const std::vector<float> grey = flat(W, H, 100.f / 255.f, 100.f / 255.f, 100.f / 255.f);
    vfxblurs::gaussianBlur(grey, W, H, 0.3, 0.2, all, store12, out);
    check(maxDiff(out, grey) <= 1.5f / 255.f, "Gaussian Blur: a flat picture stays flat");
    // Range 0 still blurs a little (sigma of 1.5 pixels' worth), and a blur of the stripes is smoother than they are
    const std::vector<float> st = stripes(W, H);
    vfxblurs::gaussianBlur(st, W, H, 0.0, 0.0, all, store12, out);
    check(maxDiff(out, st) > 0.f, "Gaussian Blur: Range 0 is not the identity");
    // channels off keep the source exactly
    const bool noGreen[4] = {true, false, true, true};
    vfxblurs::gaussianBlur(st, W, H, 0.2, 0.2, noGreen, store12, out);
    bool keep = true;
    for (size_t i = 1; i < st.size(); i += 4) keep = keep && out[i] == store12(st[i]);
    check(keep, "Gaussian Blur: a channel switched off keeps the source");
    // Unsharp Mask: Amount 0 gives the picture back; a flat picture is kept whatever the amount
    vfxblurs::unsharpMask(st, W, H, 0.05, 0.f, 0.f, store12, out);
    check(maxDiff(out, st) < 1e-6f, "Unsharp Mask: Amount 0 changes nothing");
    vfxblurs::unsharpMask(grey, W, H, 0.05, 3.f, 0.f, store12, out);
    check(maxDiff(out, grey) <= 2.f / 255.f, "Unsharp Mask: a flat picture stays flat");
    // Glow: Intensity 0 gives the picture back; full intensity with no suppression turns it to Color
    const float white[3] = {1.f, 1.f, 1.f};
    vfxblurs::glow(st, W, H, 0.1, 0.f, 0.f, white, store12, out);
    check(maxDiff(out, st) < 1e-6f, "Glow: Intensity 0 changes nothing");
    vfxblurs::glow(grey, W, H, 0.1, 100.f, 0.f, white, store12, out);
    bool white4 = true;
    for (float v : out) white4 = white4 && v == 1.f;
    check(white4, "Glow: a strong glow of a lit picture is the glow colour, alpha included");
    // Chroma Blur: grey stays grey, and the luma is the picture's
    vfxblurs::chromaBlur(grey, W, H, 4.0, 4.0, store12, out);
    check(maxDiff(out, grey) <= 2.f / 255.f, "Chroma Blur: grey stays grey");
    // odd sizes and nonsense amounts do not crash and give the right amount of numbers
    for (int sz = 1; sz <= 3; ++sz) {
        const std::vector<float> tiny = flat(sz, 1, 0.5f, 0.2f, 0.9f), tiny2 = flat(1, sz, 0.5f, 0.2f, 0.9f);
        for (const auto* p : {&tiny, &tiny2}) {
            const int w = p == &tiny ? sz : 1, h = p == &tiny ? 1 : sz;
            vfxblurs::gaussianBlur(*p, w, h, 1.0, 1.0, all, store12, out);
            check(out.size() == p->size(), "Gaussian Blur: a tiny picture");
            vfxblurs::unsharpMask(*p, w, h, 1.0, 8.f, 1.f, nullptr, out);
            check(out.size() == p->size(), "Unsharp Mask: a tiny picture");
            vfxblurs::glow(*p, w, h, 1.0, 20.f, 1.f, white, nullptr, out);
            check(out.size() == p->size(), "Glow: a tiny picture");
            vfxblurs::chromaBlur(*p, w, h, 10.0, 10.0, nullptr, out);
            check(out.size() == p->size(), "Chroma Blur: a tiny picture");
        }
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
