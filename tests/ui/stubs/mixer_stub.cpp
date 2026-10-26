// Stand-in for src/app/mixer.cpp.
#include "../../../src/app/mixer.h"
#include <QLabel>
#include <QVBoxLayout>
namespace reno { namespace app {
struct MixerWindow::Impl {};
MixerWindow::MixerWindow(Playback*, QWidget* parent) : QWidget(parent), d(new Impl()) { (new QVBoxLayout(this))->addWidget(new QLabel("mixing console (stand-in)")); }
MixerWindow::~MixerWindow() {}
bool MixerWindow::handleKey(int, Qt::KeyboardModifiers) { return false; }
bool MixerWindow::event(QEvent* e) { return QWidget::event(e); }
void MixerWindow::keyPressEvent(QKeyEvent* e) { QWidget::keyPressEvent(e); }
bool MixerWindow::eventFilter(QObject* o, QEvent* e) { return QWidget::eventFilter(o, e); }
struct MasterBusWidget::Impl {};
MasterBusWidget::MasterBusWidget(Playback*, QWidget* parent) : QWidget(parent), d(new Impl()) { (new QVBoxLayout(this))->addWidget(new QLabel("master (stand-in)")); }
MasterBusWidget::~MasterBusWidget() {}
}}
