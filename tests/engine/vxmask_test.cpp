// Tests of the built-in Bezier Masking, Bump Map, Stereoscopic 3D Adjust, Lens Correction, Smart Zoom, Warp Flow and
// Time Warp (src/ofx/builtins_mask*.cpp), their on-screen controls, and VEGAS's Dual Fish Eye Stitching identifier.
//
//   vxmask_test
//
// Covers definitions (VEGAS's identifiers, parameter names, types, choices, labels), each effect's picture, render
// scale 1/2 and 1/4, threads, the engine / OpenFX host, and the on-screen controls driven through the host's overlay
// calls. Uses fxscale_test.cpp's helpers (the file is included).
#define main fxscale_main
#include "fxscale_test.cpp"
#undef main
#include "../../src/ofx/builtins_mask_internal.h"
#include "../../src/engine/fxlog.h"
#include "../../src/vegas/vegas.h"
#include <set>
#include <thread>

static const std::string kBz = "com.vegascreativesoftware:bzmasking";
static const std::string kBump = "com.vegascreativesoftware:bumpmap";
static const std::string kBumpFilter = "com.vegascreativesoftware:bumpmapfilter";
static const std::string kStereo = "com.vegascreativesoftware:Filters.Stereoscopic3DAdjust";
static const std::string kLens = "de.magix:LensCorrection";
static const std::string kFlow = "de.magix:WarpFlowTransition";
static const std::string kTimeWarp = "de.magix:TimeWarp";
static const std::string kZoom = "com.sonycreativesoftware:smart_zoom";
static const std::string kDual = "com.magix.ofx.vr.dual.stich";

typedef std::map<std::string, std::string> StrMap;

static Buf runS(const Def& def, const Params& over, const StrMap& sover, const Buf& src, double scale = 1.0, const Buf* src2 = nullptr, double progress = 0)
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
    c.time = 3;
    c.fps = 30;
    c.src = src;
    if (src2) c.src2 = *src2;
    c.progress = progress;
    c.out.resize(src.w, src.h);
    c.frameW = src.w;
    c.frameH = src.h;
    c.scale = scale;
    def.render(c);
    return c.out;
}

static double maxDiff(const Buf& a, const Buf& b)
{
    double m = 0;
    if (a.px.size() != b.px.size()) return 1e9;
    for (size_t i = 0; i < a.px.size(); ++i) m = std::max(m, (double)std::fabs(a.px[i] - b.px[i]));
    return m;
}
static std::string f3(double v) { return strfmt("%.4f", v); }

// A horizontal ramp, the red channel x / (W - 1), the blue channel y / (H - 1) (y down), green 0.5, opaque.
static Buf ramps(int W, int H)
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) premul(b.at(x, y), (float)x / (W - 1), 0.5f, (float)y / (H - 1), 1.f);
    return b;
}

// A smooth texture (a sum of waves) that optical flow can follow, shifted by (dx, dy) pixels (so B(x) = A(x - d)).
static Buf texture(int W, int H, double dx, double dy)
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double u = (x + 0.5 - dx) / W, v = (y + 0.5 - dy) / H;
            const double r = 0.5 + 0.25 * std::sin(u * 17.0 + 1.0) * std::cos(v * 11.0) + 0.2 * std::sin(u * 5.0 + v * 7.0);
            const double g = 0.5 + 0.3 * std::sin(u * 9.0 - v * 13.0 + 0.5) + 0.1 * std::cos(u * 23.0);
            const double bl = 0.5 + 0.3 * std::cos(u * 7.0 + v * 19.0) * std::sin(v * 5.0);
            premul(b.at(x, y), (float)clampd(r, 0, 1), (float)clampd(g, 0, 1), (float)clampd(bl, 0, 1), 1.f);
        }
    return b;
}

// Rows are y down in a Buf; a mask path in the frame's fractions is y up.
static std::string rectPath(double x0, double y0, double x1, double y1)
{
    BezPath p;
    auto pt = [&](double x, double y) { BezPoint b; b.x = b.ix = b.ox = x; b.y = b.iy = b.oy = y; p.pts.push_back(b); };
    pt(x0, y1); pt(x1, y1); pt(x1, y0); pt(x0, y0);
    return formatBezPath(p);
}

// A Curve-type mask n (0..4) outlined by this text, enabled.
static void curve(Params& p, StrMap& s, int n, const std::string& path)
{
    p[maskParam("Type", n)] = {3};
    p[maskParam("Enable", n)] = {1};
    s[maskParam("AnchorList", n)] = path;
}

// Warp Flow with its internal settings given (VEGAS's has none): quality 0..2, smoothness, strength, and the flow field view.
static Buf runFlow(const Buf& A, const Buf& B, double t, int quality = 1, bool show = false, double scale = 1.0)
{
    Params store;
    StrMap strs;
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
    c.src = A;
    c.src2 = B;
    c.progress = t;
    c.out.resize(A.w, A.h);
    c.frameW = A.w;
    c.frameH = A.h;
    c.scale = scale;
    warpFlowForTest(c, quality, 0.5, 1.0, show);
    return c.out;
}


// What VEGAS's plug-in descriptions (Vfx1 / Filters / MagixCVFx / spica_cutout resource files) list for each effect:
// every parameter's type, name and label, the choices' labels in order, and its group. "G" group, "D" double, "D2",
// "D3", "I" integer, "B" boolean, "C" choice, "S" string, "X" custom, "P" push button.
struct Want { const char* kind; std::string name; std::string label; std::vector<std::string> choices; std::string parent; };

static std::vector<Want> wantBump()
{
    return {{"G", "DualLocation3DAnd2D", "Location", {}, ""}, {"D3", "DualLocation3DAnd2DSrc", "Source", {}, "DualLocation3DAnd2D"},
            {"D2", "DualLocation3DAnd2DDst", "Destination", {}, "DualLocation3DAnd2D"},
            {"C", "Type", "Light type", {"Directional", "Spotlight", "Omnidirectional"}, ""}, {"D", "Intensity", "Intensity", {}, ""},
            {"D", "Focus", "Focus", {}, ""}, {"D", "Ambience", "Ambience", {}, ""}, {"D", "Shininess", "Shininess", {}, ""},
            {"D", "BumpHeight", "Bump height", {}, ""}, {"C", "BumpChannel", "Bump channel", {"Red", "Green", "Blue", "Alpha"}, ""},
            {"B", "WhiteIsHigh", "White is high", {}, ""}, {"B", "FlipInputSurfaces", "Flip input surfaces", {}, ""}};
}
static std::vector<Want> wantBz()
{
    std::vector<Want> w = {{"G", "General Options", "General Options", {}, ""}, {"B", "Invert", "Invert", {}, "General Options"},
                           {"B", "MaskFx", "Mask FX", {}, "General Options"}, {"D", "Blend", "Blend", {}, "General Options"}};
    for (int i = 0; i < 5; ++i) {
        const std::string n = std::to_string(i + 1), g = maskParam("Mask", i), t = maskParam("Tracking", i), o = maskParam("OptionsTrk", i);
        auto P = [&](const char* b) { return maskParam(b, i); };
        w.push_back({"G", g, "Mask " + n, {}, ""});
        w.push_back({"B", P("Enable"), "Enable " + n, {}, g});
        w.push_back({"C", P("Type"), "Type", {"Oval", "Rectangle", "Diamond", "Curve"}, g});
        w.push_back({"B", P("EditMode"), "Edit Mode", {}, g});
        w.push_back({"C", P("EditTool"), "Edit Tool", {"Normal", "Add", "Delete", "Split"}, g});
        w.push_back({"D", P("Width"), "Width " + n, {}, g});
        w.push_back({"D", P("Height"), "Height " + n, {}, g});
        w.push_back({"D", P("Angle"), "Angle " + n, {}, g});
        w.push_back({"D", P("Opacity"), "Opacity " + n, {}, g});
        w.push_back({"D", P("Feather"), "Feather " + n, {}, g});
        w.push_back({"C", P("FeatherStyle"), "Feather Style " + n, {"In", "Out", "Both"}, g});
        w.push_back({"D2", P("Location"), "Location " + n, {}, g});
        w.push_back({"D2", P("AnchorScale"), "Anchor Scale", {}, g});
        w.push_back({"X", P("AnchorList"), "Anchor List", {}, g});
        w.push_back({"G", t, "Tracking", {}, g});
        w.push_back({"G", o, "Options", {}, t});
        w.push_back({"C", P("PrecisionTrk"), "Precision", {"Normal", "High"}, o});
        w.push_back({"C", P("ModeTrk"), "Mode", {"Location", "Rotation & Location", "Shape & Location", "Perspective"}, o});
        w.push_back({"C", P("DirectionTrk"), "Direction", {"Forward", "Backward", "Both"}, o});
        w.push_back({"D", P("TrkShear"), "Shear", {}, o});
        w.push_back({"I", P("KFIntervalTrk"), "Keyframe Interval", {}, o});
        w.push_back({"P", P("StartTrk"), "Start", {}, t});
    }
    return w;
}
static std::vector<Want> wantStereo()
{
    return {{"D", "Horizontal", "Horizontal offset", {}, ""}, {"G", "AdjustmentsGroup", "Corrections", {}, ""},
            {"D", "Vertical", "Vertical offset", {}, "AdjustmentsGroup"}, {"D", "Zoom", "Zoom", {}, "AdjustmentsGroup"},
            {"D", "Keystone", "Keystone", {}, "AdjustmentsGroup"}, {"D", "Rotation", "Rotation", {}, "AdjustmentsGroup"},
            {"C", "FlipHorizontal", "Flip horizontal", {"Off", "Left", "Right", "Both"}, "AdjustmentsGroup"},
            {"C", "FlipVertical", "Flip vertical", {"Off", "Left", "Right", "Both"}, "AdjustmentsGroup"},
            {"P", "AutoAlignButton", "Auto correct", {}, "AdjustmentsGroup"}, {"B", "AutoCrop", "Automatic crop", {}, ""},
            {"D", "Crop", "Crop", {}, ""}, {"G", "FloatingWindowsGroup", "Floating windows", {}, ""},
            {"D", "FloatingWindowLeft", "Left", {}, "FloatingWindowsGroup"}, {"D", "FloatingWindowRight", "Right", {}, "FloatingWindowsGroup"}};
}
static std::vector<Want> wantLens()
{
    return {{"C", "Localization", "", {"Select lens", "Nothing found"}, ""}, {"G", "GroupDb", "Lens Model", {}, ""},
            {"S", "Query", "Search for", {}, "GroupDb"}, {"C", "QueryResult", "Found Lenses", {"Nothing found"}, "GroupDb"},
            {"B", "ShowMesh", "Show Distortion", {}, ""}, {"D2", "DistCenter", "Distortion Center", {}, ""}, {"D", "Scale", "Scale", {}, ""},
            {"D", "RadialDistParam1", "Radial Distortion k1", {}, ""}, {"D", "RadialDistParam2", "Radial Distortion k2", {}, ""},
            {"D", "RadialDistParam3", "Radial Distortion k3", {}, ""}};
}
static std::vector<Want> wantTimeWarp()
{
    return {{"G", "Group Parameter", "Parameter", {}, ""}, {"C", "Choice Box Algorithm", "Method", {"Morph", "Optical Flow"}, "Group Parameter"},
            {"C", "Choice Box Quality", "Quality", {"Coarse", "Medium", "Fine"}, "Group Parameter"},
            {"D", "Slow Motion Ratio", "Playback Rate", {}, "Group Parameter"}, {"P", "Analyse Button", "Analyze Motion", {}, "Group Parameter"},
            {"P", "Clear Button", "Clear Motion Data", {}, "Group Parameter"}};
}
static std::vector<Want> wantZoom()
{
    return {{"D3", "LocationAndZoom", "Cutout", {}, ""}, {"D", "Noise", "Noise sensitivity", {}, ""}, {"B", "EnableEnhancement", "Enable enhancement", {}, ""}};
}

