// Does the UI stay responsive with big projects? Opens projects with hundreds or thousands of tiny events at 140/165 fps, many
// effects, and .veg files used as media, in the whole main window as File > Open does, then works with them as a user: waits for
// pictures and waveforms, scrolls, zooms, clicks and drags an event, splits and undoes, plays. Every event the UI thread handles
// is timed (QApplication::notify at the outermost level); one longer than a frame is a stall, over 50 ms is lag a user feels.
//
// The media are not here: a copy of each sample's folder is made in which every file the project names is a stand-in clip of the
// same kind (perf-media: clip480/720/1080.mp4, tone.wav, still.png) under the name the project gives it, nested .veg files
// included (copied beside the project that uses them).
//
//   load_lag_test <output folder> <perf-media folder> [--samples <folder>] [--bench] [--limit <ms>] [project.veg names...]
//   (the sample VEGAS projects default to the folder named by $RENO_SAMPLES; without them those parts SKIP)
// Without names: the first two .veg files under the samples folder (or the one in $RENO_SAMPLE_VEG). Without --bench the checks run: no UI stall longer than --limit (default 600 ms; ASan is
// several times slower and other tests may be running) after the project is open, and opening takes under 10 s. --bench prints
// "BENCH <project> <measure> <value>" lines only; --profile (Linux) also prints what the UI thread was doing in its longest stalls.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/vegas/vegas.h"
#include "uitestlib.h"
#include "../testdata.h"
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QElapsedTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#ifdef __linux__
#include <csignal>
#include <csetjmp>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#endif
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>

using namespace reno;
using namespace reno::app;
using uitest::check;

// An event that kept the UI thread for longer than a frame: how long, how much of that it computed itself (the rest it waited),
// and (--profile) which samples were taken in it.
struct Stall { double ms; double cpuMs; std::string phase, what; int sample0 = 0, sample1 = 0; };

