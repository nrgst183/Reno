// Preferences > Video > "VEGAS eFX audio effects" (prefs/vegasEfxSource): Reno's own versions of the MAGIX eFX effects (default)
// or the installed plug-ins. Set in the real dialog, saved with OK, read back, and the project's eFX slots rebuilt from the other
// source with settings kept (AppState::applyPreferences -> FxRuntime).
//
//   efxpref_test
//
// Build: uibuild <out> tests/ui/efxpref_test.cpp editdialogs src/app/prefsdialog.cpp src/app/midisync.cpp; QT_QPA_PLATFORM=offscreen.
#include "../../src/app/appstate.h"
#include "../../src/app/editdialogs.h"
#include "../../src/app/timeline.h"
#include "../../src/app/vprefs.h"
#include "../../src/audiofx/audioplugin.h"
#include "uitestlib.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

static void withPrefs(const std::function<void(QDialog*)>& fn)
{
    QTimer::singleShot(300, [fn] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!d) { check(false, "the Preferences dialog opened"); return; }
        fn(d);
        for (QPushButton* b : d->findChildren<QPushButton*>())
            if (b->text() == "OK") { b->click(); return; }
        d->reject();
    });
    runPreferences(nullptr);
}

static QComboBox* sourceCombo(QDialog* d) { return d->findChild<QComboBox*>("vegasEfxSource"); }

int main(int argc, char** argv)
{
    qputenv("HOME", "/tmp/efxpref_home");
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("efxpref");
    QSettings().clear();
    vprefs::reload();
    AppState& app = AppState::get();
    app.loadPreferences();
    check(vprefs::text("prefs/vegasEfxSource") == "builtin", "the default is Reno's own versions");
    check(efxPreferBuiltin(), "  and the audio host says so");

    const uint64_t slot = 7101;
    app.edit("Add eFX Limiter", [&](Project& p) {
        FxSlot s;
        s.id = slot;
        s.kind = "audio";
        s.name = "eFX Limiter";
        s.audio.format = "VST2";
        s.audio.uid = "1297625665";                 // 'MX2A'
        s.audio.name = "eFX Limiter";
        p.audioOut.push_back(s);
    });
    AudioPlugin* pl = app.fx.audio(slot);
    check(pl != nullptr && efxIsReproduction(pl->desc), "the slot is made as Reno's eFX Limiter, with no plug-in file installed");
    if (pl) pl->paramSet(5, 0.4f);

    withPrefs([](QDialog* d) {
        QComboBox* c = sourceCombo(d);
        check(c != nullptr, "Preferences > Video has the VEGAS eFX audio effects choice");
        if (!c) return;
        check(c->currentData().toString() == "builtin", "  it shows Reno's built-in versions");
        check(c->count() == 2, "  with two choices");
        c->setCurrentIndex(c->findData("real"));
    });
    uitest::settle(200);
    check(vprefs::text("prefs/vegasEfxSource") == "real", "OK saves it");
    check(!efxPreferBuiltin(), "  the audio host follows");
    check(app.fx.audio(slot) == nullptr && !app.fx.error(slot).empty(), "  the slot is made again from the installed plug-in: none here, so it says so");

    vprefs::reload();
    withPrefs([](QDialog* d) {
        QComboBox* c = sourceCombo(d);
        if (!c) { check(false, "the choice is there again"); return; }
        check(c->currentData().toString() == "real", "the dialog shows the saved choice");
        c->setCurrentIndex(c->findData("builtin"));
    });
    uitest::settle(200);
    pl = app.fx.audio(slot);
    check(efxPreferBuiltin() && pl && efxIsReproduction(pl->desc), "choosing Reno's again makes the slot again");
    check(pl && std::fabs(pl->paramGet(5) - 0.4f) < 1e-6f, "  with the setting changed before kept");
    return uitest::summary();
}
