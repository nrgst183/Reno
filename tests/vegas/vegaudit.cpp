// vegaudit: finds what Reno does not read of a VEGAS project (.veg).
//
// The reader (src/vegas) looks at the file through veg::Bytes. Built with RENO_VEG_TRACE, Bytes reports every byte the
// reader looked at, so after importing a project the tool knows which bytes of each chunk were read. Over all the
// projects given it reports, per kind of chunk (named by its place in the tree): how often it occurs, how big it is,
// and for every 4-byte word the reader never looked at, whether it is the same in every project (probably a default
// or unused) or changes (a VEGAS setting Reno does not read) and which values it takes.
//
// It also counts what the file has and what the imported project has (events, takes, effects, envelopes, markers...),
// and what the reader kept only to write back, project by project.
//
//   vegaudit [--words] [--counts] [--quiet] [folder or .veg files ...]      (default: $RENO_SAMPLES)
//     --words   per chunk kind, every unread word (default: only chunk kinds with unread words, shortened)
//     --counts  the per-project counts table
//
// Build with: g++ ... -DRENO_VEG_TRACE vegformat.cpp (the one file) ahead of the library, so that its trace hook is in.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/veglegacyfx.h"
#include "../../src/vegas/vegmanaged.h"
#include "../../src/vegas/vegorigin.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/probe.h"
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include "../testdata.h"

using namespace reno;

namespace reno {
namespace veg {
extern void (*g_vegTrace)(const uint8_t* p, size_t len);
}
}

