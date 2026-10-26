// render_bench: times the engine rendering and playing a typical heavy project built from the big test videos
// (obs1080.mp4: 1080p60 H.264, long1h.mp4: 720p30 H.264 + AAC). The project is made in code: a 1080p30 project with a
// video track holding two overlapping events (a cross-fade), a second track with a 720p picture under Event Pan/Crop
// keys and an OpenFX effect, a generated gradient track, and four audio tracks of several events each. Prints
// "BENCH <measure> <value> <unit>" (best of --reps) and, with --hash, a hash of every picture and of the mixed sound
// ("HASH <what> <hex>") so two builds can be compared byte for byte.
//
//   render_bench <obs1080.mp4> <long1h.mp4> [--reps N] [--frames N] [--hash] [--threads N] [--only export,renderas,full,half,quarter,audio,seek]
//
// "renderas" is File > Render As (renderProjectAs: several pictures at once, the encoder and the sound on their own
// threads) of --frames pictures into an uncompressed AVI with PCM sound, at --threads processors (0: all); with --hash
// it prints a hash of the file ("HASH renderas"), which must not depend on --threads.
//   RB_MULTI=n (8 video tracks, cross-fades, effects): see build()
#include "../../src/engine/engine.h"
#include "../../src/engine/renderas.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/ofxhost.h"
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

using namespace reno;

static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}
static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t hashImage(uint64_t h, const ImagePtr& img)
{
    if (!img) return fnv(h, "null", 4);
    h = fnv(h, &img->w, sizeof img->w);
    h = fnv(h, &img->h, sizeof img->h);
    for (int y = 0; y < img->h; ++y) h = fnv(h, img->row(y), (size_t)img->w * img->pixelBytes());
    return h;
}

static Json val(std::initializer_list<double> v)
{
    Json e = Json::object(), a = Json::array();
    for (double x : v) a.push(x);
    e["v"] = a;
    return e;
}
static FxSlot slot(Project& p, const std::string& id, const Json& params = Json())
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = id;
    s.name = id;
    if (params.isObject()) {
        Json st = Json::object();
        st["params"] = params;
        s.state = st;
    }
    return s;
}
static const std::string kV = "com.vegascreativesoftware:";

