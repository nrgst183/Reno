// Tests and pictures for the timeline's track headers as VEGAS 18 draws them (the send slider row, the compositing
// buttons, the track output meters, track group headers, the FX button's arrow) and for dragging events below the last
// track (VEGAS makes new tracks for them).
//
//   hdr_vegasui_test <output folder>
//
// QT_QPA_PLATFORM=offscreen. The folder gets the pictures.
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <functional>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
AppState& st() { return AppState::get(); }

uint64_t addEvent(Project& p, uint64_t track, uint64_t media, double start, double length, uint64_t group = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.group = group;
    e.name = p.findMedia(media)->name;
    p.findTrack(track)->events.push_back(e);
    p.findTrack(track)->sortEvents();
    return e.id;
}

const Track* trackOfEvent(uint64_t id)
{
    const Track* t = nullptr;
    st().project().findEvent(id, &t);
    return t;
}

size_t eventsOn(uint64_t track)
{
    for (const Track& t : st().project().tracks)
        if (t.id == track) return t.events.size();
    return 0;
}

int countKind(TrackType type)
{
    int n = 0;
    for (const Track& t : st().project().tracks) n += t.type == type;
    return n;
}

// Where an event's middle is on the timeline, and a point in the empty area below the tracks under it.
QPoint middleOf(TimelineWidget* tl, uint64_t event) { return tl->eventRectOf(event).center(); }
QPoint belowTracks(TimelineWidget* tl, uint64_t event)
{
    const Project& p = st().project();
    int bottom = 0;
    for (const Track& t : p.tracks) bottom = std::max(bottom, tl->trackRect(t.id).bottom());
    return QPoint(middleOf(tl, event).x(), bottom + 20);
}

