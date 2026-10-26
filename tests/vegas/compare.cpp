// compare: checks Reno's VEGAS importer against VEGAS itself.
//
//   compare project.veg project.reno [another.veg another.reno ...] [--examples N]
//
// project.reno is what tools/vegas/Export to Reno.cs wrote inside VEGAS from the same project (VEGAS's own reading of
// its file, with VEGAS's raw values in "vegas" entries). The .veg is imported with Reno's importer and the two are
// compared field by field (how many were compared, how many differ, examples).
//
// A diagnosis follows for the parts of the format the importer only guesses (event and track flag bits, stretch
// method and compositing mode numbers, effect flags, pan/crop rotation sense, take offsets vs. rate): the raw values
// in the .veg are counted against what VEGAS says. Several projects can be given; the tallies then cover all of them.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

using namespace reno;

static int g_examples = 5;

// ------------------------------------------------------------------------------------------ tallies
struct Tally {
    int compared = 0, differ = 0;
    std::vector<std::string> examples;
};
static std::map<std::string, Tally> g_tally;
static std::vector<std::string> g_order;

static Tally& tally(const std::string& field)
{
    auto it = g_tally.find(field);
    if (it == g_tally.end()) { g_order.push_back(field); return g_tally[field]; }
    return it->second;
}

static void same(const std::string& field, bool ok, const std::string& example)
{
    Tally& t = tally(field);
    ++t.compared;
    if (!ok) {
        ++t.differ;
        if ((int)t.examples.size() < g_examples) t.examples.push_back(example);
    }
}

static bool close(double a, double b, double tol) { return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }

static void num(const std::string& field, const std::string& where, double importer, double vegas, double tol)
{
    same(field, close(importer, vegas, tol), strfmt("%s: importer %.6g, VEGAS %.6g", where.c_str(), importer, vegas));
}

static void flag(const std::string& field, const std::string& where, bool importer, bool vegas)
{
    same(field, importer == vegas, strfmt("%s: importer %s, VEGAS %s", where.c_str(), importer ? "yes" : "no", vegas ? "yes" : "no"));
}

static void text(const std::string& field, const std::string& where, const std::string& importer, const std::string& vegas)
{
    same(field, importer == vegas, where + ": importer '" + importer + "', VEGAS '" + vegas + "'");
}

// Cross-tables for the diagnosis: what VEGAS says (rows) against a raw value from the file (columns).
static std::map<std::string, std::map<std::string, std::map<std::string, int>>> g_cross;
static std::vector<std::string> g_crossOrder;

static void cross(const std::string& table, const std::string& vegas, const std::string& raw)
{
    if (!g_cross.count(table)) g_crossOrder.push_back(table);
    ++g_cross[table][vegas][raw];
}

static std::string lowerPath(std::string s)
{
    s = toLower(s);
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
}

// ------------------------------------------------------------------------------------------ raw .veg data
// What the file holds for each track and event, read straight from the chunks (not through the importer),
// for the diagnosis.
struct RawEvent {
    double start = 0, length = 0;
    uint8_t f0 = 0, f1 = 0;
    uint32_t sub04 = 0, method = 0, audio04 = 0, audio08 = 0, audio0c = 0, audio24 = 0, fadeInCurve = 0, fadeOutCurve = 0, videoFlags = 0;
    double takeOffsetRaw = 0, rate = 1, pitch = 0;
    std::vector<uint32_t> fxFlags;
    std::vector<double> panRot;
    std::vector<uint32_t> panType;
};
struct RawTrack {
    uint32_t flags = 0, kind = 0, composite = 0, bus = 0;
    std::string compositor;                 // the GUID at 0x6c of the video settings
    std::vector<double> motionKey;          // the first track motion key's numbers (every 4 bytes, as doubles)
    std::vector<RawEvent> events;
};
// Project settings candidates: the u32 words of the first video settings block of the header, and the header's
// own first words.
struct RawProject {
    std::vector<uint32_t> header, video;
};

