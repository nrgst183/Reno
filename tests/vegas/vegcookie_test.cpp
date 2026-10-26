// vegcookie_test: Cookie Cutter (and every other video effect chain) of the sample VEGAS projects, from the .veg to the
// picture.
//
//   vegcookie_test [--list] [folder or .veg ...]           every OpenFX record in the files against what Reno's
//                                                          project gets (default folder $RENO_SAMPLES)
//   vegcookie_test --jobs JOBS.json [folder ...]           every distinct Cookie Cutter setting of the projects, at the
//                                                          frames that matter, for vegofx_wine.sh (VEGAS's own effect)
//   vegcookie_test --compare WINEOUT JOBS.json [OUTDIR]    Reno's built-in Cookie Cutter on the same jobs, against what
//                                                          VEGAS's own made (pictures side by side in OUTDIR)
//   vegcookie_test --render OUTDIR [folder ...]            the projects rendered by the engine (built-in Cookie Cutter,
//                                                          stand-in media) at the frames where Cookie Cutter is on, as PNGs
//
// 1. Every OpenFX record in each file (found by its class GUID) must reach the project: on an event, a track, a media
//    item, the video output or as a generator. All parameters must be read, with as many keyframes as the record has
//    and their interpolation. Cookie Cutter's ten settings are checked against the types VEGAS's own Cookie Cutter
//    declares (read through Reno's host under Wine), value by value against the bytes.
// 2. The chains Cookie Cutter is in are printed (--list): where, in what order, which effects are off or come before
//    pan/crop.
// 3. Copy/paste of an event and Paste Event Attributes keep the whole chain, values and keyframes.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/vegorigin.h"
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "../../src/ofx/ofxhost.h"
#include "../../src/ofx/probe.h"
#include "../../src/ofx/builtins.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include "../testdata.h"

using namespace reno;
using namespace reno::veg;

static int g_fail = 0, g_pass = 0;
static bool check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); std::fflush(stdout); }
    return ok;
}

static const char* kCookie = "com.vegascreativesoftware:cookiecutter";

// ------------------------------------------------------------------------------------------ a record, decoded on its own
// The parameters of a record as the bytes have them, decoded with the types the effect declares rather than guessed
// from their sizes (Reno's reader guesses). Only for effects whose declaration is known here (Cookie Cutter).
struct RawKey { double time = 0; uint32_t interp = 0; std::vector<double> v; };
struct RawParam { std::string name; uint32_t vlen = 0; Bytes value; };
static std::vector<RawParam> rawParams(const Bytes& b, size_t off)
{
    std::vector<RawParam> out;
    const uint32_t hs = b.u32(off), il = b.u32(off + 0x28), pl = b.u32(off + 0x2c);
    size_t p = off + hs + il + pl;
    const uint32_t blk = b.u32(p), cnt = b.u32(p + 4);
    const size_t end = p + 8 + blk;
    size_t q = p + 8;
    for (uint32_t i = 0; i < cnt && q + 8 <= end; ++i) {
        RawParam r;
        r.vlen = b.u32(q);
        const uint32_t nl = b.u32(q + 4);
        r.name = b.text(q + 8, nl);
        r.value = b.sub(q + 8 + nl, r.vlen);
        q += 8 + (size_t)nl + r.vlen;
        out.push_back(r);
    }
    return out;
}

// Cookie Cutter's declaration (VEGAS Pro 18's Vfx1.ofx, as it describes itself to Reno's host).
struct Decl { const char* name; bool whole; int dim; };
static const Decl kCookieDecl[] = {
    {"Color", false, 3}, {"Shape", true, 1}, {"Method", true, 1}, {"Feather", false, 1}, {"Border", false, 1},
    {"RepeatX", true, 1}, {"RepeatY", true, 1}, {"Size", false, 1}, {"Center", false, 2}, {"StereoscopicDepth", false, 1},
};

// Decodes one value of a declared type: doubles (8 bytes each) or 32-bit integers, then u32 keys, each key: f64 time,
// u32 interpolation, the value, and for doubles in and out tangents (two values' worth each).
static bool decode(const Bytes& v, const Decl& d, std::vector<double>& value, std::vector<RawKey>& keys)
{
    const size_t one = d.whole ? 4 : 8, vs = one * (size_t)d.dim;
    const size_t keySize = 12 + vs + (d.whole ? 0 : 4 * vs);
    if (v.size() < vs + 4) return false;
    value.clear();
    for (int k = 0; k < d.dim; ++k) value.push_back(d.whole ? (double)v.i32(k * one) : v.f64(k * one));
    const uint32_t n = v.u32(vs);
    if (v.size() != vs + 4 + (size_t)n * keySize) return false;
    keys.clear();
    for (uint32_t i = 0; i < n; ++i) {
        const size_t k0 = vs + 4 + i * keySize;
        RawKey k;
        k.time = v.f64(k0);
        k.interp = v.u32(k0 + 8);
        for (int c = 0; c < d.dim; ++c) k.v.push_back(d.whole ? (double)v.i32(k0 + 12 + c * one) : v.f64(k0 + 12 + c * one));
        keys.push_back(k);
    }
    return true;
}

