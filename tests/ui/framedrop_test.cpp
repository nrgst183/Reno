// Playing while a video effect is too slow to keep up. As in VEGAS, sound and cursor go on in real time and late pictures are
// dropped: each picture is rendered for the time the sound will be at when it is ready, shown only while still current, never
// queued; a late render is abandoned (OpenFX effects that ask abort() are told to stop). Stop, pause and seek never wait for a
// picture. The effect is the test plug-in's Test Invert slowed by TESTFX_SLOW_RENDER_MS (tests/ofx/testfx.cpp); pictures say
// their frame number, the sound says its time, and goes to a stand-in sound card that takes it in real time on its own thread.
//
//   framedrop_test <output folder> <testfx.ofx>
// With -DFRAMEDROP_OLD it uses only what Playback had before frame dropping, to measure the old behaviour.
#include "../../src/app/playback.h"
#include "../../src/engine/renderas.h"
#include "uitestlib.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

static const double kFps = 30.0;

// "frames:" 20 s of 320x180 pictures whose blue / green say the frame number; "ramp:" 20 s of sound whose value is seconds / 20,
// so any gap or jump shows.
class Reader : public MediaReader {
public:
    MediaInfo mi;
    std::string kind;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        (void)maxW;
        (void)maxH;
        if (kind != "frames") return nullptr;
        ImagePtr img = Image::make(320, 180, PixelDepth::Byte, false);
        const int f = std::max(0, std::min(599, (int)std::floor(seconds * kFps + 1e-6)));
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = (uint8_t)(f % 256); d[1] = (uint8_t)(f / 256); d[2] = 0; d[3] = 255;
            }
        return img;
    }
    bool audio(int64_t start, int frames, int rate, float* l, float* r) override
    {
        if (kind != "ramp") return false;
        for (int k = 0; k < frames; ++k) {
            const double t = (double)(start + k) / rate;
            l[k] = r[k] = (t >= 0 && t < 20.0) ? (float)(t / 20.0) : 0.f;
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> opener(const std::string& path, std::string* error)
{
    std::unique_ptr<Reader> r(new Reader());
    r->kind = path.substr(0, path.find(':'));
    r->mi.path = path;
    r->mi.duration = 20;
    if (r->kind == "frames") {
        r->mi.hasVideo = true;
        r->mi.width = 320;
        r->mi.height = 180;
        r->mi.fps = kFps;
    } else if (r->kind == "ramp") {
        r->mi.hasAudio = true;
        r->mi.sampleRate = 48000;
        r->mi.channels = 2;
    } else {
        if (error) *error = "unknown";
        return nullptr;
    }
    return std::unique_ptr<MediaReader>(r.release());
}

// A sound card: takes the sound in real time on its own thread (5 ms at a time) and keeps it.
class RealtimeOutput : public AudioOutput {
public:
    ~RealtimeOutput() override { stop(); }
    bool start(int sampleRate, Pull pull) override
    {
        stop();
        {
            std::lock_guard<std::mutex> lk(m);
            got.clear();
        }
        run = true;
        thread = std::thread([this, sampleRate, pull] {
            std::vector<float> l(4096), r(4096);
            const auto t0 = std::chrono::steady_clock::now();
            int64_t taken = 0;
            while (run) {
                const int64_t due = (int64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * sampleRate);
                while (run && taken < due) {
                    const int n = (int)std::min<int64_t>(240, due - taken);
                    pull(l.data(), r.data(), n);
                    taken += n;
                    std::lock_guard<std::mutex> lk(m);
                    got.insert(got.end(), l.begin(), l.begin() + n);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        return true;
    }
    void stop() override
    {
        run = false;
        if (thread.joinable()) thread.join();
    }
    std::string name() const override { return "test"; }
    std::vector<float> taken()
    {
        std::lock_guard<std::mutex> lk(m);
        return got;
    }
    std::atomic<bool> run{false};
    std::thread thread;
    std::mutex m;
    std::vector<float> got;
};

// How the sound went: samples taken, and gaps (silence or a jump in the ramp) after it began.
struct SoundCheck { size_t samples = 0, gaps = 0; double firstValue = 0, lastValue = 0; };
static SoundCheck checkSound(const std::vector<float>& s)
{
    SoundCheck c;
    c.samples = s.size();
    const double step = 1.0 / (20.0 * 48000.0);
    size_t k = 0;
    while (k < s.size() && s[k] == 0.f) ++k;  // until the first sound reaches the card
    if (k < s.size()) c.firstValue = s[k];
    for (size_t j = k + 1; j < s.size(); ++j) {
        const double d = (double)s[j] - (double)s[j - 1];
        if (!(d > step * 0.25 && d < step * 4.0)) ++c.gaps;
    }
    if (!s.empty()) c.lastValue = s.back();
    return c;
}

struct Shown { int frame; double pos; double wall; bool playing; };

static int frameOf(const QImage& img)
{
    if (img.isNull()) return -1;
    const QRgb px = img.pixel(img.width() / 2, img.height() / 2);
    // inverted by the effect: alpha minus colour
    return (255 - qBlue(px)) + 256 * (255 - qGreen(px));
}

static double now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Runs the event loop for 'seconds'; returns the longest stretch it went without running (the UI's worst stall).
static double runFor(double seconds)
{
    const double until = now() + seconds;
    double last = now(), worst = 0;
    while (now() < until) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const double t = now();
        worst = std::max(worst, t - last);
        last = t;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return worst;
}

// Times one call in milliseconds.
template <class F>
static double timed(F f)
{
    const double t0 = now();
    f();
    return (now() - t0) * 1000.0;
}

struct PlayStats {
    double seconds = 0, cursorMoved = 0, posMoved = 0;
    size_t shown = 0, near1 = 0, near2 = 0;
    double worstOff = 0, medianOff = 0, fps = 0, worstStall = 0;
    SoundCheck sound;
    uint64_t dropped = 0;
    size_t backwards = 0;  // pictures shown after a later frame's (forwards, no loop: none)
};

static void print(const char* what, const PlayStats& s)
{
    std::printf("MEASURE %s: %.2f s played, cursor moved %.3f s, %zu pictures shown (%.1f a second; preview says %.2f fps), "
                "dropped %llu; shown picture vs sound: median %.1f ms off, worst %.1f ms, %zu within 1 frame, %zu within 2; "
                "sound: %zu samples, %zu gaps; worst interface stall %.0f ms\n",
                what, s.seconds, s.cursorMoved, s.shown, s.shown / std::max(0.001, s.seconds), s.fps, (unsigned long long)s.dropped,
                s.medianOff * 1000, s.worstOff * 1000, s.near1, s.near2, s.sound.samples, s.sound.gaps, s.worstStall * 1000);
}

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    QApplication::setOrganizationName("RenoFramedropTest");
    QApplication::setApplicationName("framedrop_test");
    if (argc < 3) {
        std::printf("usage: framedrop_test <output folder> <testfx.ofx>\n");
        return 2;
    }
    const std::string fxPath = argv[2];
    uitest::setup({fxPath}, std::vector<std::string>());
    void* fxlib = dlopen(fxPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto counter = fxlib ? (int (*)(const char*))dlsym(fxlib, "testfx_counter") : nullptr;
    auto count = [&](const char* n) { return counter ? counter(n) : -1; };

    AppState& app = AppState::get();
    app.prefs.ramPreviewMb = 0;  // no RAM cache: played pictures would be replayed from memory, and this test is about rendering
    app.mediaOpener = opener;
    app.newProject();
    app.edit("settings", [](Project& p) {
        p.settings.width = 320;
        p.settings.height = 180;
        p.settings.fpsNum = 30;
        p.settings.fpsDen = 1;
        p.settings.sampleRate = 48000;
    });
    uint64_t videoEvent = 0, videoTrack = 0;
    app.edit("clips", [&](Project& p) {
        for (const char* path : {"frames:", "ramp:"}) {
            MediaItem m;
            m.id = p.newId();
            m.path = path;
            m.name = path;
            m.info = opener(path, nullptr)->info();
            p.media.push_back(m);
            Track t;
            t.id = p.newId();
            t.type = m.info.hasVideo ? TrackType::Video : TrackType::Audio;
            Event e;
            e.id = p.newId();
            e.mediaId = m.id;
            e.length = 20;
            t.events.push_back(e);
            if (m.info.hasVideo) {
                videoEvent = e.id;
                videoTrack = t.id;
            }
            p.tracks.push_back(t);
        }
    });
    FxTarget target;
    target.kind = FxTarget::Event;
    target.id = videoEvent;
    const uint64_t slot = app.addVideoFx(target, "reno.test.invert", "Test Invert");
    check(slot != 0, "the video event has the test effect");

    Playback pb;
    RealtimeOutput* out = new RealtimeOutput();
    pb.setAudioOutput(std::unique_ptr<AudioOutput>(out));
    pb.setPreviewSize(0, 0, false);  // project size: nothing smaller to hide the effect's cost

    std::vector<Shown> shown;
    pb.onFrame = [&](const QImage& img, double, double) {
        shown.push_back(Shown{frameOf(img), pb.playing() ? pb.position() : -1.0, now(), pb.playing()});
    };

    // Plays from 'from' for 'seconds' and reports how it went.
    auto playFor = [&](double from, double seconds) {
        PlayStats s;
        app.setCursor(from);
        runFor(0.05);
        shown.clear();
        const double t0 = now();
        pb.play();
        s.worstStall = runFor(seconds);
        s.seconds = now() - t0;
        s.cursorMoved = app.cursor() - from;
        s.posMoved = pb.position() - from;
        s.fps = pb.previewFrameRate();
#ifndef FRAMEDROP_OLD
        s.dropped = pb.droppedFrames();
#endif
        std::vector<double> offs;
        int prev = -1;
        for (const Shown& x : shown) {
            if (!x.playing || x.frame < 0) continue;
            if (x.frame == prev) continue;  // same picture again is not another one shown
            if (x.frame < prev) ++s.backwards;
            prev = x.frame;
            ++s.shown;
            // how far the picture is from what is heard: 0 while the sound is within the frame's own time
            const double a = x.frame / kFps, b = (x.frame + 1) / kFps;
            const double off = x.pos < a ? a - x.pos : (x.pos > b ? x.pos - b : 0.0);
            offs.push_back(off);
            if (off <= 1.0 / kFps + 1e-6) ++s.near1;
            if (off <= 2.0 / kFps + 1e-6) ++s.near2;
            s.worstOff = std::max(s.worstOff, off);
        }
        if (!offs.empty()) {
            std::sort(offs.begin(), offs.end());
            s.medianOff = offs[offs.size() / 2];
        }
        return s;
    };

    // ---------------------------------------------------------------- 0. a quick effect: every frame is shown, once
    {
        unsetenv("TESTFX_SLOW_RENDER_MS");
        unsetenv("TESTFX_SLOW_RENDER_ABORT");
        PlayStats s = playFor(1.0, 2.0);
        double stopMs = timed([&] { pb.stop(); });
        s.sound = checkSound(out->taken());
        print("quick effect", s);
        check(s.shown >= 45 && s.shown <= 62, "a quick effect: about every frame is shown, and none twice", strfmt("%zu in 2 s", s.shown));
        check(s.fps > 24.0 && s.fps < 31.5, "the preview's frame rate is the project's (pictures shown, not requests)", strfmt("%.2f", s.fps));
        check(s.sound.gaps == 0, "the sound is continuous", strfmt("%zu gaps", s.sound.gaps));
        check(stopMs < 50, "stopping is immediate", strfmt("%.1f ms", stopMs));
        runFor(0.4);
    }

    // ---------------------------------------------------------------- 1. 200 ms a frame (a frame lasts 33)
    PlayStats slow;
    {
        setenv("TESTFX_SLOW_RENDER_MS", "200", 1);
        runFor(0.5);  // the paused picture after the last stop has rendered by now
        slow = playFor(1.0, 4.0);
        size_t before = shown.size();
        const double cursorAtStop = app.cursor();
        const double stopMs = timed([&] { pb.stop(); });
        slow.sound = checkSound(out->taken());
        print("200 ms effect", slow);
        check(std::fabs(slow.cursorMoved - slow.seconds) < 0.2, "a 200 ms effect: the cursor moves in real time",
              strfmt("%.3f s in %.3f s", slow.cursorMoved, slow.seconds));
        check(slow.sound.gaps == 0 && slow.sound.samples > 48000 * 3.5, "the sound is continuous (no gaps)",
              strfmt("%zu gaps in %zu samples", slow.sound.gaps, slow.sound.samples));
        check(slow.shown >= 8 && slow.shown < 60, "frames are dropped (some shown, far fewer than the 120 played)", strfmt("%zu", slow.shown));
        check(slow.fps > 0.5 && slow.fps < 15.0, "the preview's frame rate says so", strfmt("%.2f fps", slow.fps));
        check(slow.shown > 0 && slow.near2 * 10 >= slow.shown * 9 && slow.medianOff <= 1.0 / kFps,
              "the pictures shown are those of the time being heard (looked ahead by the render time)",
              strfmt("median %.0f ms, worst %.0f ms, %zu of %zu within 2 frames", slow.medianOff * 1000, slow.worstOff * 1000, slow.near2, slow.shown));
        check(slow.backwards == 0, "the frames shown always go forwards (no late picture after a newer one)", strfmt("%zu", slow.backwards));
#ifndef FRAMEDROP_OLD
        check(slow.dropped >= 60, "the dropped frames are counted", strfmt("%llu", (unsigned long long)slow.dropped));
#endif
        check(stopMs < 50, "stopping is immediate", strfmt("%.1f ms", stopMs));
        check(std::fabs(app.cursor() - 1.0) < 1e-9, "and the cursor goes back", strfmt("%.3f (was %.3f)", app.cursor(), cursorAtStop));
        // no queue: after stopping, nothing late turns up - only the picture at the cursor
        runFor(1.0);
        std::vector<int> after;
        for (size_t k = before; k < shown.size(); ++k) after.push_back(shown[k].frame);
        check(after.size() <= 2 && !after.empty() && after.back() == 30, "no queue builds up: after stopping only the cursor's picture comes",
              strfmt("%zu pictures after stopping, last %d", after.size(), after.empty() ? -1 : after.back()));
    }

#ifndef FRAMEDROP_OLD
    // ---------------------------------------------------------------- 1b. scrubbing with the 200 ms effect: drafts, then the full picture
    {
        std::vector<int> widths;
        pb.onFrame = [&](const QImage& img, double, double) {
            shown.push_back(Shown{frameOf(img), -1.0, now(), pb.playing()});
            widths.push_back(img.width());
        };
        app.setCursor(2.0);
        runFor(0.6);  // one full picture, so the time a picture takes is known
        shown.clear();
        widths.clear();
        const double t0 = now();
        double stall = 0;
        for (int k = 1; k <= 20; ++k) {
            app.setCursor(2.0 + k / kFps);
            stall = std::max(stall, runFor(0.04));
        }
        const size_t during = widths.size();
        const bool draftSeen = std::count(widths.begin(), widths.end(), 160) > 0;
        runFor(1.2);
        std::printf("MEASURE scrubbing 20 frames in %.2f s with a 200 ms effect: %zu pictures during, widths", now() - t0, during);
        for (int w : widths) std::printf(" %d", w);
        std::printf("; last frame %d\n", shown.empty() ? -1 : shown.back().frame);
        check(draftSeen, "scrubbing a slow project shows quicker draft pictures (at half the size)");
        check(!widths.empty() && widths.back() == 320 && shown.back().frame == 80, "once the cursor rests, the full picture of where it is",
              strfmt("%d wide, frame %d", widths.empty() ? 0 : widths.back(), shown.empty() ? -1 : shown.back().frame));
        check(stall < 0.15, "scrubbing never waits for a picture", strfmt("worst %.0f ms", stall * 1000));
        pb.onFrame = [&](const QImage& img, double, double) {
            shown.push_back(Shown{frameOf(img), pb.playing() ? pb.position() : -1.0, now(), pb.playing()});
        };
    }
#endif

    // ---------------------------------------------------------------- 2. 2 s a frame: nothing waits for it
    {
        setenv("TESTFX_SLOW_RENDER_MS", "2000", 1);
        runFor(0.3);
        app.setCursor(3.0);  // a paused render of 2 s starts here
        runFor(0.05);
        shown.clear();
        const double t0 = now();
        double playMs = timed([&] { pb.play(); });
        double stall = runFor(0.6);
        double pauseMs = timed([&] { pb.pause(); });
        const double pausedAt = app.cursor(), pausedAfter = now() - t0;
        double seekMs = timed([&] { app.setCursor(8.0); });
        stall = std::max(stall, runFor(0.2));
        double play2Ms = timed([&] { pb.play(); });
        const double t1 = now();
        stall = std::max(stall, runFor(2.5));
        const double moved = app.cursor() - 8.0, took = now() - t1;
        const SoundCheck sc = checkSound(out->taken());
        double stopMs = timed([&] { pb.stop(); });
        // pictures shown while playing: how far each was from the sound when it came
        double worstOff = 0;
        size_t playingShown = 0;
        for (const Shown& x : shown) {
            if (!x.playing || x.frame < 0) continue;
            ++playingShown;
            const double a = x.frame / kFps, b = (x.frame + 1) / kFps;
            worstOff = std::max(worstOff, x.pos < a ? a - x.pos : (x.pos > b ? x.pos - b : 0.0));
        }
        std::printf("MEASURE 2 s effect: play %.1f ms, pause %.1f ms (at %.3f after %.3f s), seek %.1f ms, play again %.1f ms, stop %.1f ms; "
                    "cursor moved %.3f s in %.3f s; sound %zu samples, %zu gaps; worst interface stall %.0f ms; %zu pictures shown while playing, "
                    "the worst %.0f ms from the sound\n",
                    playMs, pauseMs, pausedAt, pausedAfter, seekMs, play2Ms, stopMs, moved, took, sc.samples, sc.gaps, stall * 1000,
                    playingShown, worstOff * 1000);
        check(worstOff <= 2.0 / kFps + 1e-6, "a picture that takes 2 s is shown only if it is still the one for the time heard",
              strfmt("worst %.0f ms off", worstOff * 1000));
        check(playMs < 50 && pauseMs < 50 && seekMs < 50 && play2Ms < 50 && stopMs < 50,
              "a 2 s effect: play, pause, seek, play and stop are all immediate",
              strfmt("%.1f %.1f %.1f %.1f %.1f ms", playMs, pauseMs, seekMs, play2Ms, stopMs));
        check(std::fabs(pausedAt - 3.6) < 0.2, "the cursor moved on in real time before the pause", strfmt("%.3f", pausedAt));
        check(std::fabs(moved - took) < 0.2, "and after playing again", strfmt("%.3f s in %.3f s", moved, took));
        check(sc.gaps == 0, "the sound is continuous", strfmt("%zu gaps", sc.gaps));
        check(stall < 0.15, "the interface never waits for the picture", strfmt("worst %.0f ms", stall * 1000));
        // newest request wins: the picture that finally comes is the cursor's, not a stale playing one
        const double tw = now();
        int last = -1;
        while (now() - tw < 6.5) {
            runFor(0.1);
            if (!shown.empty() && !shown.back().playing && shown.back().frame == 240) {
                last = 240;
                break;
            }
        }
        check(last == 240, "after stopping, the picture at the cursor comes once its render is done", strfmt("%.2f s", now() - tw));
        runFor(0.3);
        check(!shown.empty() && shown.back().frame == 240, "and nothing older comes after it", strfmt("%d", shown.empty() ? -1 : shown.back().frame));
    }

#ifndef FRAMEDROP_OLD
    // ---------------------------------------------------------------- 3. a 2 s effect that asks abort(): late renders are abandoned
    {
        setenv("TESTFX_SLOW_RENDER_ABORT", "1", 1);
        runFor(2.5);
        const int abortedBefore = count("renderAborted");
        PlayStats s = playFor(2.0, 3.0);
        const double t0 = now();
        shown.clear();
        pb.pause();
        const double cur = app.cursor();
        double waited = -1;
        while (now() - t0 < 5.0) {
            runFor(0.05);
            if (!shown.empty()) {
                waited = now() - t0;
                break;
            }
        }
        s.sound = checkSound(out->taken());
        print("2 s effect that asks abort()", s);
        const int aborted = count("renderAborted") - abortedBefore;
        check(aborted >= 1, "renders that are late are abandoned (the effect is told through abort())", strfmt("%d", aborted));
        check(app.fx.error(slot).empty(), "an effect told to stop has not failed (no error on it)", app.fx.error(slot));
        check(std::fabs(s.cursorMoved - s.seconds) < 0.2 && s.sound.gaps == 0, "the cursor and sound went on regardless",
              strfmt("%.3f s in %.3f s, %zu gaps", s.cursorMoved, s.seconds, s.sound.gaps));
        check(waited > 0 && waited < 2.8 && !shown.empty() && shown.back().frame == (int)std::floor(cur * kFps + 1e-6),
              "pausing abandons the playing render: the paused picture takes only its own render", strfmt("%.2f s", waited));
        unsetenv("TESTFX_SLOW_RENDER_ABORT");
        runFor(0.3);
    }

    // ---------------------------------------------------------------- 4. two slow effects: rendering ahead on two threads
    {
        setenv("TESTFX_SLOW_RENDER_MS", "100", 1);
        FxTarget tt;
        tt.kind = FxTarget::Track;
        tt.id = videoTrack;
        const uint64_t slot2 = app.addVideoFx(tt, "reno.test.invert", "Test Invert");
        check(slot2 != 0, "the track has a second test effect");
        runFor(0.6);
  // inverted twice
        pb.onFrame = [&](const QImage& img, double, double) {
            const QRgb px = img.isNull() ? 0 : img.pixel(img.width() / 2, img.height() / 2);
            shown.push_back(Shown{img.isNull() ? -1 : qBlue(px) + 256 * qGreen(px), pb.playing() ? pb.position() : -1.0, now(), pb.playing()});
        };
        app.prefs.renderThreads = 1;
        PlayStats one = playFor(1.0, 3.0);
        pb.stop();
        one.sound = checkSound(out->taken());
        runFor(0.6);
        app.prefs.renderThreads = 2;
        PlayStats two = playFor(1.0, 3.0);
        pb.stop();
        two.sound = checkSound(out->taken());
        print("two 100 ms effects, one thread", one);
        print("two 100 ms effects, two threads", two);
        check(one.sound.gaps == 0 && two.sound.gaps == 0, "two slow effects: the sound is continuous either way");
        check(two.near2 * 10 >= two.shown * 9 && two.shown > 0, "on two threads the pictures shown are still the current ones",
              strfmt("%zu of %zu within 2 frames", two.near2, two.shown));
        check(two.shown >= one.shown * 5 / 4, "two threads show more pictures (one effect of a frame works while the other does the next)",
              strfmt("%zu vs %zu", two.shown, one.shown));
        check(one.backwards == 0 && two.backwards == 0, "on either the frames shown always go forwards",
              strfmt("%zu, %zu", one.backwards, two.backwards));
        app.prefs.renderThreads = 0;
    }

    // ---------------------------------------------------------------- 5. Render As never drops a frame
    {
        pb.stop();
        runFor(0.5);
        setenv("TESTFX_SLOW_RENDER_MS", "60", 1);  // two effects: 120 ms a frame, four frame lengths
        const std::string file = std::string(argv[1]) + "/framedrop_render.mov";
        RenderSpec spec;
        spec.enc.path = file;
        spec.enc.audio = false;
        spec.enc.videoFamily = "png";  // lossless RGB: frame numbers read back exactly
        spec.enc.encodeMode = "cpu";
        const int before = count("slowRender");
        std::string err;
        int64_t progressFrames = 0;
        const double t0 = now();
        const bool ok = renderProjectAs(app.project(), app.fx, spec, 1.0, 2.0,
                                        [&](const ExportProgress& ep) { progressFrames = ep.frame; return true; }, &err, opener);
        const double took = now() - t0;
        const int renders = count("slowRender") - before;
        std::vector<int> frames;
        if (ok) {
            if (std::unique_ptr<MediaReader> rd = openMedia(file, nullptr)) {
                for (int k = 0; k < 40; ++k) {
                    ImagePtr img = rd->videoFrame((k + 0.5) / kFps, 0, 0);
                    if (!img || img->w <= 0 || img->depth != PixelDepth::Byte) break;
                    const uint8_t* px = img->row(img->h / 2) + (size_t)(img->w / 2) * 4;  // BGRA
                    const int f = px[0] + 256 * px[1];  // inverted twice: blue + 256 green
                    if (!frames.empty() && f == frames.back() && k >= 30) break;
                    frames.push_back(f);
                }
            }
        }
        std::printf("MEASURE Render As 1 s with two 60 ms effects: %s in %.2f s, %d slow renders, frames read back:", ok ? "done" : err.c_str(),
                    took, renders);
        for (int f : frames) std::printf(" %d", f);
        std::printf("\n");
        check(ok, "Render As with a slow effect finishes", err);
        check(renders == 60, "every frame's effects ran in full (30 frames, two effects: none dropped or abandoned)", strfmt("%d", renders));
        bool every = frames.size() >= 30;
        for (int k = 0; every && k < 30; ++k) every = frames[k] == 30 + k;
        check(every, "the file has every frame, in order (frames 30 to 59)", strfmt("%zu frames read", frames.size()));
        // Render As renders several frames at once, one per processor
        check(took > 30 * 0.12 * 0.9 / std::max(1, processorCount()), "it took as long as the effects need (it waits for each frame)", strfmt("%.2f s", took));
    }
#endif
    unsetenv("TESTFX_SLOW_RENDER_MS");
    return uitest::summary();
}
