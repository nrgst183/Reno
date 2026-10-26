// Tests of rendering pictures on several threads with OpenFX effects of every render thread safety:
//  - RenderGate (ofx/rendergate.h): exclusion, re-entry on the same thread, refusing a request that would deadlock;
//  - FxRuntime lanes: each picture thread gets its own instances with the live settings; output equals a single
//    thread; frames of one event render concurrently for safe effects and never two at once for "unsafe" ones.
// Run it once for each safety:   TESTFX_SAFETY=unsafe|instance (unset: fully safe)  lanes_test <testfx.ofx>
#include "../../src/engine/engine.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <future>
#include <thread>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- the media: "pat:N", a moving pattern, 30 fps, 480x270
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
                d[0] = (uint8_t)(x * 255 / w + frame * 3 + seed);
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
    r->mi.width = 480;
    r->mi.height = 270;
    r->mi.fps = 30;
    r->mi.duration = 60;
    r->seed = std::atoi(path.c_str() + 4);
    return std::unique_ptr<MediaReader>(r.release());
}

static bool same(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b) return !a && !b;
    if (a->w != b->w || a->h != b->h || a->depth != b->depth) return false;
    for (int y = 0; y < a->h; ++y)
        if (std::memcmp(a->row(y), b->row(y), (size_t)a->w * a->pixelBytes()) != 0) return false;
    return true;
}

// ---- the gate
static void gateTests()
{
    using ofx::RenderGate;
    {
        RenderGate g;
        long counter = 0;
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i)
            ts.emplace_back([&] {
                for (int k = 0; k < 3000; ++k) {
                    RenderGate::Hold h(g);
                    if (!h.ok()) continue;
                    const long v = counter;
                    if (k % 500 == 0) std::this_thread::yield();
                    counter = v + 1;
                }
            });
        for (auto& t : ts) t.join();
        check(counter == 12000, "a gate lets one thread in at a time", std::to_string(counter));
    }
    {
        RenderGate g;
        RenderGate::Hold a(g);
        RenderGate::Hold b(g);
        check(a.ok() && b.ok(), "the thread that holds a gate may take it again");
        std::atomic<int> got{0};
        std::thread t([&] { RenderGate::Hold h(g); got = h.ok() ? 1 : 2; });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(got.load() == 0, "another thread waits while it is held");
        b.release();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(got.load() == 0, "(taken twice: let go once, still held)");
        a.release();
        t.join();
        check(got.load() == 1, "and gets it when it is let go");
    }
    {
        // two threads each hold one gate and ask for the other's: one is refused, the other gets it once that one lets go
        for (int round = 0; round < 20; ++round) {
            RenderGate g1, g2;
            std::atomic<int> ready{0};
            std::atomic<int> refused{0}, taken{0};
            auto worker = [&](RenderGate& first, RenderGate& second) {
                RenderGate::Hold h1(first);
                ready++;
                while (ready.load() < 2) std::this_thread::yield();
                std::this_thread::sleep_for(std::chrono::milliseconds(round % 3));
                RenderGate::Hold h2(second);
                if (h2.ok()) taken++;
                else refused++;
            };
            std::future<void> done = std::async(std::launch::async, [&] {
                std::thread a(worker, std::ref(g1), std::ref(g2));
                std::thread b(worker, std::ref(g2), std::ref(g1));
                a.join();
                b.join();
            });
            const bool finished = done.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
            if (!finished) {
                check(false, "two threads asking for each other's gates do not wait for ever");
                std::fflush(stdout);
                std::_Exit(1);
            }
            if (refused.load() < 1 || taken.load() + refused.load() != 2) {
                check(false, "a circle of waiting is refused once", "refused " + std::to_string(refused.load()) + " taken " + std::to_string(taken.load()));
                return;
            }
        }
        check(true, "two threads asking for each other's gates: one is refused, none waits for ever (20 rounds)");
    }
}

