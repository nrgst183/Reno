// Stand-in for src/app/fxwindow.cpp.
#include "../../../src/app/fxwindow.h"
namespace reno { namespace app {
struct FxWindow::Impl { FxTarget target; };
FxWindow::FxWindow(Playback*, QWidget* parent) : QWidget(parent), d(new Impl()) { setWindowTitle("Effects (stand-in)"); }
FxWindow::~FxWindow() {}
void FxWindow::showTarget(const FxTarget& t, uint64_t) { d->target = t; }
FxTarget FxWindow::target() const { return d->target; }
// (the stand-in has no Take FX mode)
void FxWindow::setOnlySlots(const std::vector<uint64_t>&, bool) {}
void FxWindow::closeEvent(QCloseEvent*) {}
void FxWindow::showEvent(QShowEvent*) {}
void FxWindow::hideEvent(QHideEvent*) {}
}}
