// Cross Effect as VEGAS's OpenCL kernels draw it (src/ofx/vfxcross.h). Needs nothing but that header (and vfxwipes.h, vfxgauss.h):
//
//   g++ -std=c++17 -O1 -ffp-contract=off -o vfxcross_test tests/engine/vfxcross_test.cpp && ./vfxcross_test
//
// None and Zoom pixels are pinned to the float results of VEGAS's decoded kernels run on the CPU (an OpenCL shim, float32, no fused multiply-adds) with the settings the DLL's host code hands
// them: the header matched that oracle bit for bit on 60 random settings and sizes. Pixelate and Blur are pinned to the header's own results, which matched VEGAS's captured pictures
// (Pixelate: all 6 + 1 settings exact; Blur: 7 settings within one level); the rest are checks that keep the maths from drifting (the ends of the progress, no blur at size 0, nonsense settings).
#include "../../src/ofx/vfxcross.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno::ofx::builtin::crossfx;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

static const int W = 40, H = 24;
static std::vector<float> gA, gB;     // the kernels' order: the bottom row first

static void pictures()
{
    gA.assign((size_t)W * H * 4, 1.f);
    gB.assign((size_t)W * H * 4, 1.f);
    for (int y = 0; y < H; ++y)       // (built top first as Reno's pictures are, then turned over)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) {
                gA[((size_t)(H - 1 - y) * W + x) * 4 + c] = (float)((x * 7 + y * 3 + c * 11) % 256) / 255.0f;
                gB[((size_t)(H - 1 - y) * W + x) * 4 + c] = (float)((x * 5 + y * 13 + c * 29 + 100) % 256) / 255.0f;
            }
}

// the pinned pixels are given as (x, y) with y counted from the top row
static void pinned(const std::vector<float>& o, const float (*want)[3], float tol, const char* name)
{
    static const int pts[5][2] = {{5, 3}, {20, 12}, {33, 20}, {12, 18}, {27, 6}};
    bool ok = true;
    for (int i = 0; i < 5; ++i)
        for (int c = 0; c < 3; ++c)
            if (std::fabs(o[((size_t)(H - 1 - pts[i][1]) * W + pts[i][0]) * 4 + c] - want[i][c]) > tol) ok = false;
    check(ok, name);
}

static std::vector<float> run(const Settings& s)
{
    std::vector<float> o;
    render(gA, gB, o, W, H, s);
    return o;
}

static bool same(const std::vector<float>& a, const std::vector<float>& b, float tol)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(std::fabs(a[i] - b[i]) <= tol)) return false;
    return true;
}

