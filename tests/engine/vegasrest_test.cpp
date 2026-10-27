// Tests of the settings VEGAS's effects have that the built-ins lacked, and the remaining built-ins
// (src/ofx/builtins_vegasrest.cpp).
//
//   vegasrest_test
//
// Covers the settings and groups, the buttons and linked settings through the OpenFX host, and the new built-ins:
// Scene Rotation, 360 Stabilization, Displacement Map, Height Map, Histogram, Gradient Wipe.
#include "../../src/core/base.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/builtins_impl.h"
#include "../../src/ofx/ofxhost.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
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

static const std::string V = "com.vegascreativesoftware:";

// ------------------------------------------------------------------ the host
static ofx::Plugin* plugin(const std::string& id)
{
    static ofx::Binary* bin = ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath, nullptr);
    if (!bin) return nullptr;
    for (const auto& up : bin->plugins)
        if (up->info.id == id) return up->load(nullptr) ? up.get() : nullptr;
    return nullptr;
}

static std::unique_ptr<ofx::Instance> instanceOf(const std::string& id, const char* context = nullptr, int w = 160, int h = 90, double frames = 60)
{
    ofx::Plugin* pl = plugin(id);
    if (!pl) return nullptr;
    ofx::InstanceSetup setup;
    setup.width = w;
    setup.height = h;
    setup.fps = 30;
    setup.durationFrames = frames;
    std::string ctx = context ? context : pl->filterContext();
    if (ctx.empty() && pl->supportsContext(kOfxImageEffectContextTransition)) ctx = kOfxImageEffectContextTransition;
    std::string err;
    return pl->createInstance(ctx, setup, &err);
}

typedef std::function<void(int x, int y, int w, int h, double t, float rgb[3])> Paint;

static ImagePtr imageOf(int w, int h, double t, const Paint& paint)
{
    ImagePtr img = Image::make(w, h);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = img->row(h - 1 - y);          // (bottom row first)
        for (int x = 0; x < w; ++x) {
            float c[3] = {0, 0, 0};
            paint(x, y, w, h, t, c);
            for (int k = 0; k < 3; ++k) c[k] = std::min(1.f, std::max(0.f, c[k]));
            row[x * 4 + 0] = (uint8_t)std::lround(c[2] * 255.f);        // B, G, R, A
            row[x * 4 + 1] = (uint8_t)std::lround(c[1] * 255.f);
            row[x * 4 + 2] = (uint8_t)std::lround(c[0] * 255.f);
            row[x * 4 + 3] = 255;
        }
    }
    return img;
}

static void connect(ofx::Instance& inst, int w, int h, const Paint& paint)
{
    inst.setClipSource(kOfxImageEffectSimpleSourceClipName, [=](double t) { return imageOf(w, h, t, paint); });
}

static ImagePtr renderAt(ofx::Instance& inst, double t)
{
    ofx::RenderOptions ro;
    ro.interactive = false;
    std::string err;
    return inst.render(t, ro, &err);
}

static void pixAt(const ImagePtr& img, int x, int y, float c[3])
{
    const uint8_t* p = img->row(img->h - 1 - y) + (size_t)x * 4;
    c[0] = p[2] / 255.f; c[1] = p[1] / 255.f; c[2] = p[0] / 255.f;
}

static ofx::Param* P(ofx::Instance& inst, const char* name) { return inst.params.find(name); }
static double val(ofx::Instance& inst, const char* name, int k = 0)
{
    ofx::Param* p = P(inst, name);
    return p && (int)p->value.size() > k ? p->value[(size_t)k] : -9999;
}
static void setv(ofx::Instance& inst, const char* name, std::vector<double> v) { inst.userSetValue(P(inst, name), v, 0.0); }
static void press(ofx::Instance& inst, const char* name, double t = 0) { inst.userPushButton(P(inst, name), t); }

// the lowest and highest value of a channel over the picture
static void range(const ImagePtr& img, int ch, float& lo, float& hi, int step = 1)
{
    lo = 2; hi = -1;
    for (int y = 0; y < img->h; y += step)
        for (int x = 0; x < img->w; x += step) {
            float c[3];
             pixAt(img, x, y, c);
            lo = std::min(lo, c[ch]); hi = std::max(hi, c[ch]);
        }
}
static double meanOf(const ImagePtr& img, int ch)
{
    double s = 0;
    for (int y = 0; y < img->h; ++y)
        for (int x = 0; x < img->w; ++x) { float c[3];  pixAt(img, x, y, c); s += c[ch]; }
    return s / ((double)img->w * img->h);
}
static double sad(const ImagePtr& a, const ImagePtr& b)
{
    double s = 0;
    for (int y = 0; y < a->h; ++y)
        for (int x = 0; x < a->w; ++x) {
            float p[3], q[3];
             pixAt(a, x, y, p);  pixAt(b, x, y, q);
            s += std::fabs(p[0] - q[0]) + std::fabs(p[1] - q[1]) + std::fabs(p[2] - q[2]);
        }
    return s / ((double)a->w * a->h);
}

// ------------------------------------------------------------------ the settings
static std::string parentOf(ofx::Instance& inst, const char* name)
{
    ofx::Param* p = P(inst, name);
    return p ? p->props.getString(kOfxParamPropParent) : std::string("?");
}

