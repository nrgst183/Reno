// vegroundtrip: checks Reno's .veg writer against the sample VEGAS Pro 18 projects.
//
//   vegroundtrip [folder with .veg files] [--out folder]     (default $RENO_SAMPLES, searched recursively)
//
//  1. Every project is read and written back unchanged: the bytes must be exactly the file's.
//  2. Edits: values of every kind are changed (events, takes, fades, tracks, effects, project settings, markers...),
//     objects are added and deleted (media, tracks, events, effects, envelopes, buses, markers, groups), the result
//     is written and read back, and must be what was saved. Records the writer made are compared with records VEGAS
//     made (same chunk order, sizes, fields).
//  3. A project made in Reno (never a .veg) is written and read back the same way.
//  4. Saving through saveVegasProject: the old file is kept as .bak, and a second save of an unchanged project
//     writes the same bytes as the first.
// With --out, the files the edits made are kept there (for opening in VEGAS).
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/vegorigin.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include "../testdata.h"

using namespace reno;

static int g_fail = 0, g_pass = 0;
static bool g_verbose = false;

static bool check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
    return ok;
}

// ------------------------------------------------------------------------------------------ where bytes differ
// The path of chunks (by GUID) that holds byte 'off' of a file, for saying where two files part.
static std::string chunkPathAt(const veg::Chunk& c, size_t off)
{
    for (const auto& k : c.kids) {
        const size_t start = k.fileOffset;
        const size_t len = (k.isList ? 40 : 24) + k.data.size();
        if (off >= start && off < start + len) {
            const std::string me = (k.isList ? "list " + k.type.substr(0, 8) : k.id.substr(0, 8)) + strfmt("@%zu", start);
            if (k.isList) return me + " / " + chunkPathAt(k, off);
            return me + strfmt(" +0x%zx", off - start - 24);
        }
    }
    return "?";
}

static std::string firstDifference(const std::string& a, const std::string& b)
{
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    if (i == n && a.size() == b.size()) return std::string();
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)a.data(), a.size()), t);
    return strfmt("sizes %zu / %zu, first difference at byte %zu: ", a.size(), b.size(), i) + chunkPathAt(t.root, i);
}

static std::string headerPath(const std::string& bytes)
{
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    for (const auto& k : t.root.kids)
        if (!k.isList && k.id == veg::G::Header) return k.data.textZ(k.data.u32(0));
    return std::string();
}

// ------------------------------------------------------------------------------------------ comparing projects
// What the writer is meant to keep, field by field, as text: a difference prints as a line.
static std::string fmtd(double v) { return strfmt("%.9g", v); }

static std::string fxText(const FxSlot& f)
{
    std::string s = f.kind + " " + (f.kind == "ofx" ? f.pluginId : f.audio.format + ":" + f.audio.uid) + (f.bypass ? " off" : "") +
                    (f.beforePanCrop ? " pre" : "");
    if (f.kind == "ofx") {
        // the parameters, numbers as the record holds them
        for (const auto& m : f.state.get("params").members()) {
            s += " " + m.first + "=";
            const Json& e = m.second;
            if (e.has("s")) s += "'" + e.get("s").asString() + "'";
            for (const auto& k : e.get("ks").items()) s += "{" + fmtd(k.at(0).asDouble()) + ":'" + k.at(1).asString() + "'}";
            for (const auto& v : e.get("v").items()) s += fmtd(v.asDouble()) + ",";
            for (const auto& k : e.get("k").items()) {
                s += "[";
                for (const auto& x : k.items()) s += fmtd(x.asDouble()) + ",";
                s += "]";
            }
        }
    } else {
        s += " state=" + (f.state.isString() ? f.state.asString() : f.state.dump());
    }
    return s;
}

static std::string envText(const Envelope& e)
{
    std::string s = e.kind;
    for (const auto& p : e.points) s += strfmt(" (%.7f %.6g %d)", p.time, p.value, p.curve);
    return s;
}

static void describe(const Project& p, std::vector<std::string>& out)
{
    const ProjectSettings& s = p.settings;
    out.push_back(strfmt("project %dx%d %d/%d %d Hz bits %d par %s tempo %s bpm %d", s.width, s.height, s.fpsNum, s.fpsDen, s.sampleRate, s.audioBits,
                         fmtd(s.pixelAspect).c_str(), fmtd(s.tempo).c_str(), s.beatsPerMeasure));
    out.push_back("info '" + s.title + "' '" + s.artist + "' '" + s.engineer + "' '" + s.copyright + "' '" + s.comments + "'");
    out.push_back("master " + fmtd(p.masterVolume));
    std::map<uint64_t, std::string> mediaName;
    for (const auto& m : p.media) {
        mediaName[m.id] = m.generated ? "gen:" + m.name : m.path;
        out.push_back("media " + (m.generated ? "gen '" + m.name + "' " + fxText(m.generator) : m.path) + strfmt(" v%d a%d", m.info.hasVideo, m.info.hasAudio));
    }
    for (const auto& f : p.audioOut) out.push_back("  master fx " + fxText(f));
    std::map<uint64_t, std::string> busName;
    for (const auto& b : p.buses) {
        busName[b.id] = b.name;
        out.push_back("bus '" + b.name + "' " + fmtd(b.level));
        for (const auto& f : b.fx) out.push_back("  bus fx " + fxText(f));
    }
    std::map<uint64_t, int> groupNo, groupSize;
    for (const auto& t : p.tracks)
        for (const auto& e : t.events)
            if (e.group) ++groupSize[e.group];
    for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& t = p.tracks[ti];
        out.push_back(strfmt("track %zu %s '%s' mute %d solo %d level %s pan %s height %d out '%s' comp %s parent %s fade %08x %08x", ti,
                             t.type == TrackType::Video ? "V" : "A", t.name.c_str(), t.mute, t.solo, fmtd(t.level).c_str(), fmtd(t.pan).c_str(), t.height,
                             t.output ? busName[t.output].c_str() : "", t.composite.c_str(), t.parentComposite.c_str(), t.fadeTop, t.fadeBottom));
        for (const auto& f : t.fx) out.push_back("  track fx " + fxText(f));
        for (const auto& e : t.envelopes) out.push_back("  track env " + envText(e) + (e.kind == "send" ? " to '" + busName[e.target] + "'" : ""));
        for (const auto& e : t.events) {
            int g = 0;
            if (e.group && groupSize[e.group] >= 2) {
                auto it = groupNo.find(e.group);
                if (it == groupNo.end()) it = groupNo.emplace(e.group, (int)groupNo.size() + 1).first;
                g = it->second;
            }
            out.push_back(strfmt("  event %.7f +%.7f off %.7f rate %s gain %.6g fades %.7f/%.7f curves %d/%d mute %d loop %d rev %d pitch %s keep %d "
                                 "ch %s aspect %d resample %s grp %d '%s' media %s",
                                 e.start, e.length, e.offset, fmtd(e.rate).c_str(), e.gain, e.fadeIn, e.fadeOut, e.fadeInCurve, e.fadeOutCurve, e.mute, e.loop,
                                 e.reverse, e.stretchMethod == "none" ? "-" : fmtd(e.pitch).c_str(), e.stretchMethod == "none" ? 0 : e.keepPitch,
                                 e.channels.c_str(), e.keepAspect, e.resample.c_str(), g, e.name.c_str(), mediaName[e.mediaId].c_str()));
            out.push_back(strfmt("    locked %d", e.locked));
            if (t.type == TrackType::Audio)
                out.push_back(strfmt("    stretch %s %s formant %s preserve %d invert %d", e.stretchMethod.c_str(),
                                     e.stretchMethod == "none" ? "-" : e.stretchMode.c_str(), fmtd(e.formant).c_str(), e.preserveFormants, e.invertPhase));
            for (const auto& k : e.takes)
                out.push_back(strfmt("    take %s off %.7f rev %d", mediaName[k.mediaId].c_str(), k.offset, k.reverse));
            for (const auto& f : e.fx) out.push_back("    fx " + fxText(f));
            for (const auto& k : e.panCrop)
                out.push_back(strfmt("    pancrop %.7f %.6f %.6f %.6f %.6f %.5f %d", k.time, k.cx, k.cy, k.w, k.h, k.rotation, k.curve));
            for (const auto& v : e.envelopes) out.push_back("    env " + envText(v));
        }
    }
    for (const auto& m : p.markers) out.push_back(strfmt("marker %.7f +%.7f %d '%s'", m.time, m.length, m.region, m.name.c_str()));
}