static std::vector<RawTrack> readRaw(const std::string& raw, RawProject& rp)
{
    using namespace veg;
    std::vector<RawTrack> out;
    ChunkTree tree;
    readChunkTree(Bytes((const uint8_t*)raw.data(), raw.size()), tree);
    const Chunk* tracks = nullptr;
    for (const auto& k : tree.root.kids) if (k.isList && k.type == G::Tracks) tracks = &k;
    for (const auto& k : tree.root.kids) {
        if (k.isList || k.id != G::Header) continue;
        const Bytes& b = k.data;
        for (size_t i = 0; i < 0x90; i += 4) rp.header.push_back(b.u32(i));
        size_t at = b.u32(0);
        for (int i = 0; i < 4 && at < b.size(); ++i) b.textZ(at, &at);
        for (size_t i = 0; i < 0x358 && b.has(at + i, 4); i += 4) rp.video.push_back(b.u32(at + i));
    }
    if (!tracks) return out;
    for (const auto& t : tracks->kids) {
        if (!t.isList || t.type != G::Track) continue;
        RawTrack rt;
        for (const auto& k : t.kids) {
            if (!k.isList && k.id == G::TrackHeader) { rt.flags = k.data.u32(4); rt.kind = k.data.u32(0x0c); }
            if (k.isList && k.type == G::VideoTrackList)
                for (const auto& d : k.kids)
                    if (!d.isList) {
                        rt.composite = d.data.u32(0x64);
                        rt.compositor = d.data.guid(0x6c);
                    }
            if (k.isList && k.type == G::TrackMotionList)
                for (const auto& d : k.kids)
                    if (!d.isList && rt.motionKey.empty()) {
                        // the first part's first key: after the header (u32 0x24 bytes), u32 keys, u32 key bytes
                        const size_t key = d.data.u32(0) + 8;
                        for (size_t i = key; i + 8 <= d.data.size() && i < key + 0xd8; i += 4) rt.motionKey.push_back(d.data.f64(i));
                    }
            if (k.isList && k.type == G::AudioTrackList)
                for (const auto& d : k.kids) if (!d.isList) rt.bus = d.data.u32(0x08);
            if (!(k.isList && k.type == G::Events)) continue;
            for (const auto& ev : k.kids) {
                if (!ev.isList || ev.type != G::Event) continue;
                RawEvent re;
                for (const auto& c : ev.kids) {
                    if (!c.isList && c.id == G::EventData) {
                        const Bytes& d = c.data;
                        re.start = (double)d.i64(0x10) / 1e7;
                        re.length = (double)d.i64(0x18) / 1e7;
                        re.f0 = d.u8(4);
                        re.f1 = d.u8(5);
                        re.rate = d.f64(0x28);
                        const size_t s = (size_t)d.u32(0) + d.u32(0x24);
                        re.sub04 = d.u32(s + 4);
                        re.fadeInCurve = d.u32(s + 0x14);
                        re.fadeOutCurve = d.u32(s + 0x1c);
                    }
                    if (!c.isList && c.id == G::Take && re.takeOffsetRaw == 0) re.takeOffsetRaw = (double)c.data.i64(0x10) / 1e7;
                    if (c.isList && c.type == G::VideoEventList)
                        for (const auto& v : c.kids)
                            if (!v.isList) re.videoFlags = v.data.u32(4);
                    if (c.isList && c.type == G::AudioEventList)
                        for (const auto& a : c.kids)
                            if (!a.isList) {
                                re.audio04 = a.data.u32(4);
                                re.audio08 = a.data.u32(8);
                                re.audio0c = a.data.u32(0x0c);
                                re.method = a.data.u32(0x10);
                                re.pitch = a.data.f64(0x14);
                                re.audio24 = a.data.u32(0x24);
                            }
                    if (c.isList && c.type == G::EventFx)
                        for (const auto& f : c.kids)
                            if (!f.isList) re.fxFlags.push_back(f.data.u32(0) == 0x30 ? f.data.u32(0x20) : 0xffffffffu);
                    if (!c.isList && c.id == G::PanCrop) {
                        PanCropRecord pc;
                        if (readPanCrop(c.data, pc))
                            for (const auto& key : pc.keys) {
                                re.panRot.push_back(key.rot);
                                re.panType.push_back(key.type);
                            }
                    }
                }
                rt.events.push_back(re);
            }
        }
        out.push_back(rt);
    }
    return out;
}

