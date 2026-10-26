// vegmanaged2_test: the CoreGraphics effects the .veg samples do not contain (Starburst, Fill Light, Defocus, Soft
// Contrast) as records. Their XML is the preset XML VEGAS keeps in ScriptPortal.MediaSoftware.TextGen.CoreGraphics.dll
// (copied here); the OpenFX values are the same presets as VEGAS ships for the OpenFX plug-ins of the same name
// (Vfx1.ofx.bundle, and Reno's copy in src/ofx/builtins_vfxdata.cpp), so the scales are checked against two files that
// hold the same numbers in two shapes. Written back unchanged the XML is the same text; with a value changed, only that
// element differs and it reads back changed.
#include "../../src/vegas/vegmanaged.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::veg;

static int g_fail = 0, g_pass = 0;
static bool check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
    return ok;
}

static const char* const HEAD = "<?xml version=\"1.0\"?>\r\n";
static const char* const NS = " xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"";

static std::string starburst()
{
    return std::string(HEAD) + "<Starburst" + NS + ">\r\n  <Enabled>true</Enabled>\r\n  <ExpandBoundingBox>false</ExpandBoundingBox>\r\n"
        "  <DefaultUnitType>Percent</DefaultUnitType>\r\n  <Threshold>0.72508474576271187</Threshold>\r\n"
        "  <Brilliance>17.961864406779661</Brilliance>\r\n  <Strength>1</Strength>\r\n  <Radius>0.16627118644067795</Radius>\r\n"
        "  <Points>4</Points>\r\n  <CenterBurst>0.8728813559322034</CenterBurst>\r\n  <Aspect>1</Aspect>\r\n"
        "  <Orientation>0.125</Orientation>\r\n  <ReduceFlicker>false</ReduceFlicker>\r\n  <EffectOnly>false</EffectOnly>\r\n"
        "  <ShowKernel>false</ShowKernel>\r\n</Starburst>";
}
static std::string fillLight()
{
    return std::string(HEAD) + "<FillLight" + NS + ">\r\n  <Enabled>true</Enabled>\r\n  <ExpandBoundingBox>true</ExpandBoundingBox>\r\n"
        "  <DefaultUnitType>Percent</DefaultUnitType>\r\n  <Strength>0.75861243400805711</Strength>\r\n  <Bias>-0.66310068372814435</Bias>\r\n"
        "  <Softness>0.0067796610169491532</Softness>\r\n  <Correction>1</Correction>\r\n  <Hue>0.092834095721940207</Hue>\r\n"
        "  <MaskOnly>false</MaskOnly>\r\n</FillLight>";
}
static std::string defocus()
{
    return std::string(HEAD) + "<Defocus" + NS + ">\r\n  <Enabled>true</Enabled>\r\n  <ExpandBoundingBox>false</ExpandBoundingBox>\r\n"
        "  <DefaultUnitType>Percent</DefaultUnitType>\r\n  <ApertureRadius>0.056016949152542378</ApertureRadius>\r\n  <Iris>Hexagon</Iris>\r\n"
        "  <Orientation>0</Orientation>\r\n  <Curvature>-1</Curvature>\r\n  <BloomThreshold>0.94974576271186428</BloomThreshold>\r\n"
        "  <BloomStrength>10.512711864406779</BloomStrength>\r\n  <ReduceFlicker>false</ReduceFlicker>\r\n  <TileOverlay>false</TileOverlay>\r\n"
        "  <KernelOverlay>false</KernelOverlay>\r\n</Defocus>";
}
static std::string softContrast()
{
    return std::string(HEAD) + "<Softlight" + NS + ">\r\n  <Enabled>true</Enabled>\r\n  <ExpandBoundingBox>false</ExpandBoundingBox>\r\n"
        "  <DefaultUnitType>Percent</DefaultUnitType>\r\n  <Stretch>0.17796610169491522</Stretch>\r\n  <Strength>0.6652542372881356</Strength>\r\n"
        "  <Diffusion>0.076271186440677971</Diffusion>\r\n  <LowTrim>0.055084745762711863</LowTrim>\r\n  <HighTrim>0</HighTrim>\r\n"
        "  <Saturation>0.326271186440678</Saturation>\r\n  <Hue>0.11864406779661017</Hue>\r\n"
        "  <VignetteEffect xsi:type=\"Vignette\">\r\n    <Enabled>true</Enabled>\r\n    <maskType>Rectangle</maskType>\r\n"
        "    <ExteriorEffectType>Black</ExteriorEffectType>\r\n    <Strength>1</Strength>\r\n    <MaskType>Rectangle</MaskType>\r\n"
        "    <VignetteMask xsi:type=\"RectangleMask\">\r\n      <Enabled>true</Enabled>\r\n      <Blur>0.5</Blur>\r\n"
        "      <Width>0.849792531120332</Width>\r\n      <Height>0.8</Height>\r\n      <CornerRadius>0.15</CornerRadius>\r\n"
        "      <XPosition>0.5</XPosition>\r\n      <YPos>0.5</YPos>\r\n      <Inverted>true</Inverted>\r\n    </VignetteMask>\r\n"
        "  </VignetteEffect>\r\n</Softlight>";
}

