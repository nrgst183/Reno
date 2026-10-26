#include "fxcompare.h"
#include "builtins.h"
#include "probe.h"
#include "ofxinternal.h"
#include "../core/process.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <map>

namespace reno {
namespace ofx {

namespace fs = std::filesystem;

// ====================================================================================================== small things
namespace {

bool containsNoCase(const std::string& hay, const std::string& needle)
{
    return needle.empty() || toLower(hay).find(toLower(needle)) != std::string::npos;
}

// "OfxImageEffectContextFilter" -> "Filter"
std::string shortContext(const std::string& c)
{
    const std::string pre = "OfxImageEffectContext";
    return c.compare(0, pre.size(), pre) == 0 ? c.substr(pre.size()) : c;
}

// A name that is safe as part of a file name.
std::string fileSafe(const std::string& s)
{
    std::string o;
    for (char c : s) o += (std::isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
    return o.size() > 90 ? o.substr(0, 90) : o;
}

void makeDirs(const std::string& dir)
{
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
}

void removeFile(const std::string& path)
{
    std::error_code ec;
    fs::remove(fs::u8path(path), ec);
}

// Written whole and then moved into place: a reader (the run that started this process) never sees half of it.
void writeJsonFile(const std::string& path, const Json& j)
{
    const std::string tmp = path + ".part";
    if (!writeFile(tmp, j.dump(1))) return;
    std::error_code ec;
    fs::rename(fs::u8path(tmp), fs::u8path(path), ec);
    if (ec) writeFile(path, j.dump(1));
}

Json readJsonFile(const std::string& path)
{
    std::string text;
    Json j;
    if (readFile(path, text) && !text.empty()) Json::parse(text, j);
    return j;
}

bool parseSize(const std::string& v, int* w, int* h)
{
    int a = 0, b = 0;
    if (std::sscanf(v.c_str(), "%dx%d", &a, &b) != 2 || a < 16 || b < 16 || a > 8192 || b > 8192) return false;
    *w = a;
    *h = b;
    return true;
}

}  // namespace

// ====================================================================================================== PNG
namespace {
uint32_t crc32Of(const uint8_t* p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t k = 0; k < 256; ++k) {
            uint32_t c = k;
            for (int i = 0; i < 8; ++i) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[k] = c;
        }
        made = true;
    }
    crc = ~crc;
    for (size_t k = 0; k < n; ++k) crc = table[(crc ^ p[k]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
void put32(std::string& o, uint32_t v)
{
    o += (char)(v >> 24);
    o += (char)(v >> 16);
    o += (char)(v >> 8);
    o += (char)v;
}
void chunk(std::string& o, const char* type, const std::string& data)
{
    put32(o, (uint32_t)data.size());
    std::string body(type, 4);
    body += data;
    o += body;
    put32(o, crc32Of((const uint8_t*)body.data(), body.size()));
}
}  // namespace

std::string encodePng(int w, int h, const std::vector<uint8_t>& rgb)
{
    std::string out("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr += (char)8;    // bits
    ihdr += (char)2;    // RGB
    ihdr += (char)0;
    ihdr += (char)0;
    ihdr += (char)0;
    chunk(out, "IHDR", ihdr);
    std::string raw;
    raw.reserve((size_t)h * ((size_t)w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw += (char)0;
        raw.append((const char*)rgb.data() + (size_t)y * w * 3, (size_t)w * 3);
    }
    std::string z;
    z += (char)0x78;
    z += (char)0x01;
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z += (char)(last ? 1 : 0);
        z += (char)(n & 0xFF);
        z += (char)(n >> 8);
        z += (char)(~n & 0xFF);
        z += (char)((~n >> 8) & 0xFF);
        z.append(raw, pos, n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    put32(z, (b << 16) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", std::string());
    return out;
}

// ====================================================================================================== presets
namespace {
std::string unescapeXml(std::string s)
{
    static const std::pair<const char*, char> ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    for (const auto& e : ents) {
        size_t at = 0;
        const size_t n = std::strlen(e.first);
        while ((at = s.find(e.first, at)) != std::string::npos) {
            s.replace(at, n, 1, e.second);
            ++at;
        }
    }
    return s;
}

std::string attributeOf(const std::string& tag, const char* name)
{
    const std::string key = std::string(name) + "=\"";
    size_t at = 0;
    while ((at = tag.find(key, at)) != std::string::npos) {
        if (at == 0 || tag[at - 1] == ' ' || tag[at - 1] == '\t' || tag[at - 1] == '\n' || tag[at - 1] == '\r') {
            const size_t from = at + key.size();
            const size_t to = tag.find('"', from);
            return to == std::string::npos ? std::string() : unescapeXml(tag.substr(from, to - from));
        }
        at += key.size();
    }
    return std::string();
}
}  // namespace

std::vector<PresetEntry> parsePresetPackage(const std::string& xml)
{
    std::vector<PresetEntry> out;
    size_t at = 0;
    while ((at = xml.find("<OfxPreset", at)) != std::string::npos) {
        const char next = at + 10 < xml.size() ? xml[at + 10] : 0;
        if (next != ' ' && next != '>' && next != '\t' && next != '\n' && next != '\r') { at += 10; continue; }     // (OfxPresetPackage)
        const size_t tagEnd = xml.find('>', at);
        if (tagEnd == std::string::npos) break;
        const size_t close = xml.find("</OfxPreset>", tagEnd);
        const size_t end = close == std::string::npos ? xml.size() : close;
        const std::string tag = xml.substr(at, tagEnd - at);
        PresetEntry e;
        e.plugin = attributeOf(tag, "plugin");
        e.context = attributeOf(tag, "context");
        e.name = attributeOf(tag, "name");
        size_t p = tagEnd;
        while (p < end) {
            const size_t t = xml.find("<OfxParamType", p);
            if (t == std::string::npos || t >= end) break;
            const size_t te = xml.find('>', t);
            if (te == std::string::npos || te >= end) break;
            const std::string ptag = xml.substr(t, te - t);
            const std::string type = ptag.substr(1, ptag.find_first_of(" \t\r\n", 1) == std::string::npos ? std::string::npos : ptag.find_first_of(" \t\r\n", 1) - 1);
            PresetValue v;
            v.name = attributeOf(ptag, "name");
            const size_t vs = xml.find("<OfxParamValue>", te);
            const size_t ve = vs == std::string::npos ? vs : xml.find("</OfxParamValue>", vs);
            p = te + 1;
            if (vs == std::string::npos || ve == std::string::npos || ve > end) continue;
            const std::string text = unescapeXml(xml.substr(vs + 15, ve - vs - 15));
            p = ve;
            v.isText = type == "OfxParamTypeString" || type == "OfxParamTypeCustom";
            if (v.isText) v.text = text;
            else {
                const char* s = text.c_str();
                char* endp = nullptr;
                // (a Boolean's value is the word: VEGAS's files say true / false)
                if (text == "true" || text == "false") v.numbers.push_back(text == "true" ? 1.0 : 0.0);
                else for (;;) {
                    const double d = std::strtod(s, &endp);
                    if (endp == s) break;
                    v.numbers.push_back(std::isfinite(d) ? d : 0.0);
                    s = endp;
                }
            }
            if (!v.name.empty()) e.values.push_back(v);
        }
        if (!e.name.empty() && !e.plugin.empty()) out.push_back(e);
        at = end;
    }
    return out;
}

// ====================================================================================================== the test picture
namespace {

// What a frame of an effect's run is fed: the moving test picture (a transition also gets a second one).
ImagePtr sourceFrame(int w, int h, double t, bool second) { return makeTestPattern(w, h, second ? t + 40.0 : t); }

bool sameBytes(const Image& a, const Image& b)
{
    return a.w == b.w && a.h == b.h && a.depth == b.depth && std::memcmp(a.data.get(), b.data.get(), a.byteSize()) == 0;
}

// Nothing at all drawn: every pixel fully transparent.
bool isTransparent(const Image& img)
{
    if (!img.data.get()) return true;
    const size_t px = img.pixelBytes();
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* r = img.row(y);
        for (int x = 0; x < img.w; ++x)
            if (r[(size_t)x * px + 3] != 0) return false;
    }
    return true;
}

// One flat colour: every pixel the same.
bool isBlank(const Image& img)
{
    const uint8_t* first = img.data.get();
    if (!first) return true;
    const size_t px = img.pixelBytes();
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* r = img.row(y);
        for (int x = 0; x < img.w; ++x)
            if (std::memcmp(r + (size_t)x * px, first, px) != 0) return false;
    }
    return true;
}

// ====================================================================================================== scenarios
// A scenario is a way of setting an effect's settings: { "name", "kind", "frames", "set": { setting: [numbers] | "text" } }
Json makeScenario(const std::string& name, const std::string& kind, int frames)
{
    Json s = Json::object();
    s["name"] = name;
    s["kind"] = kind;
    s["frames"] = frames;
    s["set"] = Json::object();
    return s;
}

// The numbers a setting can take, one range per number: what the effect shows (display range) else what it allows,
// else something around its default.
void rangeOf(const Param* p, int k, double& lo, double& hi)
{
    const double def = (size_t)k < p->value.size() ? p->value[(size_t)k] : 0.0;
    if (p->kind == ParamKind::Bool) { lo = 0; hi = 1; return; }
    if (p->kind == ParamKind::Choice) {
        const int n = (int)p->choices().size();
        lo = 0;
        hi = std::max(0, n - 1);
        return;
    }
    auto usable = [](double v) { return std::isfinite(v) && std::fabs(v) < 1.0e6; };
    const double dmin = p->props.getDouble(kOfxParamPropDisplayMin, k, -DBL_MAX), dmax = p->props.getDouble(kOfxParamPropDisplayMax, k, DBL_MAX);
    const double pmin = p->props.getDouble(kOfxParamPropMin, k, -DBL_MAX), pmax = p->props.getDouble(kOfxParamPropMax, k, DBL_MAX);
    lo = usable(dmin) ? dmin : (usable(pmin) ? pmin : def - std::max(1.0, std::fabs(def)));
    hi = usable(dmax) ? dmax : (usable(pmax) ? pmax : def + std::max(1.0, std::fabs(def)));
    if (usable(pmin)) lo = std::max(lo, pmin);
    if (usable(pmax)) hi = std::min(hi, pmax);
    if (!(hi > lo)) hi = lo + 1.0;
}

bool isNumberSetting(const Param* p)
{
    return p->dim > 0 && !p->isText() && !p->secret() && p->name != kOfxImageEffectTransitionParamName;
}

Json numbersJson(const std::vector<double>& v)
{
    Json a = Json::array();
    for (double d : v) a.push(d);
    return a;
}

// The scenarios for an effect, from an instance of it (its ranges) and the presets it comes with.
Json makeScenarios(Instance& inst, const std::vector<PresetEntry>& presets, const std::string& context, int frames, bool quick, int maxParams)
{
    Json all = Json::array();
    all.push(makeScenario("defaults", "default", frames));
    std::vector<Param*> numeric;
    for (const auto& p : inst.params.params)
        if (isNumberSetting(p.get())) numeric.push_back(p.get());
    if (!numeric.empty()) {
        Json mn = makeScenario("every setting at its smallest", "min", frames), mid = makeScenario("every setting in the middle", "mid", frames),
             mx = makeScenario("every setting at its largest", "max", frames);
        for (const Param* p : numeric) {
            std::vector<double> a, b, c;
            for (int k = 0; k < p->dim; ++k) {
                double lo, hi;
                rangeOf(p, k, lo, hi);
                double m = (lo + hi) * 0.5;
                if (p->isWhole()) m = std::floor(m + 0.5);
                a.push_back(lo);
                b.push_back(m);
                c.push_back(hi);
            }
            mn["set"][p->name] = numbersJson(a);
            mid["set"][p->name] = numbersJson(b);
            mx["set"][p->name] = numbersJson(c);
        }
        all.push(mn);
        all.push(mid);
        all.push(mx);
    }
    if (quick) return all;
    int count = 0;
    for (const Param* p : numeric) {
        if (count >= maxParams) break;
        bool varies = false;
        std::vector<double> lows, highs;
        for (int k = 0; k < p->dim; ++k) {
            double lo, hi;
            rangeOf(p, k, lo, hi);
            lows.push_back(lo);
            highs.push_back(hi);
            const double def = (size_t)k < p->value.size() ? p->value[(size_t)k] : lo;
            if (std::fabs(def - lo) > 1e-9 || std::fabs(def - hi) > 1e-9) varies = true;
        }
        if (!varies) continue;
        ++count;
        Json a = makeScenario("min: " + p->label(), "param", 1), b = makeScenario("max: " + p->label(), "param", 1);
        a["set"][p->name] = numbersJson(lows);
        b["set"][p->name] = numbersJson(highs);
        all.push(a);
        all.push(b);
    }
    int shown = 0;
    const std::string ctxShort = shortContext(context);
    for (const PresetEntry& pe : presets) {
        if (shown >= 10) break;
        if (!pe.context.empty() && toLower(pe.context) != toLower(ctxShort) && !(pe.context == "General")) continue;
        Json s = makeScenario("preset: " + pe.name, "preset", 1);
        for (const PresetValue& v : pe.values) {
            if (v.isText) s["set"][v.name] = v.text;
            else s["set"][v.name] = numbersJson(v.numbers);
        }
        all.push(s);
        ++shown;
    }
    return all;
}

// Puts a scenario's settings into an instance. The names of settings it has no such setting for are added to 'missing'.
void applySettings(Instance& inst, const Json& set, std::vector<std::string>& missing)
{
    for (const auto& m : set.members()) {
        Param* p = inst.params.find(m.first);
        if (!p || !p->hasValue()) { missing.push_back(m.first); continue; }
        if (p->isText()) {
            if (m.second.isString()) inst.userSetString(p, m.second.asString(), 0.0);
            continue;
        }
        if (!m.second.isArray()) continue;
        std::vector<double> v = p->value;
        v.resize((size_t)p->dim, 0.0);
        for (size_t k = 0; k < v.size() && k < m.second.size(); ++k) {
            double x = m.second.at(k).asDouble();
            const double lo = p->props.getDouble(kOfxParamPropMin, (int)k, -DBL_MAX), hi = p->props.getDouble(kOfxParamPropMax, (int)k, DBL_MAX);
            if (p->kind == ParamKind::Choice) {
                const int n = (int)p->choices().size();
                if (n > 0) x = std::min(x, (double)(n - 1));
            }
            if (std::isfinite(lo) && lo > -1.0e300) x = std::max(x, lo);
            if (std::isfinite(hi) && hi < 1.0e300) x = std::min(x, hi);
            v[k] = x;
        }
        inst.userSetValue(p, v, 0.0);
    }
}

// ====================================================================================================== one effect, one source (the child)
struct RunSpec {
    std::string source;             // "real" or "builtin"
    std::string file;               // the plug-in file (real)
    std::string id, context;        // context: the full OpenFX name
    int width = 320, height = 180, frames = 3;
    std::string dir, tag;
    std::string outJson;
    std::string scenariosIn, scenariosOut, presetsFrom;
    int first = 0;
    bool quick = false;
    int maxParams = 12;
};

Plugin* findIn(Binary* b, const std::string& id)
{
    if (!b) return nullptr;
    Plugin* best = nullptr;
    for (const auto& up : b->plugins)
        if (up->info.id == id && (!best || up->info.versionMajor > best->info.versionMajor)) best = up.get();
    return best;
}

std::vector<PresetEntry> presetsFor(const RunSpec& rs, const std::string& id)
{
    std::string text;
    if (rs.presetsFrom == "builtin") text = builtinPresetPackage();
    else if (!rs.presetsFrom.empty()) readFile(rs.presetsFrom, text);
    std::vector<PresetEntry> all = parsePresetPackage(text), mine;
    for (const PresetEntry& e : all)
        if (e.plugin == id) mine.push_back(e);
    return mine;
}

InstanceSetup setupFor(const RunSpec& rs)
{
    InstanceSetup setup;
    setup.width = rs.width;
    setup.height = rs.height;
    setup.fps = 30.0;
    setup.durationFrames = 300;
    if (rs.context == kOfxImageEffectContextGenerator) setup.vegasContext = kVegasContextGenerator;
    else if (rs.context == kOfxImageEffectContextTransition) setup.vegasContext = kVegasContextEventFadeIn;
    else setup.vegasContext = kVegasContextEvent;
    return setup;
}

void hookClips(Instance& inst, const RunSpec& rs)
{
    const int w = rs.width, h = rs.height;
    for (const auto& name : inst.inputClipNames()) {
        const bool to = name == kOfxImageEffectTransitionSourceToClipName;
        inst.setClipSource(name, [w, h, to](double t) { return sourceFrame(w, h, t, to); });
    }
}

// Renders a scenario's frames. Fills 'rec' (the scenario's entry in the result); the pictures go to files.
void runScenario(Plugin* pl, const RunSpec& rs, const Json& scen, int index, Json& rec)
{
    rec = Json::object();
    rec["index"] = index;
    rec["name"] = scen.get("name").asString();
    rec["kind"] = scen.get("kind").asString();
    const int frames = std::max(1, scen.get("frames").asInt(1));
    rec["frames"] = frames;
    std::string err;
    std::unique_ptr<Instance> inst = pl->createInstance(rs.context, setupFor(rs), &err);
    if (!inst) {
        rec["ok"] = false;
        rec["error"] = "no instance: " + (err.empty() ? pl->error : err);
        return;
    }
    std::vector<std::string> missing;
    applySettings(*inst, scen.get("set"), missing);
    if (!missing.empty()) {
        Json m = Json::array();
        for (const auto& s : missing) m.push(s);
        rec["missingSettings"] = m;
    }
    hookClips(*inst, rs);
    Param* progress = rs.context == kOfxImageEffectContextTransition ? inst->params.find(kOfxImageEffectTransitionParamName) : nullptr;
    const bool filter = rs.context != kOfxImageEffectContextGenerator && rs.context != kOfxImageEffectContextTransition;
    Json ms = Json::array();
    int done = 0, blank = 0, same = 0, transparent = 0;
    bool ok = true;
    std::string e;
    for (int f = 0; f < frames; ++f) {
        const double t = 10.0 + f;
        if (progress) inst->userSetValue(progress, std::vector<double>{frames == 1 ? 0.5 : (f + 1.0) / (frames + 1.0)}, t);
        const auto t0 = std::chrono::steady_clock::now();
        RenderOptions ro;
        ro.interactive = false;
        std::string re;
        ImagePtr out = inst->render(t, ro, &re);
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms.push(took);
        if (!out || out->w != rs.width || out->h != rs.height) {
            ok = false;
            e = !out ? (re.empty() ? std::string("the render failed") : re) : strfmt("the picture is %dx%d, not %dx%d", out->w, out->h, rs.width, rs.height);
            break;
        }
        ++done;
        if (out->depth == PixelDepth::Float) out = out->converted(PixelDepth::Byte);
        if (isBlank(*out)) ++blank;
        if (isTransparent(*out)) ++transparent;
        if (filter) {
            ImagePtr src = sourceFrame(rs.width, rs.height, t, false);
            if (src && sameBytes(*out, *src)) ++same;
        }
        writeFile(pathJoin(rs.dir, strfmt("%s_%d_%d.bgra", rs.tag.c_str(), index, f)),
                  std::string((const char*)out->data.get(), out->byteSize()));
    }
    rec["ok"] = ok;
    if (!ok) rec["error"] = e;
    rec["rendered"] = done;
    rec["ms"] = ms;
    rec["blank"] = done > 0 && blank == done;
    rec["transparent"] = done > 0 && transparent == done;
    rec["unchanged"] = done > 0 && same == done;
    rec["pluginCrashed"] = pl->crashed;
}

int childRun(const RunSpec& rs)
{
    Json res = Json::object();
    res["source"] = rs.source;
    res["id"] = rs.id;
    res["context"] = rs.context;
    res["done"] = false;
    res["running"] = -1;
    res["scenarios"] = Json::array();
    auto save = [&] { writeJsonFile(rs.outJson, res); };
    save();
    Host& host = Host::get();
    std::string err;
    Binary* bin = host.loadBinary(rs.source == "builtin" ? std::string(kBuiltinBinaryPath) : rs.file, &err);
    if (!bin) {
        res["loadOk"] = false;
        res["loadError"] = err.empty() ? std::string("the file could not be loaded") : err;
        res["done"] = true;
        save();
        return 0;
    }
    Plugin* pl = findIn(bin, rs.id);
    if (!pl) {
        res["loadOk"] = false;
        res["loadError"] = "the file has no effect " + rs.id;
        res["done"] = true;
        save();
        return 0;
    }
    res["running"] = -2;                    // (loading)
    save();
    std::string le;
    if (!pl->load(&le)) {
        res["loadOk"] = false;
        res["loadError"] = le.empty() ? pl->error : le;
        res["crashedLoading"] = pl->crashed;
        res["done"] = true;
        save();
        return 0;
    }
    res["loadOk"] = true;
    res["label"] = pl->info.label;
    res["group"] = pl->info.grouping;
    res["multiResolution"] = pl->info.multiResolution;
    res["overlay"] = pl->info.hasOverlay;
    res["ownWindow"] = pl->info.hasHwndInteract;
    res["version"] = strfmt("%d.%d", pl->info.versionMajor, pl->info.versionMinor);
    if (!pl->supportsContext(rs.context)) {
        res["instanceOk"] = false;
        res["instanceError"] = "has no " + shortContext(rs.context) + " version";
        res["noContext"] = true;
        res["done"] = true;
        save();
        return 0;
    }
    res["running"] = -3;
    save();
    std::string ie;
    std::unique_ptr<Instance> probe = pl->createInstance(rs.context, setupFor(rs), &ie);
    if (!probe) {
        res["instanceOk"] = false;
        res["instanceError"] = ie.empty() ? pl->error : ie;
        {
            std::lock_guard<std::mutex> lk(pl->suiteLogMutex);
            if (!pl->missingFeature.empty()) res["missingFeature"] = pl->missingFeature;
        }
        res["done"] = true;
        save();
        return 0;
    }
    res["instanceOk"] = true;
    int params = 0;
    Json names = Json::array();
    for (const auto& p : probe->params.params)
        if (p->hasValue() && !p->secret()) { ++params; names.push(p->name); }
    res["params"] = params;
    res["paramNames"] = names;
    Json scenarios;
    if (!rs.scenariosIn.empty()) scenarios = readJsonFile(rs.scenariosIn);
    if (!scenarios.isArray() || scenarios.size() == 0) {
        scenarios = makeScenarios(*probe, presetsFor(rs, rs.id), rs.context, rs.frames, rs.quick, rs.maxParams);
        if (!rs.scenariosOut.empty()) writeJsonFile(rs.scenariosOut, scenarios);
    }
    probe.reset();
    res["scenarioCount"] = (int)scenarios.size();
    Json names2 = Json::array();
    for (const auto& s : scenarios.items()) names2.push(s.get("name"));
    res["scenarioNames"] = names2;
    for (int k = std::max(0, rs.first); k < (int)scenarios.size(); ++k) {
        res["running"] = k;
        save();
        Json rec;
        try {
            runScenario(pl, rs, scenarios.at((size_t)k), k, rec);
        } catch (...) {
            rec = Json::object();
            rec["index"] = k;
            rec["name"] = scenarios.at((size_t)k).get("name");
            rec["ok"] = false;
            rec["error"] = "the plug-in threw an exception";
        }
        Json list = res.get("scenarios");
        list.push(rec);
        res["scenarios"] = list;
        res["running"] = -1;
        save();
        if (pl->crashed) break;            // (it will not work again)
    }
    res["done"] = true;
    res["pluginCrashed"] = pl->crashed;
    save();
    return 0;
}

// What plug-in files hold: which effects, in which contexts.
int childList(const std::string& file, const std::string& outPath, bool stubs)
{
    Json res = Json::object();
    res["path"] = file;
    res["done"] = false;
    Json list = Json::array();
    res["plugins"] = list;
    writeJsonFile(outPath, res);
    Host& host = Host::get();
    host.config.stubUnknownSuites = stubs;
    host.applyConfig();
    std::string err;
    Binary* bin = host.loadBinary(file, &err);
    if (!bin) {
        res["ok"] = false;
        res["error"] = err;
        res["done"] = true;
        writeJsonFile(outPath, res);
        return 0;
    }
    res["ok"] = true;
    for (auto& up : bin->plugins) {
        Plugin* pl = up.get();
        Json j = Json::object();
        j["id"] = pl->info.id;
        std::string e;
        const bool ok = pl->load(&e);
        j["ok"] = ok;
        if (!ok) j["error"] = e.empty() ? pl->error : e;
        j["label"] = pl->info.label;
        j["group"] = pl->info.grouping;
        Json ctx = Json::array();
        if (ok) {
            const std::string f = pl->filterContext();
            if (!f.empty()) ctx.push(f);
            if (pl->supportsContext(kOfxImageEffectContextGenerator)) ctx.push(std::string(kOfxImageEffectContextGenerator));
            if (pl->supportsContext(kOfxImageEffectContextTransition)) ctx.push(std::string(kOfxImageEffectContextTransition));
        }
        j["contexts"] = ctx;
        list.push(j);
        res["plugins"] = list;
        writeJsonFile(outPath, res);
    }
    res["done"] = true;
    writeJsonFile(outPath, res);
    return 0;
}

}  // namespace

int runVegasFxChild(const std::string& cmd, const std::vector<std::string>& args)
{
    RunSpec rs;
    std::string file, out;
    bool stubs = false;
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        auto next = [&](std::string& dst) { if (k + 1 < args.size()) dst = args[++k]; };
        if (a == "--source") next(rs.source);
        else if (a == "--file") next(rs.file);
        else if (a == "--id") next(rs.id);
        else if (a == "--context") next(rs.context);
        else if (a == "--size") { std::string v; next(v); parseSize(v, &rs.width, &rs.height); }
        else if (a == "--frames") { std::string v; next(v); rs.frames = std::max(1, std::min(30, std::atoi(v.c_str()))); }
        else if (a == "--dir") next(rs.dir);
        else if (a == "--tag") next(rs.tag);
        else if (a == "--out") next(rs.outJson);
        else if (a == "--scenarios-in") next(rs.scenariosIn);
        else if (a == "--scenarios-out") next(rs.scenariosOut);
        else if (a == "--presets") next(rs.presetsFrom);
        else if (a == "--first") { std::string v; next(v); rs.first = std::atoi(v.c_str()); }
        else if (a == "--max-params") { std::string v; next(v); rs.maxParams = std::atoi(v.c_str()); }
        else if (a == "--quick") rs.quick = true;
        else if (a == "--stubs") stubs = true;
        else if (a == "--gpu-off") Host::get().config.gpu = false;
        else if (cmd == "vegasfxlist" && file.empty()) file = a;
    }
    Host::get().config.stubUnknownSuites = stubs;
    Host::get().applyConfig();
    if (cmd == "vegasfxlist") return childList(file, rs.scenariosOut.empty() ? rs.outJson : rs.scenariosOut, stubs);
    if (rs.outJson.empty() || rs.id.empty() || rs.context.empty() || rs.dir.empty() || (rs.source != "real" && rs.source != "builtin")) {
        std::fprintf(stderr, "reno_cli vegasfxrun: not meant to be run by hand (reno_cli vegasfx runs it)\n");
        return 2;
    }
    return childRun(rs);
}

// ====================================================================================================== the run (the parent)
namespace {

// Json arrays have no element assignment: the array is rebuilt with element 'idx' replaced (growing it with nulls if needed).
void setAt(Json& arr, size_t idx, const Json& v)
{
    Json out = Json::array();
    for (size_t k = 0; k < std::max(arr.size(), idx + 1); ++k) out.push(k == idx ? v : (k < arr.size() ? arr.at(k) : Json()));
    arr = out;
}

struct Effect {
    std::string id, label, group;
    bool hasReal = false;
    std::string file;
    bool realListed = false;
    std::string realError;
    std::vector<std::string> realContexts;
    bool hasBuiltin = false;
    std::vector<std::string> builtinContexts;
};

std::vector<std::string> builtinContextsOf(const BuiltinInfo& b)
{
    std::vector<std::string> c;
    if (b.filter) c.push_back(kOfxImageEffectContextFilter);
    if (b.generator) c.push_back(kOfxImageEffectContextGenerator);
    if (b.transition) c.push_back(kOfxImageEffectContextTransition);
    return c;
}

// What one side (the real plug-in, or the built-in) did.
struct Side {
    bool present = false;
    std::string status;                 // "works", "does not load", "no instance", "crashes", ...
    std::string detail;
    bool works = false;
    std::string suspect;
    bool good() const { return works && suspect.empty(); }
    int tests = 0, testsOk = 0, crashes = 0;
    double msFirst = -1, msFrame = -1;
    std::vector<std::string> failures;
    Json merged;
};

struct Compared {
    bool done = false;
    double defaultScore = 0, worstScore = 0;
    std::string worstName;
    int worstFrame = 0;
    int compared = 0;
    double maxAbs = 0;
    std::string png;
    Json list = Json::array();
};

struct RowResult {
    std::string id, label, group, context, file;
    Side real, builtin;
    Compared cmp;
    std::string verdict;
};

// One run of 'reno_cli vegasfxrun' for one effect from one source, started again after its crashes (each crashed test is
// recorded as crashed and the run goes on with the next one).
Side runSide(const CompareOptions& opt, const std::string& source, const Effect& e, const std::string& context, const std::string& tmp, const std::string& tag,
             const std::string& scenariosPath, const std::string& presetsFrom)
{
    Side s;
    s.present = true;
    Json merged = Json::object();
    Json recs = Json::array();
    std::string problem;
    int first = 0, attempts = 0, count = 0;
    Json names = Json::array();
    const std::string outJson = pathJoin(tmp, tag + ".json");
    for (;;) {
        std::vector<std::string> args = {"vegasfxrun", "--source", source, "--id", e.id, "--context", context, "--size", strfmt("%dx%d", opt.width, opt.height),
                                         "--frames", strfmt("%d", opt.frames), "--dir", tmp, "--tag", tag, "--out", outJson,
                                         "--max-params", strfmt("%d", opt.maxParams), "--first", strfmt("%d", first)};
        if (source == "real") { args.push_back("--file"); args.push_back(e.file); }
        args.push_back(fileExists(scenariosPath) ? "--scenarios-in" : "--scenarios-out");
        args.push_back(scenariosPath);
        if (!presetsFrom.empty()) { args.push_back("--presets"); args.push_back(presetsFrom); }
        if (opt.quick) args.push_back("--quick");
        if (opt.stubs) args.push_back("--stubs");
        if (!opt.gpu) args.push_back("--gpu-off");
        removeFile(outJson);
        const ProcessResult pr = runProcess(selfExePath(), args, opt.timeoutSec);
        const Json r = readJsonFile(outJson);
        problem = (!pr.started || pr.timedOut || pr.exitCode != 0) ? pr.describe() : std::string();
        if (r.isNull()) {
            if (problem.empty()) problem = "gave no result";
            break;
        }
        if (attempts == 0 || !merged.has("loadOk"))
            for (const auto& m : r.members())
                if (m.first != "scenarios" && m.first != "running" && m.first != "done") merged[m.first] = m.second;
        if (r.has("scenarioCount")) count = r.get("scenarioCount").asInt();
        if (r.has("scenarioNames")) names = r.get("scenarioNames");
        while ((int)recs.size() < count) recs.push(Json());
        for (const auto& rec : r.get("scenarios").items()) {
            const int idx = rec.get("index").asInt(-1);
            if (idx < 0) continue;
            while ((int)recs.size() <= idx) recs.push(Json());
            setAt(recs, (size_t)idx, rec);
        }
        if (problem.empty() && r.get("done").asBool()) break;
        const int running = r.get("running").asInt(-1);
        if (running >= 0 && attempts < 8) {
            Json rec = Json::object();
            rec["index"] = running;
            rec["name"] = running < (int)names.size() ? names.at((size_t)running).asString() : std::string();
            rec["ok"] = false;
            rec["crashed"] = true;
            rec["error"] = "the process " + (problem.empty() ? std::string("stopped") : problem);
            setAt(recs, (size_t)running, rec);
            first = running + 1;
            ++attempts;
            if (first >= count) { problem.clear(); break; }
            continue;
        }
        if (running == -2 || running == -3) merged["crashedAt"] = running == -2 ? "loading" : "making an instance";
        break;
    }
    merged["scenarios"] = recs;
    merged["problem"] = problem;
    s.merged = merged;
    // ---- what it comes to
    const bool loadOk = merged.get("loadOk").asBool();
    if (!merged.has("loadOk")) {
        s.status = "crashes";
        s.detail = "the checking process " + (problem.empty() ? std::string("gave no answer") : problem) + " before it said anything";
        return s;
    }
    if (!loadOk) {
        s.status = merged.get("crashedLoading").asBool() ? "crashes when loaded" : "does not load";
        s.detail = merged.get("loadError").asString();
        return s;
    }
    if (merged.get("noContext").asBool()) { s.status = "no " + toLower(shortContext(context)) + " version"; return s; }
    if (!merged.get("instanceOk").asBool(false)) {
        if (merged.has("crashedAt")) {
            s.status = "crashes";
            s.detail = "the checking process " + problem + " while " + merged.get("crashedAt").asString();
        } else {
            s.status = "makes no instance";
            s.detail = merged.get("instanceError").asString();
            if (merged.has("missingFeature") && s.detail.find(merged.get("missingFeature").asString()) == std::string::npos)
                s.detail += " (it asked the host for: " + merged.get("missingFeature").asString() + ")";
        }
        return s;
    }
    int blank = 0, unchanged = 0, rendered = 0, transparent = 0;
    const bool filter = context != kOfxImageEffectContextGenerator && context != kOfxImageEffectContextTransition;
    for (size_t k = 0; k < recs.size(); ++k) {
        const Json& rec = recs.at(k);
        if (rec.isNull()) continue;
        ++s.tests;
        if (rec.get("ok").asBool()) {
            ++s.testsOk;
            if (rec.get("blank").asBool()) ++blank;
            if (rec.get("transparent").asBool()) ++transparent;
            if (rec.get("unchanged").asBool()) ++unchanged;
            ++rendered;
        } else {
            if (rec.get("crashed").asBool()) ++s.crashes;
            if (s.failures.size() < 4) s.failures.push_back(rec.get("name").asString() + ": " + rec.get("error").asString());
        }
    }
    if (recs.size() > 0 && !recs.at(0).isNull() && recs.at(0).get("ok").asBool()) {
        const Json& ms = recs.at(0).get("ms");
        if (ms.size() > 0) s.msFirst = ms.at(0).asDouble();
        if (ms.size() > 1) {
            double sum = 0;
            for (size_t k = 1; k < ms.size(); ++k) sum += ms.at(k).asDouble();
            s.msFrame = sum / (double)(ms.size() - 1);
        } else s.msFrame = s.msFirst;
    }
    if (s.tests == 0) { s.status = "crashes"; s.detail = "the checking process " + (problem.empty() ? std::string("gave no result") : problem); return s; }
    if (s.testsOk == 0) {
        s.status = s.crashes ? "crashes" : "render fails";
        if (!s.failures.empty()) s.detail = s.failures[0];
    } else if (s.crashes > 0) {
        s.status = "crashes in some tests";
        s.detail = s.failures.empty() ? std::string() : s.failures[0];
    } else if (transparent == rendered) {
        s.status = "draws nothing";
        s.detail = "every picture it made is completely transparent";
    } else {
        s.works = true;
        s.status = s.testsOk < s.tests ? "works, but some tests fail" : "works";
        if (filter && unchanged == rendered) s.suspect = "never changes the picture";
        else if (context != kOfxImageEffectContextGenerator && blank == rendered) s.suspect = "always draws one flat colour";
        if (!s.suspect.empty()) s.status += ", but " + s.suspect;
        if (!problem.empty()) s.detail = "the process then ended badly: " + problem;
    }
    return s;
}

// ---- comparing pictures
struct Diff {
    double mean[4] = {0, 0, 0, 0};
    int maxd[4] = {0, 0, 0, 0};
    double score = 0;                   // the four means averaged
};

Diff diffBgra(const std::string& a, const std::string& b)
{
    Diff d;
    const size_t n = std::min(a.size(), b.size()) / 4;
    if (!n) return d;
    static const int chan[4] = {2, 1, 0, 3};     // R, G, B, A in BGRA
    double sum[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 4; ++c) {
            const int x = std::abs((int)(uint8_t)a[i * 4 + (size_t)chan[c]] - (int)(uint8_t)b[i * 4 + (size_t)chan[c]]);
            sum[c] += x;
            if (x > d.maxd[c]) d.maxd[c] = x;
        }
    for (int c = 0; c < 4; ++c) d.mean[c] = sum[c] / (double)n;
    d.score = (d.mean[0] + d.mean[1] + d.mean[2] + d.mean[3]) / 4.0;
    return d;
}

// Rows are stored bottom first. Premultiplied BGRA over a grey checkerboard -> top-down RGB.
void putPanel(std::vector<uint8_t>& rgb, int totalW, int panelX, const std::string& bgra, int w, int h, int amplify)
{
    for (int y = 0; y < h; ++y) {
        const size_t srcRow = (size_t)(h - 1 - y) * (size_t)w * 4;
        for (int x = 0; x < w; ++x) {
            const bool light = (((x >> 3) + (y >> 3)) & 1) != 0;
            const int bg = light ? 150 : 100;
            uint8_t px[4] = {0, 0, 0, 255};
            if (srcRow + (size_t)x * 4 + 3 < bgra.size()) std::memcpy(px, bgra.data() + srcRow + (size_t)x * 4, 4);
            int r = px[2], g = px[1], b = px[0];
            if (amplify) {
                r = std::min(255, r * amplify);
                g = std::min(255, g * amplify);
                b = std::min(255, b * amplify);
            } else {
                const int inv = 255 - px[3];
                r = std::min(255, r + bg * inv / 255);
                g = std::min(255, g + bg * inv / 255);
                b = std::min(255, b + bg * inv / 255);
            }
            uint8_t* o = &rgb[((size_t)y * (size_t)totalW + (size_t)(panelX + x)) * 3];
            o[0] = (uint8_t)r;
            o[1] = (uint8_t)g;
            o[2] = (uint8_t)b;
        }
    }
}

std::string absDiff(const std::string& a, const std::string& b)
{
    std::string d(std::min(a.size(), b.size()), '\0');
    for (size_t i = 0; i < d.size(); ++i) d[i] = (char)std::abs((int)(uint8_t)a[i] - (int)(uint8_t)b[i]);
    for (size_t i = 3; i < d.size(); i += 4) d[i] = (char)255;
    return d;
}

bool readRaw(const std::string& tmp, const std::string& tag, int scen, int frame, std::string& out)
{
    return readFile(pathJoin(tmp, strfmt("%s_%d_%d.bgra", tag.c_str(), scen, frame)), out);
}

// Compares every test both sides rendered; writes the picture of the worst one if it is worth looking at.
void compareSides(const CompareOptions& opt, RowResult& row, const std::string& tmp, const std::string& realTag, const std::string& builtinTag)
{
    Compared& c = row.cmp;
    const Json& rs = row.real.merged.get("scenarios");
    const Json& bs = row.builtin.merged.get("scenarios");
    const size_t n = std::min(rs.size(), bs.size());
    double worst = -1;
    int worstScen = -1, worstFrame = 0;
    for (size_t i = 0; i < n; ++i) {
        const Json& a = rs.at(i);
        const Json& b = bs.at(i);
        if (a.isNull() || b.isNull() || !a.get("ok").asBool() || !b.get("ok").asBool()) continue;
        const int frames = std::min(a.get("rendered").asInt(), b.get("rendered").asInt());
        for (int f = 0; f < frames; ++f) {
            std::string ra, rb;
            if (!readRaw(tmp, realTag, (int)i, f, ra) || !readRaw(tmp, builtinTag, (int)i, f, rb) || ra.size() != rb.size()) continue;
            const Diff d = diffBgra(ra, rb);
            Json j = Json::object();
            j["scenario"] = a.get("name");
            j["frame"] = f;
            Json mean = Json::array(), mx = Json::array();
            for (int k = 0; k < 4; ++k) { mean.push(d.mean[k]); mx.push(d.maxd[k]); }
            j["meanRGBA"] = mean;
            j["maxRGBA"] = mx;
            j["score"] = d.score;
            if (!a.get("missingSettings").isNull() || !b.get("missingSettings").isNull()) j["note"] = "a setting is missing on one side";
            c.list.push(j);
            ++c.compared;
            for (int k = 0; k < 4; ++k) c.maxAbs = std::max(c.maxAbs, (double)d.maxd[k]);
            if (i == 0) c.defaultScore = std::max(c.defaultScore, d.score);
            if (d.score > worst) { worst = d.score; worstScen = (int)i; worstFrame = f; c.worstName = a.get("name").asString(); }
        }
    }
    c.done = c.compared > 0;
    c.worstScore = std::max(0.0, worst);
    c.worstFrame = worstFrame;
    if (!c.done || c.worstScore < 1.0) return;
    std::string ra, rb;
    if (!readRaw(tmp, realTag, worstScen, worstFrame, ra) || !readRaw(tmp, builtinTag, worstScen, worstFrame, rb)) return;
    const int w = opt.width, h = opt.height;
    const bool filter = row.context != kOfxImageEffectContextGenerator;
    std::string in;
    if (filter) {
        ImagePtr src = sourceFrame(w, h, 10.0 + worstFrame, false);
        if (src) in.assign((const char*)src->data.get(), src->byteSize());
    }
    if (in.size() != ra.size()) in.assign(ra.size(), '\x60');
    const int totalW = w * 4 + 6;
    std::vector<uint8_t> rgb((size_t)totalW * (size_t)h * 3, 40);
    putPanel(rgb, totalW, 0, in, w, h, 0);
    putPanel(rgb, totalW, w + 2, ra, w, h, 0);
    putPanel(rgb, totalW, 2 * w + 4, rb, w, h, 0);
    putPanel(rgb, totalW, 3 * w + 6 - 0, absDiff(ra, rb), w, h, 4);
    const std::string name = fileSafe(row.id) + "_" + toLower(shortContext(row.context)) + ".png";
    if (writeFile(pathJoin(opt.outDir, name), encodePng(totalW, h, rgb))) c.png = name;
}

std::string verdictOf(const RowResult& r)
{
    const Side &re = r.real, &bi = r.builtin;
    if (re.present && bi.present) {
        if (!re.works && !bi.works) return "neither works";
        if (!re.works) return "real plug-in fails here; the built-in " + std::string(bi.works ? "takes its place" : "fails too");
        if (!bi.works) return "the built-in does not work (the real plug-in does)";
        if (!re.suspect.empty() && bi.suspect.empty()) return "the real plug-in " + re.suspect + " here (the built-in does)";
        if (re.suspect.empty() && !bi.suspect.empty()) return "the built-in " + bi.suspect + " (the real plug-in does not)";
        if (!r.cmp.done) return "both work; nothing to compare";
        const double s = r.cmp.worstScore;
        if (s < 0.25) return "same picture";
        if (s < 2.0) return "close";
        if (s < 8.0) return "differs visibly";
        return "very different";
    }
    if (re.present) return re.good() ? "works (no built-in)" : (re.works ? "suspect: it " + re.suspect + " (no built-in)" : "FAILS (no built-in to take its place)");
    if (bi.present) return bi.good() ? "built-in only (VEGAS's plug-in not on this machine)" : (bi.works ? "built-in only; it " + bi.suspect : "built-in does not work");
    return "";
}

Json sideJson(const Side& s)
{
    Json j = Json::object();
    j["present"] = s.present;
    j["status"] = s.status;
    if (!s.detail.empty()) j["detail"] = s.detail;
    j["works"] = s.works;
    if (!s.suspect.empty()) j["suspect"] = s.suspect;
    j["tests"] = s.tests;
    j["testsOk"] = s.testsOk;
    j["crashes"] = s.crashes;
    if (s.msFirst >= 0) { j["msFirstFrame"] = s.msFirst; j["msPerFrame"] = s.msFrame; }
    if (!s.failures.empty()) {
        Json f = Json::array();
        for (const auto& x : s.failures) f.push(x);
        j["failures"] = f;
    }
    for (const char* k : {"label", "version", "params", "multiResolution", "overlay", "ownWindow", "problem", "crashedAt"})
        if (s.merged.has(k)) j[k] = s.merged.get(k);
    if (s.merged.has("scenarios")) {
        Json list = Json::array();
        for (const auto& rec : s.merged.get("scenarios").items()) {
            if (rec.isNull()) continue;
            Json r = Json::object();
            for (const auto& m : rec.members()) if (m.first != "ms") r[m.first] = m.second;
            if (rec.has("ms")) {
                double sum = 0;
                for (const auto& v : rec.get("ms").items()) sum += v.asDouble();
                if (rec.get("ms").size()) r["msPerFrame"] = sum / (double)rec.get("ms").size();
            }
            list.push(r);
        }
        j["testResults"] = list;
    }
    return j;
}

Json rowJson(const RowResult& r)
{
    Json j = Json::object();
    j["id"] = r.id;
    j["label"] = r.label;
    j["group"] = r.group;
    j["context"] = shortContext(r.context);
    if (!r.file.empty()) j["file"] = r.file;
    j["verdict"] = r.verdict;
    j["real"] = sideJson(r.real);
    j["builtin"] = sideJson(r.builtin);
    if (r.cmp.done) {
        Json c = Json::object();
        c["compared"] = r.cmp.compared;
        c["defaultScore"] = r.cmp.defaultScore;
        c["worstScore"] = r.cmp.worstScore;
        c["worstTest"] = r.cmp.worstName;
        c["worstFrame"] = r.cmp.worstFrame;
        c["maxDifference"] = r.cmp.maxAbs;
        if (!r.cmp.png.empty()) c["picture"] = r.cmp.png;
        c["tests"] = r.cmp.list;
        j["compare"] = c;
    }
    return j;
}

std::string fmtMs(double ms)
{
    if (ms < 0) return "-";
    return ms < 10 ? strfmt("%.2f", ms) : (ms < 100 ? strfmt("%.1f", ms) : strfmt("%.0f", ms));
}

}  // namespace

int runVegasFxReport(const CompareOptions& optIn)
{
    CompareOptions opt = optIn;
    makeDirs(opt.outDir);
    const std::string tmp = pathJoin(opt.outDir, "vegasfx_tmp");
    makeDirs(tmp);
    Host& host = Host::get();
    host.config.stubUnknownSuites = false;
    host.applyConfig();

    std::string report;
    auto line = [&](const std::string& s) {
        report += s + "\r\n";
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
    };
    auto saveReport = [&] { writeFile(pathJoin(opt.outDir, "vegasfx_report.txt"), report); };

    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));
    line(strfmt("Reno VEGAS effects check, %s", when));
    line("Each effect is tried in a process of its own, first VEGAS's own plug-in (when it is installed here) and then");
    line("Reno's built-in version of it: loaded, an instance made, and a test picture put through it at the defaults,");
    line(strfmt("with every setting at its smallest, middle and largest value%s. Pictures are %dx%d, %d frames at the defaults.",
                opt.quick ? "" : ", one setting at a time and with the presets", opt.width, opt.height, opt.frames));
    line("");

    // ---- what is on this machine
    std::vector<std::string> dirs = Host::defaultSearchDirs();
    for (const auto& d : opt.pluginPaths) dirs.push_back(d);
    for (const auto& d : dirs) line("looked in: " + d);
    const std::vector<std::string> files = Host::findBinaries(dirs);
    std::map<std::string, Effect> effects;
    std::vector<std::string> order;
    std::vector<std::string> fileProblems;
    int realEffects = 0;
    for (const auto& f : files) {
        const std::string listJson = pathJoin(tmp, "list.json");
        removeFile(listJson);
        std::vector<std::string> args = {"vegasfxlist", "--out", listJson, f};
        if (opt.stubs) args.push_back("--stubs");
        const ProcessResult pr = runProcess(selfExePath(), args, 120);
        const Json r = readJsonFile(listJson);
        const std::string problem = (!pr.started || pr.timedOut || pr.exitCode != 0) ? pr.describe() : std::string();
        if (!r.get("ok").asBool(false)) {
            fileProblems.push_back(f + ": " + (problem.empty() ? r.get("error").asString("did not load") : "the checking process " + problem));
            continue;
        }
        if (!problem.empty()) fileProblems.push_back(f + ": the checking process " + problem + " after " + strfmt("%d", (int)r.get("plugins").size()) + " effects; the rest were not reached");
        for (const auto& p : r.get("plugins").items()) {
            const std::string id = p.get("id").asString();
            if (id.empty()) continue;
            bool builtinHas = false;
            for (const BuiltinInfo& b : builtinInfos()) if (b.id == id) builtinHas = true;
            if (!opt.all && !isVegasOwnEffect(id) && !builtinHas) continue;
            if (!opt.only.empty() && !containsNoCase(id, opt.only) && !containsNoCase(p.get("label").asString(), opt.only)) continue;
            if (effects.count(id) && effects[id].hasReal) continue;
            Effect& e = effects[id];
            if (!e.hasReal && e.id.empty()) order.push_back(id);
            e.id = id;
            e.label = p.get("label").asString();
            e.group = p.get("group").asString();
            e.hasReal = true;
            e.file = f;
            e.realListed = p.get("ok").asBool();
            e.realError = p.get("error").asString();
            for (const auto& c : p.get("contexts").items()) e.realContexts.push_back(c.asString());
            ++realEffects;
        }
    }
    int builtinCount = 0;
    for (const BuiltinInfo& b : builtinInfos()) {
        if (!opt.only.empty() && !containsNoCase(b.id, opt.only) && !containsNoCase(b.label, opt.only)) continue;
        Effect& e = effects[b.id];
        if (e.id.empty()) { e.id = b.id; order.push_back(b.id); }
        if (e.label.empty()) e.label = b.label;
        if (e.group.empty()) e.group = b.group;
        e.hasBuiltin = true;
        e.builtinContexts = builtinContextsOf(b);
        ++builtinCount;
    }
    line("");
    line(strfmt("plug-in files found: %d; effects in them to try: %d; Reno's built-in effects: %d", (int)files.size(), realEffects, builtinCount));
    for (const auto& p : fileProblems) line("  file problem: " + p);
    if (realEffects == 0) {
        line("");
        line("NOTE: none of VEGAS's own OpenFX plug-ins were found on this machine, so only Reno's built-in versions were");
        line("tried (what loads, makes an instance and draws something). Run this on the computer VEGAS is installed on to");
        line("compare them with the real ones.");
    }
    line("");
    saveReport();

    // ---- the settings of the built-ins against VEGAS's resource files (the real bundles' Contents/Resources/*.xml)
    std::vector<std::string> resDirs = opt.resourceDirs;
    for (const auto& f : files) resDirs.push_back(pathDir(pathDir(pathDir(f))));
    Json paramCheck;
    std::vector<std::string> paramLines;
    const int paramDiffers = compareParameterLists(resDirs, paramCheck, paramLines);
    (void)paramDiffers;
    auto writeParamReport = [&] {
        std::string t = "Reno built-in settings against VEGAS's resource files\r\n\r\n";
        for (const auto& l : paramLines) t += l + "\r\n";
        writeFile(pathJoin(opt.outDir, "vegasfx_params.txt"), t);
        Json full = Json::object();
        full["parameterCheck"] = paramCheck;
        writeFile(pathJoin(opt.outDir, "vegasfx_params.json"), full.dump(1));
    };
    writeParamReport();
    if (opt.paramsOnly) {
        for (const auto& l : paramLines) line(l);
        line("");
        line("report: " + pathJoin(opt.outDir, "vegasfx_params.txt"));
        saveReport();
        for (const auto& nm : listDir(tmp)) removeFile(pathJoin(tmp, nm));
        std::error_code ec0;
        fs::remove(fs::u8path(tmp), ec0);
        return 0;
    }

    // ---- every effect in every context
    std::vector<RowResult> rows;
    std::vector<std::string> pngs;
    int number = 0, total = 0;
    for (const auto& id : order) {
        const Effect& e = effects[id];
        std::set<std::string> ctx;
        if (e.hasReal && e.realListed) for (const auto& c : e.realContexts) ctx.insert(c);
        for (const auto& c : e.builtinContexts) ctx.insert(c);
        total += (int)std::max<size_t>(1, ctx.size());
    }
    Json jsonRows = Json::array();
    for (const auto& id : order) {
        const Effect& e = effects[id];
        std::vector<std::string> ctxs;
        {
            std::set<std::string> seen;
            if (e.hasReal && e.realListed) for (const auto& c : e.realContexts) if (seen.insert(c).second) ctxs.push_back(c);
            for (const auto& c : e.builtinContexts) if (seen.insert(c).second) ctxs.push_back(c);
        }
        if (ctxs.empty()) ctxs.push_back(kOfxImageEffectContextFilter);      // (a real effect that did not load: reported once)
        for (const auto& context : ctxs) {
            ++number;
            RowResult row;
            row.id = id;
            row.label = e.label;
            row.group = e.group;
            row.context = context;
            row.file = e.hasReal ? e.file : std::string();
            const std::string base = fileSafe(id) + "_" + toLower(shortContext(context));
            const std::string realTag = base + "_real", builtinTag = base + "_builtin";
            const std::string scen = pathJoin(tmp, base + ".scenarios.json");
            removeFile(scen);
            std::printf("[%d/%d] %s (%s) ...\n", number, total, id.c_str(), shortContext(context).c_str());
            std::fflush(stdout);
            if (e.hasReal) {
                if (!e.realListed) {
                    row.real.present = true;
                    row.real.status = "does not load";
                    row.real.detail = e.realError;
                } else if (std::find(e.realContexts.begin(), e.realContexts.end(), context) == e.realContexts.end()) {
                    row.real.present = true;
                    row.real.status = "no " + toLower(shortContext(context)) + " version";
                } else {
                    std::string presets = pathJoin(pathJoin(pathDir(pathDir(e.file)), "Presets"), "PresetPackage.xml");
                    std::string text;
                    bool have = false;
                    if (readFile(presets, text))
                        for (const PresetEntry& pe : parsePresetPackage(text)) if (pe.plugin == id) have = true;
                    row.real = runSide(opt, "real", e, context, tmp, realTag, scen, have ? presets : std::string("builtin"));
                }
            }
            const bool builtinHasCtx = e.hasBuiltin && std::find(e.builtinContexts.begin(), e.builtinContexts.end(), context) != e.builtinContexts.end();
            if (builtinHasCtx) row.builtin = runSide(opt, "builtin", e, context, tmp, builtinTag, scen, "builtin");
            if (row.real.works && row.builtin.works) {
                compareSides(opt, row, tmp, realTag, builtinTag);
                if (!row.cmp.png.empty()) pngs.push_back(row.cmp.png);
            }
            row.verdict = verdictOf(row);
            std::printf("    real: %s; built-in: %s%s%s\n", row.real.present ? row.real.status.c_str() : "not installed",
                        row.builtin.present ? row.builtin.status.c_str() : "none",
                        row.cmp.done ? strfmt("; difference %.2f (worst) %.2f (defaults)", row.cmp.worstScore, row.cmp.defaultScore).c_str() : "",
                        "");
            std::fflush(stdout);
            // the raw pictures are not kept (VEGAS's own, with --keep-real, are: moved to reference/)
            if (opt.keepReal && row.real.works) {
                const std::string ref = pathJoin(pathJoin(opt.outDir, "reference"), base);
                makeDirs(ref);
                std::string t;
                if (readFile(scen, t)) writeFile(pathJoin(ref, "scenarios.json"), t);
                if (readFile(pathJoin(tmp, realTag + ".json"), t)) writeFile(pathJoin(ref, "real.json"), t);
                for (const auto& n : listDir(tmp))
                    if (n.compare(0, realTag.size(), realTag) == 0 && endsWithNoCase(n, ".bgra") && readFile(pathJoin(tmp, n), t))
                        writeFile(pathJoin(ref, n.substr(realTag.size() + 1)), t);
                for (int f = 0; f < opt.frames; ++f) {
                    ImagePtr src = sourceFrame(opt.width, opt.height, 10.0 + f, false);
                    if (src) writeFile(pathJoin(ref, strfmt("source_%d.bgra", f)), std::string((const char*)src->data.get(), src->byteSize()));
                }
            }
            for (const std::string* tag : {&realTag, &builtinTag})
                for (const auto& n : listDir(tmp))
                    if (n.compare(0, tag->size(), *tag) == 0 && endsWithNoCase(n, ".bgra")) removeFile(pathJoin(tmp, n));
            jsonRows.push(rowJson(row));
            rows.push_back(row);
            if (number % 5 == 0) {
                Json partial = Json::object();
                partial["partial"] = true;
                partial["effects"] = jsonRows;
                writeFile(pathJoin(opt.outDir, "vegasfx_report.json"), partial.dump(1));
            }
        }
    }

    // ---- the report
    auto section = [&](const std::string& title) { line(""); line("==== " + title + " ===="); };
    int realLoadFail = 0, realNoInst = 0, realCrash = 0, realRenderFail = 0, realNothing = 0, realWorks = 0, realOnlyFail = 0;
    int same = 0, close = 0, visible = 0, very = 0, builtinWorks = 0, builtinFails = 0, builtinSuspect = 0, realCount = 0;
    for (const RowResult& r : rows) {
        if (r.real.present) {
            ++realCount;
            const std::string& s = r.real.status;
            if (r.real.good()) ++realWorks;
            else if (r.real.works) ++realNothing;
            else if (s == "does not load") ++realLoadFail;
            else if (s == "makes no instance") ++realNoInst;
            else if (s.compare(0, 7, "crashes") == 0) ++realCrash;
            else if (s == "render fails") ++realRenderFail;
            else if (s == "draws nothing") ++realNothing;
            if (!r.real.good() && !r.builtin.good()) ++realOnlyFail;
        }
        if (r.builtin.present) { if (r.builtin.good()) ++builtinWorks; else if (r.builtin.works) ++builtinSuspect; else ++builtinFails; }
        if (r.cmp.done) {
            if (r.verdict == "same picture") ++same;
            else if (r.verdict == "close") ++close;
            else if (r.verdict == "differs visibly") ++visible;
            else if (r.verdict == "very different") ++very;
        }
    }
    section("Summary");
    line(strfmt("effects tried (an effect used as a filter, a generator or a transition counts for each): %d", (int)rows.size()));
    if (realCount) {
        line(strfmt("VEGAS's own plug-in:   %d work, %d do not load, %d make no instance, %d crash, %d fail to render, %d draw nothing / never change the picture / make one flat colour",
                    realWorks, realLoadFail, realNoInst, realCrash, realRenderFail, realNothing));
        line(strfmt("no way to use at all:  %d (VEGAS's plug-in fails and Reno has no working built-in)", realOnlyFail));
    } else line("VEGAS's own plug-ins:  none found on this machine");
    line(strfmt("Reno's built-in:       %d work, %d work but never change the picture (or only make one flat colour) in these tests, %d do not", builtinWorks, builtinSuspect, builtinFails));
    if (same + close + visible + very)
        line(strfmt("built-in against the real one: %d the same picture, %d close, %d differ visibly, %d very different", same, close, visible, very));

    section("VEGAS effects that do not work in Reno, with no built-in to take their place");
    int n = 0;
    for (const RowResult& r : rows)
        if (r.real.present && !r.real.good() && !r.builtin.good()) {
            line(strfmt("  %s (%s)  [%s]  %s%s%s", r.label.c_str(), shortContext(r.context).c_str(), r.id.c_str(), r.real.status.c_str(), r.real.detail.empty() ? "" : ": ", r.real.detail.c_str()));
            ++n;
        }
    if (!n) line("  none");

    section("VEGAS's own plug-ins that fail (Reno uses its built-in version of them instead)");
    n = 0;
    for (const RowResult& r : rows)
        if (r.real.present && !r.real.good() && r.builtin.good()) {
            line(strfmt("  %s (%s)  [%s]  %s%s%s", r.label.c_str(), shortContext(r.context).c_str(), r.id.c_str(), r.real.status.c_str(), r.real.detail.empty() ? "" : ": ", r.real.detail.c_str()));
            ++n;
        }
    if (!n) line("  none");

    section("Built-ins that look different from the real plug-in (worst first)");
    std::vector<const RowResult*> byScore;
    for (const RowResult& r : rows) if (r.cmp.done) byScore.push_back(&r);
    std::sort(byScore.begin(), byScore.end(), [](const RowResult* a, const RowResult* b) { return a->cmp.worstScore > b->cmp.worstScore; });
    n = 0;
    line("  (difference = average difference over the picture, 0..255, of the red, green, blue and alpha channels averaged)");
    for (const RowResult* r : byScore) {
        if (r->cmp.worstScore < 0.25) break;
        line(strfmt("  %6.2f at the defaults %6.2f worst  %s (%s)  [%s]  worst in \"%s\"%s%s", r->cmp.defaultScore, r->cmp.worstScore, r->label.c_str(),
                    shortContext(r->context).c_str(), r->id.c_str(), r->cmp.worstName.c_str(), r->cmp.png.empty() ? "" : "  picture: ", r->cmp.png.c_str()));
        ++n;
    }
    if (!n) line(realCount ? "  none: every built-in is within 0.25 of the real plug-in" : "  (nothing to compare with)");

    section("Speed (milliseconds for one picture at the defaults)");
    n = 0;
    for (const RowResult& r : rows) {
        if (!(r.real.msFrame >= 0 || r.builtin.msFrame >= 0)) continue;
        if (n++ == 0) line("      real   built-in");
        line(strfmt("  %8s %8s  %s (%s)", fmtMs(r.real.msFrame).c_str(), fmtMs(r.builtin.msFrame).c_str(), r.label.c_str(), shortContext(r.context).c_str()));
    }
    if (!n) line("  none");

    section("Every effect");
    for (const RowResult& r : rows) {
        line("");
        line(strfmt("%s (%s)  [%s]%s%s", r.label.c_str(), shortContext(r.context).c_str(), r.id.c_str(), r.group.empty() ? "" : "  in ", r.group.c_str()));
        line("  verdict: " + r.verdict);
        auto describe = [&](const char* who, const Side& s) {
            if (!s.present) { line(strfmt("  %s: %s", who, "not there")); return; }
            std::string t = strfmt("  %s: %s", who, s.status.c_str());
            if (s.tests) t += strfmt(" (%d of %d tests rendered", s.testsOk, s.tests) + (s.crashes ? strfmt(", %d crashed", s.crashes) : std::string()) + ")";
            if (s.msFrame >= 0) t += strfmt(", %s ms a picture (first %s)", fmtMs(s.msFrame).c_str(), fmtMs(s.msFirst).c_str());
            if (!s.detail.empty()) t += ": " + s.detail;
            line(t);
            for (const auto& f : s.failures) line("      failed: " + f);
        };
        describe("real   ", r.real);
        describe("built-in", r.builtin);
        if (r.cmp.done)
            line(strfmt("  difference: %.2f at the defaults, %.2f worst (\"%s\"), largest single difference %.0f of 255, %d pictures compared%s%s",
                        r.cmp.defaultScore, r.cmp.worstScore, r.cmp.worstName.c_str(), r.cmp.maxAbs, r.cmp.compared, r.cmp.png.empty() ? "" : ", picture: ", r.cmp.png.c_str()));
        std::set<std::string> miss;
        for (const Side* s : {&r.real, &r.builtin})
            for (const auto& rec : s->merged.get("scenarios").items())
                for (const auto& m : rec.get("missingSettings").items()) miss.insert(m.asString() + (s == &r.real ? " (real)" : " (built-in)"));
        if (!miss.empty()) {
            std::string t = "  settings one side does not have:";
            for (const auto& m : miss) t += " " + m + ";";
            line(t);
        }
    }

    section("Settings of the built-ins against VEGAS's resource files");
    for (const auto& l : paramLines) line(l);

    if (pngs.size() > 60) {
        std::vector<const RowResult*> have;
        for (const RowResult& r : rows) if (!r.cmp.png.empty()) have.push_back(&r);
        std::sort(have.begin(), have.end(), [](const RowResult* a, const RowResult* b) { return a->cmp.worstScore > b->cmp.worstScore; });
        for (size_t k = 60; k < have.size(); ++k) removeFile(pathJoin(opt.outDir, have[k]->cmp.png));
        line("");
        line(strfmt("(pictures of the 60 worst cases were kept; %d others were not)", (int)(have.size() - 60)));
    }
    line("");
    line("report: " + pathJoin(opt.outDir, "vegasfx_report.txt"));
    saveReport();
    Json full = Json::object();
    full["reno"] = "vegasfx";
    full["when"] = when;
    full["pictureSize"] = strfmt("%dx%d", opt.width, opt.height);
    full["vegasPluginsFound"] = realEffects;
    Json fp = Json::array();
    for (const auto& p : fileProblems) fp.push(p);
    full["fileProblems"] = fp;
    full["effects"] = jsonRows;
    full["parameterCheck"] = paramCheck;
    writeFile(pathJoin(opt.outDir, "vegasfx_report.json"), full.dump(1));
    for (const auto& nm : listDir(tmp)) removeFile(pathJoin(tmp, nm));
    std::error_code ec;
    fs::remove(fs::u8path(tmp), ec);
    return 0;
}

}  // namespace ofx
}  // namespace reno

// ====================================================================================================== parameter lists against VEGAS's resource files
namespace reno {
namespace ofx {
namespace {

struct XNode {
    std::string tag, text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XNode> kids;
    std::string attr(const std::string& n) const
    {
        for (const auto& a : attrs) if (a.first == n) return a.second;
        return std::string();
    }
};

std::string xmlText(const std::string& s)
{
    std::string out;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] != '&') { out += s[k]; continue; }
        const size_t semi = s.find(';', k);
        if (semi == std::string::npos || semi - k > 10) { out += s[k]; continue; }
        const std::string e = s.substr(k + 1, semi - k - 1);
        unsigned long cp = 0;
        if (e == "amp") cp = '&'; else if (e == "lt") cp = '<'; else if (e == "gt") cp = '>'; else if (e == "quot") cp = '"'; else if (e == "apos") cp = '\'';
        else if (e.size() > 1 && e[0] == '#') cp = std::strtoul(e.c_str() + (e[1] == 'x' ? 2 : 1), nullptr, e[1] == 'x' ? 16 : 10);
        if (!cp) { out += s[k]; continue; }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        k = semi;
    }
    size_t a = 0, b = out.size();
    while (a < b && (unsigned char)out[a] <= ' ') ++a;
    while (b > a && (unsigned char)out[b - 1] <= ' ') --b;
    return out.substr(a, b - a);
}

// A small XML reader: elements, attributes and text; comments, declarations and CDATA-free. Enough for resource files.
bool parseXml(const std::string& s, XNode& root)
{
    std::vector<XNode*> stack;
    root = XNode();
    root.tag = "#root";
    stack.push_back(&root);
    size_t at = 0;
    while (at < s.size()) {
        const size_t lt = s.find('<', at);
        if (lt == std::string::npos) break;
        if (lt > at && !stack.empty()) stack.back()->text += s.substr(at, lt - at);
        if (s.compare(lt, 4, "<!--") == 0) { const size_t e = s.find("-->", lt); if (e == std::string::npos) return false; at = e + 3; continue; }
        if (s.compare(lt, 2, "<?") == 0) { const size_t e = s.find("?>", lt); if (e == std::string::npos) return false; at = e + 2; continue; }
        if (s.compare(lt, 2, "<!") == 0) { const size_t e = s.find('>', lt); if (e == std::string::npos) return false; at = e + 1; continue; }
        size_t gt = lt + 1;
        char q = 0;
        while (gt < s.size() && (q || s[gt] != '>')) {
            if (q && s[gt] == q) q = 0;
            else if (!q && (s[gt] == '"' || s[gt] == '\'')) q = s[gt];
            ++gt;
        }
        if (gt >= s.size()) return false;
        std::string body = s.substr(lt + 1, gt - lt - 1);
        at = gt + 1;
        if (!body.empty() && body[0] == '/') {
            if (stack.size() > 1) stack.pop_back();
            continue;
        }
        const bool selfClose = !body.empty() && body.back() == '/';
        if (selfClose) body.pop_back();
        XNode n;
        size_t k = 0;
        while (k < body.size() && (unsigned char)body[k] > ' ') ++k;
        n.tag = body.substr(0, k);
        while (k < body.size()) {
            while (k < body.size() && (unsigned char)body[k] <= ' ') ++k;
            const size_t ns = k;
            while (k < body.size() && body[k] != '=' && (unsigned char)body[k] > ' ') ++k;
            const std::string name = body.substr(ns, k - ns);
            while (k < body.size() && (unsigned char)body[k] <= ' ') ++k;
            if (k >= body.size() || body[k] != '=') break;
            ++k;
            while (k < body.size() && (unsigned char)body[k] <= ' ') ++k;
            if (k >= body.size() || (body[k] != '"' && body[k] != '\'')) break;
            const char qq = body[k++];
            const size_t ve = body.find(qq, k);
            if (ve == std::string::npos) break;
            n.attrs.push_back({name, xmlText(body.substr(k, ve - k))});
            k = ve + 1;
        }
        stack.back()->kids.push_back(n);
        if (!selfClose) stack.push_back(&stack.back()->kids.back());
    }
    return true;
}

struct XParam {
    std::string type, name, label;
    std::vector<std::string> choices;
    bool hasChoices = false;
};
// effect id -> context ("default", "Filter", ...) -> parameters in file order
typedef std::map<std::string, std::map<std::string, std::vector<XParam>>> XResources;

void readXParams(const XNode& ctx, std::vector<XParam>& out)
{
    for (const XNode& p : ctx.kids) {
        if (p.tag.compare(0, 12, "OfxParamType") != 0) continue;
        XParam x;
        x.type = p.tag;
        x.name = p.attr("name");
        for (const XNode& c : p.kids) {
            if (c.tag == "OfxPropLabel") x.label = xmlText(c.text);
            else if (c.tag == "OfxParamPropChoiceOption") { x.choices.push_back(xmlText(c.text)); x.hasChoices = true; }
        }
        bool dup = false;
        for (XParam& o : out) if (o.name == x.name) { dup = true; if (o.label.empty()) o.label = x.label; if (!o.hasChoices && x.hasChoices) { o.choices = x.choices; o.hasChoices = true; } }
        if (!dup) out.push_back(x);
    }
}

void readResourceFile(const std::string& path, XResources& res)
{
    std::string text;
    XNode root;
    if (!readFile(path, text) || !parseXml(text, root)) return;
    std::function<void(const XNode&)> walk = [&](const XNode& n) {
        if (n.tag == "OfxPlugin" && !n.attr("name").empty()) {
            auto& byCtx = res[n.attr("name")];
            for (const XNode& set : n.kids) {
                if (set.tag != "OfxResourceSet") continue;
                for (const XNode& ctx : set.kids) {
                    if (ctx.tag != "OfxImageEffectContext") continue;
                    std::string name = ctx.attr("name");
                    const std::string pre = "OfxImageEffectContext";
                    if (name.compare(0, pre.size(), pre) == 0) name = name.substr(pre.size());
                    readXParams(ctx, byCtx[name]);
                }
            }
            if (byCtx.empty()) byCtx["default"];
            return;
        }
        for (const XNode& k : n.kids) walk(k);
    };
    walk(root);
}

bool isLocaleName(const std::string& f)
{
    if (f.size() < 10) return false;
    const std::string t = f.substr(f.size() - 9);
    return std::isalpha((unsigned char)t[0]) && std::isalpha((unsigned char)t[1]) && (t[2] == '-' || t[2] == '_') && std::isupper((unsigned char)t[3]) &&
           std::isupper((unsigned char)t[4]) && t.compare(5, 4, ".xml") == 0;
}

void collectResourceDirs(const std::string& dir, std::vector<std::string>& out)
{
    if (dirExists(pathJoin(pathJoin(dir, "Contents"), "Resources"))) { out.push_back(pathJoin(pathJoin(dir, "Contents"), "Resources")); return; }
    for (const std::string& n : listDir(dir)) {
        const std::string sub = pathJoin(dir, n);
        if (dirExists(sub) && dirExists(pathJoin(pathJoin(sub, "Contents"), "Resources"))) out.push_back(pathJoin(pathJoin(sub, "Contents"), "Resources"));
    }
}

std::string joinList(const std::vector<std::string>& v)
{
    std::string s;
    for (size_t k = 0; k < v.size(); ++k) s += (k ? " | " : "") + v[k];
    return s;
}

}  // namespace

