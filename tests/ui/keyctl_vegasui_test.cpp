// The effects window's keyframe controller (src/app/fxwindow.cpp) as VEGAS 18 has it (manual pages 268-271, its pictures
// of the Video Event FX window): the buttons under it in VEGAS's order (Sync Cursor, First, Previous, Next, Last, Create,
// Delete Keyframe), the cursor's time in the project's time format, the top row named after the effect, Sync Cursor,
// zoom, and the Curves view with its keyframes dragged up and down. Uses VEGAS's built-in Pixelate, so no plug-in is needed.
//
//   keyctl_vegasui_test <output folder>
//
// Prints PASS / FAIL lines and a summary; exits non-zero on failure. Pictures of the window go in the output folder.
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/engine/timefmt.h"
#include "../../src/ofx/ofxhost.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QScrollBar>
#include <QSettings>
#include <QToolButton>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

void settle(int ms = 120)
{
    uitest::settle(ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

std::string strf(const char* f, double a, double b = 0)
{
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

// KeyStrip's geometry: a 150-pixel name column, 6-pixel margins, an 18-pixel ruler and 18-pixel rows.
const int kLabelW = 150, kRuler = 18, kRowH = 18;
int frameX(QWidget* strip, double f, double len) { return (int)std::lround(kLabelW + 6 + (strip->width() - kLabelW - 12) * (f / len)); }

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("keyctl_vegasui_test");
    gOut = argc > 1 ? QString(argv[1]) : QString("keyctl_out");
    QDir().mkpath(gOut);
    applyTheme();
    QSettings().remove("fxWindow");
    uitest::setup({}, {});

    // an event of 100 seconds with Pixelate on it (VEGAS's picture: Pixelate's keyframes over 1:30)
    AppState& app = AppState::get();
    app.newProject();
    const uint64_t bars = app.addGeneratedMedia("com.vegascreativesoftware:testpattern", "Test Pattern");
    const std::vector<uint64_t> evs = app.addMediaToTimeline(bars, 0.0);
    check(!evs.empty(), "an event of the test pattern");
    if (evs.empty()) return uitest::summary();
    const uint64_t ev = evs[0];
    app.edit("Length", [&](Project& p) { if (Event* e = p.findEvent(ev)) e->length = 100.0; });
    FxTarget t;
    t.kind = FxTarget::Event;
    t.id = ev;
    const uint64_t slot = app.addVideoFx(t, "com.vegascreativesoftware:pixelate", "Pixelate");
    check(slot != 0, "Pixelate on the event");
    ofx::Instance* inst = app.fx.video(slot);
    ofx::Param* hp = inst ? inst->params.find("HorizontalPixelization") : nullptr;
    ofx::Param* vp = inst ? inst->params.find("VerticalPixelization") : nullptr;
    check(hp && vp, "its two settings");
    if (!hp || !vp) return uitest::summary();
    const double fps = app.project().settings.fps(), len = std::floor(100.0 * fps + 0.5);
    // keyframes like VEGAS's picture
    inst->userSetValue(hp, {0.0}, 0.0, true);
    inst->userSetValue(hp, {0.1}, std::floor(28 * fps), true);
    inst->userSetValue(hp, {0.6}, std::floor(85 * fps), true);
    inst->userSetValue(vp, {0.0}, 0.0, true);
    inst->userSetValue(vp, {0.2}, std::floor(28 * fps), true);
    inst->userSetValue(vp, {0.7}, std::floor(85 * fps), true);

    app.setCursor(30.0);
    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    w->resize(560, 640);
    w->show();
    w->showTarget(t, slot);
    settle(300);
    QToolButton* animate = w->findChild<QToolButton*>("fxAnimate");
    check(animate != nullptr, "the Animate button");
    if (!animate) return uitest::summary();
    if (!animate->isChecked()) animate->click();
    settle(200);
    QWidget* strip = w->findChild<QWidget*>("fxKeyStrip");
    check(strip && strip->isVisible(), "Animate shows the keyframe controller");
    if (!strip) return uitest::summary();

    // ---------------------------------------------------------------- the buttons, in VEGAS 18's order
    {
        const char* names[] = {"fxKeySync", "fxKeyFirst", "fxKeyPrev", "fxKeyNext", "fxKeyLast", "fxKeyAdd", "fxKeyDelete", "fxKeyTime", "fxKeyLanes", "fxKeyCurves"};
        int lastX = -1;
        bool inOrder = true, all = true;
        std::string seen;
        for (const char* n : names) {
            QWidget* b = w->findChild<QWidget*>(n);
            if (!b || !b->isVisible()) { all = false; seen += std::string(n) + "(missing) "; continue; }
            const int x = b->mapTo(w, QPoint(0, 0)).x();
            seen += std::string(n) + "@" + std::to_string(x) + " ";
            if (x <= lastX) inOrder = false;
            lastX = x;
        }
        check(all && inOrder, "Sync Cursor, First, Previous, Next, Last, Create, Delete Keyframe, the time, Lanes | Curves: left to right", seen);
        QToolButton* sync = w->findChild<QToolButton*>("fxKeySync");
        check(sync && sync->isCheckable() && sync->isChecked(), "Sync Cursor is a toggle, on at first");
        QToolButton* lanes = w->findChild<QToolButton*>("fxKeyLanes");
        check(lanes && lanes->isChecked() && lanes->text() == "Lanes", "the Lanes view at first");
        check(w->findChild<QScrollBar*>("fxKeyHScroll") && w->findChild<QToolButton*>("fxKeyZoomIn") && w->findChild<QToolButton*>("fxKeyZoomOut"),
              "a scroll bar with + and - (zoom) under the keyframes");
    }

    // ---------------------------------------------------------------- the rows: the top one named after the effect
    {
        const QStringList rows = strip->property("rowLabels").toStringList();
        check(rows.size() == 3 && rows[0] == "Pixelate", "the top row is the effect's name (Pixelate), then its settings", rows.join(" | ").toStdString());
        // fold the effect's rows with the triangle, and unfold
        uitest::mouseClick(strip, QPoint(8, kRuler + kRowH / 2));
        settle(50);
        check(strip->property("rowLabels").toStringList().size() == 1, "the triangle folds the effect's settings away");
        uitest::mouseClick(strip, QPoint(8, kRuler + kRowH / 2));
        settle(50);
        check(strip->property("rowLabels").toStringList().size() == 3, "and unfolds them");
    }

    // ---------------------------------------------------------------- the time readout
    QLabel* where = w->findChild<QLabel*>("fxKeyTime");
    {
        app.setCursor(65.0 + 25.0 / fps);       // VEGAS's picture: 00:01:05,25
        settle(100);
        const std::string want = formatTime(app.project().settings, 65.0 + 25.0 / fps, app.project().settings.rulerFormat);
        check(where && where->text().toStdString() == want, "the cursor's time in the project's time format (from the event's start)",
              (where ? where->text().toStdString() : std::string("none")) + " / " + want);
        check(where && !where->text().contains("frame"), "no \"frame N\" in it");
        uitest::shot(w, gOut + "/keyctl_lanes.png");
    }

    // ---------------------------------------------------------------- Sync Cursor
    {
        QToolButton* sync = w->findChild<QToolButton*>("fxKeySync");
        uitest::mouseClick(strip, QPoint(frameX(strip, 40 * fps, len), kRuler + 3 * kRowH + 6));
        settle(80);
        check(std::fabs(app.cursor() - 40.0) < 0.05, "Sync Cursor on: a click in the keyframes moves the timeline's cursor", strf("%.3f", app.cursor()));
        app.setCursor(50.0);
        settle(80);
        const std::string at50 = where->text().toStdString();
        sync->click();
        settle(80);
        uitest::mouseClick(strip, QPoint(frameX(strip, 20 * fps, len), kRuler + 3 * kRowH + 6));
        settle(80);
        check(std::fabs(app.cursor() - 50.0) < 1e-6, "Sync Cursor off: the timeline's cursor stays", strf("%.3f", app.cursor()));
        const std::string at20 = formatTime(app.project().settings, 20.0, app.project().settings.rulerFormat);
        check(where->text().toStdString() == at20, "and the controller has its own cursor", where->text().toStdString() + " / " + at20);
        app.setCursor(70.0);
        settle(80);
        check(where->text().toStdString() == at20, "the timeline's cursor moving leaves it", where->text().toStdString());
        sync->click();
        settle(80);
        const std::string at70 = formatTime(app.project().settings, 70.0, app.project().settings.rulerFormat);
        check(where->text().toStdString() == at70, "Sync Cursor on again: it follows the timeline's", where->text().toStdString() + " / " + at70);
        (void)at50;
    }

    // ---------------------------------------------------------------- zoom
    {
        QScrollBar* sb = w->findChild<QScrollBar*>("fxKeyHScroll");
        check(sb->maximum() == 0, "zoomed out: nothing to scroll");
        w->findChild<QToolButton*>("fxKeyZoomIn")->click();
        settle(60);
        check(sb->maximum() > 0, "+ zooms in: the scroll bar has room", strf("%.0f", sb->maximum()));
        uitest::shot(w, gOut + "/keyctl_zoomed.png");
        w->findChild<QToolButton*>("fxKeyZoomOut")->click();
        settle(60);
        check(sb->maximum() == 0, "- zooms out again");
    }

    // ---------------------------------------------------------------- the Curves view
    {
        app.setCursor(30.0);
        QToolButton* curves = w->findChild<QToolButton*>("fxKeyCurves");
        curves->click();
        settle(150);
        check(strip->property("curvesView").toBool(), "Curves switches the view");
        uitest::shot(w, gOut + "/keyctl_curves.png");
        // drag Horizontal pixelization's keyframe at 28 s up: its value grows; sideways: its time moves
        const double kt = std::floor(28 * fps);
        const double top = kRuler + 8.0, bottom = strip->height() - 8.0;
        const int y = (int)std::lround(bottom - 0.1 * (bottom - top));
        const QPoint from(frameX(strip, kt, len), y);
        uitest::mouseDrag(strip, from, from + QPoint(0, -40), 8);
        settle(100);
        double v = -1;
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            auto it = hp->keys.find(kt);
            if (it != hp->keys.end()) v = it->second[0];
        }
        const double want = 0.1 + 40.0 / (bottom - top);
        check(std::fabs(v - want) < 0.02, "dragging a keyframe up in Curves raises its value", strf("%.3f (want %.3f)", v, want));
        check(app.undoName() == "Keyframe Value", "one undo step", app.undoName());
        app.undo();
        settle(100);
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            check(hp->keys.count(kt) && std::fabs(hp->keys[kt][0] - 0.1) < 1e-9, "undone");
        }
        const QPoint from2(frameX(strip, kt, len), y);
        uitest::mouseDrag(strip, from2, QPoint(frameX(strip, 35 * fps, len), y), 8);
        settle(100);
        bool moved = false;
        for (const auto& k : hp->keys) moved = moved || std::fabs(k.first - 35 * fps) <= 10;   // (a pixel is several frames here)
        check(moved && !hp->keys.count(kt) && vp->keys.count(kt), "dragging it sideways moves its time (that setting's only)");
        app.undo();
        settle(100);
        w->findChild<QToolButton*>("fxKeyLanes")->click();
        settle(80);
        check(!strip->property("curvesView").toBool(), "Lanes switches back");
    }

    delete w;
    return uitest::summary();
}
