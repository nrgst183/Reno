// Envelopes in the timeline: adding and removing them, drawing, editing points and segments (mouse and menus), the envelope tool,
// Lock Envelopes to Events, event envelopes, fade curves, channel mapping and bus tracks. Saves pictures of the timeline.
//
//   envelope_test <output folder>
//
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/timeline.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QMenu>
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
    uint64_t e1 = 0, e1a = 0, e3 = 0, e4 = 0, e5 = 0, e6 = 0;
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

//   V1: e1 clip 1..5 (grouped with e1a)       V2: e3 2..7, e5 10..14, e6 12..16 (a cross-fade with e5)
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
        ids.e5 = addEvent(p, ids.v2, ids.clip, 10.0, 4.0);
        ids.e6 = addEvent(p, ids.v2, ids.clip, 12.0, 4.0, 3.0);
        Bus b;
        b.id = p.newId();
        b.name = "Bus A";
        p.buses.push_back(b);
    });
    st().setSelection(std::set<uint64_t>());
    st().setCursor(0.0);
    st().setLoop(0, 0);
    st().snapping = true;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
std::string num(double v) { return strfmt("%.4f", v); }

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

// Right-clicks and picks the menu item called 'text' (or 'sub' > 'text'). 'seen' gets the menu's items.
bool menuPick(QWidget* w, QPoint pos, const QString& text, const QString& sub = QString(), QStringList* seen = nullptr)
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
                if (a->menu() && a->text() == sub) from = a->menu();
        for (QAction* a : from->actions()) {
            if (seen) *seen << a->text();
            if (!found && a->text().split('\t')[0] == text && a->isEnabled()) {
                a->trigger();
                found = true;
            }
        }
        m->close();
    });
    QContextMenuEvent e(QContextMenuEvent::Mouse, pos, w->mapToGlobal(pos));
    QApplication::sendEvent(w, &e);
    uitest::settle(50);
    return found;
}

