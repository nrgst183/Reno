// VEGAS 18's layout of three windows: the Surround Panner (tool bar with the track, the Move tool and LFE; the speakers'
// levels and the pan type in the room; Center and Smoothness only), the Motion Tracking panel (clip name, Mask, Track
// outside Region, Precision, Mode, Show Motion Vectors, the motion track indicator and status, the six icon buttons) and
// Track Motion's tool bars (Preset row, the compositing and layout drop-downs, the icon buttons, the keyframe rows with
// 2D Shadow and 2D Glow check boxes, and the list's groups).
//
//   dlgb_vegasui_test <output folder>
//
// Build: abuild dlgb tests/ui/dlgb_vegasui_test.cpp src/app/surroundpanner.cpp src/app/motiontracking.cpp src/app/editdialogs.cpp
// Offscreen. The folder gets the pictures, to be compared with the VEGAS 18 manual's (p0404_x20678, p0273_x15379..15417,
// p0264_x14839, p0355_x18980, p0263_x14806).
#include "../../src/app/surroundpanner.h"
#include "../../src/app/motiontracking.h"
#include "../../src/app/editdialogs.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QStyleFactory>
#include <QTimer>
#include <QToolButton>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
AppState& st() { return AppState::get(); }

template <class T>
T* child(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

QWidget* topLevel(const char* name)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->objectName() == name && w->isVisible()) return w;
    return nullptr;
}

void theme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(220, 220, 220), mid(62, 62, 66), accent(0, 122, 204);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, mid);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    QApplication::setPalette(p);
}

// The texts of a menu's actions (and its submenus' titles), as shown.
QStringList menuTexts(QMenu* m)
{
    QStringList out;
    if (!m) return out;
    for (QAction* a : m->actions())
        if (!a->isSeparator()) out << a->text().remove('&');
    return out;
}

// Opens a tool button's menu (filled on aboutToShow), reads it and closes it.
QStringList openMenu(QToolButton* b)
{
    QStringList texts;
    if (!b || !b->menu()) return texts;
    QMenu* m = b->menu();
    QTimer::singleShot(150, [&] {
        texts = menuTexts(m);
        m->close();
    });
    m->exec(b->mapToGlobal(QPoint(0, b->height())));
    return texts;
}