static const char* kindOf(const PDef& p)
{
    switch (p.type) {
    case PT::Group: return "G";
    case PT::Double: return "D";
    case PT::Double2D: return "D2";
    case PT::Double3D: return "D3";
    case PT::Int: return "I";
    case PT::Bool: return "B";
    case PT::Choice: return "C";
    case PT::String: return "S";
    case PT::Custom: return "X";
    case PT::Button: return "P";
    default: return "?";
    }
}

static void vegasParamsTests()
{
    using namespace reno::ofx;
    struct One { std::string id, label; std::vector<Want> want; };
    const std::vector<One> all = {{kBump, "VEGAS Bump Map", wantBump()}, {kBz, u8"VEGAS B\u00e9zier Masking", wantBz()},
                                  {kStereo, "VEGAS Stereoscopic 3D Adjust", wantStereo()}, {kLens, "VEGAS Lens Correction", wantLens()},
                                  {kZoom, "VEGAS Smart Zoom", wantZoom()}, {kFlow, "VEGAS Warp Flow", {}},
                                  {kTimeWarp, "VEGAS Slow Motion", wantTimeWarp()}};
    for (const One& o : all) {
        const Def* d = findDef(o.id);
        check(d != nullptr, o.id + ": exists");
        if (!d) continue;
        check(d->label == o.label, o.id + ": labelled as VEGAS labels it", d->label);
        bool same = d->params.size() == o.want.size();
        std::string why = strfmt("%zu parameters, VEGAS has %zu", d->params.size(), o.want.size());
        for (size_t i = 0; same && i < o.want.size(); ++i) {
            const PDef& p = d->params[i];
            const Want& w = o.want[i];
            if (p.name != w.name) { same = false; why = "parameter " + std::to_string(i) + " is " + p.name + ", VEGAS has " + w.name; }
            else if (std::string(kindOf(p)) != w.kind) { same = false; why = p.name + " is a " + kindOf(p) + ", VEGAS has a " + w.kind; }
            else if (p.label != w.label) { same = false; why = p.name + " is labelled '" + p.label + "', VEGAS has '" + w.label + "'"; }
            else if (p.choices != w.choices) { same = false; why = p.name + ": other choices"; }
            else if (p.parent != w.parent) { same = false; why = p.name + " is in group '" + p.parent + "', expected '" + w.parent + "'"; }
        }
        check(same, o.id + ": parameters, types, labels, choices and groups are VEGAS's", same ? "" : why);
    }
    // defaults VEGAS's presets give (Vfx1's Bump Map and Bezier Masking presets)
    auto defOf = [&](const std::string& id, const std::string& name) -> std::vector<double> {
        const Def* d = findDef(id);
        if (d) for (const PDef& p : d->params) if (p.name == name) return p.def;
        return {};
    };
    // (Bump Map's own defaults, from its describe code in Vfx1: a directional light from the upper left; its resource file lists
    // the 'Spotlight Up (Flat)' preset's values instead)
    check(defOf(kBump, "DualLocation3DAnd2DSrc") == std::vector<double>({-1, 1, 3}) && defOf(kBump, "Type") == std::vector<double>({0}) &&
          defOf(kBump, "Intensity") == std::vector<double>({0.25}) && defOf(kBump, "Focus") == std::vector<double>({0.5}) && defOf(kBump, "Ambience") == std::vector<double>({0.1}) &&
          defOf(kBump, "Shininess") == std::vector<double>({0.5}) && defOf(kBump, "BumpHeight") == std::vector<double>({0.5}) && defOf(kBump, "BumpChannel") == std::vector<double>({1}) &&
          defOf(kBump, "WhiteIsHigh") == std::vector<double>({1}) && defOf(kBump, "FlipInputSurfaces") == std::vector<double>({0}), "Bump Map: the defaults are VEGAS's plug-in's own");
    check(defOf(kBz, "Invert") == std::vector<double>({0}) && defOf(kBz, "Blend") == std::vector<double>({0}) && defOf(kBz, "Enable_0") == std::vector<double>({1}) &&
          defOf(kBz, "Type_0") == std::vector<double>({1}) && defOf(kBz, "Width_0") == std::vector<double>({0.5}) && defOf(kBz, "Height_0") == std::vector<double>({0.5}) &&
          defOf(kBz, "Opacity_0") == std::vector<double>({1}) && defOf(kBz, "Feather_0") == std::vector<double>({0}) && defOf(kBz, "FeatherStyle_0") == std::vector<double>({2}) &&
          defOf(kBz, "Location_0") == std::vector<double>({0.5, 0.5}) && defOf(kBz, "AnchorScale_0") == std::vector<double>({1, 1}) && defOf(kBz, "KFIntervalTrk_0") == std::vector<double>({5}),
          "Bezier Masking: mask 1 is VEGAS's default, a rectangle of half the frame each way");
    check(defOf(kBz, "Enable_1") == std::vector<double>({0}) && defOf(kBz, "Type_1") == std::vector<double>({1}) && defOf(kBz, "Width_1") == std::vector<double>({0.25}) &&
          defOf(kBz, "Height_1") == std::vector<double>({0.444}) && defOf(kBz, "Enable_4") == std::vector<double>({0}) && defOf(kBz, "Height_4") == std::vector<double>({0.444}),
          "... and masks 2 to 5 are off squares of 0.25 x 0.444");
    // and the host sees the same: a plug-in's parameter set as the host builds it
    Host& host = Host::get();
    std::string err;
    Binary* bin = host.loadBinary(kBuiltinBinaryPath, &err);
    if (bin) {
        for (const std::string& id : {kBump, kBz, kStereo, kLens, kZoom, kTimeWarp}) {
            Plugin* p = host.findPlugin(id);
            if (!p) continue;
            p->load();
            InstanceSetup st;
            st.width = 64;
            st.height = 36;
            std::string e;
            auto inst = p->createInstance(p->filterContext(), st, &e);
            check(inst != nullptr, id + ": the host makes an instance", e);
            if (!inst) continue;
            const Def* d = findDef(id);
            size_t have = 0;
            bool kinds = true;
            for (const PDef& pd : d->params) {
                Param* hp = inst->params.find(pd.name);
                if (!hp) { kinds = false; continue; }
                ++have;
                const ParamKind want = pd.type == PT::Double3D ? ParamKind::Double3D : pd.type == PT::Button ? ParamKind::PushButton : pd.type == PT::Double2D ? ParamKind::Double2D :
                                       pd.type == PT::Group ? ParamKind::Group : pd.type == PT::Custom ? ParamKind::Custom : pd.type == PT::String ? ParamKind::String :
                                       pd.type == PT::Int ? ParamKind::Int : pd.type == PT::Bool ? ParamKind::Bool : pd.type == PT::Choice ? ParamKind::Choice : ParamKind::Double;
                kinds = kinds && hp->kind == want;
                if (pd.type == PT::Double3D) kinds = kinds && hp->value.size() == 3 && hp->value[0] == pd.def[0] && hp->value[2] == pd.def[2];
            }
            check(have == d->params.size() && kinds, id + ": the host has every parameter, of VEGAS's type, with the defaults");
        }
    }
}

// ------------------------------------------------------------------ 1. registration
static void registrationTests()
{
    for (const std::string& id : {kBz, kBump, kStereo, kLens, kFlow, kTimeWarp, kZoom}) {
        const Def* d = findDef(id);
        check(d && d->render, "built-in " + id + " exists");
        if (!d) continue;
        check(d->multiRes, id + " says it supports multi-resolution");
        std::set<std::string> names;
        bool dup = false;
        for (const PDef& p : d->params) dup = dup || !names.insert(p.name).second;
        check(!dup, id + ": no parameter name is used twice");
    }
    const Def* flow = findDef(kFlow);
    check(flow && flow->transition, "Warp Flow is a transition");
    check(findDef(kDual) != nullptr, "Dual Fish Eye Stitching is a built-in with VEGAS's identifier (com.magix.ofx.vr.dual.stich)");
    // the other way round: the catalogue lists them, and nothing uses more than the room there is for built-ins
    const auto infos = ofx::builtinInfos();
    int found = 0;
    for (const auto& i : infos)
        if (i.id == kBz || i.id == kBump || i.id == kStereo || i.id == kLens || i.id == kFlow || i.id == kTimeWarp || i.id == kZoom) ++found;
    check(found == 7, "the catalogue lists all seven", strfmt("%d", found));
    check(infos.size() <= 256, "the built-ins fit the 256 the host has entry points for", strfmt("%zu", infos.size()));
    // the order of the hidden parameters etc. is the effect's own; the presets package must still parse after the additions
    check(!ofx::builtinPresetPackage().empty(), "the preset package is there");
}

static void defaultsTests()
{
    const Buf src = picture(160, 90, 0);
    for (const std::string& id : {kStereo, kLens, kZoom, kTimeWarp, kBz}) {
        const Def* d = findDef(id);
        if (!d) continue;
        Params over;
        StrMap sov;
        if (id == kZoom) over["LocationAndZoom"] = {0.5, 0.5, 1.0};
        if (id == kBz) over["Enable_0"] = {0};
        const Buf o = runS(*d, over, sov, src);
        if (id == kBz) {            // VEGAS: no mask enabled draws a transparent picture
            double m = 0;
            for (float v : o.px) m = std::max(m, (double)std::fabs(v));
            check(m == 0, id + " with no mask enabled is transparent (as VEGAS draws it)", f3(m));
        } else if (id == kStereo) { // VEGAS's automatic crop is on by default: the picture is scaled by 0.9962 about the middle
        {
            double sum = 0;
            for (size_t i = 0; i < o.px.size(); ++i) sum += std::fabs(o.px[i] - src.px[i]);
            check(sum / o.px.size() < 0.01, id + " at its defaults only takes the automatic crop's 0.9962", f3(sum / o.px.size()));
        }
        } else
            check(maxDiff(o, src) < 1e-4, id + " leaves the picture as it is when it has nothing to do", f3(maxDiff(o, src)));
    }
    // Smart Zoom at its default zoom does something
    const Def* z = findDef(kZoom);
    if (z) check(maxDiff(runS(*z, {}, {}, src), src) > 0.01, "Smart Zoom at its default zoom changes the picture");
    // Bezier Masking's default is mask 1, an oval in the middle: it keeps the middle and clears the corners
    const Def* bz = findDef(kBz);
    if (bz) {
        const Buf o = runS(*bz, {}, {}, src);
        check(o.at(80, 45)[3] > 0.99f * src.at(80, 45)[3] && o.at(1, 1)[3] < 1e-4f, "Bezier Masking's default mask is a rectangle of half the frame in the middle");
    }
}