// ------------------------------------------------------------------------------------------ comparing
static void compareParams(const std::string& where, const Json& a, const Json& b)
{
    const Json& pa = a.get("params");
    const Json& pb = b.get("params");
    for (const auto& m : pb.members()) {
        const std::string w = where + " param " + m.first;
        if (!pa.has(m.first)) {
            same("effect param present", false, w + ": VEGAS has it, the importer does not");
            continue;
        }
        same("effect param present", true, "");
        const Json& x = pa.get(m.first);
        const Json& y = m.second;
        if (y.has("s")) {
            text("effect param text", w, x.get("s").asString(), y.get("s").asString());
            continue;
        }
        const Json& vx = x.get("v");
        const Json& vy = y.get("v");
        bool ok = vx.size() == vy.size();
        for (size_t i = 0; ok && i < vy.size(); ++i) ok = close(vx.at(i).asDouble(), vy.at(i).asDouble(), 1e-4);
        std::string sx, sy;
        for (size_t i = 0; i < vx.size(); ++i) sx += strfmt(i ? ",%g" : "%g", vx.at(i).asDouble());
        for (size_t i = 0; i < vy.size(); ++i) sy += strfmt(i ? ",%g" : "%g", vy.at(i).asDouble());
        same("effect param value", ok, w + ": importer " + sx + ", VEGAS " + sy);
        const Json& kx = x.get("k");
        const Json& ky = y.get("k");
        if (kx.size() || ky.size()) {
            same("effect param keyframe count", kx.size() == ky.size(), strfmt("%s: importer %zu keys, VEGAS %zu", w.c_str(), kx.size(), ky.size()));
            for (size_t i = 0; i < kx.size() && i < ky.size(); ++i) {
                num("effect param keyframe time (frames)", w + strfmt(" key %zu", i), kx.at(i).at(0).asDouble(), ky.at(i).at(0).asDouble(), 1e-3);
                bool kv = kx.at(i).size() == ky.at(i).size();
                for (size_t d = 1; kv && d < ky.at(i).size(); ++d) kv = close(kx.at(i).at(d).asDouble(), ky.at(i).at(d).asDouble(), 1e-4);
                same("effect param keyframe value", kv, w + strfmt(" key %zu", i));
            }
        }
    }
}

static void compareChain(const std::string& where, const Json& a, const Json& b, const std::vector<uint32_t>* rawFlags)
{
    same("effect count", a.size() == b.size(), strfmt("%s: importer %zu effects, VEGAS %zu", where.c_str(), a.size(), b.size()));
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const Json& x = a.at(i);
        const Json& y = b.at(i);
        const std::string w = where + strfmt(" effect %zu", i + 1);
        text("effect kind", w, x.get("kind").asString(), y.get("kind").asString());
        if (y.get("kind").asString() == "ofx") {
            text("effect plug-in", w, x.get("plugin").asString(), y.get("plugin").asString());
            if (x.get("plugin").asString() == y.get("plugin").asString()) compareParams(w + " (" + y.get("plugin").asString() + ")", x.get("state"), y.get("state"));
        } else {
            text("audio effect name", w, x.get("audio").get("name").asString(), y.get("audio").get("name").asString());
            text("audio effect format", w, x.get("audio").get("format").asString(), y.get("audio").get("format").asString());
        }
        flag("effect bypass", w, x.get("bypass").asBool(), y.get("bypass").asBool());
        if (rawFlags && i < rawFlags->size()) {
            const uint32_t f = (*rawFlags)[i];
            cross("Effect bypass (VEGAS) against the effect record's flags at 0x20", y.get("bypass").asBool() ? "bypassed" : "on", strfmt("0x%x", f));
            cross("Effect 'apply before pan/crop' (VEGAS) against the flags at 0x20",
                  y.get("vegas").get("applyBeforePanCrop").asBool() ? "before" : "after", strfmt("0x%x", f));
        }
    }
}

// One project: its .veg through the importer against VEGAS's own export of it. Adds to the tallies; false if
// either file could not be read.
static std::vector<std::string> g_notes;

