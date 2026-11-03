// Barn Door, Push and Slide as VEGAS's OpenCL kernels draw them (src/ofx/vfxwipes.h). Needs nothing but that header:
//
//   g++ -std=c++17 -O1 -ffp-contract=off -o vfxwipes_test tests/engine/vfxwipes_test.cpp && ./vfxwipes_test
//
// The pinned pixels are the float results of VEGAS's decoded kernels run on the CPU (an OpenCL shim, float32, no fused multiply-adds) with the settings the DLL's host code
// hands them; the header matched that oracle bit for bit on 35 + 34 + 33 captured settings and 450 random ones (picture sizes 6 x 6 .. 80 x 60). The checks here keep the
// maths from drifting: pinned pixels, and the pictures at the ends of the progress.
#include "../../src/ofx/vfxwipes.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno::ofx::builtin::wipes;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++gPass; else { ++gFail; std::printf("FAIL %s\n", name); }
}

static const int W = 40, H = 24;
static std::vector<float> gA, gB;

static void pictures()
{
    gA.assign((size_t)W * H * 4, 1.f);
    gB.assign((size_t)W * H * 4, 1.f);
    for (int y = 0; y < H; ++y)       // (rows top first, as Reno's pictures are)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) {
                gA[((size_t)y * W + x) * 4 + c] = (float)((x * 7 + y * 3 + c * 11) % 256) / 255.0f;
                gB[((size_t)y * W + x) * 4 + c] = (float)((x * 5 + y * 13 + c * 29 + 100) % 256) / 255.0f;
            }
}

template <class K>
static std::vector<float> render(const K& k)
{
    Pic from{gA.data(), W, H, true}, to{gB.data(), W, H, true};
    std::vector<float> o((size_t)W * H * 4);
    for (int ky = 0; ky < H; ++ky)    // (the kernel's rows run from the bottom)
        for (int x = 0; x < W; ++x) k.pixel(from, to, x, ky, &o[((size_t)(H - 1 - ky) * W + x) * 4]);
    return o;
}

static bool same(const std::vector<float>& a, const std::vector<float>& b)
{
    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > 1e-6f) return false;
    return true;
}

static void pinned(const std::vector<float>& o, const float (*want)[3], const char* name)
{
    static const int pts[5][2] = {{5, 3}, {20, 12}, {33, 20}, {12, 18}, {27, 6}};
    bool ok = true;
    for (int i = 0; i < 5; ++i)
        for (int c = 0; c < 3; ++c)
            if (std::fabs(o[((size_t)pts[i][1] * W + pts[i][0]) * 4 + c] - want[i][c]) > 2e-6f) ok = false;
    check(ok, name);
}