static Project build(const std::string& obs, const std::string& lng)
{
    Project p;
    p.settings.width = 1920;
    p.settings.height = 1080;
    p.settings.fpsNum = 30;
    MediaItem mo, ml, gen;
    std::string err;
    mo.id = p.newId(); mo.path = obs; mo.name = "obs";
    { auto r = openMedia(obs, &err); if (!r) { std::fprintf(stderr, "%s: %s\n", obs.c_str(), err.c_str()); std::exit(2); } mo.info = r->info(); }
    ml.id = p.newId(); ml.path = lng; ml.name = "long";
    { auto r = openMedia(lng, &err); if (!r) { std::fprintf(stderr, "%s: %s\n", lng.c_str(), err.c_str()); std::exit(2); } ml.info = r->info(); }
    gen.id = p.newId(); gen.name = "gradient"; gen.generated = true;
    gen.generator = slot(p, kV + "colorgradient");
    gen.info.hasVideo = true; gen.info.width = 1920; gen.info.height = 1080;
    p.media = {mo, ml, gen};
    bool keepOnly = false;
    auto track = [&](TrackType t) { Track tr; tr.id = p.newId(); tr.type = t; return tr; };
    auto ev = [&](uint64_t media, double start, double len, double off) {
        Event e; e.id = p.newId(); e.mediaId = media; e.start = start; e.length = len; e.offset = off; return e;
    };
    // V1: two overlapping events of the 1080p60 clip (cross-fade), with colour and blur effects
    Track v1 = track(TrackType::Video);
    { Event a = ev(mo.id, 0, 4, 10); a.fx.push_back(slot(p, kV + "brightnessandcontrast")); a.fadeOut = 1; v1.events.push_back(a);
      Event b = ev(mo.id, 3, 4, 40); b.fadeIn = 1; b.fx.push_back(slot(p, kV + "saturationadjust")); v1.events.push_back(b); }
    // V2: 720p under Pan/Crop keys, vignette
    Track v2 = track(TrackType::Video);
    { Event c = ev(ml.id, 1, 5, 100); c.gain = 0.8;
      PanCropKey k0, k1; k0.time = 0; k0.w = 0.5; k0.h = 0.5; k0.cx = 0.3; k0.cy = 0.3; k1 = k0; k1.time = 5; k1.cx = 0.7; k1.cy = 0.6; k1.rotation = 10;
      c.panCrop = {k0, k1}; c.fx.push_back(slot(p, kV + "vignette")); v2.events.push_back(c); }
    // V3: generated gradient at low opacity
    Track v3 = track(TrackType::Video);
    { Event g = ev(gen.id, 0.5, 6, 0); g.gain = 0.25; v3.events.push_back(g); }
    // RB_MULTI=n: instead of the three tracks above, n video tracks (the engine's within-frame parallelism: each track's
    // picture is worked out at the same time): alternately of the 1080p60 and the 720p clip, each with a cross-fade of two
    // events and an effect, some under Pan/Crop keys, one with Track Motion, different compositing modes.
    if (const char* mt = std::getenv("RB_MULTI")) {
        const int n = std::max(1, std::atoi(mt));
        const char* fxs[] = {"brightnessandcontrast", "saturationadjust", "vignette", "gaussianblur", "hsladjust"};
        const char* modes[] = {"", "add", "multiply", "screen", ""};
        for (int k = 0; k < n; ++k) {
            Track tr = track(TrackType::Video);
            const uint64_t m = k % 2 ? ml.id : mo.id;
            Event a = ev(m, 0, 4, 10 + 7 * k);
            Event b = ev(m, 3, 4, 40 + 5 * k);
            a.fadeOut = 1; b.fadeIn = 1;
            a.fx.push_back(slot(p, kV + fxs[k % 5]));
            b.fx.push_back(slot(p, kV + fxs[(k + 2) % 5]));
            if (k % 3 == 1) {
                PanCropKey k0, k1; k0.time = 0; k0.w = 0.6; k0.h = 0.6; k0.cx = 0.4; k0.cy = 0.4; k1 = k0; k1.time = 5; k1.cx = 0.6; k1.rotation = 8;
                a.panCrop = {k0, k1}; b.panCrop = {k0, k1};
            }
            tr.events.push_back(a);
            tr.events.push_back(b);
            tr.level = 0.9;
            tr.composite = modes[k % 5];
            if (k % 4 == 3) { MotionKey mk; mk.time = 0; mk.w = 0.8; mk.h = 0.8; mk.rotation = 5; tr.motion = {mk}; }
            p.tracks.push_back(tr);
        }
        keepOnly = true;
    }
    // (RB_TRACKS=1,2,3,a: which tracks to keep; RB_NOFX: without the effects - to see where the time goes)
    if (std::getenv("RB_NOFX")) for (Track* t : {&v1, &v2, &v3}) for (Event& e : t->events.mut()) e.fx.clear();
    const char* keep = std::getenv("RB_TRACKS");
    auto on = [&](char c) { return !keep || std::strchr(keep, c); };
    if (!keepOnly && on('1')) p.tracks.push_back(v1);
    if (!keepOnly && on('2')) p.tracks.push_back(v2);
    if (!keepOnly && on('3')) p.tracks.push_back(v3);
    // audio: 4 tracks, several events each, fades, pan, some with a rate
    for (int t = 0; t < 4; ++t) {
        Track a = track(TrackType::Audio);
        a.pan = (t - 1.5) * 0.4;
        a.level = 0.7;
        for (int k = 0; k < 6; ++k) {
            Event e = ev(t % 2 ? mo.id : ml.id, k * 1.2 + t * 0.15, 1.5, 5 + t * 30 + k * 3);
            e.fadeIn = 0.1; e.fadeOut = 0.1;
            a.events.push_back(e);
        }
        if (on('a')) p.tracks.push_back(a);
    }
    return p;
}

// (a function of its own so that valgrind --toggle-collect=rbMeasured profiles just the timed frames)
__attribute__((noinline)) void rbMeasured(VideoEngine& ve, const Project& p, const FrameOptions& o, double start, int n, double fps, bool hash, uint64_t& h)
{
    for (int f = 1; f <= n; ++f) {
        ImagePtr im = ve.render(p, start + f / fps, o);
        if (hash) h = hashImage(h, im);
    }
}

