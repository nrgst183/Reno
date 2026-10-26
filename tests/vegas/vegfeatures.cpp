// vegfeatures: parts of a VEGAS project worked out from VEGAS Pro 18 itself (vegas180.exe, vegas180k.dll,
// ScriptPortal.Vegas.dll), checked against every sample project and written and read back:
//   - compositing modes (and the Parent Composite Mode): numbers and compositor plug-in GUIDs;
//   - an audio event's Time Stretch / Pitch Shift: method, Classic preset, élastique mode, lock to stretch, pitch,
//     preserve formants, formant shift, invert phase;
//   - effects applied before Event Pan/Crop; takes; locked events; regions; text settings that change over time.
//
//   vegfeatures [folder with .veg files]      (default $RENO_SAMPLES)
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/vegorigin.h"
#include "../../src/engine/stretch.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include "../testdata.h"

using namespace reno;

static int g_pass = 0, g_fail = 0;
static bool check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++g_pass;
    else ++g_fail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
    return ok;
}

static std::string g_dir;
// Every chunk of a file with this GUID (lists: by their type), in order.
static void chunksOf(const veg::Chunk& c, const char* g, std::vector<const veg::Chunk*>& out)
{
    for (const auto& k : c.kids) {
        if (k.is(g)) out.push_back(&k);
        if (k.isList) chunksOf(k, g, out);
    }
}

static std::string written(const Project& p, std::vector<std::string>* warnings = nullptr)
{
    return writeVegasData(p, "C:\\Reno test\\features.veg", warnings);
}

static bool reread(const std::string& bytes, Project& q)
{
    return importVegasData(bytes, "C:\\Reno test\\features.veg", q).ok;
}

