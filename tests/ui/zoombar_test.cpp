// The timeline's bottom scroll bar is VEGAS's zoom bar: its handle must always be the visible time as a part of the whole scrollable
// time (the project's length, or further when the view has been taken past it), and sit where the view starts. Checked after every
// way of zooming (wheel, keys, buttons, zoom to selection, fit, dragging the handle's ends, double click), of scrolling (shift+wheel,
// dragging the handle, the groove, the cursor leaving the view) and of changing the view's size (opening a project, a length change,
// window width, track list width), with a short and a long project.
//
//   zoombar_test <output folder> [sample .veg folder]
//   (the sample VEGAS projects default to the folder named by $RENO_SAMPLES; without them those parts SKIP)
//
// Build: with uibuild (tests/ui/stubs, docs/NOTES.md)
#include "../../src/app/cachedscroll.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/theme.h"
#include "../../src/app/timeline.h"
#include "../../src/vegas/vegas.h"
#include "uitestlib.h"
#include "../testdata.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

struct Box { double g0 = 0, glen = 0, a = 0, b = 0; };
static Box boxOf(QScrollBar* bar)
{
    QStyleOptionSlider opt;
    opt.initFrom(bar);
    opt.orientation = bar->orientation();
    opt.minimum = bar->minimum();
    opt.maximum = bar->maximum();
    opt.pageStep = bar->pageStep();
    opt.singleStep = bar->singleStep();
    opt.sliderPosition = opt.sliderValue = bar->value();
    opt.subControls = QStyle::SC_All;
    const QRect g = bar->style()->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarGroove, bar);
    const QRect s = bar->style()->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarSlider, bar);
    Box b;
    b.g0 = g.left();
    b.glen = g.width();
    b.a = s.left();
    b.b = s.right() + 1;
    return b;
}

static TimelineWidget* gW = nullptr;
static QScrollBar* gBar = nullptr;
static int gChecks = 0, gBad = 0;

// The handle against the view. 'total' is what the whole groove stands for: the project's length, or the view's end when that is
// further (the view taken past the end). As in VEGAS (measured from VEGAS Pro 18) the handle
// is never shorter than 20 pixels and the rest of the groove is the time scale: its length is 20 + (groove - 20) * view / total and it
// starts at (groove - 20) * view start / total.
static void verify(const std::string& what, double projectLength)
{
    uitest::settle(20);
    ++gChecks;
    const double vs = gW->viewStartTime(), ve = gW->viewEndTime(), span = ve - vs;
    // an empty project stands for zoom to fit's 30 s
    const double total = std::max(projectLength > 0 ? projectLength : 30.0 * 1.04, ve);
    const Box b = boxOf(gBar);
    const double minBox = 20.0;
    const double wantLen = std::min(b.glen, minBox + (b.glen - minBox) * span / total);
    const double len = b.b - b.a;
    bool ok = b.glen > 20;
    std::string why;
    if (std::fabs(len - wantLen) > 2.0) {
        ok = false;
        why += strfmt(" length %.1f wanted %.1f;", len, wantLen);
    }
    const double wantPos = wantLen >= b.glen ? 0.0 : (b.glen - minBox) * vs / total;
    if (std::fabs((b.a - b.g0) - wantPos) > 2.0) {
        ok = false;
        why += strfmt(" place %.1f wanted %.1f;", b.a - b.g0, wantPos);
    }
    if (!ok) ++gBad;
    check(ok, what, strfmt("view %.3f-%.3f of %.3f, groove %.0f, box %.1f-%.1f:%s", vs, ve, total, b.glen, b.a - b.g0, b.b - b.g0, why.c_str()));
}

