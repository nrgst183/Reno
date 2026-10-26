// VEGAS Pro 18 manual features (animating video, applying effects, mixing audio, the Mixing Console): keyframe types and
// Smoothness, rotation centre, Stretch to fill frame, keyframe selection / copying / presets, workspace offsets and grid, Pan/Crop's
// Edit heading and mask tools, Track Motion's aspect commands, audio event effects, Bypass All Audio FX, pre/post fader bus effects,
// assignable FX chains, the console's insert FX menus, Invert Phase, Downmix, Real-Time Render and Non-Real-Time Event FX.
//
//   man3_test <output folder> <testfx.ofx>
//
// Set HOME to a scratch folder. Pictures of the windows go to the output folder. Prints PASS / FAIL lines and a summary.
#include "../../src/app/audiotools.h"
#include "../../src/app/editdialogs.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/mixer.h"
#include "../../src/app/motiontracking.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/app/playback.h"
#include "../../src/app/timeline.h"
#include "../../src/engine/engine.h"
#include "../../src/ofx/ofxhost.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
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
template <class T>
T* child(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

QPoint at(QWidget* view, double fx, double fy)
{
    const QRectF a = view->property("area").toRectF();
    return QPoint((int)std::lround(a.left() + fx * a.width()), (int)std::lround(a.top() + fy * a.height()));
}
void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier, Qt::MouseButton button = Qt::LeftButton)
{
    uitest::mousePress(w, from, button, mods);
    for (int k = 1; k <= 8; ++k) uitest::mouseMove(w, from + (to - from) * k / 8, button, mods);
    uitest::mouseRelease(w, to, button, mods);
    uitest::settle(30);
}
const Event* ev(uint64_t id) { return AppState::get().project().findEvent(id); }
const Track* track(uint64_t id) { return const_cast<Project&>(AppState::get().project()).findTrack(id); }

// The strip's x for a time (KeyStrip: 10-pixel margin each side) and the keyframes' row.
int keyX(QWidget* strip, double secs)
{
    const double span = strip->property("span").toDouble();
    return 10 + (int)std::lround(secs / span * (strip->width() - 20));
}
int keyY(QWidget* strip) { return 18 + (strip->height() - 18) / 2; }

QStringList texts(QMenu* m)
{
    QStringList v;
    for (QAction* a : m->actions())
        if (!a->isSeparator()) v << a->text().section('\t', 0, 0).remove('&');
    return v;
}
QAction* find(QMenu* m, const QString& text)
{
    for (QAction* a : m->actions())
        if (a->text().section('\t', 0, 0).remove('&') == text) return a;
    return nullptr;
}

// The picture's brightness at a point (0..255).
int lum(const QImage& img, int x, int y)
{
    if (img.isNull() || x < 0 || y < 0 || x >= img.width() || y >= img.height()) return -1;
    return qGray(img.pixel(x, y));
}

// The mix's sound for a stretch of the timeline (left and right, summed absolute values).
double loudness(const Project& p, double from, double secs, AudioEngine* eng = nullptr)
{
    AppState& app = AppState::get();
    static MediaPool pool;
    pool.setOpener(app.mediaOpener);
    AudioEngine own(pool, app.fx);
    AudioEngine& e = eng ? *eng : own;
    const int rate = p.settings.sampleRate, n = (int)(secs * rate);
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int at = 0; at < n; at += 2048) {
        const int k = std::min(2048, n - at);
        e.render(p, (int64_t)(from * rate) + at, k, l.data() + at, r.data() + at, true);
    }
    double s = 0;
    for (int k = 0; k < n; ++k) s += std::fabs(l[(size_t)k]) + std::fabs(r[(size_t)k]);
    return s;
}