static void settingsTests()
{
    std::printf("-- the settings\n");
    // groups as VEGAS's resource files give them
    struct G { const char* effect; const char* group; const char* member; };
    const G groups[] = {
        {"push", "Border", "BorderSize"}, {"slide", "Border", "BorderColor"}, {"spiral", "Border", "BorderFeather"}, {"zoom", "Border", "BorderSize"},
        {"barndoor", "Border", "BorderSize"}, {"split", "Border", "BorderColor"}, {"squeeze", "Border", "BorderSize"}, {"swap", "Border", "BorderFeather"},
        {"dissolve", "ColorBleedSpeed", "ColorBleedSpeedRed"}, {"dissolve", "ColorMorphSpeed", "ColorMorphSpeedAlpha"},
        {"crosseffect", "EffectSettings", "EffectSettingsMaxZoom"}, {"blackbarfill", "Foreground", "Foreground Scale"}, {"blackbarfill", "Background", "Background Blur"},
        {"broadcastcolors", "Luma", "LumaMax"}, {"broadcastcolors", "Chroma", "ChromaSmoothness"}, {"broadcastcolors", "Composite", "CompositeMax"},
        {"bumpmapfilter", "Location", "LocationSrc"}, {"bumpmapfilter", "Location", "LocationDst"},
        {"channelblend", "Group RED", "RED weight R"}, {"channelblend", "Group ALPHA", "ALPHA weight Offset"},
        {"deform", "Squeeze", "SqueezeLeft"}, {"deform", "Shear", "ShearVertical"}, {"deform", "Curve", "CurveBottom"},
        {"filmeffects", "EnableParticles", "EnableParticlesAmount"}, {"glintvelvetmatter", "Mask", "MaskStrength"}, {"raysvelvetmatter", "Mask", "MaskAngle"},
        {"layerdimensionality", "Shadow", "ShadowBlur"}, {"layerdimensionality", "Glow", "GlowColor2"}, {"layerdimensionality", "Emboss", "LightLocation"},
        {"lightrays", "group0", "group0Strength"}, {"lightrays", "group1", "group1RadiusY"}, {"lightrays", "group2", "group2Noise"},
        {"pictureinpicture", "Points", "CornerBR"}, {"softcontrastvelvetmatter", "Vignette", "VignetteShape"},
        {"brightnessandcontrast", "GroupInput", "Percent"}, {"brightnessandcontrast", "GroupOutput", "Max"}, {"brightnessandcontrast", "Group", "ReferenceMode"},
    };
    for (const G& g : groups) {
        auto inst = instanceOf(V + g.effect);
        if (!inst) { check(false, std::string(g.effect) + " makes an instance"); continue; }
        ofx::Param* grp = P(*inst, g.group);
        check(grp && grp->kind == ofx::ParamKind::Group && parentOf(*inst, g.member) == g.group, std::string(g.effect) + ": " + g.member + " is in the group " + g.group);
    }
    {
        auto inst = instanceOf(V + "brightnessandcontrast");
        check(inst && parentOf(*inst, "GroupInput") == "Group" && parentOf(*inst, "GroupOutput") == "Group" && parentOf(*inst, "DoAnalysis") == "Group", "Brightness and Contrast: Input and Output are inside Automatic Contrast");
        check(inst && P(*inst, "Group") && P(*inst, "Group")->label() == "Automatic Contrast", "  labelled as VEGAS labels it");
        check(inst && P(*inst, "DoAnalysis") && P(*inst, "DoAnalysis")->kind == ofx::ParamKind::PushButton && P(*inst, "DoAnalysis")->label() == "Calculate Brightness & Contrast", "  the analysis is a button");
        check(inst && P(*inst, "ReferenceMode") && P(*inst, "ReferenceMode")->choices().size() == 3 && val(*inst, "Min") == 0 && val(*inst, "Max") == 1, "  three analysis modes, output range 0 to 1");
    }
    {
        auto inst = instanceOf(V + "border");
        check(inst && P(*inst, "color_rgb") && P(*inst, "color_rgb")->kind == ofx::ParamKind::RGB, "Border has color_rgb (RGB)");
    }
    {
        auto inst = instanceOf(V + "bumpmapfilter");
        check(inst && P(*inst, "LocationSrc") && P(*inst, "LocationSrc")->kind == ofx::ParamKind::Double3D, "Bump Map has LocationSrc (3D)");
        check(inst && std::fabs(val(*inst, "LocationSrc", 0) + 1) < 1e-12 && std::fabs(val(*inst, "LocationSrc", 1) - 1) < 1e-12 && std::fabs(val(*inst, "LocationSrc", 2) - 3) < 1e-12,
              "  whose default is VEGAS's own (-1, 1, 3): above the upper left of the frame");
    }
    {
        auto inst = instanceOf(V + "colorcorrectorsecondary");
        check(inst && P(*inst, "SelectEffectRange") && P(*inst, "SelectEffectRange")->kind == ofx::ParamKind::PushButton, "Color Corrector (Secondary) has the Select effect range button");
    }
    {
        auto inst = instanceOf(V + "layerdimensionality");
        auto gen = instanceOf(V + "layerdimensionality", kOfxImageEffectContextGeneral);
        check(inst && P(*inst, "LightLocation") && P(*inst, "LightLocation")->kind == ofx::ParamKind::Double3D, "Layer Dimensionality has LightLocation (3D)");
        check(inst && val(*inst, "LightLocation", 0) == -1 && val(*inst, "LightLocation", 1) == 1 && val(*inst, "LightLocation", 2) == 3, "  default (-1, 1, 3), the plug-in's own (from its describe code)");
        check(gen != nullptr, "  and it can be made in the General context too");
        ofx::Plugin* pl = plugin(V + "layerdimensionality");
        check(pl && pl->supportsContext(kOfxImageEffectContextFilter) && pl->supportsContext(kOfxImageEffectContextGeneral), "  Filter and General are both listed");
    }
    {
        auto inst = instanceOf(V + "timecode");
        ofx::Param* p = inst ? P(*inst, "TimecodeFormat") : nullptr;
        const auto ch = p ? p->choices() : std::vector<std::string>();
        check(ch.size() == 12 && ch[6] == "SMPTE Film Sync IVTC (23.976 fps, Video)" && ch[11] == "SMPTE 30 (30 fps, Audio)", "Timecode's format names have VEGAS's commas");
        check(inst && val(*inst, "TimecodeFormat") == 10, "  and the default is still SMPTE Drop");
    }
    {
        auto inst = instanceOf(V + "whitebalance");
        check(inst && P(*inst, "SelectWhiteColor") && P(*inst, "EnhanceHistogram") && P(*inst, "ClearData") &&
                  P(*inst, "SelectWhiteColor")->kind == ofx::ParamKind::PushButton && P(*inst, "ClearData")->kind == ofx::ParamKind::PushButton,
              "White Balance has its three buttons");
        check(inst && P(*inst, "SelectWhiteColor")->label() == "Select neutral color" && P(*inst, "ClearData")->label() == "Undo Equalization", "  with VEGAS's labels");
    }
    {
        auto inst = instanceOf("de.magix:MeshWarp");
        check(inst && P(*inst, "ScaleGridX") && P(*inst, "ScaleGridY") && P(*inst, "KeepMeshStructure") && P(*inst, "ResetMesh"), "Mesh Warp has the grid multipliers, Preserve Mesh Structure and Reset Mesh");
        check(inst && P(*inst, "EnableWarp")->label() == "Enable Warp Effect" && P(*inst, "GridCountLink")->label() == "Lock Grid size in X and Y", "  and VEGAS's labels");
        check(inst && val(*inst, "ScaleGridX") == 1 && val(*inst, "GridCountX") == 4, "  the old settings are as they were (grid 4, multiplier 1)");
    }
    {
        auto inst = instanceOf("com.sonycreativesoftware:smart_upscale");
        check(inst && P(*inst, "Scale") && P(*inst, "Scale")->kind == ofx::ParamKind::Double3D && val(*inst, "Scale", 0) == 2 && val(*inst, "Scale", 2) == 2, "Smart Upscale's Scale is three numbers, 2 2 2");
        check(inst && P(*inst, "Noise")->label() == "Noise sensitivity" && P(*inst, "EnableEnhancement")->label() == "Enable enhancement", "  with VEGAS's labels");
        // a project saved when Scale was one number
        Json st; Json::parse("{\"params\":{\"Scale\":{\"v\":[3.5]},\"Noise\":{\"v\":[1000]}}}", st);
        inst->loadState(st);
        check(val(*inst, "Scale", 0) == 3.5 && val(*inst, "Scale", 1) == 3.5 && val(*inst, "Scale", 2) == 3.5, "  an old project's single Scale 3.5 becomes 3.5 3.5 3.5");
        check(val(*inst, "Noise") == 1000, "  its other settings load as before");
    }
    {
        auto inst = instanceOf("com.magix.ofx.vr.dual.stich");
        const char* names[] = {"presets", "fov", "orient.left", "orient.right", "falloff.use", "falloff.factor", "falloff.range", "center", "narrowing", "rotation"};
        bool all = inst != nullptr;
        for (const char* n : names) all = all && inst && P(*inst, (std::string("mx.ofx.360.stitch.fisheye.") + n).c_str());
        check(all, "Dual Fish Eye Stitching has VEGAS's ten mx.ofx.360.stitch.fisheye.* settings");
        check(inst && P(*inst, "mx.ofx.360.stitch.group0_0") && P(*inst, "mx.ofx.360.stitch.group1_0") &&
                  parentOf(*inst, "mx.ofx.360.stitch.fisheye.narrowing") == "mx.ofx.360.stitch.group1_0", "  and its two groups");
        check(inst && val(*inst, "mx.ofx.360.stitch.fisheye.fov") == 180 && val(*inst, "mx.ofx.360.stitch.fisheye.falloff.factor") == 20 && val(*inst, "mx.ofx.360.stitch.fisheye.narrowing") == 100,
              "  with the defaults of VEGAS's gui.xml (180, 20, 100)");
    }
    for (const char* id : {"com.magix.ofx.vr.stabilization", "com.magixsoftware:panorotation", "com.vegascreativesoftware:histogram", "com.vegascreativesoftware:gradientwipe",
                           "com.vegascreativesoftware:displacementmap", "com.vegascreativesoftware:heightmap"}) {
        ofx::Plugin* pl = plugin(id);
        check(pl != nullptr, std::string(id) + " is a built-in");
    }
    {
        ofx::Plugin* d = plugin(V + "displacementmap");
        ofx::Plugin* h = plugin(V + "heightmap");
        check(d && h && d->supportsContext(kOfxImageEffectContextGeneral) && d->supportsContext(kOfxImageEffectContextFilter) && h->supportsContext(kOfxImageEffectContextGeneral),
              "Displacement Map and Height Map are General-context effects (and filters too, for the engine)");
        ofx::Plugin* g = plugin(V + "gradientwipe");
        check(g && g->supportsContext(kOfxImageEffectContextTransition), "Gradient Wipe is a transition");
        auto dm = instanceOf(V + "displacementmap");
        check(dm != nullptr, "  and the filter slot of the engine (filterContext) makes an instance of Displacement Map");
    }
}

