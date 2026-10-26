// Reading and writing VEGAS's managed ("CoreGraphics") video effects; record layout in vegmanaged.h.
#include "vegmanaged.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace reno {
namespace veg {

// ------------------------------------------------------------------------------------------ the record
bool isManagedFxAt(const Bytes& b, size_t off)
{
    if (!b.has(off, 0x38)) return false;
    if (b.u32(off) != 0x38) return false;
    if (b.guid(off + 4) == G::OfxEffect) return false;
    const uint32_t nameBytes = b.u32(off + 0x34);
    if (nameBytes > 0x1000 || !b.has(off + 0x38, nameBytes)) return false;
    const size_t p = off + 0x38 + nameBytes;
    // the XML follows the name, behind its length (in the static block, or at the first key)
    if (b.u32(off + 0x14) != 0) return b.has(p + 4, 5) && std::memcmp(b.data() + p + 4, "<?xml", 5) == 0;
    return b.u32(off + 0x18) > 0 && b.has(p + 0x20, 5) && std::memcmp(b.data() + p + 0x20, "<?xml", 5) == 0;
}

static std::string xmlRootOf(const std::string& xml)
{
    size_t i = 0;
    while (i < xml.size()) {
        i = xml.find('<', i);
        if (i == std::string::npos || i + 1 >= xml.size()) return std::string();
        const char c = xml[i + 1];
        if (c == '?' || c == '!') { ++i; continue; }
        size_t j = i + 1;
        while (j < xml.size() && (std::isalnum((unsigned char)xml[j]) || xml[j] == '_' || xml[j] == '.' || xml[j] == ':')) ++j;
        return xml.substr(i + 1, j - i - 1);
    }
    return std::string();
}

bool readManagedFx(const Bytes& b, size_t off, ManagedFxRecord& out)
{
    out = ManagedFxRecord();
    if (!isManagedFxAt(b, off)) return false;
    out.classGuid = b.guid(off + 4);
    const uint32_t staticBytes = b.u32(off + 0x14), count = b.u32(off + 0x18), nameBytes = b.u32(off + 0x34);
    if (count > 100000) return false;
    out.preset = b.text(off + 0x38, nameBytes);
    size_t p = off + 0x38 + nameBytes;
    if (staticBytes != 0) {
        if (staticBytes < 9 || !b.has(p, staticBytes) || b.u32(p) != staticBytes - 4) return false;
        out.hasStatic = true;
        out.staticXml.assign((const char*)b.data() + p + 4, staticBytes - 4);
        p += staticBytes;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!b.has(p, 0x20) || b.u32(p) != 0x1c) return false;
        const uint32_t blob = b.u32(p + 0x18), len = b.u32(p + 0x1c);
        if (len + 4 != blob || !b.has(p + 0x20, len)) return false;
        ManagedFxKey k;
        k.time = b.i64(p + 4);
        k.extra.assign((const char*)b.data() + p + 12, 12);
        k.xml.assign((const char*)b.data() + p + 0x20, len);
        out.keys.push_back(std::move(k));
        p += 0x20 + len;
    }
    if (!out.hasStatic && out.keys.empty()) return false;
    out.root = xmlRootOf(out.hasStatic ? out.staticXml : out.keys[0].xml);
    if (out.root.empty()) return false;
    out.end = p;
    return true;
}

std::string writeManagedFx(const Bytes& original, size_t originalOff, const ManagedFxRecord& rec)
{
    const uint32_t nameBytes = original.u32(originalOff + 0x34);
    std::string s((const char*)original.data() + originalOff, std::min<size_t>(original.size() - originalOff, 0x38 + (size_t)nameBytes));
    setU32(s, 0x14, rec.hasStatic ? (uint32_t)rec.staticXml.size() + 4 : 0u);
    setU32(s, 0x18, (uint32_t)rec.keys.size());
    if (rec.hasStatic) {
        addU32(s, (uint32_t)rec.staticXml.size());
        s += rec.staticXml;
    }
    for (const auto& k : rec.keys) {
        addU32(s, 0x1c);
        addU64(s, (uint64_t)k.time);
        std::string extra = k.extra;
        extra.resize(12, '\0');
        s += extra;
        addU32(s, (uint32_t)k.xml.size() + 4);
        addU32(s, (uint32_t)k.xml.size());
        s += k.xml;
    }
    return s;
}