void dragIntoVoid(TimelineWidget* tl)
{
    AppState& app = st();
    std::string err;

    // ---- a lone audio event dragged below the tracks: a new audio track at the end, the event on it
    app.newProject();
    uint64_t snd = app.importMedia("synthaudio:1", &err);
    const uint64_t v1 = app.addTrack(TrackType::Video), a1 = app.addTrack(TrackType::Audio);
    uint64_t lone = 0;
    app.edit("Setup", [&](Project& p) { lone = addEvent(p, a1, snd, 2.0, 3.0); });
    tl->zoomToFit();
    uitest::settle(80);
    const size_t before = app.project().tracks.size();
    {
        const QPoint from = middleOf(tl, lone), to = belowTracks(tl, lone);
        uitest::mousePress(tl, from);
        for (int k = 1; k <= 8; ++k) uitest::mouseMove(tl, QPoint(from.x(), from.y() + (to.y() - from.y()) * k / 8));
        uitest::settle(30);
        uitest::shot(tl, gOut + "/drag_void_ghost.png");
        check(app.project().tracks.size() == before, "while dragging below the tracks no track is made yet");
        uitest::mouseRelease(tl, to);
    }
    uitest::settle(30);
    const Track* lt = trackOfEvent(lone);
    check(app.project().tracks.size() == before + 1 && lt && lt->type == TrackType::Audio && lt->id != a1 && lt->id == app.project().tracks.back().id,
          "an audio event dragged below the last track goes onto a new audio track at the end",
          strfmt("%d tracks", (int)app.project().tracks.size()));
    const Event* le = app.project().findEvent(lone);
    check(le && std::fabs(le->start - 2.0) < 0.05, "it keeps its time", le ? strfmt("%.3f", le->start) : std::string("gone"));
    check(eventsOn(a1) == 0, "and has left its old track");
    uitest::shot(tl, gOut + "/drag_void_done.png");
    app.undo();
    uitest::settle(30);
    check(app.project().tracks.size() == before && trackOfEvent(lone) && trackOfEvent(lone)->id == a1, "one undo takes the move and the new track back");

    // ---- a video event with its sound (grouped) dragged below: each goes onto a new track of its kind
    app.newProject();
    const uint64_t clip = app.importMedia("synth:2", &err);
    const uint64_t vv = app.addTrack(TrackType::Video), aa = app.addTrack(TrackType::Audio);
    uint64_t pic = 0, sound = 0;
    app.edit("Setup", [&](Project& p) {
        const uint64_t g = p.newId();
        pic = addEvent(p, vv, clip, 1.0, 4.0, g);
        sound = addEvent(p, aa, clip, 1.0, 4.0, g);
    });
    tl->zoomToFit();
    uitest::settle(80);
    uitest::mouseDrag(tl, middleOf(tl, pic), belowTracks(tl, pic), 10);
    uitest::settle(30);
    const Track* pt = trackOfEvent(pic);
    const Track* stt = trackOfEvent(sound);
    check(countKind(TrackType::Video) == 2 && countKind(TrackType::Audio) == 2, "the pair gets a new video track and a new audio track",
          strfmt("%d video, %d audio", countKind(TrackType::Video), countKind(TrackType::Audio)));
    check(pt && pt->id != vv && pt->type == TrackType::Video && stt && stt->id != aa && stt->type == TrackType::Audio,
          "the picture and the sound are each on the new track of their kind");
    check(app.project().tracks.size() == 4 && app.project().tracks[1].id == (pt ? pt->id : 0) && app.project().tracks[3].id == (stt ? stt->id : 0),
          "the new video track goes after the last video track, the audio track at the end");
    app.undo();
    uitest::settle(30);
    check(app.project().tracks.size() == 2 && trackOfEvent(pic) && trackOfEvent(pic)->id == vv && trackOfEvent(sound) && trackOfEvent(sound)->id == aa,
          "one undo puts the pair back");

    // ... with a second audio track below: the sound goes down onto it instead of a new one
    const uint64_t a2 = app.addTrack(TrackType::Audio);
    tl->zoomToFit();
    uitest::settle(80);
    uitest::mouseDrag(tl, middleOf(tl, pic), belowTracks(tl, pic), 10);
    uitest::settle(30);
    check(countKind(TrackType::Video) == 2 && countKind(TrackType::Audio) == 2 && trackOfEvent(sound) && trackOfEvent(sound)->id == a2,
          "the sound moves down onto the audio track below where there is one",
          strfmt("%d video, %d audio", countKind(TrackType::Video), countKind(TrackType::Audio)));

    // ---- Ctrl+drag (copy) below: the copy on a new track, the original stays
    app.newProject();
    snd = app.importMedia("synthaudio:1", &err);
    app.addTrack(TrackType::Video);
    const uint64_t ac = app.addTrack(TrackType::Audio);
    uint64_t orig = 0;
    app.edit("Setup", [&](Project& p) { orig = addEvent(p, ac, snd, 3.0, 2.0); });
    tl->zoomToFit();
    uitest::settle(80);
    const size_t n0 = app.project().tracks.size();
    uitest::mouseDrag(tl, middleOf(tl, orig), belowTracks(tl, orig), 10, Qt::ControlModifier);
    uitest::settle(30);
    const Track* last = &app.project().tracks.back();
    check(app.project().tracks.size() == n0 + 1 && trackOfEvent(orig) && trackOfEvent(orig)->id == ac && last->events.size() == 1 &&
              last->events[0].id != orig && std::fabs(last->events[0].start - 3.0) < 0.05,
          "Ctrl+drag below the tracks puts a copy on a new track and leaves the original");
    app.undo();
    uitest::settle(30);
    check(app.project().tracks.size() == n0 && eventsOn(ac) == 1, "one undo takes the copy and its track back");
    (void)v1;
}

// Runs 'click', which is to open a popup menu; the menu gets its item 'pick' triggered (empty: just closed). The menu's
// items are returned.
QStringList withMenu(const std::function<void()>& click, const QString& pick)
{
    QStringList items;
    QTimer::singleShot(80, [&] {
        QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m) return;
        QAction* hit = nullptr;
        for (QAction* a : m->actions()) {
            if (a->isSeparator()) continue;
            items << a->text();
            if (a->text() == pick) hit = a;
        }
        m->close();
        if (hit) hit->trigger();
    });
    click();
    uitest::settle(30);
    return items;
}

