// Previewing, captioning, disc and tape commands of the main window's menus; see outputcmds.h.
#include "outputcmds.h"
#include "vprefs.h"
#include "appstate.h"
#include "loudnesswin.h"
#include "mainwindow.h"
#include "playback.h"
#include "../engine/loudness.h"
#include "../media/media.h"
#include "menuutil.h"
#include "strconv.h"
#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace reno {
namespace app {

namespace {

QAction* command(MainWindow* w, const char* name, const QString& text, std::function<void()> fn)
{
    QAction* a = new QAction(text, w);
    a->setObjectName(name);
    QObject::connect(a, &QAction::triggered, w, [fn] { fn(); });
    w->addAction(a);
    return a;
}

// A command VEGAS has that needs hardware Reno does not drive: greyed in the menu, saying why.
QAction* notAvailable(MainWindow* w, const char* name, const QString& text, const QString& why)
{
    QAction* a = new QAction(text, w);
    a->setObjectName(name);
    a->setEnabled(false);
    a->setToolTip(why);
    a->setStatusTip(why);
    a->setProperty("notAvailable", why);
    w->addAction(a);
    return a;
}

std::string projectStem()
{
    const std::string p = AppState::get().path();
    return p.empty() ? std::string("Untitled") : pathStem(p);
}

QString documentsFolder() { return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation); }

QString defaultFolder()
{
    const std::string p = AppState::get().path();
    return p.empty() ? documentsFolder() : QFileInfo(qs(p)).absolutePath();
}

}  // namespace

// ------------------------------------------------------------------------------------------------ captions
int importClosedCaptions(const std::string& path, std::string* error)
{
    AppState& app = AppState::get();
    std::vector<Caption> caps;
    if (!readCaptionFile(path, app.project().settings.fps(), caps, error)) return 0;
    // at the file's timecodes; the ruler offset moves them, as VEGAS's manual says
    const double offset = app.project().settings.rulerStart;
    int added = 0;
    app.edit("Import Closed Captioning", [&](Project& p) {
        for (const Caption& c : caps) {
            const double t = c.time - offset;
            if (t < 0) continue;
            Project::Marker m;
            m.id = p.newId();
            m.time = t;
            m.kind = "command";
            m.command = captionCommand(c.channel);
            m.name = c.markup;
            p.markers.push_back(m);
            ++added;
        }
    });
    if (!added && error) *error = "every caption was before the start of the timeline";
    return added;
}

bool exportClosedCaptions(const std::string& path, CaptionFormat format, std::string* error)
{
    const AppState& app = AppState::get();
    const std::vector<Caption> caps = projectCaptions(app.project());
    if (caps.empty()) {
        if (error) *error = "the project has no closed captions (608CC1 .. 608CC4 command markers)";
        return false;
    }
    return writeCaptionFile(path, format, caps, app.project().length(), app.project().settings.fps(), error);
}

int promoteMediaClosedCaptions()
{
    AppState& app = AppState::get();
    struct Found { double t; Caption c; };
    std::vector<Found> found;
    for (uint64_t id : app.selection()) {
        const Track* tr = nullptr;
        const Event* e = app.project().findEvent(id, &tr);
        const MediaItem* m = e ? app.project().findMedia(e->mediaId) : nullptr;
        if (!m || m->path.empty()) continue;
        // captions VEGAS read from the file and kept beside it: <file>.scc, or <name>.scc
        std::string scc = m->path + ".scc";
        if (!fileExists(scc)) scc = pathJoin(pathDir(m->path), pathStem(m->path) + ".scc");
        std::vector<Caption> caps;
        if (!fileExists(scc) || !readCaptionFile(scc, 30000.0 / 1001.0, caps, nullptr)) continue;
        const double rate = e->rate > 0 ? e->rate : 1.0;
        for (const Caption& c : caps) {
            const double t = e->start + (c.time - e->offset - m->subclipStart) / rate;
            if (t < e->start - 1e-6 || t > e->start + e->length + 1e-6) continue;
            found.push_back(Found{t, c});
        }
    }
    if (found.empty()) return 0;
    app.edit("Promote Media Closed Captioning", [&](Project& p) {
        for (const Found& f : found) {
            Project::Marker mk;
            mk.id = p.newId();
            mk.time = f.t;
            mk.kind = "command";
            mk.command = captionCommand(f.c.channel);
            mk.name = f.c.markup;
            p.markers.push_back(mk);
        }
    });
    return (int)found.size();
}