// ------------------------------------------------------------------ buttons and linked settings
static void buttonTests()
{
    std::printf("-- the buttons\n");
    const int W = 120, H = 70;
    // a low-contrast ramp: brightness 0.3 to 0.6 across
    const Paint dull = [](int x, int, int w, int, double, float c[3]) { const float v = 0.3f + 0.3f * (float)x / (w - 1); c[0] = c[1] = c[2] = v; };
    {
        auto inst = instanceOf(V + "brightnessandcontrast", nullptr, W, H);
        connect(*inst, W, H, dull);
        ImagePtr before = renderAt(*inst, 5);
        float lo, hi;
        range(before, 0, lo, hi);
        check(hi - lo < 0.32f, "Brightness and Contrast at its defaults leaves a dull picture dull", strfmt("%.2f..%.2f", lo, hi));
        setv(*inst, "Percent", {0});
        press(*inst, "DoAnalysis", 5);
        const double b = val(*inst, "Brightness"), c = val(*inst, "Contrast");
        check(c > 0.3, "Calculate Brightness & Contrast sets a positive contrast", strfmt("brightness %.3f contrast %.3f", b, c));
        ImagePtr after = renderAt(*inst, 5);
        range(after, 0, lo, hi);
        check(lo < 0.03f && hi > 0.97f, "  and the picture is now stretched to 0..1", strfmt("%.3f..%.3f", lo, hi));
        // the output range
        setv(*inst, "Min", {0.2});
        setv(*inst, "Max", {0.8});
        press(*inst, "DoAnalysis", 5);
        range(renderAt(*inst, 5), 0, lo, hi);
        check(std::fabs(lo - 0.2) < 0.03 && std::fabs(hi - 0.8) < 0.03, "  with Minimum and Maximum Brightness 0.2 and 0.8 it is stretched to those", strfmt("%.3f..%.3f", lo, hi));
        // Set for each Frame independently: no button, the frame itself decides
        auto per = instanceOf(V + "brightnessandcontrast", nullptr, W, H);
        connect(*per, W, H, [](int x, int, int w, int, double t, float c[3]) { const float lo2 = t < 10 ? 0.3f : 0.1f; c[0] = c[1] = c[2] = lo2 + 0.3f * (float)x / (w - 1); });
        setv(*per, "ReferenceMode", {2});
        setv(*per, "Percent", {0});
        float lo1, hi1, lo2, hi2;
        range(renderAt(*per, 5), 0, lo1, hi1);
        range(renderAt(*per, 20), 0, lo2, hi2);
// (VEGAS's own picture for this mode shows no stretching when a frame is rendered without its analysis: Reno leaves the frames alone too)
        check(hi1 - lo1 < 0.32f && hi2 - lo2 < 0.32f, "Set for each Frame independently: renders both frames as they are (no analysis at render)", strfmt("%.2f..%.2f, %.2f..%.2f", lo1, hi1, lo2, hi2));
        // Current Clip: frames over the clip decide (a clip that is dark at first and bright later)
        auto clip = instanceOf(V + "brightnessandcontrast", nullptr, W, H, 60);
        connect(*clip, W, H, [](int x, int, int w, int, double t, float c[3]) { const float base = 0.1f + 0.5f * (float)(t / 59.0); c[0] = c[1] = c[2] = base + 0.2f * (float)x / (w - 1); });
        setv(*clip, "ReferenceMode", {1});
        setv(*clip, "Percent", {0});
        press(*clip, "DoAnalysis", 30);
        const double bc = val(*clip, "Brightness"), cc = val(*clip, "Contrast");
        // the clip runs from 0.1 to 0.8: a stretch of 1 / 0.7
        check(std::fabs(cc - (1.0 - 0.7)) < 0.08, "Current Clip: the contrast is worked out from the whole clip (0.1..0.8), not the frame", strfmt("contrast %.3f brightness %.3f", cc, bc));
    }
    {
        auto inst = instanceOf(V + "border", nullptr, W, H);
        setv(*inst, "color_rgb", {1, 0.5, 0.25});
        check(std::fabs(val(*inst, "color", 0) - 1) < 1e-9 && std::fabs(val(*inst, "color", 1) - 0.5) < 1e-9 && std::fabs(val(*inst, "color", 2) - 0.25) < 1e-9 && val(*inst, "color", 3) == 1,
              "Border: setting color_rgb sets color (alpha kept)");
        setv(*inst, "color", {0.1, 0.2, 0.3, 0.5});
        check(std::fabs(val(*inst, "color_rgb", 0) - 0.1) < 1e-9 && std::fabs(val(*inst, "color_rgb", 2) - 0.3) < 1e-9 && val(*inst, "color", 3) == 0.5, "  and setting color sets color_rgb");
        // an old project that only has "color"
        auto old = instanceOf(V + "border", nullptr, W, H);
        { Json js; Json::parse("{\"params\":{\"color\":{\"v\":[0.9,0.8,0.7,1]},\"Size\":{\"v\":[0.1]}}}", js); old->loadState(js); }
        check(val(*old, "color", 0) == 0.9 && val(*old, "Size") == 0.1, "  an old project's colour and size load as saved");
        check(std::fabs(val(*old, "color_rgb", 1) - 0.8) < 1e-9, "  and color_rgb follows the colour it loaded with");
        // the same state loaded into a fresh instance draws what the old built-in drew: black border solid with Type 2
        auto draw = instanceOf(V + "border", nullptr, W, H);
        connect(*draw, W, H, [](int, int, int, int, double, float c[3]) { c[0] = c[1] = c[2] = 0.5f; });
        { Json js; Json::parse("{\"params\":{\"Type\":{\"v\":[2],\"i\":true},\"Size\":{\"v\":[0.2]},\"color\":{\"v\":[1,0,0,1]}}}", js); draw->loadState(js); }
        ImagePtr o = renderAt(*draw, 1);
        float c0[3], c1[3];
        pixAt(o, 2, 2, c0); pixAt(o, W / 2, H / 2, c1);
        // (VEGAS reads Border's colour in the order blue, green, red: an old project's red (1, 0, 0) is drawn blue)
        check(c0[2] > 0.9f && c0[0] < 0.1f && c0[1] < 0.1f && std::fabs(c1[0] - 0.5f) < 0.02f, "  and it draws the border from it (VEGAS reads the colour as B, G, R)");
    }
    {
        auto inst = instanceOf(V + "whitebalance", nullptr, W, H);
        connect(*inst, W, H, [](int x, int y, int w, int h, double, float c[3]) {       // bluish, with some detail
            const float t = 0.4f + 0.3f * (float)x / (w - 1) + 0.0f * y * h;
            c[0] = t * 0.7f; c[1] = t * 0.85f; c[2] = t;
        });
        ImagePtr before = renderAt(*inst, 3);
        const double rb = meanOf(before, 0), bb = meanOf(before, 2);
        press(*inst, "SelectWhiteColor", 3);
        const double temp = val(*inst, "ColorTemperature"), tint = val(*inst, "ColorTint");
        check(temp > 6600 && temp <= 15000 && std::fabs(tint) <= 100, "White Balance: Select neutral color sets a temperature for a bluish picture", strfmt("temp %.3f tint %.3f", temp, tint));
        ImagePtr after = renderAt(*inst, 3);
        const double r = meanOf(after, 0), g = meanOf(after, 1), b = meanOf(after, 2);
        check(std::fabs(r - b) < 0.02 && std::fabs(g - b) < 0.03 && std::fabs(rb - bb) > 0.1, "  and the picture comes out neutral", strfmt("r %.3f g %.3f b %.3f (was r %.3f b %.3f)", r, g, b, rb, bb));
        // equalise
        auto eq = instanceOf(V + "whitebalance", nullptr, W, H);
        connect(*eq, W, H, [](int x, int, int w, int, double, float c[3]) { const float v = 0.4f + 0.2f * (float)x / (w - 1); c[0] = c[1] = c[2] = v; });
        ImagePtr flat = renderAt(*eq, 3);
        press(*eq, "EnhanceHistogram", 3);
        check(val(*eq, "HasData") == 1, "Equalize Histogram switches HasData on");
        ImagePtr spread = renderAt(*eq, 3);
        float lo, hi, lo0, hi0;
        range(spread, 0, lo, hi);
        range(flat, 0, lo0, hi0);
        check(hi - lo > 0.8f && hi0 - lo0 < 0.25f, "  and a dull ramp is spread over the range", strfmt("%.2f..%.2f from %.2f..%.2f", lo, hi, lo0, hi0));
        press(*eq, "ClearData", 3);
        check(val(*eq, "HasData") == 0 && sad(renderAt(*eq, 3), flat) < 1e-6, "Undo Equalization switches it off and the picture is as it was");
    }
    {
        auto inst = instanceOf(V + "colorcorrectorsecondary", nullptr, W, H);
        connect(*inst, W, H, [](int x, int y, int w, int h, double, float c[3]) {       // grey, a red square in the middle
            const bool mid = x > w * 3 / 8 - 6 && x < w * 5 / 8 + 6 && y > h * 3 / 8 - 6 && y < h * 5 / 8 + 6;
            if (mid) { c[0] = 0.8f; c[1] = 0.1f; c[2] = 0.1f; } else { c[0] = c[1] = c[2] = 0.5f; }
        });
        setv(*inst, "ShowMask", {1});
        press(*inst, "SelectEffectRange", 2);
        float mid[3], corner[3];
        ImagePtr o = renderAt(*inst, 2);
         pixAt(o, W / 2, H / 2, mid);
         pixAt(o, 3, 3, corner);
        check(mid[0] > 0.95f && corner[0] < 0.05f, "Select effect range: the colour in the middle is in the mask, the grey around it is not", strfmt("%.2f / %.2f", mid[0], corner[0]));
        check(val(*inst, "EnableLimitHue") == 1 && val(*inst, "LimitHueWidth") < 90, "  the hue limit is on and narrow", strfmt("width %.1f", val(*inst, "LimitHueWidth")));
    }
    {
        // Mesh Warp's grid
        auto inst = instanceOf("de.magix:MeshWarp", nullptr, W, H);
        auto meshText = [](int cols, int rows, int movedAt, float mx, float my) {
            std::string b(16 + (size_t)cols * rows * 8, '\0');
            const int32_t hdr[4] = {cols, rows, cols * rows, 0};
            std::memcpy(&b[0], hdr, 16);
            for (int j = 0; j < rows; ++j)
                for (int i = 0; i < cols; ++i) {
                    float p[2] = {(float)i / (cols - 1), (float)j / (rows - 1)};
                    if (j * cols + i == movedAt) { p[0] = mx; p[1] = my; }
                    std::memcpy(&b[16 + ((size_t)j * cols + i) * 8], p, 8);
                }
            return base64Encode(b.data(), b.size());
        };
        auto meshOf = [](const std::string& text, int& cols, int& rows, std::vector<float>& pts) {
            const std::string b = base64Decode(text);
            if (b.size() < 16) return false;
            int32_t hdr[4];
            std::memcpy(hdr, b.data(), 16);
            cols = hdr[0]; rows = hdr[1];
            if (cols < 2 || rows < 2 || b.size() < 16 + (size_t)cols * rows * 8) return false;
            pts.resize((size_t)cols * rows * 2);
            std::memcpy(pts.data(), b.data() + 16, pts.size() * 4);
            return true;
        };
        inst->userSetString(P(*inst, "WarpData"), meshText(5, 5, 12, 0.62f, 0.5f), 0.0);
        setv(*inst, "GridCountX", {6});
        check(val(*inst, "GridCountY") == 6, "Mesh Warp: with the lock on, Grid size in Y follows X");
        int cols, rows;
        std::vector<float> pts;
        check(meshOf(inst->params.find("WarpData")->str, cols, rows, pts) && cols == 7 && rows == 7, "  and the mesh becomes 7 x 7 points (6 cells)", strfmt("%d x %d", cols, rows));
        // Preserve Mesh Structure is on: the moved middle stays moved (the middle of 7 is the point 24)
        check(cols == 7 && std::fabs(pts[(size_t)24 * 2] - 0.62f) < 0.07f, "  the warp is kept (Preserve Mesh Structure)", strfmt("middle x %.3f", cols == 7 ? pts[48] : -1.f));
        setv(*inst, "KeepMeshStructure", {0});
        setv(*inst, "ScaleGridX", {2});
        check(meshOf(inst->params.find("WarpData")->str, cols, rows, pts) && cols == 13 && rows == 13, "  with multiplier 2 it is 13 x 13", strfmt("%d x %d", cols, rows));
        check(cols == 13 && std::fabs(pts[(size_t)(6 * 13 + 6) * 2] - 0.5f) < 1e-5f, "  and without Preserve Mesh Structure it starts again as a regular grid");
        press(*inst, "ResetMesh", 0);
        check(inst->params.find("WarpData")->str.empty(), "Reset Mesh clears the warp");
        // an empty mesh is left alone by size changes
        setv(*inst, "GridCountX", {3});
        check(inst->params.find("WarpData")->str.empty(), "  a regular mesh stays empty when the grid size changes");
    }
    {
        // the stitcher's camera models
        auto inst = instanceOf("com.magix.ofx.vr.dual.stich", nullptr, W, H);
        setv(*inst, "mx.ofx.360.stitch.fisheye.presets", {3});
        check(val(*inst, "mx.ofx.360.stitch.fisheye.fov") == 191 && val(*inst, "mx.ofx.360.stitch.fisheye.orient.left") == 1 && val(*inst, "mx.ofx.360.stitch.fisheye.orient.right") == 3 &&
                  val(*inst, "mx.ofx.360.stitch.fisheye.falloff.factor") == 20,
              "Dual Fish Eye Stitching: the Model Ricoh Theta V sets its field of view, orientations and vignetting");
        setv(*inst, "mx.ofx.360.stitch.fisheye.presets", {1});
        check(std::fabs(val(*inst, "mx.ofx.360.stitch.fisheye.fov") - 191.35799) < 1e-4 && val(*inst, "mx.ofx.360.stitch.fisheye.orient.left") == 0 &&
                  val(*inst, "mx.ofx.360.stitch.fisheye.falloff.range") == 41.5,
              "  Samsung Gear 360 sets its own");
    }
}

