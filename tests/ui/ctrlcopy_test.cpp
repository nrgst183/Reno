// Two editing gestures, as in VEGAS: Delete with a time selection and no event selected takes that stretch out of
// every track's events (Edit > Delete is enabled for it); Ctrl+drag of an event copies it (its group with it) and moves
// the copies, the originals staying, while a Ctrl+click without a drag still adds an event to the selection or takes it
// out.
//
//   ctrlcopy_test [output folder]
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

struct Ids {
    uint64_t v = 0, a = 0;      // a video track and an audio track
    uint64_t ve = 0, ae = 0;    // a grouped pair of events on them, 2..6 s
    uint64_t other = 0;         // a second, ungrouped audio event 8..10 s
};

static Ids build()
{
    Ids ids;
    st().newProject();
    st().edit("Build", [&](Project& p) {
        MediaItem m;
        m.id = p.newId();
        m.path = "<generated>";
        m.generated = true;
        m.info.hasVideo = m.info.hasAudio = true;
        m.info.duration = 60;
        m.info.width = 640;
        m.info.height = 360;
        p.media.push_back(m);
        Track tv, ta;
        tv.id = p.newId();
        tv.type = TrackType::Video;
        ta.id = p.newId();
        ta.type = TrackType::Audio;
        const uint64_t group = p.newId();
        Event ev, ea, eo;
        ev.id = p.newId();
        ev.mediaId = m.id;
        ev.start = 2.0;
        ev.length = 4.0;
        ev.group = group;
        ea = ev;
        ea.id = p.newId();
        eo = ea;
        eo.id = p.newId();
        eo.group = 0;
        eo.start = 8.0;
        eo.length = 2.0;
        tv.events.push_back(ev);
        ta.events.push_back(ea);
        ta.events.push_back(eo);
        p.tracks.push_back(tv);
        p.tracks.push_back(ta);
        ids = Ids{tv.id, ta.id, ev.id, ea.id, eo.id};
    });
    st().setSelection(std::set<uint64_t>());
    st().setLoop(0, 0);
    return ids;
}

static const Track& trackOf(uint64_t id)
{
    for (const Track& t : st().project().tracks)
        if (t.id == id) return t;
    return st().project().tracks.front();
}

