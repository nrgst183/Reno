// Editing windows (src/app/editdialogs.cpp): Event Pan/Crop, Track Motion, Event Properties, Project Properties, Preferences,
// and what AppState does for them (preferences, auto-save, undo limit). Pictures of each window go to the output folder.
//
//   edit_dialogs_test <output folder> <testfx.ofx> [more .ofx ...]
//
// Set HOME to a scratch folder (preferences and the plug-in list are written there). Prints PASS / FAIL lines and a summary.
#include "../../src/app/editdialogs.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

std::string num(double v) { return QString::number(v, 'g', 6).toStdString(); }
bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

QWidget* topLevel(const char* name)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->objectName() == name && w->isVisible()) return w;
    return nullptr;
}
int countTopLevel(const char* name)
{
    int n = 0;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->objectName() == name && w->isVisible()) ++n;
    return n;
}

template <class T>
T* child(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

// A point in the box view's rectangle, as fractions of it.
QPoint at(QWidget* view, double fx, double fy)
{
    const QRectF a = view->property("area").toRectF();
    return QPoint((int)std::lround(a.left() + fx * a.width()), (int)std::lround(a.top() + fy * a.height()));
}

void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    uitest::mousePress(w, from, Qt::LeftButton, mods);
    for (int k = 1; k <= 8; ++k) uitest::mouseMove(w, from + (to - from) * k / 8, Qt::LeftButton, mods);
    uitest::mouseRelease(w, to, Qt::LeftButton, mods);
    uitest::settle(30);
}

const Event* ev(uint64_t id) { return AppState::get().project().findEvent(id); }
const Track* track(uint64_t id) { return const_cast<Project&>(AppState::get().project()).findTrack(id); }

void pick(QComboBox* c, const QString& data)
{
    const int i = c->findData(data);
    c->setCurrentIndex(i);
}

// Runs 'fn' on the modal dialog that the next exec() puts up.
void whenModal(std::function<void(QWidget*)> fn)
{
    QTimer::singleShot(150, [fn] {
        QWidget* d = QApplication::activeModalWidget();
        fn(d);
        if (d && d->isVisible()) d->close();  // don't leave it hanging if the test did not close it
    });
}

