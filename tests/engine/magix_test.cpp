// Tests of the built-in Color Curves, Tiny Planet, Mesh Warp and Smart Upscale (src/ofx/builtins_magix.cpp) and the
// effect log (src/engine/fxlog.cpp).
//
//   magix_test [vegas samples folder]      (default $RENO_SAMPLES; the project tests are skipped when it is missing)
//
// Covers the pictures, render scale 1/2 and 1/4, threads, the engine / OpenFX host, and every use of the four
// effects in the sample projects. Uses fxscale_test.cpp's helpers (the file is included).
#define main fxscale_main
#include "fxscale_test.cpp"
#undef main
#include "../../src/engine/fxlog.h"
#include "../../src/ofx/builtins_magixdata.h"
#include "../../src/vegas/vegas.h"
#include <thread>
#include "../testdata.h"

typedef std::map<std::string, std::string> StrMap;

static const std::string kCurves = "com.vegascreativesoftware:colorcurves";
static const std::string kPlanet = "de.magix:stereographicProjectionOCL";
static const std::string kMesh = "de.magix:MeshWarp";
static const std::string kUpscale = "com.sonycreativesoftware:smart_upscale";

static Buf runS(const Def& def, const Params& over, const StrMap& sover, const Buf& src, double scale, bool levels = true)
{
    Params store;
    StrMap strs;
    for (const PDef& p : def.params) {
        if (p.type == PT::Group || p.type == PT::Button) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : over) store[kv.first] = kv.second;
    for (const auto& kv : sover) strs[kv.first] = kv.second;
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
    c.levels = levels;
    c.time = 3;
    c.fps = 30;
    c.src = src;
    c.out.resize(src.w, src.h);
    c.frameW = src.w;
    c.frameH = src.h;
    c.scale = scale;
    def.render(c);
    return c.out;
}

// The "Property" (or other text) value of a preset of the built-in preset package.
static std::string presetValue(const std::string& plugin, const std::string& name, const char* param)
{
    const std::string& xml = ofx::builtinPresetPackage();
    const std::string head = "plugin=\"" + plugin + "\" context=\"Filter\" name=\"" + name + "\"";
    size_t at = xml.find(head);
    if (at == std::string::npos) return std::string();
    const size_t end = xml.find("</OfxPreset>", at);
    at = xml.find(std::string("name=\"") + param + "\"", at);
    if (at == std::string::npos || at > end) return std::string();
    at = xml.find("<OfxParamValue>", at) + 15;
    return xml.substr(at, xml.find("</OfxParamValue>", at) - at);
}

// A picture of ramps: red across, green down, blue the other way across, with alpha 'a'.
static Buf ramp(int W, int H, float a = 1.f)
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float r = (float)x / (W - 1), g = (float)y / (H - 1);
            premul(b.at(x, y), r, g, 1.f - r, a);
        }
    return b;
}

// A grey ramp: every channel x / (W - 1), the same on every row.
static Buf greyRamp(int W, int H)
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) premul(b.at(x, y), (float)x / (W - 1), (float)x / (W - 1), (float)x / (W - 1), 1.f);
    return b;
}

// Straight colour of pixel (x, y).
static void straight(const Buf& b, int x, int y, float* rgb)
{
    float a;
    unpremul(b.at(x, y), rgb[0], rgb[1], rgb[2], a);
}

static double maxDiff(const Buf& a, const Buf& b)
{
    double m = 0;
    for (size_t i = 0; i < a.px.size() && i < b.px.size(); ++i) m = std::max(m, (double)std::fabs(a.px[i] - b.px[i]));
    return m;
}

static std::string f1(double v) { return strfmt("%.4f", v); }

// ------------------------------------------------------------------ a curve set made here
// The identity curves (from the default) with the master curve replaced by 'master' (points x, y with the handles in/out dx, dy).
static std::string withMaster(const std::string& base, double y0, double y1, double h0y, double h1y)
{
    std::string b = base64Decode(base);
    const size_t o = 20 + 6 * 104;      // the master curve's first point (three curves of two points before it)
    auto put = [&](size_t at, double v) { std::memcpy(&b[at], &v, 8); };
    put(o + 8, y0);
    put(o + 80 + 8, h0y);
    put(o + 104 + 8, y1);
    put(o + 104 + 48, h1y);
    return base64Encode(b.data(), b.size());
}