// ------------------------------------------------------------------ 2a. Stereoscopic 3D Adjust
static void stereoTests()
{
    // Geometry measured from VEGAS's pictures (see stereo3dAdjust): with X, Y the pixel's index from the middle of the frame,
    // the pixel shows the source at ((s (X cos t - Y sin t) + tx) / den + (W-1)/2, (s (X sin t + Y cos t) + ty) / den + (H-1)/2),
    // t = -Rotation / 2, s = 1 + Zoom (cut down by the automatic crop), tx = (Horizontal + Keystone) W / 2, ty = 69.35 Vertical H / 180,
    // den = 1 + Keystone X / (W / 2). On the ramps the red channel is the column / (W - 1) and the blue one the row / (H - 1).
    const Def* d = findDef(kStereo);
    if (!d) return;
    const int W = 200, H = 100;
    const Buf src = ramps(W, H);
    const double hw = (W - 1) * 0.5, hh = (H - 1) * 0.5;
    {
        const Buf o = runS(*d, {{"Horizontal", {0.1}}, {"AutoCrop", {0}}}, {}, src);
        // Horizontal 0.1 = 0.1 of half the width: pixel x shows the source at x + 10
        float v = o.at(80, 50)[0], want = 90.f / (W - 1);
        check(std::fabs(v - want) < 0.002, "Horizontal offset 0.1 shows the source 10 pixels (a tenth of half the width) to the right", strfmt("%.4f vs %.4f", v, want));
        check(o.at(W - 3, 50)[3] < 0.01f && o.at(W - 3, 50)[0] == 0.f, "... and leaves the strip it uncovers empty, transparent (automatic crop off)", f3(o.at(W - 3, 50)[3]));
    }
    {
        const Buf o = runS(*d, {{"Vertical", {0.1}}, {"AutoCrop", {0}}}, {}, src);
        // Vertical 0.1 = half of it in frame heights: row y shows the source row y + 5 here
        float v = o.at(50, 30)[2], want = (float)((30 + 0.05 * H) / (H - 1));
        check(std::fabs(v - want) < 0.002, "Vertical offset 0.1 shows the source half of a tenth of the height (5 rows here) lower", strfmt("%.4f vs %.4f", v, want));
    }
    {
        const Buf o = runS(*d, {{"FlipHorizontal", {1}}, {"AutoCrop", {0}}}, {}, src);
        check(std::fabs(o.at(10, 50)[0] - src.at(W - 1 - 10, 50)[0]) < 0.01, "Flip horizontal (Left) mirrors the picture");
        const Buf r = runS(*d, {{"FlipHorizontal", {2}}, {"FlipVertical", {2}}, {"AutoCrop", {0}}}, {}, src);
        check(std::fabs(r.at(10, 50)[0] - src.at(10, 50)[0]) < 0.01 && std::fabs(r.at(50, 10)[2] - src.at(50, 10)[2]) < 0.01, "Flip Right does nothing to the left view (the one picture)");
        const Buf p = runS(*d, {{"FlipVertical", {3}}, {"AutoCrop", {0}}}, {}, src);
        check(std::fabs(p.at(50, 10)[2] - src.at(50, H - 1 - 10)[2]) < 0.01, "Flip vertical (Both) turns it upside down");
    }
    {
        const Buf o = runS(*d, {{"Zoom", {0.1}}, {"AutoCrop", {0}}}, {}, src);
        // the left view's scale is 1 - Zoom / 2 = 0.95: the pixel shows the source at 1 / 0.95 times its distance from the middle
        check(std::fabs(o.at(150, 50)[0] - (float)((hw + (150 - hw) / 0.95) / (W - 1))) < 0.003 && std::fabs(o.at(100, 50)[0] - (float)((hw + (100 - hw) / 0.95) / (W - 1))) < 0.003, "Zoom 0.1 (the left view's scale 0.95) samples the source at 1 / 0.95 times the distance from the middle");
    }
    {
        const double t = -15.0 * 0.5 * kPi / 180.0;
        const Buf o = runS(*d, {{"Rotation", {15.0}}, {"AutoCrop", {0}}}, {}, src);
        const double X = 140 - hw, Y = 40 - hh, u = std::cos(t) * X - std::sin(t) * Y + hw, v = std::sin(t) * X + std::cos(t) * Y + hh;
        check(std::fabs(o.at(140, 40)[0] - (float)(u / (W - 1))) < 0.003 && std::fabs(o.at(140, 40)[2] - (float)(v / (H - 1))) < 0.003, "Rotation 15 turns the picture by half of it (-7.5 degrees) about the middle");
        const Buf r = runS(*d, {{"Rotation", {10.0}}, {"AutoCrop", {1}}}, {}, src);
        const Buf n = runS(*d, {{"Rotation", {10.0}}, {"AutoCrop", {0}}}, {}, src);
        // the corner (0, 0) of the unturned picture is black on the ramps (0, .5, 0): the cropped one shows picture there, the other the black fill
        check(r.at(2, 2)[1] > 0.49f && n.at(0, 0)[1] < 0.01f, "Automatic crop leaves no empty border after a rotation; without it the border is black", strfmt("%.3f %.3f", r.at(2, 2)[1], n.at(0, 0)[1]));
        const Buf r2 = runS(*d, {{"Rotation", {10.0}}, {"AutoCrop", {1}}}, {}, src);
        check(maxDiff(r, r2) == 0, "the crop is deterministic");
        const Buf k = runS(*d, {{"Keystone", {0.1}}, {"AutoCrop", {1}}}, {}, src);
        check(k.at(1, 1)[1] > 0.49f && k.at(W - 2, H - 2)[1] > 0.49f, "Automatic crop after a keystone too");
    }
    {
        const Buf o = runS(*d, {{"Crop", {0.25}}, {"AutoCrop", {0}}}, {}, src);
        // a scale by 1 + Crop = 1.25 about the middle
        check(std::fabs(o.at(150, 50)[0] - (float)((hw + (150 - hw) / 1.25) / (W - 1))) < 0.003, "Crop 0.25 (automatic crop off) enlarges the picture by 1.25 about the middle");
        const Buf f = runS(*d, {{"FloatingWindowLeft", {0.1}}, {"FloatingWindowRight", {0.05}}}, {}, src);
        check(f.at(10, 50)[0] == 0.f && f.at(10, 50)[3] == 1.f && f.at(W - 5, 50)[3] == 1.f && f.at(W - 5, 50)[0] > 0.8f && f.at(100, 50)[0] > 0.3f,
              "The left floating window is a black bar at the left side (the right one belongs to the other view)");
    }
    {
        // keystone is horizontal: the middle row is not scaled (den = 1 at X = 0 only for the middle column; the shift tx moves it)
        const Buf o = runS(*d, {{"Keystone", {0.1}}, {"AutoCrop", {0}}}, {}, src);
        const double X = 150 - hw, den = 1 + 0.1 * X / (W * 0.5), u = (X + 0.1 * W * 0.5) / den + hw;
        check(std::fabs(o.at(150, 50)[0] - (float)(u / (W - 1))) < 0.003, "Keystone divides by 1 + Keystone X / (W / 2) and shifts by Keystone W / 2");
    }
}

// ------------------------------------------------------------------ 2b. Lens Correction
static Buf radiusPicture(int W, int H)
{
    // the red channel is the distance from the centre in units of 1 / (W/2) (so 1 at the left and right edges of the middle row) / 2
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double r = std::hypot(x + 0.5 - W * 0.5, y + 0.5 - H * 0.5) / (W * 0.5);
            premul(b.at(x, y), (float)(r * 0.5), 0.5f, 0.5f, 1.f);
        }
    return b;
}

static void lensTests()
{
    const Def* d = findDef(kLens);
    if (!d) return;
    const int W = 240, H = 120;
    const Buf src = radiusPicture(W, H);
    const double half = 0.5 * std::min(W, H);
    {
        // the PTLens model: the pixel at the radius r (in half the shorter side) takes the source at r (a r^3 + b r^2 + c r + d), d = 1 - a - b - c
        const double a = 0.1, b = -0.05, c = 0.03;
        const Buf o = runS(*d, {{"RadialDistParam1", {a}}, {"RadialDistParam2", {b}}, {"RadialDistParam3", {c}}}, {}, src);
        // (VEGAS's remap: the centre is the pixel (W/2, H/2) in memory rows, the map holds the places / (W, H) and reads them back
        // * (W - 1, H - 1), a place being a pixel index)
        for (int px : {30, 80, 200}) {
            const int py = H / 2, m = H - 1 - py;
            const double dx = px - W * 0.5, dy = m - H * 0.5, r = std::hypot(dx, dy) / half;
            const double f = a * r * r * r + b * r * r + c * r + (1 - a - b - c);
            const double sx = (W - 1) * (f * dx + W * 0.5) / W, sy = H - 1 - (H - 1) * (f * dy + H * 0.5) / H;
            const double want = std::hypot(sx + 0.5 - W * 0.5, sy + 0.5 - H * 0.5) / (W * 0.5) * 0.5;
            check(std::fabs(o.at(px, py)[0] - want) < 0.008, strfmt("k1, k2, k3: pixel %d takes the source at r (a r^3 + b r^2 + c r + d)", px), strfmt("%.4f vs %.4f", o.at(px, py)[0], want));
        }
        check(std::fabs(o.at(W / 2, H / 2)[0] - src.at(W / 2, H / 2)[0]) < 0.01, "... and the centre stays");
        // the edge of the shorter side stays where it is (d = 1 - a - b - c)
        check(std::fabs(o.at(W / 2, 0)[0] - src.at(W / 2, 0)[0]) < 0.02, "... and so does the middle of the top edge (r = 1)", strfmt("%.4f vs %.4f", o.at(W / 2, 0)[0], src.at(W / 2, 0)[0]));
        const Buf n = runS(*d, {{"RadialDistParam1", {-0.1}}}, {}, src);
        const Buf p = runS(*d, {{"RadialDistParam1", {0.1}}}, {}, src);
        check(n.at(30, H / 2)[0] < p.at(30, H / 2)[0] - 0.005, "a negative k1 works the other way");
    }
    {
        const Buf o = runS(*d, {{"Scale", {2.0}}}, {}, src);
        check(std::fabs(o.at(30, H / 2)[0] - src.at(W / 2 + (30 - W / 2) / 2, H / 2)[0]) < 0.01, "Scale 2 shows half the width");
        // the extra scale that hides the empty corners of a correction that reaches outside the picture
        const Buf p = runS(*d, {{"RadialDistParam1", {0.02}}}, {}, src);
        const Buf q = runS(*d, {{"RadialDistParam1", {0.02}}, {"Scale", {1.3}}}, {}, src);
        check(p.at(0, 0)[3] < 1.0f && q.at(0, 0)[3] > 0.99f, "Scale hides the empty corners the correction leaves", strfmt("%.3f -> %.3f", p.at(0, 0)[3], q.at(0, 0)[3]));
    }
    {
        // Show Distortion draws the lens's grid over the picture; off, nothing is drawn
        const Buf flat = ramps(W, H);
        const Buf off = runS(*d, {{"RadialDistParam1", {0.05}}}, {}, flat);
        const Buf on = runS(*d, {{"RadialDistParam1", {0.05}}, {"ShowMesh", {1}}}, {}, flat);
        int green = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) green += on.at(x, y)[1] > 0.85f && on.at(x, y)[0] < 0.01f;
        // VEGAS's Show Distortion draws nothing in its picture (the frame comes back as it was): so does Reno
        check(green == 0 && maxDiff(flat, on) == 0 && maxDiff(off, on) > 0.01, "Show Distortion leaves the frame as it was (VEGAS draws no grid)", strfmt("%d of %d", green, W * H));
        const Buf none = runS(*d, {{"ShowMesh", {1}}}, {}, flat);
        check(maxDiff(none, flat) == 0, "... also with nothing to correct");
    }
    {
        // a centre away from the middle: the point that stays put is the centre
        const Buf o = runS(*d, {{"RadialDistParam1", {0.1}}, {"DistCenter", {0.25, 0.5}}}, {}, ramps(W, H));
        check(std::fabs(o.at(W / 4, H / 2)[0] - ramps(W, H).at(W / 4, H / 2)[0]) < 0.01, "DistCenter moves the point the correction works about");
        // a lens that is off the picture's middle is not corrected the same on both sides
        const Buf q = runS(*d, {{"RadialDistParam1", {0.1}}}, {}, src);
        const Buf r = runS(*d, {{"RadialDistParam1", {0.1}}, {"DistCenter", {0.25, 0.5}}}, {}, src);
        check(maxDiff(q, r) > 0.05, "... and the picture differs from the centred one");
    }
    {
        // Localization, Query and QueryResult only hold their value (no lens database in Reno): the picture does not change with them
        const Buf a = runS(*d, {}, {}, src), b = runS(*d, {{"Localization", {1}}, {"QueryResult", {0}}}, {{"Query", "Canon"}}, src);
        check(maxDiff(a, b) == 0, "the lens search settings do not change the picture");
    }
}