// Two lines say the same if their words are the same, numbers within what a .veg keeps (times in 100 ns, levels
// as 32-bit floats).
static bool sameLine(const std::string& a, const std::string& b)
{
    if (a == b) return true;
    auto words = [](const std::string& s) {
        std::vector<std::string> v;
        std::string w;
        for (char c : s) {
            if (c == ' ' || c == '(' || c == ')' || c == ',' || c == '[' || c == ']' || c == '/' || c == '=' || c == '+') {
                if (!w.empty()) v.push_back(w);
                w.clear();
            } else {
                w += c;
            }
        }
        if (!w.empty()) v.push_back(w);
        return v;
    };
    const auto x = words(a), y = words(b);
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i] == y[i]) continue;
        char* e1 = nullptr;
        char* e2 = nullptr;
        const double u = std::strtod(x[i].c_str(), &e1), v = std::strtod(y[i].c_str(), &e2);
        if (*e1 || *e2 || e1 == x[i].c_str() || e2 == y[i].c_str()) return false;
        if (std::fabs(u - v) > 2e-6 * std::max(1.0, std::max(std::fabs(u), std::fabs(v)))) return false;
    }
    return true;
}

// Compares what was saved with what was read back; prints the first few differences.
static bool sameProject(const Project& a, const Project& b, const std::string& what)
{
    std::vector<std::string> x, y;
    describe(a, x);
    describe(b, y);
    bool same = x.size() == y.size();
    if (std::getenv("VEGDUMP") && what.find(std::getenv("VEGDUMP")) != std::string::npos) {
        for (const auto& l : x) std::printf("S %s\n", l.c_str());
        for (const auto& l : y) std::printf("R %s\n", l.c_str());
    }
    int shown = 0;
    for (size_t i = 0; i < std::max(x.size(), y.size()) && shown < 6; ++i) {
        const std::string l = i < x.size() ? x[i] : "(nothing)";
        const std::string r = i < y.size() ? y[i] : "(nothing)";
        if (!sameLine(l, r)) {
            same = false;
            ++shown;
            std::printf("   %s: saved   %s\n   %s: read    %s\n", what.c_str(), l.substr(0, 400).c_str(), what.c_str(), r.substr(0, 400).c_str());
        }
    }
    return check(same, what + ": what was read back is what was saved");
}

// Writes p, reads it back, compares. Returns the bytes.
static std::string saveAndRead(const Project& p, const std::string& path, const std::string& what, Project* back = nullptr,
                               std::vector<std::string>* warnings = nullptr, const std::string& readAs = std::string())
{
    std::vector<std::string> w;
    const std::string bytes = writeVegasData(p, path, &w);
    if (warnings) *warnings = w;
    if (g_verbose)
        for (const auto& l : w) std::printf("   warning: %s\n", l.c_str());
    Project q;
    const VegasImportResult r = importVegasData(bytes, readAs.empty() ? path : readAs, q);
    if (!check(r.ok, what + ": the file written reads back (" + r.error + ")")) return bytes;
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    check(!t.damaged, what + ": the file written is not damaged (" + t.problem + ")");
    sameProject(p, q, what);
    if (back) *back = q;
    return bytes;
}

// ------------------------------------------------------------------------------------------ the samples
static std::vector<std::string> findProjects(const std::string& dir)
{
    std::vector<std::string> v;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file() && endsWithNoCase(it->path().string(), ".veg")) v.push_back(it->path().string());
    }
    std::sort(v.begin(), v.end());
    return v;
}



// ------------------------------------------------------------------------------------------ records compared with VEGAS's
// For each kind of record, the 32-bit words of its fixed part that hold the same value in every record of that kind
// in every sample project. A record the writer made must have them too. (Words that vary in VEGAS's own records
// - times, ids, flags - are filled in by the writer and checked by reading the file back.)
struct KindStats {
    size_t fixed = 0;
    std::vector<std::set<uint32_t>> values;     // per word
    size_t records = 0;
};
static std::map<std::string, KindStats> g_kinds;

static std::string kindOf(const veg::Chunk& c, size_t* fixed)
{
    using namespace veg;
    const Bytes& d = c.data;
    struct K { const char* g; size_t fixed; const char* name; };
    static const K kinds[] = {
        {G::EventData, 0x60, "event"}, {G::Take, 0x20, "take"}, {G::VideoEventData, 0x80, "video event"}, {G::AudioEventData, 0x28, "audio event"},
        {G::TrackHeader, 0x38, "track header"}, {G::VideoTrackData, 0xb4, "video track"}, {G::AudioTrackData, 0x310, "audio track"},
        {G::Media, 0x80, "media"}, {G::Marker, 0x28, "marker"}, {G::Group, 0x10, "group"}, {G::EnvelopeHeader, 0x48, "envelope header"},
        {G::EnvelopePoints, 0x38, "envelope points"}, {G::Fx, 0x30, "effect"}, {G::Bus, 0xc8, "bus"}, {G::Header, 0x90, "header"},
        {G::SummaryData, 8, "summary"}, {G::Mixer, 0x7f8, "mixer"},
    };
    if (c.isList) return std::string();
    for (const auto& k : kinds) {
        if (c.id != k.g) continue;
        *fixed = std::min(k.fixed, d.size()) & ~(size_t)3;
        std::string name = k.name;
        if (c.id == G::EventData) name += d.u32(0x08) == 3 ? " (video)" : " (audio)";
        if (c.id == G::TrackHeader) name += d.u32(0x0c) == 3 ? " (video)" : " (audio)";
        if (c.id == G::Bus) name += strfmt(" (type %u)", d.u32(0x0c));
        if (c.id == G::EnvelopePoints) name += d.u32(4) == 8 ? " (track)" : " (event)";
        if (c.id == G::Fx) name += d.u32(0) == 0x30 ? " (OpenFX)" : " (other)";
        if (c.id == G::Media) {
            const std::string pth = d.text(0x80, d.u32(0x24));
            name += pth.compare(0, 14, "META:\\SubClip\\") == 0 ? " (subclip)" : pth.compare(0, 5, "META:") == 0 ? " (generated)" : " (file)";
        }
        return name;
    }
    return std::string();
}

