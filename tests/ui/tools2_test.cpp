// VEGAS Pro 18 editing tools: Shuffle, Slip, Slide, Time Stretch/Compress and Split Trim (Edit > Editing Tool and the normal tool
// button's drop-down), the resample switches (Edit > Switches and the event menu), the effects window's keyframe strip (VEGAS's
// Animate), audio effect presets, Red Eye Reduction, and the Plug-In Chooser's folders.
//
//   tools2_test <output folder> <testfx.ofx> [--vst <vst2 plug-in>]
//
// Build: uibuild tools2_test tests/ui/tools2_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/fxlibrary.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/app/redeye.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/ofx/ofxhost.h"
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTimer>
#include <QPainter>
#include <QToolButton>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <cmath>
#include <cstdio>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
MainWindow* gW = nullptr;
QString gOut;

QString plain(QString t) { return t.split('\t')[0].remove('&'); }

QAction* item(const QString& path)
{
    const QStringList parts = path.split('/');
    QList<QAction*> acts = gW->menuBar()->actions();
    QAction* found = nullptr;
    for (QString p : parts) {
        p.replace('|', '/');  // '|' stands for a '/' in a name: "Time Stretch|Compress Tool"
        found = nullptr;
        for (QAction* a : acts)
            if (!a->isSeparator() && plain(a->text()) == p) { found = a; break; }
        if (!found) return nullptr;
        if (found->menu()) {
            emit found->menu()->aboutToShow();
            acts = found->menu()->actions();
        }
    }
    return found;
}

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

struct Ids {
    uint64_t v1 = 0, v2 = 0, a1 = 0, clip = 0, snd = 0;
    uint64_t A = 0, B = 0, C = 0, D = 0, S = 0;     // V1: A [0,2) B [2,3) C [3,5); V2: D [1,4); A1: S [3,5) grouped with C
} ids;

void buildProject()
{
    AppState& app = st();
    app.newProject();
    std::string err;
    ids.clip = app.importMedia("synth:1", &err);
    ids.snd = app.importMedia("synthaudio:2", &err);
    app.edit("Test Project", [&](Project& p) {
        auto track = [&](TrackType t, const char* name) {
            Track tr;
            tr.id = p.newId();
            tr.type = t;
            tr.name = name;
            p.tracks.push_back(tr);
            return tr.id;
        };
        ids.v1 = track(TrackType::Video, "V1");
        ids.v2 = track(TrackType::Video, "V2");
        ids.a1 = track(TrackType::Audio, "A1");
        auto ev = [&](uint64_t t, uint64_t m, double start, double len, double off, uint64_t group = 0) {
            Event e;
            e.id = p.newId();
            e.mediaId = m;
            e.start = start;
            e.length = len;
            e.offset = off;
            e.group = group;
            p.findTrack(t)->events.push_back(e);
            return e.id;
        };
        ids.A = ev(ids.v1, ids.clip, 0.0, 2.0, 0.0);
        ids.B = ev(ids.v1, ids.clip, 2.0, 1.0, 4.0);
        ids.C = ev(ids.v1, ids.clip, 3.0, 2.0, 6.0, 77);
        ids.D = ev(ids.v2, ids.clip, 1.0, 3.0, 2.0);
        ids.S = ev(ids.a1, ids.snd, 3.0, 2.0, 6.0, 77);
    });
    app.setCursor(0.0);
    app.setSelection(std::set<uint64_t>());
}

const Event* E(uint64_t id) { return st().project().findEvent(id); }
const Track* T(uint64_t id)
{
    for (const Track& t : st().project().tracks) if (t.id == id) return &t;
    static Track none;
    return &none;
}

void setTool(const char* name)
{
    QAction* a = gW->vegasAction(name);
    if (a) a->trigger();
    uitest::settle(20);
}

// a point on an event's body (y: 60% down its track)
QPoint body(TimelineWidget* tl, uint64_t track, double t)
{
    const QRect r = tl->trackRect(track);
    return QPoint(tl->xAt(t), r.top() + r.height() * 6 / 10);
}

double pxPerSecond(TimelineWidget* tl) { return (tl->xAt(10.0) - tl->xAt(0.0)) / 10.0; }