// ------------------------------------------------------------------ drawing, through the render functions
typedef std::map<std::string, std::vector<double>> Params;
static Buf runDef(const Def& def, const Params& over, const Buf& src, const Buf& src2 = Buf(), double progress = 0, double scale = 1.0, double time = 3)
{
    Params store;
    std::map<std::string, std::string> strs;
    for (const PDef& p : def.params) {
        if (p.type == PT::Group || p.type == PT::Button) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : over) store[kv.first] = kv.second;
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
    c.time = time;
    c.fps = 30;
    c.src = src;
    c.src2 = src2;
    c.progress = progress;
    c.out.resize(src.w, src.h);
    c.frameW = src.w;
    c.frameH = src.h;
    c.scale = scale;
    def.render(c);
    return c.out;
}

// A picture whose red is the column, green the row, blue a pattern (so that a shift of the picture can be read off)
static Buf coded(int W, int H)
{
    Buf b;
    b.resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) premul(b.at(x, y), (x + 0.5f) / W, (y + 0.5f) / H, 0.5f + 0.5f * std::sin(x * 0.7f + y * 0.3f), 1.f);
    return b;
}
static double maxDiff(const Buf& a, const Buf& b)
{
    double m = 0;
    for (size_t i = 0; i < a.px.size() && i < b.px.size(); ++i) m = std::max(m, (double)std::fabs(a.px[i] - b.px[i]));
    return m;
}

