// VST audio plug-ins on the master bus, every way they get there, with the whole main window, playing and not, stereo and 5.1.
//
//   masterfx_test <output folder> <strictvst.so> <strictvst6.so> <strictvstu.so> [more VST2 plug-ins...]
//
// strictvst*.so are tests/ui/strictvst.cpp built with -DSTRICTVST_SURROUND and -DSTRICTVST_UNTIDY: plug-ins that count every
// time the host breaks VST's rules (sound through a plug-in that is switched off or being changed, calls from the wrong thread,
// use after close), which is what crashes real plug-ins, on Windows above all. With -DRENO_FX_HOST_PLUGIN_WINDOWS (for fxwindow.cpp)
// the effects window also opens the plug-ins' own windows. Run under ASan+UBSan. Prints PASS / FAIL lines and a summary.
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/mixer.h"
#include "../../src/app/playback.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/core/dynlib.h"
#include "../../src/engine/catalog.h"
#include "../../src/engine/engine.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <atomic>
#include <chrono>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

// The strict plug-ins' counters (read through the files; each was loaded once and stays loaded).
struct Strict {
    DynLib lib;
    int (*violations)() = nullptr;
    int (*processed)() = nullptr;
    const char* (*last)() = nullptr;
    bool open(const std::string& path)
    {
        if (!lib.open(path, nullptr)) return false;
        violations = (int (*)())lib.symbol("strictvst_violations");
        processed = (int (*)())lib.symbol("strictvst_processed");
        last = (const char* (*)())lib.symbol("strictvst_last");
        return violations && processed && last;
    }
    int bad() const { return violations ? violations() : -1; }
    int blocks() const { return processed ? processed() : -1; }
    std::string why() const { return last ? std::string(last()) : std::string(); }
};
Strict gStereo, gSurround, gUntidy;

std::string rules()
{
    return strfmt("stereo %d broken (%s), surround %d broken (%s), untidy %d broken (%s)", gStereo.bad(), gStereo.why().c_str(), gSurround.bad(),
                  gSurround.why().c_str(), gUntidy.bad(), gUntidy.why().c_str());
}
bool rulesKept() { return gStereo.bad() == 0 && gSurround.bad() == 0 && gUntidy.bad() == 0; }

void onNextChooser(std::function<void(PluginChooser*)> drive)
{
    PluginChooser::onShown = [drive](PluginChooser* c) {
        const std::function<void(PluginChooser*)> run = drive;
        PluginChooser::onShown = nullptr;
        run(c);
    };
}

// Picks plug-ins in the chooser that comes up, then OK.
void chooseOnNext(const QStringList& labels, QString* title = nullptr, bool* shown = nullptr)
{
    onNextChooser([labels, title, shown](PluginChooser* c) {
        if (title) *title = c->windowTitle();
        if (shown) *shown = true;
        c->openFolder("All");
        for (const QString& l : labels) c->addByLabel(l);
        QPushButton* ok = c->findChild<QPushButton*>("chooserOk");
        if (ok) ok->click();
        else c->accept();
    });
}

QStringList names(const FxChain& c)
{
    QStringList out;
    for (const FxSlot& s : c) out << QString::fromStdString(s.name);
    return out;
}
std::string str(const QStringList& l) { return l.join(", ").toStdString(); }

bool allLive(const FxChain& c)
{
    for (const FxSlot& s : c)
        if (s.kind == "audio" && !AppState::get().fx.audio(s.id)) return false;
    return !c.empty();
}

void clearMaster(const char* what = "Clear Master FX")
{
    AppState::get().edit(what, [](Project& p) { p.audioOut.clear(); });
}

void setSurround(bool on)
{
    AppState::get().edit(on ? "5.1 Surround" : "Stereo", [on](Project& p) { p.settings.audioChannels = on ? 6 : 2; });
}

