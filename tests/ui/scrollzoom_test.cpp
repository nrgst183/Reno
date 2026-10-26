// The timeline's scroll bars as in VEGAS: the ends of the scroll box are grips that zoom (horizontal bar: time zoom; vertical bar:
// track heights) while the other end of the view stays put; double-clicking a bar zooms out to everything; dragging the box itself
// and clicking the groove still scroll.
//
//   scrollzoom_test <output folder>
//   (the sample VEGAS projects default to the folder named by $RENO_SAMPLES; without them those parts SKIP)
//
// Build: uibuild scrollzoom_test tests/ui/scrollzoom_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/playback.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/cachedscroll.h"
#include "../../src/app/theme.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include "../testdata.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

// The scroll box of a bar in the bar's own pixels (a, b along it), and the groove's start.
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
    const bool v = bar->orientation() == Qt::Vertical;
    const QRect g = bar->style()->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarGroove, bar);
    const QRect s = bar->style()->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarSlider, bar);
    Box b;
    b.g0 = v ? g.top() : g.left();
    b.glen = v ? g.height() : g.width();
    b.a = v ? s.top() : s.left();
    b.b = v ? s.bottom() + 1 : s.right() + 1;
    return b;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoScrollZoomTest");
    QApplication::setApplicationName("scrollzoom_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("scrollzoomout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.newProject();
    uitest::buildDemoProject();

    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1400, 560);
    w.show();
    w.zoomToFit();
    uitest::settle(300);
    QScrollBar* hbar = w.findChild<QScrollBar*>("timelineHScroll");
    QScrollBar* vbar = w.findChild<QScrollBar*>("timelineVScroll");
    if (!hbar || !vbar) {
        check(false, "the timeline has its scroll bars");
        return uitest::summary();
    }
    auto span = [&] { return w.viewEndTime() - w.viewStartTime(); };

    // zoom in a few steps so the box is a part of the groove, and scroll to the middle
    for (int k = 0; k < 3; ++k) w.zoomIn();
    uitest::settle(50);
    hbar->setValue(hbar->maximum() / 3);
    uitest::settle(50);
    uitest::shot(&w, out + "/sz_start.png");

    // ---------------------------------------------------------------- the pointer
    {
        const Box b = boxOf(hbar);
        const int y = hbar->height() / 2;
        uitest::mouseMove(hbar, QPoint((int)b.a + 2, y), Qt::NoButton);
        const bool left = hbar->cursor().shape() == Qt::SizeHorCursor;
        uitest::mouseMove(hbar, QPoint((int)b.b - 2, y), Qt::NoButton);
        const bool right = hbar->cursor().shape() == Qt::SizeHorCursor;
        uitest::mouseMove(hbar, QPoint((int)((b.a + b.b) / 2), y), Qt::NoButton);
        const bool mid = hbar->cursor().shape() == Qt::ArrowCursor;
        uitest::mouseMove(hbar, QPoint((int)b.b + 40, y), Qt::NoButton);
        const bool groove = hbar->cursor().shape() == Qt::ArrowCursor;
        check(left && right && mid && groove, "horizontal bar: resize pointer over either end of the box, arrow over its middle and the groove");
        const Box vb = boxOf(vbar);
        uitest::mouseMove(vbar, QPoint(vbar->width() / 2, (int)vb.a + 2), Qt::NoButton);
        const bool vtop = vbar->cursor().shape() == Qt::SizeVerCursor;
        uitest::mouseMove(vbar, QPoint(vbar->width() / 2, (int)((vb.a + vb.b) / 2)), Qt::NoButton);
        check(vtop && vbar->cursor().shape() == Qt::ArrowCursor, "vertical bar: the vertical resize pointer over the box's ends only");
    }

    // ---------------------------------------------------------------- the right end: zoom out and in, the left edge of the view stays
    {
        const double start0 = w.viewStartTime(), span0 = span();
        const Box b = boxOf(hbar);
        const int y = hbar->height() / 2;
        const QPoint from((int)b.b - 2, y);
        uitest::mouseDrag(hbar, from, from + QPoint(60, 0));
        uitest::settle(50);
        check(std::fabs(w.viewStartTime() - start0) < 1e-6 && span() > span0 * 1.1, "right end dragged out: the view is wider (zoomed out), its left edge stays",
              strfmt("start %.4f -> %.4f, span %.3f -> %.3f", start0, w.viewStartTime(), span0, span()));
        const Box b2 = boxOf(hbar);
        check(std::fabs(b2.b - (b.b + 60)) <= 4, "the box's end follows the pointer", strfmt("end %.1f, wanted %.1f (box %.1f-%.1f, groove %.1f+%.1f, max %d page %d)", b2.b, b.b + 60, b2.a, b2.b, b2.g0, b2.glen, hbar->maximum(), hbar->pageStep()));
        const double span1 = span();
        uitest::mouseDrag(hbar, QPoint((int)b2.b - 2, y), QPoint((int)b2.b - 2 - 80, y));
        uitest::settle(50);
        check(std::fabs(w.viewStartTime() - start0) < 1e-6 && span() < span1 * 0.85, "right end dragged in: zoomed in, left edge stays",
              strfmt("span %.3f -> %.3f", span1, span()));
        uitest::shot(&w, out + "/sz_right_in.png");
    }

    // ---------------------------------------------------------------- the left end: the right edge of the view stays
    {
        hbar->setValue(hbar->maximum() / 3);
        uitest::settle(50);
        const double end0 = w.viewEndTime(), span0 = span();
        const Box b = boxOf(hbar);
        const int y = hbar->height() / 2;
        const QPoint from((int)b.a + 2, y);
        uitest::mouseDrag(hbar, from, from + QPoint(-(int)(b.a - b.g0) / 2, 0));
        uitest::settle(50);
        check(std::fabs(w.viewEndTime() - end0) < 1e-3 && span() > span0 * 1.1, "left end dragged out: zoomed out, the right edge of the view stays",
              strfmt("end %.4f -> %.4f, span %.3f -> %.3f", end0, w.viewEndTime(), span0, span()));
        const Box b2 = boxOf(hbar);
        const double end1 = w.viewEndTime(), span1 = span();
        uitest::mouseDrag(hbar, QPoint((int)b2.a + 2, y), QPoint((int)b2.a + 2 + 70, y));
        uitest::settle(50);
        check(std::fabs(w.viewEndTime() - end1) < 1e-3 && span() < span1 * 0.9, "left end dragged in: zoomed in, the right edge stays",
              strfmt("end %.4f -> %.4f, span %.3f -> %.3f", end1, w.viewEndTime(), span1, span()));
        uitest::shot(&w, out + "/sz_left_in.png");
    }

    // ---------------------------------------------------------------- no jump when an end is grabbed
    {
        // scrolled to the very end (the box against the groove's end), and zoomed in far (Qt's smallest box)
        for (int pass = 0; pass < 2; ++pass) {
            w.zoomToFit();
            for (int k = 0; k < (pass == 0 ? 3 : 7); ++k) w.zoomIn();
            uitest::settle(50);
            hbar->setValue(pass == 0 ? hbar->maximum() : hbar->maximum() / 2);
            uitest::settle(50);
            const char* where = pass == 0 ? "at the end of the scroll range" : "with the smallest box";
            const int y = hbar->height() / 2;
            for (int edge = 0; edge < 2; ++edge) {
                const Box b = boxOf(hbar);
                const double s0 = span(), st0 = w.viewStartTime(), en0 = w.viewEndTime();
                // the seconds one pixel of the groove stands for (VEGAS's scale: the whole over the groove less its 20-pixel least box)
                const double perPixel = std::max(app.project().length() > 0 ? app.project().length() : 31.2, en0) / (b.glen - 20.0);
                const QPoint from(edge == 0 ? (int)b.a + 1 : (int)b.b - 2, y);
                // press and move one pixel inwards: a small change, not a leap
                uitest::mousePress(hbar, from);
                uitest::mouseMove(hbar, from + QPoint(edge == 0 ? 1 : -1, 0), Qt::LeftButton);
                const double s1 = span();
                const bool fixedEnd = edge == 0 ? std::fabs(w.viewEndTime() - en0) < 1e-3 : std::fabs(w.viewStartTime() - st0) < 1e-6;
                // and on, 30 pixels inwards: the box's end under the pointer
                uitest::mouseMove(hbar, from + QPoint(edge == 0 ? 30 : -30, 0), Qt::LeftButton);
                const Box b2 = boxOf(hbar);
                const double endNow = edge == 0 ? b2.a : b2.b, endWanted = (edge == 0 ? b.a : b.b) + (edge == 0 ? 30 : -30);
                uitest::mouseRelease(hbar, from + QPoint(edge == 0 ? 30 : -30, 0));
                uitest::settle(30);
                check(s1 <= s0 && s0 - s1 <= perPixel * 1.5 && fixedEnd,
                      strfmt("%s: grabbing the %s end and moving it a pixel changes the zoom a little", where, edge == 0 ? "left" : "right"),
                      strfmt("span %.4f -> %.4f (a pixel is %.4f s)", s0, s1, perPixel));
                if (pass == 0)
                    check(std::fabs(endNow - endWanted) <= 4, strfmt("%s: the box's %s end stays under the pointer", where, edge == 0 ? "left" : "right"),
                          strfmt("end %.1f, pointer %.1f", endNow, endWanted));
                check(span() < s0, strfmt("%s: dragged in, zoomed in", where), strfmt("span %.4f -> %.4f", s0, span()));
            }
        }
    }

    // ---------------------------------------------------------------- the box stays with the mouse however far off the bar it goes
    check(hbar->style()->pixelMetric(QStyle::PM_MaximumDragDistance, nullptr, hbar) == -1,
          "the style never puts a dragged scroll box back when the mouse goes over the tracks (Windows does at 60 pixels)");

    // ---------------------------------------------------------------- the box stays with the mouse however far off the bar it goes
    check(hbar->style()->pixelMetric(QStyle::PM_MaximumDragDistance, nullptr, hbar) == -1,
          "the style never puts a dragged scroll box back when the mouse goes over the tracks (Windows does at 60 pixels)");

    // ---------------------------------------------------------------- wherever the edit cursor is, every step of a drag is smooth
    {
        const double len = app.project().length();
        const double cursors[] = {0.0, len * 0.5, len * 0.97, len * 1.6, len * 4.0};
        for (double cur : cursors) {
            for (int edge = 0; edge < 3; ++edge) {     // 0, 1: the ends; 2: the middle (scrolling)
                for (int dir = -1; dir <= 1; dir += 2) {
                    w.zoomToFit();
                    for (int k = 0; k < 3; ++k) w.zoomIn();
                    app.setCursor(cur);
                    uitest::settle(20);
                    hbar->setValue(hbar->maximum() / 2);
                    uitest::settle(20);
                    const Box b = boxOf(hbar);
                    const int y = hbar->height() / 2;
                    const QPoint from(edge == 0 ? (int)b.a + 2 : edge == 1 ? (int)b.b - 3 : (int)((b.a + b.b) / 2), y);
                    uitest::mousePress(hbar, from);
                    double lastS = span(), lastSt = w.viewStartTime(), lastEn = w.viewEndTime();
                    // the seconds a pixel of the groove stands for (each step below moves 3)
                    const double perPixel = ((double)hbar->maximum() + hbar->pageStep()) * lastS / hbar->pageStep() / b.glen;
                    bool smooth = true;
                    std::string why;
                    for (int k = 1; k <= 40; ++k) {
                        // the pointer wanders up over the tracks and back as well
                        const QPoint at = from + QPoint(dir * k * 3, (k % 10) * -25);
                        uitest::mouseMove(hbar, at, Qt::LeftButton);
                        const double s = span(), st = w.viewStartTime(), en = w.viewEndTime();
                        // each step moves the dragged side the same way, and only a little; the other side stays
                        const double step = (edge == 0 ? st - lastSt : edge == 1 ? en - lastEn : st - lastSt) * dir;
                        const double other = edge == 0 ? en - lastEn : edge == 1 ? st - lastSt : s - lastS;
                        if (step < -1e-6 || std::fabs(other) > 1e-3 || step > perPixel * 3 * 1.5 + 1e-6) {
                            smooth = false;
                            why = strfmt("step %d: start %.3f -> %.3f, end %.3f -> %.3f", k, lastSt, st, lastEn, en);
                            break;
                        }
                        lastS = s; lastSt = st; lastEn = en;
                    }
                    uitest::mouseRelease(hbar, from + QPoint(dir * 120, 0));
                    uitest::settle(20);
                    check(smooth, strfmt("cursor at %.1f s: dragging the %s %s, every step smooth", cur, edge == 0 ? "left end" : edge == 1 ? "right end" : "box",
                                         dir < 0 ? "left" : "right"), why);
                }
            }
        }
        app.setCursor(0);
    }

    // ---------------------------------------------------------------- limits
    {
        const Box b = boxOf(hbar);
        const int y = hbar->height() / 2;
        // as far in as it goes
        uitest::mouseDrag(hbar, QPoint((int)b.b - 2, y), QPoint((int)b.a + 1, y), 12);
        uitest::settle(50);
        const double minSpan = span();
        const double start = w.viewStartTime();
        check(minSpan > 0 && std::isfinite(minSpan) && start >= 0, "dragging an end past the other one stops at the most zoomed in", strfmt("span %.6f", minSpan));
        // all the way out
        w.zoomToFit();
        uitest::settle(50);
        const Box c = boxOf(hbar);
        uitest::mouseDrag(hbar, QPoint((int)c.b - 2, y), QPoint((int)(c.g0 + c.glen) + 200, y), 12);
        uitest::settle(50);
        check(span() > 0 && std::isfinite(span()) && w.viewStartTime() >= 0, "and out as far as it goes, without going wrong", strfmt("span %.2f", span()));
    }

    // ---------------------------------------------------------------- normal use still works
    {
        w.zoomToFit();
        for (int k = 0; k < 3; ++k) w.zoomIn();
        uitest::settle(50);
        hbar->setValue(0);
        uitest::settle(50);
        const Box b = boxOf(hbar);
        const int y = hbar->height() / 2;
        const double s0 = span(), v0 = w.viewStartTime();
        uitest::mouseDrag(hbar, QPoint((int)((b.a + b.b) / 2), y), QPoint((int)((b.a + b.b) / 2) + 50, y));
        uitest::settle(50);
        check(w.viewStartTime() > v0 + 0.01 && std::fabs(span() - s0) < 1e-6, "dragging the middle of the box scrolls without changing the zoom",
              strfmt("start %.3f -> %.3f, span %.4f -> %.4f", v0, w.viewStartTime(), s0, span()));
        const Box b2 = boxOf(hbar);
        const double v1 = w.viewStartTime();
        uitest::mouseClick(hbar, QPoint((int)b2.b + 20, y));
        uitest::settle(50);
        check(w.viewStartTime() > v1 && std::fabs(span() - s0) < 1e-6, "clicking the groove beyond the box pages along", strfmt("start %.3f -> %.3f", v1, w.viewStartTime()));
    }

    // ---------------------------------------------------------------- double-click: everything in view
    {
        for (int k = 0; k < 3; ++k) w.zoomIn();
        uitest::settle(50);
        const double narrow = span();
        const Box b = boxOf(hbar);
        uitest::mouseDoubleClick(hbar, QPoint((int)((b.a + b.b) / 2), hbar->height() / 2));
        uitest::settle(50);
        check(w.viewStartTime() < 1e-6 && span() > narrow * 3 && w.viewEndTime() >= app.project().length(),
              "double-clicking the horizontal bar zooms out to the whole project", strfmt("span %.2f -> %.2f, project %.2f", narrow, span(), app.project().length()));
    }

    // ---------------------------------------------------------------- the vertical bar: track heights
    {
        // more tracks than fit, so there is a box to take hold of
        app.edit("Heights", [](Project& pr) { for (Track& t : pr.tracks) t.height = 130; });
        uitest::settle(50);
        const Project& p = app.project();
        auto heightSum = [&] { int s = 0; for (const Track& t : p.tracks) s += w.headerRect(t.id).height(); return s; };
        const int sum0 = heightSum();
        vbar->setValue(vbar->maximum() / 3);
        uitest::settle(50);
        check(vbar->maximum() > 0, "(taller tracks: the vertical bar has a box)", strfmt("range %d", vbar->maximum()));
        uitest::shot(&w, out + "/sz_v_start.png");
        const Box b = boxOf(vbar);
        const int x = vbar->width() / 2;
        const size_t undo0 = app.undoCount();
        // the far (bottom) end dragged down: more of the content in view = shorter tracks
        const int sy0 = vbar->value();
        const double top0 = (double)sy0;
        uitest::mouseDrag(vbar, QPoint(x, (int)b.b - 2), QPoint(x, (int)b.b - 2 + (int)(b.glen - b.b) / 2));
        uitest::settle(50);
        const int sum1 = heightSum();
        check(sum1 < sum0, "vertical bar: the bottom end dragged down makes the tracks shorter", strfmt("%d -> %d", sum0, sum1));
        check(std::fabs(vbar->value() - top0 * (double)sum1 / sum0) < 40 + top0 * 0.1, "and the top of the view stays at the same place in the tracks",
              strfmt("scroll %d -> %d (tracks scaled %.2f)", sy0, vbar->value(), (double)sum1 / sum0));
        check(app.undoCount() == undo0 + 1 && app.undoName() == "Zoom Track Height", "the whole drag is one undo step", app.undoName());
        uitest::shot(&w, out + "/sz_v_shorter.png");
        // the top end dragged down: the box gets smaller from the top: tracks taller, bottom stays
        const Box b2 = boxOf(vbar);
        uitest::mouseDrag(vbar, QPoint(x, (int)b2.a + 2), QPoint(x, (int)b2.a + 2 + 40));
        uitest::settle(50);
        check(heightSum() > sum1, "vertical bar: the top end dragged down makes the tracks taller", strfmt("%d -> %d", sum1, heightSum()));
        // the box dragged: scrolls only
        const Box b3 = boxOf(vbar);
        const int sumKeep = heightSum(), v0 = vbar->value();
        if (b3.b - b3.a < b3.glen - 20) {
            uitest::mouseDrag(vbar, QPoint(x, (int)((b3.a + b3.b) / 2)), QPoint(x, (int)((b3.a + b3.b) / 2) + 15));
            uitest::settle(50);
            check(vbar->value() != v0 && heightSum() == sumKeep, "dragging the middle of the vertical box scrolls only", strfmt("%d -> %d", v0, vbar->value()));
        }
        // double-click: as many tracks as possible
        uitest::mouseDoubleClick(vbar, QPoint(x, (int)((b3.a + b3.b) / 2)));
        uitest::settle(50);
        check(vbar->value() == 0 && heightSum() < sumKeep, "double-clicking the vertical bar zooms out to show as many tracks as possible", strfmt("sum %d -> %d", sumKeep, heightSum()));
        uitest::shot(&w, out + "/sz_v_fit.png");
    }

    // ---------------------------------------------------------------- the handle shows the zoom as soon as a project is opened
    {
        // what the horizontal bar should show now: the view's share of the whole (the project's length, or the view's end if that is
        // further), and where the view starts in it
        auto handleOk = [&](const std::string& what) {
            uitest::settle(50);
            const Box b = boxOf(hbar);
            const double vs = w.viewStartTime(), sp = span();
            const double len = app.project().length();
            const double total = std::max(len > 0 ? len : 30.0 * 1.04, vs + sp);  // an empty project: zoom to fit's 30 s
            const double tol = 4.0 / b.glen + 0.01;
            const double gotSize = (b.b - b.a) / b.glen, gotPos = (b.a - b.g0) / b.glen;
            // VEGAS's box: 20 pixels plus the view's share of the rest of the groove, starting at the view start's share of that rest
            // (measured from VEGAS Pro 18)
            const double minBox = 20.0;
            const double wantSize = std::min(1.0, (minBox + (b.glen - minBox) * sp / total) / b.glen);
            const double wantPos = wantSize >= 1.0 ? 0.0 : (b.glen - minBox) * vs / total / b.glen;
            const bool ok = std::fabs(gotSize - wantSize) < tol && std::fabs(gotPos - wantPos) < tol;
            check(ok, "the handle shows the zoom: " + what,
                  strfmt("size %.3f want %.3f, position %.3f want %.3f (view %.2f..+%.2f of %.2f)", gotSize, wantSize, gotPos, wantPos, vs, sp, total));
        };
        // after the drags above, with the kind of zoom a project is opened into
        // two sample projects: $RENO_SAMPLE_VEG (else the first .veg under $RENO_SAMPLES) and, to open right after it, the next one
        std::vector<std::string> sampleVegs = testVegFiles(qEnvironmentVariable("RENO_SAMPLES").toStdString());
        if (!testSampleVeg().empty()) sampleVegs.insert(sampleVegs.begin(), testSampleVeg());
        const QString veg = sampleVegs.empty() ? QString() : QString::fromStdString(sampleVegs.front());
        const QString veg2 = sampleVegs.size() > 1 ? QString::fromStdString(sampleVegs[1]) : veg;
        std::string err;
        const bool haveVeg = !veg.isEmpty() && QFile::exists(veg);
        if (!haveVeg) std::printf("SKIP: the real-project part (set RENO_SAMPLES to the folder of sample VEGAS projects, or RENO_SAMPLE_VEG)\n");
        bool opened = haveVeg && app.open(veg.toStdString(), &err);
        if (haveVeg) check(opened, "(a real project opens)", err);
        if (opened) {
            handleOk("right after File > Open");
            w.zoomToFit();
            handleOk("after zoom to fit");
            for (int k = 0; k < 2; ++k) w.zoomIn();
            handleOk("zoomed in");
            w.resize(900, 500);
            handleOk("after the window got smaller");
            w.resize(1500, 600);
            handleOk("after the window got bigger");
            // the end dragged and let go, then another project opened at the same zoom
            const Box b = boxOf(hbar);
            uitest::mouseDrag(hbar, QPoint((int)b.b - 2, hbar->height() / 2), QPoint((int)b.b - 2 + 30, hbar->height() / 2));
            handleOk("after an end of the handle was dragged");
            app.open(veg2.toStdString(), &err);
            handleOk("opening another project right after that");
            app.newProject();
            handleOk("after File > New");
            app.open(veg.toStdString(), &err);
            handleOk("opened again");
            app.undo();
            handleOk("after undo");
            app.edit("Long", [](Project& pr) { if (!pr.tracks.empty()) { Event e; e.start = 3000; e.length = 10; pr.tracks[0].events.push_back(e); } });
            handleOk("after an event far out was added");
            app.undo();
            handleOk("after undoing it");
            app.redo();
            handleOk("after redoing it");
            app.undo();
        }
    }

    // ---------------------------------------------------------------- the handle lights up under the mouse (both looks)
    for (UiTheme theme : {UiTheme::Reno, UiTheme::Vegas}) {
        const char* tn = theme == UiTheme::Reno ? "Reno" : "VEGAS";
        applyTheme(theme);
        uitest::settle(200);
        w.zoomToFit();
        for (int k = 0; k < 3; ++k) w.zoomIn();
        uitest::settle(50);
        hbar->setValue(hbar->maximum() / 2);
        CachedScrollBar* cb = dynamic_cast<CachedScrollBar*>(hbar);
        check(cb != nullptr, std::string("(the horizontal bar is a CachedScrollBar) ") + tn);
        if (!cb) continue;
        const Box b = boxOf(hbar);
        check(b.b - b.a > 60 && b.b - b.a < b.glen - 40, std::string("(the handle is a part of the groove) ") + tn, strfmt("%.0f..%.0f of %.0f", b.a, b.b, b.glen));
        const int y = hbar->height() / 2;
        const int xBody = (int)((b.a + b.b) / 2), xGrip0 = (int)b.a + 1, xGrip1 = (int)b.b - 2, xGroove = (int)(b.g0 + 2);
        auto leave = [&] { QEvent le(QEvent::Leave); QApplication::sendEvent(hbar, &le); QCoreApplication::processEvents(); };
        auto image = [&] { return hbar->grab().toImage().convertToFormat(QImage::Format_ARGB32); };
        auto lum = [](const QImage& im, int x, int yy) { const QColor c = im.pixelColor(x, yy); return c.red() * 0.3 + c.green() * 0.59 + c.blue() * 0.11; };
        auto sameImage = [](const QImage& a, const QImage& c) { return a == c; };
        leave();
        const QImage normal = image();
        const bool darkFace = lum(normal, xBody, y) < 140;
        const double sgn = darkFace ? 1.0 : -1.0;  // a dark handle lights up lighter, a light one darker
        const std::string t = std::string(" (") + tn + ")";
        check(cb->lit() == 0 && cb->hoverPart() == 0, "the handle is not lit with the mouse away" + t);

        // over the body
        uitest::mouseMove(hbar, QPoint(xBody, y), Qt::NoButton);
        const QImage overBody = image();
        check(cb->lit() == 1 && cb->hoverPart() == 1, "the mouse over the handle lights it" + t, strfmt("lit %d part %d", cb->lit(), cb->hoverPart()));
        check(sgn * (lum(overBody, xBody, y) - lum(normal, xBody, y)) >= 8, "the handle's face is clearly lighter under the mouse" + t,
              strfmt("%.1f -> %.1f", lum(normal, xBody, y), lum(overBody, xBody, y)));
        check(sgn * (lum(overBody, xGrip0 + 5, y) - lum(normal, xGrip0 + 5, y)) > 3 && sgn * (lum(overBody, xGrip1 - 5, y) - lum(normal, xGrip1 - 5, y)) > 3,
              "and so are the grip ends" + t);
        uitest::shot(hbar, out + "/sz_lit_hover_" + tn + ".png");
        check(sameImage(overBody, image()), "(a second paint of the lit handle is the same image)" + t);

        // over a grip: that end more
        uitest::mouseMove(hbar, QPoint(xGrip0, y), Qt::NoButton);
        const QImage overGrip = image();
        check(cb->hoverPart() == 2, "the mouse over the start grip is noticed" + t, strfmt("part %d", cb->hoverPart()));
        check(sgn * (lum(overGrip, xGrip0 + 1, y) - lum(overBody, xGrip0 + 1, y)) >= 3, "the grip under the mouse lights up more than the rest" + t,
              strfmt("%.1f vs %.1f", lum(overGrip, xGrip0 + 1, y), lum(overBody, xGrip0 + 1, y)));
        check(std::fabs(lum(overGrip, xGrip1 - 1, y) - lum(overBody, xGrip1 - 1, y)) < 2 && sgn * (lum(overGrip, xBody, y) - lum(normal, xBody, y)) >= 8,
              "the other end only as much as the handle" + t);
        uitest::shot(hbar, out + "/sz_lit_grip_" + tn + ".png");
        uitest::mouseMove(hbar, QPoint(xGrip1, y), Qt::NoButton);
        check(cb->hoverPart() == 3, "the far grip is noticed too" + t, strfmt("part %d", cb->hoverPart()));

        // pressed: an end held (dragged out and back, so that the zoom stays)
        uitest::mousePress(hbar, QPoint(xGrip1, y));
        uitest::mouseMove(hbar, QPoint(xGrip1 - 6, y), Qt::LeftButton);
        const QImage held = image();
        check(cb->lit() == 2, "a grip held makes it lit most" + t, strfmt("lit %d", cb->lit()));
        check(sgn * (lum(held, xBody, y) - lum(overBody, xBody, y)) >= 6, "the handle is stronger while an end is dragged" + t,
              strfmt("%.1f vs %.1f", lum(held, xBody, y), lum(overBody, xBody, y)));
        uitest::shot(hbar, out + "/sz_lit_held_" + tn + ".png");
        uitest::mouseMove(hbar, QPoint(xGrip1, y), Qt::LeftButton);
        uitest::mouseRelease(hbar, QPoint(xGrip1, y));
        uitest::settle(50);
        check(cb->lit() == 1, "letting go: lit as under the mouse again" + t, strfmt("lit %d", cb->lit()));

        // the handle dragged by its body
        const Box b2 = boxOf(hbar);
        const int xb2 = (int)((b2.a + b2.b) / 2);
        uitest::mouseMove(hbar, QPoint(xb2, y), Qt::NoButton);
        const QImage over2 = image();
        uitest::mousePress(hbar, QPoint(xb2, y));
        uitest::mouseMove(hbar, QPoint(xb2 + 10, y), Qt::LeftButton);
        const QImage heldBody = image();
        check(cb->lit() == 2 && sgn * (lum(heldBody, xb2 + 10, y) - lum(over2, xb2, y)) >= 6, "dragging the handle by its body lights it most" + t,
              strfmt("lit %d, %.1f vs %.1f", cb->lit(), lum(heldBody, xb2 + 10, y), lum(over2, xb2, y)));
        uitest::mouseRelease(hbar, QPoint(xb2 + 10, y));

        // the mouse on the groove beside the handle, then away
        const Box b3 = boxOf(hbar);
        uitest::mouseMove(hbar, QPoint((int)b3.g0 + 2, y), Qt::NoButton);
        check(cb->lit() == 0 && cb->hoverPart() == 0, "the mouse on the groove beside the handle does not light it" + t);
        uitest::mouseMove(hbar, QPoint((int)((b3.a + b3.b) / 2), y), Qt::NoButton);
        check(cb->lit() == 1, "(back on the handle)" + t);
        leave();
        check(cb->lit() == 0 && cb->hoverPart() == 0, "the handle is normal again when the mouse leaves" + t);
        (void)xGroove;
    }
    applyTheme(UiTheme::Reno);
    return uitest::summary();
}