namespace {

std::vector<std::pair<const uint8_t*, size_t>> g_reads;
void traceRead(const uint8_t* p, size_t n) { g_reads.push_back({p, n}); }

// ------------------------------------------------------------------------------------------ names
const std::map<std::string, std::string>& names()
{
    static const std::map<std::string, std::string> m = {
        {veg::G::Header, "ProjectHeader"}, {veg::G::MediaList, "MediaList"}, {veg::G::Media, "Media"}, {veg::G::Tracks, "Tracks"},
        {veg::G::Track, "Track"}, {veg::G::TrackHeader, "TrackHeader"}, {veg::G::Event, "Event"}, {veg::G::EventData, "EventData"},
        {veg::G::Take, "Take"}, {veg::G::VideoEventList, "VideoEventList"}, {veg::G::VideoEventData, "VideoEventData"},
        {veg::G::AudioEventList, "AudioEventList"}, {veg::G::EventFx, "EventVideoFx"}, {veg::G::TrackFx, "TrackVideoFx"},
        {veg::G::Fx, "FxRecord"}, {veg::G::PanCrop, "PanCrop"}, {veg::G::EventEnvelopes, "EventEnvelopes"},
        {veg::G::TrackEnvelopes, "TrackEnvelopes"}, {veg::G::EnvelopeHeader, "EnvelopeHeader"}, {veg::G::EnvelopePoints, "EnvelopePoints"},
        {veg::G::EventAudioFx, "EventAudioFx"}, {veg::G::TrackAudioFx, "TrackAudioFx"}, {veg::G::VideoTrackList, "VideoTrackList"},
        {veg::G::VideoTrackData, "VideoTrackData"}, {veg::G::AudioTrackList, "AudioTrackList"}, {veg::G::AudioTrackData, "AudioTrackData"},
        {veg::G::TrackMotionList, "TrackMotionList"}, {veg::G::TrackMotion, "TrackMotion"}, {veg::G::TrackExtraA, "TrackExtraA"},
        {veg::G::TrackExtraB, "TrackExtraB"}, {veg::G::Groups, "Groups"}, {veg::G::Group, "Group"}, {veg::G::Markers, "Markers"},
        {veg::G::Marker, "Marker"}, {veg::G::CommandMarkers, "CommandMarkers"}, {veg::G::MarkersB, "MarkersB"}, {veg::G::CdMarkers, "CdMarkers"},
        {veg::G::Summary, "Summary"}, {veg::G::SummaryData, "SummaryData"}, {veg::G::Buses, "Buses"}, {veg::G::Mixer, "Mixer"},
        {veg::G::Bus, "Bus"}, {veg::G::ViewList, "ViewList"}, {veg::G::ViewData, "ViewData"}, {veg::G::Timelines, "Timelines"},
        {veg::G::TimelineList, "TimelineList"}, {veg::G::TimelineData, "TimelineData"}, {veg::G::WindowState, "WindowState"},
        {veg::G::EmptyListA, "EmptyListA"}, {veg::G::EmptyChunkA, "EmptyChunkA"}};
    return m;
}

std::string nameOf(const veg::Chunk& c)
{
    const std::string& g = c.isList ? c.type : c.id;
    auto it = names().find(g);
    std::string n = it != names().end() ? it->second : g.substr(0, 8);
    // lists and chunks that share a GUID (the track's event list and its header)
    return (c.isList ? "[" : "") + n + (c.isList ? "]" : "");
}

// ------------------------------------------------------------------------------------------ the tally
struct WordInfo {
    int seen = 0;               // instances big enough to have this word
    int unread = 0;             // of them, those whose bytes were not (all) read
    bool partial = false;       // some instance had only some of its bytes unread (values are masked)
    std::map<uint32_t, int> values;     // the values of the unread ones
    std::set<std::string> projects;     // projects where it is unread and non-zero
};

struct Kind {
    int count = 0;
    std::set<std::string> projects;
    std::set<size_t> sizes;
    size_t bytes = 0, unreadBytes = 0;
    std::map<size_t, WordInfo> words;       // by offset (multiple of 4)
    int fullyRead = 0;
    struct Sample { std::string data, read; };      // a copy of an instance and which of its bytes were read (1/0)
    std::vector<Sample> samples;
};

std::map<std::string, Kind> g_kinds;
std::vector<std::string> g_kindOrder;

void walk(const veg::Chunk& c, const std::string& path, const std::vector<char>& readMap, const uint8_t* base, const std::string& proj)
{
    for (const auto& k : c.kids) {
        const std::string name = path + "/" + nameOf(k);
        if (k.isList) {
            walk(k, name, readMap, base, proj);
            continue;
        }
        Kind* kind;
        auto it = g_kinds.find(name);
        if (it == g_kinds.end()) { g_kindOrder.push_back(name); kind = &g_kinds[name]; }
        else kind = &it->second;
        kind->count++;
        kind->projects.insert(proj);
        const size_t n = k.data.size();
        kind->sizes.insert(n);
        kind->bytes += n;
        const size_t off0 = (size_t)(k.data.data() - base);
        size_t unread = 0;
        for (size_t i = 0; i < n; ++i) unread += readMap[off0 + i] ? 0 : 1;
        kind->unreadBytes += unread;
        if (!unread) kind->fullyRead++;
        {
            Kind::Sample sm;
            sm.data.assign((const char*)k.data.data(), n);
            sm.read.assign(readMap.begin() + off0, readMap.begin() + off0 + n);
            kind->samples.push_back(std::move(sm));
        }
        for (size_t w = 0; w + 4 <= n; w += 4) {
            WordInfo& wi = kind->words[w];
            wi.seen++;
            // which bytes of the word were not read: the word's value is shown with the read bytes masked out, so that a
            // word the reader only half uses (a flags byte) shows what is left in its other bytes
            uint32_t mask = 0;
            for (size_t i = 0; i < 4; ++i)
                if (!readMap[off0 + w + i]) mask |= 0xffu << (8 * i);
            if (!mask) continue;
            wi.unread++;
            if (mask != 0xffffffffu) wi.partial = true;
            const uint32_t v = k.data.u32(w) & mask;
            wi.values[v]++;
            if (v) wi.projects.insert(proj);
        }
    }
}

// ------------------------------------------------------------------------------------------ counts
struct Counts {
    int tracks = 0, events = 0, takes = 0, fxChunks = 0, fxOfx = 0, audioFxChunks = 0, audioFxPlugins = 0, envelopesInFile = 0, panCrops = 0;
    int eventsInProject = 0, takesInProject = 0, fxInProject = 0, envInProject = 0, panCropKeys = 0;
    int hiddenEvents = 0, hiddenFx = 0, hiddenEnv = 0;
    int media = 0, mediaProject = 0, notes = 0;
};

void count(const veg::Chunk& c, Counts& n)
{
    for (const auto& k : c.kids) {
        if (k.isList && k.type == veg::G::Track) ++n.tracks;
        if (!k.isList && k.id == veg::G::EventData) ++n.events;
        if (!k.isList && k.id == veg::G::Take) ++n.takes;
        if (!k.isList && k.id == veg::G::Fx) ++n.fxChunks;
        if (!k.isList && k.id == veg::G::EnvelopePoints) ++n.envelopesInFile;
        if (!k.isList && k.id == veg::G::PanCrop) ++n.panCrops;
        if (!k.isList && k.id == veg::G::EventAudioFx) ++n.audioFxChunks;
        if (!k.isList && k.id == veg::G::TrackAudioFx) ++n.audioFxChunks;
        if (!k.isList && k.id == veg::G::Media) ++n.media;
        if (k.isList) count(k, n);
    }
}

std::string valuesText(const std::map<uint32_t, int>& v)
{
    std::vector<std::pair<int, uint32_t>> by;
    for (const auto& e : v) by.push_back({-e.second, e.first});
    std::sort(by.begin(), by.end());
    std::string s;
    int shown = 0;
    for (const auto& e : by) {
        if (shown++ >= 6) { s += strfmt(" ... (%zu values)", v.size()); break; }
        const uint32_t x = e.second;
        float f;
        std::memcpy(&f, &x, 4);
        if (x > 0x00100000 && x < 0x7f000000 && f == f && std::fabs(f) > 1e-6 && std::fabs(f) < 1e7 && (x >> 22) != 0)
            s += strfmt(" 0x%x(f %g)x%d", x, f, -e.first);
        else
            s += strfmt(" 0x%x x%d", x, -e.first);
    }
    return s;
}


// ------------------------------------------------------------------------------------------ accounting
// What the file holds against what the imported project holds: effects, plug-ins, envelopes, pan/crop, takes.
struct Usage { int instances = 0; std::set<std::string> projects; };
std::map<std::string, Usage> g_ofxUsage;        // OpenFX effect id -> uses
std::map<std::string, Usage> g_nonOfx;          // pre-OpenFX video effect (GUID + preset + the built-in it becomes) -> uses
std::map<std::string, Usage> g_audioUsage;      // audio plug-in (format + name) -> uses
std::map<std::string, Usage> g_envFile;         // envelope type name (event/track) in the file
std::map<std::string, Usage> g_envKept;         // envelope kinds in the project
std::map<std::string, Usage> g_flags;           // counted features of the imported project
std::map<std::string, std::map<std::string, int>> g_projFacts;      // project -> fact -> number

void fact(const std::string& proj, const std::string& what, int n = 1)
{
    g_projFacts[proj][what] += n;
    Usage& u = g_flags[what];
    u.instances += n;
    if (n) u.projects.insert(proj);
}

void forChunks(const veg::Chunk& c, const std::string& path, const std::function<void(const std::string&, const veg::Chunk&)>& fn)
{
    for (const auto& k : c.kids) {
        const std::string name = path + "/" + nameOf(k);
        if (k.isList) forChunks(k, name, fn);
        else fn(name, k);
    }
}

bool endsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}


