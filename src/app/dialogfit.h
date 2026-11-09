// Dialogs placed by hand at a fixed size fit the screen and can be resized (dialogfit.cpp).
#pragma once

namespace reno {
namespace app {

// Called once at start (with installDialogLook): every fixed-size dialog shown from then on scrolls when the screen is
// smaller than it, keeps its buttons in view, and stretches its drop-down lists, boxes and pages when made larger. A dialog
// with the property "renoNoFit" set to true is left as it is.
void installDialogFit();

}  // namespace app
}  // namespace reno
