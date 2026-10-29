// Regression tests for fixed bugs that need app state or windows; each check names the bug. Build under ASan + UBSan.
//   bugs_test <output folder> [testfx.ofx]
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/app/panels.h"
#include "uitestlib.h"
#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QDockWidget>
#include <QLayout>
#include <QElapsedTimer>
#include <QPainter>
#include <QSettings>
#include <QTreeWidget>
#include <QFile>
#include <algorithm>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

// A listener that adds many listeners while being notified, then uses what it holds; and one that removes itself.
// The listener list used to move while one ran (use after free under ASan).
static void listenersAddedWhileTelling()
{
    AppState& app = st();
    // each lambda holds one pointer, so std::function keeps it inline
    struct Ctx {
        std::vector<int> added;
        int told = 0, seen = 0, self = 0, after = 0, selfId = 0;
        std::string held = "still here";
    };
    static Ctx ctx;
    Ctx* c = &ctx;
    const int first = app.addListener([c](int changes) {
        if (!(changes & kView)) return;
        ++c->told;
        for (int k = 0; k < 200; ++k) c->added.push_back(st().addListener([](int) {}));
        // re-read c after the list has grown
        if (c->held == "still here") ++c->seen;
    });
    app.notify(kView);
    check(c->told == 1 && c->seen == 1, "AppState::notify: a listener that adds listeners keeps running with what it holds",
          strfmt("%d %d", c->told, c->seen));
    for (int id : c->added) app.removeListener(id);
    app.removeListener(first);
    c->selfId = app.addListener([c](int changes) {
        if (!(changes & kView)) return;
        st().removeListener(c->selfId);
        ++c->self;
    });
    const int other = app.addListener([c](int changes) { if (changes & kView) ++c->after; });
    app.notify(kView);
    app.notify(kView);
    check(c->self == 1 && c->after == 2, "AppState::notify: a listener that removes itself finishes, and is not told again",
          strfmt("%d %d", c->self, c->after));
    app.removeListener(other);
}

// Largest per-channel difference over all pixels (-1: sizes differ).
static int maxDiff(const QImage& a0, const QImage& b0)
{
    if (a0.isNull() || a0.size() != b0.size()) return -1;
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32), b = b0.convertToFormat(QImage::Format_ARGB32);
    int m = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* p = (const QRgb*)a.constScanLine(y);
        const QRgb* q = (const QRgb*)b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x)
            m = std::max({m, std::abs(qRed(p[x]) - qRed(q[x])), std::abs(qGreen(p[x]) - qGreen(q[x])), std::abs(qBlue(p[x]) - qBlue(q[x]))});
    }
    return m;
}

// Project Media Replace/Refresh, and a new project reusing the last one's media ids: the preview kept showing the old file.
static void previewFollowsReplacedMedia(MainWindow& w, const QString& out)
{
    AppState& app = st();
    Playback* pb = w.playback();
    // first thing in the dock page, under the tab strip
    QWidget* page = w.findChild<QDockWidget*>("dockMedia")->widget();
    MediaBin* bin = static_cast<MediaBin*>(page->layout()->itemAt(0)->widget());
    app.newProject();
    std::string err;
    const uint64_t one = app.importMedia("synth:1", &err);
    app.addMediaToTimeline(one, 0.0, 0);
    const QImage a = pb->renderNow(1.0);
    check(bin->replaceMedia(one, "synth:2"), "Replace Media: replaced");
    const QImage b = pb->renderNow(1.0);
    check(maxDiff(a, b) > 40, "Replace Media: the preview shows the new file", strfmt("difference %d", maxDiff(a, b)));
    app.undo();
    const QImage c = pb->renderNow(1.0);
    check(maxDiff(a, c) == 0, "Replace Media undone: the preview shows the old file again", strfmt("difference %d", maxDiff(a, c)));

    // new untitled project after an untitled one: first media has the same id as before
    app.newProject();
    const uint64_t three = app.importMedia("synth:3", &err);
    app.addMediaToTimeline(three, 0.0, 0);
    const QImage d = pb->renderNow(1.0);
    check(three == one && maxDiff(a, d) > 40, "a new project: the preview shows its own media, not the last project's of the same id",
          strfmt("ids %llu %llu, difference %d", (unsigned long long)one, (unsigned long long)three, maxDiff(a, d)));

    // Refresh: a picture that was unreadable (offline) and is there now
    QDir().mkpath(out);
    const QString red = out + "/red.png", blue = out + "/blue_later.png";
    QFile::remove(blue);
    QImage pic(64, 36, QImage::Format_RGB32);
    pic.fill(QColor(220, 20, 20));
    pic.save(red);
    app.newProject();
    const uint64_t m = app.importMedia(red.toStdString(), &err);
    check(m != 0, "Refresh: a picture is imported", err);
    app.addMediaToTimeline(m, 0.0, 0);
    const QImage e = pb->renderNow(0.5);
    app.edit("test: the file gone", [m, blue](Project& p) {
        if (MediaItem* mm = p.findMedia(m)) {
            mm->path = blue.toStdString();
            mm->offline = true;
        }
    });
    pb->renderNow(0.5);  // unreadable at this point
    pic.fill(QColor(20, 20, 220));
    pic.save(blue);
    bin->refreshMedia({m});
    const MediaItem* mi = app.project().findMedia(m);
    check(mi && !mi->offline, "Refresh: the file is found");
    const QImage f = pb->renderNow(0.5);
    const QRgb mid = f.isNull() ? 0 : f.pixel(f.width() / 2, f.height() / 2);
    check(!e.isNull() && qRed(e.pixel(e.width() / 2, e.height() / 2)) > 150 && qBlue(mid) > 150 && qRed(mid) < 100,
          "Refresh: the preview shows the file that has come back", strfmt("%08x", (unsigned)mid));
}