struct Rep { double wall, cpu; };
template <class F> static Rep timeIt(int reps, F f)
{
    Rep best{1e18, 1e18};
    for (int r = 0; r < reps; ++r) {
        const double w0 = nowMs(), c0 = cpuMs();
        f();
        const double w = nowMs() - w0, c = cpuMs() - c0;
        if (w < best.wall) best = {w, c};
    }
    return best;
}

int main(int argc, char** argv)
{
    std::string obs, lng, only;
    int reps = 3, frames = 30;
    bool hash = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = std::string(",") + argv[++i] + ",";
        else if (!std::strcmp(argv[i], "--hash")) hash = true;
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) setProcessorLimit(std::atoi(argv[++i]));
        else pos.push_back(argv[i]);
    }
    if (pos.size() < 2) { std::fprintf(stderr, "render_bench obs1080.mp4 long1h.mp4 [...]\n"); return 2; }
    auto want = [&](const char* n) { return only.empty() || only.find(std::string(",") + n + ",") != std::string::npos; };
    if (only.find(",fxall,") != std::string::npos) {
        // every built-in filter on a 1080p picture (the long clip's frame) with its default settings: ms for one frame
        Project base;
        base.settings.width = 1920; base.settings.height = 1080;
        MediaItem ml;
        std::string err;
        ml.id = base.newId(); ml.path = pos[1]; { auto r = openMedia(pos[1], &err); if (!r) return 2; ml.info = r->info(); }
        base.media = {ml};
        for (const auto& info : ofx::builtinInfos()) {
            if (!info.filter) continue;
            Project q = base;
            Track t; t.id = q.newId(); t.type = TrackType::Video;
            Event e; e.id = q.newId(); e.mediaId = ml.id; e.length = 5; e.offset = 100;
            e.fx.push_back(slot(q, info.id));
            t.events.push_back(e);
            q.tracks.push_back(t);
            FxRuntime f2;
            f2.prepare(q);
            MediaPool pool;
            VideoEngine ve(pool, f2);
            FrameOptions o;
            ve.render(q, 1.0, o);
            double bestW = 1e18, bestC = 1e18;
            uint64_t h = 1469598103934665603ull;
            for (int r = 0; r < reps; ++r) {
                const double w0 = nowMs(), c0 = cpuMs();
                ImagePtr im = ve.render(q, 1.0 + (r + 1) / 30.0, o);
                bestW = std::min(bestW, nowMs() - w0);
                bestC = std::min(bestC, cpuMs() - c0);
                if (r == 0) h = hashImage(h, im);
            }
            std::printf("FX %-28s %7.1f ms %7.1f cpu-ms  %016llx\n", info.id.substr(info.id.find(':') + 1).c_str(), bestW, bestC, (unsigned long long)h);
            std::fflush(stdout);
        }
        return 0;
    }
    Project p = build(pos[0], pos[1]);
    FxRuntime fx;
    fx.prepare(p);
    FrameOptions fo;
    const double t0 = 1.5;          // all three video tracks and the cross-fade (3..4) are inside t0 .. t0 + frames/30 for 90 frames
    const double fps = p.settings.fps();

    auto playVideo = [&](const char* name, double scale, int n, double start) {
        MediaPool pool;
        VideoEngine ve(pool, fx);
        FrameOptions o;
        o.scale = scale;
        // (a first frame outside the timing: files open, readers seek)
        ImagePtr first = ve.render(p, start, o);
        uint64_t h = 1469598103934665603ull;
        h = hashImage(h, first);
        const double w0 = nowMs(), c0 = cpuMs();
        rbMeasured(ve, p, o, start, n, fps, hash, h);
        const double w = nowMs() - w0, c = cpuMs() - c0;
        std::printf("BENCH %s ms/frame %.2f\nBENCH %s cpu-ms/frame %.2f\n", name, w / n, name, c / n);
        if (hash) std::printf("HASH %s %016llx\n", name, (unsigned long long)h);
        std::fflush(stdout);
    };
    // each measure is run `reps` times; the best (lowest ms) is kept
    auto best = [&](const char* name, double scale, int n, double start) {
        for (int r = 0; r < reps; ++r) playVideo(name, scale, n, start);
    };
    if (want("full")) best("video full", 1.0, frames, t0);
    if (want("half")) best("video half", 0.5, frames, t0);
    if (want("quarter")) best("video quarter", 0.25, frames, t0);
    if (want("fade")) best("video crossfade only", 0.5, std::min(frames, 25), 3.0);

    if (want("audio")) {
        const int block = 1024, rate = p.settings.sampleRate;
        const int64_t total = (int64_t)(6.0 * rate);
        for (int r = 0; r < reps; ++r) {
            MediaPool pool;
            AudioEngine ae(pool, fx);
            std::vector<float> l(block), rr(block);
            uint64_t h = 1469598103934665603ull;
            ae.render(p, (int64_t)(0.5 * rate), block, l.data(), rr.data(), true);     // (cold block outside the timing)
            const double w0 = nowMs(), c0 = cpuMs();
            int n = 0;
            for (int64_t s = (int64_t)(0.5 * rate) + block; s < total; s += block, ++n) {
                ae.render(p, s, block, l.data(), rr.data(), true);
                if (hash) { h = fnv(h, l.data(), block * 4); h = fnv(h, rr.data(), block * 4); }
            }
            const double w = nowMs() - w0, c = cpuMs() - c0;
            const double secs = (double)n * block / rate;
            std::printf("BENCH audio realtime-x %.1f\nBENCH audio cpu-ms/s %.1f\n", secs * 1000.0 / w, c / secs);
            if (hash) std::printf("HASH audio %016llx\n", (unsigned long long)h);
            std::fflush(stdout);
        }
    }

    if (want("renderas")) {
        const char* outDir = std::getenv("RB_OUT");
        for (int r = 0; r < reps; ++r) {
            RenderSpec spec;
            spec.enc.path = std::string(outDir ? outDir : "/tmp") + "/renderas_bench.avi";
            spec.enc.videoFamily = "rawvideo";
            spec.enc.audioFamily = "pcm";
            spec.width = 960; spec.height = 540;
            FxRuntime fx2;
            fx2.prepare(p);
            std::string err;
            const double w0 = nowMs(), c0 = cpuMs();
            const double secs = frames / fps;
            bool ok = renderProjectAs(p, fx2, spec, 1.5, 1.5 + secs, [](const ExportProgress&) { return true; }, &err);
            const double w = nowMs() - w0, c = cpuMs() - c0;
            std::printf("BENCH renderas(%s) fps %.2f\nBENCH renderas cpu-ms/frame %.1f\n", ok ? "ok" : err.c_str(), frames * 1000.0 / w, c / frames);
            if (hash && ok) {
                uint64_t h = 1469598103934665603ull;
                if (FILE* fp = std::fopen(spec.enc.path.c_str(), "rb")) {
                    unsigned char buf[65536];
                    size_t n;
                    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) h = fnv(h, buf, n);
                    std::fclose(fp);
                }
                std::printf("HASH renderas %016llx\n", (unsigned long long)h);
            }
            std::fflush(stdout);
        }
    }

    if (want("export")) {
        for (int r = 0; r < reps; ++r) {
            EncodeSettings es;
            es.path = (std::filesystem::temp_directory_path() / "reno_export_bench.mp4").string();
            es.width = 1920; es.height = 1080; es.fpsNum = 30;
            es.videoFamily = "h264";
            es.videoKbps = 8000;
            FxRuntime fx2;
            fx2.prepare(p);
            std::string err;
            const double w0 = nowMs(), c0 = cpuMs();
            const double secs = frames / fps;
            bool ok = exportProject(p, fx2, es, 1.5, 1.5 + secs, [](const ExportProgress&) { return true; }, &err);
            const double w = nowMs() - w0, c = cpuMs() - c0;
            std::printf("BENCH export(%s) fps %.2f\nBENCH export cpu-ms/frame %.1f\n", ok ? "ok" : err.c_str(), frames * 1000.0 / w, c / frames);
            std::fflush(stdout);
        }
    }
    return 0;
}