static double threadCpuMs()
{
#ifdef __linux__
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------- --profile (Linux): what the UI thread does while it stalls
// A watcher thread interrupts it every 5 ms once an event has run over 16 ms and keeps the stack; at the end the functions in
// the most samples are printed (link with -rdynamic for names).
#ifdef __linux__
namespace prof {
constexpr int kDepth = 48, kMax = 200000;
static void* gStacks[kMax][kDepth];
static int gDepths[kMax];
static std::atomic<int> gCount{0};
static std::atomic<long long> gEventStart{0};      // steady ns when the outermost event began, 0 when idle
static pthread_t gMain;
static std::atomic<bool> gRun{false};

static sigjmp_buf gJump;
static volatile sig_atomic_t gInHandler = 0;
// the unwinder can trip over a frame without unwind information: that sample is dropped
static void onFault(int sig)
{
    if (gInHandler) siglongjmp(gJump, 1);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void onSignal(int)
{
    const int k = gCount.fetch_add(1);
    if (k >= kMax) return;
    gDepths[k] = 0;
    gInHandler = 1;
    if (sigsetjmp(gJump, 1) == 0) gDepths[k] = backtrace(gStacks[k], kDepth);
    gInHandler = 0;
}
static const char* baseNameC(const char* p) { const char* s = std::strrchr(p, '/'); return s ? s + 1 : p; }
static long long nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void start()
{
    gMain = pthread_self();
    void* warm[4];
    backtrace(warm, 4);  // loads libgcc before the first signal
    struct sigaction sa = {};
    sa.sa_handler = onSignal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    struct sigaction sf = {};
    sf.sa_handler = onFault;
    sf.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sf, nullptr);
    sigaction(SIGBUS, &sf, nullptr);
    gRun = true;
    std::thread([] {
        while (gRun) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const long long s = gEventStart.load();
            if (s && nowNs() - s > 16000000LL) pthread_kill(gMain, SIGPROF);
        }
    }).detach();
}
// Names: shared libraries' exported functions through dladdr, the program's own through addr2line (asked once for all).
static std::map<void*, std::string> gNames;
static void resolve(const std::set<void*>& addrs)
{
    std::vector<void*> own;
    for (void* a : addrs) {
        Dl_info info;
        if (dladdr(a, &info) && info.dli_sname && info.dli_fbase) {
            int st = 0;
            char* d = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &st);
            gNames[a] = d ? d : info.dli_sname;
            free(d);
        } else if (dladdr(a, &info) && info.dli_fname && std::string(info.dli_fname).find(".so") == std::string::npos) {
            own.push_back(a);
        } else {
            char b[64];
            std::snprintf(b, sizeof b, "%s+?", info.dli_fname ? baseNameC(info.dli_fname) : "?");
            gNames[a] = b;
        }
    }
    if (own.empty()) return;
    Dl_info info;
    dladdr(own[0], &info);
    const uintptr_t base = (uintptr_t)info.dli_fbase;
    const std::string list = "/tmp/load_lag_addrs.txt";
    FILE* f = std::fopen(list.c_str(), "w");
    if (!f) return;
    for (void* a : own) std::fprintf(f, "0x%lx\n", (unsigned long)((uintptr_t)a - base - 1));
    std::fclose(f);
    char exe[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    const std::string cmd = "addr2line -f -C -e '" + std::string(exe) + "' < " + list;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return;
    char line[8192];
    for (void* a : own) {
        if (!std::fgets(line, sizeof line, p)) break;
        std::string fn = line;
        if (!fn.empty() && fn.back() == '\n') fn.pop_back();
        if (!std::fgets(line, sizeof line, p)) break;  // file:line
        gNames[a] = fn;
    }
    pclose(p);
}
static std::string name(void* a)
{
    std::string r = gNames.count(a) ? gNames[a] : "?";
    if (r.size() > 150) r = r.substr(0, 150);
    return r;
}
static void report(const char* title, const std::vector<Stall>& stalls)
{
    const int n = std::min(gCount.load(), kMax);
    std::set<void*> addrs;
    for (int k = 0; k < n; ++k)
        for (int d = 2; d < gDepths[k]; ++d) addrs.insert(gStacks[k][d]);
    resolve(addrs);
    std::map<std::string, int> incl, self;
    for (int k = 0; k < n; ++k) {
        std::set<std::string> seen;
        for (int d = 2; d < gDepths[k]; ++d) {
            const std::string f = name(gStacks[k][d]);
            if (d == 2) ++self[f];
            if (seen.insert(f).second) ++incl[f];
        }
    }
    auto top = [&](std::map<std::string, int>& m, const char* what, int count) {
        std::vector<std::pair<int, std::string>> v;
        for (auto& kv : m) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        std::printf("PROFILE %s: %d samples, %s\n", title, n, what);
        for (int k = 0; k < (int)v.size() && k < count; ++k) std::printf("PROFILE %6d  %s\n", v[k].first, v[k].second.c_str());
    };
    // the samples' paths through Reno's own code (innermost first) with the outermost library call they were in
    std::map<std::string, int> paths;
    auto pathOf = [&](int k) {
        std::string path, leaf;
        int kept = 0;
        for (int d = 2; d < gDepths[k] && kept < 7; ++d) {
            std::string f = name(gStacks[k][d]);
            const bool own = f.find("reno::") != std::string::npos;
            if (!own && path.empty()) { if (leaf.empty() || f.find('+') == std::string::npos) leaf = f.substr(0, 60); continue; }
            if (!own) continue;
            const size_t paren = f.find('(');
            if (paren != std::string::npos) f = f.substr(0, paren);
            path += (path.empty() ? "" : " < ") + f;
            ++kept;
        }
        return path.empty() ? std::string() : "[" + leaf + "] " + path;
    };
    for (int k = 0; k < n; ++k) {
        const std::string p = pathOf(k);
        if (!p.empty()) ++paths[p];
    }
    top(paths, "paths", 45);
    // the longest stalls, each with what most of its samples were doing
    std::vector<const Stall*> longest;
    for (const Stall& st : stalls) longest.push_back(&st);
    std::sort(longest.begin(), longest.end(), [](const Stall* a, const Stall* b) { return a->ms > b->ms; });
    for (size_t k = 0; k < longest.size() && k < 15; ++k) {
        const Stall& st = *longest[k];
        std::map<std::string, int> in;
        for (int j = st.sample0; j < st.sample1 && j < n; ++j) ++in[pathOf(j)];
        std::string best;
        int bestN = 0;
        for (auto& kv : in) if (kv.second > bestN) { bestN = kv.second; best = kv.first; }
        std::printf("PROFILE stall %8.1f ms (cpu %6.1f) %-10s %s | %d/%d samples: %s\n", st.ms, st.cpuMs, st.phase.c_str(), st.what.c_str(), bestN,
                    st.sample1 - st.sample0, best.substr(0, 260).c_str());
    }
    top(incl, "inclusive", 70);
    top(self, "self", 25);
    gCount = 0;
}
}
#endif

