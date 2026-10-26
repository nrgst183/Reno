// A file with picture and sound put on the timeline: the two events must be one group (VEGAS's default) - clicking one selects
// both, dragging moves both, deleting deletes both - and the sound keeps its pitch when the pair is sped up (élastique, VEGAS's
// default stretch method).
//   group_test <output folder>
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QDockWidget>
#include <QTabBar>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

static QPoint at(TimelineWidget* w, uint64_t trackId, double t)
{
    const QRect r = w->trackRect(trackId);
    return QPoint(w->xAt(t), r.center().y() + 6);
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoGroupTest");
    QApplication::setApplicationName("group_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    QDir().mkpath(args.size() > 1 ? args[1] : QString("group"));
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    MainWindow w;
    w.resize(1600, 900);
    w.show();
    uitest::settle(300);
    st().newProject();
    std::string err;
    const uint64_t media = st().importMedia("synth:1", &err);
    const MediaItem* m = st().project().findMedia(media);
    check(m && m->info.hasVideo && m->info.hasAudio, "the test file has picture and sound", err);
    const std::vector<uint64_t> made = st().addMediaToTimeline(media, 1.0, 0);
    check(made.size() == 2, "putting it on the timeline makes two events", std::to_string(made.size()));
    uint64_t v = 0, a = 0, vt = 0, at_ = 0;
    for (const auto& t : st().project().tracks)
        for (const auto& e : t.events) {
            if (t.type == TrackType::Video) { v = e.id; vt = t.id; }
            else { a = e.id; at_ = t.id; }
        }
    const Event* ve = st().project().findEvent(v);
    const Event* ae = st().project().findEvent(a);
    check(ve && ae && ve->group != 0 && ve->group == ae->group, "the picture and the sound are one group");
    check(ae && ae->keepPitch && ae->stretchMethod == "elastique", "the sound keeps its pitch with elastique when stretched",
          ae ? ae->stretchMethod + (ae->keepPitch ? " keep" : " no-keep") : "");

    TimelineWidget* tl = w.timeline();
    tl->zoomToFit();
    uitest::settle(100);
    st().setSelection(std::set<uint64_t>());
    uitest::mouseClick(tl, at(tl, vt, 3.0));
    uitest::settle(50);
    check(st().selection().count(v) && st().selection().count(a), "clicking the picture selects the sound too",
          std::to_string(st().selection().size()));
    // drag the picture 2 s later: the sound comes with it
    const double before = st().project().findEvent(a)->start;
    uitest::mouseDrag(tl, at(tl, vt, 3.0), at(tl, vt, 5.0));
    uitest::settle(50);
    const Event* a2 = st().project().findEvent(a);
    const Event* v2 = st().project().findEvent(v);
    check(a2 && v2 && std::fabs(a2->start - v2->start) < 1e-6 && a2->start > before + 1.0, "dragging the picture moves the sound with it",
          a2 ? std::to_string(a2->start) : "");
    // Normalize: the waveform is drawn again at its new height (as in VEGAS)
    {
        // a quiet event, so normalizing makes a visible difference
        // at full volume, as events usually are: Normalize raises the level past 1
        st().edit("Full", [&](Project& p) { if (Event* e = p.findEvent(a)) e->gain = 1.0; });
        uitest::settle(300);
        const QRect lane = tl->trackRect(at_);
        const QImage before = tl->grab(lane).toImage();
        st().setNormalize(std::set<uint64_t>{a}, true);
        uitest::settle(300);
        const Event* na = st().project().findEvent(a);
        const QImage after = tl->grab(lane).toImage();
        check(na && na->normalize, "Normalize switches the event on");
        check(na && na->normalizeGain > 1.01, "a quiet sound is raised", na ? std::to_string(na->normalizeGain) : "");
        check(before != after, "and its waveform is drawn again, taller");
    }

    // the docked windows' tabs have close buttons: closing the Explorer's tab takes it out of the layout
    QDockWidget* explorer = w.findChild<QDockWidget*>("dockExplorer");
    bool closed = false;
    for (QTabBar* bar : w.findChildren<QTabBar*>(QString(), Qt::FindDirectChildrenOnly))
        for (int k = 0; k < bar->count(); ++k)
            if (bar->tabText(k) == "Explorer" && bar->tabsClosable()) {
                emit bar->tabCloseRequested(k);
                closed = true;
                break;
            }
    uitest::settle(50);
    check(closed && explorer && !explorer->isVisible(), "a docked window's tab closes it");
    return uitest::summary();
}