static void newEffectTests()
{
    std::printf("-- the new effects\n");
    const int W = 256, H = 128;
    const Buf src = coded(W, H);
    {
        const Def* d = findDef("com.magixsoftware:panorotation");
        check(d != nullptr, "Scene Rotation exists");
        if (d) {
            {   // VEGAS's mapping at zero is the picture resampled by 1 / (W - 1) of a pixel (pole rows are the picture's centre point, measured on VEGAS)
                const Buf z = runDef(*d, {}, src);
                double sum = 0;
                for (size_t i = 0; i < z.px.size(); ++i) sum += std::fabs(z.px[i] - src.px[i]);
                check(sum / z.px.size() < 0.03, "Scene Rotation at zero is the picture, resampled a little (the pole rows differ)", strfmt("%.4f", sum / z.px.size()));
            }
            const std::string Y = "mx.ofx.plugin.rotate.y.angle";
            const Buf o = runDef(*d, {{Y, {90}}}, src);
            // the view turned 90 degrees to the right: the middle shows what was a quarter of the way along to the right of it
            check(std::fabs(o.at(W / 2, H / 2)[0] - 0.75f) < 0.02f, "  turning 90 degrees about y shows the picture a quarter of the width along", strfmt("%.3f", o.at(W / 2, H / 2)[0]));
            const Buf o2 = runDef(*d, {{Y, {-90}}}, src);
            check(std::fabs(o2.at(W / 2, H / 2)[0] - 0.25f) < 0.02f, "  and -90 the other way");
            const Buf o3 = runDef(*d, {{Y, {180}}}, src);
            check(std::fabs(o3.at(W / 2, H / 2)[0] - 0.0f) < 0.03f || std::fabs(o3.at(W / 2, H / 2)[0] - 1.0f) < 0.03f, "  and 180 shows the seam of the picture in the middle", strfmt("%.3f", o3.at(W / 2, H / 2)[0]));
            // pitch: looking up 90 shows the pole from the middle... the middle row's green falls towards the top row
            const Buf o4 = runDef(*d, {{"mx.ofx.plugin.rotate.z.angle", {45}}}, src);
            check(std::fabs(std::fabs(o4.at(W / 2, H / 2)[1] - 0.5f) - 0.25f) < 0.08f, "  turning 45 degrees about z (up) shows a point a quarter of the height away", strfmt("%.3f", o4.at(W / 2, H / 2)[1]));
            // stereo side by side: each half turned on its own
            const Buf s = runDef(*d, {{Y, {90}}, {"mx.ofx.plugin.rotate.stereo", {2}}}, src);
            check(std::fabs(s.at(W / 4, H / 2)[0] - (0.5f * 0.75f)) < 0.03f && std::fabs(s.at(3 * W / 4, H / 2)[0] - (0.5f + 0.5f * 0.75f)) < 0.03f, "  Side-by-Side: the halves are turned separately", strfmt("%.3f %.3f", s.at(W / 4, H / 2)[0], s.at(3 * W / 4, H / 2)[0]));
            // multi-resolution
            const Buf half = coded(W / 2, H / 2);
            const Buf oh = runDef(*d, {{Y, {90}}}, half, Buf(), 0, 0.5);
            check(std::fabs(oh.at(W / 4, H / 4)[0] - 0.75f) < 0.03f, "  the same at half size");
        }
    }
    {
        const Def* d = findDef(V + "gradientwipe");
        check(d != nullptr && d->transition, "Gradient Wipe is a transition built-in");
        if (d) {
            Buf a, b;
            a.resize(64, 36); b.resize(64, 36);
            for (int y = 0; y < 36; ++y) for (int x = 0; x < 64; ++x) { premul(a.at(x, y), 1, 0, 0, 1); premul(b.at(x, y), 0, 0, 1, 1); }
            check(maxDiff(runDef(*d, {}, a, b, 0.0), a) == 0, "  at progress 0 the first picture, untouched");
            check(maxDiff(runDef(*d, {}, a, b, 1.0), b) == 0, "  at progress 1 the second");
            const Buf mid = runDef(*d, {}, a, b, 0.5);
            check(mid.at(2, 2)[2] > 0.99f && mid.at(61, 33)[0] > 0.99f, "  half way the upper left has arrived and the lower right has not (a diagonal gradient)");
            const Buf flipped = runDef(*d, {{"HorizontalFlip", {1}}, {"VerticalFlip", {1}}}, a, b, 0.5);
            check(flipped.at(2, 2)[0] > 0.99f && flipped.at(61, 33)[2] > 0.99f, "  with both flips it is the other way round");
            const Buf rev = runDef(*d, {{"Direction", {1}}}, a, b, 0.5);
            check(rev.at(2, 2)[0] > 0.99f && rev.at(61, 33)[2] > 0.99f, "  and Direction Reverse does the same");
            const Buf soft = runDef(*d, {{"ThresholdBlend", {1}}}, a, b, 0.5);
            int between = 0;
            for (int x = 0; x < 64; ++x) { const float v = soft.at(x, 18)[2]; if (v > 0.05f && v < 0.95f) ++between; }
            check(between > 20, "  Threshold blend 1 makes a wide soft edge", std::to_string(between) + " columns in between");
        }
    }
    {
        const Def* d = findDef(V + "displacementmap");
        check(d != nullptr, "Displacement Map exists");
        if (d) {
            check(maxDiff(runDef(*d, {{"ScaleHorizontal", {0}}, {"ScaleVertical", {0}}}, src), src) < 1e-4, "  with no scale the picture is as it is");
            const Buf o = runDef(*d, {}, src);
            check(maxDiff(o, src) > 0.05, "  with a scale it moves pixels");
            for (float v : o.px) if (!std::isfinite(v)) { check(false, "  all values are numbers"); break; }
            const Buf w1 = runDef(*d, {{"EdgePixelHandling", {1}}, {"ScaleHorizontal", {0.6}}}, src);
            const Buf w0 = runDef(*d, {{"EdgePixelHandling", {0}}, {"ScaleHorizontal", {0.6}}}, src);
            int clear0 = 0, clear1 = 0;
            for (int i = 0; i < W * H; ++i) { if (w0.px[(size_t)i * 4 + 3] < 0.5f) ++clear0; if (w1.px[(size_t)i * 4 + 3] < 0.5f) ++clear1; }
            check(clear0 > 0 && clear1 == 0, "  Transparent background leaves holes where pixels came from outside, Wrap pixels around does not", strfmt("%d / %d", clear0, clear1));
        }
    }
    {
        const Def* d = findDef(V + "heightmap");
        check(d != nullptr, "Height Map exists");
        if (d) {
            const Buf o = runDef(*d, {}, src);
            check(maxDiff(o, src) > 0.02, "  it lifts pixels");
            const Buf flat = runDef(*d, {{"HeightModifiersAmplitude", {0}}, {"HeightModifiersElevation", {0}}}, src);
            check(maxDiff(flat, src) < 1e-3, "  with amplitude and elevation 0 the picture is as it is");
        }
    }
    {
        const Def* d = findDef(V + "histogram");
        check(d != nullptr, "Histogram exists");
        if (d) {
            const Buf o = runDef(*d, {}, src);
            check(maxDiff(o, src) > 0.1, "  it draws a graph");
            // outside the graph's box (a third of the width, a quarter of the height, in the middle) nothing changes
            bool same = true;
            for (int x = 0; x < W / 4; ++x) for (int y = 0; y < H; ++y) if (std::memcmp(o.at(x, y), src.at(x, y), 16) != 0) same = false;
            check(same, "  and leaves the rest of the picture alone");
            Buf grey;
            grey.resize(W, H);
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) premul(grey.at(x, y), 0.25f, 0.25f, 0.25f, 1);
            const Buf og = runDef(*d, {{"Transparency", {1}}}, grey);
            // all of the luminance is one value: one tall bar, a quarter of the way along the graph's width
            int whiteCols = 0;
            for (int x = 0; x < W; ++x) { bool hit = false; for (int y = 0; y < H; ++y) if (og.at(x, y)[0] > 0.8f) hit = true; if (hit) ++whiteCols; }
            check(whiteCols > 0 && whiteCols <= 4, "  a picture of one grey is one bar", std::to_string(whiteCols) + " columns");
            const Buf ol = runDef(*d, {{"LogarithmicScale", {1}}, {"Type", {1}}}, src);
            check(maxDiff(ol, o) > 0.05, "  Type and Logarithmic scale change the graph");
        }
    }
    {
        // threads: the same pictures from four at once
        const char* ids[] = {"com.magixsoftware:panorotation", "com.vegascreativesoftware:displacementmap", "com.vegascreativesoftware:heightmap", "com.vegascreativesoftware:histogram"};
        for (const char* id : ids) {
            const Def* d = findDef(id);
            if (!d) continue;
            Params p;
            if (std::string(id) == "com.magixsoftware:panorotation") p["mx.ofx.plugin.rotate.y.angle"] = {33};
            const Buf one = runDef(*d, p, src);
            std::vector<Buf> got(4);
            std::vector<std::thread> th;
            for (int k = 0; k < 4; ++k) th.emplace_back([&, k] { got[(size_t)k] = runDef(*d, p, src); });
            for (auto& t : th) t.join();
            bool ok = true;
            for (int k = 0; k < 4; ++k) ok = ok && maxDiff(got[(size_t)k], one) == 0;
            check(ok, std::string(id) + ": four threads draw what one does");
        }
        const Def* gw = findDef(V + "gradientwipe");
        Buf a = coded(80, 45), b = coded(80, 45);
        for (auto& v : b.px) v = 1.f - v * 0.5f;
        const Buf one = runDef(*gw, {}, a, b, 0.4);
        std::vector<Buf> got(4);
        std::vector<std::thread> th;
        for (int k = 0; k < 4; ++k) th.emplace_back([&, k] { got[(size_t)k] = runDef(*gw, {}, a, b, 0.4); });
        for (auto& t : th) t.join();
        bool ok = true;
        for (int k = 0; k < 4; ++k) ok = ok && maxDiff(got[(size_t)k], one) == 0;
        check(ok, "Gradient Wipe: four threads draw what one does");
    }
}

