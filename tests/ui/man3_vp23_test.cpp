// VEGAS Pro 23 manual features (video design and optimization): Insert Subtitles, the Video FX window's Transition Progress and
// Adjust Event Length buttons, "Apply After Track Compositing", the Optical Flow resample switch, and the Slow Motion plug-in
// changing the playback speed.
//
//   man3_vp23_test <output folder> <testfx.ofx>
//
// Set HOME to a scratch folder.
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/subtitles.h"
#include "../../src/app/playback.h"
#include "../../src/engine/engine.h"
#include "../../src/ofx/builtins.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QSettings>
#include <QToolButton>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMan3Vp23Test");
    QApplication::setApplicationName("man3_vp23_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) { qWarning("usage: man3_vp23_test <output folder> <testfx.ofx>"); return 2; }
    const QString out = args[1];
    QDir().mkpath(out);
    uitest::setup({args[2].toStdString()}, {});
    AppState& app = AppState::get();

    // ---- subtitle files
    {
        const auto lines = parseSubtitleText("Hello there\nSecond line\n\nThird", 30.0);
        check(lines.size() == 3 && lines[0].text == "Hello there" && lines[1].start == 5.0 && lines[2].start == 10.0 && lines[0].length == 5.0,
              "subtitles: lines without timing are 5 seconds each, one after the other, empty lines left out");
        const auto timed = parseSubtitleText("00:00:01;00,00:00:03;00,00:00:02;00,Hi, there\n00:00:04;00\t00:00:06;00\t00:00:02;00\tBye\n", 30.0);
        check(timed.size() == 2 && std::fabs(timed[0].start - 1.0) < 1e-6 && std::fabs(timed[0].length - 2.0) < 1e-6 && timed[1].text == "Bye" && std::fabs(timed[1].start - 4.0) < 1e-6,
              "subtitles: CSV and tab separated lines with timecodes", timed.empty() ? "" : timed[0].text);
        check(!subtitlePresetNames().empty(), "subtitles: the Titles & Text presets are offered");
    }
    app.newProject();
    {
        std::string err;
        const int n = insertSubtitles(parseSubtitleText("One\nTwo\nThree", 30.0), "", &err);
        const Project& p = app.project();
        check(n == 3 && !p.tracks.empty() && p.tracks[0].events.size() == 3, "Insert Subtitles: a new top video track with an event per line", err);
        check(app.undoName() == "Insert Subtitles", "Insert Subtitles: one undo step", app.undoName());
        app.undo();
        check(app.project().tracks.empty() || app.project().tracks[0].events.empty(), "Insert Subtitles: undone in one go");
        check(insertSubtitles({}, "", &err) == 0, "Insert Subtitles: nothing to insert is refused");
    }

    // ---- the Video FX window
    app.newProject();
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    uint64_t V = 0, VT = 0;
    for (uint64_t id : placed) {
        const Track* t = nullptr;
        app.project().findEvent(id, &t);
        if (t && t->type == TrackType::Video) { V = id; VT = t->id; }
    }
    check(V && VT, "setup: a clip on the timeline");
    MainWindow w;
    w.resize(1280, 800);
    uitest::settle(300);

    // Slow Motion: the playback rate changes how fast the media plays, Adjust Event Length stretches the event
    uint64_t slow = 0;
    app.edit("Slow Motion", [&](Project& p) {
        FxSlot s;
        s.id = p.newId();
        s.kind = "ofx";
        s.pluginId = kSlowMotionId;
        slow = s.id;
        p.findEvent(V)->fx.push_back(s);
    });
    {
        const Event* e = app.project().findEvent(V);
        const MediaItem* m = app.project().findMedia(e->mediaId);
        check(std::fabs(eventFxRate(*e) - 0.5) < 1e-9 && std::fabs(eventMediaTime(*e, m, 2.0) - 1.0) < 1e-6, "Slow Motion: the default plays at half speed", std::to_string(eventMediaTime(*e, m, 2.0)));
    }
    FxTarget t;
    t.kind = FxTarget::Event;
    t.id = V;
    w.openFx(t, slow);
    uitest::settle(300);
    QToolButton* adjust = w.fxWindow() ? w.fxWindow()->findChild<QToolButton*>("fxAdjustEventLength") : nullptr;
    check(adjust && adjust->isVisible(), "Video FX window: Adjust Event Length shows for the Slow Motion plug-in");
    if (adjust) {
        const double before = app.project().findEvent(V)->length;
        adjust->click();
        uitest::settle(100);
        const double after = app.project().findEvent(V)->length;
        check(std::fabs(after - before * 2.0) < 1e-6, "Adjust Event Length: half speed doubles the event", std::to_string(before) + " -> " + std::to_string(after));
        app.undo();
        check(std::fabs(app.project().findEvent(V)->length - before) < 1e-6, "Adjust Event Length: one undo step");
    }
    uitest::shot(w.fxWindow(), out + "/fx_slowmotion.png");
    w.fxWindow()->close();

    // a transition: the Transition Progress button puts the envelope on and takes it off
    std::string transId;
    for (const auto& info : ofx::builtinInfos())
        if (info.transition) { transId = info.id; break; }
    check(!transId.empty(), "there are built-in transitions");
    uint64_t second = 0;
    app.edit("Overlap", [&](Project& p) {
        Track* tr = p.findTrack(VT);
        Event e2 = *p.findEvent(V);
        e2.id = p.newId();
        e2.start = tr->events[0].start + tr->events[0].length - 1.0;
        e2.fx.clear();
        FxSlot s;
        s.id = p.newId();
        s.kind = "ofx";
        s.pluginId = transId;
        e2.transition = s;
        second = e2.id;
        tr->events.push_back(e2);
    });
    FxTarget tt;
    tt.kind = FxTarget::Transition;
    tt.id = second;
    w.openFx(tt, app.project().findEvent(second)->transition.id);
    uitest::settle(300);
    QToolButton* prog = w.fxWindow() ? w.fxWindow()->findChild<QToolButton*>("fxTransitionProgress") : nullptr;
    check(prog && prog->isVisible(), "Video FX window: Transition Progress shows for a transition");
    auto hasProgress = [&] {
        const Event* e = app.project().findEvent(second);
        for (const auto& en : e->envelopes) if (en.kind == "transition") return true;
        return false;
    };
    if (prog) {
        prog->click();
        uitest::settle(100);
        check(hasProgress(), "Transition Progress: the envelope is added");
        prog->click();
        uitest::settle(100);
        check(!hasProgress(), "Transition Progress: pressed again it is taken off");
    }
    uitest::shot(w.fxWindow(), out + "/fx_transition.png");
    w.fxWindow()->close();

    // Apply After Track Compositing: the chain button's flag (set directly; the menu item is the same edit)
    app.edit("Track FX", [&](Project& p) {
        FxSlot a, b;
        a.id = p.newId(); a.kind = "ofx"; a.pluginId = "x:a";
        b.id = p.newId(); b.kind = "ofx"; b.pluginId = "x:b"; b.afterComposite = true;
        p.findTrack(VT)->fx = {a, b};
    });
    {
        Project q;
        check(q.fromJson(app.project().toJson()) && q.findTrack(VT)->fx.size() == 2 && !q.findTrack(VT)->fx[0].afterComposite && q.findTrack(VT)->fx[1].afterComposite,
              "Apply After Track Compositing: saved with the project");
    }

    // Optical Flow switch
    QAction* optical = w.findChild<QAction*>("switchResampleOptical");
    check(optical != nullptr, "Switches menu: Optical Flow");
    if (optical) {
        app.selectOnly(V);
        for (QMenu* m : w.findChildren<QMenu*>()) emit m->aboutToShow();  // menus grey and tick themselves as they open
        uitest::settle(100);
        const bool en = optical->isEnabled();
        optical->trigger();
        uitest::settle(100);
        check(app.project().findEvent(V)->resample == "optical", "Switches > Optical Flow sets the event's resampling", std::string(en ? "enabled" : "disabled") + ", sel " + std::to_string(app.selection().size()) + ", resample " + app.project().findEvent(V)->resample);
    }
    return uitest::summary();
}
