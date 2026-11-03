// Add Noise as VEGAS's OpenCL kernel draws it (src/ofx/vfxnoise.h) and Gradient Map as vfx1.dll's render function does (src/ofx/vfxgradmap.h). Needs nothing but those headers:
//
//   g++ -std=c++17 -O1 -o vfxnoisegm_test tests/engine/vfxnoisegm_test.cpp && ./vfxnoisegm_test
//
// Add Noise: the pinned numbers are the first four of a column's generator after its warm-up, from the numpy replica of the kernel (which reproduced VEGAS's pictures:
// 100.00% of the bytes on 13 of 15 captured settings, 99.996% on the Gaussian ones). Gradient Map: the table of the Night Vision preset; the header matched the DLL's function on
// 400 random gradients (8-bit and float, 3.3 million values each, no difference) and VEGAS's pictures of all nine presets.
#include "../../src/ofx/vfxgradmap.h"
#include "../../src/ofx/vfxnoise.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno::ofx;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

int main()
{
    {
        float st[24], carry;
        vfxnoise::Ranlux R;
        vfxnoise::initColumn(3, 320, 0, st, &carry);
        R.load(st, carry);
        R.warmUp();
        vfxnoise::F4 u = R.next();
        check(u.x == 0.537497759f && u.y == 0.534648418f && u.z == 0.70110178f && u.w == 0.190876245f, "ranlux column 3, seed 0");
        vfxnoise::initColumn(17, 48, 12345, st, &carry);
        R.load(st, carry);
        R.warmUp();
        u = R.next();
        check(u.x == 0.539334178f && u.y == 0.708795369f && u.z == 0.199196219f && u.w == 0.0942772031f, "ranlux column 17 of 48, seed 12345");
    }
    {
        // level 0 leaves an opaque picture as it is; an empty one stays empty; the same seed gives the same grain
        const int W = 20, H = 6;
        std::vector<float> in((size_t)W * H * 4, 0.25f), a(in.size()), b(in.size());
        for (size_t i = 3; i < in.size(); i += 4) in[i] = 1.f;
        vfxnoise::Params P;
        P.level = 0.f;
        vfxnoise::render(in.data(), a.data(), W, H, P);
        check(a == in, "level 0 keeps an opaque picture");
        P.level = 0.5f; P.seed = 77; P.gauss = true; P.mono = false;
        vfxnoise::render(in.data(), a.data(), W, H, P);
        vfxnoise::render(in.data(), b.data(), W, H, P);
        check(a == b && a != in, "same seed, same grain");
        std::vector<float> zero(in.size(), 0.f);
        vfxnoise::render(zero.data(), a.data(), W, H, P);
        check(a == zero, "transparent pixels stay empty");
        check(vfxnoise::store12(0.5f) == 127, "12-bit store: half is level 127 (ties down)");
        check(vfxnoise::store12(1.f) == 255 && vfxnoise::store12(-1.f) == 0 && vfxnoise::store12(NAN) == 0, "12-bit store ends");
    }
    {
        vfxgm::Gradient g;
        g.stops = vfxgm::parseStops("0 0 0 0 0.600000024;0.41 0.125490203 0.670588255 0.164705887 1;1 1 1 1 1");
        check(g.stops.size() == 3, "three stops parsed");
        uint8_t lut[256][4];
        vfxgm::buildTable(vfxgm::prepare(g), lut);
        const int ix[] = {0, 40, 105, 200, 255};
        const int want[5][4] = {{0, 0, 0, 153}, {5, 46, 2, 187}, {42, 171, 32, 255}, {195, 239, 192, 255}, {255, 255, 255, 255}};
        bool ok = true;
        for (int k = 0; k < 5; ++k)
            for (int q = 0; q < 4; ++q) ok = ok && lut[ix[k]][q] == want[k][q];
        check(ok, "Night Vision table");
        // no stops at all: the picture is not touched
        uint8_t px[4] = {10, 20, 30, 255};
        vfxgm::Gradient none;
        vfxgm::run8(px, 4, 1, 1, none);
        check(px[0] == 10 && px[1] == 20 && px[2] == 30, "no stops, no change");
        // Reset to None (three transparent stops) keeps the pixel too
        vfxgm::Gradient reset;
        reset.stops = vfxgm::parseStops("0 0 0 0 0;0.5 0 0 0 0;1 0 0 0 0");
        vfxgm::run8(px, 4, 1, 1, reset);
        check(px[0] == 10 && px[1] == 20 && px[2] == 30 && px[3] == 255, "Reset to None is the identity");
    }
    std::printf("vfxnoisegm_test: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
