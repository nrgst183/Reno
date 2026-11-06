// Behaviour seen in the VEGAS Pro 18 beginner tutorial (second pass): the amount of a drag shown on the loop bar (a blue line and a
// signed seconds.frames box at the moving edge), the white cross-fade curve and overlap length while an event is dragged over its
// neighbour, and a new mask path that starts with Feather type None.
//
//   tut3a_test <output folder>
//
// QT_QPA_PLATFORM=offscreen. Saves drag_move.png, drag_trim.png and drag_stretch.png.
#include "../../src/app/timeline.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

// how many pixels of 'rect' in the picture are close to 'c'
static int countNear(const QImage& im, QRect rect, QColor c, int tol)
{
    int n = 0;
    for (int y = std::max(0, rect.top()); y <= std::min(im.height() - 1, rect.bottom()); ++y)
        for (int x = std::max(0, rect.left()); x <= std::min(im.width() - 1, rect.right()); ++x) {
            const QColor p = im.pixelColor(x, y);
            if (std::abs(p.red() - c.red()) <= tol && std::abs(p.green() - c.green()) <= tol && std::abs(p.blue() - c.blue()) <= tol) ++n;
        }
    return n;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTut3aTest");
    QApplication::setApplicationName("tut3a_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("tut3a_out");
    QDir().mkpath(out);
    uitest::setup({}, {});

    check(MaskPath().featherType == 2, "the model default stays Out: masks loaded from .veg files do not carry the field");

    st().newProject();
    std::string err;
    const uint64_t clip = st().importMedia("synth:10", &err);
    uint64_t track = 0, e1 = 0, e2 = 0;
    st().edit("Build", [&](Project& p) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.name = "Video";
        p.tracks.push_back(t);
        track = t.id;
        auto add = [&](double start, double len) {
            Event e;
            e.id = p.newId();
            e.mediaId = clip;
            e.start = start;
            e.length = len;
            e.name = "clip";
            p.tracks[0].events.push_back(e);
            return e.id;
        };
        e1 = add(1.0, 3.0);
        e2 = add(4.0, 3.0);
        p.autoCrossfades = true;
    });
    st().setSelection(std::set<uint64_t>());
    st().setCursor(0.0);
    st().snapping = false;

    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1400, 420);
    w.show();
    w.zoomToFit();
    uitest::settle(300);
    const double fps = st().project().settings.fps();
    auto ev = [&](uint64_t id) { return st().project().findEvent(id); };
    const QRect tr = w.trackRect(track);
    const int y = tr.center().y() + 6;
    // the loop bar: the strip just above the ruler
    const QRect bar(w.xAt(0.0), 17, w.width() - w.xAt(0.0), 12);
    const QColor blue(60, 130, 230);

    // ---- move: 0.5 s to the left, over the end of the first event
    {
        const QImage before = uitest::shot(&w, out + "/drag_before.png");
        check(countNear(before, bar, blue, 6) == 0, "no drag amount on the loop bar before a drag");
        const QPoint from(w.xAt(5.5), y), to(w.xAt(5.0), y);
        uitest::mousePress(&w, from);
        const int steps = 6;
        for (int k = 1; k <= steps; ++k) uitest::mouseMove(&w, from + (to - from) * k / steps);
        uitest::settle(50);
        const QImage during = uitest::shot(&w, out + "/drag_move.png");
        check(countNear(during, bar, blue, 6) > 6, "while an event is moved, the loop bar shows a blue line for the amount");
        check(countNear(during, bar, QColor(255, 255, 255), 3) > 20, "... and a white box with the amount");
        const QRect overlap(w.xAt(3.5) + 1, tr.top() + 1, std::max(4, w.xAt(4.0) - w.xAt(3.5) - 2), tr.height() - 2);
        check(countNear(during, overlap, QColor(255, 255, 255), 3) > 20, "... and the overlap with the neighbour shows its length in a white box");
        check(std::fabs(ev(e2)->start - 3.5) < 1.5 / fps, "the event follows the pointer", std::to_string(ev(e2)->start));
        uitest::mouseRelease(&w, to);
        uitest::settle(50);
        const QImage after = uitest::shot(&w, out + "/drag_after.png");
        check(countNear(after, bar, blue, 6) == 0, "the amount goes away when the button is let go");
    }
    st().undo();
    uitest::settle(50);

    // ---- trim the end of an event
    {
        const QPoint from(w.xAt(7.0) - 1, y), to(w.xAt(6.0), y);
        uitest::mousePress(&w, from);
        for (int k = 1; k <= 6; ++k) uitest::mouseMove(&w, from + (to - from) * k / 6);
        uitest::settle(50);
        const QImage during = uitest::shot(&w, out + "/drag_trim.png");
        check(countNear(during, bar, blue, 6) > 6, "while an edge is trimmed, the loop bar shows the amount");
        uitest::mouseRelease(&w, to);
        uitest::settle(50);
    }
    st().undo();
    uitest::settle(50);

    // ---- stretch (Ctrl+drag of an edge)
    {
        const QPoint from(w.xAt(7.0) - 1, y), to(w.xAt(8.0), y);
        uitest::mousePress(&w, from, Qt::LeftButton, Qt::ControlModifier);
        for (int k = 1; k <= 6; ++k) uitest::mouseMove(&w, from + (to - from) * k / 6, Qt::LeftButton, Qt::ControlModifier);
        uitest::settle(50);
        const QImage during = uitest::shot(&w, out + "/drag_stretch.png");
        check(countNear(during, bar, blue, 6) > 6, "while an edge is stretched, the loop bar shows the amount");
        uitest::mouseRelease(&w, to, Qt::LeftButton, Qt::ControlModifier);
        uitest::settle(50);
    }
    return uitest::summary();
}
