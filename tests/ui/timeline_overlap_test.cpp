// Pictures of the timeline where events are drawn over one another (a stutter edit after Trim Start to Cursor, stacks of
// selected and unselected events, cross-fades, muted events), at several zooms and track heights, with VEGAS's light event
// background (Preferences > Display "Reverse Event Background Color") and without, and with a time selection: to show that a
// change to how the timeline draws leaves every pixel as it was. Also times a whole repaint of the stutter edit after Trim Start.
//
//   timeline_overlap_test <output folder> [--save <folder>] [--compare <folder>] [--bench | --bench-only]
//
// --save writes one PNG a picture; --compare reads the PNGs another build saved and checks each picture is the same, pixel
// for pixel (QImage ==). --bench prints "BENCH <what> <ms>" lines: the paint's own time (the UI thread's CPU time), the best
// and the mean of several whole repaints.
#include "../../src/app/timeline.h"
#include "../../src/app/appstate.h"
#include "../../src/app/playback.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <set>

using namespace reno;
using namespace reno::app;
using uitest::check;

static double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static TimelineWidget* gTl = nullptr;

// ---------------------------------------------------------------- the projects
// The stutter edit of timeline_perf_test: 10 tracks (4 video, 6 sound) of 2- and 3-frame events at 165 fps over a minute,
// with fades, cross-fades, muted and stretched events.
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
                at += e.length - (k % 9 == 0 ? fr : 0.0);
                tr.events.push_back(e);
                ++k;
            }
            p.tracks.push_back(tr);
        }
    });
    app.setCursor(5.0);
}

// Two video and two sound tracks of wide events drawn over one another in every way: stacks starting together (each one longer
// than the last, or shorter), a long event with short ones over it, cross-fades, muted events over and under the others,
// stretched, faded, quieter, locked and grouped events.
static void buildOverlapProject()
{
    AppState& app = AppState::get();
    app.newProject(true);
    std::string err;
    const uint64_t v1 = app.importMedia("synth:1", &err), v2 = app.importMedia("synth:2", &err);
    const uint64_t a1 = app.importMedia("synthaudio:1", &err), a2 = app.importMedia("synthaudio:3", &err);
    app.edit("Overlaps", [&](Project& p) {
        p.tracks.clear();
        for (int t = 0; t < 4; ++t) {
            Track tr;
            tr.id = p.newId();
            tr.type = t < 2 ? TrackType::Video : TrackType::Audio;
            tr.name = strfmt("Track %d", t + 1);
            const bool video = t < 2;
            int k = 0;
            auto add = [&](double start, double length, const std::function<void(Event&)>& more = {}) {
                Event e;
                e.id = p.newId();
                e.mediaId = video ? (k % 2 ? v1 : v2) : (k % 2 ? a1 : a2);
                e.start = start;
                e.length = length;
                e.offset = std::fmod(k * 0.53 + t * 0.21, 4.0);
                e.name = strfmt("ev %d.%d", t, k);
                if (more) more(e);
                tr.events.push_back(e);
                ++k;
            };
            // a stack starting together, each longer than the last
            for (int i = 0; i < 6; ++i) add(0.5, 0.6 + 0.25 * i, [&](Event& e) { if (i == 2) e.fadeIn = 0.2; if (i == 4) e.mute = true; });
            // a stack starting together, each shorter than the last
            for (int i = 0; i < 6; ++i) add(3.0, 2.0 - 0.25 * i, [&](Event& e) { if (i == 1) e.fadeOut = 0.3; if (i == 3) e.rate = 1.5; });
            // a long event and short ones over it
            add(6.0, 3.0, [&](Event& e) { e.gain = 0.5; e.fadeIn = 0.4; e.fadeInCurve = 1; });
            for (int i = 0; i < 5; ++i) add(6.2 + 0.5 * i, 0.3 + 0.05 * i, [&](Event& e) { if (i == 2) e.locked = true; if (i == 3) e.group = 99; });
            // cross-fades: each event over the end of the one before
            for (int i = 0; i < 5; ++i)
                add(9.5 + 0.7 * i, 1.0, [&](Event& e) {
                    if (i == 1) e.fadeOutCurve = 2;
                    if (i == 2) e.fadeInCurve = 3;
                    if (i == 3 && video) { e.transition.kind = "ofx"; e.transition.name = "Cross Dissolve"; }
                });
            // a muted event with others over and under it; then many starting together over a muted one
            add(14.0, 1.5, [&](Event& e) { e.mute = true; });
            add(14.3, 0.6);
            add(13.8, 0.4);
            for (int i = 0; i < 20; ++i) add(16.0, 0.4 + 0.07 * i, [&](Event& e) { if (i == 0) e.mute = true; if (i % 5 == 3) e.fadeIn = 0.1; });
            std::stable_sort(tr.events.begin(), tr.events.end(), [](const Event& a, const Event& b) { return a.start < b.start; });
            p.tracks.push_back(tr);
        }
    });
    app.setCursor(2.0);
}

