// ACES color management without OpenColorIO. See colormgmt.h.
#include "colormgmt.h"
#include "project.h"
#include <algorithm>
#include <cmath>

namespace reno {

namespace {

typedef float M3[3][3];

const M3 kAp0To709 = {{2.52169f, -1.13413f, -0.38756f}, {-0.27648f, 1.37272f, -0.09624f}, {-0.01538f, -0.15298f, 1.16835f}};
const M3 k709ToAp0 = {{0.4397010f, 0.3829780f, 0.1773350f}, {0.0897923f, 0.8134230f, 0.0967616f}, {0.0175440f, 0.1115440f, 0.8707040f}};
const M3 kAp1To709 = {{1.70505f, -0.62179f, -0.08326f}, {-0.13026f, 1.14080f, -0.01055f}, {-0.02400f, -0.12897f, 1.15297f}};
const M3 k709ToAp1 = {{0.6130974f, 0.3395231f, 0.0473795f}, {0.0701937f, 0.9163539f, 0.0134524f}, {0.0206156f, 0.1095698f, 0.8698147f}};
const M3 k709To2020 = {{0.6274f, 0.3293f, 0.0433f}, {0.0691f, 0.9195f, 0.0114f}, {0.0164f, 0.0880f, 0.8956f}};
const M3 k2020To709 = {{1.6605f, -0.5876f, -0.0728f}, {-0.1246f, 1.1329f, -0.0083f}, {-0.0182f, -0.1006f, 1.1187f}};
const M3 k709ToP3 = {{0.8225f, 0.1774f, 0.0f}, {0.0332f, 0.9669f, 0.0f}, {0.0171f, 0.0724f, 0.9108f}};
const M3 k709ToXyz = {{0.4124f, 0.3576f, 0.1805f}, {0.2126f, 0.7152f, 0.0722f}, {0.0193f, 0.1192f, 0.9505f}};
// the ACES fit's input (the RRT's saturation, sRGB to its working space) and output matrices (Stephen Hill's fit)
const M3 kFitIn = {{0.59719f, 0.35458f, 0.04823f}, {0.07600f, 0.90834f, 0.01566f}, {0.02840f, 0.13383f, 0.83777f}};
const M3 kFitOut = {{1.60475f, -0.53108f, -0.07367f}, {-0.10208f, 1.10813f, -0.00605f}, {-0.00327f, -0.07276f, 1.07602f}};

void mul(const M3& m, float c[3])
{
    const float r = c[0], g = c[1], b = c[2];
    c[0] = m[0][0] * r + m[0][1] * g + m[0][2] * b;
    c[1] = m[1][0] * r + m[1][1] * g + m[1][2] * b;
    c[2] = m[2][0] * r + m[2][1] * g + m[2][2] * b;
}

// ---- curves (code value <-> linear), one channel
float rec709Decode(float v) { return v < 0.081f ? v / 4.5f : std::pow((v + 0.099f) / 1.099f, 1.0f / 0.45f); }
float rec709Encode(float l) { return l < 0.018f ? l * 4.5f : 1.099f * std::pow(l, 0.45f) - 0.099f; }
float srgbDecode(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float srgbEncode(float l) { return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f; }
float gammaEncode(float l, float g) { return l <= 0 ? 0 : std::pow(l, 1.0f / g); }

float acesccDecode(float v)
{
    if (v < (9.72f - 15.f) / 17.52f) return (std::pow(2.f, v * 17.52f - 9.72f) - std::pow(2.f, -16.f)) * 2.f;
    return std::min(65504.f, std::pow(2.f, v * 17.52f - 9.72f));
}
float acesccEncode(float l)
{
    if (l <= 0) return (-16.f + 9.72f) / 17.52f;
    if (l < std::pow(2.f, -15.f)) return (std::log2(std::pow(2.f, -16.f) + l * 0.5f) + 9.72f) / 17.52f;
    return (std::log2(l) + 9.72f) / 17.52f;
}
float acescctDecode(float v) { return v <= 0.155251141552511f ? (v - 0.0729055341958355f) / 10.5402377416545f : std::pow(2.f, v * 17.52f - 9.72f); }
float acescctEncode(float l) { return l <= 0.0078125f ? 10.5402377416545f * l + 0.0729055341958355f : (std::log2(l) + 9.72f) / 17.52f; }
float acesproxyDecode(float v) { return std::pow(2.f, (v * 1023.f - 425.f) / 50.f - 2.5f); }
float acesproxyEncode(float l) { return std::min(940.f, std::max(64.f, (std::log2(std::max(l, 1e-10f)) + 2.5f) * 50.f + 425.f)) / 1023.f; }
// ADX (film printing density): the density above the base, made linear by the film's slope (about 0.6) round mid grey
float adxDecode(float v, bool sixteen)
{
    const float d = sixteen ? (v * 65535.f - 1520.f) / 8000.f : (v * 1023.f - 95.f) / 500.f;
    return 0.18f * std::pow(10.f, (d - 0.7f) / 0.6f);
}
float adxEncode(float l, bool sixteen)
{
    const float d = 0.7f + 0.6f * std::log10(std::max(l, 1e-6f) / 0.18f);
    return std::max(0.f, sixteen ? (d * 8000.f + 1520.f) / 65535.f : (d * 500.f + 95.f) / 1023.f);
}
float slog1Decode(float v)
{
    const float y = (v * 1023.f - 64.f) / 876.f;
    return (std::pow(10.f, (y - 0.616596f - 0.03f) / 0.432699f) - 0.037584f) * 0.9f;
}
float slog1Encode(float l) { return ((0.432699f * std::log10(std::max(l / 0.9f + 0.037584f, 1e-6f)) + 0.616596f + 0.03f) * 876.f + 64.f) / 1023.f; }
float slog2Decode(float v)
{
    const float y = (v * 1023.f - 64.f) / 876.f;
    return 219.f * (std::pow(10.f, (y - 0.616596f - 0.03f) / 0.432699f) - 0.037584f) / 155.f * 0.9f;
}
float slog2Encode(float l) { return ((0.432699f * std::log10(std::max(155.f * l / (219.f * 0.9f) + 0.037584f, 1e-6f)) + 0.616596f + 0.03f) * 876.f + 64.f) / 1023.f; }
float slog3Decode(float v)
{
    const float c = v * 1023.f;
    return c >= 171.2102946929f ? std::pow(10.f, (c - 420.f) / 261.5f) * (0.18f + 0.01f) - 0.01f : (c - 95.f) * 0.01125000f / (171.2102946929f - 95.f);
}
float slog3Encode(float l)
{
    return (l >= 0.01125000f ? 420.f + std::log10((l + 0.01f) / (0.18f + 0.01f)) * 261.5f : l * (171.2102946929f - 95.f) / 0.01125000f + 95.f) / 1023.f;
}
const float kLcCut = 0.010591f, kLcA = 5.555556f, kLcB = 0.052272f, kLcC = 0.247190f, kLcD = 0.385537f, kLcE = 5.367655f, kLcF = 0.092809f;
float logcDecode(float t) { return t > kLcE * kLcCut + kLcF ? (std::pow(10.f, (t - kLcD) / kLcC) - kLcB) / kLcA : (t - kLcF) / kLcE; }
float logcEncode(float x) { return x > kLcCut ? kLcC * std::log10(kLcA * x + kLcB) + kLcD : kLcE * x + kLcF; }
float clogDecode(float v) { return v < 0.0730597f ? -(std::pow(10.f, (0.0730597f - v) / 0.529136f) - 1.f) / 10.1596f : (std::pow(10.f, (v - 0.0730597f) / 0.529136f) - 1.f) / 10.1596f; }
float clogEncode(float x) { return x < 0 ? -(0.529136f * std::log10(-x * 10.1596f + 1.f)) + 0.0730597f : 0.529136f * std::log10(10.1596f * x + 1.f) + 0.0730597f; }
float protuneDecode(float v) { return (std::pow(113.f, v) - 1.f) / 112.f; }
float protuneEncode(float x) { return std::log(std::max(x, -0.0088f) * 112.f + 1.f) / std::log(113.f); }
float vlogDecode(float v) { return v < 0.181f ? (v - 0.125f) / 5.6f : std::pow(10.f, (v - 0.598206f) / 0.241514f) - 0.00873f; }
float vlogEncode(float x) { return x < 0.01f ? 5.6f * x + 0.125f : 0.241514f * std::log10(x + 0.00873f) + 0.598206f; }
float log3g10Decode(float v) { return v < 0 ? v / 15.1927f - 0.01f : (std::pow(10.f, v / 0.224282f) - 1.f) / 155.975327f - 0.01f; }
float log3g10Encode(float x) { x += 0.01f; return x < 0 ? x * 15.1927f : 0.224282f * std::log10(x * 155.975327f + 1.f); }
// a log space spanning what the ACES RRT uses: as ACEScc's curve
float logDecode(float v) { return std::pow(2.f, v * 17.52f - 9.72f); }
float logEncode(float l) { return std::max(0.f, (std::log2(std::max(l, 1e-6f)) + 9.72f) / 17.52f); }
// PQ (SMPTE ST 2084) of nits
float pqEncode(float nits)
{
    const float y = std::max(0.f, nits / 10000.f);
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(y, m1);
    return std::pow((c1 + c2 * p) / (1.f + c3 * p), m2);
}
float hlgEncode(float e)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::max(0.f, e);
    return e <= 1.f / 12.f ? std::sqrt(3.f * e) : a * std::log(12.f * e - b) + c;
}

// The ACES RRT + an SDR output transform, fitted: scene-linear Rec. 709 in, display-linear Rec. 709 out (0..1).
void rrtFit(float c[3])
{
    mul(kFitIn, c);
    for (int k = 0; k < 3; ++k) {
        const float v = std::max(0.f, c[k]);
        c[k] = (v * (v + 0.0245786f) - 0.000090537f) / (v * (0.983729f * v + 0.4329510f) + 0.238081f);
    }
    mul(kFitOut, c);
    for (int k = 0; k < 3; ++k) c[k] = std::min(1.f, std::max(0.f, c[k]));
}

// ---- the lists
std::vector<ColorSpaceChoice> mediaList(bool v07)
{
    if (v07)
        return {{"", "Default"}, {"aces", "ACES"}, {"adx10", "ADX 10-bit"}, {"adx16", "ADX 16-bit"}, {"slog1", "Sony S-Log1 (F35/F3)"},
                {"slog2_3200", "Sony S-Log2 (F65 3200K)"}, {"slog2_5500", "Sony S-Log2 (F65 5500K)"}, {"logc", "Arri Alexa Log C"}, {"log", "Log"}};
    return {{"", "Default"},
            {"aces", "ACES - ACES2065-1"}, {"acescg", "ACES - ACEScg"}, {"acescc", "ACES - ACEScc"}, {"acescct", "ACES - ACEScct"},
            {"acesproxy", "ACES - ACESproxy"},
            {"adx10", "Input - ADX - ADX10"}, {"adx16", "Input - ADX - ADX16"},
            {"logc", "Input - ARRI - V3 LogC (EI800) - Wide Gamut"},
            {"clog", "Input - Canon - Canon-Log - Rec. 709 Daylight"},
            {"protune", "Input - GoPro - Protune Flat - Protune Native - Experimental"},
            {"vlog", "Input - Panasonic - V-Log - V-Gamut"},
            {"log3g10", "Input - RED - Log3G10 - REDWideGamutRGB"},
            {"slog1", "Input - Sony - S-Log1 - S-Gamut"}, {"slog2_3200", "Input - Sony - S-Log2 - S-Gamut 3200K"},
            {"slog2_5500", "Input - Sony - S-Log2 - S-Gamut Daylight"}, {"slog3", "Input - Sony - S-Log3 - S-Gamut3"},
            {"srgb", "Output - sRGB"}, {"rec709", "Output - Rec.709"}, {"rec2020", "Output - Rec.2020"}, {"log", "Utility - Log"}};
}

std::vector<ColorSpaceChoice> viewList(bool v07)
{
    if (v07)
        return {{"off", "Off"}, {"rrt_srgb", "ACES RRT (sRGB)"}, {"rrt_rec709", "ACES RRT (Rec. 709)"}, {"rrt_p3dci", "ACES RRT (P3 DCI)"},
                {"rrt_xyz", "ACES RRT (CIE XYZ)"}, {"log_srgb", "Log (sRGB)"}};
    return {{"off", "Off"},
            {"rrt_srgb", "sRGB (ACES)"}, {"rrt_xyz", "DCDM (ACES)"}, {"rrt_xyz_p3d60", "DCDM P3D60 Limited (ACES)"},
            {"rrt_xyz_p3d65", "DCDM P3D65 Limited (ACES)"}, {"rrt_p3d60", "P3-D60 (ACES)"}, {"rrt_p3dci_d60", "P3-DCI D60 simulation (ACES)"},
            {"rrt_p3dci_d65", "P3-DCI D65 simulation (ACES)"}, {"rrt_p3d65", "P3D65 (ACES)"}, {"rrt_p3d65_d60", "P3D65 D60 simulation (ACES)"},
            {"rrt_p3d65_709", "P3D65 Rec.709 Limited (ACES)"}, {"rrt_p3d65_pq108", "P3D65 ST2084 108 nits (ACES)"},
            {"rrt_rec2020", "Rec.2020 (ACES)"}, {"rrt_rec2020_p3", "Rec.2020 P3D65 Limited (ACES)"}, {"rrt_rec2020_709", "Rec.2020 Rec.709 Limited (ACES)"},
            {"rrt_hlg1000", "Rec.2020 HLG 1000 nits (ACES)"}, {"rrt_pq1000", "Rec.2020 ST2084 1000 nits (ACES)"},
            {"rrt_pq2000", "Rec.2020 ST2084 2000 nits (ACES)"}, {"rrt_pq4000", "Rec.2020 ST2084 4000 nits (ACES)"},
            {"rrt_rec709", "Rec.709 (ACES)"}, {"rrt_rec709_d60", "Rec.709 D60 sim. (ACES)"}, {"rrt_srgb_d60", "sRGB D60 sim. (ACES)"},
            {"log_srgb", "Log (ACES)"}};
}

bool starts(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }

}  // namespace

const std::vector<ColorSpaceChoice>& mediaColorSpaces(const std::string& acesVersion)
{
    static const std::vector<ColorSpaceChoice> v10 = mediaList(false), v07 = mediaList(true);
    return acesVersion == "0.7" ? v07 : v10;
}

const std::vector<ColorSpaceChoice>& viewTransforms(const std::string& acesVersion)
{
    static const std::vector<ColorSpaceChoice> v10 = viewList(false), v07 = viewList(true);
    return acesVersion == "0.7" ? v07 : v10;
}

const std::vector<ColorSpaceChoice>& renderColorSpaces()
{
    static const std::vector<ColorSpaceChoice> list = {
        {"", "Default"}, {"aces", "ACES"}, {"log", "Log"}, {"rrt_srgb", "ACES RRT (sRGB)"}, {"rrt_rec709", "ACES RRT (Rec. 709)"},
        {"rrt_p3dci", "ACES RRT (P3 DCI)"}, {"rrt_p3d60", "ACES RRT (P3 D60)"}, {"rrt_xyz", "ACES RRT (CIE XYZ)"}, {"adx10", "ADX 10-bit"},
        {"adx16", "ADX 16-bit"}, {"slog1", "Sony S-Log1 (F35/F3)"}, {"slog2_3200", "Sony S-Log2 (F65 3200K)"}, {"slog2_5500", "Sony S-Log2 (F65 5500K)"},
        {"logc", "Arri Alexa Log C"}, {"acescg", "ACEScg"}, {"acescc", "ACEScc"}, {"acescct", "ACEScct"}, {"slog3", "Sony S-Log3 (S-Gamut3)"},
        {"rec709", "Rec.709"}, {"srgb", "sRGB"}, {"rec2020", "Rec.2020"}, {"rrt_rec2020", "ACES RRT (Rec.2020)"}, {"rrt_pq1000", "ACES RRT (Rec.2020 ST2084 1000 nits)"},
        {"rrt_hlg1000", "ACES RRT (Rec.2020 HLG 1000 nits)"}};
    return list;
}

std::string colorSpaceLabel(const std::string& id)
{
    const std::vector<ColorSpaceChoice>* lists[5] = {&mediaColorSpaces("1.0"), &viewTransforms("1.0"), &renderColorSpaces(), &mediaColorSpaces("0.7"),
                                                     &viewTransforms("0.7")};
    for (const auto* l : lists)
        for (const ColorSpaceChoice& c : *l)
            if (c.id == id) return c.label;
    return id;
}

HdrCoding hdrCodingOf(const std::string& id)
{
    HdrCoding c;
    if (starts(id, "rrt_pq") || id == "rrt_p3d65_pq108") c.transfer = "pq";
    else if (starts(id, "rrt_hlg")) c.transfer = "hlg";
    else return c;
    c.primaries = id == "rrt_p3d65_pq108" ? "p3d65" : "bt2020";
    return c;
}

namespace {
struct MdPreset {
    const char* id;
    const char* label;
    double rx, ry, gx, gy, bx, by, wx, wy, maxNits, minNits;
};
const MdPreset kMdPresets[] = {
    {"custom", "Custom display", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {"rec2020_1000", "Rec.2020, 1000 nits", 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290, 1000, 0.005},
    {"rec2020_4000", "Rec.2020, 4000 nits", 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290, 4000, 0.005},
    {"p3d65_1000", "P3-D65, 1000 nits", 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290, 1000, 0.005},
    {"p3d65_4000", "P3-D65, 4000 nits", 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290, 4000, 0.005},
    {"p3d65_108", "P3-D65, 108 nits (cinema)", 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290, 108, 0.005},
};
}  // namespace

std::vector<std::string> masterDisplayPresetIds()
{
    std::vector<std::string> v;
    for (const MdPreset& p : kMdPresets) v.push_back(p.id);
    return v;
}

std::string masterDisplayPresetLabel(const std::string& id)
{
    for (const MdPreset& p : kMdPresets)
        if (id == p.id) return p.label;
    return "Custom display";
}

MasterDisplay masterDisplayPreset(const std::string& id, const MasterDisplay& current)
{
    for (const MdPreset& p : kMdPresets) {
        if (id != p.id || id == "custom") continue;
        MasterDisplay m = current;
        m.preset = p.id;
        m.redX = p.rx; m.redY = p.ry; m.greenX = p.gx; m.greenY = p.gy; m.blueX = p.bx; m.blueY = p.by;
        m.whiteX = p.wx; m.whiteY = p.wy; m.maxNits = p.maxNits; m.minNits = p.minNits;
        return m;
    }
    return current;
}

bool linearLightProject(const ProjectSettings& s)
{
    // 32-bit floating point (full range) with compositing gamma 1.000 (Linear), or with a View transform (ACES works in linear light)
    return s.pixelFormat == "32bit" && s.pixelRange == "full" && (s.compositingGamma < 1.5 || s.viewTransform != "off");
}
bool floatProject(const ProjectSettings& s) { return s.pixelFormat == "32bit"; }

// A color space (or view transform) name is worked out once into one of these, so the per-pixel loops below do no
// string comparisons. The steps keep the order and arithmetic of the per-name code they replace exactly.
namespace {

// Code values -> scene-linear Rec. 709: a curve on each channel (or the ADX one), S-Log2 3200K's white balance, then a
// matrix to Rec. 709 primaries.
struct DecodePlan {
    float (*curve)(float) = nullptr;
    int adxBits = 0;                    // 10 or 16: ADX's density curve instead of 'curve'
    bool slog2Tungsten = false;         // slog2_3200
    const M3* matrix = nullptr;
};

DecodePlan decodePlanOf(const std::string& cs)
{
    DecodePlan d;
    if (cs.empty() || cs == "rec709") d.curve = rec709Decode;
    else if (cs == "srgb") d.curve = srgbDecode;
    else if (cs == "rec2020") { d.curve = rec709Decode; d.matrix = &k2020To709; }
    else if (cs == "aces") d.matrix = &kAp0To709;
    else if (cs == "acescg") d.matrix = &kAp1To709;
    else if (cs == "acescc") { d.curve = acesccDecode; d.matrix = &kAp1To709; }
    else if (cs == "acescct") { d.curve = acescctDecode; d.matrix = &kAp1To709; }
    else if (cs == "acesproxy") { d.curve = acesproxyDecode; d.matrix = &kAp1To709; }
    else if (cs == "adx10") d.adxBits = 10;
    else if (cs == "adx16") d.adxBits = 16;
    else if (cs == "slog1") d.curve = slog1Decode;
    else if (starts(cs, "slog2")) { d.curve = slog2Decode; d.slog2Tungsten = cs == "slog2_3200"; }
    else if (cs == "slog3") d.curve = slog3Decode;
    else if (cs == "logc") d.curve = logcDecode;
    else if (cs == "clog") d.curve = clogDecode;
    else if (cs == "protune") d.curve = protuneDecode;
    else if (cs == "vlog") d.curve = vlogDecode;
    else if (cs == "log3g10") d.curve = log3g10Decode;
    else if (cs == "log") d.curve = logDecode;
    else d.curve = rec709Decode;
    return d;
}

void decodeColor(const DecodePlan& d, float c[3])
{
    if (d.adxBits) for (int k = 0; k < 3; ++k) c[k] = adxDecode(c[k], d.adxBits == 16);
    else if (d.curve) for (int k = 0; k < 3; ++k) c[k] = d.curve(c[k]);
    if (d.slog2Tungsten) { c[0] *= 0.93f; c[2] *= 1.08f; }
    if (d.matrix) mul(*d.matrix, c);
}

// The ACES RRT's display outputs ("rrt_..." view transforms).
enum class RrtOut { Srgb, Rec709, Rec2020, Xyz, P3 };

// Scene-linear Rec. 709 -> code values: a matrix from Rec. 709, S-Log2 3200K's white balance undone, then a curve on
// each channel (or the ADX one); or the ACES RRT with an HDR or SDR output.
struct EncodePlan {
    const M3* matrix = nullptr;
    bool slog2Tungsten = false;
    float (*curve)(float) = nullptr;
    int adxBits = 0;
    bool rrt = false;
    // HDR outputs: the light rolled off towards 'peak' nits, then PQ or HLG (after 'matrix')
    bool hdr = false, hlg = false;
    float peak = 1000.f;
    // SDR outputs: the fitted RRT, the D60 white simulation, then the display's coding
    bool d60 = false;
    RrtOut out = RrtOut::Srgb;
};

EncodePlan encodePlanOf(const std::string& t)
{
    EncodePlan e;
    if (t.empty() || t == "off" || t == "rec709") { e.curve = rec709Encode; return e; }
    if (t == "srgb") { e.curve = srgbEncode; return e; }
    if (t == "rec2020") { e.matrix = &k709To2020; e.curve = rec709Encode; return e; }
    if (t == "aces") { e.matrix = &k709ToAp0; return e; }
    if (t == "acescg") { e.matrix = &k709ToAp1; return e; }
    if (t == "acescc") { e.matrix = &k709ToAp1; e.curve = acesccEncode; return e; }
    if (t == "acescct") { e.matrix = &k709ToAp1; e.curve = acescctEncode; return e; }
    if (t == "acesproxy") { e.matrix = &k709ToAp1; e.curve = acesproxyEncode; return e; }
    if (t == "adx10" || t == "adx16") { e.adxBits = t == "adx16" ? 16 : 10; return e; }
    if (t == "slog1") { e.curve = slog1Encode; return e; }
    if (starts(t, "slog2")) { e.slog2Tungsten = t == "slog2_3200"; e.curve = slog2Encode; return e; }
    if (t == "slog3") { e.curve = slog3Encode; return e; }
    if (t == "logc") { e.curve = logcEncode; return e; }
    if (t == "clog") { e.curve = clogEncode; return e; }
    if (t == "protune") { e.curve = protuneEncode; return e; }
    if (t == "vlog") { e.curve = vlogEncode; return e; }
    if (t == "log3g10") { e.curve = log3g10Encode; return e; }
    if (t == "log" || t == "log_srgb") { e.curve = logEncode; return e; }
    if (!starts(t, "rrt_")) { e.curve = rec709Encode; return e; }
    // ACES RRT with an output transform
    e.rrt = true;
    const std::string od = t.substr(4);
    if (starts(od, "pq") || od == "p3d65_pq108" || starts(od, "hlg")) {
        e.hdr = true;
        e.hlg = starts(od, "hlg");
        e.peak = od == "p3d65_pq108" ? 108.f : od == "pq2000" ? 2000.f : od == "pq4000" ? 4000.f : 1000.f;
        e.matrix = od == "p3d65_pq108" ? &k709ToP3 : &k709To2020;
        return e;
    }
    e.d60 = od.find("d60") != std::string::npos && od != "p3d60";
    if (starts(od, "srgb")) e.out = RrtOut::Srgb;
    else if (starts(od, "rec709")) e.out = RrtOut::Rec709;
    else if (starts(od, "rec2020")) e.out = RrtOut::Rec2020;
    else if (starts(od, "xyz")) e.out = RrtOut::Xyz;
    else if (starts(od, "p3")) e.out = RrtOut::P3;
    else e.out = RrtOut::Srgb;
    return e;
}

void encodeRrt(const EncodePlan& e, float c[3])
{
    if (e.hdr) {
        // HDR: the scene's light (1.0 = 100 nits) rolled off towards the display's peak, then PQ or HLG
        mul(*e.matrix, c);
        for (int k = 0; k < 3; ++k) {
            const float x = std::max(0.f, c[k]) * 100.f;
            const float nits = x / (1.f + x / e.peak);
            c[k] = e.hlg ? hlgEncode(nits / e.peak) : pqEncode(nits);
        }
        return;
    }
    rrtFit(c);
    if (e.d60) { c[1] *= 0.985f; c[2] *= 0.92f; }
    switch (e.out) {
    case RrtOut::Srgb: for (int k = 0; k < 3; ++k) c[k] = srgbEncode(c[k]); break;
    case RrtOut::Rec709: for (int k = 0; k < 3; ++k) c[k] = gammaEncode(c[k], 2.4f); break;
    case RrtOut::Rec2020: mul(k709To2020, c); for (int k = 0; k < 3; ++k) c[k] = gammaEncode(std::max(0.f, c[k]), 2.4f); break;
    case RrtOut::Xyz: mul(k709ToXyz, c); for (int k = 0; k < 3; ++k) c[k] = gammaEncode(std::max(0.f, c[k]) * 48.f / 52.37f, 2.6f); break;
    case RrtOut::P3: mul(k709ToP3, c); for (int k = 0; k < 3; ++k) c[k] = gammaEncode(std::max(0.f, c[k]), 2.6f); break;
    }
}

void encodeColor(const EncodePlan& e, float c[3])
{
    if (e.rrt) { encodeRrt(e, c); return; }
    if (e.matrix) mul(*e.matrix, c);
    if (e.slog2Tungsten) { c[0] /= 0.93f; c[2] /= 1.08f; }
    if (e.adxBits) for (int k = 0; k < 3; ++k) c[k] = adxEncode(c[k], e.adxBits == 16);
    else if (e.curve) for (int k = 0; k < 3; ++k) c[k] = e.curve(c[k]);
}

}  // namespace

void colorToSceneLinear(float c[3], const std::string& cs) { decodeColor(decodePlanOf(cs), c); }

void colorFromSceneLinear(float c[3], const std::string& t) { encodeColor(encodePlanOf(t), c); }

ImagePtr toSceneLinear(const ImagePtr& src, const std::string& cs)
{
    if (!src) return src;
    ImagePtr img = src->depth == PixelDepth::Float ? src->clone() : src->converted(PixelDepth::Float);
    if (!img) return src;
    const DecodePlan plan = decodePlanOf(cs);
    parallelRows(img->h, (size_t)img->w * 24, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = (float*)img->row(y);
            for (int x = 0; x < img->w; ++x, p += 4) {
                const float a = p[3];
                if (a <= 1e-6f) { p[0] = p[1] = p[2] = 0; continue; }
                float c[3] = {p[0] / a, p[1] / a, p[2] / a};
                decodeColor(plan, c);
                p[0] = c[0] * a; p[1] = c[1] * a; p[2] = c[2] * a;
            }
        }
    });
    return img;
}

void fromSceneLinear(Image& img, const std::string& t)
{
    if (img.depth != PixelDepth::Float) return;
    const EncodePlan plan = encodePlanOf(t);
    parallelRows(img.h, (size_t)img.w * 24, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = (float*)img.row(y);
            for (int x = 0; x < img.w; ++x, p += 4) {
                const float a = p[3];
                if (a <= 1e-6f) { p[0] = p[1] = p[2] = 0; continue; }
                float c[3] = {p[0] / a, p[1] / a, p[2] / a};
                encodeColor(plan, c);
                p[0] = c[0] * a; p[1] = c[1] * a; p[2] = c[2] * a;
            }
        }
    });
}

}  // namespace reno
