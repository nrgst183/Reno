// Split as VEGAS's OpenCL kernels draw it (src/ofx/vfxsplit.h). Needs nothing but that header (and vfxwipes.h):
//
//   g++ -std=c++17 -O1 -ffp-contract=off -o vfxsplit_test tests/engine/vfxsplit_test.cpp && ./vfxsplit_test
//
// The pinned pixels are the float results of VEGAS's decoded kernels run on the CPU (an OpenCL shim, float32, no fused multiply-adds, the kernels' own four-read filter) with the
// arguments the DLL's host code works out: the header matched that oracle bit for bit on 190 random settings (all twelve kernels, the smoothing pass included). Against VEGAS's captured
// pictures the Push (In and Out) and Wipe (Out) settings are within one level (12 pictures, 6 exact); the other captured Split pictures turned out not to show the settings they are named
// after (several are the same picture), so border, feather, Squeeze and Wipe In rest on the oracle alone.
#include "../../src/ofx/vfxsplit.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace reno::ofx::builtin::wipes;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

static const int W = 40, H = 24;
static std::vector<float> gA, gB;     // Reno's order: the top row first

static void pictures()
{
    gA.assign((size_t)W * H * 4, 1.f);
    gB.assign((size_t)W * H * 4, 1.f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) {
                gA[((size_t)y * W + x) * 4 + c] = (float)((x * 7 + y * 3 + c * 11) % 256) / 255.0f;
                gB[((size_t)y * W + x) * 4 + c] = (float)((x * 5 + y * 13 + c * 29 + 100) % 256) / 255.0f;
            }
}

static std::vector<float> draw(const Split& s, bool hw)
{
    Split sp = s;
    sp.hwFilter = hw;
    Pic from{gA.data(), W, H, true}, to{gB.data(), W, H, true};
    const Pic& first = sp.swapPics ? to : from;
    Pic other = sp.swapPics ? from : to;
    std::vector<float> sm, out((size_t)W * H * 4);
    if (sp.smoothOther && !sp.onlyFirst) { smooth3x3(other, sm, nullptr); other = Pic{sm.data(), W, H, true}; }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) sp.pixel(first, other, x, y, &out[((size_t)y * W + x) * 4]);
    return out;
}

static void pinned(const std::vector<float>& o, const float (*want)[3], const char* name)
{
    static const int pts[5][2] = {{5, 3}, {20, 12}, {33, 20}, {12, 18}, {27, 6}};    // (x, y from the top)
    bool ok = true;
    for (int i = 0; i < 5; ++i)
        for (int c = 0; c < 3; ++c)
            if (!(std::fabs(o[((size_t)pts[i][1] * W + pts[i][0]) * 4 + c] - want[i][c]) <= 1e-7f)) ok = false;
    check(ok, name);
}