// ------------------------------------------------------------------ 360 stabilization
static void stabilizationTests()
{
    std::printf("-- 360 Stabilization\n");
    const int W = 256, H = 128;
    // a panorama of blobs; frame t is shifted along by jitter(t) pixels (the camera turns) and drifts slowly
    auto jitter = [](double t) { return 7.0 * std::sin(t * 0.9) + 0.25 * t; };
    const Paint pan = [&](int x, int y, int w, int h, double t, float c[3]) {
        const double s = jitter(t);
        double u = (x - s) / w;
        u -= std::floor(u);
        const double v = (y + 0.5) / h;
        const double a = u * 2 * 3.14159265358979, b = v * 3.14159265358979;
        c[0] = (float)(0.5 + 0.35 * std::sin(a * 3 + b * 2) + 0.15 * std::sin(a * 7 - b * 5));
        c[1] = (float)(0.5 + 0.3 * std::cos(a * 4 + b * 3) * std::sin(b));
        c[2] = (float)(0.5 + 0.3 * std::sin(a * 2 - b * 6) + 0.2 * std::cos(a * 9));
    };
    auto inst = instanceOf("com.magix.ofx.vr.stabilization", nullptr, W, H, 60);
    check(inst != nullptr, "360 Stabilization makes an instance");
    if (!inst) return;
    check(P(*inst, "mx.ofx.stab.plugin.btn.analyze") && P(*inst, "mx.ofx.stab.plugin.btn.clear") && P(*inst, "mx.ofx.stab.plugin.smoothness") &&
              P(*inst, "mx.ofx.stab.plugin.stereo") && P(*inst, "mx.ofx.stab.plugin.exclude.bottom") && P(*inst, "mx.ofx.stab.plugin.ssd.deviation"),
          "  it has VEGAS's settings");
    check(val(*inst, "mx.ofx.stab.plugin.smoothness") == 30, "  smoothing 30 by default");
    connect(*inst, W, H, pan);
    // before the analysis: the picture as it is
    {
        ImagePtr a = renderAt(*inst, 12);
        ImagePtr src = imageOf(W, H, 12, pan);
        check(sad(a, src) < 1e-6, "  before the analysis the picture is as it is");
    }
    press(*inst, "mx.ofx.stab.plugin.btn.analyze", 0);
    const std::string data = inst->params.find("mx.ofx.stab.plugin.motion")->str;
    check(!data.empty(), "  Analyze Motion Data keeps the path in the hidden setting", std::to_string(data.size()) + " characters");
    // how far each frame's picture lies from frame 20's, by search over circular shifts of the middle band
    auto shiftFrom = [&](const ImagePtr& ref, const ImagePtr& img) {
        double best = 1e30;
        int bestS = 0;
        for (int s = -20; s <= 20; ++s) {
            double acc = 0;
            for (int y = H / 4; y < 3 * H / 4; y += 2)
                for (int x = 0; x < W; x += 2) {
                    float p[3], q[3];
                     pixAt(img, ((x + s) % W + W) % W, y, p);
                     pixAt(ref, x, y, q);
                    acc += std::fabs(p[0] - q[0]) + std::fabs(p[1] - q[1]);
                }
            if (acc < best) { best = acc; bestS = s; }
        }
        return bestS;
    };
    ImagePtr inRef = imageOf(W, H, 20, pan), outRef = renderAt(*inst, 20);
    double inVar = 0, outVar = 0, inMean = 0, outMean = 0;
    std::vector<int> inS, outS;
    for (int t = 10; t <= 40; t += 2) {
        inS.push_back(shiftFrom(inRef, imageOf(W, H, t, pan)));
        outS.push_back(shiftFrom(outRef, renderAt(*inst, t)));
    }
    for (int s : inS) inMean += s;
    for (int s : outS) outMean += s;
    inMean /= inS.size(); outMean /= outS.size();
    for (int s : inS) inVar += (s - inMean) * (s - inMean);
    for (int s : outS) outVar += (s - outMean) * (s - outMean);
    inVar = std::sqrt(inVar / inS.size()); outVar = std::sqrt(outVar / outS.size());
    check(inVar > 3 && outVar < inVar * 0.5, "  the jitter of the frames (frame 10 to 40) is at least halved by it", strfmt("spread %.2f px before, %.2f after", inVar, outVar));
    // no smoothing: the path is followed exactly - the picture does not move
    setv(*inst, "mx.ofx.stab.plugin.smoothness", {0});
    ImagePtr z = renderAt(*inst, 30);
    check(sad(z, imageOf(W, H, 30, pan)) < 1e-4, "  Smoothing 0 leaves the picture alone");
    // Clear Motion Data
    press(*inst, "mx.ofx.stab.plugin.btn.clear", 0);
    check(inst->params.find("mx.ofx.stab.plugin.motion")->str.empty(), "  Clear Motion Data discards it");
    // the data survives a project save
    press(*inst, "mx.ofx.stab.plugin.btn.analyze", 0);
    const Json st = inst->saveState();
    auto again = instanceOf("com.magix.ofx.vr.stabilization", nullptr, W, H, 60);
    connect(*again, W, H, pan);
    again->loadState(st);
    setv(*again, "mx.ofx.stab.plugin.smoothness", {30});
    check(again->params.find("mx.ofx.stab.plugin.motion")->str == inst->params.find("mx.ofx.stab.plugin.motion")->str, "  the path is in the saved state and comes back");
    setv(*inst, "mx.ofx.stab.plugin.smoothness", {30});
    check(sad(renderAt(*again, 17), renderAt(*inst, 17)) < 1e-6, "  and draws the same picture");
}

