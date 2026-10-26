// gen_big: makes huge synthetic projects for the scaling measurements (perf_scale, ui_scale_test), as .veg (Reno's
// writer) and .reno, with small made-up media and a tree of nested .veg projects.
//
//   gen_big <output folder> [--events N] [--audio-tracks A] [--video-tracks V] [--nested M] [--seed S]
//                           [--tiny F] [--name base] [--media-only]
//
// - the media (made once into <output folder>/media): tones and noise as 48 kHz stereo and 44.1 kHz mono wav files,
//   a few 160x90 videos with and without sound (made with the ffmpeg program), a still picture;
// - M nested projects (<folder>/nested/nest_NNN.veg): about 60% are plain (a video track and two audio tracks with a
//   handful of events), 30% hold events that are plain nested projects, 10% hold events that are those (3 levels);
// - the main project: A audio and V video tracks and N events in an edit-heavy style: bursts of tiny stutters
//   (sub-frame events 5..40 ms among them), ordinary clips, long ones; a quarter of the events with fades, some with
//   volume / opacity envelopes, a few with effects; about 12% of the events are nested projects, some used hundreds
//   of times; the picture and sound of a clip are grouped.
// Files: <folder>/<name>.veg and <name>.reno (name defaults to big_<N>).
#include "../../src/engine/engine.h"
#include "../../src/vegas/vegas.h"
#include "../../src/audiofx/audioplugin.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/stat.h>

using namespace reno;

static std::mt19937_64 gRng(1);
static double rnd() { return std::uniform_real_distribution<double>(0, 1)(gRng); }
static double rnd(double a, double b) { return a + (b - a) * rnd(); }
static int rndi(int n) { return n <= 1 ? 0 : (int)(gRng() % (uint64_t)n); }

static void mkdirs(const std::string& p) { ::mkdir(p.c_str(), 0777); }
static bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }

static bool writeTone(const std::string& path, double seconds, double hz, int rate, int channels, bool noise)
{
    EncodeSettings es;
    es.path = path;
    es.video = false;
    es.sampleRate = rate;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    const int n = (int)(seconds * rate);
    std::vector<float> l((size_t)n), r((size_t)n);
    std::mt19937 g(7);
    for (int k = 0; k < n; ++k) {
        const double t = (double)k / rate;
        float v = (float)(0.4 * std::sin(2 * 3.14159265358979 * (hz + 20 * std::sin(t * 3)) * t));
        if (noise) v = (float)(((int)(g() & 0xFFFF) - 32768) / 32768.0 * 0.2);
        l[(size_t)k] = v;
        r[(size_t)k] = channels > 1 ? v * 0.8f : v;
    }
    return w->writeAudio(l.data(), r.data(), n) && w->finish(nullptr);
}

static bool makeMedia(const std::string& dir, std::vector<std::string>& audio, std::vector<std::string>& video, std::vector<std::string>& av, std::string& still)
{
    mkdirs(dir);
    struct A { const char* name; double sec, hz; int rate, ch; bool noise; };
    const A as[] = {{"tone_a.wav", 12, 220, 48000, 2, false}, {"tone_b.wav", 9, 330, 48000, 2, false}, {"tone_c.wav", 15, 523, 48000, 2, false},
                    {"noise.wav", 7, 0, 48000, 2, true}, {"mono44.wav", 10, 440, 44100, 1, false}, {"tone_d.wav", 20, 150, 48000, 2, false}};
    for (const A& a : as) {
        const std::string p = dir + "/" + a.name;
        if (!exists(p) && !writeTone(p, a.sec, a.hz, a.rate, a.ch, a.noise)) return false;
        audio.push_back(p);
    }
    struct V { const char* name; const char* src; int audio; };
    const V vs[] = {{"vid_a.mp4", "testsrc2=size=160x90:rate=30:duration=10", 0}, {"vid_b.mp4", "mandelbrot=size=160x90:rate=30", 0},
                    {"vid_c.mp4", "testsrc=size=160x90:rate=30:duration=8", 0}, {"av_a.mp4", "testsrc2=size=160x90:rate=30:duration=10", 1},
                    {"av_b.mp4", "smptebars=size=160x90:rate=30:duration=12", 1}};
    for (const V& v : vs) {
        const std::string p = dir + "/" + v.name;
        if (!exists(p)) {
            std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i \"" + std::string(v.src) + "\" ";
            if (v.audio) cmd += "-f lavfi -i \"sine=frequency=" + std::to_string(300 + 77 * (int)(v.name[3] - 'a')) + ":duration=12\" -c:a aac -shortest ";
            if (!std::strcmp(v.name, "vid_b.mp4")) cmd += "-t 9 ";
            cmd += "-c:v libx264 -preset ultrafast -g 15 -pix_fmt yuv420p \"" + p + "\"";
            if (std::system(cmd.c_str()) != 0) return false;
        }
        (v.audio ? av : video).push_back(p);
    }
    still = dir + "/still.png";
    if (!exists(still)) {
        const std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=160x90:rate=1 -frames:v 1 -update 1 \"" + still + "\"";
        if (std::system(cmd.c_str()) != 0) return false;
    }
    return true;
}

