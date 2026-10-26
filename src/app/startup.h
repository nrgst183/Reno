// Reno's start, as in VEGAS: a splash screen with the logo, a status line and a progress bar, shown before the main
// window while slow work is done: looking for new plug-ins (the first time, or when plug-in files changed), loading
// the plug-in list, creating the main window, and opening the project Reno was started with (command line, or the
// crashed run's).
//
// Plug-ins are created on the main thread (they make windows and use COM), so the window can't repaint while one
// starts; it is painted between plug-ins, and the line names the plug-in being waited on, so a slow or stuck plug-in
// shows on screen. The scan runs on its own thread (ScanTask, catalog.h); Skip lets Reno go on while it finishes in
// the background.
#pragma once
#include <QString>
#include <QImage>
#include <QWidget>
#include <functional>
#include <string>

class QPushButton;
class QTimer;
class QDialog;
class QLabel;
class QProgressBar;

namespace reno {
struct FxSlot;
namespace app {

class MainWindow;

// The splash screen: a dark window with Reno's logo, "Reno 1.0", a status line, a second line for what is being
// done (a plug-in's name), a progress bar and, while plug-ins are looked for, a Skip button.
class SplashScreen : public QWidget {
public:
    SplashScreen();
    ~SplashScreen() override;

    void setStatus(const QString& line, const QString& detail = QString());
    void setProgress(int done, int total);
    void showSkip(bool on);
    std::function<void()> onSkip;

    QString status() const { return line_; }
    QString detail() const { return detail_; }
    int done() const { return done_; }
    int total() const { return total_; }
    QString paintedStatus() const { return painted_; }
    int paints() const { return paints_; }
    QPushButton* skipButton() const { return skip_; }

    static SplashScreen* current();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    QString line_, detail_, painted_;
    int done_ = 0, total_ = 0, paints_ = 0, busyPhase_ = 0;
    QPushButton* skip_ = nullptr;
    QTimer* busy_ = nullptr;
    QImage logo_;
};

// While one of these lives, each plug-in instance the project's runtime creates (FxRuntime::onMaking) is named on the
// splash screen if it is up, otherwise in a small modal window over 'parent' that appears when plug-ins are slow (at
// once for a plug-in that is not Reno's own, else after 0.3 s). The event loop runs between plug-ins so the windows
// repaint. Main thread only; nesting is fine (the outer one shows).
class PluginLoadProgress {
public:
    PluginLoadProgress(QWidget* parent, const QString& what);
    ~PluginLoadProgress();
    PluginLoadProgress(const PluginLoadProgress&) = delete;
    PluginLoadProgress& operator=(const PluginLoadProgress&) = delete;

    int shown() const { return shown_; }
    QDialog* window() const { return dialog_; }

    static std::function<void(const QString& painted)>& beforePlugin();
    static QDialog* currentWindow();

private:
    void making(const FxSlot& slot, int done, int total);
    QWidget* parent_ = nullptr;
    QString what_;
    QDialog* dialog_ = nullptr;
    QLabel *title_ = nullptr, *line_ = nullptr;
    QProgressBar* bar_ = nullptr;
    long long startMs_ = 0;
    int shown_ = 0;
    bool outer_ = false;
};

// A plug-in slot as shown while it loads: "Slow Verb (VST)", and its file name in 'file'.
QString pluginLoadName(const FxSlot& slot, QString* file = nullptr);

struct StartupOptions {
    std::string probeExe;
    QString openPath;
    bool offerRecovery = true;
    std::function<MainWindow*()> makeWindow;
    std::function<void(SplashScreen*)> onSplash;
    bool showSplash = true;
};

// Reno's start-up (called from main.cpp): shows the splash screen, does the slow work behind it, creates the main
// window with makeWindow, opens the project, shows the window and closes the splash. Returns the main window (the
// caller owns it).
MainWindow* runStartup(const StartupOptions& options);

}  // namespace app
}  // namespace reno