static void scenario(const std::string& name, double length, const QString& out)
{
    TimelineWidget& w = *gW;
    const std::string n = name + ": ";
    auto len = [&] { return AppState::get().project().length(); };
    (void)length;
    w.resize(1400, 520);
    w.zoomToFit();
    uitest::settle(100);
    verify(n + "zoomed to fit", len());
    {
        const Box b = boxOf(gBar);
        check(b.b - b.a >= b.glen - 3, n + "zoomed to fit the handle fills the bar", strfmt("%.1f of %.1f", b.b - b.a, b.glen));
    }
    uitest::shot(&w, out + "/zb_" + QString::fromStdString(name) + "_fit.png");

    // ---- zooming
    for (int k = 0; k < 4; ++k) {
        uitest::wheel(&w, QPoint(700, 200), 120);
        verify(n + strfmt("wheel zoom in %d", k + 1), len());
    }
    uitest::shot(&w, out + "/zb_" + QString::fromStdString(name) + "_wheel_in.png");
    for (int k = 0; k < 2; ++k) {
        uitest::key(&w, Qt::Key_Down);
        verify(n + "Down key (zoom out)", len());
    }
    for (int k = 0; k < 3; ++k) {
        uitest::key(&w, Qt::Key_Up);
        verify(n + "Up key (zoom in)", len());
    }
    if (QWidget* bt = w.findChild<QWidget*>("zoomInButton")) {
        uitest::mouseClick(bt, QPoint(bt->width() / 2, bt->height() / 2));
        verify(n + "zoom in button", len());
    }
    if (QWidget* bt = w.findChild<QWidget*>("zoomOutButton")) {
        uitest::mouseClick(bt, QPoint(bt->width() / 2, bt->height() / 2));
        uitest::mouseClick(bt, QPoint(bt->width() / 2, bt->height() / 2));
        verify(n + "zoom out button twice", len());
    }
    uitest::key(&w, Qt::Key_Up, Qt::ControlModifier);
    verify(n + "Ctrl+Up", len());
    uitest::key(&w, Qt::Key_Down, Qt::ControlModifier);
    verify(n + "Ctrl+Down", len());
    for (int k = 0; k < 12; ++k) uitest::wheel(&w, QPoint(700, 200), -120);
    verify(n + "wheel zoomed right out", len());
    uitest::shot(&w, out + "/zb_" + QString::fromStdString(name) + "_out.png");
    for (int k = 0; k < 40; ++k) uitest::wheel(&w, QPoint(700, 200), 120);
    verify(n + "wheel zoomed right in", len());
    uitest::shot(&w, out + "/zb_" + QString::fromStdString(name) + "_max_in.png");
    w.zoomToFit();
    for (int k = 0; k < 3; ++k) uitest::wheel(&w, QPoint(700, 200), 120);

    // zoom to the time selection
    {
        AppState& app = AppState::get();
        const double L = len();
        app.setLoop(L * 0.3, L * 0.4);
        w.zoomLarge(true);
        verify(n + "zoom to the time selection", len());
        app.setLoop(0, 0);
    }
    w.zoomToFit();
    for (int k = 0; k < 3; ++k) uitest::wheel(&w, QPoint(700, 200), 120);

    // ---- scrolling
    for (int k = 0; k < 3; ++k) {
        uitest::wheel(&w, QPoint(700, 200), -120, Qt::ShiftModifier);
        verify(n + "shift+wheel scroll right", len());
    }
    uitest::wheel(&w, QPoint(700, 200), 120, Qt::ShiftModifier);
    verify(n + "shift+wheel scroll left", len());
    {
        const Box b = boxOf(gBar);
        uitest::mouseDrag(gBar, QPoint((int)((b.a + b.b) / 2), gBar->height() / 2), QPoint((int)((b.a + b.b) / 2) + 90, gBar->height() / 2));
        verify(n + "handle dragged right", len());
        uitest::shot(&w, out + "/zb_" + QString::fromStdString(name) + "_dragged.png");
        const Box c = boxOf(gBar);
        uitest::mouseClick(gBar, QPoint((int)c.b + 30, gBar->height() / 2));
        verify(n + "groove clicked (page right)", len());
    }
    gBar->setValue(gBar->maximum());
    verify(n + "scrolled to the far end", len());
    gBar->setValue(0);
    verify(n + "scrolled to the start", len());
    {
        // the cursor leaves the view (playing follows it): the page turns
        AppState& app = AppState::get();
        app.setCursor(len() * 0.8);
        w.scrollToCursor();
        verify(n + "the view turned to a cursor far on", len());
        app.setCursor(len() * 0.05);
        w.scrollToCursor();
        verify(n + "the view turned back to the cursor", len());
        w.centerOnCursor();
        verify(n + "centered on the cursor", len());
        app.setCursor(0);
    }

    // the cursor and a time selection set beyond the project's end do not change the bar (nor the view)
    {
        AppState& app = AppState::get();
        w.zoomToFit();
        for (int k = 0; k < 2; ++k) uitest::wheel(&w, QPoint(700, 200), 120);
        const double vs = w.viewStartTime(), ve = w.viewEndTime();
        const int max0 = gBar->maximum(), page0 = gBar->pageStep();
        app.setCursor(len() * 1.5);
        verify(n + "cursor set past the end", len());
        app.setLoop(len() * 1.2, len() * 1.4);
        verify(n + "time selection set past the end", len());
        check(gBar->maximum() == max0 && gBar->pageStep() == page0 && w.viewStartTime() == vs && w.viewEndTime() == ve, n + "the bar's range did not change for them");
        app.setLoop(0, 0);
        app.setCursor(0);
        verify(n + "cursor and selection back", len());
    }

    // ---- dragging the handle's ends
    {
        w.zoomToFit();
        for (int k = 0; k < 4; ++k) uitest::wheel(&w, QPoint(700, 200), 120);
        gBar->setValue(gBar->maximum() / 3);
        uitest::settle(30);
        Box b = boxOf(gBar);
        const int y = gBar->height() / 2;
        const double vs0 = w.viewStartTime(), ve0 = w.viewEndTime();
        uitest::mouseDrag(gBar, QPoint((int)b.b - 2, y), QPoint((int)b.b - 2 + 70, y));
        verify(n + "right end dragged out", len());
        check(std::fabs(w.viewStartTime() - vs0) < 1e-6, n + "right end dragged out: the left edge stays", strfmt("%.5f -> %.5f", vs0, w.viewStartTime()));
        b = boxOf(gBar);
        uitest::mouseDrag(gBar, QPoint((int)b.b - 2, y), QPoint((int)b.b - 2 - 50, y));
        verify(n + "right end dragged in", len());
        b = boxOf(gBar);
        const double ve1 = w.viewEndTime();
        uitest::mouseDrag(gBar, QPoint((int)b.a + 2, y), QPoint((int)b.a + 2 + 40, y));
        verify(n + "left end dragged in", len());
        check(std::fabs(w.viewEndTime() - ve1) < 1e-3 * std::max(1.0, ve1), n + "left end dragged in: the right edge stays", strfmt("%.5f -> %.5f", ve1, w.viewEndTime()));
        b = boxOf(gBar);
        uitest::mouseDrag(gBar, QPoint((int)b.a + 2, y), QPoint((int)b.a + 2 - 30, y));
        verify(n + "left end dragged out", len());
        (void)ve0;
        uitest::shot(&w, out + "/zb_" + QString::fromStdString(name) + "_ends.png");
        // the same end dragged again (the handle must be where the pointer left it, the next press must not jump)
        b = boxOf(gBar);
        const double s1 = w.viewEndTime() - w.viewStartTime();
        uitest::mousePress(gBar, QPoint((int)b.b - 2, y));
        uitest::mouseRelease(gBar, QPoint((int)b.b - 2, y));
        check(std::fabs((w.viewEndTime() - w.viewStartTime()) - s1) < 1e-6 * std::max(1.0, s1), n + "pressing and letting go of an end changes nothing");
        verify(n + "an end pressed and let go", len());
        uitest::mouseDoubleClick(gBar, QPoint((int)((b.a + b.b) / 2), y));
        verify(n + "double click zooms out to everything", len());
    }

    // ---- the view's size
    {
        w.zoomToFit();
        for (int k = 0; k < 3; ++k) uitest::wheel(&w, QPoint(700, 200), 120);
        gBar->setValue(gBar->maximum() / 2);
        verify(n + "before the window is resized", len());
        w.resize(900, 520);
        verify(n + "window narrower", len());
        w.resize(1700, 520);
        verify(n + "window wider", len());
        const int hw = w.headerWidth();
        w.setHeaderWidth(hw + 120);
        verify(n + "track list wider", len());
        w.setHeaderWidth(std::max(80, hw - 40));
        verify(n + "track list narrower", len());
        w.setHeaderWidth(hw);
        w.resize(1400, 520);
        verify(n + "back to the first size", len());
    }

    // ---- the project's length changes
    {
        AppState& app = AppState::get();
        const Project before = app.project();
        w.zoomToFit();
        for (int k = 0; k < 3; ++k) uitest::wheel(&w, QPoint(700, 200), 120);
        verify(n + "zoomed in before a length change", len());
        uint64_t added = 0;
        app.edit("longer", [&](Project& p) {
            for (Track& t : p.tracks)
                if (t.type == TrackType::Video && !t.events.empty()) {
                    Event e = t.events.back();
                    e.id = p.newId();
                    e.start = p.length() + 5.0;
                    e.length = 60;
                    added = e.id;
                    t.events.push_back(e);
                    return;
                }
        });
        verify(n + "project made longer", len());
        app.undo();
        verify(n + "project made shorter again (undo)", len());
        (void)added;
        (void)before;
    }
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoZoomBarTest");
    QApplication::setApplicationName("zoombar_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("zoombarout");
    const QString samples = args.size() > 3 ? args[3] : QString(qEnvironmentVariable("RENO_SAMPLES"));
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    setProjectFileLoader([](const std::string& path, Project& o, std::string* error) { return loadProjectFile(path, o, error); });
    AppState& app = AppState::get();
    app.newProject();
    uitest::buildDemoProject();

    Playback playback;
    TimelineWidget w(&playback);
    gW = &w;
    w.resize(1400, 520);
    w.show();
    uitest::settle(300);
    gBar = w.findChild<QScrollBar*>("timelineHScroll");
    if (!gBar) {
        check(false, "the timeline has its scroll bar");
        return uitest::summary();
    }

    scenario("short", 0, out);

    // a long project: one video event 4007 s long (1:06:47, as the user's longest)
    app.newProject();
    uitest::buildDemoProject();
    app.edit("long", [](Project& p) {
        for (Track& t : p.tracks)
            if (t.type == TrackType::Video && !t.events.empty()) {
                t.events.back().length = 4007.0;
                return;
            }
    });
    uitest::settle(100);
    w.zoomToFit();
    scenario("long", 4007, out);

    // opening a project: the bar shows the zoom of the project opened
    {
        QString veg;
        {   // $RENO_SAMPLE_VEG, else the .veg under the samples folder with the longest timeline
            double longest = 0;
            std::vector<std::string> all = testVegFiles(samples.toStdString());
            if (!testSampleVeg().empty()) all = {testSampleVeg()};
            for (const std::string& f : all) {
                Project pr;
                if (importVegasProject(f, pr).ok && pr.length() > longest) { longest = pr.length(); veg = QString::fromStdString(f); }
            }
        }
        if (!veg.isEmpty()) {
            std::string err;
            check(app.open(veg.toStdString(), &err), "the sample project opens", err);
            uitest::settle(400);
            verify("opened the sample: at once", app.project().length());
            w.zoomToFit();
            verify("opened the sample: fit", app.project().length());
            uitest::shot(&w, out + "/zb_sample_fit.png");
            scenario("sample", app.project().length(), out);
            app.newProject();
            uitest::settle(200);
            verify("a new project after it", app.project().length());
        } else {
            std::printf("(no sample project: the opening of a real project is not tried)\n");
        }
    }
    // an empty project, zoomed in: the box can be dragged along and stays under the pointer the whole way (it must not snap back while the view's end makes the whole the bar stands for)
    {
        AppState& a = AppState::get();
        a.newProject();
        gW->resize(1400, 520);
        gW->zoomToFit();
        uitest::settle(100);
        for (int k = 0; k < 3; ++k) uitest::key(gW, Qt::Key_Up);
        uitest::settle(50);
        verify("empty project: zoomed in", 0);
        const Box b = boxOf(gBar);
        const int y = gBar->height() / 2;
        const QPoint from((int)((b.a + b.b) / 2), y);
        uitest::mousePress(gBar, from);
        bool along = true, under = true;
        double last = gW->viewStartTime();
        std::string why;
        for (int dx = 20; dx <= 300; dx += 20) {
            uitest::mouseMove(gBar, from + QPoint(dx, 0), Qt::LeftButton);
            const double st = gW->viewStartTime();
            const Box bn = boxOf(gBar);
            if (st < last - 1e-9) { along = false; why += strfmt(" back at +%d (%.3f -> %.3f);", dx, last, st); }
            if (std::fabs((bn.a + bn.b) / 2 - (from.x() + dx)) > 3 && (bn.b < bn.g0 + bn.glen - 1)) { under = false; why += strfmt(" box %.0f pointer %d;", (bn.a + bn.b) / 2, from.x() + dx); }
            last = st;
        }
        uitest::mouseRelease(gBar, from + QPoint(300, 0));
        uitest::settle(30);
        check(along && under && last > 0, "empty project: the box drags along without snapping back", why + strfmt(" view start %.3f", last));
        verify("empty project: after the drag", 0);
    }
    std::printf("zoom bar checks: %d, wrong: %d\n", gChecks, gBad);
    return uitest::summary();
}
