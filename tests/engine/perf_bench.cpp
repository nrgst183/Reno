// Times Reno on edit-heavy projects (hundreds of tiny events, stutters, many tracks, nested projects, high
// frame rates). Each VEGAS project given is opened, its media replaced by stand-in clips, and timed:
//   open / save         loadProjectFile and writeVegasData
//   undo snapshot       copying the Project and its JSON
//   video               rendering the frames of the busiest two seconds in order, and at half size
//   audio               mixing those two seconds in 1024-sample blocks
//   nested              the project used as media in another
//   seek                random seeks in a long-GOP H.264 file, and frame steps backwards
// Prints one line per measure: "BENCH <project> <measure> <value> <unit>".
//
//   perf_bench <media folder> [--fx testfx.ofx] [--frames N] [--quick] project.veg...
// The media folder holds clip480.mp4 (640x480 29.97), clip720.mp4 (1280x720 30, 60 s, GOP 300), clip1080.mp4
// (1920x1080 60, GOP 600), tone.wav and still.png (see tests/engine/perf_media.sh). With --fx every OpenFX effect in
// the project is replaced by the test plug-in's Invert.
#include "../../src/engine/engine.h"
#include "../../src/vegas/vegas.h"
#include <chrono>
#include <ctime>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>

using namespace reno;

static double nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static std::string gName;
static void report(const char* what, double v, const char* unit)
{
    std::printf("BENCH %-28s %-26s %10.3f %s\n", gName.c_str(), what, v, unit);
    std::fflush(stdout);
}

struct Standins {
    std::string dir;
    std::map<std::string, MediaInfo> info;
    bool load(const std::string& d)
    {
        dir = d;
        for (const char* n : {"clip480.mp4", "clip720.mp4", "clip1080.mp4", "tone.wav", "still.png"}) {
            std::string err;
            auto r = openMedia(dir + "/" + n, &err);
            if (!r) { std::fprintf(stderr, "stand-in %s: %s\n", n, err.c_str()); return false; }
            info[n] = r->info();
        }
        return true;
    }
    std::string pick(const MediaItem& m) const
    {
        const std::string low = toLower(m.path);
        auto ends = [&](const char* e) { return endsWithNoCase(low, e); };
        if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".bmp") || ends(".gif") || m.info.isStill) return "still.png";
        if (ends(".wav") || ends(".mp3") || ends(".ogg") || ends(".flac") || ends(".m4a") || (m.info.hasAudio && !m.info.hasVideo)) return "tone.wav";
        if (m.info.fps > 45) return "clip1080.mp4";
        if (m.info.height > 0 && m.info.height <= 480) return "clip480.mp4";
        return "clip720.mp4";
    }
    // Every file the project names (nested projects too) becomes a stand-in clip.
    void apply(Project& p) const
    {
        for (auto& m : p.media) {
            if (m.generated) continue;
            const std::string n = pick(m);
            m.path = dir + "/" + n;
            m.info = info.at(n);
            m.offline = false;
        }
    }
};

// The busiest stretch: the start of the 'span' seconds in which the most events play (counted per frame).
static double busiest(const Project& p, double span, int* eventsThere)
{
    const double fps = p.settings.fps();
    const double len = p.length();
    const int64_t n = (int64_t)std::ceil(len * fps) + 1;
    std::vector<int> active((size_t)std::max<int64_t>(1, n), 0);
    for (const auto& t : p.tracks)
        for (const auto& e : t.events) {
            const int64_t a = std::max<int64_t>(0, (int64_t)std::floor(e.start * fps)), b = std::min<int64_t>(n - 1, (int64_t)std::ceil(e.end() * fps));
            for (int64_t f = a; f <= b; ++f) ++active[(size_t)f];
        }
    const int64_t w = std::max<int64_t>(1, (int64_t)(span * fps));
    int64_t best = 0, sum = 0, bestSum = -1;
    for (int64_t f = 0; f < n; ++f) {
        sum += active[(size_t)f];
        if (f >= w) sum -= active[(size_t)(f - w)];
        if (sum > bestSum) { bestSum = sum; best = std::max<int64_t>(0, f - w + 1); }
    }
    if (eventsThere) {
        std::set<uint64_t> ids;
        const double a = best / fps, b = a + span;
        for (const auto& t : p.tracks)
            for (const auto& e : t.events)
                if (e.start < b && e.end() > a) ids.insert(e.id);
        *eventsThere = (int)ids.size();
    }
    return best / fps;
}