static void learnKinds(const veg::Chunk& c)
{
    for (const auto& k : c.kids) {
        if (k.isList) { learnKinds(k); continue; }
        size_t fixed = 0;
        const std::string name = kindOf(k, &fixed);
        if (name.empty()) continue;
        KindStats& s = g_kinds[name];
        if (s.records == 0 || fixed < s.fixed) {
            s.fixed = s.records == 0 ? fixed : std::min(s.fixed, fixed);
            s.values.resize(s.fixed / 4);
        }
        for (size_t w = 0; w < s.fixed / 4; ++w) s.values[w].insert(k.data.u32(4 * w));
        ++s.records;
    }
}

// The words of each kind of record that the writer fills in (their meaning is known; vegformat.h, vegas.cpp).
static bool writtenField(const std::string& kind, size_t off)
{
    static const std::map<std::string, std::vector<std::pair<size_t, size_t>>> fields = {
        {"header", {{0x0c, 0x20}, {0x28, 0x2c}, {0x3c, 0x50}}},
        {"video track", {{0x30, 0x50}, {0x64, 0x7c}, {0x80, 0x84}, {0x88, 0x98}, {0x9c, 0xa8}, {0xb0, 0xb4}}},
        {"audio track", {{0x08, 0x14}, {0x1f8, 0x200}, {0x204, 0x20c}, {0x300, 0x308}}},
        {"envelope points", {{0x08, 0x1c}}},
        {"envelope header", {{0x08, 0x18}, {0x30, 0x38}}},
        {"media", {{0x24, 0x28}, {0x38, 0x44}}},
        {"event", {{0x04, 0x08}, {0x10, 0x30}, {0x40, 0x4c}}},
        {"take", {{0x10, 0x28}}},
        {"marker", {{0x08, 0x24}}},
        {"group", {{0x08, 0x0c}}},
        {"effect", {{0x20, 0x24}, {0x28, 0x30}}},
        {"bus", {{0x14, 0x28}}},
        {"audio event", {{0x04, 0x28}}},
        {"video event", {{0x04, 0x08}, {0x68, 0x6c}}},
        {"track header", {{0x04, 0x0c}, {0x10, 0x18}, {0x1c, 0x20}}},
        {"mixer", {{0x6e4, 0x6ec}}},
    };
    auto it = fields.find(kind);
    if (it == fields.end()) return false;
    for (const auto& r : it->second)
        if (off >= r.first && off < r.second) return true;
    return false;
}

// The records of 'bytes' that are not in 'orig' (byte for byte) are checked against VEGAS's constants.
static int compareRecords(const std::string& bytes, const std::string& orig, const std::string& what)
{
    veg::ChunkTree t, o;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    std::set<std::string> known;
    if (!orig.empty()) {
        veg::readChunkTree(veg::Bytes((const uint8_t*)orig.data(), orig.size()), o);
        std::function<void(const veg::Chunk&)> add = [&](const veg::Chunk& c) {
            for (const auto& k : c.kids) {
                if (k.isList) add(k);
                else known.insert(k.id + std::string((const char*)k.data.data(), k.data.size()));
            }
        };
        add(o.root);
    }
    int made = 0, bad = 0;
    std::function<void(const veg::Chunk&)> walk = [&](const veg::Chunk& c) {
        for (const auto& k : c.kids) {
            if (k.isList) { walk(k); continue; }
            if (known.count(k.id + std::string((const char*)k.data.data(), k.data.size()))) continue;
            size_t fixed = 0;
            const std::string name = kindOf(k, &fixed);
            if (name.empty()) continue;
            auto it = g_kinds.find(name);
            if (it == g_kinds.end()) { ++bad; std::printf("   %s: a %s record, a kind no sample has\n", what.c_str(), name.c_str()); continue; }
            ++made;
            const KindStats& s = it->second;
            if (fixed < s.fixed) { ++bad; std::printf("   %s: a %s record shorter than VEGAS's\n", what.c_str(), name.c_str()); continue; }
            const std::string base = name.substr(0, name.find(" ("));
            for (size_t w = 0; w < s.values.size(); ++w) {
                if (s.values[w].size() != 1 || s.records < 2) continue;
                if (writtenField(base, 4 * w)) continue;
                const uint32_t want = *s.values[w].begin(), have = k.data.u32(4 * w);
                if (have != want) {
                    ++bad;
                    if (bad < 8) std::printf("   %s: %s record at %zu: word 0x%zx is %08x, VEGAS always has %08x\n", what.c_str(), name.c_str(), k.fileOffset,
                                             4 * w, have, want);
                }
            }
        }
    };
    walk(t.root);
    check(bad == 0, what + strfmt(": the %d records Reno made have VEGAS's fixed values", made));
    return made;
}

