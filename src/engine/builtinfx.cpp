// Built-in effects: see builtinfx.h.
#include "builtinfx.h"
#include <algorithm>
#include <cmath>

namespace reno {

const char* const kColorGradeId = "reno.colorgrade";

bool isBuiltinVideo(const FxSlot& s) { return s.kind == "builtin" && s.pluginId == kColorGradeId; }
bool isVideoFxSlot(const FxSlot& s) { return s.kind == "ofx" || isBuiltinVideo(s); }

std::string builtinLabel(const std::string& id) { return id == kColorGradeId ? "Color Grading" : id; }

FxSlot makeBuiltinSlot(const std::string& id, uint64_t slotId)
{
    FxSlot s;
    s.id = slotId;
    s.kind = "builtin";
    s.pluginId = id;
    s.name = builtinLabel(id);
    if (id == kColorGradeId) s.state = ColorGrade().toJson();
    return s;
}

// ------------------------------------------------------------------------------------------ curves
double curveThrough(const std::vector<std::pair<double, double>>& in, double x)
{
    if (in.size() < 2) return x;
    // sorted, one point per x
    std::vector<std::pair<double, double>> p = in;
    std::sort(p.begin(), p.end());
    std::vector<std::pair<double, double>> q;
    for (const auto& pt : p)
        if (q.empty() || pt.first > q.back().first + 1e-9) q.push_back(pt);
    if (q.size() < 2) return x;
    // beyond the ends: straight on at 45 degrees from the end points
    if (x <= q.front().first) return q.front().second + (x - q.front().first);
    if (x >= q.back().first) return q.back().second + (x - q.back().first);
    const size_t n = q.size();
    std::vector<double> d(n - 1), m(n);
    for (size_t k = 0; k + 1 < n; ++k) d[k] = (q[k + 1].second - q[k].second) / (q[k + 1].first - q[k].first);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t k = 1; k + 1 < n; ++k) m[k] = (d[k - 1] * d[k] <= 0) ? 0.0 : (d[k - 1] + d[k]) / 2;
    // Fritsch-Carlson: slopes kept small enough that the curve does not overshoot
    for (size_t k = 0; k + 1 < n; ++k) {
        if (std::fabs(d[k]) < 1e-12) {
            m[k] = m[k + 1] = 0;
            continue;
        }
        const double a = m[k] / d[k], b = m[k + 1] / d[k];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[k] = t * a * d[k];
            m[k + 1] = t * b * d[k];
        }
    }
    size_t k = 0;
    while (k + 2 < n && x > q[k + 1].first) ++k;
    const double h = q[k + 1].first - q[k].first, t = (x - q[k].first) / h;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * q[k].second + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * q[k + 1].second + (t3 - t2) * h * m[k + 1];
}

// ------------------------------------------------------------------------------------------ the colour grade
static const double kPi = 3.14159265358979323846;
static const char* const kCurveNames[4] = {"master", "r", "g", "b"};

static ColorGrade::Wheel wheelFrom(const Json& j, const ColorGrade::Wheel& def)
{
    ColorGrade::Wheel w = def;
    if (!j.isObject()) return w;
    w.x = j.get("x").asDouble(def.x);
    w.y = j.get("y").asDouble(def.y);
    w.master = j.get("master").asDouble(def.master);
    const double r = std::hypot(w.x, w.y);
    if (r > 1) { w.x /= r; w.y /= r; }
    if (!std::isfinite(w.x) || !std::isfinite(w.y)) w.x = w.y = 0;
    if (!std::isfinite(w.master)) w.master = def.master;
    return w;
}

static Json wheelJson(const ColorGrade::Wheel& w)
{
    Json j = Json::object();
    j["x"] = w.x;
    j["y"] = w.y;
    j["master"] = w.master;
    return j;
}

