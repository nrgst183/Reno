// Editing keys in the real main window: the pitch keys (= / - / Ctrl / Shift, reset, the None method warning), Paste Event
// Attributes of a copied clip (picture and sound each onto their own kind), and Reno's built-in audio effects in the Audio FX lists
// and on a track.
//
//   editkeys_test <output folder> <testfx.ofx>
//
// Build: uibuild editkeys_test tests/ui/editkeys_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/keyboard.h"
#include "../../src/app/scripting.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/engine/catalog.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
MainWindow* gW = nullptr;
QString gOut;
std::string gStatus;

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

struct Ids { uint64_t v1 = 0, a1 = 0, a2 = 0, clip = 0, snd = 0, V = 0, S = 0, V2 = 0, S2 = 0; } ids;

// V1: V [2,3) grouped with A1: S [2,3) (the clip); V2 [5,6) and S2 [5,6) another pair; A2 empty
void buildProject()
{
    AppState& app = st();
    app.newProject();
    std::string err;
    ids.clip = app.importMedia("synth:1", &err);
    app.edit("Test Project", [&](Project& p) {
        auto track = [&](TrackType t, const char* name) {
            Track tr;
            tr.id = p.newId();
            tr.type = t;
            tr.name = name;
            if (t == TrackType::Audio) tr.panType = "add";
            p.tracks.push_back(tr);
            return tr.id;
        };
        ids.v1 = track(TrackType::Video, "V1");
        ids.a1 = track(TrackType::Audio, "A1");
        ids.a2 = track(TrackType::Audio, "A2");
        auto ev = [&](uint64_t t, double start, double len, double off, uint64_t group) {
            Event e;
            e.id = p.newId();
            e.mediaId = ids.clip;
            e.start = start;
            e.length = len;
            e.offset = off;
            e.group = group;
            p.findTrack(t)->events.push_back(e);
            return e.id;
        };
        ids.V = ev(ids.v1, 2.0, 1.0, 1.0, 50);
        ids.S = ev(ids.a1, 2.0, 1.0, 1.0, 50);
        ids.V2 = ev(ids.v1, 5.0, 1.0, 3.0, 51);
        ids.S2 = ev(ids.a1, 5.0, 1.0, 3.0, 51);
    });
    app.setCursor(0.0);
    app.setSelection(std::set<uint64_t>());
}

const Event* E(uint64_t id) { return st().project().findEvent(id); }

void testPitchKeys()
{
    AppState& app = st();
    buildProject();
    app.setSelection({ids.V, ids.S});
    auto act = [](const char* n) { if (QAction* a = gW->vegasAction(n)) a->trigger(); uitest::settle(10); };
    act("pitchUp");
    act("pitchUp");
    check(near(E(ids.S)->pitch, 2) && near(E(ids.V)->pitch, 0), "= twice: the sound two semitones up (the picture has no pitch)");
    act("pitchDownCent");
    check(near(E(ids.S)->pitch, 1.99), "Ctrl+-: a cent down");
    act("pitchUpOctave");
    check(near(E(ids.S)->pitch, 13.99), "Shift+=: an octave up");
    act("pitchReset");
    check(near(E(ids.S)->pitch, 0) && app.undoName() == "Reset Pitch", "Ctrl+Shift+=: reset");
    // the keys themselves
    bool keys = true;
    for (const auto& k : {std::make_pair("pitchUp", "="), std::make_pair("pitchDown", "-"), std::make_pair("pitchUpCent", "Ctrl+="),
                          std::make_pair("pitchDownOctave", "Shift+-"), std::make_pair("pitchReset", "Ctrl+Shift+=")}) {
        QAction* a = gW->vegasAction(k.first);
        if (!a || !a->shortcuts().contains(QKeySequence(k.second))) keys = false;
    }
    check(keys, "VEGAS's keys: = - Ctrl+= Shift+- Ctrl+Shift+=");
    // the None method: the status line says the pitch will not be heard
    app.edit("None", [](Project& p) { p.findEvent(ids.S)->stretchMethod = "none"; });
    gStatus.clear();
    act("pitchUp");
    check(gStatus.find("None") != std::string::npos, "with the None method the status line says why it is not heard", gStatus);
}