int main()
{
    pictures();
    const float col[4] = {0.9f, 0.2f, 0.4f, 1.0f};
    const float none[4] = {0.f, 0.f, 0.f, 1.f};

    {
        static const float w1[5][3] = {{0.868742347f, 0.200674027f, 0.393933833f}, {0.690196097f, 0.733333349f, 0.776470602f}, {0.378592223f, 0.1865962f, 0.279365808f},
                                       {0.541176498f, 0.58431375f, 0.627451003f}, {0.811764717f, 0.854901969f, 0.898039222f}};
        pinned(render(BarnDoor(W, H, 0, 0, 0.3, 0.1, 0.2, col)), w1, "Barn Door, In, border and feather, pinned pixels");
        static const float w2[5][3] = {{0.899999976f, 0.200000003f, 0.400000006f}, {0.392156869f, 0.505882382f, 0.619607866f}, {0.048313722f, 0.0914509743f, 0.134588242f},
                                       {0.541176498f, 0.654901981f, 0.768627465f}, {0.223529413f, 0.337254912f, 0.450980395f}};
        pinned(render(BarnDoor(W, H, 1, 1, 0.6, 0.05, 0.0, col)), w2, "Barn Door, Out, vertical, border, pinned pixels");
        static const float p1[5][3] = {{0.180392131f, 0.29411763f, 0.407843113f}, {0.396633834f, 0.263406813f, 0.343771338f}, {0.690980434f, 0.734117687f, 0.777254939f},
                                       {0.899999976f, 0.200000003f, 0.400000006f}, {0.36156863f, 0.404705882f, 0.447843164f}};
        pinned(render(Push(W, H, 3, 0.4, 0.1, 0.15, true, col)), p1, "Push right, push off previous, border and feather, pinned pixels");
        static const float p2[5][3] = {{0.172549024f, 0.215686277f, 0.258823544f}, {0.225883082f, 0.138823494f, 0.252548993f}, {0.687843084f, 0.801568568f, 0.915294051f},
                                       {0.174117625f, 0.287843108f, 0.401568592f}, {0.811764717f, 0.854901969f, 0.898039222f}};
        pinned(render(Push(W, H, 0, 0.7, 0.0, 0.0, false, col)), p2, "Push up, no border, pinned pixels");
        static const float s1[5][3] = {{0.538062274f, 0.651787758f, 0.765513241f}, {0.850712061f, 0.325292706f, 0.488441914f}, {0.137254894f, 0.180392161f, 0.223529413f},
                                       {0.857174635f, 0.24586758f, 0.427146137f}, {0.811764717f, 0.854901969f, 0.898039222f}};
        pinned(render(Slide(W, H, 30, 0, 0.45, 0.1f, 0.2f, col)), s1, "Slide In, border and feather, pinned pixels");
        static const float s2[5][3] = {{0.643137276f, 0.75686276f, 0.870588243f}, {0.337797493f, 0.380934775f, 0.424072027f}, {0.788777947f, 0.83191514f, 0.875052392f},
                                       {0.188777894f, 0.231915176f, 0.275052458f}, {0.459366173f, 0.502503395f, 0.545640707f}};
        pinned(render(Slide(W, H, 200, 1, 0.3, 0.0, 0.0, col)), s2, "Slide Out, no border, pinned pixels");
    }

    // the ends of the progress: without border and feather the outgoing picture at 0 and the incoming one at 1 (Out: the other way round). Barn Door leaves the
    // one line at its middle to the other picture at the very ends (the kernel's test is "< half + 0.0001"), counted here.
    {
        std::vector<float> a(gA), b(gB);
        auto differing = [](const std::vector<float>& x, const std::vector<float>& y) {
            int n = 0;
            for (size_t i = 0; i < x.size(); i += 4)
                if (std::fabs(x[i] - y[i]) > 1e-6f || std::fabs(x[i + 1] - y[i + 1]) > 1e-6f || std::fabs(x[i + 2] - y[i + 2]) > 1e-6f) ++n;
            return n;
        };
        check(differing(render(BarnDoor(W, H, 0, 0, 0.0, 0, 0, none)), a) == 0, "Barn Door In at 0 shows the outgoing picture");
        check(differing(render(BarnDoor(W, H, 0, 0, 1.0, 0, 0, none)), b) == H, "Barn Door In at 1 shows the incoming picture but its middle column");
        check(differing(render(BarnDoor(W, H, 1, 0, 1.0, 0, 0, none)), b) == W, "Barn Door vertical In at 1 shows the incoming picture but its middle row");
        check(differing(render(BarnDoor(W, H, 0, 1, 0.0, 0, 0, none)), a) == H, "Barn Door Out at 0 shows the outgoing picture but its middle column");
        for (int dir = 0; dir < 4; ++dir) {
            check(same(render(Push(W, H, dir, 0.0, 0, 0, false, none)), a), "Push at 0 shows the outgoing picture");
            check(same(render(Push(W, H, dir, 1.0, 0, 0, false, none)), b), "Push at 1 shows the incoming picture");
        }
        // Slide reads the moved picture half a pixel off its place (VEGAS's pictures show it), so the ends are checked on two flat pictures
        for (size_t i = 0; i < gA.size(); i += 4) { gA[i] = gA[i + 1] = gA[i + 2] = 0.25f; gB[i] = gB[i + 1] = gB[i + 2] = 0.75f; }
        a = gA; b = gB;
        check(same(render(Slide(W, H, 45, 0, 0.0, 0, 0, none)), a), "Slide In at 0 shows the outgoing picture");
        check(same(render(Slide(W, H, 45, 0, 1.0, 0, 0, none)), b), "Slide In at 1 shows the incoming picture");
        check(same(render(Slide(W, H, 90, 1, 0.0, 0, 0, none)), a), "Slide Out at 0 shows the outgoing picture");
        check(same(render(Slide(W, H, 90, 1, 1.0, 0, 0, none)), b), "Slide Out at 1 shows the incoming picture");
    }

    // the border colour shows where the two pictures meet
    {
        const std::vector<float> o = render(Push(W, H, 3, 0.5, 0.2, 0.0, false, col));
        int hit = 0;
        for (size_t i = 0; i < o.size(); i += 4)
            if (o[i] == col[0] && o[i + 1] == col[1] && o[i + 2] == col[2]) ++hit;
        check(hit >= H * 7, "Push with a border: a colour band across the frame");
    }

    std::printf("vfxwipes_test: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
