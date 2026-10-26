// Reno's start (startup.h): the splash screen, the plug-in scan behind it with Skip, the plug-ins of the project Reno starts with
// loaded behind it, and the progress window while File > Open loads slow plug-ins. Uses stand-in plug-ins that take their time
// (tests/ui/slowvst.cpp, and the test OpenFX plug-in slowed down by TESTFX_SLOW_CREATE_MS) and one that crashes when loaded.
//
//   splash_test <output folder> <testfx.ofx> <reno_cli> <folder of slow VSTs: slow1.so slow2.so crash.so newslow.so>
// HOME should be an empty folder of its own (the plug-in list and plug-in folders are made under it).
#include "../../src/app/mainwindow.h"
#include "../../src/app/startup.h"
#include "../../src/app/timeline.h"
#include "../../src/engine/catalog.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
MainWindow* gWin = nullptr;

QString home() { return QDir::homePath(); }

bool copyTo(const QString& from, const QString& to)
{
    QFile::remove(to);
    QDir().mkpath(QFileInfo(to).absolutePath());
    return QFile::copy(from, to) && QFile::setPermissions(to, QFile::permissions(from) | QFile::ExeOwner);
}

// Watches a start: the longest the event loop was kept from running while the splash said it was looking for plug-ins,
// what it said, and pictures of it.
struct Watch {
    QTimer timer;
    QElapsedTimer clock;
    long long last = -1, worstScanGap = 0;
    int scanTicks = 0;
    QStringList seen;
    bool shotScan = false;
    QString scanShot = "splash_scan.png";
    std::function<void(SplashScreen*)> each;
    void start()
    {
        clock.start();
        QObject::connect(&timer, &QTimer::timeout, [this] {
            SplashScreen* s = SplashScreen::current();
            if (!s) { last = -1; return; }
            const long long now = clock.elapsed();
            if (s->status().startsWith("Scanning for")) {
                if (last >= 0) worstScanGap = std::max(worstScanGap, now - last);
                last = now;
                ++scanTicks;
                if (!shotScan && s->detail().startsWith("Checking")) {
                    uitest::shot(s, gOut + "/" + scanShot);
                    shotScan = true;
                }
            } else last = -1;
            const QString line = s->status() + " | " + s->detail();
            if (seen.isEmpty() || seen.back() != line) seen << line;
            if (each) each(s);
        });
        timer.start(15);
    }
};

MainWindow* reuseWindow()
{
    AppState::get().newProject(true);
    if (!gWin) gWin = new MainWindow();
    else gWin->hide();
    return gWin;
}

AudioPluginDesc findAudio(const std::string& name)
{
    for (const auto& e : Catalog::get().audio())
        if (e.desc.name == name) return e.desc;
    return AudioPluginDesc();
}

// A project with two slow VSTs on an audio track, the crashing one, and two slowed-down OpenFX effects.
struct SlowProject {
    std::string path;
    uint64_t slow1 = 0, slow2 = 0, crash = 0, video1 = 0, video2 = 0;
};