// ------------------------------------------------------------------ 1. pictures
static void curveTests()
{
    const Def* d = ofx::builtin::findDef(kCurves);
    check(d != nullptr, "Color Curves is a built-in (com.vegascreativesoftware:colorcurves)");
    if (!d) return;
    const Buf src = ramp(256, 64);
    {
        const Buf o = runS(*d, {}, {}, src, 1.0);
        check(maxDiff(o, src) < 0.003, "Color Curves at its defaults leaves the picture as it is", f1(maxDiff(o, src)));
    }
    auto withPreset = [&](const char* name) {
        const std::string v = presetValue(kCurves, name, "Property");
        check(!v.empty(), std::string("VEGAS preset \"") + name + "\" is in the built-in presets");
        return runS(*d, {}, {{"Property", v}}, src, 1.0);
    };
    {
        // the blue curve of Yellow Tint runs from (0,0) to (255,198) and bends (its handles say y(128) = 84); red and green are straight
        const Buf grey = greyRamp(256, 8);
        const Buf o = runS(*d, {}, {{"Property", presetValue(kCurves, "Yellow Tint", "Property")}}, grey, 1.0);
        float q[3];
        straight(o, 255, 3, q);
        const double top = std::fabs(q[2] - 198.0 / 255) + std::fabs(q[0] - 1.0) + std::fabs(q[1] - 1.0);
        straight(o, 128, 3, q);
        const double mid = std::fabs(q[2] - 84.0 / 255) + std::fabs(q[0] - 128.0 / 255) + std::fabs(q[1] - 128.0 / 255);
        check(top < 0.01 && mid < 0.03, "Yellow Tint: white becomes (1, 1, 198/255), the middle grey (128, 128, 84)/255: the blue curve bends", strfmt("%.3f %.3f", top, mid));
    }
    {
        const Buf o = withPreset("Blues and Yellows");
        double worst = 0;
        for (int x = 0; x < 256; x += 5) {
            float s[3], q[3];
            straight(src, x, 10, s);
            straight(o, x, 10, q);
            worst = std::max(worst, (double)std::fabs(q[2] - (1.f - s[2])));
            worst = std::max(worst, (double)std::fabs(q[0] - s[0]));
        }
        check(worst < 0.012, "Blues and Yellows: the blue curve is inverted, red unchanged", f1(worst));
    }
    {
        const Buf o = withPreset("Increase Contrast");
        float s[3], q[3];
        straight(src, 64, 10, s);
        straight(o, 64, 10, q);
        const bool dark = q[0] < s[0] - 0.03f;
        straight(src, 192, 10, s);
        straight(o, 192, 10, q);
        const bool bright = q[0] > s[0] + 0.03f;
        straight(src, 128, 10, s);
        straight(o, 128, 10, q);
        check(dark && bright && std::fabs(q[0] - 0.5f) < 0.03f, "Increase Contrast: darks darker, brights brighter, the middle stays", f1(q[0]));
    }
    {
        // the Infrared preset (also the one most sample uses carry): the green curve of it dips under 0 and clamps
        const Buf grey = greyRamp(256, 8);
        const Buf o = runS(*d, {}, {{"Property", presetValue(kCurves, "Infrared", "Property")}}, grey, 1.0);
        float q[3];
        straight(o, 0, 3, q);
        check(std::fabs(q[2] - 24.0 / 255) < 0.02f && q[0] < 0.01f, "Infrared: black becomes (0, 0, 24/255): the blue curve starts at 24", strfmt("%.3f %.3f %.3f", q[0], q[1], q[2]));
        straight(o, 255, 3, q);
        check(q[0] > 0.99f && q[1] > 0.99f && q[2] > 0.99f, "Infrared: white stays white");
    }
    {
        const Buf grey = greyRamp(256, 8);
        const Buf o = runS(*d, {}, {{"Property", presetValue(kCurves, "Night", "Property")}}, grey, 1.0);
        float q[3];
        straight(o, 255, 3, q);
        check(std::fabs(q[0] - 96.5 / 255) < 0.012 && std::fabs(q[1] - 125.0 / 255) < 0.012 && std::fabs(q[2] - 171.0 / 255) < 0.012, "Night: white becomes (96.5, 125, 171)/255",
              strfmt("%.3f %.3f %.3f", q[0], q[1], q[2]));
    }
    {
        const Buf o = withPreset("Negative Shadows");
        float q[3];
        straight(o, 0, 10, q);
        const bool top = q[0] > 0.99f;
        float r[3];
        straight(o, 32, 10, r);
        check(top && std::fabs(r[0] - 81.0 / 255 * 1.0) < 0.05f && std::fabs(r[0] - 32.0 / 255) > 0.1f, "Negative Shadows: black becomes white, the shadows come down in a curve", f1(r[0]));
    }
    {
        // a master curve that inverts everything, folded in after the red, green and blue ones
        const std::string inv = withMaster(ofx::builtin::kMagixDefaultCurves, 255, 0, -51, 51);
        const Buf o = runS(*d, {}, {{"Property", inv}}, src, 1.0);
        double worst = 0;
        for (int x = 0; x < 256; x += 5)
            for (int y = 0; y < 64; y += 21) {
                float s[3], q[3];
                straight(src, x, y, s);
                straight(o, x, y, q);
                for (int k = 0; k < 3; ++k) worst = std::max(worst, (double)std::fabs(q[k] - (1.f - s[k])));
            }
        check(worst < 0.012, "a master curve from (0,255) to (255,0) inverts all three channels", f1(worst));
    }
    {
        // soft alpha, float picture: the curve works on the colour, not on the colour times alpha
        const Buf half = ramp(64, 8, 0.5f);
        const std::string inv = withMaster(ofx::builtin::kMagixDefaultCurves, 255, 0, -51, 51);
        const Buf o = runS(*d, {}, {{"Property", inv}}, half, 1.0, false);
        float s[3], q[3], a;
        straight(half, 10, 3, s);
        straight(o, 10, 3, q);
        a = o.at(10, 3)[3];
        check(std::fabs(q[0] - (1.f - s[0])) < 0.012f && std::fabs(a - 0.5f) < 1e-6f, "a half transparent float picture: the colour is curved as a straight colour, alpha stays", strfmt("%.3f alpha %.3f", q[0], a));
        // 8-bit (VEGAS's way, exact): the byte tables act on the premultiplied bytes and are held to the alpha
        const Buf o8 = runS(*d, {}, {{"Property", inv}}, half, 1.0, true);
        const float *p8 = o8.at(10, 3);
        check(std::fabs(p8[3] - 0.5f) < 0.5f / 255 + 1e-6f && p8[0] <= p8[3] + 1.f / 255 && p8[0] > 0.45f, "a half transparent 8-bit picture: the colour is curved as bytes and held to the alpha", strfmt("%.3f alpha %.3f", p8[0], p8[3]));
    }
    for (const char* bad : {"", "abc", "AQAAAAIAAAA=", "////////////////////////"}) {
        const Buf o = runS(*d, {}, {{"Property", bad}}, src, 1.0);
        check(maxDiff(o, src) == 0, std::string("text that is no curve set (\"") + bad + "\") leaves the picture as it is");
    }
}