// ---------------------------------------------------------------- the pictures
// Waits until the timeline has looked the same for a while (pictures and waveforms all in), at most 'ms'.
static QImage settled(int ms)
{
    QImage last;
    int same = 0;
    for (int waited = 0;; waited += 250) {
        uitest::settle(250);
        QImage now = gTl->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        same = now == last ? same + 1 : 0;
        if (same >= 3 || waited > ms) return now;
        last = now;
    }
}

static QString gSave, gCompare;
static int gSame = 0, gDiffer = 0, gMissing = 0;

static void picture(const std::string& name, const QImage& img)
{
    if (!gSave.isEmpty()) img.save(gSave + "/" + QString::fromStdString(name) + ".png");
    if (gCompare.isEmpty()) return;
    QImage before(gCompare + "/" + QString::fromStdString(name) + ".png");
    if (before.isNull()) {
        ++gMissing;
        check(false, "a picture to compare " + name + " with");
        return;
    }
    before = before.convertToFormat(QImage::Format_ARGB32);
    const bool same = before == img;
    if (same) ++gSame;
    else {
        ++gDiffer;
        int bad = 0;
        QRect where;
        if (before.size() == img.size())
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (before.pixel(x, y) != img.pixel(x, y)) { ++bad; where |= QRect(x, y, 1, 1); }
        std::printf("DIFF %s: %d pixels in %d,%d %dx%d\n", name.c_str(), bad, where.x(), where.y(), where.width(), where.height());
        if (!gSave.isEmpty()) before.save(gSave + "/" + QString::fromStdString(name) + "_before.png");
    }
    check(same, "same pixels: " + name);
}

// The whole timeline, settled; and parts of it painted on their own (a paint's clip smaller than the widget).
static bool gBenchOnly = false;  // --bench-only: no pictures (for profiling: valgrind --tool=callgrind --toggle-collect='bench*')

static void shoot(const std::string& name, bool parts = false)
{
    if (gBenchOnly) return;
    picture(name, settled(15000));
    if (!parts) return;
    const int w = gTl->width(), h = gTl->height();
    const QRect rects[] = {QRect(w / 3, 40, w / 5, h / 2), QRect(gTl->headerWidth() + 7, 0, 93, h), QRect(w / 2 - 1, h / 3, 3, h / 3)};
    int k = 0;
    for (const QRect& r : rects) {
        QImage img(gTl->size(), QImage::Format_ARGB32);
        img.fill(QColor(255, 0, 255));
        gTl->render(&img, QPoint(), QRegion(r));
        picture(name + strfmt("_part%d", k++), img);
    }
}

static void zoomTo(double from, double seconds)
{
    gTl->zoomToFit();
    while (gTl->viewEndTime() - gTl->viewStartTime() > seconds * 1.5) gTl->zoomIn();
    QScrollBar* h = gTl->findChild<QScrollBar*>("timelineHScroll");
    const double pps = gTl->viewEndTime() > gTl->viewStartTime() ? (gTl->width() - gTl->headerWidth() - 20) / (gTl->viewEndTime() - gTl->viewStartTime()) : 1;
    if (h) h->setValue((int)std::lround(from * pps));
    QScrollBar* v = gTl->findChild<QScrollBar*>("timelineVScroll");
    if (v) v->setValue(0);
    uitest::settle(20);
}

static std::set<uint64_t> eventsOf(const std::vector<int>& tracks, int every = 1)
{
    std::set<uint64_t> ids;
    const Project& p = AppState::get().project();
    for (int t : tracks) {
        if (t >= (int)p.tracks.size()) continue;
        int k = 0;
        for (const Event& e : p.tracks[(size_t)t].events)
            if (k++ % every == 0) ids.insert(e.id);
    }
    return ids;
}

static void setHeights(int h)
{
    AppState::get().edit("Heights", [&](Project& p) {
        for (Track& t : p.tracks) t.height = h;
    });
}