// The items of the menu a right-click at 'pos' shows, one triggered ('pick').
QStringList rightMenu(TimelineWidget* tl, const QPoint& pos, const QString& pick = QString())
{
    QMenu m;
    QStringList items;
    if (!tl->buildContextMenu(&m, pos)) return items;
    QAction* hit = nullptr;
    for (QAction* a : m.actions()) {
        if (a->isSeparator()) continue;
        items << a->text();
        if (a->text() == pick) hit = a;
    }
    if (hit) hit->trigger();
    uitest::settle(30);
    return items;
}

void saveHeader(TimelineWidget* tl, uint64_t track, const QString& name, int extraTop = 0)
{
    tl->update();
    uitest::settle(40);
    const QRect r = tl->headerRect(track).adjusted(0, -extraTop, 0, 0);
    tl->grab(r).save(gOut + "/" + name);
}

double sendLevel(uint64_t track, uint64_t bus, bool* pre = nullptr)
{
    for (const Track& t : st().project().tracks)
        if (t.id == track)
            for (const Send& s : t.sends)
                if (s.bus == bus) {
                    if (pre) *pre = s.preFader;
                    return s.level;
                }
    return -1.0;
}

const Track* findTrack(uint64_t id)
{
    for (const Track& t : st().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

void sendRow(TimelineWidget* tl)
{
    AppState& app = st();
    app.newProject();
    const uint64_t a1 = app.addTrack(TrackType::Audio);
    uint64_t fx1 = 0, busA = 0;
    app.edit("Setup", [&](Project& p) {
        p.findTrack(a1)->height = 118;
        p.findTrack(a1)->name = "Track 1";
    });
    tl->setHeaderWidth(161);
    uitest::settle(60);
    check(tl->headerPartRect(a1, TimelineWidget::PartSendLabel).isEmpty(), "no send row without busses or assignable FX");
    app.edit("Busses", [&](Project& p) {
        Bus fx;
        fx.id = fx1 = p.newId();
        fx.name = "FX 1";
        fx.assignable = true;
        p.buses.push_back(fx);
        for (const char* n : {"Bus A", "Bus B", "Bus C"}) {
            Bus b;
            b.id = p.newId();
            b.name = n;
            if (!busA) busA = b.id;
            p.buses.push_back(b);
        }
    });
    uitest::settle(60);
    const QRect label = tl->headerPartRect(a1, TimelineWidget::PartSendLabel), slider = tl->headerPartRect(a1, TimelineWidget::PartSend),
                prepost = tl->headerPartRect(a1, TimelineWidget::PartSendPrePost), pan = tl->headerPartRect(a1, TimelineWidget::PartPan);
    check(!label.isEmpty() && !slider.isEmpty() && !prepost.isEmpty() && label.top() > pan.top() && label.left() < slider.left() && slider.right() < prepost.left(),
          "with busses an audio header has a send row under Pan: chooser button, slider, Pre/Post");
    check(!tl->headerPartRect(a1, TimelineWidget::PartPhase).isEmpty() && tl->headerPartRect(a1, TimelineWidget::PartPhase).top() > label.top(),
          "the phase and output bus buttons move under it");
    saveHeader(tl, a1, "send_row_narrow.png");
    tl->setHeaderWidth(330);
    uitest::settle(60);
    check(!tl->headerPartRect(a1, TimelineWidget::PartSendValue).isEmpty(), "a wide header also shows the send's value");
    saveHeader(tl, a1, "send_row_wide.png");

    // the chooser: every assignable FX chain and bus with the level, the shown one ticked
    const QRect lab = tl->headerPartRect(a1, TimelineWidget::PartSendLabel);
    QStringList items = withMenu([&] { uitest::mouseClick(tl, lab.center()); }, QString());
    check(items == QStringList({"FX 1: -Inf.", "Bus A: -Inf.", "Bus B: -Inf.", "Bus C: -Inf."}), "the send button's menu lists FX 1 and the busses with their levels",
          items.join(" | ").toStdString());

    // dragging the slider: one undo step, made with VEGAS's send (to an assignable FX: post-fader)
    const size_t undoBefore = app.undoCount();
    {
        const QRect s = tl->headerPartRect(a1, TimelineWidget::PartSend);
        uitest::mouseDrag(tl, QPoint(s.left() + 2, s.center().y()), QPoint(s.left() + s.width() * 2 / 3, s.center().y()), 8);
    }
    bool pre = true;
    const double lv = sendLevel(a1, fx1, &pre);
    check(lv > 0.05 && !pre, "dragging the slider sets the send to FX 1 (post fader)", strfmt("%.3f", lv));
    check(app.undoCount() == undoBefore + 1 && app.undoName() == "Send Level", "as one undo step", app.undoName());
    saveHeader(tl, a1, "send_row_set.png");
    uitest::mouseClick(tl, tl->headerPartRect(a1, TimelineWidget::PartSendPrePost).center());
    sendLevel(a1, fx1, &pre);
    check(pre, "Pre/Post switches the send to pre-fader");
    uitest::mouseDoubleClick(tl, tl->headerPartRect(a1, TimelineWidget::PartSend).center());
    check(std::fabs(sendLevel(a1, fx1) - 1.0) < 1e-9, "a double-click sets the send to 0 dB");

    // choosing Bus A: the row shows the send to Bus A
    items = withMenu([&] { uitest::mouseClick(tl, lab.center()); }, "Bus A: -Inf.");
    {
        const QRect s = tl->headerPartRect(a1, TimelineWidget::PartSend);
        uitest::mouseDrag(tl, QPoint(s.left() + 2, s.center().y()), QPoint(s.left() + s.width() / 2, s.center().y()), 6);
    }
    bool preA = false;
    check(sendLevel(a1, busA, &preA) > 0.0 && preA, "after choosing Bus A the slider sends to it (a bus send starts pre-fader)");
    items = rightMenu(tl, lab.center());
    check(items.size() == 4 && items[1].startsWith("Bus A: ") && items[1] != "Bus A: -Inf.", "the menu shows Bus A's level now", items.join(" | ").toStdString());
    saveHeader(tl, a1, "send_row_busA.png");
    tl->setHeaderWidth(161);
}

void fxArrow(TimelineWidget* tl)
{
    AppState& app = st();
    app.newProject();
    const uint64_t a1 = app.addTrack(TrackType::Audio), v1 = app.addTrack(TrackType::Video);
    tl->setHeaderWidth(300);
    uitest::settle(60);
    const QRect arrow = tl->headerPartRect(a1, TimelineWidget::PartFxArrow), fx = tl->headerPartRect(a1, TimelineWidget::PartFx);
    check(!arrow.isEmpty() && arrow.left() > fx.right() - 1, "an audio track's FX button has its arrow beside it");
    check(tl->headerPartRect(v1, TimelineWidget::PartFxArrow).isEmpty(), "a video track's has none (VEGAS 18)");
    QStringList items = withMenu([&] { uitest::mouseClick(tl, arrow.center()); }, QString());
    check(items == QStringList({"FX Automation..."}), "the arrow's menu: FX Automation...", items.join(" | ").toStdString());
    int opened = 0;
    tl->onOpenFx = [&](const FxTarget& t, uint64_t) { opened += t.id == a1; };
    withMenu([&] { uitest::mouseClick(tl, arrow.center()); }, "FX Automation...");
    check(opened == 1, "with no track effects it opens the plug-in chooser (the track's FX)");
    tl->onOpenFx = nullptr;
    saveHeader(tl, a1, "fx_arrow.png");
}

void compositing(TimelineWidget* tl)
{
    AppState& app = st();
    app.newProject();
    const uint64_t v1 = app.addTrack(TrackType::Video), v2 = app.addTrack(TrackType::Video);
    app.edit("Names", [&](Project& p) {
        p.findTrack(v1)->name = "Kite";
        p.findTrack(v2)->name = "SloMo";
    });
    tl->setHeaderWidth(300);
    uitest::settle(60);
    const QRect mode = tl->headerPartRect(v2, TimelineWidget::PartCompMode), child = tl->headerPartRect(v2, TimelineWidget::PartCompChild),
                parent = tl->headerPartRect(v2, TimelineWidget::PartCompParent), level = tl->headerPartRect(v2, TimelineWidget::PartLevel);
    check(!mode.isEmpty() && !child.isEmpty() && !parent.isEmpty() && mode.left() > level.right() && child.left() > mode.left() && parent.left() > child.left() &&
              std::abs(mode.center().y() - level.center().y()) <= 2,
          "a video header's Level row ends with Compositing Mode, Make Compositing Child and Make Compositing Parent");
    saveHeader(tl, v1, "comp_before.png");
    const int strip0 = tl->headerPartRect(v2, TimelineWidget::PartStrip).left();
    uitest::mouseClick(tl, child.center());
    check(findTrack(v2) && findTrack(v2)->compositeLevel == 1, "Make Compositing Child makes the track a child");
    check(tl->headerPartRect(v2, TimelineWidget::PartStrip).left() > strip0, "the child's header is nested in its parent's");
    check(!tl->headerPartRect(v1, TimelineWidget::PartParentMotion).isEmpty() && !tl->headerPartRect(v1, TimelineWidget::PartParentComp).isEmpty(),
          "the parent shows Parent Motion and Parent Composite Mode");
    {
        const QRect top = tl->headerRect(v1), bottom = tl->headerRect(v2);
        tl->update();
        uitest::settle(40);
        tl->grab(QRect(0, top.top(), top.width(), bottom.bottom() - top.top() + 1)).save(gOut + "/comp_parent_child.png");
    }
    withMenu([&] { uitest::mouseClick(tl, tl->headerPartRect(v2, TimelineWidget::PartCompMode).center()); }, "Multiply (Mask)");
    check(findTrack(v2)->composite == "multiply", "the Compositing Mode button's menu sets the mode");
    withMenu([&] { uitest::mouseClick(tl, tl->headerPartRect(v1, TimelineWidget::PartParentComp).center()); }, "Add");
    check(findTrack(v1)->parentComposite == "add", "Parent Composite Mode's menu sets the parent's");
    uitest::mouseClick(tl, tl->headerPartRect(v2, TimelineWidget::PartCompParent).center());
    check(findTrack(v2)->compositeLevel == 0 && tl->headerPartRect(v1, TimelineWidget::PartParentMotion).isEmpty(), "Make Compositing Parent takes it out again");
    tl->setHeaderWidth(161);
    uitest::settle(60);
    check(!tl->headerPartRect(v2, TimelineWidget::PartCompMode).isEmpty(), "a narrow header keeps the Compositing Mode button");
    saveHeader(tl, v2, "comp_narrow.png");
}

void meters(TimelineWidget* tl)
{
    AppState& app = st();
    app.newProject();
    QSettings().remove("timeline/trackMeters");
    QSettings().remove("timeline/trackMetersVertical");
    const uint64_t a1 = app.addTrack(TrackType::Audio);
    app.edit("Setup", [&](Project& p) {
        p.findTrack(a1)->height = 90;
        p.findTrack(a1)->name = "Track 7";
    });
    tl->setHeaderWidth(220);
    uitest::settle(60);
    const QRect m = tl->headerPartRect(a1, TimelineWidget::PartMeter), h = tl->headerRect(a1);
    check(!m.isEmpty() && m.height() > m.width() && m.right() > tl->headerPartRect(a1, TimelineWidget::PartSolo).right(),
          "an audio header has a vertical output meter down its right");
    tl->showTrackMeterLevels(a1, 0.7f, 0.5f);
    saveHeader(tl, a1, "meter_vertical.png");
    const QStringList items = rightMenu(tl, m.center());
    check(items.contains("Reset Clip") && items.contains("Vertical") && items.contains("Show Meters") && items.contains("-60 dB"),
          "the meter's menu: Reset Clip, the ranges, Vertical, Show Meters", items.join(" | ").toStdString());
    rightMenu(tl, m.center(), "Vertical");
    const QRect mh = tl->headerPartRect(a1, TimelineWidget::PartMeter);
    check(!mh.isEmpty() && mh.width() > mh.height() && mh.top() < tl->headerPartRect(a1, TimelineWidget::PartLevel).top(),
          "Vertical off: the meter runs across under the name, the sliders under it");
    tl->showTrackMeterLevels(a1, 1.2f, 0.6f);
    saveHeader(tl, a1, "meter_horizontal.png");
    rightMenu(tl, mh.center(), "Show Meters");
    check(tl->headerPartRect(a1, TimelineWidget::PartMeter).isEmpty(), "Show Meters off: no meter");
    QMenu ham;
    tl->buildTrackHamburger(&ham, a1);
    QAction* show = nullptr;
    for (QAction* a : ham.actions())
        if (a->text() == "Show Meters") show = a;
    check(show && !show->isChecked(), "the track menu can show them again");
    if (show) show->trigger();
    rightMenu(tl, tl->headerPartRect(a1, TimelineWidget::PartMeter).center(), "Vertical");
    check(tl->headerPartRect(a1, TimelineWidget::PartMeter).height() > 40, "back to vertical meters");
    (void)h;
    tl->setHeaderWidth(161);
}

void groups(TimelineWidget* tl)
{
    AppState& app = st();
    std::string err;
    app.newProject();
    const uint64_t clip = app.importMedia("synth:3", &err);
    const uint64_t v1 = app.addTrack(TrackType::Video), v2 = app.addTrack(TrackType::Video), v3 = app.addTrack(TrackType::Video);
    app.edit("Setup", [&](Project& p) {
        p.findTrack(v1)->name = "Kite";
        p.findTrack(v2)->name = "SloMo";
        p.findTrack(v3)->name = "Sky";
        addEvent(p, v1, clip, 0.5, 3.0);
        addEvent(p, v2, clip, 2.0, 4.0);
        addEvent(p, v3, clip, 1.0, 2.0);
    });
    tl->setHeaderWidth(300);
    tl->zoomToFit();
    uitest::settle(60);
    const int y1 = tl->headerRect(v1).top();
    tl->setSelectedTracks({v1, v2});
    tl->groupTracks(true);
    uitest::settle(60);
    const QRect row = tl->trackGroupRect(v1, 0);
    check(!row.isEmpty() && row.bottom() < tl->headerRect(v1).top() && tl->headerRect(v1).top() >= y1 + 18 && tl->trackGroupRect(v3, 0).isEmpty(),
          "a track group has a header row above its tracks");
    check(!tl->headerPartRect(v1, TimelineWidget::PartGroupLane).isEmpty() && !tl->headerPartRect(v2, TimelineWidget::PartGroupLane).isEmpty() &&
              tl->headerPartRect(v3, TimelineWidget::PartGroupLane).isEmpty() &&
              tl->headerPartRect(v1, TimelineWidget::PartStrip).left() > tl->headerPartRect(v3, TimelineWidget::PartStrip).left(),
          "and a grouping lane down the left of its headers");
    {
        tl->update();
        uitest::settle(40);
        tl->grab(QRect(0, row.top(), tl->width(), tl->headerRect(v3).bottom() - row.top() + 1)).save(gOut + "/group_expanded.png");
    }
    uitest::mouseClick(tl, tl->trackGroupRect(v1, 3).center());
    check(findTrack(v1)->mute && findTrack(v2)->mute && !findTrack(v3)->mute, "Mute Track Group mutes the group's tracks");
    uitest::mouseClick(tl, tl->trackGroupRect(v1, 3).center());
    check(!findTrack(v1)->mute && !findTrack(v2)->mute, "and again unmutes them");
    uitest::mouseClick(tl, tl->trackGroupRect(v1, 4).center());
    check(findTrack(v1)->solo && findTrack(v2)->solo && !findTrack(v3)->solo, "Solo Track Group solos them");
    uitest::mouseClick(tl, tl->trackGroupRect(v1, 4).center());
    tl->setSelectedTracks({v3});
    uitest::mouseClick(tl, tl->trackGroupRect(v1, 2).center());
    check(tl->selectedTracks() == std::set<uint64_t>({v1, v2}), "a click on the group's header selects its tracks");
    uitest::mouseClick(tl, tl->trackGroupRect(v1, 1).center());
    check(tl->trackRect(v1).height() == 0 && tl->trackRect(v2).height() == 0 && tl->trackRect(v3).height() > 0 && !tl->trackGroupRect(v1, 0).isEmpty(),
          "the triangle collapses the group to its header row");
    {
        const QRect r0 = tl->trackGroupRect(v1, 0);
        tl->update();
        uitest::settle(40);
        tl->grab(QRect(0, r0.top(), tl->width(), tl->headerRect(v3).bottom() - r0.top() + 1)).save(gOut + "/group_collapsed.png");
    }
    uitest::mouseClick(tl, tl->trackGroupRect(v1, 1).center());
    check(tl->trackRect(v1).height() > 0 && tl->trackRect(v2).height() > 0, "and expands it again");

    // ---- its scribble strip: double-click, type the name, Enter (VEGAS 18 manual "Renaming track groups")
    const uint64_t group = findTrack(v1)->trackGroup;
    check(app.project().trackGroupName(group).empty(), "a new track group has no name");
    uitest::mouseDoubleClick(tl, tl->trackGroupRect(v1, 2).center());
    uitest::settle(30);
    QLineEdit* edit = nullptr;
    for (QLineEdit* e : tl->findChildren<QLineEdit*>())
        if (e->isVisible()) edit = e;
    check(edit != nullptr, "double-clicking the group's name strip opens a box to type in");
    if (edit) {
        edit->setText("Surfing");
        QMetaObject::invokeMethod(edit, "editingFinished");
        uitest::settle(30);
    }
    check(app.project().trackGroupName(group) == "Surfing" && app.undoName() == "Rename Track Group", "Enter names the group, one undo step",
          app.project().trackGroupName(group) + " / " + app.undoName());
    {
        const QRect r0 = tl->trackGroupRect(v1, 0);
        tl->grab(QRect(0, r0.top(), tl->width(), tl->headerRect(v3).bottom() - r0.top() + 1)).save(gOut + "/group_named.png");
    }
    Project back;
    const bool read = back.fromJson(app.project().toJson());
    check(read && back.trackGroupName(group) == "Surfing", "the name is kept in Reno's project file");
    app.undo();
    check(app.project().trackGroupName(group).empty(), "and undo takes it away");
    app.redo();
    check(app.project().trackGroupName(group) == "Surfing", "redo puts it back");
    tl->setHeaderWidth(161);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    gOut = argc > 1 ? QString(argv[1]) : QString("hdr_out");
    QDir().mkpath(gOut);
    uitest::setup({}, {});
    AppState::get().newProject();
    Playback pb;
    TimelineWidget tl(&pb);
    tl.resize(900, 420);
    tl.show();
    uitest::settle(100);

    dragIntoVoid(&tl);
    sendRow(&tl);
    fxArrow(&tl);
    compositing(&tl);
    meters(&tl);
    groups(&tl);

    return uitest::summary();
}
