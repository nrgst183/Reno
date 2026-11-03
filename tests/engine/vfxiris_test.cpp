// Iris as VEGAS's OpenCL kernels draw it (src/ofx/vfxiris.h). Needs nothing but that header (and vfxwipes.h):
//
//   g++ -std=c++17 -O1 -ffp-contract=off -o vfxiris_test tests/engine/vfxiris_test.cpp && ./vfxiris_test
//
// The pinned pixels are the float results of VEGAS's decoded kernels (ellipse / rectangle / triangle / arrowhead families) run on the CPU through an OpenCL shim (float32, no fused
// multiply-adds) with the arguments the DLL's host code works out; the header matched that oracle bit for bit on 380 random settings (every shape, repeats 1 .. 10, centres on and
// off the edges, all four feather / border cases, both directions). The largest-distance value of each case follows the host's own edge scan (it counts the rows from the top);
// the 48 captured pictures are within one level, 22 of them exact.
#include "../../src/ofx/vfxiris.h"
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

// the whole picture, top row first
static std::vector<float> draw(const Iris& s, int w = W, int h = H, const float* a = nullptr, const float* b = nullptr)
{
    Pic from{a ? a : gA.data(), w, h, true}, to{b ? b : gB.data(), w, h, true};
    const Pic& first = s.swapPics ? to : from;
    const Pic& second = s.swapPics ? from : to;
    std::vector<float> out((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) s.pixel(first, second, x, h - 1 - y, &out[((size_t)y * w + x) * 4]);
    return out;
}

struct Pin { int x, y; float rgb[3]; };      // (x, y from the top)
struct Case {
    int shape, rx, ry, dir;
    double cx, cy, feather, border;
    float col[3];
    double p;
    float maxDist;
    Pin pins[6];
};
static const Case kCases[] = {
    {0, 1, 1, 1, 0.5, 0.5, 0, 0, {0, 0, 0}, 0.5, 23.3238068f, {{14, 1, {0.55686754f, 0.635299981f, 0.713732362f}}, {10, 5, {0.588242888f, 0.666675329f, 0.243131995f}}, {10, 17, {0.462744743f, 0.541177213f, 0.619609594f}}, {14, 21, {0.68235445f, 0.760786891f, 0.839219332f}}, {1, 8, {0.121568628f, 0.164705887f, 0.20784314f}}, {38, 15, {0.215686277f, 0.258823544f, 0.301960796f}}}},   // t1
    {3, 1, 1, 0, 0.5, 0.5, 0.5, 0.1, {1, 1, 0}, 0.4, 12.0f, {{8, 4, {1.0f, 1.0f, 0.0f}}, {27, 4, {0.901176453f, 0.921307206f, 0.408104569f}}, {32, 11, {1.0f, 1.0f, 0.0f}}, {14, 18, {0.811503291f, 0.831633985f, 0.318431348f}}, {5, 8, {0.898039222f, 0.00784313772f, 0.121568628f}}, {36, 15, {0.858823538f, 0.972549021f, 0.0823529437f}}}},   // t2
    {6, 2, 2, 1, 0.3, 0.6, 0, 0.2, {1, 0, 0}, 0.6, 16.1946716f, {{14, 0, {1.0f, 0.0f, 0.0f}}, {15, 2, {1.0f, 0.0f, 0.0f}}, {38, 7, {1.0f, 0.0f, 0.0f}}, {17, 14, {1.0f, 0.0f, 0.0f}}, {2, 8, {0.839215696f, 0.952941179f, 0.0627451017f}}, {2, 16, {0.243137255f, 0.356862754f, 0.470588237f}}}},   // t3
    {11, 3, 2, 1, 0.5, 0.5, 0.2, 0, {0, 1, 0}, 0.55, 7.91568089f, {{7, 0, {0.394128501f, 0.479538858f, 0.564949274f}}, {26, 7, {0.471984446f, 0.557394862f, 0.642805219f}}, {7, 12, {0.215907991f, 0.301318377f, 0.386728764f}}, {26, 19, {0.894981802f, 0.980392158f, 0.0618809685f}}, {36, 7, {0.450980395f, 0.564705908f, 0.678431392f}}, {3, 16, {0.262745112f, 0.376470596f, 0.490196079f}}}},   // t4
    {10, 1, 1, 1, 0.5, 1, 0, 0.15, {0, 0, 0}, 0.5, 27.8166866f, {{3, 1, {0.0f, 0.0f, 0.0f}}, {0, 7, {0.0f, 0.0f, 0.0f}}, {14, 18, {0.0f, 0.0f, 0.0f}}, {8, 21, {0.0f, 0.0f, 0.0f}}, {39, 7, {0.509803951f, 0.623529434f, 0.737254918f}}, {32, 15, {0.78039217f, 0.894117653f, 0.00392156886f}}}},   // t5
    {1, 1, 1, 0, 0, 0, 0.3, 0, {1, 1, 1}, 0.45, 33.5195465f, {{0, 5, {0.0980410874f, 0.145884454f, 0.193727821f}}, {12, 6, {0.680510879f, 0.232754096f, 0.313017786f}}, {22, 10, {0.490007877f, 0.574826062f, 0.659644246f}}, {29, 16, {0.81114471f, 0.729032993f, 0.829893231f}}, {0, 8, {0.0941176489f, 0.137254909f, 0.180392161f}}, {2, 16, {0.243137255f, 0.286274523f, 0.329411775f}}}},   // t6
    {13, 2, 3, 0, 0.7, 0.4, 0, 0.3, {1, 0, 1}, 0.5, 10.1964712f, {{9, 0, {0.634235024f, 0.578573823f, 0.827092886f}}, {24, 5, {1.0f, 0.0f, 1.0f}}, {25, 10, {1.0f, 0.0f, 1.0f}}, {27, 17, {0.817108929f, 0.761491954f, 0.158701003f}}, {0, 8, {0.800000012f, 0.913725495f, 0.0235294122f}}, {39, 15, {0.243137255f, 0.286274523f, 0.329411775f}}}},   // t7
    {8, 1, 1, 1, 0.5, 0.5, 0.4, 0.25, {0, 0, 1}, 0.5, 26.5433025f, {{0, 0, {0.0f, 0.0f, 1.0f}}, {36, 6, {0.159371406f, 0.204682887f, 0.851565838f}}, {7, 12, {0.0f, 0.0f, 1.0f}}, {10, 18, {0.0486471616f, 0.0529626384f, 0.957237601f}}, {37, 7, {0.0941176489f, 0.137254909f, 0.180392161f}}, {38, 15, {0.215686277f, 0.258823544f, 0.301960796f}}}},   // t8
};

static void pinnedCases()
{
    int n = 0;
    for (const Case& c : kCases) {
        const float col[4] = {c.col[0], c.col[1], c.col[2], 1.f};
        const Iris s(W, H, c.shape, c.rx, c.ry, c.p, c.dir, c.cx, c.cy, c.feather, c.border, col);
        const std::vector<float> o = draw(s);
        bool ok = true;
        for (const Pin& pn : c.pins)
            for (int k = 0; k < 3; ++k)
                if (!(std::fabs(o[((size_t)pn.y * W + pn.x) * 4 + k] - pn.rgb[k]) <= 1e-7f)) ok = false;
        char name[64];
        std::snprintf(name, sizeof name, "pinned pixels, case %d (shape %d)", n, c.shape);
        check(ok, name);
        std::snprintf(name, sizeof name, "largest distance, case %d", n);
        check(std::fabs(s.maxDist - c.maxDist) <= 1e-5f * c.maxDist, name);
        ++n;
    }
}

static void geometry()
{
    const float col[4] = {0, 0, 0, 1};
    // a circle in the middle: the largest distance is the corner's, from the centre (20, 12): sqrt(20^2 + 12^2)
    const Iris c(W, H, 0, 1, 1, 0.5, 1, 0.5, 0.5, 0.0, 0.0, col);
    check(std::fabs(c.maxDist - std::sqrt(20.0f * 20.0f + 12.0f * 12.0f)) < 1e-4f, "circle: largest distance is the corner's");
    check(c.cellW == W && c.cellH == H, "one cell is the frame");
    check(std::fabs(c.distance(20, 12)) < 1e-6f, "circle: zero at the centre");
    check(std::fabs(c.distance(30, 12) - 10.0f) < 1e-5f, "circle: ten pixels along a row");
    // Center y counts from the top: a circle at y = 1 has its centre on the row above the kernels' top row (row H)
    const Iris t(W, H, 0, 1, 1, 0.5, 1, 0.5, 1.0, 0.0, 0.0, col);
    check(std::fabs(t.distance(20, H)) < 1e-5f && t.distance(20, 0) > 20.0f, "Center y = 1 is the top edge");
    // the other shapes' measures
    const Iris d(W, H, 4, 1, 1, 0.5, 1, 0.5, 0.5, 0.0, 0.0, col);
    check(std::fabs(d.distance(25, 15) - 8.0f) < 1e-5f, "diamond: |dx| + |dy|");
    const Iris sq(W, H, 5, 1, 1, 0.5, 1, 0.5, 0.5, 0.0, 0.0, col);
    check(std::fabs(sq.distance(25, 15) - 5.0f) < 1e-5f, "square: max(|dx|, |dy|)");
    const Iris rc(W, H, 3, 1, 1, 0.5, 1, 0.5, 0.5, 0.0, 0.0, col);
    check(std::fabs(rc.distance(25, 12) - 5.0f * rc.ratio) < 1e-5f && std::fabs(rc.distance(20, 15) - 3.0f) < 1e-5f, "rectangle: dy against dx scaled by the cell ratio");
    // repeats: a nine-cell lattice gives cells of 13 x 8 pixels, and a pixel in the middle of a cell is nearest its own iris
    const Iris r3(W, H, 0, 3, 3, 0.5, 1, 0.5, 0.5, 0.0, 0.0, col);
    check(r3.cellW == 13 && r3.cellH == 8, "repeat 3 x 3: whole-pixel cells");
    check(r3.distance(6, 4) < 1.0f && r3.distance(19, 12) < 2.0f, "repeat 3 x 3: irises at the middle of each cell");
}

static void blending()
{
    const float col[4] = {1, 0, 0, 1};
    // progress 0 shows the outgoing picture everywhere but at the exact centre (distance 0 is not above the threshold); at 1 the centre shows the incoming one
    const Iris s0(W, H, 0, 1, 1, 0.0, 1, 0.5, 0.5, 0.0, 0.0, col), s1(W, H, 0, 1, 1, 1.0, 1, 0.5, 0.5, 0.0, 0.0, col);
    const std::vector<float> o0 = draw(s0), o1 = draw(s1);
    bool same0 = true;
    const size_t mid = ((size_t)(H - 1 - 12) * W + 20) * 4;
    for (size_t i = 0; i < o0.size(); i += 4)
        if (i != mid && (o0[i] != gA[i] || o0[i + 1] != gA[i + 1] || o0[i + 2] != gA[i + 2])) same0 = false;
    check(same0, "progress 0 shows the outgoing picture");
    check(o1[mid] == gB[mid] && o1[mid + 1] == gB[mid + 1], "progress 1 shows the incoming picture at the centre");
    // Direction 0 swaps the two pictures and runs the progress backwards: its frame at 1 - p equals the other direction's frame at p drawn from the exchanged pictures
    const Iris a(W, H, 4, 1, 1, 0.3, 1, 0.5, 0.5, 0.2, 0.1, col), b(W, H, 4, 1, 1, 0.7, 0, 0.5, 0.5, 0.2, 0.1, col);
    check(!a.swapPics && b.swapPics && std::fabs(a.threshold - 0.3f) < 1e-6f && std::fabs(b.threshold - 0.3f) < 1e-6f, "Direction 0 swaps and reverses");
    const std::vector<float> oa = draw(a, W, H, gB.data(), gA.data()), ob = draw(b);
    bool eq = true;
    for (size_t i = 0; i < oa.size(); ++i) if (oa[i] != ob[i]) eq = false;
    check(eq, "Direction 0 at 1 - p is Direction 1 at p, pictures exchanged");
    // a border colour shows between the pictures; its alpha is one
    const Iris bd(W, H, 0, 1, 1, 0.5, 1, 0.5, 0.5, 0.0, 1.0, col);
    const std::vector<float> ob2 = draw(bd);
    bool sawBorder = false;
    for (size_t i = 0; i < ob2.size(); i += 4)
        if (ob2[i] == 1.f && ob2[i + 1] == 0.f && ob2[i + 2] == 0.f && ob2[i + 3] == 1.f) sawBorder = true;
    check(sawBorder, "the border colour shows");
}

static void nonsense()
{
    const float col[4] = {0.5f, 0.5f, 0.5f, 1.f};
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    const double vals[] = {nan, inf, -inf, 1e300, -1e300, -5.0, 5.0, 0.0, 0.5};
    int bad = 0;
    for (double v : vals)
        for (int shape = -3; shape <= 16; ++shape) {
            const bool small = v == v && std::fabs(v) < 100;
            const Iris s(W, H, shape, small ? (int)v : 3, 12, v, small ? (int)v : 1, v, v, v, v, col);
            const std::vector<float> o = draw(s);
            for (float f : o) if (!(f == f)) { ++bad; break; }
        }
    check(bad == 0, "nonsense parameters give numbers");
    // frames too small for their cells, and one pixel wide
    const int sizes[][2] = {{1, 1}, {2, 2}, {3, 1}, {1, 5}, {7, 3}};
    bad = 0;
    for (const auto& sz : sizes) {
        std::vector<float> a((size_t)sz[0] * sz[1] * 4, 0.25f), b((size_t)sz[0] * sz[1] * 4, 0.75f);
        for (int shape = 0; shape <= 13; ++shape) {
            const Iris s(sz[0], sz[1], shape, 10, 10, 0.5, 1, 0.5, 0.5, 0.3, 0.3, col);
            const std::vector<float> o = draw(s, sz[0], sz[1], a.data(), b.data());
            for (float f : o) if (!(f == f)) { ++bad; break; }
        }
    }
    check(bad == 0, "tiny frames survive");
}

int main()
{
    pictures();
    pinnedCases();
    geometry();
    blending();
    nonsense();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail != 0;
}
