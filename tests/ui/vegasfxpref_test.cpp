// Options > Preferences > Video > "VEGAS effects" (prefs/vegasFxSource): VEGAS's own plug-ins when installed, or always Reno's
// built-in versions. Set in the real dialog, saved with OK, read back, and the project's effect instances rebuilt from the other
// source with settings kept (AppState::applyPreferences -> FxRuntime).
//
//   vegasfxpref_test <standinfx.ofx>        (tests/ofx/standinfx.cpp: a plug-in with VEGAS's Invert identifier)
//
// Build: uibuild <out> tests/ui/vegasfxpref_test.cpp editdialogs src/app/prefsdialog.cpp src/app/midisync.cpp; QT_QPA_PLATFORM=offscreen.
#include "../../src/app/appstate.h"
#include "../../src/app/editdialogs.h"
#include "../../src/app/timeline.h"
#include "../../src/app/vprefs.h"
#include "../../src/ofx/builtins.h"
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

// the timeline stub in tests/ui/stubs lacks this one function
void reno::app::TimelineWidget::keyReleaseEvent(QKeyEvent*) {}

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

static QComboBox* sourceCombo(QDialog* d) { return d->findChild<QComboBox*>("vegasFxSource"); }

static bool builtinInstance(AppState& app, uint64_t slot)
{
    ofx::Instance* inst = app.fx.video(slot);
    return inst && inst->plugin && inst->plugin->binary && inst->plugin->binary->builtin;
}

int main(int argc, char** argv)
{
    qputenv("HOME", "/tmp/vegasfxpref_home");
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("vegasfxpref");
    QSettings().clear();
    vprefs::reload();
    if (argc < 2) { std::fprintf(stderr, "usage: vegasfxpref_test <standinfx.ofx>\n"); return 2; }
    ofx::Host& host = ofx::Host::get();
    host.loadBinary(ofx::kBuiltinBinaryPath, nullptr);
    check(host.loadBinary(argv[1], nullptr) != nullptr, "the stand-in plug-in loads");
    AppState& app = AppState::get();
    app.loadPreferences();
    check(vprefs::text("prefs/vegasFxSource") == "real", "the default is VEGAS's own plug-ins");
    check(!host.config.preferBuiltinVegasFx, "  and the host says so");

    const uint64_t slot = 7001;
    app.edit("Add Invert", [&](Project& p) {
        FxSlot s;
        s.id = slot;
        s.kind = "ofx";
        s.pluginId = "com.vegascreativesoftware:invert";
        s.name = "Invert";
        p.videoOut.push_back(s);
    });
    check(app.fx.video(slot) != nullptr && !builtinInstance(app, slot), "the effect is made from VEGAS's (stand-in) plug-in");
    if (ofx::Instance* inst = app.fx.video(slot)) inst->userSetValue(inst->params.find("BlendAmount"), std::vector<double>{0.25}, 0.0);

    withPrefs([](QDialog* d) {
        QComboBox* c = sourceCombo(d);
        check(c != nullptr, "Preferences > Video has the VEGAS effects choice");
        if (!c) return;
        check(c->currentData().toString() == "real", "  it shows VEGAS's own plug-ins");
        check(c->count() == 2, "  with two choices");
        c->setCurrentIndex(c->findData("builtin"));
    });
    uitest::settle(200);
    check(vprefs::text("prefs/vegasFxSource") == "builtin", "OK saves it");
    check(host.config.preferBuiltinVegasFx, "  the host follows");
    check(builtinInstance(app, slot), "  the project's effect is made again from the built-in");
    ofx::Instance* now = app.fx.video(slot);
    check(now && now->params.find("BlendAmount") && std::fabs(now->params.find("BlendAmount")->value[0] - 0.25) < 1e-9, "  with its setting kept");

    // a new run: the settings forgotten and read again
    vprefs::reload();
    withPrefs([](QDialog* d) {
        QComboBox* c = sourceCombo(d);
        if (!c) { check(false, "the choice is there again"); return; }
        check(c->currentData().toString() == "builtin", "the dialog shows the saved choice");
        c->setCurrentIndex(c->findData("real"));
    });
    uitest::settle(200);
    check(!host.config.preferBuiltinVegasFx && !builtinInstance(app, slot), "choosing VEGAS's again makes the effect from the stand-in again");
    now = app.fx.video(slot);
    check(now && now->params.find("BlendAmount") && std::fabs(now->params.find("BlendAmount")->value[0] - 0.25) < 1e-9, "  with its setting kept");
    return uitest::summary();
}