ColorGrade ColorGrade::fromJson(const Json& j)
{
    ColorGrade g;
    if (!j.isObject()) return g;
    auto num = [&](const char* k, double def, double lo, double hi) {
        const double v = j.get(k).asDouble(def);
        return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : def;
    };
    g.exposure = num("exposure", 0, -10, 10);
    g.temperature = num("temperature", 0, -100, 100);
    g.tint = num("tint", 0, -100, 100);
    g.contrast = num("contrast", 1, 0, 4);
    g.saturation = num("saturation", 1, 0, 4);
    g.lift = wheelFrom(j.get("lift"), ColorGrade::Wheel());
    g.gamma = wheelFrom(j.get("gamma"), ColorGrade::Wheel{0, 0, 1});
    g.gain = wheelFrom(j.get("gain"), ColorGrade::Wheel{0, 0, 1});
    g.offset = wheelFrom(j.get("offset"), ColorGrade::Wheel());
    g.gamma.master = std::min(4.0, std::max(0.1, g.gamma.master));
    g.gain.master = std::min(8.0, std::max(0.0, g.gain.master));
    g.lift.master = std::min(1.0, std::max(-1.0, g.lift.master));
    g.offset.master = std::min(1.0, std::max(-1.0, g.offset.master));
    // --- VEGAS Pro 23's panel
    auto lutFrom = [&](const char* key, LutRef& r) {
        const Json& l = j.get(key);
        if (!l.isObject()) return;
        r.source = l.get("source").asString();
        r.strength = std::min(1.0, std::max(0.0, l.get("strength").asDouble(1.0)));
        if (!std::isfinite(r.strength)) r.strength = 1.0;
        r.tetrahedral = l.get("tetrahedral").asBool();
        r.enabled = l.has("enabled") ? l.get("enabled").asBool() : true;
    };
    lutFrom("cameraLut", g.cameraLut);
    lutFrom("lookLut", g.lookLut);
    g.shadows = wheelFrom(j.get("shadows"), ColorGrade::Wheel());
    g.midtones = wheelFrom(j.get("midtones"), ColorGrade::Wheel());
    g.highlights = wheelFrom(j.get("highlights"), ColorGrade::Wheel());
    g.rlOffset = wheelFrom(j.get("rlOffset"), ColorGrade::Wheel());
    for (Wheel* w : {&g.shadows, &g.midtones, &g.highlights, &g.rlOffset}) w->master = std::min(1.0, std::max(-1.0, w->master));
    g.rlLow = num("rlLow", 0.25, 0, 1);
    g.rlHigh = num("rlHigh", 0.75, 0, 1);
    if (g.rlHigh < g.rlLow) std::swap(g.rlLow, g.rlHigh);
    g.blackIn = num("blackIn", 0, -1, 1);
    g.whiteIn = num("whiteIn", 1, 0, 2);
    g.blackOut = num("blackOut", 0, -1, 1);
    g.whiteOut = num("whiteOut", 1, 0, 2);
    g.brightness = num("brightness", 0, -1, 1);
    g.contrastCenter = num("contrastCenter", 0.5, 0, 1);
    g.logExposure = num("logExposure", 0, -5, 5);
    g.hue = num("hue", 0, -1, 1);
    g.hslSaturation = num("hslSaturation", 1, 0, 4);
    g.luminance = num("luminance", 0, -1, 1);
    {
        const Json& hc = j.get("hslCurves");
        static const char* const names[ColorGrade::HslCurveCount] = {"hueVsHue", "hueVsSat", "hueVsLum", "satVsSat", "satVsLum", "lumVsSat"};
        for (int k = 0; k < ColorGrade::HslCurveCount; ++k) {
            const Json& pts = hc.get(names[k]);
            if (!pts.isArray()) continue;
            for (size_t i = 0; i < pts.size() && i < 64; ++i) {
                const Json& pt = pts.at(i);
                if (!pt.isArray() || pt.size() < 2) continue;
                const double x = pt.at(0).asDouble(0), y = pt.at(1).asDouble(0.5);
                if (std::isfinite(x) && std::isfinite(y)) g.hslCurve[k].push_back(std::make_pair(std::min(1.0, std::max(0.0, x)), std::min(1.0, std::max(0.0, y))));
            }
            std::sort(g.hslCurve[k].begin(), g.hslCurve[k].end());
        }
    }
    const Json& c = j.get("curves");
    for (int k = 0; k < 4; ++k) {
        const Json& pts = c.get(kCurveNames[k]);
        if (!pts.isArray()) continue;
        for (size_t i = 0; i < pts.size() && i < 64; ++i) {
            const Json& pt = pts.at(i);
            if (!pt.isArray() || pt.size() < 2) continue;
            const double x = pt.at(0).asDouble(0), y = pt.at(1).asDouble(0);
            if (std::isfinite(x) && std::isfinite(y)) g.curve[k].push_back(std::make_pair(std::min(1.0, std::max(0.0, x)), std::min(1.5, std::max(-0.5, y))));
        }
        std::sort(g.curve[k].begin(), g.curve[k].end());
    }
    return g;
}