// ------------------------------------------------------------------------------------------ edits
// Every kind of value the writer patches, changed on every object that has it.
static void editValues(Project& p)
{
    ProjectSettings& s = p.settings;
    s.width = 1280;
    s.height = 720;
    s.fpsNum = 30000;
    s.fpsDen = 1001;
    s.sampleRate = 44100;
    s.audioBits = 24;
    s.tempo = 99.5;
    s.beatsPerMeasure = 3;
    s.pixelAspect = 1.0;
    s.title = "A title";
    s.artist = "Somebody";
    s.comments = "Saved by Reno";
    p.masterVolume = 0.75;
    for (auto& b : p.buses) b.level = 0.5;
    int ti = 0;
    for (auto& t : p.tracks) {
        ++ti;
        t.name = strfmt("Renamed %d \u00e9", ti);
        t.mute = !t.mute;
        t.solo = (ti % 3) == 0;
        t.height = 100 + ti;
        if (t.type == TrackType::Audio) {
            t.level = 0.25;
            t.pan = -0.5;
        } else {
            t.fadeTop = 0xFFFF8000;
            t.fadeBottom = 0xFF102030;
        }
        for (auto& f : t.fx) f.bypass = !f.bypass;
        for (auto& v : t.envelopes)
            for (auto& pt : v.points) pt.value = pt.value * 0.5;
        int ei = 0;
        for (auto& e : t.events) {
            ++ei;
            e.start += 0.5;
            e.length *= 0.75;
            e.offset += 0.25;
            if (t.type == TrackType::Video) e.rate *= 1.5;
            e.fadeIn = e.length / 8;
            e.fadeOut = e.length / 4;
            e.fadeInCurve = CurveSmooth;
            e.fadeOutCurve = CurveSharp;
            e.gain = 0.5;
            e.mute = (ei % 2) == 0;
            e.loop = (ei % 3) != 0;
            e.name = ei % 4 == 0 ? strfmt("event %d", ei) : std::string();
            if (t.type == TrackType::Audio) {
                e.channels = ei % 2 ? "swap" : "combine";
                e.keepPitch = true;
                e.pitch = 2.0;
            } else {
                e.resample = e.resample == "off" ? "project" : "off";
                if (!e.panCrop.empty()) e.panCrop[0].cx = 0.25;
            }
            for (auto& f : e.fx) {
                f.bypass = !f.bypass;
                if (f.kind != "ofx") continue;
                // a value that is not a whole number is a double: halve it; one keyframed parameter gets its keys moved
                Json params = f.state.get("params");
                Json np = Json::object();
                bool keyed = false;
                // VEGAS's managed effects and its older ones (Swirl...) keep their keys as 100 ns times and their value at the first key:
                // for them a keyed setting's value is its first key, and a key moves by a whole 0.1 s (a frame is not a whole tick)
                const bool tickKeys = f.pluginId.find("velvetmatter") != std::string::npos || f.pluginId.find("starburst") != std::string::npos ||
                                      f.pluginId.find(":swirl") != std::string::npos || f.pluginId.find(":pinchpunch") != std::string::npos ||
                                      f.pluginId.find(":wave") != std::string::npos || f.pluginId.find(":tvsimulator") != std::string::npos ||
                                      f.pluginId.find(":lightrays") != std::string::npos;
                const double keyShift = tickKeys ? 0.1 * p.settings.fps() : 1.0;
                for (const auto& m : params.members()) {
                    Json e2 = m.second;
                    if (e2.has("v") && e2.get("v").size() == 1 && !(tickKeys && e2.has("k"))) {
                        const double v = e2.get("v").at(0).asDouble();
                        if (v != std::floor(v)) {
                            Json nv = Json::array();
                            nv.push(v * 0.5);
                            e2["v"] = nv;
                        }
                    }
                    if (!keyed && e2.has("k") && e2.get("k").size() >= 1) {
                        Json nk = Json::array();
                        for (const auto& k : e2.get("k").items()) {
                            Json kk = Json::array();
                            kk.push(k.at(0).asDouble() + keyShift);
                            for (size_t d = 1; d < k.size(); ++d) kk.push(k.at(d).asDouble());
                            nk.push(kk);
                        }
                        e2["k"] = nk;
                        keyed = true;
                    }
                    if (e2.has("s") && m.first.find("MediaHostData") == std::string::npos && e2.get("s").asString().size() < 64)
                        e2["s"] = e2.get("s").asString() + "!";
                    // a text that changes over time: its last key moves a frame and gets another text
                    if (e2.has("ks") && e2.get("ks").size() >= 1) {
                        Json nk = Json::array();
                        const Json& ks = e2.get("ks");
                        for (size_t i = 0; i < ks.size(); ++i) {
                            Json k = Json::array();
                            const bool last = i + 1 == ks.size();
                            k.push(ks.at(i).at(0).asDouble() + (last ? 1.0 : 0.0));
                            k.push(last ? std::string("changed") : ks.at(i).at(1).asString());
                            nk.push(k);
                        }
                        e2["ks"] = nk;
                    }
                    np[m.first] = e2;
                }
                f.state["params"] = np;
            }
            for (auto& v : e.envelopes)
                for (auto& pt : v.points) pt.value = std::min(pt.value, 4.0);
            // locked; VEGAS's Time stretch / pitch shift settings; effects before pan/crop; takes
            e.locked = (ei % 5) == 0;
            if (t.type == TrackType::Audio) {
                switch (ei % 4) {
                case 0: e.stretchMethod = "elastique"; e.stretchMode = "pro"; break;
                case 1: e.stretchMethod = "classic"; e.stretchMode = strfmt("A%02d", ei % 19 + 1); break;
                case 2: e.stretchMethod = "elastique"; e.stretchMode = (ei % 8) == 2 ? "soloist-speech" : "efficient"; break;
                default: e.stretchMethod = "none"; break;
                }
                if ((ei % 7) == 3) { e.keepPitch = false; e.pitch = 0; }        // lock to stretch
                e.formant = (ei % 3) - 1.0;
                e.preserveFormants = (ei % 2) == 0;
                e.invertPhase = (ei % 3) == 0;
            } else if (!e.fx.empty() && e.fx[0].kind == "ofx" && e.fx[0].pluginId.find("velvetmatter") == std::string::npos) {
                // (not VEGAS's managed Rays/Glint: their record has no such switch)
                e.fx[0].beforePanCrop = !e.fx[0].beforePanCrop;
            }
            if (!e.takes.empty()) {
                // another take made active (VEGAS's T)
                Take& k = e.takes[0];
                std::swap(e.mediaId, k.mediaId);
                std::swap(e.offset, k.offset);
                std::swap(e.reverse, k.reverse);
                k.name.clear();
            }
        }
        // every compositing mode VEGAS has, for the picture and the parent's
        static const char* const modes[] = {"add", "subtract", "multiply", "cut", "screen", "overlay", "hardlight", "dodge", "burn", "darken",
                                            "lighten", "difference", "differencesquared", "srcalpha", "3dsrcalpha"};
        if (t.type == TrackType::Video) {
            t.composite = modes[ti % 15];
            t.parentComposite = modes[(ti + 5) % 15];
        }
    }
    int mi = 0;
    for (auto& m : p.markers) {
        m.time += 1.0;
        m.name = strfmt("Marker %d", ++mi);
        // the first a region
        if (mi == 1) {
            m.region = true;
            m.length = 1.5;
        }
    }
}

static FxSlot newOfx(Project& p, const std::string& plugin)
{
    FxSlot f;
    f.id = p.newId();
    f.kind = "ofx";
    f.pluginId = plugin;
    Json params = Json::object();
    Json a = Json::object();
    Json v = Json::array();
    v.push(0.375);
    a["v"] = v;
    a["i"] = false;
    params["Amount"] = a;
    Json b = Json::object();
    Json bv = Json::array();
    bv.push(1.0);
    b["v"] = bv;
    b["i"] = true;
    params["Enabled"] = b;
    Json c = Json::object();
    Json cv = Json::array();
    cv.push(0.1);
    cv.push(0.2);
    cv.push(0.3);
    cv.push(1.0);
    c["v"] = cv;
    c["i"] = false;
    Json ks = Json::array();
    Json k1 = Json::array();
    k1.push(0.0); k1.push(0.1); k1.push(0.2); k1.push(0.3); k1.push(1.0);
    Json k2 = Json::array();
    k2.push(12.0); k2.push(0.9); k2.push(0.8); k2.push(0.7); k2.push(1.0);
    ks.push(k1);
    ks.push(k2);
    c["k"] = ks;
    params["Colour"] = c;
    Json t = Json::object();
    t["s"] = "Hello";
    params["Text"] = t;
    f.state = Json::object();
    f.state["plugin"] = plugin;
    f.state["params"] = params;
    return f;
}

