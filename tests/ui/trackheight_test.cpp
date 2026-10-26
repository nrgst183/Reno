// Track heights as in VEGAS (src/app/timeline.cpp): the bottom edge of a track's header dragged with the mouse makes the track taller
// or shorter (all the selected tracks together, if it is one of them), with VEGAS's resize pointer; Escape cancels the drag; the least
// a track goes to is the minimised height, the most is large; Ctrl+Shift+Up / Down change every track (those with heights of their
// own too, and back again); ` minimises every track and brings them back; Ctrl+` restores the default heights. The heights are the
// tracks' own (Track::height) and are saved in the .veg, as VEGAS saves them.
//
//   trackheight_test <output folder>
//
// Build: uibuild trackheight_test tests/ui/trackheight_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/playback.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>

using namespace reno;
using namespace reno::app;
using uitest::check;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTrackHeightTest");
    QApplication::setApplicationName("trackheight_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("trackheightout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.newProject();
    uitest::buildDemoProject();

    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1400, 560);
    w.show();
    w.zoomToFit();
    uitest::settle(300);
    const Project& p = app.project();
    if (p.tracks.size() < 3) {
        check(false, "the demo project has tracks");
        return uitest::summary();
    }
    const uint64_t t0 = p.tracks[0].id, t1 = p.tracks[1].id, t2 = p.tracks[2].id;
    auto h = [&](uint64_t id) { return w.headerRect(id).height(); };
    auto own = [&](uint64_t id) { const Track* t = const_cast<Project&>(app.project()).findTrack(id); return t ? t->height : -1; };
    auto edge = [&](uint64_t id) { return QPoint(60, w.headerRect(id).bottom() - 1); };
    const int h0 = h(t0), h1 = h(t1), h2 = h(t2);

    // ---------------------------------------------------------------- the pointer
    {
        uitest::mouseMove(&w, edge(t0), Qt::NoButton);
        const bool onEdge = w.cursor().shape() == Qt::SplitVCursor;
        uitest::mouseMove(&w, QPoint(60, w.headerRect(t0).top() + h0 / 2), Qt::NoButton);
        const bool inMiddle = w.cursor().shape() != Qt::SplitVCursor;
        uitest::mouseMove(&w, QPoint(w.timeDisplayRect().width() + 200, w.headerRect(t0).bottom() - 1), Qt::NoButton);
        const bool inLane = w.cursor().shape() != Qt::SplitVCursor;
        check(onEdge && inMiddle && inLane, "the resize pointer over a header's bottom edge (not in the header's middle, not over the events)");
    }

    // ---------------------------------------------------------------- one track dragged taller and shorter
    {
        const size_t undo0 = app.undoCount();
        uitest::mouseDrag(&w, edge(t0), edge(t0) + QPoint(0, 40));
        uitest::settle(50);
        check(h(t0) == h0 + 40 && own(t0) == h0 + 40, "dragging the bottom edge of a header down 40 pixels makes the track 40 taller",
              strfmt("%d -> %d (Track::height %d)", h0, h(t0), own(t0)));
        check(h(t1) == h1 && h(t2) == h2 && w.headerRect(t1).top() == w.headerRect(t0).bottom() + 1, "the other tracks keep their heights and move down");
        check(app.undoCount() == undo0 + 1 && app.undoName() == "Track Height", "one undo step, \"Track Height\"", app.undoName());
        uitest::shot(&w, out + "/th_taller.png");
        app.undo();
        uitest::settle(50);
        check(h(t0) == h0 && own(t0) == 0, "undo puts the default height back");
        // up past the least height: the minimised height (VEGAS's collapsed header), no less
        uitest::mouseDrag(&w, edge(t0), edge(t0) - QPoint(0, 300));
        uitest::settle(50);
        check(h(t0) == 24, "dragged far up, a track stops at the least height (24)", strfmt("%d", h(t0)));
        // the edge of a small track is still there to take hold of
        uitest::mouseDrag(&w, edge(t0), edge(t0) + QPoint(0, 2000), 20);
        uitest::settle(50);
        check(h(t0) == 1000, "and dragged far down, at most 1000", strfmt("%d", h(t0)));
        app.undo();
        app.undo();
        uitest::settle(50);
        check(h(t0) == h0, "(undone)");
    }

    // ---------------------------------------------------------------- Escape in the middle of the drag
    {
        const size_t undo0 = app.undoCount();
        const QPoint e = edge(t1);
        uitest::mousePress(&w, e);
        uitest::mouseMove(&w, e + QPoint(0, 10));
        uitest::mouseMove(&w, e + QPoint(0, 30));
        const int during = h(t1);
        const bool cancelled = w.cancelMouseDrag();
        uitest::mouseRelease(&w, e + QPoint(0, 30));
        uitest::settle(50);
        check(during == h1 + 30 && cancelled && h(t1) == h1 && own(t1) == 0 && app.undoCount() == undo0,
              "the track follows the mouse while dragging; Escape puts it back with no undo step", strfmt("during %d, after %d", during, h(t1)));
    }

    // ---------------------------------------------------------------- the selected tracks together
    {
        uitest::mouseClick(&w, QPoint(60, w.headerRect(t1).top() + 8));
        uitest::mouseClick(&w, QPoint(60, w.headerRect(t2).top() + 8), Qt::LeftButton, Qt::ControlModifier);
        uitest::settle(50);
        uitest::mouseDrag(&w, edge(t1), edge(t1) + QPoint(0, 20));
        uitest::settle(50);
        check(h(t1) == h1 + 20 && h(t2) == h2 + 20 && h(t0) == h0, "with two tracks selected, dragging one's edge resizes both by as much (not the others)",
              strfmt("%d %d %d", h(t0), h(t1), h(t2)));
        check(app.undoName() == "Track Heights", "(\"Track Heights\")", app.undoName());
        // a track that is not among the selected ones: only itself
        uitest::mouseDrag(&w, edge(t0), edge(t0) + QPoint(0, 12));
        uitest::settle(50);
        check(h(t0) == h0 + 12 && h(t1) == h1 + 20 && h(t2) == h2 + 20, "a track outside the selection is resized alone");
        // a double-click on the edge does not start renaming or anything else
        uitest::mouseDoubleClick(&w, edge(t0));
        uitest::settle(50);
        check(h(t0) == h0 + 12, "a double-click on the edge leaves the height alone");
    }

    // ---------------------------------------------------------------- Ctrl+Shift+Up / Down, `, Ctrl+`
    {
        const int a = h(t0), b = h(t2);
        w.changeTrackHeights(1);
        uitest::settle(50);
        const int a1 = h(t0), b1 = h(t2);
        check(a1 > a && b1 > b, "Ctrl+Shift+Up makes every track taller, those with heights of their own too", strfmt("%d->%d, %d->%d", a, a1, b, b1));
        w.changeTrackHeights(-1);
        uitest::settle(50);
        check(h(t0) < a1 && h(t2) < b1, "Ctrl+Shift+Down shorter");
        for (int k = 0; k < 30; ++k) w.changeTrackHeights(1);
        uitest::settle(50);
        check(h(t0) == h(t1) && h(t1) == h(t2) && h(t0) >= 600, "pressed on, the tracks come to match (at the largest step)", strfmt("%d %d %d", h(t0), h(t1), h(t2)));
        for (int k = 0; k < 60; ++k) w.changeTrackHeights(-1);  // the ladder has some 34 steps
        uitest::settle(50);
        check(h(t0) == h(t1) && h(t0) == 24, "and the other way, at the least height", strfmt("%d %d", h(t0), h(t1)));
        w.resetTrackHeights();
        uitest::settle(50);
        check(h(t0) == h0 && h(t1) == h1 && h(t2) == h2 && own(t0) == 0 && own(t2) == 0, "Ctrl+` puts the default heights back");
        w.changeTrackHeights(1);
        w.changeTrackHeights(-1);
        uitest::settle(50);
        check(h(t0) == h0 && h(t1) == h1 && own(t0) == 0, "from the default heights, Up then Down comes back to them exactly");
        uitest::mouseDrag(&w, edge(t1), edge(t1) + QPoint(0, 30));
        w.minimizeAllTracks();
        uitest::settle(50);
        check(h(t0) == 24 && h(t1) == 24 && h(t2) == 24, "` minimises every track");
        w.minimizeAllTracks();
        uitest::settle(50);
        check(h(t0) == h0 && h(t1) == h1 + 30 && h(t2) == h2 + 30, "and ` again brings them back as they were (t1 and t2, still selected, resized together)",
              strfmt("%d %d %d", h(t0), h(t1), h(t2)));
    }

    // ---------------------------------------------------------------- the track menu's Maximize / Restore Track Height
    {
        const int before = h(t0), ownBefore = own(t0);
        app.currentTrack = t0;
        w.toggleMaximizeTrack();
        uitest::settle(50);
        const int big = h(t0);
        uitest::shot(&w, out + "/maximized.png");
        w.toggleMaximizeTrack();
        uitest::settle(50);
        check(big > before && big >= w.height() - 120 && h(t0) == before && own(t0) == ownBefore,
              "Maximize Track Height fills the timeline's height (not saved as the track's height); again puts it back", strfmt("%d -> %d -> %d", before, big, h(t0)));
    }

    // ---------------------------------------------------------------- kept in the project (.veg)
    {
        const std::string path = (out + "/heights.veg").toStdString();
        std::string err;
        const bool saved = app.save(path, &err);
        app.newProject();
        uitest::settle(50);
        const bool opened = saved && app.open(path, &err);
        uitest::settle(100);
        const Project& q = app.project();
        int found = 0;
        for (const Track& t : q.tracks)
            if (t.height == h1 + 30) ++found;
        check(saved && opened && found == 1, "a track's height is saved in the .veg and read back", err + strfmt(" (%d)", found));
    }
    return uitest::summary();
}