// A Glint whose mask is an ellipse: EllipseMask's own properties (Blur, XPosition / YPos from the top), not a wipe's.
static std::string glintEllipse()
{
    return std::string(HEAD) + "<Glint" + NS + ">\r\n  <Enabled>true</Enabled>\r\n  <Threshold>0.8195</Threshold>\r\n  <Boost>0.888</Boost>\r\n"
        "  <HorizontalRadius>0.5774</HorizontalRadius>\r\n  <VerticalRadius>0.7388</VerticalRadius>\r\n  <Hue>98</Hue>\r\n  <HueSweep>17.7</HueSweep>\r\n"
        "  <Saturation>0.6</Saturation>\r\n  <Rotation>0</Rotation>\r\n  <Streaks>3</Streaks>\r\n  <ReduceFlicker>false</ReduceFlicker>\r\n"
        "  <EffectOnly>false</EffectOnly>\r\n  <Mask>\r\n    <Mode>Effect</Mode>\r\n    <Type>Ellipse</Type>\r\n    <Mask xsi:type=\"EllipseMask\">\r\n"
        "      <Enabled>true</Enabled>\r\n      <AlphaMask>false</AlphaMask>\r\n      <Blur>0.2</Blur>\r\n      <Width>0.5</Width>\r\n"
        "      <Height>0.4</Height>\r\n      <XPosition>0.3</XPosition>\r\n      <YPos>0.25</YPos>\r\n      <Inverted>false</Inverted>\r\n"
        "    </Mask>\r\n  </Mask>\r\n</Glint>";
}

static ManagedFxRecord record(const std::string& root, const std::string& xml)
{
    ManagedFxRecord r;
    r.classGuid = "00000000-0000-0000-0000-000000000000";
    r.root = root;
    r.preset = "test";
    r.hasStatic = false;
    ManagedFxKey k;
    k.time = 0;
    k.extra = std::string(12, '\0');
    k.xml = xml;
    r.keys.push_back(k);
    return r;
}

static double val(const Json& state, const char* name, size_t i = 0) { return state.get("params").get(name).get("v").at(i).asDouble(); }