Json ColorGrade::toJson() const
{
    Json j = Json::object();
    j["exposure"] = exposure;
    j["temperature"] = temperature;
    j["tint"] = tint;
    j["contrast"] = contrast;
    j["saturation"] = saturation;
    j["lift"] = wheelJson(lift);
    j["gamma"] = wheelJson(gamma);
    j["gain"] = wheelJson(gain);
    j["offset"] = wheelJson(offset);
    Json c = Json::object();
    for (int k = 0; k < 4; ++k) {
        Json pts = Json::array();
        for (const auto& p : curve[k]) {
            Json pt = Json::array();
            pt.push(p.first);
            pt.push(p.second);
            pts.push(pt);
        }
        c[kCurveNames[k]] = pts;
    }
    j["curves"] = c;
    // VEGAS Pro 23's panel: written only when set, so older grades stay as they were.
    auto lutJson = [&](const char* key, const LutRef& r) {
        if (r == LutRef() && r.source.empty()) return;
        Json l = Json::object();
        l["source"] = r.source;
        l["strength"] = r.strength;
        l["tetrahedral"] = r.tetrahedral;
        l["enabled"] = r.enabled;
        j[key] = l;
    };
    lutJson("cameraLut", cameraLut);
    lutJson("lookLut", lookLut);
    auto wheelIf = [&](const char* key, const Wheel& w) {
        if (std::fabs(w.x) > 1e-12 || std::fabs(w.y) > 1e-12 || std::fabs(w.master) > 1e-12) j[key] = wheelJson(w);
    };
    wheelIf("shadows", shadows);
    wheelIf("midtones", midtones);
    wheelIf("highlights", highlights);
    wheelIf("rlOffset", rlOffset);
    auto numIf = [&](const char* key, double v, double neutral) { if (std::fabs(v - neutral) > 1e-12) j[key] = v; };
    numIf("rlLow", rlLow, 0.25);
    numIf("rlHigh", rlHigh, 0.75);
    numIf("blackIn", blackIn, 0);
    numIf("whiteIn", whiteIn, 1);
    numIf("blackOut", blackOut, 0);
    numIf("whiteOut", whiteOut, 1);
    numIf("brightness", brightness, 0);
    numIf("contrastCenter", contrastCenter, 0.5);
    numIf("logExposure", logExposure, 0);
    numIf("hue", hue, 0);
    numIf("hslSaturation", hslSaturation, 1);
    numIf("luminance", luminance, 0);
    {
        static const char* const names[HslCurveCount] = {"hueVsHue", "hueVsSat", "hueVsLum", "satVsSat", "satVsLum", "lumVsSat"};
        Json hc = Json::object();
        bool any = false;
        for (int k = 0; k < HslCurveCount; ++k) {
            if (hslCurve[k].empty()) continue;
            Json pts = Json::array();
            for (const auto& p : hslCurve[k]) {
                Json pt = Json::array();
                pt.push(p.first);
                pt.push(p.second);
                pts.push(pt);
            }
            hc[names[k]] = pts;
            any = true;
        }
        if (any) j["hslCurves"] = hc;
    }
    return j;
}

static bool straight(const std::vector<std::pair<double, double>>& c)
{
    for (const auto& p : c)
        if (std::fabs(p.first - p.second) > 1e-9) return false;
    return true;
}