static bool comparePair(const std::string& vegPath, const std::string& renoPath)
{
    Project imp;
    const VegasImportResult r = importVegasProject(vegPath, imp);
    if (!r.ok) {
        std::printf("the importer could not open %s: %s\n", vegPath.c_str(), r.error.c_str());
        return false;
    }
    std::string renoText, raw;
    Json B;
    std::string err;
    if (!readFile(renoPath, renoText) || !Json::parse(renoText, B, &err)) {
        std::printf("%s could not be read: %s\n", renoPath.c_str(), err.c_str());
        return false;
    }
    readFile(vegPath, raw);
    RawProject rawProject;
    const std::vector<RawTrack> rawTracks = readRaw(raw, rawProject);
    const Json A = imp.toJson();

    // ---------------------------------------------------------------- project
    const Json& sa = A.get("settings");
    const Json& sb = B.get("settings");
    for (const char* k : {"width", "height", "sampleRate", "pixelAspect", "tempo", "beatsPerMeasure"})
        num(std::string("project ") + k, "project", sa.get(k).asDouble(), sb.get(k).asDouble(), 1e-6);
    num("project frame rate", "project", sa.get("fpsNum").asDouble() / std::max(1.0, sa.get("fpsDen").asDouble()),
        sb.get("fpsNum").asDouble() / std::max(1.0, sb.get("fpsDen").asDouble()), 1e-6);
    for (const char* k : {"fieldOrder", "pixelFormat", "renderQuality", "deinterlace", "title", "artist", "engineer", "copyright", "comments"})
        text(std::string("project ") + k, "project", sa.get(k).asString(), sb.get(k).asString());
    if (B.get("vegas").get("buses").get("masterLevelKnown").asBool(true))
        num("master volume", "project", A.get("masterVolume").asDouble(), B.get("masterVolume").asDouble(), 1e-4);
    // where the .veg keeps the settings the importer leaves at Reno's defaults: each of VEGAS's values against
    // the raw words that could hold it (a word that changes with the value is the one)
    {
        const Json& vv = sb.get("vegas");
        for (const char* k : {"renderQuality", "fieldOrder", "pixelFormat", "motionBlur", "deinterlace", "rulerFormat"}) {
            for (size_t i = 0; i < rawProject.video.size(); ++i)
                cross(strfmt("Project %s (VEGAS) against video settings word 0x%03zx", k, i * 4), vv.get(k).asString(), strfmt("%u", rawProject.video[i]));
            for (size_t i = 0; i < rawProject.header.size(); ++i)
                cross(strfmt("Project %s (VEGAS) against header word 0x%02zx", k, i * 4), vv.get(k).asString(), strfmt("%u", rawProject.header[i]));
        }
    }
    compareChain("master bus", A.get("audioOut"), B.get("audioOut"), nullptr);
    compareChain("video output", A.get("videoOut"), B.get("videoOut"), nullptr);

    // ---------------------------------------------------------------- media (matched by path)
    std::map<uint64_t, std::string> keyA, keyB;     // media id -> its path (or "generated #n")
    auto mediaKeys = [](const Json& proj, std::map<uint64_t, std::string>& out, std::map<std::string, const Json*>& byKey) {
        int gen = 0;
        for (const auto& m : proj.get("media").items()) {
            const std::string k = m.get("generated").asBool() ? strfmt("generated #%d %s", ++gen, m.get("generator").get("plugin").asString().c_str())
                                                              : lowerPath(m.get("path").asString());
            out[m.get("id").asUInt64()] = k;
            byKey[k] = &m;
        }
    };
    std::map<std::string, const Json*> mediaA, mediaB;
    mediaKeys(A, keyA, mediaA);
    mediaKeys(B, keyB, mediaB);
    for (const auto& mb : mediaB) {
        auto ma = mediaA.find(mb.first);
        // the importer may have found a moved file somewhere else: match on the file name then
        if (ma == mediaA.end())
            for (auto it = mediaA.begin(); it != mediaA.end(); ++it)
                if (pathFileName(it->first) == pathFileName(mb.first)) { ma = it; break; }
        same("media present", ma != mediaA.end(), "VEGAS has " + mb.first + ", the importer does not");
        if (ma == mediaA.end()) continue;
        const Json& ia = ma->second->get("info");
        const Json& ib = mb.second->get("info");
        const std::string w = "media " + pathFileName(mb.first);
        for (const char* k : {"video", "audio", "still"}) flag(std::string("media ") + k, w, ia.get(k).asBool(), ib.get(k).asBool());
        if (ia.get("width").asInt() && ib.get("width").asInt()) {
            num("media width", w, ia.get("width").asDouble(), ib.get("width").asDouble(), 0);
            num("media height", w, ia.get("height").asDouble(), ib.get("height").asDouble(), 0);
        }
        if (ia.get("duration").asDouble() > 0 && ib.get("duration").asDouble() > 0)
            same("media duration (the importer's is a lower bound from subclips)", ia.get("duration").asDouble() <= ib.get("duration").asDouble() + 0.01,
                 strfmt("%s: importer %.3f, VEGAS %.3f", w.c_str(), ia.get("duration").asDouble(), ib.get("duration").asDouble()));
        if (mb.second->get("generated").asBool()) {
            const Json& ga = ma->second->get("generator");
            const Json& gb = mb.second->get("generator");
            text("generator plug-in", w, ga.get("plugin").asString(), gb.get("plugin").asString());
            compareParams(w + " generator", ga.get("state"), gb.get("state"));
        }
    }
    same("media count", mediaA.size() == mediaB.size(), strfmt("importer %zu media, VEGAS %zu", mediaA.size(), mediaB.size()));

    // ---------------------------------------------------------------- buses (matched by name)
    std::map<uint64_t, std::string> busA, busB;
    for (const auto& b : A.get("buses").items()) busA[b.get("id").asUInt64()] = b.get("name").asString();
    for (const auto& b : B.get("buses").items()) busB[b.get("id").asUInt64()] = b.get("name").asString();
    for (const auto& b : B.get("buses").items()) {
        const Json* match = nullptr;
        for (const auto& a : A.get("buses").items()) if (a.get("name").asString() == b.get("name").asString()) match = &a;
        same("bus present", match != nullptr, "VEGAS has bus " + b.get("name").asString());
        if (!match) continue;
        if (b.get("vegas").get("levelKnown").asBool(true))
            num("bus level", "bus " + b.get("name").asString(), match->get("level").asDouble(), b.get("level").asDouble(), 1e-4);
        flag("bus mute", "bus " + b.get("name").asString(), match->get("mute").asBool(), b.get("mute").asBool());
        flag("bus solo", "bus " + b.get("name").asString(), match->get("solo").asBool(), b.get("solo").asBool());
        compareChain("bus " + b.get("name").asString(), match->get("fx"), b.get("fx"), nullptr);
    }

    // ---------------------------------------------------------------- markers
    const Json& ma = A.get("markers");
    const Json& mb = B.get("markers");
    same("marker count", ma.size() == mb.size(), strfmt("importer %zu markers and regions, VEGAS %zu", ma.size(), mb.size()));
    for (const auto& m : mb.items()) {
        const Json* match = nullptr;
        for (const auto& x : ma.items())
            if (std::fabs(x.get("time").asDouble() - m.get("time").asDouble()) < 1e-4 && x.get("name").asString() == m.get("name").asString()) match = &x;
        const std::string w = strfmt("%s at %.3f '%s'", m.get("region").asBool() ? "region" : "marker", m.get("time").asDouble(), m.get("name").asString().c_str());
        same("marker found (time and name)", match != nullptr, w);
        if (match) {
            num("marker length", w, match->get("length").asDouble(), m.get("length").asDouble(), 1e-4);
            flag("marker is a region", w, match->get("region").asBool(), m.get("region").asBool());
        }
    }

    // ---------------------------------------------------------------- tracks and events
    const Json& ta = A.get("tracks");
    const Json& tb = B.get("tracks");
    same("track count", ta.size() == tb.size(), strfmt("importer %zu tracks, VEGAS %zu", ta.size(), tb.size()));
    std::map<uint64_t, uint64_t> groupMapAB;     // importer group -> VEGAS group, to compare the partitions
    for (size_t ti = 0; ti < ta.size() && ti < tb.size(); ++ti) {
        const Json& x = ta.at(ti);
        const Json& y = tb.at(ti);
        const std::string w = strfmt("track %zu", ti + 1);
        const RawTrack* rt = ti < rawTracks.size() ? &rawTracks[ti] : nullptr;
        text("track type", w, x.get("type").asString(), y.get("type").asString());
        text("track name", w, x.get("name").asString(), y.get("name").asString());
        flag("track mute", w, x.get("mute").asBool(), y.get("mute").asBool());
        flag("track solo", w, x.get("solo").asBool(), y.get("solo").asBool());
        num("track level", w, x.get("level").asDouble(), y.get("level").asDouble(), 1e-4);
        num("track pan", w, x.get("pan").asDouble(), y.get("pan").asDouble(), 1e-4);
        text("track composite mode", w, x.get("composite").asString(), y.get("composite").asString());
        same("track motion keys", x.get("motion").size() == y.get("motion").size(),
             strfmt("%s: importer %zu track motion keys, VEGAS %zu", w.c_str(), x.get("motion").size(), y.get("motion").size()));
        const std::string ob = y.get("output").asUInt64() ? busB[y.get("output").asUInt64()] : "master";
        const std::string oa = x.get("output").asUInt64() ? busA[x.get("output").asUInt64()] : "master";
        text("track output", w, oa, ob);
        if (rt) {
            for (int bit = 0; bit < 32; ++bit) {
                const std::string col = (rt->flags >> bit) & 1 ? "set" : "clear";
                cross(strfmt("Track mute (VEGAS) against track header flags bit 0x%08x", 1u << bit), y.get("mute").asBool() ? "muted" : "not muted", col);
                cross(strfmt("Track solo (VEGAS) against track header flags bit 0x%08x", 1u << bit), y.get("solo").asBool() ? "solo" : "not solo", col);
            }
            if (y.get("type").asString() == "video") {
                cross("Track compositing mode (VEGAS) against the u32 at 0x64 and the GUID at 0x6c of the video track settings",
                      y.get("vegas").get("compositeMode").asString() + " " + y.get("vegas").get("compositeEffectClassId").asString(),
                      strfmt("%d", (int32_t)rt->composite) + " {" + rt->compositor + "}");
                // track motion: which raw number of the first key holds each of VEGAS's values (0 and 1 are
                // left out: they are everywhere)
                const Json& tm = y.get("vegas").get("trackMotionRaw");
                if (tm.size() > 0 && !rt->motionKey.empty()) {
                    const char* names[] = {"time", "PositionX", "PositionY", "Width", "Height", "RotationZ", "Smoothness"};
                    for (int f = 1; f < 7; ++f) {
                        const double v = tm.at(0).at((size_t)f).asDouble();
                        if (std::fabs(v) < 1e-9 || std::fabs(v - 1) < 1e-9) continue;
                        std::string at;
                        for (size_t i = 0; i < rt->motionKey.size(); ++i)
                            if (std::fabs(rt->motionKey[i] - v) < 1e-6 * std::max(1.0, std::fabs(v))) at += strfmt(" +0x%zx", i * 4);
                        cross("Track motion: where the first key's values are in the raw key (byte offsets of doubles)", names[f], at.empty() ? "not found" : at);
                    }
                }
            }
            else
                cross("Track output bus (VEGAS) against the u32 at 0x08 of the audio track settings", ob, strfmt("%u", rt->bus));
        }
        compareChain(w, x.get("fx"), y.get("fx"), nullptr);

        // events: matched on start and length (both sides sorted by start)
        const Json& ea = x.get("events");
        const Json& eb = y.get("events");
        same("event count", ea.size() == eb.size(), strfmt("%s: importer %zu events, VEGAS %zu", w.c_str(), ea.size(), eb.size()));
        std::vector<bool> used(ea.size(), false);
        for (const auto& e2 : eb.items()) {
            const double s2 = e2.get("start").asDouble(), l2 = e2.get("length").asDouble();
            const std::string we = w + strfmt(", event at %.4f s", s2);
            const Json* e1 = nullptr;
            for (size_t i = 0; i < ea.size(); ++i)
                if (!used[i] && std::fabs(ea.at(i).get("start").asDouble() - s2) < 1e-5 && std::fabs(ea.at(i).get("length").asDouble() - l2) < 1e-5) {
                    used[i] = true;
                    e1 = &ea.at(i);
                    break;
                }
            same("event found (start and length)", e1 != nullptr, we + ": VEGAS has it, the importer has none there");
            if (!e1) continue;
            const Json& v = e2.get("vegas");
            same("event media", keyA[e1->get("media").asUInt64()] == keyB[e2.get("media").asUInt64()] ||
                                    pathFileName(keyA[e1->get("media").asUInt64()]) == pathFileName(keyB[e2.get("media").asUInt64()]),
                 we + ": importer " + keyA[e1->get("media").asUInt64()] + ", VEGAS " + keyB[e2.get("media").asUInt64()]);
            num("event offset", we, e1->get("offset").asDouble(), e2.get("offset").asDouble(), 1e-5);
            num("event rate", we, e1->get("rate").asDouble(), e2.get("rate").asDouble(), 1e-6);
            num("event fade in", we, e1->get("fadeIn").asDouble(), e2.get("fadeIn").asDouble(), 1e-5);
            num("event fade out", we, e1->get("fadeOut").asDouble(), e2.get("fadeOut").asDouble(), 1e-5);
            num("event gain", we, e1->get("gain").asDouble(), e2.get("gain").asDouble(), 1e-4);
            flag("event mute", we, e1->get("mute").asBool(), e2.get("mute").asBool());
            flag("event loop", we, e1->get("loop").asBool(), e2.get("loop").asBool());
            flag("event reverse", we, e1->get("reverse").asBool(), e2.get("reverse").asBool());
            flag("event keep pitch", we, e1->get("keepPitch").asBool(), e2.get("keepPitch").asBool());
            num("event pitch", we, e1->get("pitch").asDouble(), e2.get("pitch").asDouble(), 1e-6);
            flag("event keep aspect", we, e1->get("keepAspect").asBool(), e2.get("keepAspect").asBool());
            text("event name", we, e1->get("name").asString(), e2.get("name").asString());
            // groups: the same events must be together on both sides
            const uint64_t g1 = e1->get("group").asUInt64(), g2 = e2.get("group").asUInt64();
            if (g1 || g2) {
                bool ok = (g1 != 0) == (g2 != 0);
                if (ok && g1) {
                    auto it = groupMapAB.find(g1);
                    if (it == groupMapAB.end()) groupMapAB[g1] = g2;
                    else ok = it->second == g2;
                }
                same("event grouping", ok, we);
            }
            // pan/crop
            const Json& pa = e1->get("panCrop");
            const Json& pb = e2.get("panCrop");
            same("event pan/crop key count", pa.size() == pb.size(), strfmt("%s: importer %zu keys, VEGAS %zu", we.c_str(), pa.size(), pb.size()));
            for (size_t k = 0; k < pa.size() && k < pb.size(); ++k) {
                const char* names[] = {"time", "centre x", "centre y", "width", "height", "rotation", "curve"};
                for (int f = 0; f < 7; ++f)
                    num(std::string("event pan/crop ") + names[f], we + strfmt(" key %zu", k), pa.at(k).at(f).asDouble(), pb.at(k).at(f).asDouble(), f == 5 ? 1e-3 : 1e-4);
                const double ra = pa.at(k).at(5).asDouble(), rb = pb.at(k).at(5).asDouble();
                if (std::fabs(rb) > 1e-3)
                    cross("Pan/crop rotation: the importer's sign against VEGAS's (from the box's corners)", "VEGAS turns the box",
                          std::fabs(ra - rb) < 1e-2 ? "same" : (std::fabs(ra + rb) < 1e-2 ? "opposite" : "other"));
            }
            // effects
            const RawEvent* re = nullptr;
            if (rt)
                for (const auto& c : rt->events)
                    if (std::fabs(c.start - s2) < 1e-5 && std::fabs(c.length - l2) < 1e-5) re = &c;
            compareChain(we, e1->get("fx"), e2.get("fx"), re ? &re->fxFlags : nullptr);

            // diagnosis of the raw fields
            if (re) {
                for (int bit = 0; bit < 8; ++bit) {
                    const std::string c0 = (re->f0 >> bit) & 1 ? "set" : "clear", c1 = (re->f1 >> bit) & 1 ? "set" : "clear";
                    cross(strfmt("Event mute (VEGAS) against event flags byte 4 bit 0x%02x", 1 << bit), v.get("mute").asBool() ? "muted" : "not muted", c0);
                    cross(strfmt("Event locked (VEGAS) against event flags byte 4 bit 0x%02x", 1 << bit), v.get("locked").asBool() ? "locked" : "not locked", c0);
                    cross(strfmt("Event selected (VEGAS) against event flags byte 4 bit 0x%02x", 1 << bit), v.get("selected").asBool() ? "selected" : "not selected", c0);
                    cross(strfmt("Event loop (VEGAS) against event flags byte 5 bit 0x%02x", 1 << bit), v.get("loop").asBool() ? "loop" : "no loop", c1);
                }
                cross("Event loop (VEGAS) against the u32 at +4 after the event's name", v.get("loop").asBool() ? "loop" : "no loop", strfmt("%u", re->sub04));
                if (v.has("maintainAspectRatio"))
                    cross("Event maintain aspect ratio (VEGAS) against the flags at 0x04 of the video event record", v.get("maintainAspectRatio").asBool() ? "keeps" : "stretches",
                          strfmt("0x%x", re->videoFlags));
                if (v.has("resampleMode"))
                    cross("Event resample mode (VEGAS) against the flags at 0x04 of the video event record", v.get("resampleMode").asString(), strfmt("0x%x", re->videoFlags));
                if (v.has("reduceInterlace"))
                    cross("Event reduce interlace (VEGAS) against the flags at 0x04 of the video event record", v.get("reduceInterlace").asBool() ? "on" : "off",
                          strfmt("0x%x", re->videoFlags));
                for (size_t k = 0; k < re->panType.size() && k < v.get("panCropRaw").size(); ++k)
                    cross("Pan/crop key type (VEGAS) against the u32 at +16 of the key", v.get("panCropRaw").at(k).at(12).asString(), strfmt("%u", re->panType[k]));
                cross("Event fade-in curve (VEGAS) against the u32 at +0x14 after the name", v.get("fadeInCurve").asString(), strfmt("%d", (int32_t)re->fadeInCurve));
                cross("Event fade-out curve (VEGAS) against the u32 at +0x1c after the name", v.get("fadeOutCurve").asString(), strfmt("%d", (int32_t)re->fadeOutCurve));
                // take offsets: the file's offset times the rate is the media position, or so the importer
                // assumes; VEGAS's Take.Offset says which
                const double to = v.get("takeOffset").asDouble(), pr = v.get("playbackRate").asDouble();
                cross("Take offset: VEGAS's Take.Offset against the file's take offset", "Take.Offset is",
                      std::fabs(to - re->takeOffsetRaw) < 1e-5 ? "the file's value" : (std::fabs(to - re->takeOffsetRaw * pr) < 1e-5 ? "the file's value x rate" : "something else"));
                if (y.get("type").asString() == "audio") {
                    const std::string m = v.get("method").asString() + (v.get("pitchLock").asBool() ? ", pitch locked to the rate" : "");
                    cross("Audio stretch method (VEGAS) against the u32 at 0x10 of the audio event record", m, strfmt("%u", re->method));
                    cross("Audio pitch lock (VEGAS) against the u32 at 0x24 of the audio event record", v.get("pitchLock").asBool() ? "locked" : "free", strfmt("%u", re->audio24));
                    cross("Audio pitch lock (VEGAS) against the u32 at 0x08 of the audio event record", v.get("pitchLock").asBool() ? "locked" : "free", strfmt("0x%x", re->audio08));
                    cross("Audio elastique setting (VEGAS) against the u32 at 0x0c of the audio event record", v.get("elastique").asString() + " / " + v.get("classic").asString(),
                          strfmt("%u", re->audio0c));
                    cross("Audio elastique / classic setting (VEGAS) against the u32 at 0x08 of the audio event record",
                          v.get("elastique").asString() + " / " + v.get("classic").asString(), strfmt("%u", re->audio08));
                    cross("Audio channels (VEGAS) against the u32 at 0x04 of the audio event record", v.get("channels").asString(), strfmt("%u", re->audio04));
                    cross("Audio invert phase (VEGAS) against the u32 at 0x08 of the audio event record", v.get("invertPhase").asBool() ? "inverted" : "normal",
                          strfmt("0x%x", re->audio08));
                    num("audio pitch semitones as stored", we, re->pitch, v.get("pitchSemis").asDouble(), 1e-6);
                }
            }
        }
        for (size_t i = 0; i < ea.size(); ++i)
            same("event only in the importer", used[i], w + strfmt(", event at %.4f s", ea.at(i).get("start").asDouble()));
    }
    for (const auto& n : r.notes) g_notes.push_back(pathFileName(vegPath) + ": " + n);
    return true;
}

