// Switching the main window between the desktop interface and the touch one (touch_shell.h).
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "timeline.h"
#include "touch_shell.h"
#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

namespace reno {
namespace app {

// View > Touch Interface: one of Reno's own items (after VEGAS's in the View menu).
void MainWindow::addTouchAction()
{
    QMenu* view = nullptr;
    for (QAction* a : menuBar()->actions())
        if (a->menu() && a->text().remove('&') == "View") view = a->menu();
    if (!view) return;
    QAction* t = view->addAction("Touch Interface");
    t->setObjectName("touchInterface");
    t->setCheckable(true);
    QObject::connect(view, &QMenu::aboutToShow, t, [this, t] { t->setChecked(touchMode()); });
    QObject::connect(t, &QAction::triggered, this, [this](bool on) {
        setTouchInterfaceWanted(on);
        // after the menu has closed
        QTimer::singleShot(0, this, [this, on] { setTouchMode(on); });
    });
}

void MainWindow::setTouchMode(bool on)
{
    if (on == (touch_ != nullptr)) return;
    if (on) {
        // the desktop window's parts are hidden (and noted, to come back as they were), the timeline area is taken
        // out, and the touch interface takes its place
        touchSavedState_ = saveState(kLayoutVersion);
        touchHidden_.clear();
        auto hideIt = [this](QWidget* w) {
            if (w && !w->isHidden()) {
                w->hide();
                touchHidden_.push_back(w);
            }
        };
        hideIt(menuBar());
        hideIt(statusBar());
        for (QToolBar* tb : findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly)) hideIt(tb);
        for (QDockWidget* dw : findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly)) hideIt(dw);
        desktopCentral_ = takeCentralWidget();
        if (desktopCentral_) desktopCentral_->hide();
        touch_ = new TouchShell(this);
        touch_->onLeave = [this] {
            setTouchInterfaceWanted(false);
            QTimer::singleShot(0, this, [this] { setTouchMode(false); });
        };
        setCentralWidget(touch_);
        touch_->show();
    } else {
        TouchShell* t = touch_;
        touch_ = nullptr;
        takeCentralWidget();
        delete t;               // (gives the Video Preview back to its docked window)
        if (desktopCentral_) {
            setCentralWidget(desktopCentral_);
            desktopCentral_->show();
        }
        desktopCentral_ = nullptr;
        for (QWidget* w : touchHidden_) w->show();
        touchHidden_.clear();
        if (!touchSavedState_.isEmpty()) restoreState(touchSavedState_, kLayoutVersion);
        touchSavedState_.clear();
        if (!startLayoutRestored_ && currentLayout_.isEmpty()) applyLayout(DefaultLayout);
        if (timeline_) timeline_->setFocus();
    }
    AppState::get().notify(kView);
}

}  // namespace app
}  // namespace reno