bool ColorGrade::isIdentity() const
{
    auto neutral = [](const Wheel& w, double m) { return std::fabs(w.x) < 1e-9 && std::fabs(w.y) < 1e-9 && std::fabs(w.master - m) < 1e-9; };
    bool curvesStraight = true;
    for (const auto& c : curve) curvesStraight = curvesStraight && straight(c);
    bool hslFlat = true;
    for (const auto& c : hslCurve) hslFlat = hslFlat && c.empty();
    if (cameraLut.active() || lookLut.active() || !hslFlat) return false;
    if (!neutral(shadows, 0) || !neutral(midtones, 0) || !neutral(highlights, 0) || !neutral(rlOffset, 0)) return false;
    if (std::fabs(blackIn) > 1e-9 || std::fabs(whiteIn - 1) > 1e-9 || std::fabs(blackOut) > 1e-9 || std::fabs(whiteOut - 1) > 1e-9 ||
        std::fabs(brightness) > 1e-9 || std::fabs(logExposure) > 1e-9 || std::fabs(hue) > 1e-9 || std::fabs(hslSaturation - 1) > 1e-9 ||
        std::fabs(luminance) > 1e-9)
        return false;
    return std::fabs(exposure) < 1e-9 && std::fabs(temperature) < 1e-9 && std::fabs(tint) < 1e-9 && std::fabs(contrast - 1) < 1e-9 &&
           std::fabs(saturation - 1) < 1e-9 && neutral(lift, 0) && neutral(gamma, 1) && neutral(gain, 1) && neutral(offset, 0) && curvesStraight;
}

// How much a wheel's point pushes each channel: the point's colour (red at the top, green at the lower left, blue at
// the lower right), -radius .. radius, summing to nothing.
static void wheelPush(const ColorGrade::Wheel& w, double out[3])
{
    const double r = std::hypot(w.x, w.y);
    if (r < 1e-12) { out[0] = out[1] = out[2] = 0; return; }
    const double a = std::atan2(w.y, w.x);
    const double phase[3] = {90.0, 210.0, 330.0};
    for (int c = 0; c < 3; ++c) out[c] = r * std::cos(a - phase[c] * kPi / 180.0);
}

// ---- HSL helpers and the HSL curves (VEGAS Pro 23's Color Grading panel)
static void rgbToHsl(float r, float g, float b, float& h, float& sat, float& l)
{
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d < 1e-7f) { h = 0; sat = 0; return; }
    sat = l > 0.5f ? d / std::max(1e-7f, 2.f - mx - mn) : d / std::max(1e-7f, mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / d + 2.f;
    else h = (r - g) / d + 4.f;
    h /= 6.f;
}
static float hueToRgb(float p, float q, float t)
{
    t -= std::floor(t);
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}
static void hslToRgb(float h, float sat, float l, float& r, float& g, float& b)
{
    if (sat <= 0.f) { r = g = b = l; return; }
    const float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat, p = 2 * l - q;
    r = hueToRgb(p, q, h + 1.f / 3);
    g = hueToRgb(p, q, h);
    b = hueToRgb(p, q, h - 1.f / 3);
}

// An HSL curve at x (0.5 where it has no points; one point: that value everywhere).
static double hslCurveAt(const std::vector<std::pair<double, double>>& pts, double x, bool cyclic)
{
    if (pts.empty()) return 0.5;
    if (pts.size() == 1) return pts[0].second;
    std::vector<std::pair<double, double>> e;
    if (cyclic) {
        const bool zero = pts.front().first <= 1e-9;
        for (int k = -1; k <= 1; ++k)
            for (const auto& p : pts)
                if (!(zero && p.first >= 1.0 - 1e-9)) e.push_back(std::make_pair(p.first + k, p.second));
        std::sort(e.begin(), e.end());
    } else {
        e = pts;
        if (e.front().first > 1e-9) e.insert(e.begin(), std::make_pair(0.0, e.front().second));
        if (e.back().first < 1.0 - 1e-9) e.push_back(std::make_pair(1.0, e.back().second));
    }
    return std::min(1.0, std::max(0.0, curveThrough(e, x)));
}