// ------------------------------------------------------------------------------------------ a small XML reader
// Only what the XmlSerializer writes: elements with text or children, attributes, the declaration. It records where
// each element's text is so a value can be replaced in place, leaving the rest of the text as it was.
namespace {

struct XEl {
    std::string name;
    std::string attrs;
    size_t textBeg = 0, textEnd = 0;        // the text between the tags of an element without children
    bool leaf = false;                      // has a text to replace (not self-closing, no children)
    std::vector<XEl> kids;
};

struct XmlReader {
    const std::string& s;
    size_t i = 0;
    explicit XmlReader(const std::string& t) : s(t) {}

    void skipSpace() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; }

    bool prolog()
    {
        for (;;) {
            skipSpace();
            if (s.compare(i, 2, "<?") == 0) {
                const size_t e = s.find("?>", i);
                if (e == std::string::npos) return false;
                i = e + 2;
            } else if (s.compare(i, 4, "<!--") == 0) {
                const size_t e = s.find("-->", i);
                if (e == std::string::npos) return false;
                i = e + 3;
            } else {
                return i < s.size() && s[i] == '<';
            }
        }
    }

    bool element(XEl& el, int depth)
    {
        if (depth > 24 || i >= s.size() || s[i] != '<') return false;
        ++i;
        const size_t n0 = i;
        while (i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '>' && s[i] != '/') ++i;
        if (i == n0 || i >= s.size()) return false;
        el.name = s.substr(n0, i - n0);
        const size_t a0 = i;
        // attributes, with quoted values that may hold '>'
        while (i < s.size() && s[i] != '>' && !(s[i] == '/' && i + 1 < s.size() && s[i + 1] == '>')) {
            if (s[i] == '"' || s[i] == '\'') {
                const size_t e = s.find(s[i], i + 1);
                if (e == std::string::npos) return false;
                i = e;
            }
            ++i;
        }
        if (i >= s.size()) return false;
        el.attrs = s.substr(a0, i - a0);
        if (s[i] == '/') { i += 2; el.leaf = false; return true; }       // <X/>
        ++i;
        el.textBeg = i;
        for (;;) {
            const size_t lt = s.find('<', i);
            if (lt == std::string::npos) return false;
            if (s.compare(lt, 2, "</") == 0) {
                el.textEnd = lt;
                const size_t gt = s.find('>', lt);
                if (gt == std::string::npos) return false;
                if (s.compare(lt + 2, el.name.size(), el.name) != 0) return false;
                i = gt + 1;
                el.leaf = el.kids.empty();
                return true;
            }
            if (s.compare(lt, 4, "<!--") == 0) {
                const size_t e = s.find("-->", lt);
                if (e == std::string::npos) return false;
                i = e + 3;
                continue;
            }
            if (s.compare(lt, 9, "<![CDATA[") == 0) return false;
            i = lt;
            XEl kid;
            if (!element(kid, depth + 1)) return false;
            el.kids.push_back(std::move(kid));
        }
    }
};

bool parseXml(const std::string& xml, XEl& root)
{
    XmlReader r(xml);
    return r.prolog() && r.element(root, 0);
}

