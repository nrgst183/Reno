// Tests of plug-in instances and the threads that render with them (build under ASan + UBSan).
//
//   fxruntime_test <testfx.ofx>
//
// Instances removed (File > New, FxRuntime::clear, a frame size change) while a render holds them must stay alive
// until the render is done (FxRuntime::Hold).
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "../../src/ofx/probe.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace reno;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::printf("PASS %s\n", what.c_str()); }
    else { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    std::fflush(stdout);
}

namespace {
class Pattern : public MediaReader {
public:
    MediaInfo mi;
    Pattern()
    {
        mi.hasVideo = true;
        mi.hasAudio = true;
        mi.width = 160;
        mi.height = 90;
        mi.fps = 30;
        mi.duration = 10;
        mi.sampleRate = 48000;
        mi.channels = 2;
    }
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override { return ofx::makeTestPattern(std::min(maxW, 160), std::min(maxH, 90), seconds * 30); }
    bool audio(int64_t, int frames, int, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) l[k] = r[k] = 0.1f;
        return true;
    }
};

Project makeProject(int width)
{
    Project p;
    p.settings.width = width;
    p.settings.height = width * 9 / 16;
    MediaItem m;
    m.id = p.newId();
    m.path = "pattern";
    m.info = Pattern().mi;
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 10;
    for (const char* id : {"reno.test.invert", "reno.test.params"}) {
        FxSlot s;
        s.id = p.newId();
        s.kind = "ofx";
        s.pluginId = id;
        s.name = id;
        e.fx.push_back(s);
    }
    t.events.push_back(e);
    FxSlot ts;
    ts.id = p.newId();
    ts.kind = "ofx";
    ts.pluginId = "reno.test.invert";
    t.fx.push_back(ts);
    p.tracks.push_back(t);
    return p;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: fxruntime_test testfx.ofx\n"); return 2; }
    Catalog::get().scanInProcess({pathDir(argv[1])}, std::vector<std::string>());
    const Project small = makeProject(320), large = makeProject(480);

    MediaPool pool;
    pool.setOpener([](const std::string&, std::string*) { return std::unique_ptr<MediaReader>(new Pattern()); });
    FxRuntime fx;
    fx.prepare(small);
    check(fx.video(small.tracks[0].events[0].fx[0].id) != nullptr, "the test effect is made");

    // a picture thread rendering all the time, as Playback's does, and the interface taking the instances away
    std::atomic<bool> stop{false};
    std::atomic<int> frames{0}, inverted{0};
    std::atomic<int> which{0};          // 0: render 'small', 1: 'large'
    std::thread render([&] {
        VideoEngine video(pool, fx);
        FrameOptions opt;
        while (!stop) {
            const Project& p = which.load() ? large : small;
            ImagePtr img = video.render(p, 1.0 + (frames % 30) / 30.0, opt);
            if (img) ++frames;
        }
    });
    std::thread sound([&] {
        AudioEngine audio(pool, fx);
        std::vector<float> l(1024), r(1024);
        int64_t at = 0;
        while (!stop) {
            audio.render(small, at, 1024, l.data(), r.data(), true);
            at += 1024;
        }
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    int clears = 0, remakes = 0;
    while (std::chrono::steady_clock::now() < until) {
        // File > New: every instance goes
        fx.clear();
        ++clears;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        fx.prepare(small);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        // Project Properties: another frame size - the instances are made again for it
        which = 1;
        fx.prepare(large);
        ++remakes;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        which = 0;
        fx.prepare(small);
        ++remakes;
    }
    stop = true;
    render.join();
    sound.join();
    (void)inverted;
    check(frames > 0, strfmt("pictures rendered while the instances were taken away (%d frames, %d clears, %d remakes)", frames.load(), clears, remakes));
    // with nothing rendering, the next prepare() destroys what was kept aside, and the effects still work
    fx.prepare(small);
    VideoEngine video(pool, fx);
    FrameOptions opt;
    check(video.render(small, 2.0, opt) != nullptr && fx.video(small.tracks[0].events[0].fx[0].id) != nullptr, "afterwards the effects are there and render");
    fx.clear();
    check(fx.video(small.tracks[0].events[0].fx[0].id) == nullptr, "clear() takes them away");

    // prepareSome: a piece at a time (one slot per call with no time to spare), and only the kinds asked for
    {
        FxRuntime some;
        size_t next = 0;
        int calls = 0;
        while (!some.prepareSome(small, next, 0.0) && calls < 100) ++calls;
        check(calls == 2 && next == 3, strfmt("prepareSome with no time makes one slot a call (%d calls before the last, %zu slots)", calls, next));
        bool all = true;
        for (const FxSlot* s : {&small.tracks[0].events[0].fx[0], &small.tracks[0].events[0].fx[1], &small.tracks[0].fx[0]})
            all = all && some.video(s->id) != nullptr;
        check(all, "and then every slot has its instance, as prepare() makes them");
        check(some.prepareSome(small, next, 0.0) && next == 3, "asked again when done: nothing more to do");
        FxRuntime audioOnly;
        size_t n2 = 0;
        check(audioOnly.prepareSome(small, n2, 1e9, false, true) && audioOnly.video(small.tracks[0].fx[0].id) == nullptr,
              "asked for audio effects only, it makes no video ones");
    }
    std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
