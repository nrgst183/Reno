#include "appstate.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include "vprefs.h"
#include "prefsapply.h"
#include "logbook.h"
#include "../vegas/vegas.h"
#include "../engine/timefmt.h"
#include "../engine/fxnames.h"
#include "../engine/fxlog.h"
#include "mediacache.h"
#include "autoproxy.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <map>

namespace reno {
namespace app {

AppState& AppState::get()
{
    static AppState* s = new AppState();
    return *s;
}

AppState::AppState()
{
    mediaOpener = [](const std::string& path, std::string* error) { return openMediaOrProject(path, error); };
    snapshot_ = std::make_shared<const Project>(project_);
    fx.onVideoParamChanged = [](uint64_t, ofx::Param*, const std::string& what) {
        if (what != "value") return;
        post([] {
            AppState& a = AppState::get();
            const bool was = a.dirty_;
            a.dirty_ = true;
            a.notify(kFxParams | (was ? 0 : kFile));
        });
    };
    // automatic preview proxies (autoproxy.h): nested projects are read with the .veg reader; a finished proxy is
    // reported in the status bar
    autoproxy::Worker::get().setLoader([](const std::string& path, Project& out) {
        std::string err;
        return loadProjectFileWithNotes(path, out, &err, nullptr);
    });
    autoproxy::Worker::get().setDoneHook([](const std::string& path, bool made, const std::string& error) {
        if (!QCoreApplication::instance()) return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [path, made, error] {
                AppState::get().status(made ? "Preview proxy made for " + pathFileName(path) : "No preview proxy for " + pathFileName(path) + ": " + error);
            },
            Qt::QueuedConnection);
    });
    if (QCoreApplication::instance() && !QCoreApplication::applicationName().isEmpty()) loadPreferences();
}

void AppState::post(std::function<void()> fn)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) { fn(); return; }
    QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
}

std::shared_ptr<const Project> AppState::snapshot() const
{
    std::lock_guard<std::mutex> lk(snapMutex_);
    return snapshot_;
}

int AppState::addListener(std::function<void(int)> fn)
{
    listeners_.emplace_back(nextListener_, std::move(fn));
    return nextListener_++;
}

void AppState::removeListener(int id)
{
    for (auto& l : listeners_) if (l.first == id) l.second = nullptr;
}

void AppState::notify(int changes)
{
    if (notifyDepth_ > 0) { pending_ |= changes; return; }
    ++notifyDepth_;
    while (changes) {
        const size_t n = listeners_.size();
        for (size_t k = 0; k < n; ++k) {
            if (!listeners_[k].second) continue;
            // called on a copy: a listener that makes a window (adding listeners) or removes itself would otherwise free
            // the running function
            const std::function<void(int)> fn = listeners_[k].second;
            fn(changes);
        }
        changes = pending_;
        pending_ = 0;
    }
    --notifyDepth_;
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [](const std::pair<int, std::function<void(int)>>& l) { return !l.second; }), listeners_.end());
}

void AppState::afterChange(int extra)
{
    for (auto& t : project_.tracks) t.sortEvents();
    project_.autoCrossfades = autoCrossfades_;
    project_.quickFade = prefs.quickFadeEdits ? std::min(1000.0, std::max(1.0, prefs.quickFadeMs)) / 1000.0 : 0.0;
    fx.prepare(project_);
    {
        std::lock_guard<std::mutex> lk(snapMutex_);
        snapshot_ = std::make_shared<const Project>(project_);
    }
    ++revision_;
    autoproxy::Worker::get().projectChanged(snapshot());
    int more = 0;
    if (selection_.size() > 8) {
        std::vector<uint64_t> have;
        project_.collectEventIds(have);
        std::sort(have.begin(), have.end());
        for (auto it = selection_.begin(); it != selection_.end();) {
            if (!std::binary_search(have.begin(), have.end(), *it)) { it = selection_.erase(it); more = kSelection; }
            else ++it;
        }
    } else
    for (auto it = selection_.begin(); it != selection_.end();) {
        if (!project_.findEvent(*it)) { it = selection_.erase(it); more = kSelection; }
        else ++it;
    }
    if (currentTrack && !project_.findTrack(currentTrack)) currentTrack = 0;
    notify(kProject | extra | more);
}

void AppState::trimUndo()
{
    const size_t most = (size_t)std::max(1, prefs.undoLevels);
    if (undo_.size() > most) undo_.erase(undo_.begin(), undo_.begin() + (std::ptrdiff_t)(undo_.size() - most));
}

void AppState::commit(const std::string& what, Project before)
{
    refreshAutoNormalize(before);
    undo_.push_back({what, std::move(before), {}});
    trimUndo();
    redo_.clear();
    const bool wasDirty = dirty_;
    dirty_ = true;
    afterChange(wasDirty ? 0 : kFile);
}

// Auto Normalize: an event with the switch whose media part changed since 'before' (or is new, e.g. the right half of a
// split) gets the gain that brings its peak to the Normalize level. Done in the step that made the change.
void AppState::refreshAutoNormalize(const Project& before)
{
    for (Track& t : project_.tracks) {
        if (t.type != TrackType::Audio) continue;
        // looked at through the const side first: a track with no Auto Normalize event stays shared with the undo step
        bool any = false;
        for (const Event& e : static_cast<const Track&>(t).events) if (e.autoNormalize) { any = true; break; }
        if (!any) continue;
        for (Event& e : t.events) {
            if (!e.autoNormalize) continue;
            const Event* old = before.findEvent(e.id);
            if (old && old->autoNormalize && old->mediaId == e.mediaId && old->offset == e.offset && old->length == e.length && old->rate == e.rate &&
                old->reverse == e.reverse && old->normalize)
                continue;
            const double peak = eventPeak(e);
            e.normalize = true;
            e.normalizeGain = peak > 1e-6 ? std::min(1000.0, normalizeLevel() / peak) : 1.0;
        }
    }
}

void AppState::setAutoNormalize(const std::set<uint64_t>& ids, bool on)
{
    std::set<uint64_t> audio;
    for (uint64_t id : ids) {
        const Track* t = nullptr;
        if (project_.findEvent(id, &t) && t && t->type == TrackType::Audio) audio.insert(id);
    }
    if (audio.empty()) {
        status("Auto Normalize works on audio events: select one first.");
        return;
    }
    // turning it on measures the event: edit() -> commit() -> refreshAutoNormalize does it, once
    edit(on ? "Auto Normalize" : "Auto Normalize Off", [&](Project& p) {
        for (uint64_t id : audio)
            if (Event* e = p.findEvent(id)) {
                e->autoNormalize = on;
                if (on) e->normalize = true;
            }
    });
}

void AppState::edit(const std::string& what, const std::function<void(Project&)>& fn)
{
    if (dragging_) cancelDrag();
    Project before = project_;
    fn(project_);
    commit(what, std::move(before));
}

void AppState::editFx(const std::string& what, const std::function<void(Project&)>& fn, const std::vector<uint64_t>& fxSlots)
{
    if (fxSlots.empty()) { edit(what, fn); return; }
    if (dragging_) cancelDrag();
    Project before = project_;
    // the step's "before" has the settings the plug-ins hold now, which undo gives back to them
    for (uint64_t id : fxSlots) {
        const Json live = fxLiveState(id);
        if (!live.isNull())
            if (FxSlot* s = before.findFx(id)) s->state = live;
    }
    fn(project_);
    undo_.push_back({what, std::move(before), fxSlots});
    trimUndo();
    redo_.clear();
    const bool wasDirty = dirty_;
    dirty_ = true;
    restoreFxStates(fxSlots);
    afterChange(kFxParams | (wasDirty ? 0 : kFile));
}

void AppState::mergeUndo(size_t count, const std::string& what)
{
    if (count == 0 || count > undo_.size()) return;
    const size_t first = undo_.size() - count;
    Step merged = std::move(undo_[first]);
    merged.what = what;
    for (size_t k = first + 1; k < undo_.size(); ++k)
        for (uint64_t id : undo_[k].fxSlots)
            if (std::find(merged.fxSlots.begin(), merged.fxSlots.end(), id) == merged.fxSlots.end()) merged.fxSlots.push_back(id);
    undo_.resize(first);
    undo_.push_back(std::move(merged));
    notify(kFile);
}

void AppState::discardUndoTo(size_t count, bool dirty)
{
    if (dragging_) cancelDrag();
    bool any = false;
    while (undo_.size() > count) {
        undo();
        any = true;
    }
    if (any) redo_.clear();
    dirty_ = dirty;
    if (any) notify(kFile);
}

void AppState::beginDrag()
{
    if (dragging_) cancelDrag();
    dragging_ = true;
    dragUpdated_ = false;
    dragBase_ = project_;
}

void AppState::dragUpdate(const std::function<void(Project&)>& fn)
{
    if (!dragging_) return;
    dragUpdated_ = true;
    project_ = dragBase_;
    fn(project_);
    afterChange();
}

void AppState::endDrag(const std::string& what)
{
    if (!dragging_) return;
    dragging_ = false;
    Project before = std::move(dragBase_);
    dragBase_ = Project();
    // nothing moved: no undo step. Writing a big project out takes tens of milliseconds (twice per click on an event),
    // so first test: not changed at all, or plainly moved; only otherwise compare in full.
    if (!dragUpdated_) return;
    if (!plainlyDifferent(before, project_) && before.toJson().dump() == project_.toJson().dump()) return;
    commit(what, std::move(before));
}

// Do 'a' and 'b' differ in what a timeline drag changes (events moved, trimmed, faded; track order or count)? False says
// only that no such difference was found.
bool AppState::plainlyDifferent(const Project& a, const Project& b)
{
    if (a.tracks.size() != b.tracks.size() || a.media.size() != b.media.size()) return true;
    for (size_t t = 0; t < a.tracks.size(); ++t) {
        const Track& x = a.tracks[t];
        const Track& y = b.tracks[t];
        if (x.id != y.id || x.events.size() != y.events.size()) return true;
        for (size_t k = 0; k < x.events.size(); ++k) {
            const Event& e = x.events[k];
            const Event& f = y.events[k];
            if (e.id != f.id || e.start != f.start || e.length != f.length || e.offset != f.offset || e.rate != f.rate ||
                e.fadeIn != f.fadeIn || e.fadeOut != f.fadeOut || e.gain != f.gain || e.mediaId != f.mediaId)
                return true;
        }
    }
    return false;
}

void AppState::cancelDrag()
{
    if (!dragging_) return;
    dragging_ = false;
    project_ = dragBase_;
    dragBase_ = Project();
    afterChange();
}

void AppState::undo()
{
    if (undo_.empty() || dragging_) return;
    Step s = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({s.what, std::move(project_), s.fxSlots});
    project_ = std::move(s.before);
    dirty_ = true;
    restoreFxStates(s.fxSlots);
    afterChange(kFile | (s.fxSlots.empty() ? 0 : kFxParams));
}

void AppState::redo()
{
    if (redo_.empty() || dragging_) return;
    Step s = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({s.what, std::move(project_), s.fxSlots});
    project_ = std::move(s.before);
    dirty_ = true;
    restoreFxStates(s.fxSlots);
    afterChange(kFile | (s.fxSlots.empty() ? 0 : kFxParams));
}

// ------------------------------------------------------------------------------------------ effect settings
Json AppState::fxLiveState(uint64_t slotId) const
{
    if (ofx::Instance* inst = fx.video(slotId)) return inst->saveState();
    if (AudioPlugin* pl = fx.audio(slotId)) return Json(pl->saveState());
    return Json();
}

// The live plug-ins take the settings the project now says they have.
void AppState::restoreFxStates(const std::vector<uint64_t>& ids)
{
    for (uint64_t id : ids) {
        const FxSlot* s = project_.findFx(id);
        if (!s) continue;
        if (ofx::Instance* inst = fx.video(id)) {
            if (s->state.isObject()) inst->loadState(s->state);
        } else if (AudioPlugin* pl = fx.audio(id)) {
            if (s->state.isString()) pl->loadState(s->state.asString());
        }
    }
}