static void planetTests()
{
    const Def* d = ofx::builtin::findDef(kPlanet);
    check(d != nullptr, "Tiny Planet is a built-in (de.magix:stereographicProjectionOCL)");
    if (!d) return;
    // an equirectangular picture: red grows with the elevation (0 at the bottom pole, 1 at the top), green with the azimuth (0.5 straight ahead)
    const int SW = 512, SH = 256;
    Buf eq;
    eq.resize(SW, SH);
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x) premul(eq.at(x, y), 1.f - (float)(y + 0.5) / SH, (float)(x + 0.5) / SW, 0.2f, 1.f);
    auto center = [&](double lat, double lon, double z) {
        const Buf o = runS(*d, {{"Latitude", {lat}}, {"Longitude", {lon}}, {"Z", {z}}}, {}, eq, 1.0);
        float q[3];
        straight(o, SW / 2, SH / 2, q);
        return std::vector<float>{q[0], q[1]};
    };
    {
        auto c = center(0, 0, 20);
        check(c[0] < 0.03f, "Latitude 0: the middle of the picture is the ground (the bottom pole)", f1(c[0]));
        c = center(180, 0, 20);
        check(c[0] > 0.97f, "Latitude 180: the middle is the sky (the top pole)", f1(c[0]));
        c = center(90, 0, 20);
        check(std::fabs(c[0] - 0.5f) < 0.02f && std::fabs(c[1] - 0.5f) < 0.02f, "Latitude 90: the middle looks at the horizon straight ahead", strfmt("%.3f %.3f", c[0], c[1]));
        c = center(90, 90, 20);
        check(std::fabs(c[1] - 0.75f) < 0.02f || std::fabs(c[1] - 0.25f) < 0.02f, "Longitude 90 turns the view by a quarter of the way round", f1(c[1]));
        c = center(90, 360, 20);
        check(std::fabs(c[1] - 0.5f) < 0.02f, "Longitude 360 is a whole turn", f1(c[1]));
    }
    {
        // VEGAS: the equator circle has a radius of 2 W / Z pixels (read off its pictures), so a frame's top row middle sits at the
        // stereographic plane's Y = zoom * (H / 2 - 1.5) / (H / 2) with zoom = Z * H / (4 W); at Latitude 0 that is elevation asin((r2 - 1) / (r2 + 1))
        auto topRed = [&](double z) {
            const double zoom = z * SH / (4.0 * SW), Y = zoom * (SH / 2 - 1.5) / (SH / 2), X = zoom * 0.5 / (SH / 2), r2 = X * X + Y * Y;
            return 0.5 + std::asin((r2 - 1) / (r2 + 1)) / kPi;
        };
        const Buf o = runS(*d, {{"Latitude", {0}}, {"Longitude", {0}}, {"Z", {20}}}, {}, eq, 1.0);
        float q[3];
        straight(o, SW / 2, 1, q);
        check(std::fabs(q[0] - topRed(20)) < 0.02, "Z 20: the top row's middle is at the elevation the formula gives", f1(q[0]));
        const Buf far = runS(*d, {{"Latitude", {0}}, {"Longitude", {0}}, {"Z", {1}}}, {}, eq, 1.0);
        straight(far, SW / 2, 1, q);
        check(q[0] < 0.1f, "a smaller Z magnifies the middle: the top edge is still ground", f1(q[0]));
        const Buf small = runS(*d, {{"Latitude", {0}}, {"Longitude", {0}}, {"Z", {5}}}, {}, eq, 1.0);
        straight(small, SW / 2, 1, q);
        check(std::fabs(q[0] - topRed(5)) < 0.02, "Z 5: the top edge is at the elevation the formula gives", f1(q[0]));
    }
    {
        // the output is whole (no holes, nothing out of range) at extreme settings
        bool ok = true;
        for (double lat : {-720.0, -90.0, 0.0, 135.0, 5000.0})
            for (double z : {0.01, 1.0, 20.0, 700.0, 1e7}) {
                const Buf o = runS(*d, {{"Latitude", {lat}}, {"Longitude", {37}}, {"Z", {z}}}, {}, ramp(160, 90), 1.0);
                for (float v : o.px) if (!(v >= -1e-4f && v <= 1.0001f)) ok = false;
            }
        check(ok, "extreme Latitude and Z give valid pictures");
    }
}

