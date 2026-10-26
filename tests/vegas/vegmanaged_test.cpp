// vegmanaged_test: VEGAS's managed ("CoreGraphics") video effects, Rays and Glint, in the sample projects.
//
//   vegmanaged_test [folder with .veg files]        (default $RENO_SAMPLES, searched recursively)
//
//  1. Every such record (found in the chunk tree, by its XML) becomes an effect of Reno's matching stand-in: none is
//     left unread, and the numbers match the records' own count.
//  2. The values are numbers in the scales VEGAS shows (Rays: Threshold, Amount, Boost, Peak, Hue... as percent or degrees;
//     see the scales note in vegmanaged.cpp); a Glint with Hue keyframes comes out with its keys, in order.
//  3. (on the first sample with a Rays) A project written without a change has the bytes of the file; with edits, only the edited XML elements differ
//     and the result reads back as saved (values, keyframes added and removed, bypass, mask mode).
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
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

static std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void forEachFx(Project& p, const std::function<void(FxSlot&)>& fn)
{
    for (auto& t : p.tracks) {
        for (auto& f : t.fx) fn(f);
        for (auto& e : t.events)
            for (auto& f : e.fx) fn(f);
    }
}

static bool isMine(const FxSlot& f)
{
    return f.pluginId == "com.vegascreativesoftware:raysvelvetmatter" || f.pluginId == "com.vegascreativesoftware:glintvelvetmatter";
}

static FxSlot* firstOf(Project& p, const std::string& id, int nth = 0)
{
    FxSlot* found = nullptr;
    int n = 0;
    forEachFx(p, [&](FxSlot& f) {
        if (!found && f.pluginId == id && n++ == nth) found = &f;
    });
    return found;
}

static double val(const FxSlot& f, const char* name, size_t i = 0)
{
    return f.state.get("params").get(name).get("v").at(i).asDouble(-1e99);
}

// The project's own path as the file's header holds it: what the writer wants as its 'path'.
static std::string headerPath(const std::string& bytes)
{
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    for (const auto& k : t.root.kids)
        if (!k.isList && k.id == veg::G::Header) return k.data.textZ(k.data.u32(0));
    return std::string();
}

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// How many managed Rays/Glint records are in the chunk tree of a file.
static int recordsIn(const veg::Chunk& c)
{
    int n = 0;
    for (const auto& k : c.kids) {
        if (k.isList) { n += recordsIn(k); continue; }
        veg::ManagedFxRecord r;
        if (k.id == veg::G::Fx && veg::readManagedFx(k.data, 0, r) && !veg::managedPluginId(r).empty()) ++n;
    }
    return n;
}

static int hiddenManaged(const veg::Chunk& c)
{
    int n = 0;
    for (const auto& k : c.kids) {
        if (k.isList) { n += hiddenManaged(k); continue; }
        veg::ManagedFxRecord r;
        if (k.id == veg::G::Fx && veg::readManagedFx(k.data, 0, r) && veg::managedPluginId(r).empty()) ++n;
    }
    return n;
}

// The XML texts of the records of a file, in order (all keys).
static std::vector<std::string> xmlsOf(const std::string& bytes)
{
    std::vector<std::string> out;
    for (size_t at = bytes.find("<?xml"); at != std::string::npos; at = bytes.find("<?xml", at + 1)) {
        // up to the closing tag of the root: the root's name follows "<?xml ...?>\r\n<"
        const size_t r0 = bytes.find("<", bytes.find("?>", at));
        size_t r1 = r0 + 1;
        while (r1 < bytes.size() && std::isalnum((unsigned char)bytes[r1])) ++r1;
        const std::string root = bytes.substr(r0 + 1, r1 - r0 - 1);
        const size_t end = bytes.find("</" + root + ">", at);
        if (end == std::string::npos) break;
        out.push_back(bytes.substr(at, end + root.size() + 3 - at));
    }
    return out;
}