void AppState::beginFxEdit(uint64_t slotId)
{
    if (!slotId || fxEdits_.count(slotId)) return;
    fxEdits_[slotId] = fxLiveState(slotId);
}

void AppState::endFxEdit(uint64_t slotId, const std::string& what)
{
    auto it = fxEdits_.find(slotId);
    if (it == fxEdits_.end()) return;
    const Json before = it->second;
    fxEdits_.erase(it);
    const Json after = fxLiveState(slotId);
    if (after.isNull() || after.dump() == before.dump()) return;
    FxSlot* now = project_.findFx(slotId);
    if (!now) return;
    if (dragging_) cancelDrag();
    // Preferences > General "Create undos for FX parameter changes" cleared: the change is kept with no undo step
    if (!vprefs::on("prefs/fxUndo")) {
        now->state = after;
        const bool wasDirty = dirty_;
        dirty_ = true;
        afterChange(kFxParams | (wasDirty ? 0 : kFile));
        return;
    }
    // the step's "before" is the project with the effect's old settings; the project now has the new ones
    Project was = project_;
    if (FxSlot* old = was.findFx(slotId)) old->state = before;
    now->state = after;
    undo_.push_back({what, std::move(was), {slotId}});
    trimUndo();
    redo_.clear();
    const bool wasDirty = dirty_;
    dirty_ = true;
    afterChange(kFxParams | (wasDirty ? 0 : kFile));
}

void AppState::fxEditStep(uint64_t slotId, const std::string& what, const std::function<void()>& change)
{
    if (fxEditOpen(slotId)) { change(); return; }
    beginFxEdit(slotId);
    change();
    endFxEdit(slotId, what);
}

void AppState::newProject(bool vegasDefaults)
{
    if (dragging_) cancelDrag();
    project_ = Project();
    releaseUnusedNestedProjects();
    if (vegasDefaults) {
        // VEGAS 18's preference defaults (from vegas180.exe): dFrameRateDefault 29.97002997, eDefDeintMethod 1 (Blend
        // Fields); the rest (48 kHz, Good quality, Smart resample, pixel aspect 1, 120 bpm) are Project's own
        project_.settings.fpsNum = 30000;
        project_.settings.fpsDen = 1001;
        project_.settings.deinterlace = "blend";
    }
    // Project Properties' "Start all new projects with these settings"
    if (!prefs.newProjectSettings.empty()) {
        Json j;
        Project d;
        if (Json::parse(prefs.newProjectSettings, j) && d.fromJson(j)) project_.settings = d.settings;
    }
    // the last chosen time format (Options > Time Format) is what new projects start with; an opened project keeps its own
    if (QCoreApplication::instance()) {
        const std::string f = QSettings().value("options/rulerFormat").toString().toStdString();
        for (const TimeFormatInfo& tf : timeFormats())
            if (!f.empty() && f == tf.id) project_.settings.rulerFormat = f;
    }
    // Preferences > Reno > Files, "Start new projects from this project": its tracks, events, media and settings become
    // the new project's; the template itself is only read. File > New only.
    if (vegasDefaults) {
        const std::string tpl = QDir::fromNativeSeparators(vprefs::text("prefs/templateProject").trimmed()).toStdString();
        Project t;
        if (!tpl.empty() && fileExists(tpl) && loadProjectFileWithNotes(tpl, t, nullptr, nullptr)) {
            project_ = t;
            for (auto& m : project_.media)
                if (!m.generated && m.path.compare(0, 6, "synth:") != 0) m.offline = !fileExists(m.path);
        }
    }
    undo_.clear();
    redo_.clear();
    fxEdits_.clear();
    selection_.clear();
    currentTrack = 0;
    path_.clear();
    openNotes_.clear();
    saveWarnings_.clear();
    dirty_ = false;
    cursor_ = 0;
    loopStart = loopEnd = 0;
    fx.clear();
    afterChange(kFile | kSelection | kCursor);
}

bool AppState::open(const std::string& path, std::string* error)
{
    Project p;
    std::vector<std::string> notes;
    if (!loadProjectFileWithNotes(path, p, error, &notes)) return false;
    newProject();
    project_ = p;
    openNotes_ = notes;
    saveWarnings_.clear();
    for (auto& m : project_.media)
        if (!m.generated && m.path.compare(0, 6, "synth:") != 0) m.offline = !fileExists(m.path);
    path_ = path;
    afterChange(kFile | kSelection | kCursor);
    writeFxLog(project_, fx, path, QCoreApplication::instance() ? QCoreApplication::applicationDirPath().toStdString() : std::string());
    return true;
}

void AppState::untieFromFile()
{
    path_.clear();
    dirty_ = true;
    notify(kFile);
}

// ------------------------------------------------------------------------------------------ preferences
void Preferences::load()
{
    QSettings q;
    const Preferences def;
    auto str = [&](const char* key, const std::string& d) { return q.value(QString("prefs/") + key, QString::fromStdString(d)).toString().toStdString(); };
    previewQuality = str("previewQuality", def.previewQuality);
    undoLevels = std::min(10000, std::max(1, q.value("prefs/undoLevels", def.undoLevels).toInt()));
    autoSaveMinutes = std::min(240, std::max(0, q.value("prefs/autoSaveMinutes", def.autoSaveMinutes).toInt()));
    stillLength = std::max(0.1, q.value("prefs/stillLength", def.stillLength).toDouble());
    generatedLength = std::max(0.1, q.value("prefs/generatedLength", def.generatedLength).toDouble());
    cursorFollowsPlayback = q.value("prefs/cursorFollowsPlayback", def.cursorFollowsPlayback).toBool();
    scrollMode = str("scrollMode", def.scrollMode);
    snapping = q.value("prefs/snapping", def.snapping).toBool();
    snapToGrid = q.value("prefs/snapToGrid", def.snapToGrid).toBool();
    ofxHostName = str("ofxHostName", def.ofxHostName);
    if (ofxHostName.empty()) ofxHostName = def.ofxHostName;
    // Reno used to default the OpenFX host name to VEGAS 13's ("com.sonycreativesoftware.vegas") and saved it; VEGAS
    // 18's effects need VEGAS 18's name. A saved old default becomes the new one once.
    if (ofxHostName == "com.sonycreativesoftware.vegas" && !q.value("prefs/ofxHostName18", false).toBool()) ofxHostName = def.ofxHostName;
    vegasExtensions = q.value("prefs/vegasExtensions", def.vegasExtensions).toBool();
    stubUnknownSuites = q.value("prefs/stubUnknownSuites", def.stubUnknownSuites).toBool();
    audioDevice = str("audioDevice", def.audioDevice);
    downmixOutput = q.value("prefs/downmixOutput", def.downmixOutput).toBool();
    multichannelImport = str("multichannelImport", def.multichannelImport);
    if (multichannelImport != "pairs" && multichannelImport != "mixdown") multichannelImport = "tracks";
    panMultichannelTracks = q.value("prefs/panMultichannelTracks", def.panMultichannelTracks).toBool();
    audioEditor = str("audioEditor", def.audioEditor);
    cutToOverlap = std::min(60.0, std::max(0.01, q.value("prefs/cutToOverlap", def.cutToOverlap).toDouble()));
    theme = str("theme", def.theme) == "vegas" ? "vegas" : "reno";
    audioInputDevice = str("audioInputDevice", def.audioInputDevice);
    recordFolder = str("recordFolder", def.recordFolder);
    prerenderFolder = str("prerenderFolder", def.prerenderFolder);
    actionSafe = std::min(45.0, std::max(0.0, q.value("prefs/actionSafe", def.actionSafe).toDouble()));
    titleSafe = std::min(45.0, std::max(0.0, q.value("prefs/titleSafe", def.titleSafe).toDouble()));
    gridColumns = std::min(64, std::max(1, q.value("prefs/gridColumns", def.gridColumns).toInt()));
    gridRows = std::min(64, std::max(1, q.value("prefs/gridRows", def.gridRows).toInt()));
    videoThumbnails = str("videoThumbnails", def.videoThumbnails);
    if (videoThumbnails != "headCenterTail" && videoThumbnails != "headTail" && videoThumbnails != "head" && videoThumbnails != "none") videoThumbnails = "all";
    thumbnailNumbers = str("thumbnailNumbers", def.thumbnailNumbers);
    if (thumbnailNumbers != "frames" && thumbnailNumbers != "time" && thumbnailNumbers != "timecode") thumbnailNumbers = "none";
    loopEvents = q.value("prefs/loopEvents", def.loopEvents).toBool();
    preservePitch = q.value("prefs/preservePitch", def.preservePitch).toBool();
    // earlier builds saved "off" here as their default; stretched sound now keeps its pitch (elastique, as in VEGAS)
    // unless switched off since
    if (!q.value("prefs/stretchDefaults2").toBool()) {
        preservePitch = true;
        q.setValue("prefs/preservePitch", true);
        q.setValue("prefs/stretchDefaults2", true);
    }
    cursorPreview = std::min(60.0, std::max(0.1, q.value("prefs/cursorPreview", def.cursorPreview).toDouble()));
    createAudioProxies = q.value("prefs/createAudioProxies", def.createAudioProxies).toBool();
    vegasPeakFiles = q.value("prefs/vegasPeakFiles", def.vegasPeakFiles).toBool();
    quickFadeEdits = q.value("prefs/quickFadeEdits2", def.quickFadeEdits).toBool();
    quickFadeMs = std::min(1000.0, std::max(1.0, q.value("prefs/quickFadeMs", def.quickFadeMs).toDouble()));
    newProjectSettings = str("newProjectSettings", def.newProjectSettings);
    renderThreads = std::min(16, std::max(0, q.value("prefs/renderThreads", def.renderThreads).toInt()));
    ramPreviewMb = std::min(65536, std::max(0, q.value("prefs/ramPreviewMb", def.ramPreviewMb).toInt()));
    timeSelEnvFades = q.value("prefs/timeSelEnvFades", def.timeSelEnvFades).toBool();
    timeSelEnvFadeLength = std::min(60.0, std::max(0.0, q.value("prefs/timeSelEnvFadeLength", def.timeSelEnvFadeLength).toDouble()));
    timeSelEnvFadeAlign = str("timeSelEnvFadeAlign", def.timeSelEnvFadeAlign);
    autoOverlapMedia = q.value("prefs/autoOverlapMedia", def.autoOverlapMedia).toBool();
    noQuantizeAudioOnly = q.value("prefs/noQuantizeAudioOnly", def.noQuantizeAudioOnly).toBool();
    thinAutomation = q.value("prefs/thinAutomation", def.thinAutomation).toBool();
    rightMouseSelect = q.value("prefs/rightMouseSelect", def.rightMouseSelect).toBool();
    autoSaveKeep = std::min(50, std::max(1, q.value("prefs/autoSaveKeep", def.autoSaveKeep).toInt()));
}

