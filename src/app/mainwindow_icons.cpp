// Tool bar and menu icons, drawn with the painter.
#include "mainwindow_internal.h"
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <cmath>
#include <map>

namespace reno {
namespace app {

namespace {
const double kPi = 3.14159265358979323846;
}

// Tool bar images drawn with the painter, in the style of VEGAS 18's flat icons (light shapes with a dark rim, a few in
// colour), with none of its artwork.
QIcon toolbarIcon(Ico k)
{
    const int S = 32;           // drawn at twice the size, shown at 16
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(2.0, 2.0);
    const QColor fg(236, 236, 236), rim(28, 28, 28), tan(190, 170, 102), blue(120, 190, 226), green(126, 202, 132), pink(214, 120, 136);
    const QPen rimPen(rim, 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    p.setPen(rimPen);
    p.setBrush(fg);
    auto page = [&](double x, double y, double w, double h) {
        QPainterPath path;
        path.moveTo(x, y);
        path.lineTo(x + w - 3.5, y);
        path.lineTo(x + w, y + 3.5);
        path.lineTo(x + w, y + h);
        path.lineTo(x, y + h);
        path.closeSubpath();
        p.drawPath(path);
    };
    auto hatched = [&](const QRectF& r, const QColor& c) {
        p.save();
        p.setPen(rimPen);
        p.setBrush(c);
        p.drawRect(r);
        p.setPen(QPen(QColor(90, 40, 50), 0.7));
        p.setClipRect(r);
        for (double d = -r.height(); d < r.width(); d += 2.0) p.drawLine(QPointF(r.left() + d, r.bottom()), QPointF(r.left() + d + r.height(), r.top()));
        p.restore();
    };
    auto arrowCursor = [&](double x, double y, double s) {
        QPainterPath a;
        a.moveTo(x, y);
        a.lineTo(x, y + 10 * s);
        a.lineTo(x + 2.6 * s, y + 7.8 * s);
        a.lineTo(x + 4.4 * s, y + 11.5 * s);
        a.lineTo(x + 6 * s, y + 10.8 * s);
        a.lineTo(x + 4.2 * s, y + 7.2 * s);
        a.lineTo(x + 7.4 * s, y + 7.2 * s);
        a.closeSubpath();
        p.setPen(rimPen);
        p.setBrush(fg);
        p.drawPath(a);
    };
    auto dashedBox = [&](const QRectF& r) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(rim, 2.2));
        p.drawRect(r);
        p.setPen(QPen(fg, 1.2, Qt::DashLine));
        p.drawRect(r);
    };
    auto chain = [&](double x, double y) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(rim, 2.6));
        p.drawRect(QRectF(x, y, 6, 4));
        p.drawRect(QRectF(x + 5, y + 2.5, 6, 4));
        p.setPen(QPen(fg, 1.2));
        p.drawRect(QRectF(x, y, 6, 4));
        p.drawRect(QRectF(x + 5, y + 2.5, 6, 4));
    };
    switch (k) {
    case Ico::New: page(3.5, 1.5, 9, 13); break;
    case Ico::Open: {
        QPainterPath f;
        f.moveTo(1.5, 3.5);
        f.lineTo(6, 3.5);
        f.lineTo(7.5, 5);
        f.lineTo(14.5, 5);
        f.lineTo(14.5, 13.5);
        f.lineTo(1.5, 13.5);
        f.closeSubpath();
        p.setBrush(tan);
        p.drawPath(f);
        p.drawLine(QPointF(1.5, 6.5), QPointF(14.5, 6.5));
        break;
    }
    case Ico::Save:
    case Ico::SaveAs:
        if (k == Ico::Save) {
            p.setBrush(blue);
            p.drawRect(QRectF(1.5, 1.5, 13, 13));
            p.setBrush(fg);
            p.drawRect(QRectF(4.5, 1.5, 7, 5));
            p.setBrush(rim);
            p.drawRect(QRectF(5, 9.5, 6, 5));
            p.setBrush(fg);
            p.drawRect(QRectF(6.5, 10.5, 2, 4));
        } else {
            page(2.5, 1.5, 9, 13);
            p.setPen(QPen(rim, 3.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(7, 11), QPointF(13, 5.5));
            p.setPen(QPen(blue, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(7, 11), QPointF(13, 5.5));
            p.setPen(rimPen);
            p.setBrush(blue);
            QPolygonF h;
            h << QPointF(15, 3.5) << QPointF(10, 4.5) << QPointF(14, 8.5);
            p.drawPolygon(h);
        }
        break;
    case Ico::Render: {
        // a strip of film with an arrow out of it
        p.drawRect(QRectF(1.5, 3.5, 9, 9));
        p.setPen(Qt::NoPen);
        p.setBrush(rim);
        for (int k2 = 0; k2 < 3; ++k2) {
            p.drawRect(QRectF(2.5, 4.5 + k2 * 3, 1.5, 1.5));
            p.drawRect(QRectF(8, 4.5 + k2 * 3, 1.5, 1.5));
        }
        p.setPen(rimPen);
        p.setBrush(blue);
        QPolygonF a;
        a << QPointF(9, 6.5) << QPointF(12, 6.5) << QPointF(12, 4) << QPointF(15.5, 8) << QPointF(12, 12) << QPointF(12, 9.5) << QPointF(9, 9.5);
        p.drawPolygon(a);
        break;
    }
    case Ico::Properties:
    case Ico::Settings: {
        // a gear
        QPainterPath g;
        const QPointF c(8, 8);
        for (int t = 0; t < 8; ++t) {
            const double a0 = (t * 45.0 - 10.0) * kPi / 180.0, a1 = (t * 45.0 + 10.0) * kPi / 180.0;
            const double a2 = (t * 45.0 + 22.5) * kPi / 180.0;
            const QPointF o0(c.x() + 6.6 * std::cos(a0), c.y() + 6.6 * std::sin(a0)), o1(c.x() + 6.6 * std::cos(a1), c.y() + 6.6 * std::sin(a1));
            const QPointF i0(c.x() + 4.6 * std::cos(a1 + 0.12), c.y() + 4.6 * std::sin(a1 + 0.12)), i1(c.x() + 4.6 * std::cos(a2 + 0.25), c.y() + 4.6 * std::sin(a2 + 0.25));
            if (t == 0) g.moveTo(o0);
            else g.lineTo(o0);
            g.lineTo(o1);
            g.lineTo(i0);
            g.lineTo(i1);
        }
        g.closeSubpath();
        g.addEllipse(c, 2.2, 2.2);
        p.setBrush(fg);
        p.drawPath(g);
        break;
    }
    case Ico::Cut:
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(fg, 1.5));
        p.drawEllipse(QPointF(4.5, 11.5), 2.4, 2.4);
        p.drawEllipse(QPointF(11.5, 11.5), 2.4, 2.4);
        p.drawLine(QPointF(5.8, 9.5), QPointF(11.5, 1.5));
        p.drawLine(QPointF(10.2, 9.5), QPointF(4.5, 1.5));
        break;
    case Ico::Copy:
        page(1.5, 1.5, 8, 10);
        page(6.5, 4.5, 8, 10);
        break;
    case Ico::Paste:
        p.setBrush(QColor(170, 170, 170));
        p.drawRect(QRectF(1.5, 2.5, 10, 12));
        p.setBrush(fg);
        p.drawRect(QRectF(4, 1, 5, 3));
        page(6.5, 6.5, 8, 8);
        break;
    case Ico::Undo:
    case Ico::Redo: {
        const bool undo = k == Ico::Undo;
        if (!undo) {
            p.translate(16, 0);
            p.scale(-1, 1);
        }
        QPainterPath arc;
        arc.moveTo(4, 7);
        arc.cubicTo(7.5, 3.5, 14.5, 4.5, 13.5, 10);
        arc.cubicTo(13, 12.5, 11, 13.5, 8.5, 13.5);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(rim, 4.2, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(arc);
        p.setPen(QPen(fg, 2.4, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(arc);
        p.setPen(rimPen);
        p.setBrush(fg);
        QPolygonF h;
        h << QPointF(1, 7.5) << QPointF(6.5, 2.5) << QPointF(7, 10);
        p.drawPolygon(h);
        break;
    }
    case Ico::ToolNormal:
        arrowCursor(2, 2, 1.0);
        p.drawRect(QRectF(10.5, 2, 3.5, 6));
        p.drawRect(QRectF(11.5, 8, 1.5, 6));
        break;
    case Ico::ToolShuffle: {
        // two events trading places: a curved arrow over them
        p.setBrush(tan);
        p.drawRect(QRectF(1.5, 9.5, 6, 5));
        p.setBrush(blue);
        p.drawRect(QRectF(8.5, 9.5, 6, 5));
        QPainterPath arc;
        arc.moveTo(4.5, 8);
        arc.cubicTo(4.5, 2.5, 11.5, 2.5, 11.5, 6.5);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(rim, 3.0, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(arc);
        p.setPen(QPen(fg, 1.4, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(arc);
        p.setPen(rimPen);
        p.setBrush(fg);
        QPolygonF h;
        h << QPointF(9, 5.5) << QPointF(14, 5.5) << QPointF(11.5, 9);
        p.drawPolygon(h);
        break;
    }
    case Ico::ToolSlip:
    case Ico::ToolSlide: {
        // an event with a two-headed arrow: slip moves the image inside the frame, slide the frame over it
        const bool slip = k == Ico::ToolSlip;
        p.setBrush(slip ? fg : tan);
        p.drawRect(QRectF(3.5, 6.5, 9, 7));
        p.setBrush(slip ? blue : fg);
        p.drawRect(QRectF(slip ? 5.5 : 1.5, slip ? 8.5 : 9, slip ? 5 : 13, slip ? 3 : 2));
        p.setPen(QPen(rim, 2.6, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(3, 3), QPointF(13, 3));
        p.setPen(QPen(fg, 1.2, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(3, 3), QPointF(13, 3));
        p.setPen(rimPen);
        p.setBrush(fg);
        QPolygonF l, r;
        l << QPointF(0.5, 3) << QPointF(3.5, 0.8) << QPointF(3.5, 5.2);
        r << QPointF(15.5, 3) << QPointF(12.5, 0.8) << QPointF(12.5, 5.2);
        p.drawPolygon(l);
        p.drawPolygon(r);
        break;
    }
    case Ico::ToolStretch: {
        // an event pulled out at its end, with a spring drawn in it
        p.setBrush(green);
        p.drawRect(QRectF(1.5, 4.5, 10, 8));
        p.setPen(QPen(rim, 1.0));
        QPainterPath z;
        z.moveTo(2.5, 8.5);
        for (int i = 0; i < 4; ++i) {
            z.lineTo(3.75 + i * 2.0, 6);
            z.lineTo(4.75 + i * 2.0, 11);
        }
        p.setBrush(Qt::NoBrush);
        p.drawPath(z);
        p.setPen(rimPen);
        p.setBrush(fg);
        QPolygonF r;
        r << QPointF(15.5, 8.5) << QPointF(12, 5) << QPointF(12, 12);
        p.drawPolygon(r);
        break;
    }
    case Ico::ToolSplitTrim: {
        // an event cut in two, the piece on the right pulled away
        p.setBrush(tan);
        p.drawRect(QRectF(1.5, 5.5, 6, 8));
        p.setBrush(fg);
        p.drawRect(QRectF(9.5, 5.5, 5, 8));
        p.setPen(QPen(rim, 2.4, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(8.5, 1.5), QPointF(8.5, 15));
        p.setPen(QPen(pink, 1.2, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(8.5, 1.5), QPointF(8.5, 15));
        p.setPen(rimPen);
        break;
    }
    case Ico::ToolEnvelope:
        arrowCursor(1.5, 2, 0.9);
        p.setPen(QPen(fg, 1.3));
        p.drawLine(QPointF(9, 6), QPointF(12.5, 3.5));
        p.drawLine(QPointF(9, 6), QPointF(12, 11));
        p.setPen(rimPen);
        p.setBrush(fg);
        for (const QPointF& c : {QPointF(9, 6), QPointF(12.5, 3.5), QPointF(12, 11)}) p.drawEllipse(c, 1.8, 1.8);
        break;
    case Ico::ToolSelect:
        dashedBox(QRectF(1.5, 1.5, 11, 10));
        arrowCursor(8, 5, 0.85);
        break;
    case Ico::ToolZoom:
        dashedBox(QRectF(1.5, 1.5, 11, 10));
        p.setPen(QPen(rim, 3.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(10, 10), QPointF(14.5, 14.5));
        p.setPen(QPen(fg, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(10, 10), QPointF(14.5, 14.5));
        p.setPen(rimPen);
        p.setBrush(blue);
        p.drawEllipse(QPointF(8, 8), 3.5, 3.5);
        break;
    case Ico::Delete:
        p.setPen(QPen(rim, 3.6, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(3, 3), QPointF(13, 13));
        p.drawLine(QPointF(13, 3), QPointF(3, 13));
        p.setPen(QPen(fg, 2.0, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(3, 3), QPointF(13, 13));
        p.drawLine(QPointF(13, 3), QPointF(3, 13));
        break;
    case Ico::Trim:
        hatched(QRectF(1.5, 4.5, 3.5, 7), pink);
        hatched(QRectF(11, 4.5, 3.5, 7), pink);
        p.setBrush(fg);
        p.drawRect(QRectF(5, 2.5, 6, 11));
        break;
    case Ico::TrimStart:
    case Ico::TrimEnd: {
        const bool start = k == Ico::TrimStart;
        hatched(start ? QRectF(1.5, 4.5, 7, 7) : QRectF(7.5, 4.5, 7, 7), pink);
        p.setBrush(fg);
        p.drawRect(start ? QRectF(9, 3.5, 5, 9) : QRectF(2, 3.5, 5, 9));
        p.drawRect(QRectF(start ? 8 : 7, 1.5, 1, 13));
        break;
    }
    case Ico::Split:
        p.setBrush(blue);
        p.drawRect(QRectF(1.5, 4.5, 5.5, 7));
        p.drawRect(QRectF(9, 4.5, 5.5, 7));
        p.setBrush(fg);
        p.drawRect(QRectF(7.5, 1.5, 1, 13));
        break;
    case Ico::Lock:
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(fg, 1.6));
        p.drawRoundedRect(QRectF(5, 2, 6, 8), 3, 3);
        p.setPen(rimPen);
        p.setBrush(fg);
        p.drawRect(QRectF(3.5, 7, 9, 7.5));
        break;
    case Ico::IgnoreGroup:
        p.setBrush(blue);
        p.drawRect(QRectF(1.5, 3.5, 11, 7));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(pink, 1.3, Qt::DashLine));
        p.drawRect(QRectF(3.5, 5.5, 11, 7));
        arrowCursor(7.5, 5.5, 0.8);
        break;
    case Ico::Ripple:
        p.setBrush(fg);
        p.drawRect(QRectF(1.5, 2, 2, 12));
        p.drawRect(QRectF(5.5, 8, 9, 5.5));
        p.setBrush(blue);
        p.drawRect(QRectF(3.5, 3.5, 4, 4));
        p.setBrush(fg);
        {
            QPolygonF a;
            a << QPointF(8, 5.5) << QPointF(11, 2.5) << QPointF(11, 4.5) << QPointF(14.5, 4.5) << QPointF(14.5, 6.5) << QPointF(11, 6.5) << QPointF(11, 8.5);
            p.drawPolygon(a);
        }
        break;
    case Ico::Snap:
        // a magnet
        p.setPen(QPen(rim, 4.6, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(QRectF(3.5, 3, 9, 9.5), 180 * 16, 180 * 16);
        p.drawLine(QPointF(4.8, 8), QPointF(4.8, 2));
        p.drawLine(QPointF(11.2, 8), QPointF(11.2, 2));
        p.setPen(QPen(QColor(228, 72, 84), 2.8, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(QRectF(3.5, 3, 9, 9.5), 180 * 16, 180 * 16);
        p.drawLine(QPointF(4.8, 8), QPointF(4.8, 4.5));
        p.drawLine(QPointF(11.2, 8), QPointF(11.2, 4.5));
        p.setPen(QPen(fg, 2.8, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(4.8, 4.5), QPointF(4.8, 2.5));
        p.drawLine(QPointF(11.2, 4.5), QPointF(11.2, 2.5));
        break;
    case Ico::Quantize:
        // a stair of frames
        p.setPen(QPen(fg, 1.0));
        for (int k2 = 0; k2 < 4; ++k2) p.drawLine(QPointF(2.5 + k2 * 3.5, 2.5), QPointF(2.5 + k2 * 3.5, 13.5));
        p.setPen(QPen(blue, 1.8));
        p.drawPolyline(QPolygonF() << QPointF(1.5, 12) << QPointF(6, 12) << QPointF(6, 8) << QPointF(9.5, 8) << QPointF(9.5, 4) << QPointF(14.5, 4));
        break;
    case Ico::Marker: {
        QPolygonF f;
        f << QPointF(2.5, 1.5) << QPointF(13.5, 1.5) << QPointF(13.5, 9.5) << QPointF(5, 9.5) << QPointF(5, 14.5) << QPointF(2.5, 14.5);
        p.setBrush(tan);
        p.drawPolygon(f);
        break;
    }
    case Ico::Region: {
        QPolygonF f;
        f << QPointF(1.5, 1.5) << QPointF(14.5, 1.5) << QPointF(14.5, 14.5) << QPointF(12, 14.5) << QPointF(12, 7) << QPointF(8, 4.5)
          << QPointF(4, 7) << QPointF(4, 14.5) << QPointF(1.5, 14.5);
        p.setBrush(green);
        p.drawPolygon(f);
        break;
    }
    case Ico::AutoCrossfade:
        p.setBrush(blue);
        p.drawRect(QRectF(1.5, 1.5, 13, 13));
        p.setPen(QPen(rim, 1.2));
        p.drawLine(QPointF(1.5, 1.5), QPointF(14.5, 14.5));
        p.drawLine(QPointF(14.5, 1.5), QPointF(1.5, 14.5));
        break;
    case Ico::LockEnvelopes:
        p.setPen(QPen(fg, 1.3));
        p.drawLine(QPointF(3, 12), QPointF(6, 4));
        p.drawLine(QPointF(6, 4), QPointF(12, 3));
        p.setPen(rimPen);
        p.setBrush(fg);
        for (const QPointF& c : {QPointF(3, 12), QPointF(6, 4), QPointF(12, 3)}) p.drawEllipse(c, 1.7, 1.7);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(fg, 1.3));
        p.drawRoundedRect(QRectF(9, 6.5, 4.5, 5), 2, 2);
        p.setPen(rimPen);
        p.setBrush(fg);
        p.drawRect(QRectF(8, 9.5, 6.5, 5.5));
        break;
    case Ico::Tutorials: {
        // an open book with a play mark on its right page
        p.setBrush(fg);
        QPainterPath l, r;
        l.moveTo(8, 4); l.cubicTo(6, 2.5, 3, 2.5, 1.5, 3.5); l.lineTo(1.5, 13); l.cubicTo(3, 12, 6, 12, 8, 13.5); l.closeSubpath();
        r.moveTo(8, 4); r.cubicTo(10, 2.5, 13, 2.5, 14.5, 3.5); r.lineTo(14.5, 13); r.cubicTo(13, 12, 10, 12, 8, 13.5); r.closeSubpath();
        p.drawPath(l);
        p.drawPath(r);
        p.setPen(Qt::NoPen);
        p.setBrush(green);
        QPolygonF tri;
        tri << QPointF(9.8, 5.6) << QPointF(13.2, 8) << QPointF(9.8, 10.4);
        p.drawPolygon(tri);
        break;
    }
    case Ico::WhatsThis: {
        arrowCursor(1.5, 1.5, 1.0);
        QFont f;
        f.setPixelSize(10);
        f.setBold(true);
        QPainterPath q;
        q.addText(8.6, 13.5, f, "?");
        p.setPen(QPen(rim, 1.6));
        p.setBrush(blue);
        p.drawPath(q);
        p.setPen(Qt::NoPen);
        p.drawPath(q);
        break;
    }
    case Ico::ColorGrading: {
        // a colour wheel: three hues around a light centre
        const QRectF w(1.5, 1.5, 13, 13);
        const QColor hues[6] = {QColor(232, 86, 86), QColor(232, 200, 80), QColor(110, 204, 110), QColor(90, 200, 220), QColor(96, 120, 232), QColor(206, 96, 206)};
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 6; ++i) {
            p.setBrush(hues[i]);
            p.drawPie(w, (90 - i * 60) * 16, -60 * 16);
        }
        p.setPen(rimPen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(w);
        p.setBrush(fg);
        p.drawEllipse(QRectF(5.5, 5.5, 5, 5));
        break;
    }
    case Ico::NestParent:
    case Ico::NestOpen:
    case Ico::NestCreate: {
        // a box (the nested timeline) with events in it; Open Parent: an arrow out of it, upward; Open: an arrow into
        // it; Create: events gathered into it
        p.setBrush(QColor(70, 70, 74));
        p.drawRect(QRectF(1.5, 6.5, 13, 8));
        p.setPen(Qt::NoPen);
        p.setBrush(blue);
        p.drawRect(QRectF(3, 8.5, 5, 2));
        p.setBrush(green);
        p.drawRect(QRectF(7, 11.5, 6, 2));
        p.setPen(QPen(rim, 3.0, Qt::SolidLine, Qt::RoundCap));
        const bool out = k == Ico::NestParent;
        auto arrow = [&](const QPen& pen) {
            p.setPen(pen);
            if (k == Ico::NestCreate) {
                p.drawLine(QPointF(4, 1.5), QPointF(6.5, 4.5));
                p.drawLine(QPointF(12, 1.5), QPointF(9.5, 4.5));
            } else {
                p.drawLine(QPointF(8, out ? 7 : 0.8), QPointF(8, out ? 0.8 : 6.5));
                const double tip = out ? 0.8 : 6.5, back = out ? 3.6 : 3.7;
                p.drawLine(QPointF(8, tip), QPointF(5.4, back));
                p.drawLine(QPointF(8, tip), QPointF(10.6, back));
            }
        };
        arrow(QPen(rim, 3.0, Qt::SolidLine, Qt::RoundCap));
        arrow(QPen(fg, 1.4, Qt::SolidLine, Qt::RoundCap));
        break;
    }
    case Ico::Group:
    case Ico::Ungroup:
    case Ico::ClearGroup:
        chain(2, 7);
        p.setPen(QPen(fg, 1.4));
        if (k == Ico::Group) {
            p.drawLine(QPointF(10, 1.5), QPointF(10, 6.5));
            p.drawLine(QPointF(7.5, 4), QPointF(12.5, 4));
        } else if (k == Ico::Ungroup) {
            p.drawLine(QPointF(7.5, 4), QPointF(12.5, 4));
        } else {
            p.drawLine(QPointF(8, 2), QPointF(12, 6));
            p.drawLine(QPointF(12, 2), QPointF(8, 6));
        }
        break;
    case Ico::Cursor: {
        QPainterPath pin;
        pin.addRoundedRect(QRectF(4.5, 1.5, 7, 7), 1.5, 1.5);
        QPolygonF t;
        t << QPointF(6, 8) << QPointF(10, 8) << QPointF(8, 11);
        pin.addPolygon(t);
        p.drawPath(pin.simplified());
        p.drawLine(QPointF(8, 11), QPointF(8, 14.5));
        break;
    }
    case Ico::SelectionEnd: {
        QPolygonF t;
        t << QPointF(3.5, 3.5) << QPointF(12.5, 3.5) << QPointF(12.5, 12.5);
        p.setBrush(tan);
        p.drawPolygon(t);
        break;
    }
    case Ico::SelectionLength: {
        QPolygonF t;
        t << QPointF(1.5, 12.5) << QPointF(1.5, 8.5) << QPointF(5, 4.5) << QPointF(11, 4.5) << QPointF(14.5, 8.5) << QPointF(14.5, 12.5);
        p.setBrush(tan);
        p.drawPolygon(t);
        p.setBrush(fg);
        QPolygonF in;
        in << QPointF(4, 11) << QPointF(6, 7) << QPointF(10, 7) << QPointF(12, 11);
        p.drawPolygon(in);
        break;
    }
    case Ico::Downmix: {
        QPolygonF a, b;
        a << QPointF(1.5, 3) << QPointF(7, 8) << QPointF(1.5, 13);
        b << QPointF(14.5, 3) << QPointF(9, 8) << QPointF(14.5, 13);
        p.drawPolygon(a);
        p.drawPolygon(b);
        break;
    }
    case Ico::Dim: {
        QPolygonF s;
        s << QPointF(1.5, 6) << QPointF(4, 6) << QPointF(8, 2.5) << QPointF(8, 13.5) << QPointF(4, 10) << QPointF(1.5, 10);
        p.drawPolygon(s);
        p.setBrush(blue);
        QPolygonF a;
        a << QPointF(10, 6) << QPointF(14.5, 6) << QPointF(12.25, 11);
        p.drawPolygon(a);
        break;
    }
    case Ico::Mixer:
        for (int k2 = 0; k2 < 3; ++k2) {
            const double x = 3 + k2 * 5;
            p.setPen(QPen(fg, 1.4));
            p.drawLine(QPointF(x, 1.5), QPointF(x, 14.5));
            p.setPen(rimPen);
            p.setBrush(fg);
            p.drawRect(QRectF(x - 2, k2 == 1 ? 9 : 4, 4, 3));
        }
        break;
    case Ico::Float:
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(fg, 1.2));
        p.drawRect(QRectF(3.5, 3.5, 9, 9));
        p.fillRect(QRectF(3.5, 3.5, 9, 2), fg);
        break;
    case Ico::Close:
        p.setPen(QPen(fg, 1.6));
        p.drawLine(QPointF(4, 4), QPointF(12, 12));
        p.drawLine(QPointF(12, 4), QPointF(4, 12));
        break;
    }
    p.end();
    pm.setDevicePixelRatio(2.0);
    QIcon ic(pm);
    // the disabled look: dim grey, as VEGAS greys out what cannot be used
    QImage dim = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < dim.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(dim.scanLine(y));
        for (int x = 0; x < dim.width(); ++x) {
            const int g = 60 + qGray(line[x]) / 5;
            line[x] = qRgba(g, g, g, qAlpha(line[x]));
        }
    }
    QPixmap off = QPixmap::fromImage(dim);
    off.setDevicePixelRatio(2.0);
    ic.addPixmap(off, QIcon::Disabled);
    return ic;
}

// An image for a command put on a tool bar (Customize Toolbar and VEGAS's default sets): the command's own, one drawn
// here, or a plain square with its first letter, so no button shows as words.
QIcon commandIcon(const QString& name, const QString& text)
{
    static const std::map<QString, Ico> named = {
        {"interactiveTutorials", Ico::Tutorials}, {"whatsThis", Ico::WhatsThis}, {"colorGrading", Ico::ColorGrading},
        {"showColorGrading", Ico::ColorGrading}, {"nestParent", Ico::NestParent}, {"nestOpen", Ico::NestOpen}, {"nestCreate", Ico::NestCreate}};
    auto it = named.find(name);
    if (it != named.end()) return toolbarIcon(it->second);
    QString letter;
    for (const QChar c : text)
        if (c.isLetterOrNumber()) { letter = c.toUpper(); break; }
    const int S = 32;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(28, 28, 28), 2.0));
    p.setBrush(QColor(96, 96, 100));
    p.drawRoundedRect(QRectF(3, 3, 26, 26), 5, 5);
    QFont f;
    f.setPixelSize(18);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(236, 236, 236));
    p.drawText(QRectF(3, 3, 26, 26), Qt::AlignCenter, letter);
    p.end();
    return QIcon(pm);
}

}  // namespace app
}  // namespace reno
