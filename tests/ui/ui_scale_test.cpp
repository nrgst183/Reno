// The whole window with a huge project (tens of thousands of events; gen_big makes them): what a user waits for when painting,
// scrolling, hovering, selecting, dragging, editing and undoing. Times are the UI thread's own processor time (this machine is
// shared; wall time says nothing); "call" is the call itself and "drain" the event loop work after it (every window that listens,
// the paints).
//
//   ui_scale_test <output folder> project.veg|project.reno... [--only a,b,c]
// parts: open paint scroll hover select drag ops undo docks save play
// Prints "SCALE <project> <measure> <value> <unit>". QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/vegas/vegas.h"
#include "uitestlib.h"
#include "../perf/sampler.h"
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QScrollBar>
#include <QSettings>
#include <QTabBar>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <random>

using namespace reno;
using namespace reno::app;

static std::string gOnly;
static bool want(const char* part) { return gOnly.empty() || ("," + gOnly + ",").find(std::string(",") + part + ",") != std::string::npos; }

// RENO_PROF=part,part: those parts of the session are run under the sampling profiler (sampler.h)
static bool profWant(const char* part)
{
    const char* e = std::getenv("RENO_PROF");
    return e && ("," + std::string(e) + ",").find(std::string(",") + part + ",") != std::string::npos;
}
#define PROFSCOPE(name) prof::Scope profScope_(name, profWant(name))

static double wallMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double nowMs()
{
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}
static double rssMb()
{
    QFile st("/proc/self/status");
    if (st.open(QIODevice::ReadOnly))
        for (const QByteArray& line : st.readAll().split('\n'))
            if (line.startsWith("VmRSS:")) return line.mid(6).trimmed().split(' ')[0].toDouble() / 1024.0;
    return 0;
}

static std::string gProject;
static void bench(const std::string& what, double v, const char* unit)
{
    std::printf("SCALE %-18s %-40s %12.3f %s\n", gProject.substr(0, 18).c_str(), what.c_str(), v, unit);
    std::fflush(stdout);
}

static void drain()
{
    for (int k = 0; k < 4; ++k) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
}

struct Cost { double call = 0, drain = 0; };
static Cost timed(const std::function<void()>& fn)
{
    Cost c;
    const double t0 = nowMs();
    fn();
    const double t1 = nowMs();
    drain();
    c.call = t1 - t0;
    c.drain = nowMs() - t1;
    return c;
}
// n runs of fn(k): the mean cost of the call and of what follows it; printed as "<what> call/drain"
static Cost rep(const std::string& what, int n, const std::function<void(int)>& fn, bool print = true)
{
    Cost sum;
    for (int k = 0; k < n; ++k) {
        const Cost c = timed([&] { fn(k); });
        sum.call += c.call;
        sum.drain += c.drain;
    }
    sum.call /= n;
    sum.drain /= n;
    if (print) {
        bench(what + " call", sum.call, "ms");
        bench(what + " drain", sum.drain, "ms");
    }
    return sum;
}

static QDockWidget* dockOf(MainWindow& w, const char* name) { return w.findChild<QDockWidget*>(name); }
static void front(MainWindow& w, const char* name)
{
    QDockWidget* d = dockOf(w, name);
    if (!d) return;
    if (!d->isVisible()) d->show();
    d->raise();
    for (QTabBar* bar : w.findChildren<QTabBar*>())
        for (int k = 0; k < bar->count(); ++k)
            if (bar->tabText(k) == d->windowTitle()) bar->setCurrentIndex(k);
    drain();
}

static size_t eventCount(const Project& p)
{
    size_t n = 0;
    for (const Track& t : p.tracks) n += t.events.size();
    return n;
}

// A point in the lanes where an event is (found by clicking until something is selected).
static QPoint findEventPoint(TimelineWidget* tl, AppState& app, int skip = 0)
{
    for (int y = 70; y < tl->height() - 30; y += 9)
        for (int x = tl->headerWidth() + 20 + skip; x < tl->width() - 40; x += 23) {
            uitest::mouseClick(tl, QPoint(x, y));
            if (!app.selection().empty()) return QPoint(x, y);
        }
    return QPoint(-1, -1);
}

