// Moving the cursor while playing (VEGAS): Go to Start, a click on the ruler, a jump to a marker, Home, the arrow keys ... make
// playing continue from the new place (sound, picture and cursor) at the same rate, inside the loop region if there is one;
// stopped, the cursor moves and stays stopped. Sound comes from a stand-in sound device the test pulls from itself (a ramp whose
// value says where in the file it is).
//
//   navplay_test <output folder> <testfx.ofx>
// Build: with uibuild (tests/ui/stubs, docs/NOTES.md)
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/theme.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <chrono>
#include <cmath>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

class Reader : public MediaReader {
public:
    MediaInfo mi;
    std::string kind;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int, int) override
    {
        if (kind != "frames") return nullptr;
        ImagePtr img = Image::make(64, 36, PixelDepth::Byte, false);
        const int f = std::max(0, std::min(599, (int)std::floor(seconds * 30.0 + 1e-6)));
        for (int y = 0; y < 36; ++y)
            for (int x = 0; x < 64; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = (uint8_t)(f % 256); d[1] = (uint8_t)(f / 256); d[2] = 0; d[3] = 255;
            }
        return img;
    }
    bool audio(int64_t start, int frames, int rate, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) {
            const double t = (double)(start + k) / rate;
            l[k] = r[k] = (t >= 0 && t < 20.0) ? (float)(t / 20.0) : 0.f;
        }
        return kind != "frames";
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
        r->mi.width = 64;
        r->mi.height = 36;
        r->mi.fps = 30;
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

class ManualOutput : public AudioOutput {
public:
    Pull pull;
    bool start(int, Pull p) override { pull = std::move(p); return true; }
    void stop() override { pull = Pull(); }
    std::string name() const override { return "test"; }
    double latencySeconds() const override { return 0; }
};

static ManualOutput* gOut = nullptr;

// 'seconds' of sound in 5 ms pieces, at about the pace of a sound card; UI events are handled meanwhile.
static std::vector<float> takeSound(double seconds)
{
    std::vector<float> got;
    const int piece = 240;
    const int pieces = (int)(seconds * 48000 / piece);
    std::vector<float> l(piece), r(piece);
    for (int k = 0; k < pieces && gOut->pull; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        QCoreApplication::processEvents();
        if (!gOut->pull) break;
        gOut->pull(l.data(), r.data(), piece);
        got.insert(got.end(), l.begin(), l.end());
    }
    return got;
}

static void addClip(const std::string& path, TrackType type)
{
    AppState::get().edit("test", [&](Project& p) {
        MediaItem m;
        m.id = p.newId();
        m.path = path;
        m.name = path;
        m.info = opener(path, nullptr)->info();
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = type;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.start = 0;
        e.length = 20;
        t.events.push_back(e);
        p.tracks.push_back(t);
    });
}