// ------------------------------------------------------------------ 2c. Smart Zoom
static void zoomTests()
{
    const Def* d = findDef(kZoom);
    if (!d) return;
    const int W = 200, H = 100;
    const Buf src = ramps(W, H);
    {
        const Buf o = runS(*d, {{"LocationAndZoom", {0.5, 0.5, 2.0}}, {"EnableEnhancement", {0}}}, {}, src);
        check(std::fabs(o.at(100, 50)[0] - src.at(100, 50)[0]) < 0.01 && std::fabs(o.at(150, 50)[0] - src.at(125, 50)[0]) < 0.01 && std::fabs(o.at(0, 50)[0] - src.at(50, 50)[0]) < 0.01,
              "Zoom 2 at the middle shows the middle half of the picture");
    }
    {
        // the centre in the lower left corner: the region is kept inside the picture, so it shows the bottom-left quarter
        const Buf o = runS(*d, {{"LocationAndZoom", {0.0, 0.0, 2.0}}, {"EnableEnhancement", {0}}}, {}, src);
        check(std::fabs(o.at(0, H - 1)[0] - src.at(0, H - 1)[2 - 2]) < 0.01 && std::fabs(o.at(W - 1, 0)[0] - src.at(W / 2 - 1, H / 2)[0]) < 0.02 && o.at(0, H - 1)[3] > 0.999f,
              "Center at a corner keeps the part shown inside the picture (no empty border)");
        // a centre off the middle but inside
        const Buf p = runS(*d, {{"LocationAndZoom", {0.75, 0.5, 4.0}}, {"EnableEnhancement", {0}}}, {}, src);
        check(std::fabs(p.at(W / 2, H / 2)[0] - src.at(150, H / 2)[0]) < 0.01, "Center puts that point of the picture in the middle");
    }
    {
        // sharpening an enlarged edge raises the contrast over it
        Buf edge;
        edge.resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) premul(edge.at(x, y), x < W / 2 ? 0.2f : 0.8f, 0.5f, 0.5f, 1.f);
        const Buf plain = runS(*d, {{"LocationAndZoom", {0.5, 0.5, 3.0}}, {"EnableEnhancement", {0}}}, {}, edge);
        const Buf sharp = runS(*d, {{"LocationAndZoom", {0.5, 0.5, 3.0}}, {"EnableEnhancement", {1}}, {"Noise", {0.0}}}, {}, edge);
        const Buf noisy = runS(*d, {{"LocationAndZoom", {0.5, 0.5, 3.0}}, {"EnableEnhancement", {1}}, {"Noise", {1.0}}}, {}, edge);
        check(maxDiff(noisy, plain) < 1e-5, "Noise sensitivity 1 leaves nothing to enhance (the enhancement would sharpen the noise)");
        float lo0 = 1, hi0 = 0, lo1 = 1, hi1 = 0;
        for (int x = 0; x < W; ++x) {
            lo0 = std::min(lo0, plain.at(x, 50)[0]); hi0 = std::max(hi0, plain.at(x, 50)[0]);
            lo1 = std::min(lo1, sharp.at(x, 50)[0]); hi1 = std::max(hi1, sharp.at(x, 50)[0]);
        }
        check(hi1 - lo1 > hi0 - lo0 + 0.01f, "Sharpen adds contrast at an enlarged edge", strfmt("range %.3f -> %.3f", hi0 - lo0, hi1 - lo1));
        bool inside = true;
        for (size_t i = 0; i < sharp.px.size(); i += 4) inside = inside && sharp.px[i] <= sharp.px[i + 3] + 1e-6f && sharp.px[i] >= 0.f;
        check(inside, "... and never leaves what a premultiplied pixel can be");
    }
    {
        const Buf alpha = picture(160, 90, 0);
        const Buf o = runS(*d, {{"LocationAndZoom", {0.5, 0.5, 1.5}}}, {}, alpha);
        bool fine = true;
        for (float v : o.px) fine = fine && std::isfinite(v) && v >= 0.f && v <= 1.0001f;
        check(fine, "a picture with soft alpha zooms to valid values");
    }
}

// ------------------------------------------------------------------ 2d. Bezier Masking
static double sum(const Buf& b, int ch)
{
    double s = 0;
    for (size_t i = (size_t)ch; i < b.px.size(); i += 4) s += b.px[i];
    return s;
}

static void bezierTests()
{
    const Def* d = findDef(kBz);
    if (!d) return;
    const int W = 200, H = 100;
    Buf white;
    white.resize(W, H);
    for (size_t i = 0; i < white.px.size(); ++i) white.px[i] = 1.f;
    // a Curve mask 1 with this outline, other settings as given
    auto one = [&](const std::string& path, Params p = Params(), const Buf& src = Buf()) {
        StrMap s;
        curve(p, s, 0, path);
        return runS(*d, p, s, src.px.empty() ? white : src);
    };
    // parse / format
    {
        BezPath p;
        const std::string t = rectPath(0.25, 0.25, 0.75, 0.75);
        check(parseBezPath(t, p) && p.pts.size() == 4 && p.closed && formatBezPath(p) == t, "a mask outline's text round-trips");
        check(!parseBezPath("", p) && p.pts.empty(), "empty text is no outline");
        check(!parseBezPath("garbage", p) && !parseBezPath("bz1 C;1,2,3", p) && !parseBezPath("bz1 C;nan,0,0,0,0,0", p), "bad text is no outline");
        unsigned seed = 7;
        bool survived = true;
        for (int i = 0; i < 2000; ++i) {
            std::string s = i % 2 ? "bz1 C" : "bz1 O";
            const int n = (int)(seed = seed * 1103515245u + 12345u) % 80;
            for (int k = 0; k < n; ++k) {
                seed = seed * 1103515245u + 12345u;
                static const char alphabet[] = "0123456789.,;-e+ nafi";
                s += alphabet[(seed >> 16) % (sizeof(alphabet) - 1)];
            }
            parseBezPath(s, p);
            Buf o = one(s);
            survived = survived && o.px.size() == white.px.size();
        }
        check(survived, "2000 random texts as the curve: no crash, a picture comes out");
    }
    // a curve mask with no outline yet covers nothing, and no mask on: nothing is shown (VEGAS draws a transparent picture for both)
    {
        auto blank = [&](const Buf& b) { for (float v : b.px) if (v != 0.f) return false; return true; };
        check(blank(one("")), "a Curve mask with no outline drawn covers nothing: a transparent picture");
        check(blank(runS(*d, {{"Enable_0", {0}}}, {}, white)), "no mask on: a transparent picture (as VEGAS draws it)");
    }
    // a rectangle: the area is exact, and the edge pixel half covered
    {
        // x from 50.5 to 150 (0.2525 .. 0.75 of 200), y from 25 to 75 (rows 25..74)
        const Buf o = one(rectPath(50.5 / W, 0.25, 0.75, 0.75));
        check(std::fabs(sum(o, 3) - (99.5 * 50)) < 0.3, "a curve rectangle's mask covers its area (to whole levels of the 8-bit mask)", strfmt("%.3f vs %.3f", sum(o, 3), 99.5 * 50));
        check(std::fabs(o.at(50, 50)[3] - 0.5) < 3e-3 && o.at(51, 50)[3] > 0.9999f && o.at(49, 50)[3] < 1e-4 && o.at(100, 24)[3] < 1e-4 && o.at(100, 25)[3] > 0.9999f, "... with a half-covered pixel at a half-pixel edge");
        check(o.at(100, 50)[0] == 1.f && o.at(10, 10)[0] == 0.f, "inside the picture is kept, outside is transparent");
    }
    // the shapes: size, place, angle
    {
        const Buf r = runS(*d, {{"Type_0", {1}}, {"Width_0", {0.5}}, {"Height_0", {0.5}}}, {}, white);
        check(std::fabs(sum(r, 3) - 100.0 * 50.0) < 0.01 && r.at(100, 50)[3] > 0.9999f && r.at(40, 50)[3] < 1e-4f, "a Rectangle of Width 0.5 and Height 0.5 is half the frame each way", strfmt("%.2f", sum(r, 3)));
        const Buf o = runS(*d, {{"Type_0", {0}}, {"Width_0", {0.6}}, {"Height_0", {0.6}}}, {}, white);
        const double want = kPi * (0.3 * W) * (0.3 * H);
        check(std::fabs(sum(o, 3) - want) / want < 0.005, "an Oval has the area of an ellipse (0.5%)", strfmt("%.1f vs %.1f", sum(o, 3), want));
        check(o.at(100, 50)[3] > 0.999f && o.at(100, 3)[3] < 1e-4f && o.at(10, 50)[3] < 1e-4f, "... centred on the frame");
        const Buf dm = runS(*d, {{"Type_0", {2}}, {"Width_0", {0.5}}, {"Height_0", {0.5}}}, {}, white);
        check(std::fabs(sum(dm, 3) - 100.0 * 50.0 / 2) / (100.0 * 50.0 / 2) < 0.002 && dm.at(100, 50)[3] > 0.9999f && dm.at(55, 30)[3] < 1e-4f, "a Diamond is half of its box", strfmt("%.2f", sum(dm, 3)));
        // Location puts the middle of the shape there (y up): the right quarter of the frame, a quarter down
        const Buf loc = runS(*d, {{"Type_0", {1}}, {"Width_0", {0.2}}, {"Height_0", {0.2}}, {"Location_0", {0.75, 0.25}}}, {}, white);
        check(loc.at(150, 75)[3] > 0.9999f && loc.at(100, 50)[3] < 1e-4f && loc.at(150, 25)[3] < 1e-4f && std::fabs(sum(loc, 3) - 40.0 * 20.0) < 0.01, "Location moves the shape (as fractions of the frame, y up)", strfmt("%.2f", sum(loc, 3)));
        // Angle turns it about its middle, in pixels, anticlockwise: a wide bar of 100 x 20 pixels stands up
        const Buf bar = runS(*d, {{"Type_0", {1}}, {"Width_0", {0.5}}, {"Height_0", {0.2}}}, {}, white);
        const Buf up = runS(*d, {{"Type_0", {1}}, {"Width_0", {0.5}}, {"Height_0", {0.2}}, {"Angle_0", {90}}}, {}, white);
        check(bar.at(100 + 40, 50)[3] > 0.9999f && bar.at(100, 50 - 30)[3] < 1e-4f && up.at(100, 50 - 40)[3] > 0.999f && up.at(100 + 40, 50)[3] < 1e-4f && std::fabs(sum(up, 3) - sum(bar, 3)) < 0.01,
              "Angle 90 stands a wide bar up (same area)");
        // AnchorScale scales a curve about the frame's middle; Location moves it
        Params ps = {{"AnchorScale_0", {0.5, 0.5}}};
        const Buf small = one(rectPath(0.0, 0.0, 1.0, 1.0), ps);
        check(std::fabs(sum(small, 3) - 100.0 * 50.0) < 0.01 && small.at(100, 50)[3] > 0.9999f && small.at(40, 50)[3] < 1e-4f, "a curve's Anchor Scale 0.5 halves it about the middle", strfmt("%.2f", sum(small, 3)));
        Params pl = {{"Location_0", {0.25, 0.5}}};
        const Buf moved = one(rectPath(0.4, 0.4, 0.6, 0.6), pl);
        check(moved.at(50, 50)[3] > 0.9999f && moved.at(100, 50)[3] < 1e-4f, "... and Location moves it");
    }
    // masks together: the union, and a mask that is off
    {
        const std::string a = rectPath(0.1, 0.1, 0.6, 0.9), b = rectPath(0.4, 0.2, 0.9, 0.8);
        Params p;
        StrMap s;
        curve(p, s, 0, a);
        const Buf oa = runS(*d, p, s, white);
        curve(p, s, 1, b);
        const Buf both = runS(*d, p, s, white);
        Params pb;
        StrMap sb;
        curve(pb, sb, 0, b);
        const Buf ob = runS(*d, pb, sb, white);
        const double A = sum(oa, 3), B = sum(ob, 3), U = sum(both, 3);
        check(std::fabs(A - 8000) < 0.01 && std::fabs(B - 6000) < 0.01 && std::fabs(U - 11600) < 0.01, "two masks together cover the union of their areas (8000 + 6000 - 2400)", strfmt("%.2f %.2f %.2f", A, B, U));
        p["Enable_1"] = {0};
        check(maxDiff(runS(*d, p, s, white), oa) == 0, "a mask that is not enabled is ignored");
        // a Curve and an Oval
        p["Enable_1"] = {1};
        p["Type_1"] = {0};
        p["Width_1"] = {0.2};
        p["Height_1"] = {0.4};
        p["Location_1"] = {0.9, 0.5};
        const Buf mixed = runS(*d, p, s, white);
        check(mixed.at(180, 50)[3] > 0.99f && mixed.at(100, 50)[3] > 0.99f && mixed.at(190, 5)[3] < 1e-4f, "masks of different types add up");
    }
    // opacity, invert, blend, feather
    {
        const std::string a = rectPath(0.25, 0.25, 0.75, 0.75);
        const Buf half = one(a, {{"Opacity_0", {0.5}}});
        check(std::fabs(half.at(100, 50)[3] - 0.5) < 3e-3 && std::fabs(half.at(100, 50)[0] - 0.5) < 3e-3, "Opacity 0.5 makes the inside half transparent");
        const Buf inv = one(a, {{"Invert", {1}}});
        check(inv.at(100, 50)[3] < 1e-5 && inv.at(5, 5)[3] > 0.9999f, "Invert keeps the outside");
        const Buf blend = one(a, {{"Blend", {0.4}}});
        check(std::fabs(blend.at(5, 5)[3] - 0.4) < 1e-5 && blend.at(100, 50)[3] > 0.9999f, "Blend lets that much of the picture through outside the masks");
        check(maxDiff(one(a, {{"Blend", {1.0}}}), white) < 1e-5, "... all of it at 1");
        const Buf feathered = one(a, {{"Feather_0", {13.3333}}});       // (sigma 4 pixels: 0.04 of the frame's height; VEGAS's Feather is 0..50)
        const double edge = feathered.at(50, 50)[3];
        check(edge > 0.4 && edge < 0.7 && feathered.at(47, 50)[3] > 0.1 && feathered.at(47, 50)[3] < 0.4 && feathered.at(100, 50)[3] > 0.97, "Feather blurs the edge across a few pixels", strfmt("%.3f %.3f", edge, feathered.at(47, 50)[3]));
        check(std::fabs(sum(feathered, 3) - sum(one(a), 3)) / sum(feathered, 3) < 0.01, "... without changing the area");
        const Buf fin = one(a, {{"Feather_0", {13.3333}}, {"FeatherStyle_0", {0}}});
        const Buf fout = one(a, {{"Feather_0", {13.3333}}, {"FeatherStyle_0", {1}}});
        check(fin.at(47, 50)[3] < 1e-4f && fin.at(53, 50)[3] < 0.95f && fin.at(53, 50)[3] > 0.5f, "Feather Style In keeps the edge and softens inwards", strfmt("%.3f %.3f", fin.at(47, 50)[3], fin.at(53, 50)[3]));
        check(fout.at(53, 50)[3] > 0.9999f && fout.at(47, 50)[3] > 0.1f && fout.at(40, 50)[3] < fout.at(47, 50)[3], "Feather Style Out keeps the inside and softens outwards", strfmt("%.3f %.3f", fout.at(53, 50)[3], fout.at(47, 50)[3]));
        // a picture with alpha and colour keeps its own where the mask is white
        const Buf pic = picture(W, H, 0);
        const Buf o = one(rectPath(0, 0, 1, 1), Params(), pic);
        check(maxDiff(o, pic) < 1e-4, "a mask over the whole frame changes nothing", f3(maxDiff(o, pic)));
    }
    // a path off the frame, and a huge one
    {
        const Buf o = one(rectPath(-5, -5, 7, 7));
        check(maxDiff(o, white) < 1e-5, "an outline reaching far outside the frame covers it all");
        const Buf p = one(rectPath(-5, 2, 7, 3));
        check(sum(p, 3) == 0.0, "... and one beside it covers nothing");
    }
}

