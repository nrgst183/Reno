#include "builtintext.h"
#include "../ofx/builtins.h"
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
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

// The generator's own text layout (vfxtitles.h) takes outlines and metrics from here where the engine has no GDI: a Qt font at 2048 pixels to the em, unhinted.
class QtFace : public ofx::TextFace {
public:
    QtFace(const std::string& family, bool bold, bool italic) : font_(pickFamily(QString::fromUtf8(family.c_str())))
    {
        font_.setPixelSize(kEm);
        font_.setBold(bold);
        font_.setItalic(italic);
        font_.setHintingPreference(QFont::PreferNoHinting);
        font_.setKerning(true);
        const QFontMetricsF fm(font_);
        ascent = fm.ascent() / kEm;
        descent = fm.descent() / kEm;
        lineGap = std::max(0.0, fm.leading() / kEm);
        // A stand-in font (the machine has no Verdana, Arial or Tahoma) keeps the line metrics of the font that was asked for, so the text sits where it would with the real one.
        const QString want = QString::fromUtf8(family.c_str()).trimmed().toLower();
        if (QFontInfo(font_).family().toLower() != want) {
            if (want == "verdana") { ascent = 2059.0 / 2048; descent = 430.0 / 2048; lineGap = 0; }
            else if (want == "arial") { ascent = 1854.0 / 2048; descent = 434.0 / 2048; lineGap = 67.0 / 2048; }
            else if (want == "tahoma") { ascent = 2049.0 / 2048; descent = 423.0 / 2048; lineGap = 0; }
        }
    }
    bool glyph(uint32_t cp, ofx::TextGlyph& g) override
    {
        g = ofx::TextGlyph();
        const QString ch = QString::fromUcs4(reinterpret_cast<const char32_t*>(&cp), 1);
        const QFontMetricsF fm(font_);
        g.adv = fm.horizontalAdvance(ch) / kEm;
        QPainterPath p;
        p.addText(0, 0, font_, ch);
        for (int i = 0; i < p.elementCount(); ++i) {
            const QPainterPath::Element e = p.elementAt(i);
            ofx::TextSeg s;
            if (e.type == QPainterPath::MoveToElement) {
                if (!g.segs.empty()) { ofx::TextSeg z; z.op = 'Z'; g.segs.push_back(z); }
                s.op = 'M';
                s.p[0] = (float)(e.x / kEm); s.p[1] = (float)(-e.y / kEm);
            } else if (e.type == QPainterPath::LineToElement) {
                s.op = 'L';
                s.p[0] = (float)(e.x / kEm); s.p[1] = (float)(-e.y / kEm);
            } else if (e.type == QPainterPath::CurveToElement && i + 2 < p.elementCount()) {
                const QPainterPath::Element c2 = p.elementAt(i + 1), c3 = p.elementAt(i + 2);
                s.op = 'C';
                s.p[0] = (float)(e.x / kEm); s.p[1] = (float)(-e.y / kEm);
                s.p[2] = (float)(c2.x / kEm); s.p[3] = (float)(-c2.y / kEm);
                s.p[4] = (float)(c3.x / kEm); s.p[5] = (float)(-c3.y / kEm);
                i += 2;
            } else continue;
            g.segs.push_back(s);
        }
        if (!g.segs.empty()) { ofx::TextSeg z; z.op = 'Z'; g.segs.push_back(z); }
        return true;
    }
    double kern(uint32_t a, uint32_t b) override
    {
        const QFontMetricsF fm(font_);
        const QString sa = QString::fromUcs4(reinterpret_cast<const char32_t*>(&a), 1), sb = QString::fromUcs4(reinterpret_cast<const char32_t*>(&b), 1);
        return (fm.horizontalAdvance(sa + sb) - fm.horizontalAdvance(sa) - fm.horizontalAdvance(sb)) / kEm;
    }

private:
    // The family if this machine has it, else the first sans-serif one of a short list (Verdana and Arial are not on Linux or macOS).
    static QString pickFamily(const QString& want)
    {
        const QStringList have = QFontDatabase::families();
        if (have.contains(want, Qt::CaseInsensitive)) return want;
        const QString w = want.toLower();
        for (const char* k : {"palatino", "times", "georgia", "cambria", "garamond", "book antiqua", "bookman", "century", "baskerville", "constantia", "serif"})
            if (w.contains(QString::fromLatin1(k)))
                for (const char* f : {"DejaVu Serif", "Liberation Serif", "Noto Serif", "Times New Roman", "Times"})
                    if (have.contains(QString::fromLatin1(f), Qt::CaseInsensitive)) return QString::fromLatin1(f);
        for (const char* f : {"DejaVu Sans", "Liberation Sans", "Noto Sans", "Helvetica Neue", "Helvetica", "Arial"})
            if (have.contains(QString::fromLatin1(f), Qt::CaseInsensitive)) return QString::fromLatin1(f);
        return want;
    }
    static constexpr double kEm = 2048.0;
    QFont font_;
};

void installBuiltinTextRasterizer()
{
    ofx::setTextRasterizer(rasterize);
    ofx::setTextFaceSource([](const std::string& family, bool bold, bool italic) -> std::shared_ptr<ofx::TextFace> {
        return std::make_shared<QtFace>(family, bold, italic);
    });
}

}  // namespace app
}  // namespace reno
