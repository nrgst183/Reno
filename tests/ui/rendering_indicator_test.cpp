// The Video Preview's "rendering" indicator (src/app/panels.cpp, Playback::renderingFor): a small spinner in the picture's corner
// while the preview has waited more than 150 ms for a picture (a paused or scrubbed frame whose effect is slow, or playing that
// drops frames), gone as soon as the picture comes, and never left behind when the request is given up (a newer request rendered
// instead, playing started and stopped). Quick pictures never show it. The slow effect is the test plug-in's Test Invert slowed
// by TESTFX_SLOW_RENDER_MS (tests/ofx/testfx.cpp).
//
//   rendering_indicator_test <output folder> <testfx.ofx>
//
// Build: uibuild rendering_indicator_test tests/ui/rendering_indicator_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Runs the event loop for 'seconds'; true if 'cond' held at some point during it (checked every few ms).
template <class F>
bool sawWithin(double seconds, F cond)
{
    const double until = now() + seconds;
    bool saw = false;
    while (now() < until) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (cond()) saw = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return saw;
}
// Runs the event loop until 'cond' holds (true) or 'seconds' have passed (false).
template <class F>
bool waitFor(double seconds, F cond)
{
    const double until = now() + seconds;
    while (now() < until) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoRenderingTest");
    QApplication::setApplicationName("rendering_indicator_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) {
        std::printf("usage: rendering_indicator_test <output folder> <testfx.ofx>\n");
        return 2;
    }
    const QString out = args[1];
    QDir().mkpath(out);
    uitest::setup({args[2].toStdString()}, std::vector<std::string>());
    AppState& app = AppState::get();
    app.newProject();
    uint64_t eventId = 0;
    app.edit("clip", [&](Project& p) {
        p.settings.width = 320;
        p.settings.height = 180;
        p.settings.fpsNum = 30;
        p.settings.fpsDen = 1;
        MediaItem m;
        m.id = p.newId();
        m.path = "synth:1";
        m.name = "synth";
        std::unique_ptr<MediaReader> r = app.mediaOpener(m.path, nullptr);
        if (r) m.info = r->info();
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 10;
        eventId = e.id;
        t.events.push_back(e);
        p.tracks.push_back(t);
    });
    FxTarget target;
    target.kind = FxTarget::Event;
    target.id = eventId;
    check(app.addVideoFx(target, "reno.test.invert", "Test Invert") != 0, "the event has the slow test effect");

    Playback pb;
    pb.setAudioOutput(createSilentOutput());
    PreviewWidget pv(&pb);
    pv.resize(640, 420);
    pv.show();
    QWidget* indicator = pv.findChild<QWidget*>("previewRendering");
    check(indicator != nullptr, "the preview has the rendering indicator");
    if (!indicator) return uitest::summary();
    auto shown = [&] { return indicator->isVisible(); };
    uint64_t frames = pb.framesShown();
    auto asked = [&](double t) {
        frames = pb.framesShown();
        app.setCursor(t);
        pb.requestFrame();
    };

    // ---------------------------------------------------------------- a quick picture: no indicator
    unsetenv("TESTFX_SLOW_RENDER_MS");
    uitest::settle(300);
    asked(1.0);
    const bool quickShown = sawWithin(0.5, shown);
    check(!quickShown && pb.framesShown() > frames, "a picture that comes at once shows no indicator");

    // ---------------------------------------------------------------- a slow paused picture
    setenv("TESTFX_SLOW_RENDER_MS", "700", 1);
    asked(2.0);
    const bool early = sawWithin(0.1, shown);
    const bool comes = waitFor(0.6, shown);
    uitest::shot(&pv, out + "/rendering.png");
    check(!early && comes, "a picture that takes a while: the indicator comes after a moment (not at once)");
    const QRect pic = pv.pictureRect();  // in the picture widget's coordinates, as the indicator's
    check(pic.contains(indicator->geometry()) && indicator->x() > pic.center().x() && indicator->y() < pic.top() + 20, "in the picture's top right corner",
          strfmt("%d,%d", indicator->x(), indicator->y()));
    const bool gone = waitFor(2.0, [&] { return !shown(); });
    check(gone && pb.framesShown() > frames, "and goes when the picture comes");
    check(pb.renderingFor() == 0.0, "(nothing is being waited for then)");

    // ---------------------------------------------------------------- scrubbing: requests given up for newer ones
    asked(3.0);
    waitFor(0.3, shown);
    asked(3.5);
    uitest::settle(50);
    asked(4.0);
    const bool during = waitFor(0.6, shown);
    const bool afterScrub = waitFor(4.0, [&] { return !shown(); });
    uitest::settle(200);
    check(during && afterScrub && !shown(), "scrubbing over the slow effect: shown while waiting, gone once the last picture is rendered");

    // ---------------------------------------------------------------- playing started while waiting, then stopped
    asked(5.0);
    waitFor(0.6, shown);
    pb.play();
    const bool whilePlaying = sawWithin(1.2, shown);
    pb.stop();
    const bool afterStop = waitFor(4.0, [&] { return !shown() && pb.renderingFor() == 0.0; });
    uitest::settle(300);
    check(whilePlaying, "playing a slow effect (frames dropped): the indicator shows");
    check(afterStop && !shown(), "stopped: it does not stay behind once the paused picture is rendered");

    // ---------------------------------------------------------------- quick again
    unsetenv("TESTFX_SLOW_RENDER_MS");
    uitest::settle(1200);
    asked(6.0);
    check(!sawWithin(0.5, shown), "quick pictures again: no indicator");
    pb.play();
    const bool quickPlay = sawWithin(1.0, shown);
    pb.stop();
    uitest::settle(300);
    check(!quickPlay, "nor while playing quick pictures");
    return uitest::summary();
}