// ------------------------------------------------------------------ 2e. Warp Flow
static void flowTests()
{
    const Def* d = findDef(kFlow);
    if (!d) return;
    check(d->params.empty(), "Warp Flow has no settings (VEGAS's has none)");
    const int W = 256, H = 144;
    const Buf A = texture(W, H, 0, 0), B = texture(W, H, 6, 3);
    const Buf out0 = runS(*d, {}, {}, A, 1.0, &B, 0.0), out1 = runS(*d, {}, {}, A, 1.0, &B, 1.0);
    check(maxDiff(out0, A) == 0, "progress 0 is the outgoing picture");
    check(maxDiff(out1, B) == 0, "progress 1 is the incoming picture");
    // halfway: the picture shifted by (3, 1.5)
    const Buf ideal = texture(W, H, 3, 1.5);
    auto err = [&](const Buf& o) {
        double s = 0;
        int n = 0;
        for (int y = 20; y < H - 20; ++y)
            for (int x = 20; x < W - 20; ++x, ++n)
                for (int k = 0; k < 3; ++k) s += std::fabs(o.at(x, y)[k] - ideal.at(x, y)[k]);
        return s / (3 * n);
    };
    Buf cross;
    cross.resize(W, H);
    for (size_t i = 0; i < cross.px.size(); ++i) cross.px[i] = 0.5f * (A.px[i] + B.px[i]);
    for (int q = 0; q < 3; ++q) {
        const Buf mid = runFlow(A, B, 0.5, q);
        const double e = err(mid), ec = err(cross);
        check(e < ec * 0.5, std::string("the middle frame of a 6 x 3 pixel move is closer to the true one than a dissolve is (quality ") + (q == 0 ? "low" : q == 1 ? "medium" : "high") + ")",
              strfmt("flow %.4f, dissolve %.4f", e, ec));
    }
    // a quarter of the way
    {
        const Buf q = runS(*d, {}, {}, A, 1.0, &B, 0.25);
        const Buf idealQ = texture(W, H, 1.5, 0.75);
        double s = 0, sc = 0;
        int n = 0;
        for (int y = 20; y < H - 20; ++y)
            for (int x = 20; x < W - 20; ++x, ++n)
                for (int k = 0; k < 3; ++k) {
                    s += std::fabs(q.at(x, y)[k] - idealQ.at(x, y)[k]);
                    sc += std::fabs(0.75 * A.at(x, y)[k] + 0.25 * B.at(x, y)[k] - idealQ.at(x, y)[k]);
                }
        check(s < sc * 0.5, "a quarter of the way too", strfmt("flow %.4f, dissolve %.4f", s / n / 3, sc / n / 3));
    }
    // two pictures that have nothing in common: no crash, valid numbers, between the two
    {
        const Buf C = picture(W, H, 1);
        const Buf o = runS(*d, {}, {}, A, 1.0, &C, 0.5);
        bool fine = true;
        for (float v : o.px) fine = fine && std::isfinite(v) && v >= -1e-4f && v <= 1.0001f;
        check(fine, "two unrelated pictures morph to valid values");
    }
    // the flow field view
    {
        const Buf o = runFlow(A, B, 0.5, 1, true);
        check(o.at(100, 70)[3] == 1.f && maxDiff(o, A) > 0.1, "Show Optical Flow Field draws the flow");
    }
    // identical pictures: no motion, so the picture stays
    {
        const Buf o = runS(*d, {}, {}, A, 1.0, &A, 0.5);
        check(maxDiff(o, A) < 0.01, "no motion between the pictures: they stay as they are", f3(maxDiff(o, A)));
    }
    // other sizes: a tiny frame, a very narrow one
    {
        const Buf a = texture(5, 3, 0, 0), b = texture(5, 3, 1, 0);
        const Buf o = runS(*d, {}, {}, a, 1.0, &b, 0.5);
        check(o.w == 5 && o.px.size() == a.px.size(), "a 5 x 3 frame works");
        const Buf a2 = texture(1, 40, 0, 0), b2 = texture(1, 40, 0, 2);
        const Buf o2 = runS(*d, {}, {}, a2, 1.0, &b2, 0.5);
        check(o2.px.size() == a2.px.size(), "a 1 x 40 frame works");
    }
}

// ------------------------------------------------------------------ 2f. Time Warp
static void timeWarpTests()
{
    const Def* d = findDef(kTimeWarp);
    if (!d) return;
    bool hasRate = false;
    for (const PDef& p : d->params) hasRate = hasRate || (p.name == "Slow Motion Ratio" && p.def.size() == 1 && p.def[0] == 0.5);
    check(hasRate, "Slow Motion (Time Warp) has a 'Slow Motion Ratio' (default 0.5)");
    Project p;
    Event e;
    e.id = 1;
    check(eventFxRate(e) == 1.0, "an event without the effect plays at its rate");
    e.fx.push_back(slot(p, kTimeWarp, [] { Json j = Json::object(); j["Slow Motion Ratio"] = val({0.25}); return j; }()));
    check(std::fabs(eventFxRate(e) - 0.25) < 1e-12, "an event with Time Warp plays at its Slow Motion Ratio");
    check(eventFxResample(e) == "optical", "... resampling with the optical flow (the Method's default, Optical Flow)");
    e.fx[0].bypass = true;
    check(eventFxRate(e) == 1.0, "a bypassed Time Warp does nothing");
    e.fx[0].bypass = false;
    e.fx[0].state["params"]["Choice Box Algorithm"] = val({0});
    check(eventFxResample(e) == "force", "the Morph method blends the neighbouring frames");
    e.fx[0].state["params"]["Choice Box Algorithm"] = val({1});
    check(eventFxResample(e) == "optical", "the Optical Flow method follows the motion");
    // Reno's own Slow Motion is read by its own names
    Event e2;
    e2.id = 2;
    e2.fx.push_back(slot(p, kSlowMotionId, [] { Json j = Json::object(); j["PlaybackRate"] = val({0.1}); j["Blending"] = val({1}); return j; }()));
    check(std::fabs(eventFxRate(e2) - 0.1) < 1e-12 && eventFxResample(e2) == "off", "Slow Motion still reads its PlaybackRate and Blending");
    // the media time of an event
    e.fx[0].state["params"]["Slow Motion Ratio"] = val({0.5});
    e.start = 10;
    e.length = 100;
    const double mt = eventMediaTime(e, nullptr, 14);
    check(std::fabs(mt - 2.0) < 1e-9, "the event's media time moves at the Time Warp rate", f3(mt));
}