// ====================================================================== the editing tools
void testEditingTools(TimelineWidget* tl)
{
    AppState& app = st();
    // menus and the tool button
    for (const char* n : {"Shuffle Tool", "Slip Tool", "Slide Tool", "Time Stretch|Compress Tool", "Split Trim Tool"}) {
        QAction* a = item(QString("Edit/Editing Tool/") + n);
        check(a && a->isEnabled() && a->isCheckable() && !a->icon().isNull(), std::string("Edit > Editing Tool > ") + n + " works and has a picture");
    }
    {
        // the tools' pictures, large, for looking at
        QImage sheet(6 * 40, 40, QImage::Format_ARGB32);
        sheet.fill(QColor(50, 50, 50));
        QPainter p(&sheet);
        int x = 4;
        for (const char* n : {"toolNormal", "toolShuffle", "toolSlip", "toolSlide", "toolStretch", "toolSplitTrim"}) {
            if (QAction* a = gW->vegasAction(n)) p.drawPixmap(x, 4, a->icon().pixmap(32, 32));
            x += 40;
        }
        p.end();
        sheet.save(gOut + "/tool_icons.png");
    }
    QToolButton* button = gW->findChild<QToolButton*>("editToolButton");
    QStringList drop;
    if (button && button->menu())
        for (QAction* a : button->menu()->actions()) drop << plain(a->text());
    check(drop.join(",") == "Normal Edit Tool,Shuffle Tool,Slip Tool,Slide Tool,Time Stretch/Compress Tool,Split Trim Tool",
          "the normal tool button's drop-down holds VEGAS's event tools", drop.join(",").toStdString());

    // ---- Slip: a plain drag on an event moves its media, not the event
    buildProject();
    setTool("toolSlip");
    check(tl->options().tool == EditTool::Slip, "Slip Tool chosen");
    check(button && button->defaultAction() == gW->vegasAction("toolSlip"), "the tool button shows the Slip tool now");
    const double pps = pxPerSecond(tl);
    {
        const Event before = *E(ids.D);
        uitest::mouseDrag(tl, body(tl, ids.v2, 2.0), body(tl, ids.v2, 2.0) + QPoint((int)std::lround(pps * 0.5), 0));
        const Event* d = E(ids.D);
        check(d && near(d->start, before.start) && near(d->length, before.length) && std::fabs(d->offset - before.offset) > 0.3,
              "Slip tool: a drag moves the media inside the event (start and length stay)", d ? strfmt("start %.3f offset %.3f", d->start, d->offset) : "");
        check(app.undoName() == "Slip Event", "and it is one undo step (Slip Event)", app.undoName());
        app.undo();
        check(near(E(ids.D)->offset, before.offset), "undone");
    }
    // the tool's edges: slip trim (the media goes with the edge)
    {
        const Event before = *E(ids.D);
        const QPoint edge(tl->xAt(4.0) - 2, body(tl, ids.v2, 2.0).y());
        uitest::mouseDrag(tl, edge, edge - QPoint((int)std::lround(pps * 0.5), 0));
        const Event* d = E(ids.D);
        check(d && d->length < before.length - 0.3 && app.undoName() == "Slip Trim Event", "Slip tool: dragging an edge slip-trims",
              d ? strfmt("length %.3f, %s", d->length, app.undoName().c_str()) : "");
        app.undo();
    }

    // ---- Slide: the event moves over its media (start and offset change together)
    setTool("toolSlide");
    {
        const Event before = *E(ids.D);
        uitest::mouseDrag(tl, body(tl, ids.v2, 2.5), body(tl, ids.v2, 2.5) + QPoint((int)std::lround(pps * 0.5), 0));
        const Event* d = E(ids.D);
        const double moved = d ? d->start - before.start : 0;
        check(d && moved > 0.3 && near(d->offset - before.offset, moved, 0.05), "Slide tool: the event slides over its media",
              d ? strfmt("moved %.3f offset +%.3f", moved, d->offset - before.offset) : "");
        check(app.undoName() == "Slide Event", "one undo step (Slide Event)", app.undoName());
        app.undo();
    }

    // ---- Time Stretch/Compress: dragging the end stretches (the rate changes)
    setTool("toolStretch");
    {
        const Event before = *E(ids.D);
        const QPoint edge(tl->xAt(4.0) - 2, body(tl, ids.v2, 2.0).y());
        uitest::mouseDrag(tl, edge, edge + QPoint((int)std::lround(pps * 1.0), 0));
        const Event* d = E(ids.D);
        check(d && d->length > before.length + 0.5 && d->rate < 0.9, "Time Stretch tool: dragging the end slows the event down to fill",
              d ? strfmt("length %.3f rate %.3f", d->length, d->rate) : "");
        app.undo();
        // its body still moves as with the normal tool
        uitest::mouseDrag(tl, body(tl, ids.v2, 2.5), body(tl, ids.v2, 2.5) + QPoint((int)std::lround(pps * 1.0), 0));
        check(E(ids.D)->start > 1.5 && near(E(ids.D)->offset, before.offset), "Time Stretch tool: the body still moves the event",
              strfmt("%.3f", E(ids.D)->start));
        app.undo();
    }

    // ---- Split Trim: a click splits there
    setTool("toolSplitTrim");
    {
        const size_t n = T(ids.v2)->events.size();
        uitest::mouseClick(tl, body(tl, ids.v2, 2.5));
        check(T(ids.v2)->events.size() == n + 1, "Split Trim tool: a click splits the event where clicked");
        app.undo();
        // a drag: split, then the part the mouse goes over is trimmed away
        uitest::mouseDrag(tl, body(tl, ids.v2, 2.5), body(tl, ids.v2, 2.5) + QPoint((int)std::lround(pps * 0.6), 0));
        const Track* t = T(ids.v2);
        check(t->events.size() == n + 1 && t->events[1].start > 2.9, "Split Trim tool: dragged right, the start of the part after the split is trimmed",
              t->events.size() > 1 ? strfmt("%.3f", t->events[1].start) : "");
        while (app.canUndo() && T(ids.v2)->events.size() != n) app.undo();
    }

    // ---- Shuffle: C (grouped with S on A1) dragged to before A: C goes first, A and B move down by its length
    setTool("toolShuffle");
    {
        uitest::mouseDrag(tl, body(tl, ids.v1, 4.0), body(tl, ids.v1, 1.0) - QPoint((int)std::lround(pps * 0.2), 0), 12);
        const Event *a = E(ids.A), *b = E(ids.B), *c = E(ids.C), *s = E(ids.S);
        check(c && near(c->start, 0.0) && near(a->start, 2.0) && near(b->start, 4.0), "Shuffle tool: the event is put before the others, which make room",
              strfmt("A %.2f B %.2f C %.2f", a->start, b->start, c->start));
        check(near(c->offset, 6.0) && near(c->length, 2.0), "the shuffled event keeps its media and length");
        check(s && near(s->start, 0.0), "the event grouped with it is shuffled on its own track too", s ? strfmt("%.2f", s->start) : "");
        check(near(E(ids.D)->start, 1.0), "other tracks stay as they are");
        check(app.undoName() == "Shuffle Events", "one undo step (Shuffle Events)", app.undoName());
        app.undo();
        check(near(E(ids.C)->start, 3.0) && near(E(ids.A)->start, 0.0) && near(E(ids.S)->start, 3.0), "undone");
        // A dragged after B: B moves up to 0, A follows it
        uitest::mouseDrag(tl, body(tl, ids.v1, 1.0), body(tl, ids.v1, 2.4), 12);
        check(near(E(ids.B)->start, 0.0) && near(E(ids.A)->start, 1.0) && near(E(ids.C)->start, 3.0), "Shuffle tool: dragged after the next event, they trade places",
              strfmt("A %.2f B %.2f C %.2f", E(ids.A)->start, E(ids.B)->start, E(ids.C)->start));
        app.undo();
        // a locked event in the way: nothing moves
        app.edit("Lock", [](Project& p) { if (Event* e = p.findEvent(ids.B)) e->locked = true; });
        uitest::mouseDrag(tl, body(tl, ids.v1, 4.0), body(tl, ids.v1, 0.2), 12);
        check(near(E(ids.C)->start, 3.0) && near(E(ids.B)->start, 2.0), "Shuffle tool: a locked event in the way stops the shuffle");
        app.undo();
    }
    QImage pic = uitest::shot(gW, gOut + "/tools_shuffle.png");
    (void)pic;

    // ---- keys still win: Slip tool, Ctrl+drag an edge stretches
    setTool("toolSlip");
    {
        const Event before = *E(ids.D);
        const QPoint edge(tl->xAt(4.0) - 2, body(tl, ids.v2, 2.0).y());
        uitest::mouseDrag(tl, edge, edge + QPoint((int)std::lround(pps * 1.0), 0), 8, Qt::ControlModifier);
        check(E(ids.D)->rate < 0.9 && near(E(ids.D)->offset, before.offset), "with the Slip tool, Ctrl+drag on an edge still stretches");
        app.undo();
    }

    // ---- D / Shift+D: Normal (or the event tool on its button), Envelope, Selection, Zoom
    {
        gW->vegasAction("toolNext")->trigger();
        check(tl->options().tool == EditTool::Envelope, "D from the Slip tool: the Envelope tool (the event tools share the normal tool's place)");
        gW->vegasAction("toolPrevious")->trigger();
        check(tl->options().tool == EditTool::Slip, "Shift+D back: the Slip tool again (the tool on the normal tool's button)");
        setTool("toolNormal");
        check(button->defaultAction() == gW->vegasAction("toolNormal") && tl->options().tool == EditTool::Normal, "the normal tool back on its button");
    }
    uitest::shot(button ? (QWidget*)button->parentWidget() : (QWidget*)gW, gOut + "/tools_toolbar.png");
}

