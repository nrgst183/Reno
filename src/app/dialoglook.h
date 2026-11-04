// The light dialog look (an option, off by default: Preferences > Display, "Use light dialogs like VEGAS"). VEGAS Pro 18 draws its older dialogs (Preferences, Customize Keyboard, Insert Time, About, Project
// Properties...) with the plain Windows 10 look: grey-white window, white boxes, flat grey buttons and combo boxes; the main
// window, the docked windows and the newer dialogs (Plug-In Chooser, Slideshow Creator, Advanced Save) are dark. Every
// top-level dialog Reno opens gets the light look when the option is on, unless makeDialogDark() was called on it before it
// was shown. With the option off (the default) every dialog keeps the dark scheme of the main window, same layout.
#pragma once
#include "vprefs.h"
#include <QApplication>
#include <QColor>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOption>
#include <QWidget>

namespace reno {
namespace app {

// The option "Use light dialogs like VEGAS" (QSettings "prefs/lightDialogs", false when never set; read when a dialog or page is made).
inline bool lightDialogsOn() { return vprefs::on("prefs/lightDialogs"); }

// Colours for the hand-drawn parts of effect pages and dialogs (graphs, curves, plot boxes), which VEGAS draws on a light
// face. With the option on they are as before; otherwise they follow the dark scheme of the main window. 'grey' in softInk()
// is the shade the light drawing used for a line on white: dark mirrors it (a faint line stays faint, a strong one strong).
inline QColor pageFace() { return lightDialogsOn() ? QColor(240, 240, 240) : QApplication::palette().color(QPalette::Window); }
inline QColor pagePlot() { return lightDialogsOn() ? QColor(255, 255, 255) : QApplication::palette().color(QPalette::Base).darker(130); }
inline QColor pageInk() { return lightDialogsOn() ? QColor(0, 0, 0) : QColor(222, 222, 222); }
inline QColor pageSoftInk(int grey) { return lightDialogsOn() ? QColor(grey, grey, grey) : QColor(255 - grey + 20, 255 - grey + 20, 255 - grey + 20); }
// A coloured curve: dark blue and the like lighten so they read on the dark plot.
inline QColor pageCurve(const QColor& c)
{
    if (lightDialogsOn()) return c;
    return c.lightness() < 150 ? c.lighter(c.lightness() < 60 ? 320 : 200) : c;
}

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