SlowProject makeProject(const std::string& name)
{
    SlowProject sp;
    Project p;
    Track a;
    a.id = p.newId();
    a.type = TrackType::Audio;
    a.name = "Audio";
    auto audio = [&](const AudioPluginDesc& d, uint64_t& id) {
        FxSlot s;
        s.id = id = p.newId();
        s.kind = "audio";
        s.audio = d;
        s.name = d.name;
        a.fx.push_back(s);
    };
    audio(findAudio("Slow Verb"), sp.slow1);
    audio(findAudio("Slow Delay"), sp.slow2);
    AudioPluginDesc crash;  // not in the list: trying it crashed the checking process
    crash.format = "VST2";
    crash.path = (home() + "/.vst/crash.so").toStdString();
    crash.uid = std::to_string(0x534C5703);
    crash.name = "Crashy";
    audio(crash, sp.crash);
    Track v;
    v.id = p.newId();
    v.type = TrackType::Video;
    v.name = "Video";
    for (uint64_t* id : {&sp.video1, &sp.video2}) {
        FxSlot s;
        s.id = *id = p.newId();
        s.kind = "ofx";
        s.pluginId = "reno.test.invert";
        s.name = "Slow Invert";
        v.fx.push_back(s);
    }
    p.tracks.push_back(v);
    p.tracks.push_back(a);
    sp.path = (gOut + "/" + QString::fromStdString(name)).toStdString();
    std::string err;
    check(p.save(sp.path, &err), "made the test project " + name, err);
    return sp;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: splash_test <output folder> <testfx.ofx> <reno_cli> <slow VST folder>\n");
        return 2;
    }
    // the OpenFX plug-in where Reno looks for it, slowed down
    const QString ofxDir = QDir::homePath() + "/ofx";
    QDir().mkpath(ofxDir);
    qputenv("OFX_PLUGIN_PATH", ofxDir.toUtf8());
    qputenv("TESTFX_SLOW_CREATE_MS", "500");
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("splash_test");
    gOut = argv[1];
    QDir().mkpath(gOut);
    const QString testfx = argv[2], cli = argv[3], vstDir = argv[4];
    uitest::setup({}, {});  // made-up media; no plug-ins: the scan finds them
    check(copyTo(testfx, ofxDir + "/testfx.ofx"), "test OpenFX plug-in in place");
    const QString vstHome = home() + "/.vst";
    for (const char* f : {"slow1.so", "slow2.so", "crash.so"})
        check(copyTo(vstDir + "/" + f, vstHome + "/" + f), std::string("VST in place: ") + f);
    // what the user would see just before each plug-in starts
    QStringList beforeLines;
    PluginLoadProgress::beforePlugin() = [&](const QString& painted) { beforeLines << painted; };

    // ---- 1. the first start: no plug-in list yet, so every plug-in is checked behind the splash (each takes a while)
    {
        Watch watch;
        watch.start();
        StartupOptions o;
        o.probeExe = cli.toStdString();
        o.offerRecovery = false;
        o.makeWindow = reuseWindow;
        int splashes = 0;
        o.onSplash = [&](SplashScreen* s) {
            ++splashes;
            uitest::shot(s, gOut + "/splash_start.png");
        };
        QElapsedTimer t;
        t.start();
        MainWindow* w = runStartup(o);
        const long long ms = t.elapsed();
        check(w && w == gWin && w->isVisible(), "first start: the main window is shown after the splash");
        check(SplashScreen::current() == nullptr, "first start: the splash is gone");
        check(splashes == 1, "first start: one splash screen");
        check(watch.shotScan, "first start: the splash names the plug-in being checked", watch.seen.join(" / ").toStdString());
        check(watch.scanTicks > 20 && watch.worstScanGap < 400, "first start: the splash stays live through the scan",
              strfmt("%d ticks, longest gap %lld ms", watch.scanTicks, watch.worstScanGap));
        bool named = false;
        for (const QString& l : watch.seen) if (l.contains("slow1.so") || l.contains("slow2.so")) named = true;
        check(named, "first start: the files are named as they are checked");
        check(!findAudio("Slow Verb").path.empty() && !findAudio("Slow Delay").path.empty(), "first start: the slow VSTs are found");
        check(Catalog::get().crashedWhenTried((vstHome + "/crash.so").toStdString()),
              "first start: the crashing VST is marked as having crashed (and Reno carried on)");
        VideoFxEntry ve;
        check(Catalog::get().findVideo("reno.test.invert", &ve) && !ve.builtin, "first start: the OpenFX plug-in is found");
        std::printf("first start: %lld ms; lines: %s\n", ms, watch.seen.join(" / ").toStdString().c_str());
        uitest::settle(200);
        uitest::shot(w, gOut + "/main_after_first.png");
    }

    // ---- 2. a start with nothing new and nothing to open: the splash goes quickly
    {
        Watch watch;
        watch.start();
        StartupOptions o;
        o.probeExe = cli.toStdString();
        o.offerRecovery = false;
        o.makeWindow = reuseWindow;
        QElapsedTimer t;
        t.start();
        MainWindow* w = runStartup(o);
        const long long ms = t.elapsed();
        check(w && w->isVisible() && !SplashScreen::current(), "quick start: the main window is shown");
        check(ms < 3000, "quick start: nothing slow, the splash goes at once", strfmt("%lld ms", ms));
        check(!ScanTask::get().state().running && ScanTask::get().state().lookedOnly, "quick start: nothing new, nothing tried");
    }

    // ---- 3. started with a project whose plug-ins are slow: they are loaded behind the splash, each named first
    const SlowProject sp = makeProject("slow.reno");
    {
        beforeLines.clear();
        Watch watch;
        bool shotLoad = false;
        watch.each = [&](SplashScreen*) {};
        watch.start();
        PluginLoadProgress::beforePlugin() = [&](const QString& painted) {
            beforeLines << painted;
            if (!shotLoad && painted.contains("Slow Delay")) {
                if (SplashScreen* s = SplashScreen::current()) uitest::shot(s, gOut + "/splash_plugins.png");
                shotLoad = true;
            }
        };
        StartupOptions o;
        o.probeExe = cli.toStdString();
        o.openPath = QString::fromStdString(sp.path);
        o.makeWindow = reuseWindow;
        QElapsedTimer t;
        t.start();
        MainWindow* w = runStartup(o);
        std::printf("start with project: %lld ms; before each plug-in: %s\n", (long long)t.elapsed(), beforeLines.join(" / ").toStdString().c_str());
        check(w && w->isVisible() && !SplashScreen::current(), "start with a project: the main window is shown after");
        check(AppState::get().path() == sp.path, "start with a project: it is open");
        check(beforeLines.size() == 5, "start with a project: every plug-in shown before it is made", strfmt("%d", (int)beforeLines.size()));
        // in the runtime's order; each named once, the two OpenFX effects twice
        auto count = [&](const QStringList& lines, const char* name) {
            int n = 0;
            for (const QString& l : lines) if (l.contains(name)) ++n;
            return n;
        };
        const bool all = count(beforeLines, "Slow Invert") == 2 && count(beforeLines, "Slow Verb") == 1 &&
                         count(beforeLines, "Slow Delay") == 1 && count(beforeLines, "Crashy") == 1;
        check(all, "start with a project: the splash said which plug-in it waited on, before each", beforeLines.join(" / ").toStdString());
        check(beforeLines.size() == 5 && beforeLines[4].contains("5 of 5"), "start with a project: and how many");
        check(shotLoad, "start with a project: picture of the splash while plug-ins load");
        const FxRuntime& fx = AppState::get().fx;
        check(fx.audio(sp.slow1) && fx.audio(sp.slow2), "start with a project: the slow VSTs are made and started");
        check(fx.video(sp.video1) && fx.video(sp.video2), "start with a project: the OpenFX effects are made");
        check(!fx.audio(sp.crash) && fx.error(sp.crash).find("crashed") != std::string::npos,
              "start with a project: the crashing VST is skipped, saying why", fx.error(sp.crash));
        check(!AppState::get().fx.onMaking, "start with a project: the runtime is left as it was");
        uitest::settle(200);
        uitest::shot(w, gOut + "/main_with_project.png");
    }

    // ---- 4. File > Open of a project with slow plug-ins: a progress window says which one is loading
    {
        beforeLines.clear();
        QStringList windowLines;
        QDialog* seenWindow = nullptr;
        bool shot = false;
        PluginLoadProgress::beforePlugin() = [&](const QString& painted) {
            beforeLines << painted;
            QDialog* d = PluginLoadProgress::currentWindow();
            if (d && d->isVisible()) {
                seenWindow = d;
                windowLines << painted;
                if (!shot && painted.contains("Slow Verb")) {
                    uitest::shot(d, gOut + "/open_progress.png");
                    shot = true;
                }
            }
        };
        const SlowProject sp2 = makeProject("slow2.reno");
        // the window stays live: a timer keeps running between plug-ins
        int ticks = 0;
        QTimer tick;
        QObject::connect(&tick, &QTimer::timeout, [&] { ++ticks; });
        tick.start(10);
        const bool ok = gWin->openProject(QString::fromStdString(sp2.path));
        tick.stop();
        std::printf("File > Open: %s\n", windowLines.join(" / ").toStdString().c_str());
        check(ok && AppState::get().path() == sp2.path, "File > Open: the project opens");
        check(seenWindow != nullptr, "File > Open: a progress window is up while the plug-ins load");
        bool named = windowLines.size() == 5;
        for (const QString& l : windowLines) if (!l.contains("Opening slow2.reno")) named = false;
        bool verb = false;
        for (const QString& l : windowLines) if (l.contains("Slow Verb (VST)") && l.contains("slow1.so")) verb = true;
        check(named && verb, "File > Open: it names each plug-in and its file", windowLines.join(" / ").toStdString());
        check(ticks >= 4, "File > Open: the event loop runs between plug-ins", strfmt("%d", ticks));
        check(PluginLoadProgress::currentWindow() == nullptr && QApplication::activeModalWidget() == nullptr, "File > Open: the window is gone after");
        check(shot, "File > Open: picture of the progress window");
        check(AppState::get().fx.error(sp2.crash).find("crashed") != std::string::npos, "File > Open: the crashing VST is skipped");

        // a project with only Reno's own effects (quick) shows no window
        Project q;
        Track v;
        v.id = q.newId();
        v.type = TrackType::Video;
        FxSlot s;
        s.id = q.newId();
        s.kind = "ofx";
        s.pluginId = "com.vegascreativesoftware:brightnessandcontrast";
        v.fx.push_back(s);
        q.tracks.push_back(v);
        const std::string qp = (gOut + "/quick.reno").toStdString();
        q.save(qp);
        seenWindow = nullptr;
        gWin->openProject(QString::fromStdString(qp));
        check(seenWindow == nullptr, "File > Open: no progress window when nothing is slow");
    }

    // ---- 5. a new plug-in file: looked for at the start; Skip goes on to Reno and the scan finishes in the background
    {
        check(copyTo(vstDir + "/newslow.so", vstHome + "/newslow.so"), "VST in place: newslow.so");
        Watch watch;
        bool clicked = false, shotSkip = false;
        watch.scanShot = "splash_scan_new.png";
        watch.each = [&](SplashScreen* s) {
            if (!clicked && s->skipButton()->isVisible() && s->detail().contains("newslow.so")) {
                if (!shotSkip) { uitest::shot(s, gOut + "/splash_skip.png"); shotSkip = true; }
                clicked = true;
                s->skipButton()->click();
            }
        };
        watch.start();
        int catalogNews = 0;
        const int id = AppState::get().addListener([&](int c) { if (c & kCatalog) ++catalogNews; });
        StartupOptions o;
        o.probeExe = cli.toStdString();
        o.offerRecovery = false;
        o.makeWindow = reuseWindow;
        MainWindow* w = runStartup(o);
        check(clicked, "new plug-in: Skip is offered while the new file is checked");
        check(w && w->isVisible() && !SplashScreen::current(), "new plug-in: Skip goes on to the main window");
        check(ScanTask::get().state().running, "new plug-in: the scan goes on in the background");
        check(findAudio("New Slow").path.empty(), "new plug-in: not listed yet");
        QElapsedTimer t;
        t.start();
        while (findAudio("New Slow").path.empty() && t.elapsed() < 60000) uitest::settle(50);
        uitest::settle(400);
        check(!findAudio("New Slow").path.empty(), "new plug-in: listed when the background scan is done");
        check(catalogNews >= 1, "new plug-in: the lists are told");
        AppState::get().removeListener(id);
    }

    // ---- 6. Options > Look for New Plug-Ins: Run in Background
    {
        QFile::remove(vstHome + "/newslow.so");
        check(copyTo(vstDir + "/slow2.so", vstHome + "/newslow.so"), "VST changed: newslow.so");
        // reno_cli next to this program, where runPluginScan looks
        const QString here = QString::fromStdString(defaultProbeExe());
        const bool placed = QFileInfo(here).canonicalFilePath() == QFileInfo(cli).canonicalFilePath() || copyTo(cli, here);
        check(placed, "reno_cli next to the test program");
        bool sent = false;
        auto* t = new QTimer(qApp);
        QObject::connect(t, &QTimer::timeout, [&, t] {
            QWidget* m = QApplication::activeModalWidget();
            if (!m || m->objectName() != "scanDialog") return;
            if (auto* b = m->findChild<QPushButton*>("scanBackground")) {
                uitest::shot(m, gOut + "/scan_dialog.png");
                sent = true;
                b->click();
                t->stop();
                t->deleteLater();
            }
        });
        t->start(100);
        runPluginScan(gWin, false);
        check(sent, "Look for New Plug-Ins: Run in Background closes the window");
        QElapsedTimer c;
        c.start();
        while (ScanTask::get().state().running && c.elapsed() < 60000) uitest::settle(50);
        uitest::settle(400);
        check(!ScanTask::get().state().running && !ScanTask::get().state().cancelled, "Look for New Plug-Ins: the scan finished in the background");
        if (QFileInfo(here).canonicalFilePath() != QFileInfo(cli).canonicalFilePath()) QFile::remove(here);
    }

    delete gWin;
    gWin = nullptr;
    return uitest::summary();
}