// ====================================================================== the resample switches
void testResample(TimelineWidget* tl)
{
    AppState& app = st();
    buildProject();
    app.setSelection(std::set<uint64_t>());
    QAction* force = item("Edit/Switches/Force Resample");
    check(force && !force->isEnabled(), "Force Resample is greyed with no video event selected");
    app.setSelection({ids.D});
    force = item("Edit/Switches/Force Resample");
    QAction* proj = item("Edit/Switches/Use Project Resample Mode");
    check(force && force->isEnabled() && proj && proj->isChecked() && !force->isChecked(), "with a video event selected: Use Project Resample Mode ticked");
    force->trigger();
    uitest::settle(20);
    check(E(ids.D)->resample == "force" && app.undoName() == "Force Resample", "Edit > Switches > Force Resample sets the event's switch", E(ids.D)->resample);
    check(item("Edit/Switches/Force Resample")->isChecked() && !item("Edit/Switches/Use Project Resample Mode")->isChecked(), "and is ticked now");
    item("Edit/Switches/Disable Resample")->trigger();
    check(E(ids.D)->resample == "off", "Disable Resample");
    app.undo();
    app.undo();
    check(E(ids.D)->resample == "project", "both undone");
    // the event's right-click menu
    QMenu m;
    tl->buildContextMenu(&m, body(tl, ids.v2, 2.0));
    QAction* sw = nullptr;
    for (QAction* a : m.actions()) if (a->menu() && plain(a->text()) == "Switches") sw = a;
    QAction* dis = nullptr;
    if (sw)
        for (QAction* a : sw->menu()->actions()) if (plain(a->text()) == "Disable Resample") dis = a;
    check(dis && dis->isEnabled() && !dis->isChecked(), "the event menu's Switches > Disable Resample works");
    if (dis) dis->trigger();
    check(E(ids.D)->resample == "off", "and sets the switch");
    // the audio event's menu has none
    app.setSelection({ids.S});
    app.setSelection({ids.D});
}


// ====================================================================== the keyframe strip (Animate)
QWidget* findKeyStrip(QWidget* root) { return root->findChild<QWidget*>("fxKeyStrip"); }