static std::string meshText(int cols, int rows, const std::vector<std::pair<int, std::pair<float, float>>>& moved)
{
    std::string b(16 + (size_t)cols * rows * 8, '\0');
    const int32_t hdr[4] = {cols, rows, cols * rows, 0};
    std::memcpy(&b[0], hdr, 16);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            float p[2] = {(float)i / (cols - 1), (float)j / (rows - 1)};
            for (const auto& m : moved) if (m.first == j * cols + i) { p[0] = m.second.first; p[1] = m.second.second; }
            std::memcpy(&b[16 + ((size_t)j * cols + i) * 8], p, 8);
        }
    return base64Encode(b.data(), b.size());
}

static void meshTests()
{
    const Def* d = ofx::builtin::findDef(kMesh);
    check(d != nullptr, "Mesh Warp is a built-in (de.magix:MeshWarp)");
    if (!d) return;
    const int W = 200, H = 120;
    const Buf src = ramp(W, H);
    {
        const Buf o = runS(*d, {}, {}, src, 1.0);
        check(maxDiff(o, src) == 0, "no mesh: the picture is as it is");
        const Buf o2 = runS(*d, {}, {{"WarpData", meshText(5, 5, {})}}, src, 1.0);
        check(maxDiff(o2, src) == 0, "a mesh with every point in place: the picture is as it is");
        const Buf o3 = runS(*d, {{"EnableWarp", {0}}}, {{"WarpData", meshText(5, 5, {{12, {0.7f, 0.5f}}})}}, src, 1.0);
        check(maxDiff(o3, src) == 0, "Enable Effect off: the picture is as it is");
    }
    {
        // Target (the default): the middle point moved right; the picture at the middle goes with it
        const std::string m = meshText(5, 5, {{12, {0.7f, 0.5f}}});
        const Buf o = runS(*d, {{"WarpDirection", {1}}}, {{"WarpData", m}}, src, 1.0);
        float q[3], s[3];
        straight(o, (int)(0.7 * W), H / 2, q);
        straight(src, W / 2, H / 2, s);
        check(std::fabs(q[0] - s[0]) < 0.04f && std::fabs(q[1] - s[1]) < 0.06f, "Target: the point moved to 0.7 carries the colour that was at 0.5", strfmt("%.3f vs %.3f", q[0], s[0]));
        straight(o, 2, 2, q);
        straight(src, 2, 2, s);
        check(std::fabs(q[0] - s[0]) < 0.02f, "Target: a corner nowhere near the moved point is as it was");
        int clear = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) if (o.at(x, y)[3] < 0.5f) ++clear;
        check(clear == 0, "Target: a mesh whose outline stays put covers the whole frame", std::to_string(clear) + " clear pixels");
    }
    {
        // Source: the point says where the colour comes from
        const std::string m = meshText(5, 5, {{12, {0.7f, 0.5f}}});
        const Buf o = runS(*d, {{"WarpDirection", {0}}}, {{"WarpData", m}}, src, 1.0);
        float q[3], s[3];
        straight(o, W / 2, H / 2, q);
        straight(src, (int)(0.7 * W), H / 2, s);
        check(std::fabs(q[0] - s[0]) < 0.02f, "Source: the middle takes the colour from 0.7", strfmt("%.3f vs %.3f", q[0], s[0]));
    }
    {
        // the pulled-away mesh leaves the uncovered part transparent, and nothing is out of range
        const std::string m = meshText(3, 3, {{4, {0.9f, 0.9f}}, {0, {0.3f, 0.3f}}});
        const Buf o = runS(*d, {}, {{"WarpData", m}}, src, 1.0);
        bool ok = true;
        int clear = 0;
        for (size_t i = 0; i < o.px.size(); ++i) if (!(o.px[i] >= -1e-5f && o.px[i] <= 1.0001f)) ok = false;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) if (o.at(x, y)[3] < 0.5f) ++clear;
        check(ok && clear > 0, "a mesh pulled in from its corner leaves that corner transparent", std::to_string(clear) + " clear pixels");
    }
    for (const char* bad : {"", "abc", "AQAAAAEAAAA=", "AgAAAAIAAAAEAAAAAAAAAAAAAAA="}) {
        const Buf o = runS(*d, {}, {{"WarpData", bad}}, src, 1.0);
        check(maxDiff(o, src) == 0, std::string("text that is no mesh (\"") + bad + "\") leaves the picture as it is");
    }
}

