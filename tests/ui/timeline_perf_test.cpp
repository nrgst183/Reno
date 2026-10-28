// How fast the timeline draws itself with big projects: hundreds or thousands of tiny events on many tracks at 140/165 fps, on a
// 165 Hz screen. The timeline is put in a window of its own and worked as a user does, frame by frame, timing every paint and noting
// what each one covered (a "full" repaint is one of nearly the whole widget):
//   paint    whole repaints (QWidget::repaint), zoomed out to the whole project and zoomed in to a few seconds
//   scroll   the horizontal scroll bar moved a tenth of the view per frame (and the vertical one)
//   zoom     the wheel turned in and out over the events
//   cursor   the cursor moved a frame at a time, as playing moves it (without playing)
//   play     real playback for a while, page by page and with "smooth" scrolling (the view follows the cursor)
//   hover    the mouse moved along a track
//   select   events clicked one after another
//   drag     an event dragged along its track
//   resize   the window made wider and narrower
// in both looks (Reno's and VEGAS's), on a made-up stutter project (always) and on real projects when they are on this machine (with
// stand-in media, as load_lag_test does).
//
//   timeline_perf_test <output folder> <perf-media folder> [--samples <folder>] [--bench] [--shots <folder>]
//   (the sample VEGAS projects default to the folder named by $RENO_SAMPLES; without them those parts SKIP)
//                      [--looks reno,vegas] [--quick] [--only measure,...] [--size WxH] [project.veg names...]
// Without --bench the checks are made (thresholds generous enough for a sanitizer build with other tests running): moving the cursor,
// hovering and selecting repaint only small parts, and a whole repaint of the made-up project stays under a limit. --bench prints
// "BENCH <project> <look> <measure> <value> <unit>" lines. --shots saves pictures of fixed states (to compare two builds pixel by
// pixel; with --flat the projects used as media are played by a stand-in clip, so their pictures don't depend on when their own
// effects got made).
#include "../../src/app/timeline.h"
#include "../../src/app/appstate.h"
#include "../../src/app/playback.h"
#include "../../src/app/theme.h"
#include "../../src/vegas/vegas.h"
#include "uitestlib.h"
#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPaintEvent>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <ctime>

using namespace reno;
using namespace reno::app;
using uitest::check;

static double nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

// The timeline with its paints timed and measured.
class TimedTimeline : public TimelineWidget {
public:
    using TimelineWidget::TimelineWidget;
    struct Stats {
        int paints = 0, full = 0;
        double ms = 0, maxMs = 0;
        double area = 0;            // what was painted, in widgets' worth
        void clear() { *this = Stats(); }
    } st;

protected:
    void paintEvent(QPaintEvent* e) override
    {
  // the UI thread's own time: other programs on the machine do not count
        const double t0 = cpuMs();
        TimelineWidget::paintEvent(e);
        const double ms = cpuMs() - t0;
        ++st.paints;
        st.ms += ms;
        st.maxMs = std::max(st.maxMs, ms);
        double a = 0;
        for (const QRect& r : e->region()) a += (double)r.width() * r.height();
        a /= std::max(1.0, (double)width() * height());
        st.area += a;
        if (a > 0.9) ++st.full;
    }
};

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

static void makeStandinFolder(const QString& project, const QString& samplesRoot, const QString& media, const QString& dest)
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
}