void testKeyframes(MainWindow& w)
{
    AppState& app = st();
    buildProject();
    FxTarget t;
    t.kind = FxTarget::Event;
    t.id = ids.D;                                   // V2: [1, 4)
    const uint64_t slot = app.addVideoFx(t, "reno.test.params", "Test Parameters");
    check(slot != 0, "keyframes: the test effect on an event");
    if (!slot) return;
    app.setCursor(2.0);                             // frame 30 of the event
    w.openFx(t, slot);
    uitest::settle(300);
    FxWindow* fw = w.fxWindow();
    QToolButton* animate = fw->findChild<QToolButton*>("fxAnimate");
    check(animate && animate->isVisible() && !animate->isChecked(), "the effects window has VEGAS's Animate button (off at first)");
    if (!animate) return;
    animate->click();
    uitest::settle(200);
    QWidget* strip = findKeyStrip(fw);
    check(strip && strip->isVisible(), "Animate shows the keyframe strip under the controls");
    if (!strip) return;
    ofx::Instance* inst = app.fx.video(slot);
    ofx::Param* dbl = inst ? inst->params.find("p_dbl") : nullptr;
    ofx::Param* in = inst ? inst->params.find("p_int") : nullptr;
    check(dbl && in, "the effect's settings");
    if (!dbl || !in) return;
    // rows: the effect, its group, the settings
    const int rowH = 18, ruler = 18, labelW = 150;
    auto rowY = [&](int r) { return ruler + r * rowH + rowH / 2; };
    auto frameX = [&](double f) { return (int)std::lround(labelW + 6 + (strip->width() - labelW - 12) * (f / 90.0)); };  // the event: 3 s = 90 frames
    // Create keyframe (the All row): every setting that animates gets one at the cursor
    fw->findChild<QToolButton*>("fxKeyAdd")->click();
    uitest::settle(100);
    check(dbl->keys.count(30.0) && in->keys.count(30.0), "Create keyframe: every animatable setting gets a keyframe at the cursor (frame 30)",
          strfmt("%d %d", (int)dbl->keys.size(), (int)in->keys.size()));
    check(app.undoName() == "Add Keyframe", "one undo step (Add Keyframe)", app.undoName());
    app.undo();
    uitest::settle(100);
    check(dbl->keys.empty(), "undone");
    // find the Double Value row by its label (drawn): rows are All, [loose settings], A Group, its settings...
    // asked of the strip through a double-click at each row until p_dbl gets a key
    int dblRow = -1;
    for (int r = 1; r < 30 && dblRow < 0; ++r) {
        uitest::mouseDoubleClick(strip, QPoint(frameX(60), rowY(r)));
        uitest::settle(30);
        if (dbl->keys.count(60.0) && in->keys.empty()) dblRow = r;
        else if (app.undoName() == "Add Keyframe") app.undo();
        uitest::settle(20);
    }
    check(dblRow > 0 && dbl->keys.size() == 1 && in->keys.empty(), "a double-click on a setting's row adds a keyframe there to that setting only",
          strfmt("row %d", dblRow));
    // a second keyframe at frame 30, with a value of its own
    inst->userSetValue(dbl, std::vector<double>(1, 1.0), 30.0, true);
    inst->userSetValue(dbl, std::vector<double>(1, 5.0), 60.0, true);
    uitest::settle(100);
    QImage pic = uitest::shot(fw, gOut + "/keyframes.png");
    (void)pic;
    // drag the keyframe at 60 to 75
    uitest::mouseDrag(strip, QPoint(frameX(60), rowY(dblRow)), QPoint(frameX(75), rowY(dblRow)), 10);
    uitest::settle(100);
    check(dbl->keys.count(75.0) && !dbl->keys.count(60.0) && std::fabs(dbl->keys[75.0][0] - 5.0) < 1e-9, "dragging a keyframe moves it (with its value)",
          strfmt("%d keys", (int)dbl->keys.size()));
    check(app.undoName() == "Move Keyframe", "one undo step (Move Keyframe)", app.undoName());
    app.undo();
    uitest::settle(100);
    dbl = inst->params.find("p_dbl");
    check(dbl->keys.count(60.0) && !dbl->keys.count(75.0), "undone: back at 60");
    // interpolation from the keyframe's menu: Hold
    QStringList menuItems;
    auto strip_ = strip;
    (void)strip_;
    bool held = false;
    // the strip hands its menu to onMenu when that is set: reach it through the widget's hook
    {
        // a right-click opens the menu; the test picks "Hold" in it
        QTimer::singleShot(100, [&] {
            for (QWidget* tw : QApplication::topLevelWidgets())
                if (QMenu* m = qobject_cast<QMenu*>(tw))
                    if (m->isVisible()) {
                        for (QAction* a : m->actions()) {
                            menuItems << plain(a->text());
                            if (plain(a->text()) == "Hold") { a->trigger(); held = true; }
                        }
                        m->close();
                    }
        });
        uitest::mouseClick(strip, QPoint(frameX(30), rowY(dblRow)), Qt::RightButton);
        uitest::settle(200);
    }
    check(menuItems.join(",").startsWith("Linear,Fast,Slow,Smooth,Sharp,Hold"), "a keyframe's menu lists VEGAS's interpolation types", menuItems.join(",").toStdString());
    check(held && dbl->keyCurve(30.0) == ofx::Param::KeyHold, "Hold: the keyframe holds its value until the next");
    {
        std::lock_guard<std::recursive_mutex> lk(inst->mutex);
        check(std::fabs(dbl->valueAt(45.0)[0] - 1.0) < 1e-9, "held: half way the value is still the first keyframe's", strfmt("%.3f", dbl->valueAt(45.0)[0]));
        dbl->keyCurves[30.0] = ofx::Param::KeySmooth;
        const double v = dbl->valueAt(37.5)[0];        // a quarter of the way: smooth is 0.15625 of the change
        check(std::fabs(v - (1.0 + 4.0 * 0.15625)) < 1e-6, "Smooth eases in and out", strfmt("%.4f", v));
        dbl->keyCurves[30.0] = ofx::Param::KeyHold;
    }
    // kept in the effect's saved state
    {
        const Json state = inst->saveState();
        const Json& kc = state.get("params").get("p_dbl").get("kc");
        check(kc.size() == 1 && kc.at(0).at(1).asInt() == ofx::Param::KeyHold, "the interpolation is saved with the effect's settings", state.get("params").get("p_dbl").dump());
        inst->loadState(state);
        check(inst->params.find("p_dbl")->keyCurve(30.0) == ofx::Param::KeyHold, "and read back");
    }
    // Delete: the picked keyframe goes
    uitest::mouseClick(strip, QPoint(frameX(60), rowY(dblRow)));
    uitest::settle(50);
    uitest::key(strip, Qt::Key_Delete);
    uitest::settle(100);
    check(!inst->params.find("p_dbl")->keys.count(60.0) && inst->params.find("p_dbl")->keys.count(30.0), "Delete deletes the picked keyframe", app.undoName());
    // next / previous keyframe buttons move the cursor
    app.setCursor(1.0);
    uitest::settle(50);
    fw->findChild<QToolButton*>("fxKeyNext")->click();
    uitest::settle(50);
    check(std::fabs(app.cursor() - 2.0) < 1e-6, "Next keyframe: the cursor goes to it (frame 30 of the event = 2 s)", strfmt("%.3f", app.cursor()));
    // the strip is remembered
    animate->click();
    uitest::settle(100);
    check(!strip->isVisible(), "Animate again hides the strip");
    animate->click();
    uitest::settle(100);
}

// ====================================================================== audio effect presets
void testAudioPresets(MainWindow& w, const std::vector<std::string>& vsts)
{
    AppState& app = st();
    if (vsts.empty()) { check(false, "audio presets: no VST given (--vst)"); return; }
    buildProject();
    AudioPluginDesc desc;
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.ok && e.desc.format == "VST2") { desc = e.desc; break; }
    check(!desc.path.empty(), "audio presets: a VST2 plug-in in the catalog");
    if (desc.path.empty()) return;
    FxTarget t;
    t.kind = FxTarget::Event;
    t.id = ids.S;
    const uint64_t slot = app.addAudioFx(t, desc);
    w.openFx(t, slot);
    uitest::settle(300);
    FxWindow* fw = w.fxWindow();
    AudioPlugin* pl = app.fx.audio(slot);
    QComboBox* box = fw->findChild<QComboBox*>("fxPreset");
    QWidget* row = fw->findChild<QWidget*>("fxPresetRow");
    check(pl && box && row && row->isVisible(), "an audio effect has the Preset row too");
    if (!pl || !box) return;
    check(box->count() == std::min(pl->programCount(), 1024), "it lists the plug-in's programs", strfmt("%d listed, %d programs", box->count(), pl->programCount()));
    QToolButton* files = fw->findChild<QToolButton*>("fxPresetFile");
    check(files && files->isVisible(), "VST2: the .fxp / .fxb button");
    check(!fw->findChild<QToolButton*>("fxAnimate")->isVisible(), "no Animate button for an audio effect");
    // a user preset
    const int n = pl->paramCount();
    check(n > 0, "the plug-in has parameters", strfmt("%d", n));
    if (n <= 0) return;
    pl->paramSet(0, 0.25f);
    box->setEditText("My Sound");
    fw->findChild<QToolButton*>("fxPresetSave")->click();
    uitest::settle(100);
    const int at = box->findText("My Sound");
    check(at >= 0, "Save Preset keeps the settings as a preset in the list");
    pl->paramSet(0, 0.75f);
    if (at >= 0) {
        box->setCurrentIndex(at);
        emit box->activated(at);
        uitest::settle(100);
    }
    check(std::fabs(pl->paramGet(0) - 0.25f) < 1e-3, "choosing it puts the settings back", strfmt("%.3f", pl->paramGet(0)));
    check(app.undoName() == "Preset My Sound", "one undo step", app.undoName());
    app.undo();
    uitest::settle(50);
    check(std::fabs(app.fx.audio(slot)->paramGet(0) - 0.75f) < 1e-3, "undone", strfmt("%.3f", app.fx.audio(slot)->paramGet(0)));
    pl = app.fx.audio(slot);
    // a program of the plug-in's own
    if (pl->programCount() > 1) {
        box->setCurrentIndex(1);
        emit box->activated(1);
        uitest::settle(50);
        check(pl->currentProgram() == 1, "choosing a program sets the plug-in's program", strfmt("%d", pl->currentProgram()));
    }
    // .fxp and .fxb
    {
        const std::string fxp = pl->saveVstPresetFile(false), fxb = pl->saveVstPresetFile(true);
        check(fxp.size() > 60 && fxp.compare(0, 4, "CcnK") == 0, ".fxp: Steinberg's program file", strfmt("%d bytes", (int)fxp.size()));
        check(fxb.size() > 160 && fxb.compare(0, 4, "CcnK") == 0, ".fxb: Steinberg's bank file", strfmt("%d bytes", (int)fxb.size()));
        pl->paramSet(0, 0.4f);
        const std::string saved = pl->saveVstPresetFile(false);
        pl->paramSet(0, 0.9f);
        std::string err;
        check(pl->loadVstPresetFile(saved, &err) && std::fabs(pl->paramGet(0) - 0.4f) < 1e-3, "an .fxp read back gives the settings it saved", err);
        std::string bad = saved;
        bad[16] ^= 0x55;                        // another plug-in's id
        check(!pl->loadVstPresetFile(bad, &err) && err.find("another plug-in") != std::string::npos, "another plug-in's .fxp is refused, saying why", err);
        check(!pl->loadVstPresetFile("rubbish", &err), "a file that is not a preset is refused");
        check(!pl->loadVstPresetFile(saved.substr(0, 40), &err), "a cut-short preset is refused");
        // the bank
        pl->paramSet(0, 0.3f);
        const std::string bank = pl->saveVstPresetFile(true);
        pl->paramSet(0, 0.8f);
        check(pl->loadVstPresetFile(bank, &err) && std::fabs(pl->paramGet(0) - 0.3f) < 1e-3, "an .fxb read back gives the settings it saved", err + strfmt(" %.3f", pl->paramGet(0)));
        // through the window's file commands
        const QString path = gOut + "/test.fxp";
        QFile::remove(path);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) { f.write(saved.data(), (qint64)saved.size()); f.close(); }
    }
    // delete the user preset
    const int mine = box->findText("My Sound");
    if (mine >= 0) {
        box->setCurrentIndex(mine);
        fw->findChild<QToolButton*>("fxPresetDelete")->click();
        uitest::settle(50);
    }
    check(box->findText("My Sound") < 0, "Delete Preset deletes the user's preset");
    uitest::shot(fw, gOut + "/audio_presets.png");
}