void ColorGrade::prepare()
{
    for (int k = 0; k < 4; ++k) {
        lut_[k].clear();
        if (curve[k].size() < 2 || straight(curve[k])) continue;
        lut_[k].resize(1025);
        for (int i = 0; i <= 1024; ++i) lut_[k][(size_t)i] = (float)curveThrough(curve[k], i / 1024.0);
    }
    hasHsl_ = std::fabs(hue) > 1e-9 || std::fabs(hslSaturation - 1) > 1e-9 || std::fabs(luminance) > 1e-9;
    for (int k = 0; k < HslCurveCount; ++k) {
        hslLut_[k].clear();
        if (hslCurve[k].empty()) continue;
        hslLut_[k].resize(1025);
        for (int i = 0; i <= 1024; ++i) hslLut_[k][(size_t)i] = (float)hslCurveAt(hslCurve[k], i / 1024.0, k == HueVsHue || k == HueVsSat || k == HueVsLum);
        hasHsl_ = true;
    }
    auto flat = [](const Wheel& w) { return std::fabs(w.x) < 1e-12 && std::fabs(w.y) < 1e-12 && std::fabs(w.master) < 1e-12; };
    hasRl_ = !(flat(shadows) && flat(midtones) && flat(highlights) && flat(rlOffset));
    auto resolve = [](const LutRef& r) -> std::shared_ptr<const Lut3D> {
        if (!r.active()) return nullptr;
        if (r.source.compare(0, 7, "preset:") == 0) return lutOfPreset(r.source.substr(7), 33);
        if (r.source.compare(0, 5, "file:") == 0) return readCubeLut(r.source.substr(5), nullptr);
        return nullptr;
    };
    camera_ = resolve(cameraLut);
    look_ = resolve(lookLut);
    prepared_ = true;
}

static inline float lutAt(const std::vector<float>& lut, float x)
{
    if (lut.empty()) return x;
    if (x <= 0.f) return lut[0] + x;
    if (x >= 1.f) return lut[1024] + (x - 1.f);
    const float f = x * 1024.f;
    const int i = std::min(1023, (int)f);
    const float t = f - (float)i;
    return lut[(size_t)i] * (1.f - t) + lut[(size_t)i + 1] * t;
}
// An HSL curve's table at x in 0..1 (0.5 where it is empty)
static inline float hslLutAt(const std::vector<float>& lut, float x)
{
    if (lut.empty()) return 0.5f;
    x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    const float f = x * 1024.f;
    const int i = std::min(1023, (int)f);
    const float t = f - (float)i;
    return lut[(size_t)i] * (1.f - t) + lut[(size_t)i + 1] * t;
}