// ---------------------------------------------------------------- timing the interface thread

class TimedApp : public QApplication {
public:
    TimedApp(int& argc, char** argv) : QApplication(argc, argv) {}
    bool notify(QObject* o, QEvent* e) override
    {
        if (depth_ > 0) {
            ++depth_;
            const bool r = QApplication::notify(o, e);
            --depth_;
            return r;
        }
        ++depth_;
        // taken first: the object or event may be gone afterwards
        const char* cls = o ? o->metaObject()->className() : "?";
        const int type = e ? (int)e->type() : -1;
        const double c0 = threadCpuMs();
        const std::string ph = phase;
#ifdef __linux__
        const int s0 = prof::gCount.load();
#else
        const int s0 = 0;
#endif
        const auto t0 = std::chrono::steady_clock::now();
#ifdef __linux__
        prof::gEventStart = std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count();
#endif
        const bool r = QApplication::notify(o, e);
#ifdef __linux__
        prof::gEventStart = 0;
#endif
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        --depth_;
        if (ms > 16.0 && recording) {
            std::string what = cls;
            what += " event " + std::to_string(type);
#ifdef __linux__
            const int s1 = prof::gCount.load();
#else
            const int s1 = 0;
#endif
            stalls.push_back({ms, threadCpuMs() - c0, ph, what, s0, s1});
        }
        return r;
    }
    bool recording = true;
    std::string phase = "start";
    std::vector<Stall> stalls;

private:
    int depth_ = 0;
};

// ---------------------------------------------------------------- the stand-in copy of a sample folder
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

// Copies the folder of 'project' (every .veg in it) to 'dest' with a stand-in under the name of every file the projects there use;
// a nested project from another folder is copied in too (with stand-ins for its media).
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
                // a nested project: the one in the sample folder, or one of that name elsewhere among the samples
                QString from = src.filePath(QString::fromStdString(name));
                if (!QFileInfo::exists(from)) {
                    from.clear();
                    QDirIterator it(samplesRoot, QStringList() << QString::fromStdString(name), QDir::Files, QDirIterator::Subdirectories);
                    if (it.hasNext()) from = it.next();
                }
                if (from.isEmpty()) continue;  // missing for the user too: stays offline
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

// ---------------------------------------------------------------- the session
struct Result {
    double openMs = 0;
    double longest = 0;                 // after the open itself
    std::string longestWhat;
    int over50 = 0, over16 = 0;
    std::map<std::string, std::pair<int, double>> phases;    // phase -> (stalls > 50 ms, longest)
    double paintFit = 0, paintZoomed = 0;               // one whole repaint of the timeline, zoomed to fit / zoomed in
    std::vector<std::pair<std::string, double>> memory;  // at the end of each phase
};

static TimedApp* gApp = nullptr;

