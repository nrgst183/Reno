// VEGAS Denoise (src/ofx/vfxdenoise.h) and the recursive Gaussian of Radial Blur (src/ofx/vfxgauss.h) as VEGAS's OpenCL kernels draw them. Needs nothing but those headers:
//
//   g++ -std=c++17 -O1 -ffp-contract=off -o vfxdenoise_test tests/engine/vfxdenoise_test.cpp && ./vfxdenoise_test
//
// Pinned values are VEGAS's own: the pictures of flat colours it gives back (captured: a colour bar of the test picture is not changed by the non-local means, only by the round trip through
// 8-bit YUV planes, which saturates Red's V), the coefficients its builder hands the kernels (the DLL's function, run directly), and the sampler's border rows (a captured 320 x 180 picture was
// bit exact with them).
#include "../../src/ofx/vfxdenoise.h"
#include "../../src/ofx/vfxgauss.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno::ofx;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

static void flat(int r, int g, int b, int er, int eg, int eb, const char* name)
{
    const int W = 48, H = 48;
    std::vector<float> in((size_t)W * H * 4), out((size_t)W * H * 4);
    for (size_t i = 0; i < in.size(); i += 4) { in[i] = r / 255.f; in[i + 1] = g / 255.f; in[i + 2] = b / 255.f; in[i + 3] = 1.f; }
    vfxdn::Params p;        // the defaults: Luminance 3, Color 3
    vfxdn::run(in.data(), W, H, p, out.data());
    bool ok = true;
    for (int k = 0; k < 4 && ok; ++k) {
        const float* px = &out[((size_t)(H / 2) * W + W / 2) * 4];
        const int v = (int)(px[0] * 255.f + 0.5f), gg = (int)(px[1] * 255.f + 0.5f), bb = (int)(px[2] * 255.f + 0.5f);
        ok = v == er && gg == eg && bb == eb && (int)(px[3] * 255.f + 0.5f) == 255;
    }
    check(ok, name);
}

int main()
{
    // flat colours through the YUV round trip (captured VEGAS pictures of the test picture's colour bars)
    flat(255, 255, 255, 254, 255, 254, "Denoise: white -> 254 255 254");
    flat(255, 0, 0, 221, 17, 0, "Denoise: red -> 221 17 0 (V saturates in the byte plane)");
    flat(0, 255, 255, 34, 238, 255, "Denoise: cyan -> 34 238 255");
    flat(0, 255, 0, 5, 253, 1, "Denoise: green -> 5 253 1");
    flat(255, 0, 255, 250, 2, 254, "Denoise: magenta -> 250 2 254");
    flat(0, 0, 255, 0, 0, 255, "Denoise: blue stays");
    flat(255, 255, 0, 255, 255, 0, "Denoise: yellow stays");
    flat(16, 16, 16, 15, 16, 15, "Denoise: dark grey -> 15 16 15");
    flat(255, 128, 0, 254, 128, 0, "Denoise: orange -> 254 128 0");

    // both strengths 0: the picture is passed on (8-bit levels)
    {
        const int W = 20, H = 14;
        std::vector<float> in((size_t)W * H * 4), out((size_t)W * H * 4);
        for (size_t i = 0; i < in.size(); ++i) in[i] = (float)((i * 37) % 256) / 255.f;
        vfxdn::Params p;
        p.lum = 0; p.col = 0;
        vfxdn::run(in.data(), W, H, p, out.data());
        bool same = true;
        for (size_t i = 0; i < in.size(); ++i) if (out[i] != in[i]) same = false;
        check(same, "Denoise: Luminance 0 and Color 0 give the picture back");
    }

    // sampler: width 320 (columns), height 180 (rows)
    check(vfxdn::texel(-1, 320) == 0 && vfxdn::texel(0, 320) == 0, "texel: the first two columns read column 0");
    check(vfxdn::texel(-2, 320) == 1 && vfxdn::texel(-3, 320) == 2, "texel: the left border mirrors without repeating the edge pixel");
    check(vfxdn::texel(-6, 320) == 4, "texel: an exactly representable coordinate on the left reads one pixel less");
    check(vfxdn::texel(4, 320) == 5 && vfxdn::texel(9, 320) == 10 && vfxdn::texel(5, 320) == 5, "texel: 5/320 is exact in floats, so those columns read the next pixel");
    check(vfxdn::texel(44, 180) == 45 && vfxdn::texel(56, 180) == 56, "texel: height 180 rows");
    check(vfxdn::texel(57, 180) == 58 && vfxdn::texel(70, 180) == 71 && vfxdn::texel(58, 180) == 58 && vfxdn::texel(180, 180) == 178, "texel: height 180 rows whose coordinate lands on the 2^-21 grid read one more");
    check(vfxdn::texel(320, 320) == 319, "texel: the right border mirrors");

    // the Gaussian's coefficients as VEGAS's builder gives them (doubles run from the DLL)
    {
        const vfxgauss::Coefs c = vfxgauss::build(10.0);
        check(std::fabs(c.n[0] - 0.040049627515194718) < 1e-12 && std::fabs(c.n[1] + 0.062773817375120111) < 1e-12 && std::fabs(c.n[2] - 0.02559871382507278) < 1e-12,
              "Gaussian sigma 10: numerator");
        check(std::fabs(c.d[1] + 2.556583539603551) < 1e-12 && std::fabs(c.d[2] - 2.1946586827396839) < 1e-12 && std::fabs(c.d[3] + 0.6325474762073634) < 1e-12, "Gaussian sigma 10: feedback");
        check(std::fabs(c.bn[1] + 1.3294868790380758) < 1e-12 && std::fabs(c.bm[2] - 1.0533817099864897) < 1e-12, "Gaussian sigma 10: edge terms");
        const vfxgauss::Coefs h = vfxgauss::build(150.0);          // (the branch for sigma over 100)
        check(std::fabs(h.n[0] - 0.0025882086430387271) < 1e-12 && std::fabs(h.d[1] + 1.9832389088325659) < 1e-12 && h.d[3] == 0.0, "Gaussian sigma 150");
    }
    // a constant column stays constant (the two halves sum to the value) and a spike spreads evenly
    {
        const int W = 3, H = 200;
        std::vector<float> in((size_t)W * H * 4, 0.5f), a, b;
        const vfxgauss::Coefsf cf = vfxgauss::toFloats(vfxgauss::build(10.0));
        vfxgauss::passColumns(cf, in, W, H, a, b, [](float x, float y, float z) { return x * y + z; });
        bool ok = true;
        for (int y = 0; y < H; ++y) ok = ok && std::fabs(a[((size_t)y * W + 1) * 4] + b[((size_t)y * W + 1) * 4] - 0.5f) < 1e-3f;
        check(ok, "Gaussian: a flat column stays flat");
        std::vector<float> sp((size_t)W * H * 4, 0.f);
        sp[((size_t)100 * W + 1) * 4] = 1.f;
        vfxgauss::passColumns(cf, sp, W, H, a, b, [](float x, float y, float z) { return x * y + z; });
        double sum = 0;
        for (int y = 0; y < H; ++y) sum += a[((size_t)y * W + 1) * 4] + b[((size_t)y * W + 1) * 4];
        const float up = a[((size_t)95 * W + 1) * 4] + b[((size_t)95 * W + 1) * 4], dn = a[((size_t)105 * W + 1) * 4] + b[((size_t)105 * W + 1) * 4];
        check(std::fabs(sum - 1.0) < 0.02 && std::fabs(up - dn) < 2e-3f && up > 0.f, "Gaussian: a spike keeps its weight and spreads both ways");
    }
    std::printf("vfxdenoise_test: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
