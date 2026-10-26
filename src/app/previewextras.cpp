// The Video Preview's Closed Captioning overlays and 360° mode. See previewextras.h.
#include "previewextras.h"
#include "appstate.h"
#include "../engine/captions.h"
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace reno {
namespace app {

void drawCaptions(QPainter& p, const QRect& target, int channel, double t)
{
    if (target.isEmpty() || channel < 1 || channel > 4) return;
    const AppState& app = AppState::get();
    const std::vector<Caption> caps = projectCaptions(app.project(), channel);
    if (caps.empty()) return;
    const std::vector<CaptionRow> rows = captionScreenAt(caps, t);
    if (rows.empty()) return;
    const double safe = std::max(0.5, std::min(1.0, 1.0 - app.prefs.titleSafe / 100.0));
    const QRectF area(target.left() + target.width() * (1 - safe) / 2, target.top() + target.height() * (1 - safe) / 2,
                      target.width() * safe, target.height() * safe);
    const double rowH = area.height() / 15.0, colW = area.width() / 32.0;
    QFont f("Courier New");
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(std::max(6, (int)std::lround(rowH * 0.78)));
    p.save();
    p.setRenderHint(QPainter::TextAntialiasing, true);
    for (const CaptionRow& r : rows) {
        const QString text = QString::fromStdString(r.text);
        QFont rf = f;
        rf.setItalic(r.italic);
        rf.setUnderline(r.underline);
        p.setFont(rf);
        const QRectF box(area.left() + r.column * colW, area.top() + (r.row - 1) * rowH, std::min(32 - r.column, (int)text.size()) * colW + colW * 0.2, rowH);
        p.fillRect(box, QColor(0, 0, 0, 230));
        QColor c(255, 255, 255);
        if (r.color == "Gr") c = QColor(0, 255, 0);
        else if (r.color == "Bl") c = QColor(80, 80, 255);
        else if (r.color == "Cy") c = QColor(0, 255, 255);
        else if (r.color == "Rd") c = QColor(255, 40, 40);
        else if (r.color == "Yl") c = QColor(255, 255, 0);
        else if (r.color == "Ma") c = QColor(255, 0, 255);
        p.setPen(c);
        for (int k = 0; k < text.size() && r.column + k < 32; ++k)
            p.drawText(QRectF(box.left() + k * colW, box.top(), colW * 1.2, rowH), Qt::AlignLeft | Qt::AlignVCenter, text.mid(k, 1));
    }
    p.restore();
}

QImage view360(const QImage& equirect, const QSize& out, double yaw, double pitch, double fov)
{
    if (equirect.isNull() || out.isEmpty()) return QImage();
    const QImage src = equirect.format() == QImage::Format_RGB32 || equirect.format() == QImage::Format_ARGB32 ? equirect
                                                                                                                : equirect.convertToFormat(QImage::Format_RGB32);
    QImage img(out, QImage::Format_RGB32);
    const int W = out.width(), H = out.height(), SW = src.width(), SH = src.height();
    const double kDeg = 3.14159265358979323846 / 180.0;
    const double f = 0.5 * W / std::tan(std::max(10.0, std::min(150.0, fov)) * 0.5 * kDeg);
    const double cy = std::cos(yaw * kDeg), sy = std::sin(yaw * kDeg), cp = std::cos(pitch * kDeg), sp = std::sin(pitch * kDeg);
    for (int y = 0; y < H; ++y) {
        QRgb* line = (QRgb*)img.scanLine(y);
        for (int x = 0; x < W; ++x) {
            const double rx = x + 0.5 - W * 0.5, ry = H * 0.5 - (y + 0.5), rz = f;
            const double y1 = ry * cp + rz * sp, z1 = -ry * sp + rz * cp;
            const double x2 = rx * cy + z1 * sy, z2 = -rx * sy + z1 * cy;
            const double lon = std::atan2(x2, z2), lat = std::atan2(y1, std::sqrt(x2 * x2 + z2 * z2));
            double u = (lon / (2 * 3.14159265358979323846) + 0.5) * SW, v = (0.5 - lat / 3.14159265358979323846) * SH;
            int ui = (int)std::floor(u), vi = (int)std::floor(v);
            ui = ((ui % SW) + SW) % SW;
            vi = std::max(0, std::min(SH - 1, vi));
            line[x] = ((const QRgb*)src.constScanLine(vi))[ui] | 0xFF000000u;
        }
    }
    return img;
}

}  // namespace app
}  // namespace reno
