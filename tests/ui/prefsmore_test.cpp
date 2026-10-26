// Options > Preferences settings (src/app/vprefs.h, prefsapply.h): each one is set in the real dialog, saved with OK, read back after
// the settings were forgotten (a new run), and its effect on Reno is checked.
//
//   prefsmore_test <output folder>
//
// Build: prefsmore-objs.sh prefsmore_test (every UI component real). QT_QPA_PLATFORM=offscreen.
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/appstate.h"
#include "../../src/app/externalmonitor.h"
#include "../../src/app/logbook.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/mediacache.h"
#include "../../src/app/midisync.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/prefsapply.h"
#include "../../src/app/timeline.h"
#include "../../src/app/vprefs.h"
#include "../../src/media/vegasfiles.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <cmath>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QAction* action(MainWindow& w, const char* name)
{
    for (QAction* a : w.findChildren<QAction*>())
        if (a->objectName() == name) return a;
    return nullptr;
}

// Runs 'fn' on the modal dialog 'open' shows, then presses its OK button.
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

// Opens Options > Preferences, lets 'fn' change it, OK.
void prefs(MainWindow& w, std::function<void(QDialog*)> fn) { withDialog([&] { action(w, "preferences")->trigger(); }, fn); }

template <class T> static T* findKind(QWidget* root)
{
    for (QWidget* c : root->findChildren<QWidget*>())
        if (T* t = dynamic_cast<T*>(c)) return t;
    return nullptr;
}

