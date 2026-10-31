// The timeline keeps a picture of itself while only the cursor moves (playing) and paints the cursor's strips from it. This checks
// that what is on screen after such paints is the same pixels as a whole repaint of the timeline, after the things
// that change the picture (a selection, a time selection, zooming, a resize), and that the strips really are painted from the picture.
//
//   timeline_layer_test <output folder>
//
// QT_QPA_PLATFORM=offscreen. Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>
#include <QWindow>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

// What the window shows now (the backing store as the offscreen screen gives it) against a whole repaint of the timeline.
// Returns the number of differing pixels.
int differing(QWidget& win, TimelineWidget& tl, const QString& saveAs)
{
    const QImage screen = win.screen()->grabWindow(win.winId()).toImage().convertToFormat(QImage::Format_RGB32);
    const QPoint origin = tl.mapTo(&win, QPoint(0, 0));
    const QImage shown = screen.copy(QRect(origin, tl.size()));
    const QImage whole = tl.grab().toImage().convertToFormat(QImage::Format_RGB32);
    if (shown.size() != whole.size()) return -1;
    int n = 0;
    QImage diff(whole.size(), QImage::Format_RGB32);
    diff.fill(Qt::black);
    for (int y = 0; y < whole.height(); ++y)
        for (int x = 0; x < whole.width(); ++x)
            if (shown.pixel(x, y) != whole.pixel(x, y)) {
                ++n;
                diff.setPixel(x, y, qRgb(255, 0, 0));
            }
    if (n && !saveAs.isEmpty()) {
        shown.save(saveAs + "_shown.png");
        whole.save(saveAs + "_whole.png");
        diff.save(saveAs + "_diff.png");
    }
    return n;
}

// The cursor moves a little at a time, as playing does, with a paint after each step; returns how many paints came from the picture.
int play(TimelineWidget& tl, double from, double step, int steps)
{
    const int before = tl.layerPaints();
    for (int k = 0; k < steps; ++k) {
        st().setCursor(from + k * step);
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, 0);
        QApplication::processEvents();
    }
    return tl.layerPaints() - before;
}

void session(const QString& out, const char* look, QWidget& win, TimelineWidget& tl)
{
    const std::string tag = look;
    const QString base = out + "/layer_" + look;
    st().setCursor(0.2);
    uitest::settle(100);
    int used = play(tl, 0.3, 0.0166, 14);
    check(used >= 6, tag + ": strips come from the kept picture while the cursor moves", std::to_string(used) + " paints");
    int n = differing(win, tl, base + "_a");
    check(n == 0, tag + ": the screen equals a whole repaint after cursor moves", std::to_string(n) + " pixels differ");

    // a selection changes what an event looks like: the picture must go and be made again
    for (const Track& t : st().project().tracks)
        if (!t.events.empty()) { st().selectOnly(t.events.front().id); break; }
    uitest::settle(100);
    used = play(tl, 0.6, 0.0166, 14);
    check(used >= 6, tag + ": the picture is made again after a selection", std::to_string(used) + " paints");
    n = differing(win, tl, base + "_b");
    check(n == 0, tag + ": the screen equals a whole repaint after selecting an event", std::to_string(n) + " pixels differ");

    // a time selection
    st().setLoop(0.5, 2.0);
    uitest::settle(100);
    play(tl, 0.9, 0.0166, 12);
    n = differing(win, tl, base + "_c");
    check(n == 0, tag + ": equal after a time selection", std::to_string(n) + " pixels differ");

    // zoom: everything moves
    tl.zoomOut();
    uitest::settle(100);
    play(tl, 1.0, 0.0166, 12);
    n = differing(win, tl, base + "_d");
    check(n == 0, tag + ": equal after zooming", std::to_string(n) + " pixels differ");
    tl.zoomIn();
    uitest::settle(100);

    // a resize
    win.resize(win.width() - 90, win.height() - 40);
    uitest::settle(150);
    play(tl, 1.3, 0.0166, 12);
    n = differing(win, tl, base + "_e");
    check(n == 0, tag + ": equal after a resize", std::to_string(n) + " pixels differ");
    win.resize(win.width() + 90, win.height() + 40);
    uitest::settle(150);

    // an edit (the project changes under the cursor strips)
    st().setLoop(0, 0);
    uitest::settle(50);
    used = play(tl, 1.5, 0.0166, 8);
    st().edit("Move", [](Project& p) {
        for (Track& t : p.tracks)
            if (!t.events.empty()) { t.events.front().start += 0.2; break; }
    });
    uitest::settle(100);
    used = play(tl, 1.7, 0.0166, 14);
    check(used >= 6, tag + ": the picture is made again after an edit", std::to_string(used) + " paints");
    n = differing(win, tl, base + "_f");
    check(n == 0, tag + ": the screen equals a whole repaint after an edit", std::to_string(n) + " pixels differ");
    st().undo();
    uitest::settle(100);

    // the cursor jumping far (a click on the ruler) and coming back
    play(tl, 2.0, 0.0166, 4);
    st().setCursor(0.1);
    uitest::settle(100);
    play(tl, 0.1, 0.0166, 12);
    n = differing(win, tl, base + "_g");
    check(n == 0, tag + ": equal after the cursor jumped", std::to_string(n) + " pixels differ");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTimelineLayerTest");
    QApplication::setApplicationName("timeline_layer_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: timeline_layer_test <output folder>\n");
        return 2;
    }
    const QString out = QDir(args[1]).absolutePath();
    QDir().mkpath(out);
    uitest::setup({}, {});
    Playback pb;
    QWidget win;
    QVBoxLayout* lay = new QVBoxLayout(&win);
    lay->setContentsMargins(0, 0, 0, 0);
    TimelineWidget* tl = new TimelineWidget(&pb, &win);
    lay->addWidget(tl);
    win.resize(1100, 520);
    uitest::buildDemoProject();
    win.show();
    uitest::settle(300);
    session(out, "vegas", win, *tl);
    return uitest::summary();
}