void clickOk(QWidget* d)
{
    if (QPushButton* ok = child<QPushButton>(d, "ok")) ok->click();
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoEditTest");
    QApplication::setApplicationName("edit_dialogs_test");
    QSettings().clear();  // default preferences
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) { qWarning("usage: edit_dialogs_test <output folder> <testfx.ofx> [...]"); return 2; }
    gOut = args[1];
    QDir().mkpath(gOut);
    std::vector<std::string> ofxFiles;
    for (int k = 2; k < args.size(); ++k) ofxFiles.push_back(args[k].toStdString());
    uitest::setup(ofxFiles, {});

    AppState& app = AppState::get();
    app.newProject();
    app.edit("Small Project", [](Project& p) { p.settings.width = 640; p.settings.height = 360; });
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    uint64_t V = 0, A = 0, VT = 0;
    for (uint64_t id : placed) {
        const Track* t = nullptr;
        app.project().findEvent(id, &t);
        if (t && t->type == TrackType::Video) { V = id; VT = t->id; }
        else A = id;
    }
    check(V && A && VT, "setup: a clip with picture and sound on the timeline");
    app.setCursor(0.0);
    Playback playback;
    const QImage plain = playback.renderNow(2.0);

    // ================================================================== Event Pan/Crop
    openPanCrop(nullptr, &playback, V);
    uitest::settle();
    QPointer<QWidget> pc = topLevel("panCropWindow");
    check(pc != nullptr, "pan/crop: the window opens");
    if (!pc) return uitest::summary();
    pc->resize(1000, 660);
    uitest::settle();
    check(pc->windowTitle().startsWith("Event Pan/Crop"), "pan/crop: titled like VEGAS's", pc->windowTitle().toStdString());
    QWidget* view = child<QWidget>(pc, "boxView");
    check(view && view->property("area").toRectF().width() > 100, "pan/crop: a workspace with the media's picture");
    openPanCrop(nullptr, &playback, V);
    uitest::settle();
    check(countTopLevel("panCropWindow") == 1, "pan/crop: opening it again raises the same window");

    // moving: drag inside the box
    drag(view, at(view, 0.5, 0.5), at(view, 0.75, 0.5));
    {
        const Event* e = ev(V);
        check(e && e->panCrop.size() == 1 && near(e->panCrop[0].cx, 0.75, 0.01) && near(e->panCrop[0].cy, 0.5, 0.01) && near(e->panCrop[0].w, 1.0),
              "pan/crop: dragging inside the box moves it (one keyframe for the whole event)", e && !e->panCrop.empty() ? num(e->panCrop[0].cx) : "no keys");
        check(app.undoName() == "Event Pan/Crop", "pan/crop: the drag is one undo step", app.undoName());
    }
    app.undo();
    uitest::settle();
    check(ev(V)->panCrop.empty(), "pan/crop: undo takes the drag back");
    // sizing: drag the bottom right corner half way to the middle (keeps the shape, about the centre)
    drag(view, at(view, 1.0, 1.0), at(view, 0.75, 0.75));
    {
        const PanCropKey k = ev(V)->panCrop.empty() ? PanCropKey() : ev(V)->panCrop[0];
        check(near(k.w, 0.5, 0.01) && near(k.h, 0.5, 0.01) && near(k.cx, 0.5, 0.01) && near(k.cy, 0.5, 0.01),
              "pan/crop: dragging a corner sizes the box about its centre", num(k.w) + " x " + num(k.h));
    }
    // sizing one edge without the aspect lock (Shift inverts the lock)
    drag(view, at(view, 0.75, 0.5), at(view, 0.85, 0.5), Qt::ShiftModifier);
    {
        const PanCropKey k = ev(V)->panCrop[0];
        check(near(k.w, 0.7, 0.012) && near(k.h, 0.5, 0.01), "pan/crop: Shift on an edge changes just that way", num(k.w) + " x " + num(k.h));
    }
    app.undo();
    uitest::settle();
    // turning: drag just outside a corner round by 45 degrees
    {
        const QRectF a = view->property("area").toRectF();
        const QPointF c = a.center();
        const QPointF corner(a.left() + 0.75 * a.width(), a.top() + 0.75 * a.height());
        const QPointF d = corner - c;
        const double r = std::hypot(d.x(), d.y()) + 12;
        const double a0 = std::atan2(d.y(), d.x());
        const QPoint from((int)std::lround(c.x() + r * std::cos(a0)), (int)std::lround(c.y() + r * std::sin(a0)));
        uitest::mousePress(view, from);
        for (int k = 1; k <= 10; ++k) {
            const double ang = a0 + (3.14159265358979 / 4) * k / 10;
            uitest::mouseMove(view, QPoint((int)std::lround(c.x() + r * std::cos(ang)), (int)std::lround(c.y() + r * std::sin(ang))));
        }
        const double ang = a0 + 3.14159265358979 / 4;
        uitest::mouseRelease(view, QPoint((int)std::lround(c.x() + r * std::cos(ang)), (int)std::lround(c.y() + r * std::sin(ang))));
        uitest::settle(30);
        const PanCropKey k = ev(V)->panCrop[0];
        check(near(k.rotation, 45, 1.5) && near(k.w, 0.5, 0.01), "pan/crop: dragging outside a corner turns the box (clockwise is more)", num(k.rotation));
    }
    // the numbers
    {
        QDoubleSpinBox* wBox = child<QDoubleSpinBox>(pc, "boxWidth");
        QDoubleSpinBox* angle = child<QDoubleSpinBox>(pc, "boxAngle");
        check(wBox && near(wBox->value(), 160, 2) && angle && near(angle->value(), 45, 1.5), "pan/crop: the side panel shows the box in the media's pixels",
              wBox ? num(wBox->value()) : "");
        if (wBox && angle) {
            wBox->setValue(240);
            angle->setValue(0);
            uitest::settle();
            const PanCropKey k = ev(V)->panCrop[0];
            check(near(k.w, 0.75, 1e-6) && near(k.rotation, 0, 1e-9), "pan/crop: typing numbers sets the box", num(k.w) + " " + num(k.rotation));
        }
    }
    // presets
    if (QComboBox* presets = child<QComboBox>(pc, "panCropPreset")) {
        const int zoom = presets->findText("Zoom 2x");
        const int flip = presets->findText("Flip Horizontal");
        check(zoom > 0 && flip > 0 && presets->findText("Fit in 4:3 (letterbox)") > 0, "pan/crop: presets as in VEGAS (fits, zoom, flips)");
        presets->activated(zoom);
        uitest::settle();
        presets->activated(flip);
        uitest::settle();
        const PanCropKey k = ev(V)->panCrop[0];
        check(near(k.w, -0.5) && near(k.h, 0.5) && presets->currentIndex() == 0, "pan/crop: Zoom 2x then Flip Horizontal", num(k.w) + " x " + num(k.h));
        check(app.undoName() == "Pan/Crop Preset: Flip Horizontal", "pan/crop: each preset is an undo step", app.undoName());
        const int box43 = presets->findText("Fit in 4:3 (letterbox)");
        presets->activated(box43);
        uitest::settle();
        const PanCropKey f = ev(V)->panCrop[0];
        check(near(f.w, 1.0) && near(f.h, (16.0 / 9.0) / (4.0 / 3.0), 1e-6), "pan/crop: fitting a 16:9 picture in 4:3 makes the box taller than the picture", num(f.h));
        app.undo();
        uitest::settle();
    }
    // the preview shows it
    {
        const QImage cropped = playback.renderNow(2.0);
        check(!cropped.isNull() && cropped != plain, "pan/crop: the picture the preview shows changes");
    }
    // keyframes: one at 1 s (the window's time follows the timeline cursor)
    QToolButton* addKey = child<QToolButton>(pc, "addKey");
    QWidget* strip = child<QWidget>(pc, "keyStrip");
    app.setCursor(1.0);
    uitest::settle();
    if (addKey) addKey->click();
    uitest::settle();
    check(ev(V)->panCrop.size() == 2 && near(ev(V)->panCrop[1].time, 1.0), "pan/crop: Add Keyframe adds one at the cursor", std::to_string(ev(V)->panCrop.size()));
    drag(view, at(view, 0.5, 0.5), at(view, 0.6, 0.5));
    {
        const auto& ks = ev(V)->panCrop;
        check(ks.size() == 2 && near(ks[0].cx, 0.5, 1e-6) && near(ks[1].cx, 0.6, 0.01), "pan/crop: with two keyframes a drag changes the one at the cursor",
              ks.size() == 2 ? num(ks[0].cx) + " / " + num(ks[1].cx) : "");
    }
    if (QComboBox* curve = child<QComboBox>(pc, "keyCurve")) {
        check(curve->isEnabled(), "pan/crop: the keyframe at the cursor's interpolation can be chosen");
        curve->setCurrentIndex(curve->findData("2"));
        uitest::settle();
        check(ev(V)->panCrop[1].curve == 2, "pan/crop: ... smooth", std::to_string(ev(V)->panCrop[1].curve));
    }
    // the strip: click at its start moves the cursor; arrows go keyframe to keyframe
    if (strip) {
        const double span = strip->property("span").toDouble();
        check(near(span, ev(V)->length), "pan/crop: the strip is as long as the event", num(span));
        uitest::mouseClick(strip, QPoint(10, 30));
        uitest::settle();
        check(near(app.cursor(), 0.0, 1e-6), "pan/crop: clicking the strip moves the cursor (and the timeline's with it)", num(app.cursor()));
        if (QToolButton* next = child<QToolButton>(pc, "nextKey")) next->click();
        uitest::settle();
        check(near(app.cursor(), 1.0, 1e-6), "pan/crop: next keyframe", num(app.cursor()));
        // drag the keyframe at 1 s to 2 s
        const int x1 = 10 + (int)std::lround(1.0 / span * (strip->width() - 20)), x2 = 10 + (int)std::lround(2.0 / span * (strip->width() - 20));
        drag(strip, QPoint(x1, 36), QPoint(x2, 36));
        check(ev(V)->panCrop.size() == 2 && near(ev(V)->panCrop[1].time, 2.0, 0.05) && app.undoName() == "Move Keyframe", "pan/crop: dragging a keyframe moves it in time",
              num(ev(V)->panCrop[1].time));
    }
    {
        // between the two keys the box is in between
        app.setCursor(1.0);
        uitest::settle();
        QDoubleSpinBox* x = child<QDoubleSpinBox>(pc, "boxX");
        check(x && x->value() > 160 && x->value() < 192, "pan/crop: between keyframes the panel shows the box in between", x ? num(x->value()) : "");
        if (QToolButton* del = child<QToolButton>(pc, "deleteKey")) check(!del->isEnabled(), "pan/crop: nothing to delete where there is no keyframe");
    }
    uitest::shot(pc, gOut + "/pancrop.png");
    // turned and zoomed, for the picture
    {
        app.setCursor(0.0);
        uitest::settle();
        if (QDoubleSpinBox* angle = child<QDoubleSpinBox>(pc, "boxAngle")) angle->setValue(20);
        uitest::settle();
        uitest::shot(pc, gOut + "/pancrop_turned.png");
    }
    // all of it undoes
    while (app.canUndo() && app.undoName() != "Add to Timeline") app.undo();
    uitest::settle();
    check(ev(V) && ev(V)->panCrop.empty(), "pan/crop: undoing every step leaves the event as it was");
    while (app.canRedo()) app.redo();
    uitest::settle();
    check(ev(V) && ev(V)->panCrop.size() == 2 && near(ev(V)->panCrop[0].rotation, 20), "pan/crop: and redoing them all brings it all back");
    // deleting the event closes the window
    app.selectOnly(V);
    app.deleteSelection();
    uitest::settle(200);
    check(pc.isNull() || !pc->isVisible(), "pan/crop: deleting the event closes its window");
    app.undo();
    uitest::settle();

    // ================================================================== Track Motion
    openTrackMotion(nullptr, &playback, VT);
    uitest::settle();
    QPointer<QWidget> tm = topLevel("trackMotionWindow");
    check(tm != nullptr, "track motion: the window opens");
    if (tm) {
        tm->resize(1000, 660);
        uitest::settle();
        QWidget* tv = child<QWidget>(tm, "boxView");
        drag(tv, at(tv, 0.5, 0.5), at(tv, 0.25, 0.25));
        const Track* t = track(VT);
        check(t && t->motion.size() == 1 && near(t->motion[0].x, 0.25, 0.01) && near(t->motion[0].y, 0.25, 0.01),
              "track motion: dragging the box moves the track's picture", t && !t->motion.empty() ? num(t->motion[0].x) + ", " + num(t->motion[0].y) : "");
        drag(tv, at(tv, 0.75, 0.75), at(tv, 0.5, 0.5));
        t = track(VT);
        check(t && near(t->motion[0].w, 0.5, 0.01) && near(t->motion[0].h, 0.5, 0.01), "track motion: dragging a corner sizes it", num(t->motion[0].w));
        const QImage moved = playback.renderNow(2.0);
        check(moved != plain, "track motion: the preview shows the track moved");
        QComboBox* mode = child<QComboBox>(tm, "compositeMode");
        check(mode && mode->count() == 15 && mode->currentData().toString() == "srcalpha", "track motion: the compositing modes (3D Source Alpha too), Source Alpha chosen");
        if (mode) {
            pick(mode, "multiply");
            uitest::settle();
            check(track(VT)->composite == "multiply" && app.undoName() == "Compositing Mode", "track motion: choosing a compositing mode sets it (one step)");
            uitest::shot(tm, gOut + "/trackmotion.png");
            app.undo();
            uitest::settle();
            check(track(VT)->composite == "srcalpha" && mode->currentData().toString() == "srcalpha", "track motion: undo puts the mode back, and the window shows it");
        }
        app.undo();
        app.undo();
        uitest::settle();
        check(track(VT)->motion.empty(), "track motion: undo takes the moves back");

        // ---- the window's keys: X / Y prevent movement, Shift+X / Shift+Y prevent scaling, F8 snapping, Shift+F8 rotation, A lock aspect, C scale about centre, O object space
        auto btn = [&](const char* name) { return child<QToolButton>(tm, name); };
        auto press = [&](int k, Qt::KeyboardModifiers m = Qt::NoModifier) {
            uitest::key(tm, k, m);
            uitest::settle(20);
        };
        const bool before = btn("preventMoveX")->isChecked() || btn("preventMoveY")->isChecked() || btn("enableSnapping")->isChecked() ||
                            btn("objectSpace")->isChecked() || btn("preventScaleX")->isChecked();
        press(Qt::Key_X);
        check(!before && btn("preventMoveX")->isChecked(), "track motion keys: X turns Prevent Movement X on");
        drag(tv, at(tv, 0.5, 0.5), at(tv, 0.7, 0.6));
        t = track(VT);
        check(t && t->motion.size() == 1 && near(t->motion[0].x, 0.5, 1e-9) && near(t->motion[0].y, 0.6, 0.01), "track motion keys: ... the box then moves only up and down",
              t && !t->motion.empty() ? num(t->motion[0].x) + ", " + num(t->motion[0].y) : "");
        press(Qt::Key_X);
        press(Qt::Key_Y);
        const double y0 = t->motion[0].y;
        drag(tv, at(tv, 0.5, 0.6), at(tv, 0.6, 0.4));
        t = track(VT);
        check(btn("preventMoveY")->isChecked() && near(t->motion[0].y, y0, 1e-9) && near(t->motion[0].x, 0.6, 0.01), "track motion keys: Y keeps it from moving up and down",
              num(t->motion[0].x) + ", " + num(t->motion[0].y));
        press(Qt::Key_Y);
        app.undo();
        app.undo();
        uitest::settle();
        // Shift+X: the width stays when a corner is dragged
        press(Qt::Key_X, Qt::ShiftModifier);
        drag(tv, at(tv, 1.0, 1.0), at(tv, 0.8, 0.8));
        t = track(VT);
        check(btn("preventScaleX")->isChecked() && t->motion.size() == 1 && near(t->motion[0].w, 1.0, 1e-9) && near(t->motion[0].h, 0.6, 0.02),
              "track motion keys: Shift+X keeps the width", t->motion.empty() ? "" : num(t->motion[0].w) + " x " + num(t->motion[0].h));
        press(Qt::Key_X, Qt::ShiftModifier);
        app.undo();
        uitest::settle();
        // F8: snap to sixteenths of a frame
        press(Qt::Key_F8);
        check(btn("enableSnapping")->isChecked(), "track motion keys: F8 turns snapping on");
        drag(tv, at(tv, 0.5, 0.5), at(tv, 0.5 + 0.0625 + 0.006, 0.5));
        t = track(VT);
        check(t->motion.size() == 1 && near(t->motion[0].x, 0.5625, 1e-9), "track motion keys: ... and the box snaps to the grid", t->motion.empty() ? "" : num(t->motion[0].x));
        uitest::shot(tm, gOut + "/trackmotion_snapping.png");
        press(Qt::Key_F8);
        app.undo();
        uitest::settle();
        press(Qt::Key_F8, Qt::ShiftModifier);
        press(Qt::Key_A);
        press(Qt::Key_C);
        press(Qt::Key_O);
        check(!btn("enableRotation")->isChecked() && !btn("lockAspect")->isChecked() && !btn("sizeAboutCenter")->isChecked() && btn("objectSpace")->isChecked(),
              "track motion keys: Shift+F8, A, C and O switch rotation, lock aspect, scale about centre and object space");
        press(Qt::Key_F8, Qt::ShiftModifier);
        press(Qt::Key_A);
        press(Qt::Key_C);
        // object space: a box turned 90 degrees with X prevented moves along the frame's x
        press(Qt::Key_X);
        app.edit("Turn", [&](Project& p) {
            MotionKey k;
            k.rotation = 90;
            p.findTrack(VT)->motion = {k};
        });
        uitest::settle();
        drag(tv, at(tv, 0.5, 0.5), at(tv, 0.6, 0.5));
        t = track(VT);
        check(near(t->motion[0].x, 0.6, 0.01) && near(t->motion[0].y, 0.5, 1e-9), "track motion keys: in object space the limits follow the box's turned axes",
              num(t->motion[0].x) + ", " + num(t->motion[0].y));
        press(Qt::Key_X);
        press(Qt::Key_O);
        app.undo();
        app.undo();
        uitest::settle();

        // ---- Alt+drag keyframes: the spacing changes
        QWidget* tstrip = child<QWidget>(tm, "keyStrip");
        app.edit("Keys", [&](Project& p) {
            std::vector<MotionKey> ks(4);
            for (int k = 0; k < 4; ++k) ks[(size_t)k].time = k * 1.0;
            p.findTrack(VT)->motion = ks;
        });
        uitest::settle();
        const double tspan = tstrip->property("span").toDouble();
        auto kx = [&](double secs) { return 10 + (int)std::lround(secs / tspan * (tstrip->width() - 20)); };
        const int ky = 18 + (tstrip->height() - 18) / 2;
        drag(tstrip, QPoint(kx(1.0), ky), QPoint(kx(2.0), ky), Qt::AltModifier);
        t = track(VT);
        std::string got;
        for (const auto& k : t->motion) got += num(k.time) + " ";
        check(t->motion.size() == 4 && near(t->motion[0].time, 0) && near(t->motion[1].time, 2.0, 0.05) && near(t->motion[2].time, 3.0, 0.05) && near(t->motion[3].time, 4.0, 0.05),
              "keyframe spacing: Alt+drag the second keyframe: it and those after it spread out from the first", got);
        drag(tstrip, QPoint(kx(4.0), ky), QPoint(kx(2.0), ky), Qt::AltModifier);
        t = track(VT);
        got.clear();
        for (const auto& k : t->motion) got += num(k.time) + " ";
        check(t->motion.size() == 4 && near(t->motion[1].time, 1.0, 0.05) && near(t->motion[2].time, 1.5, 0.05) && near(t->motion[3].time, 2.0, 0.05) && app.undoName() == "Move Keyframe",
              "keyframe spacing: Alt+drag the last one back: they close up in proportion", got);
        drag(tstrip, QPoint(kx(0.0), ky), QPoint(kx(1.0), ky), Qt::AltModifier);
        t = track(VT);
        got.clear();
        for (const auto& k : t->motion) got += num(k.time) + " ";
        check(near(t->motion[0].time, 1.0, 0.05) && near(t->motion[3].time, 2.0, 1e-9) && near(t->motion[1].time, 1.5, 0.05), "keyframe spacing: the first one is spaced against the last", got);
        uitest::shot(tm, gOut + "/trackmotion_keys.png");
        app.undo();
        app.undo();
        app.undo();
        app.undo();
        uitest::settle();

        // ---- 2D Shadow and 2D Glow
        {
            // picture at half size first so the shadow shows
            app.edit("Half", [&](Project& p) {
                MotionKey k;
                k.w = k.h = 0.5;
                p.findTrack(VT)->motion = {k};
            });
            uitest::settle();
            const QImage before = playback.renderNow(2.0);
            uitest::mouseClick(btn("rowShadow"), btn("rowShadow")->rect().center());
            uitest::settle();
            QCheckBox* on = child<QCheckBox>(tm, "effectOn");
            check(tm->property("row").toInt() == 1 && on && on->isEnabled() && !on->isChecked(), "2D Shadow: its row is chosen, off");
            on->setChecked(true);
            uitest::settle();
            t = track(VT);
            check(t->shadow2d.size() == 1 && t->shadow2d[0].x > 0 && t->shadow2d[0].y > 0 && app.undoName() == "2D Shadow On", "turning it on makes a shadow down and to the right");
            // drag the shadow's box: it moves relative to the picture
            const double sx = t->shadow2d[0].x;
            drag(tv, at(tv, 0.5 + 0.02, 0.5 + 0.035), at(tv, 0.6, 0.6));
            t = track(VT);
            check(t->shadow2d.size() == 1 && near(t->shadow2d[0].x, 0.1, 0.01) && near(t->shadow2d[0].y, 0.1, 0.01) && t->motion.size() == 1 && near(t->motion[0].x, 0.5, 1e-9),
                  "dragging the shadow's box moves the shadow (not the picture)", num(sx) + " -> " + num(t->shadow2d[0].x));
            check(app.undoName() == "2D Shadow", "in one undo step");
            QDoubleSpinBox* blur = child<QDoubleSpinBox>(tm, "effectBlur");
            QDoubleSpinBox* inten = child<QDoubleSpinBox>(tm, "effectIntensity");
            blur->setValue(40);
            inten->setValue(80);
            uitest::settle();
            t = track(VT);
            check(near(t->shadow2d[0].blur, 0.4) && near(t->shadow2d[0].intensity, 0.8), "Blur and Intensity set it");
            uitest::shot(tm, gOut + "/trackmotion_shadow.png");
            // the glow
            uitest::mouseClick(btn("rowGlow"), btn("rowGlow")->rect().center());
            uitest::settle();
            check(!on->isChecked(), "2D Glow: its own row, off");
            on->setChecked(true);
            uitest::settle();
            t = track(VT);
            check(t->glow2d.size() == 1 && t->glow2d[0].color == 0xFFFFFFFFu && t->shadow2d.size() == 1, "turning it on makes a white glow (the shadow stays)");
            uitest::shot(tm, gOut + "/trackmotion_glow.png");
            QImage glowShot = playback.renderNow(2.0);
            glowShot.save(gOut + "/trackmotion_glow_preview.png");
            check(glowShot != before, "the preview shows it (a black shadow on black does not show; the white glow does)");
            on->setChecked(false);
            uitest::settle();
            check(track(VT)->glow2d.empty() && app.undoName() == "2D Glow Off", "and off again");
            uitest::mouseClick(btn("rowPosition"), btn("rowPosition")->rect().center());
            uitest::settle();
            check(!child<QCheckBox>(tm, "effectOn")->isEnabled(), "the Position row has no shadow switch");
            while (app.canUndo() && app.undoName() != "Half") app.undo();
            app.undo();
            uitest::settle();
            check(track(VT)->shadow2d.empty() && track(VT)->motion.empty(), "undo takes it all back");
        }
        tm->close();
        uitest::settle();
    }

    // ================================================================== Event Properties
    whenModal([&](QWidget* d) {
        check(d && d->objectName() == "eventPropertiesDialog", "event properties: the dialog opens");
        if (!d) return;
        child<QLineEdit>(d, "eventName")->setText("Renamed");
        child<QDoubleSpinBox>(d, "playbackRate")->setValue(2.0);
        child<QCheckBox>(d, "reverse")->setChecked(true);
        child<QCheckBox>(d, "loop")->setChecked(false);
        child<QCheckBox>(d, "maintainAspect")->setChecked(false);
        child<QDoubleSpinBox>(d, "opacity")->setValue(50);
        child<QDoubleSpinBox>(d, "fadeIn")->setValue(0.5);
        uitest::shot(d, gOut + "/event_properties.png");
        if (QTabWidget* tabs = child<QTabWidget>(d, "tabs")) {
            tabs->setCurrentIndex(1);
            uitest::settle(30);
            uitest::shot(d, gOut + "/event_properties_media.png");
        }
        clickOk(d);
    });
    runEventProperties(nullptr, V);
    {
        const Event* e = ev(V);
        check(e && e->name == "Renamed" && near(e->rate, 2.0) && e->reverse && !e->loop && !e->keepAspect && near(e->gain, 0.5) && near(e->fadeIn, 0.5),
              "event properties: name, rate, reverse, loop, aspect, opacity and fade are set");
        check(app.undoName() == "Event Properties", "event properties: one undo step", app.undoName());
        app.undo();
        e = ev(V);
        check(e && e->name != "Renamed" && near(e->rate, 1.0) && !e->reverse && e->loop && e->keepAspect, "event properties: undo puts them back");
    }
    whenModal([&](QWidget* d) {
        if (!d) return;
        check(child<QDoubleSpinBox>(d, "pitch") != nullptr && child<QDoubleSpinBox>(d, "opacity") == nullptr, "event properties: a sound event has pitch, not opacity");
        child<QDoubleSpinBox>(d, "pitch")->setValue(-3);
        child<QCheckBox>(d, "lockToRate")->setChecked(false);
        child<QDoubleSpinBox>(d, "gainDb")->setValue(-6);
        uitest::shot(d, gOut + "/event_properties_audio.png");
        clickOk(d);
    });
    runEventProperties(nullptr, A);
    {
        const Event* e = ev(A);
        check(e && near(e->pitch, -3) && e->keepPitch && near(e->gain, std::pow(10.0, -6.0 / 20.0), 1e-4), "event properties: pitch, keep pitch and gain in dB",
              e ? num(e->gain) : "");
        app.undo();
    }
    // Time stretch / pitch shift group: method, stretch attributes, lock to stretch, pitch, formants
    whenModal([&](QWidget* d) {
        if (!d) return;
        QComboBox* method = child<QComboBox>(d, "stretchMethod");
        QComboBox* mode = child<QComboBox>(d, "stretchMode");
        check(method && mode && method->count() == 3 && method->currentData().toString() == "elastique" && mode->count() == 4 &&
                  mode->currentData().toString() == "pro",
              "event properties: the time stretch method (élastique, VEGAS's default) and its four modes");
        pick(method, "classic");
        check(mode->count() == 19 && mode->currentData().toString() == "A03" && mode->itemText(0).startsWith("A01. Music 1"),
              "event properties: Classic offers VEGAS's 19 presets, A03 first picked (VEGAS's default)", mode->itemText(0).toStdString());
        check(!child<QCheckBox>(d, "preserveFormants")->isEnabled() && !child<QDoubleSpinBox>(d, "formantShift")->isEnabled(),
              "event properties: Classic has no formant settings");
        pick(mode, "A07");
        child<QCheckBox>(d, "lockToRate")->setChecked(false);
        child<QDoubleSpinBox>(d, "pitch")->setValue(2);
        child<QCheckBox>(d, "invertPhase")->setChecked(true);
        child<QCheckBox>(d, "locked")->setChecked(true);
        uitest::shot(d, gOut + "/event_properties_stretch_classic.png");
        clickOk(d);
    });
    runEventProperties(nullptr, A);
    {
        const Event* e = ev(A);
        check(e && e->stretchMethod == "classic" && e->stretchMode == "A07" && e->keepPitch && near(e->pitch, 2) && e->invertPhase && e->locked,
              "event properties: Classic A07, the pitch kept and 2 semitones up, phase inverted, locked");
        check(app.undoName() == "Event Properties", "event properties: one undo step for the stretch settings too", app.undoName());
    }
    whenModal([&](QWidget* d) {
        if (!d) return;
        QComboBox* method = child<QComboBox>(d, "stretchMethod");
        QComboBox* mode = child<QComboBox>(d, "stretchMode");
        check(method->currentData().toString() == "classic" && mode->currentData().toString() == "A07" && near(child<QDoubleSpinBox>(d, "pitch")->value(), 2),
              "event properties: the window shows the event's stretch settings");
        pick(method, "elastique");
        pick(mode, "pro");
        child<QCheckBox>(d, "preserveFormants")->setChecked(true);
        check(child<QDoubleSpinBox>(d, "formantShift")->isEnabled(), "event properties: élastique Pro with Preserve formants can shift the formants");
        child<QDoubleSpinBox>(d, "formantShift")->setValue(-3);
        uitest::shot(d, gOut + "/event_properties_stretch_elastique.png");
        pick(mode, "efficient");
        check(!child<QCheckBox>(d, "preserveFormants")->isEnabled() && !child<QDoubleSpinBox>(d, "formantShift")->isEnabled(),
              "event properties: Efficient has no formant settings (as in VEGAS)");
        pick(mode, "pro");
        clickOk(d);
    });
    runEventProperties(nullptr, A);
    {
        const Event* e = ev(A);
        check(e && e->stretchMethod == "elastique" && e->stretchMode == "pro" && e->preserveFormants && near(e->formant, -3),
              "event properties: élastique Pro, formants kept and moved 3 semitones down");
    }
    whenModal([&](QWidget* d) {
        if (!d) return;
        pick(child<QComboBox>(d, "stretchMethod"), "none");
        check(!child<QComboBox>(d, "stretchMode")->isEnabled() && !child<QCheckBox>(d, "lockToRate")->isEnabled() &&
                  !child<QDoubleSpinBox>(d, "pitch")->isEnabled(),
              "event properties: None has no stretch attributes, lock or pitch change");
        uitest::shot(d, gOut + "/event_properties_stretch_none.png");
        clickOk(d);
    });
    runEventProperties(nullptr, A);
    check(ev(A) && ev(A)->stretchMethod == "none" && near(ev(A)->pitch, 2), "event properties: None (what was set for the others is kept)");
    app.undo();
    app.undo();
    app.undo();
    check(ev(A) && ev(A)->stretchMethod == "elastique" && ev(A)->stretchMode == "pro" && !ev(A)->locked && !ev(A)->invertPhase,
          "event properties: undo puts the stretch settings back");
    whenModal([&](QWidget* d) {
        if (d) d->findChild<QPushButton*>("cancel")->click();
    });
    const std::string before = app.undoName();
    runEventProperties(nullptr, V);
    check(app.undoName() == before, "event properties: OK with nothing changed, or Cancel, makes no undo step");

    // ================================================================== Project Properties
    whenModal([&](QWidget* d) {
        check(d && d->objectName() == "projectPropertiesDialog", "project properties: the dialog opens");
        if (!d) return;
        QTabWidget* tabs = child<QTabWidget>(d, "tabs");
        check(tabs && tabs->count() == 5 && tabs->tabText(0) == "Video" && tabs->tabText(3) == "Summary" && tabs->tabText(4) == "Audio CD",
              "project properties: Video, Audio, Ruler, Summary and Audio CD tabs");
        QComboBox* tpl = child<QComboBox>(d, "template");
        const int hd720 = tpl->findText("HD 720-30p (1280x720, 29.970 fps)");
        tpl->setCurrentIndex(hd720);
        tpl->activated(hd720);
        check(child<QSpinBox>(d, "width")->value() == 1280 && child<QSpinBox>(d, "fpsNum")->value() == 30000 && child<QSpinBox>(d, "fpsDen")->value() == 1001,
              "project properties: a template fills in size and the exact frame rate");
        QComboBox* rate = child<QComboBox>(d, "frameRate");
        rate->setCurrentIndex(rate->findText("23.976 (IVTC Film)"));
        // the template becomes Custom, or HDV 720-24p (1280x720 at 23.976)
        check(child<QSpinBox>(d, "fpsNum")->value() == 24000 && child<QSpinBox>(d, "fpsDen")->value() == 1001 &&
                  (tpl->currentIndex() == 0 || tpl->currentText().contains("1280x720, 23.976")),
              "project properties: 23.976 is 24000/1001 exactly (and the template follows)", tpl->currentText().toStdString());
        pick(child<QComboBox>(d, "renderQuality"), "best");
        pick(child<QComboBox>(d, "fieldOrder"), "upper");
        uitest::shot(d, gOut + "/project_properties_video.png");
        tabs->setCurrentIndex(1);
        uitest::settle(30);
        child<QComboBox>(d, "sampleRate")->setCurrentIndex(child<QComboBox>(d, "sampleRate")->findData(44100));
        pick(child<QComboBox>(d, "bitDepth"), "24");
        uitest::shot(d, gOut + "/project_properties_audio.png");
        tabs->setCurrentIndex(2);
        uitest::settle(30);
        pick(child<QComboBox>(d, "rulerFormat"), "measures");
        child<QDoubleSpinBox>(d, "tempo")->setValue(100);
        child<QSpinBox>(d, "beatsPerMeasure")->setValue(3);
        uitest::shot(d, gOut + "/project_properties_ruler.png");
        tabs->setCurrentIndex(3);
        uitest::settle(30);
        child<QLineEdit>(d, "title")->setText("My Film");
        child<QPlainTextEdit>(d, "comments")->setPlainText("A note");
        uitest::shot(d, gOut + "/project_properties_summary.png");
        clickOk(d);
    });
    const ProjectSettings was = app.project().settings;
    runProjectPropertiesFull(nullptr);
    {
        const ProjectSettings& s = app.project().settings;
        check(s.width == 1280 && s.height == 720 && s.fpsNum == 24000 && s.fpsDen == 1001, "project properties: size and frame rate set",
              strfmt("%dx%d %d/%d", s.width, s.height, s.fpsNum, s.fpsDen));
        check(s.renderQuality == "best" && s.fieldOrder == "upper" && s.sampleRate == 44100 && s.audioBits == 24, "project properties: quality, fields, audio set");
        check(s.rulerFormat == "measures" && near(s.tempo, 100) && s.beatsPerMeasure == 3 && s.title == "My Film" && s.comments == "A note",
              "project properties: ruler and summary set");
        check(app.undoName() == "Project Properties", "project properties: one undo step", app.undoName());
        app.undo();
        const ProjectSettings& u = app.project().settings;
        check(u.width == was.width && u.fpsNum == was.fpsNum && u.renderQuality == was.renderQuality && u.title == was.title, "project properties: undo puts them back");
    }

    // ==================================================================== Project Properties: Master Display
    {
        whenModal([&](QWidget* d) {
            if (!d) return;
            QComboBox* hdr = child<QComboBox>(d, "hdrMode");
            QComboBox* md = child<QComboBox>(d, "masterDisplay");
            QPushButton* custom = child<QPushButton>(d, "masterDisplayCustomize");
            check(md && custom && custom->text() == "Customize", "master display: a combo and a Customize button");
            if (!md || !custom) { clickOk(d); return; }
            check(md->itemText(md->findData("custom")) == "Custom display", "master display: the combo's first entry is Custom display");
            pick(hdr, "off");
            hdr->activated(hdr->currentIndex());
            check(!md->isEnabled() && !custom->isEnabled(), "master display: greyed while HDR Mode is Off");
            pick(hdr, "hdr10");
            hdr->activated(hdr->currentIndex());
            check(md->isEnabled() && custom->isEnabled(), "master display: enabled with HDR10");
            pick(md, "p3d65_1000");
            md->activated(md->currentIndex());
            whenModal([&](QWidget* c) {
                check(c && c->objectName() == "masterDisplayDialog", "master display: Customize opens its dialog");
                if (!c) return;
                check(near(child<QDoubleSpinBox>(c, "whiteX")->value(), 0.3127) && near(child<QDoubleSpinBox>(c, "maxNits")->value(), 1000),
                      "master display: the dialog shows the preset's values");
                child<QDoubleSpinBox>(c, "maxNits")->setValue(4000);
                child<QSpinBox>(c, "maxCll")->setValue(900);
                child<QSpinBox>(c, "maxFall")->setValue(300);
                uitest::shot(c, gOut + "/master_display.png");
                clickOk(c);
            });
            QTimer::singleShot(0, custom, [custom] { custom->click(); });
            uitest::settle(400);
            check(md->currentData().toString() == "custom", "master display: after Customize the combo says Custom display");
            uitest::shot(d, gOut + "/project_properties_master_display.png");
            clickOk(d);
        });
        runProjectPropertiesFull(nullptr);
        const MasterDisplay& m = app.project().settings.masterDisplay;
        check(app.project().settings.hdrMode == "hdr10" && m.preset == "custom" && near(m.maxNits, 4000) && m.maxCll == 900 && m.maxFall == 300 && near(m.whiteX, 0.3127),
              "master display: kept in the project", strfmt("%s %g %d", m.preset.c_str(), m.maxNits, m.maxCll));
        // saved and read back (.reno JSON)
        Project back;
        std::string err;
        check(back.fromJson(app.project().toJson(), &err) && back.settings.masterDisplay == m, "master display: round trip through the project file", err);
        app.undo();
        check(app.project().settings.masterDisplay == MasterDisplay(), "master display: undo puts the default back");
    }

    // ================================================================== Preferences
    check(app.prefs.undoLevels == 200 && app.prefs.autoSaveMinutes == 5 && near(app.prefs.stillLength, 5.0), "preferences: the defaults to begin with");
    // VEGAS 18's defaults, read from vegas180.exe
    {
        const Preferences def;  // the defaults themselves: an old settings folder may hold other values
        check(def.preservePitch && def.loopEvents && !def.quickFadeEdits && near(def.quickFadeMs, 10.0),
              "preferences: VEGAS's defaults - preserve pitch, loop events, no quick fades (VEGAS's own renders have none; 10 ms when on)");
    }
    whenModal([&](QWidget* d) {
        check(d && d->objectName() == "preferencesDialog", "preferences: the dialog opens");
        if (!d) return;
        QTabWidget* tabs = child<QTabWidget>(d, "tabs");
        check(tabs && tabs->count() == 14 && tabs->tabText(0) == "General" && tabs->tabText(7) == "Editing" && tabs->tabText(13) == "Reno",
              "preferences: VEGAS's tabs (General ... File I/O, without CD Settings) and Reno's own");
        child<QSpinBox>(d, "undoLevels")->setValue(3);
        child<QSpinBox>(d, "autoSaveMinutes")->setValue(1);
        uitest::shot(d, gOut + "/preferences_general.png");
        tabs->setCurrentIndex(7);
        uitest::settle(30);
        child<QDoubleSpinBox>(d, "stillLength")->setValue(7);
        child<QCheckBox>(d, "snapping")->setChecked(false);
        pick(child<QComboBox>(d, "scrollMode"), "smooth");
        if (QDoubleSpinBox* qf = child<QDoubleSpinBox>(d, "quickFadeMs")) qf->setValue(25);
        uitest::shot(d, gOut + "/preferences_editing.png");
        tabs->setCurrentIndex(4);
        uitest::settle(30);
        uitest::shot(d, gOut + "/preferences_audio.png");
        tabs->setCurrentIndex(14);
        uitest::settle(30);
        QWidget* vf = child<QWidget>(d, "videoFolders");
        if (vf) {
            child<QLineEdit>(vf, "folderEdit")->setText("/tmp/more-ofx");
            child<QPushButton>(vf, "folderAdd")->click();
        }
        pick(child<QComboBox>(d, "ofxHostName"), "org.reno.host");
        child<QCheckBox>(d, "vegasExtensions")->setChecked(false);
        uitest::shot(d, gOut + "/preferences_plugins.png");
        clickOk(d);
    });
    runPreferences(nullptr);
    {
        const Preferences& p = app.prefs;
        check(p.undoLevels == 3 && p.autoSaveMinutes == 1 && near(p.stillLength, 7.0) && !p.snapping && p.scrollMode == "smooth",
              "preferences: the values chosen are kept");
        check(!app.snapping, "preferences: snapping is switched as chosen");
        check(near(p.quickFadeMs, 25.0) && (!p.quickFadeEdits || near(app.project().quickFade, 0.025)),
              "preferences: the quick fade length reaches the project (25 ms)", strfmt("%g", app.project().quickFade));
        app.prefs.quickFadeMs = 10.0;  // back to VEGAS's, for tests sharing this settings folder
        app.applyPreferences();
        QSettings q;
        check(q.value("prefs/undoLevels").toInt() == 3 && q.value("prefs/scrollMode").toString() == "smooth", "preferences: saved in the settings");
        Preferences again;
        again.load();
        check(again.undoLevels == 3 && near(again.stillLength, 7.0) && again.ofxHostName == "org.reno.host", "preferences: read back the same");
        const auto& dirs = Catalog::get().extraVideoDirs;
        check(std::find(dirs.begin(), dirs.end(), "/tmp/more-ofx") != dirs.end(), "preferences: the extra plug-in folder goes to the catalog");
        check(ofx::Host::get().config.hostName == "org.reno.host" && !ofx::Host::get().config.vegasExtensions,
              "preferences: the OpenFX host switches are applied");
        // the undo limit
        for (int k = 0; k < 5; ++k) app.edit("Step", [k](Project& pr) { pr.settings.tempo = 100 + k; });
        int n = 0;
        while (app.canUndo()) { app.undo(); ++n; }
        check(n == 3, "preferences: Undo remembers as many steps as chosen", std::to_string(n));
        while (app.canRedo()) app.redo();
        // the length of a new still
        const uint64_t pic = app.importMedia("synthpic:3", &err);
        const std::vector<uint64_t> ids = app.addMediaToTimeline(pic, 20.0);
        check(ids.size() == 1 && near(ev(ids[0])->length, 7.0), "preferences: a still put on the timeline lasts as long as chosen",
              ids.empty() ? "" : num(ev(ids[0])->length));
    }
    // auto-save: a backup next to the project, only when something is unsaved
    {
        const std::string dir = QDir::homePath().toStdString();
        const std::string path = pathJoin(dir, "autosave_test.reno");
        std::remove((path + ".bak").c_str());
        check(app.save(path, &err), "auto-save: the project saves", err);
        check(app.autoSavePath() == path + ".bak", "auto-save: the backup is <project>.reno.bak", app.autoSavePath());
        check(app.autoSaveNow(&err) && !fileExists(path + ".bak"), "auto-save: nothing is written while there is nothing unsaved");
        app.edit("Tempo", [](Project& p) { p.settings.tempo = 133; });
        check(app.autoSaveNow(&err) && fileExists(path + ".bak"), "auto-save: with unsaved changes the backup is written", err);
        Project back;
        check(back.load(path + ".bak", &err) && near(back.settings.tempo, 133), "auto-save: the backup has the changes", err);
        check(app.dirty() && app.path() == path, "auto-save: the project itself is still unsaved, under its own name");
    }
    // back to the defaults (other tests share the OpenFX host)
    {
        app.prefs = Preferences();
        Catalog::get().extraVideoDirs.clear();
        app.applyPreferences();
        check(ofx::Host::get().config.hostName == "com.vegascreativesoftware.vegas" && ofx::Host::get().config.vegasExtensions, "preferences: back to the defaults");
    }
    return uitest::summary();
}
