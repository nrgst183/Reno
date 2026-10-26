// Automatic preview proxies in the UI: the Preferences > Video switch, the status bar progress and its off button,
// queue order, proxies made in the cache folder, and the preview using a proxy below Best quality (never at Best or in renders).
//
//   autoproxy_ui_test <output folder>
#include "../../src/app/appstate.h"
#include "../../src/app/autoproxy.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/mediacache.h"
#include "../../src/app/playback.h"
#include "../../src/app/prefsapply.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

// Video of 'frames' pictures at 25 fps, left half the colour (b, g, r), a key frame every 2 s.
bool makeVideo(const std::string& path, int frames, int b, int g, int r)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = 320;
    es.height = 180;
    es.fpsNum = 25;
    es.fpsDen = 1;
    es.videoFamily = "h264";
    es.encodeMode = "cpu";
    es.rateControl = "quality";
    es.quality = 18;
    es.keyframeSeconds = 2.0;
    es.bFrames = 0;
    std::string err;
    auto out = createMediaWriter(es, &err);
    if (!out) return false;
    for (int f = 0; f < frames; ++f) {
        ImagePtr img = Image::make(320, 180, PixelDepth::Byte, false);
        for (int y = 0; y < 180; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < 320; ++x, d += 4) {
                const bool box = x >= 160 + f * 2 && x < 192 + f * 2 && y > 60 && y < 120;
                if (x < 160) { d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; }
                else { const uint8_t v = box ? 255 : 0; d[0] = d[1] = d[2] = v; }
                d[3] = 255;
            }
        }
        if (!out->writeVideo(*img)) return false;
    }
    return out->finish(&err);
}

QAction* action(MainWindow& w, const char* name)
{
    for (QAction* a : w.findChildren<QAction*>())
        if (a->objectName() == name) return a;
    return nullptr;
}
void withDialog(std::function<void()> open, std::function<void(QDialog*)> fn)
{
    QTimer::singleShot(300, [fn] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!d) {
            check(false, "the dialog opened");
            return;
        }
        fn(d);
        for (QPushButton* b : d->findChildren<QPushButton*>())
            if (b->text() == "OK") {
                b->click();
                return;
            }
        d->reject();
    });
    open();
}
void prefs(MainWindow& w, std::function<void(QDialog*)> fn) { withDialog([&] { action(w, "preferences")->trigger(); }, fn); }
void setCheck(QDialog* d, const char* name, bool on)
{
    if (QCheckBox* c = d->findChild<QCheckBox*>(name)) c->setChecked(on);
    else check(false, std::string("the dialog has ") + name);
}
bool waitFor(const std::function<bool()>& f, double seconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((int64_t)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        uitest::settle(20);
    }
    return f();
}
std::string names(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& c : v) s += (s.empty() ? "" : ", ") + pathFileName(c);
    return s;
}
struct Px { int b, g, r; };
Px sample(const QImage& img, double fx, double fy)
{
    if (img.isNull()) return {-1, -1, -1};
    const QRgb c = img.pixel(std::min(img.width() - 1, (int)(fx * img.width())), std::min(img.height() - 1, (int)(fy * img.height())));
    return {qBlue(c), qGreen(c), qRed(c)};
}
bool near(const Px& p, int b, int g, int r, int tol = 16) { return std::abs(p.b - b) <= tol && std::abs(p.g - g) <= tol && std::abs(p.r - r) <= tol; }
std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d", p.b, p.g, p.r); }

// 'n' events of 'len' seconds of the media.
void addEvents(AppState& app, uint64_t media, int n, double len, double from)
{
    app.edit("Add events", [&](Project& p) {
        Track* t = nullptr;
        for (Track& x : p.tracks)
            if (x.type == TrackType::Video) { t = &x; break; }
        if (!t) {
            Track nt;
            nt.id = p.newId();
            nt.type = TrackType::Video;
            p.tracks.push_back(nt);
            t = &p.tracks.back();
        }
        for (int k = 0; k < n; ++k) {
            Event e;
            e.id = p.newId();
            e.mediaId = media;
            e.start = from + k * len;
            e.length = len;
            e.offset = std::fmod(k * 0.7, 2.0);
            t->events.push_back(e);
        }
    });
}

}  // namespace