static void upscaleTests()
{
    const Def* d = ofx::builtin::findDef(kUpscale);
    check(d != nullptr, "Smart Upscale is a built-in (com.sonycreativesoftware:smart_upscale)");
    if (!d) return;
    const Buf pic = picture(320, 180, 0);
    check(maxDiff(runS(*d, {{"EnableEnhancement", {0}}}, {}, pic, 1.0), pic) == 0, "Enhancement off: the picture is as it is");
    check(maxDiff(runS(*d, {{"Scale", {1}}}, {}, pic, 1.0), pic) == 0, "Scale 1: the picture is as it is");
    Buf flat;
    flat.resize(64, 64);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) premul(flat.at(x, y), 0.4f, 0.5f, 0.6f, 1.f);
    check(maxDiff(runS(*d, {{"Scale", {4}}}, {}, flat, 1.0), flat) < 1e-5, "a flat picture stays flat");
    {
        // an edge gets steeper (overshoot on both sides) and the result stays in range
        Buf edge;
        edge.resize(64, 16);
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 64; ++x) premul(edge.at(x, y), x < 32 ? 0.3f : 0.7f, x < 32 ? 0.3f : 0.7f, x < 32 ? 0.3f : 0.7f, 1.f);
        const Buf o = runS(*d, {{"Scale", {4}}, {"Noise", {0}}}, {}, edge, 1.0);
        check(o.at(31, 5)[0] < 0.3f - 0.03f && o.at(32, 5)[0] > 0.7f + 0.03f, "an edge is sharpened: darker just before it, lighter just after", strfmt("%.3f %.3f", o.at(31, 5)[0], o.at(32, 5)[0]));
        bool ok = true;
        for (float v : o.px) if (!(v >= 0.f && v <= 1.0001f)) ok = false;
        check(ok, "the sharpened picture stays within range");
    }
}

// ------------------------------------------------------------------ 2. and 3. scale and threads
struct Case { std::string label, id; Params p; StrMap s; };

