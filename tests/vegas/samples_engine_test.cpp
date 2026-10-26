// samples_engine_test: runs the engine on every sample VEGAS project to catch crashes (build under ASan + UBSan).
//
//   samples_engine_test [folder with .veg files] [--ofx file.ofx ...] [--quick]
//                       (default $RENO_SAMPLES, searched recursively; it is only read)
//
// Each project is read and its picture and sound are rendered at a few points (start, a quarter, the middle, near the
// end, past the end) for a couple of seconds:
//   1. with its media offline, as the editor shows a project whose files are missing;
//   2. with stand-in media (MediaPool's opener) of the size, rate, length and kind the project says each file has;
//   3. a final render (FrameOptions::interactive false: motion blur, supersampling, fields) of a few frames, and a short
//      export with the stand-ins.
// A check fails if a render gives no picture, a picture of the wrong size, or sound that is not a number.
#include "../../src/vegas/vegas.h"
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "../../src/ofx/probe.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <unistd.h>
#include "../testdata.h"

using namespace reno;

static int g_fail = 0, g_pass = 0;

static bool check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); std::fflush(stdout); }
    return ok;
}

// A stand-in for a media file: what the project says the file is, with a test pattern and a tone.
class StandIn : public MediaReader {
public:
    MediaInfo mi;
    int tag = 1;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        if (!mi.hasVideo || mi.width <= 0 || mi.height <= 0) return nullptr;
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)(mi.width * s + 0.5)), h = std::max(1, (int)(mi.height * s + 0.5));
        const double fps = mi.fps > 0 ? mi.fps : 30.0;
        return ofx::makeTestPattern(w, h, mi.isStill ? tag : std::floor(seconds * fps + 1e-6) + tag * 20.0);
    }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        if (!mi.hasAudio) return false;
        const int64_t end = (int64_t)std::llround(mi.duration * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const float v = (n < 0 || (end > 0 && n >= end)) ? 0.f : (float)(0.3 * std::sin(2.0 * 3.14159265358979 * 110.0 * tag * (double)n / sampleRate));
            left[k] = v;
            right[k] = -v;
        }
        return true;
    }
};

// What a project says about each of its files, for the stand-ins (with sensible values where it says nothing).
static std::map<std::string, MediaInfo> standInInfo(const Project& p)
{
    std::map<std::string, MediaInfo> out;
    for (const MediaItem& m : p.media) {
        if (m.path.empty()) continue;
        MediaInfo mi = m.info;
        mi.path = m.path;
        std::string ext = m.path.substr(m.path.find_last_of('.') + 1);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        const bool picture = ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif" || ext == "tif" || ext == "webp";
        const bool sound = ext == "wav" || ext == "mp3" || ext == "flac" || ext == "ogg" || ext == "m4a" || ext == "aac" || ext == "wma";
        if (!mi.hasVideo && !mi.hasAudio) {
            mi.hasVideo = !sound;
            mi.hasAudio = !picture;
        }
        if (picture) mi.isStill = true;
        if (mi.hasVideo && (mi.width <= 0 || mi.height <= 0)) { mi.width = 640; mi.height = 360; }
        if (mi.hasVideo && !mi.isStill && mi.fps <= 0) mi.fps = 30;
        if (!mi.isStill && mi.duration <= 0) mi.duration = 60;
        if (mi.hasAudio && mi.sampleRate <= 0) mi.sampleRate = 48000;
        if (mi.hasAudio && mi.channels <= 0) mi.channels = 2;
        out[m.path] = mi;
    }
    return out;
}

static bool soundOk(const std::vector<float>& l, const std::vector<float>& r)
{
    for (size_t k = 0; k < l.size(); ++k)
        if (!std::isfinite(l[k]) || !std::isfinite(r[k])) return false;
    return true;
}