static uint64_t addMedia(Project& p, const std::string& path)
{
    for (const auto& m : p.media) if (m.path == path) return m.id;
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    std::unique_ptr<MediaReader> r = openMediaOrProject(path, nullptr);
    if (r) m.info = r->info();
    else m.offline = true;
    p.media.push_back(m);
    return m.id;
}

static Track& addTrack(Project& p, TrackType type, const std::string& name)
{
    Track t;
    t.id = p.newId();
    t.type = type;
    t.name = name;
    p.tracks.push_back(t);
    return p.tracks.back();
}

struct Gen {
    Project* p = nullptr;
    double fps = 30;
    int audioFxCount = 0;
    AudioPluginDesc audioFx;
    bool haveAudioFx = false;
    double mediaLen(uint64_t id) const { const MediaItem* m = p->findMedia(id); return m && m->info.duration > 0 ? m->info.duration : 5; }
};

// Puts one event on a track: start/length in seconds, offset into the media, optional extras by chance.
static Event& put(Gen& g, Track& t, uint64_t media, double start, double len, double offset, bool extras)
{
    Event e;
    e.id = g.p->newId();
    e.mediaId = media;
    e.start = start;
    e.length = len;
    e.offset = offset;
    e.name = strfmt("%s %d", t.type == TrackType::Audio ? "a" : "v", (int)(e.id % 100000));
    setDefaultFadeCurves(e, t.type);
    if (extras) {
        const double r = rnd();
        if (r < 0.25) {
            e.fadeIn = std::min(len * 0.45, rnd(0.005, 0.3));
            if (rnd() < 0.7) e.fadeOut = std::min(len * 0.45, rnd(0.005, 0.3));
        }
        if (rnd() < 0.3) e.gain = rnd(0.3, 1.0);
        if (rnd() < 0.05) {
            Envelope env;
            env.kind = t.type == TrackType::Audio ? "volume" : "composite";
            const int n = 3 + rndi(4);
            for (int k = 0; k < n; ++k) {
                EnvPoint pt;
                pt.time = len * k / (n - 1);
                pt.value = t.type == TrackType::Audio ? rnd(0.2, 1.2) : rnd(0.3, 1.0);
                pt.curve = rndi(4);
                env.points.push_back(pt);
            }
            e.envelopes.push_back(env);
        }
        if (t.type == TrackType::Audio && rnd() < 0.02 && g.haveAudioFx) {
            FxSlot s;
            s.id = g.p->newId();
            s.kind = "audio";
            s.audio = g.audioFx;
            s.name = g.audioFx.name;
            e.fx.push_back(s);
            ++g.audioFxCount;
        }
        if (t.type == TrackType::Video && rnd() < 0.02) {
            FxSlot s;
            s.id = g.p->newId();
            s.kind = "ofx";
            s.pluginId = "com.vegascreativesoftware:gaussianblur";
            s.name = "VEGAS Gaussian Blur";
            e.fx.push_back(s);
        }
        if (rnd() < 0.04) e.rate = rnd(0.5, 2.0);
        if (rnd() < 0.03) e.reverse = true;
        if (t.type == TrackType::Audio && rnd() < 0.03) e.pitch = (double)(rndi(13) - 6);
    }
    t.events.push_back(e);
    return t.events.back();
}