void ColorGrade::apply(float c[3]) const
{
    // the camera LUT turns the footage's own colour space into one to work in
    if (camera_) {
        float in[3] = {std::min(1.f, std::max(0.f, c[0])), std::min(1.f, std::max(0.f, c[1])), std::min(1.f, std::max(0.f, c[2]))};
        float out[3] = {in[0], in[1], in[2]};
        camera_->apply(out, cameraLut.tetrahedral);
        const float s = (float)cameraLut.strength;
        for (int k = 0; k < 3; ++k) c[k] = in[k] + (out[k] - in[k]) * s;
    }
    // exposure and white balance
    const float ex = (float)std::pow(2.0, exposure);
    const float wb[3] = {(float)(1.0 + 0.25 * temperature / 100.0), (float)(1.0 - 0.25 * tint / 100.0), (float)(1.0 - 0.25 * temperature / 100.0)};
    for (int k = 0; k < 3; ++k) c[k] *= ex * wb[k];
    // utilities: log exposure (a stop on a 14 stop log signal), levels in, brightness
    if (std::fabs(logExposure) > 1e-9)
        for (int k = 0; k < 3; ++k) c[k] += (float)(logExposure / 14.0);
    if (std::fabs(blackIn) > 1e-9 || std::fabs(whiteIn - 1) > 1e-9) {
        const double span = std::fabs(whiteIn - blackIn) > 1e-6 ? whiteIn - blackIn : 1e-6;
        for (int k = 0; k < 3; ++k) c[k] = (float)((c[k] - blackIn) / span);
    }
    if (std::fabs(brightness) > 1e-9)
        for (int k = 0; k < 3; ++k) c[k] += (float)brightness;
    // the wheels
    double pl[3], pg[3], pn[3], po[3];
    wheelPush(lift, pl);
    wheelPush(gamma, pg);
    wheelPush(gain, pn);
    wheelPush(offset, po);
    for (int k = 0; k < 3; ++k) {
        const double L = lift.master + 0.25 * pl[k];
        const double Gm = std::max(0.05, gamma.master * std::pow(2.0, 0.5 * pg[k]));
        const double G = std::max(0.0, gain.master * (1.0 + 0.5 * pn[k]));
        const double O = offset.master + 0.2 * po[k];
        double v = G * (c[k] + L * (1.0 - c[k]));
        v = v > 0 ? std::pow(v, 1.0 / Gm) : v;
        c[k] = (float)(v + O);
    }
    // the range-limited wheels: each weighted by where the colour's luma is against the two thresholds
    if (hasRl_) {
        const float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        auto step = [](double v, double edge) { const double t = std::min(1.0, std::max(0.0, (v - edge + 0.1) / 0.2)); return t * t * (3 - 2 * t); };
        const double sLo = step(y, rlLow), sHi = step(y, rlHigh);
        const double wS = 1.0 - sLo, wM = std::max(0.0, sLo - sHi), wH = sHi;
        const double wO = step(y, 0.0) * (1.0 - step(y, 1.1));
        const Wheel* ws[4] = {&shadows, &midtones, &highlights, &rlOffset};
        const double ww[4] = {wS, wM, wH, wO};
        for (int w = 0; w < 4; ++w) {
            if (ww[w] <= 0) continue;
            double pu[3];
            wheelPush(*ws[w], pu);
            for (int k = 0; k < 3; ++k) c[k] += (float)(ww[w] * (0.3 * pu[k] + 0.5 * ws[w]->master));
        }
    }
    // contrast about its pivot, then saturation about the luma
    if (std::fabs(contrast - 1) > 1e-9)
        for (int k = 0; k < 3; ++k) c[k] = (float)((c[k] - contrastCenter) * contrast + contrastCenter);
    if (std::fabs(saturation - 1) > 1e-9) {
        const float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        for (int k = 0; k < 3; ++k) c[k] = y + (c[k] - y) * (float)saturation;
    }
    // curves: the master curve, then each channel's own
    if (!prepared_) {
        for (int k = 0; k < 3; ++k) {
            c[k] = (float)curveThrough(curve[0], c[k]);
            c[k] = (float)curveThrough(curve[k + 1], c[k]);
        }
    } else {
        for (int k = 0; k < 3; ++k) c[k] = lutAt(lut_[k + 1], lutAt(lut_[0], c[k]));
    }
    // levels out
    if (std::fabs(blackOut) > 1e-9 || std::fabs(whiteOut - 1) > 1e-9)
        for (int k = 0; k < 3; ++k) c[k] = (float)(blackOut + c[k] * (whiteOut - blackOut));
    // Utilities' HSL, then the HSL curves
    if (prepared_ && hasHsl_) {
        float h, sat, l;
        rgbToHsl(std::min(1.f, std::max(0.f, c[0])), std::min(1.f, std::max(0.f, c[1])), std::min(1.f, std::max(0.f, c[2])), h, sat, l);
        h += (float)hue;
        sat = std::min(1.f, std::max(0.f, sat * (float)hslSaturation));
        l += (float)luminance;
        h -= std::floor(h);
        const float h0 = h;
        if (!hslLut_[HueVsHue].empty()) h += (hslLutAt(hslLut_[HueVsHue], h0) - 0.5f);
        if (!hslLut_[HueVsSat].empty()) sat *= 2.f * hslLutAt(hslLut_[HueVsSat], h0);
        if (!hslLut_[HueVsLum].empty()) l += (hslLutAt(hslLut_[HueVsLum], h0) - 0.5f) * 0.5f;
        if (!hslLut_[SatVsSat].empty()) sat *= 2.f * hslLutAt(hslLut_[SatVsSat], sat);
        if (!hslLut_[SatVsLum].empty()) l += (hslLutAt(hslLut_[SatVsLum], sat) - 0.5f) * 0.5f;
        if (!hslLut_[LumVsSat].empty()) sat *= 2.f * hslLutAt(hslLut_[LumVsSat], l);
        sat = std::min(1.f, std::max(0.f, sat));
        hslToRgb(h - std::floor(h), sat, std::min(1.f, std::max(0.f, l)), c[0], c[1], c[2]);
    }
    // the look LUT last
    if (look_) {
        float in[3] = {std::min(1.f, std::max(0.f, c[0])), std::min(1.f, std::max(0.f, c[1])), std::min(1.f, std::max(0.f, c[2]))};
        float out[3] = {in[0], in[1], in[2]};
        look_->apply(out, lookLut.tetrahedral);
        const float s = (float)lookLut.strength;
        for (int k = 0; k < 3; ++k) c[k] = in[k] + (out[k] - in[k]) * s;
    }
}