void testPasteAttributes()
{
    AppState& app = st();
    buildProject();
    // the first clip: reversed picture, sound pitched and reversed
    app.edit("Set", [](Project& p) {
        p.findEvent(ids.V)->reverse = true;
        p.findEvent(ids.S)->reverse = true;
        p.findEvent(ids.S)->pitch = 5;
        p.findEvent(ids.S)->gain = 0.5;
    });
    app.setSelection({ids.V, ids.S});
    app.copySelection();
    app.setSelection({ids.V2, ids.S2});
    uitest::settle(150);  // the menus' enabled states follow the selection and the clipboard a moment later
    QAction* a = gW->vegasAction("pasteAttributes");
    if (a) a->trigger();
    uitest::settle(20);
    check(E(ids.V2)->reverse && E(ids.S2)->reverse && near(E(ids.S2)->pitch, 5) && near(E(ids.S2)->gain, 0.5),
          "Paste Event Attributes of a copied clip: the picture's onto the picture, the sound's (pitch, reverse, volume) onto the sound",
          strfmt("%d %d %.2f %.2f (%s) enabled %d, '%s'", E(ids.V2)->reverse, E(ids.S2)->reverse, E(ids.S2)->pitch, E(ids.S2)->gain, gStatus.c_str(),
                 a ? a->isEnabled() : -1, app.undoName().c_str()));
    app.undo();
    // only the sound copied: onto the sound only
    app.setSelection({ids.S});
    app.copySelection();
    app.setSelection({ids.V2, ids.S2});
    uitest::settle(150);
    if (a) a->trigger();
    uitest::settle(20);
    check(!E(ids.V2)->reverse && near(E(ids.S2)->pitch, 5), "only a sound copied: the selected sound takes it, the picture stays");
}

void testBuiltinAudio()
{
    AppState& app = st();
    int listed = 0;
    AudioPluginDesc pitch;
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.format == "BUILTIN") {
            ++listed;
            if (e.desc.uid == "reno.pitchshift") pitch = e.desc;
        }
    // Reno's own effects and its VEGAS-identical versions of VEGAS's DirectX ones
    check(listed >= 9 && !pitch.uid.empty(), "the built-in audio effects are in the Audio FX lists", strfmt("%d", listed));
    buildProject();
    FxTarget t;
    t.kind = FxTarget::Track;
    t.id = ids.a1;
    const uint64_t slot = app.addAudioFx(t, pitch);
    uitest::settle(100);
    bool onTrack = false;
    for (const Track& x : app.project().tracks)
        if (x.id == ids.a1)
            for (const FxSlot& s : x.fx) if (s.id == slot && s.audio.format == "BUILTIN") onTrack = true;
    check(slot && onTrack, "Pitch Shift (built-in) goes on an audio track");
    AudioPlugin* pl = app.fx.audio(slot);
    check(pl && pl->paramCount() >= 5 && pl->programCount() > 0, "and is running, with its settings and presets");
    // its effects window: the sliders and the presets (as for any plug-in without a window of its own)
    gW->openFx(t, slot);
    uitest::settle(300);
    FxWindow* fw = gW->fxWindow();
    QComboBox* box = fw ? fw->findChild<QComboBox*>("fxPreset") : nullptr;
    check(box && pl && box->count() == pl->programCount(), "the effects window lists its presets", box ? strfmt("%d", box->count()) : "");
    if (fw) {
        QWidget* top = fw->window();
        uitest::shot(top, gOut + "/builtin_pitch_shift.png");
    }
    const int fifth = box ? box->findText("Fifth up") : -1;
    if (fifth >= 0) {
        box->setCurrentIndex(fifth);
        emit box->activated(fifth);
        uitest::settle(50);
        pl = app.fx.audio(slot);
        check(pl && pl->paramText(0).find('7') != std::string::npos, "its \"Fifth up\" preset sets +7 semitones", pl ? pl->paramText(0) : "");
    } else {
        check(false, "its presets include VEGAS's \"Fifth up\"");
    }
    pl = app.fx.audio(slot);
    if (pl) {
        app.fx.captureStates(const_cast<Project&>(app.project()));
        std::string before;
        for (const Track& x : app.project().tracks) for (const FxSlot& s : x.fx) if (s.id == slot) before = s.state.isString() ? s.state.asString() : s.state.dump();
        std::string err;
        const QString path = gOut + "/builtin_fx.reno";
        const bool saved = app.save(path.toStdString(), &err);
        Project q;
        const bool loaded = saved && q.load(path.toStdString(), &err);
        std::string state;
        for (const Track& x : q.tracks) for (const FxSlot& s : x.fx) if (s.id == slot) state = s.state.isString() ? s.state.asString() : s.state.dump();
        check(loaded && !state.empty() && state == before, "its settings are kept in the project", state.substr(0, 80));
    }
}