// ------------------------------------------------------------------------------------------------ CD layout
int layOutAudioCdFromEvents()
{
    AppState& app = AppState::get();
    std::vector<std::pair<double, double>> spans;
    for (const Track& t : app.project().tracks) {
        if (t.type != TrackType::Audio || t.mute) continue;
        for (const Event& e : t.events)
            if (!e.mute && e.length > 0) spans.push_back(std::make_pair(e.start, e.start + e.length));
    }
    if (spans.empty()) return 0;
    std::sort(spans.begin(), spans.end());
    // events that touch or overlap are one track
    std::vector<std::pair<double, double>> tracks;
    for (const auto& s : spans) {
        if (!tracks.empty() && s.first <= tracks.back().second + 1e-6) tracks.back().second = std::max(tracks.back().second, s.second);
        else tracks.push_back(s);
    }
    if (tracks.size() > 99) tracks.resize(99);          // a Red Book CD holds 99 tracks
    app.edit("Lay Out Audio CD from Events", [&](Project& p) {
        p.markers.erase(std::remove_if(p.markers.begin(), p.markers.end(), [](const Project::Marker& m) { return m.kind == "cdtrack" || m.kind == "cdindex"; }),
                        p.markers.end());
        for (const auto& t : tracks) {
            Project::Marker m;
            m.id = p.newId();
            m.time = t.first;
            m.length = t.second - t.first;
            m.region = true;
            m.kind = "cdtrack";
            p.markers.push_back(m);
        }
    });
    return (int)tracks.size();
}

// ------------------------------------------------------------------------------------------------ wireless adapter proxies
namespace {

// A proxy's clip name: XDCAM proxies are the clip's name with "S03" (or another sub-stream number) after it
// (C0001S03.MP4 for C0001.MXF); others say "proxy". Empty: not a proxy.
QString proxyClip(const QString& file)
{
    const QString stem = QFileInfo(file).completeBaseName();
    QRegularExpressionMatch m = QRegularExpression("^(.+?)S0\\d$", QRegularExpression::CaseInsensitiveOption).match(stem);
    if (m.hasMatch()) return m.captured(1);
    m = QRegularExpression("^(.+?)[ _.-]?(proxy|prx|lowres)$", QRegularExpression::CaseInsensitiveOption).match(stem);
    if (m.hasMatch()) return m.captured(1);
    return QString();
}

// The full-resolution file of a clip in 'folder' (or below), or empty.
QString fullResolution(const QString& folder, const QString& clip, const QString& proxyPath)
{
    static const QStringList exts = {"mxf", "mp4", "mov", "mts", "m2ts", "avi"};
    QStringList dirs = {folder};
    for (const QFileInfo& sub : QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) dirs << sub.absoluteFilePath();
    for (const QString& d : dirs)
        for (const QFileInfo& f : QDir(d).entryInfoList(QDir::Files))
            if (f.completeBaseName().compare(clip, Qt::CaseInsensitive) == 0 && exts.contains(f.suffix().toLower()) &&
                QFileInfo(proxyPath).absoluteFilePath() != f.absoluteFilePath())
                return f.absoluteFilePath();
    return QString();
}

}  // namespace

int wirelessProxyCount()
{
    int n = 0;
    for (const MediaItem& m : AppState::get().project().media)
        if (!m.generated && !m.path.empty() && !proxyClip(qs(m.path)).isEmpty()) ++n;
    return n;
}

int relinkWirelessMedia(const std::string& folder, bool relink)
{
    AppState& app = AppState::get();
    struct Swap { uint64_t id; std::string path; MediaInfo info; };
    std::vector<Swap> swaps;
    for (const MediaItem& m : app.project().media) {
        if (m.generated || m.path.empty()) continue;
        const QString clip = proxyClip(qs(m.path));
        if (clip.isEmpty()) continue;
        const QString full = fullResolution(qs(folder), clip, qs(m.path));
        if (full.isEmpty()) continue;
        if (!relink) {
            swaps.push_back(Swap{m.id, full.toStdString(), MediaInfo()});
            continue;
        }
        std::string err;
        std::unique_ptr<MediaReader> r;
        try {
            r = app.mediaOpener ? app.mediaOpener(full.toStdString(), &err) : openMedia(full.toStdString(), &err);
        } catch (...) {
        }
        if (!r) continue;
        swaps.push_back(Swap{m.id, full.toStdString(), r->info()});
    }
    if (!relink || swaps.empty()) return (int)swaps.size();
    app.edit("Relink Wireless Adapter Media", [&](Project& p) {
        for (const Swap& s : swaps)
            if (MediaItem* mm = p.findMedia(s.id)) {
                const bool keepName = mm->name != pathFileName(mm->path);
                mm->path = s.path;
                if (!keepName) mm->name = pathFileName(s.path);
                mm->info = s.info;
                mm->offline = false;
                mm->veg.reset();
            }
    });
    return (int)swaps.size();
}

