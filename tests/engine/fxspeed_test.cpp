// Compares the faster built-in effects (Min and Max, Defocus, Light Rays, Black Bar Fill; src/ofx/builtins_fast*.cpp)
// with the plain implementation (tests/engine/fxspeed_ref.inc). Min and Max must match exactly, the others within a
// tolerance on the mean and the largest difference in 8-bit levels.
//
//   fxspeed_test [--full] [--time] [--fast] [<a video (1080p) to take a frame from>] [<second video>]
//   --full: pictures of 1920 x 1080 (the plain versions take minutes), else 480 x 270.
//   --fast: only the fast versions, timed (no comparison).   --only <text>: just the cases with this text in their name.
#include "../../src/media/media.h"
#include "../../src/ofx/builtins_impl.h"
#include "../../src/ofx/builtins_fast.h"
#include "../../src/core/base.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::ofx::builtin;

#include "fxspeed_ref.inc"

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}
static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------ pictures
// A picture to try the effects on: smooth colour, hard edges, noise, bright spots, a soft-alpha corner.
static Buf syntheticPicture(int W, int H)
{
    Buf b;
    b.resize(W, H);
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.f; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = b.at(x, y);
            float r = 0.2f + 0.6f * x / W, g = 0.3f + 0.5f * y / H, bl = 0.5f + 0.3f * std::sin(x * 0.05f) * std::cos(y * 0.04f);
            if (((x / 40) + (y / 30)) % 2 == 0) { r *= 0.5f; bl = 1.f - bl; }            // blocks with hard edges
            const float dx = x - W * 0.7f, dy = y - H * 0.3f;
            if (dx * dx + dy * dy < (H * 0.12f) * (H * 0.12f)) { r = 1.f; g = 0.95f; bl = 0.8f; }       // a bright disc
            r += (rnd() - 0.5f) * 0.1f; g += (rnd() - 0.5f) * 0.1f; bl += (rnd() - 0.5f) * 0.1f;
            r = std::min(1.f, std::max(0.f, r)); g = std::min(1.f, std::max(0.f, g)); bl = std::min(1.f, std::max(0.f, bl));
            // (8-bit steps like a decoded video's)
            r = std::floor(r * 255.f + 0.5f) / 255.f; g = std::floor(g * 255.f + 0.5f) / 255.f; bl = std::floor(bl * 255.f + 0.5f) / 255.f;
            float a = 1.f;
            if (x < W / 6 && y > H * 5 / 6) a = 0.25f + 0.5f * x / (W / 6.f);          // soft alpha corner
            p[0] = r * a; p[1] = g * a; p[2] = bl * a; p[3] = a;
        }
    return b;
}

// A frame of a video, reduced (by averaging) to W x H, as 8-bit steps.
static bool videoPicture(const std::string& path, double t, int W, int H, Buf& out)
{
    std::string err;
    auto r = openMedia(path, &err);
    if (!r) return false;
    ImagePtr img = r->videoFrame(t, 1920, 1080);
    if (!img || img->depth != PixelDepth::Byte || img->w < W || img->h < H) return false;
    out.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int x0 = x * img->w / W, x1 = std::max(x0 + 1, (x + 1) * img->w / W), y0 = y * img->h / H, y1 = std::max(y0 + 1, (y + 1) * img->h / H);
            double s[4] = {0, 0, 0, 0};
            int n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* row = img->row(img->h - 1 - yy);
                for (int xx = x0; xx < x1; ++xx, ++n) { s[0] += row[xx * 4 + 2]; s[1] += row[xx * 4 + 1]; s[2] += row[xx * 4]; s[3] += row[xx * 4 + 3]; }
            }
            float* p = out.at(x, y);
            for (int k = 0; k < 4; ++k) p[k] = (float)(std::floor(s[k] / n + 0.5) / 255.0);
        }
    return true;
}

// ------------------------------------------------------------------ the cases
typedef std::map<std::string, std::vector<double>> Params;
struct Case {
    std::string name, id;
    Params params;
    void (*ref)(Ctx&);
    double meanTol, maxTol;         // in 8-bit levels
};

static Ctx makeCtx(const Def& def, const Params& over, const Buf& src, Params& store)
{
    store.clear();
    for (const PDef& p : def.params) store[p.name] = p.def;
    for (const auto& kv : over) store[kv.first] = kv.second;
    Ctx c;
    c.testParams = &store;
    c.src = src;
    c.out.resize(src.w, src.h);
    c.frameW = src.w;
    c.frameH = src.h;
    return c;
}

struct Diff { double mean[4], mx[4]; double meanAll, mxAll; };
static Diff compare(const Buf& a, const Buf& b)
{
    Diff d;
    for (int k = 0; k < 4; ++k) { d.mean[k] = 0; d.mx[k] = 0; }
    const size_t n = (size_t)a.w * a.h;
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 4; ++k) {
            // (what the host would store: 0..1, rounded to a byte)
            auto q = [](float v) { return std::floor(std::min(1.f, std::max(0.f, v)) * 255.f + 0.5f); };
            const double df = std::fabs((double)q(a.px[i * 4 + k]) - q(b.px[i * 4 + k]));
            d.mean[k] += df;
            d.mx[k] = std::max(d.mx[k], df);
        }
    d.meanAll = d.mxAll = 0;
    for (int k = 0; k < 4; ++k) { d.mean[k] /= (double)n; d.meanAll = std::max(d.meanAll, d.mean[k]); d.mxAll = std::max(d.mxAll, d.mx[k]); }
    return d;
}