int compareParameterLists(const std::vector<std::string>& dirs, Json& out, std::vector<std::string>& lines)
{
    out = Json::object();
    XResources res;
    std::vector<std::string> resDirs;
    for (const auto& d : dirs) collectResourceDirs(d, resDirs);
    int files = 0;
    for (const auto& rd : resDirs)
        for (const std::string& n : listDir(rd)) {
            if (!endsWithNoCase(n, ".xml") || isLocaleName(n)) continue;
            const std::string low = toLower(n);
            if (low.find("preset") != std::string::npos || low == "gui.xml") continue;
            readResourceFile(pathJoin(rd, n), res);
            ++files;
        }
    out["resourceFolders"] = (int)resDirs.size();
    out["resourceFiles"] = files;
    out["effectsInResources"] = (int)res.size();
    lines.push_back(strfmt("resource files read: %d (in %d bundles); effects described in them: %d", files, (int)resDirs.size(), (int)res.size()));
    if (res.empty()) {
        lines.push_back("  nothing to compare with (no bundle's Contents/Resources/*.xml found)");
        return 0;
    }
    Host& host = Host::get();
    Binary* bin = host.loadBinary(kBuiltinBinaryPath, nullptr);
    Json effects = Json::array(), noBuiltin = Json::array();
    int differ = 0, same = 0, labelOnly = 0, compared = 0;
    std::vector<std::string> noBuiltinLines, diffLines;
    for (const auto& kv : res) {
        const std::string& id = kv.first;
        Plugin* pl = findIn(bin, id);
        if (!pl || !pl->load(nullptr)) {
            noBuiltin.push(id);
            std::string ctxs;
            for (const auto& c : kv.second) if (!c.second.empty()) ctxs += (ctxs.empty() ? "" : ", ") + c.first + strfmt(" (%d settings)", (int)c.second.size());
            noBuiltinLines.push_back("  " + id + "  " + ctxs);
            continue;
        }
        const auto defIt = kv.second.find("default");
        Json eff = Json::object();
        eff["id"] = id;
        Json ctxArr = Json::array();
        bool effDiffers = false, effLabel = false, effGroups = false;
        std::vector<std::string> effLines;
        const char* const all[] = {"Filter", "Generator", "Transition", "General"};
        for (const char* c : all) {
            const std::string full = std::string("OfxImageEffectContext") + c;
            const auto ctxIt = kv.second.find(c);
            if (ctxIt == kv.second.end() && defIt == kv.second.end()) continue;
            if (ctxIt == kv.second.end() && !(std::string(c) == "Filter" || kv.second.size() == 1)) continue;
            std::vector<XParam> want;
            if (defIt != kv.second.end()) want = defIt->second;
            if (ctxIt != kv.second.end())
                for (const XParam& x : ctxIt->second) {
                    bool done = false;
                    for (XParam& w : want) if (w.name == x.name) { w = x; done = true; }
                    if (!done) want.push_back(x);
                }
            Json cj = Json::object();
            cj["context"] = c;
            if (!pl->supportsContext(full)) {
                if (ctxIt != kv.second.end()) {
                    cj["problem"] = "the built-in has no such context";
                    effLines.push_back(strfmt("    %s: VEGAS describes it in this context; the built-in does not support it", c));
                    effDiffers = true;
                    ctxArr.push(cj);
                }
                continue;
            }
            InstanceSetup setup;
            setup.width = 64;
            setup.height = 36;
            setup.vegasContext = full == kOfxImageEffectContextGenerator ? kVegasContextGenerator : kVegasContextEvent;
            std::string err;
            std::unique_ptr<Instance> inst = pl->createInstance(full, setup, &err);
            if (!inst) { cj["problem"] = "no instance: " + err; ctxArr.push(cj); effDiffers = true; effLines.push_back(strfmt("    %s: the built-in makes no instance (%s)", c, err.c_str())); continue; }
            ++compared;
            std::map<std::string, Param*> have, haveLower;
            for (const auto& p : inst->params.params) { have[p->name] = p.get(); haveLower[toLower(p->name)] = p.get(); }
            Json caseOnly = Json::array();
            Json missing = Json::array(), typeDiff = Json::array(), choiceDiff = Json::array(), labelDiff = Json::array(), extra = Json::array();
            Json missingGroups = Json::array();
            for (const XParam& x : want) {
                auto it = have.find(x.name);
                if (it == have.end()) {
                    auto lo = haveLower.find(toLower(x.name));
                    if (lo != haveLower.end() && lo->second->type == x.type) { caseOnly.push(x.name + " = " + lo->second->name); continue; }
                }
                if (it == have.end()) { (x.type == "OfxParamTypeGroup" ? missingGroups : missing).push(x.name + " (" + x.type + ")"); continue; }
                const Param* p = it->second;
                if (p->type != x.type) { typeDiff.push(x.name + ": VEGAS " + x.type + ", built-in " + p->type); continue; }
                if (x.hasChoices) {
                    const std::vector<std::string> mine = p->choices();
                    if (mine != x.choices) choiceDiff.push(x.name + ": VEGAS [" + joinList(x.choices) + "], built-in [" + joinList(mine) + "]");
                }
                if (!x.label.empty() && p->label() != x.label) labelDiff.push(x.name + ": VEGAS \"" + x.label + "\", built-in \"" + p->label() + "\"");
            }
            // settings the built-in has that the resource file does not list (the file only lists what it gives labels to)
            for (const auto& p : inst->params.params) {
                bool listed = false;
                for (const XParam& x : want) if (x.name == p->name || toLower(x.name) == toLower(p->name)) listed = true;
                if (!listed && p->kind != ParamKind::Group && p->kind != ParamKind::Page && p->name != kOfxImageEffectTransitionParamName) extra.push(p->name + " (" + p->type + ")");
            }
            cj["wanted"] = (int)want.size();
            cj["builtinSettings"] = (int)inst->params.params.size();
            cj["missingInBuiltin"] = missing;
            cj["groupsMissingInBuiltin"] = missingGroups;
            cj["typeDiffers"] = typeDiff;
            cj["choicesDiffer"] = choiceDiff;
            cj["labelsDiffer"] = labelDiff;
            cj["notInResourceFile"] = extra;
            cj["nameCaseOnly"] = caseOnly;
            ctxArr.push(cj);
            const bool hard = missing.size() || typeDiff.size() || choiceDiff.size();
            if (hard) effDiffers = true;
            if (labelDiff.size()) effLabel = true;
            if (missingGroups.size()) effGroups = true;
            auto addAll = [&](const char* what, const Json& a) { for (const auto& v : a.items()) effLines.push_back(strfmt("    %s: %s: %s", c, what, v.asString().c_str())); };
            addAll("missing in the built-in", missing);
            addAll("group missing in the built-in", missingGroups);
            addAll("type differs", typeDiff);
            addAll("choices differ", choiceDiff);
            addAll("label differs", labelDiff);
            if (extra.size()) {
                std::string t;
                for (const auto& v : extra.items()) t += (t.empty() ? "" : ", ") + v.asString();
                effLines.push_back(strfmt("    %s: the built-in also has (not in VEGAS's file, which lists only what it labels): %s", c, t.c_str()));
            }
        }
        eff["contexts"] = ctxArr;
        eff["differs"] = effDiffers;
        eff["labelsDiffer"] = effLabel;
        eff["groupsDiffer"] = effGroups;
        effects.push(eff);
        if (effDiffers) { ++differ; diffLines.push_back("  " + id + " (" + pl->info.label + ")"); for (const auto& l : effLines) diffLines.push_back(l); }
        else if (effLabel || effGroups) { ++labelOnly; diffLines.push_back("  " + id + " (" + pl->info.label + ")  - only labels or groups differ"); for (const auto& l : effLines) diffLines.push_back(l); }
        else ++same;
    }
    out["builtinsCompared"] = same + differ + labelOnly;
    out["sameParameters"] = same;
    out["parametersDiffer"] = differ;
    out["onlyLabelsDiffer"] = labelOnly;
    out["effects"] = effects;
    out["noBuiltin"] = noBuiltin;
    lines.push_back(strfmt("built-ins with a resource entry: %d: %d have VEGAS's settings (names, types, choices), %d differ, %d differ only in labels or groups", same + differ + labelOnly, same, differ, labelOnly));
    lines.push_back("  (the resource files list the settings they give labels to, normally all; \"missing\" = in VEGAS's list, not in the built-in)");
    lines.push_back(strfmt("effects in VEGAS's resource files with no built-in: %d", (int)noBuiltinLines.size()));
    lines.push_back("");
    lines.push_back("-- built-ins whose settings differ from VEGAS's --");
    if (diffLines.empty()) lines.push_back("  none");
    for (const auto& l : diffLines) lines.push_back(l);
    lines.push_back("");
    lines.push_back("-- effects in VEGAS's resource files with no built-in --");
    if (noBuiltinLines.empty()) lines.push_back("  none");
    for (const auto& l : noBuiltinLines) lines.push_back(l);
    return differ;
}

}  // namespace ofx
}  // namespace reno
