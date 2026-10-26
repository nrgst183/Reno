// The small drawn images on the transport and tool bar buttons, and the button that shows one.
#pragma once
#include <QAbstractButton>
#include <QColor>
#include <QPainter>
#include <QRectF>

namespace reno {
namespace app {

enum class Glyph {
    Record, Loop, PlayFromStart, Play, Pause, Stop, GoToStart, GoToEnd, PrevFrame, NextFrame,
    Properties, OutputFx, Bypass, Quality, Overlays, Copy, Save, Menu, Monitor, Split, Zoom, Spacebar, More
};

void drawGlyph(QPainter& p, Glyph g, const QRectF& r, const QColor& fg);

// A flat button with one of the glyphs, as on VEGAS's tool bars: no frame until the mouse is over it, a lit
// background while it is switched on.
class GlyphButton : public QAbstractButton {
public:
    GlyphButton(Glyph g, const QString& tip, QWidget* parent, int w = 26, int h = 24);
    bool menuArrow = false;             // a small arrow at the right: the button opens a menu
    bool boxed = false;                 // Reno's own look: drawn as a raised box even when the mouse is not over it
    void setPlaying(bool on);

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    Glyph glyph_;
    bool playing_ = false;
};

}  // namespace app
}  // namespace reno
