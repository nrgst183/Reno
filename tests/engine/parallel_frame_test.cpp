// Tests of rendering one frame on several threads (FrameOptions::parallel; parallelTasks in base.h): a project with
// many video tracks and features is rendered with parallelism on and off and the pictures must match exactly. Also
// checks that work is really handed out (parallelTaskStats), that tasks can start tasks, and that many frames
// rendered from several threads come out the same.
//
//   parallel_frame_test
#include "../../src/engine/engine.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <thread>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// "pat:N" a moving pattern (N picks the colours), 30 fps, 640x360.
class TestReader : public MediaReader {
public:
    MediaInfo mi;
    int seed = 0;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        const int frame = std::max(0, (int)std::floor(seconds * 30.0 + 1e-6));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4) {
                d[0] = (uint8_t)(x * 255 / w * (seed % 3 + 1) + frame * 3);
                d[1] = (uint8_t)(y * 255 / h + seed * 40);
                d[2] = (uint8_t)((x + y) / 2 + frame * (seed + 1));
                d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> testOpen(const std::string& path, std::string* error)
{
    if (path.compare(0, 4, "pat:") != 0) { if (error) *error = "unknown"; return nullptr; }
    std::unique_ptr<TestReader> r(new TestReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    r->mi.width = 640;
    r->mi.height = 360;
    r->mi.fps = 30;
    r->mi.duration = 60;
    r->seed = std::atoi(path.c_str() + 4);
    return std::unique_ptr<MediaReader>(r.release());
}

static uint64_t addMedia(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = path;
    std::string e;
    m.info = testOpen(path, &e)->info();
    p.media.push_back(m);
    return m.id;
}

static FxSlot slot(Project& p, const char* id)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = std::string("com.vegascreativesoftware:") + id;
    s.name = id;
    return s;
}

static Event makeEvent(Project& p, uint64_t media, double start, double len, double off)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = len;
    e.offset = off;
    return e;
}

static MaskPath rect(double x0, double y0, double x1, double y1)
{
    MaskPath m;
    for (auto q : {std::make_pair(x0, y0), std::make_pair(x1, y0), std::make_pair(x1, y1), std::make_pair(x0, y1)}) {
        MaskPoint pt;
        pt.x = q.first;
        pt.y = q.second;
        m.points.push_back(pt);
    }
    m.feather = 8;
    return m;
}

// Ten video tracks of everything the compositor does.
static Project build()
{
    Project p;
    p.settings.width = 480;
    p.settings.height = 270;
    p.settings.fpsNum = 30;
    uint64_t m[4];
    for (int i = 0; i < 4; ++i) m[i] = addMedia(p, "pat:" + std::to_string(i));
    MediaItem gen;
    gen.id = p.newId();
    gen.name = "gradient";
    gen.generated = true;
    gen.generator = slot(p, "colorgradient");
    gen.info.hasVideo = true;
    gen.info.width = 480;
    gen.info.height = 270;
    p.media.push_back(gen);
    const char* fxs[] = {"brightnessandcontrast", "saturationadjust", "vignette", "gaussianblur", "hsladjust", "sepia"};
    const char* modes[] = {"srcalpha", "add", "multiply", "screen", "overlay", "srcalpha", "difference", "srcalpha", "lighten", "srcalpha"};
    for (int k = 0; k < 10; ++k) {
        Track tr;
        tr.id = p.newId();
        tr.type = TrackType::Video;
        tr.composite = modes[k];
        tr.level = 0.6 + 0.04 * k;
        if (k == 6 || k == 7) {
            // the generator media on two tracks: one instance, two events at the same time
            Event a = makeEvent(p, gen.id, 0, 8, 0);
            a.gain = 0.5;
            tr.events.push_back(a);
        } else {
            Event a = makeEvent(p, m[k % 4], 0, 4, 1 + 0.3 * k);
            Event b = makeEvent(p, m[(k + 1) % 4], 3, 4, 5 + 0.2 * k);
            a.fadeOut = 1;
            b.fadeIn = 1;
            a.fx.push_back(slot(p, fxs[k % 6]));
            b.fx.push_back(slot(p, fxs[(k + 2) % 6]));
            if (k % 3 == 1) {
                PanCropKey k0, k1;
                k0.time = 0; k0.w = 0.6; k0.h = 0.6; k0.cx = 0.4; k0.cy = 0.4;
                k1 = k0; k1.time = 6; k1.cx = 0.6; k1.rotation = 12;
                a.panCrop = {k0, k1};
                b.panCrop = {k0, k1};
            }
            if (k == 2) {
                MaskKey mk;
                mk.paths.push_back(rect(0.2, 0.2, 0.8, 0.7));
                a.mask.push_back(mk);
                a.maskOn = true;
            }
            tr.events.push_back(a);
            tr.events.push_back(b);
            if (k % 2 == 0) tr.fx.push_back(slot(p, fxs[(k + 1) % 6]));
        }
        if (k % 4 == 3) { MotionKey mk; mk.time = 0; mk.w = 0.8; mk.h = 0.8; mk.rotation = 5; tr.motion = {mk}; }
        if (k == 5) tr.compositeLevel = 1;              // a child of track 4
        if (k == 8) { tr.adjustment = true; tr.events.clear(); tr.fx.clear(); tr.fx.push_back(slot(p, "sepia")); }
        p.tracks.push_back(tr);
    }
    return p;
}

