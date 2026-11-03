// Levels and Convolution Kernel as VEGAS's OpenCL kernels draw them (src/ofx/vfxlevelsconv.h): known answers of the float kernels, which were checked bit for bit against
// VEGAS's capture pictures (Levels 30 of 30, Convolution Kernel 42 of 42 exact with the 12-bit store).
#include "../../src/ofx/builtins_more_internal.h"
#include "../../src/ofx/vfxlevelsconv.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace reno::ofx;
static int fails = 0;
static void check(bool ok, const char* what) { if (!ok) { ++fails; std::printf("FAIL: %s\n", what); } }
static int lvl(float v) { return (int)std::lround(builtin::more::oclStore12(v) * 255.f); }
int main()
{
    // the 12-bit store is round-to-nearest of the 12-bit cut value with halves down
    bool same = true;
    for (int n = 0; n <= 4096; ++n) {
        const int viaSum = (n * 255 + 2047) >> 12;
        const int viaCeil = (int)std::ceil((double)n / 4096.0 * 255.0 - 0.5);
        if (viaSum != viaCeil) same = false;
    }
    check(same, "store rule: integer form equals ceil(v * 255 - 0.5) on 12-bit values");
    check(lvl(NAN) == 0 && lvl(-1.f) == 0 && lvl(2.f) == 255 && lvl(INFINITY) == 255, "store: not a number 0, clamped to 0..255");
    // Levels: Gamma 2 (all channels) at some levels
    {
        const vfxlc::LevelsSetup s = vfxlc::levelsSetup(0, 0, 1, 0, 1, 2.0);
        const int in[] = {0, 1, 10, 64, 128, 200, 255}, want[] = {0, 4, 34, 118, 175, 223, 255};
        bool ok = true;
        for (int i = 0; i < 7; ++i) ok = ok && lvl(vfxlc::levelsF(s, in[i] / 255.f)) == want[i];
        check(ok, "Levels Gamma 2 answers");
        const vfxlc::LevelsSetup g5 = vfxlc::levelsSetup(0, 0, 1, 0, 1, 5.0), g45 = vfxlc::levelsSetup(0, 0, 1, 0, 1, 0.45);
        check(lvl(vfxlc::levelsF(g5, 1 / 255.f)) == 21 && lvl(vfxlc::levelsF(g45, 128 / 255.f)) == 67, "Levels Gamma 5 and 0.45 (the straight toe and the power)");
    }
    // Levels: Gamma 1 is the straight map; a zero input range gives not-a-number in the kernel (the green goes to 0 in the 8-bit store)
    {
        const vfxlc::LevelsSetup s = vfxlc::levelsSetup(0, 0, 0.7, 0, 1, 1.0);
        check(lvl(vfxlc::levelsF(s, 0.35f)) == 127, "Levels Input End 0.7: 0.35 -> 0.5 (127.5 goes down)");
        const vfxlc::LevelsSetup z = vfxlc::levelsSetup(2, 1, 1, 1, 1, 5.0);
        const float p[4] = {0.5f, 0.5f, 0.5f, 1.f};
        float o[4];
        vfxlc::levelsPixel(z, p, o);
        check(o[0] == 0.5f && lvl(o[1]) == 0 && o[2] == 0.5f && o[3] == 1.f, "Levels green only, zero range: green 0, red and blue untouched");
        const float t[4] = {0.f, 0.f, 0.f, 0.f};
        vfxlc::levelsPixel(s, t, o);
        check(o[0] == 0.f && o[3] == 0.f, "Levels: a transparent pixel stays 0");
    }
    // Convolution Kernel: identity, and the 3 x 3 mean on a flat picture; the bottom row of the weights is the picture row below in the kernel's y-up numbering, i.e. above in ours
    {
        const double ident[9] = {0, 0, 0, 0, 1, 0, 0, 0, 0};
        const vfxlc::ConvSetup s = vfxlc::convSetup(ident, 0, 1, 0, true);
        const int W = 4, H = 3;
        std::vector<float> src((size_t)W * H * 4), out(src.size());
        for (int i = 0; i < W * H; ++i) { src[i * 4] = i / 16.f; src[i * 4 + 1] = 0.25f; src[i * 4 + 2] = 0.5f; src[i * 4 + 3] = 1.f; }
        vfxlc::convRows(s, src.data(), W, H, out.data(), 0, H);
        check(out == src, "Convolution Kernel: identity");
        double w[9] = {0, 1, 0, 0, 0, 0, 0, 0, 0};
        const vfxlc::ConvSetup t = vfxlc::convSetup(w, 0, 1, 0, false);
        vfxlc::convRows(t, src.data(), W, H, out.data(), 0, H);
        // Row1Column2 is A[7]: the tap at dy = +1 counted up, which is the picture row above (smaller y in Reno's rows); row 0 repeats itself
        check(out[(1 * W + 2) * 4] == src[(0 * W + 2) * 4] && out[(0 * W + 2) * 4] == src[(0 * W + 2) * 4], "Convolution Kernel: Row1Column2 reads the pixel above");
        const double many[9] = {-100, -100, -100, -100, -100, -100, -100, -100, -100};
        const vfxlc::ConvSetup m = vfxlc::convSetup(many, -360, -100, 0, false);
        check(m.A[0] == 10000.f && !m.alphaPlain, "Convolution Kernel: raw Scale -100 is used with Auto normalize off");
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
