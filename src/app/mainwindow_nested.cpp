// Nested timelines: Create Nested Timeline (Alt+C), Open Nested Timeline (Alt+N) and Open Parent (Alt+P).
#include "audioproxyui.h"
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "vprefs.h"
#include "prefsapply.h"
#include "vegasextras.h"
#include "man2dialogs.h"
#include "outputcmds.h"
#include "startup.h"
#include "recovery.h"
#include "../engine/timefmt.h"
#include "pluginchooser.h"
#include "timeline.h"
#include "fxwindow.h"
#include "mixer.h"
#include "editdialogs.h"
#include "mainwindow_tools.h"
#include "../vegas/vegas.h"
#include "renderdialog.h"
#include "keyboard.h"
#include "trackdefaults.h"
#include "smalldialogs.h"
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include "automationctl.h"
#include <algorithm>
#include <map>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace reno {
namespace app {

// ------------------------------------------------------------------------------------------ nested timelines
// VEGAS's nested timelines: the selected events are saved as a project of their own (a .veg next to this one) and
// replaced by one event that plays it (an image and a sound event, grouped, if both kinds were selected).
void createNestedTimeline(QWidget* parent)
{
    AppState& app = AppState::get();
    const Project& p = app.project();
    struct Sel { size_t track; const Event* e; };
    std::vector<Sel> sel;
    for (size_t ti = 0; ti < p.tracks.size(); ++ti)
        for (const Event& e : p.tracks[ti].events)
            if (app.selection().count(e.id)) sel.push_back({ti, &e});
    if (sel.empty()) {
        app.status("Select the events to nest first.");
        return;
    }
    if (app.path().empty()) {
        QMessageBox::information(parent, "Create Nested Timeline", "Save the project first. The nested timeline is saved as a project next to it.");
        return;
    }
    double t0 = sel[0].e->start, t1 = sel[0].e->end();
    for (const auto& s : sel) {
        t0 = std::min(t0, s.e->start);
        t1 = std::max(t1, s.e->end());
    }
    // the nested project: same settings, the media the events use, a track for each track they are on
    Project n;
    n.settings = p.settings;
    std::map<uint64_t, uint64_t> mediaMap, groupMap;
    std::map<size_t, size_t> trackMap;
    for (const auto& s : sel) {
        if (!mediaMap.count(s.e->mediaId))
            if (const MediaItem* m = p.findMedia(s.e->mediaId)) {
                MediaItem c = *m;
                c.id = n.newId();
                c.veg.reset();
                c.generator.veg.reset();
                if (c.generated) c.generator.id = n.newId();
                mediaMap[m->id] = c.id;
                n.media.push_back(c);
            }
        if (!trackMap.count(s.track)) {
            Track t;
            t.id = n.newId();
            t.type = p.tracks[s.track].type;
            t.name = p.tracks[s.track].name;
            trackMap[s.track] = n.tracks.size();
            n.tracks.push_back(t);
        }
        Event e = *s.e;
        e.id = n.newId();
        e.veg.reset();
        e.start -= t0;
        e.mediaId = mediaMap.count(e.mediaId) ? mediaMap[e.mediaId] : 0;
        if (e.group) {
            if (!groupMap.count(e.group)) groupMap[e.group] = n.newId();
            e.group = groupMap[e.group];
        }
        for (auto& f : e.fx) {
            f.id = n.newId();
            f.veg.reset();
        }
        for (auto& v : e.envelopes) v.veg.reset();
        n.tracks[trackMap[s.track]].events.push_back(e);
    }
    for (auto& t : n.tracks) t.sortEvents();
    // next to the project: "<project> nested 1.veg", "... 2.veg"...
    const QFileInfo fi(QString::fromStdString(app.path()));
    QString path;
    for (int k = 1;; ++k) {
        path = fi.dir().filePath(QString("%1 nested %2.veg").arg(fi.completeBaseName()).arg(k));
        if (!QFileInfo::exists(path)) break;
    }
    const VegasSaveResult r = saveVegasProject(n, path.toStdString());
    if (!r.ok) {
        QMessageBox::warning(parent, "Create Nested Timeline", QString("%1\n\n%2").arg(path, QString::fromStdString(r.error)));
        return;
    }
    MediaInfo info;
    std::string err;
    if (!vegasProjectMediaInfo(path.toStdString(), info, &err)) {
        info = MediaInfo();
        info.path = path.toStdString();
        info.hasVideo = info.hasAudio = true;
        info.width = p.settings.width;
        info.height = p.settings.height;
        info.duration = t1 - t0;
    }
    bool video = false, audio = false;
    size_t videoTrack = 0, audioTrack = 0;
    for (const auto& s : sel) {
        if (p.tracks[s.track].type == TrackType::Video) {
            if (!video || s.track < videoTrack) videoTrack = s.track;
            video = true;
        } else {
            if (!audio || s.track < audioTrack) audioTrack = s.track;
            audio = true;
        }
    }
    std::set<uint64_t> made;
    app.edit("Create Nested Timeline", [&](Project& pr) {
        for (auto& t : pr.tracks) {
            std::vector<Event> keep;
            for (const auto& e : t.events)
                if (!app.selection().count(e.id)) keep.push_back(e);
            t.events = keep;
        }
        MediaItem m;
        m.id = pr.newId();
        m.path = path.toStdString();
        m.name = QFileInfo(path).fileName().toStdString();
        m.info = info;
        pr.media.push_back(m);
        const uint64_t g = video && audio ? pr.newId() : 0;
        auto place = [&](size_t ti) {
            if (ti >= pr.tracks.size()) return;
            Event e;
            e.id = pr.newId();
            e.mediaId = m.id;
            e.start = t0;
            e.length = t1 - t0;
            e.group = g;
            e.name = m.name;
            setDefaultFadeCurves(e, pr.tracks[ti].type);
            pr.tracks[ti].events.push_back(e);
            pr.tracks[ti].sortEvents();
            made.insert(e.id);
        };
        if (video) place(videoTrack);
        if (audio) place(audioTrack);
    });
    app.setSelection(made);
    app.status("Nested " + std::to_string(sel.size()) + " events in " + path.toStdString() +
               (r.warnings.empty() ? std::string() : " (" + std::to_string(r.warnings.size()) + " things could not be saved in it)"));
}

// Opens the selected event's nested timeline (its .veg) in another Reno, as VEGAS does.
void openNestedTimeline(QWidget* parent)
{
    (void)parent;
    AppState& app = AppState::get();
    const Project& p = app.project();
    for (const auto& t : p.tracks)
        for (const auto& e : t.events) {
            if (!app.selection().count(e.id)) continue;
            const MediaItem* m = p.findMedia(e.mediaId);
            if (!m || m->generated || !endsWithNoCase(m->path, ".veg")) continue;
            const QString path = QString::fromStdString(m->path);
            if (!QFileInfo::exists(path)) {
                app.status("The nested timeline's project is not there: " + m->path);
                return;
            }
            // the other Reno is told where it came from, for Open Parent (Alt+P)
            QStringList args;
            args << path;
            if (!app.path().empty())
                args << "--parent" << QString::fromStdString(app.path()) << "--parent-pid" << QString::number(QCoreApplication::applicationPid());
            // tests record what would be opened instead
            const QByteArray log = qgetenv("RENO_TEST_OPEN_LOG");
            if (!log.isEmpty()) {
                QFile f(QString::fromLocal8Bit(log));
                if (f.open(QIODevice::Append)) f.write(path.toUtf8() + "\n" + args.join(" | ").toUtf8() + "\n");
            } else {
                QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
            }
            app.status("Opening " + m->path + " in another Reno");
            return;
        }
    app.status("Select an event that plays another project (a nested timeline) first.");
}

// The folder file dialogs start in: the current project's, else the last used.

#ifdef _WIN32
struct FindTop { DWORD pid; HWND found; };
static BOOL CALLBACK findTopWindow(HWND h, LPARAM lp)
{
    FindTop* f = (FindTop*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr || GetWindowTextLengthW(h) == 0) return TRUE;
    f->found = h;
    return FALSE;
}

// Brings the main window of process 'pid' to the front; false if it has none.
static bool raiseProcessWindow(qint64 pid)
{
    FindTop f{(DWORD)pid, nullptr};
    EnumWindows(findTopWindow, (LPARAM)&f);
    if (!f.found) return false;
    if (IsIconic(f.found)) ShowWindow(f.found, SW_RESTORE);
    SetForegroundWindow(f.found);
    return true;
}
#endif

// A nested timeline is opened in another Reno that is told the parent project and process ("--parent <path> --parent-pid
// <pid>" after the project). Open Parent brings that Reno's window forward (Windows); if it is gone (or elsewhere) the
// parent project is opened in another Reno.
void openParentTimeline()
{
    AppState& app = AppState::get();
    const QStringList args = QCoreApplication::arguments();
    QString parent;
    qint64 pid = 0;
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--parent") parent = args[i + 1];
        else if (args[i] == "--parent-pid") pid = args[i + 1].toLongLong();
    }
    // tests: "path|pid" in RENO_TEST_PARENT stands for the command line
    const QString testParent = QString::fromLocal8Bit(qgetenv("RENO_TEST_PARENT"));
    if (parent.isEmpty() && !testParent.isEmpty()) {
        parent = testParent.section('|', 0, 0);
        pid = testParent.section('|', 1, 1).toLongLong();
    }
    if (parent.isEmpty()) {
        app.status("This project was not opened as a nested timeline, so it has no parent.");
        return;
    }
    const QByteArray log = qgetenv("RENO_TEST_OPEN_LOG");
    if (!log.isEmpty()) {
        QFile f(QString::fromLocal8Bit(log));
        if (f.open(QIODevice::Append)) f.write("parent " + parent.toUtf8() + " | " + QByteArray::number(pid) + "\n");
        app.status("Switching to the parent project " + parent.toStdString());
        return;
    }
#ifdef _WIN32
    if (pid > 0 && raiseProcessWindow(pid)) {
        app.status("Switched to the parent project " + parent.toStdString());
        return;
    }
#endif
    if (!QFileInfo::exists(parent)) {
        app.status("The parent project is not there any more: " + parent.toStdString());
        return;
    }
    QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList() << parent);
    app.status("Opening the parent project " + parent.toStdString() + " in another Reno");
}

}  // namespace app
}  // namespace reno
