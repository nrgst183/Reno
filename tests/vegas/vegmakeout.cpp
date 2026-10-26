// vegmakeout: makes the .veg files for trying Reno's writer in VEGAS Pro 18 (see the README it writes next to them).
//
//   vegmakeout <output folder> [folder with the sample .veg files]
//
// Each file is a sample project with edits of one kind made in Reno, written as Reno writes it; plus one project made in
// Reno from nothing. The sample for each kind of edit is the smallest one under the folder that has what the edit needs
// (two tracks of events, buses, markers, ...); a kind no sample can serve is skipped. Every file is read back and must hold
// what was saved.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include "../testdata.h"

using namespace reno;

static int g_fail = 0;

static std::string headerPath(const std::string& bytes)
{
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    for (const auto& k : t.root.kids)
        if (!k.isList && k.id == veg::G::Header) return k.data.textZ(k.data.u32(0));
    return std::string();
}

// The Windows folder of a project, as its own header says.
static std::string vegasFolder(const std::string& samplePath)
{
    std::string raw;
    readFile(samplePath, raw);
    const std::string h = headerPath(raw);
    return h.substr(0, h.find_last_of('\\') + 1);
}

struct Made {
    std::string file, from, what, check;
    std::vector<std::string> warnings;
};
static std::vector<Made> g_made;

static void write(const std::string& outDir, const Project& p, const std::string& vegasDir, const std::string& name, const std::string& from,
                  const std::string& what, const std::string& look)
{
    Made m;
    m.file = name;
    m.from = from;
    m.what = what;
    m.check = look;
    const std::string bytes = writeVegasData(p, vegasDir + name, &m.warnings);
    writeFile(outDir + "/" + name, bytes);
    Project q;
    const VegasImportResult r = importVegasData(bytes, outDir + "/" + name, q);
    size_t ev = 0, eq = 0;
    for (const auto& t : p.tracks) ev += t.events.size();
    for (const auto& t : q.tracks) eq += t.events.size();
    const bool ok = r.ok && q.tracks.size() == p.tracks.size() && ev == eq && q.markers.size() == p.markers.size();
    std::printf("%s %s: %zu bytes, %zu tracks, %zu events, %zu warnings\n", ok ? "ok  " : "FAIL", name.c_str(), bytes.size(), q.tracks.size(), eq,
                m.warnings.size());
    for (const auto& w : m.warnings) std::printf("      warning: %s\n", w.c_str());
    if (!ok) ++g_fail;
    g_made.push_back(m);
}

static const FxSlot* findFx(const Project& p, const std::string& plugin)
{
    const FxSlot* found = nullptr;
    p.forEachFx([&](const FxSlot& f) {
        if (!found && (f.pluginId == plugin || (f.kind == "audio" && f.name == plugin))) found = &f;
    });
    return found;
}