// Renders 'seconds' of picture (every 'step'th frame) and sound from each of the points.
static void renderAt(const std::string& name, const Project& p, MediaPool& pool, FxRuntime& fx, const std::vector<double>& points,
                     double seconds, int step, bool interactive)
{
    VideoEngine video(pool, fx);
    AudioEngine audio(pool, fx);
    const double fps = p.settings.fps();
    const int rate = p.settings.sampleRate > 0 ? p.settings.sampleRate : 48000;
    FrameOptions opt;
    opt.interactive = interactive;
    opt.subFrames = !interactive;
    for (double t0 : points) {
        bool pictures = true, sizes = true;
        const int frames = std::max(1, (int)(seconds * fps));
        for (int f = 0; f < frames; f += step) {
            const double t = t0 + f / fps;
            ImagePtr img = video.render(p, t, opt);
            if (!img) pictures = false;
            else if (img->w != p.settings.width || img->h != p.settings.height) sizes = false;
        }
        check(pictures, name + strfmt(": a picture at every frame from %.2f s", t0));
        check(sizes, name + strfmt(": project-sized pictures from %.2f s", t0));
        const int block = 2048;
        std::vector<float> l(block), r(block);
        bool sound = true;
        const int64_t s0 = (int64_t)std::llround(t0 * rate);
        for (int64_t s = s0; s < s0 + (int64_t)(seconds * rate); s += block) {
            audio.render(p, s, block, l.data(), r.data(), true);
            if (!soundOk(l, r)) sound = false;
        }
        check(sound, name + strfmt(": sound that is a number from %.2f s", t0));
    }
}

int main(int argc, char** argv)
{
    std::string folder = testDataDir("RENO_SAMPLES");
    std::vector<std::string> ofx;
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ofx" && i + 1 < argc) ofx.push_back(argv[++i]);
        else if (a == "--quick") quick = true;
        else folder = a;
    }
    if (!ofx.empty()) {
        std::vector<std::string> dirs;
        for (const auto& f : ofx) dirs.push_back(pathDir(f));
        Catalog::get().scanInProcess(dirs, std::vector<std::string>());
    }
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });

    if (testDataMissing(folder)) return 0;
    std::vector<std::string> files;
    if (std::filesystem::is_regular_file(folder)) files.push_back(folder);
    else
    for (const auto& e : std::filesystem::recursive_directory_iterator(folder))
        if (e.is_regular_file() && isVegasProject(e.path().string())) files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    std::printf("%d projects\n", (int)files.size());

    for (const std::string& path : files) {
        const std::string name = std::filesystem::path(path).filename().string();
        Project p;
        std::string err;
        std::vector<std::string> notes;
        if (!check(loadProjectFileWithNotes(path, p, &err, &notes), name + ": reads (" + err + ")")) continue;
        const double len = p.length();
        std::printf("%s: %dx%d %.3f fps, %d tracks, %d media, %.1f s\n", name.c_str(), p.settings.width, p.settings.height, p.settings.fps(),
                    (int)p.tracks.size(), (int)p.media.size(), len);
        std::fflush(stdout);
        std::vector<double> points = {0.0, len * 0.5, std::max(0.0, len - 1.0), len + 5.0};
        if (!quick) points.insert(points.begin() + 1, len * 0.25);
        const int step = quick ? 15 : 5;

        // 1. offline
        {
            MediaPool pool;
            pool.setOpener([](const std::string&, std::string* e) {
                if (e) *e = "offline";
                return std::unique_ptr<MediaReader>();
            });
            FxRuntime fx;
            fx.prepare(p);
            renderAt(name + " (offline)", p, pool, fx, points, 2.0, step, true);
        }
        // 2. stand-ins
        const std::map<std::string, MediaInfo> infos = standInInfo(p);
        auto opener = [infos](const std::string& path, std::string* e) -> std::unique_ptr<MediaReader> {
            auto it = infos.find(path);
            if (it == infos.end()) {
                if (e) *e = "not in the project";
                return std::unique_ptr<MediaReader>();
            }
            std::unique_ptr<StandIn> r(new StandIn());
            r->mi = it->second;
            r->tag = 1 + (int)(std::hash<std::string>()(path) % 7);
            return std::unique_ptr<MediaReader>(r.release());
        };
        {
            MediaPool pool;
            pool.setOpener(opener);
            FxRuntime fx;
            fx.prepare(p);
            renderAt(name + " (stand-ins)", p, pool, fx, points, 2.0, step, true);
            // 3. final-render frames
            renderAt(name + " (final)", p, pool, fx, {len * 0.5}, 0.2, 3, false);
        }
        {
            FxRuntime fx;
            fx.prepare(p);
            EncodeSettings es;
            const std::string out = "/tmp/samples_engine_test_" + std::to_string((long)getpid()) + ".mp4";
            es.path = out;
            es.width = 320;
            es.height = 180;
            std::string e;
            Project small = p;
            small.settings.width = 320;
            small.settings.height = 180;
            const double from = std::max(0.0, len * 0.5 - 0.25);
            const bool ok = exportProject(small, fx, es, from, from + 0.5, [](const ExportProgress&) { return true; }, &e, opener);
            check(ok, name + ": half a second exports with stand-ins (" + e + ")");
            std::remove(out.c_str());
        }
    }
    std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