static bool same(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b) return !a && !b;
    if (a->w != b->w || a->h != b->h || a->depth != b->depth) return false;
    for (int y = 0; y < a->h; ++y)
        if (std::memcmp(a->row(y), b->row(y), (size_t)a->w * a->pixelBytes()) != 0) return false;
    return true;
}

// Renders 'times' with the parallelism on and off (fresh pools: the same readers' states), and compares.
static void compare(const char* name, const Project& p, const std::vector<double>& times, FrameOptions fo)
{
    FxRuntime fx;
    fx.prepare(p);
    int differ = 0, blank = 0;
    std::string first;
    for (double t : times) {
        ImagePtr pic[2];
        for (int on = 0; on < 2; ++on) {
            MediaPool pool;
            pool.setOpener(testOpen);
            VideoEngine video(pool, fx);
            fo.parallel = on == 1;
            pic[on] = video.render(p, t, fo);
        }
        if (!pic[0]) ++blank;
        if (!same(pic[0], pic[1])) { ++differ; if (first.empty()) first = strfmt("t=%.3f", t); }
    }
    check(differ == 0 && blank == 0, name, differ ? strfmt("%d of %d differ (first %s)", differ, (int)times.size(), first.c_str()) : (blank ? "no picture" : ""));
}

int main()
{
    std::string err;
    const std::vector<double> times = {0.5, 1.2, 2.0, 3.5, 3.9, 5.1, 6.4};
    uint64_t j0, t0, h0;
    parallelTaskStats(j0, t0, h0);

    Project p = build();
    FrameOptions fo;
    compare("10 tracks: cross-fades, effects, pan/crop, motion, modes, mask, group, adjustment, shared generator", p, times, fo);
    fo.interactive = false;
    compare("the same as a final render", p, times, fo);
    fo.interactive = true;
    fo.scale = 0.5;
    compare("the same at half size", p, times, fo);
    fo.scale = 1;
    fo.draft = true;
    compare("the same as a draft", p, times, fo);
    fo.draft = false;
    fo.skipEffects = true;
    compare("the same with effects bypassed", p, times, fo);
    fo.skipEffects = false;

    Project solo = p;
    solo.tracks[3].solo = true;
    solo.tracks[4].solo = true;
    compare("with tracks soloed", solo, times, fo);
    Project muted = p;
    muted.tracks[4].mute = true;
    muted.tracks[1].mute = true;
    compare("with a compositing parent and a track muted", muted, times, fo);

    Project fl = p;
    fl.settings.pixelFormat = "32bit";
    compare("a 32-bit floating point project", fl, times, fo);

    Project blur = p;
    {
        Envelope e;
        e.kind = "motionblur";
        EnvPoint a, b;
        a.time = 0; a.value = 0.5; b.time = 10; b.value = 0.5;
        e.points = {a, b};
        blur.videoEnvelopes.push_back(e);
    }
    FrameOptions fb;
    fb.interactive = false;
    compare("motion blur (sub-frames made at the same time)", blur, {1.2, 3.5}, fb);
    Project ss = p;
    {
        Envelope e;
        e.kind = "supersample";
        EnvPoint a, b;
        a.time = 0; a.value = 3; b.time = 10; b.value = 3;
        e.points = {a, b};
        ss.videoEnvelopes.push_back(e);
    }
    compare("supersampling", ss, {1.2, 3.5}, fb);
    Project il = p;
    il.settings.fieldOrder = "upper";
    compare("interlaced fields", il, {1.2, 3.5}, fb);

    uint64_t j1, t1, h1;
    parallelTaskStats(j1, t1, h1);
    if (processorCount() > 1) {
        check(t1 - t0 > 100, "the frames' parts were handed out as tasks", strfmt("%llu tasks", (unsigned long long)(t1 - t0)));
        check(h1 > h0, "and helper threads took some of them", strfmt("%llu on helpers", (unsigned long long)(h1 - h0)));
    } else {
        std::printf("(one processor: no tasks expected)\n");
    }

    // several frames at once from several threads (the playback threads), sharing the effects and the pool
    {
        FxRuntime fx;
        fx.prepare(p);
        MediaPool pool;
        pool.setOpener(testOpen);
        VideoEngine video(pool, fx);
        std::vector<ImagePtr> serial(times.size());
        FrameOptions o;
        o.parallel = false;
        for (size_t i = 0; i < times.size(); ++i) serial[i] = video.render(p, times[i], o);
        std::vector<ImagePtr> got(times.size());
        std::vector<std::thread> th;
        for (size_t i = 0; i < times.size(); ++i)
            th.emplace_back([&, i] { FrameOptions q; got[i] = video.render(p, times[i], q); });
        for (auto& t : th) t.join();
        int bad = 0;
        for (size_t i = 0; i < times.size(); ++i) if (!same(serial[i], got[i])) ++bad;
        check(bad == 0, "frames rendered from several threads at once, each on several threads, are the serial pictures", strfmt("%d differ", bad));
    }

    // the pool tasks can nest and be used from many callers without waiting for each other
    {
        std::atomic<int> sum{0};
        std::vector<std::thread> th;
        for (int c = 0; c < 4; ++c)
            th.emplace_back([&] {
                parallelTasks(6, [&](int) {
                    parallelTasks(5, [&](int) {
                        parallelRows(512, 4096, [&](int a, int b) { sum += b - a; });
                    });
                });
            });
        for (auto& t : th) t.join();
        check(sum.load() == 4 * 6 * 5 * 512, "nested parallelTasks / parallelRows from several callers finish with every piece done", std::to_string(sum.load()));
    }

    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