// ------------------------------------------------------------------------------------------ where each slot came from
static const uint8_t* recordAddress(const FxSlot& s)
{
    const auto* o = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK);
    if (!o || !o->chunk) return nullptr;
    return o->chunk->data.data() + o->entryOff;
}

struct Place { std::string where; const FxChain* chain = nullptr; size_t index = 0; const Event* event = nullptr; const Track* track = nullptr; };
static std::vector<Place> places(const Project& p)
{
    std::vector<Place> out;
    for (const MediaItem& m : p.media)
        if (m.generated) out.push_back({"generator of media '" + m.name + "'", nullptr, 0, nullptr, nullptr});
    for (size_t t = 0; t < p.tracks.size(); ++t) {
        const Track& tr = p.tracks[t];
        for (size_t i = 0; i < tr.fx.size(); ++i) out.push_back({strfmt("track %zu '%s' track fx", t + 1, tr.name.c_str()), &tr.fx, i, nullptr, &tr});
        for (size_t e = 0; e < tr.events.size(); ++e) {
            const Event& ev = tr.events[e];
            for (size_t i = 0; i < ev.fx.size(); ++i)
                out.push_back({strfmt("track %zu event %zu (%.3f s '%s')", t + 1, e + 1, ev.start, ev.name.c_str()), &ev.fx, i, &ev, &tr});
        }
    }
    for (size_t i = 0; i < p.videoOut.size(); ++i) out.push_back({"video output", &p.videoOut, i, nullptr, nullptr});
    return out;
}

static std::string chainText(const FxChain& c)
{
    std::string s;
    for (size_t i = 0; i < c.size(); ++i) {
        const FxSlot& f = c[i];
        std::string id = f.kind == "ofx" ? f.pluginId : ("audio " + f.name);
        const size_t colon = id.find(':');
        if (colon != std::string::npos) id = id.substr(colon + 1);
        s += strfmt("%s%s%s%s", i ? " > " : "", id.c_str(), f.bypass ? " (off)" : "", f.beforePanCrop ? " (pre pan/crop)" : "");
    }
    return s;
}

static std::string valuesText(const Json& state)
{
    std::string s;
    for (const auto& m : state.get("params").members()) {
        s += " " + m.first + "=";
        const Json& v = m.second.get("v");
        for (size_t i = 0; i < v.size(); ++i) s += strfmt(i ? ",%.4g" : "%.4g", v.at(i).asDouble());
        if (m.second.has("k")) s += strfmt("[%zu keys]", m.second.get("k").size());
    }
    return s;
}

