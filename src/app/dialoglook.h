// The light dialog look. VEGAS Pro 18 draws its older dialogs (Preferences, Customize Keyboard, Insert Time, About, Project
// Properties...) with the plain Windows 10 look: grey-white window, white boxes, flat grey buttons and combo boxes; the main
// window, the docked windows and the newer dialogs (Plug-In Chooser, Slideshow Creator, Advanced Save) are dark. Every
// top-level dialog Reno opens gets the light look unless makeDialogDark() was called on it before it was shown.
#pragma once
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOption>
#include <QWidget>

namespace reno {
namespace app {

// Called once at start (after applyTheme): dialogs shown from then on are light unless they ask to stay dark.
void installDialogLook();

// Keeps this dialog (or window) in the dark scheme of the main window.
void makeDialogDark(QWidget* w);

// The palette of the light dialogs (Windows 10's colours).
QPalette lightDialogPalette();

// True when the palette is the light dialog one (window colour light).
bool isLightPalette(const QPalette& p);

// The style's helpers for light widgets: each draws the widget as Windows 10 does and returns true, or returns false for what
// the dark scheme / Fusion draw as before. RenoStyle (mainwindow.cpp) calls them first.
bool lightDrawPrimitive(const QStyle* style, QStyle::PrimitiveElement pe, const QStyleOption* o, QPainter* p, const QWidget* w);
bool lightDrawComplex(const QStyle* style, QStyle::ComplexControl cc, const QStyleOptionComplex* o, QPainter* p, const QWidget* w);

}  // namespace app
}  // namespace reno