// ---------------------------------------------------------------- a made-up stutter project
// 10 tracks (4 video, 6 sound) of 2- and 3-frame events at 165 fps over a minute, with fades, cross-fades, groups,
// muted events and an envelope or two: about 14000 events.
static void buildStutterProject()
{
    AppState& app = AppState::get();
    app.newProject(true);
    std::string err;
    const uint64_t v1 = app.importMedia("synth:1", &err), v2 = app.importMedia("synth:2", &err);
    const uint64_t a1 = app.importMedia("synthaudio:1", &err), a2 = app.importMedia("synthaudio:3", &err);
    app.edit("Stutter", [&](Project& p) {
        p.settings.fpsNum = 165;
        p.settings.fpsDen = 1;
        p.tracks.clear();
        const double fr = 1.0 / 165.0;
        for (int t = 0; t < 10; ++t) {
            Track tr;
            tr.id = p.newId();
            tr.type = t < 4 ? TrackType::Video : TrackType::Audio;
            tr.name = strfmt("%s %d", t < 4 ? "Stutter" : "Sound", t + 1);
            double at = 0;
            int k = 0;
            while (at < 60.0) {
                Event e;
                e.id = p.newId();
                e.mediaId = tr.type == TrackType::Video ? (k % 3 ? v1 : v2) : (k % 3 ? a1 : a2);
                const int frames = 2 + (k % 5 == 0 ? 1 : 0) + (k % 37 == 0 ? 40 : 0);
                e.start = at;
                e.length = frames * fr;
                e.offset = std::fmod(k * 0.37, 8.0);
                if (k % 7 == 0) e.fadeIn = fr;
                if (k % 11 == 0) e.fadeOut = fr;
                if (k % 13 == 0) e.mute = true;
                if (k % 17 == 0) e.rate = 1.5;
                if (k % 37 == 0) { e.fadeIn = 8 * fr; e.fadeOut = 8 * fr; e.fadeInCurve = 1; }
                e.name = strfmt("cut %d", k);
                // an overlap now and then: a cross-fade
                at += e.length - (k % 9 == 0 ? fr : 0.0);
                tr.events.push_back(e);
                ++k;
            }
            p.tracks.push_back(tr);
        }
    });
    app.setCursor(5.0);
}

// ---------------------------------------------------------------- measuring
struct Measure {
    std::string name;
    int frames = 0;
    double wallMs = 0;          // everything the frames took (the change and its paint), by the clock
    double cpuMs = 0;  // and by the UI thread's own time
    TimedTimeline::Stats st;
};

static TimedTimeline* gTl = nullptr;
static QStringList gOnly;       // --only: just the measures whose names start with one of these (for profiling)
static bool wanted(const std::string& name)
{
    if (gOnly.isEmpty()) return true;
    for (const QString& o : gOnly)
        if (QString::fromStdString(name).startsWith(o)) return true;
    return false;
}
static QWidget* gWin = nullptr;

