// VEGAS Pro 18's menus in the main window: the commands Reno lacked, made here, and the menus put in VEGAS's order with VEGAS's names
// (MainWindow::buildVegasMenus, run once the other menus exist). Each menu starts with the commands VEGAS names, in its
// order (separators where
// VEGAS has them); anything else (Reno's own commands, later additions) follows after a separator in its old order.
// Commands Reno cannot do
// are greyed, saying why.
#include "audioproxyui.h"
#include "mainwindow.h"
#include "pluginchooser.h"
#include "smalldialogs.h"
#include "subtitles.h"
#include "mainwindow_tools.h"
#include "timeline.h"
#include "trimmer.h"
#include "redeye.h"
#include "scripting.h"
#include "audiotools.h"
#include "mixer.h"
#include "../engine/catalog.h"
#include "../engine/timefmt.h"
#include "menuutil.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QInputDialog>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QToolBar>
#include <QVBoxLayout>
#include <cmath>

namespace reno {
namespace app {

namespace {

// Puts the menu's items in VEGAS's order. Each name is an action's object name, "-" a separator, or ">Title" a submenu
// (by its current title). Names not found are skipped; unnamed items go after the rest behind a separator in their old
// order.
void arrange(QMenu* menu, const std::vector<std::string>& order)
{
    if (!menu) return;
    QList<QAction*> before = menu->actions();
    QList<QAction*> now;
    std::set<QAction*> used;
    for (const std::string& n : order) {
        if (n == "-") {
            if (!now.isEmpty() && !now.last()->isSeparator()) {
                QAction* s = new QAction(menu);
                s->setSeparator(true);
                now << s;
            }
            continue;
        }
        QAction* found = nullptr;
        for (QAction* a : before) {
            if (used.count(a) || a->isSeparator()) continue;
            if (n[0] == '>' ? (a->menu() && plainMenuText(a->text()) == QString::fromStdString(n.substr(1))) : a->objectName() == QString::fromStdString(n)) {
                found = a;
                break;
            }
        }
        if (!found) continue;
        used.insert(found);
        now << found;
    }
    QList<QAction*> rest;
    for (QAction* a : before) {
        if (used.count(a)) continue;
        if (a->isSeparator()) {
            if (!rest.isEmpty() && !rest.last()->isSeparator()) rest << a;
            continue;
        }
        rest << a;
    }
    while (!rest.isEmpty() && rest.last()->isSeparator()) rest.removeLast();
    for (QAction* a : before) menu->removeAction(a);
    while (!now.isEmpty() && now.last()->isSeparator()) now.removeLast();
    for (QAction* a : now) menu->addAction(a);
    if (!rest.isEmpty()) {
        if (!now.isEmpty()) menu->addSeparator();
        for (QAction* a : rest) menu->addAction(a);
    }
}

// A time typed as hh:mm:ss;ff (or :ff), hh:mm:ss.fraction, or seconds. -1 if unreadable.
double readTime(const QString& text, double fps)
{
    QString t = text.trimmed();
    if (t.isEmpty()) return -1;
    bool ok = false;
    const double secs = t.toDouble(&ok);
    if (ok) return secs >= 0 ? secs : -1;
    t.replace(';', ':');
    const QStringList parts = t.split(':');
    if (parts.size() < 2 || parts.size() > 4) return -1;
    double total = 0;
    int frames = 0;
    QStringList hms = parts;
    if (parts.size() == 4) {
        frames = parts[3].toInt(&ok);
        if (!ok) return -1;
        hms.removeLast();
    }
    for (const QString& p : hms) {
        const double v = p.toDouble(&ok);
        if (!ok || v < 0) return -1;
        total = total * 60.0 + v;
    }
    return total + (fps > 0 ? frames / fps : 0.0);
}

}

QAction* MainWindow::vegasAction(const char* name) const
{
    for (QAction* a : findChildren<QAction*>(QString::fromUtf8(name), Qt::FindDirectChildrenOnly)) return a;
    return nullptr;
}

void MainWindow::buildVegasMenus()
{
    AppState& app = AppState::get();
    TimelineWidget* tl = timeline_;
    auto act = [this](const char* name, const QString& text, const QList<QKeySequence>& k, std::function<void()> fn) {
        QAction* x = new QAction(text, this);
        x->setObjectName(name);
        if (!k.isEmpty()) x->setShortcuts(k);
        QObject::connect(x, &QAction::triggered, this, [fn] { fn(); });
        addAction(x);
        return x;
    };
    auto rename = [this](const char* name, const QString& text) {
        if (QAction* x = vegasAction(name)) {
            const QStringList parts = x->text().split('\t');
            x->setText(parts.size() > 1 ? text + "\t" + parts[1] : text);
        }
    };
    auto unavailable = [this](const char* name, const QString& text, const QString& why) {
        QAction* x = new QAction(text, this);
        x->setObjectName(name);
        x->setEnabled(false);
        x->setData(why);
        x->setToolTip(plainMenuText(text) + ": " + why);
        x->setStatusTip(why);
        x->setWhatsThis(plainMenuText(text) + ": " + why);
        x->setProperty("vegasUnavailable", true);
        return x;
    };
    // a key sent to the timeline as if pressed there (VEGAS's Navigate commands are the timeline's keys)
    auto timelineKey = [tl](int key, Qt::KeyboardModifiers mods) {
        QKeyEvent press(QEvent::KeyPress, key, mods);
        QApplication::sendEvent(tl, &press);
    };
    auto selected = [&app] { return app.selection(); };

    // ------------------------------------------------------------------ File
    rename("newPlain", "New Empty Pro&ject");
    rename("saveVersion", "&Incremental Save");
    act("addMedia", "&Add Media...", QList<QKeySequence>(), [this] { if (QAction* x = vegasAction("importMedia")) x->trigger(); });

    // ------------------------------------------------------------------ Edit
    rename("trim", "Tri&m");
    rename("trimStart", "Trim &Start");
    rename("trimEnd", "Trim &End");
    rename("split", "Spl&it");
    rename("smartSplit", "Smart Spl&it");
    rename("pasteRepeat", "P&aste Repeat...");
    rename("pasteInsert", "Pa&ste Insert");
    act("pasteAttributes", "Paste E&vent Attributes", QList<QKeySequence>(), [&app, selected] {
        if (!app.hasClipboard()) { app.status("Copy an event first. Paste Event Attributes applies its effects, pan/crop, envelopes, fades and switches to the selected events."); return; }
        if (selected().empty()) { app.status("Select the events to paste the attributes onto."); return; }
        app.pasteEventAttributes(selected(), AppState::AttrAll);
    });
    act("selectivePaste", "Selectively Paste Event Attributes...", QList<QKeySequence>(), [this] { selectivePasteDialog(); });
    act("eventHeal", "Event Heal", QList<QKeySequence>(), [&app, selected] {
        if (selected().empty()) { app.status("Select a split event first; Event Heal rejoins it with the piece it was split from."); return; }
        const int n = app.healEvents(selected());
        if (n) app.status(strfmt("Event Heal: %d join%s.", n, n == 1 ? "" : "s"));
    });
    act("closeGaps", "Close Gaps", QList<QKeySequence>(), [&app, selected] {
        if (selected().empty()) { app.status("Select the events to close the gaps before."); return; }
        app.closeGaps(selected());
    });
    act("undoAll", "U&ndo All", QList<QKeySequence>(), [&app] { app.undoAll(); });
    act("clearHistory", "Clear Edit Histor&y...", QList<QKeySequence>(), [this, &app] {
        if (QMessageBox::question(this, "Clear Edit History", "Clear the edit history? Nothing done so far can be undone afterwards.",
                                  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
            app.clearEditHistory();
    });
    rename("goTo", "&Go to Time...");
    rename("goStart", "Go to Project &Start");
    rename("goEnd", "Go to Project &End");
    act("goSelectionStart", "Go to &Time Selection/View Start\tHome", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_Home, Qt::NoModifier); });
    act("goSelectionEnd", "Go to Time Selection/&View End\tEnd", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_End, Qt::NoModifier); });
    act("gridLeft", "M&ove Left by Grid Marks\tPgUp", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_PageUp, Qt::NoModifier); });
    act("gridRight", "Move Right by Grid &Marks\tPgDown", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_PageDown, Qt::NoModifier); });
    act("pixelLeft", "Move Left by &Pixel\tLeft", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_Left, Qt::NoModifier); });
    act("pixelRight", "Move Right by P&ixel\tRight", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_Right, Qt::NoModifier); });
    rename("prevFrame", "Move Left by &Frame");
    rename("nextFrame", "Move Right by F&rame");
    rename("prevMarker", "Move Left by M&arker/Region");
    rename("nextMarker", "Move Rig&ht by Marker/Region");
    act("timeSelectRightMarker", "Time Sele&ct Right by Marker/Region", QList<QKeySequence>(), [tl, &app] {
        const double from = app.loopEnd > app.loopStart && std::fabs(app.cursor() - app.loopEnd) < 1e-9 ? app.loopStart : app.cursor();
        tl->cursorToMarker(1);
        if (app.cursor() > from) app.setLoop(from, app.cursor());
    });
    rename("prevEditPoint", "Move &Left by Event Edit Points");
    rename("nextEditPoint", "Move Right by Event E&dit Points");
    rename("prevSelectedEdge", "Move Left &by Selected Event");
    rename("nextSelectedEdge", "Move Right b&y Selected Event");
    rename("cursorToPlayback", "Move Cursor to Play&head");
    rename("centerCursor", "Center View aro&und Cursor");
    act("selectAfterCursor", "Select All After Cursor", QList<QKeySequence>(), [tl] { tl->selectAllAfterCursor(); });
    act("selectEventsToEnd", "Select Events to E&nd", QList<QKeySequence>(), [tl, &app] {
        if (app.selection().empty()) { app.status("Select an event first; Select Events to End selects everything after it on its track."); return; }
        tl->selectEventsToEnd();
    });
    rename("prevEdge", "Select Event Sta&rt");
    act("zoomSelection", "&Zoom Selection", QList<QKeySequence>(), [tl, &app] {
        if (app.loopEnd > app.loopStart) tl->zoomLarge(true);
        else app.status("Make a time selection first: Zoom Selection fits it to the view.");
    });
    act("selectionToView", "Set Selection to &View", QList<QKeySequence>(), [tl] { tl->setSelectionToView(); });
    act("selectionToProject", "Set Selection to &Project", QList<QKeySequence>(), [tl] { tl->setSelectionToProject(); });
    act("selectionStart", "Set Selection S&tart\tI", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_I, Qt::NoModifier); });
    act("selectionEndHere", "Set Selection E&nd\tO", QList<QKeySequence>(), [timelineKey] { timelineKey(Qt::Key_O, Qt::NoModifier); });
    rename("timeSelectPrev", "Set Selection to Prev&ious Selected Event");
    rename("timeSelectNext", "Set Selection to Ne&xt Selected Event");
    rename("restoreSelection", "Rest&ore Previous Time Selection");
    rename("toolNormal", "&Normal Edit Tool");
    QAction* shuffle = vegasAction("toolShuffle");
    QAction* slip = vegasAction("toolSlip");
    QAction* slide = vegasAction("toolSlide");
    QAction* stretch = vegasAction("toolStretch");
    QAction* splitTrim = vegasAction("toolSplitTrim");
    auto eventSwitch = [&](const char* name, const QString& text, const std::string& what, bool video, std::function<bool(const Event&)> get, std::function<void(Event&, bool)> set) {
        QAction* x = act(name, text, QList<QKeySequence>(), [&app, what, video, get, set] {
            std::set<uint64_t> ids;
            bool all = true;
            for (uint64_t id : app.selection()) {
                const Track* t = nullptr;
                const Event* e = app.project().findEvent(id, &t);
                if (!e || !t || (t->type == TrackType::Video) != video) continue;
                ids.insert(id);
                all = all && get(*e);
            }
            if (ids.empty()) { app.status(video ? "Select a video event first." : "Select an audio event first."); return; }
            app.edit(what, [&](Project& p) { for (uint64_t id : ids) if (Event* e = p.findEvent(id)) set(*e, !all); });
        });
        x->setCheckable(true);
        return x;
    };
    eventSwitch("switchInvertPhase", "&Invert Phase", "Invert Phase", false, [](const Event& e) { return e.invertPhase; }, [](Event& e, bool v) { e.invertPhase = v; });
    QAction* norm = act("switchNormalize", "&Normalize", QList<QKeySequence>(), [&app] {
        std::set<uint64_t> ids;
        bool all = true;
        for (uint64_t id : app.selection()) {
            const Track* t = nullptr;
            const Event* e = app.project().findEvent(id, &t);
            if (!e || !t || t->type != TrackType::Audio) continue;
            ids.insert(id);
            all = all && e->normalize;
        }
        app.setNormalize(ids, !all);
    });
    norm->setCheckable(true);
    // VEGAS 22's Auto Normalize: Normalize re-worked when the event is trimmed or split
    QAction* autoNorm = act("switchAutoNormalize", "&Auto Normalize", QList<QKeySequence>(), [&app] {
        std::set<uint64_t> ids;
        bool all = true;
        for (uint64_t id : app.selection()) {
            const Track* t = nullptr;
            const Event* e = app.project().findEvent(id, &t);
            if (!e || !t || t->type != TrackType::Audio) continue;
            ids.insert(id);
            all = all && e->autoNormalize;
        }
        app.setAutoNormalize(ids, !all);
    });
    autoNorm->setCheckable(true);
    eventSwitch("switchAspect", "Maintain &Aspect Ratio", "Maintain Aspect Ratio", true, [](const Event& e) { return e.keepAspect; }, [](Event& e, bool v) { e.keepAspect = v; });
    eventSwitch("switchFlicker", "Reduce Interlace &Flicker", "Reduce Interlace Flicker", true, [](const Event& e) { return e.reduceFlicker; }, [](Event& e, bool v) { e.reduceFlicker = v; });
    QActionGroup* resampleGroup = new QActionGroup(this);
    auto resampleSwitch = [&](const char* name, const QString& text, const std::string& mode) {
        QAction* x = act(name, text, QList<QKeySequence>(), [&app, mode] {
            std::set<uint64_t> ids;
            for (uint64_t id : app.selection()) {
                const Track* t = nullptr;
                if (app.project().findEvent(id, &t) && t && t->type == TrackType::Video) ids.insert(id);
            }
            if (ids.empty()) { app.status("Select a video event first."); return; }
            app.edit(mode == "force" ? "Force Resample" : mode == "off" ? "Disable Resample" : mode == "optical" ? "Optical Flow Resample" : "Use Project Resample Mode", [&](Project& p) {
                for (uint64_t id : ids) if (Event* e = p.findEvent(id)) e->resample = mode;
            });
        });
        x->setCheckable(true);
        x->setActionGroup(resampleGroup);
        return x;
    };
    QAction* resProject = resampleSwitch("switchResampleProject", "Use P&roject Resample Mode", "project");
    QAction* resForce = resampleSwitch("switchResampleForce", "Force Re&sample", "force");
    QAction* resOptical = resampleSwitch("switchResampleOptical", "&Optical Flow", "optical");
    QAction* resOff = resampleSwitch("switchResampleOff", "&Disable Resample", "off");
    resampleGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    rename("ungroup", "&Remove From");
    rename("clearGroup", "&Clear");
    act("groupCutAll", "Cut &All", QList<QKeySequence>(), [tl] { tl->selectGroups(); tl->cutSelection(); });
    act("groupCopyAll", "Cop&y All", QList<QKeySequence>(), [this, tl] {
        tl->selectGroups();
        if (QAction* c = vegasAction("copy")) c->trigger();
    });
    act("groupDeleteAll", "&Delete All", QList<QKeySequence>(), [tl] { tl->selectGroups(); tl->deleteSelection(); });
    QActionGroup* channels = new QActionGroup(this);
    const struct { const char* name; const char* text; const char* id; } chans[] = {
        {"channelsBoth", "&Both", "both"}, {"channelsLeft", "&Left Only", "left"}, {"channelsRight", "&Right Only", "right"},
        {"channelsCombine", "&Combine", "combine"}, {"channelsSwap", "&Swap", "swap"}};
    for (const auto& c : chans) {
        const std::string v = c.id;
        QAction* x = act(c.name, c.text, QList<QKeySequence>(), [&app, v] {
            std::set<uint64_t> ids;
            for (uint64_t id : app.selection()) {
                const Track* t = nullptr;
                if (app.project().findEvent(id, &t) && t && t->type == TrackType::Audio) ids.insert(id);
            }
            if (ids.empty()) { app.status("Select an audio event first."); return; }
            app.edit("Channels", [&](Project& p) { for (uint64_t id : ids) if (Event* e = p.findEvent(id)) e->channels = v; });
        });
        x->setCheckable(true);
        x->setActionGroup(channels);
    }
    channels->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    QAction* stream = unavailable("stream1", "Stream &1", "Reno uses the first video stream and the first audio stream of a file");
    stream->setCheckable(true);
    stream->setChecked(true);

    // ------------------------------------------------------------------ View
    rename("focusTimeline", "&Focus to Timeline");
    rename("showBusTracks", "Bus Tra&cks");
    rename("showMediaMarkers", "&Show Markers");
    rename("showTakeInfo", "Active Take &Information");
    rename("showWaveforms", "Audio Event &Waveforms");
    rename("showEventButtons", "Event &Buttons");
    rename("showFadeLengths", "Event Fade &Lengths");
    rename("edgeFrames", "Video &Event Edge Edit Frames");
    rename("eventHeaders", "E&vent Headers");
    rename("showEnvelopes", "S&how Envelopes");
    QAction* markerLabels = act("showMarkerLabels", "Show Marker &Labels", QList<QKeySequence>(), [tl] {
        tl->options().showMarkerLabels = !tl->options().showMarkerLabels;
        tl->optionsChanged();
    });
    markerLabels->setCheckable(true);
    QAction* eventLength = act("showEventLength", "Event &Length", QList<QKeySequence>(), [tl] {
        tl->options().showEventLength = !tl->options().showEventLength;
        tl->optionsChanged();
    });
    eventLength->setCheckable(true);
    act("minimizeAllTracks", "Minimi&ze All Tracks", QList<QKeySequence>() << QKeySequence(QString("`")), [tl] { tl->minimizeAllTracks(); });
    // View > Audio / Video Envelopes: whether envelopes of that kind are shown, on every track (or event, or the video
    // bus track) that has one. Ticked: all shown.
    struct EnvKind { const char* name; const char* text; const char* kind; int where; };
    const EnvKind envKinds[] = {
        {"viewEnvAudioMute", "Track &Mute", "mute", 0}, {"viewEnvAudioVolume", "Track &Volume", "volume", 0}, {"viewEnvAudioPan", "Track &Pan", "pan", 0},
        {"viewEnvVideoMute", "Track &Mute", "mute", 1}, {"viewEnvVideoComposite", "Track &Composite Level", "composite", 1},
        {"viewEnvVideoFade", "Track &Fade to Color", "fadecolor", 1}, {"viewEnvVelocity", "Event &Velocity", "velocity", 2},
        {"viewEnvMotionBlur", "Motion &Blur Amount", "motionblur", 3}, {"viewEnvSupersample", "Video &Supersampling", "supersample", 3}};
    auto envLists = [](Project& p, int where) {
        std::vector<Envelopes*> out;
        if (where == 3) out.push_back(&p.videoEnvelopes);
        for (Track& t : p.tracks) {
            if ((t.type == TrackType::Video) != (where != 0)) continue;
            if (where <= 1) out.push_back(&t.envelopes);
            else if (where == 2) for (Event& e : t.events) out.push_back(&e.envelopes);
        }
        return out;
    };
    for (const EnvKind& k : envKinds) {
        const std::string kind = k.kind;
        const int where = k.where;
        QAction* x = act(k.name, k.text, QList<QKeySequence>(), [&app, kind, where, envLists] {
            Project copy = app.project();
            bool any = false, all = true;
            for (Envelopes* list : envLists(copy, where))
                for (Envelope& e : *list)
                    if (e.kind == kind) { any = true; all = all && e.visible; }
            if (!any) { app.status("No envelope of that kind yet. Use Insert > Audio Envelopes / Video Envelopes to add one."); return; }
            app.edit(all ? "Hide Envelopes" : "Show Envelopes", [&](Project& p) {
                for (Envelopes* list : envLists(p, where))
                    for (Envelope& e : *list)
                        if (e.kind == kind) e.visible = !all;
            });
        });
        x->setCheckable(true);
    }

    // ------------------------------------------------------------------ Insert
    act("insertEmptyEvent", "&Empty Event", QList<QKeySequence>(), [tl] { tl->insertEmptyEvent(); });
    act("insertAdjustmentEvent", "Ad&justment Event", QList<QKeySequence>(), [tl] { tl->insertAdjustmentEvent(); });
    act("insertText", "Te&xt...", QList<QKeySequence>(), [this, &app] {
        // VEGAS's Titles & Text generator (else any generator called Text), at the cursor on the selected video track
        std::string id, label;
        for (const VideoFxEntry& v : Catalog::get().video()) {
            if (!v.generator || !v.ok) continue;
            const QString l = QString::fromStdString(v.label);
            if (l.contains("Titles & Text", Qt::CaseInsensitive)) { id = v.id; label = v.label; break; }
            if (id.empty() && l.contains("Text", Qt::CaseInsensitive)) { id = v.id; label = v.label; }
        }
        if (id.empty()) { app.status("Insert Text needs the Titles & Text generator (or another text generator), but none was found."); return; }
        browserActivated(FxBrowser::Generators, id + "\n" + label);
    });
    // Insert > Insert Subtitles From File... / From Regions... (VEGAS Pro 23; subtitles.cpp)
    act("insertSubtitlesFile", "Insert &Subtitles From File...", QList<QKeySequence>(), [this] { runInsertSubtitlesFromFile(this); });
    act("insertSubtitlesRegions", "Insert Subtitles From &Regions...", QList<QKeySequence>(), [this] { runInsertSubtitlesFromRegions(this); });
    // (Insert > Generated Media is the "generatedMedia" action: the Plug-In Chooser for a generator)
    // Insert > Slideshow...: VEGAS's Slideshow Creator (smalldialogs.cpp)
    act("insertSlideshow", "&Slideshow...", QList<QKeySequence>(), [this] { runSlideshowCreator(this); });
    act("insertTime", "&Time...", QList<QKeySequence>(), [this, &app] {
        const double fps = app.project().settings.fps();
        const double len = app.loopEnd > app.loopStart ? app.loopEnd - app.loopStart : 1.0;
        bool ok = false;
        const QString text = QInputDialog::getText(this, "Insert Time", "Amount of time to insert at the cursor:", QLineEdit::Normal,
                                                   QString::fromStdString(timecode(len, fps)), &ok);
        if (!ok) return;
        const double t = readTime(text, fps);
        if (!(t > 0)) { app.status("Could not read that length. Type it as 00:00:01;00 or as seconds."); return; }
        app.insertTime(app.project().quantize(app.cursor()), t);
    });

    // ------------------------------------------------------------------ Tools
    rename("eventPanCrop", "Video Event &Pan/Crop...");
    act("audioEventFx", "Audio &Event FX...", QList<QKeySequence>(), [this, &app] {
        for (uint64_t id : app.selection()) {
            const Track* t = nullptr;
            if (app.project().findEvent(id, &t) && t && t->type == TrackType::Audio) {
                FxTarget ft;
                ft.kind = FxTarget::Event;
                ft.id = id;
                openFx(ft, 0);
                return;
            }
        }
        app.status("Select an audio event first.");
    });
    act("videoEventFx", "Video Event &FX...", QList<QKeySequence>(), [this, &app] {
        for (uint64_t id : app.selection()) {
            const Track* t = nullptr;
            if (app.project().findEvent(id, &t) && t && t->type == TrackType::Video) {
                FxTarget ft;
                ft.kind = FxTarget::Event;
                ft.id = id;
                openFx(ft, 0);
                return;
            }
        }
        app.status("Select a video event first.");
    });
    // VEGAS's Media FX: effects that belong to the media, so every event of that media has them. Reno keeps none on the
    // media itself: they are the effects applied before Event Pan/Crop (as the .veg reader puts a media's own effects),
    // put on every event that plays the same media.
    act("mediaFx", "Media F&X...", QList<QKeySequence>(), [this, &app] {
        for (uint64_t id : app.selection()) {
            const Track* t = nullptr;
            const Event* ev = app.project().findEvent(id, &t);
            if (!ev || !t || t->type != TrackType::Video || !ev->mediaId) continue;
            FxChain now;
            for (const FxSlot& s : ev->fx)
                if (s.beforePanCrop && s.kind == "ofx") {
                    FxSlot c = s;
                    const Json live = app.fxLiveState(s.id);
                    if (!live.isNull()) c.state = live;
                    now.push_back(c);
                }
            const MediaItem* m = app.project().findMedia(ev->mediaId);
            PluginChooser dlg(ChooserKind::VideoFx, QString("Plug-In Chooser - Media FX") + (m ? QString(": ") + QString::fromStdString(m->name) : QString()), now, this);
            if (dlg.exec() != QDialog::Accepted) return;
            const FxChain chosen = dlg.chain();
            const uint64_t mediaId = ev->mediaId;
            app.edit("Media FX", [&](Project& p) {
                for (Track& tr : p.tracks)
                    for (Event& e : tr.events) {
                        if (e.mediaId != mediaId || tr.type != TrackType::Video) continue;
                        FxChain rest;
                        for (const FxSlot& s : e.fx)
                            if (!(s.beforePanCrop && s.kind == "ofx")) rest.push_back(s);
                        FxChain made;
                        for (FxSlot s : chosen) {
                            bool mine = false;
                            for (const FxSlot& o : e.fx) if (s.id && o.id == s.id) mine = true;
                            if (!mine) { s.id = p.newId(); s.veg.reset(); }
                            s.beforePanCrop = true;
                            made.push_back(s);
                        }
                        made.insert(made.end(), rest.begin(), rest.end());
                        e.fx = made;
                    }
            });
            return;
        }
        app.status("Select a video event first.");
    });
    act("videoTrackFx", "Video Track F&X...", QList<QKeySequence>(), [this, &app] {
        for (const Track& t : app.project().tracks)
            if (t.id == app.currentTrack && t.type == TrackType::Video) {
                FxTarget ft;
                ft.kind = FxTarget::Track;
                ft.id = t.id;
                openFx(ft, 0);
                return;
            }
        app.status("Select a video track first.");
    });
    act("colorGrading", "&Color Grading\tAlt+G", QList<QKeySequence>(), [this] { if (QAction* x = vegasAction("showColorGrading")) x->trigger(); });
    act("motionTracking", "Motion Trac&king\tAlt+M", QList<QKeySequence>(), [this] { if (QAction* x = vegasAction("showMotionTracking")) x->trigger(); });
    // Insert > Add Missing Streams for Selected Event: an event that plays only the image (or only the sound) of a file
    // that has both gets the other as its own event at the same place, grouped with it, on the first track of that kind
    // with room (a new track if none)
    act("addMissingStream", "Add Missing Streams for Selected Event", QList<QKeySequence>(), [&app] {
        int added = 0;
        app.edit("Add Missing Stream", [&](Project& p) {
            std::vector<uint64_t> ids(app.selection().begin(), app.selection().end());
            for (uint64_t id : ids) {
                Track* t = nullptr;
                Event* e0 = p.findEvent(id, &t);
                if (!e0 || !t) continue;
                const MediaItem* m = p.findMedia(e0->mediaId);
                if (!m || m->generated || m->offline || !m->info.hasVideo || !m->info.hasAudio || m->info.isStill) continue;
                const TrackType want = t->type == TrackType::Video ? TrackType::Audio : TrackType::Video;
                bool there = false;
                for (const Track& o : p.tracks)
                    if (o.type == want)
                        for (const Event& x : o.events)
                            if (x.mediaId == e0->mediaId && e0->group && x.group == e0->group) there = true;
                if (there) continue;
                Event e = *e0;
                if (!e.group) {
                    e.group = p.newId();
                    e0->group = e.group;
                }
                e.id = p.newId();
                e.fx.clear();
                e.transition = FxSlot();
                e.panCrop.clear();
                e.envelopes.clear();
                e.takes.clear();
                e.activeTake = 0;
                e.veg.reset();
                e.fadeIn = e0->fadeIn;
                e.fadeOut = e0->fadeOut;
                const double from = e.start, to = e.end();
                Track* target = nullptr;
                for (Track& o : p.tracks) {
                    if (o.type != want) continue;
                    bool free = true;
                    for (const Event& x : o.events) if (x.start < to - 1e-9 && x.end() > from + 1e-9) free = false;
                    if (free) { target = &o; break; }
                }
                if (!target) {
                    Track nt;
                    nt.id = p.newId();
                    nt.type = want;
                    if (want == TrackType::Audio) nt.panType = "add";
                    size_t at = 0;
                    for (size_t k = 0; k < p.tracks.size(); ++k) if (p.tracks[k].type == want) at = k + 1;
                    if (at == 0 && want == TrackType::Audio) at = p.tracks.size();
                    p.tracks.insert(p.tracks.begin() + (std::ptrdiff_t)at, nt);
                    target = &p.tracks[at];
                }
                target->events.push_back(e);
                target->sortEvents();
                ++added;
            }
        });
        if (!added) app.status("Select an event that plays only the picture or only the sound of a file that has both.");
    });
    // Tools > Audio > Open Copy in Audio Editor: the event's file is copied (beside it, "<name> (Copy).wav"), the event
    // (and those grouped with it playing the same file) play the copy, and the copy is opened in the audio editor, so
    // edits do not change the file other events or projects use
    act("audioEditorCopy", "Open &Copy in Audio Editor", QList<QKeySequence>(), [this, &app] {
        const Project& pr = app.project();
        const Event* e = nullptr;
        for (uint64_t id : app.selection())
            if (const Event* x = pr.findEvent(id)) {
                const MediaItem* m = pr.findMedia(x->mediaId);
                if (m && !m->generated && m->info.hasAudio && QFileInfo::exists(QString::fromStdString(m->path))) { e = x; break; }
            }
        if (!e) { app.status("Select an event of a sound file first."); return; }
        const MediaItem src = *pr.findMedia(e->mediaId);
        const QFileInfo fi(QString::fromStdString(src.path));
        QString copy;
        for (int n = 1; n < 1000; ++n) {
            copy = fi.dir().filePath(QString("%1 (Copy%2).%3").arg(fi.completeBaseName(), n > 1 ? QString(" %1").arg(n) : QString(), fi.suffix()));
            if (!QFileInfo::exists(copy)) break;
        }
        if (!QFile::copy(fi.filePath(), copy)) {
            const QString dir = projectFolder().isEmpty() ? QDir::tempPath() : projectFolder();
            copy = QDir(dir).filePath(QString("%1 (Copy).%2").arg(fi.completeBaseName(), fi.suffix()));
            for (int n = 2; QFileInfo::exists(copy) && n < 1000; ++n) copy = QDir(dir).filePath(QString("%1 (Copy %2).%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix()));
            if (!QFile::copy(fi.filePath(), copy)) { app.status("Could not copy " + src.path + "."); return; }
        }
        std::string err;
        const uint64_t made = app.importMedia(copy.toStdString(), &err);
        if (!made) { app.status("The copy could not be read: " + err); return; }
        const uint64_t evId = e->id, group = e->group;
        app.edit("Open Copy in Audio Editor", [&](Project& p) {
            for (Track& t : p.tracks)
                for (Event& x : t.events)
                    if (x.mediaId == src.id && (x.id == evId || (group && x.group == group))) x.mediaId = made;
        });
        openInAudioEditor(evId);
    });
    act("editSourceProject", "Edit Source Pro&ject", QList<QKeySequence>(), [this] { if (QAction* x = vegasAction("nestOpen")) x->trigger(); });
    // an event's Create Subclip: the part of the media the event plays becomes a subclip in Project Media, and the event
    // (and grouped events playing the same part of the same file) play the subclip
    act("eventSubclip", "Create Su&bclip", QList<QKeySequence>(), [this, &app] {
        const Project& pr = app.project();
        const Event* e = nullptr;
        for (uint64_t id : app.selection())
            if (const Event* x = pr.findEvent(id)) {
                const MediaItem* m = pr.findMedia(x->mediaId);
                if (m && !m->generated && !m->info.isStill && x->rate > 0) { e = x; break; }
            }
        if (!e) { app.status("Select an event of a video or sound file first."); return; }
        const MediaItem src = *pr.findMedia(e->mediaId);
        const double from = std::max(0.0, e->offset), to = e->offset + e->length * e->rate;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Create Subclip", "Name:", QLineEdit::Normal, QString::fromStdString(src.name) + " Subclip", &ok).trimmed();
        if (!ok) return;
        const uint64_t evId = e->id, group = e->group;
        app.edit("Create Subclip", [&](Project& p) {
            MediaItem m = src;
            m.id = p.newId();
            m.veg.reset();
            m.subclipStart = from;
            m.subclipLength = to - from;
            if (src.info.duration > 0) m.subclipLength = std::min(m.subclipLength, std::max(0.0, src.info.duration - from));
            m.info.duration = m.subclipLength;
            m.name = name.isEmpty() ? src.name + " Subclip" : name.toStdString();
            m.redEye.clear();
            p.media.push_back(m);
            for (Track& t : p.tracks)
                for (Event& x : t.events)
                    if (x.mediaId == src.id && (x.id == evId || (group && x.group == group)) && x.offset >= from - 1e-6 && x.offset + x.length * x.rate <= to + 1e-6)
                        x.mediaId = m.id;
        });
        app.status("Made the subclip \"" + name.toStdString() + "\" (in Project Media); the event plays it now.");
    });
    act("redEye", "Red E&ye Reduction...", QList<QKeySequence>(), [this, &app] {
        std::string why;
        const uint64_t media = RedEyeWindow::stillOfSelection(&why);
        if (!media) { app.status(why); return; }
        RedEyeWindow* w = dynamic_cast<RedEyeWindow*>(findChild<QDialog*>("redEyeWindow"));
        if (!w) w = new RedEyeWindow(this);
        if (!w->showMedia(media)) { app.status("The picture could not be read."); return; }
        w->show();
        w->raise();
        w->activateWindow();
    });
    act("openInTrimmer", "Open in Tri&mmer", QList<QKeySequence>(), [this, &app] {
        Tools* t = tools();
        if (!t || !t->trimmer) return;
        for (uint64_t id : app.selection())
            if (const Event* e = app.project().findEvent(id))
                if (t->trimmer->openMedia(e->mediaId)) {
                    if (t->trimmerDock) showDock(t->trimmerDock);
                    return;
                }
        app.status("Select an event with media first (Open in Trimmer, Alt+2).");
    });
    act("openParentInTrimmer", "Open Parent Media in Trimmer", QList<QKeySequence>(), [this, &app] {
        Tools* t = tools();
        if (!t || !t->trimmer) return;
        for (uint64_t id : app.selection())
            if (const Event* e = app.project().findEvent(id))
                if (t->trimmer->openParentMedia(e->mediaId)) {
                    if (t->trimmerDock) showDock(t->trimmerDock);
                    return;
                }
        app.status("Select an event first (Open Parent Media in Trimmer).");
    });
    act("runScript", "Ru&n Script...", QList<QKeySequence>(), [this] { runScriptDialog(this); });
    act("rescanScripts", "Res&can Script Menu Folder", QList<QKeySequence>(), [this] {
        if (QMenu* m = findChild<QMenu*>("scriptingMenu")) fillScriptMenu(m, this);
    });
    act("rebuildNestedAudio", "Rebuild Offline &Nested Audio", QList<QKeySequence>(), [] { rebuildOfflineNestedAudio(); });
    act("cleanProjectMedia", "&Clean Project Media", QList<QKeySequence>(), [this, &app] {
        int unused = 0;
        {
            std::set<uint64_t> used;
            for (const Track& t : app.project().tracks)
                for (const Event& e : t.events) {
                    used.insert(e.mediaId);
                    for (const Take& k : e.takes) used.insert(k.mediaId);
                }
            for (const MediaItem& m : app.project().media) if (!used.count(m.id)) ++unused;
        }
        if (!unused) { app.status("Clean Project Media: every piece of media is used on the timeline."); return; }
        if (QMessageBox::question(this, "Clean Project Media",
                                  QString("Remove the %1 piece%2 of media not used on the timeline from the project? (The files stay where they are.)")
                                      .arg(unused).arg(unused == 1 ? "" : "s"),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        const int n = app.cleanProjectMedia();
        app.status(strfmt("Clean Project Media: %d removed.", n));
    });

    // ------------------------------------------------------------------ Options
    QActionGroup* formats = new QActionGroup(this);
    // VEGAS's fifteen formats (engine/timefmt). The first five keep their old names, for keyboard maps.
    auto formatAction = [](const std::string& id) -> std::string {
        if (id == "samples") return "rulerSamples";
        if (id == "seconds") return "rulerSeconds";
        if (id == "frames") return "rulerFrames";
        if (id == "measures") return "rulerMeasures";
        if (id == "timecode") return "rulerTimecode";
        std::string n = "ruler" + id;
        n[5] = (char)std::toupper((unsigned char)n[5]);
        return n;
    };
    std::vector<std::string> fmts;
    for (const TimeFormatInfo& f : timeFormats()) {
        const std::string v = f.id;
        fmts.push_back(formatAction(v));
        QAction* x = act(fmts.back().c_str(), f.menuText, QList<QKeySequence>(), [&app, v] {
            if (app.project().settings.rulerFormat != v) app.edit("Ruler Format", [&](Project& p) { p.settings.rulerFormat = v; });
            QSettings().setValue("options/rulerFormat", QString::fromStdString(v));
        });
        x->setCheckable(true);
        x->setActionGroup(formats);
    }
    act("setTimeAtCursor", "Set T&ime at Cursor", QList<QKeySequence>(), [tl] { tl->setTimeAtCursor(); });
    formats->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    QActionGroup* gridGroup = new QActionGroup(this);
    std::vector<QAction*> gridItems;
    const struct { const char* name; const char* text; const char* id; } grids[] = {
        {"gridRulerMarks", "&Ruler Marks", "ruler"}, {"gridMinutes", "Mi&nutes", "minutes"}, {"gridSeconds", "&Seconds", "seconds"},
        {"gridMilliseconds", "&Milliseconds", "milliseconds"}, {"gridFrames", "&Frames", "frames"}, {"gridMeasures", "Me&asures", "measures"},
        {"gridQuarter", "&Quarter Notes", "quarter"}, {"gridQuarter3", "Q&uarter Note Triplets", "quarter3"}, {"grid8th", "&8th Notes", "8th"},
        {"grid8th3", "8th Note &Triplets", "8th3"}, {"grid16th", "&16th Notes", "16th"}, {"grid16th3", "16t&h Note Triplets", "16th3"},
        {"grid32nd", "&32nd Notes", "32nd"}, {"grid32nd3", "32nd Note Tr&iplets", "32nd3"}};
    for (const auto& g : grids) {
        const std::string id = g.id;
        QAction* x = act(g.name, g.text, QList<QKeySequence>(), [tl, id] {
            tl->options().gridSpacing = id;
            tl->optionsChanged();
        });
        x->setCheckable(true);
        x->setChecked(id == "ruler");
        x->setActionGroup(gridGroup);
        gridItems.push_back(x);
    }
    QAction* gridRuler = gridItems[0];
    rename("rippleTracks", "Affected &Tracks");
    rename("rippleMarkers", "Affected Tracks, &Bus Tracks, Markers, and Regions");
    rename("rippleAll", "&All Tracks, Markers, and Regions");
    rename("ignoreGrouping", "I&gnore Event Grouping");
    rename("lockEnvelopes", "Lock En&velopes to Events");
    rename("autoRipple", "Auto &Ripple");

    // ------------------------------------------------------------------ the menus in VEGAS's order
    QMenu* file = topMenu(this, "File");
    QMenu* edit = topMenu(this, "Edit");
    QMenu* view = topMenu(this, "View");
    QMenu* insert = topMenu(this, "Insert");
    QMenu* toolsMenu = topMenu(this, "Tools");
    QMenu* options = topMenu(this, "Options");
    QMenu* help = topMenu(this, "Help");

    if (file) {
        // not rearranged: recent projects go in after a separator of their own when the menu opens
        // VEGAS's order: New, New Empty Project, Open, Close | Save, Save As, Incremental Save | Render As | Import, Add Media
        QAction* render = vegasAction("render");
        if (render) file->removeAction(render);
        QList<QAction*> acts = file->actions();
        for (int k = 0; k < acts.size(); ++k) {
            if (acts[k]->objectName() == "new" && k + 1 < acts.size()) file->insertAction(acts[k + 1], vegasAction("newPlain"));
            if (acts[k]->objectName() == "save") file->insertSeparator(acts[k]);
            if (acts[k]->menu() && plainMenuText(acts[k]->text()) == "Import") {
                if (render) {
                    file->insertAction(acts[k], render);
                    file->insertSeparator(acts[k]);
                }
                if (k + 1 < acts.size()) file->insertAction(acts[k + 1], vegasAction("addMedia"));
            }
        }
    }
    if (edit) {
        QMenu* nav = subMenu(edit, "Navigate");
        if (nav) {
            nav->setTitle("Na&vigate");
            for (const char* n : {"goSelectionStart", "goSelectionEnd", "gridLeft", "gridRight", "pixelLeft", "pixelRight", "timeSelectRightMarker"})
                nav->addAction(vegasAction(n));
            arrange(nav, {"goTo", "goStart", "goEnd", "goSelectionStart", "goSelectionEnd", "gridLeft", "gridRight", "pixelLeft", "pixelRight",
                          "prevFrame", "nextFrame", "-", "prevMarker", "nextMarker", "timeSelectRightMarker", "-", "prevEditPoint", "nextEditPoint",
                          "prevSelectedEdge", "nextSelectedEdge", "-", "cursorToPlayback", "centerCursor"});
        }
        if (QMenu* sel = subMenu(edit, "Select")) {
            sel->setTitle("Se&lect");
            sel->addActions({vegasAction("selectAfterCursor"), vegasAction("selectEventsToEnd"), vegasAction("loop"), vegasAction("zoomSelection"),
                             vegasAction("selectionToView"), vegasAction("selectionToProject"), vegasAction("selectionStart"), vegasAction("selectionEndHere")});
            arrange(sel, {"selectAll", "selectAfterCursor", "-", "prevEdge", "nextEdge", "expandedEdit", "exitEditMode", "-", "loop", "-",
                          "zoomSelection", "selectionToView", "selectionToProject", "selectLoopRegion", "-", "selectionStart", "selectionEndHere",
                          "timeSelectPrev", "timeSelectNext", "restoreSelection", "-", "unselectAll", "selectEventsToEnd"});
        }
        if (QMenu* tool = subMenu(edit, "Editing Tool")) {
            tool->setTitle("&Editing Tool");
            tool->addActions({shuffle, slip, slide, stretch, splitTrim});
            arrange(tool, {"toolNormal", "toolShuffle", "toolSlip", "toolSlide", "toolStretch", "toolSplitTrim", "-", "toolEnvelope", "toolSelection",
                           "toolZoom", "-", "toolNext", "toolPrevious"});
        }
        if (QMenu* sw = subMenu(edit, "Switches")) {
            sw->addActions({vegasAction("switchInvertPhase"), norm, autoNorm, vegasAction("switchAspect"), vegasAction("switchFlicker"), resProject, resForce, resOptical, resOff});
            arrange(sw, {"switchMute", "lockEvent", "switchLoop", "-", "switchInvertPhase", "switchNormalize", "switchAutoNormalize", "-", "switchAspect", "switchFlicker", "-",
                         "switchResampleProject", "switchResampleForce", "switchResampleOptical", "switchResampleOff", "-", "switchReverse"});
            rename("lockEvent", "&Lock");
            rename("switchLoop", "L&oop");
        }
        if (QMenu* take = subMenu(edit, "Take")) arrange(take, {"takeRename", "takeChoose", "-", "takeNext", "takePrevious", "-", "takeDeleteActive", "takeDelete"});
        if (QMenu* group = subMenu(edit, "Group")) {
            group->addActions({vegasAction("groupCutAll"), vegasAction("groupCopyAll"), vegasAction("groupDeleteAll")});
            arrange(group, {"group", "-", "ungroup", "clearGroup", "selectGroup", "-", "groupCutAll", "groupCopyAll", "groupDeleteAll"});
        }
        QMenu* streamMenu = new QMenu("&Stream", edit);
        streamMenu->addAction(stream);
        QMenu* chanMenu = new QMenu("C&hannels", edit);
        for (const auto& c : chans) chanMenu->addAction(vegasAction(c.name));
        edit->addMenu(streamMenu);
        edit->addMenu(chanMenu);
        edit->addActions({vegasAction("pasteAttributes"), vegasAction("selectivePaste"), vegasAction("eventHeal"), vegasAction("closeGaps"),
                          vegasAction("undoAll"), vegasAction("clearHistory")});
        arrange(edit, {"undo", "redo", "-", "cut", "copy", "paste", "pasteRepeat", "pasteInsert", "pasteAttributes", "selectivePaste", "-",
                       "delete", "trim", "trimStart", "trimEnd", "split", "smartSplit", "detectScenes", "eventHeal", "closeGaps", "-", ">Navigate", ">Post-Edit Ripple",
                       ">Select", ">Editing Tool", "-", ">Switches", ">Take", ">Group", ">Stream", ">Channels", "-", "undoAll", "clearHistory", "-",
                       "rename", ">Nested Timelines", ">Pitch"});
    }
    if (view) {
        QAction* toolbar = nullptr;
        if (QToolBar* main = findChild<QToolBar*>("mainToolBar")) {
            toolbar = main->toggleViewAction();
            toolbar->setObjectName("showToolbar");
            toolbar->setText("&Toolbar");
        }
        QMenu* mediaMarkers = new QMenu("Eve&nt Media Markers", view);
        mediaMarkers->addActions({vegasAction("showMediaMarkers"), markerLabels});
        QMenu* audioEnv = new QMenu("&Audio Envelopes", view);
        audioEnv->addActions({vegasAction("viewEnvAudioMute"), vegasAction("viewEnvAudioVolume"), vegasAction("viewEnvAudioPan")});
        QMenu* videoEnv = new QMenu("Video &Envelopes", view);
        videoEnv->addActions({vegasAction("viewEnvVideoMute"), vegasAction("viewEnvVideoComposite"), vegasAction("viewEnvVideoFade")});
        videoEnv->addSeparator();
        videoEnv->addAction(vegasAction("viewEnvVelocity"));
        videoEnv->addSeparator();
        videoEnv->addActions({vegasAction("viewEnvMotionBlur"), vegasAction("viewEnvSupersample")});
        if (toolbar) view->addAction(toolbar);
        if (QAction* status = findChild<QAction*>("showStatusBar")) view->addAction(status);
        view->addMenu(mediaMarkers);
        view->addMenu(audioEnv);
        view->addMenu(videoEnv);
        view->addActions({eventLength, vegasAction("minimizeAllTracks")});
        view->removeAction(vegasAction("showMediaMarkers"));
        view->removeAction(vegasAction("externalMonitor"));
        if (QMenu* win = subMenu(view, "Window")) {
            win->setTitle("Window");
            const QStringList order = {"Explorer", "Trimmer", "Master Bus", "Video Preview", "Project Media", "Project Notes", "Transitions", "Video FX",
                                       "Media Generators", "Plug-In Manager", "Video Scopes", "Surround Panner", "Edit Details", "Device Explorer", "Loudness Meters",
                                       "Dashboard", "Mixing Console", "Audio FX", "Color Grading", "Motion Tracking", "Text to Speech"};
            QList<QAction*> acts = win->actions(), sorted;
            for (const QString& t : order)
                for (QAction* a : acts)
                    if (!sorted.contains(a) && plainMenuText(a->text()) == t) sorted << a;
            for (QAction* a : acts) if (!sorted.contains(a)) sorted << a;
            for (QAction* a : acts) win->removeAction(a);
            win->addActions(sorted);
        }
        arrange(view, {"showToolbar", "showStatusBar", "-", ">Window Layouts", "-", "focusTimeline", ">Window", "-", "showBusTracks", "-",
                       ">Event Media Markers", "showTakeInfo", "showWaveforms", "showEventButtons", "showFadeLengths", "edgeFrames", "eventHeaders",
                       "showEventLength", "-", ">Audio Envelopes", ">Video Envelopes", "showEnvelopes", "-", "minimizeAllTracks", "rebuildPeaks"});
    }
    if (insert) {
        if (QMenu* a = subMenu(insert, "Audio Envelopes")) a->setTitle("A&udio Envelopes");
        if (QMenu* v = subMenu(insert, "Video Envelopes")) v->setTitle("V&ideo Envelopes");
        insert->addActions({vegasAction("addMissingStream"), vegasAction("insertEmptyEvent"), vegasAction("insertAdjustmentEvent"), vegasAction("insertText"), vegasAction("insertSlideshow"),
                            vegasAction("insertSubtitlesFile"), vegasAction("insertSubtitlesRegions"), vegasAction("insertTime")});
        arrange(insert, {">Audio Envelopes", ">Video Envelopes", "-", "audioTrack", "videoTrack", "adjustmentTrack", "-", "audioBus", "-",
                         "addMissingStream", "insertEmptyEvent", "insertAdjustmentEvent", "insertText", "generatedMedia", "insertSlideshow", "insertSubtitlesFile", "insertSubtitlesRegions", "-", "insertTime", "-", "marker", "region", "-", "cdTrack", "cdIndex", "-",
                         "commandMarker"});
    }
    if (toolsMenu) {
        QMenu* audio = subMenu(toolsMenu, "Audio");
        QMenu* video = subMenu(toolsMenu, "Video");
        if (audio) {
            audio->addActions({vegasAction("audioEditorCopy"), vegasAction("editSourceProject"), vegasAction("audioEventFx"), vegasAction("trackFx")});
            arrange(audio, {"audioEditor", "audioEditorCopy", "editSourceProject", "-", "audioEventFx", "-", "trackFx", "-", "masterFx"});
        }
        if (video) {
            video->addActions({vegasAction("videoEventFx"), vegasAction("mediaFx"), vegasAction("colorGrading"), vegasAction("motionTracking"), vegasAction("redEye"), vegasAction("videoTrackFx")});
            arrange(video, {"eventPanCrop", "videoEventFx", "mediaFx", "colorGrading", "motionTracking", "redEye", "-", "videoTrackFx", "trackMotion", "-", "videoOutFx",
                            "-", "bypassFx"});
        }
        toolsMenu->removeAction(vegasAction("eventFx"));
        toolsMenu->removeAction(vegasAction("trackFx"));
        QMenu* scripting = new QMenu("Scrip&ting", toolsMenu);
        scripting->setObjectName("scriptingMenu");
        scripting->addActions({vegasAction("runScript"), vegasAction("rescanScripts")});
        // VEGAS's closed captioning scripts (outputcmds.cpp; kept when the Script Menu folder is scanned again)
        scripting->addSeparator();
        for (const char* n : {"scriptPromoteCaptions", "scriptExportCaptionsDvda", "scriptExportCaptionsQuickTime", "scriptExportCaptionsWmp", "scriptExportCaptionsYouTube"})
            if (QAction* a = vegasAction(n)) {
                a->setProperty("fixedScriptItem", true);
                scripting->addAction(a);
            }
        fillScriptMenu(scripting, this);
        toolsMenu->addMenu(scripting);
        toolsMenu->addActions({vegasAction("externalMonitor"), vegasAction("cleanProjectMedia"), vegasAction("rebuildNestedAudio"), vegasAction("openInTrimmer")});
        arrange(toolsMenu, {">Audio", ">Video", "-", "renderToNewTrack", "externalMonitor", "selectivePrerender", "cleanPrerender", "ramPreview", "-",
                            ">Burn Disc", "printToTape", "printToHdvTape", "-",
                            ">Scripting", ">Multicamera", "-", "cleanProjectMedia", "rebuildNestedAudio", "relinkWireless", "-", "layOutAudioCd",
                            "generateLoudnessLog", "extractAudioCd", "-", "openInTrimmer", "record", "armTrack", "armTrackPath", "dimOutput",
                            "-", "rescan", "rescanAll"});
    }
    if (options) {
        QMenu* grid = new QMenu("Gr&id Spacing", options);
        (void)gridRuler;
        for (QAction* g : gridItems) {
            grid->addAction(g);
            if (g->objectName() == "gridRulerMarks" || g->objectName() == "gridFrames") grid->addSeparator();
        }
        QMenu* ruler = new QMenu("Ruler &Format", options);
        for (const std::string& f : fmts) ruler->addAction(vegasAction(f.c_str()));
        ruler->addSeparator();
        ruler->addAction(vegasAction("setTimeAtCursor"));
        options->addMenu(grid);
        options->addMenu(ruler);
        if (QMenu* ripple = subMenu(options, "Ripple Mode")) {
            ripple->setTitle("&Ripple");
            QList<QAction*> modes = ripple->actions();
            ripple->clear();
            ripple->addAction(vegasAction("autoRipple"));
            ripple->addSeparator();
            ripple->addActions(modes);
        }
        if (QMenu* tc = subMenu(options, "Timecode")) tc->setTitle("Time&code");
        // VEGAS Pro 23: Quantize to Frames is a submenu: Enable Quantize to Frames, Show Unquantized Event Edges...
        QMenu* quant = new QMenu("&Quantize to Frames", options);
        quant->setObjectName("quantizeMenu");
        quant->addActions({vegasAction("quantize"), vegasAction("showUnquantized")});
        options->removeAction(vegasAction("quantize"));
        options->removeAction(vegasAction("showUnquantized"));
        options->addMenu(quant);
        arrange(options, {">Quantize to Frames", "-", "snapping", "snapGrid", "snapMarkers", "snapAllEvents", "-", ">Grid Spacing", ">Ruler Format", "-",
                          "autoCrossfade", "quickFade", "autoRipple", ">Ripple", "lockEnvelopes", "ignoreGrouping", "slipAllTakes", "-", "loop", "-", ">Timecode", "-",
                          "preferences"});
    }
    if (help) arrange(help, {"helpContents", "whatsThis", "shortcuts", "-", "about"});

    {
        auto putAfter = [](QMenu* m, const char* after, QAction* a) {
            if (!m || !a) return;
            const QList<QAction*> acts = m->actions();
            for (int k = 0; k < acts.size(); ++k)
                if (acts[k]->objectName() == after) {
                    if (k + 1 < acts.size()) m->insertAction(acts[k + 1], a);
                    else m->addAction(a);
                    return;
                }
            m->addAction(a);
        };
        QAction* bypassAudio = act("bypassAudioFx", "Bypass All A&udio FX", QList<QKeySequence>(), [&app] {});
        bypassAudio->setCheckable(true);
        bypassAudio->setChecked(app.bypassAudioFx.load());
        QObject::connect(bypassAudio, &QAction::toggled, this, [&app](bool on) {
            app.bypassAudioFx = on;
            app.status(on ? "Bypass All Audio FX: playback leaves out every audio effect." : "Audio effects are heard again.");
        });
        putAfter(options, "slipAllTakes", bypassAudio);
        QAction* rtRender = act("realTimeRender", "Rea&l-Time Render...", QList<QKeySequence>(), [this] { runRealTimeRender(this); });
        putAfter(file, "render", rtRender);
        if (QMenu* audio = subMenu(toolsMenu, "Audio")) {
            QAction* nrt = act("nonRealTimeFx", "Apply &Non-Real-Time Event FX...", QList<QKeySequence>(), [this] {
                NrtHooks hooks;
                hooks.preview = [this](bool play, double from) {
                    if (!playback_) return;
                    if (play) {
                        AppState::get().setCursor(from);
                        playback_->play();
                    } else {
                        playback_->stop();
                    }
                };
                runNonRealTimeEventFx(this, hooks);
            });
            putAfter(audio, "audioEventFx", nrt);
            QMenu* remove = new QMenu("&Remove Assignable FX", audio);
            remove->setObjectName("removeAssignableMenu");
            QObject::connect(remove, &QMenu::aboutToShow, remove, [remove] { fillRemoveAssignableMenu(remove); });
            fillRemoveAssignableMenu(remove);
            audio->addMenu(remove);
        }
        QAction* assignable = act("assignableFx", "Audio Assi&gnable FX...", QList<QKeySequence>(), [this] {
            const uint64_t id = insertAssignableFx();
            FxTarget t;
            t.kind = FxTarget::Bus;
            t.id = id;
            if (id) openFx(t, 0);
        });
        putAfter(insert, "audioBus", assignable);
        QAction* input = act("inputBus", "&Input Bus", QList<QKeySequence>(), [&app] {
            app.status("Input buses are not available. Reno records from the input chosen in Preferences.");
        });
        input->setToolTip("Not available: Reno records from the input chosen in Preferences");
        input->setEnabled(false);
        putAfter(insert, "assignableFx", input);
    }

    // ticks and greying of these commands follow the selection and project when a menu opens (and before the timeline's
    // right-click menus that show some of them)
    for (QMenu* m : {file, edit, view, insert, toolsMenu, options, help}) {
        if (!m) continue;
        QObject::connect(m, &QMenu::aboutToShow, this, [this] { updateVegasCommands(); });
        for (QMenu* sub : m->findChildren<QMenu*>())
            QObject::connect(sub, &QMenu::aboutToShow, this, [this] { updateVegasCommands(); });
    }
    tl->commandAction = [this](const char* name) { return vegasAction(name); };
    tl->onBeforeMenu = [this] { updateVegasCommands(); };
    updateVegasCommands();
}

void MainWindow::updateVegasCommands()
{
    AppState& app = AppState::get();
    const Project& p = app.project();
    bool anyVideo = false, anyAudio = false;
    bool invert = true, normalize = true, autoNormalize = true, aspect = true, flicker = true;
    std::string resample;
    std::string chan;
    bool sameChan = true;
    for (uint64_t id : app.selection()) {
        const Track* t = nullptr;
        const Event* e = p.findEvent(id, &t);
        if (!e || !t) continue;
        if (t->type == TrackType::Video) {
            anyVideo = true;
            aspect = aspect && e->keepAspect;
            flicker = flicker && e->reduceFlicker;
            const std::string r = (e->resample == "force" || e->resample == "off" || e->resample == "optical") ? e->resample : std::string("project");
            resample = resample.empty() || resample == r ? r : std::string("mixed");
        } else {
            const std::string c = e->channels.empty() ? std::string("both") : e->channels;
            if (!anyAudio) chan = c;
            else if (chan != c) sameChan = false;
            anyAudio = true;
            invert = invert && e->invertPhase;
            normalize = normalize && e->normalize;
            autoNormalize = autoNormalize && e->autoNormalize;
        }
    }
    auto set = [this](const char* name, bool enabled, bool checked) {
        if (QAction* x = vegasAction(name)) {
            x->setEnabled(enabled);
            if (x->isCheckable()) x->setChecked(enabled && checked);
        }
    };
    set("switchInvertPhase", anyAudio, invert);
    set("switchNormalize", anyAudio, normalize);
    set("switchAutoNormalize", anyAudio, autoNormalize);
    set("switchAspect", anyVideo, aspect);
    set("switchFlicker", anyVideo, flicker);
    set("switchResampleProject", anyVideo, resample == "project");
    set("switchResampleForce", anyVideo, resample == "force");
    set("switchResampleOptical", anyVideo, resample == "optical");
    set("switchResampleOff", anyVideo, resample == "off");
    for (const char* n : {"channelsBoth", "channelsLeft", "channelsRight", "channelsCombine", "channelsSwap"}) {
        const std::string id = n == std::string("channelsBoth") ? "both" : n == std::string("channelsLeft") ? "left" : n == std::string("channelsRight") ? "right"
                             : n == std::string("channelsCombine") ? "combine" : "swap";
        set(n, anyAudio, sameChan && chan == id);
    }
    const bool sel = !app.selection().empty();
    set("pasteAttributes", app.hasClipboard() && sel, false);
    set("selectivePaste", app.hasClipboard() && sel, false);
    set("eventHeal", sel, false);
    set("closeGaps", sel, false);
    set("selectEventsToEnd", sel, false);
    set("groupCutAll", sel, false);
    set("groupCopyAll", sel, false);
    set("groupDeleteAll", sel, false);
    set("openInTrimmer", sel, false);
    set("mediaFx", sel, false);
    set("redEye", RedEyeWindow::stillOfSelection(nullptr) != 0, false);
    if (TimelineWidget* tlw = timeline()) {
        const struct { const char* name; const char* id; } gs[] = {
            {"gridRulerMarks", "ruler"}, {"gridMinutes", "minutes"}, {"gridSeconds", "seconds"}, {"gridMilliseconds", "milliseconds"}, {"gridFrames", "frames"},
            {"gridMeasures", "measures"}, {"gridQuarter", "quarter"}, {"gridQuarter3", "quarter3"}, {"grid8th", "8th"}, {"grid8th3", "8th3"},
            {"grid16th", "16th"}, {"grid16th3", "16th3"}, {"grid32nd", "32nd"}, {"grid32nd3", "32nd3"}};
        for (const auto& g : gs) set(g.name, true, tlw->options().gridSpacing == g.id);
        set("showMarkerLabels", true, tlw->options().showMarkerLabels);
        set("showEventLength", true, tlw->options().showEventLength);
    }
    set("undoAll", app.canUndo(), false);
    set("clearHistory", app.canUndo() || app.canRedo(), false);
    set("zoomSelection", app.loopEnd > app.loopStart, false);
    for (QAction* x : findChildren<QAction*>())
        if (x->property("vegasUnavailable").toBool()) x->setEnabled(false);
    set("insertEmptyEvent", app.currentTrack != 0 && app.loopEnd > app.loopStart, false);
    {
        const std::string f = p.settings.rulerFormat;
        for (const TimeFormatInfo& tf : timeFormats()) {
            std::string n = std::string("ruler") + tf.id;
            if (n == "rulersamples") n = "rulerSamples";
            else if (n == "rulerseconds") n = "rulerSeconds";
            else if (n == "rulerframes") n = "rulerFrames";
            else if (n == "rulermeasures") n = "rulerMeasures";
            else if (n == "rulertimecode") n = "rulerTimecode";
            else n[5] = (char)std::toupper((unsigned char)n[5]);
            set(n.c_str(), true, f == tf.id);
        }
        set("setTimeAtCursor", f != "cdtime", false);
    }
    // View > Audio / Video Envelopes: ticked when every envelope of the kind is shown (greyed when there is none)
    struct K { const char* name; const char* kind; int where; };
    const K ks[] = {{"viewEnvAudioMute", "mute", 0}, {"viewEnvAudioVolume", "volume", 0}, {"viewEnvAudioPan", "pan", 0}, {"viewEnvVideoMute", "mute", 1},
                    {"viewEnvVideoComposite", "composite", 1}, {"viewEnvVideoFade", "fadecolor", 1}, {"viewEnvVelocity", "velocity", 2},
                    {"viewEnvMotionBlur", "motionblur", 3}, {"viewEnvSupersample", "supersample", 3}};
    for (const K& k : ks) {
        bool any = false, all = true;
        auto look = [&](const Envelopes& list) {
            for (const Envelope& e : list)
                if (e.kind == k.kind) { any = true; all = all && e.visible; }
        };
        if (k.where == 3) look(p.videoEnvelopes);
        for (const Track& t : p.tracks) {
            if ((t.type == TrackType::Video) != (k.where != 0)) continue;
            if (k.where <= 1) look(t.envelopes);
            else if (k.where == 2) for (const Event& e : t.events) look(e.envelopes);
        }
        set(k.name, any, all);
    }
}

// Edit > Selectively Paste Event Attributes: which of the copied event's attributes go onto the selected events.
void MainWindow::selectivePasteDialog()
{
    AppState& app = AppState::get();
    const Event* src = app.clipboardEvent();
    if (!src) { app.status("Copy an event first: its attributes are what is pasted."); return; }
    if (app.selection().empty()) { app.status("Select the events to paste the attributes onto."); return; }
    QDialog dlg(this);
    dlg.setObjectName("selectivePasteDialog");
    dlg.setWindowTitle("Selectively Paste Event Attributes");
    QVBoxLayout* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(QString("Paste from \"%1\":").arg(QString::fromStdString(src->name.empty() ? std::string("the copied event") : src->name)), &dlg));
    const struct { int attr; const char* text; } items[] = {
        {AppState::AttrFx, "Event FX"}, {AppState::AttrPanCrop, "Pan/Crop (video)"}, {AppState::AttrEnvelopes, "Envelopes"},
        {AppState::AttrFadeIn, "Fade In"}, {AppState::AttrFadeOut, "Fade Out"}, {AppState::AttrLevel, "Opacity / Volume"},
        {AppState::AttrRate, "Playback Rate and Reverse"}, {AppState::AttrSwitches, "Switches"},
        {AppState::AttrPitch, "Pitch, Time Stretch and Channels (audio)"}};
    std::vector<std::pair<QCheckBox*, int>> boxes;
    for (const auto& it : items) {
        QCheckBox* b = new QCheckBox(it.text, &dlg);
        b->setObjectName(QString("attr%1").arg(it.attr));
        b->setChecked(true);
        v->addWidget(b);
        boxes.push_back({b, it.attr});
    }
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    int attrs = 0;
    for (auto& b : boxes) if (b.first->isChecked()) attrs |= b.second;
    if (attrs) app.pasteEventAttributes(app.selection(), attrs);
}

}
}