// Lets the UI run while sound plays, for 'ms'.
void playFor(int ms) { uitest::settle(ms); }

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMasterFxTest");
    QApplication::setApplicationName("masterfx_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 5) {
        std::fprintf(stderr, "usage: masterfx_test <output folder> <strictvst.so> <strictvst6.so> <strictvstu.so> [more VST2 plug-ins]\n");
        return 2;
    }
    gOut = args[1];
    QDir().mkpath(gOut);
    std::vector<std::string> vsts;
    for (int k = 2; k < args.size(); ++k) vsts.push_back(args[k].toStdString());
    uitest::setup(std::vector<std::string>(), vsts);
    check(gStereo.open(vsts[0]) && gSurround.open(vsts[1]) && gUntidy.open(vsts[2]), "the strict test plug-ins load");
    AppState& app = AppState::get();
    QStringList audioNames;
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.ok) audioNames << QString::fromStdString(e.desc.name);
    check(audioNames.contains("Strict Stereo") && audioNames.contains("Strict Surround") && audioNames.contains("Strict Untidy"), "the catalog has them", str(audioNames));
    QString other;
    for (const QString& n : audioNames)
        if (!n.startsWith("Strict")) { other = n; break; }

    uitest::buildDemoProject();
    MainWindow w;
    w.resize(1500, 900);
    w.show();
    uitest::settle(300);
    FxWindow* fxw = w.fxWindow();
    Playback* pb = w.playback();
    FxTarget master;
    master.kind = FxTarget::AudioOut;

    // ---------------------------------------------------------------- the Master Bus window's FX button
    QToolButton* masterBtn = w.findChild<QToolButton*>("masterFx");
    check(masterBtn != nullptr, "the Master Bus window's FX button");
    QString title;
    bool shown = false;
    chooseOnNext({"Strict Stereo", "Strict Untidy"}, &title, &shown);
    if (masterBtn) masterBtn->click();
    uitest::settle(300);
    check(shown && title == "Plug-In Chooser - Master Bus FX", "Master Bus FX: the chooser, named for the master", title.toStdString());
    check(names(app.project().audioOut) == QStringList({"Strict Stereo", "Strict Untidy"}) && allLive(app.project().audioOut),
          "Master Bus FX: the VSTs are on the master and made (one whose effect holds rubbish where the host's fields are)",
          str(names(app.project().audioOut)));
    check(fxw->isVisible() && fxw->target() == master, "Master Bus FX: then the effects window for the master");
    uitest::shot(fxw, gOut + "/master_fxwindow.png");
    pb->play();
    playFor(600);
    check(gStereo.blocks() > 0, "playing: the master's VST processes", strfmt("%d blocks", gStereo.blocks()));
    pb->stop();
    uitest::settle(100);
    check(rulesKept(), "the master's VST: VST's rules kept", rules());

    // ---------------------------------------------------------------- while playing: through the mixer's master strip
    clearMaster();
    fxw->hide();
    pb->play();
    playFor(300);
    QWidget* strip = w.findChild<QWidget*>("strip:master");
    QToolButton* stripFx = strip ? strip->findChild<QToolButton*>("fx") : nullptr;
    check(stripFx != nullptr, "the Mixing Console's master strip FX button");
    shown = false;
    chooseOnNext({"Strict Stereo", "Strict Surround"}, &title, &shown);
    if (stripFx) stripFx->click();
    playFor(500);
    check(shown && names(app.project().audioOut) == QStringList({"Strict Stereo", "Strict Surround"}) && allLive(app.project().audioOut),
          "mixer master strip, while playing: two VSTs on the master", str(names(app.project().audioOut)));
    check(pb->playing(), "...still playing");
    check(rulesKept(), "...VST's rules kept", rules());

    // ---------------------------------------------------------------- Tools > Audio > Master Bus FX (the menu command)
    clearMaster();
    shown = false;
    chooseOnNext({other.isEmpty() ? QString("Strict Stereo") : other}, &title, &shown);
    w.openFx(master, 0);
    playFor(400);
    check(shown && app.project().audioOut.size() == 1 && allLive(app.project().audioOut), "the master FX command while playing: the chooser, then the effect",
          str(names(app.project().audioOut)));
    check(rulesKept(), "...VST's rules kept", rules());

    // ---------------------------------------------------------------- the effects window's Plug-In Chain button, while playing
    if (QToolButton* chainBtn = fxw->findChild<QToolButton*>("fxPluginChain")) {
        chooseOnNext({"Strict Stereo", "Strict Stereo"});
        chainBtn->click();
        playFor(400);
        check(app.project().audioOut.size() == 3 && allLive(app.project().audioOut), "Plug-In Chain on the master while playing", str(names(app.project().audioOut)));
    } else {
        check(false, "the effects window's Plug-In Chain button");
    }
    check(rulesKept(), "...VST's rules kept", rules());

    // ---------------------------------------------------------------- undo and redo while playing
    for (int k = 0; k < 6; ++k) {
        app.undo();
        playFor(60);
        app.redo();
        playFor(60);
    }
    check(app.project().audioOut.size() == 3 && pb->playing(), "undo / redo of the master's chain while playing");
    check(rulesKept(), "...VST's rules kept", rules());

    // ---------------------------------------------------------------- the sample rate changed while playing
    // the plug-ins are started again at the new rate; the sound thread must not be in them meanwhile
    for (int k = 0; k < 4; ++k) {
        app.edit("Sample Rate", [k](Project& p) { p.settings.sampleRate = k % 2 ? 48000 : 44100; });
        playFor(150);
    }
    check(allLive(app.project().audioOut), "sample rate changes while playing: the master's plug-ins still there");
    check(rulesKept(), "...VST's rules kept (no sound through a plug-in being restarted)", rules());

    // ---------------------------------------------------------------- 5.1 while playing
    // the master's plug-ins are asked for six channels; the stereo one gets two more instances, for C/LFE and Ls/Rs
    setSurround(true);
    playFor(600);
    const FxChain mo = app.project().audioOut;
    bool sixOk = false, extrasOk = false;
    for (const FxSlot& s : mo) {
        AudioPlugin* pl = app.fx.audio(s.id);
        if (!pl) continue;
        if (s.name == "Strict Surround") sixOk = pl->channels() == 6;
        if (s.name == "Strict Stereo") extrasOk = pl->channels() == 2 && app.fx.audioExtra(s.id, 1) && app.fx.audioExtra(s.id, 2);
    }
    check(extrasOk, "5.1: a stereo VST on the master runs as three instances");
    check(rulesKept(), "5.1 while playing: VST's rules kept", rules());
    // the six-channel one
    chooseOnNext({"Strict Surround"});
    if (QToolButton* chainBtn = fxw->findChild<QToolButton*>("fxPluginChain")) chainBtn->click();
    playFor(500);
    for (const FxSlot& s : app.project().audioOut)
        if (s.name == "Strict Surround")
            if (AudioPlugin* pl = app.fx.audio(s.id)) sixOk = pl->channels() == 6;
    check(sixOk, "5.1: a VST that takes 5.1 processes the six channels as one");
    check(rulesKept(), "...VST's rules kept", rules());
    // master FX through the Master Bus window in 5.1, from empty
    clearMaster();
    shown = false;
    chooseOnNext({"Strict Stereo", "Strict Surround", other.isEmpty() ? QString("Strict Stereo") : other}, &title, &shown);
    if (masterBtn) masterBtn->click();
    playFor(600);
    check(shown && app.project().audioOut.size() == 3 && allLive(app.project().audioOut), "5.1, playing: Master Bus FX from empty", str(names(app.project().audioOut)));
    check(rulesKept(), "...VST's rules kept", rules());
    // stereo again, 5.1 again, rate changes, all while playing
    for (int k = 0; k < 4; ++k) {
        setSurround(k % 2 == 1);
        playFor(120);
        app.edit("Sample Rate", [k](Project& p) { p.settings.sampleRate = k % 2 ? 44100 : 48000; });
        playFor(120);
    }
    check(pb->playing() && allLive(app.project().audioOut), "stereo / 5.1 / rate changes while playing");
    check(rulesKept(), "...VST's rules kept", rules());

    // ---------------------------------------------------------------- an Audio FX double-click with no track: the master
    clearMaster();
    app.currentTrack = 0;
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.name == "Strict Stereo") {
            const uint64_t slot = app.addAudioFx(master, e.desc);
            w.openFx(master, slot);
        }
    playFor(400);
    check(allLive(app.project().audioOut) && fxw->target() == master, "an audio effect added straight onto the master while playing");
    check(rulesKept(), "...VST's rules kept", rules());

    // ---------------------------------------------------------------- the chain changing quickly while playing
    for (int k = 0; k < 10; ++k) {
        clearMaster("Clear");
        playFor(30);
        for (const AudioFxEntry& e : Catalog::get().audio())
            if (e.ok && !e.desc.isInstrument && (k % 2 ? e.desc.name == "Strict Surround" : e.desc.name == "Strict Stereo")) app.addAudioFx(master, e.desc);
        w.openFx(master, app.project().audioOut.empty() ? 0 : app.project().audioOut.back().id);
        playFor(40);
    }
    check(pb->playing() && allLive(app.project().audioOut), "adding and removing the master's VSTs quickly while playing");
    check(rulesKept(), "...VST's rules kept", rules());

    // ---------------------------------------------------------------- VST3 on the master (tests/vst3's plug-in), stereo and 5.1, playing
    QStringList vst3;
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.ok && e.desc.format == "VST3" && !e.desc.isInstrument) vst3 << QString::fromStdString(e.desc.name);
    if (!vst3.isEmpty()) {
        setSurround(false);
        clearMaster();
        shown = false;
        chooseOnNext(vst3, &title, &shown);
        if (masterBtn) masterBtn->click();
        playFor(500);
        check(shown && app.project().audioOut.size() == (size_t)vst3.size() && allLive(app.project().audioOut), "VST3 on the master through Master Bus FX, playing",
              str(names(app.project().audioOut)));
        setSurround(true);
        playFor(500);
        app.edit("Sample Rate", [](Project& p) { p.settings.sampleRate = 44100; });
        playFor(300);
        check(pb->playing() && allLive(app.project().audioOut), "...in 5.1 and at another sample rate");
        setSurround(false);
        app.edit("Sample Rate", [](Project& p) { p.settings.sampleRate = 48000; });
        playFor(300);
        check(pb->playing() && allLive(app.project().audioOut), "...stereo again");
        uitest::shot(fxw, gOut + "/master_vst3.png");
    }
    pb->stop();
    uitest::settle(200);
    uitest::shot(&w, gOut + "/main.png");

    // ---------------------------------------------------------------- a render (export) of a 5.1 project with master VSTs
    {
        const std::shared_ptr<const Project> snap = app.snapshot();
        MediaPool pool;
        pool.setOpener(app.mediaOpener);
        AudioEngine eng(pool, app.fx);
        std::vector<float> ch[6];
        float* io[6];
        for (int c = 0; c < 6; ++c) { ch[c].assign(4800, 0.f); io[c] = ch[c].data(); }
        for (int k = 0; k < 5; ++k) eng.renderChannels(*snap, k * 4800, 4800, io, false);
        check(rulesKept(), "an offline 5.1 render through the master's VSTs: rules kept", rules());
    }

    setSurround(false);
    clearMaster();
    uitest::settle(200);
    check(rulesKept(), "at the end: VST's rules kept", rules());
    return uitest::summary();
}