// The memory the program holds now (MB; 0 where this can't be read).
static double memoryNowMb()
{
    QFile st("/proc/self/status");
    if (st.open(QIODevice::ReadOnly))
        for (const QByteArray& line : st.readAll().split('\n'))
            if (line.startsWith("VmRSS:")) return line.mid(6).trimmed().split(' ')[0].toDouble() / 1024.0;
    return 0;
}

// Runs 'steps' one after another from the event loop, each 'gapMs' after the last finished.
static void runSteps(const std::vector<std::pair<int, std::function<void()>>>& steps)
{
    QEventLoop loop;
    size_t k = 0;
    std::function<void()> next;
    next = [&] {
        if (k >= steps.size()) { loop.quit(); return; }
        const auto& s = steps[k++];
        QTimer::singleShot(s.first, gApp, [&, s] {
            s.second();
            next();
        });
    };
    next();
    loop.exec();
}

static Result session(MainWindow& w, const QString& path)
{
    Result res;
    AppState& app = AppState::get();
    TimelineWidget* tl = w.timeline();
    std::vector<std::pair<int, std::function<void()>>> steps;
    auto phase = [&](const char* name) {
        steps.push_back({0, [name, &res] {
            res.memory.push_back({gApp->phase, memoryNowMb()});
            gApp->phase = name;
        }});
    };
    auto wait = [&](int ms) { steps.push_back({ms, [] {}}); };

    gApp->stalls.clear();
    phase("open");
    steps.push_back({10, [&] {
        QElapsedTimer t;
        t.start();
        w.openProject(path);
        res.openMs = t.nsecsElapsed() / 1.0e6;
        gApp->phase = "after open";
    }});
    // the pictures and waveforms come in
    for (int k = 0; k < 40; ++k) wait(50);

    phase("scroll");
    for (int k = 0; k <= 24; ++k)
        steps.push_back({40, [tl, k] {
            QScrollBar* h = tl->findChild<QScrollBar*>("timelineHScroll");
            if (h) h->setValue(h->minimum() + (h->maximum() - h->minimum()) * (k % 13) / 12);
            QScrollBar* v = tl->findChild<QScrollBar*>("timelineVScroll");
            if (v && k % 4 == 0) v->setValue(v->minimum() + (v->maximum() - v->minimum()) * (k % 3) / 2);
        }});
    phase("zoom");
    for (int k = 0; k < 16; ++k)
        steps.push_back({40, [tl, k] { uitest::wheel(tl, QPoint(tl->headerWidth() + 300, tl->height() / 2), k < 8 ? 120 : -120); }});
    steps.push_back({40, [tl] { tl->zoomToFit(); }});
    wait(500);

    phase("click");
    steps.push_back({40, [tl, &app] {
        // click about the lanes until an event is selected
        for (int y = 70; y < tl->height() - 30 && app.selection().empty(); y += 14)
            for (int x = tl->headerWidth() + 20; x < tl->width() - 40 && app.selection().empty(); x += 37)
                uitest::mouseClick(tl, QPoint(x, y));
    }});
    wait(200);
    phase("drag");
    auto grab = std::make_shared<QPoint>(-1, -1);
    steps.push_back({40, [tl, &app, grab] {
        if (app.selection().empty()) return;
        // an event's middle: found by clicking about again, then pressed there
        for (int y = 70; y < tl->height() - 30 && grab->x() < 0; y += 14)
            for (int x = tl->headerWidth() + 20; x < tl->width() - 40 && grab->x() < 0; x += 37) {
                uitest::mouseClick(tl, QPoint(x, y));
                if (!app.selection().empty()) *grab = QPoint(x, y);
            }
        if (grab->x() >= 0) uitest::mousePress(tl, *grab);
    }});
    for (int k = 1; k <= 8; ++k)
        steps.push_back({30, [tl, k, grab] {
            if (grab->x() >= 0) uitest::mouseMove(tl, *grab + QPoint(k * 6, 0), Qt::LeftButton);
        }});
    steps.push_back({30, [tl, grab] {
        if (grab->x() >= 0) uitest::mouseRelease(tl, *grab + QPoint(48, 0));
    }});
    wait(200);

    phase("split/undo");
    steps.push_back({40, [&app] { app.setCursor(app.project().length() / 3); }});
    steps.push_back({40, [&app] { app.splitAtCursor(); }});
    steps.push_back({100, [&app] { app.undo(); }});
    steps.push_back({100, [&app] { app.redo(); }});
    steps.push_back({100, [&app] { app.undo(); }});
    wait(200);

    phase("play");
    steps.push_back({40, [&w, &app] {
        app.setCursor(0);
        w.playback()->play();
    }});
    for (int k = 0; k < 40; ++k) wait(100);
    steps.push_back({0, [&w] { w.playback()->pause(); }});
    wait(300);
    // what painting the whole timeline costs (the interface thread's own time, nothing else)
    phase("paint");
    steps.push_back({100, [tl, &res] {
        auto timeRepaints = [tl] {
            QElapsedTimer t;
            t.start();
            for (int k = 0; k < 5; ++k) tl->repaint();
            return t.nsecsElapsed() / 5.0e6;
        };
        tl->zoomToFit();
        res.paintFit = timeRepaints();
        for (int k = 0; k < 6; ++k) uitest::wheel(tl, QPoint(tl->headerWidth() + 300, tl->height() / 2), 120);
        res.paintZoomed = timeRepaints();
    }});
    phase("end");

    runSteps(steps);

    for (const Stall& s : gApp->stalls) {
        if (s.phase == "open" || s.phase == "paint") continue;  // the open itself: openMs; paint has its own measure
        if (s.ms > res.longest) { res.longest = s.ms; res.longestWhat = s.phase + ": " + s.what + strfmt(", cpu %.0f ms", s.cpuMs); }
        if (s.ms > 50) ++res.over50;
        ++res.over16;
        auto& ph = res.phases[s.phase];
        if (s.ms > 50) ++ph.first;
        ph.second = std::max(ph.second, s.ms);
    }
    return res;
}