static FxSlot newDx(Project& p)
{
    FxSlot f;
    f.id = p.newId();
    f.kind = "audio";
    f.name = "Resonant Filter";
    f.audio.format = "DX";
    f.audio.uid = "dshow:{A6A78627-D619-48BF-AD26-0C6B44B5C7D8}";
    f.audio.name = "Resonant Filter";
    f.state = std::string("S:") + base64Encode("\x01\x02\x03\x04", 4);
    return f;
}

static FxSlot newVst(Project& p)
{
    FxSlot f;
    f.id = p.newId();
    f.kind = "audio";
    f.name = "eFX Limiter";
    f.audio.format = "VST2";
    f.audio.uid = strfmt("%u", 0x4d583241u);      // 'MX2A'
    f.audio.name = "eFX Limiter";
    const float vals[3] = {0.5f, 0.25f, 1.0f};
    f.state = "P:" + base64Encode(vals, sizeof vals);
    return f;
}

static Envelope env(const std::string& kind, std::initializer_list<EnvPoint> pts, uint64_t target = 0)
{
    Envelope e;
    e.kind = kind;
    e.target = target;
    e.points = pts;
    return e;
}

static uint64_t addMedia(Project& p, const std::string& path, bool video, bool audio, int w, int h)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    m.info.path = path;
    m.info.hasVideo = video;
    m.info.hasAudio = audio;
    m.info.width = w;
    m.info.height = h;
    m.info.duration = 60;
    p.media.push_back(m);
    return m.id;
}

static Event newEvent(Project& p, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = 1.0;
    return e;
}

// Adds and deletes objects of every kind.
static void editStructure(Project& p)
{
    // delete: every third event, the last track (if there are three or more), an effect here and there, a marker
    for (auto& t : p.tracks) {
        std::vector<Event> keep;
        for (size_t i = 0; i < t.events.size(); ++i)
            if (i % 3 != 1) keep.push_back(t.events[i]);
        t.events = keep;
        for (auto& e : t.events)
            if (e.fx.size() >= 2) e.fx.erase(e.fx.begin());
    }
    if (p.tracks.size() >= 3) p.tracks.pop_back();
    if (!p.markers.empty()) p.markers.erase(p.markers.begin());
    // media that no event uses any more is deleted too (and one more file is added)
    std::set<uint64_t> used;
    for (const auto& t : p.tracks)
        for (const auto& e : t.events) {
            used.insert(e.mediaId);
            for (const auto& k : e.takes) used.insert(k.mediaId);       // (another take's media is in use too)
        }
    std::vector<MediaItem> keepMedia;
    for (const auto& m : p.media)
        if (used.count(m.id)) keepMedia.push_back(m);
    p.media = keepMedia;
    const uint64_t clip = addMedia(p, "C:\\Reno test\\new clip.mp4", true, true, 1280, 720);
    const uint64_t song = addMedia(p, "C:\\Reno test\\new song.wav", false, true, 0, 0);
    const uint64_t pic = addMedia(p, "C:\\Reno test\\new picture.png", true, false, 800, 600);
    // a bus, and a new audio track that goes to it
    Bus b;
    b.id = p.newId();
    b.name = strfmt("Bus %c", (char)('A' + p.buses.size()));
    b.level = 0.5;
    b.fx.push_back(newDx(p));
    p.buses.push_back(b);
    // new tracks with new events, effects, envelopes, groups
    Track v;
    v.id = p.newId();
    v.type = TrackType::Video;
    v.name = "New video";
    v.envelopes.push_back(env("composite", {{0.0, 1.0, CurveLinear}, {2.0, 0.25, CurveSmooth}, {4.0, 0.5, CurveHold}}));
    v.fx.push_back(newOfx(p, "com.vegascreativesoftware:sharpen"));
    Track a;
    a.id = p.newId();
    a.type = TrackType::Audio;
    a.name = "New audio";
    a.level = 0.5;
    a.pan = 0.25;
    a.output = b.id;
    a.envelopes.push_back(env("volume", {{0.0, 1.0, CurveLinear}, {1.0, 0.5, CurveFast}, {3.0, 1.5, CurveSlow}}));
    a.envelopes.push_back(env("pan", {{0.0, -1.0, CurveLinear}, {2.0, 1.0, CurveLinear}}));
    a.envelopes.push_back(env("send", {{0.0, 0.5, CurveLinear}}, b.id));
    a.fx.push_back(newVst(p));
    for (int i = 0; i < 3; ++i) {
        Event ev = newEvent(p, i == 2 ? pic : clip, 1.0 + 2.0 * i, 1.5);
        ev.fx.push_back(newOfx(p, "com.vegascreativesoftware:brightnessandcontrast"));
        ev.fadeIn = 0.25;
        ev.fadeInCurve = CurveFast;
        ev.envelopes.push_back(env("velocity", {{0.0, 1.0, CurveLinear}, {1.0, 2.0, CurveLinear}}));
        if (i == 1) {
            PanCropKey k;
            k.cx = 0.4;
            k.cy = 0.6;
            k.w = 0.5;
            k.h = 0.5;
            k.rotation = 10;
            ev.panCrop.push_back(k);
            k.time = 1.0;
            k.curve = 2;
            ev.panCrop.push_back(k);
        }
        Event ae = newEvent(p, i == 2 ? song : clip, 1.0 + 2.0 * i, 1.5);
        ae.envelopes.push_back(env("volume", {{0.0, 0.5, CurveLinear}, {1.0, 1.0, CurveLinear}}));
        ae.channels = "left";
        if (i == 0) ae.fx.push_back(newVst(p));
        if (i != 2) {
            const uint64_t g = p.newId();
            ev.group = g;
            ae.group = g;
        }
        v.events.push_back(ev);
        a.events.push_back(ae);
    }
    p.tracks.insert(p.tracks.begin(), v);
    p.tracks.push_back(a);
    // a copy of an existing event (as a split leaves it), and an event moved to another track of its kind
    for (auto& t : p.tracks)
        if (t.events.size() >= 2 && t.events[0].veg) {
            Event e2 = t.events[0];
            e2.id = p.newId();
            e2.start = t.events.back().end() + 1.0;
            t.events.push_back(e2);
            break;
        }
    Project::Marker m;
    m.id = p.newId();
    m.time = 3.0;
    m.name = "New marker";
    p.markers.push_back(m);
    for (auto& t : p.tracks) t.sortEvents();
}


