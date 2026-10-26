// VEGAS projects as Reno's own project files, through the main window: File > Open of a .veg makes it the project (Save writes
// back to it, keeping the old one as .bak), new projects are saved as .veg, a .reno file still opens and saves as .reno, File lists
// the recent projects, and what Reno can't use or couldn't save is shown in a window.
//
//   vegfile_test <output folder> [folder with sample .veg files]
//   (the sample VEGAS projects default to the folder named by $RENO_SAMPLES; without them the test SKIPs. It uses the project
//    named by $RENO_SAMPLE_VEG, else the first sample with at least two tracks that both have events)
//
// Build: uibuild vegfile_test tests/ui/vegfile_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/vegas/vegas.h"
#include "uitestlib.h"
#include "../testdata.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSettings>
#include <cmath>
#include <set>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

static std::string bytesOf(const QString& path)
{
    std::string s;
    readFile(path.toStdString(), s);
    return s;
}

static QDialog* window(MainWindow& w, const char* name)
{
    for (QDialog* d : w.findChildren<QDialog*>(name))
        if (d->isVisible()) return d;
    return nullptr;
}

static void closeWindows(MainWindow& w)
{
    for (QDialog* d : w.findChildren<QDialog*>()) d->close();
    uitest::settle(50);
}

static QAction* action(MainWindow& w, const char* name)
{
    for (QAction* a : w.findChildren<QAction*>(name)) return a;
    return nullptr;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoVegFileTest");
    QApplication::setApplicationName("vegfile_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    const QString out = QDir(args.size() > 1 ? args[1] : QString("vegfile")).absolutePath();
    const QString samples = args.size() > 2 ? args[2] : QString(qEnvironmentVariable("RENO_SAMPLES"));
    // a sample with two tracks of events; best one track of picture and one of sound with an event each (the nested timeline part needs that)
    QString orig = QString::fromStdString(testSampleVeg());
    bool simple = false;
    auto isSimple = [](const Project& pr) {
        return pr.tracks.size() == 2 && pr.tracks[0].type == TrackType::Video && pr.tracks[1].type == TrackType::Audio && pr.tracks[0].events.size() == 1 &&
               pr.tracks[1].events.size() == 1;
    };
    if (orig.isEmpty()) {
        for (const std::string& f : testVegFiles(samples.toStdString())) {
            Project pr;
            if (!importVegasProject(f, pr).ok || pr.tracks.size() < 2 || pr.tracks[0].events.empty() || pr.tracks[1].events.empty()) continue;
            if (orig.isEmpty() || (!simple && isSimple(pr))) { orig = QString::fromStdString(f); simple = isSimple(pr); }
            if (simple) break;
        }
    } else {
        Project pr;
        simple = importVegasProject(orig.toStdString(), pr).ok && isSimple(pr);
    }
    if (orig.isEmpty() || !QFileInfo(orig).exists()) { std::printf("SKIP: no sample project with two tracks of events (set RENO_SAMPLES or RENO_SAMPLE_VEG, or pass the folder)\nSUMMARY: 0 passed, 0 failed\n"); return 0; }
    QDir(out).removeRecursively();
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    MainWindow w;
    w.resize(1400, 850);
    w.show();
    uitest::settle(200);

    // ---------------------------------------------------------------- File > Open of a .veg
    const QString veg = out + "/sample.veg";
    QFile::copy(orig, veg);
    check(w.openProject(veg), "a .veg opens");
    uitest::settle(100);
    check(st().path() == veg.toStdString(), "the .veg is the project's file (not an unsaved import)", st().path());
    check(!st().dirty(), "it opens unchanged");
    check(w.windowTitle().startsWith("sample.veg"), "the title names the .veg", w.windowTitle().toStdString());
    check(st().project().tracks.size() >= 2, "its tracks are there");
    QDialog* notes = window(w, "importNotes");
    if (notes) {       // (listed when something in the project cannot be used, e.g. its media is not on this machine)
        uitest::shot(notes, out + "/notes.png");
        const QString text = notes->findChild<QPlainTextEdit*>()->toPlainText();
        check(!text.contains("VST") && !text.contains("DirectX"), "plug-ins found by id are not listed as problems");
    }
    closeWindows(w);
    check(QSettings().value("recentProjects").toStringList().value(0) == QFileInfo(veg).absoluteFilePath(), "it is first among the recent projects");

    // ---------------------------------------------------------------- Save writes back to it
    action(w, "save")->trigger();
    uitest::settle(100);
    check(st().path() == veg.toStdString() && !st().dirty(), "Save keeps the .veg as the project's file");
    check(bytesOf(veg + ".bak") == bytesOf(orig), "the file that was there is kept as sample.veg.bak");
    check(isVegasProject(veg.toStdString()), "what was saved is a VEGAS project");
    check(window(w, "saveWarnings") == nullptr, "an unchanged project saves with nothing left out");
    const std::string first = bytesOf(veg);
    action(w, "save")->trigger();
    uitest::settle(100);
    check(bytesOf(veg) == first, "saved again unchanged: the same bytes");
    {
        // only the project's own path (in the file's header) differs from what VEGAS wrote: read back, the same project
        Project a, b;
        importVegasProject(orig.toStdString(), a);
        importVegasProject(veg.toStdString(), b);
        check(a.tracks.size() == b.tracks.size() && a.tracks[0].events.size() == b.tracks[0].events.size() &&
                  a.tracks[0].events[0].start == b.tracks[0].events[0].start && a.media.size() == b.media.size(),
              "what was saved reads back as what was opened");
    }

    // ---------------------------------------------------------------- an edit, saved and opened again
    st().edit("Rename", [](Project& p) {
        p.tracks[0].name = "Renamed track";
        p.tracks[1].events[0].fadeIn = 0.5;
        p.tracks[1].events[0].fadeInCurve = CurveSlow;
    });
    check(st().dirty() && w.windowTitle().contains("*"), "an edit marks it changed");
    action(w, "save")->trigger();
    uitest::settle(100);
    check(!st().dirty(), "saved");
    check(w.openProject(veg), "opened again");
    closeWindows(w);
    check(st().project().tracks[0].name == "Renamed track" && std::fabs(st().project().tracks[1].events[0].fadeIn - 0.5) < 1e-6 &&
              st().project().tracks[1].events[0].fadeInCurve == CurveSlow,
          "the edits are in the .veg");

    // ---------------------------------------------------------------- nested timelines (Alt+C, Alt+N)
    if (simple) {
        std::set<uint64_t> all;
        double t0 = 1e9, t1 = 0;
        for (const auto& t : st().project().tracks)
            for (const auto& e : t.events) {
                all.insert(e.id);
                t0 = std::min(t0, e.start);
                t1 = std::max(t1, e.end());
            }
        st().setSelection(all);
        const size_t mediaBefore = st().project().media.size();
        action(w, "nestCreate")->trigger();
        uitest::settle(100);
        const QString nested = out + "/sample nested 1.veg";
        check(QFileInfo::exists(nested), "Create Nested Timeline saves the events as a .veg next to the project");
        const Project& p = st().project();
        int nestedEvents = 0;
        uint64_t g = 0;
        bool sameGroup = true;
        for (const auto& t : p.tracks)
            for (const auto& e : t.events) {
                const MediaItem* m = p.findMedia(e.mediaId);
                if (m && m->path == nested.toStdString() && std::fabs(e.start - t0) < 1e-9 && std::fabs(e.end() - t1) < 1e-9) {
                    ++nestedEvents;
                    if (g == 0) g = e.group;
                    sameGroup = sameGroup && e.group == g && g != 0;
                }
            }
        check(nestedEvents == 2 && sameGroup, "and puts a picture event and a sound event of it in their place, grouped", std::to_string(nestedEvents));
        check(p.media.size() == mediaBefore + 1, "the nested project is media of the project");
        Project inner;
        std::string e2;
        check(loadProjectFile(nested.toStdString(), inner, &e2) && inner.tracks.size() == 2 && inner.tracks[0].events.size() == 1 &&
                  std::fabs(inner.tracks[0].events[0].start) < 1e-9,
              "the nested project holds the events, starting at 0", e2);
        MediaInfo mi;
        check(vegasProjectMediaInfo(nested.toStdString(), mi, &e2) && std::fabs(mi.duration - (t1 - t0)) < 1e-6 && mi.hasVideo && mi.hasAudio,
              "it is media as long as the events were, with picture and sound");
        // Alt+N opens it in another Reno
        const QString log = out + "/opened.txt";
        qputenv("RENO_TEST_OPEN_LOG", log.toLocal8Bit());
        action(w, "nestOpen")->trigger();
        const QStringList lines = QString::fromStdString(bytesOf(log)).trimmed().split('\n');
        check(!lines.isEmpty() && lines[0] == nested, "Open Nested Timeline opens its .veg in another Reno", bytesOf(log));
        check(lines.size() >= 2 && lines[1].contains("--parent | " + QString::fromStdString(st().path())) &&
                  lines[1].contains("--parent-pid | " + QString::number(QCoreApplication::applicationPid())),
              "and tells it the parent project and this Reno's process (for Open Parent)", bytesOf(log));
        // Alt+P in that Reno: back to the parent (this test stands in for its command line)
        QFile::remove(log);
        qunsetenv("RENO_TEST_PARENT");
        action(w, "nestParent")->trigger();
        QLabel* statusLine = w.findChild<QLabel*>("statusText");
        check(!QFile::exists(log) && statusLine && statusLine->text().contains("no parent"), "Open Parent in a project with no parent says so",
              statusLine ? statusLine->text().toStdString() : std::string());
        qputenv("RENO_TEST_PARENT", (QString::fromStdString(st().path()) + "|4242").toLocal8Bit());
        action(w, "nestParent")->trigger();
        check(QString::fromStdString(bytesOf(log)).trimmed() == "parent " + QString::fromStdString(st().path()) + " | 4242",
              "Open Parent (Alt+P) goes to the parent's Reno (its window brought forward on Windows, else the parent opened)", bytesOf(log));
        qunsetenv("RENO_TEST_PARENT");
        st().undo();
        check(st().project().tracks[0].events.size() == 1 && st().project().media.size() == mediaBefore, "undo puts the events back");
        uitest::shot(&w, out + "/after_nesting.png");
    }

    // ---------------------------------------------------------------- what a .veg cannot hold is said
    st().edit("Motion", [](Project& p) {
        MotionKey k;
        k.x = 0.25;
        p.tracks[0].motion.push_back(k);
    });
    action(w, "save")->trigger();
    uitest::settle(100);
    QDialog* lost = window(w, "saveWarnings");
    check(lost != nullptr, "what could not be saved is listed after saving");
    if (lost) {
        uitest::shot(lost, out + "/save_warnings.png");
        check(lost->findChild<QPlainTextEdit*>()->toPlainText().contains("track motion"), "it names the track motion");
    }
    closeWindows(w);

    // ---------------------------------------------------------------- a new project is saved as .veg
    st().newProject();
    std::string err;
    const uint64_t clip = st().importMedia("synth:1", &err);
    st().addMediaToTimeline(clip, 1.0);
    const QString made = out + "/new project.veg";
    check(st().save(made.toStdString(), &err), "a new project saves as .veg", err);
    check(isVegasProject(made.toStdString()), "it is a VEGAS project");
    check(w.openProject(made), "and opens");
    closeWindows(w);
    size_t events = 0;
    for (const auto& t : st().project().tracks) events += t.events.size();
    check(events == 2, "with its picture and sound events", std::to_string(events));

    // ---------------------------------------------------------------- .reno files still open, and save as .reno
    const QString reno = out + "/old.reno";
    check(st().save(reno.toStdString(), &err), "saved as .reno (Reno's own file)", err);
    check(!isVegasProject(reno.toStdString()), "a .reno is not a .veg");
    check(w.openProject(reno), "a .reno opens");
    closeWindows(w);
    check(window(w, "importNotes") == nullptr, "with nothing to report");
    st().edit("Rename", [](Project& p) { p.tracks[0].name = "Kept as reno"; });
    action(w, "save")->trigger();
    uitest::settle(100);
    check(st().path() == reno.toStdString() && !isVegasProject(reno.toStdString()), "Save of a .reno project writes the .reno");
    Project back;
    check(back.load(reno.toStdString()) && back.tracks[0].name == "Kept as reno", "and the .reno holds the edit");

    // ---------------------------------------------------------------- File lists the recent projects
    QMenu* file = nullptr;
    for (QAction* a : w.menuBar()->actions())
        if (a->text().remove('&') == "File") file = a->menu();
    check(file != nullptr, "there is a File menu");
    if (file) {
        emit file->aboutToShow();
        QStringList listed;
        for (QAction* a : file->actions())
            if (a->property("recentProject").isValid()) listed << a->property("recentProject").toString();
        check(listed.size() >= 3 && listed.value(0) == QFileInfo(reno).absoluteFilePath() && listed.contains(QFileInfo(veg).absoluteFilePath()),
              "the recent projects are listed, the last one first", listed.join(" | ").toStdString());
        emit file->aboutToShow();
        int again = 0;
        for (QAction* a : file->actions()) again += a->property("recentProject").isValid() ? 1 : 0;
        check(again == listed.size(), "listed once each time the menu opens");
        uitest::shot(&w, out + "/window.png");
    }
    return uitest::summary();
}
