// VEGAS Newsprint (src/ofx/vfxnews.h) and Cookie Cutter (src/ofx/vfxcookie.h) as VEGAS's OpenCL kernels draw them. Needs nothing but those headers:
//
//   g++ -std=c++17 -O1 -o vfxnewscookie_test tests/engine/vfxnewscookie_test.cpp && ./vfxnewscookie_test
//
// The pinned facts are VEGAS's own, from the captured 320 x 180 pictures: Cookie Cutter's default circle has a pixel exactly on its rim (a mask of 1, half the picture: 127), a flat picture stays
// flat through Newsprint's Gaussian and dots, Newsprint with no dots is the picture.
#include "../../src/ofx/vfxcookie.h"
#include "../../src/ofx/vfxnews.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno::ofx;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

struct Su {
    int shape = 0, repX = 1, repY = 1, cw = 1, ch = 1;
    bool cutSection = false, noBorder = true, noFeather = true;
    float fw2 = 0, cxCell = 0, cyCell = 0, ratio = 1, tIn = 0, tOut = 0, rIn = 0, m1 = 0, ovalW = 0, ovalH = 0, hwF = 0, hhF = 0, boxHi = 0, boxLo = 0, nTout = 0;
    float hx = 0, hy = 0, ix0 = 0, iy0 = 0, br = 0, bg = 0, bb = 0;
    int colour[4] = {0, 0, 0, 255};
};

static std::vector<float> flatPicture(int W, int H, float r, float g, float b)
{
    std::vector<float> v((size_t)W * H * 4);
    for (size_t i = 0; i < v.size(); i += 4) { v[i] = r; v[i + 1] = g; v[i + 2] = b; v[i + 3] = 1.f; }
    return v;
}

static float store12(float v) { return vfxnews::store12f(v); }

