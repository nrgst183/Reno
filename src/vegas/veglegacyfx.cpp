// Reading and writing VEGAS's older video effects (Swirl, Pinch/Punch, Wave, TV Simulator, Light Rays): see veglegacyfx.h.
#include "veglegacyfx.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace reno {
namespace veg {

namespace {

enum class Kind { Swirl, Punch, Wave, Tv, LightRays };

// How one of Reno's settings is made from the record's numbers (not Swirl and Pinch/Punch, which have their own code).
enum class Conv { Same, OneMinus, Zoom, Flag };
struct Slot {
    const char* name;
    size_t first;           // the number it starts at
    size_t n;               // how many numbers (a position: 2, a colour: 3)
    Conv conv;
};

struct Effect {
    const char* guid;
    Kind kind;
    const char* pluginId;
    size_t count;                       // numbers in a block
    std::vector<const char*> params;    // Reno's parameters, in the order the state lists them
    std::vector<Slot> slots;            // for the kinds without code of their own
};

const std::vector<Effect>& effects()
{
    static const std::vector<Effect> list = {
        {"b3a8c4cf-e5ad-4b2f-a603-2f0fcb3b546e", Kind::Swirl, "com.vegascreativesoftware:swirl", 6,
         {"Center", "Amount", "ScalingHorizontal", "ScalingVertical", "ScalingProportional"}, {}},
        {"c8e20570-4059-4edf-9cae-eef966df3b19", Kind::Punch, "com.vegascreativesoftware:pinchpunch", 6,
         {"Center", "Amount", "ScalingHorizontal", "ScalingVertical", "ScalingProportional"}, {}},
        {"ec1a2314-0c38-11d2-9aac-00a0c99b12c5", Kind::Wave, "com.vegascreativesoftware:wave", 6,
         {"VerticalWaves", "HorizontalWaves", "VerticalAmplitude", "HorizontalAmplitude", "VerticalPhase", "HorizontalPhase"},
         {{"VerticalWaves", 0, 1, Conv::Same}, {"HorizontalWaves", 1, 1, Conv::Same}, {"VerticalAmplitude", 2, 1, Conv::Same},
          {"HorizontalAmplitude", 3, 1, Conv::Same}, {"VerticalPhase", 4, 1, Conv::Same}, {"HorizontalPhase", 5, 1, Conv::Same}}},
        {"b892d510-9487-4022-b261-738123b55e47", Kind::Tv, "com.vegascreativesoftware:tvsimulator", 8,
         {"DetailZoom", "ApertureGrill", "Interlacing", "LineSync", "VerticalSync", "ScanPhasing", "Phosphorescence", "Static"},
         {{"DetailZoom", 0, 1, Conv::Zoom}, {"ApertureGrill", 1, 1, Conv::Same}, {"Interlacing", 2, 1, Conv::Same},
          {"LineSync", 3, 1, Conv::OneMinus}, {"VerticalSync", 4, 1, Conv::OneMinus}, {"ScanPhasing", 5, 1, Conv::Same},
          {"Phosphorescence", 6, 1, Conv::Same}, {"Static", 7, 1, Conv::Same}}},
        {"1ef9e213-8361-48f6-9911-7221ed1dbc4a", Kind::LightRays, "com.vegascreativesoftware:lightrays", 13,
         {"LightSource", "TintColor", "group0Sensitivity", "group0Strength", "group1BoundRadius", "group1RadiusX", "group1RadiusY", "group1Feather",
          "group2Blend", "group2Noise"},
         {{"LightSource", 0, 2, Conv::Same}, {"group0Strength", 2, 1, Conv::Same}, {"group0Sensitivity", 3, 1, Conv::Same},
          {"group1BoundRadius", 4, 1, Conv::Flag}, {"group1RadiusX", 5, 1, Conv::Same}, {"group1RadiusY", 6, 1, Conv::Same},
          {"group1Feather", 7, 1, Conv::Same}, {"TintColor", 8, 3, Conv::Same}, {"group2Blend", 11, 1, Conv::Same}, {"group2Noise", 12, 1, Conv::Same}}},
    };
    return list;
}

const Effect* effectOfGuid(const std::string& g)
{
    for (const auto& e : effects())
        if (g == e.guid) return &e;
    return nullptr;
}

bool isArea(Kind k) { return k == Kind::Swirl || k == Kind::Punch; }

// How far the centre's numbers go: 1 is the frame edge (GUESSED, see the header).
const double kCenterReach = 0.5;
// TV Simulator's Detail zoom: the record holds the magnification (1 = none); Reno's built-in magnifies by 1 + 1.5 z.
const double kZoomSpan = 1.5;

typedef std::map<std::string, std::vector<double>> Values;

// A yes/no the record keeps as a 64-bit whole number (1 reads as the double 5e-324) in place of a double.
bool flagOf(double d)
{
    uint64_t u = 0;
    std::memcpy(&u, &d, 8);
    return u != 0;
}

double flagBits(bool on)
{
    const uint64_t u = on ? 1 : 0;
    double d = 0;
    std::memcpy(&d, &u, 8);
    return d;
}

double toReno(Conv c, double d)
{
    return c == Conv::OneMinus ? 1.0 - d : c == Conv::Zoom ? (d - 1.0) / kZoomSpan : c == Conv::Flag ? (flagOf(d) ? 1.0 : 0.0) : d;
}

double fromReno(Conv c, double v)
{
    return c == Conv::OneMinus ? 1.0 - v : c == Conv::Zoom ? 1.0 + v * kZoomSpan : c == Conv::Flag ? flagBits(v != 0) : v;
}

// A block of numbers as Reno settings (not Proportional: that depends on all the keys).
Values renoValues(const Effect& fx, const std::vector<double>& d)
{
    Values v;
    if (isArea(fx.kind)) {
        v["Center"] = {0.5 + d[1] * kCenterReach, 0.5 + d[2] * kCenterReach};
        v["Amount"] = {d[0]};
        v["ScalingHorizontal"] = {d[3]};
        v["ScalingVertical"] = {d[4]};
    } else {
        for (const auto& sl : fx.slots) {
            std::vector<double> x;
            for (size_t i = 0; i < sl.n && sl.first + i < d.size(); ++i) x.push_back(toReno(sl.conv, d[sl.first + i]));
            v[sl.name] = x;
        }
    }
    return v;
}

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

bool isWholeParam(const std::string& name) { return name == "ScalingProportional" || name == "group1BoundRadius"; }

// The numbers a record would show for a state's settings at a frame: its key there, the line between the keys
// around it, or its value.
std::vector<double> valueAt(const Json& p, double frame)
{
    std::vector<double> out;
    const Json& k = p.get("k");
    if (k.isArray() && k.size() > 0) {
        const size_t n = k.size();
        const Json* prev = nullptr;
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

}  // namespace

// ------------------------------------------------------------------------------------------ the record
bool isLegacyFxAt(const Bytes& b, size_t off)
{
    if (!b.has(off, 0x38) || b.u32(off) != 0x38) return false;
    const Effect* fx = effectOfGuid(b.guid(off + 4));
    if (!fx) return false;
    const uint32_t staticBytes = b.u32(off + 0x14), nameBytes = b.u32(off + 0x34);
    return (staticBytes == 0 || staticBytes == fx->count * 8) && nameBytes <= 0x1000 && b.has(off + 0x38, (size_t)nameBytes + staticBytes);
}

bool readLegacyFx(const Bytes& b, size_t off, LegacyFxRecord& out)
{
    out = LegacyFxRecord();
    if (!isLegacyFxAt(b, off)) return false;
    out.classGuid = b.guid(off + 4);
    const Effect* fx = effectOfGuid(out.classGuid);
    const uint32_t staticBytes = b.u32(off + 0x14), count = b.u32(off + 0x18), nameBytes = b.u32(off + 0x34);
    if (count > 100000) return false;
    out.flags = b.u32(off + 0x2c);
    out.preset = b.text(off + 0x38, nameBytes);
    size_t p = off + 0x38 + nameBytes;
    for (uint32_t i = 0; i < staticBytes / 8; ++i) out.statik.push_back(b.f64(p + 8 * (size_t)i));
    p += staticBytes;
    const size_t keyBytes = fx->count * 8;
    for (uint32_t i = 0; i < count; ++i) {
        if (!b.has(p, 0x1c + keyBytes) || b.u32(p) != 0x1c || b.u32(p + 0x18) != keyBytes) return false;
        LegacyFxKey k;
        k.time = b.i64(p + 4);
        k.extra.assign((const char*)b.data() + p + 12, 12);
        for (size_t j = 0; j < fx->count; ++j) k.v.push_back(b.f64(p + 0x1c + 8 * j));
        out.keys.push_back(std::move(k));
        p += 0x1c + keyBytes;
    }
    if (out.statik.empty() && out.keys.empty()) return false;
    // only the chunk's padding may follow
    if (b.size() - p > 7) return false;
    for (size_t q = p; q < b.size(); ++q)
        if (b.u8(q) != 0) return false;
    out.end = p;
    return true;
}

std::string writeLegacyFx(const Bytes& original, size_t originalOff, const LegacyFxRecord& rec)
{
    const uint32_t nameBytes = original.u32(originalOff + 0x34);
    std::string s((const char*)original.data() + originalOff, std::min<size_t>(original.size() - originalOff, 0x38 + (size_t)nameBytes));
    setU32(s, 0x14, (uint32_t)rec.statik.size() * 8);
    setU32(s, 0x18, (uint32_t)rec.keys.size());
    setU32(s, 0x2c, rec.flags);
    for (double d : rec.statik) addF64(s, d);
    for (const auto& k : rec.keys) {
        addU32(s, 0x1c);
        addU64(s, (uint64_t)k.time);
        std::string extra = k.extra;
        extra.resize(12, '\0');
        s += extra;
        addU32(s, (uint32_t)k.v.size() * 8);
        for (double d : k.v) addF64(s, d);
    }
    return s;
}

std::string legacyPluginId(const LegacyFxRecord& r)
{
    const Effect* fx = effectOfGuid(r.classGuid);
    return fx ? fx->pluginId : std::string();
}

bool recordBeforePanCrop(const Bytes& b, size_t off)
{
    return b.has(off, 0x30) && (b.u32(off + 0x2c) & 0x80u) != 0;
}

static void setFlagBit(std::string& record, uint32_t bit, bool on)
{
    if (record.size() < 0x30) return;
    uint32_t f = 0;
    std::memcpy(&f, record.data() + 0x2c, 4);
    f = on ? (f | bit) : (f & ~bit);
    setU32(record, 0x2c, f);
}

void setRecordBeforePanCrop(std::string& record, bool on) { setFlagBit(record, 0x80u, on); }

bool legacyBypass(const LegacyFxRecord& r) { return (r.flags & 0x01u) != 0; }

void setLegacyBypass(std::string& record, bool on) { setFlagBit(record, 0x01u, on); }

// ------------------------------------------------------------------------------------------ record -> Reno's settings
bool legacyFxState(const LegacyFxRecord& r, double fps, Json& state, std::vector<std::string>& problems)
{
    const Effect* fx = effectOfGuid(r.classGuid);
    if (!fx) return false;
    // the blocks of numbers: one per key, or the static block if there are no keys
    std::vector<const std::vector<double>*> blocks;
    std::vector<double> frames;
    if (r.keys.empty()) {
        blocks.push_back(&r.statik);
        frames.push_back(0);
    }
    for (const auto& k : r.keys) {
        blocks.push_back(&k.v);
        frames.push_back((double)k.time / 1e7 * fps);
    }
    for (const auto* b : blocks)
        if (b->size() != fx->count) {
            problems.push_back("a block of settings has the wrong size");
            return false;
        }
    std::vector<Values> snaps;
    // (Proportional is one setting for the whole effect: the first block's)
    const bool proportional = isArea(fx->kind) && flagOf((*blocks[0])[5]);
    for (const auto* b : blocks) snaps.push_back(renoValues(*fx, *b));
    Json params = Json::object();
    for (const char* name : fx->params) {
        if (std::strcmp(name, "ScalingProportional") == 0) {
            Json e = Json::object();
            Json v = Json::array();
            v.push(proportional ? 1.0 : 0.0);
            e["v"] = v;
            e["i"] = true;
            params[name] = e;
            continue;
        }
        const auto& first = snaps[0][name];
        Json e = Json::object();
        Json v = Json::array();
        for (double d : first) v.push(d);
        e["v"] = v;
        if (isWholeParam(name)) e["i"] = true;
        bool varies = false;
        for (size_t i = 1; i < snaps.size(); ++i)
            if (!sameValues(snaps[i][name], first)) varies = true;
        if (varies) {
            Json ks = Json::array();
            for (size_t i = 0; i < snaps.size(); ++i) {
                Json key = Json::array();
                key.push(frames[i]);
                for (double d : snaps[i][name]) key.push(d);
                ks.push(key);
            }
            e["k"] = ks;
        }
        params[name] = e;
    }
    state = Json::object();
    state["plugin"] = fx->pluginId;
    state["params"] = params;
    return true;
}

// ------------------------------------------------------------------------------------------ Reno's settings -> record
namespace {

// The block 'base' with the settings of 'want' that differ from it put in. 'propWanted': -1 not in the state, else
// Swirl's and Pinch/Punch's Proportional box.
std::vector<double> blockWith(const Effect& fx, const std::vector<double>& base, const Values& want, int propWanted)
{
    std::vector<double> d = base;
    const Values have = renoValues(fx, base);
    auto differs = [&](const char* name) {
        auto w = want.find(name);
        auto h = have.find(name);
        return w != want.end() && h != have.end() && w->second.size() == h->second.size() && !sameValues(w->second, h->second);
    };
    if (isArea(fx.kind)) {
        if (differs("Amount")) d[0] = want.at("Amount")[0];
        if (differs("Center")) {
            d[1] = (want.at("Center")[0] - 0.5) / kCenterReach;
            d[2] = (want.at("Center")[1] - 0.5) / kCenterReach;
        }
        const bool sizeChanged = differs("ScalingHorizontal");
        if (sizeChanged) d[3] = want.at("ScalingHorizontal")[0];
        bool prop = flagOf(d[5]);
        bool flipped = false;
        if (propWanted >= 0 && prop != (propWanted != 0)) {
            prop = propWanted != 0;
            d[5] = flagBits(prop);
            flipped = true;
        }
        if (prop) {
            // the vertical size follows the horizontal one (Proportional box)
            if ((sizeChanged || flipped) && d[4] != d[3]) d[4] = d[3];
        } else if (differs("ScalingVertical")) {
            d[4] = want.at("ScalingVertical")[0];
        }
    } else {
        for (const auto& sl : fx.slots) {
            if (!differs(sl.name)) continue;
            const std::vector<double>& w = want.at(sl.name);
            for (size_t i = 0; i < sl.n && sl.first + i < d.size() && i < w.size(); ++i) d[sl.first + i] = fromReno(sl.conv, w[i]);
        }
    }
    return d;
}

}  // namespace

bool legacyFxWith(const LegacyFxRecord& orig, const Json& state, double fps, LegacyFxRecord& out, std::vector<std::string>& warnings)
{
    const Effect* fx = effectOfGuid(orig.classGuid);
    if (!fx || !(fps > 0)) return false;
    out = orig;
    const Json& params = state.get("params");
    const Json& propParam = params.get("ScalingProportional");
    const int proportional = isArea(fx->kind) && propParam.isObject() && propParam.get("v").isArray() && propParam.get("v").size() > 0
                                 ? (propParam.get("v").at(0).asDouble(0) != 0 ? 1 : 0)
                                 : -1;

    // new key times: wherever any setting has a key; with none, the old keys' (or the static block alone)
    std::vector<double> frames;
    for (const char* name : fx->params) {
        const Json& k = params.get(name).get("k");
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
    auto wantAt = [&](double frame) {
        Values want;
        for (const char* name : fx->params) {
            const Json& p = params.get(name);
            if (!p.isObject() || std::strcmp(name, "ScalingProportional") == 0) continue;
            const std::vector<double> v = valueAt(p, frame);
            if (!v.empty()) want[name] = v;
        }
        return want;
    };
    if (frames.empty()) {
        // not animated: the old keys with one value per setting, or the static block if there were none
        if (orig.keys.empty()) {
            out.statik = blockWith(*fx, orig.statik, wantAt(0), proportional);
        } else {
            for (size_t i = 0; i < orig.keys.size(); ++i) {
                // (a key the state's lines pass through at another value: the setting's value stands for all keys)
                out.keys[i].v = blockWith(*fx, orig.keys[i].v, wantAt((double)orig.keys[i].time / 1e7 * fps), proportional);
            }
        }
        return true;
    }
    out.keys.clear();
    const std::vector<double> zeros(fx->count, 0.0);
    for (double f : frames) {
        int match = -1;
        for (size_t i = 0; i < orig.keys.size(); ++i)
            if (std::fabs((double)orig.keys[i].time / 1e7 * fps - f) < 1e-4) match = (int)i;
        LegacyFxKey k;
        const std::vector<double>* base = orig.statik.size() == fx->count ? &orig.statik : &zeros;
        if (match >= 0) {
            k = orig.keys[(size_t)match];
            base = &orig.keys[(size_t)match].v;
        } else {
            k.time = (int64_t)std::llround(std::max(0.0, f) / fps * 1e7);
            // based on the last old key before it (or the first)
            int b = orig.keys.empty() ? -1 : 0;
            for (size_t i = 0; i < orig.keys.size(); ++i)
                if (orig.keys[i].time <= k.time) b = (int)i;
            if (b >= 0) {
                k.extra = orig.keys[(size_t)b].extra;
                base = &orig.keys[(size_t)b].v;
            }
        }
        k.v = blockWith(*fx, *base, wantAt(f), proportional);
        out.keys.push_back(std::move(k));
    }
    return true;
}

}  // namespace veg
}  // namespace reno
