// Playing a project too heavy to keep up: while playing, pictures are rendered at the preview's size (Playback::setPreviewSize)
// rather than the project's; with Auto they go smaller still when frames take longer than they last; late frames are dropped and
// Playback::previewFrameRate says how many are shown; a paused picture is the project's size again. Pictures come from a
// stand-in reader that takes 60 ms a frame, and the sound goes to a stand-in sound device the test pulls from itself.
//
//   preview_perf_test
// Build: uibuild preview_perf_test tests/ui/preview_perf_test.cpp
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <chrono>
#include <cmath>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

// "slow:" 20 s of 640x360 pictures that take 60 ms each to make; "quick:" the same at once.
class Reader : public MediaReader {
public:
    MediaInfo mi;
    bool slow = false;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(60));
        int w = 640, h = 360;
        if (maxW > 0 && maxH > 0 && (maxW < w || maxH < h)) {
            const double s = std::min((double)maxW / w, (double)maxH / h);
            w = std::max(1, (int)std::lround(w * s));
            h = std::max(1, (int)std::lround(h * s));
        }
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        const int f = (int)std::floor(seconds * 30.0 + 1e-6);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = (uint8_t)(f % 256); d[1] = (uint8_t)(x * 255 / w); d[2] = 90; d[3] = 255;
            }
        return img;
    }
    bool audio(int64_t, int frames, int, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) l[k] = r[k] = 0.f;
        return false;
    }
};

static std::unique_ptr<MediaReader> opener(const std::string& path, std::string*)
{
    std::unique_ptr<Reader> r(new Reader());
    r->slow = path.compare(0, 5, "slow:") == 0;
    r->mi.path = path;
    r->mi.duration = 20;
    r->mi.hasVideo = true;
    r->mi.width = 640;
    r->mi.height = 360;
    r->mi.fps = 30;
    return std::unique_ptr<MediaReader>(r.release());
}

class ManualOutput : public AudioOutput {
public:
    Pull pull;
    bool start(int, Pull p) override { pull = std::move(p); return true; }
    void stop() override { pull = Pull(); }
    std::string name() const override { return "test"; }
};

// Plays on for 'seconds', taking the sound at about the pace of a sound card (which moves the playback on).
static void playFor(ManualOutput* out, double seconds)
{
    const int piece = 240;
    std::vector<float> l(piece), r(piece);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (std::chrono::steady_clock::now() < until && out->pull) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        QCoreApplication::processEvents();
        if (out->pull) out->pull(l.data(), r.data(), piece);
    }
}

static void setClip(const std::string& path)
{
    AppState& app = AppState::get();
    app.edit("clip", [&](Project& p) {
        p.tracks.clear();
        p.media.clear();
        MediaItem m;
        m.id = p.newId();
        m.path = path;
        m.name = path;
        m.info = opener(path, nullptr)->info();
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 20;
        t.events.push_back(e);
        p.tracks.push_back(t);
    });
}

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    AppState& app = AppState::get();
    app.mediaOpener = opener;
    app.edit("settings", [](Project& p) {
        p.settings.width = 640;
        p.settings.height = 360;
        p.settings.fpsNum = 30;
        p.settings.fpsDen = 1;
    });
    Playback pb;
    ManualOutput* out = new ManualOutput();
    pb.setAudioOutput(std::unique_ptr<AudioOutput>(out));

    // ---- quick pictures, a preview of half the size (Preview quality, or Half): rendered at that size while playing
    setClip("quick:");
    pb.setPreviewSize(320, 180, false);
    app.setCursor(1.0);
    pb.play();
    playFor(out, 1.0);
    const QImage playing = pb.lastFrame();
    const double fpsQuick = pb.previewFrameRate();
    pb.pause();
    check(playing.width() == 320 && playing.height() == 180, "while playing, pictures are rendered at the preview's size",
          strfmt("%dx%d", playing.width(), playing.height()));
    check(pb.renderScale() < 1.0 || pb.lastFrame().width() == 640, "the scale rendered at is told");
    check(fpsQuick > 10.0, "the preview's frame rate is counted while playing", strfmt("%.2f", fpsQuick));
    uitest::settle(400);
    check(pb.lastFrame().width() == 640, "paused, the picture is the project's size again", strfmt("%d", pb.lastFrame().width()));
    check(pb.previewFrameRate() == 0.0, "paused, there is no preview frame rate");

    // ---- pictures that take 60 ms each (a frame lasts 33): frames are dropped, and with Auto the size goes down
    setClip("slow:");
    pb.setPreviewSize(640, 360, true);
    app.setCursor(2.0);
    pb.play();
    playFor(out, 2.5);
    const QImage slowPic = pb.lastFrame();
    const double fpsSlow = pb.previewFrameRate();
    const double scaleSlow = pb.renderScale();
    pb.pause();
    check(fpsSlow > 0.0 && fpsSlow < 25.0, "frames that cannot keep up are dropped (the frame rate shown is below the project's)",
          strfmt("%.2f fps", fpsSlow));
    check(scaleSlow < 1.0 && slowPic.width() < 640, "Auto renders smaller when frames take longer than they last",
          strfmt("scale %.3f, %dx%d", scaleSlow, slowPic.width(), slowPic.height()));
    uitest::settle(600);
    check(pb.lastFrame().width() == 640, "paused after that, the picture is full size", strfmt("%d", pb.lastFrame().width()));

    // ---- the same without Auto: the preview's own size, however slow
    pb.setPreviewSize(640, 360, false);
    app.setCursor(5.0);
    pb.play();
    playFor(out, 1.5);
    const QImage fixedPic = pb.lastFrame();
    pb.pause();
    check(fixedPic.width() == 640, "without Auto the size stays the preview's", strfmt("%d", fixedPic.width()));
    return uitest::summary();
}
