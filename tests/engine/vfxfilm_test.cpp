// Film Effects as VEGAS's DirectX film transform draws it (src/ofx/vfxfilm.h). Needs nothing but that header (and vfxrand.h):
//
//   g++ -std=c++17 -O1 -o vfxfilm_test tests/engine/vfxfilm_test.cpp && ./vfxfilm_test
//
// The pieces (random generator, particle factories, the simulation step, the five renderers, grain, grey and tint) matched the DLL's functions bit for bit on random settings, and
// with the dialog's own preset table (sftrans1.dll 0x18056e130) the 8-bit pictures were bit for bit the same as VEGAS's 22 captures with something switched on (RGB and alpha) when
// the effect first draws one unseen picture and then the one asked for, and again one unseen picture before each picture that does not follow the last one.
#include "../../src/ofx/vfxfilm.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno::ofx::builtin;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

static bool near(double a, double b, double e = 1e-12) { return std::fabs(a - b) <= e; }

static uint64_t fnv(const std::vector<uint8_t>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; }
    return h;
}

static std::vector<uint8_t> picture(int W, int H)
{
    std::vector<uint8_t> b((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &b[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)((x * 5 + y) % 256); p[1] = (uint8_t)((x * 3 + y * 7 + 40) % 256); p[2] = (uint8_t)((x + y * 5 + 90) % 256); p[3] = 255;
        }
    return b;
}

// the effect's call history: an unseen picture first for the first one, then the one asked for
static std::vector<uint8_t> drawn(const vfxfilm::Params& P, int W, int H, int hidden)
{
    vfxfilm::Sim S;
    vfxfilm::Surf dry; dry.W = W; dry.H = H; dry.bytes = true;
    for (int i = 0; i < hidden; ++i) vfxfilm::frame(S, P, dry, 1.0 / 30);
    std::vector<uint8_t> b = picture(W, H);
    vfxfilm::Surf s; s.W = W; s.H = H; s.bytes = true; s.b = b.data();
    vfxfilm::frame(S, P, s, 1.0 / 30);
    return b;
}

int main()
{
    // the table: spot values read from the DLL
    const vfxfilm::Preset* T = vfxfilm::presets();
    check(near(T[1].grain, 0.15) && T[1].grey == 1 && near(T[1].tint, 0.25), "Very Old Film's numbers");
    check(near(T[1].g[2].c, 16.0) && near(T[1].g[2].a, 0.0005) && near(T[1].g[2].b, 0.0005) && T[1].g[2].enable == 1, "Very Old Film's dust");
    check(T[2].g[0].enable == 0 && near(T[2].g[1].c, 0.64) && T[2].g[4].enable == 0, "Low Quality Color Film has no flicker and no jitter");
    check(near(T[3].g[4].c, 25.6) && near(T[3].grain, 0.25), "Circa 1908's jitter");
    check(near(T[4].g[1].c, 0.32) && near(T[4].tintBgr[0], 0.7490196078431373), "Circa 1980's scratches and colour (blue first)");
    check(near(T[0].g[1].b, 3e-5) && near(T[0].g[2].a, 0) && near(T[0].g[2].c, 0), "Reset to None draws nothing");

    // the OpenFX presets (6-digit numbers, red first) are the table's presets
    {
        const vfxfilm::Params p = vfxfilm::paramsFor(0.15, 0.25, 0.75, 0.716875, 0.3525, true, true, 0, 0.5);
        check(near(p.g[0].c, 3.2) && near(p.g[1].c, 3.2) && near(p.g[3].c, 16.0) && near(p.g[4].c, 6.4) && p.g[4].enable == 1, "Very Old Film's settings give all five groups");
        check(near(p.tintBgr[0], 0.35294117647058826), "(and the table's own colour)");
        const vfxfilm::Params q = vfxfilm::paramsFor(0.10, 0.20, 0.352941, 0.717647, 0.749020, false, true, 0, 0.1);
        check(near(q.g[1].c, 0.32) && q.g[0].enable == 0 && near(q.tintBgr[0], 0.7490196078431373), "Circa 1980's settings give its table");
        const vfxfilm::Params r = vfxfilm::paramsFor(0.15, 0.35, 0.352941, 0.717647, 0.749020, false, true, 0, 0.2);
        check(near(r.g[2].c, 6.4) && near(r.g[3].a, 0.0002) && r.g[4].enable == 0, "Low Quality Color Film's settings give its table");
    }
    // anything else: Reset to None with the chosen type on, the slider's scaling
    {
        const vfxfilm::Params p = vfxfilm::paramsFor(0.3, 0.1, 0.5, 0.5, 0.5, false, true, 4, 0.5);       // Scratches
        check(near(p.g[1].c, 16.0) && near(p.g[1].a, 0.0001) && near(p.g[1].b, 3e-5) && p.g[1].enable == 1, "Scratches at 0.5");
        check(near(p.g[2].c, 0) && near(p.g[0].c, 0) && near(p.grain, 0.3) && near(p.tintBgr[2], 0.5), "(the others stay at nothing)");
        const vfxfilm::Params d = vfxfilm::paramsFor(0, 0, 0.5, 0.5, 0.5, false, true, 0, 1.0);          // Dust, full
        check(near(d.g[2].c, 32.0) && near(d.g[2].a, 0.0002) && near(d.g[2].b, 0.0005), "Dust at 1");
        const vfxfilm::Params off = vfxfilm::paramsFor(0, 0, 0.5, 0.5, 0.5, false, false, 0, 1.0);
        bool none = true;
        for (int k = 0; k < 7; ++k) none = none && off.g[k].enable == 0;
        check(none, "particles off: no group");
        const vfxfilm::Params nonsense = vfxfilm::paramsFor(NAN, 7, -3, 9, NAN, true, true, 99, 12);
        check(nonsense.g[1].c >= 0 && nonsense.g[1].c <= 32 && nonsense.grain >= 0 && nonsense.grain <= 1, "nonsense settings are cut to their ranges");
    }
    // nothing on: the picture is not touched
    {
        const vfxfilm::Params z = vfxfilm::paramsFor(0, 0, 0.5, 0.5, 0.5, false, false, 0, 0);
        check(drawn(z, 40, 30, 1) == picture(40, 30), "Reset to None leaves the picture");
    }
    // the pictures of the presets are the same every time and differ from the input; the unseen first picture matters
    {
        const vfxfilm::Params c80 = vfxfilm::paramsFor(0.10, 0.20, 0.352941, 0.717647, 0.749020, false, true, 0, 0.1);
        const std::vector<uint8_t> a = drawn(c80, 64, 48, 1), b = drawn(c80, 64, 48, 1), c = drawn(c80, 64, 48, 0);
        check(a == b, "the same history gives the same picture");
        check(a != picture(64, 48) && a != c, "grain and tint change it, and the history changes the grain");
        check(a[3] == 255, "alpha stays opaque");
        const vfxfilm::Params voF = vfxfilm::paramsFor(0.15, 0.25, 0.75, 0.716875, 0.3525, true, true, 0, 0.5);
        const std::vector<uint8_t> v1 = drawn(voF, 64, 48, 1);
        bool grey = true;                      // Very Old Film: grey then tinted, so the three bytes of a pixel keep a fixed order of size
        for (size_t i = 0; i < v1.size(); i += 4) grey = grey && v1[i] <= v1[i + 1] + 40;
        check(grey && fnv(v1) != fnv(a), "Very Old Film differs from Circa 1980");
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
