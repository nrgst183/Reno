// The touch interface for phones and tablets (Android and iOS builds only; touch_ui.h). It takes the
// main window's place while it is on: project bar at the top, the Video Preview, the transport, the touch timeline and
// a tool bar whose tools follow the selection. Tools open sheets with large controls. Everything else the desktop
// window can do stays reachable: the menu's "All commands" lists every command of its menus.
//
// It works on the same AppState, Playback and Video Preview as the desktop window (the preview is moved in while the
// touch interface is shown and back afterwards), so edits, undo and playback are shared.
#pragma once
#include "touch_ui.h"
#include <QWidget>
#include <functional>
#include <memory>

namespace reno {
namespace app {

class MainWindow;
class TouchTimeline;
namespace touch {
class Sheet;
}

class TouchShell : public QWidget {
public:
    explicit TouchShell(MainWindow* window);
    ~TouchShell() override;

    TouchTimeline* timeline() const;
    touch::Sheet* sheet() const;            // the one open (null: none)
    void closeSheet();
    void openSheet(touch::Sheet* s);        // made with this shell as parent; replaces any open one
    bool wide() const;                      // laid out for landscape (sheets at the side)
    bool tablet() const;

    // Tools by name, for tests: "media", "text", "split", "speed", ... (see the tool bar in touch_shell.cpp).
    void runTool(const QString& name);
    QStringList toolNames() const;          // the tools the tool bar shows now

    std::function<void()> onLeave;          // the menu's "Desktop interface"

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    friend struct TouchShellImpl;
    std::unique_ptr<struct TouchShellImpl> d;
};

// Whether Reno starts with the touch interface: unless the user switched to the desktop one (RENO_TOUCH=0 also says
// no). Always false in a build without it.
bool touchInterfaceWanted();
void setTouchInterfaceWanted(bool on);

}  // namespace app
}  // namespace reno
