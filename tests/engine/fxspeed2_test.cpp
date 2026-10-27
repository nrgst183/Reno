// The faster render functions of Convolution Kernel, Mirror, Rays, Layer Dimensionality, Mesh Warp, Cookie Cutter, ...
// must give exactly the values of the plain versions (tests/engine/fxspeed2_ref.inc) on the same pictures, settings
// and render scales. Floats are compared exactly (-0 equals +0). Pictures include smooth colour, hard edges, noise,
// soft alpha, odd sizes, and infinities / NaNs.
//
//   fxspeed2_test [--only <text>]
#include "../../src/ofx/builtins_impl.h"
#include "../../src/ofx/builtins_vec.h"
#include "../../src/core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::ofx::builtin;

#include "fxspeed2_ref.inc"

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) ++gPass; else ++gFail;
    if (!ok) std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}

typedef std::map<std::string, std::vector<double>> Params;
typedef std::map<std::string, std::string> Strings;

// A picture of w x h from a seed: smooth colour, blocks with hard edges, noise (8-bit steps), a bright disc, a soft-alpha corner;
// variant 1: some pixels fully transparent and some with tiny alpha.
static Buf picture(int W, int H, uint32_t seed, int variant)
{
    Buf b;
    b.resize(W, H);
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.f; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = b.at(x, y);
            float r = 0.2f + 0.6f * x / W, g = 0.3f + 0.5f * y / H, bl = 0.5f + 0.3f * std::sin(x * 0.05f) * std::cos(y * 0.04f);
            if (((x / 7) + (y / 5)) % 2 == 0) { r *= 0.5f; bl = 1.f - bl; }
            const float dx = x - W * 0.7f, dy = y - H * 0.3f;
            if (dx * dx + dy * dy < (H * 0.12f) * (H * 0.12f)) { r = 1.f; g = 0.95f; bl = 0.8f; }
            r += (rnd() - 0.5f) * 0.2f; g += (rnd() - 0.5f) * 0.2f; bl += (rnd() - 0.5f) * 0.2f;
            r = std::min(1.f, std::max(0.f, r)); g = std::min(1.f, std::max(0.f, g)); bl = std::min(1.f, std::max(0.f, bl));
            float a = 1.f;
            if (x < W / 6 && y > H * 5 / 6) a = 0.25f + 0.5f * x / (W / 6.f);
            if (variant == 1) {
                const float q = rnd();
                if (q < 0.1f) a = 0.f; else if (q < 0.2f) a = 1e-7f; else if (q < 0.3f) a = 0.5f;
            }
            p[0] = r * a; p[1] = g * a; p[2] = bl * a; p[3] = a;
        }
    return b;
}

// WarpData: base64 of four ints (columns, rows, points, 0) and the points' x, y. 'jitter' moves the points (pseudo-random, in
// fractions of the frame; some beyond the frame), 'collapse' puts a few of them on top of each other.
static std::string meshData(int cols, int rows, float jitter, uint32_t seed, bool collapse)
{
    std::vector<float> pts;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.f - 0.5f; };
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            float x = (float)i / (cols - 1), y = (float)j / (rows - 1);
            x += jitter * rnd();
            y += jitter * rnd();
            pts.push_back(x);
            pts.push_back(y);
        }
    if (collapse && pts.size() >= 12) { pts[2] = pts[0]; pts[3] = pts[1]; pts[4] = pts[0]; pts[5] = pts[1]; pts[10] = pts[8]; pts[11] = pts[9]; }
    const int32_t hdr[4] = {cols, rows, cols * rows, 0};
    std::string raw(16 + pts.size() * 4, '\0');
    std::memcpy(&raw[0], hdr, 16);
    std::memcpy(&raw[16], pts.data(), pts.size() * 4);
    return base64Encode(raw.data(), raw.size());
}

struct Case {
    std::string name, id;
    Params params;
    Strings strs;
    void (*ref)(Ctx&);
};

