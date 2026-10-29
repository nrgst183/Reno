// veglegacy_test: VEGAS's older video effects (Swirl, Pinch/Punch, Wave, TV Simulator, Light Rays; records by class GUID
// with their settings as plain numbers, see src/vegas/veglegacyfx.h) in the sample projects.
//
//   veglegacy_test [folder with .veg files]        (default $RENO_SAMPLES, searched recursively)
//
//  1. Census: every effect record in the sample projects is an OpenFX record, an XML (CoreGraphics) one or one of these,
//     and the importer makes an effect of each.
//  2. Values: what each project's records hold (Swirl, Pinch/Punch with its keyframes, TV Look, Wave, Light Rays) are
//     usable numbers in the ranges the effects take.
//  3. Rendering: the imported effects render, and each kind changes a picture in at least one sample.
//  4. A project written back without a change is the file, byte for byte.
//  5. Edits (on the first sample with a Swirl): values, Proportional, before pan/crop, bypass; only the edited numbers
//     change and the result reads back as saved. Damaged records do no harm.
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "../../src/ofx/builtins.h"
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/veglegacyfx.h"
#include "../../src/vegas/vegmanaged.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include "../testdata.h"

using namespace reno;

static int g_fail = 0, g_pass = 0;

static bool check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
    return ok;
}

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string headerPath(const std::string& bytes)
{
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    for (const auto& k : t.root.kids)
        if (!k.isList && k.id == veg::G::Header) return k.data.textZ(k.data.u32(0));
    return std::string();
}

static void forEachFx(Project& p, const std::function<void(FxSlot&)>& fn)
{
    for (auto& t : p.tracks) {
        for (auto& f : t.fx) fn(f);
        for (auto& e : t.events)
            for (auto& f : e.fx) fn(f);
    }
}

static FxSlot* nth(Project& p, const std::string& id, int n = 0)
{
    FxSlot* found = nullptr;
    int i = 0;
    forEachFx(p, [&](FxSlot& f) {
        if (!found && f.pluginId == id && i++ == n) found = &f;
    });
    return found;
}

static int countOf(Project& p, const std::string& id)
{
    int n = 0;
    forEachFx(p, [&](FxSlot& f) { n += f.pluginId == id; });
    return n;
}

static const char* const kV = "com.vegascreativesoftware:";
static std::string vid(const char* name) { return std::string(kV) + name; }

static double val(const FxSlot& f, const char* name, size_t i = 0)
{
    return f.state.get("params").get(name).get("v").at(i).asDouble(-1e99);
}

static Json valJson(const std::vector<double>& v)
{
    Json e = Json::object(), a = Json::array();
    for (double x : v) a.push(x);
    e["v"] = a;
    return e;
}

static void setParam(FxSlot& s, const char* name, const Json& e)
{
    Json st = s.state;
    st["params"][name] = e;
    s.state = st;
}

// ---------------------------------------------------------------------------------------------- the chunk tree
static void collectFx(const veg::Chunk& c, std::vector<const veg::Chunk*>& out)
{
    for (const auto& k : c.kids) {
        if (k.isList) collectFx(k, out);
        else if (k.id == veg::G::Fx) out.push_back(&k);
    }
}

// The legacy records of a file's bytes, in order.
static std::vector<veg::LegacyFxRecord> legacyRecords(const std::string& bytes)
{
    veg::ChunkTree tree;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), tree);
    std::vector<const veg::Chunk*> all;
    collectFx(tree.root, all);
    std::vector<veg::LegacyFxRecord> out;
    for (const auto* c : all) {
        veg::LegacyFxRecord r;
        if (veg::readLegacyFx(c->data, 0, r)) out.push_back(r);
    }
    return out;
}

// The first byte offsets at which two strings differ; the count of different bytes.
static size_t diffBytes(const std::string& a, const std::string& b, size_t* first = nullptr, size_t* last = nullptr)
{
    size_t n = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (a[i] != b[i]) {
            if (!n && first) *first = i;
            if (last) *last = i;
            ++n;
        }
    return n;
}

// ---------------------------------------------------------------------------------------------- VEGAS's presets

