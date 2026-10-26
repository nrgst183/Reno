// Tests of nested projects (projects used as media): a .reno file renders the other project's picture and sound,
// a registered loader can add other kinds (the .veg reader), self-containing projects are refused, effects are made
// through the main-thread runner, project files are shared, and sound is resampled from the nested project's own.
//
//   nested_test <testfx.ofx> <scratch folder>
#include "../../src/engine/engine.h"
#include "../../src/engine/profile.h"
#include "../../src/ofx/builtins.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <thread>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <set>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

struct Px { int b, g, r, a; };
static Px pixel(const ImagePtr& img, int x, int y)
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return {-1, -1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d a=%d", p.b, p.g, p.r, p.a); }
static bool near(const Px& p, int b, int g, int r, int tol = 6) { return std::abs(p.b - b) <= tol && std::abs(p.g - g) <= tol && std::abs(p.r - r) <= tol; }

static bool writeStill(const std::string& path, int b, int g, int r)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = 320;
    es.height = 180;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    ImagePtr img = Image::make(320, 180, PixelDepth::Byte, false);
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) {
            uint8_t* d = img->row(y) + x * 4;
            d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; d[3] = 255;
        }
    return w->writeVideo(*img) && w->finish(nullptr);
}

// A short video clip (frames getting brighter), so that reading ahead has a file to seek into.
static bool writeClip(const std::string& path, int frames)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = 320;
    es.height = 180;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    ImagePtr img = Image::make(320, 180, PixelDepth::Byte, false);
    for (int f = 0; f < frames; ++f) {
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = (uint8_t)(f * 3); d[1] = (uint8_t)x; d[2] = (uint8_t)y; d[3] = 255;
            }
        if (!w->writeVideo(*img)) return false;
    }
    return w->finish(nullptr);
}

static bool writeTone(const std::string& path, double seconds)
{
    EncodeSettings es;
    es.path = path;
    es.video = false;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    const int n = (int)(seconds * 48000);
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int k = 0; k < n; ++k) l[(size_t)k] = r[(size_t)k] = (float)(0.5 * std::sin(2 * 3.14159265358979 * 440.0 * k / 48000.0));
    return w->writeAudio(l.data(), r.data(), n) && w->finish(nullptr);
}

static MediaItem mediaItem(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    std::unique_ptr<MediaReader> r = openMediaOrProject(path, nullptr);
    if (r) m.info = r->info();
    else m.offline = true;
    return m;
}

static Track& track(Project& p, TrackType type)
{
    Track t;
    t.id = p.newId();
    t.type = type;
    p.tracks.push_back(t);
    return p.tracks.back();
}

static Event& event(Project& p, Track& t, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    t.events.push_back(e);
    return t.events.back();
}

static Project baseProject()
{
    Project p;
    p.settings.width = 320;
    p.settings.height = 180;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    p.settings.sampleRate = 48000;
    return p;
}