// ---- the panel's pickers
static void solveChannels(const double want[3], double out[2])
{
    // the wheel point (x, y) whose push on the three channels is closest to 'want' (they sum to nothing there)
    const double phase[3] = {90.0, 210.0, 330.0};
    double x = 0, y = 0;
    for (int k = 0; k < 3; ++k) {
        x += want[k] * std::cos(phase[k] * kPi / 180.0);
        y += want[k] * std::sin(phase[k] * kPi / 180.0);
    }
    x *= 2.0 / 3.0;
    y *= 2.0 / 3.0;
    const double r = std::hypot(x, y);
    if (r > 1) { x /= r; y /= r; }
    out[0] = x;
    out[1] = y;
}

void ColorGrade::setBlackPoint(const float rgb[3])
{
    // Lift: out = gain x (c + L x (1 - c)); the picked colour should come out 0, so L = -c / (1 - c) for each channel
    double L[3], mean = 0;
    for (int k = 0; k < 3; ++k) {
        const double c = std::min(0.95, std::max(0.0, (double)rgb[k]));
        L[k] = -c / (1.0 - c);
        mean += L[k] / 3.0;
    }
    double want[3];
    for (int k = 0; k < 3; ++k) want[k] = (L[k] - mean) / 0.25;
    double xy[2];
    solveChannels(want, xy);
    lift.x = xy[0];
    lift.y = xy[1];
    lift.master = std::min(1.0, std::max(-1.0, mean));
}

void ColorGrade::setWhitePoint(const float rgb[3])
{
    // Gain: the picked colour should come out 1, so G = 1 / c for each channel
    double G[3], mean = 0;
    for (int k = 0; k < 3; ++k) {
        const double c = std::max(0.05, (double)rgb[k]);
        G[k] = 1.0 / c;
        mean += G[k] / 3.0;
    }
    mean = std::min(8.0, std::max(0.0, mean));
    double want[3];
    for (int k = 0; k < 3; ++k) want[k] = mean > 1e-9 ? (G[k] / mean - 1.0) / 0.5 : 0.0;
    double xy[2];
    solveChannels(want, xy);
    gain.x = xy[0];
    gain.y = xy[1];
    gain.master = mean;
}

void ColorGrade::setNeutralTone(const float rgb[3])
{
    // The white balance multiplies (1 + a, 1 - b, 1 - a) with a = 0.25 T / 100 and b = 0.25 tint / 100: pick T and
    // tint that make the picked colour grey
    const double r = std::max(0.02, (double)rgb[0]), g = std::max(0.02, (double)rgb[1]), b = std::max(0.02, (double)rgb[2]);
    double a = (b - r) / (b + r);                          // r (1 + a) = b (1 - a)
    a = std::min(1.0, std::max(-1.0, a));
    double bb = 1.0 - r * (1.0 + a) / g;                   // g (1 - b) = r (1 + a)
    bb = std::min(1.0, std::max(-1.0, bb));
    temperature = std::min(100.0, std::max(-100.0, a / 0.25 * 100.0));
    tint = std::min(100.0, std::max(-100.0, bb / 0.25 * 100.0));
}