// Fills a track with 'count' events in an edit-heavy style: stutter bursts, ordinary clips, long ones.
static void fillTrack(Gen& g, Track& t, int count, const std::vector<uint64_t>& media, const std::vector<uint64_t>& nested, double nestedShare, double tiny,
                      std::vector<std::pair<uint64_t, Event*>>* avLinks = nullptr)
{
    double pos = rnd(0, 3);
    int made = 0;
    (void)avLinks;
    while (made < count) {
        const double r = rnd();
        if (r < tiny) {
            // a burst: k events butted (or nearly), the same bit of media again and again, or stepping through it
            const int k = std::min(count - made, 6 + rndi(35));
            uint64_t m = media[(size_t)rndi((int)media.size())];
            const double ml = g.mediaLen(m);
            const double len = rnd() < 0.15 ? rnd(0.005, 0.034) : rnd(0.034, 0.15);    // (some shorter than a frame)
            double off = rnd(0, std::max(0.01, ml - 1.0));
            const bool step = rnd() < 0.4;
            for (int i = 0; i < k; ++i) {
                put(g, t, m, pos, len, std::fmod(off, std::max(0.05, ml - len)), true);
                pos += len + (rnd() < 0.1 ? rnd(0.0, 0.05) : 0.0);
                if (step) off += len;
            }
            made += k;
        } else {
            const bool isNested = !nested.empty() && rnd() < nestedShare;
            uint64_t m = isNested ? nested[(size_t)(std::pow(rnd(), 2.5) * (double)nested.size())] : media[(size_t)rndi((int)media.size())];
            const double ml = g.mediaLen(m);
            double len = rnd() < 0.8 ? rnd(0.3, 3.0) : rnd(3.0, 14.0);
            len = std::min(len, std::max(0.1, ml));
            put(g, t, m, pos, len, rnd(0, std::max(0.0, ml - len)), true);
            pos += len * (rnd() < 0.2 ? rnd(0.5, 0.95) : 1.0) + (rnd() < 0.3 ? rnd(0.0, 1.0) : 0.0);
            ++made;
        }
    }
}

static void setupGen(Gen& g, Project& p)
{
    g.p = &p;
    for (const AudioPluginDesc& d : builtinAudioPlugins()) { g.audioFx = d; g.haveAudioFx = true; break; }
}

// A nested project: level 1 plain, level 2 and 3 use nested projects of the level below.
static void makeNested(const std::string& path, int level, const std::vector<std::string>& below, const std::vector<std::string>& audio,
                       const std::vector<std::string>& video, const std::vector<std::string>& av, const std::string& still)
{
    Project p;
    p.settings.width = 160;
    p.settings.height = 90;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    Gen g;
    setupGen(g, p);
    std::vector<uint64_t> am, vm, nm;
    for (const auto& s : audio) am.push_back(addMedia(p, s));
    for (const auto& s : video) vm.push_back(addMedia(p, s));
    for (const auto& s : av) vm.push_back(addMedia(p, s));
    vm.push_back(addMedia(p, still));
    for (const auto& s : below) nm.push_back(addMedia(p, s));
    const int per = 4 + rndi(10);
    if (level == 1) {
        fillTrack(g, addTrack(p, TrackType::Video, "V"), per, vm, {}, 0, 0.1);
        fillTrack(g, addTrack(p, TrackType::Audio, "A1"), per, am, {}, 0, 0.2);
        fillTrack(g, addTrack(p, TrackType::Audio, "A2"), per / 2 + 1, am, {}, 0, 0.1);
    } else {
        fillTrack(g, addTrack(p, TrackType::Video, "V"), per, vm, nm, 0.7, 0.05);
        fillTrack(g, addTrack(p, TrackType::Audio, "A1"), per, am, nm, 0.6, 0.1);
        fillTrack(g, addTrack(p, TrackType::Audio, "A2"), per / 2 + 1, am, {}, 0, 0.1);
    }
    for (auto& t : p.tracks) t.sortEvents();
    VegasSaveResult r = saveVegasProject(p, path);
    if (!r.ok) std::fprintf(stderr, "cannot write %s: %s\n", path.c_str(), r.error.c_str());
}