// ====================================================================== Red Eye Reduction
void testRedEye(MainWindow& w)
{
    AppState& app = st();
    buildProject();
    std::string err;
    const uint64_t pic = app.importMedia("synthpic:5", &err);
    uint64_t ev = 0;
    app.edit("Picture", [&](Project& p) {
        Event e;
        e.id = ev = p.newId();
        e.mediaId = pic;
        e.start = 6.0;
        e.length = 2.0;
        p.findTrack(ids.v2)->events.push_back(e);
    });
    app.setSelection({ids.D});
    QAction* act = item("Tools/Video/Red Eye Reduction...");
    check(act && !act->isEnabled(), "Tools > Video > Red Eye Reduction is greyed for a video that is not a still");
    app.setSelection({ev});
    act = item("Tools/Video/Red Eye Reduction...");
    check(act && act->isEnabled(), "... and works for a still picture's event");
    if (!act) return;
    act->trigger();
    uitest::settle(200);
    RedEyeWindow* rw = dynamic_cast<RedEyeWindow*>(gW->findChild<QDialog*>("redEyeWindow"));
    check(rw && rw->isVisible() && rw->media() == pic, "its window shows the picture");
    if (!rw) return;
    RedEyeView* v = rw->view();
    rw->resize(700, 500);
    uitest::settle(100);
    // a box drawn from the middle of an "eye" outwards
    const QSize ps = v->pictureSize();
    const QPointF c = v->toView(QPointF(ps.width() * 0.4, ps.height() * 0.5));
    uitest::mouseDrag(v, c.toPoint(), (c + QPointF(30, 20)).toPoint(), 8);
    uitest::settle(50);
    const MediaItem* m = app.project().findMedia(pic);
    check(m && m->redEye.size() == 1, "a drag puts a box round an eye", m ? strfmt("%d", (int)m->redEye.size()) : "");
    if (m && m->redEye.size() == 1) {
        const MediaItem::RedEyeArea a = m->redEye[0];
        check(std::fabs(a.x + a.w / 2 - 0.4) < 0.02 && std::fabs(a.y + a.h / 2 - 0.5) < 0.02, "drawn from its middle", strfmt("%.3f %.3f %.3f %.3f", a.x, a.y, a.w, a.h));
        check(app.undoName() == "Red Eye Reduction", "one undo step", app.undoName());
        // moved
        const QPointF mid = v->toView(QPointF((a.x + a.w / 2) * ps.width(), (a.y + a.h / 2) * ps.height()));
        uitest::mouseDrag(v, mid.toPoint(), (mid + QPointF(40, 0)).toPoint(), 6);
        uitest::settle(50);
        m = app.project().findMedia(pic);
        check(m->redEye.size() == 1 && m->redEye[0].x > a.x + 0.01 && std::fabs(m->redEye[0].w - a.w) < 1e-9, "dragging a box moves it", app.undoName());
        // sized by its right edge
        const MediaItem::RedEyeArea b = m->redEye[0];
        const QPointF right = v->toView(QPointF((b.x + b.w) * ps.width(), (b.y + b.h / 2) * ps.height()));
        uitest::mouseDrag(v, right.toPoint(), (right + QPointF(25, 0)).toPoint(), 6);
        uitest::settle(50);
        m = app.project().findMedia(pic);
        check(m->redEye[0].w > b.w + 0.01 && std::fabs(m->redEye[0].x - b.x) < 1e-9, "dragging its edge sizes it", app.undoName());
    }
    uitest::shot(rw, gOut + "/redeye.png");
    // the keys: zoomed in, Home / End / C scroll
    v->setZoom(v->zoom() * 4);
    uitest::settle(30);
    uitest::key(v, Qt::Key_End);
    const double right = v->scroll().x();
    uitest::key(v, Qt::Key_Home);
    check(right > 1 && std::fabs(v->scroll().x()) < 1e-9, "End / Home: the right and left edges", strfmt("%.1f", right));
    uitest::key(v, Qt::Key_C);
    const double cx = v->scroll().x() + v->width() / v->zoom() / 2;
    check(std::fabs(cx - ps.width() / 2.0) < 1.0, "C: the middle", strfmt("%.1f", cx));
    uitest::key(v, Qt::Key_PageDown);
    check(v->scroll().y() > 1, "Page Down: the bottom");
    uitest::key(v, Qt::Key_Right);
    // the box's menu: Delete All
    m = app.project().findMedia(pic);
    v->zoomToFit();
    uitest::settle(30);
    if (m && !m->redEye.empty()) {
        QStringList items;
        v->onMenu = [&](QMenu* menu) {
            for (QAction* a : menu->actions()) items << plain(a->text());
            for (QAction* a : menu->actions()) if (plain(a->text()) == "Delete All") a->trigger();
        };
        const MediaItem::RedEyeArea a = m->redEye[0];
        uitest::mouseClick(v, v->toView(QPointF((a.x + a.w / 2) * ps.width(), (a.y + a.h / 2) * ps.height())).toPoint(), Qt::RightButton);
        uitest::settle(50);
        check(items.join(",") == "Delete,Delete All", "a box's menu is VEGAS's (Delete, Delete All)", items.join(",").toStdString());
        check(app.project().findMedia(pic)->redEye.empty(), "Delete All");
        app.undo();
        check(app.project().findMedia(pic)->redEye.size() == 1, "undone");
    }
    // saved with the project
    {
        Project q;
        q.fromJson(app.project().toJson(), &err);
        const MediaItem* qm = q.findMedia(pic);
        check(qm && qm->redEye.size() == 1, "the areas are saved with the project and read back");
    }
    rw->hide();
}