template <class T>
T* find(QDialog* d, const char* name)
{
    T* c = d->findChild<T*>(name);
    if (!c) check(false, std::string("the dialog has ") + name);
    return c;
}
void setCheck(QDialog* d, const char* name, bool on) { if (QCheckBox* c = find<QCheckBox>(d, name)) c->setChecked(on); }
void setInt(QDialog* d, const char* name, int v) { if (QSpinBox* c = find<QSpinBox>(d, name)) c->setValue(v); }
void setText(QDialog* d, const char* name, const QString& v) { if (QLineEdit* c = find<QLineEdit>(d, name)) c->setText(v); }
void setPick(QDialog* d, const char* name, const QString& data)
{
    if (QComboBox* c = find<QComboBox>(d, name)) {
        const int i = c->findData(data);
        if (i < 0) check(false, std::string(name) + " offers " + data.toStdString());
        else c->setCurrentIndex(i);
    }
}
// A General tab item (a check list entry) by its key.
void setGeneral(QDialog* d, const QString& key, bool on)
{
    if (QListWidget* l = find<QListWidget>(d, "generalList"))
        for (int k = 0; k < l->count(); ++k)
            if (l->item(k)->data(Qt::UserRole).toString() == key) {
                l->item(k)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
                return;
            }
    check(false, "the General tab has " + key.toStdString());
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
std::string num(double v) { return strfmt("%.4f", v); }

qint64 folderBytes(const QString& dir)
{
    qint64 n = 0;
    for (const QFileInfo& f : QDir(dir).entryInfoList(QDir::Files)) n += f.size();
    return n;
}
void makeFiles(const QString& dir, int count, int kb)
{
    QDir().mkpath(dir);
    for (int k = 0; k < count; ++k) {
        QFile f(QString("%1/dummy%2.bin").arg(dir).arg(k));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QByteArray(kb * 1024, 'x'));
            f.flush();
  // older first
            f.setFileTime(QDateTime::currentDateTime().addSecs(-1000 + k), QFileDevice::FileModificationTime);
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    qunsetenv("QT_SCALE_FACTOR");
    qunsetenv("QT_ENABLE_HIGHDPI_SCALING");
    QApplication qapp(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);  // the test closes the main window to see what leaving asks
    QApplication::setOrganizationName("RenoPrefsMoreTest");
    QApplication::setApplicationName("prefsmore_test");
    QSettings().clear();
    vprefs::reload();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("prefsmoreout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.loadPreferences();
    app.newProject();
    uitest::buildDemoProject();

    MainWindow* w = new MainWindow();
    w->resize(1500, 900);
    w->show();
    uitest::settle(400);
    TimelineWidget* tl = w->timeline();
    Playback* pb = w->playback();

    // ======================================================================== the defaults
    {
        check(vprefs::on("prefs/useAllCores") && vprefs::number("prefs/decoderThreads") == 0 && vprefs::number("prefs/audioCacheMb") == 256 &&
                  vprefs::number("prefs/thumbCacheMb") == 512 && vprefs::number("prefs/peakCacheMb") == 512 && vprefs::text("prefs/thumbQuality") == "normal" &&
                  !vprefs::on("prefs/autoProxyLongGop") && vprefs::text("prefs/proxyFolder").isEmpty() && vprefs::text("prefs/peakFolder").isEmpty() &&
                  vprefs::number("prefs/snapDistance") == 8 && !vprefs::on("prefs/scrubAudio") && vprefs::number("prefs/defaultFadeInMs") == 0 &&
                  vprefs::number("prefs/uiScale") == 100 && vprefs::number("prefs/timelineFontPt") == 0 && vprefs::on("prefs/confirmExit") &&
                  !vprefs::on("prefs/confirmRemoveMedia") && vprefs::on("prefs/scanAtStartup") && app.prefs.autoSaveKeep == 1 && near(vprefs::real("prefs/playbackBuffer"), 0.05),
              "the new settings start as Reno behaved before they could be set");
        check(reno::processorLimit() == 0 && prefsapply::audioCacheBytes() == (size_t)256 << 20, "... and so does what they set (all processors, 256 MB of stretched sound)");
    }

    // ======================================================================== Performance
    prefs(*w, [](QDialog* d) {
        setCheck(d, "useAllCores", false);
        setInt(d, "maxThreads", 3);
        setInt(d, "decoderThreads", 5);
        setInt(d, "audioCacheMb", 7);
    });
    uitest::settle(100);
    {
        QSettings s;
        check(!s.value("prefs/useAllCores").toBool() && s.value("prefs/maxThreads").toInt() == 3 && s.value("prefs/decoderThreads").toInt() == 5 && s.value("prefs/audioCacheMb").toInt() == 7,
              "performance: processors, decoder threads and the stretched-sound cache are saved");
        check(reno::processorLimit() == 3, "performance: 'use all processors' cleared - the limit is the number given", std::to_string(reno::processorLimit()));
        check(reno::videoDecoderThreadLimit() == 5, "performance: threads per video decoder", std::to_string(reno::videoDecoderThreadLimit()));
        check(prefsapply::audioCacheBytes() == (size_t)7 << 20, "performance: the stretched-sound cache size handed to the audio engine");
    }
    vprefs::reload();
    prefs(*w, [](QDialog* d) {
        // read back, as a new run would show them
        QCheckBox* all = d->findChild<QCheckBox*>("useAllCores");
        QSpinBox* mt = d->findChild<QSpinBox*>("maxThreads");
        QSpinBox* dt = d->findChild<QSpinBox*>("decoderThreads");
        QSpinBox* ac = d->findChild<QSpinBox*>("audioCacheMb");
        check(all && !all->isChecked() && mt && mt->value() == 3 && mt->isEnabled() && dt && dt->value() == 5 && ac && ac->value() == 7,
              "performance: the dialog shows what was saved (and the processor count is free to set)");
        setCheck(d, "useAllCores", true);
        setInt(d, "decoderThreads", 0);
        setInt(d, "audioCacheMb", 256);
    });
    uitest::settle(100);
    check(reno::processorLimit() == 0 && reno::videoDecoderThreadLimit() == 0, "performance: all processors again, automatic decoder threads");

    // ======================================================================== Waveforms and pictures: cache sizes
    {
        const QString thumbs = mediacache::folder("thumbs"), peaks = mediacache::folder("peaks");
        QDir(thumbs).removeRecursively();
        QDir(peaks).removeRecursively();
        makeFiles(thumbs, 8, 300);          // 2.4 MB
        makeFiles(peaks, 8, 300);
        prefs(*w, [](QDialog* d) {
            setInt(d, "thumbCacheMb", 1);
            setInt(d, "peakCacheMb", 2);
        });
        uitest::settle(100);
        check(QSettings().value("prefs/thumbCacheMb").toInt() == 1 && QSettings().value("prefs/peakCacheMb").toInt() == 2, "cache sizes: saved");
        check(folderBytes(thumbs) <= 1024 * 1024 && folderBytes(thumbs) > 0 && QFile::exists(thumbs + "/dummy7.bin") && !QFile::exists(thumbs + "/dummy0.bin"),
              "cache sizes: the thumbnail cache was cut to its size, oldest files first", std::to_string(folderBytes(thumbs)));
        check(folderBytes(peaks) <= 2 * 1024 * 1024 && folderBytes(peaks) > 1024 * 1024, "cache sizes: the waveform cache has its own size", std::to_string(folderBytes(peaks)));
        prefs(*w, [](QDialog* d) {
            setInt(d, "thumbCacheMb", 512);
            setInt(d, "peakCacheMb", 512);
        });
        QDir(thumbs).removeRecursively();
        QDir(peaks).removeRecursively();
    }

    // ======================================================================== peak and proxy folders, thumbnail quality, automatic proxies
    {
        const QString pf = QDir(out).absoluteFilePath("peakfolder"), xf = QDir(out).absoluteFilePath("proxyfolder");
        prefs(*w, [&](QDialog* d) {
            setText(d, "peakFolder", QDir::toNativeSeparators(pf));
            setText(d, "proxyFolder", QDir::toNativeSeparators(xf));
            setPick(d, "thumbQuality", "draft");
            setCheck(d, "autoProxyLongGop", true);
            setInt(d, "autoProxyMinWidth", 1280);
            setCheck(d, "vegasPeakFiles", false);
        });
        uitest::settle(100);
        const std::string media = "/media/a/clip.wav";
        const std::string pp = vegasfiles::peakFilePath(media), xp = videoProxyPath("/media/a/clip.mp4");
        check(pp.compare(0, pf.size(), pf.toStdString()) == 0 && pp.find(".sfk") != std::string::npos, "peak files: they go to the folder chosen", pp);
        check(xp.compare(0, xf.size(), xf.toStdString()) == 0 && xp.find(".sfvp0") != std::string::npos, "video proxies: they go to the folder chosen", xp);
        check(near(vprefs::thumbScale(), 0.5), "thumbnails: draft quality asks for half-size pictures");
        check(!mediacache::vegasPeakFiles().load(), "peak files: 'keep waveforms in peak files' cleared is handed to the cache");
        MediaInfo mi;
        mi.hasVideo = true;
        mi.duration = 10;
        mi.width = 1920;
        mi.height = 1080;
        mi.videoCodec = "h264";
        check(prefsapply::wantsAutoProxy(mi), "automatic proxies: long-GOP media of the width gets one");
        mi.width = 1280;
        check(prefsapply::wantsAutoProxy(mi), "automatic proxies: ... from the width given on");
        mi.width = 1024;
        check(!prefsapply::wantsAutoProxy(mi), "automatic proxies: ... not below it");
        mi.width = 1920;
        mi.videoCodec = "prores";
        check(!prefsapply::wantsAutoProxy(mi), "automatic proxies: ... nor an intra-frame codec");
        mi.width = 3840;
        mi.height = 2160;
        check(!prefsapply::wantsAutoProxy(mi), "automatic proxies: 3840 x 2160 is not greater than 4K");
        mi.width = 4096;
        check(prefsapply::wantsAutoProxy(mi), "automatic proxies: greater than 4K gets one whatever the codec (the Video tab's setting)");
        vprefs::reload();
        prefs(*w, [](QDialog* d) {
            QLineEdit* e = d->findChild<QLineEdit*>("peakFolder");
            QComboBox* q = d->findChild<QComboBox*>("thumbQuality");
            check(e && !e->text().isEmpty() && q && q->currentData().toString() == "draft", "peak folder and thumbnail quality: the dialog shows what was saved");
            setText(d, "peakFolder", QString());
            setText(d, "proxyFolder", QString());
            setPick(d, "thumbQuality", "high");
            setCheck(d, "autoProxyLongGop", false);
            setCheck(d, "vegasPeakFiles", true);
        });
        uitest::settle(100);
        const std::string pq = vegasfiles::peakFilePath(media);
        check(pq == "/media/a/clip.sfk" || pq == "/media/a/clip.wav.sfk", "peak files: the folder cleared - beside the media again", pq);
        {
            // no folder chosen: Reno's cache folder, beside the thumbnails and waveforms it keeps
            const std::string vp = videoProxyPath("/media/a/clip.mp4"), cache = QDir::fromNativeSeparators(mediacache::folder("proxies")).toStdString();
            check(vp.compare(0, cache.size(), cache) == 0 && vp.find(".sfvp0") != std::string::npos, "video proxies: no folder chosen - Reno's cache folder", vp);
        }
        prefs(*w, [](QDialog* d) { setCheck(d, "proxyBeside", true); });
        uitest::settle(100);
        check(videoProxyPath("/media/a/clip.mp4") == "/media/a/clip.mp4.sfvp0", "video proxies: 'keep them beside the media' - beside the media, as VEGAS does");
        prefs(*w, [](QDialog* d) { setCheck(d, "proxyBeside", false); });
        check(near(vprefs::thumbScale(), 2.0), "thumbnails: high quality asks for twice the size");
        mi.videoCodec = "h264";
        mi.width = 1920;
        mi.height = 1080;
        check(!prefsapply::wantsAutoProxy(mi), "automatic proxies: off again");
        prefs(*w, [](QDialog* d) { setPick(d, "thumbQuality", "normal"); });
    }

    // ======================================================================== Files: auto-save backups kept, the template project
    {
        const QString proj = QDir(out).absoluteFilePath("keep.reno");
        std::string err;
        app.edit("Touch", [](Project& p) { p.settings.tempo = 111; });
        check(app.save(proj.toStdString(), &err), "a project to auto-save was saved", err);
        for (const char* ext : {"", ".1", ".2", ".3", ".4"}) QFile::remove(QString::fromStdString(app.autoSavePath()) + ext);
        prefs(*w, [](QDialog* d) { setInt(d, "autoSaveKeep", 3); });
        check(QSettings().value("prefs/autoSaveKeep").toInt() == 3 && app.prefs.autoSaveKeep == 3, "auto-save backups kept: saved");
        for (int k = 0; k < 5; ++k) {
            app.edit("Touch", [k](Project& p) { p.settings.tempo = 120 + k; });
            check(app.autoSaveNow(&err), "auto-saved " + std::to_string(k), err);
        }
        const QString bak = QString::fromStdString(app.autoSavePath());
        check(QFile::exists(bak) && QFile::exists(bak + ".1") && QFile::exists(bak + ".2") && !QFile::exists(bak + ".3"),
              "auto-save backups kept: three files - the newest and the two before it");
        prefs(*w, [](QDialog* d) { setInt(d, "autoSaveKeep", 1); });
        app.edit("Touch", [](Project& p) { p.settings.tempo = 150; });
        const qint64 oldSize = QFileInfo(bak + ".1").size();
        const QDateTime oldTime = QFileInfo(bak + ".1").lastModified();
        app.autoSaveNow(&err);
        check(!QFile::exists(bak + ".3") && QFileInfo(bak + ".1").size() == oldSize && QFileInfo(bak + ".1").lastModified() == oldTime,
              "auto-save backups kept: one - the file is overwritten, the older ones are left alone");

        // File > New starts from the template project
        const QString tpl = QDir(out).absoluteFilePath("template.reno");
        {
            Project t;
            Track a;
            a.id = t.newId();
            a.type = TrackType::Audio;
            a.name = "TemplateTrack";
            t.tracks.push_back(a);
            Track v;
            v.id = t.newId();
            v.type = TrackType::Video;
            v.name = "TemplateVideo";
            t.tracks.push_back(v);
            t.settings.width = 1280;
            t.settings.height = 720;
            check(t.save(tpl.toStdString(), &err), "the template project was saved", err);
        }
        prefs(*w, [&](QDialog* d) { setText(d, "templateProject", QDir::toNativeSeparators(tpl)); });
        app.newProject(true);
        check(app.project().tracks.size() == 2 && app.project().tracks[0].name == "TemplateTrack" && app.project().settings.width == 1280 && app.path().empty() && !app.dirty(),
              "template project: File > New starts with its tracks and settings (and is not tied to it)", std::to_string(app.project().tracks.size()));
        app.newProject();
        check(app.project().tracks.empty(), "template project: opening a project starts from nothing");
        vprefs::reload();
        prefs(*w, [&](QDialog* d) {
            QLineEdit* e = d->findChild<QLineEdit*>("templateProject");
            check(e && QDir::fromNativeSeparators(e->text()) == tpl, "template project: the dialog shows what was saved");
            setText(d, "templateProject", QString());
        });
        app.newProject(true);
        check(app.project().tracks.empty(), "template project: cleared - File > New is empty again");
        uitest::buildDemoProject();
    }

    // ======================================================================== Editing: fades, track heights, still cropping, snap distance
    {
        prefs(*w, [](QDialog* d) {
            setInt(d, "defaultFadeInMs", 500);
            setInt(d, "defaultFadeOutMs", 250);
            setInt(d, "newAudioTrackHeight", 120);
            setInt(d, "newVideoTrackHeight", 90);
        });
        uitest::settle(100);
        check(QSettings().value("prefs/defaultFadeInMs").toInt() == 500 && QSettings().value("prefs/newVideoTrackHeight").toInt() == 90, "editing: fade lengths and track heights saved");
        app.newProject(true);
        std::string err;
        const uint64_t clip = app.importMedia("synth:1", &err);
        const uint64_t snd = app.importMedia("synthaudio:2", &err);
        const uint64_t pic = app.importMedia("synthpic:3", &err);
        const std::vector<uint64_t> made = app.addMediaToTimeline(clip, 0.0, 0);
        bool fadesOk = !made.empty();
        for (uint64_t id : made) {
            const Event* e = app.project().findEvent(id);
            fadesOk = fadesOk && e && near(e->fadeIn, 0.5) && near(e->fadeOut, 0.25);
        }
        check(fadesOk, "editing: events put on the timeline start with the fade lengths given");
        const std::vector<uint64_t> sm = app.addMediaToTimeline(snd, 20.0, 0);
        const Event* se = sm.empty() ? nullptr : app.project().findEvent(sm[0]);
        check(se && near(se->fadeIn, 0.5) && near(se->fadeOut, 0.25), "editing: ... sound-only ones too");
        bool heights = false;
        {
            const uint64_t a = app.addTrack(TrackType::Audio), v = app.addTrack(TrackType::Video);
            Project copy = app.project();
            const Track* ta = copy.findTrack(a);
            const Track* tv = copy.findTrack(v);
            heights = ta && tv && ta->height == 120 && tv->height == 90;
        }
        check(heights, "editing: new audio and video tracks start at the heights given");
        // a still: cropped to the frame's shape when asked (the still is 16:9; the project is 4:3 to see it)
        app.edit("Shape", [](Project& p) {
            p.settings.width = 640;
            p.settings.height = 480;
        });
        const std::vector<uint64_t> p1 = app.addMediaToTimeline(pic, 40.0, 0);
        const Event* e1 = p1.empty() ? nullptr : app.project().findEvent(p1[0]);
        check(e1 && e1->panCrop.empty(), "editing: a still is put on the timeline whole by default");
        prefs(*w, [](QDialog* d) { setCheck(d, "autoCropStills", true); });
        const std::vector<uint64_t> p2 = app.addMediaToTimeline(pic, 50.0, 0);
        const Event* e2 = p2.empty() ? nullptr : app.project().findEvent(p2[0]);
        const MediaItem* pm = app.project().findMedia(pic);
        const double media = pm && pm->info.height > 0 ? (double)pm->info.width / pm->info.height : 1.0;
        check(e2 && e2->panCrop.size() == 1 && near(e2->panCrop[0].h, 1.0) && near(e2->panCrop[0].w, (4.0 / 3.0) / media, 1e-3),
              "editing: 'Automatically crop still images' - the still gets a Pan/Crop box of the 4:3 frame's shape", e2 && !e2->panCrop.empty() ? num(e2->panCrop[0].w) : "none");
        prefs(*w, [](QDialog* d) {
            setCheck(d, "autoCropStills", false);
            setInt(d, "defaultFadeInMs", 0);
            setInt(d, "defaultFadeOutMs", 0);
            setInt(d, "newAudioTrackHeight", 0);
            setInt(d, "newVideoTrackHeight", 0);
        });

        // snap distance: a click 7 pixels from an event's edge snaps with 8, not with 3
        app.newProject(true);
        const uint64_t c2 = app.importMedia("synth:1", &err);
        app.addMediaToTimeline(c2, 7.0, 0);
        app.snapping = true;
        uint64_t track = 0;
        for (const Track& t : app.project().tracks)
            if (t.type == TrackType::Video) track = t.id;
        app.setSelection(std::set<uint64_t>());
        tl->zoomToFit();
        uitest::settle(200);
        double edge = 7.0;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (t.type == TrackType::Video) edge = e.start;
        const QPoint left(tl->xAt(edge) - 7, tl->trackRect(track).top() + 40);
        app.setCursor(0);
        uitest::mouseClick(tl, left);
        check(near(app.cursor(), edge), "snap distance: 7 pixels from an edge snaps with the default 8", num(app.cursor()));
        prefs(*w, [](QDialog* d) { setInt(d, "snapDistance", 3); });
        app.setCursor(0);
        uitest::mouseClick(tl, left);
        check(!near(app.cursor(), edge), "snap distance: ... but not with 3", num(app.cursor()));
        prefs(*w, [](QDialog* d) { setInt(d, "snapDistance", 8); });
        uitest::buildDemoProject();
    }

    // ======================================================================== Scrub audio
    {
        app.setCursor(2.0);
        pb->pause();
        prefs(*w, [](QDialog* d) {
            setCheck(d, "scrubAudio", true);
            setInt(d, "scrubAudioMs", 150);
            setInt(d, "scrubVolume", 50);
        });
        uitest::settle(100);
        check(QSettings().value("prefs/scrubAudio").toBool() && QSettings().value("prefs/scrubAudioMs").toInt() == 150 && QSettings().value("prefs/scrubVolume").toInt() == 50, "scrub audio: saved");
        tl->zoomToFit();
        uitest::settle(100);
        const QRect ruler = tl->rulerRect();
        const QPoint a(tl->xAt(2.0), ruler.center().y()), b(tl->xAt(3.0), ruler.center().y());
        uitest::mousePress(tl, a);
        uitest::mouseMove(tl, QPoint((a.x() + b.x()) / 2, a.y()));
        check(pb->playing(), "scrub audio: dragging the cursor on the ruler plays a piece of the sound");
        check(near(outputGain(), 0.5, 1e-4), "scrub audio: ... at the volume given", num(outputGain()));
        uitest::mouseRelease(tl, b);
        uitest::settle(500);
        check(near(outputGain(), 1.0), "scrub audio: the volume is back for playing", num(outputGain()));
        pb->pause();
        prefs(*w, [](QDialog* d) { setCheck(d, "scrubAudio", false); });
        app.setCursor(2.0);
        uitest::mousePress(tl, a);
        uitest::mouseMove(tl, QPoint((a.x() + b.x()) / 2, a.y()));
        check(!pb->playing(), "scrub audio: off - dragging the cursor is silent");
        uitest::mouseRelease(tl, b);
        uitest::settle(100);
    }

    // ======================================================================== Interface: timeline text size, interface size, reverse event background
    {
        prefs(*w, [](QDialog* d) {
            setInt(d, "timelineFontPt", 17);
            setInt(d, "uiScale", 150);
        });
        uitest::settle(200);
        check(QSettings().value("prefs/timelineFontPt").toInt() == 17 && QSettings().value("prefs/uiScale").toInt() == 150, "interface: timeline text size and interface size saved");
        check(near(tl->font().pointSizeF(), 17.0), "interface: the timeline's text is the size given", num(tl->font().pointSizeF()));
        const QStringList set = prefsapply::earlyEnvironment();
        check(set.contains("QT_SCALE_FACTOR=1.50") && qgetenv("QT_SCALE_FACTOR") == "1.50", "interface: the interface size becomes Qt's scale factor when Reno starts", set.join(",").toStdString());
        qunsetenv("QT_SCALE_FACTOR");
        prefs(*w, [](QDialog* d) {
            setInt(d, "timelineFontPt", 0);
            setInt(d, "uiScale", 100);
        });
        uitest::settle(200);
        check(!near(tl->font().pointSizeF(), 17.0), "interface: timeline text size 0 - the program's own again");
        check(prefsapply::earlyEnvironment().isEmpty(), "interface: 100 % sets nothing");

        // Reverse Event Background Color: an audio event's body is lighter
        uitest::buildDemoProject();
        tl->zoomToFit();
        uitest::settle(300);
        uint64_t audioEvent = 0;
        for (const Track& t : app.project().tracks)
            if (t.type == TrackType::Audio && !t.events.empty() && !audioEvent) audioEvent = t.events[0].id;
        auto bodyLight = [&]() {
            const QRect r = tl->eventRectOf(audioEvent);
            const QImage img = tl->grab().toImage();
            const QColor c = img.pixelColor(std::min(img.width() - 1, r.left() + 3), std::min(img.height() - 1, r.bottom() - 3));
            return c.lightness();
        };
        const int before = bodyLight();
        prefs(*w, [](QDialog* d) { setCheck(d, "reverseEventBg", true); });
        uitest::settle(300);
        const int after = bodyLight();
        check(after > before + 30, "reverse event background: an audio event's background is lighter", std::to_string(before) + " -> " + std::to_string(after));
        prefs(*w, [](QDialog* d) { setCheck(d, "reverseEventBg", false); });
        uitest::settle(300);
        check(bodyLight() == before, "reverse event background: cleared - as before");
    }

    // ======================================================================== Questions: missing plug-ins, removing media, leaving
    {
        std::string err;
        const QString pr = QDir(out).absoluteFilePath("missing.reno");
        {
            app.newProject(true);
            app.edit("Fx", [](Project& p) {
                Track v;
                v.id = p.newId();
                v.type = TrackType::Video;
                FxSlot s;
                s.id = p.newId();
                s.kind = "ofx";
                s.pluginId = "com.nobody.NoSuchEffect";
                s.name = "No Such Effect";
                v.fx.push_back(s);
                p.tracks.push_back(v);
            });
            check(app.save(pr.toStdString(), &err), "a project with an effect nobody has was saved", err);
        }
        check(prefsapply::missingPlugins(app.project()) == std::vector<std::string>{"No Such Effect"}, "missing plug-ins: the effect nobody has is found");
        std::string last;
        auto previous = app.onStatus;
        app.onStatus = [&](const std::string& s) {
            last += s + "\n";
            if (previous) previous(s);
        };
        w->openProject(pr);
        uitest::settle(100);
        check(last.find("not installed") == std::string::npos, "missing plug-ins: nothing is said by default", last);
        prefs(*w, [](QDialog* d) { setCheck(d, "warnMissingPlugins", true); });
        last.clear();
        w->openProject(pr);
        uitest::settle(100);
        check(last.find("No Such Effect") != std::string::npos && last.find("not installed") != std::string::npos, "missing plug-ins: opening the project says which one is missing", last);
        app.onStatus = previous;
        prefs(*w, [](QDialog* d) { setCheck(d, "warnMissingPlugins", false); });

        // Ask before removing media: the Remove All Unused Media button
        app.newProject(true);
        app.importMedia("synth:4", &err);
        MediaBin* bin = nullptr;
        if (QDockWidget* dk = w->findChild<QDockWidget*>("dockMedia")) bin = findKind<MediaBin>(dk);
        check(bin != nullptr, "the Project Media window is there");
        if (bin) {
            prefs(*w, [](QDialog* d) { setCheck(d, "confirmRemoveMedia", true); });
            bool asked = false;
            QTimer::singleShot(300, [&] {
                if (QWidget* m = QApplication::activeModalWidget()) {
                    asked = true;
                    m->close();  // closed, not removed
                }
            });
            bin->removeUnusedMedia();
            check(asked && app.project().media.size() == 1, "removing media: asked first (and kept when the answer is not yes)");
            prefs(*w, [](QDialog* d) { setCheck(d, "confirmRemoveMedia", false); });
            check(bin->removeUnusedMedia() == 1 && app.project().media.empty(), "removing media: not asked by default");
        }

        // the 'leaving' part is run last: closing the main window leaves it in a state later dialogs can't open from
    }

    // ======================================================================== Plug-ins: reno_cli, start-up scan
    {
        prefs(*w, [](QDialog* d) {
            setText(d, "renoCliPath", "/bin/sh");
            setCheck(d, "scanAtStartup", false);
        });
        uitest::settle(100);
        check(defaultProbeExe() == "/bin/sh", "plug-ins: the reno_cli chosen is the one that checks plug-ins", defaultProbeExe());
        check(!QSettings().value("prefs/scanAtStartup", true).toBool() && !vprefs::on("prefs/scanAtStartup"), "plug-ins: 'look for new plug-ins when Reno starts' cleared is saved");
        prefs(*w, [](QDialog* d) {
            setText(d, "renoCliPath", QString());
            setCheck(d, "scanAtStartup", true);
        });
        uitest::settle(100);
        check(defaultProbeExe() != "/bin/sh", "plug-ins: cleared - the one next to Reno again", defaultProbeExe());
    }

    // ======================================================================== General: Wave64, QSV
    {
        prefs(*w, [](QDialog* d) {
            setGeneral(d, "prefs/wave64", true);
            setGeneral(d, "prefs/wave4gb", false);
            setGeneral(d, "prefs/qsv", false);
        });
        uitest::settle(100);
        check(vprefs::useWave64((qint64)3 << 30) && !vprefs::useWave64((qint64)1 << 30), "Wave64: a Wave render over 2 GB goes to a Wave64 file");
        prefs(*w, [](QDialog* d) { setGeneral(d, "prefs/wave4gb", true); });
        check(!vprefs::useWave64((qint64)3 << 30) && vprefs::useWave64((qint64)5 << 30), "Wave64: 'allow Wave renders up to 4 GB' moves the limit");
        prefs(*w, [](QDialog* d) { setGeneral(d, "prefs/wave64", false); });
        check(!vprefs::useWave64((qint64)5 << 30), "Wave64: off - never");
        check(!qsvEncodingAllowed(), "QSV: 'Enable QSV Encoding and Decoding' cleared - no render mode tries Quick Sync");
        prefs(*w, [](QDialog* d) {
            setGeneral(d, "prefs/wave64", true);
            setGeneral(d, "prefs/wave4gb", false);
            setGeneral(d, "prefs/qsv", true);
        });
        check(qsvEncodingAllowed(), "QSV: ... and allowed again");
    }

    // ======================================================================== Preview Device
    {
        uitest::buildDemoProject();  // a picture with something in it, and a project that can play
        uitest::settle(300);
        prefs(*w, [](QDialog* d) { setPick(d, "pdDevice", "none"); });
        ExternalMonitor em(pb, w);
        em.open();
        check(!em.isOpen(), "preview device: (None) - there is no external preview");
        prefs(*w, [](QDialog* d) { setPick(d, "pdDevice", "graphics"); });
        em.open();
        check(em.isOpen(), "preview device: Windows Graphics Card - the external preview opens");
        em.close();
        // the filters
        pb->requestFrame();
        uitest::settle(600);
        const QImage frame = pb->lastFrame();
        if (frame.isNull()) {
            check(false, "preview device: the preview has a picture to filter");
        } else {
            em.open();
            uitest::settle(100);
            const QImage plain = em.shown().convertToFormat(QImage::Format_RGBA8888);
            em.close();
            prefs(*w, [](QDialog* d) {
                setCheck(d, "pdStudioRgb", true);
                setCheck(d, "pdDeinterlace", true);
            });
            em.open();
            uitest::settle(100);
            const QImage filtered = em.shown().convertToFormat(QImage::Format_RGBA8888);
            em.close();
            int changed = 0;
            if (plain.size() == filtered.size())
                for (int y = 0; y < plain.height(); y += 7)
                    for (int x = 0; x < plain.width(); x += 7)
                        if (qGray(filtered.pixel(x, y)) != qGray(plain.pixel(x, y))) ++changed;
            check(plain.size() == filtered.size() && changed > 10, "preview device: the studio-RGB and deinterlace filters change the picture sent out", std::to_string(changed));
            prefs(*w, [](QDialog* d) {
                setCheck(d, "pdStudioRgb", false);
                setCheck(d, "pdDeinterlace", false);
            });
        }
        // the Video Preview during playback
        prefs(*w, [](QDialog* d) { setCheck(d, "pdShowInPreview", false); });
        PreviewWidget* pv = w->preview();
        QImage red(64, 36, QImage::Format_RGB32), blue(64, 36, QImage::Format_RGB32);
        red.fill(Qt::red);
        blue.fill(Qt::blue);
        pb->pause();
        pb->onFrame(red, 0.0, 1.0);
        uitest::settle(100);
        const QImage shownRed = pv->grab().toImage();
        pb->play();
        pb->onFrame(blue, 0.1, 1.0);
        uitest::settle(100);
        // the picture only: the strip at the bottom is the transport bar, which shows the time moving
        auto picture = [](const QImage& i) { return i.copy(0, 0, i.width(), std::max(1, i.height() - 70)); };
        check(picture(shownRed) == picture(pv->grab().toImage()), "preview device: with display frames cleared, the preview keeps its picture while playing");
        pb->pause();
        prefs(*w, [](QDialog* d) { setCheck(d, "pdShowInPreview", true); });
        pb->play();
        pb->onFrame(blue, 0.1, 1.0);
        uitest::settle(100);
        check(picture(pv->grab().toImage()) != picture(shownRed), "preview device: ... and shows it when set");
        pb->pause();
    }

    // ======================================================================== Audio Device: playback buffering
    {
        prefs(*w, [](QDialog* d) {
            if (QDoubleSpinBox* s = d->findChild<QDoubleSpinBox*>("playbackBuffer")) s->setValue(0.3);
            else check(false, "the dialog has playbackBuffer");
        });
        check(near(QSettings().value("prefs/playbackBuffer").toDouble(), 0.3) && near(vprefs::real("prefs/playbackBuffer"), 0.3),
              "playback buffering: saved (the sound card is opened with it the next time playback starts)");
        prefs(*w, [](QDialog* d) {
            if (QDoubleSpinBox* s = d->findChild<QDoubleSpinBox*>("playbackBuffer")) s->setValue(0.05);
        });
    }

    // ======================================================================== File I/O: the logs
    {
        QDir(logbook::folder()).removeRecursively();
        prefs(*w, [](QDialog* d) { setPick(d, "logReads", "detailed"); });
        std::string err;
        app.importMedia("synth:9", &err);
        QFile f(logbook::fileOf(logbook::Kind::Reads));
        check(f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).contains("Opened synth:9"), "log reads: a file brought in is written to reads.log");
        f.close();
        prefs(*w, [](QDialog* d) { setPick(d, "logReads", "off"); });
        const qint64 size = QFileInfo(logbook::fileOf(logbook::Kind::Reads)).size();
        app.importMedia("synth:10", &err);
        check(QFileInfo(logbook::fileOf(logbook::Kind::Reads)).size() == size, "log reads: off - nothing more is written");
    }

    // ======================================================================== Sync and MIDI
    {
        QSettings().setValue("prefs/midiOutputs", QStringList() << "Port B");
        vprefs::reload();
        const std::vector<int> only = availableMidiPorts({"Port A", "Port B", "Port C"}, true);
        check(only == std::vector<int>{1}, "MIDI: only the devices made available are offered");
        QSettings().remove("prefs/midiOutputs");
        vprefs::reload();
        check(availableMidiPorts({"Port A", "Port B"}, true).size() == 2, "MIDI: none cleared - all are offered");
        vprefs::set("midi/mtcRate", 1);
        check(MidiSync::sendRateCode(30.0) == 1, "sync: Generate MIDI Timecode > Frame rate 25 fps is used instead of the project's");
        check(near(MidiSync::timecodeLossMs(), 250.0), "sync: timecode that stops stops playback after a quarter of a second by default");
        vprefs::set("midi/freewheel", true);
        vprefs::set("midi/freewheelSlack", 0.5);
        vprefs::set("midi/freewheelPlayback", 2.0);
        check(near(MidiSync::timecodeLossMs(), 2500.0), "sync: free-wheel for timecode loss keeps playing through the slack and the free-wheel time");
        vprefs::set("midi/freewheel", false);
        vprefs::set("midi/mtcRate", -1);
        check(MidiSync::sendRateCode(30.0) == MidiSync::rateCode(30.0), "sync: 'use project frame rate' is the project's");
    }

    // ======================================================================== Leaving (last: closes the main window)
    {
        std::string err;
        // Ask to save changes when leaving: cleared, so the window closes with nothing asked and the changes are in the auto-save file
        app.newProject(true);
        app.save(QDir(out).absoluteFilePath("leave.reno").toStdString(), &err);
        app.edit("Leave", [](Project& p) { p.settings.tempo = 99; });
        check(app.dirty(), "an unsaved change to leave behind");
        const QString bak = QString::fromStdString(app.autoSavePath());
        QFile::remove(bak);
        bool asked = false;
        QTimer::singleShot(400, [&] {
            if (QWidget* m = QApplication::activeModalWidget()) {
                asked = true;
                for (QPushButton* b : m->findChildren<QPushButton*>())
                    if (b->text().contains("Cancel")) { b->click(); return; }
                m->close();
            }
        });
        const bool closedWithQuestion = w->close();
        check(asked && !closedWithQuestion, "leaving: asks to save changes by default (and stays when cancelled)");
        w->show();
        prefs(*w, [](QDialog* d) { setCheck(d, "confirmExit", false); });
        asked = false;
        QTimer::singleShot(400, [&] {
            if (QWidget* m = QApplication::activeModalWidget()) {
                asked = true;
                m->close();
            }
        });
        const bool closed = w->close();
        check(closed && !asked && QFile::exists(bak), "leaving: 'ask to save' cleared - nothing is asked, the changes are kept in the auto-save file");
        w->show();
        QSettings().setValue("prefs/confirmExit", true);
        vprefs::reload();
        uitest::settle(200);
    }

    uitest::shot(w, out + "/prefsmore_end.png");
    return uitest::summary();
}