static std::vector<Case> cases()
{
    std::vector<Case> v;
    v.push_back({"curves infrared", kCurves, {}, {{"Property", presetValue(kCurves, "Infrared", "Property")}}});
    v.push_back({"curves increase contrast", kCurves, {}, {{"Property", presetValue(kCurves, "Increase Contrast", "Property")}}});
    v.push_back({"planet tiny planet", kPlanet, {{"Latitude", {0}}, {"Longitude", {20}}, {"Z", {20}}}, {}});
    v.push_back({"planet tiny sky", kPlanet, {{"Latitude", {180}}, {"Longitude", {-40}}, {"Z", {30}}}, {}});
    v.push_back({"planet flat horizon", kPlanet, {{"Latitude", {90}}, {"Longitude", {0}}, {"Z", {20}}}, {}});
    v.push_back({"planet zoomed", kPlanet, {{"Latitude", {116.3}}, {"Longitude", {10}}, {"Z", {200}}}, {}});
    v.push_back({"mesh target", kMesh, {{"WarpDirection", {1}}}, {{"WarpData", meshText(5, 5, {{6, {0.3f, 0.15f}}, {12, {0.62f, 0.5f}}, {18, {0.7f, 0.9f}}, {8, {0.8f, 0.3f}}})}}});
    v.push_back({"mesh source", kMesh, {{"WarpDirection", {0}}}, {{"WarpData", meshText(5, 5, {{6, {0.3f, 0.15f}}, {12, {0.62f, 0.5f}}, {18, {0.7f, 0.9f}}, {8, {0.8f, 0.3f}}})}}});
    v.push_back({"upscale", kUpscale, {{"Scale", {4}}, {"Noise", {2000}}}, {}});
    v.push_back({"upscale chroma", kUpscale, {{"Scale", {8}}, {"LumaOnly", {0}}, {"Noise", {0}}}, {}});
    return v;
}

static void scaleTests()
{
    const Buf pic = picture(960, 540, 0);
    for (const Case& c : cases()) {
        const Def* d = ofx::builtin::findDef(c.id);
        if (!d) continue;
        const Buf full = runS(*d, c.p, c.s, pic, 1.0);
        const Buf orig = runS(*d, c.p, c.s, pic, 1.0);
        check(maxDiff(full, orig) == 0, c.label + ": rendering twice gives the same picture");
        for (int f : {2, 4}) {
            const Buf small = runS(*d, c.p, c.s, reduce(pic, f), 1.0 / f);
            const Diff dd = compare(small, reduce(full, f));
            // a bound per effect: the maths are the same at any size, what differs is sampling hard edges at another pixel grid
            // (Tiny Planet reads the nearest source pixel, as VEGAS's kernel does: its hard edges move by whole pixels)
            const double bound = c.id == kPlanet ? 3.5 : 0.5;
            check(dd.mean < bound, strfmt("%s at 1/%d: close to the full-size picture averaged down", c.label.c_str(), f), strfmt("mean %.3f largest %.1f levels", dd.mean, dd.mx));
        }
    }
}

static void threadTests()
{
    const Buf pic = picture(640, 360, 1);
    for (const Case& c : cases()) {
        const Def* d = ofx::builtin::findDef(c.id);
        if (!d) continue;
        const uint64_t want = hashBuf(runS(*d, c.p, c.s, pic, 1.0));
        std::vector<uint64_t> got(6, 0);
        std::vector<std::thread> th;
        for (int t = 0; t < 6; ++t)
            th.emplace_back([&, t] {
                uint64_t h = want;
                for (int i = 0; i < 3; ++i) h = hashBuf(runS(*d, c.p, c.s, pic, 1.0)) == want ? h : 0;
                got[(size_t)t] = h;
            });
        for (auto& t : th) t.join();
        bool same = true;
        for (uint64_t h : got) if (h != want) same = false;
        check(same, c.label + ": six threads rendering at once get what one thread gets");
    }
}

// ------------------------------------------------------------------ 4. the engine and the host
static Json strVal(const std::string& s)
{
    Json e = Json::object();
    e["s"] = s;
    return e;
}