// ====================================================================== the Plug-In Chooser's folders
void testChooserFolders()
{
    setPackageRoot((gOut + "/packages").toStdString());
    QDir(gOut + "/packages").removeRecursively();
    PluginChooser c(ChooserKind::VideoFx, "Plug-In Chooser - Video Event FX", FxChain());
    c.show();
    uitest::settle(100);
    // Create New Folder from the folder tree's right-click menu
    PluginChooser::menuForTests = [](QMenu* m) -> QAction* {
        for (QAction* a : m->actions()) if (plain(a->text()) == "Create New Folder") return a;
        return nullptr;
    };
    PluginChooser::folderNameForTests = [](const QString& suggested) { return suggested == "New Folder" ? QString("Favourites") : suggested; };
    emit c.folderTree()->customContextMenuRequested(QPoint(5, 5));
    uitest::settle(50);
    check(c.folders().contains("Favourites"), "Create New Folder makes a folder of the user's own", c.folders().join(",").toStdString());
    // put a plug-in in it (Add to Folder)
    c.openFolder("All");
    const QStringList all = c.listed();
    check(!all.isEmpty(), "plug-ins listed");
    if (all.isEmpty()) return;
    const QString first = all[0];
    check(c.addToFolder(first, "Favourites"), "Add to Folder");
    c.openFolder("Favourites");
    // the test catalog can list a plug-in more than once: a folder shows every entry of what was put in it
    auto distinct = [](QStringList l) { l.removeDuplicates(); return l; };
    check(distinct(c.listed()) == QStringList{first}, "the folder lists what was put in it", c.listed().join(",").toStdString());
    // the list's menu: Add to Folder > Favourites
    c.openFolder("All");
    int other = -1;
    for (int k = 0; k < all.size(); ++k) if (all[k] != first) { other = k; break; }
    if (other > 0) {
        PluginChooser::menuForTests = [](QMenu* m) -> QAction* {
            for (QAction* a : m->actions())
                if (a->menu() && plain(a->text()) == "Add to Folder")
                    for (QAction* b : a->menu()->actions()) if (plain(b->text()) == "Favourites") return b;
            return nullptr;
        };
        QListWidget* list = c.choiceList();
        // the folder's own folders come first in the list, as VEGAS lists them: the plug-in is found by its name
        for (int k = 0; k < list->count(); ++k)
            if (list->item(k)->text() == all[other] && list->item(k)->data(Qt::UserRole + 1).toInt() >= 0) {
                emit list->customContextMenuRequested(list->visualItemRect(list->item(k)).center());
                break;
            }
        uitest::settle(30);
        c.openFolder("Favourites");
        check(distinct(c.listed()).size() == 2, "the list's Add to Folder menu puts one in", c.listed().join(",").toStdString());
    }
    // Rename (from the folder's menu)
    PluginChooser::menuForTests = [](QMenu* m) -> QAction* {
        for (QAction* a : m->actions()) if (plain(a->text()) == "Rename") return a->isEnabled() ? a : nullptr;
        return nullptr;
    };
    PluginChooser::folderNameForTests = [](const QString&) { return QString("Best"); };
    {
        QTreeWidget* tree = c.folderTree();
        QTreeWidgetItem* fav = nullptr;
        QTreeWidgetItemIterator it(tree);
        for (; *it; ++it) if ((*it)->text(0) == "Favourites") fav = *it;
        if (fav) emit tree->customContextMenuRequested(tree->visualItemRect(fav).center());
        uitest::settle(30);
    }
    check(c.folders().contains("Best") && !c.folders().contains("Favourites"), "Rename renames the folder", c.folders().join(",").toStdString());
    c.openFolder("Best");
    check(c.listed().size() >= 1, "and it keeps what was in it");
    check(!c.renameFolder("Best", "Filter Packages") && !c.createFolder("VEGAS") && c.folders().contains("Best"), "a folder cannot take the name of one of Reno's own");
    uitest::shot(&c, gOut + "/chooser_folders.png");
    // packages: rename one
    c.openFolder("All");
    c.addByLabel(first);
    std::string err;
    check(c.savePackageAs("Pack One", &err), "a package saved", err);
    c.openFolder("Filter Packages");
    check(c.listed().contains("Pack One"), "listed in the packages folder");
    PluginChooser::menuForTests = [](QMenu* m) -> QAction* {
        for (QAction* a : m->actions()) if (plain(a->text()) == "Rename Package") return a;
        return nullptr;
    };
    PluginChooser::folderNameForTests = [](const QString&) { return QString("Pack Two"); };
    {
        QListWidget* list = c.choiceList();
        for (int k = 0; k < list->count(); ++k)
            if (list->item(k)->text() == "Pack One") emit list->customContextMenuRequested(list->visualItemRect(list->item(k)).center());
        uitest::settle(30);
    }
    c.openFolder("Filter Packages");
    check(c.listed().contains("Pack Two") && !c.listed().contains("Pack One"), "Rename Package renames it", c.listed().join(",").toStdString());
    // Delete the folder
    check(c.deleteFolder("Best") && !c.folders().contains("Best"), "a folder of the user's deleted");
    // kept between runs: a new chooser sees the folders
    c.createFolder("Kept");
    PluginChooser c2(ChooserKind::VideoFx, "again", FxChain());
    check(c2.folders().contains("Kept"), "the folders are kept (a new chooser has them)");
    PluginChooser c3(ChooserKind::AudioFx, "audio", FxChain());
    check(!c3.folders().contains("Kept"), "each kind of chooser has its own folders");
    PluginChooser::menuForTests = nullptr;
    PluginChooser::folderNameForTests = nullptr;
    c.reject();
}