AudioPluginDesc builtinAudio(const std::string& uid)
{
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.uid == uid) return e.desc;
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = uid;
    return d;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMan3Test");
    QApplication::setApplicationName("man3_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) { qWarning("usage: man3_test <output folder> <testfx.ofx>"); return 2; }
    gOut = args[1];
    QDir().mkpath(gOut);
    uitest::setup({args[2].toStdString()}, {});

    // ================================================================== the engine: keyframe types, Smoothness
    {
        const double s = 0.25;
        check(near(keyCurveShape(KeyLinear, s), 0.25) && near(keyCurveShape(KeyHold, s), 0.0) && near(keyCurveShape(KeySmooth, s), curveShape(CurveSmooth, s)) &&
                  near(keyCurveShape(KeyFast, s), curveShape(CurveFast, s)) && near(keyCurveShape(KeySlow, s), curveShape(CurveSlow, s)) &&
                  near(keyCurveShape(KeySharp, s), curveShape(CurveSharp, s)),
              "keyframe types: linear, hold, smooth, fast, slow, sharp shape the way between keys");
        check(keyCurveShape(KeyFast, s) > 0.25 && keyCurveShape(KeySlow, s) < 0.25, "keyframe types: fast changes quickly at first, slow slowly");
        bool round = true;
        for (int c = 0; c <= 5; ++c) round = round && keyCurveFromVegas(keyCurveToVegas(c)) == c;
        check(round && keyCurveToVegas(KeySlow) == 2 && keyCurveToVegas(KeySmooth) == 4 && keyCurveFromVegas(3) == KeyFast,
              "keyframe types: VEGAS's numbers (0 linear, 1 hold, 2 slow, 3 fast, 4 smooth, 5 sharp) both ways");
        double x = 0, y = 0;
        smoothPath(0, 0, 0, 0, 0, 1, 1, 1, 0, 0.5, x, y);
        const bool straight = near(x, 0.5) && near(y, 0.5);
        smoothPath(100, 0, 0, 0, 0, 1, 1, 1, 0, 0.5, x, y);
        check(straight && !near(y, 0.5, 1e-3) && near(x, 0.5, 0.2), "Smoothness: 0 a straight line, 100 a curve through the keys round it", num(x) + ", " + num(y));
        smoothPath(100, 0, 0, 0, 0, 1, 1, 1, 0, 1.0, x, y);
        check(near(x, 1) && near(y, 1), "Smoothness: the curve still reaches the next key");
    }

    AppState& app = AppState::get();
    app.newProject();
    app.edit("Small Project", [](Project& p) { p.settings.width = 640; p.settings.height = 360; });
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    uint64_t V = 0, A = 0, VT = 0, AT = 0;
    for (uint64_t id : placed) {
        const Track* t = nullptr;
        app.project().findEvent(id, &t);
        if (t && t->type == TrackType::Video) { V = id; VT = t->id; }
        else if (t) { A = id; AT = t->id; }
    }
    check(V && A && VT && AT, "setup: a clip with picture and sound on the timeline");
    app.setCursor(0.0);
    Playback playback;

    // ================================================================== the engine's pan/crop: types, smoothness, fill
    {
        // two keys, fast: half way in time the box is more than half way
        app.edit("Keys", [&](Project& p) {
            PanCropKey a, b;
            a.time = 0; a.cx = 0.3; a.w = a.h = 0.5; a.curve = KeyFast;
            b.time = 2; b.cx = 0.7; b.w = b.h = 0.5;
            p.findEvent(V)->panCrop = {a, b};
        });
        const QImage fast = playback.renderNow(1.0);
        app.edit("Keys", [&](Project& p) { p.findEvent(V)->panCrop[0].curve = KeyLinear; });
        const QImage linear = playback.renderNow(1.0);
        check(!fast.isNull() && fast != linear, "pan/crop keys: a Fast key gives another picture half way than a Linear one");
        // stretch to fill: a box the shape of a portrait, fitted (bars at the sides) or filling the frame
        app.edit("Box", [&](Project& p) {
            PanCropKey k;
            k.w = 0.4;
            k.h = 1.0;
            p.findEvent(V)->panCrop = {k};
        });
        const QImage fit = playback.renderNow(1.0);
        app.edit("Stretch", [&](Project& p) { p.findEvent(V)->stretchToFill = true; });
        const QImage fill = playback.renderNow(1.0);
        check(lum(fit, 6, fit.height() / 2) < 8 && lum(fill, 6, fill.height() / 2) > 8,
              "Stretch to fill frame: No fits the box (bars), Yes fills the frame with it", strfmt("%d / %d", lum(fit, 6, fit.height() / 2), lum(fill, 6, fill.height() / 2)));
        app.edit("Reset", [&](Project& p) {
            p.findEvent(V)->panCrop.clear();
            p.findEvent(V)->stretchToFill = false;
        });
        // the project file keeps the new things
        Project q = app.project();
        PanCropKey k;
        k.smoothness = 40;
        k.pivotX = 0.1;
        k.pivotY = -0.2;
        k.curve = KeySharp;
        q.findEvent(V)->panCrop = {k};
        q.findEvent(V)->stretchToFill = true;
        MotionKey mk;
        mk.smoothness = 70;
        mk.curve = KeySlow;
        q.findTrack(VT)->motion = {mk};
        Project r;
        r.fromJson(q.toJson());
        const Event* re = r.findEvent(V);
        const Track* rt = r.findTrack(VT);
        check(re && re->stretchToFill && re->panCrop.size() == 1 && near(re->panCrop[0].smoothness, 40) && near(re->panCrop[0].pivotX, 0.1) &&
                  near(re->panCrop[0].pivotY, -0.2) && re->panCrop[0].curve == KeySharp && rt && rt->motion.size() == 1 && near(rt->motion[0].smoothness, 70) &&
                  rt->motion[0].curve == KeySlow,
              "the project keeps Smoothness, the rotation centre, the new types and Stretch to fill frame");
    }

    // ================================================================== Event Pan/Crop
    openPanCrop(nullptr, &playback, V);
    uitest::settle();
    QPointer<QWidget> pc = topLevel("panCropWindow");
    check(pc != nullptr, "pan/crop: the window opens");
    if (!pc) return uitest::summary();
    pc->resize(1100, 720);
    uitest::settle();
    QWidget* view = child<QWidget>(pc, "boxView");
    QWidget* strip = child<QWidget>(pc, "keyStrip");
    QComboBox* curve = child<QComboBox>(pc, "keyCurve");
    {
        QStringList items;
        for (int i = 0; curve && i < curve->count(); ++i) items << curve->itemText(i);
        check(items == QStringList({"Linear", "Fast", "Slow", "Smooth", "Sharp", "Hold"}), "pan/crop: the keyframe types, in VEGAS's order", items.join(", ").toStdString());
    }
    // three keys
    app.edit("Keys", [&](Project& p) {
        std::vector<PanCropKey> ks(3);
        for (int k = 0; k < 3; ++k) {
            ks[(size_t)k].time = k * 1.0;
            ks[(size_t)k].cx = 0.3 + 0.2 * k;
            ks[(size_t)k].cy = k == 1 ? 0.3 : 0.6;
            ks[(size_t)k].w = ks[(size_t)k].h = 0.5;
        }
        p.findEvent(V)->panCrop = ks;
    });
    uitest::settle();
    // Ctrl+click two keyframes, then the menu's Slow sets both
    uitest::mouseClick(strip, QPoint(keyX(strip, 0.0), keyY(strip)));
    uitest::mouseClick(strip, QPoint(keyX(strip, 1.0), keyY(strip)), Qt::LeftButton, Qt::ControlModifier);
    uitest::settle();
    {
        QMenu* shown = nullptr;
        QTimer::singleShot(100, [&] {
            shown = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (shown) {
                if (QAction* a = find(shown, "Slow")) a->trigger();
                shown->close();
            }
        });
        uitest::mousePress(strip, QPoint(keyX(strip, 1.0), keyY(strip)), Qt::RightButton);
        uitest::settle(200);
        const auto& ks = ev(V)->panCrop;
        check(ks.size() == 3 && ks[0].curve == KeySlow && ks[1].curve == KeySlow && ks[2].curve == KeyLinear,
              "pan/crop keys: Ctrl+click selects two keyframes; the right-click menu's type goes to both",
              ks.size() == 3 ? strfmt("%d %d %d", ks[0].curve, ks[1].curve, ks[2].curve) : "");
    }
    // Ctrl+Right / Ctrl+Left step from keyframe to keyframe
    app.setCursor(0.0);
    uitest::settle();
    uitest::key(pc, Qt::Key_Right, Qt::ControlModifier);
    uitest::settle();
    const double afterRight = app.cursor();
    uitest::key(pc, Qt::Key_Left, Qt::ControlModifier);
    uitest::settle();
    check(near(afterRight, 1.0, 1e-6) && near(app.cursor(), 0.0, 1e-6), "pan/crop keys: Ctrl+Right / Ctrl+Left go to the next / previous keyframe", num(afterRight));
    // copy (the two selected) and paste at 2.5 s
    uitest::key(pc, Qt::Key_C, Qt::ControlModifier);
    app.setCursor(2.5);
    uitest::settle();
    uitest::key(pc, Qt::Key_V, Qt::ControlModifier);
    uitest::settle();
    {
        const auto& ks = ev(V)->panCrop;
        check(ks.size() == 5 && near(ks[3].time, 2.5, 1e-6) && near(ks[4].time, 3.5, 1e-6) && near(ks[3].cx, 0.3) && near(ks[4].cx, 0.5) && app.undoName() == "Paste Keyframes",
              "pan/crop keys: Ctrl+C copies the selected keyframes, Ctrl+V puts them at the cursor", strfmt("%d keys", (int)ks.size()));
    }
    app.undo();
    uitest::settle();
    // Ctrl+drag copies the keyframe at 2 s to 3 s
    app.setCursor(0.0);
    uitest::settle();
    uitest::mouseClick(strip, QPoint(keyX(strip, 2.0), keyY(strip)));
    drag(strip, QPoint(keyX(strip, 2.0), keyY(strip)), QPoint(keyX(strip, 3.0), keyY(strip)), Qt::ControlModifier);
    {
        const auto& ks = ev(V)->panCrop;
        check(ks.size() == 4 && near(ks[2].time, 2.0, 0.04) && near(ks[3].time, 3.0, 0.04) && near(ks[3].cx, ks[2].cx) && app.undoName() == "Copy Keyframes",
              "pan/crop keys: Ctrl+drag copies a keyframe (the original stays)", strfmt("%d keys", (int)ks.size()));
    }
    // Delete on the strip deletes the selected (the copy)
    strip->setFocus();
    uitest::key(strip, Qt::Key_Delete);
    uitest::settle();
    check(ev(V)->panCrop.size() == 3, "pan/crop keys: Delete on the strip deletes the selected keyframe", std::to_string(ev(V)->panCrop.size()));
    // Smoothness on the keyframe at 0: the path bends; the preview follows
    app.setCursor(0.0);
    uitest::settle();
    const QImage straightAt05 = playback.renderNow(0.5);
    QDoubleSpinBox* smooth = child<QDoubleSpinBox>(pc, "keySmoothness");
    if (smooth) {
        smooth->setValue(100);
        uitest::settle();
    }
    check(smooth && near(ev(V)->panCrop[0].smoothness, 100), "pan/crop: the Smoothness box sets the keyframe's smoothness");
    check(playback.renderNow(0.5) != straightAt05, "pan/crop: ... and the picture between keyframes follows the curve");
    uitest::shot(pc, gOut + "/pancrop_path.png");
    // the rotation centre: X / Y Center boxes, and turning about it moves the box
    app.edit("One key", [&](Project& p) {
        PanCropKey k;
        k.w = k.h = 0.5;
        p.findEvent(V)->panCrop = {k};
    });
    uitest::settle();
    QDoubleSpinBox* px = child<QDoubleSpinBox>(pc, "pivotX");
    QDoubleSpinBox* angle = child<QDoubleSpinBox>(pc, "boxAngle");
    const double mw = view->property("area").toRectF().width() > 0 ? 320.0 : 320.0;  // the synth clip is 320 x 180
    if (px && angle) {
        px->setValue(0.25 * mw);                // a quarter of the way across
        uitest::settle();
        angle->setValue(180);
        uitest::settle();
    }
    {
        const PanCropKey k = ev(V)->panCrop[0];
        check(px && near(k.rotation, 180) && near(k.cx, 0.0, 1e-6) && near(k.cy, 0.5, 1e-6) && near(k.cx + k.pivotX, 0.25, 1e-6),
              "pan/crop: with a rotation centre set, turning the box turns it round that point", num(k.cx) + ", " + num(k.cy) + " pivot " + num(k.pivotX));
    }
    // Stretch to fill frame
    if (QComboBox* st = child<QComboBox>(pc, "stretchToFill")) {
        st->setCurrentIndex(1);
        emit st->activated(1);
        uitest::settle();
        check(ev(V)->stretchToFill && app.undoName() == "Stretch to Fill Frame", "pan/crop: the Source heading's Stretch to fill frame");
    }
    // the workspace: X offset moves the view, Grid spacing the grid
    {
        QDoubleSpinBox* ox = child<QDoubleSpinBox>(pc, "workspaceOffsetX");
        const QRectF before = view->property("area").toRectF();
        if (ox) ox->setValue(100);
        uitest::settle();
        const QRectF after = view->property("area").toRectF();
        check(ox && after.left() > before.left() + 10, "pan/crop workspace: X offset moves the view right", num(after.left() - before.left()));
        // dragging the empty workspace pans it back
        drag(view, QPoint(8, 8), QPoint(8 - (int)(after.left() - before.left()), 8));
        check(near(view->property("area").toRectF().left(), before.left(), 2.0) && near(ox->value(), 0.0, 2.0),
              "pan/crop workspace: dragging the empty workspace pans it (and the X offset follows)", num(ox->value()));
        if (QDoubleSpinBox* g = child<QDoubleSpinBox>(pc, "gridSpacing")) g->setValue(4);
    }
    // the Edit heading follows the tool bar and the other way round
    {
        QComboBox* snap = child<QComboBox>(pc, "editSnap");
        QToolButton* snapBtn = child<QToolButton>(pc, "enableSnapping");
        QComboBox* move = child<QComboBox>(pc, "editMovement");
        QToolButton* noX = child<QToolButton>(pc, "preventMoveX");
        if (snap && snapBtn && move && noX) {
            snap->setCurrentIndex(snap->findData("1"));
            emit snap->activated(snap->currentIndex());
            const bool on = snapBtn->isChecked();
            noX->click();
            check(on && move->currentData().toString() == "y", "pan/crop Edit heading: Snap to grid turns snapping on; Prevent Movement X shows as Vertical");
            noX->click();
            snapBtn->click();
        } else {
            check(false, "pan/crop Edit heading: its drop-downs are there");
        }
    }
    uitest::shot(pc, gOut + "/pancrop.png");

    // ---- presets: save the box at the cursor, recall it at another time (a key is made there), delete it
    {
        app.edit("Two keys", [&](Project& p) {
            PanCropKey a, b;
            a.cx = 0.2; a.w = a.h = 0.4;
            b.time = 1; b.cx = 0.8; b.w = b.h = 0.4;
            p.findEvent(V)->panCrop = {a, b};
        });
        app.setCursor(0.0);
        uitest::settle();
        QComboBox* presets = child<QComboBox>(pc, "panCropPreset");
        QToolButton* save = child<QToolButton>(pc, "savePreset");
        QToolButton* del = child<QToolButton>(pc, "deletePreset");
        if (presets && save && del) {
            presets->setEditText("Left side");
            save->click();
            uitest::settle();
            const int i = presets->findText("Left side");
            check(i > 0, "pan/crop presets: Save Preset keeps the box under the name typed");
            app.setCursor(2.0);
            uitest::settle();
            emit presets->activated(i);
            uitest::settle();
            const auto& ks = ev(V)->panCrop;
            check(ks.size() == 3 && near(ks[2].time, 2.0, 1e-6) && near(ks[2].cx, 0.2) && near(ks[2].w, 0.4),
                  "pan/crop presets: choosing one where there is no keyframe makes one there with its settings", strfmt("%d keys", (int)ks.size()));
            // the Mask row has presets of its own
            if (QComboBox* row = child<QComboBox>(pc, "keyRow")) {
                row->setCurrentIndex(1);
                emit row->activated(1);
                uitest::settle();
                check(presets->findText("Left side") < 0, "pan/crop presets: the Mask row lists its own presets");
                row->setCurrentIndex(0);
                emit row->activated(0);
                uitest::settle();
            }
            presets->setEditText("Left side");
            del->click();
            uitest::settle();
            check(presets->findText("Left side") < 0, "pan/crop presets: Delete Preset");
        } else {
            check(false, "pan/crop presets: the Preset box, Save and Delete are there");
        }
    }

    // ---- the mask tools: Alt shapes, Alt+Shift selection, Ctrl+drag duplicate, scale and turn handles
    {
        app.edit("No keys", [&](Project& p) { p.findEvent(V)->panCrop.clear(); });
        app.setCursor(0.0);
        uitest::settle();
        QToolButton* rect = child<QToolButton>(pc, "toolRectMask");
        QToolButton* oval = child<QToolButton>(pc, "toolOvalMask");
        QToolButton* normal = child<QToolButton>(pc, "toolNormal");
        if (rect && oval && normal) {
            rect->click();
            drag(view, at(view, 0.1, 0.1), at(view, 0.4, 0.4), Qt::AltModifier);
            const auto* e = ev(V);
            const size_t n1 = e->mask.empty() ? 0 : e->mask[0].paths.size();
            check(n1 == 1 && e->mask[0].paths[0].points.size() == 8, "mask: Alt+drag with the Rectangle tool makes a rectangle with round corners (8 points)",
                  n1 ? std::to_string(e->mask[0].paths[0].points.size()) : "no mask");
            oval->click();
            drag(view, at(view, 0.6, 0.1), at(view, 0.9, 0.4), Qt::AltModifier);
            e = ev(V);
            check(e->mask[0].paths.size() == 2 && e->mask[0].paths[1].points.size() == 3, "mask: Alt+drag with the Oval tool makes half an oval");
            normal->click();
            uitest::settle();
            // Alt+Shift+click selects the first shape
            uitest::mouseClick(view, at(view, 0.25, 0.25), Qt::LeftButton, Qt::AltModifier | Qt::ShiftModifier);
            uitest::settle();
            uitest::shot(pc, gOut + "/pancrop_mask_selected.png");
            // its transform box: the bottom right corner (9 px outside the shape) dragged out doubles it from the top left
            const MaskPath before = ev(V)->mask[0].paths[0];
            double l = 1, t = 1, r = 0, b = 0;
            for (const MaskPoint& q : before.points) { l = std::min(l, q.x); r = std::max(r, q.x); t = std::min(t, q.y); b = std::max(b, q.y); }
            const QPoint br = at(view, r, b) + QPoint(9, 9);
            const QPoint tl = at(view, l, t) - QPoint(9, 9);
            drag(view, br, tl + (br - tl) * 2);
            double r2 = 0;
            for (const MaskPoint& q : ev(V)->mask[0].paths[0].points) r2 = std::max(r2, q.x);
            check(r2 > r + 0.1 && app.undoName() == "Scale Mask", "mask: the selected shape's corner handle scales it", num(r) + " -> " + num(r2));
            app.undo();
            uitest::settle();
            // the handle above turns it: half a turn swaps top and bottom
            const QPoint c = (at(view, l, t) + at(view, r, b)) / 2;
            const QPoint handle(c.x(), at(view, l, t).y() - 9 - 18);
            uitest::mousePress(view, handle);
            for (int k = 1; k <= 12; ++k) {
                const double a = -3.14159265358979 / 2 + 3.14159265358979 * k / 12;
                const double rr = c.y() - handle.y();
                uitest::mouseMove(view, QPoint(c.x() + (int)std::lround(rr * std::cos(a)), c.y() + (int)std::lround(rr * std::sin(a))));
            }
            uitest::mouseRelease(view, QPoint(c.x(), c.y() + (c.y() - handle.y())));
            uitest::settle();
            const MaskPoint p0 = ev(V)->mask[0].paths[0].points[0];
            check(app.undoName() == "Rotate Mask" && near(p0.y, b - (before.points[0].y - t), 0.03),
                  "mask: the handle above the selected shape turns it", num(before.points[0].y) + " -> " + num(p0.y));
            app.undo();
            uitest::settle();
            // Ctrl+drag a shape: a copy is dragged
            drag(view, at(view, 0.25, 0.25), at(view, 0.25, 0.7), Qt::ControlModifier);
            check(ev(V)->mask[0].paths.size() == 3 && app.undoName() == "Duplicate Mask", "mask: Ctrl+drag of a shape duplicates it");
            uitest::shot(pc, gOut + "/pancrop_mask.png");
        } else {
            check(false, "mask: the tools are there");
        }
    }
    pc->close();
    uitest::settle();

    // ================================================================== Track Motion
    openTrackMotion(nullptr, &playback, VT);
    uitest::settle();
    QPointer<QWidget> tm = topLevel("trackMotionWindow");
    check(tm != nullptr, "track motion: the window opens");
    if (tm) {
        tm->resize(1100, 720);
        uitest::settle();
        QWidget* tv = child<QWidget>(tm, "boxView");
        app.edit("Half", [&](Project& p) {
            MotionKey k;
            k.w = k.h = 0.5;
            p.findTrack(VT)->motion = {k};
        });
        uitest::settle();
        // the right-click menu's shapes
        QMenu* menu = nullptr;
        QTimer::singleShot(100, [&] {
            menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QStringList t = menu ? texts(menu) : QStringList();
            if (menu) {
                if (QAction* a = find(menu, "Square Aspect")) a->trigger();
                menu->close();
            }
            check(t.contains("Match Output Aspect") && t.contains("Match Source Aspect") && t.contains("Square Aspect") && t.contains("Restore"),
                  "track motion: the right-click menu has VEGAS's Restore, Center, flips and the three shapes", t.join(", ").toStdString());
        });
        uitest::mousePress(tv, at(tv, 0.5, 0.5), Qt::RightButton);
        uitest::settle(200);
        {
            const MotionKey k = track(VT)->motion[0];
            check(near(k.w * 640, k.h * 360, 0.5), "track motion: Square Aspect makes the box square in the frame", num(k.w * 640) + " x " + num(k.h * 360));
        }
        // presets: kept per row and apart for 2D and 3D
        QComboBox* presets = child<QComboBox>(tm, "motionPreset");
        check(presets && presets->findText("Picture in picture, top right") > 0, "track motion presets: VEGAS-style picture-in-picture presets");
        if (presets) {
            presets->setEditText("Square");
            child<QToolButton>(tm, "savePreset")->click();
            uitest::settle();
            check(presets->findText("Square") > 0, "track motion presets: Save Preset");
            QToolButton* shadow = child<QToolButton>(tm, "rowShadow");
            if (shadow) shadow->click();
            uitest::settle();
            check(presets->findText("Square") < 0, "track motion presets: the 2D Shadow row has presets of its own");
            if (QToolButton* pos = child<QToolButton>(tm, "rowPosition")) pos->click();
            uitest::settle();
            app.edit("3D", [&](Project& p) { p.findTrack(VT)->composite = "3dsrcalpha"; });
            uitest::settle();
            check(presets->findText("Square") < 0, "track motion presets: 3D Track Motion's presets are kept apart from 2D's");
            app.undo();
            uitest::settle();
        }
        // Smoothness and the keyframe types for Track Motion
        app.edit("Keys", [&](Project& p) {
            std::vector<MotionKey> ks(3);
            for (int k = 0; k < 3; ++k) {
                ks[(size_t)k].time = k * 1.0;
                ks[(size_t)k].x = 0.3 + 0.2 * k;
                ks[(size_t)k].y = k == 1 ? 0.3 : 0.6;
                ks[(size_t)k].w = ks[(size_t)k].h = 0.4;
            }
            p.findTrack(VT)->motion = ks;
        });
        app.setCursor(0.0);
        uitest::settle();
        const MotionKey straight = motionKeyAt(track(VT)->motion, 0.5);
        if (QDoubleSpinBox* sm = child<QDoubleSpinBox>(tm, "keySmoothness")) sm->setValue(100);
        uitest::settle();
        const MotionKey curved = motionKeyAt(track(VT)->motion, 0.5);
        check(near(track(VT)->motion[0].smoothness, 100) && !near(curved.x, straight.x, 1e-3), "track motion: Smoothness bends the path between keyframes",
              num(straight.x) + " -> " + num(curved.x));
        uitest::shot(tm, gOut + "/trackmotion_path.png");
        tm->close();
        uitest::settle();
    }

    // ================================================================== Motion Tracking (VEGAS 18's panel)
    {
        FxTarget vt;
        vt.kind = FxTarget::Event;
        vt.id = V;
        const uint64_t overlay = app.addVideoFx(vt, "com.vegascreativesoftware:mirror", "Mirror");  // its Center: a position in the frame
        uitest::settle(200);
        app.selectOnly(V);
        app.setCursor(2.0);
        app.setLoop(1.5, 2.5);
        MotionTrackingWindow mt(&playback);
        mt.resize(800, 600);
        mt.show();
        uitest::settle(200);
        check(child<QWidget>(&mt, "trackClipName") && child<QComboBox>(&mt, "trackPrecision") && child<QComboBox>(&mt, "trackMode") &&
                  child<QWidget>(&mt, "trackVectors") && child<QWidget>(&mt, "trackIndicator") && child<QWidget>(&mt, "trackBackOne") &&
                  child<QWidget>(&mt, "trackBoth") && child<QWidget>(&mt, "trackForwardOne") && child<QWidget>(&mt, "trackTransfer"),
              "motion tracking: VEGAS 18's panel (clip name, precision, mode, motion vectors, indicator, five track buttons, transfer)");
        mt.setPrecision(0);
        check(mt.precision() == 0, "motion tracking: Precision Fast");
        mt.setPrecision(1);
        mt.setBox(0.5, 0.5, 0.2, 0.3);
        const int one = mt.trackOneFrame(1);
        check(one == 1 && mt.points().size() == 2 && near(app.cursor(), app.project().frameTime(app.project().frameAt(2.0) + 1), 1e-6),
              "motion tracking: Track one frame forward (the cursor goes with it)", strfmt("%d frame(s), %d points", one, (int)mt.points().size()));
        mt.clearPoints();
        app.setCursor(2.0);
        mt.setBox(0.5, 0.5, 0.2, 0.3);
        const int bothN = mt.trackBoth();
        bool before = false, after = false;
        for (const auto& pt : mt.points()) {
            before = before || pt.time < 2.0 - 1e-6;
            after = after || pt.time > 2.0 + 1e-6;
        }
        check(bothN > 0 && before && after, "motion tracking: Track both directions goes back and forward from the cursor", strfmt("%d frames", bothN));
        uitest::shot(&mt, gOut + "/motion_tracking.png");
        const std::vector<uint64_t> evs = mt.transferEvents();
        const auto settings = mt.positionSettings(V);
        bool found = false;
        for (const auto& t2 : settings) found = found || (t2.slot == overlay && t2.param == "Center");
        check(!evs.empty() && evs[0] == V && found, "motion tracking: the transfer menu lists the event's effects' position settings",
              strfmt("slot %llu live %d (%s), %d settings, %d events", (unsigned long long)overlay, app.fx.video(overlay) != nullptr, app.fx.error(overlay).c_str(), (int)settings.size(), (int)evs.size()));
        const bool applied = mt.applyToFxParam(V, overlay, "Center");
        ofx::Instance* inst = app.fx.video(overlay);
        ofx::Param* center = inst ? inst->params.find("Center") : nullptr;
        check(applied && center && center->keys.size() >= 2, "motion tracking: transferring the motion keyframes the effect's position",
              center ? std::to_string(center->keys.size()) + " keys" : "no setting");
        app.setLoop(0, 0);
        while (app.canUndo() && app.undoName() != "Add to Timeline") app.undo();
        uitest::settle();
    }

    // ================================================================== the audio engine
    {
        // event effects are heard: Amplitude Modulation on the clip's sound changes it
        const Project before = app.project();
        const double plain = loudness(before, 1.0, 0.5);
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = A;
        const uint64_t slot = app.addAudioFx(t, builtinAudio("reno.ampmod"));
        uitest::settle();
        const double withFx = loudness(app.project(), 1.0, 0.5);
        check(slot && plain > 0 && std::fabs(withFx - plain) > plain * 0.02, "audio event FX: an event's effects change its sound", num(plain) + " -> " + num(withFx));
        // Bypass All Audio FX leaves them out
        {
            static MediaPool pool;
            pool.setOpener(app.mediaOpener);
            AudioEngine eng(pool, app.fx);
            eng.bypassFx = true;
            const double bypassed = loudness(app.project(), 1.0, 0.5, &eng);
            check(near(bypassed, plain, plain * 1e-4), "Bypass All Audio FX: the mix is as without effects", num(bypassed));
        }
        app.undo();
        uitest::settle();
        // a bus with a gain-changing effect: pre and post fader differ when the bus is turned down (Distortion
        // clips loud sound harder than quiet)
        uint64_t bus = 0;
        app.edit("Bus", [&](Project& p) {
            Bus b;
            b.id = bus = p.newId();
            b.name = "Bus A";
            b.level = 0.1;
            p.buses.push_back(b);
            p.findTrack(AT)->output = bus;
        });
        FxTarget bt;
        bt.kind = FxTarget::Bus;
        bt.id = bus;
        app.addAudioFx(bt, builtinAudio("reno.distortion"));
        uitest::settle();
        const double pre = loudness(app.project(), 1.0, 0.5);
        app.edit("Post", [&](Project& p) { p.findBus(bus)->fxPostFader = true; });
        const double post = loudness(app.project(), 1.0, 0.5);
        check(pre > 0 && post > 0 && std::fabs(pre - post) > 0.02 * pre, "bus Post Fader Insert FX: the effects after the fader give another mix", num(pre) + " / " + num(post));
        app.edit("Master post", [&](Project& p) { p.masterFxPostFader = true; });
        Project j;
        j.fromJson(app.project().toJson());
        check(j.findBus(bus) && j.findBus(bus)->fxPostFader && j.masterFxPostFader, "the project keeps pre/post fader effects");
        while (app.canUndo() && app.undoName() != "Add to Timeline") app.undo();
        uitest::settle();
    }

    // ================================================================== the Mixing Console
    {
        MixerWindow mixer(&playback);
        FxTarget opened;
        mixer.onOpenFx = [&](const FxTarget& t, uint64_t) { opened = t; };
        mixer.resize(1200, 640);
        mixer.show();
        uitest::settle();
        QToolButton* insAssign = child<QToolButton>(&mixer, "insertAssignableFx");
        check(insAssign && insAssign->isEnabled(), "console: Insert Assignable FX is on the tool bar");
        if (insAssign) insAssign->click();
        uitest::settle(200);
        const Bus* fx1 = app.project().buses.empty() ? nullptr : &app.project().buses.back();
        check(fx1 && fx1->assignable && fx1->name == "FX 1" && opened.kind == FxTarget::Bus && opened.id == fx1->id,
              "console: Insert Assignable FX makes FX 1 and asks for its effects", fx1 ? fx1->name : "none");
        const uint64_t fxId = fx1 ? fx1->id : 0;
        if (QToolButton* insBus = child<QToolButton>(&mixer, "insertBus")) insBus->click();
        uitest::settle(200);
        const uint64_t busId = app.project().buses.back().id;
        QWidget* tstrip = mixer.findChild<QWidget*>(QString("strip:track:%1").arg(AT));
        QWidget* fstrip = mixer.findChild<QWidget*>(QString("strip:bus:%1").arg(fxId));
        check(tstrip && fstrip, "console: a strip for the assignable FX chain");
        if (tstrip) {
            QComboBox* out = tstrip->findChild<QComboBox*>("output");
            bool hasFx = false, hasBus = false;
            for (int i = 0; out && i < out->count(); ++i) {
                hasFx = hasFx || out->itemData(i).toULongLong() == fxId;
                hasBus = hasBus || out->itemData(i).toULongLong() == busId;
            }
            check(out && hasBus && !hasFx, "console: a track's output can be a bus but not an assignable FX chain");
            // sends: to the bus pre volume, to the chain post volume (VEGAS's defaults)
            QWidget* toBus = tstrip->findChild<QWidget*>(QString("send:%1").arg(busId));
            QWidget* toFx = tstrip->findChild<QWidget*>(QString("send:%1").arg(fxId));
            if (toBus && toFx) {
                for (QWidget* w : {toBus, toFx}) {
                    const QRect bar = w->property("bar").toRect();
                    uitest::mouseClick(w, QPoint(bar.left() + bar.width() * 3 / 4, bar.center().y()));
                    uitest::settle();
                }
                bool preBus = false, preFx = true;
                for (const Send& s : track(AT)->sends) {
                    if (s.bus == busId) preBus = s.preFader;
                    if (s.bus == fxId) preFx = s.preFader;
                }
                check(preBus && !preFx, "console: a new send to a bus is pre volume, to an assignable FX chain post volume");
            } else {
                check(false, "console: the track strip has a send to the bus and one to the chain");
            }
            // Invert Track Phase
            if (QToolButton* ph = tstrip->findChild<QToolButton*>("invertPhase")) {
                ph->click();
                uitest::settle();
                check(track(AT)->invertPhase && app.undoName() == "Invert Track Phase", "console: the track strip's Invert Phase button");
                app.undo();
            } else {
                check(false, "console: the track strip has an Invert Phase button");
            }
        }
        // the bus strip's Pre/Post FX button
        if (QWidget* bstrip = mixer.findChild<QWidget*>(QString("strip:bus:%1").arg(busId))) {
            QToolButton* pp = bstrip->findChild<QToolButton*>("prePostFx");
            if (pp) pp->click();
            uitest::settle();
            check(pp && app.project().findBus(busId)->fxPostFader && pp->text() == "Post", "console: the bus strip's Pre/Post Fader Insert FX button");
        }
        // the insert FX region: right-click an effect for Show / Bypass / Remove / Presets and other plug-ins; a
        // click on an empty row lists the plug-ins to add
        if (tstrip) {
            FxTarget t;
            t.kind = FxTarget::Track;
            t.id = AT;
            const uint64_t slot = app.addAudioFx(t, builtinAudio("reno.reverb"));
            uitest::settle();
            QWidget* fxRows = tstrip->findChild<QWidget*>("fxSlots");
            QStringList items;
            QStringList adds;
            // the console's menus go to the test hook: the right-click one is read and its Bypass chosen
            mixerMenuHookForTests() = [&](QMenu* m) -> QAction* {
                items = texts(m);
                return find(m, "Bypass Reverb");
            };
            if (fxRows) uitest::mouseClick(fxRows, QPoint(30, 7), Qt::RightButton);
            uitest::settle();
            const FxSlot* s = nullptr;
            for (const FxSlot& x : track(AT)->fx)
                if (x.id == slot) s = &x;
            check(items.contains("Show Reverb") && items.contains("Bypass Reverb") && items.contains("Remove Reverb") && items.contains("Presets") && s && s->bypass,
                  "console: an insert effect's right-click menu (Show, Bypass, Remove, Presets, other plug-ins); Bypass bypasses it", items.join(", ").toStdString());
            mixerMenuHookForTests() = [&](QMenu* m) -> QAction* {
                adds = texts(m);
                for (QAction* a : m->actions())
                    if (a->menu())
                        for (QAction* b : a->menu()->actions())
                            if (b->text().startsWith("Chorus")) return b;
                return nullptr;
            };
            if (fxRows) uitest::mouseClick(fxRows, QPoint(30, 22));
            uitest::settle();
            check(adds.contains("Plug-In Chain...") && track(AT)->fx.size() == 2 && track(AT)->fx[1].name == "Chorus",
                  "console: a click on an empty insert row offers the plug-ins (Add New Insert FX); choosing one adds it", adds.join(", ").toStdString());
            // the FX button's right-click: Bypass All
            mixerMenuHookForTests() = [&](QMenu* m) -> QAction* { return find(m, "Bypass All"); };
            if (QToolButton* fxb = tstrip->findChild<QToolButton*>("fx")) emit fxb->customContextMenuRequested(QPoint(4, 4));
            uitest::settle();
            bool allBypassed = !track(AT)->fx.empty();
            for (const FxSlot& x : track(AT)->fx) allBypassed = allBypassed && x.bypass;
            check(allBypassed, "console: the FX button's right-click Bypass All");
            mixerMenuHookForTests() = nullptr;
        }
        // the Downmix menu
        {
            QMenu m;
            fillDownmixMenu(&m, &playback);
            QStringList t = texts(&m);
            check(t == QStringList({"5.1 Surround Output", "Stereo Output", "Mono Output"}) && !find(&m, "5.1 Surround Output")->isEnabled(),
                  "Downmix Output: 5.1 (greyed in a stereo project), stereo, mono", t.join(", ").toStdString());
            find(&m, "Mono Output")->trigger();
            check(playback.monoOutput(), "Downmix Output: Mono");
            playback.setMonoOutput(false);
        }
        // the view controls show and hide the assignable strips
        if (QToolButton* sa = child<QToolButton>(&mixer, "showAssignable")) {
            check(sa->isEnabled() && sa->isChecked(), "console: the View Controls' Assign. FX button works");
            sa->click();
            uitest::settle();
            check(fstrip && !fstrip->isVisible(), "console: ... and hides the assignable FX strips");
            sa->click();
            uitest::settle();
        }
        // the VU Meters region: shown from the View Controls, moving while playing; its menu has VEGAS's kinds
        if (QToolButton* vuBtn = child<QToolButton>(&mixer, "regionVu")) {
            vuBtn->click();
            uitest::settle();
            QWidget* vu = tstrip ? tstrip->findChild<QWidget*>("vuMeter") : nullptr;
            app.setCursor(1.0);
            playback.play();
            uitest::settle(900);
            const double lvl = vu ? vu->property("vuLeft").toDouble() : -200;
            uitest::shot(&mixer, gOut + "/mixer_vu.png");
            playback.stop();
            uitest::settle();
            check(vu && vu->isVisible() && lvl > -100, "console: the VU Meters region shows the track's level while playing", num(lvl));
            QStringList t;
            mixerMenuHookForTests() = [&](QMenu* m) -> QAction* {
                t = texts(m);
                return find(m, "EBU PPM");
            };
            if (vu) {
                uitest::mouseClick(vu, QPoint(20, 20), Qt::RightButton);
                uitest::settle();
            }
            mixerMenuHookForTests() = nullptr;
            check(t.contains("Show VU/PPM") && t.contains("Reset Clip") && t.contains("Traditional VU") && t.contains("Extended VU") && t.contains("Logarithmic VU") &&
                      t.contains("UK PPM") && t.contains("EBU PPM") && t.contains("DIN PPM") && t.contains("Nordic PPM") && t.contains("Show Labels") &&
                      t.contains("Show Peaks") && t.contains("Hold Peaks") && t.contains("Hold Valleys") && t.contains("Vintage Mode"),
                  "console: the VU meter's menu (VEGAS's items)", t.join(", ").toStdString());
            check(QSettings().value("mixer/vu").toString().startsWith("4 "), "console: choosing EBU PPM changes the meters (and is kept)", QSettings().value("mixer/vu").toString().toStdString());
        }
        uitest::shot(&mixer, gOut + "/mixer.png");
        // Remove Assignable FX
        {
            QMenu m;
            fillRemoveAssignableMenu(&m);
            if (QAction* a = find(&m, "FX 1")) a->trigger();
            uitest::settle();
            bool gone = !app.project().findBus(fxId);
            for (const Send& s : track(AT)->sends) gone = gone && s.bus != fxId;
            check(gone, "Tools > Audio > Remove Assignable FX deletes the chain and the sends to it");
        }
        // the chain menu (track header FX button, console): Delete All
        {
            FxTarget t;
            t.kind = FxTarget::Track;
            t.id = AT;
            QMenu m;
            fillChainMenu(&m, t);
            check(texts(&m) == QStringList({"Enable All", "Bypass All", "Delete All"}), "the FX button's menu: Enable All, Bypass All, Delete All");
            find(&m, "Enable All")->trigger();
            bool on = true;
            for (const FxSlot& x : track(AT)->fx) on = on && !x.bypass;
            find(&m, "Delete All")->trigger();
            check(on && track(AT)->fx.empty(), "the FX button's menu: Enable All, then Delete All");
        }
        while (app.canUndo() && app.undoName() != "Add to Timeline") app.undo();
        uitest::settle();
    }

    // ================================================================== Real-Time Render, Non-Real-Time Event FX
    {
        const std::string wav = QDir(gOut).filePath("realtime.wav").toStdString();
        QFile::remove(QString::fromStdString(wav));
        std::string e2;
        const bool ok = realTimeRenderTo(wav, &e2);
        std::unique_ptr<MediaReader> r = ok ? openMedia(wav, &e2) : nullptr;
        check(ok && r && r->info().hasAudio && !r->info().hasVideo && near(r->info().duration, app.project().length(), 0.1),
              "File > Real-Time Render: the project's sound in a .wav as long as the project", e2);
        // Non-Real-Time Event FX: the clip's sound through Amplitude Modulation, into a new file, as a new take
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = A;
        const uint64_t slot = app.addAudioFx(t, builtinAudio("reno.ampmod"));
        uitest::settle();
        FxChain chain;
        for (const FxSlot& s : ev(A)->fx)
            if (s.id == slot) chain.push_back(s);
        const std::string out = QDir(gOut).filePath("nrt.wav").toStdString();
        QFile::remove(QString::fromStdString(out));
        const uint64_t oldMedia = ev(A)->mediaId;
        const bool done = applyNonRealTimeFx(A, chain, out, &e2);
        const Event* e = ev(A);
        const MediaItem* nm = e ? app.project().findMedia(e->mediaId) : nullptr;
        check(done && e && e->mediaId != oldMedia && nm && QFileInfo(QString::fromStdString(nm->path)) == QFileInfo(QString::fromStdString(out)) && e->fx.empty() &&
                  e->takes.size() == 1 && e->takes[0].mediaId == oldMedia && app.undoName() == "Apply Non-Real-Time Event FX",
              "Apply Non-Real-Time Event FX: a new file, the event's new active take; the effects are not kept on the event", e2);
        check(nm && near(nm->info.duration, e->length, 0.1), "Apply Non-Real-Time Event FX: the file is as long as the event", nm ? num(nm->info.duration) : "");
    }

    // ================================================================== the main window's menus
    {
        MainWindow w;
        w.resize(1280, 800);
        uitest::settle(300);
        auto action = [&](const char* name) { return w.findChild<QAction*>(name); };
        QAction* bypass = action("bypassAudioFx");
        check(action("realTimeRender") && action("nonRealTimeFx") && action("assignableFx") && action("inputBus") && !action("inputBus")->isEnabled() && bypass,
              "menus: Real-Time Render, Apply Non-Real-Time Event FX, Audio Assignable FX, Input Bus (not available), Bypass All Audio FX");
        if (bypass) {
            bypass->trigger();
            check(app.bypassAudioFx.load(), "Options > Bypass All Audio FX turns the bypass on");
            bypass->trigger();
        }
        check(w.findChild<QMenu*>("removeAssignableMenu") != nullptr, "menus: Tools > Audio > Remove Assignable FX");
        // the timeline: a video track's Track Motion keyframes along the bottom of its lane
        TimelineWidget* tl = w.timeline();
        app.edit("Keys", [&](Project& p) {
            std::vector<MotionKey> ks(2);
            ks[0].time = 1.0;
            ks[1].time = 2.0;
            ks[1].x = 0.7;
            p.findTrack(VT)->motion = ks;
        });
        uitest::settle(200);
        if (tl) {
            const QRect tr = tl->trackRect(VT);
            const int y = tr.top() + tr.height() - 8;
            const QPoint k1(tl->xAt(1.0), y);
            uitest::shot(tl, gOut + "/timeline_trackkeys.png");
            QMenu m;
            const bool built = tl->buildContextMenu(&m, k1);
            const QStringList t = texts(&m);
            check(built && t.contains("Hold") && t.contains("Fast") && t.contains("Smooth") && t.contains("Delete") && t.contains("Track Motion..."),
                  "timeline: a track keyframe's right-click menu (its types, Delete, Track Motion)", t.join(", ").toStdString());
            if (QAction* f = find(&m, "Fast")) f->trigger();
            uitest::settle();
            check(track(VT)->motion[0].curve == KeyFast, "timeline: ... Fast sets the keyframe's type");
            drag(tl, k1, QPoint(tl->xAt(1.5), y));
            check(near(track(VT)->motion[0].time, 1.5, 0.05) && app.undoName() == "Move Track Keyframe", "timeline: dragging a track keyframe moves it",
                  num(track(VT)->motion[0].time));
        } else {
            check(false, "timeline: the main window has one");
        }
    }
    return uitest::summary();
}