// Preview Cursor Position (Num 0) with Loop Playback on and the cursor outside the loop: it played the loop
// instead of stopping where the preview stops.
static void cursorPreviewIgnoresLoop(MainWindow& w)
{
    AppState& app = st();
    Playback* pb = w.playback();
    app.newProject();
    std::string err;
    app.addMediaToTimeline(app.importMedia("synth:1", &err), 0.0, 0);
    app.setLoop(0.0, 1.0);
    app.loopOn = true;
    app.notify(kPlayback);
    app.setCursor(5.0);
    pb->previewCursor(0.5);
    double lo = 1e9, hi = -1e9;
    QElapsedTimer t;
    t.start();
    while (pb->playing() && t.elapsed() < 4000) {
        uitest::settle(10);
        lo = std::min(lo, app.cursor());
        hi = std::max(hi, app.cursor());
    }
    const bool stopped = !pb->playing();
    pb->pause();
    check(stopped && lo >= 4.7 && hi <= 5.3 && std::fabs(app.cursor() - 5.0) < 1e-6,
          "Preview Cursor Position with the loop on: plays round the cursor and stops there",
          strfmt("%s, cursor went %.3f .. %.3f, now %.3f", stopped ? "stopped" : "still playing", lo, hi, app.cursor()));
    app.loopOn = false;
    app.notify(kPlayback);
}

// Waveform per column of an event: light pixels down the middle of its track (title bar and bottom-right buttons left out).
static std::vector<double> waveProfile(TimelineWidget* tl, uint64_t trackId, double from, double to)
{
    const QImage g = tl->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const QRect tr = tl->trackRect(trackId);
    const int x0 = tl->xAt(from) + 12, x1 = tl->xAt(to) - 60;
    std::vector<double> out;
    for (int x = x0; x < x1; ++x) {
        int n = 0;
        for (int y = tr.top() + tr.height() / 3; y < tr.bottom() - 4; ++y) {
            const QRgb c = g.pixel(x, y);
            if (qRed(c) + qGreen(c) + qBlue(c) > 480) ++n;
        }
        out.push_back(n);
    }
    return out;
}

static double correlation(const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0;
    double ma = 0, mb = 0;
    for (size_t k = 0; k < n; ++k) { ma += a[k]; mb += b[k]; }
    ma /= n;
    mb /= n;
    double ab = 0, aa = 0, bb = 0;
    for (size_t k = 0; k < n; ++k) {
        ab += (a[k] - ma) * (b[k] - mb);
        aa += (a[k] - ma) * (a[k] - ma);
        bb += (b[k] - mb) * (b[k] - mb);
    }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0;
}

// A reversed sound event drew its waveform forwards.
static void reversedWaveform(MainWindow& w)
{
    AppState& app = st();
    TimelineWidget* tl = w.timeline();
    app.newProject();
    app.setLoop(0, 0);
    app.setSelection({});
    std::string err;
    // the test sound swells and fades twice a second, so three seconds reversed is the swell upside down
    const std::vector<uint64_t> made = app.addMediaToTimeline(app.importMedia("synthaudio:1", &err), 0.0, 0);
    check(made.size() == 1, "reversed waveform: an audio event is made");
    if (made.empty()) return;
    const uint64_t id = made[0];
    app.edit("test: three seconds", [id](Project& p) {
        if (Event* e = p.findEvent(id)) e->length = 3.0;
    });
    const Track* t = nullptr;
    app.project().findEvent(id, &t);
    const uint64_t track = t ? t->id : 0;
    tl->zoomToFit();
    std::vector<double> fwd;
    for (int k = 0; k < 40; ++k) {  // until the waveform has been read
        uitest::settle(100);
        fwd = waveProfile(tl, track, 0.0, 3.0);
        if (*std::max_element(fwd.begin(), fwd.end()) > 4) break;
    }
    app.edit("test: reverse", [id](Project& p) {
        if (Event* e = p.findEvent(id)) e->reverse = true;
    });
    uitest::settle(200);
    const std::vector<double> rev = waveProfile(tl, track, 0.0, 3.0);
    const double c = correlation(fwd, rev);
    check(!fwd.empty() && c < -0.5, "a reversed sound event's waveform is drawn reversed", strfmt("correlation with the forward one %.2f", c));
}

