#include "touch_style.h"
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <cmath>
#include <map>
#include <tuple>

namespace reno {
namespace app {
namespace touch {

int videoTrackHeight(bool tablet) { return tablet ? 84 : 64; }
int audioTrackHeight(bool tablet) { return tablet ? 64 : 48; }

QFont uiFont(int px, bool bold)
{
    QFont f = QApplication::font();
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

namespace {

// Icons are drawn on a 24 x 24 grid, as most icon sets are, and scaled to the rectangle asked for.
struct Grid {
    QPainter& p;
    QPen pen;
    Grid(QPainter& painter, const QRectF& r, const QColor& c) : p(painter)
    {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        const double s = std::min(r.width(), r.height()) / 24.0;
        p.translate(r.center().x() - 12 * s, r.center().y() - 12 * s);
        p.scale(s, s);
        pen = QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
    }
    ~Grid() { p.restore(); }
    void fill() { p.setBrush(pen.color()); }
    void hollow() { p.setBrush(Qt::NoBrush); }
    void line(double x1, double y1, double x2, double y2) { p.drawLine(QPointF(x1, y1), QPointF(x2, y2)); }
    void poly(std::initializer_list<QPointF> pts, bool closed)
    {
        QPainterPath path;
        bool first = true;
        for (const QPointF& pt : pts) {
            if (first) path.moveTo(pt);
            else path.lineTo(pt);
            first = false;
        }
        if (closed) path.closeSubpath();
        p.drawPath(path);
    }
    void rect(double x, double y, double w, double h, double r = 2) { p.drawRoundedRect(QRectF(x, y, w, h), r, r); }
    void circle(double x, double y, double r) { p.drawEllipse(QPointF(x, y), r, r); }
    void arc(double x, double y, double r, double startDeg, double spanDeg)
    {
        p.drawArc(QRectF(x - r, y - r, 2 * r, 2 * r), (int)(startDeg * 16), (int)(spanDeg * 16));
    }
};

}  // namespace

void drawIcon(QPainter& p, Icon icon, const QRectF& r, const QColor& c)
{
    Grid g(p, r, c);
    switch (icon) {
    case Icon::Play:
        g.fill();
        g.poly({{7, 4.5}, {19.5, 12}, {7, 19.5}}, true);
        break;
    case Icon::Pause:
        g.fill();
        g.rect(6, 4.5, 4, 15, 1);
        g.rect(14, 4.5, 4, 15, 1);
        break;
    case Icon::Undo:
        g.poly({{8, 5}, {4, 9}, {8, 13}}, false);
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(4, 9); pa.lineTo(14, 9); pa.cubicTo(22, 9, 22, 20, 14, 20); pa.lineTo(9, 20); return pa; }());
        break;
    case Icon::Redo:
        g.poly({{16, 5}, {20, 9}, {16, 13}}, false);
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(20, 9); pa.lineTo(10, 9); pa.cubicTo(2, 9, 2, 20, 10, 20); pa.lineTo(15, 20); return pa; }());
        break;
    case Icon::Split:
        g.line(12, 3, 12, 21);
        g.rect(3, 7, 6, 10, 1.5);
        g.rect(15, 7, 6, 10, 1.5);
        break;
    case Icon::Delete:
        g.line(4, 6.5, 20, 6.5);
        g.poly({{9, 6.5}, {9.5, 3.5}, {14.5, 3.5}, {15, 6.5}}, false);
        g.poly({{6, 6.5}, {7, 20.5}, {17, 20.5}, {18, 6.5}}, false);
        g.line(10, 10.5, 10, 16.5);
        g.line(14, 10.5, 14, 16.5);
        break;
    case Icon::Add:
        g.line(12, 5, 12, 19);
        g.line(5, 12, 19, 12);
        break;
    case Icon::Text:
        g.line(5, 5.5, 19, 5.5);
        g.line(12, 5.5, 12, 19.5);
        g.line(9, 19.5, 15, 19.5);
        break;
    case Icon::Music:
        g.line(9, 17, 9, 5);
        g.line(9, 5, 19, 3.5);
        g.line(19, 3.5, 19, 15);
        g.fill();
        g.circle(6.5, 17.5, 2.5);
        g.circle(16.5, 15.5, 2.5);
        break;
    case Icon::Fx:
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(12, 3); pa.lineTo(14.2, 9.8); pa.lineTo(21, 12); pa.lineTo(14.2, 14.2);
            pa.lineTo(12, 21); pa.lineTo(9.8, 14.2); pa.lineTo(3, 12); pa.lineTo(9.8, 9.8); pa.closeSubpath(); return pa; }());
        break;
    case Icon::Speed:
        g.arc(12, 14, 8.5, 0, 180);
        g.line(12, 14, 16.5, 8.5);
        g.fill();
        g.circle(12, 14, 1.6);
        break;
    case Icon::Volume:
        g.poly({{4, 9.5}, {8, 9.5}, {13, 5}, {13, 19}, {8, 14.5}, {4, 14.5}}, true);
        g.arc(13.5, 12, 4, -45, 90);
        g.arc(13.5, 12, 7.5, -45, 90);
        break;
    case Icon::Opacity:
        g.circle(12, 12, 8);
        g.fill();
        g.p.drawPie(QRectF(4, 4, 16, 16), 90 * 16, 180 * 16);
        break;
    case Icon::Fade:
        g.poly({{3, 19}, {9, 7}, {15, 7}, {21, 19}}, false);
        g.line(3, 19, 21, 19);
        break;
    case Icon::Crop:
        g.poly({{7, 3}, {7, 17}, {21, 17}}, false);
        g.poly({{3, 7}, {17, 7}, {17, 21}}, false);
        break;
    case Icon::Copy:
        g.rect(8, 8, 12, 12, 2);
        g.poly({{16, 4}, {6, 4}, {4, 6}, {4, 16}}, false);
        break;
    case Icon::Paste:
        g.rect(5, 5, 14, 16, 2);
        g.rect(9, 3, 6, 4, 1);
        break;
    case Icon::Duplicate:
        g.rect(3, 6, 10, 12, 2);
        g.rect(11, 6, 10, 12, 2);
        break;
    case Icon::More:
        g.fill();
        g.circle(6, 12, 1.8);
        g.circle(12, 12, 1.8);
        g.circle(18, 12, 1.8);
        break;
    case Icon::Back:
    case Icon::Left:
        g.poly({{15, 5}, {8, 12}, {15, 19}}, false);
        break;
    case Icon::Right:
        g.poly({{9, 5}, {16, 12}, {9, 19}}, false);
        break;
    case Icon::Up:
        g.poly({{5, 15}, {12, 8}, {19, 15}}, false);
        break;
    case Icon::Down:
        g.poly({{5, 9}, {12, 16}, {19, 9}}, false);
        break;
    case Icon::Export:
        g.line(12, 15, 12, 3.5);
        g.poly({{7, 8}, {12, 3.5}, {17, 8}}, false);
        g.poly({{5, 12}, {5, 20}, {19, 20}, {19, 12}}, false);
        break;
    case Icon::Check:
        g.poly({{4.5, 12.5}, {9.5, 17.5}, {19.5, 6.5}}, false);
        break;
    case Icon::Close:
        g.line(6, 6, 18, 18);
        g.line(18, 6, 6, 18);
        break;
    case Icon::Transition:
        g.rect(3, 6, 10, 12, 2);
        g.p.setBrush(QColor(c.red(), c.green(), c.blue(), 110));
        g.rect(11, 6, 10, 12, 2);
        g.hollow();
        g.line(11, 6, 13, 18);
        break;
    case Icon::Track:
        g.rect(3, 4, 18, 6, 1.5);
        g.rect(3, 14, 18, 6, 1.5);
        break;
    case Icon::Mute:
        g.poly({{3, 9.5}, {7, 9.5}, {12, 5}, {12, 19}, {7, 14.5}, {3, 14.5}}, true);
        g.line(15, 9, 21, 15);
        g.line(21, 9, 15, 15);
        break;
    case Icon::Solo:
        g.circle(12, 12, 8.5);
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(15, 8.5); pa.cubicTo(13, 7, 9, 7.5, 9.5, 10); pa.cubicTo(10, 12.5, 15, 11.5, 14.6, 14.2);
            pa.cubicTo(14.2, 16.6, 10, 16.8, 8.8, 15.2); return pa; }());
        break;
    case Icon::Lock:
        g.rect(5, 10.5, 14, 10, 2);
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(8, 10.5); pa.lineTo(8, 7.5); pa.arcTo(8, 3.5, 8, 8, 180, -180); pa.lineTo(16, 10.5); return pa; }());
        break;
    case Icon::Magnet:
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(5, 4); pa.lineTo(5, 12); pa.arcTo(5, 5, 14, 14, 180, 180); pa.lineTo(19, 4); return pa; }());
        g.line(5, 8, 9, 8);
        g.line(15, 8, 19, 8);
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(9, 4); pa.lineTo(9, 12); pa.arcTo(9, 9, 6, 6, 180, 180); pa.lineTo(15, 4); return pa; }());
        break;
    case Icon::ToStart:
        g.line(6, 5, 6, 19);
        g.fill();
        g.poly({{19, 5}, {9, 12}, {19, 19}}, true);
        break;
    case Icon::ToEnd:
        g.line(18, 5, 18, 19);
        g.fill();
        g.poly({{5, 5}, {15, 12}, {5, 19}}, true);
        break;
    case Icon::Menu:
        g.line(4, 6.5, 20, 6.5);
        g.line(4, 12, 20, 12);
        g.line(4, 17.5, 20, 17.5);
        break;
    case Icon::Search:
        g.circle(10.5, 10.5, 6);
        g.line(15, 15, 20, 20);
        break;
    case Icon::Folder:
        g.poly({{3, 6}, {3, 19}, {21, 19}, {21, 8}, {11.5, 8}, {9.5, 5}, {3.5, 5}}, true);
        break;
    case Icon::Save:
        g.poly({{4, 4}, {16.5, 4}, {20, 7.5}, {20, 20}, {4, 20}}, true);
        g.rect(8, 13, 8, 7, 1);
        g.line(8, 4, 8, 8.5);
        g.line(8, 8.5, 14, 8.5);
        g.line(14, 8.5, 14, 4);
        break;
    case Icon::Media:
        g.rect(3, 5, 18, 14, 2);
        g.poly({{3, 16}, {9, 10.5}, {13, 14}, {16, 11.5}, {21, 16}}, false);
        g.fill();
        g.circle(16, 8.5, 1.5);
        break;
    case Icon::Settings:
        for (int k = 0; k < 8; ++k) {
            const double a = k * 0.78539816339744831;   // 45 degrees
            g.line(12 + 6.5 * std::cos(a), 12 + 6.5 * std::sin(a), 12 + 9 * std::cos(a), 12 + 9 * std::sin(a));
        }
        g.circle(12, 12, 6);
        g.circle(12, 12, 2.5);
        break;
    case Icon::Reverse:
        g.poly({{8, 4}, {4, 8}, {8, 12}}, false);
        g.line(4, 8, 20, 8);
        g.poly({{16, 12}, {20, 16}, {16, 20}}, false);
        g.line(20, 16, 4, 16);
        break;
    case Icon::Freeze:
        g.line(12, 3, 12, 21);
        g.line(4.2, 7.5, 19.8, 16.5);
        g.line(4.2, 16.5, 19.8, 7.5);
        g.poly({{9.5, 4}, {12, 6.5}, {14.5, 4}}, false);
        g.poly({{9.5, 20}, {12, 17.5}, {14.5, 20}}, false);
        break;
    case Icon::Desktop:
        g.rect(3, 4, 18, 12, 2);
        g.line(9, 20, 15, 20);
        g.line(12, 16, 12, 20);
        break;
    case Icon::Eye:
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(2.5, 12); pa.cubicTo(7, 4.5, 17, 4.5, 21.5, 12); pa.cubicTo(17, 19.5, 7, 19.5, 2.5, 12); return pa; }());
        g.circle(12, 12, 3);
        break;
    case Icon::EyeOff:
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(2.5, 12); pa.cubicTo(7, 4.5, 17, 4.5, 21.5, 12); pa.cubicTo(17, 19.5, 7, 19.5, 2.5, 12); return pa; }());
        g.line(4, 20, 20, 4);
        break;
    case Icon::Fullscreen:
        g.poly({{4, 9}, {4, 4}, {9, 4}}, false);
        g.poly({{15, 4}, {20, 4}, {20, 9}}, false);
        g.poly({{20, 15}, {20, 20}, {15, 20}}, false);
        g.poly({{9, 20}, {4, 20}, {4, 15}}, false);
        break;
    case Icon::Motion:
        g.rect(3, 3, 18, 18, 2);
        g.rect(8, 8, 10, 7, 1);
        g.line(5.5, 18.5, 9.5, 14.5);
        break;
    case Icon::Keyframe:
        g.poly({{12, 4}, {20, 12}, {12, 20}, {4, 12}}, true);
        break;
    case Icon::Marker:
        g.fill();
        g.poly({{6, 3}, {18, 3}, {18, 13}, {12, 18}, {6, 13}}, true);
        break;
    case Icon::Group:
        g.rect(3, 4, 12, 7, 1.5);
        g.rect(9, 13, 12, 7, 1.5);
        g.line(9, 11, 9, 13);
        break;
    case Icon::Loop:
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(6, 15); pa.lineTo(6, 11); pa.cubicTo(6, 8, 8, 7, 11, 7); pa.lineTo(18, 7); return pa; }());
        g.poly({{15, 4}, {18, 7}, {15, 10}}, false);
        g.p.drawPath([] { QPainterPath pa; pa.moveTo(18, 9); pa.lineTo(18, 13); pa.cubicTo(18, 16, 16, 17, 13, 17); pa.lineTo(6, 17); return pa; }());
        g.poly({{9, 14}, {6, 17}, {9, 20}}, false);
        break;
    case Icon::Record:
        g.fill();
        g.circle(12, 12, 7);
        break;
    }
}

