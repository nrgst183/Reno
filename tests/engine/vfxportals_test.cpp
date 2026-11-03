// Portals as sfpagepeel.dll draws it (src/ofx/vfxportals.h). Needs nothing but that header:
//
//   g++ -std=c++17 -O1 -o vfxportals_test tests/engine/vfxportals_test.cpp && ./vfxportals_test
//
// The header's layout, stretch and border matched the DLL's own functions (loaded and called: 400 random layouts, 13000 stretches, 3000 borders, no difference) and its whole
// pictures matched VEGAS's own pictures of the default / Mondrian setting at 25, 50 and 75 % with no pixel differing. The pinned hashes below keep the maths from drifting.
#include "../../src/ofx/vfxportals.h"
#include <cstdio>
#include <vector>

using namespace reno::ofx::vfxportals;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

static const int W = 48, H = 27;

static uint64_t hashOf(const std::vector<uint8_t>& o)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t v : o) { h ^= v; h *= 1099511628211ull; }
    return h;
}

int main()
{
    std::vector<uint8_t> a((size_t)W * H * 4), b(a.size()), o(a.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 4; ++c) {
                a[((size_t)y * W + x) * 4 + c] = c == 3 ? 255 : (uint8_t)((x * 7 + y * 3 + c * 11) % 256);
                b[((size_t)y * W + x) * 4 + c] = c == 3 ? 255 : (uint8_t)((x * 5 + y * 13 + c * 29 + 100) % 256);
            }
    Props sets[3];                                              // the default (Mondrian), White Wash, and no squares at all
    sets[1].seed = 83; sets[1].squares = 12; sets[1].maxTransparency = sets[1].maxOffset = sets[1].maxScale = sets[1].borderSize = 0.5;
    sets[1].borderColor[0] = sets[1].borderColor[1] = sets[1].borderColor[2] = 1.f;
    sets[2].seed = 1234; sets[2].squares = 0;
    struct Pin { int set; double progress; uint64_t hash; };
    const Pin pins[] = {
        {0, 0.2, 0xf6d56b1175df07b1ull}, {0, 0.5, 0x87f6defb267f2d2cull}, {0, 0.8, 0xdb24d514da390055ull},
        {1, 0.2, 0x34c38cda64c8e6a7ull}, {1, 0.5, 0x0796df78cefc4f4dull}, {1, 0.8, 0x91e2eae55b02914eull},
        {2, 0.2, 0x25a7b2d98c8ead94ull}, {2, 0.5, 0xf79eace2ec206c69ull}, {2, 0.8, 0xd96999af1cf3424bull},
    };
    for (const Pin& p : pins) {
        render(sets[p.set], p.progress, 1.0, a.data(), b.data(), W, H, o.data());
        check(hashOf(o) == p.hash, "pinned picture");
    }
    // the ends: the outgoing picture, then the incoming one
    for (const Props& s : sets) {
        render(s, 0.0, 1.0, a.data(), b.data(), W, H, o.data());
        check(o == a, "progress 0 is the outgoing picture");
        render(s, 1.0, 1.0, a.data(), b.data(), W, H, o.data());
        check(o == b, "progress 1 is the incoming picture");
    }
    // layout counting: 2N + 1 steps; backdrop first, then N rectangles; alpha 1 x max for the finished ones, the step's fraction for the running one
    {
        Props p;                                                // N = 6, 0.85
        std::vector<Item> l = layout(p, 0.25, 1.0);             // 13 * 0.25 = 3.25: items 0..2 whole, item 3 a quarter
        check(l.size() == 7 && l[0].noBorder && !l[0].incoming, "backdrop is the outgoing picture before the middle step");
        check(l[1].alpha == (float)0.85 && l[3].alpha == (float)0.85 && std::fabs(l[4].alpha - 0.25f * 0.85f) < 1e-6f && l[5].alpha == 0.f, "alphas before the middle step");
        l = layout(p, 6.5 / 13.0, 1.0);                         // the middle step: both pictures over the whole frame, every rectangle whole
        check(l.size() == 8 && !l[0].incoming && l[1].incoming && l[1].noBorder && std::fabs(l[1].alpha - 0.5f) < 1e-6f && l[7].alpha == (float)0.85, "middle step");
        l = layout(p, 9.75 / 13.0, 1.0);                        // 9.75: the last two rectangles are gone, the one before fading out
        check(l.size() == 7 && l[0].incoming && l[1].alpha == (float)0.85 && l[3].alpha == (float)0.85 && std::fabs(l[4].alpha - 0.25f * 0.85f) < 1e-6f && l[5].alpha == 0.f, "alphas after the middle step");
        check(l[1].r[0] >= 0.f && l[1].r[2] <= 1.f && l[1].r[1] >= 0.f && l[1].r[3] <= 1.f, "rectangles stay in the frame");
    }
    std::printf("%d checks passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