// Project Media Usage column (events per file) was not refreshed when only the count changed (split, paste, delete one of several).
static void mediaUsageFollowsSplits(MainWindow& w)
{
    AppState& app = st();
    QWidget* page = w.findChild<QDockWidget*>("dockMedia")->widget();
    QWidget* bin = page->layout()->itemAt(0)->widget();
    QTreeWidget* tree = nullptr;  // the media list with the Usage column
    for (QTreeWidget* t : bin->findChildren<QTreeWidget*>())
        if (t->columnCount() >= 7) tree = t;
    app.newProject();
    std::string err;
    const uint64_t m = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> made = app.addMediaToTimeline(m, 0.0, 0);
    uitest::settle(300);
    auto usage = [&]() -> QString {
        for (int k = 0; tree && k < tree->topLevelItemCount(); ++k)
            if (tree->topLevelItem(k)->data(0, Qt::UserRole).toULongLong() == m || tree->topLevelItem(k)->text(0) == "synth:1")
                return tree->topLevelItem(k)->text(6);
        return QString("(not listed)");
    };
    const QString before = usage();
    app.setSelection(std::set<uint64_t>(made.begin(), made.end()));
    app.setCursor(4.0);
    app.splitAtCursor();
    uitest::settle(400);
    const QString after = usage();
    check(before == "2" && after == "4", "Project Media's Usage count follows a split", (before + " -> " + after).toStdString());
}

