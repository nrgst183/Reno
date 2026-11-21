// VEGAS Pro 18's FX Automation Chooser (the arrow beside a track header's Track FX button, FX Automation...): a button for
// each effect in the track's chain and, for the one picked, a check box for each setting that can be automated. A ticked
// setting has a track envelope; OK adds the envelopes of the settings ticked and removes those of the ones cleared.
#pragma once
#include <cstdint>

class QDialog;
class QWidget;

namespace reno {
namespace app {

// VEGAS's FX Automation Chooser for a track's effects (the track header FX button's arrow menu). True if OK changed something.
bool runFxAutomationChooser(QWidget* parent, uint64_t trackId);
// For tests: the dialog itself, not shown.
QDialog* makeFxAutomationChooser(QWidget* parent, uint64_t trackId);

}  // namespace app
}  // namespace reno