// How many pixels in a rectangle are clearly blue (or red, 'red').
int coloured(const QImage& img, const QRect& r, bool red)
{
    int n = 0;
    for (int y = std::max(0, r.top()); y <= std::min(img.height() - 1, r.bottom()); ++y)
        for (int x = std::max(0, r.left()); x <= std::min(img.width() - 1, r.right()); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (red ? (c.red() > c.green() + 90 && c.red() > c.blue() + 90) : (c.blue() > c.red() + 90 && c.blue() > c.green() + 50)) ++n;
        }
    return n;
}


}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("envelope_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("shots");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    buildProject();
    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1400, 600);
    w.show();
    w.zoomToFit();
    uitest::settle(300);
    const Project& p = st().project();

    // ---------------------------------------------------------------- putting envelopes on (V, P), hiding, removing
    const EnvelopeRef vol = trackEnv(ids.a2, "volume"), pan = trackEnv(ids.a2, "pan");
    {
        st().currentTrack = ids.a2;
        w.toggleTrackEnvelope("volume");
        const Envelope* e = envOf(vol);
        check(e && e->visible && e->points.size() == 1 && e->points[0].value == 1.0 && e->points[0].time == 0.0, "V puts a volume envelope on the track at 0 dB");
        check(st().undoName() == "Insert Envelope", "(one undo step)", st().undoName());
        w.toggleTrackEnvelope("volume");
        check(envOf(vol) && !envOf(vol)->visible, "V again hides it");
        w.toggleTrackEnvelope("volume");
        check(envOf(vol) && envOf(vol)->visible, "and again shows it");
        w.toggleTrackEnvelope("pan");
        check(envOf(pan) && envOf(pan)->points[0].value == 0.0, "P puts a pan envelope on, centred");
        w.removeTrackEnvelope("pan");
        check(!envOf(pan), "Shift+P takes it off");
        st().undo();
        check(envOf(pan) != nullptr, "undo puts it back");
        st().currentTrack = ids.v2;
        w.toggleTrackEnvelope("volume");
        check(envOf(trackEnv(ids.v2, "composite")) != nullptr, "V on a video track puts a composite level envelope on");
        w.toggleTrackEnvelope("pan");
        check(!envOf(trackEnv(ids.v2, "pan")), "P does nothing on a video track");
        // the track header's menu
        QStringList items;
        const QPoint hdr = w.headerRect(ids.v2).center();
        check(menuPick(&w, hdr, "Fade to Color", "Insert/Remove Envelope", &items), "track menu: Insert/Remove Envelope > Fade to Color");
        check(envOf(trackEnv(ids.v2, "fadecolor")) != nullptr, "(the fade to colour envelope is there)");
        // a new envelope's segments start Smooth, VEGAS 18's default envelope fade type
        check(envOf(trackEnv(ids.v2, "fadecolor"))->points[0].curve == CurveSmooth, "a new envelope point starts Smooth (VEGAS's default)");
        check(items.contains("Composite Level") && items.contains("Mute") && !items.contains("Pan"), "a video track offers Composite Level, Fade to Color, Mute",
              items.join(", ").toStdString());
        items.clear();
        menuPick(&w, w.headerRect(ids.a1).center(), "(nothing)", "Insert/Remove Envelope", &items);
        check(items.contains("Volume") && items.contains("Pan") && items.contains("Mute") && items.contains("Bus A"),
              "an audio track offers Volume, Pan, Mute and a send to each bus", items.join(", ").toStdString());
        check(menuPick(&w, w.headerRect(ids.a1).center(), "Bus A", "Insert/Remove Envelope") && envOf(trackEnv(ids.a1, "send", p.buses[0].id)),
              "a send envelope to Bus A");
        check(menuPick(&w, w.headerRect(ids.a1).center(), "Bus A", "Insert/Remove Envelope") && !envOf(trackEnv(ids.a1, "send", p.buses[0].id)),
              "picked again, it comes off");
        st().edit("Fade", [](Project& pp) {
            Envelope* e = AppState::findEnvelope(pp, trackEnv(ids.v2, "fadecolor"));
            e->points[0].value = 0.5;
        });
    }
    uitest::settle(200);
    {
        const QImage img = uitest::shot(&w, out + "/env_on.png");
        const QRect a2 = w.trackRect(ids.a2);
        const QPoint p0 = w.envelopePointPos(vol, 0);
        const int lineY = p0.y();
        check(lineY > a2.top() && lineY < a2.top() + a2.height() / 2, "the volume line at 0 dB is drawn in the upper part of the track", strfmt("%d in %d..%d", lineY, a2.top(), a2.bottom()));
        check(coloured(img, QRect(w.xAt(8.0), lineY - 2, 200, 5), false) > 150, "a blue line for volume", strfmt("%d", coloured(img, QRect(w.xAt(8.0), lineY - 2, 200, 5), false)));
        const int panY = w.envelopePointPos(pan, 0).y();
        check(std::abs(panY - (a2.top() + a2.height() / 2)) <= 3, "the pan line in the middle (centre)", strfmt("%d", panY));
        check(coloured(img, QRect(w.xAt(8.0), panY - 2, 200, 5), true) > 150, "a red line for pan");
        const QRect v2 = w.trackRect(ids.v2);
        const int fy = w.envelopePointPos(trackEnv(ids.v2, "fadecolor"), 0).y();
        check(fy > v2.top() && fy < v2.center().y(), "fade to colour at 50% top is drawn above the middle", strfmt("%d", fy));
        // hidden envelopes are not drawn
        st().showEnvelope(pan, false);
        uitest::settle(50);
        const QImage img2 = uitest::shot(&w, out + "/env_hidden.png");
        check(coloured(img2, QRect(w.xAt(8.0), panY - 2, 200, 5), true) < 20 && w.envelopePointPos(pan, 0).x() < 0, "a hidden envelope is not drawn");
        st().showEnvelope(pan, true);
    }

    // ---------------------------------------------------------------- editing points with the mouse
    {
        st().snapping = false;
        const QPoint p0 = w.envelopePointPos(vol, 0);
        uitest::mouseDoubleClick(&w, QPoint(w.xAt(2.0), p0.y()));
        const Envelope* e = envOf(vol);
        check(e && e->points.size() == 2 && near(e->points[1].time, 2.0, 0.05) && near(e->points[1].value, 1.0, 1e-9), "double-clicking the line adds a point on it",
              e ? strfmt("%d points, %.3f s, %.3f", (int)e->points.size(), e->points.back().time, e->points.back().value) : "none");
        check(st().undoName() == "Add Envelope Point", "(an undo step)", st().undoName());
        uitest::mouseDoubleClick(&w, QPoint(w.xAt(4.0), p0.y()));
        check(envOf(vol)->points.size() == 3, "and another");
        // drag the point at 4 s down
        QPoint q = w.envelopePointPos(vol, 2);
        uitest::mouseDrag(&w, q, q + QPoint(0, 20));
        e = envOf(vol);
        const double v1 = e->points[2].value;
        check(v1 < 0.6 && near(e->points[2].time, envOf(vol)->points[2].time) && near(e->points[2].time, 4.0, 0.05), "dragging a point down turns it down",
              strfmt("%.3f (%.1f dB) at %.3f", v1, 20 * std::log10(v1), e->points[2].time));
        check(st().undoName() == "Move Envelope Points", "(one undo step)", st().undoName());
        check(e->points[1].value == 1.0, "the others stay");
        // Ctrl: fine, and the time stays put
        q = w.envelopePointPos(vol, 2);
        const double t0 = e->points[2].time;
        const double f0 = std::cbrt(v1 / 2.0);
        uitest::mouseDrag(&w, q, q + QPoint(40, 16), 8, Qt::ControlModifier);
        e = envOf(vol);
        const double f1 = std::cbrt(e->points[2].value / 2.0);
        const double fullStep = 16.0 / (w.trackRect(ids.a2).height() - 9.0);
        check(near(e->points[2].time, t0) && near(f0 - f1, fullStep / 8.0, 0.004), "Ctrl-drag: fine steps, and the time does not move",
              strfmt("moved %.4f of the height (a plain drag: %.4f)", f0 - f1, fullStep));
        // Alt: the time only
        const double val = e->points[2].value;
        q = w.envelopePointPos(vol, 2);
        uitest::mouseDrag(&w, q, q + QPoint(w.xAt(4.5) - w.xAt(4.0), 30), 8, Qt::AltModifier);
        e = envOf(vol);
        check(e->points[2].value == val && near(e->points[2].time, t0 + 0.5, 0.05), "Alt-drag: the time only", strfmt("%.3f s", e->points[2].time));
        // it cannot pass its neighbours
        q = w.envelopePointPos(vol, 2);
        uitest::mouseDrag(&w, q, QPoint(w.xAt(0.2), q.y()), 8, Qt::AltModifier);
        e = envOf(vol);
        check(near(e->points[2].time, e->points[1].time) && e->points[1].time <= e->points[2].time, "a point stops at the one before it", num(e->points[2].time));
        st().undo();
        // snapping: to the cursor
        st().snapping = true;
        st().setCursor(5.0);
        q = w.envelopePointPos(vol, 2);
        uitest::mouseDrag(&w, q, QPoint(w.xAt(5.0) + 4, q.y()), 8, Qt::AltModifier);
        check(near(envOf(vol)->points[2].time, 5.0, 1e-6), "a dragged point snaps to the cursor", num(envOf(vol)->points[2].time));
        // a segment dragged up: both its ends go up together
        const double a0 = envOf(vol)->points[1].value, b0 = envOf(vol)->points[2].value;
        const QPoint pa = w.envelopePointPos(vol, 1), pb = w.envelopePointPos(vol, 2);
        const QPoint mid((pa.x() + pb.x()) / 2, (pa.y() + pb.y()) / 2);
        uitest::mouseDrag(&w, mid, mid + QPoint(0, 10));
        e = envOf(vol);
        const double da = std::cbrt(a0 / 2) - std::cbrt(e->points[1].value / 2), db = std::cbrt(b0 / 2) - std::cbrt(e->points[2].value / 2);
        check(da > 0.05 && near(da, db, 0.006) && near(e->points[2].time, 5.0), "dragging a segment moves both its points up or down", strfmt("%.3f, %.3f", da, db));
        check(st().undoName() == "Move Envelope Segment", "(one undo step)", st().undoName());
    }
    // the point menu: curves, set to, flip, reset, delete
    {
        QPoint q = w.envelopePointPos(vol, 1);
        QStringList items;
        check(menuPick(&w, q, "Fast Fade", QString(), &items) && envOf(vol)->points[1].curve == CurveFast, "point menu: Fast");
        check(items.contains("Linear Fade") && items.contains("Slow Fade") && items.contains("Smooth Fade") && items.contains("Sharp Fade") && items.contains("Hold") &&
                  items.contains("Delete") && items.contains("Reset All") && items.contains("Flip All Points") && items.contains("Hide"),
              "the point menu has VEGAS's curves and commands", items.join(", ").toStdString());
        check(menuPick(&w, q, "Hold") && envOf(vol)->points[1].curve == CurveHold, "point menu: Hold");
        uitest::settle(50);
        uitest::shot(&w, out + "/env_points.png");
        check(menuPick(&w, w.envelopePointPos(vol, 1), "-6.0 dB", "Set To") && near(envOf(vol)->points[1].value, std::pow(10.0, -6.0 / 20.0)), "Set To > -6.0 dB",
              num(envOf(vol)->points[1].value));
        const double before = envOf(vol)->points[1].value;
        check(menuPick(&w, w.envelopePointPos(vol, 1), "Flip All Points"), "Flip All Points");
        const double fb = std::cbrt(before / 2.0), fa = std::cbrt(envOf(vol)->points[1].value / 2.0);
        check(near(fa, 1.0 - fb, 1e-9) && envOf(vol)->points[0].value < 0.1, "turns each point upside down on the fader scale",
              strfmt("%.3f -> %.3f", before, envOf(vol)->points[1].value));
        st().undo();
        // a line's menu adds a point
        // the segment after the hold point is level
        const QPoint pa = w.envelopePointPos(vol, 1);
        check(menuPick(&w, QPoint(w.xAt(3.0), pa.y()), "Add Point") && envOf(vol)->points.size() == 4 && near(envOf(vol)->points[2].time, 3.0, 0.05),
              "line menu: Add Point");
        check(menuPick(&w, w.envelopePointPos(vol, 2), "Delete") && envOf(vol)->points.size() == 3, "point menu: Delete");
    }
    // several points: in the time selection, with the envelope tool, deleted together
    {
        st().snapping = false;
        st().setLoop(1.5, 5.5);
        const std::vector<EnvPoint> was = envOf(vol)->points;
        const QPoint q = w.envelopePointPos(vol, 1);
        uitest::mouseDrag(&w, q, q + QPoint(w.xAt(0.5) - w.xAt(0.0), 0), 8, Qt::AltModifier);

        const Envelope* e = envOf(vol);
        check(near(e->points[1].time, was[1].time + 0.5, 0.05) && near(e->points[2].time, was[2].time + 0.5, 0.05) && near(e->points[0].time, was[0].time),
              "the points in the time selection move together", strfmt("%.3f %.3f", e->points[1].time, e->points[2].time));
        st().undo();
        st().setLoop(0, 0);
        w.options().tool = EditTool::Envelope;
        w.optionsChanged();
        const QRect a2 = w.trackRect(ids.a2);
        uitest::mouseDrag(&w, QPoint(w.xAt(1.5), a2.top() + 2), QPoint(w.xAt(6.0), a2.bottom() - 2));
        const std::vector<int> sel = w.selectedEnvelopePoints();
        check(sel.size() == 2 && sel[0] == 1 && sel[1] == 2 && w.selectedEnvelope() == vol, "the envelope tool selects the points it is dragged round",
              strfmt("%d selected", (int)sel.size()));
        // with the envelope tool, events stay put
        const double s4 = p.findEvent(ids.e4)->start;
        uitest::mouseDrag(&w, QPoint(w.xAt(3.0), a2.bottom() - 6), QPoint(w.xAt(4.0), a2.bottom() - 6));
        check(p.findEvent(ids.e4)->start == s4, "the envelope tool does not move events");
        uitest::mouseDrag(&w, QPoint(w.xAt(1.5), a2.top() + 2), QPoint(w.xAt(6.0), a2.bottom() - 2));
        w.deleteSelection();
        check(envOf(vol)->points.size() == 1 && p.findEvent(ids.e4) != nullptr, "Delete deletes the selected points (and not the event)",
              strfmt("%d points", (int)envOf(vol)->points.size()));
        w.options().tool = EditTool::Normal;
        w.optionsChanged();
        st().undo();
    }

    // ---------------------------------------------------------------- Lock Envelopes to Events
    {
        const std::vector<EnvPoint> was = envOf(vol)->points;       // 0, ~2, 5
        const QRect a2 = w.trackRect(ids.a2);
        st().snapping = false;
        uitest::mouseDrag(&w, QPoint(w.xAt(3.0), a2.bottom() - 8), QPoint(w.xAt(4.0), a2.bottom() - 8));
        const Envelope* e = envOf(vol);
        const double moved = p.findEvent(ids.e4)->start - 0.5;
        check(moved > 0.9 && near(e->points[1].time, was[1].time + moved, 1e-6) && near(e->points[2].time, was[2].time + moved, 1e-6) && e->points[0].time == 0.0,
              "Lock Envelopes to Events: the points under a moved event go with it", strfmt("moved %.3f: %.3f %.3f", moved, e->points[1].time, e->points[2].time));
        st().undo();
        w.options().lockEnvelopes = false;
        uitest::mouseDrag(&w, QPoint(w.xAt(3.0), a2.bottom() - 8), QPoint(w.xAt(4.0), a2.bottom() - 8));
        check(near(envOf(vol)->points[1].time, was[1].time) && p.findEvent(ids.e4)->start > 1.4, "with it off they stay where they were");
        st().undo();
        w.options().lockEnvelopes = true;
    }

    // ---------------------------------------------------------------- event envelopes, channels, fade types
    EnvelopeRef evVol;
    evVol.owner.kind = EnvelopeOwner::Event;
    evVol.owner.id = ids.e4;
    evVol.kind = "volume";
    {
        st().removeEnvelope(vol);
        st().removeEnvelope(pan);
        const QRect a2 = w.trackRect(ids.a2);
        QStringList items;
        check(menuPick(&w, QPoint(w.xAt(3.0), a2.bottom() - 10), "Volume", "Insert/Remove Envelope", &items) && envOf(evVol), "event menu: Insert/Remove Envelope > Volume");
        check(items.contains("Pan") && !items.contains("Velocity"), "a sound event offers Volume and Pan", items.join(", ").toStdString());
        items.clear();
        menuPick(&w, QPoint(w.xAt(4.0), w.trackRect(ids.v2).bottom() - 10), "Velocity", "Insert/Remove Envelope", &items);
        check(items.contains("Composite Level") && items.contains("Velocity") && !items.contains("Pan"), "a video event offers Composite Level and Velocity",
              items.join(", ").toStdString());
        EnvelopeRef vel = evVol;
        vel.owner.id = ids.e3;
        vel.kind = "velocity";
        check(envOf(vel) && envOf(vel)->points[0].value == 1.0, "(a velocity envelope at 100 %)");
        // VEGAS's velocity point menu: Set to Maximum Forward Velocity ... 100% Reverse Velocity
        {
            items.clear();
            const QPoint vp = w.envelopePointPos(vel, 0);
            const bool ok = menuPick(&w, vp, "Set to 50% Forward Velocity", QString(), &items);
            check(ok && near(envOf(vel)->points[0].value, 0.5), "velocity point menu: Set to 50% Forward Velocity", items.join(", ").toStdString());
            check(items.contains("Set to Maximum Forward Velocity") && items.contains("Set to Normal Velocity") && items.contains("Set to 0% Velocity") &&
                      items.contains("Set to 100% Reverse Velocity") && items.contains("Smooth Fade"),
                  "the velocity menu lists VEGAS's Set to values", items.join(", ").toStdString());
            check(menuPick(&w, w.envelopePointPos(vel, 0), "Set to 100% Reverse Velocity") && near(envOf(vel)->points[0].value, -1.0), "Set to 100% Reverse Velocity");
            check(menuPick(&w, w.envelopePointPos(vel, 0), "Set to Normal Velocity") && near(envOf(vel)->points[0].value, 1.0), "Set to Normal Velocity");
        }
        // a point inside the event, at 2 s into it
        const QPoint e0 = w.envelopePointPos(evVol, 0);
        check(e0.x() == w.xAt(0.5) && e0.y() > a2.top() && e0.y() < a2.bottom(), "an event's envelope starts at the event's start, inside it", strfmt("%d,%d", e0.x(), e0.y()));
        st().snapping = false;
        uitest::mouseDoubleClick(&w, QPoint(w.xAt(2.5), e0.y()));
        check(envOf(evVol)->points.size() == 2 && near(envOf(evVol)->points[1].time, 2.0, 0.05), "a point added on it counts from the event's start",
              num(envOf(evVol)->points[1].time));
        const QPoint q = w.envelopePointPos(evVol, 1);
        uitest::mouseDrag(&w, q, QPoint(w.xAt(9.0), q.y() + 15));
        check(envOf(evVol)->points[1].time <= 6.0 + 1e-9, "its points stay inside the event", num(envOf(evVol)->points[1].time));
        st().undo();
        // trimming the event's start leaves the envelope where it was on the timeline
        const double at = 0.5 + envOf(evVol)->points[1].time;
        // held low on the edge, away from the envelope's first point, which is on the edge too
        const int edgeY = std::abs(a2.center().y() - e0.y()) > 12 ? a2.center().y() : a2.bottom() - 8;
        uitest::mouseDrag(&w, QPoint(w.xAt(0.5), edgeY), QPoint(w.xAt(1.5), edgeY));
        const Event* e4 = p.findEvent(ids.e4);
        check(near(e4->start, 1.5, 0.05) && near(e4->start + envOf(evVol)->points[1].time, at, 1e-6), "trimming an event's start leaves its envelope points in place",
              strfmt("start %.3f, point at %.3f (was %.3f)", e4->start, e4->start + envOf(evVol)->points[1].time, at));
        st().undo();
        // splitting: the right half's envelope carries on the same
        st().setSelection({ids.e4});
        st().setCursor(2.0);
        st().splitAtCursor();
        uint64_t right = 0;
        for (const Track& t : p.tracks) if (t.id == ids.a2) for (const Event& o : t.events) if (o.id != ids.e4) right = o.id;
        const Event* re = p.findEvent(right);
        check(re && near(re->envelopes[0].valueAt(2.4 - re->start, 1.0), p.findEvent(ids.e4)->envelopes[0].valueAt(2.4 - 0.5, 1.0), 1e-9),
              "after a split the right half's envelope goes on as before");
        st().undo();
        st().setSelection({});

        // channels
        check(menuPick(&w, QPoint(w.xAt(5.0), a2.bottom() - 10), "Swap", "Channels") && p.findEvent(ids.e4)->channels == "swap", "event menu: Channels > Swap");
        items.clear();
        menuPick(&w, QPoint(w.xAt(5.0), a2.bottom() - 10), "Both", "Channels", &items);
        // VEGAS's order (from its menu resource): Combine before Swap
        check(p.findEvent(ids.e4)->channels == "both" && items.join(",") == "Both,Left Only,Right Only,Combine,Swap", "VEGAS's channel choices", items.join(",").toStdString());

        // fade types: a fade in, and a cross-fade
        st().edit("Fade", [](Project& pp) { pp.findEvent(ids.e4)->fadeIn = 1.5; });
        check(menuPick(&w, QPoint(w.xAt(1.0), a2.bottom() - 10), "Fast", "Fade Type") && p.findEvent(ids.e4)->fadeInCurve == CurveFast, "right-click in a fade: Fade Type > Fast");
        const QRect v2 = w.trackRect(ids.v2);
        check(menuPick(&w, QPoint(w.xAt(13.0), v2.bottom() - 10), "Slow", "Fade Type") && p.findEvent(ids.e6)->fadeInCurve == CurveSlow &&
                  p.findEvent(ids.e5)->fadeOutCurve == CurveFast,
              "right-click in a cross-fade: both events' curves (slow in, fast out)");
        // event Properties shows them too (editdialogs; checked there) - here: the picture
        uitest::settle(100);
        uitest::shot(&w, out + "/env_events.png");
    }

    // ---------------------------------------------------------------- pan types
    {
        const QRect pan = w.headerPartRect(ids.a1, TimelineWidget::PartPan);
        auto panOf = [&](uint64_t id) { for (const auto& t : p.tracks) if (t.id == id) return t.panType; return std::string("?"); };
        QStringList items;
        const bool picked = !pan.isEmpty() && menuPick(&w, pan.center(), "Balance (-3 dB Center)", QString(), &items);
        check(picked && panOf(ids.a1) == "balance3", "right-click on the pan slider: Balance (-3 dB Center)", items.join(", ").toStdString());
        check(items.join(",") == "Add Channels (0 dB Center),Balance (0 dB Center),Balance (-3 dB Center),Balance (-6 dB Center),Constant Power,Film",
              "VEGAS's pan types in its order", items.join(",").toStdString());
        check(menuPick(&w, w.headerRect(ids.a1).center(), "Add Channels (0 dB Center)", "Pan Type") && panOf(ids.a1) == "add",
              "the track's menu: Pan Type > Add Channels");
        check(st().undoName() == "Track Pan Type", "(an undo step)", st().undoName());
    }

    // ---------------------------------------------------------------- bus tracks
    {
        check(w.busTrackRect(EnvelopeOwner{EnvelopeOwner::Master, 0}).isEmpty(), "no bus tracks until they are asked for");
        w.options().showBusTracks = true;
        w.optionsChanged();
        uitest::settle(50);
        EnvelopeOwner master{EnvelopeOwner::Master, 0}, video{EnvelopeOwner::VideoOut, 0}, busA{EnvelopeOwner::Bus, p.buses[0].id};
        const QRect mr = w.busTrackRect(master), vr = w.busTrackRect(video), br = w.busTrackRect(busA);
        check(!mr.isEmpty() && !vr.isEmpty() && !br.isEmpty() && vr.top() > w.trackRect(ids.a2).bottom() && mr.top() > vr.top() && br.top() > mr.top(),
              "Show Bus Tracks: the video output, the master and Bus A under the tracks");
        QStringList items;
        check(menuPick(&w, mr.center(), "Volume", "Insert/Remove Envelope", &items) && findEnvelope(p.masterEnvelopes, "volume"), "the master's menu puts a volume envelope on");
        check(items.contains("Mute") && !items.contains("Pan"), "the master offers Volume and Mute", items.join(", ").toStdString());
        items.clear();
        check(menuPick(&w, vr.center(), "Fade to Color", "Insert/Remove Envelope", &items) && findEnvelope(p.videoEnvelopes, "fadecolor"), "the video output's menu: Fade to Color");
        check(items.contains("Mute") && items.contains("Motion Blur Amount") && items.contains("Video Supersampling"),
              "the video output offers fade to colour, mute, motion blur and supersampling", items.join(", ").toStdString());
        menuPick(&w, br.center(), "Pan", "Insert/Remove Envelope");
        check(findEnvelope(p.buses[0].envelopes, "pan") != nullptr, "a bus's envelopes");
        EnvelopeRef mv;
        mv.owner = master;
        mv.kind = "volume";
        const QPoint m0 = w.envelopePointPos(mv, 0);
        uitest::mouseDoubleClick(&w, QPoint(w.xAt(6.0), m0.y()));
        check(p.masterEnvelopes[0].points.size() == 2, "points are added on bus tracks the same way");
        const QPoint m1 = w.envelopePointPos(mv, 1);
        uitest::mouseDrag(&w, m1, m1 + QPoint(0, 15));
        check(p.masterEnvelopes[0].points[1].value < 0.8, "and dragged", num(p.masterEnvelopes[0].points[1].value));
        st().edit("Fade", [](Project& pp) { pp.videoEnvelopes[0].points[0].value = -0.6; });
        uitest::settle(100);
        const QImage img = uitest::shot(&w, out + "/env_bus.png");
        check(coloured(img, QRect(w.xAt(8.0), mr.top() + 2, 100, mr.height() - 4), false) > 60, "the master's volume envelope is drawn on its bus track");
        // View > Show Envelopes off: nothing drawn, nothing to take hold of
        w.options().showEnvelopes = false;
        w.optionsChanged();
        const QImage off = uitest::shot(&w, out + "/env_off.png");
        check(coloured(off, QRect(w.xAt(8.0), mr.top() + 2, 100, mr.height() - 4), false) < 10 && w.envelopePointPos(mv, 0).x() < 0, "Show Envelopes off hides them all");
        w.options().showEnvelopes = true;
        w.options().showBusTracks = false;
        w.optionsChanged();
    }

    // ---------------------------------------------------------------- fade lengths, waveforms zoomed vertically
    {
        st().setSelection({ids.e4});
        w.options().showFadeLengths = true;
        w.optionsChanged();
        uitest::settle(50);
        uitest::shot(&w, out + "/env_fadelengths.png");
        w.options().showFadeLengths = false;
        w.optionsChanged();
        st().setSelection({});
        // any sound grows taller with Shift+Up
        const QRect a1 = w.trackRect(ids.a1);
        const QRect area(w.xAt(1.5), a1.top() + 4, w.xAt(4.5) - w.xAt(1.5), a1.height() - 8);
        auto light = [&](const QImage& img) {
            int n = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); x += 2) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() + c.green() + c.blue() > 560) ++n;
                }
            return n;
        };
        const int before = light(w.grab().toImage());
        w.setFocus();
        uitest::key(&w, Qt::Key_Up, Qt::ShiftModifier);
        uitest::key(&w, Qt::Key_Up, Qt::ShiftModifier);
        const int after = light(uitest::shot(&w, out + "/env_wavezoom.png"));
        check(after > before * 1.3, "Shift+Up zooms the waveforms vertically", strfmt("%d -> %d light pixels", before, after));
        uitest::key(&w, Qt::Key_Down, Qt::ShiftModifier);
        uitest::key(&w, Qt::Key_Down, Qt::ShiftModifier);
        check(light(w.grab().toImage()) == before, "and Shift+Down back");
    }

    // ---------------------------------------------------------------- E, saving
    {
        st().currentTrack = ids.v2;
        w.cycleEnvelope(1);
        w.cycleEnvelope(1);
        check(st().project().tracks[1].envelopes.size() == 2, "(two envelopes on the track to go through)");
        const std::string json = st().project().toJson().dump();
        Project back;
        std::string err;
        Json j;
        check(Json::parse(json, j) && back.fromJson(j, &err) && back.findTrack(ids.v2)->envelopes.size() == 2 && back.findEvent(ids.e6)->fadeInCurve == CurveSlow &&
                  back.masterEnvelopes.size() == 1 && back.masterEnvelopes[0].points.size() == 2,
              "envelopes and fade curves are saved with the project", err);
    }

    return uitest::summary();
}