int main(int argc, char** argv)
{
    bool full = false, timing = false, fastOnly = false;
    std::string only;
    std::vector<std::string> videos;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--full")) full = true;
        else if (!std::strcmp(argv[i], "--time")) timing = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!std::strcmp(argv[i], "--fast")) fastOnly = timing = true;
        else videos.push_back(argv[i]);
    }
    const int W = full ? 1920 : 480, H = full ? 1080 : 270;
    std::vector<std::pair<std::string, Buf>> pictures;
    pictures.push_back({"synthetic", syntheticPicture(W, H)});
    for (size_t i = 0; i < videos.size(); ++i) {
        Buf b;
        if (videoPicture(videos[i], 1.0 + 0.5 * (double)i, W, H, b)) pictures.push_back({"video" + std::to_string(i), b});
        else std::printf("(no frame from %s)\n", videos[i].c_str());
    }
    const std::string V = "com.vegascreativesoftware:";
    std::vector<Case> cases = {
        // Min and Max: exact
        {"minmax default", V + "minandmax", {}, ref::minAndMax, 0, 0},
        {"minmax maximum", V + "minandmax", {{"Operation", {1}}}, ref::minAndMax, 0, 0},
        {"minmax round minimum", V + "minandmax", {{"CircularAreaProcessing", {1}}}, ref::minAndMax, 0, 0},
        {"minmax round maximum wide", V + "minandmax", {{"Operation", {1}}, {"CircularAreaProcessing", {1}}, {"HorizontalRange", {0.9}}, {"VerticalRange", {0.3}}}, ref::minAndMax, 0, 0},
        {"minmax horizontal only", V + "minandmax", {{"VerticalRange", {0}}}, ref::minAndMax, 0, 0},
        // (Median, Linear Blur and Radial Blur are VEGAS's own algorithms with no slower original: checked against VEGAS's pictures)
        // light
        {"defocus default", V + "defocusvelvetmatter", {}, ref::defocus, 0.02, 1},
        {"defocus hexagon curved", V + "defocusvelvetmatter", {{"IrisShape", {2}}, {"DiscCurvature", {-60}}, {"Orientation", {0.2}}}, ref::defocus, 0.02, 1},
        {"light rays default", V + "lightrays", {}, ref::lightRays, 0.8, 24},
        {"light rays off centre", V + "lightrays", {{"LightSource", {0.2, 0.8}}, {"group0Sensitivity", {0.8}}, {"group0Strength", {0.6}}, {"TintColor", {1, 0.6, 0.2}}}, ref::lightRays, 0.8, 24},
        // (Rays, Glint, Starburst and Fill Light follow VEGAS's managed filters step for step and are checked against VEGAS's own pictures by tools/effectref_compare.py)
        {"black bar fill default", V + "blackbarfill", {}, ref::blackBarFill, 0.8, 24},
        {"black bar fill small foreground", V + "blackbarfill", {{"Foreground Scale", {0.5}}, {"Foreground Location", {0.3, 0.6}}, {"Foreground Shadow Blur", {0.5}}, {"Background Scale", {2}}, {"Background Blur", {0.2}}}, ref::blackBarFill, 1.0, 24},
    };
    for (const auto& pic : pictures) {
        for (const Case& cs : cases) {
            if (!only.empty() && cs.name.find(only) == std::string::npos) continue;
            const Def* def = findDef(cs.id);
            if (!def) { check(false, cs.name, "no such built-in"); continue; }
            Params store1, store2;
            Ctx fast = makeCtx(*def, cs.params, pic.second, store1), slow = makeCtx(*def, cs.params, pic.second, store2);
            const double t0 = nowMs();
            def->render(fast);
            const double t1 = nowMs();
            if (!fastOnly) cs.ref(slow);
            const double t2 = nowMs();
            if (fastOnly) { std::printf("%-34s [%s] new %.0f ms\n", cs.name.c_str(), pic.first.c_str(), t1 - t0); continue; }
            const Diff d = compare(fast.out, slow.out);
            char buf[300];
            std::snprintf(buf, sizeof buf, "[%s] mean %.3f %.3f %.3f %.3f max %.0f %.0f %.0f %.0f levels%s", pic.first.c_str(), d.mean[0], d.mean[1], d.mean[2], d.mean[3],
                          d.mx[0], d.mx[1], d.mx[2], d.mx[3], "");
            std::string detail = buf;
            if (timing) { std::snprintf(buf, sizeof buf, "  new %.0f ms, old %.0f ms", t1 - t0, t2 - t1); detail += buf; }
            check(d.meanAll <= cs.meanTol && d.mxAll <= cs.maxTol, cs.name, detail);
        }
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
