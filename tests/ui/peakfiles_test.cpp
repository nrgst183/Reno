// The timeline's waveforms and VEGAS's peak files: a waveform is taken from a current .sfk beside the media (VEGAS's own, used as
// is), made and written there as VEGAS writes it when there is none or it is stale, kept in Reno's own folder when the media's
// folder can't be written, and made again by Rebuild Audio Peaks; Preferences > Audio has the two switches; Tools has Rebuild
// Offline Nested Audio.
//
//   peakfiles_test <output folder> [testfx.ofx ...]
#include "uitestlib.h"
#include "../../src/app/appstate.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/mediacache.h"
#include "../../src/media/vegasfiles.h"
#include "../testdata.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenuBar>
#include <QTimer>
#include <QPushButton>
#include <QSettings>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>

using namespace reno;
using namespace reno::app;
using uitest::check;

// VEGAS's own .sfk files and their .wav (folder in RENO_VEGAS_FILES; not in the repository)
static std::string fixtures() { return testDataDir("RENO_VEGAS_FILES"); }

static bool copyFile(const std::string& from, const std::string& to)
{
    std::string d;
    return readFile(from, d) && writeFile(to, d);
}

static int64_t mtimeNs(const std::string& p)
{
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return 0;
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

// Waits until 'cond' holds (or a few seconds have passed).
static bool waitFor(const std::function<bool()>& cond, int ms = 15000)
{
    for (int k = 0; k < ms / 50; ++k) {
        if (cond()) return true;
        uitest::settle(50);
    }
    return cond();
}

// A new project with the file at 'path' on an audio track (its waveform asked for by the timeline).
static void showFile(MainWindow& w, const std::string& path)
{
    AppState& app = AppState::get();
    app.newProject(true);
    std::string err;
    const uint64_t id = app.importMedia(path, &err);
    if (id) app.addMediaToTimeline(id, 0.0);
    w.timeline()->zoomToFit();
    uitest::settle(300);
}

static int rpkCount()
{
    return QDir(QString::fromStdString(pathJoin(pathJoin(userDataDir(), "cache"), "peaks"))).entryList(QStringList() << "*.rpk", QDir::Files).size();
}

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    QApplication::setOrganizationName("RenoPeakFilesTest");
    QApplication::setApplicationName("peakfiles_test");
    QSettings().clear();
    const std::string out = argc > 1 ? argv[1] : "peakfiles_out";
    QDir().mkpath(QString::fromStdString(out));
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.prefs.vegasPeakFiles = true;
    app.prefs.createAudioProxies = true;
    app.applyPreferences();
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    uitest::settle(300);
    // ---------------------------------------------------------------- a 5.1 file: no .sfk is written (made here)
    {
        const std::string dir = pathJoin(out, "multi");
        QDir().mkpath(QString::fromStdString(dir));
        const std::string wav = pathJoin(dir, "six.wav"), sfk = pathJoin(dir, "six.sfk");
        const int rate = 48000, frames = rate * 2, ch = 6;
        std::string d;
        auto p32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) d += (char)((v >> (8 * k)) & 255); };
        auto p16 = [&](uint32_t v) { d += (char)(v & 255); d += (char)((v >> 8) & 255); };
        d += "RIFF"; p32((uint32_t)(36 + frames * ch * 2)); d += "WAVEfmt "; p32(16); p16(1); p16(ch); p32(rate); p32(rate * ch * 2); p16(ch * 2); p16(16);
        d += "data"; p32((uint32_t)(frames * ch * 2));
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < ch; ++c) p16((uint32_t)(uint16_t)(int16_t)(2000 * (c + 1) * ((i / 100) % 2 ? 1 : -1)));
        writeFile(wav, d);
        showFile(w, wav);
        // no .sfk of more than two channels made by VEGAS has been seen, so none is written for VEGAS to misread
        uitest::settle(4000);
        check(!fileExists(sfk), "a 6-channel file gets no .sfk beside it (its waveform is cached by Reno)");
    }

    const std::string fx = fixtures();
    if (testDataMissing(pathJoin(fx, "peaks_b.sfk"))) return uitest::summary();

    // ---------------------------------------------------------------- VEGAS's own .sfk, current: used as it is
    {
        const std::string dir = pathJoin(out, "vegas");
        QDir().mkpath(QString::fromStdString(dir));
        const std::string wav = pathJoin(dir, "peaks_b.wav"), sfk = pathJoin(dir, "peaks_b.sfk");
        copyFile(pathJoin(fx, "peaks_b.wav"), wav);
        copyFile(pathJoin(fx, "peaks_b.sfk"), sfk);
        // the copy given the time the original had when VEGAS made the .sfk (2023-01-13 19:01:49.924 UTC)
        struct utimbuf t;
        t.actime = t.modtime = 1673636509;
        utime(wav.c_str(), &t);
        vegasfiles::PeakFile pf;
        check(vegasfiles::readPeakFile(sfk, pf) && vegasfiles::peakFileCurrent(pf, wav), "VEGAS's peaks_b.sfk is current for the copy of peaks_b.wav");
        const int64_t before = mtimeNs(sfk);
        const int rpk = rpkCount();
        showFile(w, wav);
        uitest::settle(1500);
        check(mtimeNs(sfk) == before && rpkCount() == rpk, "its waveform is taken from VEGAS's .sfk: nothing is read or written again");
        uitest::shot(&w, QString::fromStdString(pathJoin(out, "vegas_sfk.png")));
    }

    // ---------------------------------------------------------------- none: made and written beside the media, as VEGAS writes it
    {
        const std::string dir = pathJoin(out, "new");
        QDir().mkpath(QString::fromStdString(dir));
        const std::string wav = pathJoin(dir, "peaks_a.wav"), sfk = pathJoin(dir, "peaks_a.sfk");
        copyFile(pathJoin(fx, "peaks_a.wav"), wav);
        showFile(w, wav);
        check(waitFor([&] { return fileExists(sfk); }), "a .sfk is written beside a file that has none (peaks_a.wav -> peaks_a.sfk)");
        std::string mine, theirs;
        readFile(sfk, mine);
        readFile(pathJoin(fx, "peaks_a.sfk"), theirs);
        check(mine.size() == theirs.size() && mine.size() > 64 && mine.compare(64, std::string::npos, theirs, 64, std::string::npos) == 0 &&
                  mine.compare(16, 20, theirs, 16, 20) == 0,
              "with VEGAS's numbers: the same header (but the stamps) and the same peaks as VEGAS's own peaks_a.sfk");
        vegasfiles::PeakFile pf;
        check(vegasfiles::readPeakFile(sfk, pf) && vegasfiles::peakFileCurrent(pf, wav), "stamped with the file's time of last change");

        // stale: VEGAS's .sfk of a file that has changed since (here a copy with a new time) is made again
        {
            const std::string sd = pathJoin(out, "stale");
            QDir().mkpath(QString::fromStdString(sd));
            const std::string sw = pathJoin(sd, "peaks_a.wav"), ss = pathJoin(sd, "peaks_a.sfk");
            copyFile(pathJoin(fx, "peaks_a.wav"), sw);
            copyFile(pathJoin(fx, "peaks_a.sfk"), ss);
            check(vegasfiles::readPeakFile(ss, pf) && !vegasfiles::peakFileCurrent(pf, sw), "VEGAS's peaks_a.sfk is stale for a copy of peaks_a.wav made now");
            const int64_t before = mtimeNs(ss);
            showFile(w, sw);
            check(waitFor([&] { return mtimeNs(ss) != before; }), "a stale .sfk is made again");
            check(vegasfiles::readPeakFile(ss, pf) && vegasfiles::peakFileCurrent(pf, sw), "and is current then");
        }

        // Rebuild Audio Peaks makes it again even when current
        showFile(w, wav);
        const int64_t again = mtimeNs(sfk);
        uitest::settle(1100);
        w.timeline()->rebuildPeaks();
        check(waitFor([&] { return mtimeNs(sfk) != again; }), "Rebuild Audio Peaks writes the .sfk again from the file");
    }

    // ---------------------------------------------------------------- a file named without its length (as a .veg names its media)
    {
        const std::string dir = pathJoin(out, "nolength");
        QDir().mkpath(QString::fromStdString(dir));
        const std::string wav = pathJoin(dir, "peaktest.wav"), sfk = pathJoin(dir, "peaktest.sfk");
        copyFile(pathJoin(fx, "peaks_b.wav"), wav);
        app.newProject(true);
        std::string err;
        const uint64_t id = app.importMedia(wav, &err);
        app.edit("Forget the length", [id](Project& p) {
            if (MediaItem* m = p.findMedia(id)) {
                m->info.duration = 0;
                m->info.sampleRate = m->info.channels = 0;
                m->info.audioCodec.clear();
            }
        });
        uint64_t track = app.addTrack(TrackType::Audio);
        app.edit("An event", [id, track](Project& p) {
            for (Track& t : p.tracks)
                if (t.id == track) {
                    Event e;
                    e.id = p.newId();
                    e.mediaId = id;
                    e.length = 0.19;
                    t.events.push_back(e);
                }
        });
        w.timeline()->zoomToFit();
        check(waitFor([&] { return fileExists(sfk); }), "a file whose length the project does not give still gets its waveform (looked at first) and its .sfk");
    }

    // ---------------------------------------------------------------- the .sfk cannot be written: Reno's own cache
    {
        // a folder in the way of the file stands for a folder that can't be written (root, who runs these tests, could write anyway)
        const std::string dir = pathJoin(out, "readonly");
        QDir().mkpath(QString::fromStdString(pathJoin(dir, "peaks_b.sfk.part")));
        const std::string wav = pathJoin(dir, "peaks_b.wav");
        copyFile(pathJoin(fx, "peaks_b.wav"), wav);
        const int rpk = rpkCount();
        showFile(w, wav);
        check(waitFor([&] { return rpkCount() > rpk; }) && !fileExists(pathJoin(dir, "peaks_b.sfk")),
              "where the .sfk cannot be written beside the media, the waveform goes to Reno's cache");
    }

    // ---------------------------------------------------------------- turned off: Reno's cache only
    {
        app.prefs.vegasPeakFiles = false;
        app.applyPreferences();
        const std::string dir = pathJoin(out, "off");
        QDir().mkpath(QString::fromStdString(dir));
        const std::string wav = pathJoin(dir, "peaktest.wav");
        copyFile(pathJoin(fx, "peaks_a.wav"), wav);
        const int rpk = rpkCount();
        showFile(w, wav);
        check(waitFor([&] { return rpkCount() > rpk; }) && !fileExists(pathJoin(dir, "peaktest.sfk")), "with the preference off, no .sfk is written (Reno's cache)");
        app.prefs.vegasPeakFiles = true;
        app.applyPreferences();
    }

    // ---------------------------------------------------------------- the preference and the menu
    {
        QDialog* prefs = nullptr;
        QTimer::singleShot(400, [&] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (t->objectName() == "preferencesDialog" && t->isVisible()) prefs = qobject_cast<QDialog*>(t);
            QCheckBox* a = prefs ? prefs->findChild<QCheckBox*>("createAudioProxies") : nullptr;
            QCheckBox* b = prefs ? prefs->findChild<QCheckBox*>("vegasPeakFiles") : nullptr;
            check(a && b && a->isChecked() && b->isChecked(), "Preferences > Audio: Create audio proxy files, Keep waveforms in peak files (both on)");
            if (a) a->setChecked(false);
            if (prefs) prefs->findChild<QPushButton*>("ok") ? prefs->findChild<QPushButton*>("ok")->click() : prefs->reject();
        });
        bool found = false;
        for (QAction* m : w.menuBar()->actions())
            if (m->menu())
                for (QAction* a : m->menu()->actions()) {
                    if (a->text().remove('&') == "Preferences...") {
                        a->trigger();
                        found = true;
                    }
                }
        uitest::settle(600);
        check(found && !app.prefs.createAudioProxies, "unticking it turns building audio proxies off");
        app.prefs.createAudioProxies = true;
        app.applyPreferences();
        QAction* rebuild = nullptr;
        for (QAction* m : w.menuBar()->actions())
            if (m->menu() && m->text().remove('&') == "Tools")
                for (QAction* a : m->menu()->actions())
                    if (a->text().remove('&') == "Rebuild Offline Nested Audio") rebuild = a;
        check(rebuild != nullptr, "Tools > Rebuild Offline Nested Audio");
        app.newProject(true);
        if (rebuild) rebuild->trigger();
        uitest::settle(100);
    }
    return uitest::summary();
}