int main(int argc, char** argv)
{
    qunsetenv("QT_SCALE_FACTOR");
    qunsetenv("QT_ENABLE_HIGHDPI_SCALING");
    QApplication qapp(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setOrganizationName("RenoAutoProxyTest");
    QApplication::setApplicationName("autoproxy_ui_test");
    QSettings().clear();
    vprefs::reload();
    const QStringList args = QApplication::arguments();
    const QString out = QDir(args.size() > 1 ? args[1] : QString("autoproxyout")).absolutePath();
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.loadPreferences();
    app.newProject();
    const std::string A = (out + "/A.mp4").toStdString(), B = (out + "/B.mp4").toStdString(), C = (out + "/C.mp4").toStdString(), P = (out + "/P.mp4").toStdString(),
                      Q = (out + "/Q.mp4").toStdString();
    bool made = makeVideo(A, 100, 200, 40, 10) && makeVideo(B, 100, 10, 200, 10) && makeVideo(C, 100, 10, 10, 200) && makeVideo(P, 100, 200, 40, 10) && makeVideo(Q, 100, 10, 200, 10);
    check(made, "the test videos can be made");
    if (!made) return uitest::summary();
    // clean up proxies of an earlier run
    for (const std::string& f : {A, B, C, P, Q}) QFile::remove(QString::fromStdString(videoProxyPath(f)));

    MainWindow* w = new MainWindow();
    w->resize(1400, 800);
    w->show();
    uitest::settle(300);
    autoproxy::Worker& wk = autoproxy::Worker::get();

    // ======================================================================== the preferences
    check(vprefs::on("prefs/autoProxyPreview") && vprefs::on("prefs/autoProxy4k") && !vprefs::on("prefs/autoProxyLongGop"), "defaults: automatic preview proxies on (the old 'at once' rule still off)");
    check(wk.preferencesControl() && wk.enabled(), "the main window gives the worker to Preferences; it is on by default");
    {
        const std::string folder = videoProxyFolder(), cache = QDir::fromNativeSeparators(mediacache::folder("proxies")).toStdString();
        check(folder == cache, "proxies go to Reno's cache folder by default", folder);
        check(videoProxyPath("/media/x/clip.mp4").compare(0, cache.size(), cache) == 0, "the proxy of a file is named in it (<hash>_<name>.sfvp0)", videoProxyPath("/media/x/clip.mp4"));
    }
    prefs(*w, [](QDialog* d) {
        check(d->findChild<QCheckBox*>("autoProxyPreview") && d->findChild<QCheckBox*>("autoProxyPreview")->isChecked(), "Preferences > Video: the check box is there and on");
        setCheck(d, "autoProxyPreview", false);
        setCheck(d, "autoProxy4k", false);
    });
    uitest::settle(100);
    check(!wk.enabled() && !vprefs::on("prefs/autoProxyPreview"), "cleared in the dialog (with the 4K one): the worker is off");
    prefs(*w, [](QDialog* d) {
        setCheck(d, "autoProxyPreview", true);
        setCheck(d, "autoProxy4k", true);
    });
    uitest::settle(100);
    check(wk.enabled(), "set again: on");

    // ======================================================================== a project's media: queued in order, shown in the status bar
    // stand-in maker that holds the second file until told
    std::atomic<bool> hold{false}, holdSecond{false};
    std::vector<std::string> madeOrder;
    std::mutex mm;
    wk.setSettleMs(60);
    wk.setMaker([&](const std::string& path, const std::function<bool(double)>& progress, std::string*) {
        for (int k = 1; k <= 4; ++k) {
            if (!progress(k / 4.0)) return false;
            while ((hold || (holdSecond && pathFileName(path) == "B.mp4"))) {
                if (!progress(0.5)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        writeFile(videoProxyPath(path), "a proxy");
        videoProxiesChanged();
        std::lock_guard<std::mutex> lk(mm);
        madeOrder.push_back(path);
        return true;
    });
    hold = true;
    std::string err;
    const uint64_t ma = app.importMedia(A, &err), mb = app.importMedia(B, &err), mc = app.importMedia(C, &err);
    check(ma && mb && mc, "three long-GOP videos are imported", err);
    // C 3 events, B 10, A 30: A is jumped about in most, then B, then C
    addEvents(app, mc, 3, 0.3, 0);
    addEvents(app, mb, 10, 0.3, 10);
    addEvents(app, ma, 30, 0.2, 20);
    check(waitFor([&] { return wk.status().working; }, 15), "media of the open project: the worker starts making proxies by itself");
    {
        const autoproxy::Status st = wk.status();
        check(st.current == "A.mp4" && st.total == 3, "the file the project jumps about in most is first (A: 30 short events)", st.current + strfmt(", total %d", st.total));
        check(names(wk.queued()) == "B.mp4, C.mp4", "then B (10), then C (3)", names(wk.queued()));
        uitest::settle(900);
        QLabel* text = w->findChild<QLabel*>("videoProxyText");
        check(text && text->text().contains("Making preview proxies: 1 of 3") && text->text().contains("A.mp4"), "the status bar says which, '1 of 3'", text ? text->text().toStdString() : "no label");
        QWidget* piece = w->findChild<QWidget*>("videoProxyProgress");
        check(piece && piece->isVisibleTo(w), "and the piece is shown");
        uitest::shot(w, out + "/status_making.png");
    }
    holdSecond = true;
    hold = false;
    check(waitFor([&] { return wk.status().current == "B.mp4"; }, 15), "the second is B");
    uitest::settle(900);
    {
        QLabel* text = w->findChild<QLabel*>("videoProxyText");
        check(text && text->text().contains("2 of 3") && text->text().contains("B.mp4"), "the status bar: '2 of 3'", text ? text->text().toStdString() : "no label");
    }
    // the button turns the automatic proxies off and stops the one under way
    {
        QToolButton* cancel = w->findChild<QToolButton*>("cancelVideoProxy");
        check(cancel != nullptr, "there is a button to turn them off");
        if (cancel) cancel->click();
        check(waitFor([&] { return !wk.status().working; }, 10), "pressed: the one under way is stopped");
        check(!wk.enabled() && !vprefs::on("prefs/autoProxyPreview") && !vprefs::on("prefs/autoProxy4k"), "and the preferences are off");
        uitest::settle(900);
        QWidget* piece = w->findChild<QWidget*>("videoProxyProgress");
        check(piece && !piece->isVisibleTo(w), "the status bar piece is gone");
        std::lock_guard<std::mutex> lk(mm);
        check(names(madeOrder) == "A.mp4", "only A had been made", names(madeOrder));
        check(!hasVideoProxy(B) && !hasVideoProxy(C), "B and C have none");
    }
    holdSecond = false;
    wk.setMaker(nullptr);
    QFile::remove(QString::fromStdString(videoProxyPath(A)));
    videoProxiesChanged();

    // ======================================================================== real proxies, made in the cache folder
    prefs(*w, [](QDialog* d) { setCheck(d, "autoProxyPreview", true); });
    uitest::settle(100);
    check(wk.enabled(), "turned on in Preferences again");
    check(waitFor([&] { return hasVideoProxy(A) && hasVideoProxy(B) && hasVideoProxy(C); }, 120), "the proxies are made, A, B and C");
    check(waitFor([&] { return !wk.status().working; }, 30), "and it is done");
    {
        const std::string cache = QDir::fromNativeSeparators(mediacache::folder("proxies")).toStdString();
        check(videoProxyPath(A).compare(0, cache.size(), cache) == 0 && std::filesystem::exists(videoProxyPath(A)), "A's proxy is a file in Reno's cache folder", videoProxyPath(A));
        check(!std::filesystem::exists(A + ".sfvp0"), "and nothing was written beside the user's media");
        std::string e;
        auto r = openMedia(videoProxyPath(A), &e);
        check(r && r->info().hasVideo && r->info().width <= 960 && std::fabs(r->info().duration - 4.0) < 0.1, "it plays: a small video of the same length", r ? strfmt("%dx%d %.2f s", r->info().width, r->info().height, r->info().duration) : e);
    }

    // ======================================================================== the preview takes a proxy the moment it is there; Best quality does not
    // P and Q are two videos in different colours; Q stands in for P's proxy
    {
        app.newProject();
        app.edit("settings", [](Project& p) {
            p.settings.width = 320;
            p.settings.height = 180;
            p.settings.fpsNum = 25;
            p.settings.fpsDen = 1;
        });
        const uint64_t mp = app.importMedia(P, &err);
        addEvents(app, mp, 1, 4.0, 0);
        QFile::remove(QString::fromStdString(videoProxyPath(P)));
        wk.setModes(false, false, false);  // worker out of the way, proxy placed by hand
        videoProxiesChanged();
        Playback pb;
        pb.setPreviewSize(0, 0, false);
        app.prefs.ramPreviewMb = 256;
        app.setCursor(0.5);
        pb.requestFrame();
        uitest::settle(600);
        check(near(sample(pb.lastFrame(), 0.2, 0.5), 200, 40, 10), "preview: without a proxy, the original", pxs(sample(pb.lastFrame(), 0.2, 0.5)));
        check(pb.ramPreviewFrames() >= 1, "the picture is in the RAM preview");
        std::filesystem::create_directories(std::filesystem::path(videoProxyPath(P)).parent_path());
        std::filesystem::copy_file(Q, videoProxyPath(P), std::filesystem::copy_options::overwrite_existing);
        videoProxiesChanged();
        pb.requestFrame();
        uitest::settle(800);
        check(near(sample(pb.lastFrame(), 0.2, 0.5), 10, 200, 10), "preview: a new proxy is used at once (not the RAM-cached original)", pxs(sample(pb.lastFrame(), 0.2, 0.5)));
        pb.setFullQuality(true);
        pb.requestFrame();
        uitest::settle(800);
        check(near(sample(pb.lastFrame(), 0.2, 0.5), 200, 40, 10), "Best quality: the original, with the proxy there", pxs(sample(pb.lastFrame(), 0.2, 0.5)));
        pb.setFullQuality(false);
        pb.requestFrame();
        uitest::settle(800);
        check(near(sample(pb.lastFrame(), 0.2, 0.5), 10, 200, 10), "below Best again: the proxy", pxs(sample(pb.lastFrame(), 0.2, 0.5)));
        // proxy removed: the original again
        QFile::remove(QString::fromStdString(videoProxyPath(P)));
        videoProxiesChanged();
        pb.requestFrame();
        uitest::settle(800);
        check(near(sample(pb.lastFrame(), 0.2, 0.5), 200, 40, 10), "the proxy removed: the original again", pxs(sample(pb.lastFrame(), 0.2, 0.5)));
        // playing makes the proxy maker wait
        pb.setAudioOutput(createSilentOutput());
        app.setCursor(0.0);
        check(!previewPlaying(), "the proxy maker is not told to wait while nothing plays");
        pb.play();
        check(previewPlaying(), "play: the proxy maker is told the preview is playing");
        pb.pause();
        check(!previewPlaying(), "pause: and that it has stopped");
    }
    delete w;
    return uitest::summary();
}