int main(int argc, char** argv)
{
    std::string out;
    int events = 10000, audioTracks = 30, videoTracks = 20, nestedCount = 100, seed = 1;
    double tiny = 0.2;
    std::string name;
    bool mediaOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--events") && i + 1 < argc) events = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--audio-tracks") && i + 1 < argc) audioTracks = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--video-tracks") && i + 1 < argc) videoTracks = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--nested") && i + 1 < argc) nestedCount = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiny") && i + 1 < argc) tiny = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!std::strcmp(argv[i], "--media-only")) mediaOnly = true;
        else out = argv[i];
    }
    if (out.empty()) { std::fprintf(stderr, "usage: gen_big <output folder> [--events N] ...\n"); return 2; }
    gRng.seed((uint64_t)seed * 7919 + 13);
    if (name.empty()) name = strfmt("big_%d", events);
    setProjectFileLoader([](const std::string& path, Project& o, std::string* error) { return loadProjectFile(path, o, error); });
    mkdirs(out);
    std::vector<std::string> audio, video, av;
    std::string still;
    if (!makeMedia(out + "/media", audio, video, av, still)) { std::fprintf(stderr, "cannot make the media\n"); return 1; }
    if (mediaOnly) return 0;

    // ---- the nested projects (60% level 1, 30% level 2, 10% level 3), made bottom up
    mkdirs(out + "/nested");
    std::vector<std::string> l1, l2, l3;
    const int n1 = std::max(1, nestedCount * 6 / 10), n2 = nestedCount * 3 / 10, n3 = nestedCount - n1 - n2;
    auto pick = [&](const std::vector<std::string>& from, int n) {
        std::vector<std::string> v;
        for (int k = 0; k < n && !from.empty(); ++k) v.push_back(from[(size_t)rndi((int)from.size())]);
        return v;
    };
    for (int k = 0; k < n1; ++k) { const std::string f = out + strfmt("/nested/nest1_%03d.veg", k); makeNested(f, 1, {}, audio, video, av, still); l1.push_back(f); }
    for (int k = 0; k < n2; ++k) { const std::string f = out + strfmt("/nested/nest2_%03d.veg", k); makeNested(f, 2, pick(l1, 3 + rndi(4)), audio, video, av, still); l2.push_back(f); }
    for (int k = 0; k < n3; ++k) { const std::string f = out + strfmt("/nested/nest3_%03d.veg", k); makeNested(f, 3, pick(l2, 2 + rndi(3)), audio, video, av, still); l3.push_back(f); }

    // ---- the main project
    Project p;
    p.settings.width = 160;
    p.settings.height = 90;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    Gen g;
    setupGen(g, p);
    std::vector<uint64_t> am, vm, avm, nestedMedia;
    for (const auto& s : audio) am.push_back(addMedia(p, s));
    for (const auto& s : video) vm.push_back(addMedia(p, s));
    for (const auto& s : av) avm.push_back(addMedia(p, s));
    vm.push_back(addMedia(p, still));
    for (const auto& s : l1) nestedMedia.push_back(addMedia(p, s));
    for (const auto& s : l2) nestedMedia.push_back(addMedia(p, s));
    for (const auto& s : l3) nestedMedia.push_back(addMedia(p, s));
    std::vector<uint64_t> vAll = vm;
    for (uint64_t m : avm) vAll.push_back(m);
    const int tracks = audioTracks + videoTracks;
    std::vector<int> counts((size_t)tracks, 0);
    for (int k = 0; k < events; ++k) ++counts[(size_t)rndi(tracks)];
    for (int k = 0; k < videoTracks; ++k) fillTrack(g, addTrack(p, TrackType::Video, strfmt("Video %d", k + 1)), counts[(size_t)k], vAll, nestedMedia, 0.12, tiny);
    for (int k = 0; k < audioTracks; ++k) {
        std::vector<uint64_t> aud = am;
        for (uint64_t m : avm) aud.push_back(m);
        fillTrack(g, addTrack(p, TrackType::Audio, strfmt("Audio %d", k + 1)), counts[(size_t)videoTracks + (size_t)k], aud, nestedMedia, 0.12, tiny);
    }
    // the picture and sound of some clips grouped: an audio event placed with each of the first events of a few video tracks
    uint64_t grp = 0;
    for (int k = 0; k < std::min(videoTracks, audioTracks) && k < 8; ++k) {
        Track& vt = p.tracks[(size_t)k];
        Track& at = p.tracks[(size_t)videoTracks + (size_t)k];
        for (size_t i = 0; i < vt.events.size() && i < 40; i += 4) {
            Event& v = vt.events[i];
            const MediaItem* m = p.findMedia(v.mediaId);
            if (!m || !m->info.hasAudio) continue;
            Event& a = put(g, at, v.mediaId, v.start, v.length, v.offset, false);
            a.group = ++grp;
            vt.events[i].group = grp;
        }
    }
    for (auto& t : p.tracks) t.sortEvents();
    // a few markers and regions
    for (int k = 0; k < 50; ++k) {
        Project::Marker mk;
        mk.id = p.newId();
        mk.time = rnd(0, 300);
        mk.name = strfmt("marker %d", k);
        p.markers.push_back(mk);
    }
    size_t total = 0;
    for (const auto& t : p.tracks) total += t.events.size();
    const std::string veg = out + "/" + name + ".veg", reno = out + "/" + name + ".reno";
    VegasSaveResult r = saveVegasProject(p, veg);
    if (!r.ok) { std::fprintf(stderr, "cannot write %s: %s\n", veg.c_str(), r.error.c_str()); return 1; }
    std::string err;
    if (!p.save(reno, &err)) { std::fprintf(stderr, "cannot write %s: %s\n", reno.c_str(), err.c_str()); return 1; }
    std::printf("made %s and %s: %zu events on %d tracks, %zu media, %d nested (%zu/%zu/%zu), length %.0f s\n", veg.c_str(), reno.c_str(), total, tracks,
                p.media.size(), nestedCount, l1.size(), l2.size(), l3.size(), p.length());
    return 0;
}