// What the engine can do with the effects of a project on this machine (the render-level check): every effect slot is
// given an instance by FxRuntime; those without one (no plug-in of that id here, no built-in either) are counted with
// the reason, and those that run on Reno's built-in because the plug-in file is missing are counted too.
struct FxCensus { int slots = 0, ok = 0, fallback = 0, missing = 0; std::map<std::string, int> missingBy, fallbackBy; };
void fxCensus(const std::string& proj, Project& p, FxCensus& c, std::map<std::string, FxCensus>& per)
{
    FxRuntime fx;
    fx.prepare(p);
    FxCensus& mine = per[proj];
    p.forEachFx([&](FxSlot& slot) {
        for (FxCensus* t : {&c, &mine}) {
            ++t->slots;
            const bool audio = slot.kind == "audio";
            const std::string id = audio ? ("audio " + slot.audio.format + " " + slot.audio.name) : slot.pluginId;
            if (audio ? fx.audio(slot.id) != nullptr : fx.video(slot.id) != nullptr) {
                ++t->ok;
                const std::string fb = fx.fallback(slot.id);
                if (!fb.empty()) { ++t->fallback; t->fallbackBy[id + "  [" + fb.substr(0, 60) + "]"]++; }
            } else {
                ++t->missing;
                t->missingBy[id + "  [" + fx.error(slot.id).substr(0, 60) + "]"]++;
            }
        }
    });
}