// The paint's own time of 'n' whole repaints: the best and the mean.
static void bench(const std::string& what, int n)
{
    double best = 1e9, sum = 0;
    for (int k = 0; k < n; ++k) {
        const double t0 = cpuMs();
        gTl->repaint();
        const double ms = cpuMs() - t0;
        best = std::min(best, ms);
        sum += ms;
    }
    std::printf("BENCH %-40s best %8.2f ms  mean %8.2f ms\n", what.c_str(), best, sum / n);
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTimelineOverlapTest");
    QApplication::setApplicationName("timeline_overlap_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: timeline_overlap_test <output folder> [--save dir] [--compare dir] [--bench]\n");
        return 2;
    }
    QDir().mkpath(args[1]);
    bool doBench = false;
    for (int k = 2; k < args.size(); ++k) {
        if (args[k] == "--save" && k + 1 < args.size()) gSave = QDir(args[++k]).absolutePath();
        else if (args[k] == "--compare" && k + 1 < args.size()) gCompare = QDir(args[++k]).absolutePath();
        else if (args[k] == "--bench") doBench = true;
        else if (args[k] == "--bench-only") doBench = gBenchOnly = true;
    }
    if (!gSave.isEmpty()) QDir().mkpath(gSave);
    uitest::setup({}, {});

    Playback pb;
    QWidget win;
    QVBoxLayout* lay = new QVBoxLayout(&win);
    lay->setContentsMargins(0, 0, 0, 0);
    TimelineWidget* tl = new TimelineWidget(&pb, &win);
    lay->addWidget(tl);
    gTl = tl;
    win.resize(1600, 820);
    win.show();
    uitest::settle(200);
    AppState& app = AppState::get();

    // ---- the stutter edit, its selected events trimmed to the cursor (they start together and lie over one another)
    buildStutterProject();
    const double len = app.project().length();
    struct Zoom { const char* name; double from, seconds; };
    const Zoom zooms[] = {{"fit", 0, 0}, {"4s", len * 0.37, 4.0}, {"1s", len * 0.37, 1.0}, {"quarter", len * 0.37 + 1.0, 0.25}};
    auto zoom = [&](const Zoom& z) {
        if (z.seconds <= 0) {
            gTl->zoomToFit();
            uitest::settle(20);
        } else
            zoomTo(z.from, z.seconds);
        app.setCursor(gTl->viewStartTime() + (gTl->viewEndTime() - gTl->viewStartTime()) * 0.3);
    };
    for (const Zoom& z : zooms) {
        const std::string n = std::string("stutter_") + z.name;
        zoom(z);
        app.setSelection({});
        shoot(n + "_plain");
        app.setSelection(eventsOf({0, 1, 4, 5}));
        shoot(n + "_selected");
        gTl->trimStartToCursor();
        shoot(n + "_trimstart", z.seconds == 4.0);
        if (doBench && z.seconds == 4.0) bench(n + " after Trim Start, 4 tracks", 5);
        app.undo();
        app.setSelection(eventsOf({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 2));
        gTl->trimEndToCursor();
        shoot(n + "_trimend");
        app.undo();
        app.setSelection(eventsOf({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        gTl->trimStartToCursor();
        if (doBench && z.seconds == 4.0) {
            settled(15000);
            bench(n + " after Trim Start, every track", 5);
        }
        vprefs::set("prefs/reverseEventBg", true);
        app.setLoop(gTl->viewStartTime() + 0.2 * (gTl->viewEndTime() - gTl->viewStartTime()),
                    gTl->viewStartTime() + 0.6 * (gTl->viewEndTime() - gTl->viewStartTime()));
        gTl->update();
        shoot(n + "_trimstart_all_reversed_timesel");
        vprefs::set("prefs/reverseEventBg", false);
        app.setLoop(0, 0);
        app.undo();
    }
    // smaller and taller tracks over the trimmed stutter edit
    for (int h : {44, 140}) {
        setHeights(h);
        zoom(zooms[1]);
        app.setSelection(eventsOf({0, 1, 4, 5}));
        gTl->trimStartToCursor();
        shoot(strfmt("stutter_4s_trimstart_height%d", h));
        app.undo();
        app.undo();
    }

    // ---- the hand-made overlaps
    buildOverlapProject();
    struct Look { const char* name; double seconds, from; };
    const Look looks[] = {{"fit", 0, 0}, {"wide", 6.0, 0.3}, {"close", 2.0, 15.6}, {"closer", 0.8, 2.9}};
    auto look = [&](const Look& l) {
        if (l.seconds <= 0) {
            gTl->zoomToFit();
            uitest::settle(20);
        } else
            zoomTo(l.from, l.seconds);
    };
    for (const Look& l : looks) {
        look(l);
        const std::string n = std::string("overlap_") + l.name;
        app.setSelection({});
        shoot(n + "_plain", l.seconds == 6.0);
        app.setSelection(eventsOf({0, 1, 2, 3}, 2));
        shoot(n + "_alternate");
        app.setSelection(eventsOf({0, 1, 2, 3}));
        shoot(n + "_all", l.seconds == 6.0);
        vprefs::set("prefs/reverseEventBg", true);
        gTl->update();
        shoot(n + "_all_reversed");
        app.setSelection(eventsOf({0, 2}, 3));
        app.setLoop(gTl->viewStartTime() + 0.25 * (gTl->viewEndTime() - gTl->viewStartTime()),
                    gTl->viewStartTime() + 0.7 * (gTl->viewEndTime() - gTl->viewStartTime()));
        shoot(n + "_some_reversed_timesel");
        vprefs::set("prefs/reverseEventBg", false);
        gTl->update();
        shoot(n + "_some_timesel");
        app.setLoop(0, 0);
    }
    for (int h : {40, 72, 160}) {
        setHeights(h);
        look(looks[1]);
        app.setSelection(eventsOf({0, 1, 2, 3}, 3));
        shoot(strfmt("overlap_wide_height%d", h), h == 160);
        look(looks[3]);
        shoot(strfmt("overlap_closer_height%d", h));
        app.undo();
    }
    app.setSelection({});
    app.newProject(true);
    uitest::settle(100);
    if (!gCompare.isEmpty()) std::printf("COMPARED %d pictures: %d the same, %d different, %d missing\n", gSame + gDiffer + gMissing, gSame, gDiffer, gMissing);
    return uitest::summary();
}