void Preferences::save() const
{
    QSettings q;
    q.setValue("prefs/previewQuality", QString::fromStdString(previewQuality));
    q.setValue("prefs/actionSafe", actionSafe);
    q.setValue("prefs/titleSafe", titleSafe);
    q.setValue("prefs/gridColumns", gridColumns);
    q.setValue("prefs/gridRows", gridRows);
    q.setValue("prefs/videoThumbnails", QString::fromStdString(videoThumbnails));
    q.setValue("prefs/thumbnailNumbers", QString::fromStdString(thumbnailNumbers));
    q.setValue("prefs/loopEvents", loopEvents);
    q.setValue("prefs/preservePitch", preservePitch);
    q.setValue("prefs/cursorPreview", cursorPreview);
    q.setValue("prefs/createAudioProxies", createAudioProxies);
    q.setValue("prefs/vegasPeakFiles", vegasPeakFiles);
    q.setValue("prefs/quickFadeEdits2", quickFadeEdits);
    q.setValue("prefs/quickFadeMs", quickFadeMs);
    q.setValue("prefs/newProjectSettings", QString::fromStdString(newProjectSettings));
    q.setValue("prefs/renderThreads", renderThreads);
    q.setValue("prefs/ramPreviewMb", ramPreviewMb);
    q.setValue("prefs/timeSelEnvFades", timeSelEnvFades);
    q.setValue("prefs/timeSelEnvFadeLength", timeSelEnvFadeLength);
    q.setValue("prefs/timeSelEnvFadeAlign", QString::fromStdString(timeSelEnvFadeAlign));
    q.setValue("prefs/autoOverlapMedia", autoOverlapMedia);
    q.setValue("prefs/noQuantizeAudioOnly", noQuantizeAudioOnly);
    q.setValue("prefs/thinAutomation", thinAutomation);
    q.setValue("prefs/rightMouseSelect", rightMouseSelect);
    q.setValue("prefs/autoSaveKeep", autoSaveKeep);
    q.setValue("prefs/undoLevels", undoLevels);
    q.setValue("prefs/autoSaveMinutes", autoSaveMinutes);
    q.setValue("prefs/stillLength", stillLength);
    q.setValue("prefs/generatedLength", generatedLength);
    q.setValue("prefs/cursorFollowsPlayback", cursorFollowsPlayback);
    q.setValue("prefs/scrollMode", QString::fromStdString(scrollMode));
    q.setValue("prefs/snapping", snapping);
    q.setValue("prefs/snapToGrid", snapToGrid);
    q.setValue("prefs/ofxHostName", QString::fromStdString(ofxHostName));
    q.setValue("prefs/ofxHostName18", true);
    q.setValue("prefs/vegasExtensions", vegasExtensions);
    q.setValue("prefs/stubUnknownSuites", stubUnknownSuites);
    q.setValue("prefs/audioDevice", QString::fromStdString(audioDevice));
    q.setValue("prefs/downmixOutput", downmixOutput);
    q.setValue("prefs/multichannelImport", QString::fromStdString(multichannelImport));
    q.setValue("prefs/panMultichannelTracks", panMultichannelTracks);
    q.setValue("prefs/audioEditor", QString::fromStdString(audioEditor));
    q.setValue("prefs/cutToOverlap", cutToOverlap);
    q.setValue("prefs/theme", QString::fromStdString(theme));
    q.setValue("prefs/audioInputDevice", QString::fromStdString(audioInputDevice));
    q.setValue("prefs/recordFolder", QString::fromStdString(recordFolder));
    q.setValue("prefs/prerenderFolder", QString::fromStdString(prerenderFolder));
}

void AppState::loadPreferences()
{
    prefs.load();
    snapping = prefs.snapping;
    applyPreferences();
}

void AppState::applyPreferences()
{
    prefs.save();
    trimUndo();
    // the OpenFX host: rebuilt only when it really changes (existing effects keep what they were told)
    ofx::HostConfig& hc = ofx::Host::get().config;
    if (hc.hostName != prefs.ofxHostName || hc.vegasExtensions != prefs.vegasExtensions || hc.stubUnknownSuites != prefs.stubUnknownSuites) {
        hc.hostName = prefs.ofxHostName;
        hc.vegasExtensions = prefs.vegasExtensions;
        hc.stubUnknownSuites = prefs.stubUnknownSuites;
        ofx::Host::get().applyConfig();
    }
    if (QCoreApplication::instance()) {
        if (!autoSaveTimer_) {
            autoSaveTimer_ = new QTimer(QCoreApplication::instance());
            QObject::connect(autoSaveTimer_, &QTimer::timeout, [] {
                AppState& a = AppState::get();
                std::string err;
                if (a.autoSaveInBackground()) return;
                if (!a.autoSaveNow(&err) && !err.empty()) a.status("Auto-save failed: " + err);
            });
        }
        if (prefs.autoSaveMinutes > 0) {
            const int ms = prefs.autoSaveMinutes * 60 * 1000;
            if (!autoSaveTimer_->isActive() || autoSaveTimer_->interval() != ms) autoSaveTimer_->start(ms);
        } else {
            autoSaveTimer_->stop();
        }
    }
    vprefs::reload();
    defaultFadeTypes().audio = vprefs::curveCode("prefs/curveAudioGain");
    defaultFadeTypes().video = vprefs::curveCode("prefs/curveVideoOpacity");
    setAudioProxyOptions(true, prefs.createAudioProxies);
    mediacache::vegasPeakFiles().store(prefs.vegasPeakFiles);
    // Preferences > Reno and the VEGAS ones that set how Reno works inside: threads, caches, proxy and peak folders...
    const bool builtinFxBefore = hc.preferBuiltinVegasFx;
    const bool builtinEfxBefore = efxPreferBuiltin();
    const bool builtinDxBefore = dxPreferBuiltin();
    const bool previewSizeBefore = hc.previewSizeForAll;
    prefsapply::apply();
    // the choice of VEGAS's plug-ins / Reno's built-in versions changed: effects are remade from the other; the effect
    // working size for previews changed: frames kept so far are stale
    if (hc.preferBuiltinVegasFx != builtinFxBefore || hc.previewSizeForAll != previewSizeBefore || efxPreferBuiltin() != builtinEfxBefore ||
        dxPreferBuiltin() != builtinDxBefore) afterChange();
    const double quick = prefs.quickFadeEdits ? std::min(1000.0, std::max(1.0, prefs.quickFadeMs)) / 1000.0 : 0.0;
    if (project_.quickFade != quick) {
        if (dragging_) dragBase_.quickFade = quick;
        afterChange();
    }
    notify(kPrefs);
}

std::string AppState::autoSavePath() const
{
    if (path_.empty()) return pathJoin(userDataDir(), "untitled.reno.bak");
    if (endsWithNoCase(path_, ".reno")) return path_ + ".bak";
    return path_ + ".reno.bak";
}

// The timer's auto-save for a big project: the copy is taken here (it shares the events, so it is quick) and written on
// a thread of its own, since writing tens of thousands of events stopped the window for a good part of a second. True if
// this handled the auto-save (or one is still being written); false: do it the plain way.
static std::atomic<bool> gAutoSaving{false};

// Preferences > Reno > Files "Auto-save backups kept": before a new auto-save is written the existing ones move up a
// place ("x.bak" -> "x.bak.1" -> "x.bak.2"...), the oldest beyond 'keep' dropped. keep 1: one file, overwritten.
static void rotateAutoSaves(const std::string& where, int keep)
{
    if (keep <= 1 || !fileExists(where)) return;
    keep = std::min(keep, 50);
    auto name = [&](int k) { return QString::fromStdString(where + "." + std::to_string(k)); };
    QFile::remove(name(keep - 1));
    for (int k = keep - 2; k >= 1; --k)
        if (QFile::exists(name(k))) QFile::rename(name(k), name(k + 1));
    QFile::remove(name(1));
    QFile::rename(QString::fromStdString(where), name(1));
}
bool AppState::autoSaveInBackground()
{
    if (!dirty_ || dragging_) return true;
    size_t events = 0;
    for (const Track& t : project_.tracks) events += t.events.size();
    if (events < 3000) return false;
    if (gAutoSaving.exchange(true)) return true;
    auto copy = std::make_shared<Project>(project_);
    fx.captureStates(*copy);
    const std::string where = autoSavePath();
    const int keep = prefs.autoSaveKeep;
    std::thread([copy, where, keep] {
        std::string err;
        rotateAutoSaves(where, keep);
        const bool ok = copy->save(where, &err);
        AppState::post([ok, err, where] {
            if (ok) AppState::get().status("Auto-saved to " + where);
            else AppState::get().status("Auto-save failed: " + err);
        });
        gAutoSaving = false;
    }).detach();
    return true;
}

bool AppState::autoSaveNow(std::string* error)
{
    if (!dirty_ || dragging_) return true;
    // a copy, so what is being edited is not touched: its settings are brought up to date in the copy only
    Project copy = project_;
    fx.captureStates(copy);
    const std::string where = autoSavePath();
    rotateAutoSaves(where, prefs.autoSaveKeep);
    if (!copy.save(where, error)) return false;
    status("Auto-saved to " + where);
    return true;
}

bool AppState::save(const std::string& path, std::string* error)
{
    fx.captureStates(project_);
    if (endsWithNoCase(path, ".veg")) {
        // the project is tied to the file written, so the next save starts from it
        const VegasSaveResult r = saveVegasProject(project_, path);
        if (!r.ok) {
            if (error) *error = r.error;
            return false;
        }
        saveWarnings_ = r.warnings;
    } else {
        if (!project_.save(path, error)) return false;
        saveWarnings_.clear();
    }
    path_ = path;
    dirty_ = false;
    {
        std::lock_guard<std::mutex> lk(snapMutex_);
        snapshot_ = std::make_shared<const Project>(project_);
    }
    notify(kFile);
    return true;
}

// ------------------------------------------------------------------------------------------ media
uint64_t AppState::importMedia(const std::string& path, std::string* error)
{
    for (const auto& m : project_.media)
        if (!m.generated && m.path == path) return m.id;
    std::string err;
    std::unique_ptr<MediaReader> r = mediaOpener(path, &err);
    if (!r) {
        if (error) *error = err;
        return 0;
    }
    MediaItem m;
    m.path = path;
    m.name = pathFileName(path);
    m.info = r->info();
    uint64_t id = 0;
    edit("Add Media", [&](Project& p) {
        m.id = id = p.newId();
        p.media.push_back(m);
    });
    // a proxy soon: Preferences' rules for media that comes in at once; the rest are the automatic preview proxies of
    // media the project uses (autoproxy.h)
    if (!hasVideoProxy(path)) {
        autoproxy::Worker::get().hint(path, prefsapply::wantsAutoProxy(m.info));
        autoproxy::Worker::get().projectChanged(snapshot());
    }
    logbook::write(logbook::Kind::Reads, logbook::Basic, QString("Opened %1").arg(QString::fromStdString(path)));
    logbook::write(logbook::Kind::Reads, logbook::Detailed,
                   QString("  video %1 %2 x %3 at %4 fps, audio %5 %6 Hz %7 ch, %8 s")
                       .arg(m.info.hasVideo ? QString::fromStdString(m.info.videoCodec) : QString("none")).arg(m.info.width).arg(m.info.height).arg(m.info.fps, 0, 'f', 3)
                       .arg(m.info.hasAudio ? QString::fromStdString(m.info.audioCodec) : QString("none")).arg(m.info.sampleRate).arg(m.info.channels).arg(m.info.duration, 0, 'f', 3));
    return id;
}

uint64_t AppState::importImageSequence(const std::string& firstFile, double fps, std::string* error)
{
    if (!(fps > 0 && fps <= 1000)) {
        if (error) *error = "The frame rate must be above 0.";
        return 0;
    }
    for (const auto& m : project_.media)
        if (!m.generated && m.path == firstFile && m.sequenceFps == fps) return m.id;
    const int count = imageSequenceLength(firstFile);
    if (count < 2) {
        if (error) *error = pathFileName(firstFile) + (count ? " is not followed by more numbered pictures." : " has no number at the end of its name.");
        return 0;
    }
    std::string err;
    MediaItem m;
    m.path = firstFile;
    m.sequenceFps = fps;
    std::unique_ptr<MediaReader> r = mediaOpener(m.readPath(), &err);
    if (!r || !r->info().hasVideo) {
        if (error) *error = err.empty() ? "The pictures cannot be read as a sequence." : err;
        return 0;
    }
    m.info = r->info();
    m.info.path = firstFile;
    std::string name = pathFileName(firstFile);
    {
        const size_t dot = name.find_last_of('.');
        size_t end = dot == std::string::npos ? name.size() : dot, begin = end;
        while (begin > 0 && name[begin - 1] >= '0' && name[begin - 1] <= '9') --begin;
        name = name.substr(0, begin) + std::string(end - begin, '#') + name.substr(end);
    }
    m.name = name;
    uint64_t id = 0;
    edit("Add Media", [&](Project& p) {
        m.id = id = p.newId();
        p.media.push_back(m);
    });
    return id;
}