// More edits: effect settings of audio plug-ins, media swapped and relinked, events reversed, generators,
// envelopes with points added and removed, buses and the master given effects, markers made regions.
static void editMore(Project& p)
{
    std::vector<uint64_t> fileMedia;
    for (auto& m : p.media)
        if (!m.generated) fileMedia.push_back(m.id);
    for (auto& m : p.media) {
        if (m.generated) {
            Json params = m.generator.state.get("params");
            Json np = Json::object();
            for (const auto& kv : params.members()) {
                Json e = kv.second;
                if (e.has("s") && kv.first == "Text") e["s"] = std::string("{\\rtf1 changed}");
                np[kv.first] = e;
            }
            m.generator.state["params"] = np;
        }
    }
    if (!fileMedia.empty()) {
        // a file moved: its media item points at the new place
        MediaItem* m = p.findMedia(fileMedia.back());
        m->path = "C:\\Moved\\moved " + pathFileName(m->path);
    }
    int n = 0;
    for (auto& t : p.tracks) {
        for (auto& f : t.fx)
            if (f.kind == "audio" && f.state.isString() && f.state.asString().compare(0, 2, "P:") == 0) {
                std::string d = base64Decode(f.state.asString().substr(2));
                if (d.size() >= 4) {
                    float v = 0.125f;
                    std::memcpy(&d[0], &v, 4);
                    f.state = "P:" + base64Encode(d.data(), d.size());
                }
            }
        if (t.type == TrackType::Audio && !t.envelopes.empty()) t.envelopes.erase(t.envelopes.begin());
        for (auto& e : t.events) {
            ++n;
            for (auto& f : e.fx) {
                if (f.kind != "audio" || !f.state.isString()) continue;
                const std::string st = f.state.asString();
                if (st.compare(0, 2, "S:") == 0) {
                    std::string d = base64Decode(st.substr(2));
                    if (!d.empty()) d[0] = (char)(d[0] ^ 0x5a);
                    f.state = "S:" + base64Encode(d.data(), d.size());
                } else if (st.compare(0, 2, "P:") == 0) {
                    std::string d = base64Decode(st.substr(2));
                    if (d.size() >= 4) {
                        float v = 0.75f;
                        std::memcpy(&d[0], &v, 4);
                        f.state = "P:" + base64Encode(d.data(), d.size());
                    }
                }
            }
            if (n % 5 == 0 && fileMedia.size() >= 2) e.mediaId = fileMedia[(size_t)n % fileMedia.size()];
            if (n % 7 == 0) e.reverse = !e.reverse;
            if (n % 3 == 0 && e.fx.size() >= 3) e.fx.erase(e.fx.begin() + 1);
            for (auto& v : e.envelopes) {
                EnvPoint pt;
                pt.time = e.length / 2;
                pt.value = 0.5;
                pt.curve = CurveFast;
                v.points.push_back(pt);
                std::stable_sort(v.points.begin(), v.points.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
            }
            if (t.type == TrackType::Video && n % 4 == 0 && !findEnvelope(e.envelopes, "velocity"))
                e.envelopes.push_back(env("velocity", {{0.0, 0.5, CurveLinear}, {e.length, 2.0, CurveLinear}}));
            if (t.type == TrackType::Audio && n % 4 == 1 && !findEnvelope(e.envelopes, "pan"))
                e.envelopes.push_back(env("pan", {{0.0, -0.5, CurveSmooth}}));
        }
    }
    p.audioOut.push_back(newDx(p));
    if (p.audioOut.size() >= 2) p.audioOut.erase(p.audioOut.begin());
    for (auto& b : p.buses) {
        b.name = b.name + " renamed";
        if (!b.fx.empty()) b.fx.pop_back();
        else b.fx.push_back(newVst(p));
    }
}

// Nonsense in every field the writer reads: it must not crash, and what it writes must open.
static void fuzz(Project& p, uint64_t seed)
{
    uint64_t s = seed * 2654435761ull + 7;
    auto rnd = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    auto weird = [&]() -> double {
        switch (rnd() % 8) {
        case 0: return std::nan("");
        case 1: return 1e300;
        case 2: return -5;
        case 3: return 0;
        case 4: return INFINITY;
        default: return (double)(rnd() % 100000) / 7.0;
        }
    };
    for (auto& t : p.tracks) {
        if (rnd() % 4 == 0) t.name = std::string(300, 'x') + "\xff\xfe";
        t.level = weird();
        t.pan = weird();
        t.height = (int)(rnd() % 5000) - 100;
        if (rnd() % 3 == 0) t.output = rnd();
        if (rnd() % 3 == 0) t.composite = "multiply";
        for (auto& e : t.events) {
            if (rnd() % 3 == 0) e.start = weird();
            if (rnd() % 3 == 0) e.length = weird();
            if (rnd() % 3 == 0) e.offset = weird();
            if (rnd() % 3 == 0) e.rate = weird();
            if (rnd() % 3 == 0) e.gain = weird();
            if (rnd() % 5 == 0) e.mediaId = rnd();
            if (rnd() % 5 == 0) e.reverse = !e.reverse;
            if (rnd() % 5 == 0) e.fadeInCurve = (int)(rnd() % 20) - 5;
            if (rnd() % 5 == 0) e.channels = "nonsense";
            if (rnd() % 4 == 0) {
                Envelope v;
                v.kind = rnd() % 2 ? "velocity" : "fx";
                v.points.push_back({weird(), weird(), (int)(rnd() % 10)});
                e.envelopes.push_back(v);
            }
            for (auto& f : e.fx) {
                if (rnd() % 3 == 0) f.state = Json::array();
                if (rnd() % 3 == 0) f.state = std::string("P:%%%%");
                if (rnd() % 5 == 0) f.audio.format = "VST3";
                if (rnd() % 5 == 0) f.pluginId.clear();
            }
            if (rnd() % 4 == 0) {
                PanCropKey k;
                k.w = weird();
                k.cx = weird();
                e.panCrop.push_back(k);
            }
            if (rnd() % 6 == 0) e.transition.kind = "ofx";
        }
        if (rnd() % 4 == 0) std::reverse(t.events.begin(), t.events.end());
    }
    if (rnd() % 2) p.settings.fpsDen = 0;
    if (rnd() % 2) p.settings.width = -3;
    for (auto& m : p.markers) m.time = weird();
    for (auto& m : p.media)
        if (rnd() % 3 == 0) m.path.clear();
    if (rnd() % 3 == 0 && !p.buses.empty()) p.buses.erase(p.buses.begin());
    for (int i = 0; i < 30 && rnd() % 2; ++i) {
        Bus b;
        b.id = p.newId();
        b.name = "B";
        p.buses.push_back(b);
    }
}

// A project made in Reno: no VEGAS file behind any of it.
static Project madeInReno()
{
    Project p;
    p.settings.width = 1280;
    p.settings.height = 720;
    p.settings.fpsNum = 25;
    p.settings.fpsDen = 1;
    p.settings.sampleRate = 44100;
    p.settings.title = "New project";
    const uint64_t clip = addMedia(p, "C:\\Reno test\\clip.mp4", true, true, 1920, 1080);
    const uint64_t song = addMedia(p, "C:\\Reno test\\song.mp3", false, true, 0, 0);
    const uint64_t nested = addMedia(p, "C:\\Reno test\\other project.veg", true, true, 1920, 1080);
    MediaItem gen;
    gen.id = p.newId();
    gen.generated = true;
    gen.name = "Solid colour";
    gen.info.hasVideo = true;
    gen.info.width = 1280;
    gen.info.height = 720;
    gen.generator = newOfx(p, "com.vegascreativesoftware:solidcolor");
    p.media.push_back(gen);
    Track v;
    v.id = p.newId();
    v.type = TrackType::Video;
    v.name = "Video";
    Event e1 = newEvent(p, clip, 0, 2);
    e1.fx.push_back(newOfx(p, "com.vegascreativesoftware:sharpen"));
    Event e2 = newEvent(p, nested, 2, 2);
    Event e3 = newEvent(p, gen.id, 4, 1);
    e3.offset = 0;
    v.events = {e1, e2, e3};
    Track a;
    a.id = p.newId();
    a.type = TrackType::Audio;
    a.name = "Music";
    Event s1 = newEvent(p, song, 0, 5);
    s1.fadeOut = 1.0;
    s1.fx.push_back(newDx(p));
    Event s2 = newEvent(p, clip, 0, 2);
    const uint64_t g = p.newId();
    e1.group = g;
    s2.group = g;
    v.events[0].group = g;
    a.events = {s1, s2};
    a.sortEvents();
    a.envelopes.push_back(env("volume", {{0.0, 1.0, CurveLinear}, {5.0, 0.0, CurveSlow}}));
    p.tracks = {v, a};
    p.audioOut.push_back(newVst(p));
    Project::Marker m;
    m.id = p.newId();
    m.time = 1.0;
    m.name = "Here";
    p.markers.push_back(m);
    return p;
}

int main(int argc, char** argv)
{
    std::string dir = testDataDir("RENO_SAMPLES"), outDir;
    for (int a = 1; a < argc; ++a) {
        const std::string s = argv[a];
        if (s == "--out" && a + 1 < argc) outDir = argv[++a];
        else if (s == "-v") g_verbose = true;
        else dir = s;
    }
    if (testDataMissing(dir)) return 0;
    const std::vector<std::string> files = findProjects(dir);
    std::printf("%zu projects\n", files.size());

    // 1. unchanged: the same bytes
    int identical = 0;
    for (const auto& f : files) {
        std::string raw;
        if (!readFile(f, raw)) { check(false, f + ": read"); continue; }
        Project p;
        const VegasImportResult r = importVegasData(raw, f, p);
        if (!check(r.ok, f + ": opens")) continue;
        {
            veg::ChunkTree t;
            veg::readChunkTree(veg::Bytes((const uint8_t*)raw.data(), raw.size()), t);
            learnKinds(t.root);
        }
        std::vector<std::string> warnings;
        const std::string out = writeVegasData(p, headerPath(raw), &warnings);
        const std::string diff = firstDifference(raw, out);
        if (check(diff.empty(), pathFileName(f) + ": written back unchanged, byte for byte (" + diff + ")")) ++identical;
        check(warnings.empty(), pathFileName(f) + strfmt(": no warnings for an unchanged project (%zu)", warnings.size()));
        if (!warnings.empty() && g_verbose)
            for (const auto& w : warnings) std::printf("   %s\n", w.c_str());
    }
    std::printf("identical: %d of %zu\n", identical, files.size());

    // 2. edits
    int n = 0, madeRecords = 0;
    for (const auto& f : files) {
        Project p;
        if (!importVegasProject(f, p).ok) continue;
        const std::string name = pathFileName(f);
        Project a = p;
        editValues(a);
        const std::string ba = saveAndRead(a, headerPath([&] { std::string r; readFile(f, r); return r; }()), name + " (values changed)", nullptr, nullptr, f);
        Project c = p;
        editMore(c);
        const std::string bc = saveAndRead(c, "C:\\Reno test\\" + name, name + " (more edits)", nullptr, nullptr, f);
        Project b = p;
        editStructure(b);
        const std::string bb = saveAndRead(b, "C:\\Reno test\\" + name, name + " (added and deleted)", nullptr, nullptr, f);
        std::string raw;
        readFile(f, raw);
        madeRecords += compareRecords(ba, raw, name + " (values changed)");
        madeRecords += compareRecords(bb, raw, name + " (added and deleted)");
        madeRecords += compareRecords(bc, raw, name + " (more edits)");
        // nonsense: no crash, and a file that opens
        for (uint64_t seed = 1; seed <= 3; ++seed) {
            Project z = p;
            fuzz(z, seed + 10 * (uint64_t)n);
            std::vector<std::string> w;
            const std::string bz = writeVegasData(z, "C:\\x.veg", &w);
            Project q;
            const VegasImportResult r = importVegasData(bz, "C:\\x.veg", q);
            veg::ChunkTree t;
            veg::readChunkTree(veg::Bytes((const uint8_t*)bz.data(), bz.size()), t);
            check(r.ok && !t.damaged, name + strfmt(" (nonsense %d): writes a file that opens", (int)seed));
        }
        if (!outDir.empty()) {
            writeFile(outDir + "/" + pathStem(name) + " - values changed.veg", ba);
            writeFile(outDir + "/" + pathStem(name) + " - added and deleted.veg", bb);
            writeFile(outDir + "/" + pathStem(name) + " - more edits.veg", bc);
        }
        ++n;
    }
    std::printf("records made by the writer and checked against VEGAS's: %d\n", madeRecords);
    // effects copied from other projects (as a copy and paste between projects, or a nested timeline, does): their
    // records come from the other file, every setting kept, and audio plug-ins keep VEGAS's names for them
    {
        // the base: a sample with a video track of events over an audio track of events; the other: the sample with the most video effects
        Project die, other;
        std::string dieFile;
        size_t baseEvents = 0, mostFx = 0;
        bool haveOther = false;
        for (const std::string& f : files) {
            Project cand;
            if (!importVegasProject(f, cand).ok) continue;
            size_t events = 0, fxCount = 0;
            for (const auto& t : cand.tracks) for (const auto& e : t.events) { ++events; if (t.type == TrackType::Video) fxCount += e.fx.size(); }
            if (cand.tracks.size() >= 2 && cand.tracks[0].type == TrackType::Video && cand.tracks[1].type == TrackType::Audio && !cand.tracks[0].events.empty() &&
                !cand.tracks[1].events.empty() && (dieFile.empty() || events < baseEvents)) { dieFile = f; die = cand; baseEvents = events; }
            if (fxCount > mostFx) { mostFx = fxCount; other = cand; haveOther = true; }
        }
        if (!dieFile.empty() && haveOther) {
            int copied = 0;
            for (const auto& t : other.tracks)
                for (const auto& e : t.events)
                    for (const auto& f : e.fx)
                        if (t.type == TrackType::Video) { die.tracks[0].events[0].fx.push_back(f); ++copied; }
            for (const auto& f : other.audioOut) die.tracks[1].events[0].fx.push_back(f);
            for (const auto& b : other.buses)
                for (const auto& f : b.fx) die.tracks[1].events[0].fx.push_back(f);
            std::vector<std::string> warnings;
            saveAndRead(die, "C:\\Reno test\\base with copied effects.veg", "effects copied from another project", nullptr, &warnings,
                        dieFile);
            check(copied > 0 && warnings.empty(), strfmt("%d copied video effects and the audio ones saved with nothing left out (%zu warnings)", copied, warnings.size()));
            for (const auto& w : warnings) std::printf("   %s\n", w.c_str());
        }
        // an effect whose state says nothing about whole numbers, and no record anywhere says how it is laid out
        Project p = madeInReno();
        FxSlot f;
        f.id = p.newId();
        f.kind = "ofx";
        f.pluginId = "com.example:unknown";
        f.state = Json::object();
        Json params = Json::object();
        Json a = Json::object();
        Json v = Json::array();
        v.push(2.0);
        a["v"] = v;
        params["Mystery"] = a;
        f.state["params"] = params;
        p.tracks[0].events[0].fx.push_back(f);
        std::vector<std::string> warnings;
        writeVegasData(p, "C:\\x.veg", &warnings);
        bool said = false;
        for (const auto& w : warnings) said = said || w.find("Mystery") != std::string::npos;
        check(said, "a setting that cannot be laid out is left out, and that is said");
    }
    // 3. a project made in Reno
    {
        Project p = madeInReno();
        std::vector<std::string> warnings;
        const std::string bytes = saveAndRead(p, "C:\\Reno test\\made in Reno.veg", "a project made in Reno", nullptr, &warnings);
        compareRecords(bytes, std::string(), "a project made in Reno");
        // a region: VEGAS keeps it in the marker list with its region bit and its length
        Project::Marker r;
        r.id = p.newId();
        r.time = 5.0;
        r.length = 2.0;
        r.region = true;
        r.name = "A region";
        p.markers.push_back(r);
        std::vector<std::string> w2;
        Project q;
        const std::string withRegion = writeVegasData(p, "C:\\Reno test\\made in Reno.veg", &w2);
        importVegasData(withRegion, "C:\\x.veg", q);
        bool said = false;
        for (const auto& w : w2) said = said || w.find("region") != std::string::npos;
        check(!said && q.markers.size() == 2 && q.markers[1].region && q.markers[1].time == 5.0 && std::fabs(q.markers[1].length - 2.0) < 1e-6 &&
                  q.markers[1].name == "A region",
              "a region is saved as a region (the marker list's region bit and its length), with no warning");
        // command markers and CD track / index markers: the lists VEGAS keeps them in (90020ef5, 90020ef7), the
        // command as "<command>:<parameter>" in the record's second text
        {
            Project c = p;
            auto add = [&](double t, double len, const char* kind, const char* name, const char* cmd) {
                Project::Marker mk;
                mk.id = c.newId();
                mk.time = t;
                mk.length = len;
                mk.region = len > 0;
                mk.kind = kind;
                mk.name = name;
                mk.command = cmd;
                c.markers.push_back(mk);
            };
            add(1.0, 0, "command", "http://example.com/a:b", "URL");
            add(1.5, 0, "command", "Hello", "TEXT");
            add(0.0, 3.0, "cdtrack", "Track one", "");
            add(1.25, 0, "cdindex", "Index", "");
            using namespace veg;
            std::vector<std::string> w3;
            const std::string cb = writeVegasData(c, "C:\\Reno test\\made in Reno.veg", &w3);
            Project cq;
            importVegasData(cb, "C:\\Reno test\\made in Reno.veg", cq);
            int cmds = 0, tracks = 0, indexes = 0;
            for (const auto& mk : cq.markers) {
                if (mk.kind == "command" && mk.command == "URL" && mk.name == "http://example.com/a:b" && mk.time == 1.0) ++cmds;
                if (mk.kind == "command" && mk.command == "TEXT" && mk.name == "Hello") ++cmds;
                if (mk.kind == "cdtrack" && mk.region && std::fabs(mk.length - 3.0) < 1e-6 && mk.name == "Track one") ++tracks;
                if (mk.kind == "cdindex" && !mk.region && mk.time == 1.25 && mk.name == "Index") ++indexes;
            }
            bool saidM = false;
            for (const auto& w : w3) saidM = saidM || w.find("marker") != std::string::npos;
            check(cmds == 2 && tracks == 1 && indexes == 1 && cq.markers.size() == c.markers.size() && !saidM,
                  "command markers and CD track / index markers are saved and read back, with no warning");
            check(cb.find(guidBytes(G::CommandMarkers)) != std::string::npos && cb.find(guidBytes(G::CdMarkers)) != std::string::npos &&
                      cb.find(utf16("URL:http://example.com/a:b", true)) != std::string::npos,
                  "they are in VEGAS's command and CD marker lists, the command as 'URL:<parameter>'");
            std::string a, b2;
            splitMarkerCommand(joinMarkerCommand("A:B\tC", "x:y"), a, b2);
            check(a == "A:B\tC" && b2 == "x:y", "a command with ':' or a TAB in it comes back as it was");
            // saved again from what was read: the same bytes
            const std::string cb2 = writeVegasData(cq, "C:\\Reno test\\made in Reno.veg", nullptr);
            check(cb2 == cb, "command and CD markers read back are written back byte for byte");
            if (!outDir.empty()) writeFile(outDir + "/made in Reno - command and CD markers.veg", cb);
        }
        for (const auto& w : warnings) std::printf("   warning: %s\n", w.c_str());
        if (!outDir.empty()) writeFile(outDir + "/made in Reno.veg", bytes);
    }
    // 4. saving through saveVegasProject: the previous file is kept as .bak; a project saved, then saved again with no
    // change, gives the same bytes (it is tied to the file it was saved to); and edits after a save come out right
    {
        namespace fs = std::filesystem;
        const std::string tmp = (fs::temp_directory_path() / "reno_vegroundtrip").string();
        fs::remove_all(tmp);
        fs::create_directories(tmp);
        int saved = 0, same = 0;
        for (const auto& f : files) {
            const std::string name = pathFileName(f);
            Project p;
            if (!importVegasProject(f, p).ok) continue;
            // (each in a folder of its own, so that no other project is found next to it as media)
            const std::string folder = pathJoin(tmp, strfmt("%d", saved));
            fs::create_directories(folder);
            const std::string target = pathJoin(folder, name);
            fs::copy_file(f, target, fs::copy_options::overwrite_existing);
            editValues(p);
            VegasSaveResult r1 = saveVegasProject(p, target);
            if (!check(r1.ok, name + ": saved (" + r1.error + ")")) continue;
            ++saved;
            std::string first, orig, bak;
            readFile(target, first);
            readFile(f, orig);
            check(readFile(target + ".bak", bak) && bak == orig, name + ": the file that was there is kept as .bak");
            check(!fs::exists(target + ".reno-saving"), name + ": no temporary file is left");
            // saved again, unchanged: the same bytes
            const VegasSaveResult r2 = saveVegasProject(p, target);
            std::string second;
            readFile(target, second);
            const std::string diff = firstDifference(first, second);
            if (check(r2.ok && diff.empty(), name + ": saved again unchanged: the same bytes (" + diff + ")")) ++same;
            // edited after saving: what is read back is what was saved
            editStructure(p);
            const VegasSaveResult r3 = saveVegasProject(p, target);
            // (read as if it were where the sample is, so that its media is found the same way)
            Project q;
            std::string third;
            readFile(target, third);
            if (check(r3.ok && importVegasData(third, f, q).ok, name + ": saved after more edits")) sameProject(p, q, name + " (edited after saving)");
        }
        std::printf("saved through saveVegasProject: %d, saved again identically: %d\n", saved, same);
        fs::remove_all(tmp);
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
