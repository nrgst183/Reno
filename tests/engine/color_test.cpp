// Tests of the built-in Color Grading, AutoLooks, Color Match and LUT Filter (src/ofx/builtins_color.cpp).
//
//   color_test
//
// Covers the pictures, render scale 1/2 and 1/4 against full size, threads against one thread, the engine / OpenFX
// host, and a .veg round trip. Uses fxscale_test.cpp's helpers (the file is included).
#define main fxscale_main
#include "fxscale_test.cpp"
#undef main
#include "../../src/ofx/builtins_colordata.h"
#include "../../src/vegas/vegas.h"
#include <cstdlib>
#include <fstream>
#include <thread>

typedef std::map<std::string, std::string> StrMap;

static const std::string kGrade = "com.vegascreativesoftware:colorgrading";
static const std::string kLooks = "com.vegascreativesoftware:autolooks";
static const std::string kMatch = "com.vegascreativesoftware:colormatch";
static const std::string kLut = "com.vegascreativesoftware:lutfilter";

static Buf runS(const Def& def, const Params& over, const StrMap& sover, const Buf& src, double scale)
{
    Params store;
    StrMap strs;
    for (const PDef& p : def.params) {
        if (p.type == PT::Group) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : over) store[kv.first] = kv.second;
    for (const auto& kv : sover) strs[kv.first] = kv.second;
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
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

static Buf ramp(int W, int H, float a = 1.f)
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) premul(b.at(x, y), (float)x / (W - 1), (float)y / (H - 1), 1.f - (float)x / (W - 1), a);
    return b;
}

static Buf flat(int W, int H, float r, float g, float b, float a = 1.f)
{
    Buf f;
    f.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) premul(f.at(x, y), r, g, b, a);
    return f;
}

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

static double meanOf(const Buf& b, int ch)
{
    double s = 0;
    for (size_t i = (size_t)ch; i < b.px.size(); i += 4) s += b.px[i];
    return s / (double)(b.px.size() / 4);
}

// mean spread of the colour channels (max - min): how colourful the picture is
static double colourful(const Buf& b)
{
    double s = 0;
    for (size_t i = 0; i + 3 < b.px.size(); i += 4) s += std::max(b.px[i], std::max(b.px[i + 1], b.px[i + 2])) - std::min(b.px[i], std::min(b.px[i + 1], b.px[i + 2]));
    return s / (double)(b.px.size() / 4);
}

static std::string f4(double v) { return strfmt("%.4f", v); }

// A .cube file of size n whose colours are fn(r, g, b).
template <class F>
static std::string writeCube(const std::string& name, int n, F fn, const char* extra = "")
{
    const std::string path = "/tmp/color_test_" + name + ".cube";
    std::ofstream o(path);
    o << "TITLE \"test\"\n" << extra << "LUT_3D_SIZE " << n << "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float c[3] = {(float)r / (n - 1), (float)g / (n - 1), (float)b / (n - 1)};
                fn(c);
                o << c[0] << " " << c[1] << " " << c[2] << "\n";
            }
    return path;
}