static std::vector<std::string> vegFiles(const std::vector<std::string>& args)
{
    std::vector<std::string> files;
    for (const auto& a : args) {
        if (std::filesystem::is_regular_file(a)) { files.push_back(a); continue; }
        for (const auto& e : std::filesystem::recursive_directory_iterator(a))
            if (e.is_regular_file() && endsWithNoCase(e.path().string(), ".veg")) files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// ------------------------------------------------------------------------------------------ 1 + 2: records against the project
static void checkFile(const std::string& path, bool list)
{
    const std::string name = std::filesystem::path(path).filename().string();
    std::string bytes;
    if (!check(readFile(path, bytes), name + ": read")) return;
    Project p;
    if (!check(importVegasProject(path, p).ok, name + ": imports")) return;

    // the records in the file as Reno's project holds them (VegOrigin keeps the file's bytes; addresses are in those)
    const VegFile* vf = nullptr;
    p.forEachFx([&](const FxSlot& s) { if (!vf && s.veg && s.veg->file) vf = s.veg->file.get(); });
    std::map<const uint8_t*, int> reached;
    p.forEachFx([&](const FxSlot& s) { if (const uint8_t* a = recordAddress(s)) ++reached[a]; });
    int records = 0, missed = 0;
    if (vf) {
        const Bytes all((const uint8_t*)vf->bytes.data(), vf->bytes.size());
        for (const OfxRecord& r : findOfxRecords(all, 0)) {
            ++records;
            const uint8_t* a = all.data() + (r.end - 0);       // (end only; the start is found below)
            (void)a;
        }
        // findOfxRecords does not say where each record starts: walk them again by their GUID
        static const uint8_t g[16] = {0x41, 0x1a, 0xfc, 0xa2, 0xf6, 0x01, 0x0e, 0x46, 0xae, 0x5a, 0x2b, 0xe4, 0xaf, 0x57, 0xb9, 0x2e};
        size_t f = all.find(g, 16, 0);
        while (f != std::string::npos) {
            OfxRecord r;
            if (f >= 4 && readOfxRecord(all, f - 4, r) && r.end > f) {
                if (!reached.count(all.data() + f - 4)) {
                    ++missed;
                    std::printf("  NOT REACHED: %s record at %zu (%s)\n", name.c_str(), f - 4, r.pluginId.c_str());
                }
                f = all.find(g, 16, r.end);
            } else {
                f = all.find(g, 16, f + 1);
            }
        }
    }
    check(missed == 0, strfmt("%s: all %d OpenFX records reach the project (%d do not)", name.c_str(), records, missed));

    // every slot: all parameters read, keys as many as the bytes have, Cookie Cutter value by value
    p.forEachFx([&](const FxSlot& s) {
        if (s.kind != "ofx") return;
        const uint8_t* a = recordAddress(s);
        if (!a || !vf) return;
        const Bytes all((const uint8_t*)vf->bytes.data(), vf->bytes.size());
        const size_t off = (size_t)(a - all.data());
        { OfxRecord probe; if (!readOfxRecord(all, off, probe)) return; }     // (VEGAS's managed effects - Rays, Glint - are not OpenFX records: vegmanaged_test)
        const std::vector<RawParam> raw = rawParams(all, off);
        const Json& ps = s.state.get("params");
        int unread = 0;
        for (const RawParam& r : raw) unread += ps.has(r.name) ? 0 : 1;
        check(unread == 0, strfmt("%s: %s at %zu: every parameter read (%d left out)", name.c_str(), s.pluginId.c_str(), off, unread));
        if (s.pluginId != kCookie) return;
        for (const Decl& d : kCookieDecl) {
            const RawParam* r = nullptr;
            for (const RawParam& x : raw) if (x.name == d.name) r = &x;
            if (!check(r != nullptr, strfmt("%s: Cookie Cutter at %zu has %s", name.c_str(), off, d.name))) continue;
            std::vector<double> v;
            std::vector<RawKey> keys;
            if (!check(decode(r->value, d, v, keys), strfmt("%s: Cookie Cutter at %zu: %s is laid out as declared", name.c_str(), off, d.name))) continue;
            const Json& e = ps.get(d.name);
            bool same = e.get("v").size() == v.size() && e.get("i").asBool() == d.whole;
            for (size_t k = 0; same && k < v.size(); ++k) same = e.get("v").at(k).asDouble() == v[k];
            check(same, strfmt("%s: Cookie Cutter at %zu: %s's value (%s)", name.c_str(), off, d.name, e.dump().c_str()));
            const Json& ks = e.get("k");
            bool keysSame = ks.size() == keys.size();
            for (size_t k = 0; keysSame && k < keys.size(); ++k) {
                keysSame = ks.at(k).at(0).asDouble() == keys[k].time && ks.at(k).size() == 1 + keys[k].v.size();
                for (size_t c = 0; keysSame && c < keys[k].v.size(); ++c) keysSame = ks.at(k).at(c + 1).asDouble() == keys[k].v[c];
            }
            check(keysSame, strfmt("%s: Cookie Cutter at %zu: %s's %zu keyframes", name.c_str(), off, d.name, keys.size()));
        }
    });

    if (!list) return;
    for (const Place& pl : places(p)) {
        if (!pl.chain) continue;
        const FxSlot& f = (*pl.chain)[pl.index];
        bool any = false;
        for (const FxSlot& x : *pl.chain) any = any || x.pluginId == kCookie;
        if (!any) continue;
        if (pl.index == 0) std::printf("%s: %s: %s\n", name.c_str(), pl.where.c_str(), chainText(*pl.chain).c_str());
        if (f.pluginId == kCookie) std::printf("      cookiecutter%s%s:%s\n", f.bypass ? " (off)" : "", f.beforePanCrop ? " (pre pan/crop)" : "", valuesText(f.state).c_str());
    }
}

// ------------------------------------------------------------------------------------------ keyframe curves through the .veg
// A number's keyframes keep their interpolation (Fast, Slow, Smooth, Sharp, Hold) from the .veg into Reno's state
// ("kc") and back: a Cookie Cutter record (of the first sample that has one) is given keyed Feather and Center with curves, written, read again.
// True when the file had a Cookie Cutter record (and the checks were made).
static bool checkKeyCurves(const std::string& file)
{
    std::string bytes;
    if (!readFile(file, bytes)) return false;
    const Bytes all((const uint8_t*)bytes.data(), bytes.size());
    for (const OfxRecord& r0 : findOfxRecords(all, 0)) {
        if (r0.pluginId != kCookie) continue;
        // where it starts: its end minus its length is not kept; look for it again
        static const uint8_t g[16] = {0x41, 0x1a, 0xfc, 0xa2, 0xf6, 0x01, 0x0e, 0x46, 0xae, 0x5a, 0x2b, 0xe4, 0xaf, 0x57, 0xb9, 0x2e};
        size_t f = all.find(g, 16, 0), start = std::string::npos;
        while (f != std::string::npos) {
            OfxRecord r;
            if (f >= 4 && readOfxRecord(all, f - 4, r) && r.end == r0.end) { start = f - 4; break; }
            f = all.find(g, 16, f + 1);
        }
        if (!check(start != std::string::npos, "the Cookie Cutter record found again")) return true;
        const Bytes orig = all.sub(start, r0.end - start);
        OfxWriteInput in;
        in.rawId = r0.rawId;
        in.preset = r0.preset;
        in.flags = r0.flags;
        in.original = orig;
        in.state = r0.state;
        check(writeOfxRecord(in).record == std::string((const char*)orig.data(), orig.size()), "the record written from its own state is the same bytes");
        Json st = r0.state;
        Json feather = Json::object(), center = Json::object();
        Json fv = Json::array(); fv.push(0.1); feather["v"] = fv;
        Json fk = Json::array();
        for (double t : {0.0, 10.0, 20.0, 30.0}) { Json k = Json::array(); k.push(t); k.push(t / 40.0); fk.push(k); }
        feather["k"] = fk;
        Json fc = Json::array();
        const int curves[3] = {1, 3, 5};        // Fast at 0, Smooth at 10, Hold at 20, Linear at 30
        for (int i = 0; i < 3; ++i) { Json c = Json::array(); c.push(i * 10.0); c.push((double)curves[i]); fc.push(c); }
        feather["kc"] = fc;
        st["params"]["Feather"] = feather;
        Json cv = Json::array(); cv.push(0.5); cv.push(0.5); center["v"] = cv;
        Json ck = Json::array();
        for (double t : {0.0, 12.0}) { Json k = Json::array(); k.push(t); k.push(0.2 + t / 24); k.push(0.7); ck.push(k); }
        center["k"] = ck;
        Json cc = Json::array(); Json one = Json::array(); one.push(0.0); one.push(4.0); cc.push(one);
        center["kc"] = cc;
        st["params"]["Center"] = center;
        in.state = st;
        const std::string rec = writeOfxRecord(in).record;
        OfxRecord back;
        if (!check(readOfxRecord(Bytes((const uint8_t*)rec.data(), rec.size()), 0, back), "the record with curves reads back")) return true;
        const Json& bf = back.state.get("params").get("Feather");
        check(bf.get("k").size() == 4 && bf.get("kc").dump() == fc.dump(), "Feather's key curves come back: " + bf.get("kc").dump());
        check(back.state.get("params").get("Center").get("kc").dump() == cc.dump(), "Center's Sharp key comes back");
        // the interpolation numbers VEGAS's scripting uses: Fast 2, Smooth 4, Hold 6, Linear 1
        check(curveOfOfxInterp(2) == 1 && curveOfOfxInterp(4) == 3 && curveOfOfxInterp(6) == 5 && curveOfOfxInterp(1) == 0 &&
              curveOfOfxInterp(7) == 0 && ofxInterpOfCurve(0) == 1 && ofxInterpOfCurve(5) == 6, "interpolation numbers");
        // back to Linear: every key 1 again
        st["params"]["Feather"]["kc"] = Json::array();
        in.state = st;
        in.original = Bytes((const uint8_t*)rec.data(), rec.size());
        const std::string rec2 = writeOfxRecord(in).record;
        OfxRecord back2;
        readOfxRecord(Bytes((const uint8_t*)rec2.data(), rec2.size()), 0, back2);
        check(!back2.state.get("params").get("Feather").has("kc"), "curves set back to Linear are written as Linear");
        return true;
    }
    return false;
}

static ImagePtr pattern(const std::string& kind, int w, int h);

// ------------------------------------------------------------------------------------------ presets
// VEGAS's Cookie Cutter presets give Repeat X and Y as 0 (the range is 1..100): a preset's settings are kept inside each
// setting's range ({"preset": true} states, as dragged from Video FX), so "Circle, Center" shows its circle; a project's
// settings are taken as saved. Through Reno's built-in Cookie Cutter.
static void checkPresetRange()
{
    std::string err;
    ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath, &err);
    ofx::Plugin* pl = ofx::Host::get().findPlugin(kCookie);
    if (!check(pl && pl->load(&err), "the built-in Cookie Cutter loads")) return;
    check(ofx::builtinPresetPackage().find("name=\"Circle, Center\"") != std::string::npos, "the built-in Cookie Cutter has VEGAS's presets");
    for (int preset = 0; preset < 2; ++preset) {
        ofx::InstanceSetup setup;
        setup.width = 320;
        setup.height = 180;
        std::unique_ptr<ofx::Instance> inst = pl->createInstance(pl->filterContext(), setup, &err);
        if (!check(inst != nullptr, "made")) return;
        Json st = Json::object();
        st["plugin"] = kCookie;
        Json ps = Json::object();
        for (const char* n : {"RepeatX", "RepeatY"}) { Json e = Json::object(); Json v = Json::array(); v.push(0.0); e["v"] = v; e["i"] = true; ps[n] = e; }
        { Json e = Json::object(); Json v = Json::array(); v.push(0.2); e["v"] = v; ps["Size"] = e; }
        st["params"] = ps;
        if (preset) st["preset"] = true;
        inst->loadState(st);
        for (const auto& c : inst->inputClipNames()) inst->setClipSource(c, [](double) { return pattern("white", 320, 180); });
        ofx::RenderOptions ro;
        ImagePtr out = inst->render(0, ro, &err);
        if (!check(out != nullptr, "rendered")) return;
        int opaque = 0;
        for (int y = 0; y < out->h; ++y) for (int x = 0; x < out->w; ++x) opaque += out->row(y)[x * 4 + 3] > 127;
        const double rx = inst->params.find("RepeatX")->value[0];
        if (preset) check(rx == 1 && opaque > 1000, strfmt("a preset's Repeat X of 0 is taken as 1 (%g) and the circle is there (%d pixels)", rx, opaque));
        else check(rx == 0 && opaque == 0, strfmt("a project's Repeat X of 0 stays 0 (%g) and cuts everything away, as VEGAS's effect does (%d pixels)", rx, opaque));
    }
}

// ------------------------------------------------------------------------------------------ jobs for VEGAS's own effect
// Every Cookie Cutter of the projects (switched off ones too), at the project's frame size, at its first frame and at
// its keyframes (the frames where the settings change), on colour bars.
static void writeJobs(const std::string& out, const std::vector<std::string>& files)
{
    Json jobs = Json::array();
    std::set<std::string> seen;
    for (const auto& path : files) {
        Project p;
        if (!importVegasProject(path, p).ok) continue;
        const std::string stem = pathStem(path);
        int n = 0;
        p.forEachFx([&](const FxSlot& s) {
            if (s.pluginId != kCookie) return;
            std::set<double> times = {0.0};
            for (const auto& m : s.state.get("params").members())
                for (const auto& k : m.second.get("k").items()) if (times.size() < 8) times.insert(k.at(0).asDouble() + 0.25);
            for (double t : times) {
                const std::string key = s.state.dump() + strfmt("@%g", t);
                if (!seen.insert(key).second) continue;
                Json j = Json::object();
                j["name"] = strfmt("%s_%d_t%g", stem.c_str(), n, t);
                for (char& c : const_cast<std::string&>(j.get("name").asString())) if (c == ' ') c = '_';
                j["plugin"] = kCookie;
                j["state"] = s.state;
                j["w"] = p.settings.width;
                j["h"] = p.settings.height;
                j["fps"] = p.settings.fps();
                j["time"] = t;
                j["pattern"] = "bars";
                jobs.push(j);
            }
            ++n;
        });
    }
    Json all = Json::object();
    Json d = Json::array();
    d.push(std::string(kCookie));
    all["describe"] = d;
    all["jobs"] = jobs;
    check(writeFile(out, all.dump(1)), "jobs written to " + out);
    std::printf("%zu jobs\n", jobs.size());
}

// ------------------------------------------------------------------------------------------ Reno's own against VEGAS's
// The test pictures vegofx_wine.cpp gives the effect (the same code).
static ImagePtr pattern(const std::string& kind, int w, int h)
{
    ImagePtr img = Image::make(w, h);
    static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {16, 16, 16}};
    for (int y = 0; y < h; ++y) {
        uint8_t* r = img->row(h - 1 - y);
        for (int x = 0; x < w; ++x) {
            uint8_t c[4] = {255, 255, 255, 255};
            if (kind != "white") {
                if (y < h * 3 / 4) {
                    const uint8_t* b = bars[x * 8 / w];
                    c[0] = b[0]; c[1] = b[1]; c[2] = b[2];
                } else {
                    c[0] = c[1] = c[2] = (uint8_t)(x * 255 / std::max(1, w - 1));
                }
            }
            if (kind == "half") { for (int k = 0; k < 3; ++k) c[k] = (uint8_t)((c[k] * 128 + 127) / 255); c[3] = 128; }
            r[x * 4 + 0] = c[2]; r[x * 4 + 1] = c[1]; r[x * 4 + 2] = c[0]; r[x * 4 + 3] = c[3];
        }
    }
    return img;
}

