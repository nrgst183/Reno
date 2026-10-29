// The main window's menus and the timeline's right-click menus against VEGAS Pro 18's (read from vegas180.exe),
// and that each command works and is one undo step: Paste Event Attributes / Selectively Paste, Event Heal, Close Gaps, Undo All,
// Clear Edit History, Select Events to End / All After Cursor, the switches (Invert Phase, Normalize, Reduce Interlace Flicker...),
// Channels, Group > Cut / Copy / Delete All, Insert Time / Empty Event, Clean Project Media, Duplicate Track, View > Audio / Video
// Envelopes, Ruler Format, Minimize All Tracks, Set Selection to View / Project, the Navigate items, Tools > Scripting's Script Menu.
//
//   menus_test <output folder>
//
// Build: uibuild menus_test tests/ui/menus_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/engine/engine.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <QPushButton>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
MainWindow* gW = nullptr;

QString plain(QString t) { return t.split('\t')[0].remove('&'); }

// The menu bar's item at a path like "Edit/Switches/Normalize" (null if there is none).
QAction* item(const QString& path)
{
    const QStringList parts = path.split(path.contains('|') ? '|' : '/');
    QList<QAction*> acts = gW->menuBar()->actions();
    QAction* found = nullptr;
    for (const QString& p : parts) {
        found = nullptr;
        for (QAction* a : acts)
            if (!a->isSeparator() && plain(a->text()) == p) { found = a; break; }
        if (!found) return nullptr;
        if (found->menu()) {
            emit found->menu()->aboutToShow();
            acts = found->menu()->actions();
        }
    }
    return found;
}

// The items of a menu, as plain texts (separators as "-").
QStringList texts(QMenu* m)
{
    QStringList out;
    if (!m) return out;
    for (QAction* a : m->actions())
        out << (a->isSeparator() ? QString("-") : a->objectName() == "undo" ? QString("Undo") : a->objectName() == "redo" ? QString("Redo") : plain(a->text()));
    return out;
}

// 'want' appears in 'have' in this order (other items may come between).
bool inOrder(const QStringList& have, const QStringList& want, QString* missing)
{
    int at = 0;
    for (const QString& w : want) {
        int k = at;
        while (k < have.size() && have[k] != w) ++k;
        if (k >= have.size()) {
            *missing = w;
            return false;
        }
        at = k + 1;
    }
    return true;
}

void checkOrder(const QStringList& have, const QStringList& want, const std::string& name)
{
    QString missing;
    const bool ok = inOrder(have, want, &missing);
    check(ok, name, ok ? std::string() : ("missing or out of order: " + missing + " in [" + have.join(" | ") + "]").toStdString());
}

QMenu* subOf(QMenu& m, const QString& title)
{
    for (QAction* a : m.actions())
        if (a->menu() && plain(a->text()) == title) return a->menu();
    return nullptr;
}

// The right-click menu at 'pos' in the timeline, without showing it.
QStringList contextAt(TimelineWidget* tl, QPoint pos, QMenu& m)
{
    m.clear();
    tl->buildContextMenu(&m, pos);
    return texts(&m);
}

// Answers the next dialog of this name (or a message box) when it opens.
void answer(const char* name, std::function<void(QDialog*)> fn, bool accept = true)
{
    QTimer::singleShot(150, [name, fn, accept] {
        for (QWidget* t : QApplication::topLevelWidgets())
            if (QDialog* d = qobject_cast<QDialog*>(t))
                if (d->isVisible() && (d->objectName() == name || QString(name).isEmpty())) {
                    fn(d);
                    if (accept) d->accept();
                    else d->reject();
                    return;
                }
    });
}

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

struct Ids {
    uint64_t v1 = 0, v2 = 0, a1 = 0;
    uint64_t clip = 0, snd = 0, pic = 0;
} ids;

// Two video tracks and an audio track, events with gaps between them.
void buildProject()
{
    AppState& app = st();
    app.newProject();
    std::string err;
    ids.clip = app.importMedia("synth:1", &err);
    ids.snd = app.importMedia("synthaudio:2", &err);
    ids.pic = app.importMedia("synthpic:3", &err);
    app.edit("Test Project", [&](Project& p) {
        auto track = [&](TrackType t, const char* name) {
            Track tr;
            tr.id = p.newId();
            tr.type = t;
            tr.name = name;
            p.tracks.push_back(tr);
            return tr.id;
        };
        ids.v1 = track(TrackType::Video, "V1");
        ids.v2 = track(TrackType::Video, "V2");
        ids.a1 = track(TrackType::Audio, "A1");
        auto ev = [&](uint64_t t, uint64_t m, double start, double len, double off) {
            Event e;
            e.id = p.newId();
            e.mediaId = m;
            e.start = start;
            e.length = len;
            e.offset = off;
            p.findTrack(t)->events.push_back(e);
            return e.id;
        };
        ev(ids.v1, ids.clip, 0.0, 2.0, 0.0);
        ev(ids.v1, ids.clip, 3.0, 2.0, 3.0);
        ev(ids.v2, ids.pic, 1.0, 3.0, 0.0);
        ev(ids.a1, ids.snd, 0.0, 4.0, 0.0);
        Project::Marker m;
        m.id = p.newId();
        m.time = 4.0;
        m.name = "M1";
        p.markers.push_back(m);
    });
    app.setCursor(1.0);
}

const Event* evAt(uint64_t track, int k)
{
    for (const Track& t : st().project().tracks)
        if (t.id == track) return k < (int)t.events.size() ? &t.events[(size_t)k] : nullptr;
    return nullptr;
}

size_t eventCount(uint64_t track)
{
    for (const Track& t : st().project().tracks)
        if (t.id == track) return t.events.size();
    return 0;
}