bool visibleNamed(QWidget* w, const char* type, const QString& text)
{
    for (QWidget* c : w->findChildren<QWidget*>()) {
        if (!c->isVisibleTo(w) || !c->inherits(type)) continue;
        QString t;
        if (QLabel* l = qobject_cast<QLabel*>(c)) t = l->text();
        else if (QAbstractButton* b = qobject_cast<QAbstractButton*>(c)) t = b->text();
        if (t.remove('&') == text) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------- the Surround Panner
void surroundPanner()
{
    AppState& app = st();
    app.newProject();
    std::string err;
    const uint64_t m1 = app.importMedia("synthaudio:1", &err);
    app.addMediaToTimeline(m1, 0.0);
    uint64_t t1 = 0;
    for (const auto& t : app.project().tracks)
        if (t.type == TrackType::Audio) t1 = t.id;
    app.edit("5.1", [&](Project& p) {
        p.settings.audioChannels = 6;
        setSurroundPanType(p, t1, "power");
    });
    app.currentTrack = t1;
    SurroundPannerWindow win;
    win.resize(258, 344);
    win.show();
    uitest::settle(100);
    SurroundField* f = win.field();
    QToolButton* trackBtn = child<QToolButton>(&win, "surroundTargetButton");
    QToolButton* move = child<QToolButton>(&win, "surroundMoveTool");
    QToolButton* lfe = child<QToolButton>(&win, "surroundLfeButton");
    check(trackBtn && move && lfe && trackBtn->isVisible() && move->isVisible() && lfe->isVisible(),
          "surround panner: VEGAS's tool bar (the track, the Move tool, LFE)");
    bool noCombo = true;
    for (QComboBox* c : win.findChildren<QComboBox*>()) noCombo = noCombo && !c->isVisible();
    check(noCombo && !child<QCheckBox>(&win, "surroundLfe")->isVisible() && !child<QPushButton>(&win, "surroundAddKey")->isVisible(),
          "no drop-downs, LFE check box or keyframe buttons in the window (VEGAS 18 has none)");
    check(visibleNamed(&win, "QLabel", "Center:") && visibleNamed(&win, "QLabel", "Smoothness:") && !visibleNamed(&win, "QLabel", "Motion:"),
          "under the room only Center and Smoothness");
    // the room: the speakers' levels and the pan type's name
    f->grab();
    const QStringList levels = f->property("levels").toStringList();
    check(levels.size() == 5 && f->property("panTypeName").toString() == "Constant Power", "the room writes the five speakers' levels and the pan type",
          levels.join(" ").toStdString() + " / " + f->property("panTypeName").toString().toStdString());
    uitest::key(f, Qt::Key_7, Qt::KeypadModifier);
    f->grab();
    check(f->property("levels").toStringList().value(4) == "-Inf." && f->property("levels").toStringList().value(0) != "-Inf.",
          "front left: the rear right speaker gets -Inf.", f->property("levels").toStringList().join(" ").toStdString());
    uitest::key(f, Qt::Key_5, Qt::KeypadModifier);
    uitest::key(f, Qt::Key_Up);
    uitest::key(f, Qt::Key_Up);
    uitest::settle(30);
    uitest::shot(&win, gOut + "/surround_panner.png");
    // the Move tool: a click goes on to the next motion
    const SurroundPan* sp = surroundPanOf(app.project(), t1);
    move->click();
    check(sp && surroundPanOf(app.project(), t1)->motion == "x", "a click on the Move tool: Move Left/Right Only");
    move->click();
    check(surroundPanOf(app.project(), t1)->motion == "y", "again: Move Front/Back Only");
    move->click();
    check(surroundPanOf(app.project(), t1)->motion == "free", "and again: Move Freely");
    const QStringList items = openMenu(move);
    check(items.contains("Move Freely") && items.contains("Move Left/Right Only") && items.contains("Move Front/Back Only") && items.contains("Pan Type") &&
              items.contains("Route") && items.contains("Enable LFE") && items.contains("Add Keyframe") && items.contains("Delete Keyframe") &&
              items.contains("Clear Keyframes"),
          "the Move tool's arrow: the motions, Pan Type, Route, Enable LFE and the keyframes", items.join(", ").toStdString());
    // LFE: VEGAS's LFE mode (LFE Only) and back
    lfe->click();
    check(surroundPanOf(app.project(), t1)->route == "lfe" && lfe->isChecked(), "LFE sends the sound to the LFE only");
    f->grab();
    check(f->property("panTypeName").toString() == "LFE Only", "and the room says so");
    uitest::shot(&win, gOut + "/surround_panner_lfe.png");
    lfe->click();
    check(surroundPanOf(app.project(), t1)->route == "surround", "a second click: surround panning again");
    // keyframes from the menu, the path drawn in the room
    app.setCursor(0.0);
    child<QPushButton>(&win, "surroundAddKey")->click();
    app.setCursor(1.0);
    uitest::key(f, Qt::Key_9, Qt::KeypadModifier);
    app.setCursor(2.0);
    uitest::key(f, Qt::Key_3, Qt::KeypadModifier);
    app.setCursor(1.5);
    uitest::settle(30);
    check(surroundPanOf(app.project(), t1)->keys.size() == 3, "keyframes are made at the cursor");
    uitest::shot(&win, gOut + "/surround_panner_path.png");
    // the track button's menu: the tracks and buses
    const QStringList targets = openMenu(trackBtn);
    check(targets.size() >= 2 && targets[0] == "(the current track)", "the track button lists the panners to show", targets.join(", ").toStdString());
}

// ---------------------------------------------------------------------------------- Motion Tracking
void motionTracking(Playback* pb)
{
    AppState& app = st();
    app.newProject();
    std::string err;
    const std::vector<uint64_t> made = app.addMediaToTimeline(app.importMedia("synth:1", &err), 0.0, 0);
    uint64_t ev = 0;
    for (uint64_t id : made) {
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = id;
        if (app.targetIsVideo(t)) ev = id;
    }
    check(ev != 0, "motion tracking: a video event to track", err);
    app.edit("Name", [&](Project& p) { if (Event* e = p.findEvent(ev)) e->name = "clip01"; });
    app.selectOnly(ev);
    app.setCursor(1.0);
    MotionTrackingWindow mt(pb);
    mt.resize(256, 520);
    mt.show();
    uitest::settle(150);
    QLabel* name = child<QLabel>(&mt, "trackClipName");
    check(name && name->text() == "clip01" && name->font().bold(), "the clip's name in bold, nothing before it", name ? name->text().toStdString() : "");
    QComboBox* mask = child<QComboBox>(&mt, "trackMask");
    QToolButton* add = child<QToolButton>(&mt, "trackAddMask");
    QToolButton* remove = child<QToolButton>(&mt, "trackRemoveMask");
    check(mask && add && remove && visibleNamed(&mt, "QLabel", "Mask:") && mask->count() == 0 && add->isEnabled() && !remove->isEnabled(),
          "Mask: with Add Mask and Remove Mask (no region yet)");
    add->click();
    uitest::settle(30);
    check(mask->count() == 1 && mask->currentText() == "Region 1" && !add->isEnabled() && remove->isEnabled(), "Add Mask makes Region 1");
    check(visibleNamed(&mt, "QCheckBox", "Track outside Region") && visibleNamed(&mt, "QLabel", "Precision:") && visibleNamed(&mt, "QLabel", "Mode:") &&
              visibleNamed(&mt, "QCheckBox", "Show Motion Vectors") && visibleNamed(&mt, "QLabel", "Motion tracked:"),
          "Track outside Region, Precision, Mode, Show Motion Vectors and Motion tracked:");
    // the six buttons: icons in a row, in VEGAS's order
    const char* ids[6] = {"trackBackward", "trackBackOne", "trackBoth", "trackForwardOne", "trackForward", "trackTransfer"};
    bool icons = true;
    int lastX = -1;
    for (const char* id : ids) {
        QToolButton* b = child<QToolButton>(&mt, id);
        icons = icons && b && !b->icon().isNull() && b->text().isEmpty() && b->isVisible() && b->x() > lastX;
        if (b) lastX = b->x();
    }
    check(icons, "six icon buttons: backward, one frame backward, both, one frame forward, forward, motion transfer");
    check(!visibleNamed(&mt, "QPushButton", "Clear") && !visibleNamed(&mt, "QPushButton", "Apply") && !visibleNamed(&mt, "QLabel", "Apply to:"),
          "no Clear, Apply to or Apply in the panel");
    app.setLoop(0.5, 1.5);
    const int n = mt.trackBoth();
    app.setLoop(0, 0);
    QLabel* status = child<QLabel>(&mt, "trackStatus");
    check(n > 0 && status && status->text() == "Successful", "tracking both ways: Successful", status ? status->text().toStdString() : "");
    const QStringList menu = openMenu(child<QToolButton>(&mt, "trackTransfer"));
    check(menu.contains("Clear Motion Data") && menu.contains("Show Picture") && menu.join("|").contains("clip01"),
          "the transfer menu has the events, Clear Motion Data and Show Picture", menu.join(", ").toStdString());
    uitest::shot(&mt, gOut + "/motion_tracking.png");
    remove->click();
    check(mask->count() == 0 && mt.points().empty(), "Remove Mask takes the region and its motion data away");
}

// ---------------------------------------------------------------------------------- Track Motion
void trackMotion(Playback* pb)
{
    AppState& app = st();
    app.newProject();
    std::string err;
    app.addMediaToTimeline(app.importMedia("synth:1", &err), 0.0, 0);
    uint64_t vt = 0;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Video) vt = t.id;
    QWidget host;
    openTrackMotion(&host, pb, vt);
    uitest::settle(150);
    QPointer<QWidget> tm = topLevel("trackMotionWindow");
    check(tm != nullptr, "track motion opens");
    if (!tm) return;
    tm->resize(646, 515);
    uitest::settle(100);
    QComboBox* presets = child<QComboBox>(tm, "motionPreset");
    QComboBox* mode = child<QComboBox>(tm, "compositeMode");
    QComboBox* layout = child<QComboBox>(tm, "layout3d");
    QToolButton* save = child<QToolButton>(tm, "savePreset");
    QToolButton* del = child<QToolButton>(tm, "deletePreset");
    check(presets && save && del && !save->icon().isNull() && !del->icon().isNull() && presets->y() < mode->y(),
          "the Preset row on top (with Save and Delete icons), the compositing mode under it");
    check(!visibleNamed(tm, "QLabel", "Compositing Mode:") && mode->currentText() == "Source Alpha", "the mode with no label: Source Alpha");
    bool iconOnly = true;
    for (const char* id : {"enableRotation", "enableSnapping", "objectSpace", "preventMoveX", "preventMoveY", "lockAspect", "sizeAboutCenter", "preventScaleX",
                           "preventScaleY"}) {
        QToolButton* b = child<QToolButton>(tm, id);
        iconOnly = iconOnly && b && b->isVisible() && !b->icon().isNull() && b->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                   std::abs(b->y() - mode->y()) < 12;
    }
    check(iconOnly, "Rotation, Snap, Object Space, Prevent Movement, Lock, Center, Prevent Scaling: icons beside the mode");
    QWidget* names = child<QWidget>(tm, "keyRowNames");
    QCheckBox* shadowOn = child<QCheckBox>(tm, "shadowOn");
    QCheckBox* glowOn = child<QCheckBox>(tm, "glowOn");
    check(names && names->isVisible() && shadowOn && glowOn && shadowOn->isVisible() && child<QToolButton>(tm, "rowShadow")->isVisible() &&
              child<QToolButton>(tm, "rowShadow")->text() == "2D Shadow",
          "the keyframe rows' names: Position, 2D Shadow and 2D Glow with check boxes");
    // the list: X, Y, Width, Height; Orientation; Rotation's Angle and offsets
    QStringList groups;
    for (QLabel* l : tm->findChildren<QLabel*>("propGroup"))
        if (l->isVisible()) groups << l->text();
    check(groups.size() >= 3 && groups[0] == "Position" && groups[1] == "Orientation" && groups[2] == "Rotation" && visibleNamed(tm, "QLabel", "X") &&
              visibleNamed(tm, "QLabel", "X Offset"),
          "the list: Position (X, Y, Width, Height), Orientation, Rotation (Angle, X Offset, Y Offset)", groups.join(", ").toStdString());
    uitest::shot(tm, gOut + "/track_motion.png");
    // 2D Shadow: its check box turns it on, the list shows the 2D Shadow group first
    shadowOn->click();
    uitest::settle(50);
    const Track* t = const_cast<Project&>(app.project()).findTrack(vt);
    groups.clear();
    for (QLabel* l : tm->findChildren<QLabel*>("propGroup"))
        if (l->isVisible()) groups << l->text();
    check(t && !t->shadow2d.empty() && tm->property("row").toInt() == 1 && groups.value(0) == "2D Shadow" && visibleNamed(tm, "QLabel", "Blur(%)") &&
              visibleNamed(tm, "QLabel", "Intensity(%)") && visibleNamed(tm, "QLabel", "Color"),
          "the 2D Shadow check box: on, its row picked, the 2D Shadow group (Blur(%), Intensity(%), Color) first", groups.join(", ").toStdString());
    uitest::shot(tm, gOut + "/track_motion_shadow.png");
    child<QToolButton>(tm, "rowGlow")->click();
    uitest::settle(30);
    groups.clear();
    for (QLabel* l : tm->findChildren<QLabel*>("propGroup"))
        if (l->isVisible()) groups << l->text();
    check(tm->property("row").toInt() == 2 && groups.value(0) == "2D Glow" && !glowOn->isChecked(), "2D Glow's name picks its row (off)");
    shadowOn->click();
    uitest::settle(30);
    check(const_cast<Project&>(app.project()).findTrack(vt)->shadow2d.empty(), "and off again");
    child<QToolButton>(tm, "rowPosition")->click();
    uitest::settle(30);
    // X and Y from the frame's middle
    QDoubleSpinBox* x = child<QDoubleSpinBox>(tm, "boxX");
    check(x && std::fabs(x->value()) < 0.01, "X is 0 with the picture in the middle", x ? std::to_string(x->value()) : "");
    // 3D Source Alpha: the layout drop-down, no number before its items
    mode->setCurrentIndex(mode->findData("3dsrcalpha"));
    uitest::settle(80);
    check(layout && layout->isVisible() && layout->itemText(0) == "Single Pane" && static_cast<QWidget*>(layout)->x() > static_cast<QWidget*>(mode)->x() &&
              !shadowOn->isVisible() && child<QToolButton>(tm, "preventMoveZ")->isVisible(),
          "3D Source Alpha: Layout (Single Pane) after the mode, Prevent Movement Z; no 2D Shadow or Glow rows");
    uitest::key(tm, Qt::Key_4);
    uitest::settle(30);
    check(layout->currentIndex() == 3 && layout->currentText() == "Four Panes", "key 4: Four Panes");
    uitest::key(tm, Qt::Key_1);
    uitest::settle(30);
    uitest::shot(tm, gOut + "/track_motion_3d.png");
    tm->close();
    uitest::settle(50);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    gOut = argc > 1 ? QString(argv[1]) : QString("dlgb_vegasui_out");
    QDir().mkpath(gOut);
    theme();
    uitest::setup({}, {});
    surroundPanner();
    Playback pb;
    motionTracking(&pb);
    trackMotion(&pb);
    return uitest::summary();
}