uint64_t AppState::addGeneratedMedia(const std::string& pluginId, const std::string& label)
{
    uint64_t id = 0;
    edit("Add Generated Media", [&](Project& p) {
        MediaItem m;
        m.id = id = p.newId();
        m.name = label;
        m.generated = true;
        m.generator.id = p.newId();
        m.generator.kind = "ofx";
        m.generator.pluginId = pluginId;
        m.generator.name = label;
        m.info.hasVideo = true;
        m.info.width = p.settings.width;
        m.info.height = p.settings.height;
        m.info.fps = p.settings.fps();
        p.media.push_back(m);
    });
    return id;
}

static bool rangeFree(const Track& t, double a, double b)
{
    for (const auto& e : t.events)
        if (e.start < b - 1e-9 && e.end() > a + 1e-9) return false;
    return true;
}

uint64_t AppState::addTrack(TrackType type, int index)
{
    uint64_t id = 0;
    edit(type == TrackType::Video ? "Insert Video Track" : "Insert Audio Track", [&](Project& p) {
        Track t;
        t.id = id = p.newId();
        t.type = type;
        if (type == TrackType::Audio) t.panType = "add";
        if (onNewTrack) onNewTrack(p, t);
        if (index < 0 || index > (int)p.tracks.size()) {
            index = (int)p.tracks.size();
            if (type == TrackType::Video) {
                index = 0;
                for (size_t k = 0; k < p.tracks.size(); ++k) if (p.tracks[k].type == TrackType::Video) index = (int)k + 1;
            }
        }
        p.tracks.insert(p.tracks.begin() + index, t);
    });
    return id;
}

void AppState::removeTrack(uint64_t trackId)
{
    edit("Delete Track", [&](Project& p) {
        p.tracks.erase(std::remove_if(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return t.id == trackId; }), p.tracks.end());
    });
}

// VEGAS's "Set pan for new tracks from multichannel": a new track made for some of a multichannel file's channels is
// panned to where they belong: in 5.1 its surround panner on that speaker (front or rear pair to front or rear speakers,
// LFE to LFE alone), in stereo the pan of a left or right channel to that side.
static void panForChannels(const Project& p, Track& t, const MediaInfo& mi, int first, int count)
{
    auto name = [&](int c) { return c >= 0 && c < (int)mi.channelNames.size() ? mi.channelNames[(size_t)c] : std::string(); };
    const std::string a = name(first), b = count >= 2 ? name(first + 1) : std::string();
    const bool left = !a.empty() && a[0] == 'L' && a != "LFE", right = !a.empty() && a[0] == 'R';
    if (!p.settings.surround()) {
        if (count < 2) t.pan = left ? -1.0 : (right ? 1.0 : 0.0);
        return;
    }
    SurroundPan& s = t.surround;
    if (count >= 2) {
        if (a == "L" && b == "R") s.route = "front";
        else if ((a == "Ls" || a == "Lb") && (b == "Rs" || b == "Rb")) s.route = "rear";
        return;
    }
    if (a == "LFE") {
        s.route = "lfe";
        return;
    }
    const bool rear = a == "Ls" || a == "Rs" || a == "Lb" || a == "Rb" || a == "Cs";
    if (a.empty()) return;
    s.x = left ? -1.0 : (right ? 1.0 : 0.0);
    s.y = rear ? -1.0 : 1.0;
}

std::vector<uint64_t> AppState::addMediaToTimeline(uint64_t mediaId, double time, uint64_t trackHint)
{
    std::vector<uint64_t> made;
    const MediaItem* m0 = project_.findMedia(mediaId);
    if (!m0) return made;
    const MediaItem m = *m0;
    const MediaRange range = nextAddRange;
    nextAddRange = MediaRange();
    const bool ranged = range.mediaId == mediaId && range.out > range.in && !m.generated && !m.info.isStill;
    const int streams = range.mediaId == mediaId && m.info.hasVideo && m.info.hasAudio ? range.streams : 0;
    const bool wantVideo = m.info.hasVideo && streams != 2, wantAudio = m.info.hasAudio && streams != 1;
    edit("Add to Timeline", [&](Project& p) {
        const double start = std::max(0.0, p.quantize(time));
        double len = m.info.duration;
        if (m.generated) len = prefs.generatedLength;
        else if (m.info.isStill || len <= 0) len = prefs.stillLength;
        if (ranged) len = range.out - range.in;
        else if (m.subclipLength > 0 && !m.generated && !m.info.isStill) len = m.subclipLength;
        const double end = start + len;
        // A file with more than two channels (Preferences > Audio): one event per channel or pair, each on its own
        // track, or (mixdown) one event mixed to stereo.
        std::vector<std::pair<int, int>> picks;
        // Preferences > General "Import MXF as multichannel" cleared: an MXF file's sound is one stereo event; "Import
        // stereo as dual mono": a two-channel file is two mono events on two tracks, panned hard left and right
        const bool mxfStereo = endsWithNoCase(m.path, ".mxf") && !vprefs::on("prefs/mxfMultichannel");
        const bool dualMono = wantAudio && m.info.channels == 2 && vprefs::on("prefs/stereoDualMono");
        if (wantAudio && m.info.channels > 2 && prefs.multichannelImport != "mixdown" && !mxfStereo) {
            const int n = std::min(m.info.channels, 64);
            const bool pairs = prefs.multichannelImport == "pairs";
            for (int c = 0; c < n; c += pairs ? 2 : 1) picks.emplace_back(c, pairs && c + 1 < n ? 2 : 1);
        }
        if (dualMono) {
            picks.emplace_back(0, 1);
            picks.emplace_back(1, 1);
        }
        const uint64_t group = ((wantVideo && wantAudio) || picks.size() > 1) ? p.newId() : 0;
        bool madeTrack = false;
        auto place = [&](TrackType type, int first = -1, int count = 0) {
            madeTrack = false;
            Track* target = nullptr;
            if (Track* hint = p.findTrack(trackHint))
                if (hint->type == type && rangeFree(*hint, start, end)) target = hint;
            if (!target)
                for (auto& t : p.tracks)
                    if (t.type == type && rangeFree(t, start, end)) { target = &t; break; }
            if (!target) {
                Track t;
                t.id = p.newId();
                t.type = type;
                if (type == TrackType::Audio) t.panType = "add";
                size_t at = p.tracks.size();
                if (type == TrackType::Video) at = 0;
                p.tracks.insert(p.tracks.begin() + (std::ptrdiff_t)at, t);
                target = &p.tracks[at];
                madeTrack = true;
            }
            Event e;
            e.id = p.newId();
            e.mediaId = m.id;
            e.start = start;
            e.length = len;
            if (ranged) e.offset = range.in;
            else if (m.subclipLength > 0 && !m.generated && !m.info.isStill) e.offset = m.subclipStart;
            e.group = group;
            e.name = m.name;
            e.loop = prefs.loopEvents;
            setDefaultFadeCurves(e, type);
            // Preferences > Reno > Editing: the fade lengths a new event starts with, each at most half its length
            if (len > 0 && !m.info.isStill) {
                e.fadeIn = std::min(len * 0.5, std::max(0, vprefs::number("prefs/defaultFadeInMs")) / 1000.0);
                e.fadeOut = std::min(len * 0.5, std::max(0, vprefs::number("prefs/defaultFadeOutMs")) / 1000.0);
            }
            if (type == TrackType::Audio) e.keepPitch = prefs.preservePitch;
            // Preferences > Editing, "Automatically crop still images added to timeline": the still is cropped to the
            // project frame's shape (Event Pan/Crop) so it fills the frame
            if (type == TrackType::Video && m.info.isStill && !m.generated && vprefs::on("prefs/autoCropStills") && m.info.width > 0 && m.info.height > 0 &&
                p.settings.width > 0 && p.settings.height > 0) {
                const double media = (double)m.info.width / m.info.height, frame = (double)p.settings.width / p.settings.height;
                PanCropKey k;
                if (media > frame) k.w = frame / media;
                else k.h = media / frame;
                if (media != frame) e.panCrop.push_back(k);
            }
            if (first >= 0) {
                e.mediaChannel = first;
                e.mediaChannels = count;
            }
            target->events.push_back(e);
            target->sortEvents();
            made.push_back(e.id);
            if (first >= 0 && madeTrack && dualMono) target->pan = first == 0 ? -1.0 : 1.0;
            else if (first >= 0 && madeTrack && prefs.panMultichannelTracks) panForChannels(p, *target, m.info, first, count);
        };
        if (wantVideo) place(TrackType::Video);
        if (wantAudio && picks.empty()) place(TrackType::Audio);
        for (const auto& pk : picks) place(TrackType::Audio, pk.first, pk.second);
    });
    return made;
}

// ------------------------------------------------------------------------------------------ effects
FxChain* AppState::chainOf(Project& p, const FxTarget& t) const
{
    switch (t.kind) {
    case FxTarget::Event: { Event* e = p.findEvent(t.id); return e ? &e->fx : nullptr; }
    case FxTarget::Track: { Track* tr = p.findTrack(t.id); return tr ? &tr->fx : nullptr; }
    case FxTarget::VideoOut: return &p.videoOut;
    case FxTarget::AudioOut: return &p.audioOut;
    case FxTarget::Bus: { reno::Bus* b = p.findBus(t.id); return b ? &b->fx : nullptr; }
    default: return nullptr;
    }
}

const FxChain* AppState::chainOf(const Project& p, const FxTarget& t) const
{
    return chainOf(const_cast<Project&>(p), t);
}

bool AppState::targetIsVideo(const FxTarget& t) const
{
    switch (t.kind) {
    case FxTarget::Event: {
        const Track* tr = nullptr;
        return project_.findEvent(t.id, &tr) && tr && tr->type == TrackType::Video;
    }
    case FxTarget::Track: {
        Track* tr = const_cast<Project&>(project_).findTrack(t.id);
        return tr && tr->type == TrackType::Video;
    }
    case FxTarget::VideoOut: return true;
    default: return false;
    }
}

std::string AppState::targetName(const FxTarget& t) const
{
    switch (t.kind) {
    case FxTarget::Event: {
        const Event* e = project_.findEvent(t.id);
        return std::string("Event: ") + (e ? e->name : std::string("?"));
    }
    case FxTarget::Track: {
        for (size_t k = 0; k < project_.tracks.size(); ++k)
            if (project_.tracks[k].id == t.id)
                return project_.tracks[k].name.empty() ? strfmt("Track %d", (int)k + 1) : "Track: " + project_.tracks[k].name;
        return "Track";
    }
    case FxTarget::VideoOut: return "Video Output";
    case FxTarget::AudioOut: return "Master Bus";
    case FxTarget::Bus: {
        const reno::Bus* b = project_.findBus(t.id);
        return b ? "Bus: " + b->name : std::string("Bus");
    }
    default: return std::string();
    }
}

uint64_t AppState::addVideoFx(const FxTarget& t, const std::string& pluginId, const std::string& label)
{
    uint64_t id = 0;
    edit("Add Video Effect", [&](Project& p) {
        FxChain* c = chainOf(p, t);
        if (!c) return;
        FxSlot s;
        s.id = id = p.newId();
        s.kind = "ofx";
        s.pluginId = pluginId;
        s.name = label;
        c->push_back(s);
    });
    return id;
}