QIcon icon(Icon which, const QColor& c, int size)
{
    // made once per icon, colour and size: QIcon pixmaps are drawn at the screen's scale
    static std::map<std::tuple<int, QRgb, int>, QIcon> made;
    const auto key = std::make_tuple((int)which, c.rgba(), size);
    auto it = made.find(key);
    if (it != made.end()) return it->second;
    QIcon out;
    for (double scale : {1.0, 2.0, 3.0}) {
        QPixmap pm((int)std::ceil(size * scale), (int)std::ceil(size * scale));
        pm.fill(Qt::transparent);
        pm.setDevicePixelRatio(scale);
        QPainter p(&pm);
        drawIcon(p, which, QRectF(0, 0, size, size), c);
        p.end();
        out.addPixmap(pm);
    }
    made[key] = out;
    return out;
}

const QPixmap& iconPixmap(Icon which, const QColor& c, int size, qreal dpr)
{
    static std::map<std::tuple<int, QRgb, int, int>, QPixmap> made;
    const auto key = std::make_tuple((int)which, c.rgba(), size, (int)std::lround(dpr * 100));
    auto it = made.find(key);
    if (it != made.end()) return it->second;
    QPixmap pm((int)std::ceil(size * dpr), (int)std::ceil(size * dpr));
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QPainter p(&pm);
    drawIcon(p, which, QRectF(0, 0, size, size), c);
    p.end();
    return made.emplace(key, pm).first->second;
}

QString styleSheet()
{
    return QString(
        "QWidget#touchShell { background: %1; }"
        "QLineEdit { background: %2; color: %3; border: 1px solid %4; border-radius: 8px; padding: 10px 12px; font-size: 16px;"
        " selection-background-color: %5; }"
        "QLineEdit:focus { border-color: %5; }"
        "QScrollArea { background: transparent; border: none; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"
        "QScrollBar:vertical { background: transparent; width: 4px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: %4; border-radius: 2px; min-height: 30px; }"
        "QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; }"
        "QScrollBar:horizontal { background: transparent; height: 4px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: %4; border-radius: 2px; min-width: 30px; }")
        .arg(color::bg.name(), color::raised.name(), color::text.name(), color::line.name(), color::accent.name());
}

}  // namespace touch
}  // namespace app
}  // namespace reno
