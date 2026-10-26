#include "builtintext.h"
#include "../ofx/builtins.h"
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace reno {
namespace app {

// Draws the text lines white on a transparent image just big enough for them and returns its alpha. Safe from any thread
// (renders call it).
static bool rasterize(const ofx::TextRequest& req, ofx::TextMask& out)
{
    const QStringList lines = QString::fromUtf8(req.text.data(), (qsizetype)req.text.size()).split('\n');
    QFont font(QString::fromUtf8(req.font.c_str()));
    const double px = std::min(4000.0, std::max(1.0, req.pixelSize));
    font.setPixelSize(std::max(1, (int)std::lround(px)));
    font.setBold(req.bold);
    font.setItalic(req.italic);
    font.setUnderline(req.underline);
    if (std::fabs(req.tracking) > 1e-6) font.setLetterSpacing(QFont::AbsoluteSpacing, req.tracking * px);
    font.setStyleStrategy(QFont::PreferAntialias);
    const QFontMetricsF fm(font);
    const double lineH = std::max(1.0, fm.lineSpacing() * req.lineSpacing);
    double w = 1;
    for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l));
    // room for italic overhang and the generator's outline
    const double pad = std::ceil(px * 0.15);
    const double h = lineH * (lines.size() - 1) + fm.height();
    const int W = (int)std::ceil(w + 2 * pad), H = (int)std::ceil(h + 2 * pad);
    if (W <= 0 || H <= 0 || (double)W * H > 64.0 * 1024 * 1024) return false;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) return false;
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(font);
        p.setPen(Qt::white);
        for (int k = 0; k < lines.size(); ++k) {
            const double lw = fm.horizontalAdvance(lines[k]);
            const double x = pad + (req.align == 0 ? 0.0 : (req.align == 2 ? w - lw : (w - lw) * 0.5));
            p.drawText(QPointF(x, pad + k * lineH + fm.ascent()), lines[k]);
        }
    }
    out.w = W;
    out.h = H;
    out.a.assign((size_t)W * H, 0);
    for (int y = 0; y < H; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < W; ++x) out.a[(size_t)y * W + x] = (uint8_t)qAlpha(row[x]);
    }
    return true;
}

void installBuiltinTextRasterizer() { ofx::setTextRasterizer(rasterize); }

}  // namespace app
}  // namespace reno
