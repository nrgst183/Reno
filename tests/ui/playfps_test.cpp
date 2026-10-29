// Playing shows every frame of the project, at the project's own rate, when the frames are quick to make: none is
// skipped because the sound card takes the sound in bursts (Windows' shared mode asks for 10-40 ms at a time, at
// uneven moments), and the rate the preview reads out is the project's, not a frame or two below it.
// Projects at 23.976, 29.97, 30, 59.94 and 60 fps are each played for 4 s with a sound card that takes steady pieces
// and one that takes bursts.
//
//   playfps_test <output folder>
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <random>
#include <thread>
#include <vector>

using namespace reno;
using namespace reno::app;
using uitest::check;

static double gFps = 30.0;

// Each frame carries its own number in its first two bytes.
class Reader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int, int) override
    {
        ImagePtr img = Image::make(160, 90, PixelDepth::Byte, false);
        const int f = std::max(0, (int)std::floor(seconds * gFps + 1e-6));
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) {
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
    r->mi.duration = 60;
    r->mi.hasVideo = true;
    r->mi.width = 160;
    r->mi.height = 90;
    r->mi.fps = gFps;
    return std::unique_ptr<MediaReader>(r.release());
}

// A sound card on its own thread that takes the sound in real time: in steady 10 ms pieces, or (bursty) in pieces of
// 10 ms taken several at once at uneven moments, the way shared-mode devices often do - never more than 'buffer'
// ahead of what has been played.
class TestOutput : public AudioOutput {
public:
    explicit TestOutput(bool bursty) : bursty_(bursty) {}
    ~TestOutput() override { stop(); }
    bool start(int sampleRate, Pull pull) override
    {
        stop();
        run = true;
        thread = std::thread([this, sampleRate, pull] {
            std::vector<float> l(4096), r(4096);
            std::mt19937 rng(7);
            const int piece = sampleRate / 100;
            const auto t0 = std::chrono::steady_clock::now();
            int64_t taken = 0;
            while (run) {
                const double played = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * sampleRate;
                // keep 'buffer' ahead of what is played: steady takes a piece as soon as there is room; bursty waits
                // until a few pieces' room and then takes them all
                const int64_t room = (int64_t)(played + buffer * sampleRate) - taken;
                const int want = bursty_ ? (int)(1 + rng() % 4) * piece : piece;
                if (room >= want) {
                    for (int got = 0; got < want; got += piece) pull(l.data(), r.data(), piece);
                    taken += want;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(bursty_ ? 2000 + (int)(rng() % 9000) : 1000));
            }
        });
        return true;
    }
    void stop() override
    {
        run = false;
        if (thread.joinable()) thread.join();
    }
    double latencySeconds() const override { return buffer; }
    std::string name() const override { return "test"; }
    double buffer = 0.05;
    std::atomic<bool> run{false};
    std::thread thread;
private:
    bool bursty_;
};

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void runFor(double seconds, const std::function<void()>& each = nullptr)
{
    const double until = now() + seconds;
    while (now() < until) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        if (each) each();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

static int frameOf(const QImage& img)
{
    if (img.isNull()) return -1;
    const QRgb px = img.pixel(img.width() / 2, img.height() / 2);
    return qBlue(px) + 256 * qGreen(px);
}

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    QApplication::setOrganizationName("RenoPlayFpsTest");
    QApplication::setApplicationName("playfps_test");
    setenv("RENO_DISPLAY_HZ", "60", 1);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.prefs.ramPreviewMb = 0;
    app.mediaOpener = opener;

    struct Rate { int num, den; };
    const Rate rates[] = {{24000, 1001}, {30000, 1001}, {30, 1}, {60000, 1001}, {60, 1}};
    for (const Rate& rt : rates)
        for (int bursty = 0; bursty < 2; ++bursty) {
            gFps = (double)rt.num / rt.den;
            app.newProject();
            app.edit("settings", [&](Project& p) {
                p.settings.width = 160;
                p.settings.height = 90;
                p.settings.fpsNum = rt.num;
                p.settings.fpsDen = rt.den;
                p.settings.sampleRate = 48000;
            });
            app.edit("clip", [&](Project& p) {
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
                a.length = 30;
                t.events.push_back(a);
                p.tracks.push_back(t);
            });
            Playback pb;
            pb.setAudioOutput(std::unique_ptr<AudioOutput>(new TestOutput(bursty != 0)));
            pb.setPreviewSize(0, 0, false);
            std::vector<int> shown;
            pb.onFrame = [&](const QImage& img, double, double) {
                if (pb.playing()) shown.push_back(frameOf(img));
            };
            app.setCursor(1.0);
            runFor(0.3);
            pb.play();
            std::vector<double> readouts;
            double lastRead = now();
            runFor(4.0, [&] {
                if (now() - lastRead >= 0.25) {
                    lastRead = now();
                    readouts.push_back(pb.previewFrameRate());
                }
            });
            pb.stop();
            runFor(0.2);
            // frames from the first half second on (playing settles), each new one once
            std::vector<int> seq;
            for (int f : shown)
                if (f >= 0 && (seq.empty() || f != seq.back())) seq.push_back(f);
            const int from = (int)std::ceil((1.0 + 0.5) * gFps);
            int skipped = 0, first = -1, last = -1;
            for (size_t k = 1; k < seq.size(); ++k) {
                if (seq[k] < from || seq[k - 1] < from) continue;
                if (first < 0) first = seq[k - 1];
                last = seq[k];
                if (seq[k] > seq[k - 1] + 1) skipped += seq[k] - seq[k - 1] - 1;
            }
            double lo = 1e9, hi = 0;
            for (size_t k = 4; k < readouts.size(); ++k) {   // after the first second
                lo = std::min(lo, readouts[k]);
                hi = std::max(hi, readouts[k]);
            }
            const std::string what = strfmt("%.3f fps, %s sound card", gFps, bursty ? "bursty" : "steady");
            std::printf("MEASURE %s: frames %d..%d, %d skipped, %llu dropped, readout %.2f..%.2f\n", what.c_str(), first, last, skipped,
                        (unsigned long long)pb.droppedFrames(), lo, hi);
            check(last - first >= (int)(2.0 * gFps), what + ": played on", strfmt("%d..%d", first, last));
            // A frame not rendered in time (the machine busy) is dropped and the rate read out is the real one; the
            // play clock itself skips none (before it followed the sound smoothly, about one in five at 60 fps).
            const int frames = std::max(1, last - first);
            check(skipped * 50 <= frames, what + ": no frame skipped (2% at most, frames the machine was too busy for)",
                  strfmt("%d skipped of %d", skipped, frames));
            if (pb.droppedFrames() == 0)
                check(lo >= gFps - 0.01 && hi <= gFps + 0.01, what + ": the rate read out is the project's", strfmt("%.2f..%.2f", lo, hi));
            else
                check(lo >= gFps * 0.95 && hi <= gFps + 0.01, what + ": the rate read out is the real one, a little below the project's",
                      strfmt("%.2f..%.2f, %llu dropped", lo, hi, (unsigned long long)pb.droppedFrames()));
        }
    return uitest::summary();
}
