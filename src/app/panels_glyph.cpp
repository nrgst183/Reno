// The small drawn images on the transport and tool bar buttons: see panels_glyph.h.
#include "panels_glyph.h"
#include <QPainterPath>

namespace reno {
namespace app {

void drawGlyph(QPainter& p, Glyph g, const QRectF& r, const QColor& fg)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c = r.center();
    p.setPen(Qt::NoPen);
    p.setBrush(fg);
    auto tri = [&](double x0, double y0, double x1, double y1, double x2, double y2) {
        QPainterPath path;
        path.moveTo(x0, y0);
        path.lineTo(x1, y1);
        path.lineTo(x2, y2);
        path.closeSubpath();
        p.drawPath(path);
    };
    const double cx = c.x(), cy = c.y();
    switch (g) {
    case Glyph::Record:
        p.drawRoundedRect(QRectF(cx - 3.0, cy - 7.0, 6.0, 9.5), 3.0, 3.0);
        p.drawRect(QRectF(cx - 0.75, cy + 3.5, 1.5, 3.0));
        p.drawRect(QRectF(cx - 3.0, cy + 6.0, 6.0, 1.5));
        break;
    case Glyph::Menu:
        for (int k = -1; k <= 1; ++k) p.drawRect(QRectF(cx - 6.0, cy + k * 4.0 - 1.0, 12.0, 2.2));
        break;
    case Glyph::Zoom: {
        QPen pen(fg, 1.8);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(cx - 6.0, cy - 6.5, 9.5, 9.5));
        p.drawLine(QPointF(cx + 2.2, cy + 1.8), QPointF(cx + 6.5, cy + 6.2));
        break;
    }
    case Glyph::Spacebar: {
        QPen pen(fg, 1.6);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath b;
        b.moveTo(cx - 6.5, cy - 2.5);
        b.lineTo(cx - 6.5, cy + 3.5);
        b.lineTo(cx + 6.5, cy + 3.5);
        b.lineTo(cx + 6.5, cy - 2.5);
        p.drawPath(b);
        break;
    }
    case Glyph::More:
        for (int k = -1; k <= 1; ++k) p.drawEllipse(QPointF(cx, cy + k * 4.5), 1.5, 1.5);
        break;
    case Glyph::Monitor:
        p.drawRect(QRectF(cx - 6.5, cy - 5.5, 13.0, 9.0));
        p.drawRect(QRectF(cx - 1.0, cy + 3.5, 2.0, 2.5));
        p.drawRect(QRectF(cx - 4.0, cy + 5.5, 8.0, 1.5));
        p.setBrush(QColor(45, 45, 45));
        p.drawRect(QRectF(cx - 5.0, cy - 4.0, 10.0, 6.0));
        break;
    case Glyph::Split:
        p.drawRect(QRectF(cx - 6.5, cy - 5.5, 13.0, 11.0));
        p.setBrush(QColor(45, 45, 45));
        p.drawRect(QRectF(cx + 0.5, cy - 4.0, 4.5, 8.0));
        break;
    case Glyph::Loop: {
        QPen pen(fg, 1.8);
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(cx - 6, cy - 5, 12, 10), 30 * 16, 300 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        tri(cx + 6.5, cy - 7.0, cx + 6.5, cy - 0.5, cx + 0.5, cy - 3.5);
        break;
    }
    case Glyph::PlayFromStart:
        p.drawRect(QRectF(cx - 6, cy - 5.5, 2, 11));
        tri(cx - 3, cy - 6, cx - 3, cy + 6, cx + 6, cy);
        break;
    case Glyph::Play: tri(cx - 4.5, cy - 6.5, cx - 4.5, cy + 6.5, cx + 6.5, cy); break;
    case Glyph::Pause:
        p.drawRect(QRectF(cx - 5, cy - 5.5, 3.5, 11));
        p.drawRect(QRectF(cx + 1.5, cy - 5.5, 3.5, 11));
        break;
    case Glyph::Stop: p.drawRect(QRectF(cx - 5, cy - 5, 10, 10)); break;
    case Glyph::GoToStart:
        p.drawRect(QRectF(cx - 5.5, cy - 6, 2.2, 12));
        tri(cx + 5, cy - 6, cx + 5, cy + 6, cx - 3, cy);
        break;
    case Glyph::GoToEnd:
        p.drawRect(QRectF(cx + 3.3, cy - 6, 2.2, 12));
        tri(cx - 5, cy - 6, cx - 5, cy + 6, cx + 3, cy);
        break;
    case Glyph::PrevFrame:
        tri(cx - 0.5, cy - 6, cx - 0.5, cy + 6, cx - 7, cy);
        p.drawRect(QRectF(cx + 1, cy - 6, 2, 12));
        p.drawRect(QRectF(cx + 4.5, cy - 6, 2, 12));
        break;
    case Glyph::NextFrame:
        p.drawRect(QRectF(cx - 6.5, cy - 6, 2, 12));
        p.drawRect(QRectF(cx - 3, cy - 6, 2, 12));
        tri(cx + 0.5, cy - 6, cx + 0.5, cy + 6, cx + 7, cy);
        break;
    case Glyph::Properties: {
        QPainterPath gear;
        for (int t = 0; t < 8; ++t) {
            const double a = t * 0.785398, w = 0.2;
            const QPointF o0(cx + 6.5 * std::cos(a - w), cy + 6.5 * std::sin(a - w)), o1(cx + 6.5 * std::cos(a + w), cy + 6.5 * std::sin(a + w));
            const QPointF i0(cx + 4.5 * std::cos(a + 0.33), cy + 4.5 * std::sin(a + 0.33)), i1(cx + 4.5 * std::cos(a + 0.45 + 0.33), cy + 4.5 * std::sin(a + 0.45 + 0.33));
            if (t == 0) gear.moveTo(o0);
            else gear.lineTo(o0);
            gear.lineTo(o1);
            gear.lineTo(i0);
            gear.lineTo(i1);
        }
        gear.closeSubpath();
        gear.addEllipse(c, 2.2, 2.2);
        p.drawPath(gear);
        break;
    }
    case Glyph::OutputFx: {
        QFont f = p.font();
        f.setFamily("serif");
        f.setPixelSize(15);
        f.setItalic(true);
        p.setFont(f);
        p.setPen(fg);
        p.drawText(r, Qt::AlignCenter, "fx");
        break;
    }
    case Glyph::Bypass: {
        QFont f = p.font();
        f.setPixelSize(11);
        f.setItalic(true);
        p.setFont(f);
        p.setPen(fg);
        p.drawText(r, Qt::AlignCenter, "fx");
        p.setPen(QPen(fg, 1.4));
        p.drawLine(QPointF(cx - 6, cy + 6), QPointF(cx + 6, cy - 6));
        break;
    }
    case Glyph::Quality: {
        p.setPen(QPen(fg, 1.1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(cx - 6, cy - 5, 12, 10));
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRect(QRectF(cx - 6, cy - 5, 6, 5));
        p.drawRect(QRectF(cx, cy, 3, 2.5));
        p.drawRect(QRectF(cx + 3, cy + 2.5, 3, 2.5));
        break;
    }
    case Glyph::Overlays:
        for (int k = 0; k < 4; ++k) {
            p.drawRect(QRectF(cx - 6 + k * 3.6, cy - 6, 1.4, 12));
            p.drawRect(QRectF(cx - 6, cy - 6 + k * 3.6, 12, 1.4));
        }
        break;
    case Glyph::Copy:
        p.drawRect(QRectF(cx - 6.5, cy - 6.5, 8, 10));
        p.drawRect(QRectF(cx - 1.5, cy - 2.5, 8, 9.5));
        break;
    case Glyph::Save: {
        QPainterPath disk;
        disk.addRect(QRectF(cx - 6.5, cy - 6.5, 13, 13));
        disk.addRect(QRectF(cx - 3.5, cy - 6.5, 7, 4.5));
        disk.addRect(QRectF(cx - 3.5, cy + 1.5, 7, 5));
        disk.setFillRule(Qt::OddEvenFill);
        p.drawPath(disk);
        break;
    }
    }
    p.restore();
}

GlyphButton::GlyphButton(Glyph g, const QString& tip, QWidget* parent, int w, int h) : QAbstractButton(parent), glyph_(g)
{
    setFixedSize(w, h);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(tip);
    setAccessibleName(tip);
}

void GlyphButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    // VEGAS's tool buttons: flat, bright blue square when on
    QColor bg(0, 0, 0, 0), edge(0, 0, 0, 0);
    if (isChecked()) { bg = QColor(18, 176, 255); edge = bg; }
    else if (isDown()) { bg = QColor(46, 143, 214); edge = bg; }
    else if (underMouse() && isEnabled()) { bg = QColor(90, 90, 90); edge = QColor(110, 110, 110); }
    p.setPen(QPen(edge, 1.0));
    p.setBrush(bg);
    p.drawRect(r);
    QColor fg = isEnabled() ? QColor(238, 238, 238) : QColor(104, 104, 104);
    if (glyph_ == Glyph::Record) fg = isEnabled() ? QColor(232, 140, 150) : QColor(132, 104, 108);
    if (glyph_ == Glyph::Play && playing_) fg = QColor(120, 220, 140);
    if (glyph_ == Glyph::Save && isEnabled()) fg = QColor(150, 202, 240);
    QRectF g = r;
    if (menuArrow) {
        g.setRight(g.right() - 6);
        QPainterPath a;
        a.moveTo(r.right() - 7, r.center().y() - 1);
        a.lineTo(r.right() - 2, r.center().y() - 1);
        a.lineTo(r.right() - 4.5, r.center().y() + 2);
        a.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawPath(a);
    }
    if (isEnabled() && glyph_ != Glyph::OutputFx && glyph_ != Glyph::Bypass)
        for (const QPointF& o : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) drawGlyph(p, glyph_, g.translated(o), QColor(28, 28, 28));
    drawGlyph(p, glyph_, g, fg);
}

void GlyphButton::setPlaying(bool on)
{
    if (playing_ == on) return;
    playing_ = on;
    update();
}

}  // namespace app
}  // namespace reno