static void engineChecks()
{
    Rig rig;
    for (const Case& c : cases()) {
        Json params = Json::object();
        for (const auto& kv : c.p) {
            Json e = Json::object(), a = Json::array();
            for (double x : kv.second) a.push(x);
            e["v"] = a;
            params[kv.first] = e;
        }
        for (const auto& kv : c.s) params[kv.first] = strVal(kv.second);
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        e.fx.push_back(slot(p, c.id, params));
        p.tracks[0].events.push_back(e);
        Project plain = rig.base();
        MediaItem m2 = rig.still(plain, "synth:0");
        plain.tracks[0].events.push_back(rig.event(plain, m2, 0, 5));
        rig.fresh();
        ImagePtr full = rig.render(p, 1.0, 1.0);
        const std::string why = rig.fx.error(p.tracks[0].events[0].fx[0].id);
        check(full && why.empty() && rig.fx.video(p.tracks[0].events[0].fx[0].id) != nullptr, c.label + ": the effect loads and renders through the OpenFX host", why);
        if (!full) continue;
        ImagePtr half = rig.render(p, 1.0, 0.5), quarter = rig.render(p, 1.0, 0.25);
        rig.fresh();
        ImagePtr none = rig.render(plain, 1.0, 1.0);
        check(meanDiff(full, none) > 0.05, c.label + ": it changes the picture", strfmt("%.3f", meanDiff(full, none)));
        double mx = 0;
        const double d2 = meanDiff(half, reduceImg(full, 2), &mx), d4 = meanDiff(quarter, reduceImg(full, 4));
        // (Tiny Planet: VEGAS's nearest-pixel read; at a quarter of 320 x 180 a source pixel is a large step)
        const double bound = c.id == kPlanet ? 6.0 : (c.id == kMesh ? 2.0 : 1.5);
        check(half && quarter && d2 < bound && d4 < bound, c.label + ": the engine's half and quarter size pictures agree with the full-size one", strfmt("1/2 mean %.3f max %.0f, 1/4 mean %.3f", d2, mx, d4));
        // the instance is told the render scale: it says it supports multi-resolution
        ofx::Plugin* pl = findVideoPlugin(c.id, nullptr);
        check(pl && pl->info.multiResolution && pl->safety == 2, c.label + ": described as multi-resolution and fully render thread safe");
        // parallel rendering of the frame (the engine's own threads) gives the same picture
        rig.fo.parallel = true;
        rig.fx.prepare(p);
        rig.fo.scale = 1.0;
        ImagePtr par = rig.video.render(p, 1.0, rig.fo);
        rig.fo.parallel = false;
        check(par && full && meanDiff(par, full) == 0, c.label + ": rendering with the engine's threads gives the same picture");
    }
}

// ------------------------------------------------------------------ 5. the sample projects
static bool isMagixId(const std::string& id) { return id == kCurves || id == kPlanet || id == kMesh || id == kUpscale; }

static void collectFx(Project& p, std::vector<FxSlot*>& out)
{
    p.forEachFx([&](FxSlot& s) { if (s.kind == "ofx" && isMagixId(s.pluginId)) out.push_back(&s); });
}

static bool sameParams(const Json& a, const Json& b, std::string& why)
{
    for (const auto& m : a.get("params").members()) {
        const Json& x = m.second;
        const Json& y = b.get("params").get(m.first);
        if (y.isNull()) { why = m.first + " is gone"; return false; }
        if (x.has("s")) {
            if (!y.has("s") || x.get("s").asString() != y.get("s").asString()) { why = m.first + " (text) changed"; return false; }
            continue;
        }
        const Json &xv = x.get("v"), &yv = y.get("v");
        if (xv.size() == 1 && yv.size() == 3) {         // a Double3D saved when it was one number: the number three times
            for (size_t i = 0; i < 3; ++i)
                if (std::fabs(xv.at(0).asDouble() - yv.at(i).asDouble()) > 1e-9) { why = m.first + " is not the old number three times"; return false; }
            continue;
        }
        if (xv.size() != yv.size()) { why = m.first + " has another number of values"; return false; }
        for (size_t i = 0; i < xv.size(); ++i)
            if (std::fabs(xv.at(i).asDouble() - yv.at(i).asDouble()) > 1e-9) { why = strfmt("%s value %zu: %g -> %g", m.first.c_str(), i, xv.at(i).asDouble(), yv.at(i).asDouble()); return false; }
        if (x.has("k")) {
            if (x.get("k").size() != y.get("k").size()) { why = m.first + " lost keyframes"; return false; }
        }
    }
    return true;
}