void accounting(const std::string& proj, const Project& p, const veg::ChunkTree& tree)
{
    std::set<std::string> builtins;
    for (const auto& b : ofx::builtinInfos()) builtins.insert(b.id);
    // the file
    forChunks(tree.root, "", [&](const std::string& path, const veg::Chunk& c) {
        const bool inEvent = path.find("/[Event]/") != std::string::npos;
        if (endsWith(path, "/[EventVideoFx]/FxRecord") || endsWith(path, "/[TrackVideoFx]/FxRecord")) {
            const char* where = inEvent ? "event" : "track";
            veg::OfxRecord r;
            if (veg::readOfxRecord(c.data, 0, r)) {
                g_ofxUsage[r.pluginId].instances++;
                g_ofxUsage[r.pluginId].projects.insert(proj);
                fact(proj, strfmt("video fx record (OpenFX) on a %s", where));
                if (r.keyframedParams) fact(proj, "video fx record with keyframed parameters");
                if (r.unreadParams) fact(proj, "video fx record with unreadable parameters", r.unreadParams);
                if (r.bypass()) fact(proj, "video fx switched off (bypass)");
            } else {
                std::string key = c.data.size() >= 0x38 && c.data.u32(0) == 0x38 ? c.data.guid(4) + " '" + c.data.text(0x38, c.data.u32(0x34)) + "'" : "(unrecognised record)";
                // what Reno does with it: the XML (CoreGraphics) and the older (DirectX-era) records become built-in effects
                std::string used;
                veg::ManagedFxRecord mr;
                veg::LegacyFxRecord lr;
                if (veg::readManagedFx(c.data, 0, mr)) used = veg::managedPluginId(mr);
                else if (veg::readLegacyFx(c.data, 0, lr)) used = veg::legacyPluginId(lr);
                key += used.empty() ? "  -> NOT USED" : "  -> " + used;
                g_nonOfx[key].instances++;
                g_nonOfx[key].projects.insert(proj);
                fact(proj, strfmt("video fx record NOT OpenFX on a %s", where));
            }
        }
        if (endsWith(path, "/EventAudioFx") || endsWith(path, "/TrackAudioFx") || endsWith(path, "/Bus")) {
            size_t at = c.data.find(veg::guidBytes(veg::G::AudioChain).data(), 16);
            if (at == std::string::npos) return;
            std::vector<veg::AudioPluginRecord> recs;
            std::string prob;
            veg::readAudioChain(c.data, at, recs, &prob);
            for (const auto& r : recs) {
                const std::string key = (r.format.empty() ? "?" : r.format) + " " + r.name + (r.state.empty() ? " [no state]" : "");
                g_audioUsage[key].instances++;
                g_audioUsage[key].projects.insert(proj);
                fact(proj, "audio plug-in entries in the file");
                if (r.bypass) fact(proj, "audio plug-in switched off (bypass)");
            }
        }
        if (endsWith(path, "/EnvelopePoints")) {
            veg::EnvelopeRecord e;
            if (veg::readEnvelope(c.data, e)) {
                const std::string key = std::string(inEvent ? "event " : "track ") + veg::envelopeName(e.type) + strfmt(" [type %u]", e.type);
                g_envFile[key].instances++;
                g_envFile[key].projects.insert(proj);
                fact(proj, "envelopes in the file");
                fact(proj, "envelope points in the file", (int)e.points.size());
            } else {
                fact(proj, "envelopes unreadable");
            }
        }
        if (endsWith(path, "/[Event]/PanCrop")) {
            veg::PanCropRecord r;
            if (veg::readPanCrop(c.data, r)) {
                fact(proj, "pan/crop records");
                if (r.keys.size() > 1) fact(proj, "pan/crop records with more than one key");
                if (r.mask) fact(proj, "pan/crop records with a mask");
            } else fact(proj, "pan/crop records unreadable");
        }
        if (endsWith(path, "/[Event]/Take")) fact(proj, "takes in the file");
        if (endsWith(path, "/[Event]/EventData")) fact(proj, "events in the file");
        if (endsWith(path, "/[Groups]/Group")) fact(proj, "group records in the file");
    });
    // the project
    std::set<uint64_t> groups;
    for (const auto& t : p.tracks) {
        fact(proj, t.type == TrackType::Video ? "video tracks" : "audio tracks");
        if (auto* to = vegOriginAs<VegTrackOrigin>(t.veg, VegOrigin::TrackK)) {
            if (!to->hiddenEvents.empty()) fact(proj, "track: events kept hidden", (int)to->hiddenEvents.size());
            for (const auto& h : to->hiddenFx) fact(proj, "track: video effects kept hidden (not OpenFX)", (int)h.chunks.size());
            for (const auto& h : to->hiddenEnvelopes) fact(proj, "track: envelopes kept hidden", (int)h.chunks.size());
        }
        for (const auto& e : t.envelopes) { g_envKept[std::string("track ") + e.kind].instances++; g_envKept[std::string("track ") + e.kind].projects.insert(proj); fact(proj, "envelopes in the project"); fact(proj, "envelope points in the project", (int)e.points.size()); }
        for (const auto& f : t.fx) fact(proj, f.kind == "audio" ? "audio plug-ins in the project" : "video fx in the project");
        if (t.mute) fact(proj, "muted tracks");
        if (t.solo) fact(proj, "soloed tracks");
        if (t.composite != "srcalpha") fact(proj, "tracks with a compositing mode other than Source Alpha");
        if (!t.motion.empty()) fact(proj, "tracks with track motion");
        if (t.compositeLevel) fact(proj, "child tracks (compositing)");
        if (t.automation) fact(proj, "tracks in automation mode");
        for (const auto& e : t.events) {
            fact(proj, "events in the project");
            if (e.group) groups.insert(e.group);
            if (auto* eo = vegOriginAs<VegEventOrigin>(e.veg, VegOrigin::EventK)) {
                for (const auto& h : eo->hiddenFx) fact(proj, "event: video effects kept hidden (not OpenFX)", (int)h.chunks.size());
                for (const auto& h : eo->hiddenEnvelopes) fact(proj, "event: envelopes kept hidden", (int)h.chunks.size());
                for (size_t i = 0; i < eo->takesOk.size(); ++i) if (!eo->takesOk[i]) fact(proj, "takes that could not be read");
                fact(proj, "takes in the project", (int)std::max<size_t>(1, e.takes.size() + 1) - 0);
            }
            for (const auto& f : e.fx) fact(proj, f.kind == "audio" ? "audio plug-ins in the project" : "video fx in the project");
            for (const auto& en : e.envelopes) { g_envKept[std::string("event ") + en.kind].instances++; g_envKept[std::string("event ") + en.kind].projects.insert(proj); fact(proj, "envelopes in the project"); fact(proj, "envelope points in the project", (int)en.points.size()); }
            fact(proj, "pan/crop keys in the project", (int)e.panCrop.size());
            if (!e.mask.empty() || e.maskOn) fact(proj, "events with a mask");
            if (!e.transition.kind.empty()) fact(proj, "events with a transition");
            if (e.reverse) fact(proj, "reversed events");
            if (e.rate != 1.0) fact(proj, "events with a rate other than 1");
            if (e.pitch != 0) fact(proj, "events with a pitch change");
            if (e.mute) fact(proj, "muted events");
            if (e.locked) fact(proj, "locked events");
            if (e.fadeIn > 0 || e.fadeOut > 0) fact(proj, "events with a fade");
            if (e.normalize) fact(proj, "normalized events");
            if (e.invertPhase) fact(proj, "events with inverted phase");
            if (e.channels != "both") fact(proj, "audio events with a channel setting");
            if (e.stretchMethod != "elastique") fact(proj, "audio events with a non-default stretch method");
            if (!e.name.empty()) fact(proj, "named events");
        }
    }
    fact(proj, "event groups (distinct group ids)", (int)groups.size());
    fact(proj, "markers in the project", (int)p.markers.size());
    fact(proj, "buses in the project", (int)p.buses.size());
    fact(proj, "media items in the project", (int)p.media.size());
    for (const auto& m : p.media) {
        if (m.generated) fact(proj, "generated media");
        if (m.subclipLength > 0) fact(proj, "subclips");
        if (m.offline) fact(proj, "offline media");
    }
    for (const auto& f : p.videoOut) { (void)f; fact(proj, "video output fx"); }
    for (const auto& f : p.audioOut) { (void)f; fact(proj, "master fx"); }
    for (const auto& b : p.buses) for (const auto& f : b.fx) { (void)f; fact(proj, "bus fx"); }
    (void)builtins;
}