int main()
{
    pictures();
    {   // None: a fade range of 0.6 at progress 0.37
        Settings s; s.type = 0; s.progress = 0.37; s.fadeRange = 0.6;
        static const float want[5][3] = {{0.305882394f, 0.369019628f, 0.432156891f}, {0.605751634f, 0.668888927f, 0.73202616f}, {0.112810463f, 0.175947711f, 0.239084974f}, {0.541176498f, 0.604313731f, 0.667451024f}, {0.645098031f, 0.708235264f, 0.771372557f}};
        pinned(run(s), want, 2e-6f, "none fade range 0.6");
    }
    {   // Zoom on the outgoing picture only, max zoom 4, centre (0.3, 0.6)
        Settings s; s.type = 1; s.apply = 0; s.progress = 0.37; s.maxZoom = 4.0; s.srcX = 0.3; s.srcY = 0.6;
        static const float want[5][3] = {{0.411886364f, 0.481141269f, 0.550396144f}, {0.514286935f, 0.583541811f, 0.652796745f}, {0.558184385f, 0.62743932f, 0.696694255f}, {0.513208687f, 0.582463562f, 0.651718497f}, {0.497745395f, 0.56700027f, 0.636255205f}};
        pinned(run(s), want, 2e-6f, "zoom A");
    }
    {   // incoming only, max zoom 8, centre (0.7, 0.4)
        Settings s; s.type = 1; s.apply = 1; s.progress = 0.37; s.maxZoom = 8.0; s.dstX = 0.7; s.dstY = 0.4;
        static const float want[5][3] = {{0.240285993f, 0.309540898f, 0.378795803f}, {0.641593933f, 0.710848808f, 0.780103683f}, {0.359362602f, 0.428617507f, 0.474668026f}, {0.562572777f, 0.631827712f, 0.701082587f}, {0.701320171f, 0.770575047f, 0.839829981f}};
        pinned(run(s), want, 2e-6f, "zoom B");
    }
    {   // both, separate centres
        Settings s; s.type = 1; s.apply = 2; s.progress = 0.37; s.maxZoom = 3.0; s.fadeRange = 0.5; s.srcX = 0.2; s.srcY = 0.2; s.dstX = 0.8; s.dstY = 0.7;
        static const float want[5][3] = {{0.365092516f, 0.253979802f, 0.314058214f}, {0.54629904f, 0.606377482f, 0.666455925f}, {0.854145765f, 0.914224267f, 0.777177691f}, {0.488781244f, 0.548859775f, 0.608938158f}, {0.587575972f, 0.647654414f, 0.707732856f}};
        pinned(run(s), want, 2e-6f, "zoom A and B");
    }
    {   // Pixelate
        Settings s; s.type = 2; s.apply = 0; s.progress = 0.5; s.maxX = 0.8; s.maxY = 0.6;
        static const float want[5][3] = {{0.390196085f, 0.468627483f, 0.547058821f}, {0.548039198f, 0.626470566f, 0.704901934f}, {0.13529411f, 0.213725507f, 0.292156875f}, {0.552941203f, 0.631372571f, 0.709803939f}, {0.52352941f, 0.594117701f, 0.633333325f}};
        pinned(run(s), want, 2e-6f, "pixelate A");
        Settings t; t.type = 2; t.apply = 2; t.progress = 0.8;
        static const float want2[5][3] = {{0.538823605f, 0.63843143f, 0.600000024f}, {0.462352961f, 0.561960816f, 0.661568642f}, {0.279279709f, 0.231372565f, 0.33098039f}, {0.627451003f, 0.727058887f, 0.711932838f}, {0.41568628f, 0.374117672f, 0.445490211f}};
        pinned(run(t), want2, 2e-6f, "pixelate A and B");
    }
    {   // Blur
        Settings s; s.type = 3; s.apply = 1; s.progress = 0.6; s.maxBlur = 0.5;
        static const float want[5][3] = {{0.454903662f, 0.540393412f, 0.625430346f}, {0.511372566f, 0.596862793f, 0.68235296f}, {0.104181245f, 0.171081036f, 0.25646925f}, {0.541176498f, 0.626666665f, 0.712156296f}, {0.458824158f, 0.544313729f, 0.629803896f}};
        pinned(run(s), want, 2e-5f, "blur B");
    }
    // the ends of the progress: the outgoing picture at 0, the incoming one at 1 (Zoom / Pixelate / Blur do nothing to it there for the picture that is shown)
    for (int type = 0; type < 4; ++type) {
        Settings s; s.type = type; s.apply = 2; s.maxZoom = 5;
        s.progress = 0.0;
        std::vector<float> o0 = run(s);
        s.progress = 1.0;
        std::vector<float> o1 = run(s);
        if (type == 0 || type == 1) {
            check(same(o0, gA, 1e-6f), "progress 0 is the outgoing picture");
            check(same(o1, gB, 1e-6f), "progress 1 is the incoming picture");
        } else {
            check(same(o0, gA, 1e-5f), "progress 0 is the outgoing picture (pixelate / blur)");
        }
    }
    {   // a blur of size 0 changes nothing
        Settings s; s.type = 3; s.apply = 2; s.progress = 0.5; s.maxBlur = 0.0;
        Settings n; n.type = 0; n.progress = 0.5;
        check(same(run(s), run(n), 1e-6f), "blur of size 0");
    }
    {   // nonsense: no crash, the numbers stay numbers
        Settings s; s.type = 9; s.apply = -4; s.progress = 7; s.fadeRange = -3; s.maxZoom = 0; s.maxX = 50; s.maxY = -50; s.maxBlur = 1e9; s.srcX = 1e30; s.dstY = -1e30;
        bool ok = true;
        for (int type = -1; type < 5; ++type)
            for (int apply = -1; apply < 4; ++apply) {
                s.type = type; s.apply = apply;
                for (float v : run(s)) if (!(v == v)) ok = false;
            }
        check(ok, "nonsense settings");
    }
    std::printf("vfxcross_test: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