static void replaceEffects(Project& p)
{
    p.forEachFx([](FxSlot& s) {
        if (s.kind != "ofx") return;
        s.pluginId = "reno.test.invert";
        s.state = Json();
        s.bypass = false;
    });
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: perf_bench <media folder> [--fx testfx.ofx] [--frames N] [--quick] project.veg...\n");
        return 2;
    }
    Standins si;
    if (!si.load(argv[1])) return 1;
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    bool fx = false, quick = false, seek = true;
    int frames = 60;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fx") && i + 1 < argc) {
            std::string err;
            if (!ofx::Host::get().loadBinary(argv[++i], &err)) { std::fprintf(stderr, "testfx: %s\n", err.c_str()); return 1; }
            fx = true;
        } else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--quick")) quick = true;
        else if (!std::strcmp(argv[i], "--no-seek")) seek = false;
        else files.push_back(argv[i]);
    }

    // ---- seeking in long-GOP H.264 (the stand-in clip, through the pool as the compositor asks)
    if (seek) {
        gName = "clip720.mp4";
        MediaPool pool;
        MediaItem m;
        m.id = 1;
        m.path = si.dir + "/clip720.mp4";
        m.info = si.info.at("clip720.mp4");
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> at(0.0, m.info.duration - 0.1);
        pool.videoFrame(1, m, 0.0, 1280, 720);
        double t0 = nowMs(), c0 = cpuMs();
        const int seeks = quick ? 10 : 30;
        for (int k = 0; k < seeks; ++k) pool.videoFrame(1, m, at(rng), 1280, 720);
        report("seek random", (nowMs() - t0) / seeks, "ms/frame");
        report("seek random cpu", (cpuMs() - c0) / seeks, "ms/frame");
        // stepping backwards a frame at a time from the end of a GOP (Ctrl+Left / J playback)
        t0 = nowMs();
        const int steps = quick ? 30 : 90;
        for (int k = 0; k < steps; ++k) pool.videoFrame(1, m, 29.9 - k / 30.0, 1280, 720);
        report("step backwards", (nowMs() - t0) / steps, "ms/frame");
        // many events of one file (a stutter): each event a few frames, all over the file
        t0 = nowMs();
        int shown = 0;
        for (int ev = 0; ev < (quick ? 20 : 60); ++ev)
            for (int f = 0; f < 3; ++f, ++shown) pool.videoFrame(100 + ev, m, 12.0 + (ev % 6) * 0.1 + f / 30.0, 1280, 720);
        report("stutter events", (nowMs() - t0) / shown, "ms/frame");
        if (getenv("PERF_SEEK_ONLY")) return 0;
    }

    for (const std::string& path : files) {
        gName = pathFileName(path);
        if (gName.size() > 28) gName = gName.substr(0, 28);
        Project p;
        std::string err;
        double t0 = nowMs();
        if (!loadProjectFile(path, p, &err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); continue; }
        report("open", nowMs() - t0, "ms");
        size_t events = 0, fxCount = 0, envPts = 0;
        int vtracks = 0, atracks = 0;
        for (const auto& t : p.tracks) {
            events += t.events.size();
            (t.type == TrackType::Video ? vtracks : atracks)++;
            for (const auto& e : t.events)
                for (const auto& env : e.envelopes) envPts += env.points.size();
        }
        p.forEachFx([&](const FxSlot&) { ++fxCount; });
        std::printf("INFO  %s: %dx%d %.3f fps, %d video + %d audio tracks, %zu events, %zu media, %zu effects, %.1f s\n", gName.c_str(),
                    p.settings.width, p.settings.height, p.settings.fps(), vtracks, atracks, events, p.media.size(), fxCount, p.length());

        t0 = nowMs();
        std::vector<std::string> warn;
        const std::string data = writeVegasData(p, path, &warn);
        report("save", nowMs() - t0, "ms");
        (void)data;

        // undo: an edit copies the project (before), keeps it, and makes the render threads' snapshot
        {
            const int n = 20;
            t0 = nowMs();
            for (int k = 0; k < n; ++k) {
                Project before = p;
                (void)before;
            }
            report("project copy", (nowMs() - t0) / n, "ms");
            t0 = nowMs();
            for (int k = 0; k < 3; ++k) (void)p.toJson().dump();
            report("project json", (nowMs() - t0) / 3, "ms");
        }

        si.apply(p);
        if (fx) replaceEffects(p);
        FxRuntime rt;
        rt.prepare(p);
        int there = 0;
        const double from = busiest(p, 2.0, &there);
        std::printf("INFO  %s: busiest 2 s from %.2f s: %d events\n", gName.c_str(), from, there);
        if (getenv("PERF_DUMP"))
            for (size_t ti = 0; ti < p.tracks.size(); ++ti)
                for (const auto& e : p.tracks[ti].events)
                    if (e.start < from + 2 && e.end() > from)
                        std::printf("  track %zu (%s %s) event %llu media %llu start %.3f len %.3f offset %.3f rate %.3f%s%s fx %zu\n", ti,
                                    p.tracks[ti].type == TrackType::Video ? "video" : "audio", p.tracks[ti].composite.c_str(),
                                    (unsigned long long)e.id, (unsigned long long)e.mediaId, e.start, e.length, e.offset, e.rate,
                                    e.reverse ? " reversed" : "", findEnvelope(e.envelopes, "velocity") ? " velocity" : "", e.fx.size());

        // ---- the picture, frame after frame (as playback asks)
        {
            MediaPool pool;
            VideoEngine ve(pool, rt);
            FrameOptions fo;
            double worst = 0, total = 0;
            const double c0 = cpuMs();
            for (int k = 0; k < frames; ++k) {
                const double a = nowMs();
                ImagePtr img = ve.render(p, p.frameTime(p.frameAt(from) + k), fo);
                const double d = nowMs() - a;
                total += d;
                worst = std::max(worst, d);
                if (!img) { std::printf("render failed\n"); break; }
            }
            report("video frame avg", total / frames, "ms");
            report("video frame cpu", (cpuMs() - c0) / frames, "ms");
            report("video frame worst", worst, "ms");
            report("video fps", 1000.0 * frames / std::max(1e-6, total), "fps");
        }

        // ---- playing the busiest two seconds as the preview does at 30 pictures a second (a picture every 1/30 s,
        // whatever the project's rate), at the project's size and at half of it (the preview's Preview / Half quality)
        for (double scale : {1.0, 0.5}) {
            MediaPool pool;
            VideoEngine ve(pool, rt);
            FrameOptions fo;
#ifndef PERF_BENCH_NO_SCALE
            fo.scale = scale;
#else
            if (scale < 1) continue;            // (built against a Reno that cannot render smaller pictures)
#endif
            const int n = 60;
            const double a = nowMs(), c0 = cpuMs();
            for (int k = 0; k < n; ++k) ve.render(p, from + k / 30.0, fo);
            const double wall = (nowMs() - a) / n, cpu = (cpuMs() - c0) / n;
            report(scale < 1 ? "play 2s half: frame" : "play 2s: frame", wall, "ms");
            report(scale < 1 ? "play 2s half: cpu" : "play 2s: cpu", cpu, "ms");
        }

        // ---- the sound, in playback's blocks
        {
            MediaPool pool;
            AudioEngine ae(pool, rt);
            const int rate = p.settings.sampleRate;
            const int block = 1024;
            const int blocks = (int)(2.0 * rate / block);
            std::vector<float> l(block), r(block);
            const int64_t s0 = (int64_t)std::llround(from * rate);
            double total = 0, worst = 0;
            const double c0 = cpuMs();
            for (int k = 0; k < blocks; ++k) {
                const double a = nowMs();
                ae.render(p, s0 + (int64_t)k * block, block, l.data(), r.data(), true);
                const double d = nowMs() - a;
                total += d;
                worst = std::max(worst, d);
            }
            report("audio block avg", total / blocks, "ms");
            report("audio block cpu", (cpuMs() - c0) / blocks, "ms");
            report("audio block worst", worst, "ms");
            report("audio realtime x", (2.0 * 1000.0) / std::max(1e-6, total), "x");
        }

        // ---- the project nested in another (as a stand-in-media .reno), its picture as the parent shows it
        if (!quick) {
            const std::string nested = "/tmp/perf_bench_nested.reno";
            Project inner = p;
            if (inner.save(nested, &err)) {
                Project outer;
                outer.settings = p.settings;
                MediaItem m;
                m.id = outer.newId();
                m.path = nested;
                std::unique_ptr<MediaReader> r = openMediaOrProject(nested, &err);
                if (r) {
                    m.info = r->info();
                    outer.media.push_back(m);
                    Track t;
                    t.id = outer.newId();
                    Event e;
                    e.id = outer.newId();
                    e.mediaId = m.id;
                    e.start = 0;
                    e.offset = from;
                    e.length = 2.0;
                    t.events.push_back(e);
                    outer.tracks.push_back(t);
                    MediaPool pool;
                    FxRuntime ort;
                    VideoEngine ve(pool, ort);
                    FrameOptions fo;
                    const int n = std::min(frames, 30);
                    const double a = nowMs(), c0 = cpuMs();
                    for (int k = 0; k < n; ++k) ve.render(outer, outer.frameTime(k), fo);
                    report("nested frame avg", (nowMs() - a) / n, "ms");
                    report("nested frame cpu", (cpuMs() - c0) / n, "ms");
                }
            }
        }
        rt.clear();
    }
    return 0;
}