// ====================================================================== View > Event Length / Show Marker Labels, Create Subclip
void testViewAndSubclip(TimelineWidget* tl)
{
    AppState& app = st();
    buildProject();
    QAction* len = item("View/Event Length");
    check(len && len->isEnabled() && len->isCheckable() && !len->isChecked(), "View > Event Length works (off at first)");
    if (len) len->trigger();
    check(tl->options().showEventLength, "and turns the lengths on");
    QAction* labels = item("View/Event Media Markers/Show Marker Labels");
    check(labels && labels->isEnabled() && labels->isChecked(), "View > Event Media Markers > Show Marker Labels works (on, as in VEGAS)");
    if (labels) labels->trigger();
    check(!tl->options().showMarkerLabels, "and turns the names off");
    if (labels) labels->trigger();
    uitest::shot(tl, gOut + "/event_length.png");
    if (len) len->trigger();
    // Options > Grid Spacing
    {
        QAction* g = item("Options/Grid Spacing/Seconds");
        check(g && g->isEnabled() && item("Options/Grid Spacing/Ruler Marks")->isChecked(), "Options > Grid Spacing works (Ruler Marks at first)");
        if (g) g->trigger();
        check(tl->options().gridSpacing == "seconds" && item("Options/Grid Spacing/Seconds")->isChecked(), "Grid Spacing > Seconds");
        app.setCursor(2.3);
        uitest::key(tl, Qt::Key_PageDown);
        check(std::fabs(app.cursor() - 3.0) < 1e-6, "Page Down goes to the next grid mark (a whole second)", strfmt("%.3f", app.cursor()));
        item("Options/Grid Spacing/Quarter Notes")->trigger();
        app.setCursor(0.1);
        uitest::key(tl, Qt::Key_PageDown);
        check(std::fabs(app.cursor() - 0.5) < 1e-6, "Quarter Notes at 120 bpm: half a second", strfmt("%.3f", app.cursor()));
        item("Options/Grid Spacing/Measures")->trigger();
        app.setCursor(0.1);
        uitest::key(tl, Qt::Key_PageDown);
        check(std::fabs(app.cursor() - 2.0) < 1e-6, "Measures (4/4 at 120 bpm): two seconds", strfmt("%.3f", app.cursor()));
        uitest::shot(tl, gOut + "/grid_measures.png");
        item("Options/Grid Spacing/Ruler Marks")->trigger();
        check(tl->options().gridSpacing == "ruler", "back to the ruler marks");
    }
    // Create Subclip from the event's menu
    app.setSelection({ids.C});
    QMenu m;
    tl->buildContextMenu(&m, body(tl, ids.v1, 4.0));
    QAction* sub = nullptr;
    for (QAction* a : m.actions()) if (plain(a->text()) == "Create Subclip") sub = a;
    check(sub && sub->isEnabled(), "an event's menu has Create Subclip");
    if (!sub) return;
    QTimer::singleShot(150, [] {
        for (QWidget* w : QApplication::topLevelWidgets())
            if (QDialog* d = qobject_cast<QDialog*>(w))
                if (d->isVisible()) {
                    if (QLineEdit* e = d->findChild<QLineEdit*>()) e->setText("Middle Bit");
                    d->accept();
                }
    });
    const size_t before = app.project().media.size();
    sub->trigger();
    uitest::settle(100);
    const Project& p = app.project();
    const MediaItem* made = p.media.size() == before + 1 ? &p.media.back() : nullptr;
    check(made && made->name == "Middle Bit" && std::fabs(made->subclipStart - 6.0) < 1e-9 && std::fabs(made->subclipLength - 2.0) < 1e-9,
          "Create Subclip: the part the event plays is a subclip in Project Media", made ? strfmt("%s %.2f %.2f", made->name.c_str(), made->subclipStart, made->subclipLength) : "none");
    check(made && E(ids.C)->mediaId == made->id && std::fabs(E(ids.C)->offset - 6.0) < 1e-9, "the event plays the subclip now (the same part of the file)");
    check(made && E(ids.S)->mediaId == ids.snd, "an event of another file grouped with it keeps its own media");
    check(app.undoName() == "Create Subclip", "one undo step", app.undoName());
    app.undo();
    check(app.project().media.size() == before && E(ids.C)->mediaId == ids.clip, "undone");
}


// ====================================================================== the right-drag menu
void rightDrag(TimelineWidget* tl, QPoint from, QPoint to)
{
    uitest::mousePress(tl, from, Qt::RightButton);
    for (int k = 1; k <= 8; ++k) uitest::mouseMove(tl, from + (to - from) * k / 8, Qt::RightButton);
    uitest::mouseRelease(tl, to, Qt::RightButton);
    uitest::settle(50);
}