// ------------------------------------------------------------------------------------------------ loudness log
bool generateLoudnessLog(const std::string& path, std::string* written, std::string* error)
{
    AppState& app = AppState::get();
    std::shared_ptr<const Project> p = app.snapshot();
    const bool sel = app.loopEnd > app.loopStart;
    const double from = sel ? app.loopStart : 0.0, to = sel ? app.loopEnd : p->length();
    if (!(to > from)) {
        if (error) *error = "there is nothing on the timeline to measure";
        return false;
    }
    std::string out = path;
    if (out.empty()) out = QDir(defaultFolder()).filePath(qs(projectStem() + "_loud.txt")).toStdString();
    const int rate = p->settings.sampleRate > 0 ? p->settings.sampleRate : 48000;
    const int channels = p->settings.surround() ? 6 : 2;
    MediaPool pool;
    if (app.mediaOpener) pool.setOpener(app.mediaOpener);
    AudioEngine engine(pool, app.fx);
    LoudnessMeter meter(rate);
    meter.setPeakBlocking(QSettings().value("loudness/peakBlocking", false).toBool());
    std::vector<std::vector<float>> buf((size_t)channels, std::vector<float>(4096));
    std::vector<float*> ptrs;
    for (auto& b : buf) ptrs.push_back(b.data());
    std::vector<LoudnessLogPoint> points;
    const int64_t a = (int64_t)std::llround(from * rate), b = (int64_t)std::llround(to * rate);
    for (int64_t at = a; at < b; at += 4096) {
        const int n = (int)std::min<int64_t>(4096, b - at);
        engine.renderChannels(*p, at, n, ptrs.data(), false);
        meter.processChannels(ptrs.data(), channels, n);
        const double sec = (double)(at + n - a) / rate;
        if (points.empty() ? sec >= 1.0 : sec >= points.back().time - from + 1.0) {
            LoudnessLogPoint pt;
            pt.time = from + std::floor(sec);
            pt.momentary = meter.momentary();
            pt.shortTerm = meter.shortTerm();
            points.push_back(pt);
        }
    }
    const bool atsc = QSettings().value("loudness/mode").toString() == "atsc";
    const std::string format = strfmt("%d Hz, %s, %s", rate, channels == 6 ? "5.1 Surround" : "Stereo", sel ? "the time selection" : "the whole timeline");
    const std::string text = loudnessLogText(app.path().empty() ? std::string("Untitled") : app.path(), format, atsc, from, to, meter, points);
    if (!writeFile(out, text)) {
        if (error) *error = out + " could not be written";
        return false;
    }
    if (written) *written = out;
    return true;
}