static size_t eventCount()
{
    size_t n = 0;
    for (const Track& t : st().project().tracks) n += t.events.size();
    return n;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoCtrlCopyTest");
    QApplication::setApplicationName("ctrlcopy_test");
    QSettings().clear();
    applyTheme();
    uitest::setup({}, {});
    MainWindow mw;
    mw.resize(1500, 900);
    mw.show();
    uitest::settle(300);
    TimelineWidget& w = *mw.timeline();

    // ---- Delete with a time selection and nothing selected
    {
        Ids ids = build();
        st().snapping = false;
        w.zoomToFit();
        uitest::settle(100);
        st().setLoop(3.0, 5.0);
        uitest::settle(50);
        QAction* del = nullptr;
        for (QAction* a : mw.findChildren<QAction*>())
            if (a->objectName() == "delete" || a->text() == "&Delete") del = a;
        check(del && del->isEnabled(), "Edit > Delete is enabled with a time selection and no event selected");
        w.setFocus();
        uitest::key(&w, Qt::Key_Delete);
        uitest::settle(50);
        const Event* left = st().project().findEvent(ids.ve);
        double vLen = 0;
        for (const Event& e : trackOf(ids.v).events) vLen += e.length;
        check(left && std::fabs(left->length - 1.0) < 1e-6 && std::fabs(vLen - 2.0) < 1e-6,
              "Delete takes the time selection out of the events on every track (3..5 s gone, 2..3 and 5..6 s left)",
              std::to_string(vLen));
        const Event* other = st().project().findEvent(ids.other);
        check(other && other->start == 8.0 && other->length == 2.0, "events outside the time selection are untouched");
    }

    // ---- Ctrl+drag copies; Ctrl+click toggles
    {
        Ids ids = build();
        st().snapping = false;
        w.zoomToFit();
        uitest::settle(100);
        const size_t before = eventCount();
        const QRect r = w.eventRectOf(ids.ae);
        const QPoint from(r.left() + r.width() / 3, r.center().y());
        const QPoint to(from.x() + (w.xAt(9.0) - w.xAt(2.0)) / 2 + (w.xAt(13.0) - w.xAt(9.0)), from.y());  // about 5.5 s right
        uitest::mouseDrag(&w, from, to, 10, Qt::ControlModifier);
        uitest::settle(50);
        check(eventCount() == before + 2, "Ctrl+drag copies the event and its group partner", std::to_string(eventCount()) + " events");
        const Event* ov = st().project().findEvent(ids.ve);
        const Event* oa = st().project().findEvent(ids.ae);
        check(ov && oa && ov->start == 2.0 && oa->start == 2.0, "the originals stay where they were");
        const Event* cv = nullptr;
        const Event* ca = nullptr;
        for (const Event& e : trackOf(ids.v).events)
            if (e.id != ids.ve) cv = &e;
        for (const Event& e : trackOf(ids.a).events)
            if (e.id != ids.ae && e.id != ids.other) ca = &e;
        check(cv && ca && cv->start > 6.0 && std::fabs(cv->start - ca->start) < 1e-9, "the copies moved right together",
              cv ? std::to_string(cv->start) : std::string("none"));
        check(cv && ca && cv->group && cv->group == ca->group && cv->group != ov->group, "the copies are a new group of their own");
        check(cv && st().selection().count(cv->id) && ca && st().selection().count(ca->id) && !st().selection().count(ids.ve),
              "the copies are selected");
        check(st().undoName().find("Copy") != std::string::npos, "one undo step", st().undoName());
        st().undo();
        check(eventCount() == before, "Undo takes the copies away");

        // dragging left works as well
        st().setSelection(std::set<uint64_t>());
        const QRect ro = w.eventRectOf(ids.other);
        uitest::mouseDrag(&w, ro.center(), ro.center() - QPoint((w.xAt(9.0) - w.xAt(2.0)) - 10, 0), 10, Qt::ControlModifier);
        uitest::settle(50);
        double copyStart = -1;
        for (const Event& e : trackOf(ids.a).events)
            if (e.id != ids.ae && e.id != ids.other) copyStart = e.start;
        check(copyStart >= 0 && copyStart < 8.0 && st().project().findEvent(ids.other)->start == 8.0, "Ctrl+drag to the left copies too",
              std::to_string(copyStart));

        // a Ctrl+click without a drag: in and out of the selection, nothing copied
        Ids k = build();
        w.zoomToFit();
        uitest::settle(50);
        const size_t n0 = eventCount();
        uitest::mouseClick(&w, w.eventRectOf(k.other).center(), Qt::LeftButton, Qt::ControlModifier);
        check(st().selection().count(k.other) == 1, "Ctrl+click adds an event to the selection");
        uitest::mouseClick(&w, w.eventRectOf(k.other).center(), Qt::LeftButton, Qt::ControlModifier);
        check(st().selection().count(k.other) == 0 && eventCount() == n0, "Ctrl+click again takes it out; nothing copied");
    }
    // ---- lengthening an event from near its top takes hold of the edge, not the fade handle
    {
        Ids ids = build();
        st().snapping = false;
        w.zoomToFit();
        uitest::settle(100);
        const QRect r = w.eventRectOf(ids.other);
        for (int dy : {2, 5, 9}) {
            st().edit("Reset", [&](Project& p) {
                Event* e = p.findEvent(ids.other);
                e->length = 2.0;
                e->fadeOut = 0;
            });
            const QPoint from(r.right(), r.top() + dy);
            uitest::mouseDrag(&w, from, from + QPoint(40, 0), 8);
            const Event* e = st().project().findEvent(ids.other);
            check(e && e->length > 2.05 && e->fadeOut == 0, "a drag on the right edge " + std::to_string(dy) + " px from the top lengthens the event",
                  e ? std::to_string(e->length) + " fade " + std::to_string(e->fadeOut) : std::string());
        }
        // the corner triangle inside the event still makes a fade
        st().edit("Reset", [&](Project& p) { p.findEvent(ids.other)->length = 2.0; });
        const QRect r2 = w.eventRectOf(ids.other);
        uitest::mouseDrag(&w, QPoint(r2.right() - 4, r2.top() + 2), QPoint(r2.right() - 40, r2.top() + 2), 8);
        const Event* e = st().project().findEvent(ids.other);
        check(e && e->fadeOut > 0.05 && std::fabs(e->length - 2.0) < 1e-6, "a drag from the top corner inside the event makes a fade out",
              e ? std::to_string(e->fadeOut) : std::string());
    }
    return uitest::summary();
}