int main(int argc, char** argv)
{
    TimedApp qapp(argc, argv);
    gApp = &qapp;
    QApplication::setOrganizationName("RenoLoadLagTest");
    QApplication::setApplicationName("load_lag_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: load_lag_test <output folder> <perf-media folder> [--samples dir] [--bench] [--limit ms] [names...]\n");
        return 2;
    }
    const QString out = QDir(args[1]).absolutePath();
    const QString media = QDir(args[2]).absolutePath();
    QString samples = qEnvironmentVariable("RENO_SAMPLES");
    bool bench = false, profile = false;
    double limit = 600;
    QStringList names;
    for (int k = 3; k < args.size(); ++k) {
        if (args[k] == "--samples" && k + 1 < args.size()) samples = args[++k];
        else if (args[k] == "--bench") bench = true;
        else if (args[k] == "--profile") profile = bench = true;
        else if (args[k] == "--limit" && k + 1 < args.size()) limit = args[++k].toDouble();
        else names << args[k];
    }
    if (names.isEmpty()) {
        std::vector<std::string> all = testVegFiles(samples.toStdString());
        if (!testSampleVeg().empty()) all = {testSampleVeg()};
        for (size_t k = 0; k < all.size() && k < 2; ++k) names << QFileInfo(QString::fromStdString(all[k])).fileName();
    }
    QDir().mkpath(out);
    if (!QFileInfo(samples).isDir() || !QFileInfo(media + "/clip720.mp4").exists()) {
        // the real projects and stand-in clips are only on the machine this was written on
        std::printf("SKIP: no sample projects in %s or no stand-in clips in %s\nSUMMARY: 0 passed, 0 failed\n", qPrintable(samples), qPrintable(media));
        return 0;
    }
    // a double-click on a nested project's event opens it in another Reno: noted instead
    qputenv("RENO_TEST_OPEN_LOG", (out + "/opened.txt").toLocal8Bit());
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    // as the program itself: real files, nested .veg files read with the VEGAS reader
    AppState::get().mediaOpener = [](const std::string& path, std::string* error) { return openMediaOrProject(path, error); };
    setProjectFileLoader([](const std::string& path, Project& o, std::string* error) { return loadProjectFile(path, o, error); });