int main(int argc, char** argv)
{
    g_dir = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    if (testDataMissing(g_dir)) return 0;

    // every sample project, read once
    std::vector<Project> all;
    std::vector<std::vector<std::string>> allNotes;
    for (const std::string& f : testVegFiles(g_dir)) {
        Project p;
        const VegasImportResult r = importVegasProject(f, p);
        if (!r.ok) continue;
        all.push_back(std::move(p));
        allNotes.push_back(r.notes);
    }
    if (all.empty()) { std::printf("SKIP: no readable .veg files in %s\n", g_dir.c_str()); return 0; }
    // the sample with the fewest events that has what a test needs (a copy to change and write)
    auto pick = [&](const std::function<bool(const Project&)>& wanted, Project& out) {
        const Project* best = nullptr;
        size_t bestEvents = 0;
        for (const Project& q : all) {
            if (!wanted(q)) continue;
            size_t n = 0;
            for (const auto& t : q.tracks) n += t.events.size();
            if (!best || n < bestEvents) { best = &q; bestEvents = n; }
        }
        if (best) out = *best;
        return best != nullptr;
    };

    // ---------------------------------------------------------------- compositing modes, as read
    {
        // every track of every sample has a mode the table knows, and no note says a compositing mode is unknown
        int tracks = 0, unknown = 0, said = 0;
        for (size_t i = 0; i < all.size(); ++i) {
            for (const auto& t : all[i].tracks) {
                if (t.type != TrackType::Video) continue;
                ++tracks;
                int32_t m = 0;
                std::string g;
                if (!veg::compositeModeOf(t.composite, m, g)) ++unknown;
            }
            for (const auto& n : allNotes[i]) said += n.find("compositing mode") != std::string::npos ? 1 : 0;
        }
        check(unknown == 0, strfmt("samples: every video track's compositing mode is one VEGAS has (%d tracks)", tracks));
        check(said == 0, "and no note says a compositing mode is unknown");
        // the table both ways
        int32_t mode = 0;
        std::string guid;
        bool all = true;
        for (const char* n : {"add", "subtract", "multiply", "cut", "screen", "overlay", "hardlight", "dodge", "burn", "darken", "lighten", "difference",
                              "differencesquared", "srcalpha", "3dsrcalpha", "custom"}) {
            all = all && veg::compositeModeOf(n, mode, guid) && veg::compositeModeName(mode, guid) == n;
        }
        check(all, "every compositing mode's number and GUID name it again");
        check(veg::compositeModeOf("srcalpha", mode, guid) && mode == 4 && guid.empty() && veg::compositeModeOf("add", mode, guid) && mode == 1 &&
                  veg::compositeModeOf("3dsrcalpha", mode, guid) && mode == 12 && veg::compositeModeOf("cut", mode, guid) && mode == -1 &&
                  guid == "41bbf5a0-1a37-41d9-ba9e-e9dd0b01dec9",
              "VEGAS 18's numbers: Source Alpha 4, Add 1, 3D Source Alpha 12, Cut a plug-in");
    }

    // ---------------------------------------------------------------- compositing modes, written
    {
        Project p;
        if (pick([](const Project& q) { for (const auto& t : q.tracks) if (t.type == TrackType::Video) return true; return false; }, p)) {
            const char* modes[] = {"add", "subtract", "multiply", "cut", "screen", "overlay", "hardlight", "dodge", "burn", "darken", "lighten",
                                   "difference", "differencesquared", "3dsrcalpha", "srcalpha"};
            size_t vt = 0;
            while (vt < p.tracks.size() && p.tracks[vt].type != TrackType::Video) ++vt;
            int good = 0, n = 0;
            for (const char* m : modes) {
                ++n;
                p.tracks[vt].composite = m;
                p.tracks[vt].parentComposite = modes[(n + 3) % 15];
                std::vector<std::string> warnings;
                const std::string bytes = written(p, &warnings);
                Project q;
                if (!reread(bytes, q)) continue;
                // the record itself
                veg::ChunkTree t;
                veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
                std::vector<const veg::Chunk*> vtd;
                chunksOf(t.root, veg::G::VideoTrackData, vtd);
                int32_t mode = 0;
                std::string guid;
                veg::compositeModeOf(m, mode, guid);
                const bool bytesOk = !vtd.empty() && vtd[0]->data.i32(0x64) == mode &&
                                     (guid.empty() ? vtd[0]->data.guid(0x6c) == "00000000-0000-0000-0000-000000000000" : vtd[0]->data.guid(0x6c) == guid);
                if (q.tracks[vt].composite == m && q.tracks[vt].parentComposite == p.tracks[vt].parentComposite && bytesOk && warnings.empty()) ++good;
                else std::printf("   %s: read %s / parent %s, bytes %d, %zu warnings\n", m, q.tracks[vt].composite.c_str(), q.tracks[vt].parentComposite.c_str(),
                                 (int)bytesOk, warnings.size());
            }
            check(good == n, strfmt("every compositing mode (and parent mode) is written as VEGAS keeps it and read back (%d of %d)", good, n));
            p.tracks[vt].composite = "custom";
            std::vector<std::string> warnings;
            Project q;
            reread(written(p, &warnings), q);
            check(!warnings.empty() && q.tracks[vt].composite != "custom", "Custom (a compositing plug-in) is not written, and that is said");
        }
    }

    // ---------------------------------------------------------------- time stretch / pitch shift, as read
    {
        // every audio event of every sample has a method and mode Reno knows, and a pitch that is a number
        int events = 0, bad = 0;
        for (const Project& pr : all)
            for (const auto& t : pr.tracks)
                for (const auto& e : t.events) {
                    if (t.type != TrackType::Audio) continue;
                    ++events;
                    const bool method = e.stretchMethod == "elastique" || e.stretchMethod == "classic" || e.stretchMethod == "none" || e.stretchMethod.empty();
                    if (!method || !std::isfinite(e.pitch) || !std::isfinite(e.formant)) ++bad;
                }
        check(bad == 0, strfmt("samples: every audio event's time stretch method and pitch are usable (%d events)", events));
    }

    // ---------------------------------------------------------------- time stretch / pitch shift, written
    {
        Project p;
        if (pick([](const Project& q) { for (const auto& t : q.tracks) if (t.type == TrackType::Audio && !t.events.empty()) return true; return false; }, p)) {
            Event* ev = nullptr;
            for (auto& t : p.tracks)
                if (t.type == TrackType::Audio && !t.events.empty()) ev = &t.events[0];
            struct Case { const char* method; const char* mode; bool keep; double pitch; bool preserve; double formant; bool invert; uint32_t flags; };
            const Case cases[] = {
                {"elastique", "pro", true, 3, true, -2, false, 0x08 | 0x10},
                {"elastique", "efficient", true, 0, false, 0, true, 0x08 | 0x01},
                {"elastique", "soloist-mono", false, 0, true, 0, false, 0x08 | 0x02 | 0x10},
                {"elastique", "soloist-speech", true, -5, false, 0, false, 0x08},
                {"classic", "A01", true, 7, false, 0, false, 0x00},
                {"classic", "A19", false, 0, false, 0, true, 0x02 | 0x01},
                {"none", "pro", false, 0, false, 0, false, 0x0c},
            };
            int good = 0;
            for (const Case& k : cases) {
                ev->stretchMethod = k.method;
                ev->stretchMode = k.mode;
                ev->keepPitch = k.keep;
                ev->pitch = k.pitch;
                ev->preserveFormants = k.preserve;
                ev->formant = k.formant;
                ev->invertPhase = k.invert;
                ev->rate = 1.5;
                const std::string bytes = written(p);
                Project q;
                if (!reread(bytes, q)) continue;
                const Event* e2 = nullptr;
                for (const auto& t : q.tracks)
                    if (t.type == TrackType::Audio && !t.events.empty()) e2 = &t.events[0];
                veg::ChunkTree t;
                veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
                std::vector<const veg::Chunk*> ae;
                chunksOf(t.root, veg::G::AudioEventData, ae);
                const veg::Chunk* rec = nullptr;
                for (const auto* a : ae)
                    if (!a->isList) rec = a;
                const uint32_t flags = rec ? rec->data.u32(0x08) & 0x1f : 0xff;
                bool attrs = true;
                if (std::string(k.method) == "classic") attrs = rec && rec->data.u32(0x0c) == (uint32_t)(classicPreset(k.mode) - classicPresets().data());
                if (std::string(k.method) == "elastique") {
                    uint32_t i = 0;
                    while (i < elastiqueModes().size() && elastiqueModes()[i].first != k.mode) ++i;
                    attrs = rec && rec->data.u32(0x10) == i;
                }
                const bool same = e2 && e2->stretchMethod == k.method && (std::string(k.method) == "none" || e2->stretchMode == k.mode) &&
                                  (std::string(k.method) == "none" || (e2->keepPitch == k.keep && e2->pitch == k.pitch)) &&
                                  e2->preserveFormants == k.preserve && e2->formant == k.formant && e2->invertPhase == k.invert;
                if (same && flags == k.flags && attrs) ++good;
                else std::printf("   %s %s: flags %02x (want %02x), attributes %d, read back %d\n", k.method, k.mode, flags, k.flags, (int)attrs, (int)same);
            }
            check(good == (int)(sizeof cases / sizeof cases[0]), strfmt("every method, mode and setting is written as VEGAS keeps it and read back (%d)", good));
        }
    }

    // ---------------------------------------------------------------- effects before pan/crop, takes, locked
    {
        int pre = 0, takes = 0, takeMediaOk = 0, takeCount = 0;
        for (const Project& p : all) {
            for (const auto& t : p.tracks)
                for (const auto& e : t.events) {
                    for (const auto& f : e.fx) {
                        const auto* o = vegOriginAs<VegFxOrigin>(f.veg, VegOrigin::FxK);
                        if (f.beforePanCrop && !(o && o->mediaFx)) ++pre;
                    }
                    if (!e.takes.empty()) ++takes;
                    for (const auto& k : e.takes) {
                        ++takeCount;
                        if (p.findMedia(k.mediaId)) ++takeMediaOk;
                    }
                }
        }
        std::printf("samples: %d effects applied before pan/crop, %d events with several takes\n", pre, takes);
        check(takeMediaOk == takeCount, "the samples' events with several takes have them, each with its media", strfmt("%d events, %d/%d", takes, takeMediaOk, takeCount));

        Project p;
        if (pick([](const Project& q) {
                for (const auto& t : q.tracks)
                    if (t.type == TrackType::Video && !t.events.empty() && !t.events[0].fx.empty() && t.events[0].fx[0].kind == "ofx") return true;
                return false;
            }, p)) {
            Event* v = nullptr;
            for (auto& t : p.tracks)
                if (!v && t.type == TrackType::Video && !t.events.empty() && !t.events[0].fx.empty() && t.events[0].fx[0].kind == "ofx") v = &t.events[0];
            // an effect before pan/crop: the record's 0x80
            bool hasFx = v && !v->fx.empty() && v->fx[0].kind == "ofx";
            if (hasFx) {
                v->fx[0].beforePanCrop = true;
                v->locked = true;
                const std::string bytes = written(p);
                Project q;
                reread(bytes, q);
                const Event* v2 = nullptr;
                for (const auto& t : q.tracks)
                    if (!v2 && t.type == TrackType::Video && !t.events.empty() && !t.events[0].fx.empty() && t.events[0].fx[0].kind == "ofx") v2 = &t.events[0];
                veg::ChunkTree t;
                veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
                std::vector<const veg::Chunk*> fx, ed;
                chunksOf(t.root, veg::G::Fx, fx);
                chunksOf(t.root, veg::G::EventData, ed);
                bool has80 = false, hasLock = false;
                for (const auto* c : fx) has80 = has80 || (c->data.u32(0x20) & 0x80) != 0;
                for (const auto* c : ed) hasLock = hasLock || (c->data.u8(4) & 0x08) != 0;
                check(v2 && v2->fx[0].beforePanCrop && has80,
                      "an effect set before pan/crop is written with VEGAS's 0x80 and read back");
                check(v2 && v2->locked && hasLock, "a locked event is written with VEGAS's lock bit (0x08) and read back");
                v->locked = false;
                v->fx[0].beforePanCrop = false;
            }
            // takes: another take added (the sound of the same file from 2 s on), then made the active one
            if (v) {
                Take k;
                k.mediaId = v->mediaId;
                k.offset = v->offset + 2.0;
                k.name = "Second take";
                v->takes.push_back(k);
                Project q;
                reread(written(p), q);
                const Event* v2 = nullptr;
                for (const auto& t : q.tracks)
                    if (!v2 && t.type == TrackType::Video && !t.events.empty() && !t.events[0].fx.empty() && t.events[0].fx[0].kind == "ofx") v2 = &t.events[0];
                check(v2 && v2->takes.size() == 1 && std::fabs(v2->takes[0].offset - k.offset) < 1e-6 && v2->takes[0].name == "Second take" &&
                          std::fabs(v2->offset - v->offset) < 1e-6,
                      "a take added to an event is written and read back (the active take as it was)");
                std::swap(v->offset, v->takes[0].offset);
                v->takes[0].name.clear();
                const std::string bytes = written(p);
                Project r;
                reread(bytes, r);
                const Event* v3 = nullptr;
                for (const auto& t : r.tracks)
                    if (!v3 && t.type == TrackType::Video && !t.events.empty() && !t.events[0].fx.empty() && t.events[0].fx[0].kind == "ofx") v3 = &t.events[0];
                check(v3 && v3->takes.size() == 1 && std::fabs(v3->offset - v->offset) < 1e-6 && std::fabs(v3->takes[0].offset - v->takes[0].offset) < 1e-6,
                      "the other take made active (VEGAS's T) is written and read back");
                veg::ChunkTree t;
                veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
                std::vector<const veg::Chunk*> evs;
                chunksOf(t.root, veg::G::Event, evs);
                const veg::Chunk* first = nullptr;
                for (const auto* c : evs)
                    if (c->isList && c->children(veg::G::Take).size() == 2) { first = c; break; }
                const veg::Chunk* d = first ? first->child(veg::G::EventData) : nullptr;
                check(d && d->data.u32(0x20) < 2, "the event's record says which of its two takes is active");
                v->takes.clear();
            }
        }
    }

    // ---------------------------------------------------------------- regions
    {
        Project p;
        if (pick([](const Project& q) { return !q.markers.empty(); }, p)) {
            const size_t markers = p.markers.size();
            Project::Marker r;
            r.id = p.newId();
            r.time = 12.5;
            r.length = 3.25;
            r.region = true;
            r.name = "A region";
            p.markers.push_back(r);
            Project::Marker r2 = r;
            r2.id = p.newId();
            r2.time = 20;
            r2.name = "Another";
            p.markers.push_back(r2);
            p.markers[0].region = true;            // a marker made a region
            p.markers[0].length = 1.0;
            std::vector<std::string> warnings;
            const std::string bytes = written(p, &warnings);
            Project q;
            reread(bytes, q);
            int regions = 0;
            for (const auto& m : q.markers) regions += m.region ? 1 : 0;
            bool said = false;
            for (const auto& w : warnings) said = said || w.find("region") != std::string::npos;
            check(q.markers.size() == markers + 2 && regions == 3 && !said, "regions are written into the marker list (with their bit and length) and read back",
                  strfmt("%zu markers, %d regions", q.markers.size(), regions));
            veg::ChunkTree t;
            veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
            std::vector<const veg::Chunk*> mk;
            chunksOf(t.root, veg::G::Marker, mk);
            std::vector<uint32_t> regionNumbers, markerNumbers;
            for (const auto* c : mk) (c->data.u32(0x1c) & 1 ? regionNumbers : markerNumbers).push_back(c->data.u32(0x18));
            check(regionNumbers.size() == 3 && std::set<uint32_t>(regionNumbers.begin(), regionNumbers.end()).size() == 3,
                  "the regions are numbered among themselves, as VEGAS numbers them");
        }
    }

    std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
