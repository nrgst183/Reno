// More of VEGAS Pro's stock video effects as built-in stand-ins (see builtins.h): blurs, colour, and the creative and light effects the VEGAS Pro 23 manual describes.
// Identifiers, parameter names, labels, defaults and presets are VEGAS Pro 18's (builtins_vfxdata.cpp, made by tools/gen_vfxdata.py from VEGAS's resource and preset files);
// the frames approximate VEGAS's.
//
// This file turns the spec text into definitions and registers the effects. The effects are in builtins_more_blur.cpp, _colour.cpp, _creative.cpp and _light.cpp,
// with shared helpers in builtins_more_internal.h.
#include "builtins_vfxdata.h"
#include "builtins_more_internal.h"
#include "builtins_vec.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {

// ------------------------------------------------------------------ the spec text into a definition
static std::vector<std::string> splitOn(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

static std::vector<double> numbersOf(const std::string& s)
{
    std::vector<double> v;
    std::istringstream in(s);
    double x;
    while (in >> x) v.push_back(x);
    return v;
}

Def vfxDef(const std::string& id, void (*fn)(Ctx&))
{
    Def d;
    const VfxSpec* spec = nullptr;
    for (int k = 0; k < kVfxSpecCount; ++k)
        if (id == kVfxSpecs[k].id) { spec = &kVfxSpecs[k]; break; }
    if (!spec) return d;
    d.id = spec->id;
    d.label = spec->label;
    d.group = "Reno Built-in";
    d.transition = std::strcmp(spec->context, "Transition") == 0;
    d.description = std::string("Reno's own, standing in for VEGAS ") + spec->label + ".";
    d.render = fn;
    if (!spec->params[0]) return d;
    for (const std::string& item : splitOn(spec->params, ';')) {
        const std::vector<std::string> f = splitOn(item, '|');
        if (f.size() < 4) continue;
        const std::string& kind = f[0];
        const std::string &name = f[1], &label = f[2];
        if (kind == "D" && f.size() >= 6) d.params.push_back(dbl(name, label, atof(f[3].c_str()), atof(f[4].c_str()), atof(f[5].c_str())));
        else if (kind == "I" && f.size() >= 6) d.params.push_back(intp(name, label, atoi(f[3].c_str()), atoi(f[4].c_str()), atoi(f[5].c_str())));
        else if (kind == "B") d.params.push_back(boolean(name, label, atoi(f[3].c_str()) != 0));
        else if (kind == "C" && f.size() >= 5) d.params.push_back(choice(name, label, atoi(f[3].c_str()), splitOn(f[4], ',')));
        else if (kind == "K") {
            const auto n = numbersOf(f[3]);
            if (n.size() >= 4) d.params.push_back(rgba(name, label, n[0], n[1], n[2], n[3]));
        } else if (kind == "R") {
            const auto n = numbersOf(f[3]);
            if (n.size() >= 3) d.params.push_back(rgb(name, label, n[0], n[1], n[2]));
        } else if (kind == "P") {
            const auto n = numbersOf(f[3]);
            if (n.size() >= 2) d.params.push_back(point(name, label, n[0], n[1]));
        } else if (kind == "S") d.params.push_back(text(name, label, f.size() > 3 ? f[3] : std::string(), false));
    }
    return d;
}

void vfxTune(Def& d, const char* name, double def, double lo, double hi)
{
    for (PDef& p : d.params) {
        if (p.name != name) continue;
        p.def = {def};
        if (!std::isnan(lo)) { p.min = lo; p.dmin = lo; }
        if (!std::isnan(hi)) { p.max = hi; p.dmax = hi; }
    }
}

using namespace more;

// Slow Motion (VEGAS Pro 23): the frame is not changed here; the engine reads the settings and plays the event's media at the Playback Rate, making in-between frames
// by the Quality and Blending (see eventFxRate in project.cpp).
static void slowMotion(Ctx& c) { copyThrough(c); }

// Tiny Planet (360 Effects): the frame taken as an equirectangular panorama and seen from above its floor, so the floor is in the middle of a ball with the sky round it.
// Latitude tilts the ball (90 = from above, 180 = from below), Longitude turns it, Z is how far away it is.
void tinyPlanet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double tilt = (c.d("Latitude") - 90.0) * kPi / 180.0, lon = c.d("Longitude") * kPi / 180.0, zoom = std::max(0.05, c.d("Z"));
    const double ct = std::cos(tilt), st = std::sin(tilt);
    const double R = 0.5 * c.frameH * zoom;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double px = (x + 0.5 - W * 0.5) / R, py = (y + 0.5 - H * 0.5) / R;       // y runs down
            const double r2 = px * px + py * py, k = 1.0 / (1.0 + r2);
            const double X = 2 * px * k, Z = 2 * py * k, Y = (r2 - 1.0) * k;
            const double Y2 = Y * ct - Z * st, Z2 = Y * st + Z * ct;
            const double lonS = std::atan2(X, Z2) + lon, latS = std::asin(clampd(Y2, -1, 1));
            double u = 0.5 + lonS / (2 * kPi);
            u -= std::floor(u);
            const double v = 0.5 - latS / kPi;
            float p[4];
            c.src.sample(u * W, v * H, p);
            std::memcpy(c.out.at(x, y), p, sizeof(p));
        }
    });
}

