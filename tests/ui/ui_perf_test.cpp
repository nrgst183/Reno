// How fast is the whole UI (not only the timeline) with big projects? timeline_perf_test times the timeline's own painting and
// load_lag_test looks for stalls; this one times the rest of what a user waits for, in the whole main window with a made-up plug-in
// catalog of several hundred plug-ins:
//   start     building the main window, showing it, its first paint
//   open      File > Open of a project to the first full paint of the window
//   edit      one edit (a change to an event's gain), the call itself and the event loop's work that follows it (every window that
//             listens), with the Project Media, Video FX, Mixing Console tabs in front in turn
//   select    changing the selection; the cursor moving; undo and redo
//   drag      an event dragged on the timeline, a mouse move at a time
//   fxwindow  the Video FX window opening on an effect, and switching between effects
//   chooser   the Plug-In Chooser opening
//   media     Project Media with hundreds of media; adding media
//   memory    the program's memory after many edits (undo steps)
//   play      the UI thread's busy time while playing
// A "BENCH <project> <measure> <value> <unit>" line each. Pictures of fixed states (--shots <folder>) are for comparing two builds
// pixel by pixel.
//
//   ui_perf_test <output folder> <perf-media folder> [--samples <folder>] [--shots <folder>] [--tracks N] [project.veg names...]
//   (the sample VEGAS projects default to the folder named by $RENO_SAMPLES; without them those parts SKIP)
// QT_QPA_PLATFORM=offscreen. Needs the sample projects and the stand-in clips; skips itself without them.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/vegas/vegas.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QScrollBar>
#include <QTabBar>
#include <QSettings>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <functional>
#include <memory>
#include <set>

using namespace reno;
using namespace reno::app;
using uitest::check;

// --only a,b: just those parts of the session (edit, select, drag, scroll, fxwindow, chooser, mixer, media, memory, play)
static bool want(const char* part)
{
    static const QByteArray only = qgetenv("RENO_PERF_ONLY");
    return only.isEmpty() || (',' + only + ',').contains(QByteArray(",") + part + ",");
}

// The time measures are the UI thread's own processor time (not the clock): this machine is shared with builds and other tests, and
// what they take from the processor says nothing about the UI.
static double nowWallMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static double nowMs()
{
#ifdef __linux__
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
#else
    return nowWallMs();
#endif
}

// The UI thread's busy time: every outermost event the application handles is timed.
class TimedApp : public QApplication {
public:
    TimedApp(int& argc, char** argv) : QApplication(argc, argv) {}
    bool notify(QObject* o, QEvent* e) override
    {
        if (depth_ > 0) {
            if (e->type() != QEvent::Paint || !o) {
                ++depth_;
                const bool r = QApplication::notify(o, e);
                --depth_;
                return r;
            }
            // a widget's paint, inside the window's update request: counted by the widget's class and name
            ++depth_;
            const double p0 = nowMs();
            const bool r = QApplication::notify(o, e);
            const double pm = nowMs() - p0;
            --depth_;
            Tally& t = tally[std::string("paint ") + o->metaObject()->className() + " " + o->objectName().toStdString()];
            t.ms += pm;
            ++t.n;
            return r;
        }
        ++depth_;
        // taken first: the object or event may be gone afterwards
        const int type = (int)e->type();
        const std::string who = o ? std::string(o->metaObject()->className()) + " " + o->objectName().toStdString() : std::string("?");
        const double t0 = nowMs();
        const bool r = QApplication::notify(o, e);
        const double ms = nowMs() - t0;
        --depth_;
        busy += ms;
        if (ms > longest) longest = ms;
        if (type != QEvent::UpdateRequest && type != QEvent::Paint) {
            Tally& t = tally["event " + who + " type " + std::to_string(type)];
            t.ms += ms;
            ++t.n;
        }
        return r;
    }
    double busy = 0, longest = 0;
    struct Tally { double ms = 0; int n = 0; };
    std::map<std::string, Tally> tally;
    // RENO_PERF_BREAKDOWN=1: what the time went to since the last call, widget class by widget class
    void report(const char* title)
    {
        std::vector<std::pair<double, std::string>> v;
        for (auto& kv : tally) v.push_back({kv.second.ms, kv.first + strfmt("  x%d", kv.second.n)});
        std::sort(v.rbegin(), v.rend());
        std::printf("BREAKDOWN %s\n", title);
        for (size_t k = 0; k < v.size() && k < 14; ++k) std::printf("   %8.1f ms  %s\n", v[k].first, v[k].second.c_str());
        tally.clear();
    }

private:
    int depth_ = 0;
};
static TimedApp* gApp = nullptr;