// ------------------------------------------------------------------ 3. scale
static void scaleTests()
{
    const int W = 480, H = 272;           // (both divisible by 4: the reduced pictures cover exactly the same frame)
    const Buf pic = picture(W, H, 0), ramp = ramps(W, H);
    struct Case { std::string id, label; Params p; StrMap s; };
    std::vector<Case> cases;
    const std::string ell = [] {
        BezPath p;
        const double rx = 0.3, ry = 0.3, k = 0.5522847498;
        auto sm = [&](double x, double y, double dx, double dy) { BezPoint b; b.x = x; b.y = y; b.ix = x - dx; b.iy = y - dy; b.ox = x + dx; b.oy = y + dy; p.pts.push_back(b); };
        sm(0.5, 0.5 + ry, k * rx, 0); sm(0.5 + rx, 0.5, 0, -k * ry); sm(0.5, 0.5 - ry, -k * rx, 0); sm(0.5 - rx, 0.5, 0, k * ry);
        return formatBezPath(p);
    }();
    cases.push_back({kBz, "Bezier Masking, feathered curve", {{"Feather_0", {1}}, {"Type_0", {3}}}, {{"AnchorList_0", ell}}});
    cases.push_back({kBz, "Bezier Masking, feathered oval and a curve", {{"Feather_0", {1}}, {"Feather_1", {1}}, {"Enable_1", {1}}, {"Type_1", {3}}, {"Angle_0", {20}}}, {{"AnchorList_1", rectPath(0.1, 0.1, 0.5, 0.9)}}});
    cases.push_back({kStereo, "Stereoscopic 3D Adjust", {{"Horizontal", {0.03}}, {"Rotation", {3}}, {"Keystone", {0.1}}}, {}});
    cases.push_back({kLens, "Lens Correction", {{"RadialDistParam1", {0.03}}, {"RadialDistParam2", {-0.02}}, {"Scale", {1.1}}}, {}});
    cases.push_back({kLens, "Lens Correction, scaled up", {{"RadialDistParam1", {0.05}}, {"Scale", {1.2}}}, {}});
    cases.push_back({kZoom, "Smart Zoom", {{"LocationAndZoom", {0.3, 0.6, 2.0}}}, {}});
    cases.push_back({kBump, "Bump Map", {{"BumpHeight", {0.4}}}, {}});
    for (const Case& c : cases) {
        const Def* d = findDef(c.id);
        if (!d) continue;
        // (the masks cut a picture of hard edges where they cross them, which a smaller picture can not average the same way: a smooth one)
        const Buf& use = c.id == kBz ? ramp : pic;
        const Buf full = runS(*d, c.p, c.s, use, 1.0);
        double change = maxDiff(full, use);
        check(change > 0.02, c.label + ": changes the picture", f3(change));
        for (int f : {2, 4}) {
            const Buf small = runS(*d, c.p, c.s, reduce(use, f), 1.0 / f);
            const Diff df = compare(small, reduce(full, f));
            // Not scale free: Smart Zoom's Lanczos and sharpening, thin line grids, Stereoscopic 3D Adjust's 8-bit warp,
            // and Lens Correction's edge rows and columns (one pixel wide at any size).
            const double bound = c.id == kZoom ? 3.0 : c.id == kLens ? 5.0 : c.id == kStereo ? 3.0 : c.label.find("grid") != std::string::npos ? 6.0 : 1.5;
            int bx = 0, by = 0;
            {
                const Buf r = reduce(full, f);
                float bm = 0;
                for (int y = 0; y < r.h; ++y)
                    for (int x = 0; x < r.w; ++x)
                        for (int k = 0; k < 4; ++k) {
                            const float dd = std::fabs(r.at(x, y)[k] - small.at(x, y)[k]);
                            if (dd > bm) { bm = dd; bx = x; by = y; }
                        }
            }
            check(df.mean < bound, c.label + strfmt(": at 1/%d the size it agrees with the full-size one averaged down", f), strfmt("mean %.3f max %.0f levels at %d,%d", df.mean, df.mx, bx, by));
        }
    }
    // the flow transition
    const Def* fl = findDef(kFlow);
    if (fl) {
        const Buf A = texture(W, H, 0, 0), B = texture(W, H, 8, 4);
        const Buf full = runS(*fl, {}, {}, A, 1.0, &B, 0.5);
        for (int f : {2, 4}) {
            const Buf a = reduce(A, f), b = reduce(B, f);
            const Buf small = runS(*fl, {}, {}, a, 1.0 / f, &b, 0.5);
            const Diff df = compare(small, reduce(full, f));
            check(df.mean < 3.0, strfmt("Warp Flow: at 1/%d the size it agrees with the full-size one averaged down", f), strfmt("mean %.3f max %.0f levels", df.mean, df.mx));
        }
    }
}

// ------------------------------------------------------------------ 4. threads
static void threadTests()
{
    const int W = 320, H = 180;
    const Buf pic = picture(W, H, 0);
    const Buf A = texture(W, H, 0, 0), B = texture(W, H, 5, 2);
    for (const std::string& id : {kBz, kStereo, kLens, kZoom, kFlow}) {
        const Def* d = findDef(id);
        if (!d) continue;
        Params over;
        StrMap sov;
        if (id == kBz) { curve(over, sov, 0, rectPath(0.2, 0.2, 0.8, 0.7)); over["Feather_0"] = {1}; }
        if (id == kStereo) over = {{"Rotation", {4}}, {"Horizontal", {0.02}}};
        if (id == kLens) over = {{"RadialDistParam1", {0.05}}, {"Scale", {1.2}}};
        const bool tr = id == kFlow;
        const Buf serial = runS(*d, over, sov, tr ? A : pic, 1.0, tr ? &B : nullptr, 0.5);
        std::vector<Buf> got(4);
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t) th.emplace_back([&, t] { got[(size_t)t] = runS(*d, over, sov, tr ? A : pic, 1.0, tr ? &B : nullptr, 0.5); });
        for (auto& t : th) t.join();
        bool same = true;
        for (const Buf& g : got) same = same && g.px == serial.px;
        check(same, id + ": four threads rendering at once get exactly what one gets");
    }
}

// ------------------------------------------------------------------ 5. through the engine and the OpenFX host
static void engineChecks()
{
    Rig rig;
    struct Case { std::string id, label; Json params; };
    std::vector<Case> cases;
    auto strVal = [](const std::string& s) { Json e = Json::object(); e["s"] = Json(s); return e; };
    {
        Json p = Json::object();
        p["AnchorList_0"] = strVal(rectPath(0.2, 0.2, 0.8, 0.8));
        p["Type_0"] = val({3});
        p["Feather_0"] = val({0.5});
        cases.push_back({kBz, "Bezier Masking", p});
    }
    {
        Json p = Json::object();
        p["Horizontal"] = val({0.05});
        p["Rotation"] = val({4});
        cases.push_back({kStereo, "Stereoscopic 3D Adjust", p});
    }
    {
        Json p = Json::object();
        p["RadialDistParam1"] = val({0.04});
        p["Scale"] = val({1.1});
        cases.push_back({kLens, "Lens Correction", p});
    }
    cases.push_back({kZoom, "Smart Zoom", Json::object()});
    cases.push_back({kBump, "Bump Map", [] { Json p = Json::object(); p["BumpHeight"] = val({0.5}); return p; }()});
    for (const Case& c : cases) {
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        e.fx.push_back(slot(p, c.id, c.params));
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
        check(meanDiff(full, none) > 0.03, c.label + ": it changes the picture", strfmt("%.3f", meanDiff(full, none)));
        double mx = 0;
        const double d2 = meanDiff(half, reduceImg(full, 2), &mx), d4 = meanDiff(quarter, reduceImg(full, 4));
        // (Bump Map lights the slopes of the picture's channels: at a hard edge the 3x3 gradient of a smaller picture spans more of
        // the picture, as VEGAS's own does)
        const double bound = c.id == kZoom ? 4.0 : c.id == kBump ? 3.5 : c.id == kLens ? 5.0 : 2.0;
        check(half && quarter && d2 < bound && d4 < bound, c.label + ": the engine's half and quarter size pictures agree with the full-size one", strfmt("1/2 mean %.3f max %.0f, 1/4 mean %.3f", d2, mx, d4));
        ofx::Plugin* pl = findVideoPlugin(c.id, nullptr);
        check(pl && pl->info.multiResolution && pl->safety == 2, c.label + ": described as multi-resolution and fully render thread safe");
        rig.fo.parallel = true;
        rig.fx.prepare(p);
        rig.fo.scale = 1.0;
        ImagePtr par = rig.video.render(p, 1.0, rig.fo);
        rig.fo.parallel = false;
        check(par && full && meanDiff(par, full) == 0, c.label + ": rendering with the engine's threads gives the same picture");
    }
    // Time Warp as an event effect: the media time follows the rate; the effect itself is a plain pass-through that loads
    {
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        Json params = Json::object();
        params["Slow Motion Ratio"] = val({0.5});
        e.fx.push_back(slot(p, kTimeWarp, params));
        p.tracks[0].events.push_back(e);
        Project plain = rig.base();
        MediaItem m2 = rig.still(plain, "synth:0");
        plain.tracks[0].events.push_back(rig.event(plain, m2, 0, 5));
        rig.fresh();
        ImagePtr a = rig.render(p, 1.0, 1.0);
        const std::string why = rig.fx.error(p.tracks[0].events[0].fx[0].id);
        check(a && why.empty() && rig.fx.video(p.tracks[0].events[0].fx[0].id) != nullptr, "Time Warp: loads and renders through the OpenFX host", why);
        rig.fresh();
        ImagePtr b = rig.render(plain, 1.0, 1.0);
        check(a && b && meanDiff(a, b) < 0.5, "... and a still picture stays the same");
    }
    // Warp Flow as a transition between two events: renders, differs from the plain dissolve of cross
    {
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0"), m2 = rig.still(p, "synth:1");
        Event e1 = rig.event(p, m, 0, 5), e2 = rig.event(p, m2, 3, 5);
        e2.id = p.newId();
        p.tracks[0].events.push_back(e1);
        p.tracks[0].events.push_back(e2);
        rig.fresh();
        ImagePtr before = rig.render(p, 1.0, 1.0);
        check(before != nullptr, "two overlapping events render (the setting for a transition)");
        const Def* d = findDef(kFlow);
        check(d && d->transition, "Warp Flow is offered as a transition");
        ofx::Plugin* pl = findVideoPlugin(kFlow, nullptr);
        check(pl != nullptr, "Warp Flow is found by the host as a plug-in");
    }
}

// ------------------------------------------------------------------ 6. the on-screen controls
// OpenGL stand-ins that count what the controls draw and where.
static int gBegins = 0, gEnds = 0, gVerts = 0, gOpen = 0, gBad = 0;
static double gMinX = 1e9, gMaxX = -1e9, gMinY = 1e9, gMaxY = -1e9;
static void fakeBegin(unsigned) { ++gBegins; ++gOpen; }
static void fakeEnd() { ++gEnds; --gOpen; }
static void fakeVertex(double x, double y)
{
    ++gVerts;
    if (gOpen != 1 || !std::isfinite(x) || !std::isfinite(y)) ++gBad;
    gMinX = std::min(gMinX, x); gMaxX = std::max(gMaxX, x); gMinY = std::min(gMinY, y); gMaxY = std::max(gMaxY, y);
}
static void fakeColor(float, float, float, float) {}
static void fakeWidth(float) {}
static void fakeEnable(unsigned) {}
static void fakeBlend(unsigned, unsigned) {}
static void resetGL() { gBegins = gEnds = gVerts = gOpen = gBad = 0; gMinX = gMinY = 1e9; gMaxX = gMaxY = -1e9; }

