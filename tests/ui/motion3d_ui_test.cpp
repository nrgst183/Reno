// Track Motion in 3D Source Alpha (3D workspace, layouts and panes, 3D keys 1-6 / Z / Shift+Z, 3D values), Parent Motion and the
// track menu's compositing parent/child commands, the Trimmer's markers and regions (M, R, S, Shift+S) and Show Video in Preview
// Window (Ctrl+P), with the saved markers on the media's events.
//
//   motion3d_ui_test <output folder>
//
// Build: uibuild motion3d_ui_test tests/ui/motion3d_ui_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// (with EXTRA_CFLAGS for the Qt private headers, as keys_test). QT_QPA_PLATFORM=offscreen.
#include "../../src/app/editdialogs.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/mainwindow_tools.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/trimmer.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QToolButton>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
QString gOut;
std::string num(double v) { return strfmt("%.4f", v); }
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

QWidget* topLevel(const char* name)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->objectName() == name && w->isVisible()) return w;
    return nullptr;
}
template <class T>
T* child(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

const Track* track(uint64_t id) { return const_cast<Project&>(st().project()).findTrack(id); }

std::vector<QWidget*> panesOf(QWidget* win)
{
    std::vector<QWidget*> v;
    if (QWidget* view = child<QWidget>(win, "view3d"))
        for (QWidget* p : view->findChildren<QWidget*>("pane3d"))
            if (!p->property("deleting").toBool()) v.push_back(p);
    return v;
}
// Where a pane draws the box (painted first so it is current).
QPointF centreIn(QWidget* pane)
{
    pane->grab();
    return pane->property("boxCentre").toPointF();
}
QPointF cornerIn(QWidget* pane, int i)
{
    pane->grab();
    return pane->property(qPrintable(QString("corner%1").arg(i))).toPointF();
}
void drag(QWidget* w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const QPoint a = from.toPoint(), b = to.toPoint();
    uitest::mousePress(w, a, Qt::LeftButton, mods);
    for (int k = 1; k <= 8; ++k) uitest::mouseMove(w, a + (b - a) * k / 8, Qt::LeftButton, mods);
    uitest::mouseRelease(w, b, Qt::LeftButton, mods);
    uitest::settle(30);
}
void pick(QComboBox* c, const QString& data)
{
    const int i = c->findData(data);
    if (i >= 0) c->setCurrentIndex(i);
    uitest::settle(30);
}
QAction* findAction(QMenu* m, const QString& text)
{
    for (QAction* a : m->actions()) {
        if (a->text().split('\t').first() == text) return a;
        if (a->menu())
            if (QAction* x = findAction(a->menu(), text)) return x;
    }
    return nullptr;
}
QMenu* findSubmenu(QMenu* m, const QString& text)
{
    for (QAction* a : m->actions())
        if (a->menu() && a->text() == text) return a->menu();
    return nullptr;
}
double meanDiff(const QImage& a0, const QImage& b0)
{
    if (a0.isNull() || b0.isNull()) return -1;
    const QImage a = a0.convertToFormat(QImage::Format_RGB32), b = b0.convertToFormat(QImage::Format_RGB32).scaled(a.size());
    double sum = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
        }
    return sum / (3.0 * a.width() * a.height());
}

// Answers the 2D / 3D conversion question when it comes up (the dialog is modal: its loop runs the timer).
void answerConversion(const char* button, bool dontShowAgain = false)
{
    QTimer::singleShot(150, [button, dontShowAgain] {
        QWidget* d = topLevel("motionConversionDialog");
        if (!d) return;
        if (dontShowAgain) child<QCheckBox>(d, "dontShowAgain")->setChecked(true);
        if (QPushButton* b = child<QPushButton>(d, button)) b->click();
    });
}

uint64_t clip = 0, V1 = 0, V2 = 0, V3 = 0;