// The sound after a jump: the lowest and highest place in the file heard (seconds), skipping the first 'skip' samples.
static void range(const std::vector<float>& s, size_t skip, double& lo, double& hi)
{
    lo = 1e9;
    hi = -1e9;
    for (size_t k = skip; k < s.size(); ++k) {
        lo = std::min(lo, (double)s[k] * 20.0);
        hi = std::max(hi, (double)s[k] * 20.0);
    }
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoNavPlayTest");
    QApplication::setApplicationName("navplay_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("navplayout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.newProject();
    app.mediaOpener = opener;
    app.edit("settings", [](Project& p) {
        p.settings.sampleRate = 48000;
        p.settings.width = 64;
        p.settings.height = 36;
    });
    addClip("ramp:", TrackType::Audio);
    addClip("frames:", TrackType::Video);
    app.edit("marker", [](Project& p) {
        Project::Marker m;
        m.id = p.newId();
        m.time = 15.0;
        m.name = "m";
        p.markers.push_back(m);
    });

    MainWindow w;
    w.resize(1400, 800);
    w.show();
    w.activateWindow();
    uitest::settle(300);
    Playback* pb = w.playback();
    TimelineWidget* tl = w.timeline();
    gOut = new ManualOutput();
    pb->setAudioOutput(std::unique_ptr<AudioOutput>(gOut));
    int shownFrame = -1;
    double shownAt = -1;
    pb->onFrame = [&](const QImage& img, double seconds, double) {
        shownAt = seconds;
        shownFrame = qBlue(img.pixel(10, 10)) + 256 * qGreen(img.pixel(10, 10));
    };
    tl->zoomToFit();
    uitest::settle(100);

    auto trigger = [&](const char* name) {
        QAction* a = w.vegasAction(name);
        check(a != nullptr, std::string("the action ") + name + " exists");
        if (a) a->trigger();
    };
    // the x of a time on the ruler
    auto xOf = [&](double t) {
        const double vs = tl->viewStartTime(), ve = tl->viewEndTime();
        const int left = tl->headerWidth(), right = tl->width() - 17;
        return left + (int)std::lround((t - vs) / (ve - vs) * (right - left));
    };
    const int rulerY = 1 + 16 + 12 + 2 + 8;

    // Plays from 'from' for a moment, then does 'jump'; checks that it plays on from 'expect' (+- tol) and returns the sound heard after.
    struct Case { const char* name; double from; double expect; double tol; };
    auto playThen = [&](const Case& c, const std::function<void()>& jump, double rate = 1.0) {
        app.setLoop(0, 0);
        app.loopOn = false;
        pb->pause();
        app.setCursor(c.from);
        uitest::settle(50);
        pb->playAt(rate);
        std::vector<float> before = takeSound(0.3);
        const bool playingBefore = pb->playing();
        jump();
        std::vector<float> after = takeSound(0.5);
        uitest::settle(30);
        const double posA = pb->position();
        std::vector<float> later = takeSound(0.3);
        uitest::settle(30);
        const double posB = pb->position();
        double lo, hi;
        range(after, 0, lo, hi);
        const std::string n = c.name;
        check(playingBefore, n + ": it was playing before the jump");
        check(pb->playing(), n + ": still playing after it");
        check(!after.empty() && std::fabs(rate) > 0, n + ": sound keeps coming", strfmt("%zu samples", after.size()));
        check(lo > c.expect - c.tol - 0.15 && hi < c.expect + c.tol + 0.15 + 0.7 * std::fabs(rate), n + ": the sound is from the new place, none from the old",
              strfmt("heard %.3f .. %.3f s, wanted near %.2f", lo, hi, c.expect));
        check(std::fabs(app.cursor() - posB) < 0.35 && std::fabs(posA - c.expect) < c.tol + 0.9 * std::fabs(rate) && posB > posA + 0.1 * std::fabs(rate),
              n + ": the cursor is at the new place and advancing", strfmt("%.3f then %.3f, wanted near %.2f, cursor %.3f", posA, posB, c.expect, app.cursor()));
        (void)before;
        (void)later;
        return after;
    };

    // ------------------------------------------------------------------ Go to Start
    playThen({"Go to Start while playing", 10.0, 0.0, 0.1}, [&] { trigger("goStart"); });
    check(pb->rate() == 1.0, "Go to Start keeps the rate");
    uitest::settle(150);
    check(shownAt >= 0 && shownAt < 2.0 && shownFrame == (int)std::floor(shownAt * 30.0 + 1e-6), "the picture is from the start too", strfmt("%.3f s, frame %d", shownAt, shownFrame));
    pb->stop();

    // ------------------------------------------------------------------ the keys of the timeline
    tl->setFocus();
    playThen({"Ctrl+Home in the timeline while playing", 12.0, 0.0, 0.1}, [&] { uitest::key(tl, Qt::Key_Home, Qt::ControlModifier); });
    playThen({"Home in the timeline while playing", 12.0, tl->viewStartTime(), 0.1}, [&] { uitest::key(tl, Qt::Key_Home); });
    playThen({"Page Down while playing", 3.0, 3.0, 20.0}, [&] { uitest::key(tl, Qt::Key_PageDown); });
    playThen({"Right arrow while playing", 3.0, 3.0, 2.0}, [&] { uitest::key(tl, Qt::Key_Right); });
    pb->pause();

    // ------------------------------------------------------------------ a click on the ruler
    playThen({"a click on the ruler while playing", 12.0, 5.0, 0.2}, [&] {
        const QPoint p(xOf(5.0), rulerY);
        uitest::mouseClick(tl, p);
    });
    uitest::shot(tl, out + "/navplay_ruler.png");
    // a drag along the ruler (the sound is quiet meanwhile; playing goes on from where it is let go)
    playThen({"a drag along the ruler while playing", 12.0, 8.0, 0.25}, [&] {
        uitest::mouseDrag(tl, QPoint(xOf(3.0), rulerY), QPoint(xOf(8.0), rulerY), 6);
    });

    // ------------------------------------------------------------------ a marker
    playThen({"Go to marker 1 while playing", 2.0, 15.0, 0.2}, [&] { tl->goToMarker(1); });
    playThen({"next marker while playing", 2.0, 15.0, 0.2}, [&] { tl->cursorToMarker(1); });

    // ------------------------------------------------------------------ the rate and the loop region
    playThen({"Go to Start while playing at 2x", 12.0, 0.0, 0.1}, [&] { trigger("goStart"); }, 2.0);
    check(pb->rate() == 2.0, "Go to Start while playing at 2x: still 2x", strfmt("%.2f", pb->rate()));
    pb->pause();
    {
        // the loop region 4 - 6 s is on: going to the start plays on from the region's start, round the region
        app.setLoop(4.0, 6.0);
        app.loopOn = true;
        app.setCursor(5.0);
        pb->play();
        takeSound(0.3);
        trigger("goStart");
        std::vector<float> after = takeSound(1.2);
        double lo, hi;
        range(after, 0, lo, hi);
        check(pb->playing() && lo > 3.9 && hi < 6.1, "with a loop region on: Go to Start plays on inside it", strfmt("%.3f .. %.3f s", lo, hi));
        pb->pause();
        app.loopOn = false;
        app.setLoop(0, 0);
    }

    // ------------------------------------------------------------------ Go to End: playing stops (there is nothing after it)
    app.setCursor(8.0);
    pb->play();
    takeSound(0.3);
    trigger("goEnd");
    uitest::settle(100);
    check(!pb->playing() && std::fabs(app.cursor() - app.project().length()) < 1e-6, "Go to End while playing stops at the end", strfmt("%.3f", app.cursor()));

    // ------------------------------------------------------------------ stopped stays stopped
    pb->pause();
    app.setCursor(9.0);
    uitest::settle(100);
    trigger("goStart");
    uitest::settle(100);
    check(!pb->playing() && app.cursor() == 0.0, "stopped: Go to Start moves the cursor and nothing plays");
    uitest::mouseClick(tl, QPoint(xOf(6.0), rulerY));
    uitest::settle(100);
    check(!pb->playing() && std::fabs(app.cursor() - 6.0) < 0.3, "stopped: a click on the ruler moves the cursor and nothing plays", strfmt("%.3f", app.cursor()));
    tl->goToMarker(1);
    uitest::settle(100);
    check(!pb->playing() && std::fabs(app.cursor() - 15.0) < 1e-6, "stopped: a jump to a marker only moves the cursor");
    uitest::key(tl, Qt::Key_Home, Qt::ControlModifier);
    uitest::settle(100);
    check(!pb->playing() && app.cursor() == 0.0, "stopped: Ctrl+Home only moves the cursor");

    // Stop after a jump goes back to the place it was jumped to (where this playing began)
    app.setCursor(12.0);
    pb->play();
    takeSound(0.3);
    trigger("goStart");
    takeSound(0.3);
    pb->stop();
    check(!pb->playing() && app.cursor() == 0.0, "Stop after Go to Start returns to the start", strfmt("%.3f", app.cursor()));
    uitest::shot(tl, out + "/navplay_end.png");
    return uitest::summary();
}
