// The presets Reno's built-in plug-ins come with (shown as thumbnails in Media Generators / Video FX and in the effects window's Preset list), kept as an OfxPresetPackage like VEGAS's
// bundles; they follow the kind of presets VEGAS Pro 18's effects have.
#include "builtins.h"
#include "builtins_vfxdata.h"
#include "builtins_magixdata.h"
#include "builtins_colordata.h"
#include <sstream>

namespace reno {
namespace ofx {
namespace {

struct V { const char* type; std::string name, value; };
struct P { const char* plugin; const char* context; const char* name; std::vector<V> values; };

const char* const D = "OfxParamTypeDouble";
const char* const B = "OfxParamTypeBoolean";
const char* const C = "OfxParamTypeChoice";
const char* const RGBA = "OfxParamTypeRGBA";
const char* const RGB = "OfxParamTypeRGB";
const char* const D2 = "OfxParamTypeDouble2D";
const char* const S = "OfxParamTypeString";
const char* const I = "OfxParamTypeInteger";

std::vector<P> table()
{
    const char* sc = "com.vegascreativesoftware:solidcolor";
    const char* cb = "com.vegascreativesoftware:checkerboard";
    const char* nt = "com.vegascreativesoftware:noisetexture";
    const char* tp = "com.vegascreativesoftware:testpattern";
    const char* cg = "com.vegascreativesoftware:colorgradient";
    const char* tt = "com.vegascreativesoftware:titlesandtext";
    const char* g = "Generator";
    const char* f = "Filter";
    auto noise = [](const char* style, const char* layers, const char* c1, const char* c2, const char* fx, const char* fy, const char* amp, const char* bias, const char* lo = "0", const char* hi = "1",
                     const char* ox = "0", const char* oy = "0", const char* prog = "0", const char* fall = "0.5") {
        return std::vector<V>{{C, "NoiseStyle", style}, {D, "NumberOfLayers", layers}, {B, "Swapcolors", "false"}, {RGBA, "Color1", c1}, {RGBA, "Color2", c2},
                              {D, "FrequencyX", fx}, {D, "FrequencyY", fy}, {D, "OffsetX", ox}, {D, "OffsetY", oy}, {D, "OffsetprogressinDegrees", prog},
                              {D, "NoiseParametersMin", lo}, {D, "NoiseParametersMax", hi}, {D, "NoiseParametersBias", bias},
                              {D, "NoiseParametersAmplitude", amp}, {D, "NoiseParametersGrainAmplitudeFalloff", fall}};
    };
    auto tiles = [](const char* w, const char* h, const char* sq, const char* bh, const char* bv, const char* c1, const char* c2) {
        return std::vector<V>{{D, "TileDimensionsWidth", w}, {D, "TileDimensionsHeight", h}, {B, "TileDimensionsSquare", sq},
                              {D, "EdgeBlendingHorizontal", bh}, {D, "EdgeBlendingVertical", bv}, {B, "EdgeBlendingProportional", "false"},
                              {RGBA, "Color1", c1}, {RGBA, "Color2", c2}, {D, "GridOffsetX", "0"}, {D, "GridOffsetY", "0"}};
    };
    auto title = [](const char* text, const char* size, const char* loc, const char* align, const char* color, bool shadow, const char* outline) {
        return std::vector<V>{{S, "Text", text}, {D, "FontSize", size}, {D2, "Location", loc}, {C, "Alignment", align}, {RGBA, "TextColor", color},
                              {B, "ShadowEnable", shadow ? "true" : "false"}, {D, "OutlineWidth", outline}};
    };
    auto anim = [](std::vector<V> v, const char* name) {
        v.push_back({S, "AnimationName", name});
        return v;
    };
    auto kernel = [](const char* nine, const char* scale, const char* offset) {
        std::vector<V> v;
        std::istringstream in(nine);
        std::string x;
        for (int k = 0; k < 9 && in >> x; ++k) v.push_back({D, "Row" + std::to_string(k / 3 + 1) + "Column" + std::to_string(k % 3 + 1), x});
        v.push_back({D, "Rotation", "0"});
        v.push_back({D, "Scale", scale});
        v.push_back({D, "Offset", offset});
        v.push_back({B, "AutoNormalize", "true"});
        return v;
    };
    return {
        {sc, g, "Black", {{RGBA, "Color", "0 0 0 1"}}},
        {sc, g, "White", {{RGBA, "Color", "1 1 1 1"}}},
        {sc, g, "Red", {{RGBA, "Color", "1 0 0 1"}}},
        {sc, g, "Green", {{RGBA, "Color", "0 1 0 1"}}},
        {sc, g, "Blue", {{RGBA, "Color", "0 0 1 1"}}},
        {sc, g, "Yellow", {{RGBA, "Color", "1 1 0 1"}}},
        {sc, g, "Cyan", {{RGBA, "Color", "0 1 1 1"}}},
        {sc, g, "Magenta", {{RGBA, "Color", "1 0 1 1"}}},
        {sc, g, "Orange", {{RGBA, "Color", "1 0.5 0 1"}}},
        {sc, g, "Olive", {{RGBA, "Color", "0.5 0.5 0.2 1"}}},
        {sc, g, "Gray", {{RGBA, "Color", "0.5 0.5 0.5 1"}}},
        {sc, g, "Transparent", {{RGBA, "Color", "0 0 0 0"}}},
        {cg, g, "Black to White", {{C, "GradientType", "0"}, {RGBA, "Color1", "0 0 0 1"}, {RGBA, "Color2", "1 1 1 1"}, {D, "AspectRatioAngle", "0"}}},
        {cg, g, "Sunset", {{C, "GradientType", "0"}, {RGBA, "Color1", "1 0.55 0.1 1"}, {RGBA, "Color2", "0.2 0.1 0.5 1"}, {D, "AspectRatioAngle", "90"}}},
        {cg, g, "Spotlight", {{C, "GradientType", "1"}, {RGBA, "Color1", "1 1 1 1"}, {RGBA, "Color2", "0 0 0 1"}, {D, "Size", "1.2"}}},
        {cg, g, "Box", {{C, "GradientType", "2"}, {RGBA, "Color1", "0 0 0 0"}, {RGBA, "Color2", "0 0 0 1"}, {D, "Size", "1.2"}}},
        {cb, g, "Large Tiles", tiles("0.1", "0.1", "true", "0", "0", "0 0 0 1", "1 1 1 1")},
        {cb, g, "Small Tiles", tiles("0.03", "0.03", "true", "0", "0", "0 0 0 1", "1 1 1 1")},
        {cb, g, "Horizontal Stripes", tiles("1", "0.07", "false", "0", "0.5", "0 0 0 0", "1 0.7 0.6 1")},
        {cb, g, "Vertical Stripes", tiles("0.05", "1", "false", "0.5", "0", "0 0 0 0", "0.6 0.8 1 1")},
        // VEGAS's own Noise Texture presets (the first ten are its exact values; the others are Reno's best match to the look)
        {nt, g, "Standard Noise", noise("0", "1", "0 0 0 1", "1 1 1 1", "10", "10", "1", "0")},
        {nt, g, "Standard Turbulence", noise("1", "1", "0 0 0 1", "1 1 1 1", "10", "10", "1", "0")},
        {nt, g, "Soft Clouds", noise("0", "4", "1 0.894118 1 1", "0.31 0.31 1 1", "2", "6", "3.7", "-1", "0", "1", "0", "2", "1")},
        {nt, g, "Puffy Clouds", noise("1", "3.5", "0.411765 0.34902 1 1", "1 1 1 1", "1.5", "3", "2", "-0.3", "0", "1", "1.5", "0", "0")},
        {nt, g, "Cloud Cover", noise("0", "3", "0.54902 0.54902 0.54902 1", "0 0 0 0", "2", "8", "4", "-1", "0", "1", "0", "0", "0", "0.7")},
        {nt, g, "Lava", noise("0", "2", "1 0.760784 0 1", "0.87451 0.133333 0 1", "15", "5", "1.5", "0", "0.1", "1", "0.1")},
        {nt, g, "Plasma", noise("0", "4", "1 0 1 1", "0 1 1 1", "5", "5", "1", "0")},
        {nt, g, "Blood Cells", noise("1", "2", "0.509804 0 0.090196 1", "1 0.8 0.886667 1", "10", "10", "1.5", "0", "0", "0.8")},
        {nt, g, "Camouflage", noise("0", "2", "0.254902 0.705882 0 1", "0.415686 0.223529 0 1", "15", "15", "4", "-0.8", "0.48", "0.5", "0", "0", "1", "0.3")},
        {nt, g, "Gravel", noise("0", "2", "0 0 0 1", "1 1 1 1", "20", "20", "1.2", "0", "0.2", "1", "0", "0", "0", "1")},
        {nt, g, "Marble 1", noise("2", "4", "0.55 0.55 0.6 1", "0.95 0.95 0.9 1", "4", "4", "1", "0")},
        {nt, g, "Marble 2", noise("3", "4", "0.55 0.4 0.5 1", "0.98 0.95 0.97 1", "3", "3", "1", "0")},
        {nt, g, "Moss", noise("0", "5", "0.05 0.2 0.05 1", "0.35 0.6 0.2 1", "12", "12", "1.4", "0")},
        {nt, g, "Wood", noise("6", "2", "0.35 0.2 0.08 1", "0.75 0.5 0.25 1", "3", "3", "1", "0")},
        {nt, g, "Microscopic Threads 1", noise("1", "4", "0.1 0.3 0.8 1", "0.2 0.8 0.5 1", "20", "20", "1.5", "0")},
        {nt, g, "Microscopic Threads 2", noise("1", "4", "0.55 0.05 0.3 1", "1 0.3 0.6 1", "20", "20", "1.5", "0")},
        {nt, g, "Lightning", noise("1", "5", "1 1 0.69 1", "0 0 0 1", "1.6", "0.6", "1", "0", "0", "0.15")},
        {nt, g, "Starry Sky", noise("0", "1", "0 0 0 1", "1 1 1 1", "60", "60", "1", "0", "0.66", "0.72")},
        {nt, g, "Charcoal Scribbles", noise("4", "3", "0.1 0.08 0.06 1", "0.95 0.9 0.8 1", "6", "6", "2", "0")},
        {nt, g, "Chalk Scribbles", noise("4", "3", "1 0.7 0.85 1", "0.1 0.05 0.08 1", "6", "6", "2", "0")},
        {tp, g, "SMPTE Bars (NTSC)", {{C, "TestPatternType", "5"}}},
        {tp, g, "SMPTE Bars (PAL)", {{C, "TestPatternType", "6"}}},
        {tp, g, "Color Bars (NTSC)", {{C, "TestPatternType", "0"}}},
        {tp, g, "Color Bars (PAL)", {{C, "TestPatternType", "1"}}},
        {tp, g, "White Porches", {{C, "TestPatternType", "7"}}},
        {tp, g, "Yellow Porches", {{C, "TestPatternType", "8"}}},
        {tp, g, "Plugs and Porches", {{C, "TestPatternType", "3"}}},
        {tp, g, "Ramp", {{C, "TestPatternType", "4"}}},
        {tp, g, "Gradient", {{C, "TestPatternType", "2"}}},
        {tt, g, "Default Text", title("Sample Text", "48", "0.5 0.5", "4", "1 1 1 1", false, "0")},
        {tt, g, "Lower Third", title("Name\nTitle", "28", "0.08 0.2", "6", "1 1 1 1", true, "0")},
        {tt, g, "Big Title", title("TITLE", "96", "0.5 0.55", "4", "1 0.85 0.2 1", true, "4")},
        {tt, g, "Subtitle", title("Subtitle text", "24", "0.5 0.08", "7", "1 1 1 1", false, "3")},
        // with VEGAS's animations (named as VEGAS's own presets that use them)
        {tt, g, "Bounce", anim(title("Bounce", "48", "0.5 0.5", "4", "0 0.5 0 1", false, "6"), "_Bounce")},
        {tt, g, "Fly In", anim(title("Fly In", "48", "0.5 0.5", "4", "1 1 1 1", true, "0"), "_Fly_in")},
        {tt, g, "Fly in from Right", anim(title("Fly in from Right", "48", "0.5 0.5", "4", "1 1 1 1", true, "0"), "_Fly_in_from_Right")},
        {tt, g, "Slide Up", anim(title("Slide Up", "48", "0.5 0.5", "4", "1 1 1 1", false, "4"), "_Slide_Up")},
        {tt, g, "Popup", anim(title("Popup", "60", "0.5 0.5", "4", "1 0.85 0.2 1", false, "4"), "_Popup")},
        {tt, g, "Fall Down", anim(title("Fall Down", "48", "0.5 0.5", "4", "1 1 1 1", false, "4"), "_Fall_Down")},
        {tt, g, "Earthquake", anim(title("EARTHQUAKE", "60", "0.5 0.5", "4", "0.9 0.3 0.1 1", true, "0"), "_Earthquake")},
        {tt, g, "Scroll", anim(title("Credits\nDirected by\nStarring", "36", "0.5 0.5", "4", "1 1 1 1", false, "0"), "_Scroll")},
        {tt, g, "Scroll Left", anim(title("News crawl text goes here", "28", "0.5 0.1", "4", "1 1 1 1", false, "0"), "_Scroll_Left")},
        {"com.vegascreativesoftware:invert", f, "100% Inversion", {{D, "BlendAmount", "1"}}},
        {"com.vegascreativesoftware:invert", f, "50% Inversion", {{D, "BlendAmount", "0.5"}}},
        {"com.vegascreativesoftware:blackandwhite", f, "100% Black and White", {{D, "BlendAmount", "1"}}},
        {"com.vegascreativesoftware:blackandwhite", f, "50% Black and White", {{D, "BlendAmount", "0.5"}}},
        {"com.vegascreativesoftware:brightnessandcontrast", f, "Brighter", {{D, "Brightness", "0.1"}, {D, "Contrast", "0"}, {D, "ContrastCenter", "0.5"}}},
        {"com.vegascreativesoftware:brightnessandcontrast", f, "Darker", {{D, "Brightness", "-0.1"}, {D, "Contrast", "0"}, {D, "ContrastCenter", "0.5"}}},
        {"com.vegascreativesoftware:brightnessandcontrast", f, "More Contrast", {{D, "Brightness", "0"}, {D, "Contrast", "0.2"}, {D, "ContrastCenter", "0.5"}}},
        {"com.vegascreativesoftware:brightnessandcontrast", f, "Flat", {{D, "Brightness", "0"}, {D, "Contrast", "-0.5"}, {D, "ContrastCenter", "0.5"}}},
        {"com.vegascreativesoftware:sepia", f, "Neutral", {{RGB, "Color", "0.65 0.65 0"}, {D, "BlendingStrength", "0.72"}, {D, "BlendingFalloff", "0.63"}}},
        {"com.vegascreativesoftware:sepia", f, "Warm", {{RGB, "Color", "0.8 0.45 0"}, {D, "BlendingStrength", "0.72"}, {D, "BlendingFalloff", "0.63"}}},
        {"com.vegascreativesoftware:hsladjust", f, "Desaturate", {{D, "AddToHue", "0"}, {D, "Saturation", "0"}, {D, "Luminance", "1"}}},
        {"com.vegascreativesoftware:hsladjust", f, "Max Saturation", {{D, "AddToHue", "0"}, {D, "Saturation", "2"}, {D, "Luminance", "1"}}},
        {"com.vegascreativesoftware:hsladjust", f, "Invert Hue", {{D, "AddToHue", "0.5"}, {D, "Saturation", "1"}, {D, "Luminance", "1"}}},
        {"com.vegascreativesoftware:levels", f, "Studio RGB to Computer RGB", {{C, "Channel", "0"}, {D, "InputStart", "0.0627"}, {D, "InputEnd", "0.9216"}, {D, "OutputStart", "0"}, {D, "OutputEnd", "1"}, {D, "Gamma", "1"}}},
        {"com.vegascreativesoftware:levels", f, "Computer RGB to Studio RGB", {{C, "Channel", "0"}, {D, "InputStart", "0"}, {D, "InputEnd", "1"}, {D, "OutputStart", "0.0627"}, {D, "OutputEnd", "0.9216"}, {D, "Gamma", "1"}}},
        {"com.vegascreativesoftware:mirror", f, "Reflect Top", {{D, "Angle", "0"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:mirror", f, "Reflect Bottom", {{D, "Angle", "180"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:mirror", f, "Reflect Left", {{D, "Angle", "90"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:mirror", f, "Reflect Right", {{D, "Angle", "-90"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:gaussianblur", f, "Light Blur", {{D, "HorizontalRange", "0.01"}, {D, "VerticalRange", "0.01"}}},
        {"com.vegascreativesoftware:gaussianblur", f, "Medium Blur", {{D, "HorizontalRange", "0.06"}, {D, "VerticalRange", "0.06"}}},
        {"com.vegascreativesoftware:gaussianblur", f, "Extreme Blur", {{D, "HorizontalRange", "0.19"}, {D, "VerticalRange", "0.19"}}},
        {"com.vegascreativesoftware:pixelate", f, "Small", {{D, "HorizontalPixelization", "0.12"}, {D, "VerticalPixelization", "0.12"}}},
        {"com.vegascreativesoftware:pixelate", f, "Large", {{D, "HorizontalPixelization", "0.5"}, {D, "VerticalPixelization", "0.5"}}},
        {"com.vegascreativesoftware:sharpen", f, "Light", {{D, "Amount", "0.25"}}},
        {"com.vegascreativesoftware:sharpen", f, "Heavy", {{D, "Amount", "1"}}},
        {"com.vegascreativesoftware:chromakeyer", f, "Blue Screen", {{RGB, "color", "0.094 0.278 0.945"}, {D, "LowThreshold", "0"}, {D, "HighThreshold", "0.4"}}},
        {"com.vegascreativesoftware:chromakeyer", f, "Green Screen", {{RGB, "color", "0.161 0.957 0.094"}, {D, "LowThreshold", "0"}, {D, "HighThreshold", "0.4"}}},
        {"com.vegascreativesoftware:border", f, "Soft Edge", {{C, "Type", "0"}, {D, "Size", "0.05"}}},
        {"com.vegascreativesoftware:border", f, "Solid White Border", {{C, "Type", "2"}, {D, "Size", "0.1"}, {RGBA, "color", "1 1 1 1"}}},
        {"com.vegascreativesoftware:border", f, "Bevel", {{C, "Type", "1"}, {D, "Size", "0.1"}, {D, "Angle", "135"}, {RGBA, "color", "1 1 1 1"}}},
        {"com.vegascreativesoftware:glow", f, "White Soft Glow", {{D, "GlowPercent", "0.22"}, {D, "Intensity", "3.2"}, {D, "Suppression", "0.3"}, {RGB, "Color", "1 1 1"}}},
        {"com.vegascreativesoftware:glow", f, "Red Glow", {{D, "GlowPercent", "0.5"}, {D, "Intensity", "1"}, {D, "Suppression", "0.2"}, {RGB, "Color", "1 0 0"}}},
        {"com.vegascreativesoftware:addnoise", f, "Grainy", {{D, "NoiseLevel", "0.3"}, {B, "Monochromatic", "true"}, {B, "GaussianNoise", "true"}, {B, "Animate", "true"}}},
        {"com.vegascreativesoftware:addnoise", f, "Color Noise", {{D, "NoiseLevel", "0.5"}, {B, "Monochromatic", "false"}, {B, "GaussianNoise", "false"}, {B, "Animate", "true"}}},
        {"com.vegascreativesoftware:threshold", f, "50%", {{D, "Threshold", "0.5"}}},
        {"com.vegascreativesoftware:linearblur", f, "Horizontal Light", {{D, "Angle", "0"}, {D, "Amount", "0.05"}}},
        {"com.vegascreativesoftware:linearblur", f, "Vertical Light", {{D, "Angle", "90"}, {D, "Amount", "0.05"}}},
        {"com.vegascreativesoftware:colorbalance", f, "Warm Midtones", {{D, "Red", "0.1"}, {D, "Green", "0"}, {D, "Blue", "-0.1"}, {C, "Type", "1"}, {B, "PreserveLuminosity", "true"}}},
        {"com.vegascreativesoftware:colorbalance", f, "Cool Midtones", {{D, "Red", "-0.1"}, {D, "Green", "0"}, {D, "Blue", "0.1"}, {C, "Type", "1"}, {B, "PreserveLuminosity", "true"}}},
        {"com.vegascreativesoftware:colorcorrectorsecondary", f, "Invert Color", {{D, "RotateHue", "180"}, {D, "Saturation", "1"}}},
        {"com.vegascreativesoftware:colorcorrectorsecondary", f, "Desaturate All but Reds", {{D, "Saturation", "0"}, {D, "LimitHueCenter", "283.5"}, {D, "LimitHueWidth", "300"}, {D, "LimitHueSmooth", "15"}}},
        {"com.vegascreativesoftware:colorcorrectorsecondary", f, "Desaturate Highs", {{D, "Saturation", "0"}, {D, "LimitLuminanceLow", "192"}, {D, "LimitLuminanceHigh", "255"}, {D, "LimitLuminanceSmooth", "96"}}},
        {"com.vegascreativesoftware:colorcorrectorsecondary", f, "Boost Mids", {{D, "Saturation", "1.25"}, {D, "Gamma", "1.25"}, {D, "LimitLuminanceLow", "128"}, {D, "LimitLuminanceHigh", "128"}, {D, "LimitLuminanceSmooth", "128"}}},
        {"com.vegascreativesoftware:colorcorrectorsecondary", f, "Saturate Greens", {{D, "Saturation", "2.5"}, {D, "LimitHueCenter", "240.7"}, {D, "LimitHueWidth", "30"}, {D, "LimitHueSmooth", "30"}}},
        {"com.vegascreativesoftware:colorcorrector", f, "Red Midtones", {{D2, "Mid", "103.5 0.5"}}},
        {"com.vegascreativesoftware:colorcorrector", f, "Green Midtones", {{D2, "Mid", "240.7 0.5"}}},
        {"com.vegascreativesoftware:colorcorrector", f, "Blue Midtones", {{D2, "Mid", "347.1 0.5"}}},
        {"com.vegascreativesoftware:colorcorrector", f, "Remove Yellow Cast", {{D2, "Low", "347.1 0.3"}, {D2, "Mid", "347.1 0.4"}, {D2, "High", "347.1 0.3"}}},
        {"com.vegascreativesoftware:convolutionkernel", f, "Blur", kernel("4 4 4 4 5 4 4 4 4", "0", "0")},
        {"com.vegascreativesoftware:convolutionkernel", f, "Emboss", kernel("2 0 0 0 0 0 0 0 -2", "1", "0.5")},
        {"com.vegascreativesoftware:convolutionkernel", f, "Bump", kernel("20 0 0 0 1 0 0 0 -20", "1", "0")},
        {"com.vegascreativesoftware:convolutionkernel", f, "Find Edges", kernel("1 1 1 1 -8 1 1 1 1", "1", "0")},
        {"com.vegascreativesoftware:convolutionkernel", f, "Sharpen Edges", kernel("1 1 1 1 -7 1 1 1 1", "1", "0")},
        {"com.vegascreativesoftware:convolutionkernel", f, "Sharpen", kernel("-1 -2 -1 -2 16 -2 -1 -2 -1", "0", "0")},
        {"com.vegascreativesoftware:spherize", f, "Maximum Sphere In", {{D, "Amount", "1"}, {D, "ScalingHorizontal", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:spherize", f, "Medium Sphere In", {{D, "Amount", "0.5"}, {D, "ScalingHorizontal", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:spherize", f, "Maximum Sphere Out", {{D, "Amount", "-1"}, {D, "ScalingHorizontal", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:spherize", f, "Medium Sphere Out", {{D, "Amount", "-0.5"}, {D, "ScalingHorizontal", "1"}, {B, "ScalingProportional", "true"}}},
        // Swirl, Pinch/Punch and Wave, VEGAS's own presets
        {"com.vegascreativesoftware:swirl", f, "Maximum Swirl, Clockwise", {{D, "Amount", "1"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:swirl", f, "Medium Swirl, Clockwise", {{D, "Amount", "0.5"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:swirl", f, "Maximum Swirl, Counter Clockwise", {{D, "Amount", "-1"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:swirl", f, "Medium Swirl, Counter Clockwise", {{D, "Amount", "-0.5"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:swirl", f, "Maximum Swirl, Clockwise, Half Scale", {{D, "Amount", "1"}, {D, "ScalingHorizontal", "0.5"}, {D, "ScalingVertical", "0.5"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:swirl", f, "Maximum Swirl, Counter Clockwise, Half Scale", {{D, "Amount", "-1"}, {D, "ScalingHorizontal", "0.5"}, {D, "ScalingVertical", "0.5"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:pinchpunch", f, "Maximum Punch", {{D, "Amount", "1"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:pinchpunch", f, "Medium Punch", {{D, "Amount", "0.5"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:pinchpunch", f, "Maximum Pinch", {{D, "Amount", "-1"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:pinchpunch", f, "Medium Pinch", {{D, "Amount", "-0.5"}, {D, "ScalingHorizontal", "1"}, {D, "ScalingVertical", "1"}, {B, "ScalingProportional", "true"}}},
        {"com.vegascreativesoftware:wave", f, "Large", {{D, "VerticalWaves", "5.4"}, {D, "HorizontalWaves", "5.4"}, {D, "VerticalAmplitude", "0.05"}, {D, "HorizontalAmplitude", "0.05"}, {D, "VerticalPhase", "0"}, {D, "HorizontalPhase", "0"}}},
        {"com.vegascreativesoftware:wave", f, "Medium", {{D, "VerticalWaves", "14"}, {D, "HorizontalWaves", "14"}, {D, "VerticalAmplitude", "0.05"}, {D, "HorizontalAmplitude", "0.05"}, {D, "VerticalPhase", "0"}, {D, "HorizontalPhase", "0"}}},
        {"com.vegascreativesoftware:wave", f, "Small", {{D, "VerticalWaves", "20"}, {D, "HorizontalWaves", "20"}, {D, "VerticalAmplitude", "0.03"}, {D, "HorizontalAmplitude", "0.03"}, {D, "VerticalPhase", "0"}, {D, "HorizontalPhase", "0"}}},
        {"com.vegascreativesoftware:wave", f, "Vertical Only", {{D, "VerticalWaves", "10"}, {D, "HorizontalWaves", "0"}, {D, "VerticalAmplitude", "0.04"}, {D, "HorizontalAmplitude", "0"}, {D, "VerticalPhase", "0.8"}, {D, "HorizontalPhase", "0"}}},
        {"com.vegascreativesoftware:wave", f, "Horizontal Only", {{D, "VerticalWaves", "0"}, {D, "HorizontalWaves", "10"}, {D, "VerticalAmplitude", "0"}, {D, "HorizontalAmplitude", "0.04"}, {D, "VerticalPhase", "0"}, {D, "HorizontalPhase", "0.8"}}},
        {"com.vegascreativesoftware:newsprint", f, "Newspaper", {{D, "DotSize", "0.12"}, {C, "Type", "1"}}},
        {"com.vegascreativesoftware:newsprint", f, "Color Print", {{D, "DotSize", "0.12"}, {C, "Type", "0"}}},
        {"com.vegascreativesoftware:newsprint", f, "Offset Print", {{D, "DotSize", "0.32"}, {C, "Type", "2"}}},
        {"com.vegascreativesoftware:newsprint", f, "Color Print Zoomed", {{D, "DotSize", "0.62"}, {C, "Type", "0"}}},
        // Cookie Cutter, VEGAS's presets as Vfx1.ofx has them - Repeat X and Y of 0 are taken as 1, the least they can be
        {"com.vegascreativesoftware:cookiecutter", f, "Circle, Center", {{RGB, "Color", "0 0 0"}, {C, "Shape", "0"}, {C, "Method", "0"}, {D, "Feather", "0"}, {D, "Border", "0"}, {I, "RepeatX", "0"}, {I, "RepeatY", "0"}, {D, "Size", "0.2"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Square, Center, White Border", {{RGB, "Color", "1 1 1"}, {C, "Shape", "5"}, {C, "Method", "0"}, {D, "Feather", "0"}, {D, "Border", "0.1"}, {I, "RepeatX", "0"}, {I, "RepeatY", "0"}, {D, "Size", "0.2"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Diamond, Center, Blurred", {{RGB, "Color", "0 1 0"}, {C, "Shape", "4"}, {C, "Method", "0"}, {D, "Feather", "0.3"}, {D, "Border", "0"}, {I, "RepeatX", "0"}, {I, "RepeatY", "0"}, {D, "Size", "0.2"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Circle Cut Out, Center, Blurred", {{RGB, "Color", "0 0 0"}, {C, "Shape", "0"}, {C, "Method", "1"}, {D, "Feather", "0.5"}, {D, "Border", "0"}, {I, "RepeatX", "0"}, {I, "RepeatY", "0"}, {D, "Size", "0.25"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Two Rectangles", {{RGB, "Color", "0 0 0"}, {C, "Shape", "3"}, {C, "Method", "0"}, {D, "Feather", "0"}, {D, "Border", "0"}, {I, "RepeatX", "0"}, {I, "RepeatY", "2"}, {D, "Size", "0.2"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Triangle Cut Out, Blue Border", {{RGB, "Color", "0 0 1"}, {C, "Shape", "6"}, {C, "Method", "1"}, {D, "Feather", "0"}, {D, "Border", "0.1"}, {I, "RepeatX", "0"}, {I, "RepeatY", "1"}, {D, "Size", "0.1"}, {D2, "Center", "0.5 0.5"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Square, Top Left, Border", {{RGB, "Color", "1 0 0"}, {C, "Shape", "5"}, {C, "Method", "0"}, {D, "Feather", "0"}, {D, "Border", "0.05"}, {I, "RepeatX", "0"}, {I, "RepeatY", "1"}, {D, "Size", "0.15"}, {D2, "Center", "0.25 0.65"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Diamond Cut Out, Bottom Right", {{RGB, "Color", "0 0 0"}, {C, "Shape", "4"}, {C, "Method", "1"}, {D, "Feather", "0.1"}, {D, "Border", "0"}, {I, "RepeatX", "0"}, {I, "RepeatY", "1"}, {D, "Size", "0.15"}, {D2, "Center", "0.75 0.35"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Top Left Portrait", {{RGB, "Color", "1 1 1"}, {C, "Shape", "2"}, {C, "Method", "0"}, {D, "Feather", "0"}, {D, "Border", "0.02"}, {I, "RepeatX", "0"}, {I, "RepeatY", "1"}, {D, "Size", "0.1"}, {D2, "Center", "0.13 0.7"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Yellow Glowing Portrait", {{RGB, "Color", "1 1 0"}, {C, "Shape", "0"}, {C, "Method", "0"}, {D, "Feather", "0.28"}, {D, "Border", "0.06"}, {I, "RepeatX", "0"}, {I, "RepeatY", "1"}, {D, "Size", "0.13"}, {D2, "Center", "0.75 0.75"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Pop-up, Bottom Right", {{RGB, "Color", "1 1 1"}, {C, "Shape", "0"}, {C, "Method", "0"}, {D, "Feather", "0"}, {D, "Border", "0.04"}, {I, "RepeatX", "0"}, {I, "RepeatY", "1"}, {D, "Size", "0.28"}, {D2, "Center", "1 0"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Picture-In-Picture", {{RGB, "Color", "0 0 0"}, {C, "Shape", "3"}, {C, "Method", "0"}, {D, "Feather", "0"}, {D, "Border", "0.01"}, {I, "RepeatX", "0"}, {I, "RepeatY", "1"}, {D, "Size", "0.2"}, {D2, "Center", "0.75 0.25"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "Grate", {{RGB, "Color", "0 0 0"}, {C, "Shape", "4"}, {C, "Method", "1"}, {D, "Feather", "0"}, {D, "Border", "0.08"}, {I, "RepeatX", "10"}, {I, "RepeatY", "10"}, {D, "Size", "0.15"}, {D2, "Center", "0.5 0.5"}}},
        // Slow Motion, VEGAS Pro 23's; its Default preset is half speed with medium quality
        {"com.vegascreativesoftware:slowmotion", f, "Default", {{C, "Quality", "1"}, {C, "Blending", "0"}, {D, "PlaybackRate", "0.5"}, {B, "ShowOpticalFlowField", "false"}}},
        {"com.vegascreativesoftware:slowmotion", f, "Quarter Speed, High Quality", {{C, "Quality", "2"}, {C, "Blending", "0"}, {D, "PlaybackRate", "0.25"}, {B, "ShowOpticalFlowField", "false"}}},
        {"com.vegascreativesoftware:slowmotion", f, "Tenth Speed, High Quality", {{C, "Quality", "2"}, {C, "Blending", "0"}, {D, "PlaybackRate", "0.1"}, {B, "ShowOpticalFlowField", "false"}}},
        {"com.vegascreativesoftware:slowmotion", f, "Half Speed, Nearest Frame", {{C, "Quality", "0"}, {C, "Blending", "1"}, {D, "PlaybackRate", "0.5"}, {B, "ShowOpticalFlowField", "false"}}},
        {"com.vegascreativesoftware:slowmotion", f, "Double Speed", {{C, "Quality", "0"}, {C, "Blending", "0"}, {D, "PlaybackRate", "2"}, {B, "ShowOpticalFlowField", "false"}}},
        {"com.vegascreativesoftware:cookiecutter", f, "TicTacToe", {{RGB, "Color", "1 1 1"}, {C, "Shape", "5"}, {C, "Method", "1"}, {D, "Feather", "0"}, {D, "Border", "0.04"}, {I, "RepeatX", "3"}, {I, "RepeatY", "3"}, {D, "Size", "0.25"}, {D2, "Center", "0.5 0.5"}}},
    };
}

std::string xmlEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

}  // namespace

const std::string& builtinPresetPackage()
{
    static const std::string xml = [] {
        std::ostringstream o;
        o << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<OfxPresetPackage>\n";
        for (const P& p : table()) {
            o << "  <OfxPreset plugin=\"" << p.plugin << "\" context=\"" << p.context << "\" name=\"" << xmlEscape(p.name) << "\">\n";
            o << "    <OfxPlugin>" << p.plugin << "</OfxPlugin>\n    <OfxPluginVersion>1 0</OfxPluginVersion>\n";
            for (const V& v : p.values)
                o << "    <" << v.type << " name=\"" << v.name << "\"><OfxParamValue>" << xmlEscape(v.value) << "</OfxParamValue></" << v.type << ">\n";
            o << "  </OfxPreset>\n";
        }
        for (int k = 0; k < builtin::kVfxPresetChunkCount; ++k) o << "  " << builtin::kVfxPresetChunks[k];       // VEGAS's own presets for the stand-ins
        for (int k = 0; k < builtin::kMagixPresetChunkCount; ++k) o << "  " << builtin::kMagixPresetChunks[k];
        for (int k = 0; k < builtin::kColorPresetChunkCount; ++k) o << "  " << builtin::kColorPresetChunks[k];
        o << "</OfxPresetPackage>\n";
        std::string x = o.str();
        // VEGAS Pro 18 runs Rays and Glint as managed CoreGraphics filters whose presets (in ScriptPortal.MediaSoftware.TextGen.CoreGraphics.dll) differ from the OpenFX preset file where the
        // conversion lost values: the rays' light position of "Luminance" (0.8599, 0.2288 from the top) and "Alpha" (0.4732, 0.3220), and the small soft ellipse of "Center Mask, Glint"
        // (19.71% x 28.42%, blur 0.0349 of the width).
        auto patch = [&x](const char* plugin, const char* preset, const char* param, const char* value) {
            const size_t at = x.find(std::string("plugin=\"com.vegascreativesoftware:") + plugin + "\" context=\"Filter\" name=\"" + preset + "\"");
            if (at == std::string::npos) return;
            const size_t end = x.find("</OfxPreset>", at);
            const size_t p = x.find(std::string("name=\"") + param + "\"><OfxParamValue>", at);
            if (p == std::string::npos || p > end) return;
            const size_t v0 = x.find("<OfxParamValue>", p) + 15, v1 = x.find("</OfxParamValue>", v0);
            if (v1 != std::string::npos && v1 < end) x.replace(v0, v1 - v0, value);
        };
        patch("raysvelvetmatter", "Luminance", "EffectPosition", "0.859928 0.771186");
        patch("raysvelvetmatter", "Alpha", "EffectPosition", "0.473159 0.677966");
        patch("glintvelvetmatter", "Center Mask, Glint", "MaskWidth", "19.709544");
        patch("glintvelvetmatter", "Center Mask, Glint", "MaskHeight", "28.423237");
        patch("glintvelvetmatter", "Center Mask, Glint", "MaskSoftness", "0.034855");
        return x;
    }();
    return xml;
}

}  // namespace ofx
}  // namespace reno