void buildProject()
{
    st().newProject();
    std::string err;
    clip = st().importMedia("synth:1", &err);
    st().edit("Build", [&](Project& p) {
        p.settings.width = 640;
        p.settings.height = 360;
        for (int k = 0; k < 3; ++k) {
            Track t;
            t.id = p.newId();
            t.type = TrackType::Video;
            t.name = strfmt("Video %d", k + 1);
            Event e;
            e.id = p.newId();
            e.mediaId = clip;
            e.length = 4;
            t.events.push_back(e);
            p.tracks.push_back(t);
        }
        V1 = p.tracks[0].id;
        V2 = p.tracks[1].id;
        V3 = p.tracks[2].id;
    });
    st().setCursor(1.0);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMotion3dTest");
    QApplication::setApplicationName("motion3d_ui_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = QDir(args.size() > 1 ? args[1] : QString("motion3dout")).absolutePath();
    QDir().mkpath(gOut);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    buildProject();
    MainWindow w;
    w.resize(1600, 900);
    w.show();
    w.activateWindow();
    uitest::settle(300);
    buildProject();
    Playback* pb = w.playback();

    // ================================================================================== 3D Track Motion
    openTrackMotion(&w, pb, V2);
    uitest::settle(100);
    QPointer<QWidget> tm = topLevel("trackMotionWindow");
    check(tm != nullptr, "track motion opens");
    if (tm) {
        tm->resize(1150, 760);
        uitest::settle(100);
        QComboBox* mode = child<QComboBox>(tm, "compositeMode");
        check(mode && mode->count() == 15 && mode->findData("3dsrcalpha") >= 0 && mode->itemText(mode->findData("3dsrcalpha")) == "3D Source Alpha",
              "the compositing modes include 3D Source Alpha");
        check(child<QWidget>(tm, "boxView")->isVisible() && !child<QWidget>(tm, "view3d")->isVisible() && !child<QWidget>(tm, "layout3d")->isVisible() &&
                  !child<QWidget>(tm, "boxZ")->isVisible() && child<QWidget>(tm, "rowShadow")->isVisible(),
              "in Source Alpha the window is the 2D one");
        // keys 1..6 and Z do nothing in 2D
        uitest::key(tm, Qt::Key_Z);
        uitest::settle(20);
        check(!child<QToolButton>(tm, "preventMoveZ")->isChecked(), "Z does nothing while the track is 2D");
        // the top track covers the frame: muted so the preview shows Video 2
        st().edit("Mute", [](Project& p) { if (Track* t = p.findTrack(V1)) t->mute = true; });
        uitest::settle(30);
        const QImage flat = pb->renderNow(1.0);
        pick(mode, "3dsrcalpha");
        check(track(V2)->composite == "3dsrcalpha" && st().undoName() == "Compositing Mode", "choosing 3D Source Alpha sets the track's mode");
        check(tm->property("mode3d").toBool() && !child<QWidget>(tm, "boxView")->isVisible() && child<QWidget>(tm, "view3d")->isVisible() &&
                  child<QWidget>(tm, "layout3d")->isVisible() && child<QWidget>(tm, "boxZ")->isVisible() && child<QWidget>(tm, "boxOriX")->isVisible() &&
                  !child<QWidget>(tm, "rowShadow")->isVisible() && !child<QWidget>(tm, "effectOn")->isVisible() && child<QWidget>(tm, "preventMoveZ")->isVisible(),
              "in 3D Source Alpha it becomes the 3D one: the 3D workspace, the layouts, the 3D values (no 2D shadow or glow)");
        std::vector<QWidget*> panes = panesOf(tm);
        check(panes.size() == 1 && panes[0]->property("angle").toInt() == 0, "one pane at first, looking through the camera");
        uitest::shot(tm, gOut + "/trackmotion3d_single.png");

        // ---- the layouts (1 to 6) and the Z switches
        int counts[7] = {0};
        for (int k = 1; k <= 6; ++k) {
            uitest::key(tm, Qt::Key_0 + k);
            uitest::settle(40);
            counts[k] = (int)panesOf(tm).size();
        }
        check(counts[1] == 1 && counts[2] == 2 && counts[3] == 2 && counts[4] == 4 && counts[5] == 3 && counts[6] == 3 && tm->property("layout3d").toInt() == 6,
              "keys 1 to 6 choose the layouts: 1, 2, 2, 4, 3 and 3 panes", strfmt("%d %d %d %d %d %d", counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]));
        uitest::key(tm, Qt::Key_Z);
        uitest::settle(20);
        const bool z1 = child<QToolButton>(tm, "preventMoveZ")->isChecked();
        uitest::key(tm, Qt::Key_Z, Qt::ShiftModifier);
        uitest::settle(20);
        const bool z2 = child<QToolButton>(tm, "preventScaleZ")->isChecked();
        uitest::key(tm, Qt::Key_Z);
        uitest::key(tm, Qt::Key_Z, Qt::ShiftModifier);
        uitest::settle(20);
        check(z1 && z2 && !child<QToolButton>(tm, "preventMoveZ")->isChecked(), "Z and Shift+Z switch Prevent Movement Z and Prevent Scaling Z");

        // ---- dragging in the panes: layout 2 is the camera beside the top view
        uitest::key(tm, Qt::Key_2);
        uitest::settle(80);
        panes = panesOf(tm);
        if (panes.size() == 2) {
            QWidget* cam = panes[0];
            QWidget* top = panes[1];
            check(cam->property("angle").toInt() == 0 && top->property("angle").toInt() == 5, "layout 2: Camera and Top");
            QPointF c = centreIn(cam);
            drag(cam, c, c + QPointF(60, 0));
            const Track* t = track(V2);
            check(t->motion.size() == 1 && t->motion[0].x > 0.55 && near(t->motion[0].y, 0.5, 0.005) && near(t->motion[0].z, 0) &&
                      st().undoName() == "Track Motion",
                  "a drag across the box in the Camera pane moves the picture across", t->motion.empty() ? "" : num(t->motion[0].x) + ", " + num(t->motion[0].y));
            const double x0 = t->motion[0].x;
            c = centreIn(top);
            drag(top, c, c + QPointF(0, 40));
            t = track(V2);
            check(t->motion[0].z > 0.05 && near(t->motion[0].x, x0, 1e-6), "a drag down in the Top pane brings it towards the camera (Position Z)",
                  num(t->motion[0].z));
            const QImage nearer = pb->renderNow(1.0);
            check(meanDiff(flat, nearer) > 2, "and the preview shows it moved in 3D", num(meanDiff(flat, nearer)));
            const double z0 = t->motion[0].z;
            uitest::key(tm, Qt::Key_Z);
            c = centreIn(top);
            drag(top, c, c + QPointF(30, 30));
            t = track(V2);
            check(near(t->motion[0].z, z0, 1e-9) && t->motion[0].x > x0 + 0.01, "with Prevent Movement Z the Top pane only moves it across",
                  num(t->motion[0].z) + " " + num(t->motion[0].x));
            uitest::key(tm, Qt::Key_Z);
            // a corner of the box in the camera pane sizes it, keeping its shape
            const QPointF k2 = cornerIn(cam, 2), cc = centreIn(cam);
            drag(cam, k2, k2 + (k2 - cc) * 0.3);
            t = track(V2);
            check(t->motion[0].w > 1.15 && near(t->motion[0].w, t->motion[0].h, 1e-6), "dragging a corner sizes the box (Lock Aspect Ratio keeps its shape)",
                  num(t->motion[0].w) + " x " + num(t->motion[0].h));
            // just outside a corner in the Top pane: the box turns about the up-down axis (Rotation Y)
            const QPointF k1 = cornerIn(top, 1), tc = centreIn(top);
            drag(top, k1 + QPointF(0, -14), tc + QPointF(40, -60));
            t = track(V2);
            check(std::fabs(t->motion[0].rotY) > 10 && near(t->motion[0].rotation, 0) && near(t->motion[0].rotX, 0), "turning in the Top pane turns it about the up-down axis",
                  num(t->motion[0].rotY));
        }
        // ---- Enable Snapping (F8) works in the 3D panes: the box's middle (or an unturned box's edge) goes onto the grid
        if (panes.size() == 2) {
            QToolButton* snapBtn = child<QToolButton>(tm, "enableSnapping");
            check(snapBtn && snapBtn->isEnabled(), "Enable Snapping is available in the 3D window");
            st().edit("Plain box", [](Project& p) {
                MotionKey k;
                k.x = 0.5; k.y = 0.5; k.w = 0.4; k.h = 0.4;
                if (Track* t = p.findTrack(V2)) t->motion = {k};
            });
            uitest::settle(40);
            if (QDoubleSpinBox* g = child<QDoubleSpinBox>(tm, "gridSpacing")) g->setValue(4);
            uitest::key(tm, Qt::Key_F8);
            uitest::settle(20);
            check(snapBtn && snapBtn->isChecked(), "F8 switches snapping on in 3D");
            QWidget* cam = panes[0];
            const double pxPerX = QLineF(cornerIn(cam, 0), cornerIn(cam, 1)).length() / 0.4;
            QPointF c = centreIn(cam);
            drag(cam, c, c + QPointF((0.26 * pxPerX), 0));
            check(near(track(V2)->motion[0].x, 0.75, 1e-6) && near(track(V2)->motion[0].y, 0.5, 0.01),
                  "a drag that ends near a grid line puts the box's middle on it", num(track(V2)->motion[0].x));
            // not near one (grid 16 now: lines every 0.0625; 8 pixels is far less than half of that here): left as dragged
            uitest::key(tm, Qt::Key_F8);
            c = centreIn(cam);
            drag(cam, c, c + QPointF(0.03 * pxPerX, 0));
            const double freeX = track(V2)->motion[0].x;
            check(freeX > 0.75 && !near(freeX, 0.75, 1e-6), "with snapping off the same kind of drag lands where it was dragged", num(freeX));
            uitest::key(tm, Qt::Key_F8);
            // sizing: the edge that moves goes onto a grid line
            st().edit("Plain box", [](Project& p) {
                MotionKey k;
                k.x = 0.5; k.y = 0.5; k.w = 0.4; k.h = 0.4;
                if (Track* t = p.findTrack(V2)) t->motion = {k};
            });
            uitest::settle(40);
            const QPointF k2 = cornerIn(cam, 2), cc = centreIn(cam);
            const QPointF dir = (k2 - cc) / std::hypot((k2 - cc).x(), (k2 - cc).y());
            // 0.4 -> about 0.52 wide: the right edge (0.5 + 0.26) is near the line at 0.75
            drag(cam, k2, k2 + dir * (0.2 * pxPerX * 0.35));
            const double nw = track(V2)->motion[0].w;
            check(near(0.5 + nw / 2, 0.75, 1e-3) || near(0.5 + nw / 2, 0.8125, 1e-3), "sizing snaps the moving edge to the grid", num(nw));
            uitest::key(tm, Qt::Key_F8);
        }
        // ---- the Perspective view can be turned (Alt+drag or the middle button), and Restore View puts it back
        if (panes.size() == 2) {
            if (QComboBox* angle = child<QComboBox>(panes[1], "paneAngle")) pick(angle, QString::number(7));
            QWidget* per = panes[1];
            per->grab();
            const double yaw0 = per->property("yaw").toDouble(), pitch0 = per->property("pitch").toDouble();
            check(near(yaw0, 35) && near(pitch0, 25), "the Perspective view starts looking from the front right and above");
            const QPoint a(8, per->height() - 8);
            uitest::mousePress(per, a, Qt::LeftButton, Qt::AltModifier);
            uitest::mouseMove(per, a + QPoint(40, 0), Qt::LeftButton, Qt::AltModifier);
            uitest::mouseMove(per, a + QPoint(80, -20), Qt::LeftButton, Qt::AltModifier);
            uitest::mouseRelease(per, a + QPoint(80, -20), Qt::LeftButton, Qt::AltModifier);
            per->grab();
            check(near(per->property("yaw").toDouble(), 35 + 40, 1e-6) && near(per->property("pitch").toDouble(), 25 - 10, 1e-6),
                  "Alt+drag turns the Perspective view", strfmt("%.1f %.1f", per->property("yaw").toDouble(), per->property("pitch").toDouble()));
            uitest::mousePress(per, a, Qt::MiddleButton);
            uitest::mouseMove(per, a + QPoint(-20, 0), Qt::MiddleButton);
            uitest::mouseRelease(per, a + QPoint(-20, 0), Qt::MiddleButton);
            per->grab();
            check(near(per->property("yaw").toDouble(), 35 + 30, 1e-6) && near(per->property("pitch").toDouble(), 15, 1e-6),
                  "so does dragging with the middle button", strfmt("%.1f", per->property("yaw").toDouble()));
            // the box is drawn from the new direction: its corners moved
            const QPointF before = cornerIn(per, 0);
            uitest::mousePress(per, a, Qt::MiddleButton);
            uitest::mouseMove(per, a + QPoint(60, 0), Qt::MiddleButton);
            uitest::mouseRelease(per, a + QPoint(60, 0), Qt::MiddleButton);
            check(QLineF(before, cornerIn(per, 0)).length() > 5, "and the box is drawn from there");
            // a plain drag in the Camera pane (not Perspective) ignores Alt: nothing is turned there
            if (QComboBox* angle = child<QComboBox>(per, "paneAngle")) { pick(angle, QString::number(0)); pick(angle, QString::number(7)); }
            per->grab();
            check(near(per->property("yaw").toDouble(), 35) && near(per->property("pitch").toDouble(), 25), "choosing the view again (Restore View) puts the direction back");
            if (QComboBox* angle = child<QComboBox>(per, "paneAngle")) pick(angle, QString::number(5));
        }
        // ---- the panel's 3D values
        if (QDoubleSpinBox* rx = child<QDoubleSpinBox>(tm, "boxRotX")) {
            rx->setValue(30);
            uitest::settle(20);
            check(near(track(V2)->motion[0].rotX, 30), "the panel's Rotation X sets it", num(track(V2)->motion[0].rotX));
        }
        if (QDoubleSpinBox* oz = child<QDoubleSpinBox>(tm, "boxOriZ")) {
            oz->setValue(-20);
            uitest::settle(20);
            check(near(track(V2)->motion[0].oriZ, -20), "and Orientation Z", num(track(V2)->motion[0].oriZ));
        }
        if (QDoubleSpinBox* z = child<QDoubleSpinBox>(tm, "boxZ")) {
            z->setValue(-320);
            uitest::settle(20);
            check(near(track(V2)->motion[0].z, -0.5, 1e-6), "and Position Z (in pixels of the frame's width)", num(track(V2)->motion[0].z));
        }
        uitest::key(tm, Qt::Key_4);
        uitest::settle(100);
        uitest::shot(tm, gOut + "/trackmotion3d_four.png");
        uitest::key(tm, Qt::Key_5);
        uitest::settle(100);
        if (panesOf(tm).size() == 3) {
            QComboBox* angle = child<QComboBox>(panesOf(tm)[1], "paneAngle");
            if (angle) pick(angle, QString::number(7));
        }
        uitest::shot(tm, gOut + "/trackmotion3d_perspective.png");
        // back to Source Alpha: VEGAS asks first (the 3D values are lost); No leaves it as it was
        st().edit("3D values", [](Project& p) { if (Track* t = p.findTrack(V2)) t->motion[0].rotX = 30; });
        QSettings().remove("dontShow/motion3dTo2d");
        answerConversion("conversionNo");
        pick(mode, "srcalpha");
        check(track(V2)->composite == "3dsrcalpha" && near(track(V2)->motion[0].rotX, 30) && mode->currentData().toString() == "3dsrcalpha",
              "going back to 2D asks about the lost 3D values; No keeps the track in 3D");
        answerConversion("conversionYes", true);
        pick(mode, "srcalpha");
        check(track(V2)->composite == "srcalpha" && near(track(V2)->motion[0].rotX, 0) && near(track(V2)->motion[0].z, 0) && near(track(V2)->motion[0].oriZ, 0) &&
                  !tm->property("mode3d").toBool() && child<QWidget>(tm, "boxView")->isVisible() && child<QWidget>(tm, "rowShadow")->isVisible(),
              "Yes makes it 2D: the window is the 2D one and the 3D values are gone");
        // to 3D: the 2D Shadow and Glow keyframes are deleted (3D has none)
        st().edit("Shadow and Glow", [](Project& p) {
            Track* t = p.findTrack(V2);
            GlowKey g;
            t->shadow2d = {g};
            t->glow2d = {g};
        });
        answerConversion("conversionNo");
        pick(mode, "3dsrcalpha");
        check(track(V2)->composite == "srcalpha" && !track(V2)->shadow2d.empty() && !track(V2)->glow2d.empty(),
              "going to 3D asks about the 2D Shadow and Glow keyframes that will be deleted; No keeps them");
        answerConversion("conversionYes");
        pick(mode, "3dsrcalpha");
        check(track(V2)->composite == "3dsrcalpha" && track(V2)->shadow2d.empty() && track(V2)->glow2d.empty() && !child<QWidget>(tm, "rowShadow")->isVisible(),
              "Yes deletes them: 3D track motion has no Shadow or Glow (as in VEGAS)");
        // "Don't show me this again" was ticked for 3D -> 2D: that question is not asked again
        st().edit("3D values", [](Project& p) { if (Track* t = p.findTrack(V2)) t->motion[0].rotX = 20; });
        pick(mode, "srcalpha");
        check(track(V2)->composite == "srcalpha", "with \"Don't show me this again\" ticked the question is not asked again");
        pick(mode, "3dsrcalpha");
        tm->close();
        uitest::settle(50);
    }

    // ================================================================================== parents and Parent Motion
    TimelineWidget* tl = w.timeline();
    check(tl && tl->onTrackMenu, "the timeline has the track menu");
    if (tl && tl->onTrackMenu) {
        QMenu m3;
        tl->onTrackMenu(&m3, V3);
        QAction* makeChild = findAction(&m3, "Make Compositing Child");
        QAction* makeParent = findAction(&m3, "Make Compositing Parent");
        check(makeChild && makeChild->isEnabled() && makeParent && !makeParent->isEnabled() && !findSubmenu(&m3, "Parent Composite Mode"),
              "a track's menu: Make Compositing Child (Make Compositing Parent only for a child)");
        if (makeChild) makeChild->trigger();
        uitest::settle(30);
        check(track(V3)->compositeLevel == 1 && st().undoName() == "Make Compositing Child", "Make Compositing Child makes it a child of the track above");
        QMenu m2;
        tl->onTrackMenu(&m2, V2);
        QMenu* pmodes = findSubmenu(&m2, "Parent Composite Mode");
        QAction* pmotion = findAction(&m2, "Parent Motion...");
        check(pmodes && pmotion, "the parent's menu has Parent Composite Mode and Parent Motion");
        if (pmodes)
            if (QAction* a = findAction(pmodes, "3D Source Alpha")) a->trigger();
        uitest::settle(30);
        check(track(V2)->parentComposite == "3dsrcalpha", "Parent Composite Mode: 3D Source Alpha");
        if (pmotion) pmotion->trigger();
        uitest::settle(150);
        QPointer<QWidget> pm = topLevel("parentMotionWindow");
        check(pm && pm->property("mode3d").toBool() && pm->windowTitle() == "Parent Track Motion: Video 2" &&
                  child<QComboBox>(pm, "compositeMode")->currentData().toString() == "3dsrcalpha" && !child<QWidget>(pm, "rowShadow")->isVisible(),
              "Parent Motion opens in 3D (its drop-down is the Parent Composite Mode)",
              pm ? strfmt("%s, 3D %d, mode %s, shadow row %d", pm->windowTitle().toStdString().c_str(), (int)pm->property("mode3d").toBool(),
                          child<QComboBox>(pm, "compositeMode")->currentData().toString().toStdString().c_str(),
                          (int)child<QWidget>(pm, "rowShadow")->isVisible())
                 : std::string());
        if (pm) {
            pm->resize(1150, 760);
            uitest::settle(100);
            uitest::key(pm, Qt::Key_2);
            uitest::settle(80);
            std::vector<QWidget*> panes = panesOf(pm);
            if (panes.size() == 2) {
                const QPointF c = centreIn(panes[1]);
                drag(panes[1], c, c + QPointF(0, -50));        // up in the Top pane: away from the camera
                const Track* t = track(V2);
                check(t->parentMotion.size() == 1 && t->parentMotion[0].z < -0.05 && t->motion.size() == 1 && st().undoName() == "Parent Motion",
                      "dragging in Parent Motion moves the group (its own Track Motion stays)", t->parentMotion.empty() ? "" : num(t->parentMotion[0].z));
                uitest::shot(pm, gOut + "/parentmotion3d.png");
            }
            pm->close();
            uitest::settle(50);
        }
        QMenu m3b;
        tl->onTrackMenu(&m3b, V3);
        if (QAction* a = findAction(&m3b, "Make Compositing Parent")) a->trigger();
        uitest::settle(30);
        check(track(V3)->compositeLevel == 0, "Make Compositing Parent takes it out again");
    }

    // ================================================================================== the Trimmer's markers
    MainWindow::Tools* tools = w.tools();
    if (tools && tools->trimmer) {
        TrimmerWindow* tr = tools->trimmer;
        tools->trimmerDock->show();
        uitest::settle(50);
        check(tr->openMedia(clip), "the Trimmer opens the clip");
        for (int k = 0; k < 40 && tr->picture().isNull(); ++k) uitest::settle(50);
        tr->setFocus();
        tr->setPosition(1.0);
        uitest::key(tr, Qt::Key_M, Qt::NoModifier, "m");
        uitest::settle(20);
        check(tr->markers().size() == 1 && near(tr->markers()[0].time, 1.0) && !tr->markersSaved() && st().project().findMedia(clip)->markers.empty(),
              "M puts a marker at the Trimmer's cursor (not saved yet)");
        uitest::key(tr, Qt::Key_R, Qt::NoModifier, "r");
        check(tr->markers().size() == 1, "R with no selection makes no region");
        tr->setSelection(2.0, 3.0);
        uitest::key(tr, Qt::Key_R, Qt::NoModifier, "r");
        uitest::settle(20);
        check(tr->markers().size() == 2 && near(tr->markers()[1].time, 2.0) && near(tr->markers()[1].length, 1.0), "R makes a region of the selection");
        const size_t eventsBefore = st().project().tracks[0].events.size();
        uitest::key(tr, Qt::Key_S, Qt::NoModifier, "s");
        uitest::settle(20);
        const MediaItem* mi = st().project().findMedia(clip);
        check(tr->markersSaved() && mi->markers.size() == 2 && st().undoName() == "Save Trimmer Markers/Regions" && st().project().tracks[0].events.size() == eventsBefore,
              "S saves them with the media (and does not split the timeline's events)");
        uitest::shot(tr, gOut + "/trimmer_markers.png");
        st().undo();
        uitest::settle(20);
        check(st().project().findMedia(clip)->markers.empty() && tr->markers().empty(), "undo takes the saved markers back (the Trimmer follows)");
        st().redo();
        uitest::settle(20);
        check(st().project().findMedia(clip)->markers.size() == 2 && tr->markers().size() == 2, "and redo puts them back");
        uitest::key(tr, Qt::Key_S, Qt::ShiftModifier, "S");
        uitest::settle(20);
        check(tr->autoSaveMarkers(), "Shift+S: Auto-Save Trimmer Markers/Regions");
        tr->setPosition(3.5);
        uitest::key(tr, Qt::Key_M, Qt::NoModifier, "m");
        uitest::settle(20);
        check(st().project().findMedia(clip)->markers.size() == 3 && tr->markersSaved(), "with it on a new marker is saved at once");
        uitest::key(tr, Qt::Key_S, Qt::ShiftModifier, "S");
        check(!tr->autoSaveMarkers(), "Shift+S again turns it off");

        // Ctrl+P: the Trimmer's picture in the Video Preview
        PreviewWidget* pv = w.preview();
        st().setCursor(0.0);
        uitest::settle(300);
        const QImage before = pv ? pv->pictureWidget()->grab().toImage() : QImage();
        tr->setPosition(7.0);
        uitest::settle(200);
        uitest::key(tr, Qt::Key_P, Qt::ControlModifier);
        uitest::settle(100);
        const QImage during = pv ? pv->pictureWidget()->grab().toImage() : QImage();
        check(tr->videoInPreview() && meanDiff(before, during) > 1, "Ctrl+P shows the Trimmer's picture in the Video Preview", num(meanDiff(before, during)));
        uitest::shot(&w, gOut + "/trimmer_in_preview.png");
        uitest::key(tr, Qt::Key_P, Qt::ControlModifier);
        uitest::settle(400);
        const QImage after = pv ? pv->pictureWidget()->grab().toImage() : QImage();
        check(!tr->videoInPreview() && meanDiff(before, after) < 1, "Ctrl+P again: the timeline's picture is back", num(meanDiff(before, after)));

        // the saved markers on the media's events (View > Event Media Markers)
        if (QAction* show = w.findChild<QAction*>("showMediaMarkers")) {
            if (!show->isChecked()) show->trigger();
        }
        uitest::settle(100);
        if (tl) uitest::shot(tl, gOut + "/event_media_markers.png");
    }

    return uitest::summary();
}
