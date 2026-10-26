#include "logo.h"
#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace reno {
namespace app {

const char* renoVersion() { return "1.0"; }

// Like VEGAS Pro's icon (dark square, thin rim, big letter, version in the corner) but in the inverse of VEGAS's blue: orange.
QImage renoLogo(int size)
{
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    const double s = size;

    // VEGAS's blue runs from (14, 196, 255) at the top to (21, 159, 255) lower down; this is its inverse:
    const QColor top(241, 59, 0), bottom(234, 96, 0);
    QLinearGradient orange(QPointF(0, 0), QPointF(0, s));
    orange.setColorAt(0.0, QColor(255 - 14, 255 - 196, 0));
    orange.setColorAt(1.0, QColor(255 - 21, 255 - 159, 0));
    (void)top;
    (void)bottom;

    // rim and dark square inside it
    const double rim = std::max(1.0, s * 0.035);
    p.fillRect(QRectF(0, 0, s, s), orange);
    QLinearGradient dark(QPointF(0, 0), QPointF(s, s));
    dark.setColorAt(0.0, QColor(14, 14, 14));
    dark.setColorAt(1.0, QColor(34, 34, 34));
    p.fillRect(QRectF(rim, rim, s - 2 * rim, s - 2 * rim), dark);

    auto glyphs = [&](const QString& text, double pixelSize, int weight) {
        QFont f;
        f.setFamilies({"Segoe UI", "Arial", "DejaVu Sans", "Liberation Sans"});
        f.setPixelSize(std::max(1, (int)std::lround(pixelSize)));
        f.setWeight((QFont::Weight)weight);
        f.setHintingPreference(QFont::PreferNoHinting);
        QPainterPath path;
        path.addText(0, 0, f, text);
        return path;
    };
    const bool small = size < 40;
    // the R, a little above the middle; small icons leave the version out and centre it
    QPainterPath r = glyphs("R", s * (small ? 0.78 : 0.66), QFont::Medium);
    QRectF rb = r.boundingRect();
    const double rx = s / 2 - rb.center().x(), ry = (small ? s / 2 : s * 0.45) - rb.center().y();
    p.fillPath(r.translated(rx, ry), orange);
    if (!small) {
        QPainterPath v = glyphs(QString(renoVersion()).section('.', 0, 0), s * 0.25, QFont::DemiBold);
        QRectF vb = v.boundingRect();
        p.fillPath(v.translated(s * 0.93 - vb.right(), s * 0.92 - vb.bottom()), orange);
    }
    p.end();
    return img;
}

QIcon renoIcon()
{
    QIcon icon;
    for (int s : {16, 20, 24, 32, 40, 48, 64, 128, 256}) icon.addPixmap(QPixmap::fromImage(renoLogo(s)));
    return icon;
}

}  // namespace app
}  // namespace reno
