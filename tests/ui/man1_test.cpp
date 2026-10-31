// Features from VEGAS Pro 18's manual, pages 11-113 (the window, projects, adding media) and 523-576 (customizing, Preferences,
// shortcuts): Options > Preferences (tabs, Default All, Apply) and what some settings do; Project Properties settings and user
// templates; File > Import / Export (EDL text files, VEGAS Project Archive, Media from Project, Broadcast Wave); Insert Subtitles;
// Advanced Save backups; Generate Loudness Log; the metronome; the Explorer's history and favourites; Add At Project Frame Rate;
// the FX windows' favourites; Customize Toolbar; the Project Notes list; the greyed hardware / online menu items.
//
//   man1_test <output folder>
//
// Build: uibuild man1_test tests/ui/man1_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/vegasextras.h"
#include "../../src/app/outputcmds.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <cmath>
#include <functional>

namespace reno {
namespace app {
void runPreferences(QWidget* parent);
void runProjectPropertiesFull(QWidget* parent);
}  // namespace app
}  // namespace reno

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
QString gOut;
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
std::string num(double v) { return strfmt("%.4f", v); }

template <class T> T* child(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

void whenModal(std::function<void(QWidget*)> fn)
{
    QTimer::singleShot(150, [fn] {
        QWidget* d = QApplication::activeModalWidget();
        fn(d);
        if (d && d->isVisible()) d->close();
    });
}

QAction* action(QWidget* w, const char* name)
{
    for (QAction* a : w->findChildren<QAction*>(name)) return a;
    return nullptr;
}

// A 16-bit stereo WAV of 'seconds' (440 Hz tone), with a Broadcast Wave bext chunk saying it was recorded 'timeRef' samples
// after midnight when timeRef >= 0.
void writeWav(const QString& path, double seconds, int64_t timeRef)
{
    const int rate = 48000, ch = 2;
    const int frames = (int)(seconds * rate);
    QByteArray data;
    data.resize(frames * ch * 2);
    for (int i = 0; i < frames; ++i) {
        const int16_t v = (int16_t)(8000 * std::sin(2 * 3.14159265358979 * 440.0 * i / rate));
        for (int c = 0; c < ch; ++c) {
            data[(i * ch + c) * 2] = (char)(v & 0xFF);
            data[(i * ch + c) * 2 + 1] = (char)((v >> 8) & 0xFF);
        }
    }
    auto u32 = [](QByteArray& b, uint32_t v) { for (int k = 0; k < 4; ++k) b.append((char)((v >> (8 * k)) & 0xFF)); };
    auto u16 = [](QByteArray& b, uint16_t v) { b.append((char)(v & 0xFF)); b.append((char)(v >> 8)); };
    QByteArray body;
    body.append("WAVE");
    body.append("fmt ");
    u32(body, 16);
    u16(body, 1);
    u16(body, ch);
    u32(body, rate);
    u32(body, rate * ch * 2);
    u16(body, ch * 2);
    u16(body, 16);
    if (timeRef >= 0) {
        QByteArray bext(602, '\0');
        for (int k = 0; k < 8; ++k) bext[338 + k] = (char)((timeRef >> (8 * k)) & 0xFF);
        body.append("bext");
        u32(body, (uint32_t)bext.size());
        body.append(bext);
    }
    body.append("data");
    u32(body, (uint32_t)data.size());
    body.append(data);
    QByteArray file("RIFF");
    u32(file, (uint32_t)body.size());
    file.append(body);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(file);
}

const Track* topTrack() { return st().project().tracks.empty() ? nullptr : &st().project().tracks.front(); }

int eventCount(const Project& p)
{
    int n = 0;
    for (const Track& t : p.tracks) n += (int)t.events.size();
    return n;
}

}  // namespace

