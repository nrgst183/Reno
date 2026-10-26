// Workflows that VEGAS tutorials teach: Event Pan/Crop's mask (its tools, Mask row and keyframes, Path properties, Apply to FX; the
// preview shows it), splitting an event that has Pan/Crop keyframes, the Undo / Redo drop-downs listing the edit history, and Paste
// Event Attributes taking the mask along. Pictures of the Pan/Crop window go to the output folder.
//
//   tutor_test <output folder> <testfx.ofx> [more .ofx ...]
//
// Set HOME to a scratch folder. QT_QPA_PLATFORM=offscreen.
#include "../../src/app/editdialogs.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/smalldialogs.h"
#include "../../src/engine/catalog.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

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
void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    uitest::mousePress(w, from, Qt::LeftButton, mods);
    for (int k = 1; k <= 8; ++k) uitest::mouseMove(w, from + (to - from) * k / 8, Qt::LeftButton, mods);
    uitest::mouseRelease(w, to, Qt::LeftButton, mods);
    uitest::settle(30);
}
void click(QWidget* w, QPoint p)
{
    uitest::mousePress(w, p);
    uitest::mouseRelease(w, p);
    uitest::settle(20);
}
const Event* ev(uint64_t id) { return AppState::get().project().findEvent(id); }
std::vector<MaskPath> pathsAt(uint64_t id, double t)
{
    const Event* e = ev(id);
    return e ? maskAt(e->mask, t) : std::vector<MaskPath>();
}
// how bright the picture is at a fraction of it (0..255, the brightest channel)
int bright(const QImage& img, double fx, double fy)
{
    if (img.isNull()) return -1;
    const QRgb c = img.pixel(std::min(img.width() - 1, (int)(fx * img.width())), std::min(img.height() - 1, (int)(fy * img.height())));
    return std::max(qRed(c), std::max(qGreen(c), qBlue(c)));
}
std::string num(double v) { return QString::number(v, 'g', 6).toStdString(); }

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTutorTest");
    QApplication::setApplicationName("tutor_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) { qWarning("usage: tutor_test <output folder> <testfx.ofx> [...]"); return 2; }
    const QString out = args[1];
    QDir().mkpath(out);
    std::vector<std::string> ofxFiles;
    for (int k = 2; k < args.size(); ++k) ofxFiles.push_back(args[k].toStdString());
    uitest::setup(ofxFiles, {});

    AppState& app = AppState::get();
    app.newProject();
    app.edit("Small Project", [](Project& p) { p.settings.width = 640; p.settings.height = 360; });
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    uint64_t V = 0;
    for (uint64_t id : placed) {
        const Track* t = nullptr;
        app.project().findEvent(id, &t);
        if (t && t->type == TrackType::Video) V = id;
    }
    check(V != 0, "setup: a clip on the timeline");
    app.setCursor(0.0);
    Playback playback;
    const QImage plain = playback.renderNow(1.0);
    check(bright(plain, 0.05, 0.5) > 20 && bright(plain, 0.5, 0.5) > 20, "setup: the clip's picture fills the frame",
          num(bright(plain, 0.05, 0.5)) + " " + num(bright(plain, 0.5, 0.5)));

    // ================================================================== the mask in Event Pan/Crop
    openPanCrop(nullptr, &playback, V);
    uitest::settle();
    QPointer<QWidget> pc = topLevel("panCropWindow");
    check(pc != nullptr, "pan/crop: the window opens");
    if (!pc) return uitest::summary();
    pc->resize(1100, 700);
    uitest::settle(80);
    QWidget* view = child<QWidget>(pc, "boxView");
    QComboBox* row = child<QComboBox>(pc, "keyRow");
    QCheckBox* maskSwitch = child<QCheckBox>(pc, "maskSwitch");
    QToolButton* rectTool = child<QToolButton>(pc, "toolRectMask");
    QToolButton* ovalTool = child<QToolButton>(pc, "toolOvalMask");
    QToolButton* anchorTool = child<QToolButton>(pc, "toolAnchor");
    QToolButton* normalTool = child<QToolButton>(pc, "toolNormal");
    QToolButton* delTool = child<QToolButton>(pc, "toolAnchorDelete");
    check(view && row && maskSwitch && rectTool && ovalTool && anchorTool && normalTool && delTool && child<QToolButton>(pc, "toolSplitTangent"),
          "mask: the Mask switch, the keyframe rows and VEGAS's mask tools are in the window");
    if (!view || !row || !maskSwitch || !rectTool || !ovalTool || !anchorTool || !normalTool || !delTool) return uitest::summary();
    check(row->currentData().toString() == "0" && !maskSwitch->isChecked(), "mask: it starts on the Position row with the mask off");
    {
        QToolButton* play = child<QToolButton>(pc, "boxPlay");
        QToolButton* stop = child<QToolButton>(pc, "boxStop");
        check(play && stop, "pan/crop: VEGAS's Play and Stop buttons under the keyframes");
        if (play && stop) {
            app.setCursor(30.0);  // outside the event: play starts at its start
            play->click();
            uitest::settle(150);
            const bool was = playback.playing();
            stop->click();
            uitest::settle(50);
            check(was && !playback.playing(), "pan/crop: Play plays, Stop stops", was ? "" : "did not play");
            app.setCursor(0.0);
            uitest::settle(30);
        }
    }

    // the Rectangle tool: a drag draws a rectangle, the Mask row is chosen and the mask goes on
    rectTool->click();
    uitest::settle(30);
    check(row->currentData().toString() == "1", "mask: choosing a mask tool shows the Mask row");
    const size_t undoBefore = app.undoCount();
    drag(view, at(view, 0.3, 0.3), at(view, 0.7, 0.7));
    {
        const Event* e = ev(V);
        const std::vector<MaskPath> ps = pathsAt(V, 0);
        check(e && e->maskOn && maskSwitch->isChecked(), "mask: drawing a shape turns the mask on");
        check(ps.size() == 1 && ps[0].closed && ps[0].points.size() == 4 && std::fabs(ps[0].points[0].x - 0.3) < 0.01 &&
                  std::fabs(ps[0].points[2].y - 0.7) < 0.01,
              "mask: the Rectangle tool draws a closed four-point path where it was dragged",
              ps.empty() ? "none" : num(ps[0].points[0].x) + "," + num(ps[0].points[0].y) + " .. " + num(ps[0].points[2].x) + "," + num(ps[0].points[2].y));
        check(app.undoCount() == undoBefore + 1 && app.undoName() == "Rectangle Mask", "mask: one undo step, \"Rectangle Mask\"", app.undoName());
    }
    QImage masked = playback.renderNow(1.0);
    check(bright(masked, 0.05, 0.5) < 8 && bright(masked, 0.5, 0.5) > 20, "mask: the preview shows the picture cut to the rectangle",
          num(bright(masked, 0.05, 0.5)) + " " + num(bright(masked, 0.5, 0.5)));
    pc->grab().save(out + "/pancrop_mask_rect.png");

    // the Path properties work on the chosen path
    QComboBox* mode = child<QComboBox>(pc, "maskMode");
    QDoubleSpinBox* feather = child<QDoubleSpinBox>(pc, "maskFeather");
    QComboBox* ftype = child<QComboBox>(pc, "maskFeatherType");
    QDoubleSpinBox* opacity = child<QDoubleSpinBox>(pc, "maskOpacity");
    check(mode && feather && ftype && opacity && mode->isEnabled(), "mask: the Path properties are there, for the shape just drawn");
    if (mode && feather && ftype && opacity) {
        mode->setCurrentIndex(1);
        emit mode->activated(1);
        uitest::settle(20);
        check(!pathsAt(V, 0).empty() && pathsAt(V, 0)[0].negative, "mask: Mode Negative");
        masked = playback.renderNow(1.0);
        check(bright(masked, 0.05, 0.5) > 20 && bright(masked, 0.5, 0.5) < 8, "mask: a negative path shows what is outside it",
              num(bright(masked, 0.05, 0.5)) + " " + num(bright(masked, 0.5, 0.5)));
        mode->setCurrentIndex(0);
        emit mode->activated(0);
        feather->setValue(30);
        uitest::settle(20);
        check(!pathsAt(V, 0).empty() && std::fabs(pathsAt(V, 0)[0].feather - 30) < 1e-9, "mask: Feather (%)");
        ftype->setCurrentIndex(3);
        emit ftype->activated(3);
        opacity->setValue(80);
        uitest::settle(20);
        check(!pathsAt(V, 0).empty() && pathsAt(V, 0)[0].featherType == 3 && std::fabs(pathsAt(V, 0)[0].opacity - 0.8) < 1e-9, "mask: Feather type and Opacity");
        feather->setValue(0);
        opacity->setValue(100);
        uitest::settle(20);
    }

    // the Normal tool drags a whole shape by its inside, and one point by itself
    normalTool->click();
    uitest::settle(20);
    drag(view, at(view, 0.5, 0.5), at(view, 0.6, 0.5));
    {
        const std::vector<MaskPath> ps = pathsAt(V, 0);
        check(ps.size() == 1 && std::fabs(ps[0].points[0].x - 0.4) < 0.01 && std::fabs(ps[0].points[0].y - 0.3) < 0.01,
              "mask: the Normal tool moves the whole shape", ps.empty() ? "" : num(ps[0].points[0].x));
        check(app.undoName() == "Move Mask", "mask: \"Move Mask\" undo step", app.undoName());
    }
    drag(view, at(view, 0.8, 0.7), at(view, 0.9, 0.8));
    {
        const std::vector<MaskPath> ps = pathsAt(V, 0);
        check(ps.size() == 1 && std::fabs(ps[0].points[2].x - 0.9) < 0.01 && std::fabs(ps[0].points[2].y - 0.8) < 0.01 &&
                  std::fabs(ps[0].points[1].x - 0.8) < 0.01,
              "mask: dragging a point moves only that point", ps.empty() ? "" : num(ps[0].points[2].x) + "," + num(ps[0].points[2].y));
    }
    app.undo();
    app.undo();
    uitest::settle(20);

    // the Oval tool with Shift: a circle (as round in the media's pixels)
    ovalTool->click();
    uitest::settle(20);
    drag(view, at(view, 0.05, 0.1), at(view, 0.25, 0.2), Qt::ShiftModifier);
    {
        const std::vector<MaskPath> ps = pathsAt(V, 0);
        bool round = false;
        if (ps.size() == 2 && ps[1].points.size() == 4) {
            const QSizeF media(640, 360);  // synth media at the project's size
            const double wpx = (ps[1].points[1].x - ps[1].points[3].x) * media.width(), hpx = (ps[1].points[2].y - ps[1].points[0].y) * media.height();
            round = std::fabs(wpx - hpx) < 2.0 && ps[1].points[0].outX != 0;
        }
        check(round, "mask: the Oval tool with Shift draws a circle of Bezier curves");
    }
    pc->grab().save(out + "/pancrop_mask_oval.png");
    // Delete takes the chosen shape away
    uitest::key(pc, Qt::Key_Delete);
    uitest::settle(20);
    check(pathsAt(V, 0).size() == 1, "mask: Delete removes the chosen shape", num(pathsAt(V, 0).size()));

    // the Anchor tool: four clicks and a click on the first point make a closed Bezier path; a drag pulls out handles
    anchorTool->click();
    uitest::settle(20);
    click(view, at(view, 0.1, 0.1));
    click(view, at(view, 0.25, 0.1));
    drag(view, at(view, 0.25, 0.25), at(view, 0.3, 0.3));
    click(view, at(view, 0.1, 0.25));
    {
        const std::vector<MaskPath> ps = pathsAt(V, 0);
        check(ps.size() == 2 && !ps[1].closed && ps[1].points.size() == 4, "mask: the Anchor tool adds points to an open path",
              ps.size() == 2 ? num(ps[1].points.size()) : num(ps.size()));
        check(ps.size() == 2 && ps[1].points.size() == 4 && std::fabs(ps[1].points[2].outX - 0.05) < 0.01 && std::fabs(ps[1].points[2].inX + 0.05) < 0.01,
              "mask: dragging while adding a point pulls out its curve handles");
        check(maskActive(*ev(V)), "mask: the open path does not stop the closed one working");
    }
    click(view, at(view, 0.1, 0.1));
    {
        const std::vector<MaskPath> ps = pathsAt(V, 0);
        check(ps.size() == 2 && ps[1].closed && ps[1].points.size() == 4, "mask: clicking the first point again closes the path");
    }
    // the Anchor Deletion tool takes a point out
    delTool->click();
    uitest::settle(20);
    click(view, at(view, 0.1, 0.25));
    check(pathsAt(V, 0).size() == 2 && pathsAt(V, 0)[1].points.size() == 3, "mask: the Anchor Deletion tool removes a point");
    pc->grab().save(out + "/pancrop_mask_bezier.png");

    // keyframes on the Mask row: a second key at 2 s with the shape moved; half way it is half way
    normalTool->click();
    {
        QToolButton* addKey = child<QToolButton>(pc, "addKey");
        app.setCursor(2.0);
        uitest::settle(40);
        if (addKey) addKey->click();
        uitest::settle(20);
        const Event* e = ev(V);
        check(e && e->mask.size() == 2 && e->panCrop.empty(), "mask: Add Keyframe on the Mask row adds a mask keyframe (not a Pan/Crop one)",
              e ? num(e->mask.size()) + " " + num(e->panCrop.size()) : "");
        // drag the rectangle (path 0) right by 0.2 at 2 s
        drag(view, at(view, 0.5, 0.5), at(view, 0.7, 0.5));
        const std::vector<MaskPath> at0 = pathsAt(V, 0), at1 = pathsAt(V, 1.0), at2 = pathsAt(V, 2.0);
        check(at0.size() == 2 && at2.size() == 2 && std::fabs(at2[0].points[0].x - at0[0].points[0].x - 0.2) < 0.01 &&
                  std::fabs(at1[0].points[0].x - at0[0].points[0].x - 0.1) < 0.01,
              "mask: the shape moves between its keyframes", at1.empty() ? "" : num(at1[0].points[0].x - at0[0].points[0].x));
        app.setCursor(0.0);
        uitest::settle(40);
    }

    // Apply to FX
    {
        QComboBox* apply = child<QComboBox>(pc, "maskApplyToFx");
        check(apply != nullptr, "mask: Apply to FX is there");
        if (apply) {
            apply->setCurrentIndex(1);
            emit apply->activated(1);
            uitest::settle(20);
            check(ev(V)->maskApplyToFx, "mask: Apply to FX Yes");
            masked = playback.renderNow(1.0);
            check(bright(masked, 0.95, 0.9) > 20, "mask: with Apply to FX (and no effects) the picture is not cut",
                  num(bright(masked, 0.95, 0.9)));
            apply->setCurrentIndex(0);
            emit apply->activated(0);
        }
    }

    // the Mask switch off: the picture is whole again; the shapes are kept
    maskSwitch->click();
    uitest::settle(20);
    check(!ev(V)->maskOn && !ev(V)->mask.empty(), "mask: the switch off keeps the shapes");
    masked = playback.renderNow(1.0);
    check(bright(masked, 0.95, 0.9) > 20, "mask: off, the picture is whole");
    maskSwitch->click();
    uitest::settle(20);

    // back to the Position row: the box is what is edited again
    row->setCurrentIndex(0);
    emit row->activated(0);
    uitest::settle(20);
    drag(view, at(view, 0.5, 0.5), at(view, 0.55, 0.5));
    check(!ev(V)->panCrop.empty() && ev(V)->mask.size() == 2, "mask: on the Position row a drag moves the Pan/Crop box, not the mask");
    app.undo();
    pc->close();
    uitest::settle(40);

    // ================================================================== Paste Event Attributes takes the mask along
    {
        app.setSelection({V});
        app.copySelection();
        app.edit("Second", [](Project& p) {
            for (Track& t : p.tracks)
                if (t.type == TrackType::Video && !t.events.empty()) {
                    Event e = t.events[0];
                    e.id = p.newId();
                    e.start = 20;
                    e.mask.clear();
                    e.maskOn = false;
                    e.group = 0;
                    t.events.push_back(e);
                    break;
                }
        });
        uint64_t second = 0;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (e.start == 20) second = e.id;
        app.pasteEventAttributes({second}, AppState::AttrPanCrop);
        const Event* s = ev(second);
        check(s && s->maskOn && s->mask.size() == ev(V)->mask.size(), "Paste Event Attributes: the mask comes with Event Pan/Crop");
    }

    // ================================================================== splitting keeps Pan/Crop and mask keyframes in step
    {
        app.edit("Pan/Crop keys", [V](Project& p) {
            Event* e = p.findEvent(V);
            PanCropKey a, b;
            a.time = 0;
            a.cx = 0.3;
            b.time = 4;
            b.cx = 0.7;
            e->panCrop = {a, b};
        });
        const Event before = *ev(V);
        app.setSelection({V});
        app.setCursor(2.0);
        app.splitAtCursor();
        uint64_t right = 0;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (std::fabs(e.start - 2.0) < 1e-9 && t.type == TrackType::Video) right = e.id;
        const Event* r = ev(right);
        bool keys = false, mask = false;
        if (r) {
            // the right part's first moment is where the whole event was at 2 s
            double cx = 0;
            for (size_t k = 0; k + 1 < r->panCrop.size(); ++k)
                if (r->panCrop[k].time <= 0 && r->panCrop[k + 1].time > 0) {
                    const double f = (0 - r->panCrop[k].time) / (r->panCrop[k + 1].time - r->panCrop[k].time);
                    cx = r->panCrop[k].cx + (r->panCrop[k + 1].cx - r->panCrop[k].cx) * f;
                }
            keys = std::fabs(cx - 0.5) < 1e-9;
            const std::vector<MaskPath> a = maskAt(before.mask, 2.0), b = maskAt(r->mask, 0.0);
            mask = !a.empty() && a.size() == b.size() && std::fabs(a[0].points[0].x - b[0].points[0].x) < 1e-9;
        }
        check(r && keys, "split: the part after the cut goes on panning from where it was (Pan/Crop keyframes moved with it)");
        check(r && mask, "split: the mask's keyframes are moved with it too");
    }

    // ================================================================== Insert > Slideshow (VEGAS's Slideshow Creator)
    {
        app.setCursor(30.0);
        SlideshowSettings ss;
        ss.files = {"synthpic:3", "synthpic:4", "nonexistent.png", "synthpic:5"};
        ss.perPicture = 4;
        ss.overlap = 1;
        ss.effect = "Fades with Pan and Zoom";
        const size_t undo0 = app.undoCount();
        std::string serr;
        std::vector<std::string> skipped;
        const std::vector<uint64_t> made = makeSlideshow(ss, &serr, &skipped);
        bool placed = made.size() == 3;
        for (size_t k = 0; placed && k < made.size(); ++k) {
            const Event* e = ev(made[k]);
            placed = e && std::fabs(e->start - (30.0 + 3.0 * k)) < 1e-6 && std::fabs(e->length - 4) < 1e-6 && e->panCrop.size() == 2;
        }
        check(placed, "slideshow: three pictures, 4 s each, overlapping by 1 s from the cursor, each with Pan/Crop keyframes", serr);
        check(skipped.size() == 1 && skipped[0] == "nonexistent.png", "slideshow: a file that cannot be read is left out and named");
        check(app.undoCount() == undo0 + 1 && app.undoName() == "Insert Slideshow", "slideshow: one undo step", app.undoName());
        {
            const Event* e = ev(made.empty() ? 0 : made[0]);
            const bool zoomOrPan = e && e->panCrop.size() == 2 && (e->panCrop[0].w != e->panCrop[1].w || e->panCrop[0].cx != e->panCrop[1].cx || e->panCrop[0].cy != e->panCrop[1].cy);
            const bool inside = e && e->panCrop.size() == 2 && e->panCrop[1].cx - e->panCrop[1].w / 2 > -1e-9 && e->panCrop[1].cx + e->panCrop[1].w / 2 < 1 + 1e-9;
            check(zoomOrPan && inside, "slideshow: Pan and Zoom moves the box, staying inside the picture");
        }
        app.undo();
        ss.useTotal = true;
        ss.total = 10;
        ss.effect = "Random";
        ss.files = {"synthpic:3", "synthpic:4", "synthpic:5"};
        const std::vector<uint64_t> m2 = makeSlideshow(ss, &serr);
        const Event* last = m2.size() == 3 ? ev(m2[2]) : nullptr;
        check(last && std::fabs(last->end() - 40.0) < 0.05 && last->panCrop.empty(), "slideshow: Total length shares 10 s out among the pictures (no Pan/Crop without Pan or Zoom)",
              last ? num(last->end()) : "");
        bool anyTransition = false;
        for (const VideoFxEntry& v : Catalog::get().video()) if (v.transition && v.ok) anyTransition = true;
        check(!anyTransition || (m2.size() == 3 && ev(m2[1])->transition.kind == "ofx" && ev(m2[0])->transition.kind.empty()),
              "slideshow: Random puts a transition on each overlap (if any are installed)");
        app.undo();
        check(makeSlideshow(SlideshowSettings(), &serr).empty() && !serr.empty(), "slideshow: no pictures, nothing made (and why)");
        // the dialog: pictures put in its list, Insert Slideshow
        slideshowDialogShown = [](QWidget* d) {
            QListWidget* list = d->findChild<QListWidget*>("slideshowPictures");
            for (const char* f : {"synthpic:3", "synthpic:4"}) {
                QListWidgetItem* it = new QListWidgetItem(f, list);
                it->setData(Qt::UserRole, QString(f));
            }
            if (QComboBox* e = d->findChild<QComboBox*>("slideshowEffect")) e->setCurrentText("Fades with Zoom");
            if (QDoubleSpinBox* o = d->findChild<QDoubleSpinBox*>("slideshowOverlap")) o->setValue(0.5);
            if (QRadioButton* r = d->findChild<QRadioButton*>("slideshowPerPicture")) r->setChecked(true);
            if (QDoubleSpinBox* p = d->findChild<QDoubleSpinBox*>("slideshowPerSeconds")) p->setValue(3);
            if (QDoubleSpinBox* p = d->findChild<QDoubleSpinBox*>("slideshowPerSeconds")) emit p->valueChanged(3);
            d->grab().save(QString(qgetenv("TUTOR_OUT")) + "/slideshow_creator.png");
            if (QPushButton* ok = d->findChild<QPushButton*>("ok")) ok->click();
        };
        qputenv("TUTOR_OUT", out.toUtf8());
        const size_t u1 = app.undoCount();
        runSlideshowCreator(nullptr);
        slideshowDialogShown = nullptr;
        bool two = app.undoCount() == u1 + 1 && app.undoName() == "Insert Slideshow" && app.selection().size() == 2;
        for (uint64_t id : app.selection()) {
            const Event* e = ev(id);
            if (!e || std::fabs(e->length - 3) > 1e-6 || e->panCrop.size() != 2) two = false;
        }
        check(two, "slideshow: the Slideshow Creator dialog makes it (two pictures, 3 s, zooming)");
        app.undo();
    }

    // ================================================================== the Undo / Redo drop-downs list the edit history
    {
        MainWindow w;
        w.resize(1200, 800);
        w.show();
        uitest::settle(200);
        app.newProject();
        for (const char* n : {"Step One", "Step Two", "Step Three"})
            app.edit(n, [](Project& p) { p.settings.width += 2; });
        QMenu* um = w.findChild<QMenu*>("undoMenu");
        check(um != nullptr, "undo: the Undo button has its drop-down");
        if (um) {
            emit um->aboutToShow();
            QStringList items;
            for (QAction* a : um->actions())
                if (!a->isSeparator()) items << a->text();
            check(items.size() >= 4 && items[0] == "Undo Step Three" && items[1] == "Undo Step Two" && items[2] == "Undo Step One" && items.last() == "Undo All",
                  "undo: the drop-down lists every step by name, the latest first, then Undo All", items.join(" | ").toStdString());
            QAction* second = w.findChild<QAction*>("undoStep2");
            if (second) second->trigger();
            check(app.undoName() == "Step One" && app.redoCount() == 2, "undo: choosing the second undoes it and the one above it", app.undoName());
            QMenu* rm = w.findChild<QMenu*>("redoMenu");
            if (rm) {
                emit rm->aboutToShow();
                QStringList r;
                for (QAction* a : rm->actions())
                    if (!a->isSeparator()) r << a->text();
                check(r.size() >= 2 && r[0] == "Redo Step Two" && r[1] == "Redo Step Three", "undo: the Redo drop-down lists what can be redone", r.join(" | ").toStdString());
            }
        }
        // not closed: closing asks whether to save the changed project
        return uitest::summary();
    }
}