uint64_t AppState::addAudioFx(const FxTarget& t, const AudioPluginDesc& desc)
{
    uint64_t id = 0;
    edit("Add Audio Effect", [&](Project& p) {
        FxChain* c = chainOf(p, t);
        if (!c) return;
        FxSlot s;
        s.id = id = p.newId();
        s.kind = "audio";
        s.audio = desc;
        s.name = desc.name;
        c->push_back(s);
    });
    return id;
}

void AppState::removeFx(const FxTarget& t, uint64_t slotId)
{
    edit("Remove Effect", [&](Project& p) {
        FxChain* c = chainOf(p, t);
        if (!c) return;
        c->erase(std::remove_if(c->begin(), c->end(), [&](const FxSlot& s) { return s.id == slotId; }), c->end());
    });
}

void AppState::moveFx(const FxTarget& t, uint64_t slotId, int delta)
{
    edit("Reorder Effects", [&](Project& p) {
        FxChain* c = chainOf(p, t);
        if (!c) return;
        for (size_t k = 0; k < c->size(); ++k)
            if ((*c)[k].id == slotId) {
                const std::ptrdiff_t to = (std::ptrdiff_t)k + delta;
                if (to < 0 || to >= (std::ptrdiff_t)c->size()) return;
                std::swap((*c)[k], (*c)[(size_t)to]);
                return;
            }
    });
}

void AppState::setFxBypass(const FxTarget& t, uint64_t slotId, bool bypass)
{
    edit(bypass ? "Bypass Effect" : "Enable Effect", [&](Project& p) {
        FxChain* c = chainOf(p, t);
        if (!c) return;
        for (auto& s : *c) if (s.id == slotId) s.bypass = bypass;
    });
}

void AppState::setTransition(uint64_t eventId, const std::string& pluginId, const std::string& label)
{
    edit("Change Transition", [&](Project& p) {
        Event* e = p.findEvent(eventId);
        if (!e) return;
        e->transition = FxSlot();
        if (pluginId.empty()) return;
        e->transition.id = p.newId();
        e->transition.kind = "ofx";
        e->transition.pluginId = pluginId;
        e->transition.name = label;
    });
}

// ------------------------------------------------------------------------------------------ selection, cursor
void AppState::setSelection(const std::set<uint64_t>& ids)
{
    if (ids == selection_) return;
    selection_ = ids;
    notify(kSelection);
}

void AppState::selectOnly(uint64_t eventId)
{
    std::set<uint64_t> s;
    if (eventId) s.insert(eventId);
    setSelection(s);
}

void AppState::setCursor(double seconds)
{
    if (!(seconds > 0)) seconds = 0;
    if (seconds == cursor_) return;
    cursor_ = seconds;
    notify(kCursor);
}

void AppState::setLoop(double a, double b)
{
    loopStart = std::max(0.0, std::min(a, b));
    loopEnd = std::max(a, b);
    notify(kCursor);
}

// ------------------------------------------------------------------------------------------ editing commands
static void renewFx(Project& p, Event& e)
{
    for (auto& s : e.fx) s.id = p.newId();
    if (!e.transition.kind.empty()) e.transition.id = p.newId();
}

// Scene detection: the frame is read about every 1/fps (at most 25 a second) as a tiny grey image; a cut is where the
// mean change from the last frame is both large and several times the recent average.
std::vector<double> AppState::detectScenes(uint64_t eventId, int sensitivity)
{
    std::vector<double> cuts;
    const Track* tr = nullptr;
    const Event* e = project_.findEvent(eventId, &tr);
    if (!e || !tr || tr->type != TrackType::Video || e->length <= 0 || e->adjustment) return cuts;
    const MediaItem* m = project_.findMedia(e->mediaId);
    if (!m || m->generated || !m->info.hasVideo) return cuts;
    std::string err;
    std::unique_ptr<MediaReader> r = mediaOpener ? mediaOpener(m->path, &err) : openMediaOrProject(m->path, &err);
    if (!r) return cuts;
    sensitivity = std::max(0, std::min(100, sensitivity));
    const double threshold = 0.30 - 0.0027 * sensitivity;
    double fps = m->info.fps > 0 ? m->info.fps : 30.0;
    fps = std::min(fps, 25.0);
    const double step = 1.0 / fps;
    const double rate = e->rate > 0 ? e->rate : 1.0;
    const int gw = 32, gh = 18;
    auto grab = [&](double mediaTime, std::vector<float>& g) {
        ImagePtr img = r->videoFrame(mediaTime, 160, 90);
        if (!img || img->w < 1 || img->h < 1) return false;
        g.assign((size_t)gw * gh, 0.f);
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const uint8_t* px = img->row(y * img->h / gh) + (size_t)(x * img->w / gw) * 4;
                g[(size_t)y * gw + x] = (0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2]) / 255.f;
            }
        return true;
    };
    std::vector<float> prev, cur;
    const long n = (long)std::min(e->length * rate / step, 200000.0);
    double avg = 0;
    long lastCut = -1000;
    for (long k = 0; k <= n; ++k) {
        const double mt = e->offset + k * step;
        if (!grab(mt, cur)) break;
        if (!prev.empty()) {
            double d = 0;
            for (size_t i = 0; i < cur.size(); ++i) d += std::fabs(cur[i] - prev[i]);
            d /= (double)cur.size();
            if (d > threshold && d > 3.0 * avg && k - lastCut > 2) {
                cuts.push_back(e->start + (k * step) / rate);
                lastCut = k;
            }
            avg = 0.9 * avg + 0.1 * d;
        }
        prev = cur;
    }
    return cuts;
}

int AppState::splitAtScenes(int sensitivity)
{
    std::set<uint64_t> ids = selection_;
    std::vector<double> all;
    for (uint64_t id : ids)
        for (double t : detectScenes(id, sensitivity)) all.push_back(t);
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end(), [](double a, double b) { return std::fabs(a - b) < 1e-6; }), all.end());
    if (all.empty()) { status("Detect Scenes: no scene changes found in the selected video events."); return 0; }
    const double keep = cursor_;
    for (double t : all) {
        cursor_ = t;
        splitAtCursor();
    }
    cursor_ = keep;
    status("Detect Scenes: split at " + std::to_string(all.size()) + " scene change(s).");
    notify(kSelection);
    return (int)all.size();
}

// ---- changing many events without unsharing every track. A track's events are shared with the undo steps until changed, so a non-const loop over all tracks would copy them all; these look through the const side first and change only tracks that have an event they want.
template <class Want, class Change>
static void changeEventsWhere(Project& p, Want want, Change change)
{
    for (Track& tr : p.tracks) {
        const CowVec<Event>& cv = tr.events;
        bool any = false;
        for (const Event& e : cv) if (want(e)) { any = true; break; }
        if (!any) continue;
        for (Event& e : tr.events) if (want(e)) change(e);
    }
}
template <class Want>
static void eraseEventsWhere(Project& p, Want want)
{
    for (Track& tr : p.tracks) {
        const CowVec<Event>& cv = tr.events;
        bool any = false;
        for (const Event& e : cv) if (want(e)) { any = true; break; }
        if (!any) continue;
        tr.events.erase(std::remove_if(tr.events.begin(), tr.events.end(), [&](const Event& e) { return want(e); }), tr.events.end());
    }
}

void AppState::splitAtCursor()
{
    const double t = project_.quantize(cursor_);
    std::vector<uint64_t> ids;
    for (const auto& tr : project_.tracks)
        for (const auto& e : tr.events)
            if (e.start < t - 1e-9 && e.end() > t + 1e-9 && (selection_.empty() || selection_.count(e.id)) && !e.locked) ids.push_back(e.id);
    if (ids.empty()) return;
    std::set<uint64_t> newSel = selection_;
    edit("Split", [&](Project& p) {
        fx.captureStates(p);
        std::map<uint64_t, uint64_t> groups;
        std::set<Track*> touched;
        for (uint64_t id : ids) {
            Track* tr = nullptr;
            Event* e = p.findEvent(id, &tr);
            if (!e || !tr) continue;
            Event right = *e;
            right.id = p.newId();
            right.start = t;
            right.length = e->end() - t;
            right.offset = e->offset + (t - e->start) * e->rate;
            for (Take& k : right.takes) k.offset += (t - e->start) * e->rate;
            shiftEventEnvelopes(right, t - e->start);
            shiftEventKeyframes(right, t - e->start);
            right.fadeIn = 0;
            right.transition = FxSlot();
            renewFx(p, right);
            if (e->group) {
                auto it = groups.find(e->group);
                if (it == groups.end()) it = groups.emplace(e->group, p.newId()).first;
                right.group = it->second;
            }
            e->length = t - e->start;
            e->fadeOut = 0;
            if (selection_.count(id)) newSel.insert(right.id);
            tr->events.push_back(right);
            touched.insert(tr);
        }
        // once per track, not per event; same order: a stable sort puts each new half after the events that start with it
        for (Track* tr : touched) tr->sortEvents();
    });
    setSelection(newSel);
}

void AppState::deleteSelection()
{
    if (selection_.empty()) return;
    std::set<uint64_t> ids = selection_;
    for (uint64_t id : lockedAmong(ids)) ids.erase(id);
    if (ids.empty()) { status("The selected events are locked and cannot be deleted."); return; }
    edit("Delete", [&](Project& p) {
        eraseEventsWhere(p, [&](const Event& e) { return ids.count(e.id) != 0; });
    });
}

void AppState::copySelection()
{
    if (selection_.empty()) return;
    fx.captureStates(project_);
    clipboard_.clear();
    int firstTrack = -1;
    double first = 1e300;
    for (size_t k = 0; k < project_.tracks.size(); ++k)
        for (const auto& e : project_.tracks[k].events)
            if (selection_.count(e.id)) {
                if (firstTrack < 0) firstTrack = (int)k;
                first = std::min(first, e.start);
                clipboard_.push_back({e, project_.tracks[k].type, (int)k - firstTrack});
            }
    clipboardStart_ = first;
    notify(kSelection);
}

void AppState::cutSelection()
{
    copySelection();
    if (selection_.empty()) return;
    std::set<uint64_t> ids = selection_;
    for (uint64_t id : lockedAmong(ids)) ids.erase(id);
    if (ids.empty()) { status("The selected events are locked; they were copied but not cut."); return; }
    edit("Cut", [&](Project& p) {
        eraseEventsWhere(p, [&](const Event& e) { return ids.count(e.id) != 0; });
    });
}

void AppState::pasteAtCursor()
{
    if (clipboard_.empty()) return;
    const double at = project_.quantize(cursor_);
    std::set<uint64_t> made;
    edit("Paste", [&](Project& p) {
        // onto the current track (if right for the first item) and the tracks below it, else where they came from
        int base = -1;
        for (size_t k = 0; k < p.tracks.size(); ++k)
            if (p.tracks[k].id == currentTrack && p.tracks[k].type == clipboard_[0].type) base = (int)k;
        if (base < 0)
            for (size_t k = 0; k < p.tracks.size() && base < 0; ++k)
                if (p.tracks[k].type == clipboard_[0].type) base = (int)k;
        if (base < 0) return;
        std::map<uint64_t, uint64_t> groups;
        std::set<size_t> touchedTracks;
        for (const auto& c : clipboard_) {
            int idx = base + c.trackOffset;
            // the nearest track of the right kind at or after where it should go
            while (idx < (int)p.tracks.size() && p.tracks[(size_t)idx].type != c.type) ++idx;
            if (idx < 0 || idx >= (int)p.tracks.size()) continue;
            Event e = c.event;
            e.id = p.newId();
            e.start = at + (c.event.start - clipboardStart_);
            renewFx(p, e);
            if (e.group) {
                auto it = groups.find(e.group);
                if (it == groups.end()) it = groups.emplace(e.group, p.newId()).first;
                e.group = it->second;
            }
            if (!p.findMedia(e.mediaId)) continue;
            p.tracks[(size_t)idx].events.push_back(e);
            touchedTracks.insert((size_t)idx);
            made.insert(e.id);
        }
        // once per track, not per event: the same order, since a stable sort puts each pasted event after the ones that start with it
        for (size_t idx : touchedTracks) p.tracks[idx].sortEvents();
    });
    if (!made.empty()) setSelection(made);
}