static std::string lineDiff(const std::string& a, const std::string& b)
{
    std::vector<std::string> la, lb;
    std::stringstream sa(a), sb(b);
    std::string l;
    while (std::getline(sa, l)) la.push_back(l.size() && l.back() == '\r' ? l.substr(0, l.size() - 1) : l);
    while (std::getline(sb, l)) lb.push_back(l.size() && l.back() == '\r' ? l.substr(0, l.size() - 1) : l);
    if (la.size() != lb.size()) return "different line counts";
    std::string out;
    for (size_t i = 0; i < la.size(); ++i)
        if (la[i] != lb[i]) out += "[" + la[i] + " | " + lb[i] + "]";
    return out;
}

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    if (testDataMissing(dir)) return 0;
    int projectsWithFx = 0, rays = 0, glints = 0, recordTotal = 0;
    std::string dfs, glintPath;

    // ---------------------------------------------------------------- 1. every record is read
    for (const auto& ent : std::filesystem::recursive_directory_iterator(dir)) {
        if (!ent.is_regular_file() || ent.path().extension() != ".veg") continue;
        const std::string path = ent.path().string();
        const std::string bytes = readFile(path);
        veg::ChunkTree tree;
        veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), tree);
        const int inFile = recordsIn(tree.root);
        check(hiddenManaged(tree.root) == 0, path + ": a managed effect that Reno has no stand-in for");
        if (inFile == 0) continue;
        ++projectsWithFx;
        recordTotal += inFile;
        Project p;
        const VegasImportResult r = importVegasProject(path, p);
        check(r.ok, path + ": opens");
        int got = 0;
        forEachFx(p, [&](FxSlot& f) {
            if (!isMine(f)) return;
            ++got;
            (f.pluginId.find("rays") != std::string::npos ? rays : glints)++;
            check(f.state.get("params").isObject() && f.state.get("params").members().size() >= 10, path + ": settings read");
            check(f.veg != nullptr, path + ": keeps its origin");
        });
        check(got == inFile, path + strfmt(": %d records, %d effects read", inFile, got));
        for (const auto& n : r.notes)
            check(n.find("managed") == std::string::npos, path + ": says a managed effect was skipped: " + n);
        // 3a. no change: the same bytes
        std::vector<std::string> warn;
        const std::string again = writeVegasData(p, headerPath(bytes), &warn);
        check(again == bytes, path + ": written back unchanged is the file");
        if (dfs.empty() && firstOf(p, "com.vegascreativesoftware:raysvelvetmatter")) dfs = path;
        if (glintPath.empty())
            for (int n = 0; n < 99; ++n) {
                FxSlot* g = firstOf(p, "com.vegascreativesoftware:glintvelvetmatter", n);
                if (!g) break;
                const Json& hk = g->state.get("params").get("EffectHue").get("k");
                if (hk.size() >= 3) { glintPath = path; break; }
            }
    }
    std::printf("projects with Rays/Glint: %d (%d records: %d Rays, %d Glint)\n", projectsWithFx, recordTotal, rays, glints);
    if (dfs.empty()) {
        std::printf("SKIP: no sample project has a Rays: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    // ---------------------------------------------------------------- 2. the values
    const std::string bytes = readFile(dfs);
    const std::string dfsHdr = headerPath(bytes);
    Project p;
    check(importVegasProject(dfs, p).ok, "the sample with a Rays opens");
    FxSlot* rs = firstOf(p, "com.vegascreativesoftware:raysvelvetmatter");
    if (!check(rs != nullptr, "it has a Rays")) return 1;
    check(rs->kind == "ofx", "Rays is an OpenFX effect here");
    {
        bool finite = true;
        for (const char* n : {"EffectThreshold", "EffectAmount", "EffectBoost", "EffectPeak", "EffectNoise", "EffectHue", "EffectHueSweep", "EffectSaturation", "EffectQuality", "EffectAspect"})
            finite = finite && std::isfinite(val(*rs, n)) && val(*rs, n) > -1e98;
        check(finite, "Rays: the settings are numbers");
        // percent scales: what VEGAS shows is 0..100 (the XML holds fractions)
        check(val(*rs, "EffectThreshold") >= 0 && val(*rs, "EffectThreshold") <= 100 && val(*rs, "EffectAmount") >= 0 && val(*rs, "EffectAmount") <= 100 &&
                  val(*rs, "EffectBoost") >= 0 && val(*rs, "EffectBoost") <= 100 && val(*rs, "EffectSaturation") >= 0 && val(*rs, "EffectSaturation") <= 100,
              "Rays: Threshold, Amount, Boost and Saturation are percentages");
        check(val(*rs, "EffectHue") >= 0 && val(*rs, "EffectHue") <= 360 && val(*rs, "EffectHueSweep") >= 0 && val(*rs, "EffectHueSweep") <= 360, "Rays: Hue and Hue sweep are degrees");
        check(val(*rs, "EffectQuality") > 0 && val(*rs, "EffectQuality") <= 1 && val(*rs, "EffectAspect") > 0, "Rays: Quality and Aspect");
        check(val(*rs, "MaskMaskMode") == 0 || val(*rs, "MaskMaskMode") == 1 || val(*rs, "MaskMaskMode") == 2 || val(*rs, "MaskMaskMode") == 3, "Rays: the mask mode is one of the modes");
    }
    // a Glint of the sample with Hue keyframes: its keys come out in order
    {
        FxSlot* gk = nullptr;
        for (int n = 0; n < 99; ++n) {
            FxSlot* g = firstOf(p, "com.vegascreativesoftware:glintvelvetmatter", n);
            if (!g) break;
            if (g->state.get("params").get("EffectHue").has("k")) { gk = g; break; }
        }
        if (gk) {
            const Json& k = gk->state.get("params").get("EffectHue").get("k");
            bool ordered = k.size() >= 2;
            for (size_t i = 1; i < k.size(); ++i) ordered = ordered && k.at(i).at(0).asDouble(-1) > k.at(i - 1).at(0).asDouble(-1);
            check(ordered, strfmt("a Glint's Hue keys come out in order (%zu keys)", k.size()));
        }
    }

    // ---------------------------------------------------------------- 3. edits
    const auto origXml = xmlsOf(bytes);
    size_t raysAt = 0;           // the XML text of the Rays
    for (size_t i = 0; i < origXml.size(); ++i)
        if (origXml[i].find("<Rays") != std::string::npos) { raysAt = i; break; }
    const double amount0 = val(*rs, "EffectAmount");
    {   // a value
        Project q = p;
        FxSlot* r = firstOf(q, "com.vegascreativesoftware:raysvelvetmatter");
        Json st = r->state;
        Json e = st.get("params").get("EffectThreshold");
        Json v = Json::array();
        v.push(50.0);
        e["v"] = v;
        st["params"]["EffectThreshold"] = e;
        r->state = st;
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        check(warn.empty(), "an edit of a Rays value warns nothing");
        const auto outXml = xmlsOf(out);
        check(outXml.size() == origXml.size(), "the same number of XML texts");
        size_t changed = 0;
        for (size_t i = 0; i < outXml.size() && i < origXml.size(); ++i) {
            if (outXml[i] == origXml[i]) continue;
            ++changed;
            const std::string d = lineDiff(origXml[i], outXml[i]);
            check(d.find("<Threshold>0.5</Threshold>") != std::string::npos && d.find('[') == d.rfind('['), "only Threshold changed: " + d);
        }
        check(changed >= 1, strfmt("the XML of the edited effect changed (%zu texts)", changed));
        Project back;
        check(importVegasData(out, dfs, back).ok, "the edited file opens");
        FxSlot* rb = firstOf(back, "com.vegascreativesoftware:raysvelvetmatter");
        check(rb && near(val(*rb, "EffectThreshold"), 50) && near(val(*rb, "EffectAmount"), amount0, 0.005), "Threshold 50 reads back, Amount the same");
    }
    {   // bypass and mask mode
        Project q = p;
        FxSlot* r = firstOf(q, "com.vegascreativesoftware:raysvelvetmatter");
        r->bypass = true;
        Json st = r->state;
        Json e = st.get("params").get("MaskMaskMode");
        Json v = Json::array();
        v.push(1);
        e["v"] = v;
        st["params"]["MaskMaskMode"] = e;
        r->state = st;
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        Project back;
        check(importVegasData(out, dfs, back).ok, "bypassed file opens");
        FxSlot* rb = firstOf(back, "com.vegascreativesoftware:raysvelvetmatter");
        check(rb && rb->bypass && near(val(*rb, "MaskMaskMode"), 1), "bypass and mask mode read back");
        const auto outXml = xmlsOf(out);
        check(outXml.size() == origXml.size() && outXml[raysAt].find("<Enabled>false</Enabled>") != std::string::npos &&
                  outXml[raysAt].find("<Mode>Effect</Mode>") != std::string::npos,
              "Enabled false and Mode Effect are in the XML");
        // a mask of another type is not written (its object is not known)
        Json e2 = r->state.get("params").get("MaskMaskType");
        Json v2 = Json::array();
        v2.push(3);
        e2["v"] = v2;
        Json st2 = r->state;
        st2["params"]["MaskMaskType"] = e2;
        r->state = st2;
        std::vector<std::string> warn2;
        const std::string out2 = writeVegasData(q, dfsHdr, &warn2);
        check(!warn2.empty(), "a changed mask type is warned about");
        check(xmlsOf(out2)[raysAt].find("<Type>3</Type>") == std::string::npos, "and the type stays what it was");
    }
    if (!glintPath.empty()) {   // keyframes (of the first sample with a keyed Glint): move one, add one, then remove them all
        Project q;
        importVegasProject(glintPath, q);
        const std::string gbytes = readFile(glintPath);
        const std::string gHdr = headerPath(gbytes);
        FxSlot* g = nullptr;
        for (int n = 0; n < 99; ++n) {
            FxSlot* c = firstOf(q, "com.vegascreativesoftware:glintvelvetmatter", n);
            if (c && c->state.get("params").get("EffectHue").has("k")) g = c;
        }
        const double fps = q.settings.fps();
        Json st = g->state;
        const size_t n0 = g->state.get("params").get("EffectHue").get("k").size();
        const double lastTime = g->state.get("params").get("EffectHue").get("k").at(n0 - 1).at(0).asDouble();
        Json e = st.get("params").get("EffectHue");
        Json ks = Json::array();
        for (size_t i = 0; i < e.get("k").size(); ++i) {
            Json key = e.get("k").at(i);
            if (i == 1) {
                Json k2 = Json::array();
                k2.push(key.at(0).asDouble());
                k2.push(180.0);
                key = k2;
            }
            ks.push(key);
        }
        Json extra = Json::array();
        extra.push(0.05 * fps);
        extra.push(100.0);
        ks.push(extra);
        e["k"] = ks;
        st["params"]["EffectHue"] = e;
        g->state = st;
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, gHdr, &warn);
        Project back;
        check(importVegasData(out, glintPath, back).ok, "keyframed file opens");
        FxSlot* gb = nullptr;
        for (int n = 0; n < 99; ++n) {
            FxSlot* c = firstOf(back, "com.vegascreativesoftware:glintvelvetmatter", n);
            if (c && c->state.get("params").get("EffectHue").has("k")) gb = c;
        }
        if (check(gb != nullptr, "the keyed Glint is still there")) {
            const Json& k = gb->state.get("params").get("EffectHue").get("k");
            check(k.size() == n0 + 1, strfmt("one key more (%zu)", k.size()));
            if (k.size() == n0 + 1 && n0 >= 3) {
                check(near(k.at(0).at(0).asDouble(), 0) && near(k.at(1).at(0).asDouble(), 0.05 * fps, 1e-4), "the new key is second, at 0.05 s");
                check(near(k.at(1).at(1).asDouble(), 100) && near(k.at(2).at(1).asDouble(), 180), "its value and the changed one");
                check(near(k.at(n0).at(0).asDouble(), lastTime, 1e-6), "the last key kept its time exactly");
            }
        }
        // every key repeats the other settings
        const auto outXml = xmlsOf(out);
        int glintsXml = 0;
        for (const auto& x : outXml) glintsXml += x.find("<Glint ") != std::string::npos ? 1 : 0;
        check(glintsXml >= (int)n0 + 1, "the XML has a text per key");
        // no keys at all: one key
        Json st3 = g->state;
        Json e3 = st3.get("params").get("EffectHue");
        Json v3 = Json::array();
        v3.push(120.0);
        e3["v"] = v3;
        Json plain = Json::object();
        plain["v"] = v3;
        st3["params"]["EffectHue"] = plain;
        g->state = st3;
        const std::string out3 = writeVegasData(q, gHdr, &warn);
        Project back3;
        check(importVegasData(out3, glintPath, back3).ok, "unkeyed file opens");
        bool found = false;
        forEachFx(back3, [&](FxSlot& f) {
            if (f.pluginId.find("glint") != std::string::npos && near(val(f, "EffectHue"), 120)) found = !f.state.get("params").get("EffectHue").has("k");
        });
        check(found, "Hue 120 without keys reads back");
        // the parts that are not effects are as they were
        veg::ChunkTree t1, t3;
        veg::readChunkTree(veg::Bytes((const uint8_t*)gbytes.data(), gbytes.size()), t1);
        veg::readChunkTree(veg::Bytes((const uint8_t*)out3.data(), out3.size()), t3);
        check(!t3.damaged, "the written file's chunks are whole");
        check(t1.count == t3.count, strfmt("the same number of chunks (%zu / %zu)", t1.count, t3.count));
    }
    {   // another effect in its place becomes an OpenFX record
        Project q = p;
        FxSlot* r = firstOf(q, "com.vegascreativesoftware:raysvelvetmatter");
        r->pluginId = "com.vegascreativesoftware:glintvelvetmatter";
        r->state = Json();
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        Project back;
        check(importVegasData(out, dfs, back).ok, "swapped file opens");
        int n = 0;
        forEachFx(back, [&](FxSlot& f) { n += f.pluginId.find("raysvelvetmatter") != std::string::npos ? 1 : 0; });
        check(n == 0, "no Rays is left");
        veg::ChunkTree t;
        veg::readChunkTree(veg::Bytes((const uint8_t*)out.data(), out.size()), t);
        check(!t.damaged, "the swapped file is whole");
    }
    {   // an effect added in Reno (no VEGAS record behind it) is written as OpenFX, which VEGAS has
        Project q = p;
        FxSlot copy = *firstOf(q, "com.vegascreativesoftware:raysvelvetmatter");
        copy.veg.reset();
        copy.id = q.newId();
        for (auto& t : q.tracks)
            if (t.type == TrackType::Video) { t.fx.push_back(copy); break; }
        std::vector<std::string> warn;
        const std::string out = writeVegasData(q, dfsHdr, &warn);
        Project back;
        check(importVegasData(out, dfs, back).ok, "file with a new Rays opens");
        int n = 0;
        forEachFx(back, [&](FxSlot& f) { n += f.pluginId.find("raysvelvetmatter") != std::string::npos ? 1 : 0; });
        check(n == 2, strfmt("a new Rays is written next to the old (%d)", n));
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
