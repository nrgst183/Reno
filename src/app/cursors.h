// The timeline's mouse pointers, drawn by Reno in the style of VEGAS's (white with a black rim, drawn at screen scale so
// they stay sharp on high-DPI).
#pragma once
#include <QCursor>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <array>
#include <cmath>

namespace reno {
namespace app {

enum class EditCursor {
    TrimLeft, TrimRight,            // event's left or right edge
    StretchLeft, StretchRight,      // Ctrl: the edge stretches the media
    SlipTrimLeft, SlipTrimRight,    // Alt: the media moves with the edge
    FadeIn, FadeOut,                // a fade handle
    Slip,                           // Alt over an event: media slips inside it
    EnvLine, EnvPoint,              // envelope line (moved up and down), point (moved any way)
    Zoom, SelectTool, EnvelopeTool, // tools other than Normal, over the tracks
    Count
};

namespace cursor_detail {

// Draws 'path' as a pointer line: black rim round a white line.
inline void ink(QPainter& p, const QPainterPath& path, double width = 1.6)
{
    p.strokePath(path, QPen(Qt::black, width + 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.strokePath(path, QPen(Qt::white, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
}

// A filled white shape with a black rim.
inline void solid(QPainter& p, const QPainterPath& path)
{
    p.setPen(QPen(Qt::black, 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::white);
    p.drawPath(path);
}

// A small arrow head at 'tip' pointing along the unit step (dx, dy).
inline QPainterPath head(QPointF tip, double dx, double dy, double size = 4.0)
{
    QPainterPath h;
    const QPointF back(tip.x() - dx * size, tip.y() - dy * size), side(-dy * size * 0.85, dx * size * 0.85);
    h.moveTo(tip);
    h.lineTo(back + side);
    h.lineTo(back - side);
    h.closeSubpath();
    return h;
}

// A horizontal arrow from x0 to x1 at y, heads at both ends (or only x1).
inline void hArrow(QPainter& p, double x0, double x1, double y, bool both)
{
    QPainterPath line;
    const double dir = x1 > x0 ? 1.0 : -1.0;
    line.moveTo(both ? x0 + dir * 3.0 : x0, y);
    line.lineTo(x1 - dir * 3.0, y);
    ink(p, line, 1.4);
    solid(p, head(QPointF(x1, y), dir, 0.0));
    if (both) solid(p, head(QPointF(x0, y), -dir, 0.0));
}

inline void vArrow(QPainter& p, double x, double y0, double y1)
{
    QPainterPath line;
    line.moveTo(x, y0 + 3.0);
    line.lineTo(x, y1 - 3.0);
    ink(p, line, 1.4);
    solid(p, head(QPointF(x, y0), 0.0, -1.0));
    solid(p, head(QPointF(x, y1), 0.0, 1.0));
}

// The ordinary small arrow with its tip at (x, y); tools draw what they do beside it.
inline void arrow(QPainter& p, double x, double y)
{
    QPainterPath a;
    a.moveTo(x, y);
    a.lineTo(x, y + 14.0);
    a.lineTo(x + 3.5, y + 10.8);
    a.lineTo(x + 6.0, y + 16.5);
    a.lineTo(x + 8.2, y + 15.5);
    a.lineTo(x + 5.8, y + 10.0);
    a.lineTo(x + 10.0, y + 10.0);
    a.closeSubpath();
    solid(p, a);
}

// A bracket at x (the edge), arms reaching 'dir' towards the event; hot spot on its upright.
inline void bracket(QPainter& p, double x, double dir)
{
    QPainterPath b;
    b.moveTo(x + dir * 4.0, 7.0);
    b.lineTo(x, 7.0);
    b.lineTo(x, 25.0);
    b.lineTo(x + dir * 4.0, 25.0);
    ink(p, b, 1.8);
}

// A short strip of film centred at (cx, cy).
inline void film(QPainter& p, double cx, double cy, double w)
{
    QPainterPath f;
    f.addRect(QRectF(cx - w * 0.5, cy - 3.5, w, 7.0));
    solid(p, f);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (double x = cx - w * 0.5 + 2.0; x < cx + w * 0.5 - 1.0; x += 3.0) {
        p.drawRect(QRectF(x, cy - 2.5, 1.0, 1.0));
        p.drawRect(QRectF(x, cy + 1.5, 1.0, 1.0));
    }
}

inline QCursor make(EditCursor k)
{
    const double dpr = qGuiApp ? std::max(1.0, qGuiApp->devicePixelRatio()) : 1.0;
    QPixmap pm(QSize((int)std::ceil(32 * dpr), (int)std::ceil(32 * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPoint hot(16, 16);
    switch (k) {
    case EditCursor::TrimLeft:
    case EditCursor::TrimRight:
    case EditCursor::StretchLeft:
    case EditCursor::StretchRight:
    case EditCursor::SlipTrimLeft:
    case EditCursor::SlipTrimRight: {
        // bracket on the edge opening into the event; the drag's action on its outside
        const bool left = k == EditCursor::TrimLeft || k == EditCursor::StretchLeft || k == EditCursor::SlipTrimLeft;
        const double dir = left ? 1.0 : -1.0;
        bracket(p, 16.0, dir);
        if (k == EditCursor::TrimLeft || k == EditCursor::TrimRight) {
            hArrow(p, 16.0 - dir * 2.5, 16.0 - dir * 13.0, 16.0, false);
            hArrow(p, 16.0 + dir * 2.5, 16.0 + dir * 11.0, 16.0, false);
        } else if (k == EditCursor::StretchLeft || k == EditCursor::StretchRight) {
            QPainterPath z;
            z.moveTo(16.0 - dir * 3.0, 16.0);
            for (int i = 1; i <= 5; ++i) z.lineTo(16.0 - dir * (3.0 + i * 2.2), 16.0 + (i % 2 ? -3.5 : 3.5));
            ink(p, z, 1.4);
            hArrow(p, 16.0 + dir * 2.5, 16.0 + dir * 11.0, 16.0, false);
        } else {
            film(p, 16.0 + dir * 7.5, 16.0, 11.0);
            hArrow(p, 16.0 - dir * 2.5, 16.0 - dir * 13.0, 16.0, false);
        }
        break;
    }
    case EditCursor::FadeIn:
    case EditCursor::FadeOut: {
        // fade slope under a two-way arrow; hot spot at the top where the handle is
        const bool in = k == EditCursor::FadeIn;
        QPainterPath s;
        if (in) {
            s.moveTo(7.0, 27.0);
            s.cubicTo(13.0, 26.0, 19.0, 21.0, 25.0, 13.0);
        } else {
            s.moveTo(7.0, 13.0);
            s.cubicTo(13.0, 21.0, 19.0, 26.0, 25.0, 27.0);
        }
        ink(p, s, 1.6);
        hArrow(p, 6.0, 26.0, 7.0, true);
        hot = QPoint(16, 7);
        break;
    }
    case EditCursor::Slip:
        film(p, 16.0, 16.0, 14.0);
        hArrow(p, 8.0, 2.0, 16.0, false);
        hArrow(p, 24.0, 30.0, 16.0, false);
        break;
    case EditCursor::EnvLine: {
        QPainterPath l;
        l.moveTo(6.0, 16.0);
        l.lineTo(26.0, 16.0);
        ink(p, l, 1.2);
        vArrow(p, 16.0, 4.0, 28.0);
        break;
    }
    case EditCursor::EnvPoint: {
        vArrow(p, 16.0, 4.0, 28.0);
        hArrow(p, 4.0, 28.0, 16.0, true);
        QPainterPath b;
        b.addRect(QRectF(13.0, 13.0, 6.0, 6.0));
        solid(p, b);
        break;
    }
    case EditCursor::Zoom: {
        QPainterPath ring, handle;
        ring.addEllipse(QPointF(13.0, 13.0), 7.5, 7.5);
        handle.moveTo(18.5, 18.5);
        handle.lineTo(26.0, 26.0);
        p.strokePath(handle, QPen(Qt::black, 5.0, Qt::SolidLine, Qt::RoundCap));
        p.strokePath(handle, QPen(Qt::white, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.setPen(QPen(Qt::black, 1.0));
        p.setBrush(QColor(255, 255, 255, 210));
        p.drawEllipse(QPointF(13.0, 13.0), 8.0, 8.0);
        QPainterPath plus;
        plus.moveTo(9.5, 13.0);
        plus.lineTo(16.5, 13.0);
        plus.moveTo(13.0, 9.5);
        plus.lineTo(13.0, 16.5);
        p.strokePath(plus, QPen(Qt::black, 1.6));
        hot = QPoint(13, 13);
        break;
    }
    case EditCursor::SelectTool: {
        arrow(p, 2.0, 2.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::black, 1.0));
        p.drawRect(QRectF(13.5, 15.5, 14.0, 10.0));
        p.setPen(QPen(Qt::white, 1.0, Qt::DashLine));
        p.drawRect(QRectF(13.5, 15.5, 14.0, 10.0));
        hot = QPoint(2, 2);
        break;
    }
    case EditCursor::EnvelopeTool: {
        arrow(p, 2.0, 2.0);
        QPainterPath e;
        e.moveTo(12.0, 26.0);
        e.lineTo(17.0, 20.0);
        e.lineTo(23.0, 23.0);
        e.lineTo(29.0, 16.0);
        ink(p, e, 1.3);
        for (const QPointF& pt : {QPointF(17.0, 20.0), QPointF(23.0, 23.0)}) {
            QPainterPath d;
            d.addRect(QRectF(pt.x() - 1.8, pt.y() - 1.8, 3.6, 3.6));
            solid(p, d);
        }
        hot = QPoint(2, 2);
        break;
    }
    case EditCursor::Count: break;
    }
    p.end();
    return QCursor(pm, hot.x(), hot.y());
}

}  // namespace cursor_detail

// The pointer for k (made on first request, then kept).
inline const QCursor& editCursor(EditCursor k)
{
    static std::array<QCursor, (size_t)EditCursor::Count> made;
    static std::array<bool, (size_t)EditCursor::Count> done{};
    const size_t i = (size_t)k < made.size() ? (size_t)k : 0;
    if (!done[i]) {
        made[i] = cursor_detail::make((EditCursor)i);
        done[i] = true;
    }
    return made[i];
}

}  // namespace app
}  // namespace reno