void AppState::groupSelection(bool group)
{
    if (selection_.empty()) return;
    const std::set<uint64_t> ids = selection_;
    edit(group ? "Group" : "Ungroup", [&](Project& p) {
        const uint64_t g = group ? p.newId() : 0;
        changeEventsWhere(p, [&](const Event& e) { return ids.count(e.id) != 0; }, [&](Event& e) { e.group = g; });
    });
}

void AppState::setLocked(const std::set<uint64_t>& ids, bool locked)
{
    if (ids.empty()) return;
    edit(locked ? "Lock Events" : "Unlock Events", [&](Project& p) {
        for (uint64_t id : ids)
            if (Event* e = p.findEvent(id)) e->locked = locked;
    });
}

std::set<uint64_t> AppState::lockedAmong(const std::set<uint64_t>& ids) const
{
    std::set<uint64_t> out;
    for (uint64_t id : ids)
        if (const Event* e = project_.findEvent(id))
            if (e->locked) out.insert(id);
    return out;
}

// ------------------------------------------------------------------------------------------ takes
std::vector<Take> AppState::takesOf(const Event& e)
{
    const int at = std::max(0, std::min(e.activeTake, (int)e.takes.size()));
    std::vector<Take> all(e.takes.begin(), e.takes.begin() + at);
    Take active;
    active.mediaId = e.mediaId;
    active.offset = e.offset;
    active.name = e.name;
    all.push_back(active);
    all.insert(all.end(), e.takes.begin() + at, e.takes.end());
    return all;
}

void AppState::setActiveTake(Event& e, int index)
{
    std::vector<Take> all = takesOf(e);
    if (index < 0 || index >= (int)all.size()) return;
    const Take t = all[(size_t)index];
    e.mediaId = t.mediaId;
    e.offset = t.offset;
    e.name = t.name;
    all.erase(all.begin() + index);
    e.takes = all;
    e.activeTake = index;
}

static std::vector<uint64_t> eventsWithTakes(const Project& p, const std::set<uint64_t>& ids)
{
    std::set<uint64_t> groups;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (ids.count(e.id) && e.group) groups.insert(e.group);
    std::vector<uint64_t> out;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if ((ids.count(e.id) || (e.group && groups.count(e.group))) && !e.takes.empty()) out.push_back(e.id);
    return out;
}

bool AppState::stepTakes(const std::set<uint64_t>& ids, int dir)
{
    const std::vector<uint64_t> evs = eventsWithTakes(project_, ids);
    if (evs.empty()) { status("The selected events have only one take (Edit > Take > Add adds more)."); return false; }
    std::string shown;
    edit(dir > 0 ? "Next Take" : "Previous Take", [&](Project& p) {
        for (uint64_t id : evs)
            if (Event* e = p.findEvent(id)) {
                const int n = (int)e->takes.size() + 1;
                setActiveTake(*e, ((std::max(0, std::min(e->activeTake, n - 1)) + dir) % n + n) % n);
                shown = strfmt("Take %d of %d: ", e->activeTake + 1, n) + e->name;
            }
    });
    status(shown);
    return true;
}

bool AppState::chooseTake(const std::set<uint64_t>& ids, int index, const std::string& what)
{
    std::vector<uint64_t> evs;
    for (uint64_t id : eventsWithTakes(project_, ids))
        if (const Event* e = project_.findEvent(id))
            if (index >= 0 && index <= (int)e->takes.size() && index != e->activeTake) evs.push_back(id);
    if (evs.empty()) return false;
    edit(what, [&](Project& p) {
        for (uint64_t id : evs)
            if (Event* e = p.findEvent(id)) setActiveTake(*e, index);
    });
    return true;
}

int AppState::addTakes(uint64_t eventId, const std::vector<uint64_t>& media)
{
    const Track* tr = nullptr;
    const Event* e = project_.findEvent(eventId, &tr);
    if (!e || !tr) return 0;
    std::vector<uint64_t> ok;
    for (uint64_t m : media)
        if (const MediaItem* mi = project_.findMedia(m))
            if (tr->type == TrackType::Video ? (mi->info.hasVideo || mi->generated) : mi->info.hasAudio) ok.push_back(m);
    if (ok.empty()) return 0;
    edit(ok.size() > 1 ? "Add Takes" : "Add Take", [&](Project& p) {
        Event* ev = p.findEvent(eventId);
        if (!ev) return;
        for (uint64_t m : ok) {
            Take t;
            t.mediaId = m;
            t.offset = 0;
            if (const MediaItem* mi = p.findMedia(m)) t.name = mi->name;
            ev->takes.push_back(t);
        }
    });
    return (int)ok.size();
}

bool AppState::deleteActiveTake(uint64_t eventId)
{
    const Event* e = project_.findEvent(eventId);
    if (!e || e->takes.empty()) return false;
    edit("Delete Active Take", [&](Project& p) {
        Event* ev = p.findEvent(eventId);
        if (!ev || ev->takes.empty()) return;
        std::vector<Take> all = takesOf(*ev);
        const int at = std::max(0, std::min(ev->activeTake, (int)all.size() - 1));
        all.erase(all.begin() + at);
        const int next = std::min(at, (int)all.size() - 1);
        ev->mediaId = all[(size_t)next].mediaId;
        ev->offset = all[(size_t)next].offset;
        ev->name = all[(size_t)next].name;
        all.erase(all.begin() + next);
        ev->takes = all;
        ev->activeTake = next;
    });
    return true;
}

bool AppState::deleteTakes(uint64_t eventId, const std::set<int>& indices)
{
    const Event* e = project_.findEvent(eventId);
    if (!e || indices.empty()) return false;
    const int n = (int)e->takes.size() + 1;
    int kept = 0;
    for (int k = 0; k < n; ++k) if (!indices.count(k)) ++kept;
    if (kept == 0) { status("An event keeps at least one take."); return false; }
    edit("Delete Takes", [&](Project& p) {
        Event* ev = p.findEvent(eventId);
        if (!ev) return;
        const std::vector<Take> all = takesOf(*ev);
        const int active = ev->activeTake;
        std::vector<Take> left;
        int newActive = -1;
        for (int k = 0; k < (int)all.size(); ++k) {
            if (indices.count(k)) continue;
            if (k == active) newActive = (int)left.size();
            left.push_back(all[(size_t)k]);
        }
        if (newActive < 0) newActive = std::min((int)left.size() - 1, std::max(0, active - 1));
        ev->mediaId = left[(size_t)newActive].mediaId;
        ev->offset = left[(size_t)newActive].offset;
        ev->name = left[(size_t)newActive].name;
        left.erase(left.begin() + newActive);
        ev->takes = left;
        ev->activeTake = newActive;
    });
    return true;
}

void AppState::renameActiveTake(uint64_t eventId, const std::string& name)
{
    const Event* e = project_.findEvent(eventId);
    if (!e || e->name == name) return;
    edit("Rename Take", [&](Project& p) {
        if (Event* ev = p.findEvent(eventId)) ev->name = name;
    });
}

void AppState::setAutoCrossfades(bool on)
{
    if (autoCrossfades_ == on) return;
    autoCrossfades_ = on;
    if (dragging_) dragBase_.autoCrossfades = on;
    afterChange();
}

// ================================================================================================ envelopes
void shiftEventEnvelopes(Event& e, double delta)
{
    // points that end up before the event's start are kept: the line goes on through them as before, and they return if
    // the edge is dragged out again
    for (Envelope& env : e.envelopes)
        for (EnvPoint& pt : env.points) pt.time -= delta;
}

void shiftEventKeyframes(Event& e, double delta)
{
    // keys before the new start are kept: the box and mask still move between them and the next as before
    for (PanCropKey& k : e.panCrop) k.time -= delta;
    for (MaskKey& k : e.mask) k.time -= delta;
}

Envelopes* AppState::envelopesOf(Project& p, const EnvelopeOwner& o)
{
    switch (o.kind) {
    case EnvelopeOwner::Track: { Track* t = p.findTrack(o.id); return t ? &t->envelopes : nullptr; }
    case EnvelopeOwner::Event: { Event* e = p.findEvent(o.id); return e ? &e->envelopes : nullptr; }
    case EnvelopeOwner::Bus: { Bus* b = p.findBus(o.id); return b ? &b->envelopes : nullptr; }
    case EnvelopeOwner::Master: return &p.masterEnvelopes;
    case EnvelopeOwner::VideoOut: return &p.videoEnvelopes;
    default: return nullptr;
    }
}

const Envelopes* AppState::envelopesOf(const Project& p, const EnvelopeOwner& o)
{
    return envelopesOf(const_cast<Project&>(p), o);
}

Envelope* AppState::findEnvelope(Project& p, const EnvelopeRef& r)
{
    Envelopes* list = envelopesOf(p, r.owner);
    if (!list) return nullptr;
    for (Envelope& e : *list)
        if (e.kind == r.kind && e.target == r.target && (r.kind != "fx" || e.param == r.param)) return &e;
    return nullptr;
}

const Envelope* AppState::findEnvelope(const Project& p, const EnvelopeRef& r)
{
    return findEnvelope(const_cast<Project&>(p), r);
}

int AppState::envelopeDefaultCurve(const std::string& kind)
{
    if (kind == "transition") return CurveLinear;
    if (kind == "mute") return CurveHold;
    // Preferences > External Control & Automation "New audio envelopes", "New audio FX automation envelopes", "New video
    // envelopes" (VEGAS defaults: Smooth, Linear, Smooth)
    if (kind == "fx") return vprefs::curveCode("prefs/curveAudioFx");
    if (kind == "composite" || kind == "fadecolor" || kind == "velocity" || kind == "motionblur" || kind == "supersample")
        return vprefs::curveCode("prefs/curveVideoEnv");
    return vprefs::curveCode("prefs/curveAudioEnv");
}

double AppState::envelopeDefault(const std::string& kind)
{
    if (kind == "volume" || kind == "send" || kind == "composite" || kind == "velocity" || kind == "supersample" || kind == "duck") return 1.0;
    return 0.0;
}