int main()
{
    pictures();
    const float black[4] = {0, 0, 0, 1}, yellow[4] = {1, 1, 0, 1}, red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    // Split(w, h, mode, direction, progress, centre x, centre y, border size, feather, colour)
    const Split t1(W, H, 0, 1, 0.5, 0.5, 0.5, 0, 0, black);
    const Split t2(W, H, 1, 0, 0.4, 0.3, 0.6, 0.1, 0.2, yellow);
    const Split t3(W, H, 2, 0, 0.6, 0.5, 0.5, 0, 0.3, black);
    const Split t4(W, H, 0, 1, 0.6, 0.5, 0.5, 0.2, 0, red);
    const Split t5(W, H, 2, 1, 0.45, 0.4, 0.5, 0.15, 0.1, green);
    check(t1.variant == 0 && t2.variant == 3 && t3.variant == 1 && t4.variant == 2 && t5.variant == 3, "kernel choice");
    check(!t1.smoothOther && !t2.smoothOther && t3.smoothOther && t5.smoothOther, "smoothing only for Squeeze under way");
    static const float w1[5][3] = {{0.517647088f, 0.56078434f, 0.603921592f}, {0.392156869f, 0.505882382f, 0.619607866f}, {0.796078444f, 0.839215696f, 0.882352948f}, {0.541176498f, 0.654901981f, 0.768627465f}, {0.223529413f, 0.337254912f, 0.450980395f}};
    static const float w2[5][3] = {{0.735876322f, 0.749645829f, 0.082616888f}, {0.690196097f, 0.733333349f, 0.776470602f}, {0.880764186f, 0.895052791f, 0.0349824168f}, {0.541176498f, 0.58431375f, 0.627451003f}, {0.811764717f, 0.854901969f, 0.898039222f}};
    static const float w3[5][3] = {{0.818503857f, 0.926313937f, 0.0824212581f}, {0.690196097f, 0.733333349f, 0.776470602f}, {0.778386891f, 0.886196911f, 0.0423042625f}, {0.0486485958f, 0.0525263809f, 0.0564041659f}, {0.0729728863f, 0.076850675f, 0.0807284638f}};
    static const float w4[5][3] = {{1.f, 0.f, 0.f}, {0.392156869f, 0.505882382f, 0.619607866f}, {1.f, 0.f, 0.f}, {0.541176498f, 0.654901981f, 0.768627465f}, {0.223529413f, 0.337254912f, 0.450980395f}};
    static const float w5[5][3] = {{0.381429076f, 0.424566329f, 0.467703581f}, {0.392156869f, 0.505882382f, 0.619607866f}, {0.92262125f, 0.965758562f, 0.193182677f}, {0.541176498f, 0.654901981f, 0.768627465f}, {0.f, 1.f, 0.f}};
    pinned(draw(t1, false), w1, "Push, neither border nor feather");
    pinned(draw(t2, false), w2, "Wipe In, border and feather");
    pinned(draw(t3, false), w3, "Squeeze In, feather, smoothed picture behind");
    pinned(draw(t4, false), w4, "Push, border only");
    pinned(draw(t5, false), w5, "Squeeze Out, border and feather, smoothed");

    // the card's filter stays within two levels of the four-read one
    for (const Split* s : {&t1, &t2, &t3, &t4, &t5}) {
        const std::vector<float> a = draw(*s, false), b = draw(*s, true);
        float worst = 0;
        for (size_t i = 0; i < a.size(); ++i) worst = std::fmax(worst, std::fabs(a[i] - b[i]));
        check(worst < 3.0f / 255.0f, "hardware filter near the software one");
    }

    // the ends of the progress: Out starts on the outgoing picture, In ends on it
    {
        const std::vector<float> o = draw(Split(W, H, 0, 1, 0.0, 0.5, 0.5, 0.1, 0.2, black), true);
        bool same = true;
        for (size_t i = 0; i < o.size(); ++i) if (o[i] != gA[i]) same = false;
        check(same, "Out starts on the first picture");
        const std::vector<float> e = draw(Split(W, H, 1, 0, 1.0, 0.5, 0.5, 0.1, 0.2, black), true);
        same = true;
        for (size_t i = 0; i < e.size(); ++i) if (e[i] != gB[i]) same = false;
        check(same, "In ends on the second picture");
    }

    // nonsense settings: every pixel stays a number, no crash
    {
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        const double odd[][7] = {{nan, nan, nan, nan, nan, 0.5, 0}, {inf, inf, inf, inf, inf, inf, 0}, {-1e30, -50, -3, -1, -2, -0.5, 0}, {1e30, 1e30, 1e9, 1e9, 1e30, 2.0, 0},
                                 {1, 1, 0.5, 0.5, 1, 1.0, 0}, {0, 0, 0, 1, 1, 0.5, 0}, {0.5, 0.5, 1, 1, 1, 0.0, 0}};
        bool finite = true;
        for (const auto& v : odd)
            for (int md = -1; md < 4; ++md)
                for (int dr = 0; dr < 2; ++dr)
                    for (int hw = 0; hw < 2; ++hw) {
                        const Split s(W, H, md, dr, v[5], v[0], v[1], v[2], v[3], yellow);
                        for (float f : draw(s, hw != 0)) if (!(f == f) || std::fabs(f) > 1e6f) finite = false;
                    }
        check(finite, "nonsense settings give finite pictures");
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