static void zoomTo(TimelineWidget* tl, AppState& app, double span, double center)
{
    tl->zoomToFit();
    drain();
    for (int k = 0; k < 40 && tl->viewEndTime() - tl->viewStartTime() > span; ++k) tl->zoomIn();
    app.setCursor(center);
    tl->centerOnCursor();
    drain();
}

static void session(MainWindow& w, const QString& path, const QString& saveDir)
{
    AppState& app = AppState::get();
    TimelineWidget* tl = w.timeline();
    std::mt19937 rng(11);

    // ---- open
    {
        const double m0 = rssMb();
        const double t0 = nowMs();
        w.openProject(path);
        const double t1 = nowMs();
        drain();
        const double t2 = nowMs();
        w.grab();
        const double t3 = nowMs();
        bench("open call", t1 - t0, "ms");
        bench("open drain", t2 - t1, "ms");
        bench("open first grab", t3 - t2, "ms");
        bench("open total", t3 - t0, "ms");
        bench("open memory", rssMb() - m0, "MB");
    }
    for (int k = 0; k < 20; ++k) uitest::settle(50);
    const Project& proj = app.project();
    bench("events", (double)eventCount(proj), "");
    bench("tracks", (double)proj.tracks.size(), "");
    bench("memory after open", rssMb(), "MB");
    const double len = std::max(1.0, proj.length());
    const double mid = len / 2;

    // ---- painting
    if (want("paint")) {
        PROFSCOPE("paint");
        tl->zoomToFit();
        drain();
        rep("paint fit (repaint)", 3, [&](int) { tl->repaint(); });
        rep("window grab fit", 2, [&](int) { w.grab(); });
        zoomTo(tl, app, 20.0, mid);
        bench("view span (zoomed in)", tl->viewEndTime() - tl->viewStartTime(), "s");
        rep("paint zoomed 20s (repaint)", 3, [&](int) { tl->repaint(); });
        zoomTo(tl, app, 2.0, mid);
        rep("paint zoomed 2s (repaint)", 3, [&](int) { tl->repaint(); });
        tl->zoomToFit();
        drain();
    }

    // ---- scrolling and zooming
    if (want("scroll")) {
        PROFSCOPE("scroll");
        zoomTo(tl, app, 20.0, mid);
        QScrollBar* h = tl->findChild<QScrollBar*>("timelineHScroll");
        rep("hscroll step (zoomed 20s)", 20, [&](int k) {
            if (h) h->setValue(h->minimum() + (h->maximum() - h->minimum()) * ((k * 7) % 20) / 19);
        });
        rep("zoom in/out wheel (zoomed)", 12, [&](int k) { uitest::wheel(tl, QPoint(tl->headerWidth() + 300, tl->height() / 2), k % 2 ? -120 : 120); });
        tl->zoomToFit();
        drain();
        rep("zoom fit", 4, [&](int) { tl->zoomToFit(); });
        QScrollBar* v = tl->findChild<QScrollBar*>("timelineVScroll");
        if (v) rep("vscroll step", 12, [&](int k) { v->setValue(v->minimum() + (v->maximum() - v->minimum()) * ((k * 5) % 12) / 11); });
        if (v) v->setValue(v->minimum());
        drain();
    }

    // ---- hover (hit testing, no buttons)
    if (want("hover")) {
        PROFSCOPE("hover");
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 0) { tl->zoomToFit(); drain(); }
            else zoomTo(tl, app, 20.0, mid);
            const QPoint a(tl->headerWidth() + 10, 80);
            rep(pass == 0 ? "hover move (fit)" : "hover move (zoomed 20s)", 60, [&](int k) {
                uitest::mouseMove(tl, a + QPoint((k * 29) % std::max(100, tl->width() - tl->headerWidth() - 40), (k * 13) % std::max(50, tl->height() - 140)), Qt::NoButton);
            });
        }
    }

    // ---- selecting
    if (want("select")) {
        PROFSCOPE("select");
        tl->zoomToFit();
        drain();
        app.setSelection(std::set<uint64_t>());
        drain();
        zoomTo(tl, app, 30.0, mid);
        rep("click select (zoomed 30s)", 20, [&](int k) { uitest::mouseClick(tl, QPoint(tl->headerWidth() + 40 + (k * 61) % std::max(100, tl->width() - tl->headerWidth() - 80), 90 + (k * 17) % 300)); });
        // select all (Edit > Select All)
        {
            std::set<uint64_t> all;
            const double t0 = nowMs();
            for (const auto& t : app.project().tracks) for (const auto& e : t.events) all.insert(e.id);
            const double t1 = nowMs();
            app.setSelection(all);
            const double t2 = nowMs();
            drain();
            bench("select all: collect ids", t1 - t0, "ms");
            bench("select all: setSelection call", t2 - t1, "ms");
            bench("select all: drain (paints, listeners)", nowMs() - t2, "ms");
        }
        rep("cursor move with all selected", 10, [&](int k) { app.setCursor(mid + k * 0.1); });
        rep("repaint with all selected (zoomed)", 2, [&](int) { tl->repaint(); });
        app.setSelection(std::set<uint64_t>());
        drain();
        // rubber band with the Selection tool
        {
            tl->options().tool = EditTool::Selection;
            tl->optionsChanged();
            zoomTo(tl, app, 30.0, mid);
            const QPoint from(tl->headerWidth() + 30, 80);
            const QPoint to(tl->width() - 60, std::max(200, tl->height() - 80));
            const Cost c = timed([&] { uitest::mouseDrag(tl, from, to, 6); });
            bench("rubber band drag (6 moves) call", c.call, "ms");
            bench("rubber band drag drain", c.drain, "ms");
            bench("rubber band selected", (double)app.selection().size(), "events");
            tl->options().tool = EditTool::Normal;
            tl->optionsChanged();
            app.setSelection(std::set<uint64_t>());
            drain();
        }
    }

    // ---- dragging
    if (want("drag")) {
        PROFSCOPE("drag");
        zoomTo(tl, app, 30.0, mid);
        app.setSelection(std::set<uint64_t>());
        QPoint grab = findEventPoint(tl, app);
        if (grab.x() >= 0) {
            const size_t undo0 = app.undoCount();
            uitest::mousePress(tl, grab);
            drain();
            rep("drag 1 event: mouse move", 30, [&](int k) { uitest::mouseMove(tl, grab + QPoint(1 + k * 2, 0), Qt::LeftButton); });
            const double t0 = nowMs();
            uitest::mouseRelease(tl, grab + QPoint(61, 0));
            const double t1 = nowMs();
            drain();
            bench("drag 1 event: release call", t1 - t0, "ms");
            bench("drag 1 event: release drain", nowMs() - t1, "ms");
            if (app.undoCount() > undo0) app.undo();
            drain();
        }
        // all events selected, one dragged
        {
            std::set<uint64_t> all;
            for (const auto& t : app.project().tracks) for (const auto& e : t.events) all.insert(e.id);
            app.setSelection(all);
            drain();
            grab = findEventPoint(tl, app);
            // the click selected one: select all again, press on the same event
            app.setSelection(all);
            drain();
            if (grab.x() >= 0) {
                const size_t undo0 = app.undoCount();
                uitest::mousePress(tl, grab);
                drain();
                rep("drag ALL events: mouse move", 6, [&](int k) { uitest::mouseMove(tl, grab + QPoint(1 + k * 3, 0), Qt::LeftButton); });
                const double t0 = nowMs();
                uitest::mouseRelease(tl, grab + QPoint(19, 0));
                const double t1 = nowMs();
                drain();
                bench("drag ALL events: release call", t1 - t0, "ms");
                bench("drag ALL events: release drain", nowMs() - t1, "ms");
                if (app.undoCount() > undo0) {
                    const Cost u = timed([&] { app.undo(); });
                    bench("undo of drag ALL call", u.call, "ms");
                    bench("undo of drag ALL drain", u.drain, "ms");
                }
            }
            app.setSelection(std::set<uint64_t>());
            drain();
        }
    }

    // ---- editing commands on many events
    if (want("ops")) {
        PROFSCOPE("ops");
        // a block of events: every 4th event of the project (a quarter of them), then all
        std::vector<uint64_t> ids;
        for (const auto& t : app.project().tracks) for (size_t k = 0; k < t.events.size(); k += 4) ids.push_back(t.events[k].id);
        std::set<uint64_t> quarter(ids.begin(), ids.end());
        bench("ops selection size", (double)quarter.size(), "events");
        auto each = [&](const char* name, const std::function<void()>& fn, bool undoAfter) {
            app.setSelection(quarter);
            app.setCursor(mid);
            drain();
            const size_t u0 = app.undoCount();
            const Cost c = timed(fn);
            bench(std::string(name) + " call", c.call, "ms");
            bench(std::string(name) + " drain", c.drain, "ms");
            if (undoAfter) {
                while (app.undoCount() > u0) app.undo();
                drain();
            }
        };
        each("copy", [&] { app.copySelection(); }, false);
        each("paste", [&] { app.pasteAtCursor(); }, true);
        each("split (cursor, quarter selected)", [&] { app.splitAtCursor(); }, true);
        // split every selected event (cursor placed in the middle of many events: split after a time at the busiest place)
        each("delete (timeline, auto ripple)", [&] { tl->deleteSelection(); }, true);
        each("cut", [&] { tl->cutSelection(); }, true);
        each("gain change of selection", [&] {
            app.edit("Gain", [&](Project& p) { for (uint64_t id : quarter) if (Event* e = p.findEvent(id)) e->gain = 0.5; });
        }, true);
        each("group selection", [&] { app.groupSelection(true); }, true);
        each("lock selection", [&] { app.setLocked(quarter, true); }, true);
        app.setSelection(std::set<uint64_t>());
        drain();
    }

    // ---- single edits, undo, memory
    if (want("undo")) {
        PROFSCOPE("undo");
        uint64_t evId = 0;
        double best = 1e18;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events) if (std::fabs(e.start - mid) < best) { best = std::fabs(e.start - mid); evId = e.id; }
        auto editGain = [&](int k) {
            app.edit("Gain", [&](Project& p) { if (Event* e = p.findEvent(evId)) e->gain = 0.5 + 0.01 * (k % 40); });
        };
        zoomTo(tl, app, 30.0, mid);
        const double m0 = rssMb();
        const size_t u0 = app.undoCount();
        // RENO_EDITS: how many single edits in all (a baseline with a full copy per undo step runs out of memory at 50k)
        const int nEdits = std::getenv("RENO_EDITS") ? std::max(5, std::atoi(std::getenv("RENO_EDITS"))) : 100;
        const int nFirst = std::max(2, nEdits / 5), nMore = nEdits - nFirst;
        rep("single event edit", nFirst, editGain);
        const double m1 = rssMb();
        rep("single event edit (more)", nMore, editGain, false);
        const double m2 = rssMb();
        bench("memory before edits", m0, "MB");
        bench("memory per undo step (first 20)", (m1 - m0) / nFirst, "MB");
        bench("memory per undo step (next 80)", (m2 - m1) / nMore, "MB");
        bench("undo steps", (double)(app.undoCount() - u0), "");
        rep("undo", std::min(10, nFirst), [&](int) { app.undo(); });
        rep("redo", std::min(10, nFirst), [&](int) { app.redo(); });
        // a track level change
        const uint64_t tid = app.project().tracks.back().id;
        rep("track level edit", 20, [&](int k) { app.edit("Volume", [&](Project& p) { if (Track* t = p.findTrack(tid)) t->level = 0.5 + 0.02 * (k % 20); }); });
        // an edit on every event of the project (one undo step: what a script or Select All + a change makes)
        {
            const double a = rssMb();
            const Cost c = timed([&] { app.edit("All", [&](Project& p) { for (auto& t : p.tracks) for (auto& e : t.events) e.gain = 0.9; }); });
            bench("edit of every event call", c.call, "ms");
            bench("edit of every event drain", c.drain, "ms");
            bench("edit of every event: memory", rssMb() - a, "MB");
        }
        bench("memory after edits", rssMb(), "MB");
    }

    // ---- the docked windows that listen
    if (want("docks")) {
        PROFSCOPE("docks");
        uint64_t evId = 0;
        double best = 1e18;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events) if (std::fabs(e.start - mid) < best) { best = std::fabs(e.start - mid); evId = e.id; }
        auto editGain = [&](int k) {
            app.edit("Gain", [&](Project& p) { if (Event* e = p.findEvent(evId)) e->gain = 0.5 + 0.01 * (k % 40); });
        };
        for (const char* d : {"dockMedia", "dockVideoFx", "dockMixer", "dockDetails", "dockExplorer", "dockTransitions", "dockTrimmer"}) {
            if (!dockOf(w, d)) continue;
            front(w, d);
            rep(std::string("edit [") + d + "]", 6, editGain);
        }
        front(w, "dockMedia");
    }

    // ---- saving
    if (want("save")) {
        PROFSCOPE("save");
        const std::string vegPath = (saveDir + "/saved.veg").toStdString();
        const std::string renoPath = (saveDir + "/saved.reno").toStdString();
        std::string err;
        const Cost a = timed([&] { app.save(vegPath, &err); });
        bench("save .veg call", a.call, "ms");
        bench("save .veg drain", a.drain, "ms");
        const Cost b = timed([&] { app.save(renoPath, &err); });
        bench("save .reno call", b.call, "ms");
        bench("save .reno drain", b.drain, "ms");
        app.edit("Gain", [&](Project& p) { if (!p.tracks.empty() && !p.tracks[0].events.empty()) p.tracks[0].events[0].gain = 0.77; });
        drain();
        const Cost c = timed([&] { app.autoSaveNow(&err); });
        bench("auto-save call", c.call, "ms");
        bench("auto-save drain", c.drain, "ms");
    }

    // ---- playing: the interface thread's busy time
    if (want("play")) {
        PROFSCOPE("play");
        app.setCursor(0);
        drain();
        const double c0 = nowMs();
        const double t0 = wallMs();
        w.playback()->play();
        while (wallMs() - t0 < 2500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(2);  // not a busy loop, which would be counted as the UI being busy
        }
        w.playback()->pause();
        const double wall = wallMs() - t0;
        bench("play: ui busy", 100.0 * (nowMs() - c0) / wall, "% of wall time");
        uitest::settle(200);
    }
    bench("memory at end", rssMb(), "MB");
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoUiScaleTest");
    QApplication::setApplicationName("ui_scale_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: ui_scale_test <output folder> project... [--only parts]\n");
        return 2;
    }
    const QString out = QDir(args[1]).absolutePath();
    QDir().mkpath(out);
    QStringList projects;
    for (int k = 2; k < args.size(); ++k) {
        if (args[k] == "--only" && k + 1 < args.size()) gOnly = args[++k].toStdString();
        else projects << args[k];
    }
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    // the stand-in media opener of uitest::setup knows the made-up media only: real files and nested projects go to the real one
    AppState::get().mediaOpener = [](const std::string& path, std::string* error) { return openMediaOrProject(path, error); };
    setProjectFileLoader([](const std::string& path, Project& o, std::string* error) { return loadProjectFile(path, o, error); });
    // a click on an event's menu button opens a menu that waits for an answer: closed at once so the run goes on
    QTimer closePopups;
    QObject::connect(&closePopups, &QTimer::timeout, [] { if (QWidget* pw = QApplication::activePopupWidget()) pw->close(); });
    closePopups.start(20);
    auto w = std::make_unique<MainWindow>();
    w->resize(1600, 1000);
    w->show();
    drain();
    uitest::settle(300);
    for (const QString& p : projects) {
        AppState::get().newProject(true);
        uitest::settle(100);
        gProject = QFileInfo(p).fileName().toStdString();
        session(*w, p, out);
        AppState::get().newProject(true);
        uitest::settle(200);
    }
    return 0;
}
