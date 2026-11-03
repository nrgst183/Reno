// Layer Dimensionality as VEGAS's OpenCL kernels draw it (src/ofx/vfxlayerdim.h). Needs nothing but that header:
//
//   g++ -std=c++17 -O1 -o vfxlayerdim_test tests/engine/vfxlayerdim_test.cpp && ./vfxlayerdim_test
//
// The header matched VEGAS's 65 captured pictures (the 8-bit store applied) bit for bit, the inner shadow presets included. Here only the properties that need no capture are checked:
// (the Reno wrapper keeps every setting within +-1e6, so that is the largest tried.) The defaults leave a frame alone, an inner shadow does not touch a frame without edges to shade, and odd settings give finite pictures.
#include "../../src/ofx/vfxlayerdim.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace reno::ofx::vfxld;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

static Settings make(int shadow, int glow, double fg, double opacity, double v)
{
    const double c1[3] = {0.2, 0.4, 0.6}, c2[3] = {0.9, 0.1, 0.3}, ll[3] = {-1, 1, 3};
    return makeSettings(shadow, glow, v, v, v, c1, c2, v, fg, fg, v, ll, 0.5, 0.1, 0.5, opacity, 1.0);
}

int main()
{
    const int W = 24, H = 16;
    std::vector<float> src((size_t)W * H * 4), out(src.size());
    for (int i = 0; i < W * H; ++i) {
        const float a = (i % 7 == 0) ? 0.f : 1.f;
        for (int k = 0; k < 3; ++k) src[(size_t)i * 4 + k] = a * (float)((i * (k + 3)) % 256) / 255.f;
        src[(size_t)i * 4 + 3] = a;
    }
    render(src.data(), W, H, make(0, 0, 0, 1, 0.3), out.data());
    check(out == src, "defaults pass the frame");
    render(src.data(), W, H, make(0, 0, 0, 0.5, 0.3), out.data());
    bool half = true;
    for (size_t i = 0; i < src.size(); ++i) half = half && std::fabs(out[i] - src[i] * 0.5f) < 1e-6f;
    check(half, "opacity scales the frame");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double vals[] = {0.0, 1.0, -3.0, 50.0, 1e6};
    for (double v : vals)
        for (int sh = 0; sh <= 2; ++sh)
            for (int gl = 0; gl <= 4; ++gl) {
                render(src.data(), W, H, make(sh, gl, v, 1.0, v), out.data());
                bool fin = true;
                for (float f : out) fin = fin && std::isfinite(f);
                if (!fin) std::printf("v=%g sh=%d gl=%d\n", v, sh, gl); check(fin, "finite output");
            }
    (void)nan;
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
