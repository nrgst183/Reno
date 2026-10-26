// Reno - the editor.
#include "theme.h"
#include "../media/media.h"
#include "scripting.h"
#include "mainwindow.h"
#include "timeline.h"
#include "logo.h"
#include "recovery.h"
#include "builtintext.h"
#include "startup.h"
#include "../vegas/vegas.h"
#include "../engine/engine.h"
#include "prefsapply.h"
#include <QApplication>
#include <QFileInfo>
#include <QTimer>
#include <memory>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <objbase.h>
#  include <dwmapi.h>
#  include <QEvent>
#  include <QWidget>
#endif

using namespace reno;
using namespace reno::app;

#ifdef _WIN32
namespace {

// Windows draws title bars light unless each window is told it is dark. Every window is told just before it is first
// shown. Attribute 20 is
// DWMWA_USE_IMMERSIVE_DARK_MODE since Windows 10 20H1; 1809 to 1909 knew it as 19. On older Windows both fail and the
// title bar is unchanged.
void darkTitleBar(HWND hwnd)
{
    const BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &on, sizeof(on)))) DwmSetWindowAttribute(hwnd, 19, &on, sizeof(on));
}

class DarkTitleBars : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if ((e->type() == QEvent::Show || e->type() == QEvent::WinIdChange) && o->isWidgetType()) {
            QWidget* w = static_cast<QWidget*>(o);
            if (w->isWindow() && w->testAttribute(Qt::WA_WState_Created) && !(w->windowFlags() & Qt::FramelessWindowHint) &&
                w->windowType() != Qt::Popup && w->windowType() != Qt::ToolTip)
                darkTitleBar((HWND)w->winId());
        }
        return false;
    }
};

}  // namespace
#endif

int main(int argc, char** argv)
{
    // nested projects (.veg files used as media) are read with the VEGAS project reader
    reno::setProjectFileLoader([](const std::string& path, reno::Project& out, std::string* error) { return reno::loadProjectFile(path, out, error); });

#ifdef _WIN32
    // plug-ins use COM and expect windows to be made on a single-threaded apartment thread
    OleInitialize(nullptr);
    // sound goes out through Qt Multimedia's Windows backend (no FFmpeg needed)
    if (qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND")) qputenv("QT_MEDIA_BACKEND", "windows");
#endif
    // settings names; interface size and High DPI preferences are read before Qt starts
    QApplication::setOrganizationName("Reno");
    QApplication::setApplicationName("Reno");
    reno::app::prefsapply::earlyEnvironment();
    QApplication qapp(argc, argv);
    QApplication::setWindowIcon(reno::app::renoIcon());
    QApplication::setApplicationVersion(reno::app::renoVersion());   // logo on every window and in the task bar
    QApplication::setOrganizationName("Reno");
    QApplication::setApplicationName("Reno");
    applyTheme();
#ifdef _WIN32
    qapp.installEventFilter(new DarkTitleBars(&qapp));
#endif

    reno::app::installBuiltinTextRasterizer();     // built-in Titles & Text draws with Qt's fonts

    // this run's record for crash recovery (recovery.h)
    reno::app::session::begin();

    // Start as VEGAS does: splash screen while the plug-in list loads, new plug-ins are looked for, the project Reno was
    // started with is
    // opened (after a crash, VEGAS's question about the auto-saved project), then the main window (startup.h).
    StartupOptions start;
    const QStringList args = QApplication::arguments();
    // VEGAS's command line (/NOLOGO, /OPEN, /RUNSCRIPT, /SCRIPTARGS, /CMDMODULE, /OPENPRJ, /COLORS; scripting.h), or a file alone
    std::vector<std::string> argList;
    for (const QString& a : args) argList.push_back(a.toStdString());
    const reno::app::CommandLine cmd = reno::app::parseCommandLine(argList);
    if (!cmd.open.empty() && QFileInfo::exists(QString::fromStdString(cmd.open))) start.openPath = QString::fromStdString(cmd.open);
    if (!cmd.openPrj.empty()) {
        const std::string project = reno::mediaProjectReference(cmd.openPrj);
        if (!project.empty() && QFileInfo::exists(QString::fromStdString(project))) start.openPath = QString::fromStdString(project);
    }
    start.showSplash = !cmd.noLogo;
    if (cmd.colors == 1) applyTheme(reno::app::UiTheme::Vegas);
    start.makeWindow = [] {
        // the starting project is a new one as VEGAS makes it: 29.970 fps, or "Start all new projects with these settings"
        reno::app::AppState::get().newProject(true);
        MainWindow* w = new MainWindow();
#ifdef _WIN32
        ofx::Host::get().config.mainWindow = (void*)w->winId();
        ofx::Host::get().applyConfig();
#endif
        return w;
    };
    std::unique_ptr<MainWindow> w(runStartup(start));
    if (!w) return 1;
    if (!cmd.script.empty()) {
        // once the window is up, as VEGAS runs it
        MainWindow* mw = w.get();
        const std::string script = cmd.script, scriptArgs = cmd.scriptArgs;
        QTimer::singleShot(0, mw, [mw, script, scriptArgs] { reno::app::runScript(mw, script, scriptArgs); });
    }
    if (!cmd.cmdModule.empty())
        reno::app::AppState::get().status("VEGAS Pro extensions (/CMDMODULE " + cmd.cmdModule + ") cannot be loaded: Reno runs VEGAS scripts, not extensions.");
    const int rc = qapp.exec();
    // the window (with the playback engine, nested project effects and plug-in windows) goes now, while COM is still set
    // up: DirectX plug-ins and property pages are COM objects
    w.reset();
    reno::app::session::end();
#ifdef _WIN32
    OleUninitialize();
#endif
    return rc;
}