std::vector<EnvelopeChoice> AppState::envelopeChoices(const EnvelopeOwner& o) const
{
    std::vector<EnvelopeChoice> out;
    auto add = [&](const char* kind, const std::string& name, uint64_t target = 0, const std::string& param = std::string()) {
        EnvelopeChoice c;
        c.ref.owner = o;
        c.ref.kind = kind;
        c.ref.target = target;
        c.ref.param = param;
        c.name = name;
        out.push_back(c);
    };
    auto fxOf = [&](const FxChain& chain) {
        for (const FxSlot& s : chain) {
            if (s.kind != "audio") continue;
            AudioPlugin* pl = fx.audio(s.id);
            if (!pl) continue;
            for (int i = 0, n = std::min(pl->paramCount(), 256); i < n; ++i) {
                const std::string pn = pl->paramName(i);
                add("fx", fxSlotLabel(s) + ": " + (pn.empty() ? strfmt("%d", i) : pn), s.id, pn.empty() ? strfmt("%d", i) : pn);
            }
        }
    };
    const Project& p = project_;
    switch (o.kind) {
    case EnvelopeOwner::Track:
        for (const Track& t : p.tracks)
            if (t.id == o.id) {
                if (t.type == TrackType::Audio) {
                    add("volume", "Volume");
                    add("pan", "Pan");
                    add("mute", "Mute");
                    for (const Bus& b : p.buses) add("send", b.name.empty() ? std::string("Bus") : b.name, b.id);
                    fxOf(t.fx);
                } else {
                    add("composite", "Composite Level");
                    add("fadecolor", "Fade to Color");
                    add("mute", "Mute");
                }
            }
        break;
    case EnvelopeOwner::Event: {
        const Track* t = nullptr;
        if (p.findEvent(o.id, &t) && t) {
            if (t->type == TrackType::Audio) {
                add("volume", "Volume");
                add("pan", "Pan");
            } else {
                add("composite", "Composite Level");
                add("velocity", "Velocity");
                for (const Track& tt : p.tracks)
                    for (const Event& ev : tt.events)
                        if (ev.id == o.id && !ev.transition.kind.empty()) add("transition", "Transition Progress");
            }
        }
        break;
    }
    case EnvelopeOwner::Bus:
        if (const Bus* b = p.findBus(o.id)) {
            add("volume", "Volume");
            add("pan", "Pan");
            add("mute", "Mute");
            fxOf(b->fx);
        }
        break;
    case EnvelopeOwner::Master:
        add("volume", "Volume");
        add("mute", "Mute");
        fxOf(p.audioOut);
        break;
    case EnvelopeOwner::VideoOut:
        add("fadecolor", "Fade to Color");
        add("mute", "Mute");
        add("motionblur", "Motion Blur Amount");
        add("supersample", "Video Supersampling");
        break;
    default: break;
    }
    return out;
}

bool AppState::insertEnvelope(const EnvelopeRef& r)
{
    if (!r.valid()) return false;
    if (const Envelope* e = findEnvelope(project_, r)) {
        if (e->visible) return false;
        showEnvelope(r, true);
        return true;
    }
    if (!envelopesOf(project_, r.owner)) return false;
    double value = envelopeDefault(r.kind);
    if (r.kind == "fx") {
        if (AudioPlugin* pl = fx.audio(r.target))
            for (int i = 0, n = pl->paramCount(); i < n; ++i)
                if (pl->paramName(i) == r.param || strfmt("%d", i) == r.param) { value = pl->paramGet(i); break; }
    }
    edit("Insert Envelope", [&](Project& p) {
        Envelopes* list = envelopesOf(p, r.owner);
        if (!list) return;
        Envelope e;
        e.kind = r.kind;
        e.target = r.target;
        e.param = r.param;
        EnvPoint pt;
        pt.time = 0;
        pt.value = value;
        pt.curve = envelopeDefaultCurve(r.kind);
        e.points.push_back(pt);
        if (r.kind == "transition") {
            // VEGAS's Transition Progress starts as the transition goes by itself: 0% where the overlap starts, 100% where it ends
            const Track* tr = nullptr;
            if (const Event* ev = p.findEvent(r.owner.id, &tr); ev && tr) {
                double end = ev->start;
                for (const Event& o : tr->events)
                    if (o.id != ev->id && o.start < ev->start && o.end() > ev->start) end = std::max(end, std::min(o.end(), ev->end()));
                if (end > ev->start) {
                    EnvPoint last;
                    last.time = end - ev->start;
                    last.value = 1.0;
                    e.points.push_back(last);
                }
            }
        }
        list->push_back(e);
    });
    return true;
}

void AppState::removeEnvelope(const EnvelopeRef& r)
{
    if (!findEnvelope(project_, r)) return;
    edit("Remove Envelope", [&](Project& p) {
        Envelopes* list = envelopesOf(p, r.owner);
        if (!list) return;
        for (size_t k = 0; k < list->size(); ++k) {
            const Envelope& e = (*list)[k];
            if (e.kind == r.kind && e.target == r.target && (r.kind != "fx" || e.param == r.param)) {
                list->erase(list->begin() + (long)k);
                return;
            }
        }
    });
}

void AppState::showEnvelope(const EnvelopeRef& r, bool visible)
{
    const Envelope* e = findEnvelope(project_, r);
    if (!e || e->visible == visible) return;
    edit(visible ? "Show Envelope" : "Hide Envelope", [&](Project& p) {
        if (Envelope* x = findEnvelope(p, r)) x->visible = visible;
    });
}

void AppState::toggleEnvelope(const EnvelopeRef& r)
{
    const Envelope* e = findEnvelope(project_, r);
    if (!e) insertEnvelope(r);
    else showEnvelope(r, !e->visible);
}

// ------------------------------------------------------------------------------------------ VEGAS's Edit menu commands
// (Edit > Undo All, Clear Edit History, Paste Event Attributes, Event Heal, Close Gaps, Switches > Normalize; Insert >
// Time, Empty Event; Tools > Clean Project Media; Duplicate Track; Select Events to End...)
void AppState::undoAll()
{
    if (dragging_) return;
    while (!undo_.empty()) undo();
}

void AppState::clearEditHistory()
{
    if (dragging_) return;
    undo_.clear();
    redo_.clear();
    notify(kFile);
}

const Event* AppState::clipboardEvent() const
{
    return clipboard_.empty() ? nullptr : &clipboard_[0].event;
}

bool AppState::pasteEventAttributes(const std::set<uint64_t>& ids, int attrs)
{
    if (clipboard_.empty() || ids.empty() || !attrs) return false;
    // The first video event and first audio event copied: a copied clip (frame and sound) pastes its frame's attributes
    // onto the selected video events and its sound's onto the audio events, as VEGAS does.
    const Event* srcV = nullptr;
    const Event* srcA = nullptr;
    for (const ClipItem& c : clipboard_) {
        if (c.type == TrackType::Video && !srcV) srcV = &c.event;
        if (c.type != TrackType::Video && !srcA) srcA = &c.event;
    }
    const bool srcVideo = srcV != nullptr && srcA == nullptr;
    bool any = false;
    for (uint64_t id : ids) {
        const Track* t = nullptr;
        if (project_.findEvent(id, &t) && t && ((t->type == TrackType::Video) ? srcV != nullptr : srcA != nullptr)) any = true;
    }
    if (!any) {
        status(srcVideo ? "The copied event is a video event; its attributes can only be pasted onto video events."
                        : "The copied event is an audio event; its attributes can only be pasted onto audio events.");
        return false;
    }
    edit((attrs & AttrAll) == AttrAll ? "Paste Event Attributes" : "Selectively Paste Event Attributes", [&](Project& p) {
        for (uint64_t id : ids) {
            Track* t = nullptr;
            Event* e = p.findEvent(id, &t);
            if (!e || !t) continue;
            const bool isVideo = t->type == TrackType::Video;
            const Event* from = isVideo ? srcV : srcA;
            if (!from || e->id == from->id) continue;
            const Event& src = *from;
            if (attrs & AttrFx) {
                e->fx = src.fx;
                for (auto& s : e->fx) s.id = p.newId();
            }
            if ((attrs & AttrPanCrop) && isVideo) {
                e->panCrop = src.panCrop;
                e->mask = src.mask;
                e->maskOn = src.maskOn;
                e->maskApplyToFx = src.maskApplyToFx;
            }
            if (attrs & AttrEnvelopes) {
                e->envelopes.clear();
                for (const Envelope& env : src.envelopes)
                    if (env.kind != "fx") e->envelopes.push_back(env);
            }
            if (attrs & AttrFadeIn) {
                e->fadeIn = std::min(src.fadeIn, e->length);
                e->fadeInCurve = src.fadeInCurve;
            }
            if (attrs & AttrFadeOut) {
                e->fadeOut = std::min(src.fadeOut, e->length);
                e->fadeOutCurve = src.fadeOutCurve;
            }
            if (attrs & AttrLevel) e->gain = src.gain;
            if (attrs & AttrRate) {
                e->rate = src.rate;
                e->reverse = src.reverse;
            }
            if (attrs & AttrSwitches) {
                e->mute = src.mute;
                e->loop = src.loop;
                e->keepAspect = src.keepAspect;
                e->reduceFlicker = src.reduceFlicker;
                e->resample = src.resample;
                e->invertPhase = src.invertPhase;
                // normalizing is measured on each event's own sound: only the switch is pasted, then measured below
                e->normalize = src.normalize;
            }
            if ((attrs & AttrPitch) && !isVideo) {
                e->pitch = src.pitch;
                e->keepPitch = src.keepPitch;
                e->stretchMethod = src.stretchMethod;
                e->stretchMode = src.stretchMode;
                e->formant = src.formant;
                e->preserveFormants = src.preserveFormants;
                e->channels = src.channels;
            }
        }
    });
    if ((attrs & AttrSwitches) && srcA && srcA->normalize) {
        std::set<uint64_t> norm;
        for (uint64_t id : ids)
            if (const Event* e = project_.findEvent(id))
                if (e->normalize) norm.insert(id);
        if (!norm.empty()) {
            std::map<uint64_t, double> gains;
            for (uint64_t id : norm)
                if (const Event* e = project_.findEvent(id)) {
                    const double peak = eventPeak(*e);
                    gains[id] = peak > 1e-6 ? std::min(1000.0, normalizeLevel() / peak) : 1.0;
                }
            for (auto& kv : gains)
                if (Event* e = project_.findEvent(kv.first)) e->normalizeGain = kv.second;
            afterChange();
        }
    }
    return true;
}

// Events that follow on from 'a' in their media: 'b' starts where 'a' ends and shows what comes next in the same media,
// at the same rate, with the same takes (as Split leaves them).
static bool continues(const Event& a, const Event& b, double fps)
{
    const double tol = 0.25 / std::max(1.0, fps);
    if (a.mediaId != b.mediaId || a.reverse || b.reverse || a.locked || b.locked) return false;
    if (std::fabs(a.rate - b.rate) > 1e-9 || std::fabs(b.start - a.end()) > tol) return false;
    if (std::fabs(b.offset - (a.offset + a.length * a.rate)) > tol * std::max(1.0, a.rate)) return false;
    if (a.takes.size() != b.takes.size() || a.activeTake != b.activeTake) return false;
    for (size_t k = 0; k < a.takes.size(); ++k)
        if (a.takes[k].mediaId != b.takes[k].mediaId || std::fabs(b.takes[k].offset - (a.takes[k].offset + a.length * a.rate)) > tol * std::max(1.0, a.rate))
            return false;
    return true;
}

int AppState::healEvents(const std::set<uint64_t>& ids)
{
    std::set<uint64_t> all = ids, groups;
    for (uint64_t id : ids)
        if (const Event* e = project_.findEvent(id))
            if (e->group) groups.insert(e->group);
    for (const Track& t : project_.tracks)
        for (const Event& e : t.events)
            if (e.group && groups.count(e.group)) all.insert(e.id);
    const double fps = project_.settings.fps();
    int joins = 0;
    {
        for (const Track& t : project_.tracks)
            for (size_t k = 0; k + 1 < t.events.size(); ++k)
                if ((all.count(t.events[k].id) || all.count(t.events[k + 1].id)) && continues(t.events[k], t.events[k + 1], fps)) ++joins;
    }
    if (!joins) {
        status("Event Heal: the selected events do not continue from a neighbouring event.");
        return 0;
    }
    std::set<uint64_t> keep = selection_;
    edit("Event Heal", [&](Project& p) {
        for (Track& t : p.tracks) {
            t.sortEvents();
            for (size_t k = 0; k + 1 < t.events.size();) {
                Event& a = t.events[k];
                const Event& b = t.events[k + 1];
                if ((all.count(a.id) || all.count(b.id)) && continues(a, b, fps)) {
                    // the joined event keeps the first one's effects and settings, the second one's end and fade-out;
                    // the second one's envelopes join after the first one's
                    const double shift = b.start - a.start;
                    for (const Envelope& env : b.envelopes) {
                        Envelope* into = reno::findEnvelope(a.envelopes, env.kind, env.target);
                        if (!into || env.kind == "fx") continue;
                        for (EnvPoint pt : env.points)
                            if (pt.time >= 0) {
                                pt.time += shift;
                                into->points.push_back(pt);
                            }
                        std::stable_sort(into->points.begin(), into->points.end(), [](const EnvPoint& x, const EnvPoint& y) { return x.time < y.time; });
                    }
                    a.length = b.end() - a.start;
                    a.fadeOut = b.fadeOut;
                    a.fadeOutCurve = b.fadeOutCurve;
                    if (keep.count(b.id)) keep.insert(a.id);
                    keep.erase(b.id);
                    all.insert(a.id);
                    t.events.erase(t.events.begin() + (std::ptrdiff_t)k + 1);
                } else {
                    ++k;
                }
            }
        }
    });
    setSelection(keep);
    return joins;
}

