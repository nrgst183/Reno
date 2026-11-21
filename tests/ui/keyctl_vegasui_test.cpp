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

        // ---- Manual / Split Manual: the key's handles shape the curve, and are dragged in Curves
        inst->userSetKeyCurve(hp, kt, ofx::Param::KeyManual);
        settle(100);
        std::vector<double> h0;
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            if (hp->keyHandles.count(kt)) h0 = hp->keyHandles[kt];
        }
        check(h0.size() == 4 && h0[0] < 0 && h0[2] > 0, "a key made Manual gets an in and an out handle", strf("%.0f handles", (double)h0.size()));
        const double mid = std::floor((28 + 85) * 0.5 * fps), linearMid = 0.1 + (0.6 - 0.1) * (mid - kt) / (std::floor(85 * fps) - kt);
        if (h0.size() == 4) {
            std::vector<double> lifted = h0;
            lifted[3] = 0.9;                                        // the out handle well above the line
            inst->userSetKeyHandles(hp, kt, lifted);
        }
        const double bent = hp->valueAt(mid)[0];
        check(bent > linearMid + 0.05, "a handle pulled up bends the curve up after the key", strf("%.3f against %.3f", bent, linearMid));
        if (h0.size() == 4) inst->userSetKeyHandles(hp, kt, h0);
        settle(100);
        uitest::shot(w, gOut + "/keyctl_manual.png");
        // drag the out handle up: on a Manual key the in handle turns with it (down)
        auto handleY = [&](double v) { return (int)std::lround(bottom - v * (bottom - top)); };
        auto outHandlePos = [&](const std::vector<double>& h) { return QPoint(frameX(strip, kt + h[2], len), handleY(h[3])); };
        std::vector<double> h1;
        if (h0.size() == 4) {
            uitest::mouseDrag(strip, outHandlePos(h0), outHandlePos(h0) + QPoint(0, -30), 8);
            settle(100);
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            h1 = hp->keyHandles[kt];
        }
        check(h1.size() == 4 && h1[3] > h0[3] + 0.02 && h1[1] < h0[1] - 0.005, "dragging a Manual key's out handle up turns its in handle down",
              h1.size() == 4 ? strf("out %.3f, in %.3f", h1[3], h1[1]) : std::string("no handles"));
        check(app.undoName() == "Keyframe Handle", "one undo step", app.undoName());
        // Split Manual: the in handle stays
        inst->userSetKeyCurve(hp, kt, ofx::Param::KeySplit);
        settle(100);
        std::vector<double> h2;
        if (h1.size() == 4) {
            uitest::mouseDrag(strip, outHandlePos(h1), outHandlePos(h1) + QPoint(0, -20), 8);
            settle(100);
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            h2 = hp->keyHandles[kt];
        }
        check(h2.size() == 4 && h2[3] > h1[3] + 0.01 && std::fabs(h2[1] - h1[1]) < 1e-9, "on a Split Manual key the other handle stays",
              h2.size() == 4 ? strf("out %.3f, in %.3f", h2[3], h2[1]) : std::string("no handles"));
        // the key's value moved: its handles go with it
        {
            const double before = h2.size() == 4 ? h2[3] : 0;
            inst->userSetValue(hp, {0.3}, kt, true);
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            check(hp->keyHandles.count(kt) && std::fabs(hp->keyHandles[kt][3] - (before + 0.2)) < 1e-9, "raising the key raises its handles with it");
        }
        // kept in the effect's state, and read back
        {
            const Json st = inst->saveState();
            const Json kh = st.get("params").get("HorizontalPixelization").get("kh");
            std::vector<double> saved;
            {
                std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                saved = hp->keyHandles[kt];
            }
            inst->userSetKeyHandles(hp, kt, h0);
            inst->loadState(st);
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            check(kh.size() == 1 && hp->keyCurve(kt) == ofx::Param::KeySplit && hp->keyHandles.count(kt) && hp->keyHandles[kt] == saved,
                  "the curve and its handles are kept in the project (\"kh\")");
        }
        app.undo();
        app.undo();
        settle(100);
        w->findChild<QToolButton*>("fxKeyLanes")->click();
        settle(80);
        check(!strip->property("curvesView").toBool(), "Lanes switches back");
    }

    delete w;
    return uitest::summary();
}