// ---------------------------------------------------------------------------------------------- rendering
class Still : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        // black left, white right, with a bright spot and a grey bar: something for every effect to act on
        ImagePtr img = Image::make(std::min(maxW, mi.width), std::min(maxH, mi.height), PixelDepth::Byte, false);
        for (int y = 0; y < img->h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < img->w; ++x, d += 4) {
                int v = x >= img->w / 2 ? 255 : 0;
                if (y > img->h / 3 && y < img->h / 2) v = 128;
                if ((x - img->w / 4) * (x - img->w / 4) + (y - img->h / 4) * (y - img->h / 4) < 40) v = 255;
                d[0] = d[1] = d[2] = (uint8_t)v;
                d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> openStill(const std::string&, std::string*)
{
    std::unique_ptr<Still> r(new Still());
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = 160;
    r->mi.height = 90;
    return std::unique_ptr<MediaReader>(r.release());
}

struct Rig {
    MediaPool pool;
    FxRuntime fx;
    VideoEngine video{pool, fx};
    FrameOptions fo;
    Rig() { pool.setOpener(openStill); }
    ~Rig() { fx.releaseExcept({}); }
    // One event of the still over 0..5 s, carrying 'slot' (if it has a plug-in id).
    Project with(const FxSlot* slot)
    {
        Project p;
        p.settings.width = 160;
        p.settings.height = 90;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        p.tracks.push_back(t);
        MediaItem mi;
        mi.id = p.newId();
        mi.path = "still";
        std::string err;
        mi.info = openStill("still", &err)->info();
        p.media.push_back(mi);
        Event e;
        e.id = p.newId();
        e.mediaId = mi.id;
        e.length = 5;
        if (slot) {
            FxSlot s;
            s.id = p.newId();
            s.kind = "ofx";
            s.pluginId = slot->pluginId;
            s.name = slot->pluginId;
            s.state = slot->state;
            e.fx.push_back(s);
        }
        p.tracks[0].events.push_back(e);
        return p;
    }
    ImagePtr render(const Project& p, double t)
    {
        fx.clear();
        pool.clear();
        fx.prepare(p);
        return video.render(p, t, fo);
    }
    std::string error(const Project& p)
    {
        const Event& e = p.tracks[0].events[0];
        return e.fx.empty() ? std::string() : fx.error(e.fx[0].id);
    }
};

static long pixelsDiffer(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b || a->w != b->w || a->h != b->h || a->depth != b->depth) return -1;
    long n = 0;
    for (int y = 0; y < a->h; ++y) {
        const size_t bytes = (size_t)a->w * 4 * (a->depth == PixelDepth::Float ? 4 : 1);
        if (std::memcmp(a->row(y), b->row(y), bytes) == 0) continue;
        for (int x = 0; x < a->w; ++x) {
            const size_t px = bytes / a->w;
            if (std::memcmp(a->row(y) + x * px, b->row(y) + x * px, px) != 0) ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------- main
int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    if (testDataMissing(dir)) return 0;
    std::map<std::string, int> kinds;
    int projectsWith = 0, unreadRecords = 0, recordTotal = 0;
    std::vector<std::pair<std::string, Project>> withLegacy;     // the samples that have older effects, read

    // ---------------------------------------------------------------- 1. census, and a round trip of every project that has one
    for (const auto& ent : std::filesystem::recursive_directory_iterator(dir)) {
        if (!ent.is_regular_file() || ent.path().extension() != ".veg") continue;
        const std::string path = ent.path().string();
        const std::string bytes = readFile(path);
        veg::ChunkTree tree;
        veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), tree);
        std::vector<const veg::Chunk*> all;
        collectFx(tree.root, all);
        int legacy = 0, other = 0;
        std::map<std::string, int> mine;
        for (const auto* c : all) {
            veg::OfxRecord o;
            veg::ManagedFxRecord m;
            veg::LegacyFxRecord l;
            if (!c->isList && veg::readOfxRecord(c->data, 0, o)) continue;
            if (veg::readManagedFx(c->data, 0, m) && !veg::managedPluginId(m).empty()) continue;
            if (veg::readLegacyFx(c->data, 0, l) && !veg::legacyPluginId(l).empty()) {
                ++legacy;
                kinds[veg::legacyPluginId(l)]++;
                mine[veg::legacyPluginId(l)]++;
                continue;
            }
            ++other;
        }
        unreadRecords += other;
        if (!check(other == 0, path + strfmt(": %d effect record(s) that no reader takes", other))) continue;
        if (legacy == 0) continue;
        ++projectsWith;
        recordTotal += legacy;
        Project p;
        const VegasImportResult r = importVegasProject(path, p);
        check(r.ok, path + ": opens");
        if (r.ok) withLegacy.push_back({path, p});
        for (const auto& kv : mine) check(countOf(p, kv.first) == kv.second, path + strfmt(": %d %s records, %d effects", kv.second, kv.first.c_str(), countOf(p, kv.first)));
        for (const auto& n : r.notes) {
            check(n.find("not OpenFX") == std::string::npos && n.find("managed") == std::string::npos && n.find("older kind") == std::string::npos &&
                      n.find("could not be read") == std::string::npos,
                  path + ": says an effect was skipped: " + n);
        }
        forEachFx(p, [&](FxSlot& f) {
            if (f.pluginId.find("swirl") != std::string::npos || f.pluginId.find("pinchpunch") != std::string::npos ||
                f.pluginId.find("tvsimulator") != std::string::npos || f.pluginId.find("wave") != std::string::npos ||
                f.pluginId.find("lightrays") != std::string::npos)
                check(f.veg != nullptr && f.state.get("params").isObject(), path + ": " + f.pluginId + " keeps its origin and settings");
        });
        // 4. written back unchanged: the file
        std::vector<std::string> warn;
        const std::string again = writeVegasData(p, headerPath(bytes), &warn);
        check(again == bytes, path + ": written back unchanged is the file");
    }
    std::printf("projects with the older effects: %d (%d records)\n", projectsWith, recordTotal);
    check(unreadRecords == 0, "no effect record anywhere is left unread");
    if (withLegacy.empty()) { std::printf("SKIP: no sample project has the older effects: %d passed, %d failed\n", g_pass, g_fail); return g_fail ? 1 : 0; }

    // ---------------------------------------------------------------- 2. values
    {
        int effects = 0, bad = 0;
        for (auto& wp : withLegacy)
            forEachFx(wp.second, [&](FxSlot& f) {
                if (f.pluginId.find("swirl") == std::string::npos && f.pluginId.find("pinchpunch") == std::string::npos &&
                    f.pluginId.find("tvsimulator") == std::string::npos && f.pluginId.find("wave") == std::string::npos &&
                    f.pluginId.find("lightrays") == std::string::npos)
                    return;
                ++effects;
                for (const auto& m : f.state.get("params").members()) {
                    const Json& v = m.second.get("v");
                    for (size_t i = 0; i < v.size(); ++i)
                        if (v.at(i).isNumber() && !std::isfinite(v.at(i).asDouble())) ++bad;
                    const Json& k = m.second.get("k");
                    for (size_t i = 0; i < k.size(); ++i)
                        if (!(k.at(i).at(0).asDouble(-1) >= 0)) ++bad;        // key times are frames from the start
                }
            });
        check(bad == 0, strfmt("the settings and key times of the %d older effects are usable numbers", effects));
    }

    // ---------------------------------------------------------------- 3. rendering
    {
        Rig rig;
        const ImagePtr plain = rig.render(rig.with(nullptr), 1.0);
        check(plain != nullptr, "the plain picture renders");
        for (const char* kind : {"swirl", "pinchpunch", "tvsimulator", "wave", "lightrays"}) {
            int found = 0, changed = 0;
            for (auto& wp : withLegacy)
                for (int n = 0; n < 3; ++n) {
                    FxSlot* s = nth(wp.second, vid(kind), n);
                    if (!s) break;
                    ++found;
                    const Project proj = rig.with(s);
                    for (double t : {0.2, 1.0}) {
                        const ImagePtr img = rig.render(proj, t);
                        check(img != nullptr && rig.error(proj).empty(), std::string(kind) + strfmt(" renders at %.1f s", t) + (rig.error(proj).empty() ? "" : ": " + rig.error(proj)));
                        if (pixelsDiffer(img, plain) > 20) { ++changed; break; }
                    }
                }
            if (found) check(changed > 0, std::string(kind) + strfmt(" changes the picture in at least one sample (%d of %d)", changed, found));
        }
    }

    // ---------------------------------------------------------------- 5. edits
    // the first sample with a Swirl is the base; the projects were read from files that are unchanged by the writing above
    std::string basePath;
    Project dfs;
    for (auto& wp : withLegacy)
        if (nth(wp.second, vid("swirl"))) { basePath = wp.first; dfs = wp.second; break; }
    if (basePath.empty()) { std::printf("no sample has a Swirl: edits not tried\n"); std::printf("%d passed, %d failed\n", g_pass, g_fail); return g_fail ? 1 : 0; }
    const std::string dfsBytes = readFile(basePath);
    const std::string dfsHdr = headerPath(dfsBytes);
    const std::string dfsPath = basePath;
    const auto origRecs = legacyRecords(dfsBytes);
    auto rawOf = [&](const std::string& bytes, const std::string& plugin, int n) {
        auto recs = legacyRecords(bytes);
        int i = 0;
        for (auto& r : recs)
            if (veg::legacyPluginId(r) == plugin && i++ == n) return r;
        return veg::LegacyFxRecord();
    };
    // the record of the base's first Swirl, as it is in the file
    const veg::LegacyFxRecord swirl0 = rawOf(dfsBytes, vid("swirl"), 0);
    {   // a value: one number of one key changes
        Project q = dfs;
        FxSlot* s = nth(q, vid("swirl"));
        const double oldH = val(*s, "ScalingHorizontal"), oldC = val(*s, "Center", 0);
        setParam(*s, "Amount", valJson({-0.25}));
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        check(warn.empty(), "an edit of a Swirl's Amount warns nothing");
        check(out.size() == dfsBytes.size(), "the file keeps its size");
        size_t first = 0, last = 0;
        const size_t n = diffBytes(dfsBytes, out, &first, &last);
        check(n <= 16 && (n == 0 || last - first < 0x100), strfmt("only the Amount's bytes changed (%zu bytes, %zu apart)", n, last - first));
        Project back;
        check(importVegasData(out, dfsPath, back).ok, "the edited file opens");
        FxSlot* b = nth(back, vid("swirl"));
        check(b && near(val(*b, "Amount"), -0.25) && near(val(*b, "ScalingHorizontal"), oldH) && near(val(*b, "Center", 0), oldC), "Amount -0.25 reads back, the rest the same");
        const auto raw = rawOf(out, vid("swirl"), 0);
        check(!raw.keys.empty() && raw.keys[0].v[0] == -0.25 && raw.statik == swirl0.statik && raw.keys[0].v[3] == swirl0.keys[0].v[3], "the static block and the other numbers are the file's");
    }
    {   // the centre and Proportional: V follows H
        Project q = dfs;
        FxSlot* s = nth(q, vid("swirl"));
        setParam(*s, "Center", valJson({0.25, 0.75}));
        {
            Json pe = valJson({1});
            pe["i"] = true;
            setParam(*s, "ScalingProportional", pe);
        }
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        const auto raw = rawOf(out, vid("swirl"), 0);
        if (check(!raw.keys.empty() && !swirl0.keys.empty(), "the Swirl has a key")) {
            check(near(raw.keys[0].v[1], -0.5) && near(raw.keys[0].v[2], 0.5), "the centre is written as the offset from the middle");
            check(raw.keys[0].v[3] == swirl0.keys[0].v[3] && raw.keys[0].v[4] == swirl0.keys[0].v[3], "Proportional: the vertical size follows the horizontal");
            uint64_t bits = 0;
            std::memcpy(&bits, &raw.keys[0].v[5], 8);
            check(bits == 1, "and the box is a 64-bit 1");
        }
        Project back;
        check(importVegasData(out, dfsPath, back).ok, "the proportional file opens");
        FxSlot* b = nth(back, vid("swirl"));
        check(b && val(*b, "ScalingProportional") == 1 && near(val(*b, "Center", 0), 0.25) && near(val(*b, "Center", 1), 0.75), "and reads back");
    }
    {   // before pan/crop: one bit per effect
        Project q = dfs;
        FxSlot* s = nth(q, vid("swirl"));
        const bool was = s->beforePanCrop;
        s->beforePanCrop = !was;
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        check(warn.empty(), "before pan/crop of a Swirl is saved without a warning");
        check(out.size() == dfsBytes.size() && diffBytes(dfsBytes, out) == 1, strfmt("one byte changed (%zu)", diffBytes(dfsBytes, out)));
        Project back;
        check(importVegasData(out, dfsPath, back).ok, "opens");
        FxSlot* bs = nth(back, vid("swirl"));
        check(bs && bs->beforePanCrop == !was, "and reads back");
    }
    {   // bypass: bit 0x01 of the record's flags, as in the OpenFX record (inferred: no sample record is off)
        Project q = dfs;
        FxSlot* s = nth(q, vid("swirl"));
        const bool was = s->bypass;
        const bool before = s->beforePanCrop;
        s->bypass = !was;
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        check(warn.empty(), "bypass of a Swirl is saved without a warning");
        check(out.size() == dfsBytes.size() && diffBytes(dfsBytes, out) == 1, "one byte changed");
        Project back;
        check(importVegasData(out, dfsPath, back).ok, "the bypassed file opens");
        FxSlot* bs = nth(back, vid("swirl"));
        check(bs && bs->bypass == !was && bs->beforePanCrop == before, "bypass reads back, and before pan/crop stays");
        // and put back
        bs->bypass = was;
        const std::string out2 = writeVegasData(back, dfsHdr, &warn);
        check(out2 == dfsBytes, "put back: the file's own bytes");
    }
    {   // another effect in the place of one: written as an OpenFX effect, the others untouched
        Project q = dfs;
        const int pinches = countOf(q, vid("pinchpunch")), swirls = countOf(q, vid("swirl"));
        FxSlot* s = nth(q, vid("swirl"));
        s->pluginId = vid("pinchpunch");
        Json st = s->state;
        st["plugin"] = vid("pinchpunch");
        s->state = st;
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        Project back;
        check(importVegasData(out, dfsPath, back).ok, "a replaced effect: the file opens");
        check(countOf(back, vid("pinchpunch")) == pinches + 1 && countOf(back, vid("swirl")) == swirls - 1, "the Swirl came back as a Pinch/Punch");
    }
    {   // a new Swirl in a project (no record to start from)
        Project q = dfs;
        const int swirls = countOf(q, vid("swirl"));
        Event* ev = nullptr;
        for (auto& t : q.tracks)
            for (auto& e : t.events)
                if (!ev && t.type == TrackType::Video && !e.fx.empty()) ev = &e;
        if (ev) {
            FxSlot n;
            n.id = q.newId();
            n.kind = "ofx";
            n.pluginId = vid("swirl");
            Json params = Json::object();
            params["Amount"] = valJson({0.5});
            Json st = Json::object();
            st["plugin"] = n.pluginId;
            st["params"] = params;
            n.state = st;
            ev->fx.push_back(n);
            std::vector<std::string> warn;
            const std::string out = writeVegasData(q, dfsHdr, &warn);
            Project back;
            check(importVegasData(out, dfsPath, back).ok && countOf(back, vid("swirl")) == swirls + 1, "an added Swirl is saved and read back");
        }
    }

    // ---------------------------------------------------------------- damaged records
    {
        const auto recs = legacyRecords(dfsBytes);
        veg::ChunkTree tree;
        veg::readChunkTree(veg::Bytes((const uint8_t*)dfsBytes.data(), dfsBytes.size()), tree);
        std::vector<const veg::Chunk*> all;
        collectFx(tree.root, all);
        int cut = 0, tried = 0;
        for (const auto* c : all) {
            veg::LegacyFxRecord r;
            if (!veg::readLegacyFx(c->data, 0, r)) continue;
            std::string raw((const char*)c->data.data(), c->data.size());
            for (size_t n = 0; n < r.end; ++n) {
                veg::LegacyFxRecord t;
                ++tried;
                if (veg::readLegacyFx(veg::Bytes((const uint8_t*)raw.data(), n), 0, t)) ++cut;
            }
            // damaged counts and sizes
            for (size_t off : {0x14u, 0x18u, 0x34u}) {
                for (uint32_t v : {0u, 1u, 7u, 0x7fffffffu, 0xffffffffu}) {
                    std::string d = raw;
                    std::memcpy(&d[off], &v, 4);
                    veg::LegacyFxRecord t;
                    veg::readLegacyFx(veg::Bytes((const uint8_t*)d.data(), d.size()), 0, t);
                }
            }
            break;
        }
        check(tried > 100, "cut-off records were tried");
        check(cut == 0, strfmt("no cut-off record is taken for whole (%d of %d)", cut, tried));
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