static void overlayTests()
{
    {
        MaskOverlayGL g;
        g.Begin = fakeBegin; g.End = fakeEnd; g.Vertex2d = fakeVertex; g.Color4f = fakeColor; g.LineWidth = fakeWidth; g.Enable = fakeEnable; g.BlendFunc = fakeBlend;
        maskOverlayUseGL(&g);
    }
    using namespace reno::ofx;
    Host& host = Host::get();
    std::string err;
    Binary* bin = host.loadBinary(kBuiltinBinaryPath, &err);
    check(bin != nullptr, "the built-in plug-ins load through the host", err);
    if (!bin) return;
    auto make = [&](const std::string& id, int w, int h) -> std::unique_ptr<Instance> {
        Plugin* p = host.findPlugin(id);
        if (!p) return nullptr;
        p->load();
        InstanceSetup s;
        s.width = w;
        s.height = h;
        std::string e;
        return p->createInstance(p->filterContext(), s, &e);
    };
    for (const std::string& id : {kLens, kZoom, kBz}) {
        Plugin* p = host.findPlugin(id);
        check(p && p->info.hasOverlay, id + ": the plug-in says it has on-screen controls");
    }
    Instance::OverlayArgs a;
    a.pixelScale[0] = a.pixelScale[1] = 1.0;
    a.viewport[0] = 640;
    a.viewport[1] = 360;
    // the dot: Lens Correction's DistCenter and Smart Zoom's LocationAndZoom (x and y)
    for (const std::string& id : {kLens, kZoom}) {
        auto inst = make(id, 640, 360);
        check(inst && inst->createOverlay(), id + ": the overlay is made");
        if (!inst) continue;
        const bool lens = id == kLens;
        Param* center = inst->params.find(lens ? "DistCenter" : "LocationAndZoom");
        const size_t dim = lens ? 2 : 3;
        check(center && center->value.size() == dim && center->value[0] == 0.5 && center->value[1] == 0.5, id + ": the dot starts in the middle");
        if (!center) continue;
        const double z0 = dim == 3 ? center->value[2] : 0;
        check(!inst->overlayPen(kOfxInteractActionPenDown, a, 100, 100, 1, 0, 0), id + ": a pen down away from the dot is not taken");
        check(inst->overlayPen(kOfxInteractActionPenDown, a, 322, 178, 1, 0, 0), id + ": a pen down on the dot is taken");
        check(inst->overlayPen(kOfxInteractActionPenMotion, a, 160, 90, 1, 0, 0) && std::fabs(center->value[0] - 0.25) < 1e-9 && std::fabs(center->value[1] - 0.25) < 1e-9, id + ": dragging moves it (as fractions of the frame, from the bottom left)");
        inst->overlayPen(kOfxInteractActionPenMotion, a, 5000, -300, 1, 0, 0);
        check(center->value[0] == 1.0 && center->value[1] == 0.0, id + ": ... and keeps it inside the frame");
        check(center->value.size() == dim && (dim == 2 || center->value[2] == z0), id + ": ... the zoom of a Double3D stays as it was");
        check(inst->overlayPen(kOfxInteractActionPenUp, a, 5000, -300, 0, 0, 0), id + ": pen up ends the drag");
        check(!inst->overlayPen(kOfxInteractActionPenMotion, a, 10, 10, 1, 0, 0), id + ": motion afterwards is not taken");
        resetGL();
        check(inst->overlayDraw(a) && gVerts >= 8 && gBegins == gEnds && gOpen == 0 && gBad == 0, id + ": it draws a cross and a square (every Begin ended, every vertex inside one)", strfmt("%d vertices", gVerts));
        const double ccx = center->value[0] * 640, ccy = center->value[1] * 360;
        check(std::fabs(gMinX - (ccx - 14)) < 0.01 && std::fabs(gMaxX - (ccx + 14)) < 0.01 && std::fabs(gMinY - (ccy - 14)) < 0.01 && std::fabs(gMaxY - (ccy + 14)) < 0.01, id + ": ... at the dot (in canonical coordinates)", strfmt("x %.1f..%.1f y %.1f..%.1f", gMinX, gMaxX, gMinY, gMaxY));
    }
    // the masks
    auto inst = make(kBz, 640, 360);
    check(inst && inst->createOverlay(), "Bezier Masking: the overlay is made");
    if (!inst) return;
    Param *editMode = inst->params.find("EditMode_0"), *type = inst->params.find("Type_0"), *loc = inst->params.find("Location_0"), *tool = inst->params.find("EditTool_0");
    Param* path = inst->params.find("AnchorList_0");
    check(editMode && type && loc && tool && path, "the mask's EditMode_0, Type_0, Location_0, EditTool_0 and AnchorList_0 exist");
    if (!editMode || !type || !loc || !tool || !path) return;
    // Edit Mode off: nothing is drawn and the pen is not taken
    resetGL();
    inst->overlayDraw(a);
    check(gVerts == 0 && !inst->overlayPen(kOfxInteractActionPenDown, a, 320, 180, 1, 0, 0), "with Edit Mode off nothing is drawn and the pen is not taken");
    // an Oval (0.3 x 0.533 of the frame: VEGAS's default is a rectangle of half the frame): shown, and dragged inside to move Location
    type->value[0] = 0;
    if (Param* ww = inst->params.find("Width_0")) ww->value[0] = 0.3;
    if (Param* hh = inst->params.find("Height_0")) hh->value[0] = 0.533;
    editMode->value[0] = 1;
    resetGL();
    check(inst->overlayDraw(a) && gVerts >= 40 && gBegins == gEnds && gOpen == 0 && gBad == 0, "an Oval in Edit Mode is outlined", strfmt("%d vertices", gVerts));
    // (0.3 of the width = 192 pixels across, 0.533 of the height = 192 pixels: a circle of 96 pixels radius about the middle)
    check(gMinX > 320 - 100 && gMaxX < 320 + 100 && gMinY > 180 - 100 && gMaxY < 180 + 100 && gMaxX - gMinX > 150, "... where the oval is", strfmt("x %.1f..%.1f y %.1f..%.1f", gMinX, gMaxX, gMinY, gMaxY));
    check(!inst->overlayPen(kOfxInteractActionPenDown, a, 20, 20, 1, 0, 0), "a pen down outside the oval is not taken");
    check(inst->overlayPen(kOfxInteractActionPenDown, a, 320, 180, 1, 0, 0), "a pen down inside it is");
    inst->overlayPen(kOfxInteractActionPenMotion, a, 420, 230, 1, 0, 0);
    inst->overlayPen(kOfxInteractActionPenUp, a, 420, 230, 0, 0, 0);
    check(std::fabs(loc->value[0] - (0.5 + 100.0 / 640)) < 1e-9 && std::fabs(loc->value[1] - (0.5 + 50.0 / 360)) < 1e-9, "dragging moves its Location", strfmt("%.4f %.4f", loc->value[0], loc->value[1]));
    loc->value[0] = loc->value[1] = 0.5;
    // a Curve: points are drawn by clicking
    type->value[0] = 3;
    auto text = [&]() { return path->str; };
    auto click = [&](double x, double y) {
        const bool d = inst->overlayPen(kOfxInteractActionPenDown, a, x, y, 1, 0, 0);
        inst->overlayPen(kOfxInteractActionPenUp, a, x, y, 0, 0, 0);
        return d;
    };
    BezPath bp;
    // three clicks and a click on the first point make a closed triangle
    check(click(100, 60) && click(500, 60) && click(300, 300), "three clicks add three points");
    check(parseBezPath(text(), bp) && bp.pts.size() == 3 && !bp.closed, "... which are an open outline", text());
    check(std::fabs(bp.pts[0].x - 100.0 / 640) < 1e-3 && std::fabs(bp.pts[0].y - 60.0 / 360) < 1e-3, "... at the places clicked (fractions of the frame)");
    check(click(102, 62), "a click on the first point is taken");
    check(parseBezPath(text(), bp) && bp.pts.size() == 3 && bp.closed, "... and closes the outline");
    // drawing the outline and its handles
    {
        resetGL();
        check(inst->overlayDraw(a) && gVerts >= 12 && gBegins == gEnds && gOpen == 0 && gBad == 0, "the outline and its points are drawn (every Begin ended, every vertex inside one)", strfmt("%d vertices", gVerts));
        check(gMinX >= 99 - 6 && gMaxX <= 501 + 6 && gMinY >= 59 - 6 && gMaxY <= 301 + 6, "... where the outline is", strfmt("x %.1f..%.1f y %.1f..%.1f", gMinX, gMaxX, gMinY, gMaxY));
    }
    // the picture now
    auto areaOf = [&](const Param* anchors, const std::vector<std::pair<const char*, Param*>>& extra) {
        Buf white;
        white.resize(64, 36);
        for (float& v : white.px) v = 1.f;
        Params ps = {{"Type_0", {3}}};
        for (const auto& e : extra) ps[e.first] = e.second->value;
        const Buf o = runS(*findDef(kBz), ps, {{"AnchorList_0", anchors->str}}, white);
        return sum(o, 3) / (64.0 * 36.0);
    };
    {
        // the triangle (100,60) (500,60) (300,300) of 640 x 360 (y up): its area is half base times height = 0.5 * 400 * 240 = 48000 of 230400
        const double frac = areaOf(path, {}), want = 48000.0 / 230400.0;
        check(std::fabs(frac - want) < 0.01, "the closed triangle masks the picture to its area", strfmt("%.4f vs %.4f", frac, want));
    }
    // dragging a point
    check(inst->overlayPen(kOfxInteractActionPenDown, a, 300, 300, 1, 0, 0), "a pen down on a point is taken");
    inst->overlayPen(kOfxInteractActionPenMotion, a, 320, 320, 1, 0, 0);
    inst->overlayPen(kOfxInteractActionPenUp, a, 320, 320, 0, 0, 0);
    check(parseBezPath(text(), bp) && std::fabs(bp.pts[2].x - 320.0 / 640) < 1e-3 && std::fabs(bp.pts[2].y - 320.0 / 360) < 1e-3, "dragging moves the point");
    // the Edit Tool: Normal does not add a point on a side, Add and Split do, Delete removes a point
    check(!click(300, 60), "with the Normal tool a click on a side is not taken");
    check(parseBezPath(text(), bp) && bp.pts.size() == 3, "... and adds nothing");
    tool->value[0] = 1;
    check(click(300, 60), "with the Add tool a click on a side is taken");
    check(parseBezPath(text(), bp) && bp.pts.size() == 4 && std::fabs(bp.pts[1].x - 300.0 / 640) < 0.01, "... and adds a point there, between the neighbours", text());
    tool->value[0] = 2;
    check(click(300, 60), "with the Delete tool a click on a point is taken");
    check(parseBezPath(text(), bp) && bp.pts.size() == 3, "... and removes it");
    tool->value[0] = 3;
    check(click(300, 60) && parseBezPath(text(), bp) && bp.pts.size() == 4, "the Split tool adds a point on a side too");
    tool->value[0] = 0;
    // delete the selected point
    check(inst->overlayKey(kOfxInteractActionKeyDown, a, 0xffff, ""), "Delete is taken");
    check(parseBezPath(text(), bp) && bp.pts.size() == 3, "... and removes the selected point");
    // with a placement, the outline on the preview is where it is clicked and the picture matches it
    {
        loc->value = {0.6, 0.45};
        inst->params.find("Angle_0")->value[0] = 30;
        inst->params.find("AnchorScale_0")->value = {1.5, 0.8};
        path->str.clear();
        check(click(100, 60) && click(500, 60) && click(300, 300) && click(101, 61), "with Location, Angle and Anchor Scale set, three points and a close are taken");
        BezPath q;
        check(parseBezPath(text(), q) && q.pts.size() == 3 && q.closed, "... as a closed outline");
        resetGL();
        inst->overlayDraw(a);
        check(gMinX >= 99 - 6 && gMaxX <= 501 + 6 && gMinY >= 59 - 6 && gMaxY <= 301 + 6, "... drawn where it was clicked (the placement is undone for what is kept)", strfmt("x %.1f..%.1f y %.1f..%.1f", gMinX, gMaxX, gMinY, gMaxY));
        const double frac = areaOf(path, {{"Location_0", loc}, {"Angle_0", inst->params.find("Angle_0")}, {"AnchorScale_0", inst->params.find("AnchorScale_0")}});
        check(std::fabs(frac - 48000.0 / 230400.0) < 0.01, "... and the picture is masked to the triangle that was drawn", strfmt("%.4f", frac));
        // dragging one of its points moves it by what the mouse moved
        check(inst->overlayPen(kOfxInteractActionPenDown, a, 300, 300, 1, 0, 0), "a point of a placed outline can be picked");
        inst->overlayPen(kOfxInteractActionPenMotion, a, 330, 330, 1, 0, 0);
        inst->overlayPen(kOfxInteractActionPenUp, a, 330, 330, 0, 0, 0);
        resetGL();
        inst->overlayDraw(a);
        check(gMaxY >= 330 - 6 && gMaxY <= 330 + 6, "... and dragged where the mouse took it", strfmt("top %.1f", gMaxY));
        loc->value = {0.5, 0.5};
        inst->params.find("Angle_0")->value[0] = 0;
        inst->params.find("AnchorScale_0")->value = {1, 1};
    }
    // a clean triangle again, then a handle: select a point and make it smooth, then pull a handle
    path->str.clear();
    inst->overlayKey(kOfxInteractActionKeyDown, a, 0xff1b, "");
    click(100, 60); click(500, 60); click(300, 300); click(101, 61);
    check(parseBezPath(text(), bp) && bp.pts.size() == 3 && bp.closed, "a triangle again");
    check(inst->overlayPen(kOfxInteractActionPenDown, a, 100, 60, 1, 0, 0), "a corner is picked");
    inst->overlayPen(kOfxInteractActionPenUp, a, 100, 60, 0, 0, 0);
    inst->overlayKey(kOfxInteractActionKeyDown, a, 's', "s");
    check(parseBezPath(text(), bp) && (bp.pts[0].ox != bp.pts[0].x || bp.pts[0].oy != bp.pts[0].y), "S gives the selected point handles");
    {
        const double hx = bp.pts[0].ox * 640, hy = bp.pts[0].oy * 360;
        check(inst->overlayPen(kOfxInteractActionPenDown, a, hx, hy, 1, 0, 0), "its out handle can be picked");
        inst->overlayPen(kOfxInteractActionPenMotion, a, hx + 30, hy + 40, 1, 0, 0);
        inst->overlayPen(kOfxInteractActionPenUp, a, hx + 30, hy + 40, 0, 0, 0);
        BezPath q;
        parseBezPath(text(), q);
        check(std::fabs(q.pts[0].ox * 640 - (hx + 30)) < 1e-2 && std::fabs(q.pts[0].oy * 360 - (hy + 40)) < 1e-2, "dragging a handle moves it");
        // the other handle follows, opposite
        const double ix = (q.pts[0].ix - q.pts[0].x) * 640, iy = (q.pts[0].iy - q.pts[0].y) * 360;
        const double ox = (q.pts[0].ox - q.pts[0].x) * 640, oy = (q.pts[0].oy - q.pts[0].y) * 360;
        check(std::fabs(ix * oy - iy * ox) < 1e-3 * std::hypot(ix, iy) * std::hypot(ox, oy) + 1e-6 && ix * ox + iy * oy < 0, "... and the other handle of a smooth point stays opposite");
    }
    // C makes it a corner again
    inst->overlayKey(kOfxInteractActionKeyDown, a, 'c', "c");
    check(parseBezPath(text(), bp) && bp.pts[0].ox == bp.pts[0].x && bp.pts[0].ix == bp.pts[0].x, "C takes the handles away");
    // Delete with nothing selected clears the whole outline
    inst->overlayKey(kOfxInteractActionKeyDown, a, 0xff1b, "");
    check(inst->overlayKey(kOfxInteractActionKeyDown, a, 0xffff, "") && path->str.empty(), "Delete with nothing selected clears the outline");
    // the first mask whose Edit Mode is on is the one edited
    {
        Param *e1 = inst->params.find("EditMode_1"), *t1 = inst->params.find("Type_1"), *a1 = inst->params.find("AnchorList_1");
        check(e1 && t1 && a1, "mask 2's settings exist");
        if (e1 && t1 && a1) {
            click(100, 60); click(500, 60); click(300, 300); click(101, 61);
            const std::string first = path->str;
            editMode->value[0] = 0;
            e1->value[0] = 1;
            t1->value[0] = 3;
            check(click(100, 100), "with Edit Mode on for mask 2 only, the click starts that mask's outline");
            check(!a1->str.empty() && path->str == first, "... and the first mask is left alone");
            editMode->value[0] = 1;
            resetGL();
            inst->overlayDraw(a);
            check(gBegins == gEnds && gOpen == 0, "with two masks in Edit Mode, the first is the one shown");
            e1->value[0] = 0;
        }
    }
    // junk: random pen events and keys never crash
    {
        unsigned seed = 99;
        for (int i = 0; i < 3000; ++i) {
            seed = seed * 1103515245u + 12345u;
            const double x = (int)((seed >> 8) % 900) - 130, y = (int)((seed >> 4) % 600) - 120;
            const int what = (int)((seed >> 20) % 5);
            if (what == 0) inst->overlayPen(kOfxInteractActionPenDown, a, x, y, 1, 0, 0);
            else if (what == 1) inst->overlayPen(kOfxInteractActionPenMotion, a, x, y, 1, 0, 0);
            else if (what == 2) inst->overlayPen(kOfxInteractActionPenUp, a, x, y, 0, 0, 0);
            else {
                static const int syms[] = {0xffff, 0xff08, 0xff0d, 0xff1b, 'r', 'e', 'c', 's', 'x'};
                inst->overlayKey(kOfxInteractActionKeyDown, a, syms[(seed >> 12) % 9], "");
            }
            if (i % 500 == 0) tool->value[0] = (double)((seed >> 3) % 4);
        }
        check(inst->overlayMade(), "3000 random pen and key events: the overlay is still there");
        resetGL();
        inst->overlayDraw(a);
        check(gBegins == gEnds && gOpen == 0 && gBad == 0, "... and still draws sensibly");
    }
    maskOverlayUseGL(nullptr);
}

