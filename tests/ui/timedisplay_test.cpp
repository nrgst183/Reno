// The time display over the track headers and the ruler at frame rates over 100 (src/engine/timefmt.cpp formatTime,
// src/app/timeline.cpp): Time & Frames writes the frame number with two digits, three from frame 100 on (00:02:06:102, then 00:02:07:06), so
// the time display's text keeps its size and width from one frame to the next.
//
//   timedisplay_test <output folder>
//
// Build: uibuild timedisplay_test tests/ui/timedisplay_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/midisync.h"
#include "../../src/app/playback.h"
#include "../../src/app/timesource.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QImage>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

// The columns the time's digits take in the time display (its lower part, under the format's name): the first and last column
// with a pixel unlike the display's background.
std::pair<int, int> textSpan(TimelineWidget& w)
{
    const QRect r = w.timeDisplayRect();
    const QImage img = w.grab(r).toImage();
    const QRgb back = img.pixel(img.width() / 2, 13);  // between the format's name and the time
    int first = -1, last = -1;
    for (int x = 10; x < img.width() - 8; ++x)
        for (int y = 16; y < img.height() - 2; ++y) {
            const QRgb c = img.pixel(x, y);
            if (std::abs(qRed(c) - qRed(back)) + std::abs(qGreen(c) - qGreen(back)) + std::abs(qBlue(c) - qBlue(back)) > 90) {
                if (first < 0) first = x;
                last = x;
                break;
            }
        }
    return {first, last};
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTimeDisplayTest");
    QApplication::setApplicationName("timedisplay_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("timedisplayout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.newProject();
    uitest::buildDemoProject();
    app.edit("120 fps", [](Project& p) {
        p.settings.fpsNum = 120;
        p.settings.fpsDen = 1;
        p.settings.rulerFormat = "timecode";
        if (!p.tracks.empty() && !p.tracks[0].events.empty()) p.tracks[0].events[0].length = 200;
    });

    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1200, 300);
    w.show();
    uitest::settle(200);
    {
        uitest::settle(150);
        const char* name = "VEGAS";
        app.setCursor(126.0 + 102.0 / 120.0);
        uitest::settle(100);
        const std::string a = rulerTime(app.project(), app.cursor());
        const auto spanA = textSpan(w);
        uitest::shot(&w, out + QString("/time120_a_%1.png").arg(name));
        app.setCursor(127.0 + 6.0 / 120.0);
        uitest::settle(100);
        const std::string b = rulerTime(app.project(), app.cursor());
        const auto spanB = textSpan(w);
        uitest::shot(&w, out + QString("/time120_b_%1.png").arg(name));
        check(a == "00:02:06:102" && b == "00:02:07:06", std::string(name) + ": 120 fps reads 00:02:06:102 then 00:02:07:06", a + " " + b);
        // the text is one digit shorter at frame 6 but drawn at the same size: its digits are as wide as the other's
        const int wa = spanA.second - spanA.first, wb = spanB.second - spanB.first;
        check(spanA.first >= 0 && spanB.first >= 0 && wa > wb && wa - wb <= wa / 8,
              std::string(name) + ": the time display's text keeps its size (one digit narrower at frame 6)",
              strfmt("%d..%d and %d..%d", spanA.first, spanA.second, spanB.first, spanB.second));
    }
    // VEGAS's ruler leaves the frames off whole-second marks (00:00:15 over 15-second marks), and its time display shows the
    // cursor's time in the MTC rate with "MTC Input Not Active" over it while no timecode comes in
    {
        app.edit("140 fps", [](Project& p) { p.settings.fpsNum = 140; p.settings.fpsDen = 1; p.settings.rulerFormat = "timecode"; p.settings.rulerStart = 0; });
        const Project& p = app.project();
        check(rulerLabelText(p, 15.0, 15 * 140) == "00:00:15" && rulerLabelText(p, 60.0, 30 * 140) == "00:01:00",
              "ruler: whole-second marks have no frames", rulerLabelText(p, 15.0, 15 * 140));
        check(rulerLabelText(p, 15.0, 70) == "00:00:15:00" && rulerLabelText(p, 15.5, 70) == "00:00:15:70",
              "ruler: marks closer than a second keep their frames", rulerLabelText(p, 15.0, 70) + " " + rulerLabelText(p, 15.5, 70));
        app.edit("time", [](Project& p2) { p2.settings.rulerFormat = "time"; });
        check(rulerLabelText(app.project(), 15.0, 15 * 140) == "00:00:15.000", "ruler: other formats are written as they are",
              rulerLabelText(app.project(), 15.0, 15 * 140));
        app.edit("timecode", [](Project& p2) { p2.settings.rulerFormat = "timecode"; });
        app.setCursor(12.0 + 42.0 / 140.0);
        check(rulerTime(app.project(), app.cursor()) == "00:00:12:42", "cursor readout: 00:00:12:42 at 140 fps", rulerTime(app.project(), app.cursor()));
        MidiSync midi(&playback);
        timeDisplaySource() = TimeDisplaySource::MtcIn;
        uitest::settle(100);
        check(midiTimeDisplayText()(TimeDisplaySource::MtcIn) == "00:00:12:09", "MTC input not active: the cursor's time at 30 fps",
              midiTimeDisplayText()(TimeDisplaySource::MtcIn));
        check(midiTimeDisplayLabel()(TimeDisplaySource::MtcIn) == "MTC Input Not Active", "MTC input not active: its label");
        uitest::shot(&w, out + "/mtc_inactive.png");
        timeDisplaySource() = TimeDisplaySource::Cursor;
    }
    // the ruler, zoomed in to frames: its labels have three-digit frame numbers too, and don't overlap
    app.setCursor(127.0);
    w.zoomToFrames();
    uitest::settle(150);
    uitest::shot(&w, out + "/ruler120.png");
    return uitest::summary();
}