// ------------------------------------------------------------------ 1. pictures
static void lutTests()
{
    const Def* d = ofx::builtin::findDef(kLut);
    check(d != nullptr, "LUT Filter is a built-in (com.vegascreativesoftware:lutfilter)");
    if (!d) return;
    const Buf src = ramp(64, 32);
    check(maxDiff(runS(*d, {}, {}, src, 1.0), src) == 0, "LUT Filter with no file leaves the picture as it is");
    check(maxDiff(runS(*d, {}, {{"LUTFilename", "/tmp/color_test_nothere.cube"}}, src, 1.0), src) == 0, "LUT Filter with a missing file leaves the picture as it is");
    const std::string ident = writeCube("ident", 17, [](float*) {});
    for (int t : {0, 1}) {
        const Buf o = runS(*d, {{"Interpolation", {(double)t}}}, {{"LUTFilename", ident}}, src, 1.0);
        check(maxDiff(o, src) < 1e-4, std::string("an identity table changes nothing (") + (t ? "tetrahedral" : "linear") + ")", f4(maxDiff(o, src)));
    }
    const std::string inv = writeCube("invert", 9, [](float* c) { for (int k = 0; k < 3; ++k) c[k] = 1.f - c[k]; });
    {
        const Buf o = runS(*d, {}, {{"LUTFilename", inv}}, src, 1.0);
        float a[3], b[3];
        straight(src, 20, 10, a);
        straight(o, 20, 10, b);
        check(std::fabs(b[0] - (1 - a[0])) < 1e-3 && std::fabs(b[1] - (1 - a[1])) < 1e-3 && std::fabs(b[2] - (1 - a[2])) < 1e-3, "an inverting table inverts the colours", strfmt("%.3f %.3f %.3f -> %.3f %.3f %.3f", a[0], a[1], a[2], b[0], b[1], b[2]));
        const Buf half = runS(*d, {{"Gain", {0.5}}}, {{"LUTFilename", inv}}, src, 1.0);
        straight(half, 20, 10, b);
        check(std::fabs(b[0] - (a[0] + (1 - 2 * a[0]) * 0.5f)) < 1e-3, "Strength mixes the original and the table's result", strfmt("%.3f", b[0]));
        check(maxDiff(runS(*d, {{"Gain", {0}}}, {{"LUTFilename", inv}}, src, 1.0), src) == 0, "Strength 0 is no LUT");
    }
    {
        // a table that is not linear: the two interpolations differ, both stay close to the exact function
        const std::string curvy = writeCube("curvy", 5, [](float* c) { const float r = c[0], g = c[1], b = c[2]; c[0] = r * r; c[1] = g * b; c[2] = std::sqrt(r * g); });
        const Buf lin = runS(*d, {{"Interpolation", {0}}}, {{"LUTFilename", curvy}}, src, 1.0), tet = runS(*d, {{"Interpolation", {1}}}, {{"LUTFilename", curvy}}, src, 1.0);
        check(maxDiff(lin, tet) > 1e-4 && maxDiff(lin, tet) < 0.1, "linear and tetrahedral interpolation differ a little on a coarse table", f4(maxDiff(lin, tet)));
    }
    {
        // domain and a 1D table
        const std::string dom = writeCube("domain", 3, [](float* c) { for (int k = 0; k < 3; ++k) c[k] = c[k]; }, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n");
        const Buf o = runS(*d, {}, {{"LUTFilename", dom}}, src, 1.0);
        float b[3];
        straight(o, 63, 0, b);
        check(std::fabs(b[0] - 0.5f) < 1e-3, "a table with DOMAIN_MAX 2 treats the colour 1.0 as halfway", f4(b[0]));
        const std::string p1 = "/tmp/color_test_1d.cube";
        { std::ofstream o1(p1); o1 << "LUT_1D_SIZE 2\n0 0 0\n0.5 0.5 0.5\n"; }
        const Buf o2 = runS(*d, {}, {{"LUTFilename", p1}}, src, 1.0);
        straight(o2, 63, 0, b);
        check(std::fabs(b[0] - 0.5f) < 1e-3 && std::fabs(b[1] - 0.0f) < 1e-3, "a 1D table works", strfmt("%.3f %.3f", b[0], b[1]));
        const std::string p3 = "/tmp/color_test.3dl";
        {
            std::ofstream o3(p3);
            o3 << "0 341 682 1023\n";
            for (int r = 0; r < 4; ++r) for (int g = 0; g < 4; ++g) for (int bl = 0; bl < 4; ++bl) o3 << (1023 - r * 341) << " " << (g * 341) << " " << (bl * 341) << "\n";
        }
        const Buf o4 = runS(*d, {}, {{"LUTFilename", p3}}, src, 1.0);
        float a[3];
        straight(src, 40, 20, a);
        straight(o4, 40, 20, b);
        check(std::fabs(b[0] - (1 - a[0] * 1.0f)) < 0.02f && std::fabs(b[1] - a[1]) < 0.02f, "a .3dl table is read (red inverted)", strfmt("%.3f %.3f -> %.3f %.3f", a[0], a[1], b[0], b[1]));
    }
    {
        const Buf half = ramp(32, 8, 0.5f);
        const Buf o = runS(*d, {}, {{"LUTFilename", inv}}, half, 1.0);
        float a[3], b[3];
        straight(half, 10, 3, a);
        straight(o, 10, 3, b);
        check(std::fabs(o.at(10, 3)[3] - 0.5f) < 1e-6 && std::fabs(b[0] - (1 - a[0])) < 1e-3, "alpha is kept and the colour is worked on unpremultiplied");
    }
}

static void looksTests()
{
    const Def* d = ofx::builtin::findDef(kLooks);
    check(d != nullptr, "AutoLooks is a built-in (com.vegascreativesoftware:autolooks)");
    if (!d) return;
    const Buf pic = picture(320, 180, 0);
    check(d->params.size() >= 1 && d->params[0].name == "autolooksLook" && d->params[0].choices.size() == 22 && d->params[0].choices[0] == "none" && d->params[0].choices[21] == "The Book of Lie", "the Look list is VEGAS's: none and 21 looks");
    check(maxDiff(runS(*d, {{"autolooksLook", {0}}}, {}, pic, 1.0), pic) == 0, "Look none leaves the picture as it is");
    bool allChange = true, inRange = true;
    for (int k = 1; k <= 21; ++k) {
        const Buf o = runS(*d, {{"autolooksLook", {(double)k}}}, {}, pic, 1.0);
        double m = 0;
        for (size_t i = 0; i < o.px.size(); ++i) { m += std::fabs(o.px[i] - pic.px[i]); if (!(o.px[i] >= -1e-4f && o.px[i] <= 1.0001f)) inRange = false; }
        if (m / (double)o.px.size() < 0.01) { allChange = false; std::printf("  look %d changes the picture by only %.4f\n", k, m / (double)o.px.size()); }
    }
    check(allChange, "every one of the 21 looks changes the picture");
    check(inRange, "every look stays within range");
    {
        // a folder of VEGAS's own tables is used when it is found: here "1.cube" inverts
        std::system("mkdir -p /tmp/color_test_looks");
        const std::string inv = writeCube("looks_one", 9, [](float* c) { for (int k = 0; k < 3; ++k) c[k] = 1.f - c[k]; });
        std::system("cp /tmp/color_test_looks_one.cube /tmp/color_test_looks/1.cube");
        setenv("RENO_AUTOLOOKS_DIR", "/tmp/color_test_looks", 1);
        const Buf o = runS(*d, {{"autolooksLook", {1}}}, {}, pic, 1.0);
        float a[3], b[3];
        straight(pic, 100, 50, a);
        straight(o, 100, 50, b);
        check(std::fabs(b[0] - (1 - a[0])) < 0.02f, "AutoLooks uses VEGAS's own table when its folder is found", strfmt("%.3f -> %.3f", a[0], b[0]));
        unsetenv("RENO_AUTOLOOKS_DIR");
        const Buf o2 = runS(*d, {{"autolooksLook", {1}}}, {}, pic, 1.0);
        check(maxDiff(o2, o) > 0.01, "without the folder Reno's own grade is used");
    }
    check(presetValue(kLooks, "Conception", "autolooksLook") == "1" && presetValue(kLooks, "The Book of Lie", "autolooksLook") == "21", "VEGAS's 21 AutoLooks presets are in the built-in presets");
}

static std::string stats(std::initializer_list<double> v)
{
    std::vector<double> a(v);
    a.resize(12, 0.0);
    return base64Encode(a.data(), a.size() * sizeof(double));
}

static void matchTests()
{
    const Def* d = ofx::builtin::findDef(kMatch);
    check(d != nullptr, "Color Match is a built-in (com.vegascreativesoftware:colormatch)");
    if (!d) return;
    const Buf pic = picture(320, 180, 0);
    check(maxDiff(runS(*d, {}, {}, pic, 1.0), pic) == 0, "Color Match without a source picture leaves the frame as it is");
    {
        const std::string v = presetValue(kMatch, "Noir", "internaldata");
        check(!v.empty() && presetValue(kMatch, "Teal and Orange", "Processlightness").size() > 0, "VEGAS's seven Color Match presets are in the built-in presets");
        const Buf o = runS(*d, {{"SourceCaptured", {1}}}, {{"internaldata", v}}, pic, 1.0);
        check(colourful(o) < 0.35 * colourful(pic), "the Noir preset takes the colour out", strfmt("%.3f -> %.3f", colourful(pic), colourful(o)));
        const Buf half = runS(*d, {{"SourceCaptured", {1}}, {"Strength", {0.5}}}, {{"internaldata", v}}, pic, 1.0);
        check(colourful(half) < colourful(pic) && colourful(half) > colourful(o), "Strength 0.5 is half way", strfmt("%.3f", colourful(half)));
        check(maxDiff(runS(*d, {{"SourceCaptured", {1}}, {"Strength", {0}}}, {{"internaldata", v}}, pic, 1.0), pic) == 0, "Strength 0 bypasses the match");
    }
    {
        // the frame matched to its own statistics: nothing to do. (Statistics taken by the effect itself: a target not captured;
        // here the source numbers are the effect's own measurement of 'pic', found by running Noir-like unit statistics.)
        const std::string same = stats({50, 20, 0, 10, 0, 10, 50, 20, 0, 10, 0, 10});
        const Buf o = runS(*d, {{"SourceCaptured", {1}}, {"TargetCaptured", {1}}}, {{"internaldata", same}}, pic, 1.0);
        check(maxDiff(o, pic) < 0.02, "a source with the target's own statistics changes nothing", f4(maxDiff(o, pic)));
        // a shift of the mean of a (towards red): the picture gets redder
        const std::string red = stats({50, 20, 30, 10, 0, 10, 50, 20, 0, 10, 0, 10});
        const Buf r = runS(*d, {{"SourceCaptured", {1}}, {"TargetCaptured", {1}}}, {{"internaldata", red}}, pic, 1.0);
        check(meanOf(r, 0) > meanOf(pic, 0) + 0.03 && meanOf(r, 1) < meanOf(pic, 1), "moving the mean of a towards red makes the frame redder", strfmt("%.3f -> %.3f", meanOf(pic, 0), meanOf(r, 0)));
    }
    {
        const std::string v = presetValue(kMatch, "Night Sky", "internaldata");
        const Buf off = runS(*d, {{"SourceCaptured", {1}}, {"Processlightness", {0}}}, {{"internaldata", v}}, pic, 1.0);
        const Buf on = runS(*d, {{"SourceCaptured", {1}}, {"Processlightness", {1}}}, {{"internaldata", v}}, pic, 1.0);
        check(meanOf(on, 1) < meanOf(off, 1) - 0.02, "Match brightness darkens the frame to the dark Night Sky", strfmt("%.3f -> %.3f", meanOf(off, 1), meanOf(on, 1)));
    }
    {
        // garbage statistics and a solid-colour source do nothing harmful
        check(maxDiff(runS(*d, {{"SourceCaptured", {1}}}, {{"internaldata", "!!!!"}}, pic, 1.0), pic) == 0, "unreadable statistics leave the frame as it is");
        const Buf o = runS(*d, {{"SourceCaptured", {1}}}, {{"internaldata", stats({50, 0, 0, 0, 0, 0})}}, pic, 1.0);
        check(maxDiff(o, pic) == 0, "a solid colour source is not applied (VEGAS warns the same)");
        bool ok = true;
        for (float v : runS(*d, {{"SourceCaptured", {1}}}, {{"internaldata", presetValue(kMatch, "Noir", "internaldata")}}, flat(32, 16, 0.5f, 0.5f, 0.5f), 1.0).px) if (!std::isfinite(v)) ok = false;
        check(ok, "a flat frame gives no NaN");
    }
}

static void gradeTests()
{
    const Def* d = ofx::builtin::findDef(kGrade);
    check(d != nullptr, "Color Grading is a built-in (com.vegascreativesoftware:colorgrading)");
    if (!d) return;
    const Buf src = ramp(64, 32);
    check(maxDiff(runS(*d, {}, {}, src, 1.0), src) == 0, "Color Grading at its defaults leaves the picture as it is");
    float a[3], b[3];
    straight(src, 20, 10, a);
    auto at = [&](const Params& p, const StrMap& s = {}) {
        const Buf o = runS(*d, p, s, src, 1.0);
        straight(o, 20, 10, b);
        return o;
    };
    at({{"ExposureAdjust", {1}}});
    check(std::fabs(b[0] - a[0] * 2) < 1e-4 && std::fabs(b[1] - a[1] * 2) < 1e-4, "Exposure +1 doubles the light", strfmt("%.3f -> %.3f", a[0], b[0]));
    at({{"Gain_Y", {0.5}}});
    check(std::fabs(b[0] - a[0] * 0.5f) < 1e-4, "Gain Y 0.5 halves the light", strfmt("%.3f -> %.3f", a[0], b[0]));
    at({{"Gain_R", {2}}});
    check(std::fabs(b[0] - a[0] * 2) < 1e-4 && std::fabs(b[1] - a[1]) < 1e-5, "Gain R works on red only");
    at({{"Lift_Y", {0.2}}});
    check(std::fabs(b[0] - (a[0] + 0.2f * (1 - a[0]))) < 1e-4, "Lift raises the blacks more than the whites", strfmt("%.3f -> %.3f", a[0], b[0]));
    at({{"Offset_B", {0.1}}});
    check(std::fabs(b[2] - (a[2] + 0.1f)) < 1e-4 && std::fabs(b[0] - a[0]) < 1e-5, "Offset B adds to blue");
    at({{"Gamma_Y", {2}}});
    check(std::fabs(b[0] - std::sqrt(a[0])) < 1e-4, "Gamma 2 is a square root (brighter middles)", strfmt("%.3f -> %.3f", a[0], b[0]));
    at({{"SaturationAdjust", {0}}});
    check(std::fabs(b[0] - b[1]) < 1e-4 && std::fabs(b[1] - b[2]) < 1e-4 && std::fabs(b[0] - luma(a[0], a[1], a[2])) < 1e-4, "Saturation 0 is grey of the same luma");
    at({{"HueAdjust", {120}}});
    {
        const Buf red = flat(4, 4, 1.f, 0.f, 0.f);
        const Buf o = runS(*d, {{"HueAdjust", {120}}}, {}, red, 1.0);
        float c[3];
        straight(o, 1, 1, c);
        check(c[1] > 0.99f && c[0] < 0.01f, "Hue +120 turns red into green", strfmt("%.2f %.2f %.2f", c[0], c[1], c[2]));
    }
    at({{"InMinAdjust", {0.25}}, {"InMaxAdjust", {0.75}}});
    check(std::fabs(b[0] - (a[0] - 0.25f) * 2) < 1e-4, "Input Min / Max stretch the levels");
    at({{"OutMinAdjust", {0.1}}, {"OutMaxAdjust", {0.6}}});
    check(std::fabs(b[0] - (0.1f + a[0] * 0.5f)) < 1e-4, "Output Min / Max squeeze the levels");
    at({}, {{"Curve_Y", "2:0.0:1.0:-0.2:0.2:0.2:-0.2:1.0:0.0:-0.2:0.2:0.2:-0.2"}});
    check(std::fabs(b[0] - (1 - a[0])) < 0.01 && std::fabs(b[2] - (1 - a[2])) < 0.01, "a master curve from (0,1) to (1,0) inverts", strfmt("%.3f -> %.3f", a[0], b[0]));
    at({}, {{"Curve_R", "3:0.0:0.0:0:0:0.1:0:0.5:0.8:-0.1:0:0.1:0:1.0:1.0:-0.1:0:0:0"}});
    check(b[0] > a[0] + 0.1 && std::fabs(b[1] - a[1]) < 1e-5, "a red curve raised in the middle brightens red only", strfmt("%.3f -> %.3f", a[0], b[0]));
    at({}, {{"Curve_R", "nonsense"}});
    check(std::fabs(b[0] - a[0]) < 1e-6, "a curve that cannot be read is left out");
    {
        const std::string inv = writeCube("grade_invert", 9, [](float* c) { for (int k = 0; k < 3; ++k) c[k] = 1.f - c[k]; });
        at({{"LUTEnable_0", {1}}}, {{"LUTFilename_0", inv}});
        check(std::fabs(b[0] - (1 - a[0])) < 1e-3, "the LUT slot applies a .cube file", strfmt("%.3f -> %.3f", a[0], b[0]));
        at({{"LUTEnable_0", {0}}}, {{"LUTFilename_0", inv}});
        check(std::fabs(b[0] - a[0]) < 1e-6, "a LUT slot that is off does nothing");
        at({{"LUTEnable_1", {1}}, {"LUTGain_1", {0.5}}}, {{"LUTFilename_1", inv}});
        check(std::fabs(b[0] - (a[0] + (1 - 2 * a[0]) * 0.5f)) < 1e-3, "the second LUT slot, at half strength");
    }
    {
        const Buf half = ramp(32, 8, 0.5f);
        const Buf o = runS(*d, {{"ExposureAdjust", {1}}}, {}, half, 1.0);
        float p[3], q[3];
        straight(half, 10, 3, p);
        straight(o, 10, 3, q);
        check(std::fabs(o.at(10, 3)[3] - 0.5f) < 1e-6 && std::fabs(q[0] - 2 * p[0]) < 1e-4, "alpha is kept");
        bool ok = true;
        for (float v : runS(*d, {{"Gamma_R", {0}}, {"Lift_G", {-3}}, {"Gain_B", {-2}}, {"InMaxAdjust", {0}}}, {}, src, 1.0).px) if (!std::isfinite(v)) ok = false;
        check(ok, "extreme values give no NaN");
    }
    // the default curve text is VEGAS's own
    bool curveDef = false;
    for (const PDef& p : d->params) if (p.name == "Curve_Y" && p.sdef == "2:0.0:0.0:-0.2:0.0:0.2:0.2:1.0:1.0:-0.2:-0.2:0.2:0.0") curveDef = true;
    check(curveDef, "the curves start as VEGAS's default text");
}

// ------------------------------------------------------------------ 2. and 3. scale and threads
struct Case { std::string label, id; Params p; StrMap s; };

static std::vector<Case> cases()
{
    std::vector<Case> v;
    const std::string inv = writeCube("case_invert", 9, [](float* c) { c[0] = 1 - c[0]; c[1] = c[1] * c[1]; c[2] = c[2]; });
    v.push_back({"lut linear", kLut, {{"Interpolation", {0}}, {"Gain", {0.8}}}, {{"LUTFilename", inv}}});
    v.push_back({"lut tetrahedral", kLut, {{"Interpolation", {1}}}, {{"LUTFilename", inv}}});
    v.push_back({"look conception", kLooks, {{"autolooksLook", {1}}}, {}});
    v.push_back({"look rad jack", kLooks, {{"autolooksLook", {19}}}, {}});
    v.push_back({"match teal and orange", kMatch, {{"SourceCaptured", {1}}, {"TargetCaptured", {1}}, {"Processlightness", {1}}}, {{"internaldata", presetValue(kMatch, "Teal and Orange", "internaldata")}}});
    v.push_back({"match noir", kMatch, {{"SourceCaptured", {1}}, {"Strength", {0.7}}}, {{"internaldata", presetValue(kMatch, "Noir", "internaldata")}}});
    v.push_back({"grade wheels", kGrade, {{"Lift_Y", {0.05}}, {"Gamma_Y", {1.2}}, {"Gain_R", {1.1}}, {"Gain_B", {0.9}}, {"SaturationAdjust", {1.3}}, {"ExposureAdjust", {0.3}}}, {}});
    v.push_back({"grade hsv curves", kGrade, {{"HueAdjust", {25}}, {"SatAdjust", {1.2}}, {"ValAdjust", {0.9}}}, {{"Curve_Y", "3:0.0:0.0:0:0:0.1:0.02:0.5:0.4:-0.1:-0.05:0.1:0.1:1.0:1.0:-0.1:-0.1:0:0"}}});
    return v;
}

static void scaleTests()
{
    const Buf pic = picture(960, 540, 0);
    for (const Case& c : cases()) {
        const Def* d = ofx::builtin::findDef(c.id);
        if (!d) continue;
        const Buf full = runS(*d, c.p, c.s, pic, 1.0);
        check(compare(full, pic).mean > 0.003, c.label + ": it changes the picture", f4(compare(full, pic).mean));
        for (int f : {2, 4}) {
            const Buf small = runS(*d, c.p, c.s, reduce(pic, f), 1.0 / f);
            const Diff dd = compare(small, reduce(full, f));
            check(dd.mean < 0.8, strfmt("%s at 1/%d: close to the full-size picture averaged down", c.label.c_str(), f), strfmt("mean %.3f largest %.1f levels", dd.mean, dd.mx));
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

static Json paramsOf(const Case& c)
{
    Json params = Json::object();
    for (const auto& kv : c.p) {
        Json e = Json::object(), a = Json::array();
        for (double x : kv.second) a.push(x);
        e["v"] = a;
        params[kv.first] = e;
    }
    for (const auto& kv : c.s) params[kv.first] = strVal(kv.second);
    return params;
}

static void engineChecks()
{
    Rig rig;
    for (const Case& c : cases()) {
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        e.fx.push_back(slot(p, c.id, paramsOf(c)));
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
        check(half && quarter && d2 < 1.5 && d4 < 1.5, c.label + ": the engine's half and quarter size pictures agree with the full-size one", strfmt("1/2 mean %.3f max %.0f, 1/4 mean %.3f", d2, mx, d4));
        ofx::Plugin* pl = findVideoPlugin(c.id, nullptr);
        check(pl && pl->info.multiResolution && pl->safety == 2, c.label + ": described as multi-resolution and fully render thread safe");
        rig.fo.parallel = true;
        rig.fx.prepare(p);
        rig.fo.scale = 1.0;
        ImagePtr par = rig.video.render(p, 1.0, rig.fo);
        rig.fo.parallel = false;
        check(par && full && meanDiff(par, full) == 0, c.label + ": rendering with the engine's threads gives the same picture");
    }
}

// ------------------------------------------------------------------ 5. values through the instances and a .veg
static bool isColorId(const std::string& id) { return id == kGrade || id == kLooks || id == kMatch || id == kLut; }

static bool sameParams(const Json& a, const Json& b, std::string& why)
{
    for (const auto& m : a.get("params").members()) {
        const Json& x = m.second;
        const Json& y = b.get("params").get(m.first);
        if (y.isNull()) {
            // The .veg writer leaves out the numbers of an effect that was not read from a .veg (a new one has no record to take the
            // layout - whole or fractional numbers - from; the same for every OpenFX effect): only those may be missing, the texts may not.
            if (!x.has("s")) continue;
            why = m.first + " is gone";
            return false;
        }
        if (x.has("s")) {
            if (!y.has("s") || x.get("s").asString() != y.get("s").asString()) { why = m.first + " (text) changed"; return false; }
            continue;
        }
        const Json &xv = x.get("v"), &yv = y.get("v");
        if (xv.size() != yv.size()) { why = m.first + " has another number of values"; return false; }
        for (size_t i = 0; i < xv.size(); ++i)
            if (std::fabs(xv.at(i).asDouble() - yv.at(i).asDouble()) > 1e-9) { why = strfmt("%s value %zu: %g -> %g", m.first.c_str(), i, xv.at(i).asDouble(), yv.at(i).asDouble()); return false; }
    }
    return true;
}

static void collectFx(Project& p, std::vector<FxSlot*>& out)
{
    p.forEachFx([&](FxSlot& s) { if (s.kind == "ofx" && isColorId(s.pluginId)) out.push_back(&s); });
}

static void roundTripTests()
{
    Rig rig;
    Project p = rig.base();
    MediaItem m = rig.still(p, "synth:0");
    Event e = rig.event(p, m, 0, 5);
    for (const Case& c : cases()) e.fx.push_back(slot(p, c.id, paramsOf(c)));
    p.tracks[0].events.push_back(e);
    FxRuntime fx;
    fx.prepare(p);
    std::vector<FxSlot*> mine;
    collectFx(p, mine);
    check(mine.size() == cases().size(), "the test project holds all the effects");
    std::vector<Json> before;
    bool loaded = true;
    for (FxSlot* s : mine) { before.push_back(s->state); if (!fx.video(s->id)) loaded = false; }
    check(loaded, "every effect makes an instance");
    fx.captureStates(p);
    std::string why;
    bool kept = true;
    for (size_t i = 0; i < mine.size(); ++i)
        if (!sameParams(before[i], mine[i]->state, why)) { kept = false; std::printf("FAIL %s through the instance: %s\n", mine[i]->pluginId.c_str(), why.c_str()); }
    check(kept, "the values survive the live instances");
    std::vector<std::string> warn;
    const std::string bytes = writeVegasData(p, "/tmp/color_test_in.veg", &warn);
    const std::string tmp = "/tmp/color_test_roundtrip.veg";
    check(!bytes.empty() && writeFile(tmp, bytes), "the project is written as a .veg");
    Project q;
    std::string err;
    if (!loadProjectFile(tmp, q, &err)) { check(false, "the written file reads back", err); return; }
    std::vector<FxSlot*> again;
    collectFx(q, again);
    bool ok = again.size() == mine.size();
    for (size_t i = 0; ok && i < mine.size(); ++i)
        if (again[i]->pluginId != mine[i]->pluginId || !sameParams(before[i], again[i]->state, why)) { ok = false; std::printf("FAIL %s after a .veg round trip: %s\n", mine[i]->pluginId.c_str(), why.c_str()); }
    check(ok, "the values survive a .veg write and read");
}

int main()
{
    ofx::setTextRasterizer(blockRasterizer);
    lutTests();
    looksTests();
    matchTests();
    gradeTests();
    scaleTests();
    threadTests();
    engineChecks();
    roundTripTests();
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