// Note placement: Grid Spacing in notes at the project's tempo, Snap to Grid - a dragged event lands on the beat
void testGridSnap()
{
    AppState& app = st();
    buildProject();
    TimelineWidget* tl = gW->timeline();
    app.edit("Tempo", [](Project& p) { p.settings.tempo = 150; });   // a beat = 0.4 s, a 16th = 0.1 s
    app.snapping = true;
    tl->options().snapToGrid = true;
    tl->options().gridSpacing = "16th";
    tl->optionsChanged();
    tl->zoomToFit();
    uitest::settle(100);
    // the clip's picture V [2,3): dragged right by about 0.43 s -> its start lands on 2.4 (a 16th grid line)
    const QRect r = tl->trackRect(ids.v1);
    const int y = r.top() + r.height() * 6 / 10;
    const double pps = (tl->xAt(10.0) - tl->xAt(0.0)) / 10.0;
    uitest::mouseDrag(tl, QPoint(tl->xAt(2.5), y), QPoint(tl->xAt(2.5) + (int)std::lround(pps * 0.43), y));
    uitest::settle(50);
    const double s1 = E(ids.V)->start;
    const double beats = s1 / 0.1;
    check(std::fabs(beats - std::round(beats)) < 1e-6 && s1 > 2.25 && s1 < 2.6, "a dragged event snaps to the 16th-note grid at 150 bpm", strfmt("%.4f", s1));
    tl->options().snapToGrid = false;
    tl->options().gridSpacing = "ruler";
    tl->optionsChanged();
}

// Script Menu items can have keys (Options > Customize Keyboard), kept when the menu is scanned again.
void testScriptKeys()
{
    QAction* sc = nullptr;
    for (QAction* a : gW->findChildren<QAction*>())
        if (a->objectName() == "Script.My Script") sc = a;
    check(sc != nullptr, "a Script Menu item has a command name (Script.<name>)");
    KeyMap map = windowKeyMap(gW);
    const int i = map.find("Script.My Script");
    check(i >= 0 && map.commands[(size_t)i].group.contains("Scripting"), "it is listed in Customize Keyboard under Tools > Scripting");
    if (i < 0) return;
    map.replace("Script.My Script", QKeySequence("Ctrl+Alt+Y"));
    applyKeyMap(gW, map);
    QSettings().setValue("keyboard/bindings", map.toIni("Mine"));
    check(sc->shortcuts().contains(QKeySequence("Ctrl+Alt+Y")), "it takes a key");
    if (QAction* rescan = gW->vegasAction("rescanScripts")) rescan->trigger();
    uitest::settle(200);
    QAction* again = nullptr;
    for (QAction* a : gW->findChildren<QAction*>())
        if (a->objectName() == "Script.My Script") again = a;
    check(again && again->shortcuts().contains(QKeySequence("Ctrl+Alt+Y")), "and keeps it when the Script Menu is scanned again");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoEditKeysTest");
    QApplication::setApplicationName("editkeys_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("editkeys");
    QDir().mkpath(gOut);
    std::vector<std::string> ofx;
    for (int i = 2; i < args.size(); ++i) ofx.push_back(args[i].toStdString());
    uitest::setup(ofx, {});
    // a script in Documents\Vegas Script Menu (for the keys test)
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/Vegas Script Menu";
    QDir().mkpath(docs);
    {
        QFile f(docs + "/My Script.cs");
        if (f.open(QIODevice::WriteOnly)) f.write("// (only listed, not run)\n");
    }

    buildProject();
    MainWindow w;
    gW = &w;
    w.resize(1400, 860);
    w.show();
    uitest::settle(300);
    auto previous = st().onStatus;
    st().onStatus = [previous](const std::string& s) { gStatus = s; if (previous) previous(s); };
    testPitchKeys();
    testPasteAttributes();
    testBuiltinAudio();
    testScriptKeys();
    testGridSnap();
    return uitest::summary();
}