static void projectTests(const std::string& folder)
{
    std::vector<std::string> files;
    std::function<void(const std::string&)> walk = [&](const std::string& dir) {
        for (const std::string& n : listDir(dir)) {
            const std::string path = pathJoin(dir, n);
            if (dirExists(path)) walk(path);
            else if (endsWithNoCase(n, ".veg")) files.push_back(path);
        }
    };
    walk(folder);
    std::sort(files.begin(), files.end());
    int projects = 0, slots = 0, bad = 0;
    std::map<std::string, int> perId;
    for (const std::string& f : files) {
        Project p;
        std::string err;
        std::vector<std::string> notes;
        if (!loadProjectFileWithNotes(f, p, &err, &notes)) continue;
        std::vector<FxSlot*> mine;
        collectFx(p, mine);
        if (mine.empty()) continue;
        ++projects;
        FxRuntime fx;
        fx.prepare(p);
        std::vector<Json> before;
        for (FxSlot* s : mine) {
            ++slots;
            ++perId[s->pluginId];
            before.push_back(s->state);
            const std::string why = fx.error(s->id);
            if (!fx.video(s->id) || !why.empty()) { ++bad; std::printf("FAIL %s: %s did not load: %s\n", pathFileName(f).c_str(), s->pluginId.c_str(), why.c_str()); ++gFail; }
        }
        // through the live instance and a write and read of the .veg
        fx.captureStates(p);
        std::string why;
        bool keptLive = true;
        for (size_t i = 0; i < mine.size(); ++i)
            if (!sameParams(before[i], mine[i]->state, why)) { keptLive = false; std::printf("FAIL %s: %s through the instance: %s\n", pathFileName(f).c_str(), mine[i]->pluginId.c_str(), why.c_str()); }
        check(keptLive, pathFileName(f) + ": the values of the " + std::to_string(mine.size()) + " effects survive the live instances");
        std::vector<std::string> warn;
        const std::string bytes = writeVegasData(p, f, &warn);
        const std::string tmp = pathJoin("/tmp", "magix_roundtrip.veg");
        check(!bytes.empty() && writeFile(tmp, bytes), pathFileName(f) + ": written as a .veg");
        Project q;
        std::string err2;
        if (!loadProjectFile(tmp, q, &err2)) { check(false, pathFileName(f) + ": the written file reads back", err2); continue; }
        std::vector<FxSlot*> again;
        collectFx(q, again);
        bool ok = again.size() == mine.size();
        for (size_t i = 0; ok && i < mine.size(); ++i)
            if (again[i]->pluginId != mine[i]->pluginId || !sameParams(before[i], again[i]->state, why)) { ok = false; std::printf("FAIL %s: %s after a .veg round trip: %s\n", pathFileName(f).c_str(), mine[i]->pluginId.c_str(), why.c_str()); }
        check(ok, pathFileName(f) + ": the values survive a .veg write and read");
        // the log names the effects
        const std::string log = fxLogText(p, fx, f);
        bool named = true;
        for (const auto& kv : perId) if (log.find(kv.first) == std::string::npos && std::find_if(mine.begin(), mine.end(), [&](FxSlot* s) { return s->pluginId == kv.first; }) != mine.end()) named = false;
        check(named && log.find("built-in") != std::string::npos && log.find("instances made:") != std::string::npos, pathFileName(f) + ": the effect log lists them and says they are built-in");
        fx.releaseExcept({});
    }
    std::printf("INFO %d projects use the four effects, %d slots (", projects, slots);
    for (const auto& kv : perId) std::printf(" %s x%d", kv.first.c_str(), kv.second);
    std::printf(" )\n");
    if (projects > 0) check(bad == 0, "every use of the four effects in the sample projects loads", std::to_string(bad) + " failed");
}

// ------------------------------------------------------------------ the log
static void logTests()
{
    Rig rig;
    Project p = rig.base();
    MediaItem m = rig.still(p, "synth:0");
    Event e = rig.event(p, m, 0, 5);
    e.fx.push_back(slot(p, kCurves));
    e.fx.push_back(slot(p, kCurves));
    e.fx.push_back(slot(p, "com.NewBlue.NotHere"));
    p.tracks[0].events.push_back(e);
    rig.fx.prepare(p);
    const std::string log = fxLogText(p, rig.fx, "/some/where/test.veg");
    std::printf("---- fx-log.txt of the test project ----\n%s----\n", log.c_str());
    check(log.find(kCurves) != std::string::npos && log.find("used 2 times: 2 video event") != std::string::npos, "the log counts the uses of an effect");
    check(log.find("from: built-in") != std::string::npos && log.find("instances made: 2 of 2") != std::string::npos, "the log says where a built-in comes from and that its instances were made");
    check(log.find("com.NewBlue.NotHere") != std::string::npos && log.find("NOT FOUND") != std::string::npos && log.find("ERROR (1 time): ") != std::string::npos &&
              log.find("is not installed") != std::string::npos,
          "the log names a missing effect and the exact error");
    const std::string dir = "/tmp/magix_logdir";
    std::system("mkdir -p /tmp/magix_logdir");
    check(writeFxLog(p, rig.fx, "/some/where/test.veg", dir), "fx-log.txt is written");
    std::string back;
    check(readFile(dir + "/fx-log.txt", back) && back == log, "fx-log.txt holds the log");
}

int main(int argc, char** argv)
{
    const std::string samples = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    ofx::setTextRasterizer(blockRasterizer);
    curveTests();
    planetTests();
    meshTests();
    upscaleTests();
    scaleTests();
    threadTests();
    engineChecks();
    logTests();
    if (dirExists(samples)) projectTests(samples);
    else std::printf("INFO %s not found: the project tests are skipped\n", samples.c_str());
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
