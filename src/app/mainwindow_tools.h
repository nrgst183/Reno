// Windows and tools of the main window kept out of mainwindow.cpp (MainWindow::buildToolWindows): Trimmer (Alt+2), recording,
// Loudness Meters (Ctrl+Alt+7), Selective Prerender (Shift+M) and others listed in MainWindow::Tools.
#pragma once
#include "mainwindow.h"
#include <memory>

class QDockWidget;
class QLabel;

namespace reno {
namespace app {

class TrimmerWindow;
class Recorder;
class LoudnessWindow;
class TransportBar;
class PluginManager;
class Dashboard;
class DeviceExplorer;
class ExternalMonitor;
class ColorGradingPanel;
class MotionTrackingWindow;
class TextToSpeechWindow;
class MidiSync;
class SurroundPannerWindow;

struct MainWindow::Tools {
    TrimmerWindow* trimmer = nullptr;
    QDockWidget* trimmerDock = nullptr;
    std::unique_ptr<Recorder> recorder;
    QWidget* inputMeter = nullptr;              // in the status bar while a track is armed
    LoudnessWindow* loudness = nullptr;
    QDockWidget* loudnessDock = nullptr;
    SurroundPannerWindow* surround = nullptr;   // Ctrl+Alt+3
    QDockWidget* surroundDock = nullptr;
    PluginManager* plugins = nullptr;
    QDockWidget* pluginsDock = nullptr;
    Dashboard* dashboard = nullptr;
    QDockWidget* dashboardDock = nullptr;
    DeviceExplorer* devices = nullptr;
    QDockWidget* devicesDock = nullptr;
    ExternalMonitor* monitor = nullptr;         // window of its own, full screen on another display
    ColorGradingPanel* grading = nullptr;
    QDockWidget* gradingDock = nullptr;
    std::function<void()> colorGradingLayout;   // Alt+D, C
    MotionTrackingWindow* tracking = nullptr;
    QDockWidget* trackingDock = nullptr;
    TextToSpeechWindow* speech = nullptr;
    QDockWidget* speechDock = nullptr;
    std::unique_ptr<MidiSync> midi;
    TransportBar* transport = nullptr;
    int listener = 0;
    ~Tools();

    // Shift+M: prerenders the time selection (with a progress window). False if there was none or it failed.
    std::function<bool()> prerender;
    // Ctrl+R: starts or stops recording.
    std::function<void()> toggleRecord;
};

}  // namespace app
}  // namespace reno