// An 8-bit RGBA PAM (top row first) as vegofx_wine writes them, into a Byte image (bottom row first, B G R A).
static ImagePtr readPam(const std::string& path)
{
    std::string s;
    if (!readFile(path, s)) return nullptr;
    const size_t e = s.find("ENDHDR\n");
    if (e == std::string::npos) return nullptr;
    int w = 0, h = 0;
    std::sscanf(s.c_str() + s.find("WIDTH"), "WIDTH %d", &w);
    std::sscanf(s.c_str() + s.find("HEIGHT"), "HEIGHT %d", &h);
    const size_t at = e + 7;
    if (w <= 0 || h <= 0 || s.size() < at + (size_t)w * h * 4) return nullptr;
    ImagePtr img = Image::make(w, h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = (const uint8_t*)s.data() + at + (size_t)y * w * 4;
        uint8_t* d = img->row(h - 1 - y);
        for (int x = 0; x < w; ++x) { d[x * 4] = src[x * 4 + 2]; d[x * 4 + 1] = src[x * 4 + 1]; d[x * 4 + 2] = src[x * 4]; d[x * 4 + 3] = src[x * 4 + 3]; }
    }
    return img;
}

static bool writePam(const std::string& path, const Image& a, const Image* b)
{
    // side by side: VEGAS's on the left, Reno's on the right, each over a grey checkerboard
    const int W = b ? a.w * 2 + 4 : a.w;
    std::string s = strfmt("P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", W, a.h);
    for (int y = a.h - 1; y >= 0; --y)
        for (int x = 0; x < W; ++x) {
            const Image* im = x < a.w ? &a : (x >= a.w + 4 ? b : nullptr);
            const int ix = x < a.w ? x : x - a.w - 4;
            uint8_t px[4] = {255, 255, 255, 255};
            if (im) {
                const uint8_t* p = im->row(y) + ix * 4;
                const int bg = (((ix / 8) + (y / 8)) % 2) ? 70 : 110;
                for (int c = 0; c < 3; ++c) px[c] = (uint8_t)std::min(255, p[2 - c] + bg * (255 - p[3]) / 255);
            }
            s.append((const char*)px, 4);
        }
    return writeFile(path, s);
}