#ifdef __linux__
    if (profile) prof::start();
#endif
    MainWindow w;
    w.resize(1600, 1000);
    w.show();
    uitest::settle(300);

    for (const QString& name : names) {
        QString orig;
        QDirIterator it(samples, QStringList() << name, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) orig = it.next();
        if (orig.isEmpty()) { check(false, "found " + name.toStdString()); continue; }
        const QString folder = out + "/" + QFileInfo(orig).absoluteDir().dirName();
        makeStandinFolder(orig, samples, media, folder);
        const QString path = folder + "/" + name;
        AppState::get().newProject(true);
        uitest::settle(200);
        const Result r = session(w, path);
        const std::string n = name.toStdString();
        std::printf("BENCH %-26s open_ms %10.1f\n", n.c_str(), r.openMs);
        std::printf("BENCH %-26s longest_stall_ms %10.1f  (%s)\n", n.c_str(), r.longest, r.longestWhat.c_str());
        std::printf("BENCH %-26s stalls_over_50ms %6d\n", n.c_str(), r.over50);
        std::printf("BENCH %-26s stalls_over_16ms %6d\n", n.c_str(), r.over16);
        std::printf("BENCH %-26s timeline_paint_ms %8.1f (zoomed to fit)  %8.1f (zoomed in)\n", n.c_str(), r.paintFit, r.paintZoomed);
        {
            // most memory held so far
            QFile st("/proc/self/status");
            if (st.open(QIODevice::ReadOnly))
                for (const QByteArray& line : st.readAll().split('\n'))
                    if (line.startsWith("VmHWM:")) std::printf("BENCH %-26s peak_memory_mb %8.0f\n", n.c_str(), line.mid(6).trimmed().split(' ')[0].toDouble() / 1024.0);
        }
        {
            std::string mem;
            for (const auto& m : r.memory) mem += strfmt(" %s %.0f", m.first.c_str(), m.second);
            std::printf("BENCH %-26s memory_mb_after:%s\n", n.c_str(), mem.c_str());
        }
        for (const auto& ph : r.phases)
            std::printf("BENCH %-26s   phase %-12s over50 %4d longest %8.1f\n", n.c_str(), ph.first.c_str(), ph.second.first, ph.second.second);
#ifdef __linux__
        if (profile) prof::report(n.c_str(), gApp->stalls);
#endif
        std::fflush(stdout);
        if (!bench) {
            check(AppState::get().path() == path.toStdString(), n + " opened");
            check(r.openMs < 10000, n + " opens in under 10 s", strfmt("%.0f ms", r.openMs));
            check(r.longest < limit, n + strfmt(" interface never stalls over %.0f ms", limit), strfmt("%.0f ms (%s)", r.longest, r.longestWhat.c_str()));
        }
    }
    AppState::get().newProject(true);
    uitest::settle(200);
    if (bench) return 0;
    // waveforms and thumbnails are kept on disk (mediacache.h)
    const QString cache = QString::fromStdString(pathJoin(userDataDir(), "cache"));
    // as VEGAS does, in .sfk peak files beside the media; Reno's cache where that can't be
    int sfk = 0;
    for (QDirIterator it(out, QStringList() << "*.sfk", QDir::Files, QDirIterator::Subdirectories); it.hasNext(); it.next()) ++sfk;
    check(sfk > 0 || !QDir(cache + "/peaks").entryList(QDir::Files).isEmpty(), "waveforms are kept on disk", strfmt("%d .sfk files; %s", sfk, qPrintable(cache)));
    check(!QDir(cache + "/thumbs").entryList(QDir::Files).isEmpty(), "thumbnails are kept on disk", qPrintable(cache));
    return uitest::summary();
}
