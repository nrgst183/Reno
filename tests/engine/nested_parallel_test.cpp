// Tests of rendering nested projects' pictures on several threads: the nestedVideoParallelSafe verdict, and that two
// threads rendering frames of a project with nested projects get the same pictures as one thread.
//
//   nested_parallel_test <testfx.ofx> <scratch folder>
#include "../../src/engine/engine.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

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
            d[0] = (uint8_t)(b + x / 4); d[1] = (uint8_t)(g + y); d[2] = (uint8_t)r; d[3] = 255;
        }
    return w->writeVideo(*img) && w->finish(nullptr);
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

static Event& addEvent(Project& p, Track& t, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    t.events.push_back(e);
    return t.events.back();
}

static Project base()
{
    Project p;
    p.settings.width = 320;
    p.settings.height = 180;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    p.settings.sampleRate = 48000;
    return p;
}

static Project showing(const std::string& media, double length, bool invert)
{
    Project p = base();
    MediaItem m = mediaItem(p, media);
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    p.tracks.push_back(t);
    Event& e = addEvent(p, p.tracks[0], m.id, 0, length);
    if (invert) {
        FxSlot inv;
        inv.id = p.newId();
        inv.kind = "ofx";
        inv.pluginId = "reno.test.invert";
        inv.name = "Invert";
        e.fx.push_back(inv);
    }
    return p;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: nested_parallel_test <testfx.ofx> <scratch folder>\n");
        return 2;
    }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);
    const std::string dir = argv[2];
    const std::string red = pathJoin(dir, "pred.bmp"), blue = pathJoin(dir, "pblue.bmp");
    check(writeStill(red, 0, 0, 255) && writeStill(blue, 200, 20, 0), "the test media is written");
    const std::string withFx = pathJoin(dir, "p_with_fx.reno"), plain = pathJoin(dir, "p_plain.reno"), outerFile = pathJoin(dir, "p_outer.reno");
    check(showing(red, 2.0, true).save(withFx, &err) && showing(blue, 2.0, false).save(plain, &err), "the nested projects are saved", err);

    // the effects are made on the spot, as the command-line tools do
    setMainThreadRunner([&](const std::function<void()>& fn) {
        fn();
        return true;
    });

    Project outer = base();
    MediaItem a = mediaItem(outer, withFx), b = mediaItem(outer, plain);
    outer.media.push_back(a);
    outer.media.push_back(b);
    Track v1, v2;
    v1.id = outer.newId();
    v1.type = v2.type = TrackType::Video;
    v2.id = outer.newId();
    outer.tracks.push_back(v1);
    outer.tracks.push_back(v2);
    addEvent(outer, outer.tracks[0], a.id, 0, 2.0);
    addEvent(outer, outer.tracks[1], b.id, 0, 2.0).fx.clear();
    outer.tracks[1].motion.resize(1);
    outer.tracks[1].motion[0].w = 0.5;
    outer.tracks[1].motion[0].h = 0.5;

    check(nestedVideoParallelSafe(showing(red, 1.0, false)), "a project showing only a picture has nothing nested to hold it back");
    check(nestedVideoParallelSafe(outer), "nested projects whose effects are all safe: several threads may render them");

    // frames rendered one after the other, and by two threads at once (two engines on shared effects and files, as the picture threads have)
    FxRuntime fx;
    MediaPool pool;
    outer.settings.width = 320;
    fx.prepare(outer);
    const int frames = 40;
    std::vector<ImagePtr> serial((size_t)frames), par((size_t)frames);
    {
        VideoEngine ve(pool, fx);
        FrameOptions fo;
        for (int f = 0; f < frames; ++f) serial[(size_t)f] = ve.render(outer, outer.frameTime(f), fo);
    }
    {
        std::atomic<int> next{0};
        auto work = [&] {
            VideoEngine ve(pool, fx);
            FrameOptions fo;
            for (;;) {
                const int f = next.fetch_add(1);
                if (f >= frames) return;
                par[(size_t)f] = ve.render(outer, outer.frameTime(f), fo);
            }
        };
        std::thread t1(work), t2(work);
        t1.join();
        t2.join();
    }
    int same = 0;
    bool anyNull = false;
    for (int f = 0; f < frames; ++f) {
        const ImagePtr &x = serial[(size_t)f], &y = par[(size_t)f];
        if (!x || !y) { anyNull = true; continue; }
        if (x->w == y->w && x->h == y->h && x->rowBytes == y->rowBytes && std::memcmp(x->data.get(), y->data.get(), x->rowBytes * (size_t)x->h) == 0) ++same;
    }
    check(!anyNull && same == frames, "two threads render the same pictures of a project showing nested projects", std::to_string(same) + " of " + std::to_string(frames));

    // an unsafe effect inside a nested project: one thread
    ofx::Plugin* pl = ofx::Host::get().findPlugin("reno.test.invert");
    check(pl != nullptr, "the effect is found");
    if (pl) {
        const int was = pl->safety;
        pl->safety = 0;
        check(!nestedVideoParallelSafe(outer), "an unsafe effect in a nested project keeps playing to one thread");
        // ... unless that project is not shown by any video event
        Project other = outer;
        other.tracks[0].events.clear();
        check(nestedVideoParallelSafe(other), "(a nested project nothing shows does not count)");
        pl->safety = was;
        check(nestedVideoParallelSafe(outer), "and several threads again once it is safe");
    }
    // nesting deeper: a project showing the project with the effect
    {
        Project mid = base();
        MediaItem m = mediaItem(mid, withFx);
        mid.media.push_back(m);
        Track t;
        t.id = mid.newId();
        t.type = TrackType::Video;
        mid.tracks.push_back(t);
        addEvent(mid, mid.tracks[0], m.id, 0, 2.0);
        const std::string midFile = pathJoin(dir, "p_mid.reno");
        check(mid.save(midFile, &err), "a project showing the one with the effect is saved", err);
        Project top = base();
        MediaItem tm = mediaItem(top, midFile);
        top.media.push_back(tm);
        Track tt;
        tt.id = top.newId();
        tt.type = TrackType::Video;
        top.tracks.push_back(tt);
        addEvent(top, top.tracks[0], tm.id, 0, 2.0);
        check(nestedVideoParallelSafe(top), "nested two deep: safe");
        if (ofx::Plugin* p2 = ofx::Host::get().findPlugin("reno.test.invert")) {
            const int was = p2->safety;
            p2->safety = 0;
            check(!nestedVideoParallelSafe(top), "nested two deep: the unsafe effect at the bottom is found");
            p2->safety = was;
        }
    }
    // effects not made yet: not known, so not safe yet
    {
        setMainThreadRunner([&](const std::function<void()>&) { return false; });
        releaseUnusedNestedProjects();
        const std::string fresh = pathJoin(dir, "p_fresh.reno");
        Project f = showing(red, 2.0, true);
        f.save(fresh, &err);
        Project top = base();
        MediaItem tm = mediaItem(top, fresh);
        top.media.push_back(tm);
        Track tt;
        tt.id = top.newId();
        tt.type = TrackType::Video;
        top.tracks.push_back(tt);
        addEvent(top, top.tracks[0], tm.id, 0, 2.0);
        check(!nestedVideoParallelSafe(top), "effects not made yet: not known to be safe");
    }
    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
