// VEGAS Pro 18 manual pages 114-251 (editing events, the Trimmer, multicamera, stereoscopic 3D, markers and commands, track
// editing, automation). Envelopes: the brush (Shift+drag), Thin All / Selected Points, Flip Selected Points, time selection
// envelope fades, copying and pasting points with the Envelope tool; the rest section by section below.
//
//   man2_test <output folder>
//
// Build: uibuild man2_test tests/ui/man2_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/app/man2dialogs.h"
#include <QAbstractButton>
#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QToolButton>
#include <QMouseEvent>
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

struct Ids {
    uint64_t v1 = 0, v2 = 0, a1 = 0, a2 = 0;
    uint64_t clip = 0, snd = 0;
    uint64_t e1 = 0, e1a = 0, e3 = 0, e4 = 0;
} ids;

uint64_t addEvent(Project& p, uint64_t track, uint64_t media, double start, double length, double offset = 0, uint64_t group = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = offset;
    e.group = group;
    e.name = p.findMedia(media)->name;
    p.findTrack(track)->events.push_back(e);
    p.findTrack(track)->sortEvents();
    return e.id;
}

//   V1: e1 clip 1..5 (grouped with e1a)       V2: e3 2..7
//   A1: e1a 1..5                              A2: e4 sound 0.5..6.5
void buildProject()
{
    st().newProject();
    std::string err;
    ids.clip = st().importMedia("synth:1", &err);
    ids.snd = st().importMedia("synthaudio:2", &err);
    st().edit("Build", [&](Project& p) {
        const char* names[] = {"Titles", "Camera", "Dialogue", "Music"};
        for (int k = 0; k < 4; ++k) {
            Track t;
            t.id = p.newId();
            t.type = k < 2 ? TrackType::Video : TrackType::Audio;
            t.name = names[k];
            p.tracks.push_back(t);
        }
        ids.v1 = p.tracks[0].id;
        ids.v2 = p.tracks[1].id;
        ids.a1 = p.tracks[2].id;
        ids.a2 = p.tracks[3].id;
        const uint64_t g = p.newId();
        ids.e1 = addEvent(p, ids.v1, ids.clip, 1.0, 4.0, 0, g);
        ids.e1a = addEvent(p, ids.a1, ids.clip, 1.0, 4.0, 0, g);
        ids.e3 = addEvent(p, ids.v2, ids.clip, 2.0, 5.0, 1.0);
        ids.e4 = addEvent(p, ids.a2, ids.snd, 0.5, 6.0);
    });
    st().setSelection(std::set<uint64_t>());
    st().setCursor(0.0);
    st().setLoop(0, 0);
    st().snapping = false;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

EnvelopeRef trackEnv(uint64_t track, const std::string& kind, uint64_t target = 0)
{
    EnvelopeRef r;
    r.owner.kind = EnvelopeOwner::Track;
    r.owner.id = track;
    r.kind = kind;
    r.target = target;
    return r;
}

const Envelope* envOf(const EnvelopeRef& r) { return AppState::findEnvelope(st().project(), r); }

const Track* trk(uint64_t id)
{
    for (const Track& t : st().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

QString plain(QString t)
{
    t = t.split('\t')[0];
    t.remove('&');
    return t;
}

// Right-clicks at 'pos' and picks the item 'text' (or 'sub' > 'text'). 'seen' gets the items of the menu; 'enabled' says
// whether the item was enabled.
bool menuPick(QWidget* w, QPoint pos, const QString& text, const QString& sub = QString(), QStringList* seen = nullptr, bool* enabled = nullptr)
{
    bool found = false;
    QTimer::singleShot(120, [&] {
        QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m)
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QMenu* mm = qobject_cast<QMenu*>(t))
                    if (mm->isVisible()) m = mm;
        if (!m) return;
        QMenu* from = m;
        if (!sub.isEmpty())
            for (QAction* a : m->actions())
                if (a->menu() && plain(a->text()) == sub) from = a->menu();
        for (QAction* a : from->actions()) {
            if (seen) *seen << plain(a->text());
            if (!found && plain(a->text()) == text) {
                if (enabled) *enabled = a->isEnabled();
                if (a->isEnabled()) {
                    a->trigger();
                    found = true;
                }
            }
        }
        m->close();
    });
    QContextMenuEvent e(QContextMenuEvent::Mouse, pos, w->mapToGlobal(pos));
    QApplication::sendEvent(w, &e);
    uitest::settle(50);
    return found;
}

// ================================================================================================ envelopes
void envelopeTests(TimelineWidget& w, const QString& out)
{
    const EnvelopeRef vol = trackEnv(ids.a2, "volume");
    st().currentTrack = ids.a2;
    w.toggleTrackEnvelope("volume");
    uitest::settle(100);
    check(envOf(vol) && envOf(vol)->points.size() == 1, "a volume envelope to work on");
    const int lineY = w.envelopePointPos(vol, 0).y();

    // the brush: Shift+drag over the line draws points
    {
        const QPoint a(w.xAt(1.0), lineY), b(w.xAt(3.0), lineY + 25);
        uitest::mousePress(&w, a, Qt::LeftButton, Qt::ShiftModifier);
        for (int k = 1; k <= 20; ++k) uitest::mouseMove(&w, a + (b - a) * k / 20, Qt::LeftButton, Qt::ShiftModifier);
        uitest::mouseRelease(&w, b, Qt::LeftButton, Qt::ShiftModifier);
        const Envelope* e = envOf(vol);
        check(e && e->points.size() >= 3, "Shift+drag over an envelope draws points (the envelope brush)", e ? strfmt("%d points", (int)e->points.size()) : "none");
        check(st().undoName() == "Draw Envelope", "(one undo step)", st().undoName());
        if (e && e->points.size() >= 3) {
            const double atEnd = e->valueAt(2.95, 1.0), atStart = e->valueAt(1.05, 1.0);
            check(atEnd < atStart * 0.8, "the drawn line goes down where the mouse went down", strfmt("%.3f -> %.3f", atStart, atEnd));
            check(near(e->points[0].value, 1.0) && near(e->points[0].time, 0.0), "the point before the stroke stays");
            // a straight stroke is thinned to few points (Smooth and thin automation data after drawing, on)
            check(e->points.size() <= 6, "a straight stroke is thinned to a few points", strfmt("%d", (int)e->points.size()));
        }
        st().undo();
        check(envOf(vol)->points.size() == 1, "undo takes the drawing away");
    }
    // Thin All Points: a wobbly line of many points becomes few; a corner stays
    {
        st().edit("Many points", [&](Project& p) {
            Envelope* e = AppState::findEnvelope(p, vol);
            e->points.clear();
            for (int k = 0; k <= 40; ++k) {
                EnvPoint pt;
                pt.time = k * 0.1;
                pt.value = k <= 20 ? 1.0 : 1.0 - (k - 20) * 0.04;  // flat, then a slope down
                pt.curve = CurveLinear;
                e->points.push_back(pt);
            }
        });
        bool en = false;
        check(menuPick(&w, w.envelopePointPos(vol, 5), "Thin All Points", QString(), nullptr, &en), "an envelope point's menu has Thin All Points");
        const Envelope* e = envOf(vol);
        check(e->points.size() >= 3 && e->points.size() <= 4, "Thin All Points leaves the ends and the corner", strfmt("%d points", (int)e->points.size()));
        check(near(e->valueAt(1.0, 0), 1.0, 1e-3) && near(e->valueAt(3.0, 0), 0.6, 0.01), "and the envelope keeps its shape",
              strfmt("%.4f %.4f", e->valueAt(1.0, 0), e->valueAt(3.0, 0)));
        check(st().undoName() == "Thin All Points", "(an undo step)", st().undoName());
        st().undo();
        // Thin Selected Points: only those in the time selection
        st().setLoop(0.0, 1.5);
        check(menuPick(&w, w.envelopePointPos(vol, 25), "Thin Selected Points"), "Thin Selected Points (with a time selection)");
        e = envOf(vol);
        int before = 0, after = 0;
        for (const EnvPoint& pt : e->points) (pt.time <= 1.5 + 1e-9 ? before : after)++;
        check(before == 2 && after == 25, "only the points in the time selection are thinned", strfmt("%d in, %d after", before, after));
        st().undo();
        // Flip Selected Points
        st().setLoop(2.95, 4.05);
        check(menuPick(&w, w.envelopePointPos(vol, 5), "Flip Selected Points"), "Flip Selected Points");
        e = envOf(vol);
        const double f30 = std::cbrt(e->points[30].value / 2.0), f10 = std::cbrt(e->points[10].value / 2.0);
        check(near(f30, 1.0 - std::cbrt(0.6 / 2.0), 0.01) && near(e->points[10].value, 1.0, 1e-9), "the points in the selection are turned upside down, the others stay",
              strfmt("%.3f %.3f", f30, f10));
        st().undo();
        st().setLoop(0, 0);
        bool enabled = true;
        menuPick(&w, w.envelopePointPos(vol, 5), "Flip Selected Points", QString(), nullptr, &enabled);
        check(!enabled, "with no time selection Flip Selected Points is greyed");
        st().edit("One point", [&](Project& p) {
            Envelope* e2 = AppState::findEnvelope(p, vol);
            e2->points.resize(1);
            e2->points[0].value = 1.0;
        });
    }
    // Time selection envelope fades: a drag in the time selection moves the envelope there, with fades outside it
    {
        st().prefs.timeSelEnvFades = true;
        st().prefs.timeSelEnvFadeLength = 0.25;
        st().prefs.timeSelEnvFadeAlign = "outside";
        st().setLoop(2.0, 4.0);
        uitest::settle(50);
        const QPoint at(w.xAt(3.0), w.envelopePointPos(vol, 0).y());
        uitest::mouseDrag(&w, at, at + QPoint(0, 30));
        const Envelope* e = envOf(vol);
        check(e && e->points.size() == 5, "dragging the line in the time selection adds the fades' points", e ? strfmt("%d points", (int)e->points.size()) : "");
        if (e && e->points.size() == 5) {
            check(near(e->points[1].time, 1.75, 1e-6) && near(e->points[1].value, 1.0) && near(e->points[2].time, 2.0) && e->points[2].value < 0.8 &&
                      near(e->points[3].time, 4.0) && near(e->points[3].value, e->points[2].value) && near(e->points[4].time, 4.25) && near(e->points[4].value, 1.0),
                  "the selection goes down, fading in and out over 0.25 s outside it",
                  strfmt("%.2f/%.2f %.2f/%.2f %.2f/%.2f %.2f/%.2f", e->points[1].time, e->points[1].value, e->points[2].time, e->points[2].value, e->points[3].time,
                         e->points[3].value, e->points[4].time, e->points[4].value));
        }
        check(st().undoName() == "Move Envelope Segment", "(one undo step)", st().undoName());
        st().undo();
        st().prefs.timeSelEnvFadeAlign = "inside";
        uitest::mouseDrag(&w, at, at + QPoint(0, 30));
        e = envOf(vol);
        check(e && e->points.size() == 5 && near(e->points[1].time, 2.0) && near(e->points[2].time, 2.25) && near(e->points[4].time, 4.0),
              "Inside: the fades are inside the selection");
        st().undo();
        st().prefs.timeSelEnvFades = false;
        uitest::mouseDrag(&w, at, at + QPoint(0, 30));
        check(envOf(vol)->points.size() == 1, "with the preference off, the line moves as a whole (no fades)");
        st().undo();
        st().prefs.timeSelEnvFades = true;
        st().setLoop(0, 0);
    }
    // copying and pasting envelope points with the Envelope tool
    {
        st().edit("Points", [&](Project& p) {
            Envelope* e = AppState::findEnvelope(p, vol);
            e->points.clear();
            for (double t : {0.0, 1.0, 1.5, 2.0}) {
                EnvPoint pt;
                pt.time = t;
                pt.value = t == 1.5 ? 0.25 : 1.0;
                e->points.push_back(pt);
            }
        });
        w.options().tool = EditTool::Envelope;
        w.optionsChanged();
        st().setLoop(0.9, 2.1);
        uitest::mouseClick(&w, w.envelopePointPos(vol, 2));
        check(w.copyEnvelopePoints(), "Edit > Copy with the Envelope tool copies the points in the time selection");
        st().setLoop(0, 0);
        st().setCursor(4.0);
        check(w.pasteEnvelopePoints(), "Edit > Paste puts them at the cursor");
        const Envelope* e = envOf(vol);
        check(e->points.size() == 7 && near(e->points[4].time, 4.1) && near(e->points[5].time, 4.6) && near(e->points[5].value, 0.25),
              "the pasted points keep their spacing from the selection's start", strfmt("%d points", (int)e->points.size()));
        check(st().undoName() == "Paste Envelope Points", "(an undo step)", st().undoName());
        // into another envelope (pan): the same heights
        const EnvelopeRef pan = trackEnv(ids.a2, "pan");
        w.toggleTrackEnvelope("pan");
        uitest::settle(50);
        uitest::mouseClick(&w, w.envelopePointPos(pan, 0) + QPoint(30, 0));
        st().setCursor(1.0);
        check(w.pasteEnvelopePoints() && envOf(pan)->points.size() == 4, "and into another kind of envelope");
        w.options().tool = EditTool::Normal;
        w.optionsChanged();
        check(!w.copyEnvelopePoints(), "with the Normal tool, Copy copies events again");
    }
    uitest::shot(&w, out + "/envelopes.png");
}


// A drop of 'md' at 'pos' on the timeline, with the right button held (VEGAS's right-drag) or the left.
void dropAt(QWidget* w, QMimeData* md, QPoint pos, bool right)
{
    const Qt::MouseButtons b = right ? Qt::RightButton : Qt::LeftButton;
    QDragEnterEvent enter(pos, Qt::CopyAction, md, b, Qt::NoModifier);
    QApplication::sendEvent(w, &enter);
    QDragMoveEvent move(pos, Qt::CopyAction, md, b, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
    QDropEvent drop(QPointF(pos), Qt::CopyAction, md, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &drop);
    uitest::settle(50);
}

// The right-drag menu's item 'path' ("Video Only/Add Across Time") is picked when the menu shows.
void pickNextMenu(TimelineWidget& w, const QString& path, QStringList* seen = nullptr)
{
    w.rightDragMenu = [path, seen](QMenu* m) -> QAction* {
        QStringList parts = path.split('/');
        QMenu* in = m;
        if (seen) for (QAction* a : m->actions()) *seen << (a->isSeparator() ? QString("-") : plain(a->text()));
        while (parts.size() > 1) {
            QMenu* next = nullptr;
            for (QAction* a : in->actions())
                if (a->menu() && plain(a->text()) == parts.first()) next = a->menu();
            if (!next) return nullptr;
            in = next;
            parts.removeFirst();
        }
        for (QAction* a : in->actions())
            if (plain(a->text()) == parts.first()) return a;
        return nullptr;
    };
}

int eventsOn(uint64_t track)
{
    for (const Track& t : st().project().tracks)
        if (t.id == track) return (int)t.events.size();
    return -1;
}

// ================================================================================================ adding media
void dropTests(TimelineWidget& w)
{
    buildProject();
    std::string err;
    const uint64_t m1 = st().importMedia("synth:3", &err), m2 = st().importMedia("synth:4", &err), m3 = st().importMedia("synthaudio:5", &err);
    // room after the drops: the view cannot zoom out past the whole project, so an event far on makes it long
    st().edit("Room", [](Project& p) { addEvent(p, ids.a2, ids.snd, 40.0, 1.0); });
    w.zoomToFit();
    uitest::settle(50);
    const double len1 = st().project().findMedia(m1)->info.duration, len2 = st().project().findMedia(m2)->info.duration;
    QMimeData md;
    md.setData("application/x-reno-media", QByteArray::number((qulonglong)m2));
    md.setData("application/x-reno-media-list", QString("%1 %2").arg(m2).arg(m1).toUtf8());
    const int y = w.trackRect(ids.v2).center().y();
    const size_t tracksBefore = st().project().tracks.size();
    // Add Across Time: one after another, the one clicked first
    {
        QStringList seen;
        pickNextMenu(w, "Add Across Time", &seen);
        st().setSelection(std::set<uint64_t>());
        dropAt(&w, &md, QPoint(w.xAt(8.0), y), true);
        check(seen.join("|") == "Add Across Time|Add Across Tracks|Add As Takes|-|Video Only|Audio Only|-|Cancel", "a right-drag of media ends with VEGAS's menu",
              seen.join("|").toStdString());
        const Project& p = st().project();
        const Event* a = nullptr;
        const Event* b = nullptr;
        for (const Event& e : trk(ids.v2)->events) {
            if (e.mediaId == m2) a = &e;
            if (e.mediaId == m1) b = &e;
        }
        check(a && b && near(a->start, p.quantize(8.0), 1e-6) && near(b->start, p.quantize(a->start + len2), 0.02),
              "Add Across Time: the files one after another, the one dragged first", a && b ? strfmt("%.3f %.3f", a->start, b->start) : "missing");
        check(st().undoName() == "Add Across Time", "(one undo step)", st().undoName());
        st().undo();
    }
    // Automatically overlap multiple selected media when added
    {
        st().prefs.autoOverlapMedia = true;
        st().prefs.cutToOverlap = 0.5;
        pickNextMenu(w, "Add Across Time");
        dropAt(&w, &md, QPoint(w.xAt(8.0), y), true);
        const Event* a = nullptr;
        const Event* b = nullptr;
        for (const Event& e : trk(ids.v2)->events) {
            if (e.mediaId == m2) a = &e;
            if (e.mediaId == m1) b = &e;
        }
        check(a && b && near(b->start, a->start + len2 - 0.5, 0.05), "with the preference on, they overlap by the cut-to-overlap amount",
              a && b ? strfmt("%.3f %.3f", a->start, b->start) : "missing");
        st().undo();
        st().prefs.autoOverlapMedia = false;
    }
    // Add Across Tracks
    {
        pickNextMenu(w, "Add Across Tracks");
        dropAt(&w, &md, QPoint(w.xAt(8.0), y), true);
        int at20 = 0;
        std::set<uint64_t> tracks;
        for (const Track& t : st().project().tracks)
            for (const Event& e : t.events)
                if (t.type == TrackType::Video && near(e.start, st().project().quantize(8.0), 1e-6)) {
                    ++at20;
                    tracks.insert(t.id);
                }
        check(at20 == 2 && tracks.size() == 2, "Add Across Tracks: side by side on tracks of their own", strfmt("%d on %d tracks", at20, (int)tracks.size()));
        check(st().undoName() == "Add Across Tracks", "(one undo step)", st().undoName());
        st().undo();
    }
    // Add As Takes: one event with both files as takes
    {
        pickNextMenu(w, "Add As Takes");
        dropAt(&w, &md, QPoint(w.xAt(8.0), y), true);
        const Event* a = nullptr;
        for (const Event& e : trk(ids.v2)->events)
            if (near(e.start, st().project().quantize(8.0), 1e-6)) a = &e;
        check(a && a->mediaId == m2 && a->takes.size() == 1 && a->takes[0].mediaId == m1, "Add As Takes: one event, the second file a take of it");
        st().undo();
        // onto an event: the files become takes of it, the first of them the active one
        pickNextMenu(w, "Add As Takes");
        dropAt(&w, &md, QPoint(w.xAt(4.0), y), true);
        const Event* e3 = st().project().findEvent(ids.e3);
        check(e3 && e3->takes.size() == 2 && e3->mediaId == m2, "dropped on an event, Add As Takes adds them to it as takes (the first active)",
              e3 ? strfmt("%d takes", (int)e3->takes.size()) : "");
        st().undo();
    }
    // Video Only / Audio Only
    {
        pickNextMenu(w, "Video Only/Add Across Time");
        const int audioBefore = eventsOn(ids.a1) + eventsOn(ids.a2);
        dropAt(&w, &md, QPoint(w.xAt(8.0), y), true);
        int audioAfter = 0;
        for (const Track& t : st().project().tracks)
            if (t.type == TrackType::Audio) audioAfter += (int)t.events.size();
        check(audioAfter == audioBefore && st().project().tracks.size() == tracksBefore, "Video Only: only the pictures go on the timeline",
              strfmt("%d -> %d audio events", audioBefore, audioAfter));
        st().undo();
        pickNextMenu(w, "Audio Only/Add Across Time");
        const int videoBefore = eventsOn(ids.v1) + eventsOn(ids.v2);
        dropAt(&w, &md, QPoint(w.xAt(8.0), w.trackRect(ids.a2).center().y()), true);
        int videoAfter = 0;
        for (const Track& t : st().project().tracks)
            if (t.type == TrackType::Video) videoAfter += (int)t.events.size();
        check(videoAfter == videoBefore, "Audio Only: only the sound");
        st().undo();
        pickNextMenu(w, "Cancel");
        const size_t steps = st().undoCount();
        dropAt(&w, &md, QPoint(w.xAt(8.0), y), true);
        check(st().undoCount() == steps, "Cancel adds nothing");
    }
    // a plain (left) drag of several files from Project Media: all of them, one after another
    {
        w.rightDragMenu = nullptr;
        dropAt(&w, &md, QPoint(w.xAt(8.0), y), false);
        int n = 0;
        for (const Event& e : trk(ids.v2)->events)
            if (e.mediaId == m1 || e.mediaId == m2) ++n;
        check(n == 2, "a left drag of several files from Project Media adds every one of them", strfmt("%d", n));
        st().undo();
        st().undo();
    }
    (void)m3;
    (void)len1;
}

// ================================================================================================ snap offset
void snapOffsetTests(TimelineWidget& w)
{
    buildProject();
    // room after the event: the view cannot zoom out past the whole project, so an event far on makes it long
    st().edit("Room", [](Project& p) { addEvent(p, ids.a2, ids.snd, 40.0, 1.0); });
    w.zoomToFit();
    st().snapping = true;
    st().setSelection(std::set<uint64_t>{ids.e3});
    uitest::settle(50);
    // drag the flag at the bottom left of e3 (2..7) to 1 s in, snapping to the cursor
    st().setCursor(3.0);
    const QRect r = w.eventRectOf(ids.e3);
    const QPoint flag(r.left() + 2, r.bottom() - 2);
    uitest::mouseDrag(&w, flag, QPoint(w.xAt(3.0) + 3, flag.y()), 10);
    const Event* e3 = st().project().findEvent(ids.e3);
    check(e3 && near(e3->snapOffset, 1.0, 1e-6), "dragging the snap offset flag sets the event's snap offset (snapping to the cursor)",
          e3 ? strfmt("%.4f", e3->snapOffset) : "");
    check(st().undoName() == "Event Snap Offset", "(one undo step)", st().undoName());
    // moving the event: the snap offset snaps (to a marker at 9 s: pressing on an event moves the cursor to the press)
    st().edit("Marker", [](Project& p) { Project::Marker m; m.id = p.newId(); m.time = 9.0; p.markers.push_back(m); });
    const QRect r2 = w.eventRectOf(ids.e3);
    const QPoint grab(r2.center());
    const double want = 9.0 - 1.0;
    uitest::mouseDrag(&w, grab, grab + QPoint(w.xAt(want) - w.xAt(2.0) + 2, 0), 12);
    e3 = st().project().findEvent(ids.e3);
    check(e3 && near(e3->start, want, 1e-6), "moving the event snaps its snap offset point (not its start) to the marker", e3 ? strfmt("%.4f", e3->start) : "");
    st().snapping = false;
}

// ================================================================================================ sync links
void syncLinkTests(TimelineWidget& w)
{
    buildProject();
    w.zoomToFit();
    uitest::settle(450);  // a right-click right after a right-drag's menu is not taken: rightMenuAt
    // e4 (A2, 0.5..6.5) linked to e3 (V2, 2..7)
    st().setSelection(std::set<uint64_t>{ids.e3, ids.e4});
    const QPoint onE3 = w.eventRectOf(ids.e3).center();
    QStringList seenItems;
    bool en = false;
    check(menuPick(&w, onE3, "Create Sync Link with Selected Events", QString(), &seenItems, &en), "an event's menu: Create Sync Link with Selected Events",
          (en ? "enabled: " : "disabled: ") + seenItems.join("|").toStdString());
    check(st().project().findEvent(ids.e4)->syncLink == ids.e3 && st().project().findEvent(ids.e3)->syncLink == 0, "the other selected event is linked to it");
    // moving the sync event moves the linked one
    st().setSelection(std::set<uint64_t>{ids.e3});
    w.nudgeSelection(0, 0, 0);
    const double e4Was = st().project().findEvent(ids.e4)->start;
    {
        const QRect r = w.eventRectOf(ids.e3);
        uitest::mouseDrag(&w, r.center(), r.center() + QPoint(w.xAt(3.0) - w.xAt(2.0), 0), 10);
    }
    const double moved = st().project().findEvent(ids.e3)->start - 2.0;
    check(moved > 0.5 && near(st().project().findEvent(ids.e4)->start, e4Was + moved, 1e-6), "moving the sync event moves the linked event as far",
          strfmt("%.3f %.3f", moved, st().project().findEvent(ids.e4)->start));
    // moving the linked event does not move the sync event
    const double e3At = st().project().findEvent(ids.e3)->start;
    st().setSelection(std::set<uint64_t>{ids.e4});
    w.nudgeSelection(10, 0, 0);
    check(near(st().project().findEvent(ids.e3)->start, e3At), "moving the linked event leaves the event it is linked to");
    // the keypad moves follow too
    st().setSelection(std::set<uint64_t>{ids.e3});
    const double e4b = st().project().findEvent(ids.e4)->start;
    w.nudgeSelection(5, 0, 0);
    check(near(st().project().findEvent(ids.e4)->start - e4b, st().project().findEvent(ids.e3)->start - e3At, 1e-6), "and so does Num 3 (a frame at a time)");
    // purple outline on the linked event while the sync event is selected
    uitest::settle(50);
    const QImage img = uitest::shot(&w, QString("man2_shots/synclink.png"));
    const QRect r4 = w.eventRectOf(ids.e4);
    int purple = 0;
    for (int x = std::max(0, r4.left() + 2); x < std::min(img.width(), r4.right() - 2); ++x) {
        const QColor c = img.pixelColor(x, r4.top() + 1);
        if (c.red() > 140 && c.blue() > 200 && c.green() < 130) ++purple;
    }
    check(purple > 20, "the linked event is outlined in purple while the sync event is selected", strfmt("%d", purple));
    // Sync Link > Select All, Unlink
    st().setSelection(std::set<uint64_t>{ids.e4});
    check(menuPick(&w, w.eventRectOf(ids.e4).center(), "Select All", "Sync Link") && st().selection() == std::set<uint64_t>({ids.e3, ids.e4}),
          "Sync Link > Select All selects the events of the link");
    st().setSelection(std::set<uint64_t>{ids.e4});
    check(menuPick(&w, w.eventRectOf(ids.e4).center(), "Unlink", "Sync Link") && st().project().findEvent(ids.e4)->syncLink == 0, "Sync Link > Unlink");
    st().undo();
    st().setSelection(std::set<uint64_t>{ids.e3, ids.e4});
    check(menuPick(&w, w.eventRectOf(ids.e3).center(), "Remove Sync Link from Selected Events") && st().project().findEvent(ids.e4)->syncLink == 0,
          "Remove Sync Link from Selected Events");
}

// ================================================================================================ automation
void automationTests(TimelineWidget& w, Playback& playback)
{
    buildProject();
    w.zoomToFit();
    uitest::settle(50);
    const QRect autoBtn = w.headerPartRect(ids.a2, TimelineWidget::PartAuto);
    check(!autoBtn.isEmpty(), "the audio track header has the Automation Settings button");
    uitest::mouseClick(&w, autoBtn.center());
    const Track* a2 = trk(ids.a2);
    check(a2 && a2->automation && a2->automationMode == "read", "clicking it turns Automation Settings on (Read mode)");
    // the mode from its right-click menu
    check(menuPick(&w, autoBtn.center(), "Off", "Automation") && trk(ids.a2)->automationMode == "off", "its menu sets the automation mode (Off)");
    // Off: the volume slider sets the automation value; the trim stays
    {
        const QRect lv = w.headerPartRect(ids.a2, TimelineWidget::PartLevel);
        uitest::mouseDrag(&w, QPoint(lv.left() + lv.width() * 3 / 4, lv.center().y()), QPoint(lv.left() + lv.width() / 3, lv.center().y()), 8);
        a2 = trk(ids.a2);
        check(near(a2->level, 1.0) && a2->autoLevel < 0.5, "in Off mode the volume slider sets the automation value (the trim stays)",
              strfmt("trim %.3f auto %.3f", a2->level, a2->autoLevel));
        check(trackAutomationValue(*a2, "volume", 1.0) == a2->autoLevel, "and that is what applies (no envelope)");
    }
    // Touch, stopped: the slider puts a point on the volume envelope at the cursor
    {
        check(menuPick(&w, autoBtn.center(), "Automation Write (Touch)", "Automation"), "Automation Write (Touch)");
        st().setCursor(3.0);
        const QRect lv = w.headerPartRect(ids.a2, TimelineWidget::PartLevel);
        uitest::mouseDrag(&w, QPoint(lv.left() + 4, lv.center().y()), QPoint(lv.left() + lv.width() * 2 / 3, lv.center().y()), 8);
        const Envelope* e = findEnvelope(trk(ids.a2)->envelopes, "volume");
        check(e && e->points.size() == 2 && near(e->points[1].time, 3.0, 1e-6), "in Touch mode the slider sets the volume envelope at the cursor (an envelope is put on)",
              e ? strfmt("%d points", (int)e->points.size()) : "none");
        // Read: the envelope sets it; the slider does nothing
        check(menuPick(&w, autoBtn.center(), "Read", "Automation"), "Read");
        const size_t steps = st().undoCount();
        uitest::mouseDrag(&w, QPoint(lv.left() + 4, lv.center().y()), QPoint(lv.right() - 4, lv.center().y()), 8);
        check(st().undoCount() == steps, "in Read mode a track with an envelope cannot be changed with the slider");
    }
    // Off dims... and leaves the envelopes out: the automation value takes over at the cursor's value
    {
        st().setCursor(3.0);
        const double at3 = trackAutomationValue(*trk(ids.a2), "volume", 3.0);
        menuPick(&w, autoBtn.center(), "Off", "Automation");
        const Track* t = trk(ids.a2);
        check(t->automationOff() && near(t->autoLevel, at3, 1e-9) && near(trackAutomationValue(*t, "volume", 0.0), at3, 1e-9),
              "switching to Off: the value at the cursor holds all along (the envelope is left out)");
    }
    // the Mute Automation button
    {
        menuPick(&w, autoBtn.center(), "Automation Write (Latch)", "Automation");
        st().setCursor(2.0);
        uitest::mouseClick(&w, w.headerPartRect(ids.a2, TimelineWidget::PartMute).center());
        const Envelope* m = findEnvelope(trk(ids.a2)->envelopes, "mute");
        check(m && m->valueAt(2.5, 0) >= 0.5 && !trk(ids.a2)->mute, "in automation mode Mute writes the mute envelope at the cursor (the track's own mute stays)");
    }
    // recording while playing (Touch): points along the stretch played while the slider was held
    {
        menuPick(&w, autoBtn.center(), "Automation Write (Touch)", "Automation");
        st().edit("Clear", [&](Project& p) { p.findTrack(ids.a2)->envelopes.clear(); });
        st().setCursor(1.0);
        playback.play();
        uitest::settle(150);
        const QRect lv = w.headerPartRect(ids.a2, TimelineWidget::PartLevel);
        uitest::mousePress(&w, QPoint(lv.left() + lv.width() / 2, lv.center().y()));
        for (int k = 0; k < 10; ++k) {
            uitest::mouseMove(&w, QPoint(lv.left() + lv.width() / 2 - k * 4, lv.center().y()));
            uitest::settle(60);
        }
        uitest::mouseRelease(&w, QPoint(lv.left() + lv.width() / 2 - 40, lv.center().y()));
        uitest::settle(100);
        playback.pause();
        uitest::settle(100);
        const Envelope* e = findEnvelope(trk(ids.a2)->envelopes, "volume");
        check(e && e->points.size() >= 3, "recording in Touch mode writes envelope points while playing", e ? strfmt("%d points", (int)e->points.size()) : "none");
        if (e && e->points.size() >= 3) check(e->points.back().time > 1.2 && e->points.back().value < e->points[1].value, "the points follow the slider down as time goes on",
                                              strfmt("%.2f s %.3f", e->points.back().time, e->points.back().value));
        check(st().undoName() == "Record Automation", "(one undo step)", st().undoName());
    }
    // Ctrl+click on Mute: that track alone; again: none
    {
        st().edit("Plain", [&](Project& p) { for (Track& t : p.tracks) { t.automation = false; t.mute = false; } p.findTrack(ids.a1)->mute = true; });
        uitest::mouseClick(&w, w.headerPartRect(ids.v2, TimelineWidget::PartMute).center(), Qt::LeftButton, Qt::ControlModifier);
        int muted = 0;
        for (const Track& t : st().project().tracks) muted += t.mute ? 1 : 0;
        check(muted == 1 && trk(ids.v2)->mute, "Ctrl+click on Mute: only that track muted");
        uitest::mouseClick(&w, w.headerPartRect(ids.v2, TimelineWidget::PartMute).center(), Qt::LeftButton, Qt::ControlModifier);
        muted = 0;
        for (const Track& t : st().project().tracks) muted += t.mute ? 1 : 0;
        check(muted == 0, "Ctrl+click on it again: none muted");
    }
    // several tracks selected: their faders move together
    {
        uitest::mouseClick(&w, w.headerPartRect(ids.a1, TimelineWidget::PartName).center());
        uitest::mouseClick(&w, w.headerPartRect(ids.a2, TimelineWidget::PartName).center(), Qt::LeftButton, Qt::ControlModifier);
        st().edit("Levels", [&](Project& p) {
            p.findTrack(ids.a1)->level = 1.0;
            p.findTrack(ids.a2)->level = 0.5;
            p.findTrack(ids.a1)->height = 100;
            p.findTrack(ids.a2)->height = 100;
        });
        uitest::settle(50);
        const QRect lv = w.headerPartRect(ids.a1, TimelineWidget::PartLevel);
        uitest::mouseDrag(&w, QPoint(lv.right() - 2, lv.center().y()), QPoint(lv.left() + lv.width() / 2, lv.center().y()), 8);
        const double l1 = trk(ids.a1)->level, l2 = trk(ids.a2)->level;
        check(l1 < 0.9 && near(l2 / l1, 0.5, 0.01), "with several tracks selected, dragging one fader moves them all (keeping their difference)", strfmt("%.3f %.3f", l1, l2));
        // and the phase button inverts them all
        const QRect ph = w.headerPartRect(ids.a1, TimelineWidget::PartPhase);
        if (!ph.isEmpty()) {
            uitest::mouseClick(&w, ph.center());
            check(trk(ids.a1)->invertPhase && trk(ids.a2)->invertPhase, "Invert Track Phase inverts every selected audio track");
        } else {
            check(true, "(the track is too short for its phase button)");
        }
    }    // pictures of the headers in automation mode (Reno's look and VEGAS's)
    {
        st().edit("Look", [&](Project& p) {
            for (Track& t : p.tracks) {
                t.automation = true;
                t.automationMode = t.type == TrackType::Video ? "touch" : "read";
                t.height = 100;
            }
            p.findTrack(ids.v2)->autoFade = 0.5;
            p.findTrack(ids.a1)->output = 0;
        });
        uitest::settle(50);
        check(!w.headerPartRect(ids.v2, TimelineWidget::PartFade).isEmpty(), "a video track in automation mode shows the Fade slider");
        uitest::shot(&w, "man2_shots/automation_reno.png");
        applyTheme(UiTheme::Vegas);
        uitest::settle(100);
        uitest::shot(&w, "man2_shots/automation_vegas.png");
        applyTheme(UiTheme::Reno);
        uitest::settle(50);
    }
}

// Runs 'fn' on the modal dialog (or message box) that shows next, found by its object name.
void onModal(const char* name, std::function<void(QWidget*)> fn, int ms = 150)
{
    QTimer::singleShot(ms, [name, fn] {
        QWidget* w = QApplication::activeModalWidget();
        if (!w)
            for (QWidget* t : QApplication::topLevelWidgets())
                if (t->isVisible() && t->objectName() == name) w = t;
        if (w) fn(w);
    });
}

// ================================================================================================ markers and commands
void markerTests(TimelineWidget& w)
{
    buildProject();
    w.zoomToFit();
    uitest::settle(450);
    // command markers: Command Properties with Comment, Position and templates
    {
        CommandProps c;
        c.command = "Text";
        c.parameter = "Hello";
        c.comment = "first caption";
        c.position = 1.0;
        w.insertCommandMarker(c.command, c.parameter, c.comment, c.position);
        const Project::Marker* m = st().project().markers.empty() ? nullptr : &st().project().markers.back();
        check(m && m->kind == "command" && m->command == "Text" && m->name == "Hello" && m->comment == "first caption" && near(m->time, 1.0),
              "a command marker keeps its command, parameter, comment and position");
        const uint64_t id = m ? m->id : 0;
        // Edit... (its tag's menu) opens Command Properties on it; change it there
        bool shown = false;
        onModal("commandMarkerDialog", [&](QWidget* d) {
            shown = true;
            if (QLineEdit* e = d->findChild<QLineEdit*>("parameter")) e->setText("Changed");
            if (QLineEdit* e = d->findChild<QLineEdit*>("comment")) e->setText("second");
            if (QLineEdit* e = d->findChild<QLineEdit*>("position")) e->setText("00:00:02;00");
            // a template saved under a name
            if (QComboBox* t = d->findChild<QComboBox*>("template")) t->setCurrentText("Greeting");
            if (QToolButton* b = d->findChild<QToolButton*>("saveTemplate")) b->click();
            static_cast<QDialog*>(d)->accept();
        }, 400);
        check(menuPick(&w, w.markerTagRect(id).center(), "Edit..."), "a command marker's menu: Edit...");
        uitest::settle(600);
        m = nullptr;
        for (const auto& mk : st().project().markers) if (mk.id == id) m = &mk;
        check(shown && m && m->name == "Changed" && m->comment == "second" && near(m->time, 2.0, 1e-6), "Command Properties changes it (parameter, comment, position)",
              m ? strfmt("%s %s %.3f", m->name.c_str(), m->comment.c_str(), m->time) : "gone");
        bool kept = false;
        for (const auto& t : commandTemplates()) kept = kept || (t.first == "Greeting" && t.second.parameter == "Changed" && t.second.comment == "second");
        check(kept, "Save keeps a command template by name");
        // a double-click on its tag opens it too
        shown = false;
        onModal("commandMarkerDialog", [&](QWidget* d) { shown = true; static_cast<QDialog*>(d)->reject(); }, 300);
        uitest::mouseDoubleClick(&w, w.markerTagRect(id).center());
        uitest::settle(500);
        check(shown, "a double-click on a command marker's tag opens Command Properties");
        saveCommandTemplate("Greeting", CommandProps{"", "", "", 0});
    }
    // double-clicks: the marker bar selects the whole project; a region's tag selects the region
    {
        uitest::settle(450);
        st().setLoop(0, 0);
        uitest::mouseDoubleClick(&w, QPoint(w.xAt(3.0), w.markerBarRect().center().y()));
        check(near(st().loopStart, 0.0) && near(st().loopEnd, st().project().length(), 1e-6), "a double-click on the marker bar selects the whole project",
              strfmt("%.3f..%.3f", st().loopStart, st().loopEnd));
        st().setLoop(1.0, 2.5);
        w.insertRegion();
        st().setLoop(0, 0);
        uint64_t rid = 0;
        for (const auto& mk : st().project().markers) if (mk.region) rid = mk.id;
        uitest::settle(50);
        uitest::mouseDoubleClick(&w, w.markerTagRect(rid).center());
        check(near(st().loopStart, 1.0, 1e-6) && near(st().loopEnd, 2.5, 1e-6), "a double-click on a region's tag selects the region");
    }
    // the Marker Tool
    {
        st().edit("Markers", [&](Project& p) {
            p.markers.clear();
            for (double t : {1.0, 2.0, 3.0, 4.0}) {
                Project::Marker m;
                m.id = p.newId();
                m.time = t;
                p.markers.push_back(m);
            }
        });
        uitest::settle(50);
        QAbstractButton* tool = w.findChild<QAbstractButton*>("markerToolButton");
        check(tool && tool->isVisible(), "the Marker Tool button at the top right of the timeline");
        if (tool) tool->click();
        const std::vector<Project::Marker> ms = st().project().markers;
        uitest::mouseClick(&w, w.markerTagRect(ms[0].id).center());
        uitest::mouseClick(&w, w.markerTagRect(ms[2].id).center(), Qt::LeftButton, Qt::ShiftModifier);
        // drag the second: the three chosen move together
        st().snapping = false;
        const QPoint from = w.markerTagRect(ms[1].id).center();
        uitest::mouseDrag(&w, from, from + QPoint(w.xAt(1.5) - w.xAt(1.0), 0), 8);
        const std::vector<Project::Marker> now = st().project().markers;
        check(near(now[0].time, 1.5, 0.05) && near(now[1].time, 2.5, 0.05) && near(now[2].time, 3.5, 0.05) && near(now[3].time, 4.0),
              "markers chosen with the Marker Tool (Shift+click) move together", strfmt("%.2f %.2f %.2f %.2f", now[0].time, now[1].time, now[2].time, now[3].time));
        // Ctrl+click takes one away; Delete deletes the chosen ones
        uitest::mouseClick(&w, w.markerTagRect(ms[1].id).center(), Qt::LeftButton, Qt::ControlModifier);
        uitest::key(&w, Qt::Key_Delete);
        check(st().project().markers.size() == 2, "Delete deletes the chosen markers", strfmt("%d left", (int)st().project().markers.size()));
        // Right: the next marker
        uitest::key(&w, Qt::Key_Right);
        check(st().project().markers.size() == 2, "(Right moves along the markers)");
        uitest::mouseClick(&w, QPoint(w.xAt(3.0), w.trackRect(ids.v2).center().y()));
        check(tool && !w.findChild<QAbstractButton*>("markerToolButton")->isDown(), "a click elsewhere puts the Marker Tool away");
    }
}

// ================================================================================================ more timeline behaviour
void timelineTests(TimelineWidget& w)
{
    buildProject();
    w.zoomToFit();
    uitest::settle(450);
    // a double-click on an event's edge: expanded edit mode
    {
        st().edit("Meet", [&](Project& p) { addEvent(p, ids.v2, ids.clip, 7.0, 2.0); });
        w.zoomToFit();
        uitest::settle(50);
        const QRect r = w.eventRectOf(ids.e3);
        uitest::mouseDoubleClick(&w, QPoint(r.right() - 1, r.center().y()));
        check(w.edgeTrimming(), "a double-click on an event's edge goes into expanded edit mode");
        uitest::mouseDoubleClick(&w, QPoint(r.right() - 1, r.center().y()));
        check(!w.edgeTrimming(), "and again out of it");
    }
    // the right button held and the left dragging an event of a group: it alone moves
    {
        uitest::settle(450);
        const QRect r = w.eventRectOf(ids.e1);
        const QPoint at = r.center();
        uitest::mousePress(&w, at, Qt::RightButton);
        QMouseEvent lp(QEvent::MouseButtonPress, QPointF(at), w.mapToGlobal(QPointF(at)), Qt::LeftButton, Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &lp);
        for (int k = 1; k <= 8; ++k) {
            const QPoint p = at + QPoint((w.xAt(2.0) - w.xAt(1.0)) * k / 8, 0);
            QMouseEvent mv(QEvent::MouseMove, QPointF(p), w.mapToGlobal(QPointF(p)), Qt::NoButton, Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
            QApplication::sendEvent(&w, &mv);
        }
        const QPoint end = at + QPoint(w.xAt(2.0) - w.xAt(1.0), 0);
        QMouseEvent lr(QEvent::MouseButtonRelease, QPointF(end), w.mapToGlobal(QPointF(end)), Qt::LeftButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &lr);
        QMouseEvent rr(QEvent::MouseButtonRelease, QPointF(end), w.mapToGlobal(QPointF(end)), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &rr);
        uitest::settle(50);
        const Event* e1 = st().project().findEvent(ids.e1);
        const Event* e1a = st().project().findEvent(ids.e1a);
        check(e1 && e1a && e1->start > 1.5 && near(e1a->start, 1.0), "with the right button held, the left drags one event of a group alone",
              e1 && e1a ? strfmt("%.3f %.3f", e1->start, e1a->start) : "");
        check(!w.options().ignoreGrouping, "(grouping comes back afterwards)");
        st().undo();
    }
    // the Selection tool: a right click while dragging goes round rectangle, time, track selection
    {
        w.options().tool = EditTool::Selection;
        w.optionsChanged();
        st().setSelection(std::set<uint64_t>());
        const QPoint a(w.xAt(0.2), w.trackRect(ids.v2).top() + 5), b(w.xAt(2.5), w.trackRect(ids.v2).bottom() - 5);
        uitest::mousePress(&w, a);
        uitest::mouseMove(&w, b);
        QMouseEvent rp(QEvent::MouseButtonPress, QPointF(b), w.mapToGlobal(QPointF(b)), Qt::RightButton, Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &rp);
        QMouseEvent rr(QEvent::MouseButtonRelease, QPointF(b), w.mapToGlobal(QPointF(b)), Qt::RightButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &rr);
        uitest::mouseMove(&w, b + QPoint(2, 0));
        const std::set<uint64_t> timeSel = st().selection();
        check(timeSel.count(ids.e4) && timeSel.count(ids.e1) && timeSel.count(ids.e3) && st().loopEnd > st().loopStart,
              "a right click while dragging with the Selection tool: time selection (every track over that time)", strfmt("%d", (int)timeSel.size()));
        uitest::mouseRelease(&w, b + QPoint(2, 0));
        w.options().tool = EditTool::Normal;
        w.optionsChanged();
        st().setLoop(0, 0);
    }
    // Select All After Cursor asks when several tracks are selected
    {
        uitest::mouseClick(&w, w.headerPartRect(ids.v1, TimelineWidget::PartName).center());
        uitest::mouseClick(&w, w.headerPartRect(ids.v2, TimelineWidget::PartName).center(), Qt::LeftButton, Qt::ControlModifier);
        st().setCursor(0.0);
        bool asked = false;
        onModal("selectAfterCursorQuestion", [&](QWidget* d) {
            asked = true;
            for (QAbstractButton* b : d->findChildren<QAbstractButton*>())
                if (plain(b->text()) == "Selected Tracks") b->click();
        });
        w.selectAllAfterCursor();
        uitest::settle(300);
        check(asked && st().selection().count(ids.e1) && st().selection().count(ids.e3) && !st().selection().count(ids.e4),
              "Select All After Cursor asks All Tracks / Selected Tracks; Selected Tracks takes theirs only");
    }
    // a video cross-fade's menu: Transition
    {
        st().edit("Overlap", [&](Project& p) {
            for (Event& e : p.findTrack(ids.v2)->events)
                if (e.id != ids.e3) e.start = 6.0;
        });
        w.zoomToFit();
        uitest::settle(450);
        QStringList items;
        menuPick(&w, QPoint(w.xAt(6.5), w.trackRect(ids.v2).center().y()), "(none)", "Transition", &items);
        check(items.size() >= 1 && items[0] == "Insert Other...", "a video cross-fade's menu has Transition > Insert Other... and the transitions",
              items.join("|").toStdString());
    }
    // a stretched video event shows the zigzag; an out-of-sync event its offset (with Active Take Information)
    {
        st().edit("Stretch", [&](Project& p) {
            p.findEvent(ids.e3)->rate = 0.5;
            p.findEvent(ids.e1a)->offset = 0.5;  // the sound slipped half a second against its picture
        });
        w.options().showTakeInfo = true;
        w.optionsChanged();
        uitest::settle(100);
        const QImage img = uitest::shot(&w, "man2_shots/stretch_sync.png");
        const QRect r = w.eventRectOf(ids.e1a);
        int red = 0;
        for (int x = r.left() + 2; x < r.right() - 2; ++x) {
            const QColor c = img.pixelColor(x, r.top() + 1);
            if (c.red() > 200 && c.green() < 80) ++red;
        }
        check(red > 20, "an event out of step with its grouped partner is marked red (Active Take Information)", strfmt("%d", red));
        w.options().showTakeInfo = false;
        w.optionsChanged();
    }
}
}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("man2_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("man2_shots");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    buildProject();
    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1400, 600);
    w.show();
    w.zoomToFit();
    uitest::settle(300);

    envelopeTests(w, out);
    w.resize(1400, 900);
    dropTests(w);
    snapOffsetTests(w);
    syncLinkTests(w);
    automationTests(w, playback);
    markerTests(w);
    timelineTests(w);

    return uitest::summary();
}