// "Mask/Mask/Progress" below 'el'.
const XEl* findEl(const XEl& el, const std::string& path)
{
    const XEl* cur = &el;
    size_t at = 0;
    while (at <= path.size() && cur) {
        const size_t slash = path.find('/', at);
        const std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        const XEl* next = nullptr;
        for (const auto& k : cur->kids)
            if (k.name == part) { next = &k; break; }
        cur = next;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return cur;
}

std::string trimmed(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string textOf(const std::string& xml, const XEl* el)
{
    return el && el->leaf ? trimmed(xml.substr(el->textBeg, el->textEnd - el->textBeg)) : std::string();
}

bool parseNumber(const std::string& t, double& out)
{
    if (t.empty()) return false;
    char* e = nullptr;
    const double v = std::strtod(t.c_str(), &e);
    if (e == t.c_str() || *e != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::string lowerNoSep(const std::string& s)
{
    std::string o;
    for (char c : s)
        if (c != ' ' && c != '_' && c != '-') o += (char)std::tolower((unsigned char)c);
    return o;
}

// A number as .NET writes a double: the shortest text that reads back as the same number.
std::string fmtDouble(double v)
{
    if (v == 0) return "0";
    char buf[48];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    for (auto& c : s)
        if (c == ',') c = '.';
    return s;
}

// ------------------------------------------------------------------------------------------ the effects
// One setting: Reno's OpenFX parameter, how the XML stores it (element path below the root, a second element for a
// position's y), and the factor between them (the XML holds fractions, the OpenFX parameters percent).
// PosDown: a position whose y the XML counts down from the top of the frame (OpenFX counts up).
enum class K { Double, Bool, Choice, Int, Pos, PosDown };
struct Map {
    const char* ofx;
    K kind;
    const char* xml;
    const char* xml2;
    double scale;
    const std::vector<const char*>* names = nullptr;    // a Choice: the XML's enum names in the OpenFX choice's order (none: a mask's)
    bool otherObject = false;                           // a Choice that is another object in the XML when it changes (not saved)
};

struct Effect {
    const char* root;
    const char* pluginId;
    std::vector<const char*> maskTypes;          // the OpenFX choice's order
    std::vector<Map> maps;
};

const std::vector<const char*>& maskModes()
{
    static const std::vector<const char*> m = {"Disabled", "Effect", "Source", "ShowMaskOnly"};
    return m;
}

const std::vector<const char*>& irisShapes()
{
    static const std::vector<const char*> m = {"Circle", "Pentagon", "Hexagon", "Heptagon", "Octagon"};
    return m;
}

const std::vector<const char*>& exteriorEffects()
{
    static const std::vector<const char*> m = {"None", "Blur", "Black", "White", "Transparent"};
    return m;
}

const std::vector<const char*>& vignetteShapes()
{
    static const std::vector<const char*> m = {"Rectangle", "Ellipse"};
    return m;
}

// Settings every effect with a mask has (the mask's own settings live in <Mask><Mask xsi:type="...">). Each mask kind
// is its own class with its own properties: WipeMask Progress, Falloff, Angle (degrees; Reno's Angle is in turns),
// Strength; BarMask Progress, Width, Falloff, Angle, LeadingRegion, TrailingRegion; RectangleMask Blur (Reno's
// Softness), Width, Height, CornerRadius, XPosition, YPos (from the top); EllipseMask the same without CornerRadius.
// A setting is read from and written to the element the mask has.
void addMaskMaps(std::vector<Map>& m)
{
    m.push_back({"MaskMaskMode", K::Choice, "Mask/Mode", nullptr, 1});
    m.push_back({"MaskMaskType", K::Choice, "Mask/Type", nullptr, 1});
    m.push_back({"MaskProgress", K::Double, "Mask/Mask/Progress", nullptr, 100});
    m.push_back({"MaskSoftness", K::Double, "Mask/Mask/Blur", nullptr, 1});
    m.push_back({"MaskWidth", K::Double, "Mask/Mask/Width", nullptr, 100});
    m.push_back({"MaskHeight", K::Double, "Mask/Mask/Height", nullptr, 100});
    m.push_back({"MaskCornerRadius", K::Double, "Mask/Mask/CornerRadius", nullptr, 1});
    m.push_back({"MaskPosition", K::PosDown, "Mask/Mask/XPosition", "Mask/Mask/YPos", 1});
    m.push_back({"MaskFalloff", K::Double, "Mask/Mask/Falloff", nullptr, 1});
    m.push_back({"MaskAngle", K::Double, "Mask/Mask/Angle", nullptr, 1.0 / 360.0});
    m.push_back({"MaskStrength", K::Double, "Mask/Mask/Strength", nullptr, 100});
    m.push_back({"MaskLeadingRegion", K::Double, "Mask/Mask/LeadingRegion", nullptr, 1});
    m.push_back({"MaskTrailingRegion", K::Double, "Mask/Mask/TrailingRegion", nullptr, 1});
}

// Scales: the "Luminance" Rays XML in a sample project has Hue 211.555, HueSweep 48.8136, Saturation 0.394068, Quality
// 0.5, Aspect 1, Peak 1, which equal VEGAS's OpenFX preset of that name (211.56, 48.81, 39.41, 0.5, 1, 100) once
// fractions become percent; Threshold, Amount and Boost (0.3179, 0.1719, 0.3406) show in VEGAS's window as 31.79,
// 17.19, 34.06. Glint's "Sparkle" (Threshold 67.04, Boost 44.92, radii 62.04, Hue 116.78, Sweep 240, Streaks 3)
// matches the other samples' XML the same way (Boost 0.449153, radii 0.62041, Hue 116.7764).
const std::vector<Effect>& effects()
{
    static std::vector<Effect> list;
    if (!list.empty()) return list;
    {
        Effect e;
        e.root = "Rays";
        e.pluginId = "com.vegascreativesoftware:raysvelvetmatter";
        e.maskTypes = {"Wipe", "Rectangle", "Bar", "Ellipse"};
        e.maps = {
            {"EffectKeyAlphaChannel", K::Bool, "Alpha", nullptr, 1},
            {"EffectThreshold", K::Double, "Threshold", nullptr, 100},
            {"EffectAmount", K::Double, "Amount", nullptr, 100},
            {"EffectPosition", K::PosDown, "CenterX", "CenterY", 1},
            {"EffectAspect", K::Double, "Aspect", nullptr, 1},
            {"EffectBoost", K::Double, "Boost", nullptr, 100},
            {"EffectPeak", K::Double, "Peak", nullptr, 100},
            {"EffectNoise", K::Double, "Noise", nullptr, 100},
            {"EffectHue", K::Double, "Hue", nullptr, 1},
            {"EffectHueSweep", K::Double, "HueSweep", nullptr, 1},
            {"EffectSaturation", K::Double, "Saturation", nullptr, 100},
            {"EffectQuality", K::Double, "Quality", nullptr, 1},
            {"EffectReduceFlicker", K::Bool, "ReduceFlicker", nullptr, 1},
            {"EffectEffectOnly", K::Bool, "EffectOnly", nullptr, 1},
        };
        addMaskMaps(e.maps);
        list.push_back(std::move(e));
    }
    {
        Effect e;
        e.root = "Glint";
        e.pluginId = "com.vegascreativesoftware:glintvelvetmatter";
        e.maskTypes = {"Rectangle", "Bar", "Wipe", "Ellipse"};
        e.maps = {
            {"EffectThreshold", K::Double, "Threshold", nullptr, 100},
            {"EffectBoost", K::Double, "Boost", nullptr, 100},
            {"EffectHorizontalRadius", K::Double, "HorizontalRadius", nullptr, 100},
            {"EffectVerticalRadius", K::Double, "VerticalRadius", nullptr, 100},
            {"EffectHue", K::Double, "Hue", nullptr, 1},
            {"EffectHueSweep", K::Double, "HueSweep", nullptr, 1},
            {"EffectSaturation", K::Double, "Saturation", nullptr, 100},
            {"EffectOrientation", K::Double, "Rotation", nullptr, 1},
            {"EffectStreaks", K::Int, "Streaks", nullptr, 1},
            {"EffectReduceFlicker", K::Bool, "ReduceFlicker", nullptr, 1},
            {"EffectEffectOnly", K::Bool, "EffectOnly", nullptr, 1},
        };
        addMaskMaps(e.maps);
        list.push_back(std::move(e));
    }
    {
        // Starburst: scales from VEGAS's shipped XML presets against the same presets as OpenFX ("Simple 4 Point",
        // "Bright Center"): Threshold 0.72508 = 72.51, Brilliance 8.78390 = 878.39, Radius 0.16627 = 16.63,
        // CenterBurst 0.87288 = 87.29, Orientation 0.125 = 45 (fraction of a turn). Aspect and Points as is.
        // The XML's SoftEdges setting is not in the presets and is not mapped.
        Effect e;
        e.root = "Starburst";
        e.pluginId = "com.vegascreativesoftware:starburstvelvetmatter";
        e.maps = {
            {"Threshold", K::Double, "Threshold", nullptr, 100},
            {"Brilliance", K::Double, "Brilliance", nullptr, 100},
            {"Strength", K::Double, "Strength", nullptr, 100},
            {"Radius", K::Double, "Radius", nullptr, 100},
            {"Points", K::Int, "Points", nullptr, 1},
            {"CenterBurst", K::Double, "CenterBurst", nullptr, 100},
            {"Aspect", K::Double, "Aspect", nullptr, 1},
            {"Orientation", K::Double, "Orientation", nullptr, 360},
            {"ReduceFlicker", K::Bool, "ReduceFlicker", nullptr, 1},
            {"EffectOnly", K::Bool, "EffectOnly", nullptr, 1},
        };
        list.push_back(std::move(e));
    }
    {
        // Fill Light: Strength 0.52132 = 52.13, Bias -0.66310 = -66.31, Correction 1 = Tint 100, Softness 0.0067797 =
        // 3.39 (factor 500), Hue 0.092834 = 33.42 (fraction of a turn).
        Effect e;
        e.root = "FillLight";
        e.pluginId = "com.vegascreativesoftware:filllightvelvetmatter";
        e.maps = {
            {"Hue", K::Double, "Hue", nullptr, 360},
            {"Strength", K::Double, "Strength", nullptr, 100},
            {"Bias", K::Double, "Bias", nullptr, 100},
            {"Tint", K::Double, "Correction", nullptr, 100},
            {"Softness", K::Double, "Softness", nullptr, 500},
        };
        list.push_back(std::move(e));
    }
    {
        // Defocus: ApertureRadius 0.034831 = Radius 3.48, Curvature -1 = -100, BloomThreshold 0.92644 = 92.64,
        // BloomStrength 6.77542 = 6.78. Orientation is 0 in every preset; assumed a fraction of a turn like Starburst's.
        Effect e;
        e.root = "Defocus";
        e.pluginId = "com.vegascreativesoftware:defocusvelvetmatter";
        e.maps = {
            {"Radius", K::Double, "ApertureRadius", nullptr, 100},
            {"IrisShape", K::Choice, "Iris", nullptr, 1, &irisShapes()},
            {"Orientation", K::Double, "Orientation", nullptr, 360},
            {"DiscCurvature", K::Double, "Curvature", nullptr, 100},
            {"BloomThreshold", K::Double, "BloomThreshold", nullptr, 100},
            {"BloomStrength", K::Double, "BloomStrength", nullptr, 1},
            {"ReduceFlicker", K::Bool, "ReduceFlicker", nullptr, 1},
        };
        list.push_back(std::move(e));
    }
    {
        // Soft Contrast (class Softlight): Stretch 0.17797 = 17.8, Strength 0.66525 = Contrast 66.53, Diffusion
        // 0.076271 = 15.25 (factor 200), LowTrim 0.055085 = 5.51, Saturation 0.32627 = Tint 32.63, Hue 0.11864 =
        // Correction hue 42.71 (fraction of a turn). Its vignette: Blur 0.2 = Softness 20, Width 0.84979 = 84.98,
        // CornerRadius 0.15 = 15, exterior effect None / Blur / Black / White / Transparent. The vignette position
        // counts down like the other effects' (assumed; every preset has it in the middle).
        Effect e;
        e.root = "Softlight";
        e.pluginId = "com.vegascreativesoftware:softcontrastvelvetmatter";
        e.maps = {
            {"EffectStretchRange", K::Double, "Stretch", nullptr, 100},
            {"EffectContrast", K::Double, "Strength", nullptr, 100},
            {"EffectDiffusion", K::Double, "Diffusion", nullptr, 200},
            {"EffectLowTrim", K::Double, "LowTrim", nullptr, 100},
            {"EffectHighTrim", K::Double, "HighTrim", nullptr, 100},
            {"EffectTint", K::Double, "Saturation", nullptr, 100},
            {"EffectCorrectionHue", K::Double, "Hue", nullptr, 360},
            {"VignetteExteriorEffect", K::Choice, "VignetteEffect/ExteriorEffectType", nullptr, 1, &exteriorEffects()},
            {"VignetteStrength", K::Double, "VignetteEffect/Strength", nullptr, 100},
            {"VignetteShape", K::Choice, "VignetteEffect/MaskType", nullptr, 1, &vignetteShapes(), true},
            {"VignetteSoftness", K::Double, "VignetteEffect/VignetteMask/Blur", nullptr, 100},
            {"VignetteWidth", K::Double, "VignetteEffect/VignetteMask/Width", nullptr, 100},
            {"VignetteHeight", K::Double, "VignetteEffect/VignetteMask/Height", nullptr, 100},
            {"VignetteCornerRadius", K::Double, "VignetteEffect/VignetteMask/CornerRadius", nullptr, 100},
            {"VignettePosition", K::PosDown, "VignetteEffect/VignetteMask/XPosition", "VignetteEffect/VignetteMask/YPos", 1},
        };
        list.push_back(std::move(e));
    }
    return list;
}

const Effect* effectOfRoot(const std::string& root)
{
    for (const auto& e : effects())
        if (root == e.root) return &e;
    return nullptr;
}

// One snapshot of the object's settings: the OpenFX parameter values (a position has two) and whether it is enabled.
struct Snap {
    std::map<std::string, std::vector<double>> v;
    bool enabled = true;
    bool hasEnabled = false;
};

int indexIn(const std::vector<const char*>& names, const std::string& text)
{
    const std::string want = lowerNoSep(text);
    for (size_t i = 0; i < names.size(); ++i)
        if (want == lowerNoSep(names[i])) return (int)i;
    return -1;
}

// Reads the elements that are present; what is missing or not understood is left out and reported.
bool snapOf(const std::string& xml, const Effect& fx, Snap& out, std::vector<std::string>* problems)
{
    XEl root;
    if (!parseXml(xml, root) || root.name != fx.root) return false;
    out = Snap();
    if (const XEl* en = findEl(root, "Enabled")) {
        out.hasEnabled = true;
        out.enabled = lowerNoSep(textOf(xml, en)) != "false";
    }
    for (const Map& m : fx.maps) {
        const XEl* e1 = findEl(root, m.xml);
        if (!e1) continue;
        const std::string t1 = textOf(xml, e1);
        double a = 0;
        switch (m.kind) {
        case K::Double:
        case K::Int:
            if (!parseNumber(t1, a)) { if (problems) problems->push_back(m.xml); continue; }
            a *= m.scale;
            out.v[m.ofx] = {m.kind == K::Int ? std::floor(a + 0.5) : a};
            break;
        case K::Bool: {
            const std::string t = lowerNoSep(t1);
            if (t != "true" && t != "false") { if (problems) problems->push_back(m.xml); continue; }
            out.v[m.ofx] = {t == "true" ? 1.0 : 0.0};
            break;
        }
        case K::Choice: {
            const std::vector<const char*>& names = m.names ? *m.names : std::strcmp(m.ofx, "MaskMaskMode") == 0 ? maskModes() : fx.maskTypes;
            const int idx = indexIn(names, t1);
            if (idx < 0) { if (problems) problems->push_back(std::string(m.xml) + " = " + t1); continue; }
            out.v[m.ofx] = {(double)idx};
            break;
        }
        case K::Pos:
        case K::PosDown: {
            const XEl* e2 = findEl(root, m.xml2);
            double b = 0;
            if (!e2 || !parseNumber(t1, a) || !parseNumber(textOf(xml, e2), b)) { if (problems) problems->push_back(m.xml); continue; }
            out.v[m.ofx] = {a * m.scale, m.kind == K::PosDown ? 1.0 - b * m.scale : b * m.scale};
            break;
        }
        }
    }
    return true;
}

bool isWhole(K k) { return k == K::Bool || k == K::Choice || k == K::Int; }
bool isPos(K k) { return k == K::Pos || k == K::PosDown; }

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

bool sameValues(const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!nearlyEqual(a[i], b[i])) return false;
    return true;
}

}  // namespace

std::string managedPluginId(const ManagedFxRecord& r)
{
    const Effect* fx = effectOfRoot(r.root);
    return fx ? fx->pluginId : std::string();
}

// ------------------------------------------------------------------------------------------ record -> Reno's settings
bool managedFxState(const ManagedFxRecord& r, double fps, Json& state, bool& bypass, std::vector<std::string>& problems)
{
    const Effect* fx = effectOfRoot(r.root);
    if (!fx) return false;
    // snapshots: one per key, or the static block's if there are no keys
    std::vector<Snap> snaps;
    std::vector<double> frames;
    if (r.keys.empty()) {
        Snap s;
        if (!snapOf(r.staticXml, *fx, s, &problems)) return false;
        snaps.push_back(s);
        frames.push_back(0);
    }
    for (const auto& k : r.keys) {
        Snap s;
        // (problems are the same at every key: reported once)
        if (!snapOf(k.xml, *fx, s, snaps.empty() ? &problems : nullptr)) return false;
        snaps.push_back(s);
        frames.push_back((double)k.time / 1e7 * fps);
    }
    bypass = snaps[0].hasEnabled && !snaps[0].enabled;
    Json params = Json::object();
    for (const Map& m : fx->maps) {
        auto first = snaps[0].v.find(m.ofx);
        if (first == snaps[0].v.end()) continue;
        Json e = Json::object();
        Json v = Json::array();
        for (double d : first->second) v.push(d);
        e["v"] = v;
        if (isWhole(m.kind)) e["i"] = true;
        // keys only where the value changes from key to key
        bool varies = false;
        for (size_t i = 1; i < snaps.size(); ++i) {
            auto it = snaps[i].v.find(m.ofx);
            if (it != snaps[i].v.end() && !sameValues(it->second, first->second)) varies = true;
        }
        if (varies) {
            Json ks = Json::array();
            for (size_t i = 0; i < snaps.size(); ++i) {
                auto it = snaps[i].v.find(m.ofx);
                if (it == snaps[i].v.end()) continue;
                Json key = Json::array();
                key.push(frames[i]);
                for (double d : it->second) key.push(d);
                ks.push(key);
            }
            e["k"] = ks;
        }
        params[m.ofx] = e;
    }
    state = Json::object();
    state["plugin"] = fx->pluginId;
    state["params"] = params;
    return true;
}

// ------------------------------------------------------------------------------------------ Reno's settings -> record
namespace {

// A parameter's values at a frame: its key there, the line between the keys around it, or its value.
std::vector<double> valueAt(const Json& p, double frame)
{
    std::vector<double> out;
    const Json& k = p.get("k");
    if (k.isArray() && k.size() > 0) {
        const size_t n = k.size();
        const Json* prev = nullptr;
        // (in time order, whatever order the keys are stored in)
        std::vector<size_t> order;
        for (size_t i = 0; i < n; ++i)
            if (k.at(i).isArray() && k.at(i).size() >= 2) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return k.at(a).at(0).asDouble() < k.at(b).at(0).asDouble(); });
        for (size_t oi = 0; oi < order.size(); ++oi) {
            const Json& key = k.at(order[oi]);
            const double t = key.at(0).asDouble();
            if (t >= frame - 1e-6) {
                if (!prev || std::fabs(t - frame) <= 1e-6) {
                    for (size_t j = 1; j < key.size(); ++j) out.push_back(key.at(j).asDouble());
                    return out;
                }
                const double t0 = prev->at(0).asDouble();
                const double f = t > t0 ? (frame - t0) / (t - t0) : 0.0;
                for (size_t j = 1; j < key.size() && j < prev->size(); ++j)
                    out.push_back(prev->at(j).asDouble() + (key.at(j).asDouble() - prev->at(j).asDouble()) * f);
                return out;
            }
            prev = &key;
        }
        if (prev) {
            for (size_t j = 1; j < prev->size(); ++j) out.push_back(prev->at(j).asDouble());
            return out;
        }
    }
    const Json& v = p.get("v");
    for (size_t j = 0; v.isArray() && j < v.size(); ++j) out.push_back(v.at(j).asDouble());
    return out;
}

struct Edit {
    size_t beg, end;
    std::string text;
};

// The XML with the settings of 'want' that differ from 'have' put in (and <Enabled>).
std::string patchXml(const std::string& xml, const Effect& fx, const Snap& have, const std::map<std::string, std::vector<double>>& want,
                     bool enabled, std::vector<std::string>& warnings)
{
    XEl root;
    if (!parseXml(xml, root)) return xml;
    std::vector<Edit> edits;
    auto put = [&](const char* path, const std::string& text) {
        const XEl* e = findEl(root, path);
        if (e && e->leaf) edits.push_back({e->textBeg, e->textEnd, text});
    };
    if (have.hasEnabled && have.enabled != enabled) put("Enabled", enabled ? "true" : "false");
    for (const auto& w : want)
    for (const Map& mm : fx.maps) {
        if (w.first != mm.ofx) continue;
        const Map* m = &mm;
        auto h = have.v.find(w.first);
        if (h == have.v.end() || sameValues(h->second, w.second)) continue;
        if (w.second.size() != h->second.size()) continue;
        switch (m->kind) {
        case K::Double:
            put(m->xml, fmtDouble(w.second[0] / m->scale));
            break;
        case K::Int:
            put(m->xml, fmtDouble(std::floor(w.second[0] + 0.5)));
            break;
        case K::Bool:
            put(m->xml, w.second[0] != 0 ? "true" : "false");
            break;
        case K::Pos:
        case K::PosDown:
            put(m->xml, fmtDouble(w.second[0] / m->scale));
            put(m->xml2, fmtDouble((m->kind == K::PosDown ? 1.0 - w.second[1] : w.second[1]) / m->scale));
            break;
        case K::Choice: {
            const bool mode = std::strcmp(m->ofx, "MaskMaskMode") == 0;
            const std::vector<const char*>& names = m->names ? *m->names : mode ? maskModes() : fx.maskTypes;
            const long idx = std::lround(w.second[0]);
            if (idx < 0 || idx >= (long)names.size()) break;
            if (m->otherObject || (!mode && !m->names)) {
                // another mask kind is another object in the XML, whose settings are not known for every kind
                warnings.push_back("a different mask type was not saved (VEGAS keeps each kind of mask as an object of its own)");
                break;
            }
            put(m->xml, names[(size_t)idx]);
            break;
        }
        }
    }
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.beg > b.beg; });
    std::string out = xml;
    for (const auto& e : edits) out.replace(e.beg, e.end - e.beg, e.text);
    return out;
}

}  // namespace