int main()
{
    // Cookie Cutter, defaults (Circle, Size 0.25, no border / feather) on a flat green 320 x 180 picture: inside the circle the picture, outside nothing; capture pixel (123, 5) from the top
    // is on the rim exactly (mask 1) and shows half the picture (127), as do the 19 other rim pixels
    {
        const int W = 320, H = 180;
        Su su;
        vfxcookie::setupCore(su, W, H, 0, 0, 0.0, 0.0, 0.25, 1, 1, 0.5, 0.5, 0.0, 0.0, 0.0);
        vfxcookie::MaskGeo geo;
        vfxcookie::maskPrepare(su, W, H, geo);
        std::vector<float> mask((size_t)W * H);
        for (int y = 0; y < H; ++y) vfxcookie::maskRow(su, geo, W, y, &mask[(size_t)y * W], true);
        const std::vector<float> pic = flatPicture(W, H, 0.f, 1.f, 0.f);
        const float col[4] = {0, 0, 0, 1};
        int half = 0, inside = 0, outside = 0;
        float rim[4] = {0, 0, 0, 0};
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float r[4];
                vfxcookie::composite(&pic[((size_t)y * W + x) * 4], mask[(size_t)y * W + x], su.cutSection, su.noBorder, col, r);
                const int a = (int)std::lround(store12(r[3]) * 255.f);
                if (a == 255) ++inside; else if (a == 0) ++outside; else ++half;
                if (x == 123 && y == H - 1 - 5) for (int k = 0; k < 4; ++k) rim[k] = store12(r[k]) * 255.f;
            }
        check(half == 20, "Cookie Cutter: twenty pixels sit exactly on the default circle's rim");
        check(inside > 20000 && outside > 25000, "Cookie Cutter: inside and outside of the circle");
        check(std::lround(rim[0]) == 0 && std::lround(rim[1]) == 127 && std::lround(rim[2]) == 0 && std::lround(rim[3]) == 127, "Cookie Cutter: the rim pixel (123, 5) is half the picture (0, 127, 0, 127)");
        // Method 1 (cut away the section): the other way round, the rim pixel still half
        Su s1;
        vfxcookie::setupCore(s1, W, H, 0, 1, 0.0, 0.0, 0.25, 1, 1, 0.5, 0.5, 0.0, 0.0, 0.0);
        float r[4];
        vfxcookie::composite(&pic[0], 0.f, s1.cutSection, s1.noBorder, col, r);
        check(r[3] == 0.f, "Cookie Cutter: Method 1 shows nothing where the mask is 0");
        vfxcookie::composite(&pic[0], 2.f, s1.cutSection, s1.noBorder, col, r);
        check(r[3] == 1.f && r[1] == 1.f, "Cookie Cutter: Method 1 shows the picture where the mask is 2");
    }
    // the feather keeps a flat mask flat (the filter's gain is 1) and spreads an edge
    {
        const int W = 64, H = 48;
        std::vector<float> m((size_t)W * H, 2.f);
        float sx, sy;
        vfxcookie::featherSigmas(4.f, 1.f, 1.f, sx, sy);
        vfxcookie::feather(m, W, H, sx, sy);
        check(std::fabs(m[(size_t)(H / 2) * W + W / 2] - 2.f) < 1e-3f, "Cookie Cutter: the feather keeps a flat mask flat");
        std::vector<float> e((size_t)W * H, 0.f);
        for (int y = 0; y < H; ++y) for (int x = W / 2; x < W; ++x) e[(size_t)y * W + x] = 2.f;
        vfxcookie::feather(e, W, H, sx, sy);
        const float a = e[(size_t)(H / 2) * W + W / 2 - 1], b = e[(size_t)(H / 2) * W + W / 2];
        check(a > 0.f && a < 1.f && b > 1.f && b < 2.f && std::fabs(a + b - 2.f) < 1e-2f, "Cookie Cutter: the feather spreads an edge symmetrically");
    }
    // Newsprint: no dots is the picture, white stays white, a black picture with Monochrome dots stays black
    {
        const int W = 96, H = 64;
        vfxnews::Opts o;
        std::vector<float> out;
        std::vector<float> pic((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float* p = &pic[((size_t)y * W + x) * 4]; p[0] = (float)(x * 255 / (W - 1)) / 255.f; p[1] = (float)(y * 255 / (H - 1)) / 255.f; p[2] = 0.25f; p[3] = 1.f; }
        vfxnews::render(pic, W, H, 0.0, 0, 1, 1, 1, true, o, out);
        bool same = true;
        for (size_t i = 0; i < out.size(); ++i) same = same && std::fabs(out[i] - pic[i]) < 1.5f / 255.f;
        check(same, "Newsprint: Dot size 0 is the picture");
        const std::vector<float> white = flatPicture(W, H, 1.f, 1.f, 1.f), black = flatPicture(W, H, 0.f, 0.f, 0.f);
        vfxnews::render(white, W, H, 0.5, 0, 1, 1, 1, true, o, out);
        bool allWhite = true;
        for (size_t i = 0; i < out.size(); ++i) allWhite = allWhite && out[i] == 1.f;
        check(allWhite, "Newsprint: a white picture has no dots");
        vfxnews::render(black, W, H, 0.5, 1, 1, 1, 1, true, o, out);
        bool dark = true;
        for (size_t i = 0; i < out.size(); i += 4) dark = dark && out[i] < 0.5f && out[i + 3] == 1.f;
        check(dark, "Newsprint: a black picture is covered by its dots (Monochrome)");
        // all three types and a float project survive odd sizes
        for (int type = 0; type < 3; ++type) {
            vfxnews::render(flatPicture(1, 1, 0.5f, 0.5f, 0.5f), 1, 1, 1.0, type, 1, 1, 1, false, o, out);
            check(out.size() == 4, "Newsprint: a one pixel picture");
            vfxnews::render(flatPicture(7, 3, 0.5f, 0.2f, 0.9f), 7, 3, 0.2, type, 1, 1, 1, true, o, out);
            check(out.size() == 7 * 3 * 4, "Newsprint: a small picture");
        }
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