// ---- the project: one video track, events of 'frames' frames each with the invert effect (slow, so threads show)
static Project build(int events, double eventLen)
{
    Project p;
    p.settings.width = 480;
    p.settings.height = 270;
    p.settings.fpsNum = 30;
    MediaItem m;
    m.id = p.newId();
    m.path = "pat:1";
    m.name = "pat";
    m.info = testOpen(m.path, nullptr)->info();
    p.media.push_back(m);
    Track tr;
    tr.id = p.newId();
    tr.type = TrackType::Video;
    for (int i = 0; i < events; ++i) {
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.start = i * eventLen;
        e.length = eventLen;
        FxSlot s;
        s.id = p.newId();
        s.kind = "ofx";
        s.pluginId = "reno.test.invert";
        s.name = "Invert";
        e.fx.push_back(s);
        tr.events.push_back(e);
    }
    p.tracks.push_back(tr);
    return p;
}

// Frames 0 .. n-1 (of 30 fps) rendered by 'threads' threads, thread i taking frames i, i+threads, ... on lane i (0: none).
static double renderAll(const Project& p, FxRuntime& fx, int threads, int n, bool useLanes, std::vector<ImagePtr>& out)
{
    MediaPool pool;
    pool.setOpener(testOpen);
    VideoEngine video(pool, fx);
    out.assign((size_t)n, ImagePtr());
    const double t0 = now();
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i)
        ts.emplace_back([&, i] {
            FxRuntime::setThreadLane(useLanes ? i : 0);
            FrameOptions fo;
            fo.parallel = false;
            for (int f = i; f < n; f += threads) out[(size_t)f] = video.render(p, f / 30.0 + 1e-6, fo);
            FxRuntime::setThreadLane(0);
        });
    for (auto& t : ts) t.join();
    return now() - t0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: [TESTFX_SAFETY=unsafe|instance] lanes_test <testfx.ofx>\n");
        return 2;
    }
    const char* safetyEnv = std::getenv("TESTFX_SAFETY");
    const std::string mode = safetyEnv ? safetyEnv : "full";
    std::printf("effects say: %s\n", mode.c_str());
    gateTests();

    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);
    setMainThreadRunner([&](const std::function<void()>& fn) {
        fn();
        return true;
    });
    const bool unsafe = mode == "unsafe";
    const int T = 4, N = 24;                            // threads; frames (all in one event)
    Project p = build(2, 1.0);                          // frames 0..23 are in the first event
    FxRuntime fx;
    fx.prepare(p);
    ofx::Instance* live = fx.video(p.tracks[0].events[0].fx[0].id);
    check(live != nullptr, "the effect has an instance");
    if (!live) return 1;
    check((live->plugin->safety == 0) == unsafe, "the plug-in says what the test asked", std::to_string(live->plugin->safety));
    check(fx.laneInstances() == 0, "no lane instances before any are asked for");

    // the pictures one thread makes (fast: no delay)
    unsetenv("TESTFX_SLOW_RENDER_MS");
    std::vector<ImagePtr> ref, par;
    renderAll(p, fx, 1, N, false, ref);
    bool all = true;
    for (auto& i : ref) all = all && i;
    check(all, "the reference pictures are made");

    // lanes are made for the events in the window
    check(fx.prepareLanes(p, T, 0.0, 0.5, 1.0e9), "the lane instances are made (window of the first event)");
    const int lanesMade = fx.laneInstances();
    if (unsafe) check(lanesMade == 0, "unsafe effects get no lane instances", std::to_string(lanesMade));
    else check(lanesMade == (T - 1), "the first event's effect gets an instance for each other thread", std::to_string(lanesMade));
    check(fx.prepareLanes(p, T, 0.0, 0.5, 1.0e9) && fx.laneInstances() == lanesMade, "asking again makes nothing more");
    check(fx.prepareLanes(p, T, 0.0, 2.0, 1.0e9), "(window of both events)");
    check(fx.laneInstances() == (unsafe ? 0 : 2 * (T - 1)), "the second event's too", std::to_string(fx.laneInstances()));

    // the same pictures from threads on lanes
    renderAll(p, fx, T, N, true, par);
    int differ = 0;
    for (int f = 0; f < N; ++f) if (!same(ref[(size_t)f], par[(size_t)f])) ++differ;
    check(differ == 0, "pictures rendered on " + std::to_string(T) + " threads on their lanes are exactly one thread's", std::to_string(differ) + " differ");

    // frames take time: with lanes they overlap (unless the effect is unsafe: then one at a time in the plug-in)
    setenv("TESTFX_SLOW_RENDER_MS", "40", 1);
    std::vector<ImagePtr> junk;
    const double serial = renderAll(p, fx, 1, N, false, junk);
    const double shared = renderAll(p, fx, T, N, false, junk);          // all threads on the live instance
    const double laned = renderAll(p, fx, T, N, true, junk);
    std::printf("  %d frames of 40 ms: one thread %.0f ms; %d threads on one instance %.0f ms; %d threads on their lanes %.0f ms\n", N, serial * 1000, T,
                shared * 1000, T, laned * 1000);
    check(shared > serial * 0.8, "threads on one instance take turns");
    if (unsafe) check(laned > serial * 0.8, "an unsafe effect is rendered one picture at a time whatever the threads");
    else check(laned < serial * 0.6, "threads on their lanes render at the same time");

    // settings changes reach the lanes' instances
    unsetenv("TESTFX_SLOW_RENDER_MS");
    ofx::Param* zero = live->params.find("ZeroRed");
    check(zero != nullptr, "the effect has the ZeroRed setting");
    if (zero) {
        live->userSetValue(zero, {1.0}, 0, false);
        std::vector<ImagePtr> ref2, par2;
        renderAll(p, fx, 1, N, false, ref2);
        const uint64_t before = fx.laneSyncs();
        renderAll(p, fx, T, N, true, par2);
        int d2 = 0, changed = 0;
        for (int f = 0; f < N; ++f) {
            if (!same(ref2[(size_t)f], par2[(size_t)f])) ++d2;
            if (!same(ref[(size_t)f], ref2[(size_t)f])) ++changed;
        }
        check(changed == N, "the setting changes the pictures", std::to_string(changed));
        check(d2 == 0, "a changed setting is what the lanes render with", std::to_string(d2) + " differ");
        if (!unsafe) check(fx.laneSyncs() > before && fx.laneSyncs() - before <= (uint64_t)(T - 1), "the lanes took the new setting once each", std::to_string(fx.laneSyncs() - before));
        const uint64_t again = fx.laneSyncs();
        std::vector<ImagePtr> par3;
        renderAll(p, fx, T, N, true, par3);
        check(fx.laneSyncs() == again, "and no more times while it does not change");
    }

    // the effects made again (the project's size changed): the lanes go with the old instances, and new ones are made
    p.settings.width = 320;
    p.settings.height = 180;
    fx.prepare(p);
    check(fx.laneInstances() == 0, "effects made again have no lane instances yet", std::to_string(fx.laneInstances()));
    check(fx.prepareLanes(p, T, 0.0, 0.5, 1.0e9), "(made again)");
    check(fx.laneInstances() == (unsafe ? 0 : T - 1), "lanes made for the new instance", std::to_string(fx.laneInstances()));

    // a budget stops the making and the next ask goes on
    FxRuntime fx2;
    Project big = build(12, 0.5);
    fx2.prepare(big);
    const bool doneAtOnce = fx2.prepareLanes(big, T, 0.0, 6.0, 0.0);
    if (!unsafe) check(!doneAtOnce && fx2.laneInstances() < 12 * (T - 1), "with no time to spare, only a little is made", std::to_string(fx2.laneInstances()));
    int rounds = 0;
    while (!fx2.prepareLanes(big, T, 0.0, 6.0, 0.0) && rounds < 1000) ++rounds;
    check(fx2.laneInstances() == (unsafe ? 0 : 12 * (T - 1)), "asked again and again, they are all made", std::to_string(fx2.laneInstances()));

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