// A project that shows 'media' (a picture or another project) for 'length' seconds from 0, on one video track.
static Project showing(const std::string& media, double length, double motionW = 1.0)
{
    Project p = baseProject();
    MediaItem m = mediaItem(p, media);
    p.media.push_back(m);
    Track& t = track(p, TrackType::Video);
    event(p, t, m.id, 0, length);
    if (motionW != 1.0) {
        t.motion.resize(1);
        t.motion[0].w = motionW;
        t.motion[0].h = motionW;
    }
    return p;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: nested_test <testfx.ofx> <scratch folder>\n");
        return 2;
    }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);
    const std::string dir = argv[2];
    const std::string red = pathJoin(dir, "red.bmp"), blue = pathJoin(dir, "blue.bmp"), tone = pathJoin(dir, "tone.wav");
    check(writeStill(red, 0, 0, 255) && writeStill(blue, 255, 0, 0) && writeTone(tone, 1.0), "the test media is written");

    check(isProjectFile("a/b/c.veg") && isProjectFile("C:\\x\\Y.VEG") && isProjectFile("p.reno") && !isProjectFile("clip.mp4"),
          "project files are told apart from media files by their extension");

    // ---------------------------------------------------------------- a .reno file as media
    const std::string inner = pathJoin(dir, "inner.reno");
    {
        Project p = showing(red, 2.0, 0.5);         // a red box in the middle, see-through around it
        MediaItem t = mediaItem(p, tone);
        p.media.push_back(t);
        Track& at = track(p, TrackType::Audio);
        event(p, at, t.id, 0, 1.0);
        check(p.save(inner, &err), "the inner project is saved", err);
    }
    std::unique_ptr<MediaReader> r = openMediaOrProject(inner, &err);
    check(r != nullptr, "a .reno file opens as media", err);
    if (r) {
        const MediaInfo& mi = r->info();
        check(mi.hasVideo && mi.hasAudio && mi.width == 320 && mi.height == 180 && std::fabs(mi.fps - 30) < 1e-9 && std::fabs(mi.duration - 2.0) < 1e-9 && !mi.isStill,
              "its media information is the project's: 320x180, 30 fps, 2 s, picture and sound",
              strfmt("%dx%d %.2f fps %.3f s v%d a%d", mi.width, mi.height, mi.fps, mi.duration, mi.hasVideo, mi.hasAudio));
        ImagePtr img = r->videoFrame(0.5, 320, 180);
        check(img && img->w == 320 && img->h == 180 && near(pixel(img, 160, 90), 0, 0, 255) && pixel(img, 160, 90).a == 255,
              "its picture is the project's picture (the red box)", pxs(pixel(img, 160, 90)));
        check(pixel(img, 10, 10).a == 0, "and where nothing covers it, it is see-through", pxs(pixel(img, 10, 10)));
        ImagePtr small = r->videoFrame(0.5, 160, 160);
        check(small && small->w == 160 && small->h == 90, "asked for in a smaller box, it keeps its shape", small ? strfmt("%dx%d", small->w, small->h) : "none");
        ImagePtr again = r->videoFrame(0.51, 320, 180);
        check(again.get() == img.get(), "the same frame asked for again comes from the cache");
        // A smaller preview makes the nested project at that size, its effects too (not at its full size and shrunk
        // afterwards: a project of nested layers cost many times over); a final render keeps making it at full size.
        {
            ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath, nullptr);
            const std::string inv = pathJoin(dir, "inverted.reno");
            Project p = showing(red, 2.0);
            FxSlot s;
            s.id = p.newId();
            s.kind = "ofx";
            s.pluginId = "com.vegascreativesoftware:invert";
            s.name = "Invert";
            p.tracks[0].events[0].fx.push_back(s);
            check(p.save(inv, &err), "a project with an effect on its event is saved", err);
            std::unique_ptr<MediaReader> ir = openMediaOrProject(inv, &err);
            check(ir != nullptr, "  and opens as media", err);
            if (ir) {
                auto invertRow = [] {
                    const profile::Report rep = profile::snapshot();
                    for (const profile::PluginRow& row : rep.plugins)
                        if (row.id.find("invert") != std::string::npos) return row;
                    return profile::PluginRow();
                };
                profile::reset();
                profile::setEnabled(true);
                ImagePtr q = ir->videoFrame(0.5, 80, 45);
                profile::PluginRow row = invertRow();
                check(q && q->w == 80 && q->h == 45 && row.scaledCalls > 0 && row.fullCalls == 0,
                      "  asked for at a quarter of its size, its effect works at that size",
                      strfmt("%dx%d scaled %llu full %llu", q ? q->w : 0, q ? q->h : 0, (unsigned long long)row.scaledCalls, (unsigned long long)row.fullCalls));
                check(near(pixel(q, 40, 22), 255, 255, 0, 12), "  and the picture is the inverted one", pxs(pixel(q, 40, 22)));
                profile::reset();
                {
                    FinalRenderScope final;
                    q = ir->videoFrame(1.0, 80, 45);
                }
                row = invertRow();
                check(q && q->w == 80 && q->h == 45 && row.fullCalls > 0 && row.scaledCalls == 0,
                      "  in a final render it is made at full size and fitted, as before",
                      strfmt("scaled %llu full %llu", (unsigned long long)row.scaledCalls, (unsigned long long)row.fullCalls));
                profile::setEnabled(false);
            }
        }
        ImagePtr past = r->videoFrame(5.0, 320, 180);
        check(past && near(pixel(past, 160, 90), 0, 0, 255), "past its end: its last frame");
        std::vector<float> l(4800), rr(4800);
        bool ok = r->audio(4800, 4800, 48000, l.data(), rr.data());
        float peak = 0;
        for (float v : l) peak = std::max(peak, std::fabs(v));
        check(ok && peak > 0.4f && peak < 0.6f, "its sound is the project's mix (the tone)", strfmt("peak %.3f", peak));
        ok = r->audio(48000 + 4800, 4800, 48000, l.data(), rr.data());
        peak = 0;
        for (float v : l) peak = std::max(peak, std::fabs(v));
        check(ok && peak == 0.f, "and silent where the project is", strfmt("peak %.3f", peak));
        ok = r->audio(-2400, 4800, 44100, l.data(), rr.data());
        peak = 0;
        for (int k = 0; k < 2400; ++k) peak = std::max(peak, std::fabs(l[(size_t)k]));
        float later = 0;
        for (int k = 2400; k < 4800; ++k) later = std::max(later, std::fabs(l[(size_t)k]));
        check(ok && peak == 0.f && later > 0.3f, "at another sample rate too, silent before its start", strfmt("%.3f / %.3f", peak, later));
        // at another rate the project is mixed at its own and resampled: the tone keeps its pitch and level
        std::vector<float> l2(24000), r2(24000);
        ok = r->audio(0, 24000, 24000, l2.data(), r2.data());
        int crossings = 0;
        float top = 0;
        for (int k = 2400; k < 21600; ++k) {
            if ((l2[(size_t)k - 1] < 0) != (l2[(size_t)k] < 0)) ++crossings;
            top = std::max(top, std::fabs(l2[(size_t)k]));
        }
        check(ok && std::abs(crossings - 704) <= 6 && top > 0.45f && top < 0.55f, "read at 24 kHz: the same 440 Hz tone, as loud",
              strfmt("%d crossings in 0.8 s, peak %.3f", crossings, top));
        ok = r->audio(0, 2400, 96000, l2.data(), r2.data());
        crossings = 0;
        for (int k = 1; k < 2400; ++k)
            if ((l2[(size_t)k - 1] < 0) != (l2[(size_t)k] < 0)) ++crossings;
        check(ok && std::abs(crossings - 22) <= 2, "and at 96 kHz", strfmt("%d crossings in 25 ms", crossings));
    }

    // ---------------------------------------------------------------- used in another project
    {
        Project outer = showing(inner, 4.0);
        // a blue track under it shows through the nested project's see-through parts
        MediaItem b = mediaItem(outer, blue);
        outer.media.push_back(b);
        Track& below = track(outer, TrackType::Video);
        event(outer, below, b.id, 0, 4.0);
        MediaPool pool;
        FxRuntime fx;
        VideoEngine video(pool, fx);
        AudioEngine audio(pool, fx);
        FrameOptions fo;
        ImagePtr img = video.render(outer, 1.0, fo);
        check(near(pixel(img, 160, 90), 0, 0, 255) && near(pixel(img, 10, 10), 255, 0, 0),
              "a nested project on a track: its picture, and the track below through its see-through parts",
              pxs(pixel(img, 160, 90)) + " / " + pxs(pixel(img, 10, 10)));
        // its sound in the outer mix
        Track& at = track(outer, TrackType::Audio);
        event(outer, at, outer.media[0].id, 0, 4.0);
        std::vector<float> l(2048), r2(2048);
        audio.render(outer, 4800, 2048, l.data(), r2.data(), true);
        float peak = 0;
        for (float v : l) peak = std::max(peak, std::fabs(v));
        check(peak > 0.3f, "and its sound plays in the outer project's mix", strfmt("peak %.3f", peak));

        // one more level: the outer project saved and nested again
        const std::string outerPath = pathJoin(dir, "outer.reno");
        check(outer.save(outerPath, &err), "a project with a nested project in it is saved", err);
        Project top = showing(outerPath, 4.0);
        MediaPool pool2;
        FxRuntime fx2;
        VideoEngine video2(pool2, fx2);
        img = video2.render(top, 1.0, fo);
        check(near(pixel(img, 160, 90), 0, 0, 255) && near(pixel(img, 10, 10), 255, 0, 0), "a nested project inside a nested project renders too",
              pxs(pixel(img, 160, 90)) + " / " + pxs(pixel(img, 10, 10)));
    }

    // ---------------------------------------------------------------- reading ahead inside nested projects (playback)
    {
        // a nested project with two clips on it, from 0.5 s and from 1.2 s; the outer project plays it from 1 s
        const std::string clip = pathJoin(dir, "clip.mp4");
        check(writeClip(clip, 90), "a video clip is written");
        const std::string warmInner = pathJoin(dir, "warm_inner.reno");
        {
            Project p = baseProject();
            MediaItem c = mediaItem(p, clip);
            p.media.push_back(c);
            Track& t = track(p, TrackType::Video);
            event(p, t, c.id, 0.5, 0.5);
            event(p, t, c.id, 1.2, 1.0);
            check(p.save(warmInner, &err), "a nested project with two clips is saved", err);
        }
        Project outer = baseProject();
        MediaItem n = mediaItem(outer, warmInner);
        outer.media.push_back(n);
        Track& t = track(outer, TrackType::Video);
        const Event nest = event(outer, t, n.id, 1.0, 3.0);
        MediaPool pool;
        FxRuntime fx;
        VideoEngine video(pool, fx);
        FrameOptions fo;
        std::set<uint64_t> claimed;
        auto claim = [&](uint64_t key) { return claimed.insert(key).second; };
        // play at 0.8 s, looking 1.5 s ahead: the nested project plays its own 0 .. 1.3 s, where both clips come up
        int got = video.prewarmNested(outer, nest, 0.8, 2.3, fo, 2.5, nullptr, claim, 1);
        check(got == 1 && claimed.size() == 1, "the nested project's clips coming up are read ahead, one a call when asked for one",
              strfmt("%d read, %d claimed", got, (int)claimed.size()));
        got = video.prewarmNested(outer, nest, 0.8, 2.3, fo, 2.5, nullptr, claim, 2);
        check(got == 1 && claimed.size() == 2, "  then the other one", strfmt("%d read, %d claimed", got, (int)claimed.size()));
        got = video.prewarmNested(outer, nest, 0.8, 2.3, fo, 2.5, nullptr, claim, 2);
        check(got == 0, "  and none again once each was read", strfmt("%d", got));
        // a stretch before the second clip comes up: only the first
        std::set<uint64_t> early;
        got = video.prewarmNested(outer, nest, 0.8, 1.6, fo, 2.5, nullptr, [&](uint64_t k) { return early.insert(k).second; }, 5);
        check(got == 1, "only the clips that come up in the stretch looked at are read", strfmt("%d", got));
        // one level further in: a project that plays the outer one
        const std::string warmOuter = pathJoin(dir, "warm_outer.reno");
        check(outer.save(warmOuter, &err), "the outer project is saved", err);
        Project top = baseProject();
        MediaItem o = mediaItem(top, warmOuter);
        top.media.push_back(o);
        Track& tt = track(top, TrackType::Video);
        const Event nest2 = event(top, tt, o.id, 0.0, 4.0);
        std::set<uint64_t> deep;
        got = video.prewarmNested(top, nest2, 0.0, 3.0, fo, 2.5, nullptr, [&](uint64_t k) { return deep.insert(k).second; }, 5);
        check(got == 2, "the clips of a nested project inside a nested project are read ahead too", strfmt("%d", got));
        // a stop asked for: nothing is read
        std::atomic<bool> stop{true};
        std::set<uint64_t> none;
        got = video.prewarmNested(outer, nest, 0.8, 2.3, fo, 2.5, &stop, [&](uint64_t k) { return none.insert(k).second; }, 5);
        check(got == 0, "told to stop, it reads nothing", strfmt("%d", got));
    }

    // ---------------------------------------------------------------- recursion is refused
    {
        const std::string self = pathJoin(dir, "self.reno");
        Project p = baseProject();
        MediaItem m;
        m.id = p.newId();
        m.path = self;
        m.name = "self.reno";
        p.media.push_back(m);
        Track& t = track(p, TrackType::Video);
        event(p, t, m.id, 0, 2.0);
        check(p.save(self, &err), "a project that uses itself is saved");
        err.clear();
        std::unique_ptr<MediaReader> rs = openMediaOrProject(self, &err);
        check(!rs && err.find("contains itself") != std::string::npos, "a project that uses itself is refused as media", err);

        const std::string a = pathJoin(dir, "loop_a.reno"), b = pathJoin(dir, "loop_b.reno");
        Project pa = baseProject(), pb = baseProject();
        MediaItem ma;
        ma.id = pa.newId();
        ma.path = b;
        pa.media.push_back(ma);
        event(pa, track(pa, TrackType::Video), ma.id, 0, 2.0);
        MediaItem mb;
        mb.id = pb.newId();
        mb.path = pathJoin(dir, ".") + "/loop_a.reno";          // (named another way: still the same file)
        pb.media.push_back(mb);
        event(pb, track(pb, TrackType::Video), mb.id, 0, 2.0);
        pa.save(a, nullptr);
        pb.save(b, nullptr);
        err.clear();
        rs = openMediaOrProject(a, &err);
        check(!rs && err.find("contains itself") != std::string::npos, "two projects that use each other are refused", err);

        // a project that uses one of those is not itself in a circle: it opens (the circular one is just missing in it)
        const std::string c = pathJoin(dir, "uses_loop.reno");
        Project pc = showing(red, 2.0);
        MediaItem mc;
        mc.id = pc.newId();
        mc.path = a;
        pc.media.push_back(mc);
        event(pc, track(pc, TrackType::Video), mc.id, 0, 2.0);
        pc.save(c, nullptr);
        err.clear();
        rs = openMediaOrProject(c, &err);
        ImagePtr img = rs ? rs->videoFrame(1.0, 320, 180) : nullptr;
        check(rs && near(pixel(img, 160, 90), 0, 0, 255), "a project that only uses a circular one still opens and renders", err + " " + pxs(pixel(img, 160, 90)));
    }

    // ---------------------------------------------------------------- a registered loader (the .veg reader's place)
    {
        std::atomic<int> asked{0};
        setProjectFileLoader([&](const std::string& path, Project& out, std::string* error) {
            ++asked;
            if (!endsWithNoCase(path, ".veg")) {
                if (error) *error = "not a VEGAS project";
                return false;
            }
            out = showing(blue, 3.0);
            return true;
        });
        err.clear();
        std::unique_ptr<MediaReader> rv = openMediaOrProject(pathJoin(dir, "made_up.veg"), &err);
        ImagePtr img = rv ? rv->videoFrame(1.0, 320, 180) : nullptr;
        check(rv && std::fabs(rv->info().duration - 3.0) < 1e-9 && near(pixel(img, 100, 100), 255, 0, 0), "a .veg file is read with the registered loader",
              err + " " + pxs(pixel(img, 100, 100)));
        std::unique_ptr<MediaReader> rr = openMediaOrProject(inner, &err);
        check(rr != nullptr && asked > 1, "a .reno file the loader turns down is still read as a Reno project");
        setProjectFileLoader(ProjectFileLoader());
        err.clear();
        rv = openMediaOrProject(pathJoin(dir, "made_up.veg"), &err);
        check(!rv && !err.empty(), "without a loader a .veg file cannot be read here", err);
    }

    // ---------------------------------------------------------------- effects in the nested project
    {
        const std::string withFx = pathJoin(dir, "with_fx.reno");
        Project p = showing(red, 2.0);
        FxSlot inv;
        inv.id = p.newId();
        inv.kind = "ofx";
        inv.pluginId = "reno.test.invert";
        inv.name = "Invert";
        p.tracks[0].events[0].fx.push_back(inv);
        check(p.save(withFx, &err), "a project with an effect is saved", err);
        std::atomic<int> runs{0};
        std::thread::id ranOn;
        setMainThreadRunner([&](const std::function<void()>& fn) {
            ++runs;
            ranOn = std::this_thread::get_id();
            fn();
            return true;
        });
        ImagePtr img;
        std::thread worker([&] {
            std::unique_ptr<MediaReader> rf = openMediaOrProject(withFx, nullptr);
            if (rf) img = rf->videoFrame(1.0, 320, 180);
        });
        worker.join();
        check(runs >= 1, "the nested project's effects are made through the main-thread runner", strfmt("%d runs", (int)runs));
        check(near(pixel(img, 160, 90), 255, 255, 0), "and they run (red inverted is cyan)", pxs(pixel(img, 160, 90)));
        setMainThreadRunner([&](const std::function<void()>&) { return false; });
        // (the instances just made are shared by every reader of the file while any is open, and a while after: let go of)
        releaseUnusedNestedProjects();
        std::unique_ptr<MediaReader> rf = openMediaOrProject(withFx, nullptr);
        img = rf ? rf->videoFrame(1.0, 320, 180) : nullptr;
        check(near(pixel(img, 160, 90), 0, 0, 255), "if the runner cannot run them, it renders without its effects (no crash, no hang)", pxs(pixel(img, 160, 90)));
        rf.reset();
        setMainThreadRunner(MainThreadRunner());

        // ---------------------------------------------------------------- made a few at a time
        // The interface's thread is this one; what is handed to it waits in 'queue' until pump() runs it.
        releaseUnusedNestedProjects();
        std::mutex qm;
        std::deque<std::function<void()>> queue;
        const std::thread::id mainId = std::this_thread::get_id();
        setMainThreadPoster(
            [&](std::function<void()> fn) {
                std::lock_guard<std::mutex> lk(qm);
                queue.push_back(std::move(fn));
            },
            [&] { return std::this_thread::get_id() == mainId; });
        auto pump = [&] {
            int n = 0;
            for (;; ++n) {
                std::function<void()> fn;
                {
                    std::lock_guard<std::mutex> lk(qm);
                    if (queue.empty()) return n;
                    fn = std::move(queue.front());
                    queue.pop_front();
                }
                fn();
            }
        };
        std::atomic<int> told{0};
        std::string toldPath;
        setNestedProjectReadyHook([&](const std::string& path) {
            ++told;
            toldPath = path;
        });
        const uint64_t gen0 = nestedProjectsGeneration();
        uint64_t leftOut = 0;
        img.reset();
        std::thread w1([&] {
            std::unique_ptr<MediaReader> rd = openMediaOrProject(withFx, nullptr);
            const uint64_t l0 = nestedEffectsLeftOut();
            if (rd) img = rd->videoFrame(1.0, 320, 180);
            leftOut = nestedEffectsLeftOut() - l0;
        });
        w1.join();
        check(near(pixel(img, 160, 90), 0, 0, 255), "with a poster: the first picture comes at once, without the effects being made", pxs(pixel(img, 160, 90)));
        check(leftOut == 1, "and the thread is told it left them out", strfmt("%llu", (unsigned long long)leftOut));
        bool queued;
        {
            std::lock_guard<std::mutex> lk(qm);
            queued = !queue.empty();
        }
        check(queued && told == 0, "making them is handed to the interface's thread");
        pump();
        check(told == 1 && toldPath == withFx && nestedProjectsGeneration() > gen0, "once they are made the interface is told", toldPath);
        std::thread w2([&] {
            std::unique_ptr<MediaReader> rd = openMediaOrProject(withFx, nullptr);
            if (rd) img = rd->videoFrame(1.0, 320, 180);
        });
        w2.join();
        check(near(pixel(img, 160, 90), 255, 255, 0), "and the pictures made after that have them", pxs(pixel(img, 160, 90)));
        pump();

        // a final render (a file being written) waits for them while the interface makes them
        releaseUnusedNestedProjects();
        pump();
        ImagePtr fin;
        std::atomic<bool> done{false};
        std::thread w3([&] {
            const FinalRenderScope finalScope;
            std::unique_ptr<MediaReader> rd = openMediaOrProject(withFx, nullptr);
            if (rd) fin = rd->videoFrame(1.0, 320, 180);
            done = true;
        });
        int steps = 0;
        while (!done) {
            steps += pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        w3.join();
        pump();
        check(steps >= 1 && near(pixel(fin, 160, 90), 255, 255, 0), "a final render waits for the effects", pxs(pixel(fin, 160, 90)));

        // the interface's thread itself asking: handed over too (it never waits for itself)
        releaseUnusedNestedProjects();
        pump();
        {
            std::unique_ptr<MediaReader> rd = openMediaOrProject(withFx, nullptr);
            ImagePtr a = rd ? rd->videoFrame(1.0, 320, 180) : nullptr;
            pump();
            ImagePtr b = rd ? rd->videoFrame(1.0, 320, 180) : nullptr;
            check(near(pixel(a, 160, 90), 0, 0, 255) && near(pixel(b, 160, 90), 255, 255, 0),
                  "asked for on the interface's thread: at once without them, with them once made", pxs(pixel(a, 160, 90)) + " / " + pxs(pixel(b, 160, 90)));
        }
        pump();
        setNestedProjectReadyHook(nullptr);
        setMainThreadPoster(MainThreadPoster(), std::function<bool()>());
    }

    // ---------------------------------------------------------------- one copy of each project file
    {
        const std::string once = pathJoin(dir, "once.reno");
        Project p = showing(red, 1.0);
        check(p.save(once, &err), "a project is saved", err);
        std::atomic<int> reads{0};
        setProjectFileLoader([&](const std::string& path, Project& out, std::string* e) {
            ++reads;
            return out.load(path, e);
        });
        std::unique_ptr<MediaReader> a = openMediaOrProject(once, nullptr), b = openMediaOrProject(once, nullptr);
        Project outer = showing(once, 1.0);
        std::unique_ptr<MediaReader> c = openMediaOrProject(once, nullptr);
        check(a && b && c && reads == 1, "a project file used by many readers is read once", strfmt("%d reads", (int)reads));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Project changed = showing(blue, 1.5);
        check(changed.save(once, &err), "the file is saved again", err);
        std::unique_ptr<MediaReader> d = openMediaOrProject(once, nullptr);
        check(d && reads == 2 && std::fabs(d->info().duration - 1.5) < 1e-9, "and read again when it changes", strfmt("%d reads", (int)reads));
        check(mediaSignature(once) != mediaSignature(red) && !mediaSignature(once).empty() && mediaSignature(pathJoin(dir, "none.reno")).empty(),
              "its signature (for caches on disk) is its own; a missing file has none");
        setProjectFileLoader(ProjectFileLoader());
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