struct Diff { double alphaAgree = 0, meanDiff = 0; };
static Diff compare(const Image& a, const Image& b)
{
    Diff d;
    if (a.w != b.w || a.h != b.h) return d;
    size_t agree = 0;
    double sum = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const uint8_t* p = a.row(y) + x * 4;
            const uint8_t* q = b.row(y) + x * 4;
            agree += ((p[3] > 127) == (q[3] > 127)) ? 1 : 0;
            for (int c = 0; c < 4; ++c) sum += std::abs(p[c] - q[c]);
        }
    const double n = (double)a.w * a.h;
    d.alphaAgree = agree / n;
    d.meanDiff = sum / (n * 4);
    return d;
}

static void compareJobs(const std::string& wineOut, const std::string& jobsPath, const std::string& outDir)
{
    std::string text, err;
    Json jobs;
    if (!check(readFile(jobsPath, text) && Json::parse(text, jobs, &err), "jobs read: " + jobsPath)) return;
    ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath, &err);
    ofx::Plugin* pl = ofx::Host::get().findPlugin(kCookie);
    if (!check(pl && pl->load(&err) && pl->binary && pl->binary->builtin, "Reno's built-in Cookie Cutter is there")) return;
    double worstAgree = 1, sumAgree = 0;
    int n = 0;
    for (const auto& j : jobs.get("jobs").items()) {
        const std::string name = j.get("name").asString();
        ImagePtr vegas = readPam(pathJoin(wineOut, name + ".pam"));
        if (!vegas) { std::printf("  (no picture of VEGAS's for %s)\n", name.c_str()); continue; }
        ofx::InstanceSetup setup;
        setup.width = j.get("w").asInt();
        setup.height = j.get("h").asInt();
        setup.fps = j.has("fps") ? j.get("fps").asDouble() : 30.0;
        std::unique_ptr<ofx::Instance> inst = pl->createInstance(pl->filterContext(), setup, &err);
        if (!check(inst != nullptr, name + ": made")) continue;
        const std::string pat = j.get("pattern").asString();
        const int w = setup.width, h = setup.height;
        for (const auto& c : inst->inputClipNames()) inst->setClipSource(c, [pat, w, h](double) { return pattern(pat, w, h); });
        inst->loadState(j.get("state"));
        ofx::RenderOptions ro;
        ro.interactive = false;
        ImagePtr reno = inst->render(j.get("time").asDouble(), ro, &err);
        if (!check(reno != nullptr, name + ": rendered")) continue;
        const Diff d = compare(*vegas, *reno);
        std::printf("  %-34s cut agrees on %6.2f%% of the pixels, mean difference %5.2f / 255\n", name.c_str(), d.alphaAgree * 100, d.meanDiff);
        worstAgree = std::min(worstAgree, d.alphaAgree);
        sumAgree += d.alphaAgree;
        ++n;
        if (!outDir.empty()) writePam(pathJoin(outDir, name + ".pam"), *vegas, reno.get());
    }
    if (n) std::printf("  %d pictures: the cut agrees on %.2f%% of the pixels on average, %.2f%% at worst\n", n, sumAgree / n * 100, worstAgree * 100);
    check(n > 0 && worstAgree > 0.97, "Reno's Cookie Cutter cuts where VEGAS's does (97 % of the pixels of every picture or more)");
}