// Puts a setting into a definition before the one called 'before' (at the end when there is none).
static void insertParam(Def& d, const char* before, const PDef& p)
{
    size_t at = d.params.size();
    for (size_t k = 0; k < d.params.size(); ++k)
        if (d.params[k].name == before) { at = k; break; }
    d.params.insert(d.params.begin() + (std::ptrdiff_t)at, p);
}

void addFiltersMore(std::vector<Def>& out)
{
    struct Row { const char* id; void (*fn)(Ctx&); };
    const std::string P = "com.vegascreativesoftware:";
    const Row rows[] = {
        {"quickblur", quickBlur}, {"radialblur", radialBlur}, {"defocusvelvetmatter", defocus}, {"unsharpmask", unsharpMask}, {"median", median},
        {"minandmax", minAndMax}, {"chromablur", chromaBlur}, {"saturationadjust", saturationAdjust}, {"softcontrastvelvetmatter", softContrast},
        {"whitebalance", whiteBalance}, {"labadjust", labAdjust}, {"blackrestore", blackRestore}, {"broadcastcolors", broadcastColors},
        {"channelblend", channelBlend}, {"blackbarfill", blackBarFill}, {"crop", cropFx}, {"pictureinpicture", pictureInPicture}, {"filmgrain", filmGrain},
        {"filmeffects", filmEffects}, {"vignette", vignette}, {"tvsimulator", tvSimulator}, {"radialpixelate", radialPixelate}, {"maskgenerator", maskGenerator},
        {"timecode", timecodeFx}, {"layerdimensionality", layerDimensionality}, {"deform", deform}, {"bumpmapfilter", bumpMap}, {"lensflare", lensFlare},
        {"lightrays", lightRays}, {"raysvelvetmatter", raysVelvet}, {"glintvelvetmatter", glintVelvet}, {"starburstvelvetmatter", starburstVelvet},
        {"filllightvelvetmatter", fillLightVelvet}, {"gradientmap", gradientMap},
    };
    for (const Row& r : rows) {
        Def d = vfxDef(P + r.id, r.fn);
        if (d.id.empty()) continue;
        // defaults of the neutral kind where VEGAS's first preset is a strong look
        if (std::string(r.id) == "labadjust") {
            vfxTune(d, "Lightness", 1.0, 0, 2); vfxTune(d, "Channela", 0, -1, 1); vfxTune(d, "Channelb", 0, -1, 1);
        }
        if (std::string(r.id) == "channelblend") {
            const char* grp[4] = {"RED", "GREEN", "BLUE", "ALPHA"};
            const char* prt[5] = {" weight R", " weight G", " weight B", " weight A", " weight Offset"};
            for (int g = 0; g < 4; ++g)
                for (int k = 0; k < 5; ++k) vfxTune(d, (std::string(grp[g]) + prt[k]).c_str(), g == k ? 1.0 : 0.0, -10, 10);
            vfxTune(d, "Normalize", 0, NAN, NAN);
        }
        if (std::string(r.id) == "whitebalance") {
            vfxTune(d, "ColorTemperature", 6500, 1700, 15000); vfxTune(d, "ColorTint", 0, -100, 100); vfxTune(d, "Brightness", 1, 0.1, 2);
            // VEGAS's three buttons (its resource file lists them with the settings): the order of its file
            insertParam(d, "AmountOfCorrection", button("SelectWhiteColor", "Select neutral color"));
            d.params.push_back(button("EnhanceHistogram", "Equalize Histogram"));
            d.params.push_back(button("ClearData", "Undo Equalization"));
            d.params.push_back(custom("EqualizeData", "Equalize data", ""));
            for (auto& p : d.params) {
                if (p.name == "ColorTemperature") p.hint = "Temperature of selected neutral color.";
                if (p.name == "ColorTint") p.hint = "Tint of selected neutral color.";
            }
            d.changed = whiteBalanceChanged;
        }
        if (std::string(r.id) == "bumpmapfilter") {
            // VEGAS's plug-in's own defaults (its describe code): a directional light from the upper left (its resource file lists
            // the "Spotlight Up (flat)" preset's values)
            insertParam(d, "LocationDst", point3("LocationSrc", "Source", -1, 1, 3, -10, 10));
            for (auto& p : d.params) {
                if (p.name == "LocationSrc") { p.dmin = -3; p.dmax = 6; }
                if (p.name == "LocationDst") { p.normalised = false; p.min = -10; p.max = 10; p.dmin = -3; p.dmax = 3; }
            }
            vfxTune(d, "Type", 0, NAN, NAN); vfxTune(d, "Intensity", 0.25, NAN, NAN); vfxTune(d, "Focus", 0.5, NAN, NAN);
            vfxTune(d, "Ambience", 0.1, NAN, NAN); vfxTune(d, "Shininess", 0.5, NAN, NAN); vfxTune(d, "BumpHeight", 0.5, NAN, NAN);
        }
        if (std::string(r.id) == "layerdimensionality") {
            d.params.push_back(point3("LightLocation", "Light Location", -3, 3, 3, -6, 6));
            d.params.back().dmin = -3; d.params.back().dmax = 6;
            d.general = true;
        }
        if (std::string(r.id) == "broadcastcolors") {
            // (the real ranges and the starting values that limit nothing)
            vfxTune(d, "LumaMin", 0, -8, 40); vfxTune(d, "LumaMax", 110, 60, 110); vfxTune(d, "ChromaMax", 148, 0, 148);
            vfxTune(d, "CompositeMin", -47, -47, -8); vfxTune(d, "CompositeMax", 148, 100, 148);
            vfxTune(d, "StudioRGB16To135", 1, NAN, NAN);
        }
        if (std::string(r.id) == "timecode") {
            vfxTune(d, "Width", 1, 0.5, 2); vfxTune(d, "Height", 1, 0.5, 2);
            // (the spec text cannot hold commas: VEGAS's names of the SMPTE formats have them)
            for (auto& p : d.params)
                if (p.name == "TimecodeFormat")
                    p.choices = {"Time", "Seconds", "Time & Frames", "Absolute Frames", "Feet and Frames 16mm (40fpf)", "Feet and Frames 35mm (16fpf)",
                                 "SMPTE Film Sync IVTC (23.976 fps, Video)", "SMPTE Film Sync (24 fps, Film)", "SMPTE EBU (25 fps, Video)",
                                 "SMPTE Non-Drop (29.97 fps, Video)", "SMPTE Drop (29.97 fps, Video)", "SMPTE 30 (30 fps, Audio)"};
        }
        if (std::string(r.id) == "lensflare") {
            // VEGAS's own: nothing drawn until Intensity is raised (0 to 2); the light position runs -1 to 1 from the middle of the frame
            vfxTune(d, "Intensity", 0, 0, 2); vfxTune(d, "Perspective", 0, -1, 1);
            for (PDef& p : d.params)
                if (p.name == "LightPosition") { p.normalised = false; p.min = -1; p.max = 1; p.dmin = -1; p.dmax = 1; }
        }
        if (std::string(r.id) == "radialblur") vfxTune(d, "Type", 0, NAN, NAN);            // (VEGAS's own default: Proportional)
        if (std::string(r.id) == "saturationadjust") vfxTune(d, "Amount", 0, -1, 1);
        if (std::string(r.id) == "saturationadjust") { vfxTune(d, "Center", 0.5, 0, 1); vfxTune(d, "Spread", 0.5, 0.01, 1); }
        // (VEGAS's Unsharp Mask starts with Amount and Radius at 0 - its pictures with only one setting changed are the source)
        // (VEGAS's Median starts with ranges of 0.1: its default picture is that)
        if (std::string(r.id) == "median") { vfxTune(d, "HorizontalRange", 0.1, NAN, NAN); vfxTune(d, "VerticalRange", 0.1, NAN, NAN); }
        if (std::string(r.id) == "unsharpmask") { vfxTune(d, "Amount", 0, -7, 8); vfxTune(d, "Radius", 0, NAN, NAN); }
        if (std::string(r.id) == "vignette") {
            vfxTune(d, "Rotation", 0, 0, 180); vfxTune(d, "Outer", 1, 0, 3); vfxTune(d, "Aspect", 1, 0.25, 4); vfxTune(d, "Feather", 5, 0, 10);
        }
        // VEGAS's Defocus really starts at the middle of each range (its pictures show that), not at the XML defaults
        if (std::string(r.id) == "defocusvelvetmatter") { vfxTune(d, "Radius", 5.6, 0, 11.2); vfxTune(d, "BloomThreshold", 94.97, 0, 189.94); vfxTune(d, "BloomStrength", 10.51, 0, 21.02); }
        if (std::string(r.id) == "crop") vfxTune(d, "Angle", 0, -360, 360);
        if (std::string(r.id) == "pictureinpicture") { vfxTune(d, "Angle", 0, -1800, 1800); vfxTune(d, "Scale", 0.5, 0.1, 10); vfxTune(d, "DistortionScaleY", 0.5, 0.1, 10); }
        if (std::string(r.id) == "starburstvelvetmatter") vfxTune(d, "Orientation", 45, 0, 360);
        if (std::string(r.id) == "glintvelvetmatter") vfxTune(d, "EffectOrientation", 0, 0, 360);
        // VEGAS's managed Rays starts from its "Luminance" preset, whose light sits at (0.86, 0.23 from the top); the OpenFX preset file Reno's data comes from has 0.5 0.5 there (see builtinPresetPackage)
        if (std::string(r.id) == "raysvelvetmatter")
            for (PDef& p : d.params)
                if (p.name == "EffectPosition") p.def = {0.85992826995360627, 1.0 - 0.22881355932203384};
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:tinyplanet";
        d.label = "Tiny Planet";
        d.group = "Reno Built-in";
        d.description = "360 effect: a panorama wrapped into a little planet seen from above.";
        d.render = tinyPlanet;
        d.params.push_back(dbl("Latitude", "Latitude", 90, 0, 180));
        d.params.push_back(dbl("Longitude", "Longitude", 0, -180, 180));
        d.params.push_back(dbl("Z", "Z", 1.0, 0.1, 4.0));
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:slowmotion";
        d.label = "Slow Motion";
        d.group = "Reno Built-in";
        d.description = "Plays the event slower (or faster) with new in-between frames made by optical flow or blending.";
        d.render = slowMotion;
        d.params.push_back(choice("Quality", "Quality", 1, {"Low", "Medium", "High"}));
        d.params.push_back(choice("Blending", "Blending", 0, {"Blend", "Nearest Frame"}));
        d.params.push_back(dbl("PlaybackRate", "Playback Rate", 0.5, 0.01, 4.0));
        d.params.push_back(boolean("ShowOpticalFlowField", "Show Optical Flow Field", false));
        out.push_back(d);
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