// Mean per-channel difference of two pictures (-1: sizes differ).
static double meanDiff(const QImage& a0, const QImage& b0)
{
    if (a0.isNull() || a0.size() != b0.size()) return -1;
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32), b = b0.convertToFormat(QImage::Format_ARGB32);
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* p = (const QRgb*)a.constScanLine(y);
        const QRgb* q = (const QRgb*)b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x)
            sum += std::abs(qRed(p[x]) - qRed(q[x])) + std::abs(qGreen(p[x]) - qGreen(q[x])) + std::abs(qBlue(p[x]) - qBlue(q[x]));
    }
    return sum / (3.0 * a.width() * a.height());
}
// Same at an eighth of the size (averaged): a lossy file's frame against the project's, where fine grain does not survive the codec.
static double meanDiffCoarse(const QImage& a, const QImage& b)
{
    if (a.isNull() || a.size() != b.size()) return -1;
    const QSize s(std::max(1, a.width() / 8), std::max(1, a.height() / 8));
    return meanDiff(a.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), b.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// Selectively Prerender Video: the preview shows the prerendered frames while they match the project, and stops using them once it changes.
static void prerenderedFramesLookRight(MainWindow& w, const QString& out)
{
    AppState& app = st();
    Playback* pb = w.playback();
    app.newProject();
    uitest::buildDemoProject();
    app.clearEditHistory();
    // move the cursor away from the frame looked at, so the preview cache can't answer
    app.setCursor(0.0);
    uitest::settle(400);
    const double t = 1.2;
    const QImage fresh = pb->renderNow(t);
    std::string err;
    const bool made = pb->prerender(0.5, 1.5, out.toStdString(), nullptr, &err);
    check(made, "prerender: a stretch is prerendered", err);
    auto shownAt = [&](double at) {
        // an earlier request can come back first
        double shownTime = -1;
        auto before = pb->onFrame;
        pb->onFrame = [&](const QImage& image, double seconds, double ms) {
            shownTime = seconds;
            if (before) before(image, seconds, ms);
        };
        app.setCursor(at);
        pb->requestFrame();
        QElapsedTimer el;
        el.start();
        while (std::fabs(shownTime - at) > 1e-9 && el.elapsed() < 20000) uitest::settle(20);
        pb->onFrame = before;
        return pb->lastFrame();
    };
    const uint64_t pre0 = pb->prerenderedFramesShown();
    const QImage shown = shownAt(t);
    const double d = meanDiffCoarse(shown, fresh);
    std::string ranges;
    for (const auto& r : pb->prerenderedRanges()) ranges += strfmt(" [%.3f-%.3f %s %s]", r.from, r.to, r.valid ? "valid" : "not valid", r.path.c_str());
    check(pb->prerenderedFramesShown() > pre0 && d >= 0 && d < 12, "prerender: the preview shows the prerendered frame, and it looks as the project does",
          strfmt("from the file: %s, mean difference %.1f;", pb->prerenderedFramesShown() > pre0 ? "yes" : "no", d) + ranges);
    if (d >= 12) {
        // which of the project's frames it is
        int best = -1;
        double bestD = 1e9;
        for (int k = 0; k < 60; ++k) {
            const double dk = meanDiffCoarse(shown, pb->renderNow(k / 30.0));
            if (dk >= 0 && dk < bestD) { bestD = dk; best = k; }
        }
        std::printf("prerender: the frame shown at %.3f (frame %d) is most like the project's frame %d (%.1f)\n", t, (int)std::lround(t * 30), best, bestD);
        shown.save(out + "/prerender_shown.png");
        fresh.save(out + "/prerender_fresh.png");
    }
    // project changed (first video event muted): prerendered frames are stale
    app.edit("test: mute", [](Project& p) {
        for (Track& tr : p.tracks)
            if (tr.type == TrackType::Video)
                for (Event& e : tr.events) e.mute = true;
    });
    uitest::settle(100);
    const QImage now = pb->renderNow(t);
    const QImage shown2 = shownAt(t);
    // the live preview is drawn at the preview's size (half here), the prerendered file and renderNow at the project's: compare at one size
    const double d2 = shown2.isNull() ? -1 : meanDiffCoarse(shown2.scaled(now.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation), now);
    check(d2 >= 0 && d2 < 2, "prerender: after a change the preview shows the project, not the old prerendered frames",
          strfmt("mean difference %.1f (shown %dx%d, project %dx%d)", d2, shown2.width(), shown2.height(), now.width(), now.height()));
    pb->clearPrerendered(true);
}

// Patches (8 x 8 blocks, 12+ pixels a shade apart) where the screen after partial repaints differs from a full repaint.
static int screenPatches(MainWindow& w, const QString& save = QString())
{
    TimelineWidget* tl = w.timeline();
    uitest::settle(150);
    QBackingStore* bs = w.backingStore();
    QPaintDevice* pd = bs ? bs->paintDevice() : nullptr;
    if (!pd || pd->devType() != QInternal::Image) return -1;
    const QImage fresh = tl->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const QPoint at = tl->mapTo(&w, QPoint(0, 0)) * fresh.devicePixelRatio();
    const QImage shown = static_cast<QImage*>(pd)->copy(QRect(at, fresh.size())).convertToFormat(QImage::Format_ARGB32);
    if (shown.size() != fresh.size()) return -1;
    const int bw = (fresh.width() + 7) / 8, bh = (fresh.height() + 7) / 8;
    std::vector<int> blocks((size_t)bw * bh, 0);
    for (int y = 0; y < fresh.height(); ++y) {
        const QRgb* a = (const QRgb*)fresh.constScanLine(y);
        const QRgb* b = (const QRgb*)shown.constScanLine(y);
        for (int x = 0; x < fresh.width(); ++x)
            if (std::abs(qRed(a[x]) - qRed(b[x])) > 40 || std::abs(qGreen(a[x]) - qGreen(b[x])) > 40 || std::abs(qBlue(a[x]) - qBlue(b[x])) > 40)
                ++blocks[(size_t)(y / 8) * bw + x / 8];
    }
    int n = 0;
    for (int v : blocks) n += v >= 12;
    if (n > 0 && !save.isEmpty()) {
        fresh.save(save + "_whole.png");
        shown.save(save + "_shown.png");
    }
    return n;
}

// Select All / Unselect All: selected events (and their waveforms) must be lit everywhere.
static void selectAllRepaints(MainWindow& w, const QString& out)
{
    AppState& app = st();
    app.newProject();
    uitest::buildDemoProject();
    w.timeline()->zoomToFit();
    uitest::settle(1500);
    screenPatches(w);
    std::set<uint64_t> all;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events) all.insert(e.id);
    app.setSelection(all);
    const int a = screenPatches(w, out + "/selectall");
    app.setSelection({});
    const int b = screenPatches(w, out + "/unselectall");
    check(a == 0 && b == 0, "Select All / Unselect All: the timeline on the screen is what a whole repaint draws", strfmt("%d and %d patches", a, b));
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoBugsTest");
    QApplication::setApplicationName("bugs_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("bugs");
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k)
        if (args[k].endsWith(".ofx")) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, std::vector<std::string>());

    listenersAddedWhileTelling();

    MainWindow w;
    w.resize(1400, 900);
    w.show();
    uitest::settle(300);

    previewFollowsReplacedMedia(w, out);
    cursorPreviewIgnoresLoop(w);
    reversedWaveform(w);
    mediaUsageFollowsSplits(w);
    prerenderedFramesLookRight(w, out);
    selectAllRepaints(w, out);

    st().newProject();
    return uitest::summary();
}
