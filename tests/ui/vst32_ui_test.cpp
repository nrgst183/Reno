// The effects window with a 32-bit plug-in, on Linux: tests/vst32/bridgevst.cpp (a Linux VST2 plug-in) run through the bridge
// (RENO_VST32_FORCE=1 sends every VST2 plug-in through it; RENO_VST32_SERVER is the 64-bit build of reno_vst32). Built with
// -DRENO_FX_HOST_PLUGIN_WINDOWS so the window embeds the plug-in's own window, as on Windows.
//
//   vst32_ui_test <output folder> <reno_vst32> <bridgevst.so>
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/fxwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <unistd.h>

using namespace reno;
using namespace reno::app;
using uitest::check;

// the timeline stand-in this is linked with lacks it
void reno::app::TimelineWidget::keyReleaseEvent(QKeyEvent*) {}

namespace {
FxWindow* gWin = nullptr;
void settle(int ms = 150) { uitest::settle(ms); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

template <class T>
T* one(const char* name)
{
    for (T* w : gWin->findChildren<T*>(name))
        if (w->isVisibleTo(gWin)) return w;
    return nullptr;
}

std::vector<int> servers()
{
    std::vector<int> out;
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "pgrep -P %d reno_vst32", (int)getpid());
    FILE* f = popen(cmd, "r");
    int pid;
    while (f && std::fscanf(f, "%d", &pid) == 1) out.push_back(pid);
    if (f) pclose(f);
    return out;
}
}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("vst32_ui_test");
    const QStringList args = QApplication::arguments();
    if (args.size() < 4) { qWarning("usage: vst32_ui_test <output folder> <reno_vst32> <bridgevst.so>"); return 2; }
    const QString out = args[1];
    QDir().mkpath(out);
    setenv("RENO_VST32_SERVER", args[2].toUtf8().constData(), 1);
    setenv("RENO_VST32_FORCE", "1", 1);
    const std::string plug = args[3].toStdString();
    uitest::setup({}, {plug});

    AppState& app = AppState::get();
    app.newProject();
    AudioPluginDesc desc;
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.path == plug) desc = e.desc;
    check(desc.format == "VST2" && desc.bridged32, "the catalog lists the plug-in as a 32-bit, bridged one", desc.toJson().dump());

    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    gWin = w;
    w->resize(560, 600);
    FxTarget master;
    master.kind = FxTarget::AudioOut;
    const uint64_t T = app.addAudioFx(master, desc);
    w->showTarget(master, T);
    w->show();
    settle(400);

    AudioPlugin* pl = app.fx.audio(T);
    check(pl && pl->desc.bridged32 && app.fx.error(T).empty(), "the effect is made (through the bridge)", app.fx.error(T));
    check(servers().size() == 1, "one bridge process", std::to_string(servers().size()));
    QWidget* host = one<QWidget>("fxPluginWindow");
    check(host != nullptr, "the plug-in's window is put in the effects window");
    // the plug-in asks for 320x240 while it opens
    check(host && host->size() == QSize(320, 240), "and has the size the plug-in asked for",
          host ? std::to_string(host->width()) + "x" + std::to_string(host->height()) : "");
    uitest::shot(w, out + "/vst32_window.png");
    check(w->windowTitle() == "Master Bus FX", "title", w->windowTitle().toStdString());
    QLabel* name = one<QLabel>("fxTitleName");
    check(name && !name->text().contains("stopped"), "the title says nothing of stopping", name ? name->text().toStdString() : "no title");

    // playing; the plug-in's process dies (as if it had crashed)
    std::vector<float> l(256, 0.1f), r(256, 0.1f);
    if (pl) pl->process(l.data(), r.data(), 256, 0, true);
    for (int pid : servers()) kill(pid, SIGKILL);
    // the next sound block finds out
    if (pl) {
        pl->process(l.data(), r.data(), 256, 0, true);
        check(!pl->stoppedReason().empty(), "the plug-in says it stopped", pl->stoppedReason());
    }
    settle(500);
    name = one<QLabel>("fxTitleName");
    check(name && name->text().contains("stopped"), "the window's title says (stopped)", name ? name->text().toStdString() : "no title");
    check(name && !name->toolTip().isEmpty(), "with the reason as a tip", name ? name->toolTip().toStdString() : "");
    QLabel* note = nullptr;
    for (QLabel* lb : gWin->findChildren<QLabel*>())
        if (lb->isVisibleTo(gWin) && lb->text().contains("stopped:")) note = lb;
    check(note != nullptr, "and in place of the plug-in's window a note says why");
    uitest::shot(w, out + "/vst32_stopped.png");

    // the next play starts it again
    if (pl) {
        pl->process(l.data(), r.data(), 256, 256, false);
        pl->process(l.data(), r.data(), 256, 256, true);
        for (int k = 0; k < 100 && !pl->stoppedReason().empty(); ++k) settle(100);
        check(pl->stoppedReason().empty(), "started again by the next play");
    }
    settle(500);
    name = one<QLabel>("fxTitleName");
    check(name && !name->text().contains("stopped"), "the title is as before", name ? name->text().toStdString() : "no title");
    host = one<QWidget>("fxPluginWindow");
    check(host && host->size() == QSize(320, 240), "the plug-in's window is open again, in the new process",
          host ? std::to_string(host->width()) + "x" + std::to_string(host->height()) : "no window");
    check(servers().size() == 1, "one bridge process", std::to_string(servers().size()));
    uitest::shot(w, out + "/vst32_restarted.png");

    w->hide();
    settle();
    delete w;
    app.newProject();
    settle();
    return uitest::summary();
}