bool managedFxWith(const ManagedFxRecord& orig, const Json& state, bool bypass, double fps, ManagedFxRecord& out,
                   std::vector<std::string>& warnings)
{
    const Effect* fx = effectOfRoot(orig.root);
    if (!fx || !(fps > 0)) return false;
    out = orig;
    const Json& params = state.get("params");

    // the old record's keys, as settings
    std::vector<Snap> have;
    for (const auto& k : orig.keys) {
        Snap s;
        if (!snapOf(k.xml, *fx, s, nullptr)) return false;
        have.push_back(s);
    }
    Snap haveStatic;
    if (orig.hasStatic && !snapOf(orig.staticXml, *fx, haveStatic, nullptr)) return false;

    // new key times: wherever any setting has a key; with none, the old keys' (or time 0)
    std::vector<double> frames;
    for (const auto& m : fx->maps) {
        const Json& k = params.get(m.ofx).get("k");
        for (size_t i = 0; k.isArray() && i < k.size(); ++i)
            if (k.at(i).isArray() && k.at(i).size() >= 2) frames.push_back(k.at(i).at(0).asDouble());
    }
    std::sort(frames.begin(), frames.end());
    {
        std::vector<double> uniq;
        for (double f : frames)
            if (uniq.empty() || f - uniq.back() > 1e-6) uniq.push_back(f);
        frames.swap(uniq);
    }
    std::vector<int64_t> ticks;
    std::vector<int> base;          // which old key each new key is made from (-1: the static block)
    if (frames.empty()) {
        if (orig.keys.empty()) { frames.push_back(0); ticks.push_back(0); base.push_back(-1); }
        for (size_t i = 0; i < orig.keys.size(); ++i) {
            frames.push_back((double)orig.keys[i].time / 1e7 * fps);
            ticks.push_back(orig.keys[i].time);
            base.push_back((int)i);
        }
    } else {
        for (double f : frames) {
            int match = -1;
            for (size_t i = 0; i < orig.keys.size(); ++i)
                if (std::fabs((double)orig.keys[i].time / 1e7 * fps - f) < 1e-4) match = (int)i;
            if (match >= 0) {
                ticks.push_back(orig.keys[(size_t)match].time);
                base.push_back(match);
            } else {
                int64_t t = (int64_t)std::llround(std::max(0.0, f) / fps * 1e7);
                int b = orig.keys.empty() ? -1 : 0;
                for (size_t i = 0; i < orig.keys.size(); ++i)
                    if (orig.keys[i].time <= t) b = (int)i;
                ticks.push_back(t);
                base.push_back(b);
            }
        }
    }

    out.keys.clear();
    for (size_t n = 0; n < frames.size(); ++n) {
        std::map<std::string, std::vector<double>> want;
        for (const auto& m : fx->maps) {
            const Json& p = params.get(m.ofx);
            if (!p.isObject()) continue;
            std::vector<double> v = valueAt(p, frames[n]);
            if (v.size() != (isPos(m.kind) ? 2u : 1u)) continue;
            want[m.ofx] = v;
        }
        ManagedFxKey k;
        const Snap* snap;
        const std::string* xml;
        if (base[n] >= 0) {
            k = orig.keys[(size_t)base[n]];
            snap = &have[(size_t)base[n]];
            xml = &orig.keys[(size_t)base[n]].xml;
        } else {
            snap = &haveStatic;
            xml = &orig.staticXml;
        }
        k.time = ticks[n];
        k.xml = patchXml(*xml, *fx, *snap, want, !bypass, warnings);
        out.keys.push_back(std::move(k));
    }
    if (orig.hasStatic) {
        // the static block follows the first key's settings
        std::map<std::string, std::vector<double>> want;
        for (const auto& m : fx->maps) {
            const Json& p = params.get(m.ofx);
            if (!p.isObject()) continue;
            std::vector<double> v = valueAt(p, frames[0]);
            if (v.size() == (isPos(m.kind) ? 2u : 1u)) want[m.ofx] = v;
        }
        out.staticXml = patchXml(orig.staticXml, *fx, haveStatic, want, !bypass, warnings);
    }
    if (orig.keys.empty() && !orig.hasStatic) return false;
    // without keys in the old record the new one has the one made from the static block
    return true;
}

}  // namespace veg
}  // namespace reno
