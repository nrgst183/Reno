// Spiral as VEGAS's OpenCL kernels draw it (src/ofx/vfxspiral.h). Needs nothing but that header (and vfxwipes.h):
//
//   g++ -std=c++17 -O1 -ffp-contract=off -o vfxspiral_test tests/engine/vfxspiral_test.cpp && ./vfxspiral_test
//
// The pinned pixels are the float results of VEGAS's decoded kernels run on the CPU (an OpenCL shim, float32, no fused multiply-adds, the kernels' own four-read filter) with the
// arguments the DLL's host code works out: the header matched that oracle bit for bit on 80 + 80 random settings (all three used kernels, the smoothing pass included). With the card's own
// filter (the header's default) the 41 captured pictures are within one level, one exact.
#include "../../src/ofx/vfxspiral.h"
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

// the whole picture, top row first; hw: the card's filter
static std::vector<float> draw(const Spiral& s, bool hw)
{
    Spiral sp = s;
    sp.hwFilter = hw;
    Pic from{gA.data(), W, H, true}, to{gB.data(), W, H, true};
    const Pic& first = sp.swapPics ? to : from;
    Pic mover = sp.swapPics ? from : to;
    std::vector<float> sm, out((size_t)W * H * 4);
    if (sp.smoothMoved) { Spiral::smooth(mover, sm, nullptr); mover = Pic{sm.data(), W, H, true}; }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) sp.pixel(first, mover, x, H - 1 - y, &out[((size_t)y * W + x) * 4]);
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
    const float black[4] = {0, 0, 0, 1}, yellow[4] = {1, 1, 0, 1}, red[4] = {1, 0, 0, 1};
    // Spiral(w, h, turns, zoom %, orientation, motion, direction, progress, border size, feather, colour)
    const Spiral s1(W, H, 1, 50, 0, 0, 0, 0.5, 0, 0, black);
    const Spiral s2(W, H, 1, 50, 3, 1, 0, 0.4, 0, 0.2, yellow);
    const Spiral s3(W, H, 2.5, 40, 1, 0, 1, 0.6, 0.3, 0.1, red);
    const Spiral s4(W, H, 1, 0, 0, 0, 0, 0.5, 0, 0, black);
    check(s1.variant == 0 && s2.variant == 1 && s3.variant == 3 && s4.variant == 0, "kernel choice");
    check(!s1.smoothMoved && !s2.smoothMoved && !s3.smoothMoved && s4.smoothMoved, "smoothing only for a picture drawn at half size or less");
    static const float w1[5][3] = {{0.172549024f, 0.215686277f, 0.258823544f}, {0.690196097f, 0.733333349f, 0.776470602f}, {0.814379096f, 0.928104579f, 0.0379085168f}, {0.541176498f, 0.58431375f, 0.627451003f}, {0.709803879f, 0.823529363f, 0.937254786f}};
    static const float w2[5][3] = {{0.172549009f, 0.215686262f, 0.258823544f}, {0.770997524f, 0.802883983f, 0.573955595f}, {0.137254894f, 0.180392146f, 0.223529413f}, {0.541176498f, 0.58431375f, 0.627451003f}, {0.758128941f, 0.829269528f, 0.525956452f}};
    static const float w3[5][3] = {{0.643137276f, 0.75686276f, 0.870588243f}, {0.616387069f, 0.319264978f, 0.391037703f}, {1.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.223529398f, 0.337254912f, 0.450980395f}};
    static const float w4[5][3] = {{0.172549024f, 0.215686277f, 0.258823544f}, {0.690196097f, 0.733333349f, 0.776470602f}, {0.137254909f, 0.180392161f, 0.223529413f}, {0.541176498f, 0.58431375f, 0.627451003f}, {0.502941191f, 0.616666675f, 0.730392158f}};
    pinned(draw(s1, false), w1, "no border, no feather");
    pinned(draw(s2, false), w2, "feather only");
    pinned(draw(s3, false), w3, "border and feather, Direction Out");
    pinned(draw(s4, false), w4, "smoothed picture (Zoom 0)");

    // the card's filter stays within two levels of the four-read one
    for (const Spiral* s : {&s1, &s2, &s3, &s4}) {
        const std::vector<float> a = draw(*s, false), b = draw(*s, true);
        float worst = 0;
        for (size_t i = 0; i < a.size(); ++i) worst = std::fmax(worst, std::fabs(a[i] - b[i]));
        check(worst < 3.0f / 255.0f, "hardware filter near the software one");
    }

    // the outgoing picture is all there is at the start (Direction In) and the incoming one's frame is covered at the end
    {
        const Spiral a(W, H, 1, 50, 0, 0, 0, 0.0, 0, 0, black);
        const std::vector<float> o = draw(a, true);
        bool same = true;
        for (size_t i = 0; i < o.size(); ++i) if (o[i] != gA[i]) same = false;
        check(same, "start shows the outgoing picture");
        const Spiral e(W, H, 1, 50, 0, 0, 0, 1.0, 0, 0, black);
        const std::vector<float> q = draw(e, true);
        // (the picture is read at the pixel's place plus half a pixel, so each pixel is the mean of four of the incoming picture's)
        float worst = 0;
        for (int y = 2; y < H - 2; ++y)
            for (int x = 2; x < W - 2; ++x)
                for (int c = 0; c < 3; ++c) {
                    auto b = [&](int xx, int yy) { return gB[((size_t)yy * W + xx) * 4 + c]; };
                    const float mean = 0.25f * (b(x, y) + b(x + 1, y) + b(x, y - 1) + b(x + 1, y - 1));
                    worst = std::fmax(worst, std::fabs(q[((size_t)y * W + x) * 4 + c] - mean));
                }
        check(worst < 1e-5f, "end shows the incoming picture");
    }
    // Zoom 100 is a plain slide: nothing is smoothed
    check(!Spiral(W, H, 1, 100, 0, 0, 0, 0.5, 0, 0, black).smoothMoved, "no smoothing at Zoom 100");

    // nonsense settings: every pixel stays a number, no crash
    {
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        const double odd[][9] = {{nan, nan, 0, 0, 0, nan, nan, 0.5, 0}, {inf, inf, 9, 9, 9, inf, inf, inf, 0}, {-1e30, -50, -3, -1, 5, -2, -2, -0.5, 0},
                                 {1e30, 1e30, 1e9, 1e9, 1e9, 1e30, 1e30, 2.0, 0}, {1, 50, 0, 0, 0, 1, 1, 0.5, 0}, {1e6, 0, 2, 1, 1, 0.5, 0, 1.0, 0}};
        bool finite = true;
        for (const auto& v : odd)
            for (int hw = 0; hw < 2; ++hw) {
                const Spiral s(W, H, v[0], v[1], (int)std::fmax(-9, std::fmin(9, v[2])), (int)std::fmax(-9, std::fmin(9, v[3])), (int)std::fmax(-9, std::fmin(9, v[4])), v[7], v[5], v[6], yellow);
                for (float f : draw(s, hw != 0)) if (!(f == f) || std::fabs(f) > 1e6f) finite = false;
            }
        check(finite, "nonsense settings give finite pictures");
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