static std::string gProject;
static void bench(const std::string& what, double v, const char* unit)
{
    std::printf("BENCH %-26s %-34s %10.2f %s\n", gProject.substr(0, 26).c_str(), what.c_str(), v, unit);
    std::fflush(stdout);
}

// Lets everything queued run: the paints a change asked for, the work listeners posted.
static void drain()
{
    for (int k = 0; k < 4; ++k) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
}

// The time of 'fn' (the call itself) and of the event loop work after it, in ms.
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

// n runs of 'fn(k)': the mean cost of the call and of what follows it.
static Cost mean(int n, const std::function<void(int)>& fn)
{
    gApp->tally.clear();
    Cost sum;
    for (int k = 0; k < n; ++k) {
        const Cost c = timed([&] { fn(k); });
        sum.call += c.call;
        sum.drain += c.drain;
    }
    sum.call /= n;
    sum.drain /= n;
    return sum;
}

static void benchCost(const std::string& what, const Cost& c, const char* unit = "ms")
{
    bench(what + " call", c.call, unit);
    bench(what + " drain", c.drain, unit);
    if (qEnvironmentVariableIsSet("RENO_PERF_BREAKDOWN")) gApp->report(what.c_str());
}

static double memoryNowMb()
{
    QFile st("/proc/self/status");
    if (st.open(QIODevice::ReadOnly))
        for (const QByteArray& line : st.readAll().split('\n'))
            if (line.startsWith("VmRSS:")) return line.mid(6).trimmed().split(' ')[0].toDouble() / 1024.0;
    return 0;
}

// ---------------------------------------------------------------- the stand-in copy of a sample folder (as load_lag_test)
static std::string baseName(const std::string& p)
{
    const size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? p : p.substr(k + 1);
}

static std::string pickStandin(const MediaItem& m)
{
    const std::string low = toLower(m.path);
    auto ends = [&](const char* e) { return endsWithNoCase(low, e); };
    if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".bmp") || ends(".gif") || ends(".tif") || m.info.isStill) return "still.png";
    if (ends(".wav") || ends(".mp3") || ends(".ogg") || ends(".flac") || ends(".m4a") || ends(".aac") || (m.info.hasAudio && !m.info.hasVideo))
        return "tone.wav";
    if (m.info.fps > 45) return "clip1080.mp4";
    if (m.info.height > 0 && m.info.height <= 480) return "clip480.mp4";
    return "clip720.mp4";
}

static bool makeStandinFolder(const QString& project, const QString& samplesRoot, const QString& media, const QString& dest)
{
    QDir().mkpath(dest);
    const QDir src = QFileInfo(project).absoluteDir();
    std::vector<QString> todo;
    for (const QFileInfo& f : src.entryInfoList(QStringList() << "*.veg", QDir::Files)) {
        const QString to = dest + "/" + f.fileName();
        if (!QFileInfo::exists(to)) QFile::copy(f.absoluteFilePath(), to);
        todo.push_back(f.absoluteFilePath());
    }
    std::set<QString> done;
    while (!todo.empty()) {
        const QString orig = todo.back();
        todo.pop_back();
        if (!done.insert(orig).second) continue;
        Project p;
        if (!importVegasProject(orig.toStdString(), p).ok) continue;
        for (const MediaItem& m : p.media) {
            if (m.generated || m.path.empty()) continue;
            const std::string name = baseName(m.path);
            if (name.empty()) continue;
            const QString to = dest + "/" + QString::fromStdString(name);
            if (endsWithNoCase(name, ".veg")) {
                QString from = src.filePath(QString::fromStdString(name));
                if (!QFileInfo::exists(from)) {
                    from.clear();
                    QDirIterator it(samplesRoot, QStringList() << QString::fromStdString(name), QDir::Files, QDirIterator::Subdirectories);
                    if (it.hasNext()) from = it.next();
                }
                if (from.isEmpty()) continue;
                if (!QFileInfo::exists(to)) QFile::copy(from, to);
                todo.push_back(from);
                continue;
            }
            if (QFileInfo::exists(to) || QFileInfo(to).isSymLink()) continue;
            QFile::link(media + "/" + QString::fromStdString(pickStandin(m)), to);
        }
    }
    return true;
}