static void one(const char* name, const std::string& root, const std::string& xml, const std::string& plugin,
                const std::vector<std::pair<const char*, double>>& expect, const char* changeParam, double changeTo, const char* changedElement)
{
    ManagedFxRecord r = record(root, xml);
    check(managedPluginId(r) == plugin, std::string(name) + ": maps to " + plugin);
    Json st;
    bool bypass = true;
    std::vector<std::string> problems;
    const bool ok = managedFxState(r, 25.0, st, bypass, problems);
    check(ok && !bypass && problems.empty(), std::string(name) + ": the record reads");
    if (!ok) return;
    for (const auto& e : expect) {
        const double v = val(st, e.first);
        const bool good = std::fabs(v - e.second) < 0.006 * std::max(1.0, std::fabs(e.second)) * 0.5 + 0.006;
        check(good, std::string(name) + ": " + e.first + " = " + std::to_string(v) + " (VEGAS's preset " + std::to_string(e.second) + ")");
    }
    // written back unchanged: the same text
    ManagedFxRecord same;
    std::vector<std::string> warn;
    check(managedFxWith(r, st, false, 25.0, same, warn) && same.keys.size() == 1 && same.keys[0].xml == xml, std::string(name) + ": unchanged settings are written as the same XML");
    // one value changed
    Json st2 = st;
    Json p = st2.get("params").get(changeParam);
    Json v = Json::array();
    v.push(changeTo);
    Json params = st2.get("params");
    Json e = Json::object();
    e["v"] = v;
    if (p.get("i").asBool()) e["i"] = true;
    params[changeParam] = e;
    st2["params"] = params;
    ManagedFxRecord out;
    warn.clear();
    check(managedFxWith(r, st2, false, 25.0, out, warn) && out.keys.size() == 1, std::string(name) + ": a changed value is written");
    if (out.keys.size() == 1) {
        check(out.keys[0].xml != xml && out.keys[0].xml.find(changedElement) != std::string::npos, std::string(name) + ": only " + changedElement + " changed");
        Json st3;
        bool b3 = false;
        std::vector<std::string> pr;
        check(managedFxState(out, 25.0, st3, b3, pr) && std::fabs(val(st3, changeParam) - changeTo) < 1e-6, std::string(name) + ": the change reads back");
        // everything else is the same text: compare by cutting out the changed element
        const std::string tag = std::string("<") + changedElement + ">";
        const size_t a = xml.find(tag), b = out.keys[0].xml.find(tag);
        const size_t ae = xml.find("</", a), be = out.keys[0].xml.find("</", b);
        check(a != std::string::npos && b != std::string::npos && xml.substr(0, a) == out.keys[0].xml.substr(0, b) && xml.substr(ae) == out.keys[0].xml.substr(be),
              std::string(name) + ": the rest of the XML is untouched");
    }
}

int main()
{
    one("Starburst (Bright Center)", "Starburst", starburst(), "com.vegascreativesoftware:starburstvelvetmatter",
        {{"Threshold", 72.51}, {"Brilliance", 1796.1}, {"Strength", 100}, {"Radius", 16.63}, {"Points", 4}, {"CenterBurst", 87.29}, {"Aspect", 1}, {"Orientation", 45}},
        "Orientation", 90, "Orientation");
    one("Fill Light (Strong, Warm)", "FillLight", fillLight(), "com.vegascreativesoftware:filllightvelvetmatter",
        {{"Hue", 33.42}, {"Strength", 75.86}, {"Bias", -66.31}, {"Tint", 100}, {"Softness", 3.39}}, "Strength", 40, "Strength");
    one("Defocus (Hexagon)", "Defocus", defocus(), "com.vegascreativesoftware:defocusvelvetmatter",
        {{"Radius", 5.6}, {"IrisShape", 2}, {"Orientation", 0}, {"DiscCurvature", -100}, {"BloomThreshold", 94.97}, {"BloomStrength", 10.51}},
        "Radius", 7.0, "ApertureRadius");
    one("Soft Contrast (mixed presets)", "Softlight", softContrast(), "com.vegascreativesoftware:softcontrastvelvetmatter",
        {{"EffectStretchRange", 17.8}, {"EffectContrast", 66.53}, {"EffectDiffusion", 15.25}, {"EffectLowTrim", 5.51}, {"EffectTint", 32.63}, {"EffectCorrectionHue", 42.71},
         {"VignetteExteriorEffect", 2}, {"VignetteStrength", 100}, {"VignetteShape", 0}, {"VignetteSoftness", 50}, {"VignetteWidth", 84.98}, {"VignetteHeight", 80},
         {"VignetteCornerRadius", 15}},
        "VignetteWidth", 60, "Width");
    one("Glint (ellipse mask)", "Glint", glintEllipse(), "com.vegascreativesoftware:glintvelvetmatter",
        {{"MaskMaskMode", 1}, {"MaskMaskType", 3}, {"MaskSoftness", 0.2}, {"MaskWidth", 50}, {"MaskHeight", 40}}, "MaskSoftness", 0.35, "Blur");
    {
        // the position: XPosition as it is, YPos counted down from the top (OpenFX's y counts up)
        ManagedFxRecord r = record("Glint", glintEllipse());
        Json st;
        bool bypass = false;
        std::vector<std::string> problems;
        check(managedFxState(r, 25.0, st, bypass, problems) && std::fabs(val(st, "MaskPosition", 0) - 0.3) < 1e-9 && std::fabs(val(st, "MaskPosition", 1) - 0.75) < 1e-9,
              "Glint (ellipse mask): the position reads from XPosition / YPos");
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