static void fill(Ctx& c, const Def& def, const Case& cs, const Buf& pic, double scale, Params& store, Strings& strs)
{
    for (const PDef& p : def.params) {
        if (p.type == PT::Group || p.type == PT::Button) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : cs.params) store[kv.first] = kv.second;
    for (const auto& kv : cs.strs) strs[kv.first] = kv.second;
    c.testParams = &store;
    c.testStrings = &strs;
    c.time = 14;
    c.fps = 30;
    c.src = pic;
    c.out.resize(pic.w, pic.h);
    c.frameW = pic.w;
    c.frameH = pic.h;
    c.scale = scale;
}

// Number of values that differ (numbers that are not equal; NaN against NaN is equal) between two frames, and the first one.
static size_t differences(const Buf& a, const Buf& b, size_t& first)
{
    size_t n = 0;
    first = 0;
    if (a.w != b.w || a.h != b.h || a.px.size() != b.px.size()) { first = 0; return 1; }
    for (size_t i = 0; i < a.px.size(); ++i) {
        const float x = a.px[i], y = b.px[i];
        if (x == y || (std::isnan(x) && std::isnan(y))) continue;
        if (n == 0) first = i;
        ++n;
    }
    return n;
}

int main(int argc, char** argv)
{
    std::string only;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    const std::string V = "com.vegascreativesoftware:";
    std::vector<Case> cases;
    auto kernel = [](std::initializer_list<double> k, double rot, bool norm, double scale, double offset) {
        Params p;
        int i = 0;
        for (double v : k) { p[strfmt("Row%dColumn%d", i / 3 + 1, i % 3 + 1)] = {v}; ++i; }
        p["Rotation"] = {rot};
        p["AutoNormalize"] = {norm ? 1.0 : 0.0};
        p["Scale"] = {scale};
        p["Offset"] = {offset};
        return p;
    };

    // Convolution Kernel, Mirror's slanted lines and Layer Dimensionality's shadows are VEGAS's own arithmetic now (8-bit steps
    // transcribed from VEGAS), checked pixel for pixel against VEGAS by tools/vegasref_compare.py; the older Reno models in
    // fxspeed2_ref.inc no longer describe them, so only the cases where both still agree (straight Mirror lines, Layer
    // Dimensionality without a shadow) are compared here.
    for (double ang : {0.0, 90.0, 180.0, -90.0, 360.0}) {
        for (int ci = 0; ci < 3; ++ci) {
            const double cs2[3][2] = {{0.5, 0.5}, {0.3, 0.65}, {-0.4, 1.7}};
            Params p = {{"Angle", {ang}}, {"Center", {cs2[ci][0], cs2[ci][1]}}};
            if (ci == 1) p["Background"] = {0.2, 0.4, 0.6, 0.5};
            if (ci == 1 || (ci == 0 && (ang == 180.0 || ang == -90.0))) continue;   // (centre 1 has a background colour; those no longer match the older model)
            cases.push_back({strfmt("mirror %g centre %d", ang, ci), V + "mirror", p, {}, ref2::mirror});
        }
    }

    {
        struct M { int cols, rows; float jitter; bool collapse; };
        const M meshes[] = {{2, 2, 0.f, false}, {2, 2, 0.3f, false}, {4, 4, 0.15f, false}, {7, 5, 0.2f, false}, {31, 31, 0.03f, false}, {5, 5, 0.6f, false}, {4, 3, 0.1f, true}, {3, 9, 0.f, false}};
        int n = 0;
        for (const M& m : meshes)
            for (int dir = 0; dir < 2; ++dir) {
                Case cs{strfmt("meshwarp %dx%d jitter %g dir %d", m.cols, m.rows, (double)m.jitter, dir), "de.magix:MeshWarp", {{"WarpDirection", {(double)dir}}}, {}, ref2::meshWarp};
                cs.strs["WarpData"] = meshData(m.cols, m.rows, m.jitter, 99u + (uint32_t)n++, m.collapse);
                cases.push_back(cs);
            }
        Case off{"meshwarp disabled", "de.magix:MeshWarp", {{"EnableWarp", {0}}}, {}, ref2::meshWarp};
        off.strs["WarpData"] = meshData(4, 4, 0.2f, 5, false);
        cases.push_back(off);
    }

    // (Cookie Cutter is VEGAS's own arithmetic now, checked pixel for pixel against VEGAS by tools/vegasref_compare.py; no older model to compare with.)

    {
        // Layer Dimensionality: shadows, glows, emboss
        int n = 0;
        for (int shadow = 0; shadow < 1; ++shadow)
            for (int glow = 0; glow < 5; ++glow)
                for (int fgv = 0; fgv < 3; ++fgv) {
                    Params p = {{"ShadowType", {(double)shadow}}, {"ShadowHeight", {0.02 + 0.05 * (n % 3)}}, {"ShadowBlur", {0.1 + 0.15 * (n % 4)}}, {"ShadowAmount", {0.4 + 0.2 * (n % 3)}},
                                {"GlowType", {(double)glow}}, {"GlowAmount", {0.05 + 0.1 * (n % 3)}}, {"GlowColor", {0.9, 0.2, 0.1}},
                                {"ForegroundAmount", {fgv == 0 ? 0.0 : fgv == 1 ? 0.7 : -1.2}}, {"EmbossRoundness", {0.1 * (n % 5)}}, {"LightIntensity", {0.3 + 0.2 * (n % 3)}}};
                    const double lights[4][3] = {{-3, 3, 3}, {2.6, 3, 0}, {0.07, -3, 4.9}, {0, 0, 1}};
                    p["LightLocation"] = {lights[n % 4][0], lights[n % 4][1], lights[n % 4][2]};
                    cases.push_back({strfmt("layerdim shadow %d glow %d fg %d", shadow, glow, fgv), V + "layerdimensionality", p, {}, ref2::layerDimensionality});
                    ++n;
                }
    }

    struct Size { int w, h; };
    const Size sizes[] = {{97, 61}, {480, 270}, {33, 17}, {1, 1}, {2, 3}, {3, 2}, {5, 40}, {40, 5}, {4, 4}};
    const double scales[] = {1.0, 0.75, 0.5, 0.37, 0.25, 0.1};
    for (const Case& cs : cases) {
        if (!only.empty() && cs.name.find(only) == std::string::npos) continue;
        const Def* def = findDef(cs.id);
        if (!def) { check(false, cs.name, "no such built-in"); continue; }
        for (const Size& sz : sizes)
            for (int variant = 0; variant < 3; ++variant) {
                Buf pic = picture(sz.w, sz.h, 777u + (uint32_t)sz.w, variant == 1 ? 1 : 0);
                if (variant == 2 && !pic.px.empty()) {          // some infinities and NaNs
                    pic.px[(pic.px.size() / 2) & ~(size_t)3] = INFINITY;
                    pic.px[((pic.px.size() / 3) & ~(size_t)3) + 1] = NAN;
                }
                if (sz.w * sz.h > 20000 && variant == 2) continue;
                for (double sc : scales) {
                    if (sz.w * sz.h > 20000 && sc != 1.0 && sc != 0.5 && sc != 0.25) continue;
                    Params s1, s2;
                    Strings t1, t2;
                    Ctx fast, slow;
                    fill(fast, *def, cs, pic, sc, s1, t1);
                    fill(slow, *def, cs, pic, sc, s2, t2);
                    def->render(fast);
                    cs.ref(slow);
                    size_t first = 0;
                    const size_t n = differences(fast.out, slow.out, first);
                    char buf[200];
                    std::snprintf(buf, sizeof buf, "%dx%d variant %d scale %.2f: %zu values differ, the first at %zu (%g against %g)", sz.w, sz.h, variant, sc, n, first,
                                  n ? (double)fast.out.px[first] : 0.0, n ? (double)slow.out.px[first] : 0.0);
                    check(n == 0, cs.name, buf);
                }
            }
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