// ---------------------------------------------------------------- a made-up plug-in catalog (hundreds of plug-ins)
static void writeFakeCatalog(int nVideo, int nAudio)
{
    Json files = Json::object();
    Json plugins = Json::array();
    static const char* groups[] = {"Color", "Blur", "Distort", "Stylize", "Keying", "Transform", "Generators", "Time"};
    for (int k = 0; k < nVideo; ++k) {
        Json p = Json::object();
        p["id"] = strfmt("com.fake.vendor%d:effect%d", k % 12, k);
        p["label"] = strfmt("Fake Effect %03d", k);
        p["group"] = std::string(groups[k % 8]);
        p["versionMajor"] = 1;
        p["versionMinor"] = 0;
        p["ok"] = true;
        Json ctx = Json::array();
        ctx.push(std::string(k % 11 == 0 ? "OfxImageEffectContextGenerator" : k % 13 == 0 ? "OfxImageEffectContextTransition" : "OfxImageEffectContextFilter"));
        p["contexts"] = ctx;
        plugins.push(p);
    }
    Json ofx = Json::object();
    ofx["stamp"] = std::string("fake");
    ofx["kind"] = std::string("ofx");
    Json res = Json::object();
    res["plugins"] = plugins;
    ofx["result"] = res;
    files["/fake/many.ofx"] = ofx;
    Json dx = Json::array();
    for (int k = 0; k < nAudio; ++k) {
        Json p = Json::object();
        p["format"] = std::string("DX");
        p["uid"] = strfmt("{FAKE%04d}", k);
        p["name"] = strfmt("Fake Audio %03d", k);
        p["vendor"] = strfmt("Vendor %d", k % 9);
        p["inputs"] = 2;
        p["outputs"] = 2;
        dx.push(p);
    }
    Json dxe = Json::object();
    dxe["stamp"] = std::string("dx");
    dxe["kind"] = std::string("dx");
    dxe["result"] = dx;
    files["<DirectX audio effects>"] = dxe;
    Json j = Json::object();
    j["files"] = files;
    j["scanVersion"] = catalogScanVersion();
    writeFile(pathJoin(userDataDir(), "plugins.json"), j.dump(1));
    Catalog::get().loadCache();
}

static QDockWidget* dockOf(MainWindow& w, const char* name) { return w.findChild<QDockWidget*>(name); }

// Brings a docked window to the front of its tabs.
static void front(MainWindow& w, const char* name)
{
    QDockWidget* d = dockOf(w, name);
    if (!d) return;
    if (!d->isVisible()) d->show();
    d->raise();
    // (docked windows share tabs: the tab is picked, as a click on it does)
    for (QTabBar* bar : w.findChildren<QTabBar*>())
        for (int k = 0; k < bar->count(); ++k)
            if (bar->tabText(k) == d->windowTitle()) bar->setCurrentIndex(k);
    drain();
}

static QPoint findEventPoint(TimelineWidget* tl, AppState& app)
{
    for (int y = 70; y < tl->height() - 30; y += 14)
        for (int x = tl->headerWidth() + 20; x < tl->width() - 40; x += 37) {
            uitest::mouseClick(tl, QPoint(x, y));
            if (!app.selection().empty()) return QPoint(x, y);
        }
    return QPoint(-1, -1);
}

// The event nearest the middle of the project.
static uint64_t middleEvent(const Project& p)
{
    uint64_t best = 0;
    double bestD = 1e18;
    const double mid = p.length() / 2;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events) {
            const double d = std::fabs(e.start - mid);
            if (d < bestD) { bestD = d; best = e.id; }
        }
    return best;
}