// ------------------------------------------------------------------------------------------ the projects' pictures
// Stand-ins for the media files (none of them is on this machine): a test pattern of the size the project says.
class StandIn : public MediaReader {
public:
    MediaInfo mi;
    int tag = 1;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        if (!mi.hasVideo || mi.width <= 0 || mi.height <= 0) return nullptr;
        const double sc = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)(mi.width * sc + 0.5)), h = std::max(1, (int)(mi.height * sc + 0.5));
        return ofx::makeTestPattern(w, h, mi.isStill ? tag : std::floor(seconds * 30 + 1e-6) + tag * 20.0);
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static int diffPixels(const Image& a, const Image& b, std::vector<uint8_t>* mask)
{
    int n = 0;
    if (mask) mask->assign((size_t)a.w * a.h, 0);
    for (int y = 0; y < a.h && y < b.h; ++y)
        for (int x = 0; x < a.w && x < b.w; ++x) {
            const uint8_t* p = a.row(y) + x * 4;
            const uint8_t* q = b.row(y) + x * 4;
            const bool d = std::abs(p[0] - q[0]) > 3 || std::abs(p[1] - q[1]) > 3 || std::abs(p[2] - q[2]) > 3;
            if (d) ++n;
            if (mask && d) (*mask)[(size_t)y * a.w + x] = 1;
        }
    return n;
}