// ------------------------------------------------------------------------------------------ classification
// What the never-read bytes of each kind are. Each 4-byte word (not wholly read in some instance) is
//   zero      0 in every instance: reserved or unused
//   constant  the same non-zero value in every instance: a format constant, version or default
//   copy      always equal to another word of the same instance that the reader does read (or one at +0x4 away:
//             the other half of a 64-bit number): derived data
//   few       2..8 different values: a flag or a setting (the interesting ones)
//   many      more values than that: an id, counter, time, size, hash or uninitialised memory
void classify()
{
    std::printf("\n== classification of the unread words (per chunk kind: words by class; the 'few' words are listed)\n");
    for (const auto& name : g_kindOrder) {
        const Kind& k = g_kinds[name];
        if (k.samples.empty()) continue;
        size_t maxLen = 0;
        for (const auto& sm : k.samples) maxLen = std::max(maxLen, sm.data.size());
        int zero = 0, constant = 0, copy = 0, few = 0, many = 0;
        std::vector<std::string> fewLines, copyLines, manyLines;
        auto word = [](const Kind::Sample& sm, size_t w) { uint32_t v; std::memcpy(&v, sm.data.data() + w, 4); return v; };
        auto fullyRead = [](const Kind::Sample& sm, size_t w) { return sm.read[w] && sm.read[w + 1] && sm.read[w + 2] && sm.read[w + 3]; };
        for (size_t w = 0; w + 4 <= maxLen; w += 4) {
            std::map<uint32_t, int> vals;
            int inst = 0, unreadInst = 0;
            for (const auto& sm : k.samples) {
                if (w + 4 > sm.data.size()) continue;
                ++inst;
                if (fullyRead(sm, w)) continue;
                ++unreadInst;
                vals[word(sm, w)]++;
            }
            if (!unreadInst) continue;
            if (vals.size() == 1 && vals.begin()->first == 0) { ++zero; continue; }
            if (vals.size() == 1) { ++constant; continue; }
            // a copy of a word that is read (same offset in every instance)?
            int copyOf = -1;
            for (size_t o = 0; o + 4 <= maxLen && copyOf < 0; o += 4) {
                if (o == w) continue;
                int eq = 0, tot = 0;
                for (const auto& sm : k.samples) {
                    if (w + 4 > sm.data.size() || o + 4 > sm.data.size() || fullyRead(sm, w) || !fullyRead(sm, o)) continue;
                    ++tot;
                    eq += word(sm, w) == word(sm, o);
                }
                if (tot >= 3 && eq == tot) copyOf = (int)o;
            }
            if (copyOf >= 0) { ++copy; copyLines.push_back(strfmt("+0x%zx=+0x%x", w, copyOf)); continue; }
            if (vals.size() <= 8) {
                ++few;
                fewLines.push_back(strfmt("   +0x%zx: %d/%d unread; %s", w, unreadInst, inst, valuesText(vals).c_str()));
            } else {
                ++many;
                manyLines.push_back(strfmt("+0x%zx(%zu)", w, vals.size()));
            }
        }
        std::printf("%s  (n=%zu): zero %d, constant %d, copy %d, few %d, many %d\n", name.c_str(), k.samples.size(), zero, constant, copy, few, many);
        if (!copyLines.empty()) { std::printf("   copies:"); for (size_t i = 0; i < copyLines.size() && i < 12; ++i) std::printf(" %s", copyLines[i].c_str()); std::printf("\n"); }
        if (!manyLines.empty()) { std::printf("   many (distinct values):"); for (size_t i = 0; i < manyLines.size() && i < 40; ++i) std::printf(" %s", manyLines[i].c_str()); std::printf("%s\n", manyLines.size() > 40 ? " ..." : ""); }
        for (size_t i = 0; i < fewLines.size() && i < 40; ++i) std::printf("%s\n", fewLines[i].c_str());
    }
}

}  // namespace

