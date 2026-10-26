// Builds a small project out of made-up media, shows the main window without a screen and saves pictures of it, to look at the UI
// on a machine with no display.
//
//   uishot <output folder> [testfx.ofx] [more .ofx files ...] [--vst file ...]
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/fxwindow.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>

using namespace reno;
using namespace reno::app;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("uishot");
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: uishot <output folder> [file.ofx ...] [--vst file ...]"); return 2; }
    const QString out = args[1];
    QDir().mkpath(out);
    std::vector<std::string> ofxFiles, vstFiles;
    bool vst = false;
    for (int k = 2; k < args.size(); ++k) {
        if (args[k] == "--vst") { vst = true; continue; }
        (vst ? vstFiles : ofxFiles).push_back(args[k].toStdString());
    }
    uitest::setup(ofxFiles, vstFiles);
    uitest::buildDemoProject();

    MainWindow w;
    w.resize(1600, 900);
    w.show();
    w.timeline()->zoomToFit();
    AppState::get().setCursor(2.5);
    uitest::settle(400);
    uitest::shot(&w, out + "/main.png");

    // the effects window on the first event that has effects
    const Project& p = AppState::get().project();
    for (const auto& t : p.tracks)
        for (const auto& e : t.events)
            if (!e.fx.empty()) {
                FxTarget target;
                target.kind = FxTarget::Event;
                target.id = e.id;
                w.openFx(target, e.fx[0].id);
                uitest::settle(300);
                uitest::shot(w.fxWindow(), out + "/fxwindow.png");
                w.fxWindow()->hide();
                return 0;
            }
    return 0;
}