// ------------------------------------------------------------------ the pictures of the changed effects
static void effectTests()
{
    std::printf("-- the changed effects\n");
    const int W = 200, H = 120;
    const Buf src = coded(W, H);
    {
        // Bump Map: where the light is (LocationSrc) matters for the Spotlight type
        const Def* d = findDef(V + "bumpmapfilter");
        const Buf a = runDef(*d, {{"Type", {1}}, {"BumpHeight", {0.5}}}, src);
        const Buf def = runDef(*d, {{"Type", {1}}, {"BumpHeight", {0.5}}, {"LocationSrc", {-1, 1, 3}}}, src);
        const Buf moved = runDef(*d, {{"Type", {1}}, {"BumpHeight", {0.5}}, {"LocationSrc", {0.8, -0.6, 0.7}}}, src);
        check(maxDiff(a, def) == 0, "Bump Map: the default light place is VEGAS's (-1, 1, 3)");
        check(maxDiff(a, moved) > 0.02, "  moving the light changes the picture");
        const Buf high = runDef(*d, {{"Type", {1}}, {"BumpHeight", {0.5}}, {"LocationSrc", {-1, 1, 6.0}}}, src);
        check(maxDiff(a, high) > 0.02, "  and so does raising it");
    }
    {
        const Def* d = findDef(V + "layerdimensionality");
        Buf round;
        round.resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double r = std::hypot(x - W / 2.0, y - H / 2.0);
                premul(round.at(x, y), 0.6f, 0.6f, 0.6f, (float)clampd((H * 0.35 - r) / 4.0, 0, 1));
            }
        const Params emboss = {{"ForegroundAmount", {1}}, {"EmbossRoundness", {0.5}}};
        const Buf a = runDef(*d, emboss, round);
        Params same = emboss;
        same["LightLocation"] = {-3, 3, 3};
        check(maxDiff(a, runDef(*d, same, round)) == 0, "Layer Dimensionality: the default light is the old one");
        Params other = emboss;
        other["LightLocation"] = {3, -3, 3};
        const Buf b = runDef(*d, other, round);
        // lit from the lower right the lit and shaded edges swap: the upper left of the disc's edge is darker than with the default light
        const int x = (int)(W / 2.0 - H * 0.35 * 0.707), y = (int)(H / 2.0 - H * 0.35 * 0.707);
        check(std::fabs(a.at(x, y)[0] - b.at(x, y)[0]) > 0.02f, "  light from the lower right lights the other edge", strfmt("%.3f vs %.3f", a.at(x, y)[0], b.at(x, y)[0]));
        Params low = emboss;
        low["LightLocation"] = {-3, 3, 1};
        check(maxDiff(a, runDef(*d, low, round)) > 0.01, "  and the light's height changes the strength");
    }
    {
        // Smart Upscale with three scale numbers: the mean of X and Y counts
        const Def* d = findDef("com.sonycreativesoftware:smart_upscale");
        const Buf a = runDef(*d, {{"Scale", {2, 2, 2}}}, src), b = runDef(*d, {{"Scale", {3, 1, 1}}}, src);
        check(maxDiff(a, b) == 0, "Smart Upscale: Scale (3, 1, 1) is as strong as (2, 2, 2): the mean of X and Y");
        check(maxDiff(a, runDef(*d, {{"Scale", {6, 6, 6}}}, src)) > 0.001, "  a bigger scale sharpens more");
    }
    {
        // Dual fisheye: two discs side by side
        const Def* d = findDef("com.magix.ofx.vr.dual.stich");
        Buf fish;
        fish.resize(400, 200);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 400; ++x) {
                const bool left = x < 200;
                const double cx = left ? 100 : 300, r = std::hypot(x - cx, y - 100.0);
                if (r < 98) premul(fish.at(x, y), left ? 0.9f : 0.1f, (float)(0.2 + 0.6 * (x % 40) / 40.0), left ? 0.1f : 0.9f, 1.f);
                else premul(fish.at(x, y), 0, 0, 0, 1);
            }
        const std::string pre = "mx.ofx.360.stitch.fisheye.";
        const Buf a = runDef(*d, {}, fish);
        float mean = 0;
        for (size_t i = 0; i < a.px.size(); i += 4) mean += a.px[i] + a.px[i + 1] + a.px[i + 2];
        check(mean / (a.px.size() / 4) > 0.3f, "Dual Fish Eye Stitching: draws a picture from two discs");
        const Buf turned = runDef(*d, {{pre + "orient.left", {2}}, {pre + "orient.right", {2}}}, fish);
        check(maxDiff(a, turned) > 0.02, "  the orientation of the lenses changes it");
        check(maxDiff(a, runDef(*d, {{pre + "rotation", {30}}}, fish)) > 0.02, "  so does the rotation between them");
        check(maxDiff(a, runDef(*d, {{pre + "falloff.use", {0}}}, fish)) > 0.001, "  and the vignetting setting");
        check(maxDiff(a, runDef(*d, {{pre + "narrowing", {80}}}, fish)) > 0.02, "  and the radius");
        check(maxDiff(a, runDef(*d, {{pre + "center", {5}}}, fish)) > 0.01, "  and the centre offset");
        check(maxDiff(a, runDef(*d, {{pre + "fov", {200}}}, fish)) > 0.02, "  and the field of view");
    }
}

int main()
{
    ofx::Host::get();
    settingsTests();
    buttonTests();
    newEffectTests();
    stabilizationTests();
    effectTests();
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