int main(int argc, char** argv)
{
    bool words = false, counts = false, census = false;
    std::vector<std::string> ofxFiles;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--words") words = true;
        else if (a == "--counts") counts = true;
        else if (a == "--fx") census = true;
        else if (a == "--ofx" && i + 1 < argc) ofxFiles.push_back(argv[++i]);
        else if (std::filesystem::is_directory(a)) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(a))
                if (e.is_regular_file() && e.path().extension() == ".veg") files.push_back(e.path().string());
        } else files.push_back(a);
    }
    if (files.empty()) {
        if (testDataMissing(testDataDir("RENO_SAMPLES"))) return 0;
        for (const auto& e : std::filesystem::recursive_directory_iterator(testDataDir("RENO_SAMPLES")))
            if (e.is_regular_file() && e.path().extension() == ".veg") files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());

    if (!ofxFiles.empty()) {
        std::vector<std::string> dirs;
        for (const auto& f : ofxFiles) dirs.push_back(pathDir(f));
        Catalog::get().scanInProcess(dirs, std::vector<std::string>());
    }
    FxCensus allFx;
    std::map<std::string, FxCensus> perFx;
    veg::g_vegTrace = traceRead;
    std::vector<std::pair<std::string, Counts>> perProject;
    std::map<std::string, int> noteCounts;
    for (const auto& f : files) {
        g_reads.clear();
        Project p;
        const VegasImportResult r = importVegasProject(f, p);
        if (!r.ok) {
            std::printf("FAILED %s: %s\n", f.c_str(), r.error.c_str());
            continue;
        }
        auto po = std::dynamic_pointer_cast<const VegProjectOrigin>(p.veg);
        if (!po || !po->file) { std::printf("no origin for %s\n", f.c_str()); continue; }
        const veg::ChunkTree& tree = po->file->tree;
        const uint8_t* base = (const uint8_t*)po->file->bytes.data();
        std::vector<char> readMap(po->file->bytes.size(), 0);
        for (const auto& rd : g_reads) {
            if (rd.first < base || rd.first + rd.second > base + readMap.size()) continue;
            std::memset(&readMap[(size_t)(rd.first - base)], 1, rd.second);
        }
        const std::string proj = std::filesystem::path(f).filename().string();
        walk(tree.root, "", readMap, base, proj);
        g_reads.clear();
        accounting(proj, p, tree);
        if (census) { veg::g_vegTrace = nullptr; fxCensus(proj, p, allFx, perFx); veg::g_vegTrace = traceRead; }

        Counts n;
        count(tree.root, n);
        n.media = (int)po->media.size();
        for (const auto& m : p.media) n.mediaProject += m.generated || !m.path.empty() ? 1 : 0;
        for (const auto& t : p.tracks) {
            for (const auto& e : t.events) {
                ++n.eventsInProject;
                n.takesInProject += (int)std::max<size_t>(1, e.takes.size());
                n.panCropKeys += (int)e.panCrop.size();
            }
            if (auto* to = vegOriginAs<VegTrackOrigin>(t.veg, VegOrigin::TrackK)) {
                for (const auto& h : to->hiddenEvents) n.hiddenEvents += (int)h.chunks.size();
                for (const auto& h : to->hiddenFx) n.hiddenFx += (int)h.chunks.size();
                for (const auto& h : to->hiddenEnvelopes) n.hiddenEnv += (int)h.chunks.size();
            }
        }
        p.forEachFx([&](const FxSlot&) { ++n.fxInProject; });
        n.notes = (int)r.notes.size();
        for (const auto& s : r.notes) {
            std::string k = s;
            for (auto& ch : k) if (ch >= '0' && ch <= '9') ch = '#';
            noteCounts[k.substr(0, 150)]++;
        }
        perProject.push_back({proj, n});
    }
    veg::g_vegTrace = nullptr;

    if (counts) {
        std::printf("\n== per project: in the file / in the project\n");
        std::printf("%-52s %6s %6s %6s %6s %6s %6s %6s %6s %5s %5s\n", "project", "tracks", "events", "evtP", "takes", "takeP", "fxRec", "fxP", "envF",
                    "hidEv", "notes");
        for (const auto& pp : perProject)
            std::printf("%-52s %6d %6d %6d %6d %6d %6d %6d %6d %5d %5d\n", pp.first.c_str(), pp.second.tracks, pp.second.events, pp.second.eventsInProject,
                        pp.second.takes, pp.second.takesInProject, pp.second.fxChunks, pp.second.fxInProject, pp.second.envelopesInFile,
                        pp.second.hiddenEvents, pp.second.notes);
    }

    std::printf("\n== chunk kinds over %zu projects (count, projects, sizes, bytes unread of total)\n", perProject.size());
    for (const auto& name : g_kindOrder) {
        const Kind& k = g_kinds[name];
        std::string sizes;
        if (k.sizes.size() <= 6) {
            for (size_t s : k.sizes) sizes += strfmt("%zu ", s);
        } else {
            sizes = strfmt("%zu..%zu (%zu distinct)", *k.sizes.begin(), *k.sizes.rbegin(), k.sizes.size());
        }
        std::printf("%-70s n=%-5d proj=%-2zu size %s  unread %zu of %zu bytes (%.1f%%), %d of %d fully read\n", name.c_str(), k.count, k.projects.size(),
                    sizes.c_str(), k.unreadBytes, k.bytes, k.bytes ? 100.0 * (double)k.unreadBytes / (double)k.bytes : 0.0, k.fullyRead, k.count);
    }

    std::printf("\n== unread words (offset in the chunk: instances with it unread / instances big enough; values)\n");
    for (const auto& name : g_kindOrder) {
        const Kind& k = g_kinds[name];
        std::vector<std::string> lines;
        size_t runStart = (size_t)-1, runEnd = 0;
        int runN = 0;
        auto flushRun = [&]() {
            if (runStart == (size_t)-1) return;
            lines.push_back(strfmt("   +0x%zx..0x%zx: %zu words unread, all zero (in every instance)", runStart, runEnd + 3, (runEnd - runStart) / 4 + 1));
            runStart = (size_t)-1;
            runN = 0;
        };
        for (const auto& w : k.words) {
            const WordInfo& wi = w.second;
            if (!wi.unread) { flushRun(); continue; }
            const bool allZero = wi.values.size() == 1 && wi.values.begin()->first == 0;
            if (allZero && !words) {
                if (runStart == (size_t)-1) runStart = w.first;
                runEnd = w.first;
                ++runN;
                continue;
            }
            flushRun();
            lines.push_back(strfmt("   +0x%zx: %d/%d unread%s, %zu project(s) non-zero; %s", w.first, wi.unread, wi.seen,
                                   wi.partial ? " (some bytes)" : "", wi.projects.size(), valuesText(wi.values).c_str()));
        }
        flushRun();
        if (lines.empty()) continue;
        std::printf("%s\n", name.c_str());
        // too many lines of a big variable-size chunk are noise: keep the first 60
        size_t shown = 0;
        for (const auto& l : lines) {
            if (shown++ >= (words ? 100000u : 70u)) { std::printf("   ... (%zu more lines)\n", lines.size() - 70); break; }
            std::printf("%s\n", l.c_str());
        }
    }


    classify();

    // ------------------------------------------------------------------ accounting
    std::printf("\n== counted features (instances over all projects; in how many projects)\n");
    for (const auto& f : g_flags) std::printf("%-62s %8d  in %2zu projects\n", f.first.c_str(), f.second.instances, f.second.projects.size());
    std::set<std::string> builtinIds;
    for (const auto& b : ofx::builtinInfos()) builtinIds.insert(b.id);
    std::printf("\n== OpenFX effects used (instances, projects, whether Reno has a built-in of that id)\n");
    for (const auto& u : g_ofxUsage)
        std::printf("%-58s %6d  %2zu proj  %s\n", u.first.c_str(), u.second.instances, u.second.projects.size(), builtinIds.count(u.first) ? "built-in" : "NO BUILT-IN");
    std::printf("\n== video effects that are not OpenFX (guid + preset -> the built-in Reno runs for it)\n");
    for (const auto& u : g_nonOfx) std::printf("%-80s %6d  %2zu proj\n", u.first.c_str(), u.second.instances, u.second.projects.size());
    std::printf("\n== audio plug-ins used\n");
    for (const auto& u : g_audioUsage) std::printf("%-70s %6d  %2zu proj\n", u.first.c_str(), u.second.instances, u.second.projects.size());
    std::printf("\n== envelopes: in the file / kept as envelopes of the project (by kind)\n");
    for (const auto& u : g_envFile) std::printf("file    %-50s %6d  %2zu proj\n", u.first.c_str(), u.second.instances, u.second.projects.size());
    for (const auto& u : g_envKept) std::printf("project %-50s %6d  %2zu proj\n", u.first.c_str(), u.second.instances, u.second.projects.size());
    std::printf("\n== per project facts\n");
    for (const auto& pp : g_projFacts) {
        std::printf("%s\n", pp.first.c_str());
        for (const auto& f : pp.second) if (f.second) std::printf("    %-60s %d\n", f.first.c_str(), f.second);
    }
    if (census) {
        std::printf("\n== effect instances on this machine (render level): %d slots, %d have an instance (%d of them on a built-in because the plug-in file is missing), %d have none\n",
                    allFx.slots, allFx.ok, allFx.fallback, allFx.missing);
        for (const auto& m : allFx.missingBy) std::printf("   none : %-100s %5d\n", m.first.c_str(), m.second);
        for (const auto& m : allFx.fallbackBy) std::printf("   built-in instead: %-100s %5d\n", m.first.c_str(), m.second);
        std::printf("   per project (slots / instance / none):");
        for (const auto& pp : perFx) std::printf("\n      %-52s %5d %5d %5d", pp.first.c_str(), pp.second.slots, pp.second.ok, pp.second.missing);
        std::printf("\n");
    }

    std::printf("\n== importer notes over all projects (numbers masked)\n");
    for (const auto& n : noteCounts) std::printf("%4d  %s\n", n.second, n.first.c_str());
    return 0;
}