static void session(MainWindow& w, const QString& path, const QString& shots, int extraTracks)
{
    AppState& app = AppState::get();
    TimelineWidget* tl = w.timeline();

    // ---- open
    {
        const double t0 = nowMs();
        w.openProject(path);
        const double t1 = nowMs();
        drain();
        const double t2 = nowMs();
        w.grab();
        const double t3 = nowMs();
        bench("open_call", t1 - t0, "ms");
        bench("open_drain", t2 - t1, "ms");
        bench("open_first_full_grab", t3 - t2, "ms");
        bench("open_to_painted", t3 - t0, "ms");
    }
    // let the pictures and waveforms come in
    for (int k = 0; k < 30; ++k) uitest::settle(50);
    {
        const Project& proj = app.project();
        size_t events = 0;
        for (const Track& t : proj.tracks) events += t.events.size();
        bench("events", (double)events, "");
        bench("tracks", (double)proj.tracks.size(), "");
        bench("media", (double)proj.media.size(), "");
    }
    tl->zoomToFit();
    drain();
    if (!shots.isEmpty()) {
        QDir().mkpath(shots);
        uitest::shot(&w, shots + "/window_fit.png");
    }

    // ---- full window grab, the timeline alone
    {
        if (QScrollBar* hb = tl->findChild<QScrollBar*>("timelineHScroll")) {
            const double t0 = nowMs();
            for (int k = 0; k < 20; ++k) hb->grab();
            bench("hscrollbar_grab", (nowMs() - t0) / 20, "ms");
        }
        double t0 = nowMs();
        for (int k = 0; k < 3; ++k) w.grab();
        bench("window_grab", (nowMs() - t0) / 3, "ms");
        t0 = nowMs();
        for (int k = 0; k < 3; ++k) tl->repaint();
        bench("timeline_repaint_fit", (nowMs() - t0) / 3, "ms");
    }

    // ---- edits: one event's gain changed, with each docked window in front in turn
    const uint64_t evId = middleEvent(app.project());
    auto editGain = [&](int k) {
        app.edit("Gain", [&](Project& p) {
            if (Event* e = p.findEvent(evId)) e->gain = 0.5 + 0.01 * (k % 40);
        });
    };
    for (const char* dockName : {"dockMedia", "dockVideoFx", "dockMixer", "dockDetails"}) {
        if (!want("edit")) break;
        front(w, dockName);
        benchCost(std::string("edit [") + dockName + "]", mean(20, editGain));
    }
    front(w, "dockMedia");

    // ---- selection, cursor, undo
    if (want("select"))
    {
        const Project& proj = app.project();
        std::vector<uint64_t> ids;
        for (const Track& t : proj.tracks)
            for (size_t k = 0; k < t.events.size() && ids.size() < 200; k += std::max<size_t>(1, t.events.size() / 20)) ids.push_back(t.events[k].id);
        if (!ids.empty()) benchCost("select", mean(30, [&](int k) { app.selectOnly(ids[(size_t)k % ids.size()]); }));
        const double len = std::max(1.0, proj.length());
        benchCost("cursor", mean(60, [&](int k) { app.setCursor(len * (k % 60) / 60.0); }));
        benchCost("undo", mean(10, [&](int) { app.undo(); }));
        benchCost("redo", mean(10, [&](int) { app.redo(); }));
    }

    // ---- dragging an event
    if (want("drag"))
    {
        tl->zoomToFit();
        drain();
        const QPoint grab = findEventPoint(tl, app);
        if (grab.x() >= 0) {
            uitest::mousePress(tl, grab);
            drain();
            benchCost("drag mouse move", mean(30, [&](int k) { uitest::mouseMove(tl, grab + QPoint(1 + k * 2, 0), Qt::LeftButton); }));
            const double t0 = nowMs();
            uitest::mouseRelease(tl, grab + QPoint(61, 0));
            drain();
            bench("drag_release", nowMs() - t0, "ms");
            app.undo();
            drain();
        }
    }

    // ---- scroll and zoom (wheel) with the whole window
    if (want("scroll"))
    {
        tl->zoomToFit();
        drain();
        for (int k = 0; k < 5; ++k) uitest::wheel(tl, QPoint(tl->headerWidth() + 300, tl->height() / 2), 120);
        drain();
        QScrollBar* h = tl->findChild<QScrollBar*>("timelineHScroll");
        benchCost("scroll", mean(30, [&](int k) {
            if (h) h->setValue(h->minimum() + (h->maximum() - h->minimum()) * (k % 15) / 14);
        }));
        benchCost("zoom wheel", mean(20, [&](int k) { uitest::wheel(tl, QPoint(tl->headerWidth() + 300, tl->height() / 2), k < 10 ? 120 : -120); }));
        tl->zoomToFit();
        drain();
    }

    // ---- the Video FX window: an event with an effect (put some on if the project has none)
    if (want("fxwindow"))
    {
        std::vector<std::pair<uint64_t, uint64_t>> fxs;      // (event, slot)
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                for (const FxSlot& s : e.fx) if (fxs.size() < 8) fxs.push_back({e.id, s.id});
        if (fxs.empty()) {
            const Track* vt = nullptr;
            for (const Track& t : app.project().tracks) if (t.type == TrackType::Video && !t.events.empty()) { vt = &t; break; }
            if (vt) {
                const uint64_t fxEvent = vt->events[vt->events.size() / 2].id;
                FxTarget tg;
                tg.kind = FxTarget::Event;
                tg.id = fxEvent;
                int added = 0;
                for (const VideoFxEntry& v : Catalog::get().video()) {
                    if (!v.filter || !v.builtin || added >= 4) continue;
                    app.addVideoFx(tg, v.id, v.label);
                    ++added;
                }
                if (const Event* e = app.project().findEvent(fxEvent))
                    for (const FxSlot& s : e->fx) fxs.push_back({fxEvent, s.id});
            }
        }
        if (!fxs.empty()) {
            FxTarget tg;
            tg.kind = FxTarget::Event;
            tg.id = fxs[0].first;
            benchCost("fxwindow open", timed([&] { w.openFx(tg, fxs[0].second); }));
            uitest::settle(100);
            benchCost("fxwindow switch", mean((int)fxs.size() * 3, [&](int k) {
                const auto& s = fxs[(size_t)k % fxs.size()];
                FxTarget t2;
                t2.kind = FxTarget::Event;
                t2.id = s.first;
                w.openFx(t2, s.second);
            }));
            benchCost("edit [fxwindow open]", mean(10, editGain));
            if (!shots.isEmpty()) uitest::shot(w.fxWindow(), shots + "/fxwindow.png");
            w.fxWindow()->hide();
            drain();
        }
    }

    // ---- the Plug-In Chooser
    if (want("chooser"))
    {
        double ctor = 0, show = 0;
        for (int round = 0; round < 3; ++round) {
            const double t0 = nowMs();
            PluginChooser ch(ChooserKind::VideoFx, "Plug-In Chooser - Video Event FX", FxChain());
            const double t1 = nowMs();
            ch.show();
            drain();
            const double t2 = nowMs();
            ctor += t1 - t0;
            show += t2 - t1;
            if (round == 0 && !shots.isEmpty()) uitest::shot(&ch, shots + "/chooser.png");
        }
        bench("chooser ctor", ctor / 3, "ms");
        bench("chooser show", show / 3, "ms");
        PluginChooser ca(ChooserKind::AudioFx, "Plug-In Chooser - Audio Track FX", FxChain());
        const double t0 = nowMs();
        ca.show();
        drain();
        bench("chooser audio show", nowMs() - t0, "ms");
    }

    // ---- the Mixing Console with many tracks
    if (want("mixer"))
    {
        const double t0 = nowMs();
        for (int k = 0; k < extraTracks; ++k) app.addTrack(TrackType::Audio, -1);
        drain();
        bench("add_audio_tracks", (nowMs() - t0) / std::max(1, extraTracks), "ms per track");
        const std::string n = std::to_string(app.project().tracks.size());
        benchCost("mixer show [" + n + " tracks]", timed([&] { front(w, "dockMixer"); }));
        benchCost("edit [mixer, " + n + " tracks]", mean(10, editGain));
        benchCost("window grab [mixer, " + n + " tracks]", timed([&] { w.grab(); }));
        if (!shots.isEmpty()) uitest::shot(&w, shots + "/mixer.png");
        // a track's volume changed (a fader drag): the one edit the console is about
        const uint64_t tid = app.project().tracks.back().id;
        benchCost("track fader edit [mixer]", mean(20, [&](int k) {
            app.edit("Volume", [&](Project& p) { if (Track* t = p.findTrack(tid)) t->level = 0.5 + 0.02 * (k % 20); });
        }));
        front(w, "dockMedia");
    }

    // ---- Project Media with hundreds of media
    if (want("media"))
    {
        const Cost add = timed([&] {
            app.edit("Media", [&](Project& p) {
                for (int k = 0; k < 400; ++k) {
                    MediaItem m;
                    m.id = p.newId();
                    m.path = strfmt("synth:%d", 1 + k % 5);
                    m.name = strfmt("media %03d.mp4", k);
                    m.info.hasVideo = true;
                    m.info.hasAudio = true;
                    m.info.width = 320;
                    m.info.height = 180;
                    m.info.fps = 30;
                    m.info.duration = 10;
                    m.info.sampleRate = 48000;
                    m.info.channels = 2;
                    p.media.push_back(m);
                }
            });
        });
        benchCost("media add 400", add);
        front(w, "dockMedia");
        benchCost("edit [media 400+]", mean(10, editGain));
        benchCost("window grab [media 400+]", timed([&] { w.grab(); }));
        if (!shots.isEmpty()) uitest::shot(&w, shots + "/media.png");
    }

    // ---- memory over a long editing session
    if (want("memory"))
    {
        const double m0 = memoryNowMb();
        mean(150, editGain);
        bench("memory_before_150_edits", m0, "MB");
        bench("memory_after_150_edits", memoryNowMb(), "MB");
        bench("undo_steps", (double)app.undoCount(), "");
    }

    // ---- playing
    if (want("play"))
    {
        app.setCursor(0);
        drain();
        const double b0 = gApp->busy;
        gApp->longest = 0;
        gApp->tally.clear();
        const double t0 = nowWallMs();
        w.playback()->play();
        while (nowWallMs() - t0 < 2000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        w.playback()->pause();
        const double wall = nowWallMs() - t0;
        bench("play_ui_busy", 100.0 * (gApp->busy - b0) / wall, "% of wall time");
        bench("play_longest_event", gApp->longest, "ms");
        if (qEnvironmentVariableIsSet("RENO_PERF_BREAKDOWN")) gApp->report("play");
        uitest::settle(200);
    }
}

int main(int argc, char** argv)
{
    TimedApp qapp(argc, argv);
    gApp = &qapp;
    QApplication::setOrganizationName("RenoUiPerfTest");
    QApplication::setApplicationName("ui_perf_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: ui_perf_test <output folder> <perf-media folder> [--samples dir] [--shots dir] [--tracks N] [names...]\n");
        return 2;
    }
    const QString out = QDir(args[1]).absolutePath();
    const QString media = QDir(args[2]).absolutePath();
    QString samples = qEnvironmentVariable("RENO_SAMPLES"), shots;
    int extraTracks = 60;
    QStringList names;
    for (int k = 3; k < args.size(); ++k) {
        if (args[k] == "--samples" && k + 1 < args.size()) samples = args[++k];
        else if (args[k] == "--shots" && k + 1 < args.size()) shots = QDir(args[++k]).absolutePath();
        else if (args[k] == "--tracks" && k + 1 < args.size()) extraTracks = args[++k].toInt();
        else names << args[k];
    }
    if (names.isEmpty()) names << "stutter combined 1.veg";
    QDir().mkpath(out);
    if (!QFileInfo(samples).isDir() || !QFileInfo(media + "/clip720.mp4").exists()) {
        std::printf("SKIP: no sample projects in %s or no stand-in clips in %s\nSUMMARY: 0 passed, 0 failed\n", qPrintable(samples), qPrintable(media));
        return 0;
    }
    qputenv("RENO_TEST_OPEN_LOG", (out + "/opened.txt").toLocal8Bit());
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    setProjectFileLoader([](const std::string& path, Project& o, std::string* error) { return loadProjectFile(path, o, error); });
    writeFakeCatalog(450, 120);

    gProject = "(start)";
    {
        const double t0 = nowWallMs();
        auto w = std::make_unique<MainWindow>();
        const double t1 = nowWallMs();
        w->resize(1600, 1000);
        w->show();
        drain();
        const double t2 = nowWallMs();
        w->grab();
        const double t3 = nowWallMs();
        bench("start_ctor", t1 - t0, "ms");
        bench("start_show", t2 - t1, "ms");
        bench("start_first_grab", t3 - t2, "ms");
        uitest::settle(300);

        for (const QString& name : names) {
            QString orig;
            QDirIterator it(samples, QStringList() << name, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext()) orig = it.next();
            if (orig.isEmpty()) { check(false, "found " + name.toStdString()); continue; }
            const QString folder = out + "/" + QFileInfo(orig).absoluteDir().dirName();
            makeStandinFolder(orig, samples, media, folder);
            AppState::get().newProject(true);
            uitest::settle(200);
            gProject = name.toStdString();
            session(*w, folder + "/" + name, shots, extraTracks);
            AppState::get().newProject(true);
            uitest::settle(200);
        }
    }
    return 0;
}