void testRightDrag(TimelineWidget* tl)
{
    AppState& app = st();
    buildProject();
    setTool("toolNormal");
    const double pps = pxPerSecond(tl);
    QString choose;
    QStringList seen;
    tl->rightDragMenu = [&](QMenu* m) -> QAction* {
        seen.clear();
        QAction* got = nullptr;
        for (QAction* a : m->actions()) {
            seen << (a->isSeparator() ? QString("-") : plain(a->text()));
            if (plain(a->text()) == choose) got = a;
        }
        return got;
    };
    const QPoint c = body(tl, ids.v1, 4.0), c2 = c + QPoint((int)std::lround(pps * 2.0), 0);
    // Copy Here
    choose = "Copy Here";
    rightDrag(tl, c, c2);
    check(seen.join(",") == "Move Here,Copy Here,Shuffle Events,-,Cancel", "a right-drag ends with VEGAS's menu", seen.join(",").toStdString());
    const Track* v1 = T(ids.v1);
    check(near(E(ids.C)->start, 3.0) && v1->events.size() == 4 && near(v1->events.back().start, 5.0), "Copy Here: a copy where it was dropped, the event stays",
          strfmt("%d events, last at %.2f", (int)v1->events.size(), v1->events.back().start));
    const Track* a1 = T(ids.a1);
    check(a1->events.size() == 2 && a1->events.back().group != 0 && a1->events.back().group == T(ids.v1)->events.back().group && a1->events.back().group != E(ids.C)->group,
          "the event grouped with it is copied too, the copies grouped together");
    check(app.undoName() == "Copy Events", "one undo step (Copy Events)", app.undoName());
    app.undo();
    // Move Here
    choose = "Move Here";
    rightDrag(tl, c, c2);
    check(near(E(ids.C)->start, 5.0) && near(E(ids.S)->start, 5.0) && T(ids.v1)->events.size() == 3, "Move Here: moved (with its group)", strfmt("%.2f", E(ids.C)->start));
    app.undo();
    // Cancel
    choose = "Cancel";
    rightDrag(tl, c, c2);
    check(near(E(ids.C)->start, 3.0) && T(ids.v1)->events.size() == 3, "Cancel: nothing changes");
    // Shuffle Events: A dropped just past B trades places with it
    choose = "Shuffle Events";
    rightDrag(tl, body(tl, ids.v1, 1.0), body(tl, ids.v1, 1.0) + QPoint((int)std::lround(pps * 1.4), 0));
    check(near(E(ids.B)->start, 0.0) && near(E(ids.A)->start, 1.0) && near(E(ids.C)->start, 3.0), "Shuffle Events: the event changes places with the one it was dropped past",
          strfmt("A %.2f B %.2f C %.2f", E(ids.A)->start, E(ids.B)->start, E(ids.C)->start));
    check(app.undoName() == "Shuffle Events", "one undo step (Shuffle Events)", app.undoName());
    app.undo();
    tl->rightDragMenu = nullptr;
}


// ====================================================================== Insert > Add Missing Streams for Selected Event
void testMissingStream(TimelineWidget* tl)
{
    AppState& app = st();
    buildProject();
    // A on V1 plays only the picture of synth:1 (which has sound too)
    app.setSelection({ids.A});
    QAction* a = item("Insert/Add Missing Streams for Selected Event");
    check(a && a->isEnabled(), "Insert > Add Missing Streams for Selected Event");
    if (!a) return;
    const size_t tracks = app.project().tracks.size();
    a->trigger();
    uitest::settle(30);
    const Event* va = E(ids.A);
    const Event* found = nullptr;
    const Track* on = nullptr;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Audio)
            for (const Event& e : t.events)
                if (e.mediaId == ids.clip && va->group && e.group == va->group) { found = &e; on = &t; }
    check(found && near(found->start, 0.0) && near(found->length, 2.0) && near(found->offset, 0.0), "the sound of the file added under it, at the same place",
          found ? strfmt("%.2f %.2f", found->start, found->length) : "none");
    check(found && va->group != 0, "grouped with the picture's event");
    check(on && on->id == ids.a1 && app.project().tracks.size() == tracks, "on the audio track that had room there");
    check(app.undoName() == "Add Missing Stream", "one undo step", app.undoName());
    // again: nothing more (it is there now)
    const size_t evs = T(ids.a1)->events.size();
    a->trigger();
    check(T(ids.a1)->events.size() == evs, "not added twice");
    // B's sound: A1 is free at 2..3 (S starts at 3); C's span (3..5) is taken by S: a new audio track
    app.setSelection({ids.C});
    E(ids.C);
    app.edit("Ungroup C", [](Project& p) { if (Event* e = p.findEvent(ids.C)) e->group = 0; });
    item("Insert/Add Missing Streams for Selected Event")->trigger();
    check(app.project().tracks.size() == tracks + 1 && app.project().tracks.back().type == TrackType::Audio, "where the audio tracks have no room: a new audio track",
          strfmt("%d tracks", (int)app.project().tracks.size()));
    (void)tl;
}


// ====================================================================== Tools > Audio > Open Copy in Audio Editor
void testAudioEditorCopy()
{
    AppState& app = st();
    buildProject();
    const QString wav = gOut + "/sound.wav";
    QFile::remove(wav);
    QFile::remove(gOut + "/sound (Copy).wav");
    if (!QFile::copy("/usr/lib/libreoffice/share/gallery/sounds/space2.wav", wav)) { check(false, "Open Copy in Audio Editor: no test sound file"); return; }
    std::string err;
    const uint64_t m = app.importMedia(wav.toStdString(), &err);
    check(m != 0, "a sound file imported", err);
    if (!m) return;
    uint64_t ev = 0;
    app.edit("Sound", [&](Project& p) {
        Event e;
        e.id = ev = p.newId();
        e.mediaId = m;
        e.start = 6.0;
        e.length = 0.5;
        p.findTrack(ids.a1)->events.push_back(e);
    });
    app.prefs.audioEditor = "/bin/true";  // an "audio editor" that starts and ends
    app.setSelection({ev});
    QAction* a = item("Tools/Audio/Open Copy in Audio Editor");
    check(a && a->isEnabled(), "Tools > Audio > Open Copy in Audio Editor");
    if (!a) return;
    a->trigger();
    uitest::settle(100);
    const Event* e = E(ev);
    const MediaItem* now = e ? app.project().findMedia(e->mediaId) : nullptr;
    check(QFileInfo::exists(gOut + "/sound (Copy).wav"), "the file is copied beside it");
    check(now && now->id != m && QString::fromStdString(now->path).endsWith("sound (Copy).wav"), "the event plays the copy", now ? now->path : "");
    check(app.project().findMedia(m) != nullptr, "the original stays in the project");
    check(item("Tools/Audio/Edit Source Project") != nullptr, "Tools > Audio > Edit Source Project is there");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTools2Test");
    QApplication::setApplicationName("tools2_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("tools2");
    QDir().mkpath(gOut);
    std::vector<std::string> ofx, vst;
    for (int i = 2; i < args.size(); ++i) {
        if (args[i] == "--vst" && i + 1 < args.size()) vst.push_back(args[++i].toStdString());
        else ofx.push_back(args[i].toStdString());
    }
    uitest::setup(ofx, vst);

    buildProject();
    MainWindow w;
    gW = &w;
    w.resize(1500, 900);
    w.show();
    uitest::settle(300);
    buildProject();
    TimelineWidget* tl = w.timeline();
    tl->zoomToFit();
    uitest::settle(100);

    setUserPresetDir((gOut + "/user presets").toStdString());
    setAudioPresetDir((gOut + "/audio presets").toStdString());
    QDir(gOut + "/audio presets").removeRecursively();
    testEditingTools(tl);
    testResample(tl);
    testKeyframes(w);
    testAudioPresets(w, vst);
    testRedEye(w);
    testChooserFolders();
    testViewAndSubclip(tl);
    testRightDrag(tl);
    testMissingStream(tl);
    testAudioEditorCopy();
    return uitest::summary();
}
