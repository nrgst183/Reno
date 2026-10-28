// Playing looks ahead by the average render time, per stretch between cuts: a slow clip's first frame is started its
// own average before the clip begins, so it is shown on time, while the quick clip before it keeps playing.
// The project: 0-3 s plain frames, 3-8 s the same frames through the test plug-in's Test Invert slowed by
// TESTFX_SLOW_RENDER_MS (tests/ofx/testfx.cpp). Played twice from 1 s: the first play learns how slow the second clip
// is, the second shows its first frame at the cut. No RAM preview (that would replay frames from memory).
//
//   lookahead_test <output folder> <testfx.ofx>
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <vector>

using namespace reno;
using namespace reno::app;
using uitest::check;

static const double kFps = 30.0;

class Reader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int, int) override
    {
        ImagePtr img = Image::make(320, 180, PixelDepth::Byte, false);
        const int f = std::max(0, std::min(599, (int)std::floor(seconds * kFps + 1e-6)));
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = (uint8_t)(f % 256); d[1] = (uint8_t)(f / 256); d[2] = 0; d[3] = 255;
            }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> opener(const std::string& path, std::string*)
{
    std::unique_ptr<Reader> r(new Reader());
    r->mi.path = path;
    r->mi.duration = 20;
    r->mi.hasVideo = true;
    r->mi.width = 320;
    r->mi.height = 180;
    r->mi.fps = kFps;
    return std::unique_ptr<MediaReader>(r.release());
}

// A sound card that takes the sound in real time on its own thread (playing follows the sound).
class RealtimeOutput : public AudioOutput {
public:
    ~RealtimeOutput() override { stop(); }
    bool start(int sampleRate, Pull pull) override
    {
        stop();
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
    std::atomic<bool> run{false};
    std::thread thread;
};

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void runFor(double seconds)
{
    const double until = now() + seconds;
    while (now() < until) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// The frame a picture shows, and whether it went through the (slow) effect.
struct Pic { int frame; bool inverted; };
static Pic picOf(const QImage& img)
{
    if (img.isNull()) return {-1, false};
    const QRgb px = img.pixel(img.width() / 2, img.height() / 2);
    const bool inv = qRed(px) > 128;
    return inv ? Pic{(255 - qBlue(px)) + 256 * (255 - qGreen(px)), true} : Pic{qBlue(px) + 256 * qGreen(px), false};
}

struct Shown { Pic pic; double pos; };

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    QApplication::setOrganizationName("RenoLookaheadTest");
    QApplication::setApplicationName("lookahead_test");
    if (argc < 3) { std::printf("usage: lookahead_test <output folder> <testfx.ofx>\n"); return 2; }
    uitest::setup({argv[2]}, std::vector<std::string>());
    AppState& app = AppState::get();
    app.prefs.ramPreviewMb = 0;
    app.mediaOpener = opener;
    app.newProject();
    app.edit("settings", [](Project& p) {
        p.settings.width = 320;
        p.settings.height = 180;
        p.settings.fpsNum = 30;
        p.settings.fpsDen = 1;
        p.settings.sampleRate = 48000;
    });
    uint64_t slowEvent = 0;
    app.edit("clips", [&](Project& p) {
        MediaItem m;
        m.id = p.newId();
        m.path = "frames:";
        m.name = "frames";
        m.info = opener(m.path, nullptr)->info();
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event a;
        a.id = p.newId();
        a.mediaId = m.id;
        a.start = 0;
        a.length = 3;
        Event b = a;
        b.id = p.newId();
        b.start = 3;
        b.length = 5;
        b.offset = 3;
        slowEvent = b.id;
        t.events.push_back(a);
        t.events.push_back(b);
        p.tracks.push_back(t);
    });
    FxTarget target;
    target.kind = FxTarget::Event;
    target.id = slowEvent;
    check(app.addVideoFx(target, "reno.test.invert", "Test Invert") != 0, "the second clip has the slow effect");
    setenv("TESTFX_SLOW_RENDER_MS", "250", 1);

    Playback pb;
    pb.setAudioOutput(std::unique_ptr<AudioOutput>(new RealtimeOutput()));
    pb.setPreviewSize(0, 0, false);
    std::vector<Shown> shown;
    pb.onFrame = [&](const QImage& img, double, double) {
        if (pb.playing()) shown.push_back(Shown{picOf(img), pb.position()});
    };

    // Plays 1 s to 4.5 s; returns where play was when the clip's first frame (frame 90, through the effect) was shown
    // (-1: never), and how many frames of the quick clip were shown.
    auto play = [&](double& firstSlowAt, int& quickShown, int& slowShown) {
        app.setCursor(1.0);
        runFor(0.3);
        shown.clear();
        pb.play();
        runFor(3.5);
        pb.stop();
        runFor(0.3);
        firstSlowAt = -1;
        quickShown = slowShown = 0;
        int prev = -1;
        for (const Shown& s : shown) {
            if (s.pic.frame == prev) continue;
            prev = s.pic.frame;
            if (!s.pic.inverted && s.pic.frame >= 30 && s.pic.frame < 90) ++quickShown;
            if (s.pic.inverted) ++slowShown;
            if (s.pic.inverted && s.pic.frame == 90 && firstSlowAt < 0) firstSlowAt = s.pos;
        }
        std::string seen;
        prev = -1;
        for (const Shown& s : shown)
            if (s.pic.frame != prev) { prev = s.pic.frame; if (s.pos > 2.8) seen += strfmt(" %d%s@%.3f", s.pic.frame, s.pic.inverted ? "i" : "", s.pos); }
        std::printf("SHOWN after 2.8 s:%s\n", seen.c_str());
    };
    double first1, first2;
    int quick1, quick2, slow1, slow2;
    play(first1, quick1, slow1);
    std::printf("MEASURE first play: clip's first frame shown at %.3f s (cut at 3.000), %d quick frames, %d slow frames\n", first1, quick1, slow1);
    play(first2, quick2, slow2);
    std::printf("MEASURE second play: clip's first frame shown at %.3f s (cut at 3.000), %d quick frames, %d slow frames\n", first2, quick2, slow2);
    check(first2 >= 2.99 && first2 <= 3.0 + 2.0 / kFps, "second play: the slow clip's first frame is shown at the cut (started its average ahead)",
          strfmt("shown at %.3f s", first2));
    check(quick2 >= 40, "  and the quick clip before it still plays (most of its 60 frames)", strfmt("%d", quick2));
    check(slow2 >= 2, "  and later frames of the slow clip are shown too (at its own pace)", strfmt("%d", slow2));
    return uitest::summary();
}