// ------------------------------------------------------------------------------------------------ the menus
void addOutputCommands(MainWindow* w)
{
    AppState& app = AppState::get();

    // ---- File > Import > Closed Captioning..., File > Export > XDCAM Disc...
    QMenu* file = topMenu(w, "File");
    QAction* importCc = command(w, "importClosedCaptioning", "&Closed Captioning...", [w] {
        const QString f = QFileDialog::getOpenFileName(w, "Import Closed Captioning", QSettings().value("captions/folder", defaultFolder()).toString(),
                                                       "Closed captioning files (*.scc *.rt *.srt *.smi *.sami *.txt *.mcc *.sub);;"
                                                       "Scenarist Closed Caption (*.scc);;RealPlayer Captioning Files (*.rt);;SubRip Subtitles (*.srt);;"
                                                       "Windows Media Player Captioning Files (*.smi *.sami);;Transcript or QuickTime Captioning Files (*.txt);;"
                                                       "CPC MacCaption Files (*.mcc);;DVD Architect Subtitles (*.sub);;All files (*)");
        if (f.isEmpty()) return;
        QSettings().setValue("captions/folder", QFileInfo(f).absolutePath());
        std::string err;
        const int n = importClosedCaptions(f.toStdString(), &err);
        if (!n) QMessageBox::warning(w, "Import Closed Captioning", QString("%1: %2").arg(QFileInfo(f).fileName(), qs(err)));
        else AppState::get().status(strfmt("Imported %d closed caption marker%s from %s.", n, n == 1 ? "" : "s", QFileInfo(f).fileName().toStdString().c_str()));
    });
    if (QMenu* imp = subMenu(file, "Import")) imp->addAction(importCc);
    QAction* xdcam = notAvailable(w, "exportXdcamDisc", "&XDCAM Disc...", "Reno cannot write to XDCAM discs (it needs an XDCAM drive or deck)");
    if (file) {
        QMenu* exp = subMenu(file, "Export");
        if (!exp) {
            exp = new QMenu("&Export", file);
            // after Import
            QAction* before = nullptr;
            const QList<QAction*> acts = file->actions();
            for (int k = 0; k + 1 < acts.size(); ++k)
                if (acts[k]->menu() && acts[k]->text().remove('&') == "Import") before = acts[k + 1];
            file->insertMenu(before, exp);
        }
        exp->addAction(xdcam);
    }

    // ---- View > Window > XDCAM Explorer
    if (QMenu* win = subMenu(topMenu(w, "View"), "Window"))
        win->addAction(notAvailable(w, "showXdcamExplorer", "XDCAM Explorer", "Reno has no XDCAM Explorer: XDCAM discs need an XDCAM drive or deck "
                                                                              "(XDCAM files copied to the computer open as any media)"));

    // ---- Tools
    QMenu* tools = topMenu(w, "Tools");
    QMenu* burn = new QMenu("&Burn Disc", tools ? (QWidget*)tools : (QWidget*)w);
    burn->menuAction()->setObjectName("burnDiscMenu");
    const QString noBurn = "Reno does not burn discs. Render the project (File > Render As) and burn the file with a disc burning program.";
    burn->addAction(notAvailable(w, "burnTaoCd", "&Track-at-Once Audio CD...", noBurn));
    burn->addAction(notAvailable(w, "burnDaoCd", "&Disc-at-Once Audio CD...", noBurn));
    burn->addAction(notAvailable(w, "burnDvd", "D&VD...", noBurn));
    burn->addAction(notAvailable(w, "burnBluray", "&Blu-ray Disc...", noBurn));
    QAction* tape = notAvailable(w, "printToTape", "Print &Video to Tape...", "Reno does not drive DV, SDI or HDMI tape decks");
    QAction* hdvTape = notAvailable(w, "printToHdvTape", "Print Video to &HDV Tape...", "Reno does not drive HDV tape decks");
    QAction* relink = command(w, "relinkWireless", "Relink &Wireless Adapter Media...", [w] {
        const int n = wirelessProxyCount();
        if (n == 0) {
            QMessageBox::information(w, "Relink Wireless Adapter Media", "There are no proxy files in this project to relink.");
            return;
        }
        if (QMessageBox::question(w, "Relink Wireless Adapter Media",
                                  QString("%1 proxy file%2 can be relinked to full-resolution media. Relink now?").arg(n).arg(n == 1 ? "" : "s"),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        // a folder at a time, until every proxy has its full-resolution file (or the user stops)
        int done = 0;
        while (wirelessProxyCount() > 0) {
            const QString folder = QFileDialog::getExistingDirectory(w, "Browse for the full-resolution clips' folder", QSettings().value("relink/folder", defaultFolder()).toString());
            if (folder.isEmpty()) break;
            QSettings().setValue("relink/folder", folder);
            const int k = relinkWirelessMedia(folder.toStdString());
            done += k;
            if (k == 0) QMessageBox::information(w, "Relink Wireless Adapter Media", "No full-resolution clips for this project's proxies were found in " + folder + ".");
        }
        AppState::get().status(strfmt("Relinked %d proxy file%s to full-resolution media.", done, done == 1 ? "" : "s"));
    });
    QAction* layout = command(w, "layOutAudioCd", "Lay &Out Audio CD from Events", [] {
        const int n = layOutAudioCdFromEvents();
        AppState::get().status(n ? strfmt("Laid out %d CD track%s.", n, n == 1 ? "" : "s") : std::string("Lay Out Audio CD from Events: there are no audio events."));
    });
    QAction* loudLog = command(w, "generateLoudnessLog", "Generate &Loudness Log", [w] {
        QProgressDialog busy("Measuring the loudness...", QString(), 0, 0, w);
        busy.setWindowTitle("Generate Loudness Log");
        busy.setMinimumDuration(200);
        busy.setValue(0);
        QCoreApplication::processEvents();
        std::string out, err;
        if (!generateLoudnessLog(std::string(), &out, &err)) {
            QMessageBox::warning(w, "Generate Loudness Log", qs(err));
            return;
        }
        AppState::get().status("Loudness log written: " + out);
        // Preferences > Audio "Open editor when new loudness log is generated"
        if (vprefs::on("prefs/loudnessOpenEditor")) QDesktopServices::openUrl(QUrl::fromLocalFile(qs(out)));
    });
    // Extract Audio from CD (VEGAS 23): the tracks of a CD copied to the computer open as any audio files
    QAction* extractCd = notAvailable(w, "extractAudioCd", "E&xtract Audio from CD...", "Reno does not read audio CDs. Copy the tracks with a ripping program and import the files.");
    // Options > Metronome: the switch (the click is not played yet)
    if (tools) {
        tools->addMenu(burn);
        tools->addActions({tape, hdvTape, relink, layout, loudLog, extractCd});
    }

    // ---- Tools > Scripting closed captioning commands (added by buildVegasMenus after its own items)
    command(w, "scriptPromoteCaptions", "Promote Media Closed Captioning", [] {
        const int n = promoteMediaClosedCaptions();
        AppState::get().status(n ? strfmt("Promoted %d closed caption%s to command markers.", n, n == 1 ? "" : "s")
                                 : std::string("Promote Media Closed Captioning: the selected events' media have no captions (an .scc file beside the media)."));
    });
    const struct { const char* name; const char* text; const char* filter; const char* ext; CaptionFormat f; } exports[] = {
        {"scriptExportCaptionsDvda", "Export Closed Captioning for DVD Architect", "DVD Architect Subtitles (*.sub)", "sub", CaptionFormat::Sub},
        {"scriptExportCaptionsQuickTime", "Export Closed Captioning for QuickTime", "QuickTime Captioning Files (*.txt)", "txt", CaptionFormat::QuickTime},
        {"scriptExportCaptionsWmp", "Export Closed Captioning for Windows Media Player", "Windows Media Player Captioning Files (*.smi)", "smi", CaptionFormat::Smi},
        {"scriptExportCaptionsYouTube", "Export Closed Captioning for YouTube", "SubRip Subtitles (*.srt)", "srt", CaptionFormat::Srt}};
    for (const auto& e : exports) {
        const QString title = e.text, filter = e.filter, ext = e.ext;
        const CaptionFormat f = e.f;
        command(w, e.name, title, [w, title, filter, ext, f] {
            const QString suggested = QDir(defaultFolder()).filePath(qs(projectStem()) + "." + ext);
            const QString path = QFileDialog::getSaveFileName(w, title, suggested, filter);
            if (path.isEmpty()) return;
            std::string err;
            if (!exportClosedCaptions(path.toStdString(), f, &err)) QMessageBox::warning(w, title, qs(err));
            else AppState::get().status("Closed captioning exported: " + path.toStdString());
        });
    }

    // ---- Options: Loudness Meters, External Control (Mute All, Bypass All Audio FX and the metronome are in vegasextras.cpp and mainwindow_menus.cpp)
    QMenu* options = topMenu(w, "Options");
    QMenu* loudness = new QMenu("Lou&dness Meters", options ? (QWidget*)options : (QWidget*)w);
    loudness->setObjectName("loudnessMetersMenu");
    loudness->menuAction()->setObjectName("loudnessMetersMenu");
    loudness->addAction("(options)");      // filled when it opens
    QObject::connect(loudness, &QMenu::aboutToShow, loudness, [loudness] { fillLoudnessMenu(loudness, nullptr); });
    QAction* external = notAvailable(w, "externalControl", "E&xternal Control", "Reno does not use control surfaces (Mackie Control, FaderPort, "
                                                                                 "generic MIDI controllers) or Vegas Pro Connect");
    if (options) {
        options->addMenu(loudness);
        options->addAction(external);
    }
    (void)app;
}

}  // namespace app
}  // namespace reno
