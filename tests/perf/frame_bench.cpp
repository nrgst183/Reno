// frame_bench: renders a stretch of a project frame after frame as the playing preview does (interactive, at a preview
// scale: FrameOptions::scale) and times each frame. Needs no interface. Media that is not where the project says is
// looked for by file name beside the project.
//
//   frame_bench <project.veg|.reno> <from s> <to s> <scale> [--threads N] [--each] [--hash] [--list] [--draft] [--profile]
//   --list: only list the effects the project uses and whether each supports multi-resolution
//   --hash: a hash of every picture (to compare two builds)
#include "../../src/engine/engine.h"
#include "../../src/engine/fxruntime.h"
#include "../../src/engine/profile.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/ofxhost.h"
#include "../../src/vegas/vegas.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <ctime>

using namespace reno;
#ifdef FXB_BASE
#define FXB_MULTI(pl) false
#else
#define FXB_MULTI(pl) ((pl)->info.multiResolution)
#endif

static double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::printf("usage: frame_bench <project> <from s> <to s> <scale> [--each] [--hash] [--list] [--draft]\n");
        return 2;
    }
    bool each = false, hash = false, list = false, draft = false, prof = false;
    for (int i = 5; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--each")) each = true;
        else if (!std::strcmp(argv[i], "--hash")) hash = true;
        else if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--draft")) draft = true;
        else if (!std::strcmp(argv[i], "--profile")) prof = true;
    }
    Project p;
    std::string err;
    std::vector<std::string> notes;
    if (!loadProjectFileWithNotes(argv[1], p, &err, &notes)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const std::string dir = std::string(argv[1]).substr(0, std::string(argv[1]).find_last_of("/\\") + 1);
    int missing = 0;
    for (auto& m : p.media) {
        if (m.generated || m.path.compare(0, 6, "synth:") == 0) continue;
        FILE* f = std::fopen(m.path.c_str(), "rb");
        if (f) { std::fclose(f); continue; }
        std::string name = m.path.substr(m.path.find_last_of("/\\") + 1);
        std::string alt = dir + name;
        f = std::fopen(alt.c_str(), "rb");
        if (f) { std::fclose(f); m.path = alt; m.offline = false; }
        else { m.offline = true; ++missing; }
    }
    std::printf("project %dx%d, %.2f fps, %zu tracks, %zu media (%d missing)\n", p.settings.width, p.settings.height, p.settings.fps(), p.tracks.size(), p.media.size(), missing);
    // the effects the project uses
    std::map<std::string, int> uses;
    auto count = [&](const FxChain& c) { for (const auto& s : c) if (s.kind == "ofx") ++uses[s.pluginId]; };
    for (const auto& t : p.tracks) {
        count(t.fx);
        for (const auto& e : t.events) { count(e.fx); if (e.transition.kind == "ofx") ++uses[e.transition.pluginId]; }
    }
    for (const auto& m : p.media) if (m.generated) ++uses[m.generator.pluginId];
    MediaPool pool;
    FxRuntime fx;
    fx.prepare(p);
    for (const auto& u : uses) {
        ofx::Plugin* pl = ofx::Host::get().findPlugin(u.first);
        bool loaded = pl && pl->load();
        std::printf("  %-52s x%-3d %s\n", u.first.c_str(), u.second, !pl ? "(not found)" : !loaded ? "(failed to load)" : FXB_MULTI(pl) ? "multi-resolution" : "project size only");
    }
    if (list) {
        // where the effects are on the timeline
        for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
            const auto& t = p.tracks[ti];
            for (const auto& e : t.events) {
                std::string names;
                for (const auto& s : e.fx) if (s.kind == "ofx" && !s.bypass) names += (names.empty() ? "" : ", ") + s.pluginId.substr(s.pluginId.find(':') == std::string::npos ? 0 : s.pluginId.find(':') + 1);
                if (e.transition.kind == "ofx") names += (names.empty() ? "" : ", ") + std::string("transition ") + e.transition.pluginId;
                if (!names.empty()) std::printf("  track %zu  %.2f - %.2f s: %s\n", ti, e.start, e.start + e.length, names.c_str());
            }
            if (!t.fx.empty()) std::printf("  track %zu has %zu track effects\n", ti, t.fx.size());
        }
        return 0;
    }
    VideoEngine video(pool, fx);
    const double from = std::atof(argv[2]), to = std::atof(argv[3]), scale = std::atof(argv[4]);
    const int64_t f0 = p.frameAt(from), f1 = std::max(f0 + 1, p.frameAt(to));
    FrameOptions fo;
    fo.interactive = true;
    fo.draft = draft;
    fo.scale = scale;
    ImagePtr first = video.render(p, p.frameTime(f0), fo);      // (opens the readers, makes the effects)
    std::printf("picture %dx%d\n", first ? first->w : 0, first ? first->h : 0);
    if (prof) { profile::setEnabled(true); profile::reset(); }
    uint64_t h = 1469598103934665603ull;
    double worst = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const double cpu0 = cpuMs();
    for (int64_t f = f0; f < f1; ++f) {
        const auto a = std::chrono::steady_clock::now();
        ImagePtr img = video.render(p, p.frameTime(f), fo);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        worst = std::max(worst, ms);
        if (each) std::printf("frame %lld %.1f ms\n", (long long)f, ms);
        if (hash && img)
            for (int y = 0; y < img->h; ++y) h = fnv(h, img->row(y), (size_t)img->w * img->pixelBytes());
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%lld frames at scale %.2f in %.2f s: %.1f ms a frame (%.1f fps), worst %.1f ms\n", (long long)(f1 - f0), scale, total, total * 1000 / (double)(f1 - f0),
                (double)(f1 - f0) / total, worst);
    std::printf("CPU %.1f ms a frame (all threads; the machine is shared, wall time is noisy)\n", (cpuMs() - cpu0) / (double)(f1 - f0));
    if (prof) std::printf("%s\n", profile::formatReport(profile::snapshot(), total * 1000).c_str());
    if (hash) std::printf("HASH %016llx\n", (unsigned long long)h);
    return 0;
}