int AppState::closeGaps(const std::set<uint64_t>& ids)
{
    if (ids.empty()) return 0;
    // how far each chosen event moves back: to the end of the event before it on its track (already moved, if it moved);
    // events grouped with it go the same way
    Project p = project_;
    std::map<uint64_t, double> moveBy;
    for (Track& t : p.tracks) {
        t.sortEvents();
        double prevEnd = -1;
        for (Event& e : t.events) {
            if (ids.count(e.id) && !e.locked && prevEnd >= 0 && e.start > prevEnd + 1e-9) {
                moveBy[e.id] = e.start - prevEnd;
                e.start = prevEnd;
            }
            prevEnd = std::max(prevEnd, e.end());
        }
    }
    if (moveBy.empty()) {
        status("Close Gaps: there are no gaps before the selected events.");
        return 0;
    }
    std::map<uint64_t, double> groupBy;
    for (const Track& t : project_.tracks)
        for (const Event& e : t.events)
            if (moveBy.count(e.id) && e.group) groupBy.emplace(e.group, moveBy[e.id]);
    int moved = 0;
    edit("Close Gaps", [&](Project& q) {
        for (Track& t : q.tracks) {
            for (Event& e : t.events) {
                double d = 0;
                if (moveBy.count(e.id)) d = moveBy[e.id];
                else if (e.group && groupBy.count(e.group) && !ids.count(e.id) && !e.locked) d = groupBy[e.group];
                if (d > 0) {
                    e.start = std::max(0.0, e.start - d);
                    ++moved;
                }
            }
            t.sortEvents();
        }
    });
    return moved;
}

void AppState::insertTime(double at, double length)
{
    if (!(length > 0) || !(at >= 0)) return;
    edit("Insert Time", [&](Project& p) {
        for (Track& t : p.tracks) {
            std::vector<Event> added;
            for (Event& e : t.events) {
                if (e.start >= at - 1e-9) {
                    e.start += length;
                } else if (e.end() > at + 1e-9) {
                    // across the insertion point: split there, second half after the new time
                    Event right = e;
                    right.id = p.newId();
                    right.start = at + length;
                    right.length = e.end() - at;
                    right.offset = e.offset + (at - e.start) * e.rate;
                    for (Take& k : right.takes) k.offset += (at - e.start) * e.rate;
                    shiftEventEnvelopes(right, at - e.start);
                    shiftEventKeyframes(right, at - e.start);
                    right.fadeIn = 0;
                    right.transition = FxSlot();
                    for (auto& s : right.fx) s.id = p.newId();
                    right.group = 0;
                    e.length = at - e.start;
                    e.fadeOut = 0;
                    added.push_back(right);
                }
            }
            for (Event& e : added) t.events.push_back(e);
            t.sortEvents();
            for (Envelope& env : t.envelopes)
                for (EnvPoint& pt : env.points) if (pt.time >= at) pt.time += length;
            for (MotionKey& k : t.motion) if (k.time >= at) k.time += length;
            for (MotionKey& k : t.parentMotion) if (k.time >= at) k.time += length;
        }
        for (Bus& b : p.buses)
            for (Envelope& env : b.envelopes)
                for (EnvPoint& pt : env.points) if (pt.time >= at) pt.time += length;
        for (Envelopes* list : {&p.masterEnvelopes, &p.videoEnvelopes})
            for (Envelope& env : *list)
                for (EnvPoint& pt : env.points) if (pt.time >= at) pt.time += length;
        for (Project::Marker& m : p.markers) {
            if (m.time >= at) m.time += length;
            else if (m.region && m.time + m.length > at) m.length += length;
        }
    });
}

uint64_t AppState::insertEmptyEvent(uint64_t trackId, double start, double length, bool adjustment)
{
    if (!(length > 0) || !project_.findTrack(trackId)) return 0;
    if (adjustment && project_.findTrack(trackId)->type != TrackType::Video) return 0;
    uint64_t made = 0;
    edit(adjustment ? "Insert Adjustment Event" : "Insert Empty Event", [&](Project& p) {
        Track* t = p.findTrack(trackId);
        if (!t) return;
        Event e;
        e.id = made = p.newId();
        e.mediaId = 0;
        e.start = std::max(0.0, start);
        e.length = length;
        e.name = adjustment ? "Adjustment Event" : "Empty Event";
        e.adjustment = adjustment;
        setDefaultFadeCurves(e, t->type);
        t->events.push_back(e);
        t->sortEvents();
    });
    if (made) selectOnly(made);
    return made;
}

int AppState::cleanProjectMedia()
{
    std::set<uint64_t> used;
    for (const Track& t : project_.tracks)
        for (const Event& e : t.events) {
            used.insert(e.mediaId);
            for (const Take& k : e.takes) used.insert(k.mediaId);
        }
    int n = 0;
    for (const MediaItem& m : project_.media) if (!used.count(m.id)) ++n;
    if (!n) return 0;
    edit("Clean Project Media", [&](Project& p) {
        p.media.erase(std::remove_if(p.media.begin(), p.media.end(), [&](const MediaItem& m) { return !used.count(m.id); }), p.media.end());
    });
    return n;
}

uint64_t AppState::duplicateTrack(uint64_t trackId)
{
    if (!project_.findTrack(trackId)) return 0;
    fx.captureStates(project_);
    uint64_t made = 0;
    edit("Duplicate Track", [&](Project& p) {
        for (size_t k = 0; k < p.tracks.size(); ++k) {
            if (p.tracks[k].id != trackId) continue;
            Track t = p.tracks[k];
            t.id = made = p.newId();
            t.veg.reset();
            t.trackGroup = p.tracks[k].trackGroup;
            std::map<uint64_t, uint64_t> slotMap;
            for (auto& s : t.fx) { const uint64_t n = p.newId(); slotMap[s.id] = n; s.id = n; s.veg.reset(); }
            std::map<uint64_t, uint64_t> groups;
            for (Event& e : t.events) {
                e.id = p.newId();
                e.veg.reset();
                for (auto& s : e.fx) { const uint64_t n = p.newId(); slotMap[s.id] = n; s.id = n; s.veg.reset(); }
                if (!e.transition.kind.empty()) e.transition.id = p.newId();
                // the copies are not grouped with the original's partners; copies of one group stay one group
                if (e.group) {
                    auto it = groups.find(e.group);
                    if (it == groups.end()) it = groups.emplace(e.group, p.newId()).first;
                    e.group = it->second;
                }
                for (Envelope& env : e.envelopes) { env.veg.reset(); if (env.kind == "fx" && slotMap.count(env.target)) env.target = slotMap[env.target]; }
            }
            for (Envelope& env : t.envelopes) { env.veg.reset(); if (env.kind == "fx" && slotMap.count(env.target)) env.target = slotMap[env.target]; }
            if (!p.tracks[k].name.empty()) t.name = p.tracks[k].name;
            p.tracks.insert(p.tracks.begin() + (std::ptrdiff_t)k + 1, t);
            return;
        }
    });
    return made;
}

std::set<uint64_t> AppState::eventsToEnd(const std::set<uint64_t>& ids) const
{
    std::set<uint64_t> out;
    std::map<uint64_t, double> from;
    for (const Track& t : project_.tracks)
        for (const Event& e : t.events)
            if (ids.count(e.id)) {
                auto it = from.find(t.id);
                if (it == from.end() || e.start < it->second) from[t.id] = e.start;
            }
    for (const Track& t : project_.tracks) {
        auto it = from.find(t.id);
        if (it == from.end()) continue;
        for (const Event& e : t.events)
            if (e.start >= it->second - 1e-9) out.insert(e.id);
    }
    return out;
}

std::set<uint64_t> AppState::eventsAfter(double time, uint64_t onlyTrack) const
{
    std::set<uint64_t> out;
    for (const Track& t : project_.tracks)
        if (!onlyTrack || t.id == onlyTrack)
            for (const Event& e : t.events)
                if (e.end() > time + 1e-9) out.insert(e.id);
    return out;
}

double AppState::normalizeLevel()
{
    // Preferences > Audio > Normalize peak level (dB), VEGAS's -0.1 dB unless changed (in QSettings, read here so the
    // Preferences struct need not change)
    double db = QSettings().value("prefs/normalizePeakDb", -0.1).toDouble();
    if (!(db >= -60.0 && db <= 0.0)) db = -0.1;
    return std::pow(10.0, db / 20.0);
}

double AppState::eventPeak(const Event& e) const
{
    const MediaItem* m = project_.findMedia(e.mediaId);
    if (!m || m->generated || m->path.empty() || !m->info.hasAudio) return 0;
    std::string err;
    std::unique_ptr<MediaReader> r = mediaOpener ? mediaOpener(m->path, &err) : openMediaOrProject(m->path, &err);
    if (!r) return 0;
    const int rate = 48000, chunk = 8192;
    double from = e.offset, to = e.offset + e.length * e.rate;
    if (m->info.duration > 0) to = std::min(to, std::max(from, m->info.duration));
    int64_t pos = (int64_t)std::floor(std::max(0.0, from) * rate);
    const int64_t end = std::min<int64_t>((int64_t)std::ceil(to * rate), pos + (int64_t)rate * 60 * 60 * 3);
    std::vector<float> l(chunk), rr(chunk);
    double peak = 0;
    while (pos < end) {
        const int n = (int)std::min<int64_t>(chunk, end - pos);
        if (!r->audioPick(pos, n, rate, e.mediaChannel, e.mediaChannels, l.data(), rr.data())) break;
        for (int k = 0; k < n; ++k) peak = std::max(peak, (double)std::max(std::fabs(l[(size_t)k]), std::fabs(rr[(size_t)k])));
        pos += n;
    }
    return std::isfinite(peak) ? peak : 0;
}

void AppState::setNormalize(const std::set<uint64_t>& ids, bool on)
{
    std::map<uint64_t, double> gains;
    for (uint64_t id : ids) {
        const Track* t = nullptr;
        const Event* e = project_.findEvent(id, &t);
        if (!e || !t || t->type != TrackType::Audio) continue;
        if (!on) { gains[id] = 1.0; continue; }
        const double peak = eventPeak(*e);
        gains[id] = peak > 1e-6 ? std::min(1000.0, normalizeLevel() / peak) : 1.0;
    }
    if (gains.empty()) {
        status("Normalize works on audio events: select one first.");
        return;
    }
    edit(on ? "Normalize" : "Normalize Off", [&](Project& p) {
        for (auto& kv : gains)
            if (Event* e = p.findEvent(kv.first)) {
                e->normalize = on;
                if (!on) e->autoNormalize = false;
                e->normalizeGain = kv.second;
            }
    });
}

std::string timecode(double seconds, double fps)
{
    if (!(seconds >= 0)) seconds = 0;
    if (!(fps > 0)) fps = 30;
    const long long frames = (long long)std::floor(seconds * fps + 1e-6);
    const int nominal = std::max(1, (int)std::lround(fps));
    const long long f = frames % nominal, s = frames / nominal;
    return strfmt("%02lld:%02lld:%02lld;%0*lld", s / 3600, (s / 60) % 60, s % 60, frameDigits(nominal), f);
}

}
}