int main(int argc, char** argv)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    std::string pending;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--examples" && i + 1 < argc) { g_examples = std::atoi(argv[++i]); continue; }
        if (pending.empty()) pending = a;
        else { pairs.push_back({pending, a}); pending.clear(); }
    }
    if (pairs.empty() || !pending.empty()) {
        std::fprintf(stderr, "usage: compare project.veg project.reno [another.veg another.reno ...] [--examples N]\n");
        return 2;
    }
    int read = 0;
    std::string names;
    for (const auto& pr : pairs) {
        if (comparePair(pr.first, pr.second)) ++read;
        names += (names.empty() ? "" : ", ") + pathFileName(pr.second);
    }
    if (read == 0) return 1;


    // ---------------------------------------------------------------- report
    std::printf("Reno's VEGAS importer against VEGAS (%d project%s: %s)\n\n", read, read == 1 ? "" : "s", names.c_str());
    std::printf("%-62s %9s %7s\n", "field", "compared", "differ");
    int bad = 0;
    for (const auto& f : g_order) {
        const Tally& t = g_tally[f];
        std::printf("%-62s %9d %7d%s\n", f.c_str(), t.compared, t.differ, t.differ ? "  <--" : "");
        bad += t.differ ? 1 : 0;
    }
    std::printf("\n%d field%s differ%s.\n", bad, bad == 1 ? "" : "s", bad == 1 ? "s" : "");
    for (const auto& f : g_order) {
        const Tally& t = g_tally[f];
        if (!t.differ) continue;
        std::printf("\n%s (%d of %d differ):\n", f.c_str(), t.differ, t.compared);
        for (const auto& e : t.examples) std::printf("  %s\n", e.c_str());
    }
    std::printf("\n---------------- diagnosis: raw values in the .veg against what VEGAS says ----------------\n");
    for (const auto& name : g_crossOrder) {
        const auto& tab = g_cross[name];
        // the flag-bit and raw-word tables are only worth showing where the bit or word tells VEGAS's answers
        // apart (and there are at least two answers to tell apart)
        if (name.find(" bit 0x") != std::string::npos || name.find(" word 0x") != std::string::npos) {
            if (tab.size() < 2) continue;
            bool separates = true;
            std::set<std::string> colsOfRow0;
            for (const auto& c : tab.begin()->second) colsOfRow0.insert(c.first);
            for (auto it = std::next(tab.begin()); it != tab.end(); ++it)
                for (const auto& c : it->second) separates = separates && !colsOfRow0.count(c.first);
            if (!separates) continue;
        }
        std::printf("\n%s:\n", name.c_str());
        for (const auto& row : tab) {
            std::printf("  %-40s", row.first.c_str());
            for (const auto& c : row.second) std::printf("  %s: %d", c.first.c_str(), c.second);
            std::printf("\n");
        }
    }
    std::printf("\n(a flag-bit or raw-word table is shown only when that bit or word alone tells VEGAS's answers apart)\n");
    std::printf("\nimporter notes (%zu):\n", g_notes.size());
    for (const auto& n : g_notes) std::printf("  - %s\n", n.c_str());
    return bad ? 1 : 0;
}