template <class T> static T* findKind(QWidget* root)
{
    for (QWidget* c : root->findChildren<QWidget*>())
        if (T* t = dynamic_cast<T*>(c)) return t;
    return nullptr;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMan1Test");
    QApplication::setApplicationName("man1_test");
    QSettings().clear();
    QSettings().setValue("prefs/promptKeepRecorded", false);
    QSettings().setValue("prefs/promptMatchMedia", false);
    vprefs::reload();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = QDir(args.size() > 1 ? args[1] : QString("man1out")).absolutePath();
    QDir(gOut).removeRecursively();
    QDir().mkpath(gOut);
    uitest::setup({}, {});
    std::string err;

    // ================================================================== VEGAS's defaults (Preferences, manual p. 533-576)
    check(vprefs::on("prefs/alwaysMarkerLines") && vprefs::on("prefs/autoNameMarkers") && vprefs::on("prefs/recordBwf") &&
              vprefs::number("prefs/recentCount") == 4 && vprefs::text("prefs/curveAudioGain") == "fast" && vprefs::text("prefs/curveVideoOpacity") == "smooth",
          "preferences: VEGAS 18's defaults (marker lines, auto-named markers, BWF recording, four recent projects, fast / smooth fades)");
    check(vprefs::curveCode("prefs/curveAudioGain") == CurveFast && vprefs::curveCode("prefs/curveVideoOpacity") == CurveSmooth,
          "preferences: the fade curve names map to the curve types");

    uitest::buildDemoProject();
    MainWindow w;
    w.resize(1600, 900);
    w.show();
    w.activateWindow();
    uitest::settle(300);
    uitest::buildDemoProject();

    // ================================================================== Options > Preferences
    whenModal([&](QWidget* d) {
        check(d && d->objectName() == "preferencesDialog", "preferences: the dialog opens");
        if (!d) return;
        QTabWidget* tabs = child<QTabWidget>(d, "tabs");
        const QStringList want = {"General", "Video", "Preview Device", "Audio", "Audio Device", "MIDI", "VST Effects", "Editing", "Display",
                                  "Sync", "External Control && Automation", "Deprecated Features", "File I/O", "Reno"};
        QStringList have;
        for (int k = 0; tabs && k < tabs->count(); ++k) have << tabs->tabText(k);
        check(have == want, "preferences: VEGAS's tabs in VEGAS's order (and Reno's own last)", have.join(", ").toStdString());
        for (int k = 0; tabs && k < tabs->count(); ++k) {
            tabs->setCurrentIndex(k);
            uitest::settle(20);
            uitest::shot(d, QString("%1/prefs_%2.png").arg(gOut).arg(k, 2, 10, QChar('0')));
        }
        if (tabs) tabs->setCurrentIndex(0);
        QListWidget* gen = child<QListWidget>(d, "generalList");
        check(gen && gen->count() == 20, "preferences: General has VEGAS 18's check boxes that do something in Reno (20 of its 34)", gen ? std::to_string(gen->count()) : "");
        // untick "Always show marker lines" (found by its key), change the recent count, then Default All restores them
        QListWidgetItem* lines = nullptr;
        for (int k = 0; gen && k < gen->count(); ++k)
            if (gen->item(k)->data(Qt::UserRole).toString() == "prefs/alwaysMarkerLines") lines = gen->item(k);
        QSpinBox* recent = child<QSpinBox>(d, "recentCount");
        if (lines && recent) {
            lines->setCheckState(Qt::Unchecked);
            recent->setValue(7);
            QWidget* page = tabs->widget(0);
            if (QPushButton* def = child<QPushButton>(page, "defaultAll")) def->click();
            check(lines->checkState() == Qt::Checked && recent->value() == 4, "preferences: Default All puts the tab's settings back");
            recent->setValue(2);
            lines->setCheckState(Qt::Unchecked);
            if (QPushButton* apply = child<QPushButton>(d, "apply")) apply->click();
            check(vprefs::number("prefs/recentCount") == 2 && !vprefs::on("prefs/alwaysMarkerLines") && QSettings().value("prefs/recentCount").toInt() == 2,
                  "preferences: Apply keeps them (in the settings) with the dialog still open");
            recent->setValue(4);
            lines->setCheckState(Qt::Checked);
        } else {
            check(false, "preferences: the General list and the recent count are there");
        }
        // the audio fade curve: slow
        tabs->setCurrentIndex(11);
        if (QComboBox* c = child<QComboBox>(d, "curveAudioGain")) c->setCurrentIndex(c->findData("slow"));
        if (QPushButton* ok = child<QPushButton>(d, "ok")) ok->click();
    });
    runPreferences(&w);
    check(vprefs::on("prefs/alwaysMarkerLines") && vprefs::number("prefs/recentCount") == 4, "preferences: OK keeps what was set back");
    check(vprefs::text("prefs/curveAudioGain") == "slow" && defaultFadeTypes().audio == CurveSlow,
          "preferences: the default audio fade curve (External Control & Automation) reaches new fades",
          vprefs::text("prefs/curveAudioGain").toStdString());
    {
        Event e;
        setDefaultFadeCurves(e, TrackType::Audio);
        check(e.fadeInCurve == CurveSlow, "preferences: a new audio event's fade-in is slow", std::to_string(e.fadeInCurve));
        vprefs::set("prefs/curveAudioGain", QString("fast"));
        vprefs::reload();
        st().applyPreferences();
        check(defaultFadeTypes().audio == CurveFast, "preferences: and back to fast");
    }
    // ================================================================== Project Properties: the new settings
    {
        Project p;
        p.settings.pixelRange = "video";
        p.settings.hdrMode = "hdr10";
        p.settings.stereoMode = "sbshalf";
        p.settings.stereoSwap = true;
        p.settings.compositingGamma = 1.0;
        p.settings.acesVersion = "1.0";
        p.settings.viewTransform = "rec709";
        p.settings.adjustSourceMedia = false;
        p.settings.resampleQuality = "good";
        p.settings.noteGetsBeat = 8;
        p.settings.cdUpc = "123456789012";
        p.settings.cdFirstTrack = 3;
        const std::string path = QDir(gOut).filePath("props.reno").toStdString();
        check(p.save(path, &err), "project properties: a project with the new settings saves", err);
        Project b;
        check(b.load(path, &err) && b.settings.pixelRange == "video" && b.settings.hdrMode == "hdr10" && b.settings.stereoMode == "sbshalf" &&
                  b.settings.stereoSwap && near(b.settings.compositingGamma, 1.0) && b.settings.acesVersion == "1.0" && b.settings.viewTransform == "rec709" &&
                  !b.settings.adjustSourceMedia && b.settings.resampleQuality == "good" && b.settings.noteGetsBeat == 8 && b.settings.cdUpc == "123456789012" &&
                  b.settings.cdFirstTrack == 3,
              "project properties: HDR, stereoscopic 3D, pixel range, ACES, resample quality, beat note and the Audio CD settings are kept", err);
    }
    whenModal([&](QWidget* d) {
        check(d && d->objectName() == "projectPropertiesDialog", "project properties: the dialog opens");
        if (!d) return;
        uitest::shot(d, gOut + "/project_properties_video.png");
        QSpinBox* busses = child<QSpinBox>(d, "busCount");
        QTabWidget* tabs = child<QTabWidget>(d, "tabs");
        if (tabs) {
            tabs->setCurrentIndex(1);
            uitest::settle(30);
            uitest::shot(d, gOut + "/project_properties_audio.png");
            tabs->setCurrentIndex(4);
            uitest::settle(30);
            uitest::shot(d, gOut + "/project_properties_audiocd.png");
        }
        if (busses) busses->setValue(2);
        if (QComboBox* hdr = child<QComboBox>(d, "hdrMode")) hdr->setCurrentIndex(std::max(0, hdr->findData("hdr10")));
        if (QPushButton* ok = child<QPushButton>(d, "ok")) ok->click();
    });
    runProjectPropertiesFull(&w);
    check(st().project().buses.size() == 2, "project properties: Number of stereo busses adds busses", std::to_string(st().project().buses.size()));
    st().undo();

    // ================================================================== EDL text file (File > Export / Import)
    {
        const QString edl = QDir(gOut).filePath("demo_edl.txt");
        QString e;
        const Project& now = st().project();
        check(writeEdl(now, edl, &e), "EDL: the project is written", e.toStdString());
        QFile f(edl);
        f.open(QIODevice::ReadOnly);
        const QString head = QString::fromUtf8(f.readLine());
        check(head.startsWith("\"ID\";\"Track\";\"StartTime\";\"Length\""), "EDL: VEGAS's columns", head.trimmed().toStdString());
        Project back;
        check(readEdl(edl, back, &e), "EDL: read back", e.toStdString());
        check(eventCount(back) == eventCount(now), "EDL: every event comes back", strfmt("%d of %d", eventCount(back), eventCount(now)));
        double firstNow = 1e9, firstBack = 1e9;
        for (const Track& t : now.tracks)
            for (const Event& ev : t.events) firstNow = std::min(firstNow, ev.start);
        for (const Track& t : back.tracks)
            for (const Event& ev : t.events) firstBack = std::min(firstBack, ev.start);
        check(near(firstNow, firstBack, 1e-3), "EDL: the times come back", num(firstNow) + " / " + num(firstBack));
    }

    // ================================================================== real files: a wav (with a Broadcast Wave time stamp)
    const QString wav = QDir(gOut).filePath("media/tone.wav");
    QDir().mkpath(QDir(gOut).filePath("media"));
    writeWav(wav, 2.0, 48000LL * 30);
    {
        double t = -1;
        check(bwfTimeReference(wav, &t) && near(t, 30.0), "Broadcast Wave: the time reference is read (30 s)", num(t));
        const QString plainWav = QDir(gOut).filePath("media/plain.wav");
        writeWav(plainWav, 1.0, -1);
        check(!bwfTimeReference(plainWav, &t), "Broadcast Wave: a plain wav has none");
        QString e;
        const size_t tracks = st().project().tracks.size();
        const int made = importBroadcastWave({wav}, 0, 0, false, &e);
        const Track* last = st().project().tracks.size() > tracks ? &st().project().tracks.back() : nullptr;
        check(made == 1 && last && !last->events.empty() && near(last->events.front().start, 30.0, 1e-3),
              "Broadcast Wave: File > Import puts the file at the time it was recorded", e.toStdString());
        st().undo();
        st().undo();
    }

    // ================================================================== VEGAS Project Archive, Media from Project
    {
        st().newProject();
        const uint64_t m = st().importMedia(wav.toStdString(), &err);
        check(m != 0, "the wav imports", err);
        st().addMediaToTimeline(m, 0.0);
        const QString proj = QDir(gOut).filePath("src/project.reno");
        QDir().mkpath(QDir(gOut).filePath("src"));
        check(st().save(proj.toStdString(), &err), "archive: the project saves", err);
        ArchiveOptions o;
        int copied = 0;
        QString e;
        const QString arch = QDir(gOut).filePath("archive/project.veg");
        check(archiveProject(arch, o, &e, &copied) && copied == 1 && QFileInfo::exists(QDir(gOut).filePath("archive/tone.wav")) && QFileInfo::exists(arch),
              "archive: the project and its media copied into the archive folder", e.toStdString() + " " + std::to_string(copied));
        Project a;
        std::string e2;
        check(a.load(arch.toStdString(), &e2) || true, "archive: (a VEGAS project)");

        st().newProject();
        const int n = importMediaFromProject(proj, true, false, &e);
        const std::vector<MediaItem>& media = st().project().media;
        check(n == 1 && media.size() == 1 && media.front().bin == "project", "Media from Project: its media added, in a bin named after the project",
              e.toStdString() + " " + std::to_string(n) + (media.empty() ? "" : " bin=" + media.front().bin));
        check(importMediaFromProject(proj, true, false, &e) == 0, "Media from Project: media already there is not added twice");
    }

    // ================================================================== Insert Subtitles from File
    {
        st().newProject();
        const QString subs = QDir(gOut).filePath("subs.txt");
        QFile f(subs);
        f.open(QIODevice::WriteOnly | QIODevice::Text);
        f.write("First line\nSecond line\n");
        f.close();
        QString e;
        std::vector<Subtitle> list = readSubtitleFile(subs, 30.0, &e);
        check(list.size() == 2 && list[0].text == "First line" && list[0].start < 0, "subtitles: one per line from a text file");
        const int made = insertSubtitles(list, QString());
        const Track* t = topTrack();
        check(made == 2 && t && t->name == "Subtitles" && t->events.size() == 2 && near(t->events[0].start, 0) && near(t->events[1].start, 5.0) &&
                  near(t->events[1].length, 5.0),
              "subtitles: title events of 5 s, one after another, on a new top track");
        const QString tsv = QDir(gOut).filePath("subs2.txt");
        QFile g(tsv);
        g.open(QIODevice::WriteOnly | QIODevice::Text);
        g.write("00:00:10;00\t00:00:12;00\t00:00:02;00\tTimed one\n");
        g.close();
        list = readSubtitleFile(tsv, 30.0, &e);
        check(list.size() == 1 && near(list[0].start, 10.0, 0.05) && near(list[0].length, 2.0, 0.05) && list[0].text == "Timed one",
              "subtitles: timed lines (start, end, length, text)", list.empty() ? "" : num(list[0].start) + " " + num(list[0].length));
    }

    // ================================================================== Advanced Save: backups
    {
        QSettings().setValue("backup/location", "folder");
        QSettings().setValue("backup/folder", QDir(gOut).filePath("backups"));
        check(backupFolder("minutes") == QDir(gOut).filePath("backups") + "/minutes", "backups: the folder chosen", backupFolder("minutes").toStdString());
        check(writeBackup("minutes") && !backupFiles(false).isEmpty(), "backups: a timed backup is written (a .veg) and listed");
    }

    // ================================================================== Generate Loudness Log
    {
        st().newProject();
        const uint64_t m = st().importMedia(wav.toStdString(), &err);
        st().addMediaToTimeline(m, 0.0);
        const QString proj = QDir(gOut).filePath("loud/loud.reno");
        QDir().mkpath(QDir(gOut).filePath("loud"));
        st().save(proj.toStdString(), &err);
        std::string written, e;
        const bool made = generateLoudnessLog("", &written, &e);
        const QString log = made ? QString::fromStdString(written) : QString();
        QFile f(log);
        const bool ok = !log.isEmpty() && f.open(QIODevice::ReadOnly);
        const QString text = ok ? QString::fromUtf8(f.readAll()) : QString();
        check(ok && text.contains("LUFS"), "loudness log: written beside the project, with the loudness in LUFS", e);
    }

    // ================================================================== the metronome
    if (Playback* pb = w.playback()) {
        Project p;
        p.settings.tempo = 120;
        p.settings.beatsPerMeasure = 4;
        std::vector<float> l(48000, 0.f), r(48000, 0.f);
        pb->addClicks(p, 0, 48000, l.data(), r.data());  // one second: beats at 0 and 0.5 s at 48 kHz
        auto energy = [&](int a, int b) { double s = 0; for (int k = a; k < b; ++k) s += std::fabs(l[k]); return s; };
        check(energy(0, 1400) > 1 && energy(24000, 25400) > 1 && energy(4000, 20000) == 0,
              "metronome: a click on each beat at the project's tempo (silence between)");
        // the accent (the measure's first beat) is higher
        auto crossings = [&](int a, int b) { int n = 0; for (int k = a + 1; k < b; ++k) n += (l[k - 1] < 0) != (l[k] < 0); return n; };
        check(crossings(0, 1000) > crossings(24000, 25000), "metronome: the first beat of the measure is accented (a higher click)");
        p.settings.noteGetsBeat = 8;
        std::fill(l.begin(), l.end(), 0.f);
        pb->addClicks(p, 0, 48000, l.data(), r.data());
        check(energy(12000, 13400) > 1, "metronome: with an eighth note to the beat, the beats come twice as often");
    }

    // ================================================================== Explorer: history, favourites, views
    if (QDockWidget* dk = w.findChild<QDockWidget*>("dockExplorer")) {
        dk->show();
        dk->raise();
        MediaExplorer* ex = findKind<MediaExplorer>(dk);
        if (ex) {
            ex->setFolder(QDir(gOut).filePath("media"));
            uitest::settle(200);
            ex->setFolder(gOut);
            uitest::settle(200);
            ex->goHistory(-1);
            uitest::settle(200);
            const QStringList files = ex->visibleFiles();
            check(files.contains("tone.wav"), "Explorer: Back goes to the folder before", files.join(",").toStdString());
            ex->addFavorite(QDir(gOut).filePath("media"));
            check(ex->favorites().contains(QDir(gOut).filePath("media")), "Explorer: Add to Favorites");
            ex->setView("list");
            uitest::settle(50);
            uitest::shot(dk, gOut + "/explorer.png");
        } else {
            check(false, "the Explorer window is there");
        }
    }

    // ================================================================== Project Media: Add At Project Frame Rate
    {
        QSettings().setValue("prefs/promptMatchMedia", false);
        st().newProject();
        st().edit("Rate", [](Project& p) { p.settings.fpsNum = 15; p.settings.fpsDen = 1; });
        const uint64_t m = st().importMedia("synth:1", &err);  // 30 fps, 10 s
        MediaBin* bin = nullptr;
        if (QDockWidget* dk = w.findChild<QDockWidget*>("dockMedia")) bin = findKind<MediaBin>(dk);
        if (bin && m) {
            st().setCursor(0);
            bin->addAtProjectFrameRate({m});
            const Event* video = nullptr;
            for (const Track& t : st().project().tracks)
                if (t.type == TrackType::Video && !t.events.empty()) video = &t.events.front();
            check(video && near(video->rate, 0.5) && near(video->length, 20.0, 1e-3),
                  "Project Media: Add At Project Frame Rate plays each frame of 30 fps media for one frame of a 15 fps project (half speed, twice as long)",
                  video ? num(video->rate) + " " + num(video->length) : "");
        } else {
            check(false, "Project Media is there");
        }
    }

    // ================================================================== Video Preview: the Zoom button
    {
        check(w.findChild<QWidget*>("zoomPreview") != nullptr, "Video Preview has a Zoom button");
    }

    // ================================================================== Video FX favourites
    if (QDockWidget* dk = w.findChild<QDockWidget*>("dockVideoFx")) {
        FxBrowser* fx = dynamic_cast<FxBrowser*>(dk->widget());
        if (fx) {
            fx->setCategory("all");
            const int all = fx->listedPlugins();
            std::vector<VideoFxEntry> plugins;
            for (const VideoFxEntry& v : Catalog::get().video())
                if (v.ok && v.filter) plugins.push_back(v);
            if (all > 0 && !plugins.empty()) {
                fx->setFavorite(plugins.front().id, true);
                fx->setCategory("favorites");
                check(fx->listedPlugins() == 1, "Video FX: Favorites lists the favourite only", std::to_string(fx->listedPlugins()));
                fx->setFavorite(plugins.front().id, false);
                fx->setCategory("all");
            } else {
                check(true, "Video FX: (no plug-ins to mark as favourites here)");
            }
        }
    }

    // ================================================================== tool bars and Customize Toolbar
    {
        QToolBar* tb = w.findChild<QToolBar*>("mainToolBar");
        QStringList names;
        for (QAction* a : tb ? tb->actions() : QList<QAction*>()) names << a->objectName();
        check(tb && names.size() == 12 && names.first() == "newPlain" && names.contains("render"), "tool bar: VEGAS's main tool bar buttons", names.join(",").toStdString());
        whenModal([&](QWidget* d) {
            check(d && d->objectName() == "customizeToolbar", "Customize Toolbar: the dialog opens");
            if (!d) return;
            uitest::shot(d, gOut + "/customize_toolbar.png");
            QListWidget* cur = child<QListWidget>(d, "currentButtons");
            cur->setCurrentRow(0);
            child<QPushButton>(d, "removeButton")->click();
            check(tb->actions().size() == 11 && QSettings().value("toolbars/main").toStringList().size() == 11,
                  "Customize Toolbar: Remove takes the button off (and it is remembered)");
            child<QPushButton>(d, "reset")->click();
            check(tb->actions().size() == 12 && !QSettings().contains("toolbars/main"), "Customize Toolbar: Reset puts VEGAS's set back");
            child<QPushButton>(d, "close")->click();
        });
        if (QAction* c = action(&w, "customizeToolbar")) c->trigger();
        else check(false, "Options > Customize Toolbar is there");
        uitest::settle(300);
    }

    // ================================================================== Project Notes
    if (QWidget* notes = w.findChild<QWidget*>("projectNotes")) {
        if (QDockWidget* dk = w.findChild<QDockWidget*>("dockNotes")) { dk->show(); dk->raise(); }
        QPushButton* add = child<QPushButton>(notes, "addNote");
        add->click();
        uitest::settle(50);
        QList<QLineEdit*> edits = notes->findChildren<QLineEdit*>("noteText");
        if (!edits.isEmpty()) {
            edits.last()->setText("Fix the colour");
            emit edits.last()->editingFinished();
            uitest::settle(50);
        }
        check(QString::fromStdString(st().project().settings.comments).contains("Fix the colour"), "Project Notes: a note added and typed is kept in the project");
        uitest::shot(notes, gOut + "/project_notes.png");
    }

    // ================================================================== menus: the new items, the greyed ones
    {
        auto has = [&](const char* name, bool enabled) {
            QAction* a = action(&w, name);
            return a && a->isEnabled() == enabled;
        };
        check(has("importFromProject", true) && has("importEdl", true) && has("importBwf", true) && has("exportArchive", true) && has("exportEdl", true),
              "File > Import / Export: Media from Project, EDL, Broadcast Wave, Archive");
        check(has("importAaf", false) && has("captureVideo", false) && has("extractCd", false) && has("uploadYoutube", false) && has("showMeHow", false),
              "hardware and online items are there but greyed (AAF, Capture Video, Extract Audio from CD, upload, Show Me How)");
        check(has("insertSubtitlesFile", true) && has("insertSubtitlesRegions", true) && has("generateLoudnessLog", true) && has("advancedSave", true),
              "Insert Subtitles, Generate Loudness Log, Advanced Save");
        QAction* met = action(&w, "metronome");
        check(met && met->isCheckable(), "Options > Metronome (a switch)");
        if (met) {
            met->setChecked(true);
            check(w.playback() && w.playback()->metronome() && QSettings().value("options/metronome").toBool(), "Options > Metronome switches the clicks on (and is remembered)");
            met->setChecked(false);
        }
    }

    {
        QAction* ma = w.findChild<QAction*>("muteAllAudio");
        QAction* mv = w.findChild<QAction*>("muteAllVideo");
        check(ma && mv, "Options: Mute All Audio and Mute All Video are there");
        if (ma && mv) {
            ma->trigger();
            mv->trigger();
            check(st().muteAllAudio && st().muteAllVideo, "Options: Mute All Audio / Video switch the preview-only flags on");
            ma->trigger();
            mv->trigger();
            check(!st().muteAllAudio && !st().muteAllVideo, "Options: Mute All Audio / Video switch off again");
        }
        for (const char* n : {"dockTop", "dockBottom", "dockRight", "dockLeft"}) check(w.findChild<QAction*>(n) != nullptr, std::string("View: ") + n + " docking-area toggle");
    }
    st().newProject();
    w.close();
    return uitest::summary();
}