static Envelope env(const std::string& kind, std::initializer_list<EnvPoint> pts, uint64_t target = 0)
{
    Envelope e;
    e.kind = kind;
    e.target = target;
    e.points = pts;
    return e;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: vegmakeout <output folder> [samples folder]\n"); return 2; }
    const std::string out = argv[1];
    const std::string dir = argc > 2 ? argv[2] : testDataDir("RENO_SAMPLES");
    if (testDataMissing(dir)) return 0;
    // every sample, read once; each edit takes the smallest sample that suits it
    struct Sample { std::string path; Project p; size_t events = 0; };
    std::vector<Sample> samples;
    for (const std::string& f : testVegFiles(dir)) {
        Sample sm;
        sm.path = f;
        if (!importVegasProject(f, sm.p).ok) continue;
        for (const auto& t : sm.p.tracks) sm.events += t.events.size();
        samples.push_back(std::move(sm));
    }
    if (samples.empty()) { std::printf("SKIP: no readable .veg files in %s\n", dir.c_str()); return 0; }
    auto pick = [&](const std::function<bool(const Project&)>& wanted) -> const Sample* {
        const Sample* best = nullptr;
        for (const auto& sm : samples)
            if (wanted(sm.p) && (!best || sm.events < best->events)) best = &sm;
        return best;
    };
    // an effect of this plug-in (or audio effect of this name) from any sample
    auto anyFx = [&](const std::string& plugin) -> const FxSlot* {
        for (const auto& sm : samples)
            if (const FxSlot* f = findFx(sm.p, plugin)) return f;
        return nullptr;
    };
    auto fileOf = [](const std::string& path) { return path.substr(path.find_last_of("/\\") + 1); };
    const Sample* baseS = pick([](const Project& q) {
        return q.tracks.size() >= 2 && q.tracks[0].type == TrackType::Video && q.tracks[1].type == TrackType::Audio && !q.tracks[0].events.empty() &&
               !q.tracks[1].events.empty() && !q.media.empty() && !q.tracks[0].events[0].fx.empty();
    });
    const Sample* busesS = pick([](const Project& q) { return q.buses.size() >= 3; });
    const Sample* markersS = pick([](const Project& q) {
        bool env = false;
        for (const auto& t : q.tracks) for (const auto& e : t.envelopes) env = env || (e.kind == "volume" && e.points.size() > 4);
        return q.markers.size() >= 3 && env && !q.tracks.empty();
    });
    const Sample* eventsS = pick([](const Project& q) { return q.tracks.size() >= 2 && q.tracks[0].events.size() >= 4 && q.tracks[1].events.size() >= 4; });
    const Sample* compS = pick([](const Project& q) { int n = 0; for (const auto& t : q.tracks) n += t.type == TrackType::Video; return n >= 2; });
    const Sample* stretchS = pick([](const Project& q) { size_t n = 0; for (const auto& t : q.tracks) if (t.type == TrackType::Audio) n += t.events.size(); return n >= 3; });
    // media paths for the made-from-nothing project, and a Titles & Text generator
    std::string video, sound;
    const MediaItem* titleGen = nullptr;
    for (const auto& sm : samples)
        for (const auto& m : sm.p.media) {
            if (video.empty() && endsWithNoCase(m.path, ".mp4")) video = m.path;
            if (sound.empty() && endsWithNoCase(m.path, ".wav")) sound = m.path;
            if (!titleGen && m.generated) titleGen = &m;
        }
    const FxSlot* sharpenFx = anyFx("com.vegascreativesoftware:sharpen");
    const FxSlot* invertFx = anyFx("com.vegascreativesoftware:invert");
    const FxSlot* reverbFx = anyFx("VEGAS Reverb");
    const FxSlot* limiterFx = anyFx("eFX Limiter");
    const FxSlot* eqFx = anyFx("VEGAS Graphic EQ");
    const FxSlot* distortionFx = anyFx("VEGAS Distortion");
    std::string baseDir;
    if (baseS) baseDir = vegasFolder(baseS->path);

    // 1. unchanged
    if (baseS)
        write(out, baseS->p, baseDir, "base - reno unchanged.veg", fileOf(baseS->path), "The sample read by Reno and written back with no change.",
              "It must open exactly as the sample does (it is the sample byte for byte, apart from the project's own path in the file).");

    // 2. values changed
    if (baseS) {
        Project p = baseS->p;
        p.settings.title = "Edited in Reno";
        Track& v = p.tracks[0];
        Track& a = p.tracks[1];
        v.name = "Reno video";
        a.name = "Reno audio";
        v.height = 90;
        a.level = 0.5;          // -6 dB
        a.pan = -0.3;
        for (Track* t : {&v, &a}) {
            Event& e = t->events[0];
            e.start += 1.0;
            e.length -= 1.0;
            e.offset += 0.25;
            e.fadeIn = 0.5;
            e.fadeInCurve = CurveSlow;
            e.fadeOut = 0.3;
            e.fadeOutCurve = CurveSmooth;
        }
        a.events[0].gain = 0.5;
        v.events[0].mute = true;
        FxSlot& bc = v.events[0].fx[0];
        Json params = bc.state.get("params");
        Json np = Json::object();
        for (const auto& m : params.members()) {
            Json e = m.second;
            if (m.first == "Brightness" || m.first == "Contrast") {
                Json val = Json::array();
                val.push(m.first == "Brightness" ? 0.25 : -0.5);
                e["v"] = val;
            }
            np[m.first] = e;
        }
        bc.state["params"] = np;
        write(out, p, baseDir, "base - reno values.veg", fileOf(baseS->path),
              "Values changed: both events moved 1 s later and 1 s shorter (and 0.25 s further into the media), fades in (0.5 s, slow curve) and out "
              "(0.3 s, smooth curve), the video event muted, the sound event at half volume; the tracks renamed 'Reno video' and 'Reno audio', the "
              "video track taller, the audio track at -6 dB panned 30% left; Brightness and Contrast set to 0.25 and -0.5; the project's title "
              "(File > Properties > Summary) 'Edited in Reno'.",
              "That each of these shows in VEGAS as described (Event Properties, the fade curves, the track headers, the effect's settings).");
    }

    // 3. new tracks, events, effects, envelopes
    if (baseS) {
        Project p = baseS->p;
        const uint64_t media = p.media[0].id;
        Track v;
        v.id = p.newId();
        v.type = TrackType::Video;
        v.name = "New video track";
        v.envelopes.push_back(env("composite", {{0.0, 1.0, CurveLinear}, {1.5, 0.25, CurveSmooth}, {3.0, 1.0, CurveLinear}}));
        Track a;
        a.id = p.newId();
        a.type = TrackType::Audio;
        a.name = "New audio track";
        a.envelopes.push_back(env("volume", {{0.0, 1.0, CurveLinear}, {1.0, 0.25, CurveFast}, {2.5, 1.0, CurveSlow}}));
        a.envelopes.push_back(env("pan", {{0.0, -1.0, CurveLinear}, {3.0, 1.0, CurveLinear}}));
        const uint64_t g = p.newId();
        Event ve;
        ve.id = p.newId();
        ve.mediaId = media;
        ve.start = 0.5;
        ve.length = 3.0;
        ve.offset = 600.0;
        ve.group = g;
        ve.envelopes.push_back(env("velocity", {{0.0, 1.0, CurveLinear}, {1.5, 2.0, CurveLinear}, {3.0, 0.5, CurveLinear}}));
        PanCropKey k;
        k.cx = 0.4;
        k.cy = 0.45;
        k.w = 0.6;
        k.h = 0.6;
        ve.panCrop.push_back(k);
        k.time = 3.0;
        k.w = k.h = 0.3;
        k.rotation = 15;
        ve.panCrop.push_back(k);
        if (sharpenFx) ve.fx.push_back(*sharpenFx);
        if (invertFx) ve.fx.push_back(*invertFx);
        Event ae = ve;
        ae.id = p.newId();
        ae.fx.clear();
        ae.envelopes.clear();
        ae.panCrop.clear();
        ae.channels = "combine";
        if (reverbFx) ae.fx.push_back(*reverbFx);
        if (limiterFx) ae.fx.push_back(*limiterFx);
        v.events.push_back(ve);
        a.events.push_back(ae);
        p.tracks.push_back(v);
        p.tracks.push_back(a);
        Project::Marker m;
        m.id = p.newId();
        m.time = 1.0;
        m.name = "Made in Reno";
        p.markers.push_back(m);
        write(out, p, baseDir, "base - reno new tracks.veg", fileOf(baseS->path),
              "Two new tracks under the old ones: a video track with an event of the same video (0.5 s to 3.5 s, from 10:00 into the video) that has "
              "Sharpen and Invert (copied from other samples), a velocity envelope (100% -> 200% -> 50%) and pan/crop (zoomed in, "
              "turning 15 degrees); an audio track with the sound of the same video, grouped with the picture, its channels combined to mono, VEGAS "
              "Reverb and eFX Limiter (copied from other samples); a composite level (opacity) envelope on the new video track; volume and pan "
              "envelopes on the new audio track; a marker 'Made in Reno' at 1 s.",
              "The new tracks, events, effects (with their settings), envelopes, pan/crop and the marker; that the two new events move together "
              "(grouped). The opacity and pan envelopes' ranges were not in any sample project: check their values look right.");
    }

    // 4. buses
    if (busesS) {
        Project p = busesS->p;
        p.buses[0].level = 0.5;
        p.buses[1].name = "Bus B";
        if (!p.buses[2].fx.empty()) p.buses[2].fx.pop_back();
        if (distortionFx) p.buses[0].fx.push_back(*distortionFx);
        if (!p.audioOut.empty()) p.audioOut.erase(p.audioOut.begin());
        p.masterVolume = 0.8;
        for (auto& t : p.tracks)
            if (t.type == TrackType::Audio && t.output == 0) { t.output = p.buses[0].id; break; }
        write(out, p, vegasFolder(busesS->path), "buses - reno buses.veg", fileOf(busesS->path),
              "Buses: Bus A at -6 dB with VEGAS Distortion added to it; the last effect of Bus C (VEGAS Reverb) removed; the first effect of the "
              "master (ExpressFX Graphic EQ) removed; the master at 0.8 (-1.9 dB); the first audio track that went to the master now goes to Bus A.",
              "The Mixing Console: the bus levels, the effects on each bus and on the master, and the track's output.");
    }

    // 5. markers and envelopes
    if (markersS) {
        Project p = markersS->p;
        if (p.markers.size() >= 3) {
            p.markers[0].time += 2.0;
            p.markers[1].name = "Renamed in Reno";
            p.markers.erase(p.markers.begin() + 2);
        }
        Project::Marker m;
        m.id = p.newId();
        m.time = 5.0;
        m.name = "New marker";
        p.markers.push_back(m);
        for (auto& t : p.tracks)
            for (auto& e : t.envelopes)
                if (e.kind == "volume" && e.points.size() > 4) {
                    for (size_t i = 0; i < e.points.size(); i += 2) e.points[i].value *= 0.5;
                    e.points.erase(e.points.begin() + 1);
                    break;
                }
        p.tracks.back().mute = false;
        p.tracks.back().solo = true;
        write(out, p, vegasFolder(markersS->path), "markers - reno markers.veg", fileOf(markersS->path),
              "Markers and envelopes: the first marker 2 s later, the second renamed 'Renamed in Reno', the third deleted, a marker 'New marker' "
              "added at 5 s; the first track volume envelope with every other point at half its level and its second point deleted; the "
              "reference track (the last) unmuted and soloed.",
              "The markers, the volume envelope's points, the last track's mute and solo buttons.");
    }

    // 6. reversed events, deleted events, a split
    if (eventsS) {
        Project p = eventsS->p;
        Track& t = p.tracks[0];
        int reversed = 0;
        for (size_t i = 0; i < t.events.size() && reversed < 3; ++i)
            if (!t.events[i].reverse && t.events[i].group == 0) {
                t.events[i].reverse = true;
                ++reversed;
            }
        // every fourth event of the second track deleted
        if (p.tracks.size() > 1) {
            std::vector<Event> keep;
            for (size_t i = 0; i < p.tracks[1].events.size(); ++i)
                if (i % 4 != 3) keep.push_back(p.tracks[1].events[i]);
            p.tracks[1].events = keep;
        }
        // the first event split in two (as S does)
        if (!t.events.empty()) {
            Event& a = t.events[0];
            Event b = a;
            b.id = p.newId();
            const double half = a.length / 2;
            a.length = half;
            b.start += half;
            b.length = half;
            b.offset += half * b.rate;
            b.group = 0;
            t.events.insert(t.events.begin() + 1, b);
        }
        write(out, p, vegasFolder(eventsS->path), "events - reno reversed and split.veg", fileOf(eventsS->path),
              strfmt("Events reversed, deleted and split: %d events of the first track reversed (Reno makes VEGAS's reversed subclips for them), every "
                     "fourth event of the second track deleted, and the first event split in the middle.",
                     reversed),
              "That the reversed events play backwards from the right place, nothing else moved, and the split halves join seamlessly.");
    }

    // 7. a project made in Reno
    if (baseS && !video.empty() && !sound.empty()) {
        Project p;
        p.settings.width = 1280;
        p.settings.height = 720;
        p.settings.fpsNum = 30000;
        p.settings.fpsDen = 1001;
        p.settings.sampleRate = 48000;
        p.settings.title = "Made in Reno";
        auto add = [&](const std::string& path, bool v, bool a) {
            MediaItem m;
            m.id = p.newId();
            m.path = path;
            m.name = path.substr(path.find_last_of('\\') + 1);
            m.info.hasVideo = v;
            m.info.hasAudio = a;
            m.info.width = v ? 640 : 0;
            m.info.height = v ? 480 : 0;
            p.media.push_back(m);
            return m.id;
        };
        // media the sample projects use (a video, a sound file) and one of them as a nested project
        std::string raw;
        readFile(baseS->path, raw);
        const std::string basePath = headerPath(raw);
        const uint64_t vid = add(video, true, true), snd = add(sound, false, true), nested = add(basePath, true, true);
        MediaItem title;
        title.id = p.newId();
        title.generated = true;
        title.name = "Made in Reno";
        title.info.hasVideo = true;
        title.info.width = 1280;
        title.info.height = 720;
        for (const MediaItem* gm : {titleGen})
            if (gm) {
                const MediaItem& m = *gm;
                title.generator = m.generator;
                Json params = title.generator.state.get("params");
                Json np = Json::object();
                for (const auto& kv : params.members()) {
                    Json e = kv.second;
                    if (kv.first == "Text") {
                        std::string s = e.get("s").asString();
                        const size_t at = s.find("Spin Count: 3");
                        if (at != std::string::npos) s.replace(at, 13, "Made in Reno");
                        e["s"] = s;
                    }
                    np[kv.first] = e;
                }
                title.generator.state["params"] = np;
                break;
            }
        title.generator.id = p.newId();
        p.media.push_back(title);
        Track tt;
        tt.id = p.newId();
        tt.type = TrackType::Video;
        tt.name = "Title";
        Event te;
        te.id = p.newId();
        te.mediaId = title.id;
        te.start = 0;
        te.length = 3;
        te.fadeOut = 1;
        tt.events.push_back(te);
        Track v;
        v.id = p.newId();
        v.type = TrackType::Video;
        v.name = "Pictures";
        Track a;
        a.id = p.newId();
        a.type = TrackType::Audio;
        a.name = "Sound";
        const uint64_t g = p.newId();
        Event e1;
        e1.id = p.newId();
        e1.mediaId = vid;
        e1.start = 0;
        e1.length = 4;
        e1.offset = 10;
        e1.group = g;
        if (sharpenFx) e1.fx.push_back(*sharpenFx);
        Event e1a = e1;
        e1a.id = p.newId();
        e1a.fx.clear();
        Event e2;
        e2.id = p.newId();
        e2.mediaId = nested;
        e2.start = 4;
        e2.length = 3;
        e2.fadeIn = 0.5;
        Event e2a = e2;
        e2a.id = p.newId();
        Event s1;
        s1.id = p.newId();
        s1.mediaId = snd;
        s1.start = 7;
        s1.length = 2;
        if (eqFx) s1.fx.push_back(*eqFx);
        v.events = {e1, e2};
        a.events = {e1a, e2a, s1};
        a.envelopes.push_back(env("volume", {{0.0, 1.0, CurveLinear}, {8.0, 1.0, CurveSlow}, {9.0, 0.0, CurveLinear}}));
        p.tracks = {tt, v, a};
        Project::Marker m;
        m.id = p.newId();
        m.time = 4.0;
        m.name = "nested project starts";
        p.markers.push_back(m);
        write(out, p, baseDir, "made in Reno.veg", "(nothing: a new project)",
              "A project made in Reno from nothing, 1280x720 at 29.97 fps: a title (VEGAS Titles & Text saying 'Made in Reno', fading out), a video "
              "your projects use with Sharpen and its sound grouped with it, the first sample as a nested project (picture and sound) with a fade-in, "
              "a sound file with VEGAS Graphic EQ, a volume envelope fading the sound out at the end, and a marker at 4 s.",
              "That it opens at all (it is written from the template project, not from one of your projects), and then each part as described. "
              "It names media by the paths your projects use (" + video + ", " + sound + ").");
    }

    // 8. compositing modes
    if (compS) {
        Project p = compS->p;
        static const char* const modes[] = {"add", "subtract", "multiply", "cut", "screen", "overlay", "hardlight", "dodge", "burn", "darken",
                                            "lighten", "difference", "differencesquared", "3dsrcalpha"};
        static const char* const names[] = {"Add", "Subtract", "Multiply (Mask)", "Cut", "Screen", "Overlay", "Hard Light", "Dodge", "Burn", "Darken",
                                            "Lighten", "Difference", "Difference Squared", "3D Source Alpha"};
        std::string list;
        int k = 0;
        for (auto& t : p.tracks) {
            if (t.type != TrackType::Video || k >= 14) continue;
            t.composite = modes[k];
            t.name = std::string("Reno: ") + names[k];
            list += strfmt("%s%s", k ? ", " : "", names[k]);
            ++k;
        }
        write(out, p, vegasFolder(compS->path), "compositing - reno compositing.veg", fileOf(compS->path),
              "Compositing modes: the video tracks, from the top, set to " + list + " (each track is named after its mode). VEGAS keeps the "
              "plug-in modes (Cut, Screen, Overlay, Hard Light, Dodge, Burn, Darken, Lighten, Difference, Difference Squared) as a compositor "
              "plug-in's GUID and the others as a number: both were read off VEGAS's own files (vegas180.exe, vegas180k.dll).",
              "Each track's Compositing Mode button (in its header) shows the mode its name says, and the picture looks like that mode. "
              "One field next to the mode (word 0x60 of the track's record, a number VEGAS keeps only on tracks whose mode is not Source "
              "Alpha) is left as it was: if VEGAS shows Source Alpha on a track that was Source Alpha before, that field is why - say which.");
    }

    // 9. time stretch / pitch shift
    if (stretchS) {
        Project p = stretchS->p;
        struct Set { const char* method; const char* mode; bool keep; double pitch; bool preserve; double formant; bool invert; const char* what; };
        const Set sets[] = {
            {"elastique", "pro", true, 3, true, -2, false, "élastique, Pro, Lock to stretch off, pitch change +3, Preserve formants on, formant shift -2"},
            {"elastique", "efficient", false, 0, false, 0, false, "élastique, Efficient, Lock to stretch on"},
            {"elastique", "soloist-mono", true, 0, true, 0, false, "élastique, Soloist (Monophonic), Lock to stretch off, Preserve formants on"},
            {"elastique", "soloist-speech", true, -5, false, 0, false, "élastique, Soloist (Speech), Lock to stretch off, pitch change -5"},
            {"classic", "A07", true, 2, false, 0, false, "Classic, A07. Speech 1, Lock to stretch off, pitch change +2"},
            {"classic", "A19", false, 0, false, 0, false, "Classic, A19. Drums (tiny echo), Lock to stretch on"},
            {"none", "pro", false, 0, false, 0, false, "None"},
            {"elastique", "pro", true, 0, false, 0, true, "élastique, Pro, Lock to stretch off, and Invert Phase on"},
        };
        std::string list;
        size_t k = 0;
        for (auto& t : p.tracks) {
            if (t.type != TrackType::Audio) continue;
            for (auto& e : t.events) {
                if (k >= sizeof sets / sizeof sets[0]) break;
                const Set& st = sets[k++];
                e.stretchMethod = st.method;
                e.stretchMode = st.mode;
                e.keepPitch = st.keep;
                e.pitch = st.pitch;
                e.preserveFormants = st.preserve;
                e.formant = st.formant;
                e.invertPhase = st.invert;
                list += strfmt("%sthe event at %.3f s: %s", list.empty() ? "" : "; ", e.start, st.what);
            }
        }
        write(out, p, vegasFolder(stretchS->path), "stretch - reno time stretch.veg", fileOf(stretchS->path),
              "Audio events' Time stretch / pitch shift settings (Audio Event Properties): " + list + ".",
              "Each of those events' Properties (Audio Event tab) shows those settings, and the events sound that way. The record's layout was "
              "read off VEGAS's own Audio Event Properties code (vegas180.exe), so this checks the reading of it end to end.");
    }

    // 10. effects before pan/crop, takes, a locked event, regions
    if (baseS) {
        Project p = baseS->p;
        Track& v = p.tracks[0];
        Track& a = p.tracks[1];
        Event& ve = v.events[0];
        if (!ve.fx.empty()) ve.fx[0].beforePanCrop = true;
        // pan/crop zoomed in on the middle, so that before/after pan/crop shows
        PanCropKey k;
        k.cx = 0.5; k.cy = 0.5; k.w = 0.5; k.h = 0.5;
        ve.panCrop.assign(1, k);
        if (!ve.fx.empty() && ve.fx[0].pluginId == "com.vegascreativesoftware:brightnessandcontrast") {
            // (brighter, so that it shows)
            Json b = Json::array();
            b.push(0.3);
            ve.fx[0].state["params"]["Brightness"]["v"] = b;
        }
        // a second take of each event: the same file from 10 s further on (the sound's made the active take)
        for (Event* e : {&ve, &a.events[0]}) {
            Take t;
            t.mediaId = e->mediaId;
            t.offset = e->offset + 10.0;
            t.name = "Reno take 2";
            e->takes.push_back(t);
        }
        std::swap(a.events[0].offset, a.events[0].takes[0].offset);
        a.events[0].takes[0].name = "the first take";
        a.events[0].locked = true;
        Project::Marker r1;
        r1.id = p.newId();
        r1.time = 1.0;
        r1.length = 1.5;
        r1.region = true;
        r1.name = "Reno region";
        Project::Marker r2 = r1;
        r2.id = p.newId();
        r2.time = 3.0;
        r2.length = 0.5;
        r2.name = "Another region";
        Project::Marker m1;
        m1.id = p.newId();
        m1.time = 2.0;
        m1.name = "A marker";
        p.markers = {r1, m1, r2};
        write(out, p, baseDir, "base - reno takes regions pre-pancrop.veg", fileOf(baseS->path),
              "The video event's Brightness and Contrast (Brightness 0.3) is applied before Event Pan/Crop, and its pan/crop zooms in 2x on the middle; both events "
              "have a second take (the same file from 10 s further on, named 'Reno take 2' and 'the first take'), and the sound event plays its "
              "second take and is locked; two regions ('Reno region' from 1 s to 2.5 s, 'Another region' from 3 s to 3.5 s) and a marker at 2 s.",
              "In the video event's Video Event FX the effect sits before Pan/Crop in the chain; T / Shift+T on each event switch between its two "
              "takes (Edit > Take); the sound event cannot be moved (locked); the regions show as regions (with their ends) in the marker bar and "
              "in Edit Details, numbered 1 and 2, and the marker as marker 1.");
    }

    // the README
    std::string r;
    r += "Files written by Reno's .veg writer, for opening in VEGAS Pro 18\n";
    r += "================================================================\n\n";
    r += "Each file is one of your projects with edits of one kind made in Reno (or, for the last, a project made in Reno\n";
    r += "from nothing), saved the way Reno now saves every project. They name your media by the paths your projects use,\n";
    r += "so copy each into the folder of the project it came from (listed below) before opening it - VEGAS then finds the\n";
    r += "media as it does for the originals. Nothing here changes your own projects.\n\n";
    r += "What to send back, for each file: whether VEGAS opened it, any message it showed, and anything that is not as\n";
    r += "described (a screenshot helps).\n\n";
    int n = 0;
    for (const auto& m : g_made) {
        r += strfmt("%d. %s\n", ++n, m.file.c_str());
        r += "   from: " + m.from + "\n";
        r += "   what Reno changed: " + m.what + "\n";
        r += "   what to look at: " + m.check + "\n";
        if (!m.warnings.empty()) {
            r += "   what Reno said it could not save:\n";
            for (const auto& w : m.warnings) r += "     - " + w + "\n";
        }
        r += "\n";
    }
    r += "What Reno still needs a project of (to learn how VEGAS keeps it)\n";
    r += "----------------------------------------------------------------\n";
    r += "None of your projects has the things below, and VEGAS's own files do not show their records plainly, so Reno keeps them\n";
    r += "when it reads a project but cannot make them. One small project with all of them would settle it. In VEGAS Pro 18:\n\n";
    r += "  1. File > New. Import any video (with sound) and put it on the timeline twice, on two video tracks (and their sound).\n";
    r += "  2. VST3: add a VST3 audio effect to the first sound event (Event FX) and another VST3 effect to the second audio track\n";
    r += "     (Track FX). Change one setting of each from its default and write down which setting and value.\n";
    r += "  3. Compositing: click the lower video track's \"Make Compositing Child\" button (the arrow in its header), then on the\n";
    r += "     upper (parent) track set Parent Composite Mode to Multiply and move its Parent Motion a little (to the right).\n";
    r += "  4. Track group: select both audio tracks, right-click a track header > Group > Group Selected Tracks.\n";
    r += "  5. Markers: Insert > Command (any command, e.g. URL with https://example.com), Insert > Audio CD Track Region\n";
    r += "     around a few seconds, and Insert > Audio CD Track Index inside it.\n";
    r += "  6. If the Compositing Mode menu of a video track offers Custom..., pick a compositor in it (any).\n";
    r += "  7. Save it as \"reno samples 1.veg\". Then change only the VST3 setting of step 2 on the event to another value and\n";
    r += "     Save As \"reno samples 2.veg\".\n\n";
    r += "Send both .veg files and the notes from step 2.\n";
    writeFile(out + "/README.txt", r);
    std::printf("%s\n", g_fail ? "FAILED" : "all written and read back");
    return g_fail ? 1 : 0;
}