bool applyBuiltin(const FxSlot& s, Image& img)
{
    if (s.pluginId != kColorGradeId) return false;
    ColorGrade g = ColorGrade::fromJson(s.state);
    if (g.isIdentity()) return true;
    g.prepare();
    const bool fl = img.depth == PixelDepth::Float;
    for (int y = 0; y < img.h; ++y) {
        if (fl) {
            float* p = (float*)img.row(y);
            for (int x = 0; x < img.w; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0.f) continue;
                float c[3] = {p[0] / a, p[1] / a, p[2] / a};
                g.apply(c);
                for (int k = 0; k < 3; ++k) p[k] = c[k] * a;
            }
        } else {
            uint8_t* p = img.row(y);
            for (int x = 0; x < img.w; ++x, p += 4) {
                const int a = p[3];
                if (a == 0) continue;
                const float inv = 1.f / (float)a;
                float c[3] = {p[2] * inv, p[1] * inv, p[0] * inv};
                g.apply(c);
                for (int k = 0; k < 3; ++k) {
                    const float v = std::min(1.f, std::max(0.f, c[k])) * (float)a + 0.5f;
                    p[2 - k] = (uint8_t)std::min(255.f, v);
                }
            }
        }
    }
    return true;
}

ImagePtr applyRedEye(const ImagePtr& img, const std::vector<MediaItem::RedEyeArea>& areas)
{
    if (!img || areas.empty() || img->w <= 0 || img->h <= 0) return img;
    ImagePtr out = img->clone();
    if (!out) return img;
    const bool flt = out->depth == PixelDepth::Float;
    for (const MediaItem::RedEyeArea& a : areas) {
        if (!(a.w > 0 && a.h > 0)) continue;
        const double cx = (a.x + a.w / 2) * out->w, cy = (a.y + a.h / 2) * out->h;
        const double rx = a.w / 2 * out->w, ry = a.h / 2 * out->h;
        if (rx < 0.5 || ry < 0.5) continue;
        const int x0 = std::max(0, (int)std::floor(cx - rx)), x1 = std::min(out->w - 1, (int)std::ceil(cx + rx));
        const int y0 = std::max(0, (int)std::floor(cy - ry)), y1 = std::min(out->h - 1, (int)std::ceil(cy + ry));
        for (int y = y0; y <= y1; ++y) {
            uint8_t* row = out->row(out->h - 1 - y);
            for (int x = x0; x <= x1; ++x) {
                const double dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
                const double d = dx * dx + dy * dy;
                if (d >= 1.0) continue;
                const double edge = d < 0.7 ? 1.0 : (1.0 - d) / 0.3;     // softened towards the ellipse's edge
                float r, g, b;
                if (flt) {
                    float* px = (float*)row + (size_t)x * 4;
                    r = px[0]; g = px[1]; b = px[2];
                } else {
                    uint8_t* px = row + (size_t)x * 4;
                    b = px[0] / 255.f; g = px[1] / 255.f; r = px[2] / 255.f;
                }
                const float other = std::max(g, b);
                // how red: nothing until red is half as much again as the others, all of it at twice
                const float ratio = other > 1e-4f ? r / other : (r > 0.05f ? 10.f : 0.f);
                if (ratio <= 1.5f || r < 0.08f) continue;
                const double amount = std::min(1.0, (ratio - 1.5) / 0.5) * edge;
                const float target = (g + b) * 0.5f;
                const float nr = (float)(r + (target - r) * amount);
                if (flt) ((float*)row)[(size_t)x * 4 + 0] = nr;
                else row[(size_t)x * 4 + 2] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, nr)) * 255.f);
            }
        }
    }
    return out;
}

}  // namespace reno