// ------------------------------------------------------------------ 7. a project with the effects: live instances, a .veg round trip
// (allowMissingDoubles: the .veg writer leaves out the decimal settings of an effect it has no VEGAS record of, and
// warns about it)
static bool sameParamsJson(const Json& a, const Json& b, std::string& why, bool allowMissingDoubles = false)
{
    for (const auto& m : a.get("params").members()) {
        const Json& x = m.second;
        const Json& y = b.get("params").get(m.first);
        if (y.isNull() && allowMissingDoubles && x.has("v") && !x.has("i")) continue;
        if (y.isNull()) { why = m.first + " is gone"; return false; }
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

static void projectRoundTrip()
{
    Rig rig;
    Project p = rig.base();
    MediaItem m = rig.still(p, "synth:0");
    Event e = rig.event(p, m, 0, 5);
    auto strVal = [](const std::string& s) { Json v = Json::object(); v["s"] = Json(s); return v; };
    {
        Json j = Json::object();
        j["AnchorList_0"] = strVal(rectPath(0.2, 0.3, 0.7, 0.8));
        j["Type_0"] = val({3});
        j["Feather_0"] = val({0.25});
        j["FeatherStyle_1"] = val({1});
        j["Invert"] = val({1});
        e.fx.push_back(slot(p, kBz, j));
    }
    {
        Json j = Json::object();
        j["Horizontal"] = val({0.04});
        j["Rotation"] = val({-3.5});
        j["FlipVertical"] = val({2});
        e.fx.push_back(slot(p, kStereo, j));
    }
    {
        Json j = Json::object();
        j["RadialDistParam1"] = val({0.035});
        j["DistCenter"] = val({0.4, 0.6});
        e.fx.push_back(slot(p, kLens, j));
    }
    {
        Json j = Json::object();
        j["LocationAndZoom"] = val({0.2, 0.8, 3.5});
        e.fx.push_back(slot(p, kZoom, j));
    }
    {
        Json j = Json::object();
        j["Slow Motion Ratio"] = val({0.25});
        j["Choice Box Quality"] = val({2});
        e.fx.push_back(slot(p, kTimeWarp, j));
    }
    e.fx.push_back(slot(p, kBump, [] { Json j = Json::object(); j["BumpHeight"] = val({0.7}); return j; }()));
    p.tracks[0].events.push_back(e);
    FxRuntime fx;
    fx.prepare(p);
    std::vector<FxSlot*> mine;
    p.forEachFx([&](FxSlot& s) { if (s.kind == "ofx") mine.push_back(&s); });
    check(mine.size() == 6, "the project has six of the effects");
    std::vector<Json> before;
    for (FxSlot* s : mine) {
        before.push_back(s->state);
        const std::string why = fx.error(s->id);
        check(fx.video(s->id) != nullptr && why.empty(), s->pluginId + ": the live instance is made", why);
    }
    fx.captureStates(p);
    std::string why;
    bool live = true;
    for (size_t i = 0; i < mine.size(); ++i)
        if (!sameParamsJson(before[i], mine[i]->state, why)) { live = false; std::printf("   %s through the instance: %s\n", mine[i]->pluginId.c_str(), why.c_str()); }
    check(live, "the values survive the live instances");
    std::vector<std::string> warn;
    const std::string bytes = writeVegasData(p, "/tmp/vxmask_in.veg", &warn);
    const std::string tmp = "/tmp/vxmask_roundtrip.veg";
    check(!bytes.empty() && writeFile(tmp, bytes), "written as a .veg");
    Project q;
    std::string err;
    check(loadProjectFile(tmp, q, &err), "the written file reads back", err);
    std::vector<FxSlot*> again;
    q.forEachFx([&](FxSlot& s) { if (s.kind == "ofx") again.push_back(&s); });
    bool ok = again.size() == mine.size();
    for (size_t i = 0; ok && i < mine.size(); ++i)
        if (again[i]->pluginId != mine[i]->pluginId || !sameParamsJson(before[i], again[i]->state, why, true)) { ok = false; std::printf("   %s after a .veg round trip: %s\n", mine[i]->pluginId.c_str(), why.c_str()); }
    check(ok, "text and whole-number settings survive a .veg write and read", why);
    check(!warn.empty() && warn[0].find("were not saved") != std::string::npos, "... and the writer says what it left out");
    const std::string log = fxLogText(p, fx, "/some/where/test.veg");
    check(log.find(kBz) != std::string::npos && log.find(kLens) != std::string::npos && log.find("built-in") != std::string::npos, "fx-log.txt names them and says they are built-in");
    fx.releaseExcept({});
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    ofx::setTextRasterizer(blockRasterizer);
    registrationTests();
    vegasParamsTests();
    defaultsTests();
    stereoTests();
    lensTests();
    zoomTests();
    bezierTests();
    flowTests();
    timeWarpTests();
    scaleTests();
    threadTests();
    engineChecks();
    projectRoundTrip();
    overlayTests();
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