bool trigger(const QString& path)
{
    QAction* a = item(path);
    if (!a || !a->isEnabled()) return false;
    a->trigger();
    uitest::settle(30);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMenusTest");
    QApplication::setApplicationName("menus_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("menus");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    // a Script Menu folder (one of the places VEGAS's Script Menu is taken from: "Script Menu" beside the program)
    const QString scriptDir = QCoreApplication::applicationDirPath() + "/Script Menu";
    QDir(scriptDir).removeRecursively();
    QDir().mkpath(scriptDir + "/Folder");
    for (const QString& f : {scriptDir + "/Alpha Script.cs", scriptDir + "/Folder/Beta.js", scriptDir + "/notes.txt"}) {
        QFile file(f);
        if (file.open(QIODevice::WriteOnly)) file.write("// test\n");
    }
    buildProject();
    MainWindow w;
    gW = &w;
    w.resize(1500, 900);
    w.show();
    uitest::settle(300);
    buildProject();
    TimelineWidget* tl = w.timeline();
    uitest::settle(100);
    AppState& app = st();

    // ------------------------------------------------------------------ the main menus have VEGAS's items, in its order
    {
        QStringList bar;
        for (QAction* m : w.menuBar()->actions()) bar << plain(m->text());
        check(bar.join(",") == "File,Edit,View,Insert,Tools,Options,Help", "the menu bar is VEGAS's", bar.join(",").toStdString());
        auto menuOf = [](const QString& path) { QAction* a = item(path); return a ? a->menu() : nullptr; };
        checkOrder(texts(menuOf("File")), {"New...", "New Empty Project", "Open...", "Close", "-", "Save", "Save As...", "Incremental Save", "Render As...",
                                           "Import", "Add Media...", "Properties...", "Exit"}, "File: VEGAS's items in its order");
        checkOrder(texts(menuOf("Edit")), {"Undo", "Redo", "-", "Cut", "Copy", "Paste", "Paste Repeat...", "Paste Insert", "Paste Event Attributes",
                                           "Selectively Paste Event Attributes...", "-", "Delete", "Trim", "Trim Start", "Trim End", "Split", "Smart Split",
                                           "Event Heal", "Close Gaps", "-", "Navigate", "Post-Edit Ripple", "Select", "Editing Tool", "-", "Switches", "Take",
                                           "Group", "Stream", "Channels", "-", "Undo All", "Clear Edit History..."},
                   "Edit: VEGAS's items in its order");
        checkOrder(texts(menuOf("Edit/Navigate")), {"Go to Time...", "Go to Project Start", "Go to Project End", "Go to Time Selection/View Start",
                                                    "Go to Time Selection/View End", "Move Left by Grid Marks", "Move Right by Grid Marks",
                                                    "Move Left by Pixel", "Move Right by Pixel", "Move Left by Frame", "Move Right by Frame", "-",
                                                    "Move Left by Marker/Region", "Move Right by Marker/Region", "Time Select Right by Marker/Region", "-",
                                                    "Move Left by Event Edit Points", "Move Right by Event Edit Points", "Move Left by Selected Event",
                                                    "Move Right by Selected Event", "-", "Move Cursor to Playhead", "Center View around Cursor"},
                   "Edit > Navigate");
        checkOrder(texts(menuOf("Edit/Post-Edit Ripple")), {"Affected Tracks", "Affected Tracks, Bus Tracks, Markers, and Regions",
                                                            "Affected Tracks, Markers, and Regions", "All Tracks, Markers, and Regions"}, "Edit > Post-Edit Ripple");
        checkOrder(texts(menuOf("Edit/Select")), {"Select All", "Select All After Cursor", "-", "Select Event Start", "Select Event End",
                                                  "Enter Expanded Edit Mode", "Exit Edit Mode", "-", "Loop Playback", "-", "Zoom Selection",
                                                  "Set Selection to View", "Set Selection to Project", "Select Loop Region", "-", "Set Selection Start",
                                                  "Set Selection End", "Set Selection to Previous Selected Event", "Set Selection to Next Selected Event",
                                                  "Restore Previous Time Selection"}, "Edit > Select");
        checkOrder(texts(menuOf("Edit/Editing Tool")), {"Normal Edit Tool", "Shuffle Tool", "Slip Tool", "Slide Tool", "Time Stretch/Compress Tool",
                                                        "Split Trim Tool", "-", "Envelope", "Selection", "Zoom", "-", "Next Tool", "Previous Tool"},
                   "Edit > Editing Tool");
        checkOrder(texts(menuOf("Edit/Switches")), {"Mute", "Lock", "Loop", "-", "Invert Phase", "Normalize", "-", "Maintain Aspect Ratio",
                                                    "Reduce Interlace Flicker", "-", "Use Project Resample Mode", "Force Resample", "Disable Resample"},
                   "Edit > Switches");
        checkOrder(texts(menuOf("Edit/Take")), {"Rename Active", "Choose Active...", "-", "Next Take", "Previous Take", "-", "Delete Active", "Delete..."},
                   "Edit > Take");
        checkOrder(texts(menuOf("Edit/Group")), {"Create New", "-", "Remove From", "Clear", "Select All", "-", "Cut All", "Copy All", "Delete All"},
                   "Edit > Group");
        checkOrder(texts(menuOf("Edit/Channels")), {"Both", "Left Only", "Right Only", "Combine", "Swap"}, "Edit > Channels");
        checkOrder(texts(menuOf("View")), {"Toolbar", "Status Bar", "-", "Window Layouts", "-", "Focus to Timeline", "Window", "-", "Bus Tracks", "-",
                                           "Event Media Markers", "Active Take Information", "Audio Event Waveforms", "Event Buttons", "Event Fade Lengths",
                                           "Video Event Edge Edit Frames", "Event Headers", "Event Length", "-", "Audio Envelopes", "Video Envelopes",
                                           "Show Envelopes", "-", "Minimize All Tracks", "Rebuild Audio Peaks"}, "View: VEGAS's items in its order");
        checkOrder(texts(menuOf("View/Window")), {"Explorer", "Trimmer", "Master Bus", "Video Preview", "Project Media", "Project Notes", "Transitions",
                                                  "Video FX", "Media Generators", "Plug-In Manager", "Video Scopes", "Edit Details", "Device Explorer",
                                                  "Loudness Meters", "Dashboard"}, "View > Window: VEGAS's windows in its order");
        checkOrder(texts(menuOf("View/Audio Envelopes")), {"Track Mute", "Track Volume", "Track Pan"}, "View > Audio Envelopes");
        checkOrder(texts(menuOf("View/Video Envelopes")), {"Track Mute", "Track Composite Level", "Track Fade to Color", "-", "Event Velocity", "-",
                                                           "Motion Blur Amount", "Video Supersampling"}, "View > Video Envelopes");
        checkOrder(texts(menuOf("Insert")), {"Audio Envelopes", "Video Envelopes", "-", "Audio Track", "Video Track", "-", "Audio Bus", "-", "Empty Event",
                                             "Text...", "Generated Media...", "-", "Time...", "-", "Marker", "Region", "-", "Audio CD Track Region",
                                             "Audio CD Track Index", "-", "Command..."}, "Insert: VEGAS's items in its order");
        checkOrder(texts(menuOf("Tools")), {"Audio", "Video", "-", "Render to New Track...", "Preview on External Monitor", "Selectively Prerender Video...",
                                            "Clean Up Prerendered Video", "Build Dynamic RAM Preview", "-", "Scripting", "Multicamera", "-",
                                            "Clean Project Media", "Rebuild Offline Nested Audio"}, "Tools: VEGAS's items in its order");
        checkOrder(texts(menuOf("Tools/Audio")), {"Open in Audio Editor", "-", "Audio Event FX...", "Apply Non-Real-Time Event FX...", "-", "Track FX..."}, "Tools > Audio");
        checkOrder(texts(menuOf("Tools/Video")), {"Video Event Pan/Crop...", "Video Event FX...", "Media FX...", "Color Grading", "Motion Tracking", "-",
                                                  "Video Track FX...", "Track Motion...", "-", "Video Output FX..."}, "Tools > Video");
        checkOrder(texts(menuOf("Tools/Scripting")), {"Run Script...", "Rescan Script Menu Folder", "-", "Alpha Script", "Folder"},
                   "Tools > Scripting: VEGAS's items, then the scripts of the Script Menu folders (sub-folders as submenus)");
        check(!texts(menuOf("Tools/Scripting")).contains("notes"), "Tools > Scripting lists only scripts");
        check(item("Tools/Scripting/Folder/Beta") != nullptr, "Tools > Scripting > Folder > Beta (a script in a sub-folder)");
        checkOrder(texts(menuOf("Tools/Multicamera")), {"Create Multicamera Track", "Enable Multicamera Editing"}, "Tools > Multicamera");
        checkOrder(texts(menuOf("Options")), {"Quantize to Frames", "-", "Enable Snapping", "Snap to Grid", "Snap to Markers", "Snap to All Events", "-",
                                              "Grid Spacing", "Ruler Format", "-", "Automatic Crossfades", "Quickfade Audio Edits", "Auto Ripple", "Ripple", "Lock Envelopes to Events",
                                              "Ignore Event Grouping", "Slip All Takes", "-", "Loop Playback", "-", "Timecode", "-", "Preferences..."},
                   "Options: VEGAS's items in its order");
        checkOrder(texts(menuOf("Options/Ripple")), {"Auto Ripple", "-", "Affected Tracks", "Affected Tracks, Bus Tracks, Markers, and Regions",
                                                     "All Tracks, Markers, and Regions"}, "Options > Ripple");
        checkOrder(texts(menuOf("Options/Timecode")), {"Generate MIDI Timecode", "Generate MIDI Clock", "Trigger from MIDI Timecode"}, "Options > Timecode");
        checkOrder(texts(menuOf("Help")), {"Contents and Index", "What's This?", "Keyboard Shortcuts..."}, "Help");
        // what Reno cannot do is there, greyed, saying why
        QAction* slip = item("Edit/Editing Tool/Slip Tool");
        check(slip && slip->isEnabled() && slip->isCheckable(), "Edit > Editing Tool > Slip Tool is a working tool");
        QAction* script = item("Tools/Scripting/Rescan Script Menu Folder");
        check(script && script->isEnabled() && item("Tools/Scripting/Run Script...")->isEnabled(), "Tools > Scripting works");
        // Rescan Script Menu Folder finds a new script and forgets a removed one
        {
            QFile f(scriptDir + "/Gamma.cs");
            if (f.open(QIODevice::WriteOnly)) f.write("// test\n");
        }
        QFile::remove(scriptDir + "/Alpha Script.cs");
        trigger("Tools/Scripting/Rescan Script Menu Folder");
        const QStringList after = texts(menuOf("Tools/Scripting"));
        check(after.contains("Gamma") && !after.contains("Alpha Script") && after.count("Folder") == 1, "Rescan Script Menu Folder",
              after.join(" | ").toStdString());
        // no key is taken twice
        std::map<QString, QStringList> owners;
        for (QAction* a : w.findChildren<QAction*>()) {
            // a window's own keys (Edit Details' Ctrl+C / Ctrl+V) only work while that window has focus
            if (a->shortcutContext() == Qt::WidgetShortcut || a->shortcutContext() == Qt::WidgetWithChildrenShortcut) continue;
            for (const QKeySequence& k : a->shortcuts()) owners[k.toString()] << a->objectName();
        }
        std::string clashes;
        for (const auto& o : owners)
            if (o.second.size() > 1) clashes += o.first.toStdString() + " (" + o.second.join(", ").toStdString() + ") ";
        check(clashes.empty(), "no key belongs to two commands", clashes);
    }

    // ------------------------------------------------------------------ the timeline's right-click menus
    {
        QMenu m;
        const QRect v1 = tl->trackRect(ids.v1), a1 = tl->trackRect(ids.a1);
        const QPoint onVideo(tl->xAt(1.0), v1.center().y()), onAudio(tl->xAt(2.0), a1.center().y());
        QStringList t = contextAt(tl, onVideo, m);
        checkOrder(t, {"Open in Trimmer", "Create Nested Timeline", "Open Nested Timeline", "-", "Video Event Pan/Crop...", "Video Event FX...",
                       "Media FX...", "Edit in VEGAS Effects", "Motion Tracking", "-", "Cut", "Copy", "Paste Event Attributes", "Selectively Paste Event Attributes...", "-", "Delete", "Trim",
                       "Trim Start", "Trim End", "Split", "Smart Split", "Event Heal", "Close Gaps", "-", "Reverse", "-", "Select Events to End",
                       "Select All After Cursor", "Insert/Remove Envelope", "-", "Switches", "Take", "Group", "Properties..."},
                   "video event menu: VEGAS's items in its order");
        checkOrder(texts(subOf(m, "Switches")), {"Mute", "Lock", "Loop", "-", "Maintain Aspect Ratio", "Reduce Interlace Flicker", "-",
                                                 "Use Project Resample Mode", "Force Resample", "Disable Resample"}, "video event menu: Switches");
        checkOrder(texts(subOf(m, "Group")), {"Create New", "-", "Remove From", "Clear", "Select All", "-", "Cut All", "Copy All", "Delete All"},
                   "video event menu: Group");
        checkOrder(texts(subOf(m, "Take")), {"Rename Active", "Choose Active...", "-", "Next Take", "Previous Take", "-", "Delete Active", "Delete..."},
                   "video event menu: Take");
        t = contextAt(tl, onAudio, m);
        checkOrder(t, {"Open in Trimmer", "Open in Audio Editor", "-", "Audio Event FX...", "Apply Non-Real-Time Event FX...", "-", "Cut", "Copy", "Paste Event Attributes",
                       "Selectively Paste Event Attributes...", "-", "Delete", "Trim", "Trim Start", "Trim End", "Split", "Event Heal", "Close Gaps", "-",
                       "Reverse", "-", "Select Events to End", "Select All After Cursor", "-", "Switches", "Take", "Group", "Channels", "Properties..."},
                   "audio event menu: VEGAS's items in its order");
        checkOrder(texts(subOf(m, "Switches")), {"Mute", "Lock", "Loop", "Invert Phase", "Normalize"}, "audio event menu: Switches");
        checkOrder(texts(subOf(m, "Channels")), {"Both", "Left Only", "Right Only", "Combine", "Swap"}, "audio event menu: Channels");
        // track headers
        const QRect hv = tl->headerRect(ids.v1), ha = tl->headerRect(ids.a1);
        t = contextAt(tl, QPoint(hv.left() + 60, hv.bottom() - 8), m);
        checkOrder(t, {"Rename", "-", "Insert Video Track", "Duplicate Track", "Delete Track", "-", "Insert/Remove Envelope", "-", "Switches", "Fade Colors",
                       "-", "Track Group", "Track Display Color"}, "video track header menu: VEGAS's items in its order");
        checkOrder(texts(subOf(m, "Fade Colors")), {"Top...", "Bottom..."}, "video track header menu: Fade Colors");
        checkOrder(texts(subOf(m, "Switches")), {"Mute", "Solo"}, "video track header menu: Switches");
        // beside the pan slider: on it, the menu is VEGAS's pan types (checked below)
        const QRect haPan = tl->headerPartRect(ids.a1, TimelineWidget::PartPan);
        QPoint inA(ha.left() + 60, ha.bottom() - 8);
        if (haPan.adjusted(-8, -3, 8, 3).contains(inA)) inA.setY(haPan.top() - 6);
        t = contextAt(tl, inA, m);
        checkOrder(t, {"Rename", "-", "Insert Audio Track", "Duplicate Track", "Delete Track", "-", "Arm for Record", "Pan Type", "Insert/Remove Envelope", "-",
                       "Switches", "-", "Track Group", "Track Display Color"}, "audio track header menu: VEGAS's items in its order");
        checkOrder(texts(subOf(m, "Pan Type")), {"Add Channels (0 dB Center)", "Balance (0 dB Center)", "Balance (-3 dB Center)", "Balance (-6 dB Center)",
                                                 "Constant Power", "Film"}, "audio track header menu: Pan Type");
        if (!haPan.isEmpty()) {
            t = contextAt(tl, haPan.center(), m);
            checkOrder(t, {"Add Channels (0 dB Center)", "Balance (0 dB Center)", "Balance (-3 dB Center)", "Balance (-6 dB Center)", "Constant Power", "Film"},
                       "the pan slider's menu: VEGAS's pan types (its Pan Models menu)");
        }
        // the empty part of a track, inside and outside the time selection
        app.setSelection(std::set<uint64_t>());
        app.setLoop(5.5, 7.0);
        t = contextAt(tl, QPoint(tl->xAt(6.0), v1.center().y()), m);
        checkOrder(t, {"Insert Empty Event", "Zoom Selection", "-", "Paste", "-", "Delete", "Trim", "Trim Start", "Trim End", "Split", "Event Heal",
                       "Close Gaps", "-", "Select All After Cursor", "-", "Insert Video Track", "Duplicate Track", "Delete Track", "-",
                       "Insert/Remove Envelope", "-", "Fade Colors"}, "empty video track area in the time selection");
        t = contextAt(tl, QPoint(tl->xAt(9.0), a1.center().y()), m);
        check(!t.contains("Insert Empty Event") && t.contains("Insert Audio Track") && t.contains("Select All After Cursor"),
              "empty audio track area outside the time selection", t.join(" | ").toStdString());
        // the ruler area: the marker bar, a marker, the time selection's bar, the ruler
        t = contextAt(tl, QPoint(tl->xAt(9.0), tl->markerBarRect().center().y()), m);
        checkOrder(t, {"Loop Playback", "-", "Set Selection to View", "Set Selection to Project", "Set Selection to All", "Select Loop Region", "-",
                       "Markers/Regions", "-", "Quantize to Frames", "-", "Enable Snapping", "Snap to Grid", "Snap to Markers", "Snap to All Events", "-",
                       "Selectively Prerender Video...", "Clean Up Prerendered Video..."}, "marker bar menu (VEGAS's General)");
        checkOrder(texts(subOf(m, "Markers/Regions")), {"Insert Marker", "Insert Region", "-", "Delete All", "Delete All in Selection"},
                   "marker bar menu: Markers/Regions");
        const uint64_t marker = app.project().markers.empty() ? 0 : app.project().markers[0].id;
        t = contextAt(tl, tl->markerTagRect(marker).center(), m);
        checkOrder(t, {"Go To", "Rename", "-", "Delete"}, "marker menu");
        const QRect loopBar = tl->loopBarRect();
        t = contextAt(tl, QPoint(tl->xAt(6.2), loopBar.center().y()), m);
        checkOrder(t, {"Insert Region", "-", "Loop Playback", "-", "Zoom Selection", "Set Selection to View", "Set Selection to Project",
                       "Select Loop Region", "-", "Render to New Track...", "Selectively Prerender Video..."}, "time selection bar menu (VEGAS's Loop)");
        t = contextAt(tl, tl->rulerRect().center(), m);
        check(t.contains("Time Format"), "ruler menu: Time Format", t.join(" | ").toStdString());
        // a marker's Go To moves the cursor
        contextAt(tl, tl->markerTagRect(marker).center(), m);
        for (QAction* a : m.actions())
            if (plain(a->text()) == "Go To") a->trigger();
        check(near(app.cursor(), 4.0), "marker menu: Go To puts the cursor at the marker");
        // the right-click menu's Selectively Paste is the main window's command (greyed with nothing copied)
        contextAt(tl, onVideo, m);
        bool greyed = false;
        for (QAction* a : m.actions())
            if (plain(a->text()) == "Paste Event Attributes") greyed = !a->isEnabled();
        check(greyed, "event menu: Paste Event Attributes is greyed with nothing copied");
        uitest::shot(&w, out + "/menus_window.png");
        // pictures of the event menu and the Edit menu as they look
        {
            QMenu shown;
            contextAt(tl, onVideo, shown);
            shown.popup(tl->mapToGlobal(onVideo));
            uitest::settle(150);
            uitest::shot(&shown, out + "/menu_video_event.png");
            shown.close();
            QMenu* editMenu = item("Edit")->menu();
            editMenu->popup(w.mapToGlobal(QPoint(40, 20)));
            uitest::settle(150);
            uitest::shot(editMenu, out + "/menu_edit.png");
            editMenu->close();
        }
    }

    // ------------------------------------------------------------------ Paste Event Attributes / Selectively Paste
    {
        buildProject();
        const uint64_t src = evAt(ids.v1, 0)->id, dst = evAt(ids.v1, 1)->id, pic = evAt(ids.v2, 0)->id;
        app.edit("Set Up", [&](Project& p) {
            Event* e = p.findEvent(src);
            e->fadeIn = 0.5;
            e->fadeOutCurve = CurveFast;
            e->fadeOut = 0.25;
            e->gain = 0.6;
            e->reduceFlicker = true;
            PanCropKey k;
            k.w = 0.5;
            k.h = 0.5;
            e->panCrop.push_back(k);
            Envelope env;
            env.kind = "velocity";
            env.points.push_back(EnvPoint{0.0, 2.0, CurveLinear});
            e->envelopes.push_back(env);
        });
        app.selectOnly(src);
        app.copySelection();
        app.setSelection(std::set<uint64_t>{dst, pic});
        check(trigger("Edit/Paste Event Attributes"), "Edit > Paste Event Attributes is there and enabled");
        const Event* d = app.project().findEvent(dst);
        const Event* p2 = app.project().findEvent(pic);
        check(d && near(d->fadeIn, 0.5) && near(d->fadeOut, 0.25) && d->fadeOutCurve == CurveFast && near(d->gain, 0.6) && d->reduceFlicker &&
                  d->panCrop.size() == 1 && d->envelopes.size() == 1 && p2 && p2->panCrop.size() == 1,
              "Paste Event Attributes: fades, level, switches, pan/crop and envelopes go onto the selected events");
        check(app.undoName() == "Paste Event Attributes", "... in one undo step", app.undoName());
        app.undo();
        check(app.project().findEvent(dst)->fadeIn == 0 && app.project().findEvent(dst)->panCrop.empty(), "... which undoes");
        // selectively: only the fades
        app.setSelection(std::set<uint64_t>{dst});
        answer("selectivePasteDialog", [](QDialog* dlg) {
            for (QCheckBox* b : dlg->findChildren<QCheckBox*>())
                b->setChecked(b->text() == "Fade In" || b->text() == "Fade Out");
        });
        check(trigger("Edit/Selectively Paste Event Attributes..."), "Edit > Selectively Paste Event Attributes... is there");
        d = app.project().findEvent(dst);
        check(d && near(d->fadeIn, 0.5) && near(d->fadeOut, 0.25) && d->panCrop.empty() && !d->reduceFlicker && near(d->gain, 1.0),
              "Selectively Paste Event Attributes: only what was ticked (the fades)");
        // an audio event's attributes do not go onto video events
        const uint64_t aud = evAt(ids.a1, 0)->id;
        app.selectOnly(aud);
        app.copySelection();
        app.selectOnly(dst);
        const std::string before = app.undoName();
        app.pasteEventAttributes(app.selection(), AppState::AttrAll);
        check(app.undoName() == before, "an audio event's attributes are not pasted onto a video event");
    }

    // ------------------------------------------------------------------ Event Heal and Close Gaps
    {
        buildProject();
        const uint64_t first = evAt(ids.v1, 0)->id;
        app.selectOnly(evAt(ids.v1, 1)->id);
        app.setCursor(4.0);
        app.splitAtCursor();
        check(eventCount(ids.v1) == 3, "set up: the second event split in two");
        app.selectOnly(evAt(ids.v1, 2)->id);
        check(trigger("Edit/Event Heal"), "Edit > Event Heal is there");
        check(eventCount(ids.v1) == 2 && near(evAt(ids.v1, 1)->start, 3.0) && near(evAt(ids.v1, 1)->length, 2.0) && near(evAt(ids.v1, 1)->offset, 3.0),
              "Event Heal joins the split pieces back into one event",
              strfmt("%d events", (int)eventCount(ids.v1)));
        check(app.undoName() == "Event Heal", "... in one undo step", app.undoName());
        // events that do not follow on in their media stay apart
        app.selectOnly(first);
        const std::string before = app.undoName();
        app.healEvents(app.selection());
        check(eventCount(ids.v1) == 2 && app.undoName() == before, "Event Heal leaves events that do not follow on alone");
        // Close Gaps: the second event (3 s) moves back to where the first ends (2 s)
        app.selectOnly(evAt(ids.v1, 1)->id);
        check(trigger("Edit/Close Gaps"), "Edit > Close Gaps is there");
        check(near(evAt(ids.v1, 1)->start, 2.0), "Close Gaps moves the event back to the end of the one before it",
              strfmt("%.3f", evAt(ids.v1, 1)->start));
        app.undo();
        check(near(evAt(ids.v1, 1)->start, 3.0), "... and undoes");
    }

    // ------------------------------------------------------------------ selection commands
    {
        buildProject();
        app.selectOnly(evAt(ids.v1, 0)->id);
        check(trigger("Edit/Select/Select Events to End"), "Edit > Select > Select Events to End");
        check(app.selection().size() == 2 && app.selection().count(evAt(ids.v1, 1)->id), "Select Events to End: the event and the ones after it on its track");
        app.setCursor(3.5);
        check(trigger("Edit/Select/Select All After Cursor"), "Edit > Select > Select All After Cursor");
        check(app.selection().size() == 3 && !app.selection().count(evAt(ids.v1, 0)->id), "Select All After Cursor: what ends after the cursor",
              strfmt("%d", (int)app.selection().size()));
        check(trigger("Edit/Select/Set Selection to Project") && near(app.loopStart, 0.0) && near(app.loopEnd, app.project().length()),
              "Set Selection to Project");
        check(trigger("Edit/Select/Set Selection to View") && near(app.loopStart, std::max(0.0, tl->viewStartTime()), 1e-3) &&
                  near(app.loopEnd, tl->viewEndTime(), 1e-3), "Set Selection to View");
        app.setCursor(1.0);
        trigger("Edit/Select/Set Selection Start");
        app.setCursor(2.5);
        trigger("Edit/Select/Set Selection End");
        check(near(app.loopStart, 1.0) && near(app.loopEnd, 2.5), "Set Selection Start / End (I and O)", strfmt("%.3f %.3f", app.loopStart, app.loopEnd));
        app.setCursor(0.0);
        const bool tsr = trigger("Edit|Navigate|Time Select Right by Marker/Region");
        if (!tsr) std::printf("note: Time Select Right by Marker/Region not triggered\n");
        check(near(app.loopStart, 0.0) && near(app.loopEnd, 4.0), "Time Select Right by Marker/Region", strfmt("%.3f %.3f", app.loopStart, app.loopEnd));
        app.setLoop(0, 0);
        app.setCursor(2.0);
        trigger("Edit/Navigate/Move Right by Frame");
        check(near(app.cursor(), app.project().frameTime(app.project().frameAt(2.0) + 1)), "Navigate > Move Right by Frame");
        const double c = app.cursor();
        trigger("Edit/Navigate/Move Right by Grid Marks");
        check(app.cursor() > c, "Navigate > Move Right by Grid Marks moves the cursor on");
        trigger("Edit/Navigate/Go to Project Start");
        check(near(app.cursor(), 0.0), "Navigate > Go to Project Start");
        // Group > Cut All
        app.setSelection(std::set<uint64_t>{evAt(ids.v1, 0)->id, evAt(ids.a1, 0)->id});
        app.groupSelection(true);
        app.selectOnly(evAt(ids.v1, 0)->id);
        const size_t n = eventCount(ids.v1) + eventCount(ids.a1);
        check(trigger("Edit/Group/Cut All") && eventCount(ids.v1) + eventCount(ids.a1) == n - 2 && app.hasClipboard(),
              "Edit > Group > Cut All cuts the whole group");
    }

    // ------------------------------------------------------------------ switches and channels
    {
        buildProject();
        const uint64_t aud = evAt(ids.a1, 0)->id, vid = evAt(ids.v1, 0)->id;
        app.setSelection(std::set<uint64_t>{aud, vid});
        check(trigger("Edit/Switches/Invert Phase") && app.project().findEvent(aud)->invertPhase && app.undoName() == "Invert Phase",
              "Edit > Switches > Invert Phase (audio events)");
        check(item("Edit/Switches/Invert Phase")->isChecked(), "... and is ticked after");
        check(trigger("Edit/Switches/Reduce Interlace Flicker") && app.project().findEvent(vid)->reduceFlicker, "Edit > Switches > Reduce Interlace Flicker");
        check(trigger("Edit/Switches/Maintain Aspect Ratio") && !app.project().findEvent(vid)->keepAspect, "Edit > Switches > Maintain Aspect Ratio (off)");
        check(trigger("Edit/Switches/Normalize"), "Edit > Switches > Normalize");
        const Event* a = app.project().findEvent(aud);
        // the test sound's loudest sample is about 0.65: normalized to -0.1 dB it is turned up by about 1.5
        check(a && a->normalize && a->normalizeGain > 1.2 && a->normalizeGain < 2.0 && app.undoName() == "Normalize",
              "Normalize measures the sound and turns it up to -0.1 dB", a ? strfmt("gain %.3f", a->normalizeGain) : std::string());
        // the engine plays it louder by that much
        {
            MediaPool pool;
            pool.setOpener(app.mediaOpener);
            AudioEngine ae(pool, app.fx);
            const int nn = 4800;
            std::vector<float> l(nn), r(nn);
            Project p = app.project();
            ae.render(p, 48000, nn, l.data(), r.data(), true);
            double on = 0;
            for (float v : l) on = std::max(on, (double)std::fabs(v));
            p.findEvent(aud)->normalize = false;
            MediaPool pool2;
            pool2.setOpener(app.mediaOpener);
            AudioEngine ae2(pool2, app.fx);
            ae2.render(p, 48000, nn, l.data(), r.data(), true);
            double off = 0;
            for (float v : l) off = std::max(off, (double)std::fabs(v));
            check(off > 0 && near(on / off, a->normalizeGain, 0.05), "the mix plays a normalized event louder by its normalizing gain", strfmt("%.4f / %.4f", on, off));
        }
        check(trigger("Edit/Switches/Normalize") && !app.project().findEvent(aud)->normalize, "Normalize again turns it off");
        // Reduce Interlace Flicker changes the picture (a light vertical blur)
        {
            MediaPool pool;
            pool.setOpener(app.mediaOpener);
            VideoEngine ve(pool, app.fx);
            Project p = app.project();
            p.findEvent(vid)->keepAspect = true;
            FrameOptions fo;
            ImagePtr withBlur = ve.render(p, 0.5, fo);
            p.findEvent(vid)->reduceFlicker = false;
            MediaPool pool2;
            pool2.setOpener(app.mediaOpener);
            VideoEngine ve2(pool2, app.fx);
            ImagePtr plainPic = ve2.render(p, 0.5, fo);
            bool differ = false;
            if (withBlur && plainPic && withBlur->byteSize() == plainPic->byteSize())
                differ = std::memcmp(withBlur->data.get(), plainPic->data.get(), withBlur->byteSize()) != 0;
            check(differ, "Reduce Interlace Flicker changes the rendered picture");
        }
        app.selectOnly(aud);
        check(trigger("Edit/Channels/Swap") && app.project().findEvent(aud)->channels == "swap", "Edit > Channels > Swap");
        check(item("Edit/Channels/Swap")->isChecked() && !item("Edit/Channels/Both")->isChecked(), "... ticked in the menu");
        // .reno keeps the new switches
        {
            const QString path = out + "/switches.reno";
            app.setSelection(std::set<uint64_t>{aud});
            trigger("Edit/Switches/Normalize");
            const double g = app.project().findEvent(aud)->normalizeGain;
            std::string err;
            check(app.save(path.toStdString(), &err), "a project with the switches saves", err);
            check(app.open(path.toStdString(), &err), "and opens", err);
            const Event* x = evAt(ids.a1, 0);
            const Event* y = evAt(ids.v1, 0);
            check(x && x->normalize && near(x->normalizeGain, g, 1e-6) && y && y->reduceFlicker, "Normalize and Reduce Interlace Flicker are kept in a .reno");
        }
    }

    // ------------------------------------------------------------------ Undo All, Clear Edit History
    {
        buildProject();
        app.selectOnly(evAt(ids.v1, 0)->id);
        app.splitAtCursor();
        app.addTrack(TrackType::Audio);
        check(trigger("Edit/Undo All") && !app.canUndo() && app.project().tracks.empty(), "Edit > Undo All undoes every step");
        check(app.canRedo(), "... which can be redone");
        while (app.canRedo()) app.redo();
        QTimer::singleShot(150, [] {
            if (QMessageBox* mb = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) mb->button(QMessageBox::Yes)->click();
        });
        const bool cleared = trigger("Edit/Clear Edit History...");
        check(cleared && !app.canUndo() && !app.canRedo() && !app.project().tracks.empty(),
              "Edit > Clear Edit History empties undo and redo and keeps the project", strfmt("%d %d %d %d", cleared, app.canUndo(), app.canRedo(), (int)app.project().tracks.size()));
    }

    // ------------------------------------------------------------------ Insert > Time, Empty Event, Text; Duplicate Track; Clean Project Media
    {
        buildProject();
        app.setCursor(1.0);
        app.insertTime(1.0, 2.0);
        check(eventCount(ids.v1) == 3 && near(evAt(ids.v1, 1)->start, 3.0) && near(evAt(ids.v1, 2)->start, 5.0) && near(app.project().markers[0].time, 6.0),
              "Insert Time: an event across it is split, the rest and the markers move later", strfmt("%d", (int)eventCount(ids.v1)));
        app.undo();
        check(eventCount(ids.v1) == 2 && near(app.project().markers[0].time, 4.0), "... and undoes");
        QTimer::singleShot(150, [] {
            if (QInputDialog* d = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                d->setTextValue("1.5");
                d->accept();
            }
        });
        app.setCursor(5.0);
        check(trigger("Insert/Time...") && near(app.project().markers[0].time, 4.0) && app.undoName() == "Insert Time",
              "Insert > Time... (at the cursor; nothing after it but the end)");
        app.currentTrack = ids.v2;
        app.setLoop(6.0, 8.0);
        check(trigger("Insert/Empty Event") && eventCount(ids.v2) == 2 && evAt(ids.v2, 1)->mediaId == 0 && near(evAt(ids.v2, 1)->length, 2.0),
              "Insert > Empty Event over the time selection on the selected track");
        uitest::settle(100);
        uitest::shot(tl, out + "/empty_event.png");
        {
            MediaPool pool;
            pool.setOpener(app.mediaOpener);
            VideoEngine ve(pool, app.fx);
            check(ve.render(app.project(), 7.0, FrameOptions()) != nullptr, "an empty event renders (as nothing)");
        }
        const size_t tracks = app.project().tracks.size();
        const uint64_t made = app.duplicateTrack(ids.v1);
        const Track* copy = nullptr;
        for (const Track& t : app.project().tracks) if (t.id == made) copy = &t;
        check(copy && app.project().tracks.size() == tracks + 1 && copy->events.size() == eventCount(ids.v1) && copy->events[0].id != evAt(ids.v1, 0)->id &&
                  app.project().tracks[1].id == made, "Duplicate Track: a copy with new ids, under the original");
        std::string err;
        app.importMedia("synth:9", &err);
        const size_t media = app.project().media.size();
        QTimer::singleShot(150, [] {
            if (QMessageBox* mb = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) mb->button(QMessageBox::Yes)->click();
        });
        check(trigger("Tools/Clean Project Media") && app.project().media.size() == media - 1, "Tools > Clean Project Media removes unused media");
    }

    // ------------------------------------------------------------------ View: envelopes, minimize; Options: Ruler Format
    {
        buildProject();
        EnvelopeRef r;
        r.owner.kind = EnvelopeOwner::Track;
        r.owner.id = ids.a1;
        r.kind = "volume";
        app.insertEnvelope(r);
        check(item("View/Audio Envelopes/Track Volume")->isEnabled() && item("View/Audio Envelopes/Track Volume")->isChecked(),
              "View > Audio Envelopes > Track Volume is ticked with a volume envelope shown");
        trigger("View/Audio Envelopes/Track Volume");
        check(!AppState::findEnvelope(app.project(), r)->visible, "... and hides the track volume envelopes");
        check(!item("View/Video Envelopes/Event Velocity")->isEnabled(), "View > Video Envelopes > Event Velocity is greyed with none");
        check(trigger("Options/Ruler Format/Absolute Frames") && app.project().settings.rulerFormat == "frames" &&
                  item("Options/Ruler Format/Absolute Frames")->isChecked(), "Options > Ruler Format > Absolute Frames");
        // Options > Quickfade Audio Edits: on as in VEGAS (10 ms); off takes the quick fades away
        if (!app.prefs.quickFadeEdits) trigger("Options/Quickfade Audio Edits");  // an old settings folder may have it off
        check(item("Options/Quickfade Audio Edits")->isChecked() && app.prefs.quickFadeEdits &&
                  std::fabs(app.project().quickFade - app.prefs.quickFadeMs / 1000.0) < 1e-9 && app.project().quickFade > 0,
              "Options > Quickfade Audio Edits is on, its length from Preferences", strfmt("%g", app.project().quickFade));
        check(trigger("Options/Quickfade Audio Edits") && !app.prefs.quickFadeEdits && app.project().quickFade == 0.0, "... off: no quick fades");
        check(trigger("Options/Quickfade Audio Edits") && app.prefs.quickFadeEdits && app.project().quickFade > 0, "... and on again");
        const int h0 = tl->trackRect(ids.v1).height();
        trigger("View/Minimize All Tracks");
        uitest::settle(50);
        const int h1 = tl->trackRect(ids.v1).height();
        trigger("View/Minimize All Tracks");
        uitest::settle(50);
        check(h1 < h0 && tl->trackRect(ids.v1).height() == h0, "View > Minimize All Tracks, and back", strfmt("%d %d", h0, h1));
        // a script of the Script Menu runs (on Linux: the message that scripts need Windows)
        QString said;
        QTimer::singleShot(150, [&said] {
            if (QMessageBox* mb = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                said = mb->text();
                mb->accept();
            }
        });
        {
            // Reno's ScriptPortal.Vegas source beside the program, as the Windows build has it
            const QString from = QFileInfo(QString(__FILE__)).absolutePath() + "/../../src/scripting/", to = QCoreApplication::applicationDirPath() + "/scripting/";
            QDir().mkpath(to);
            for (const char* f : {"ScriptPortal.Vegas.cs", "ScriptPortal.Vegas.Unsupported.cs"}) {
                QFile::remove(to + f);
                QFile::copy(from + f, to + f);
            }
        }
        trigger("Tools/Scripting/Gamma");
        check(said.contains("needs Windows"), "Tools > Scripting > a script: on Linux, says running scripts needs Windows", said.toStdString());
    }

    return uitest::summary();
}
