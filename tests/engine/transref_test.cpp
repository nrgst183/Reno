// Checks details of VEGAS Pro 18's stock transitions that the built-ins follow (measured on VEGAS's own pictures, see
// tools/vegasref_compare.py), through the render functions: Clock Wipe, Slide, Push, Page Roll, Zoom, Iris.
//
//   transref_test (no arguments)
#include "../../src/core/base.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/builtins_impl.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::ofx::builtin;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

typedef std::map<std::string, std::vector<double>> Params;
static const std::string V = "com.vegascreativesoftware:";
static const int W = 320, H = 180;

static Buf run(const std::string& id, const Params& over, const Buf& a, const Buf& b, double progress)
{
    const Def* def = findDef(V + id);
    Buf none;
    if (!def) return none;
    Params store;
    std::map<std::string, std::string> strs;
    for (const PDef& p : def->params) {
        if (p.type == PT::Group || p.type == PT::Button) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : over) store[kv.first] = kv.second;
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
    c.time = 0;
    c.fps = 30;
    c.src = a;
    c.src2 = b;
    c.progress = progress;
    c.out.resize(a.w, a.h);
    c.frameW = a.w;
    c.frameH = a.h;
    def->render(c);
    return c.out;
}

static Buf flat(float v)
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) { float* p = b.at(x, y); p[0] = p[1] = p[2] = v; p[3] = 1; }
    return b;
}
// red: the column, green: the row (so that where a pixel was taken from can be read off)
static Buf coded()
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) { float* p = b.at(x, y); p[0] = (x + 0.5f) / W; p[1] = (y + 0.5f) / H; p[2] = 0.5f; p[3] = 1; }
    return b;
}
static float at(const Buf& b, int x, int y, int k = 0) { return b.w > x && b.h > y ? b.at(x, y)[k] : -1.f; }
static std::string num(double v) { char s[32]; std::snprintf(s, sizeof s, "%.3f", v); return s; }

int main()
{
    const Buf black = flat(0), white = flat(1), code = coded();
    {
        // Clock Wipe, hard edge, half way: the incoming picture covers the right half. The 12 o'clock ray runs between the
        // columns 159 and 160, the hand (straight down) through column 161 at 0.21: VEGAS's pixel there is 79 % incoming.
        const Buf o = run("clockwipe", {{"FeatherAngle", {0}}}, black, white, 0.5);
        check(std::fabs(at(o, 161, 140) - 0.79f) < 0.02f, "Clock Wipe: the hard edge is anti-aliased (column 161 below the centre)", num(at(o, 161, 140)));
        check(at(o, 160, 140) < 0.01f && at(o, 162, 140) > 0.99f, "  the columns either side are whole");
        check(at(o, 160, 40) > 0.99f && at(o, 159, 40) < 0.01f, "  above the centre the edge lies between the columns 159 and 160");
        const Buf cc = run("clockwipe", {{"FeatherAngle", {0}}, {"Direction", {1}}}, black, white, 0.5);
        check(std::fabs(at(cc, 159, 140) - 0.79f) < 0.02f && at(cc, 161, 40) < 0.01f && at(cc, 160, 40) > 0.99f, "  counter-clockwise is the mirror picture about x = 160.5");
    }
    {
        // Slide with Feather 1 (no size), half way from the left: the frame has travelled e = F (w + F) / W = 101 pixels more,
        // so its right edge is at 210.6, where the (black) feather ring is solid; without the extra travel it would be at 160.
        const Buf o = run("slide", {{"Angle", {0}}, {"Direction", {0}}, {"BorderSize", {0}}, {"BorderFeather", {1}}, {"BorderColor", {0, 0, 0}}}, white, white, 0.5);
        check(at(o, 210, 90) < 0.03f, "Slide: with Feather the frame travels further (the ring's middle at x = 210)", num(at(o, 210, 90)));
        check(at(o, 160, 90) > 0.1f, "  and not at x = 160", num(at(o, 160, 90)));
    }
    {
        // Push to the left with a border: only a hard border makes the incoming frame start a pixel early
        const double L = W, w = 0.05 * L, p = 0.5, seam = (L + 2 * w) - p * (L + 3 * w);
        const int x = (int)seam + 20;
        const Buf hard = run("push", {{"Direction", {2}}, {"BorderSize", {0.05}}, {"BorderFeather", {0}}}, black, code, p);
        const Buf soft = run("push", {{"Direction", {2}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.02}}}, black, code, p);
        const double want0 = (x + 0.5 - seam) / W, want1 = (x + 0.5 - seam + 1) / W;
        check(std::fabs(at(hard, x, 90) - want1) < 1e-3, "Push: a hard border has the incoming frame a pixel early", num(at(hard, x, 90) * W));
        check(std::fabs(at(soft, x, 90) - want0) < 1e-3, "  a soft one does not", num(at(soft, x, 90) * W));
    }
    {
        // Page Roll from the right edge with Slide amount 1: the flat page is pulled by sig = progress * H - pi/2 R into the roll
        const double R = 0.1 * H, sig = 0.5 * H - 0.5 * 3.14159265358979 * R;
        const Buf o = run("pageroll", {{"RollAngle", {0}}, {"FoldRadius", {0.1}}, {"SlideAmount", {1}}}, code, black, 0.5);
        const double want = (100 + 0.5 - sig) / W;
        check(std::fabs(at(o, 100, 90) - want) < 2e-3, "Page Roll: Slide amount pulls the flat page into the roll", num(at(o, 100, 90) * W) + " / " + num(want * W));
        const Buf o0 = run("pageroll", {{"RollAngle", {0}}, {"FoldRadius", {0.1}}, {"SlideAmount", {0}}}, code, black, 0.5);
        check(std::fabs(at(o0, 100, 90) - 100.5 / W) < 2e-3, "  without it the flat page stays");
    }
    {
        // Zoom in a quarter of the way: the incoming picture of one-pixel stripes is shrunk to a quarter with a box filter
        Buf stripes = flat(0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; x += 2) { float* q = stripes.at(x, y); q[0] = q[1] = q[2] = 1; }
        const Buf o = run("zoom", {{"Direction", {0}}}, black, stripes, 0.25);
        float lo = 1, hi = 0;
        for (int x = 150; x < 170; ++x) { lo = std::min(lo, at(o, x, 90)); hi = std::max(hi, at(o, x, 90)); }
        check(lo > 0.45f && hi < 0.55f, "Zoom: the shrunk picture is box-filtered (stripes average to grey)", num(lo) + " .. " + num(hi));
    }
    {
        // Iris with 6 x 6 cells: VEGAS's pictures (matched exactly) put the sixth cell's circle about x = 292
        const Buf o = run("iris", {{"Shape", {0}}, {"Direction", {1}}, {"RepeatX", {6}}, {"RepeatY", {6}}}, black, white, 0.05);
        check(at(o, 292, 15) > 0.5f && at(o, 289, 15) < 0.1f && at(o, 295, 15) < 0.1f, "Iris: the sixth circle opens about x = 292", num(at(o, 292, 15)));
    }
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