// Each event with a Cookie Cutter that is on, at a frame inside it: the project's picture (A), the same with the event
// left out (B: what is under it) and with its Cookie Cutter switched off (C). Where the cut takes the event away
// (C differs from B) the picture must be what is under it (A = B), and somewhere the cut must leave the event (A
// differs from B): Cookie Cutter's alpha is honoured by the compositing, whatever follows it in the chain, the track's
// compositing mode, parent/child and track motion.
static void renderProjects(const std::string& outDir, const std::vector<std::string>& files)
{
    std::string err;
    ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath, &err);
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    for (const auto& path : files) {
        Project p;
        if (!importVegasProject(path, p).ok) continue;
        std::map<std::string, MediaInfo> infos;
        for (MediaItem& m : p.media) {
            if (m.path.empty()) continue;
            // (the files are not here: the stand-ins take their place, as if they were)
            m.offline = false;
            if (!m.info.hasVideo && !m.info.hasAudio) m.info.hasVideo = true;
            if (m.info.width <= 0) { m.info.width = p.settings.width; m.info.height = p.settings.height; }
            if (m.info.fps <= 0) m.info.fps = 30;
            if (m.info.duration <= 0) m.info.duration = 600;
            MediaInfo mi = m.info;
            mi.path = m.path;
            if (!mi.hasVideo && !mi.hasAudio) mi.hasVideo = true;
            if (mi.width <= 0 || mi.height <= 0) { mi.width = p.settings.width; mi.height = p.settings.height; }
            if (mi.fps <= 0) mi.fps = 30;
            if (mi.duration <= 0) mi.duration = 600;
            infos[m.path] = mi;
        }
        auto opener = [infos](const std::string& f, std::string* e) -> std::unique_ptr<MediaReader> {
            const bool own = endsWithNoCase(f, ".this-event");
            auto it = infos.find(own ? f.substr(0, f.size() - 11) : f);
            if (it == infos.end()) { if (e) *e = "not in the project"; return std::unique_ptr<MediaReader>(); }
            std::unique_ptr<StandIn> r(new StandIn());
            r->mi = it->second;
            r->tag = own ? 13 : 1 + (int)(std::hash<std::string>()(f) % 7);
            return std::unique_ptr<MediaReader>(r.release());
        };
        const std::string stem = pathStem(path);
        for (size_t t = 0; t < p.tracks.size(); ++t)
            for (size_t e = 0; e < p.tracks[t].events.size(); ++e) {
                const Event& ev = p.tracks[t].events[e];
                int slot = -1;
                for (size_t i = 0; i < ev.fx.size(); ++i)
                    if (ev.fx[i].pluginId == kCookie && !ev.fx[i].bypass) slot = (int)i;
                if (slot < 0 || ev.mute) continue;
                // a moment where no other event of the track covers it (stutter edits stack events)
                double at = -1;
                for (int i = 0; i < 15 && at < 0; ++i) {
                    const int k = 8 + ((i % 2) ? -(i + 1) / 2 : i / 2);       // the middle first
                    const double tt = ev.start + ev.length * k / 16.0;
                    bool alone = true;
                    for (size_t o = 0; o < p.tracks[t].events.size(); ++o)
                        if (o != e && p.tracks[t].events[o].start <= tt && p.tracks[t].events[o].end() > tt) alone = false;
                    if (alone) at = tt;
                }
                if (at < 0) { std::printf("  (%s track %zu event %zu is covered by other events all along)\n", stem.c_str(), t + 1, e + 1); continue; }
                // the event gets a picture of its own (its file's stand-in may be under it on other tracks too)
                Project a = p;
                if (const MediaItem* m = a.findMedia(ev.mediaId)) {
                    MediaItem own = *m;
                    own.id = a.newId();
                    own.path = m->path + ".this-event";
                    a.media.push_back(own);
                    a.tracks[t].events[e].mediaId = own.id;
                }
                // twice: with the event's whole chain (shown; the chains' other effects - NewBlue's and such, not
                // here - are left out, and Reno's stand-ins for VEGAS's colour effects are not VEGAS's), and with the
                // Cookie Cutter alone on the event (checked)
                const Project whole = a;
                for (int pass = 0; pass < 2; ++pass) {
                    if (pass == 1)
                        for (size_t i = 0; i < a.tracks[t].events[e].fx.size(); ++i)
                            if ((int)i != slot) a.tracks[t].events[e].fx[i].bypass = true;
                    Project b = a, c = a;
                    b.tracks[t].events[e].mute = true;
                    c.tracks[t].events[e].fx[(size_t)slot].bypass = true;
                    ImagePtr img[3];
                    const Project* proj[3] = {&a, &b, &c};
                    for (int k = 0; k < 3; ++k) {
                        MediaPool pool;
                        pool.setOpener(opener);
                        FxRuntime fx;
                        fx.prepare(*proj[k]);
                        VideoEngine video(pool, fx);
                        FrameOptions opt;
                        opt.interactive = false;
                        img[k] = video.render(*proj[k], at, opt);
                    }
                    const std::string what = strfmt("%s track %zu event %zu at %.3f s%s", stem.c_str(), t + 1, e + 1, at, pass ? " (Cookie Cutter alone)" : " (whole chain)");
                    if (!check(img[0] && img[1] && img[2], what + ": rendered")) continue;
                    std::vector<uint8_t> covered;
                    const int cov = diffPixels(*img[2], *img[1], &covered);     // where the event shows without the cut
                    int through = 0, cutAway = 0, kept = 0;
                    for (int y = 0; y < img[0]->h; ++y)
                        for (int x = 0; x < img[0]->w; ++x) {
                            if (!covered[(size_t)y * img[0]->w + x]) continue;
                            const uint8_t* a = img[0]->row(y) + x * 4;
                            const uint8_t* bb = img[1]->row(y) + x * 4;
                            const bool same = std::abs(a[0] - bb[0]) <= 3 && std::abs(a[1] - bb[1]) <= 3 && std::abs(a[2] - bb[2]) <= 3;
                            if (same) ++through; else ++kept;
                        }
                    cutAway = through;
                    std::printf("  %s: the event covers %d pixels; with Cookie Cutter %d of them show what is under it, %d keep the event\n",
                                what.c_str(), cov, cutAway, kept);
                    if (pass == 1) {
                        check(cov > 0 && cutAway > 0, what + ": the cut lets what is under the event show through");
                        check(kept > 0, what + ": the cut leaves some of the event");
                    }
                    if (!outDir.empty()) {
                        std::string s = strfmt("P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", img[0]->w * 3, img[0]->h);
                        for (int y = img[0]->h - 1; y >= 0; --y)
                            for (int k = 0; k < 3; ++k)
                                for (int x = 0; x < img[0]->w; ++x) {
                                    const uint8_t* q = img[k]->row(y) + x * 4;
                                    const uint8_t px[4] = {q[2], q[1], q[0], 255};
                                    s.append((const char*)px, 4);
                                }
                        std::string name = strfmt("%s_t%zu_e%zu%s.pam", stem.c_str(), t + 1, e + 1, pass ? "_alone" : "");
                        for (char& ch : name) if (ch == ' ') ch = '_';
                        writeFile(pathJoin(outDir, name), s);
                    }
                    a = whole;
                }
            }
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    bool list = false;
    std::string jobs, wineOut, compareJobsPath, outDir, renderDir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") list = true;
        else if (a == "--jobs" && i + 1 < argc) jobs = argv[++i];
        else if (a == "--render" && i + 1 < argc) renderDir = argv[++i];
        else if (a == "--compare" && i + 2 < argc) {
            wineOut = argv[++i];
            compareJobsPath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') outDir = argv[++i];
        }
        else args.push_back(a);
    }
    if (!compareJobsPath.empty()) {
        compareJobs(wineOut, compareJobsPath, outDir);
        std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (args.empty()) {
        args.push_back(testDataDir("RENO_SAMPLES"));
        if (testDataMissing(args[0])) return 0;
    }
    if (!renderDir.empty()) {
        renderProjects(renderDir, vegFiles(args));
        std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (!jobs.empty()) {
        writeJobs(jobs, vegFiles(args));
        std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    for (const auto& f : vegFiles(args)) checkFile(f, list);
    {
        bool done = false;
        for (const std::string& f : testVegFiles(testDataDir("RENO_SAMPLES")))
            if (checkKeyCurves(f)) { done = true; break; }
        if (!done) std::printf("  (no sample has a Cookie Cutter: key curves not checked)\n");
    }
    checkPresetRange();
    std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