// Runs 'step' 'n' times, each followed by the paint it caused (as the event loop does it).
static Measure frames(const std::string& name, int n, const std::function<void(int)>& step)
{
    if (!wanted(name)) n = 0;
    QCoreApplication::processEvents();
    gTl->st.clear();
    Measure m;
    m.name = name;
    const double t0 = nowMs(), c0 = cpuMs();
    for (int k = 0; k < n; ++k) {
        step(k);
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
    m.wallMs = nowMs() - t0;
    m.cpuMs = cpuMs() - c0;
    m.frames = n;
    m.st = gTl->st;
    return m;
}

static bool gBench = false;
static std::vector<std::string> gStale;     // where what is on the screen was not what a whole repaint shows
static QString gOut;                        // the output folder

// Whether what the window shows now (the backing store, after all the partial repaints and moved pixels) is what a whole repaint
// of the timeline draws: the number of pixels that differ by more than a shade (-1: cannot tell). A line painted under a smaller
// clip can come out a pixel different where it crosses the clip's edge (Qt clips a line before it rasterises it), so something left
// behind is a patch of differences: 12 or more pixels in an 8 x 8 block. Those are counted in gStale.
static int staleAfter(const std::string& what)
{
    uitest::settle(30);
    QBackingStore* bs = gWin->backingStore();
    QPaintDevice* pd = bs ? bs->paintDevice() : nullptr;
    if (!pd || pd->devType() != QInternal::Image) return -1;
    const QImage& screen = *static_cast<QImage*>(pd);
    const QImage fresh = gTl->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal dpr = fresh.devicePixelRatio();
    const QPoint at = gTl->mapTo(gWin, QPoint(0, 0)) * dpr;
    const QImage shown = screen.copy(QRect(at, fresh.size())).convertToFormat(QImage::Format_ARGB32);
    if (shown.size() != fresh.size()) return -1;
    int bad = 0;
    QRect where;
    const int bw = (fresh.width() + 7) / 8, bh = (fresh.height() + 7) / 8;
    std::vector<int> blocks((size_t)bw * bh, 0);
    for (int y = 0; y < fresh.height(); ++y) {
        const QRgb* a = (const QRgb*)fresh.constScanLine(y);
        const QRgb* b = (const QRgb*)shown.constScanLine(y);
        for (int x = 0; x < fresh.width(); ++x)
            if (std::abs(qRed(a[x]) - qRed(b[x])) > 40 || std::abs(qGreen(a[x]) - qGreen(b[x])) > 40 || std::abs(qBlue(a[x]) - qBlue(b[x])) > 40) {
                ++bad;
                where |= QRect(x, y, 1, 1);
                ++blocks[(size_t)(y / 8) * bw + x / 8];
            }
    }
    int patches = 0;
    for (int n : blocks)
        if (n >= 12) ++patches;
    if (bad > 0)
        std::printf("STALE %s: %d pixels at %d,%d %dx%d, %d patches\n", what.c_str(), bad, where.x(), where.y(), where.width(), where.height(), patches);
    if (bad > 0) {
        if (patches > 0) gStale.push_back(what + strfmt(" (%d patches)", patches));
        static int saved = 0;
        if (saved++ < 12) {
            fresh.save(gOut + QString::fromStdString(strfmt("/stale_%d_whole.png", saved)));
            shown.save(gOut + QString::fromStdString(strfmt("/stale_%d_shown.png", saved)));
            std::printf("STALE pictures: stale_%d_whole.png, stale_%d_shown.png\n", saved, saved);
        }
    }
    return bad;
}
static std::string gProject, gLook;
static std::map<std::string, Measure> gResults;     // "<project>|<look>|<name>"

static void report(const Measure& m)
{
    if (!wanted(m.name)) return;
    // and whether the screen shows what it should after those frames
    if (m.name.compare(0, 6, "paint_") != 0) {
        staleAfter(gProject + " " + gLook + " " + m.name);
    }
    gResults[gProject + "|" + gLook + "|" + m.name] = m;
    // fps: what the UI thread's time per frame would allow
    const double perFrame = m.frames ? m.cpuMs / m.frames : 0;
    const double paintPer = m.st.paints ? m.st.ms / m.st.paints : 0;
    std::printf("BENCH %-24s %-5s %-12s frame %8.2f ms  fps %7.1f  paints %4d  paint %8.2f ms (max %7.2f)  area/frame %5.3f  full %3d  wall/frame %8.2f ms\n",
                gProject.substr(0, 24).c_str(), gLook.c_str(), m.name.c_str(), perFrame, perFrame > 0 ? 1000.0 / perFrame : 0.0, m.st.paints, paintPer,
                m.st.maxMs, m.frames ? m.st.area / m.frames : 0.0, m.st.full, m.frames ? m.wallMs / m.frames : 0.0);
    std::fflush(stdout);
}

// a function of its own, for profiling just these: valgrind --tool=callgrind --toggle-collect=repaintTimes*
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void repaintTimes(int n)
{
    for (int k = 0; k < n; ++k) gTl->repaint();
}

// Whole repaints, n of them.
static Measure wholePaints(const std::string& name, int n)
{
    gTl->st.clear();
    Measure m;
    m.name = name;
    if (!wanted(name)) n = 0;
    const double t0 = nowMs(), c0 = cpuMs();
    repaintTimes(n);
    m.wallMs = nowMs() - t0;
    m.cpuMs = cpuMs() - c0;
    m.frames = n;
    m.st = gTl->st;
    return m;
}

// Waits until the timeline has looked the same for 2.5 seconds (pictures and waveforms all in), at most 'ms'.
static QImage settled(int ms)
{
    QImage last;
    int same = 0;
    const double t0 = nowMs();
    for (;;) {
        uitest::settle(500);
        QImage now = gTl->grab().toImage();
        same = now == last ? same + 1 : 0;
        if (same >= 5 || nowMs() - t0 > ms) return now;
        last = now;
    }
}

// A place on event 'e' (on track rectangle 'tr') to click or take hold of it by its body: not on an edge, a fade handle or one of
// its buttons ((-1,-1): none).
static QPoint bodyPoint(const Event& e, const QRect& tr)
{
    const int x0 = gTl->xAt(e.start), x1 = gTl->xAt(e.end());
    const QRect buttons = gTl->eventMenuRect(e.id) | gTl->eventButtonRect(e.id, false) | gTl->eventButtonRect(e.id, true);
    for (int y : {tr.top() + tr.height() / 2, tr.top() + 14, tr.bottom() - 4})
        for (double f : {0.5, 0.3, 0.2, 0.7}) {
            const QPoint pt((int)std::lround(x0 + (x1 - x0) * f), y);
            if (pt.x() < x0 + 6 || pt.x() > x1 - 6 || pt.y() < tr.top() + 13 || pt.y() > tr.bottom() - 2 || buttons.contains(pt)) continue;
            return pt;
        }
    return QPoint(-1, -1);
}

// Events on visible tracks, in the view, at least 'minPx' wide: where to take hold of them (at most 'n').
static std::vector<QPoint> eventsToClick(double minPx, size_t n)
{
    std::vector<QPoint> out;
    const Project& p = AppState::get().project();
    for (const Track& t : p.tracks) {
        const QRect tr = gTl->trackRect(t.id);
        if (tr.isEmpty() || tr.top() < 60 || tr.bottom() > gTl->height() - 30) continue;
        for (const Event& e : t.events) {
            const int x0 = gTl->xAt(e.start), x1 = gTl->xAt(e.end());
            if (x1 - x0 < minPx || x0 < gTl->headerWidth() + 40 || x1 > gTl->width() - 120 || e.locked) continue;
            const QPoint pt = bodyPoint(e, tr);
            if (pt.x() < 0) continue;
            out.push_back(pt);
            if (out.size() >= n) return out;
            break;  // one a track, then the next track
        }
    }
    return out;
}

static void setLook(UiTheme t)
{
    uiThemeSetting() = t;
    AppState::get().notify(kPrefs);
    uitest::settle(50);
}

// Zoomed so that 'seconds' of the project fill the view, from 'from'.
static void zoomTo(double from, double seconds)
{
    gTl->zoomToFit();
    const double fitSeconds = gTl->viewEndTime() - gTl->viewStartTime();
    // (zoomIn is 1.5 times; close enough)
    while (gTl->viewEndTime() - gTl->viewStartTime() > seconds * 1.5) gTl->zoomIn();
    (void)fitSeconds;
    QScrollBar* h = gTl->findChild<QScrollBar*>("timelineHScroll");
    const double pps = gTl->viewEndTime() > gTl->viewStartTime() ? (gTl->width() - gTl->headerWidth() - 20) / (gTl->viewEndTime() - gTl->viewStartTime()) : 1;
    if (h) h->setValue((int)std::lround(from * pps));
    uitest::settle(20);
}

static void session(Playback& pb, const QString& shots, bool quick)
{
    AppState& app = AppState::get();
    QScrollBar* hbar = gTl->findChild<QScrollBar*>("timelineHScroll");
    QScrollBar* vbar = gTl->findChild<QScrollBar*>("timelineVScroll");
    const double len = std::max(1.0, app.project().length());
    const int nScroll = quick ? 20 : 60;
    auto shot = [&](const char* what) {
        if (shots.isEmpty()) return;
        const QImage img = settled(20000);
        QString n = QString::fromStdString(gProject + "_" + gLook + "_" + what);
        n.replace(' ', '_');
        img.save(shots + "/" + n + ".png");
    };

    for (int level = 0; level < 2; ++level) {
        const std::string lv = level == 0 ? "out" : "in";
        app.setSelection({});
        if (level == 0) gTl->zoomToFit();
        else zoomTo(len * 0.37, 4.0);
        app.setCursor(gTl->viewStartTime() + (gTl->viewEndTime() - gTl->viewStartTime()) * 0.3);
        if (vbar) vbar->setValue(0);
        settled(quick ? 1500 : 6000);
        shot(lv.c_str());
        report(wholePaints("paint_" + lv, quick ? 5 : 15));

        // scrolling: a tenth of the view a frame (less where there is less to scroll through), to and fro between the
        // scroll bar's ends; then down and up through the tracks a row at a time
        auto toAndFro = [](QScrollBar* bar, int step) {
            return [bar, step, dir = 1](int) mutable {
                if (!bar || bar->maximum() <= bar->minimum()) return;
                const int s = std::max(1, std::min(step, (bar->maximum() - bar->minimum()) / 6));
                int next = bar->value() + dir * s;
                if (next > bar->maximum() || next < bar->minimum()) {
                    dir = -dir;
                    next = bar->value() + dir * s;
                }
                bar->setValue(next);
            };
        };
        const int h0 = hbar ? hbar->value() : 0;
        report(frames("scroll_" + lv, nScroll, toAndFro(hbar, std::max(1, (gTl->width() - gTl->headerWidth()) / 10))));
        if (hbar) hbar->setValue(h0);
        report(frames("vscroll_" + lv, nScroll / 2, toAndFro(vbar, 24)));
        if (vbar) vbar->setValue(0);

        // the wheel: in and out again over the events
        const QPoint mid(gTl->headerWidth() + (gTl->width() - gTl->headerWidth()) / 2, gTl->height() / 2);
        report(frames("zoom_" + lv, 20, [&](int k) { uitest::wheel(gTl, mid, k < 10 ? 120 : -120); }));
        if (level == 0) gTl->zoomToFit();
        else zoomTo(len * 0.37, 4.0);
        uitest::settle(30);

        // the cursor moved on a frame of a 165 Hz screen at a time (not playing: no scrolling)
        const double c0 = gTl->viewStartTime() + (gTl->viewEndTime() - gTl->viewStartTime()) * 0.3;
        report(frames("cursor_" + lv, nScroll, [&](int k) { app.setCursor(c0 + k / 165.0); }));

        // the mouse along the middle of the tracks
        report(frames("hover_" + lv, nScroll, [&](int k) {
            uitest::mouseMove(gTl, QPoint(gTl->headerWidth() + 30 + k * 9, gTl->height() / 2 + (k % 3) * 5), Qt::NoButton);
        }));

        // clicking events one after another
        const std::vector<QPoint> clicks = eventsToClick(14, 12);
        if (!clicks.empty()) {
            report(frames("select_" + lv, (int)clicks.size(), [&](int k) { uitest::mouseClick(gTl, clicks[(size_t)k]); }));
            if (level == 1) shot("selected");
        }
        app.setSelection({});
        uitest::settle(20);

        // an event dragged along its track (and the move undone)
        const std::vector<QPoint> grabs = eventsToClick(level == 0 ? 14 : 30, 1);
        const QPoint g = grabs.empty() ? QPoint(-1, -1) : grabs[0];
        if (g.x() >= 0) {
            const uint64_t rev = app.revision();
            uitest::mousePress(gTl, g);
            Measure dm = frames("drag_" + lv, 30, [&](int k) { uitest::mouseMove(gTl, g + QPoint(4 + k * 3, 0), Qt::LeftButton); });
            report(dm);
            uitest::mouseRelease(gTl, g + QPoint(4 + 29 * 3, 0));
            uitest::settle(20);
            if (app.revision() != rev) app.undo();
            app.setSelection({});
        }
    }

    // resizing the window
    {
        gTl->zoomToFit();
        uitest::settle(50);
        const QSize s0 = gWin->size();
        report(frames("resize", quick ? 6 : 16, [&](int k) { gWin->resize(s0.width() - 40 * ((k % 4) + 1), s0.height() - 10 * (k % 3)); }));
        gWin->resize(s0);
        uitest::settle(50);
    }

    // playing: real playback, the view turning pages, then following the cursor smoothly
    for (const char* mode : {"page", "smooth"}) {
        if (!wanted(std::string("play_") + mode)) continue;
        app.prefs.scrollMode = mode;
        zoomTo(len * 0.2, 4.0);
        // from near the right edge so the page turns soon; from just before the middle so the view follows
        const double span = gTl->viewEndTime() - gTl->viewStartTime();
        app.setCursor(gTl->viewStartTime() + span * (std::string(mode) == "page" ? 0.85 : 0.45));
        uitest::settle(100);
        gTl->st.clear();
        const double t0 = nowMs(), c0 = cpuMs();
        pb.play();
        uitest::settle(quick ? 1200 : 3000);
        pb.pause();
        Measure m;
        m.name = std::string("play_") + mode;
        m.wallMs = nowMs() - t0;
        m.st = gTl->st;
        // a frame here is a paint: playing moves the cursor some 66 times a second
        m.frames = std::max(1, m.st.paints);
        m.cpuMs = m.st.ms;      // fps reported is what painting alone would allow
        report(m);
        (void)c0;
        uitest::settle(100);
    }
    app.prefs.scrollMode = "page";
    app.setCursor(0);
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTimelinePerfTest");
    QApplication::setApplicationName("timeline_perf_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: timeline_perf_test <output folder> <perf-media folder> [--samples dir] [--bench] [--shots dir] [--looks reno,vegas] [--quick] [--only measures] [--size WxH] [--no-synthetic] [--flat] [names...]\n");
        return 2;
    }
    const QString out = QDir(args[1]).absolutePath();
    gOut = out;
    const QString media = QDir(args[2]).absolutePath();
    QString samples = qEnvironmentVariable("RENO_SAMPLES"), shots;
    QStringList names, looks{"reno", "vegas"};
    bool quick = false, synthetic = true;
    bool flat = false;  // --flat: projects used as media are played by a stand-in clip too (pictures that don't depend on timing)
    QSize winSize(1600, 820);
    for (int k = 3; k < args.size(); ++k) {
        if (args[k] == "--samples" && k + 1 < args.size()) samples = args[++k];
        else if (args[k] == "--bench") gBench = true;
        else if (args[k] == "--shots" && k + 1 < args.size()) shots = QDir(args[++k]).absolutePath();
        else if (args[k] == "--looks" && k + 1 < args.size()) looks = args[++k].split(',');
        else if (args[k] == "--quick") quick = true;
        else if (args[k] == "--only" && k + 1 < args.size()) gOnly = args[++k].split(',');
        else if (args[k] == "--size" && k + 1 < args.size()) {
            const QStringList wh = args[++k].split('x');
            if (wh.size() == 2) winSize = QSize(wh[0].toInt(), wh[1].toInt());
        }
        else if (args[k] == "--no-synthetic") synthetic = false;
        else if (args[k] == "--flat") flat = true;
        else names << args[k];
    }
    if (names.isEmpty() && !gBench) names << QDir(samples).entryList(QStringList() << "*.veg", QDir::Files, QDir::Name).mid(0, 1);     // the first sample project
    QDir().mkpath(out);
    if (!shots.isEmpty()) QDir().mkpath(shots);
    uitest::setup({}, {});
    // the media: made-up clips for the made-up project; for the samples, real files (the stand-ins) found by name
    const auto synthOpener = AppState::get().mediaOpener;
    AppState::get().mediaOpener = [synthOpener, flat, media](const std::string& path, std::string* error) -> std::unique_ptr<MediaReader> {
        if (path.compare(0, 5, "synth") == 0) return synthOpener(path, error);
        if (flat && endsWithNoCase(path, ".veg")) return openMedia((media + "/clip720.mp4").toStdString(), error);
        return openMediaOrProject(path, error);
    };
    setProjectFileLoader([](const std::string& path, Project& o, std::string* error) { return loadProjectFile(path, o, error); });

    // a menu opened by a click would wait for an answer that never comes: closed straight away
    QTimer popupCloser;
    QObject::connect(&popupCloser, &QTimer::timeout, [] {
        if (QWidget* w = QApplication::activePopupWidget()) w->close();
        if (QWidget* w = QApplication::activeModalWidget()) w->close();
    });
    popupCloser.start(300);

    Playback pb;
    QWidget win;
    QVBoxLayout* lay = new QVBoxLayout(&win);
    lay->setContentsMargins(0, 0, 0, 0);
    TimedTimeline* tl = new TimedTimeline(&pb, &win);
    lay->addWidget(tl);
    gTl = tl;
    gWin = &win;
    win.resize(winSize);
    win.show();
    uitest::settle(200);

    std::vector<std::pair<std::string, QString>> projects;     // (name, path; empty: the made-up one)
    if (synthetic) projects.push_back({"synthetic stutter", QString()});
    const bool haveSamples = QFileInfo(samples).isDir() && QFileInfo(media + "/clip720.mp4").exists();
    if (!haveSamples && !names.isEmpty()) std::printf("SKIP: no sample projects in %s or no stand-in clips in %s\n", qPrintable(samples), qPrintable(media));
    if (haveSamples)
        for (const QString& name : names) {
            QString orig;
            QDirIterator it(samples, QStringList() << name, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext()) orig = it.next();
            if (orig.isEmpty()) { check(false, "found " + name.toStdString()); continue; }
            const QString folder = out + "/" + QFileInfo(orig).absoluteDir().dirName();
            makeStandinFolder(orig, samples, media, folder);
            projects.push_back({name.toStdString(), folder + "/" + name});
        }

    for (const auto& pr : projects) {
        gProject = pr.first;
        AppState& app = AppState::get();
        if (pr.second.isEmpty()) buildStutterProject();
        else {
            std::string err;
            if (!app.open(pr.second.toStdString(), &err)) { check(false, "opened " + pr.first, err); continue; }
        }
        size_t events = 0;
        for (const Track& t : app.project().tracks) events += t.events.size();
        std::printf("PROJECT %s: %zu tracks, %zu events, %.1f s\n", gProject.c_str(), app.project().tracks.size(), events, app.project().length());
        for (const QString& look : looks) {
            gLook = look.toStdString();
            setLook(look == "vegas" ? UiTheme::Vegas : UiTheme::Reno);
            session(pb, shots, quick);
        }
    }
    setLook(UiTheme::Reno);
    AppState::get().newProject(true);
    uitest::settle(100);
    if (gBench) return 0;

    // ---- the checks (on the made-up project; generous for sanitizer builds)
    for (const char* look : {"reno", "vegas"}) {
        auto get = [&](const std::string& n) -> const Measure* {
            auto it = gResults.find(std::string("synthetic stutter|") + look + "|" + n);
            return it == gResults.end() ? nullptr : &it->second;
        };
        const std::string L = std::string(" (") + look + ")";
        for (const char* lv : {"out", "in"}) {
            const std::string l = std::string(" zoomed ") + lv + L;
            if (const Measure* m = get(std::string("paint_") + lv))
                check(m->st.ms / std::max(1, m->st.paints) < 250.0, "a whole repaint takes under 250 ms" + l, strfmt("%.1f ms", m->st.ms / std::max(1, m->st.paints)));
            if (const Measure* m = get(std::string("cursor_") + lv)) {
                check(m->st.full == 0, "moving the cursor never repaints everything" + l, strfmt("%d full", m->st.full));
                check(m->st.area / std::max(1, m->frames) < 0.06, "moving the cursor repaints thin strips only" + l, strfmt("%.3f of the widget a frame", m->st.area / std::max(1, m->frames)));
            }
            if (const Measure* m = get(std::string("hover_") + lv))
                check(m->st.full == 0 && m->st.area / std::max(1, m->frames) < 0.15, "hovering repaints only what it lights up" + l,
                      strfmt("%d full, %.3f a frame", m->st.full, m->st.area / std::max(1, m->frames)));
            if (const Measure* m = get(std::string("select_") + lv))
                check(m->st.full == 0, "selecting an event repaints only the events that changed" + l, strfmt("%d full of %d", m->st.full, m->frames));
            if (const Measure* m = get(std::string("drag_") + lv))
                check(m->st.full <= 1, "dragging an event repaints only where it was and is" + l, strfmt("%d full of %d", m->st.full, m->frames));
            // with the screen scaled by a fraction, Qt moves pixels across only by whole device pixels: a scroll bar's step of an odd number
            // of pixels at 150% redraws everything
            const double dpr = gTl->devicePixelRatioF();
            if (const Measure* m = get(std::string("scroll_") + lv); m && dpr == std::floor(dpr))
                check(m->st.area / std::max(1, m->frames) < 0.6, "scrolling repaints only what comes into view" + l, strfmt("%.3f a frame", m->st.area / std::max(1, m->frames)));
        }
        if (const Measure* m = get("play_smooth"))
            check(m->st.full == 0 && m->st.area / std::max(1, m->frames) < 0.4, "playing with the view following the cursor moves what is drawn across" + L,
                  strfmt("%d full, %.3f a frame", m->st.full, m->st.area / std::max(1, m->frames)));
        if (const Measure* m = get("play_page"))
            check(m->st.area / std::max(1, m->frames) < 0.1, "playing repaints thin strips (and a page now and then)" + L, strfmt("%.3f a frame", m->st.area / std::max(1, m->frames)));
    }
    std::string stale;
    for (const std::string& s : gStale) stale += (stale.empty() ? "" : "; ") + s;
    check(gStale.empty(), "screen matches a whole repaint after scrolling, zooming, playing, hovering, selecting and dragging", stale);
    return uitest::summary();
}
