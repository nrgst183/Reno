// Video Scopes: waveform, vectorscope, histogram and RGB parade.
#include "panels_internal.h"
#include "vprefs.h"
#include "audiotools.h"
#include "../engine/colormgmt.h"
#include "../engine/captions.h"
#include "previewextras.h"
#include "../engine/timefmt.h"
#include "../engine/fxnames.h"
#include "logo.h"
#include "fxlibrary.h"
#include "fxthumbs.h"
#include "theme.h"
#include "playback.h"
#include "timeline.h"
#include "../media/media.h"
#include "lazyrefresh.h"
#include "strconv.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListWidget>
#include <QInputDialog>
#include <QDesktopServices>
#include <QProcess>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QLocale>
#include <QStyle>
#include <QTreeWidgetItemIterator>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QCursor>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QDir>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QListView>
#include <QDrag>
#include <QActionGroup>
#include <QSplitter>
#include <QFileSystemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace reno {
namespace app {

namespace {

// Rec. 709 luma and color difference of an 8-bit RGB pixel (computer RGB: 0..255).
inline double lumaOf(int r, int g, int b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }
inline double cbOf(int r, int g, int b) { return -0.1146 * r - 0.3854 * g + 0.5 * b; }
inline double crOf(int r, int g, int b) { return 0.5 * r - 0.4542 * g - 0.0458 * b; }

// CIE L*a*b* of an sRGB pixel: L 0..100, a and b about -128..127.
void labOf(int r8, int g8, int b8, double& L, double& a, double& b)
{
    auto lin = [](double c) { c /= 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const double r = lin(r8), g = lin(g8), bl = lin(b8);
    const double X = (0.4124 * r + 0.3576 * g + 0.1805 * bl) / 0.95047, Y = 0.2126 * r + 0.7152 * g + 0.0722 * bl, Z = (0.0193 * r + 0.1192 * g + 0.9505 * bl) / 1.08883;
    auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
    const double fx = f(X), fy = f(Y), fz = f(Z);
    L = 116 * fy - 16;
    a = 500 * (fx - fy);
    b = 200 * (fy - fz);
}

struct ScopeData {
    int width = 0;
    // luma, red, green, blue, composite (luma +- chroma): [column * 256 + level]
    std::vector<uint32_t> wave[5];
    // histograms: luma, red, green, blue, alpha, L* (0..255 for 0..100), a*, b* (0..255 for -128..127)
    uint32_t hist[8][256] = {};
    std::vector<uint32_t> vector;
    VideoScopes::Stats stats;
};

ScopeData analyse(const QImage& source)
{
    ScopeData d;
    if (source.isNull()) return d;
    const bool alpha = source.hasAlphaChannel();
    QImage img = source.width() > 720 ? source.scaledToWidth(720, Qt::FastTransformation) : source;
    img = img.convertToFormat(alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    d.width = w;
    for (auto& v : d.wave) v.assign((size_t)w * 256, 0);
    d.vector.assign(256 * 256, 0);
    double sum = 0, sum2 = 0;
    int lo = 255, hi = 0;
    for (int y = 0; y < h; ++y) {
        const QRgb* line = (const QRgb*)img.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            const double yv = lumaOf(r, g, b);
            const int yl = std::min(255, std::max(0, (int)std::lround(yv)));
            const int lv[4] = {yl, r, g, b};
            for (int k = 0; k < 4; ++k) {
                ++d.wave[k][(size_t)x * 256 + (size_t)lv[k]];
                ++d.hist[k][lv[k]];
            }
            ++d.hist[4][alpha ? qAlpha(line[x]) : 255];
            const double cb = cbOf(r, g, b), cr = crOf(r, g, b);
            const double c = std::sqrt(cb * cb + cr * cr);
            ++d.wave[4][(size_t)x * 256 + (size_t)std::min(255, std::max(0, (int)std::lround(yv + c)))];
            ++d.wave[4][(size_t)x * 256 + (size_t)std::min(255, std::max(0, (int)std::lround(yv - c)))];
            double L, A, B;
            labOf(r, g, b, L, A, B);
            ++d.hist[5][std::min(255, std::max(0, (int)std::lround(L * 2.55)))];
            ++d.hist[6][std::min(255, std::max(0, (int)std::lround(A + 128)))];
            ++d.hist[7][std::min(255, std::max(0, (int)std::lround(B + 128)))];
            const int cbi = std::min(255, std::max(0, (int)std::lround(cb) + 128));
            const int cri = std::min(255, std::max(0, (int)std::lround(cr) + 128));
            ++d.vector[(size_t)cri * 256 + (size_t)cbi];
            sum += yv;
            sum2 += yv * yv;
            lo = std::min(lo, yl);
            hi = std::max(hi, yl);
        }
    }
    const double n = std::max(1, w * h);
    d.stats.pixels = w * h;
    d.stats.meanLuma = sum / n;
    d.stats.stdDevLuma = std::sqrt(std::max(0.0, sum2 / n - d.stats.meanLuma * d.stats.meanLuma));
    d.stats.minLuma = (w > 0 && h > 0) ? lo : 0;
    d.stats.maxLuma = (w > 0 && h > 0) ? hi : 0;
    return d;
}

void histStats(const uint32_t* h, double& mean, double& sd)
{
    double n = 0, s = 0, s2 = 0;
    for (int v = 0; v < 256; ++v) {
        n += h[v];
        s += (double)v * h[v];
        s2 += (double)v * v * h[v];
    }
    mean = n > 0 ? s / n : 0;
    sd = n > 0 ? std::sqrt(std::max(0.0, s2 / n - mean * mean)) : 0;
}

const QColor kScopeBg(0, 0, 0), kScopeGrid(200, 160, 0, 150), kScopeText(230, 200, 60), kScopeTarget(220, 60, 220);

void drawWave(QPainter& p, const QRect& r, const std::vector<uint32_t>& counts, int columns, int rows, const QColor& tint,
              const std::function<double(int)>& level)
{
    if (columns <= 0 || r.isEmpty()) return;
    QImage img(r.size(), QImage::Format_RGB32);
    img.fill(kScopeBg);
    const double norm = std::log(1.0 + std::max(1.0, rows / 24.0));
    for (int x = 0; x < r.width(); ++x) {
        const int c0 = x * columns / r.width(), c1 = std::max(c0 + 1, (x + 1) * columns / r.width());
        for (int yv = 0; yv < 256; ++yv) {
            uint32_t n = 0;
            for (int c = c0; c < c1 && c < columns; ++c) n += counts[(size_t)c * 256 + (size_t)yv];
            if (!n) continue;
            const double a = std::min(1.0, 0.25 + 0.75 * std::log(1.0 + n / (double)(c1 - c0)) / norm);
            const int y = (int)std::lround((r.height() - 1) * (1.0 - std::min(1.0, std::max(0.0, level(yv)))));
            QRgb& px = ((QRgb*)img.scanLine(y))[x];
            const QColor old = QColor::fromRgb(px);
            px = qRgb(std::min(255, old.red() + (int)(tint.red() * a)), std::min(255, old.green() + (int)(tint.green() * a)),
                      std::min(255, old.blue() + (int)(tint.blue() * a)));
        }
    }
    p.drawImage(r.topLeft(), img);
}

class ScopeView : public QWidget {
public:
    explicit ScopeView(QWidget* parent) : QWidget(parent)
    {
        setMinimumSize(200, 140);
        setMouseTracking(true);
    }
    VideoScopes::Scope scope = VideoScopes::Waveform;
    ScopeData data;
    bool haveData = false;
    bool composite = false;
    int histogram = 0;
    bool setup75 = false, studioRgb = false;
    QString readout;

    // The waveform's scale: IRE of an 8-bit level (Studio RGB: 16 is black, 235 white; 7.5 IRE setup lifts black).
    double ire(int level) const
    {
        const double v = studioRgb ? (level - 16) / 219.0 : level / 255.0;
        return setup75 ? 7.5 + v * 92.5 : v * 100.0;
    }
    static double waveY(double ire) { return (ire + 20.0) / 140.0; }
    QRect area() const { return rect().adjusted(30, 8, -8, -8); }

protected:
    void leaveEvent(QEvent* e) override
    {
        readout.clear();
        update();
        QWidget::leaveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QRect a = area();
        const QPointF pt = e->position();
        readout.clear();
        if (haveData && a.contains(pt.toPoint())) {
            const double fy = 1.0 - (pt.y() - a.top()) / std::max(1, a.height() - 1);
            switch (scope) {
            case VideoScopes::Waveform: readout = QString("%1 IRE").arg(fy * 140.0 - 20.0, 0, 'f', 1); break;
            case VideoScopes::Parade: readout = QString("%1").arg((int)std::lround(std::min(1.0, std::max(0.0, fy)) * 255)); break;
            case VideoScopes::Histogram: {
                const double fx = (pt.x() - a.left()) / std::max(1, a.width() - 1);
                const int v = std::min(255, std::max(0, (int)std::lround(fx * 255)));
                const int chart = histogram == VideoScopes::HistLuminanceRGB ? 0 : histogram == VideoScopes::HistLab ? 5 : std::max(0, std::min(4, histogram));
                readout = QString("%1: %2 pixels").arg(v).arg(data.hist[chart][v]);
                break;
            }
            case VideoScopes::Vectorscope: {
                const int side = std::min(a.width(), a.height());
                const QPointF c(a.left() + a.width() / 2.0, a.top() + a.height() / 2.0);
                const double dx = (pt.x() - c.x()) / (side / 2.0), dy = (c.y() - pt.y()) / (side / 2.0);
                double hue = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
                if (hue < 0) hue += 360;
                readout = QString("Hue %1°  Saturation %2%").arg((int)std::lround(hue)).arg((int)std::lround(std::sqrt(dx * dx + dy * dy) * 100));
                break;
            }
            }
        }
        update();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(45, 45, 45));
        QFont f = font();
        f.setPixelSize(9);
        p.setFont(f);
        const QRect area = this->area();
        if (!haveData) {
            p.setPen(QColor(170, 170, 170));
            p.drawText(rect(), Qt::AlignCenter, "No picture");
            return;
        }
        auto levelToWave = [this](int v) { return waveY(ire(v)); };
        switch (scope) {
        case VideoScopes::Waveform: {
            p.fillRect(area, kScopeBg);
            drawWave(p, area, data.wave[composite ? 4 : 0], data.width, 256, QColor(120, 230, 120), levelToWave);
            for (int v = -20; v <= 120; v += 10) {
                const int y = (int)std::lround(area.bottom() - waveY(v) * (area.height() - 1));
                p.setPen(QPen(v == 0 || v == 100 ? kScopeText : kScopeGrid, 1, v % 20 ? Qt::DotLine : Qt::SolidLine));
                p.drawLine(area.left(), y, area.right(), y);
                if (v % 20 == 0) {
                    p.setPen(kScopeText);
                    p.drawText(QRect(0, y - 7, area.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(v));
                }
            }
            if (setup75) {
                const int y = (int)std::lround(area.bottom() - waveY(7.5) * (area.height() - 1));
                p.setPen(QPen(kScopeText, 1, Qt::DashLine));
                p.drawLine(area.left(), y, area.right(), y);
            }
            p.setPen(kScopeText);
            p.drawText(area.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignRight, composite ? "Composite" : "Luminance");
            break;
        }
        case VideoScopes::Parade: {
            const int gap = 4, w3 = (area.width() - 2 * gap) / 3;
            const QColor tints[3] = {QColor(240, 70, 70), QColor(80, 230, 80), QColor(80, 120, 255)};
            for (int k = 0; k < 3; ++k) {
                const QRect r(area.left() + k * (w3 + gap), area.top(), w3, area.height());
                p.fillRect(r, kScopeBg);
                drawWave(p, r, data.wave[k + 1], data.width, 256, tints[k], [](int v) { return v / 255.0; });
                for (int v = 0; v <= 256; v += 64) {
                    const int y = (int)std::lround(r.bottom() - std::min(255, v) / 255.0 * (r.height() - 1));
                    p.setPen(kScopeGrid);
                    p.drawLine(r.left(), y, r.right(), y);
                    if (k == 0) {
                        p.setPen(kScopeText);
                        p.drawText(QRect(0, y - 7, area.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(std::min(255, v)));
                    }
                }
            }
            break;
        }
        case VideoScopes::Vectorscope: {
            const int side = std::min(area.width(), area.height());
            const QRect r(area.left() + (area.width() - side) / 2, area.top() + (area.height() - side) / 2, side, side);
            QImage img(256, 256, QImage::Format_RGB32);
            img.fill(kScopeBg);
            for (int cr = 0; cr < 256; ++cr)
                for (int cb = 0; cb < 256; ++cb) {
                    const uint32_t n = data.vector[(size_t)cr * 256 + (size_t)cb];
                    if (!n) continue;
                    const double a = std::min(1.0, 0.3 + 0.7 * std::log(1.0 + n) / std::log(1.0 + data.stats.pixels / 200.0 + 1.0));
                    const QRgb c = qRgb((int)(230 * a), (int)(230 * a), (int)(230 * a));
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int yy = 255 - cr + dy, xx = cb + dx;
                            if (yy < 0 || yy > 255 || xx < 0 || xx > 255) continue;
                            QRgb& px = ((QRgb*)img.scanLine(yy))[xx];
                            if (qGreen(px) < qGreen(c)) px = c;
                        }
                }
            p.drawImage(r, img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(Qt::NoBrush);
            p.setPen(kScopeGrid);
            const QPointF c = QRectF(r).center();
            for (double fr : {1.0, 0.75, 0.5, 0.25}) p.drawEllipse(c, r.width() / 2.0 * fr, r.height() / 2.0 * fr);
            p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
            p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
            const struct { const char* name; int r, g, b; } bars[] = {{"R", 191, 0, 0}, {"Mg", 191, 0, 191}, {"B", 0, 0, 191},
                                                                     {"Cy", 0, 191, 191}, {"G", 0, 191, 0}, {"Yl", 191, 191, 0}};
            for (const auto& b : bars) {
                const double x = r.left() + (cbOf(b.r, b.g, b.b) + 128) * r.width() / 256.0;
                const double y = r.top() + (127 - crOf(b.r, b.g, b.b)) * r.height() / 256.0;
                p.setPen(kScopeTarget);
                p.drawRect(QRectF(x - 5, y - 5, 10, 10));
                p.drawText(QRectF(x + 6, y - 14, 24, 12), Qt::AlignLeft | Qt::AlignBottom, b.name);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
            break;
        }
        case VideoScopes::Histogram: {
            const QRect r = area.adjusted(0, 14, 0, -18);
            p.fillRect(area, kScopeBg);
            std::vector<int> charts;
            std::vector<QColor> tints;
            switch (histogram) {
            case VideoScopes::HistLuminance: charts = {0}; tints = {QColor(130, 110, 255)}; break;
            case VideoScopes::HistRed: charts = {1}; tints = {QColor(240, 60, 60)}; break;
            case VideoScopes::HistGreen: charts = {2}; tints = {QColor(60, 220, 60)}; break;
            case VideoScopes::HistBlue: charts = {3}; tints = {QColor(70, 110, 255)}; break;
            case VideoScopes::HistAlpha: charts = {4}; tints = {QColor(200, 200, 200)}; break;
            case VideoScopes::HistLuminanceRGB:
                charts = {0, 1, 2, 3};
                tints = {QColor(130, 110, 255), QColor(240, 60, 60), QColor(60, 220, 60), QColor(70, 110, 255)};
                break;
            default: charts = {5, 6, 7}; tints = {QColor(220, 220, 220), QColor(230, 90, 160), QColor(230, 200, 60)}; break;
            }
            const int n = (int)charts.size(), gapH = 4;
            const int hEach = std::max(10, (r.height() - gapH * (n - 1)) / n);
            for (int i = 0; i < n; ++i) {
                const QRect cr(r.left(), r.top() + i * (hEach + gapH), r.width(), hEach);
                const uint32_t* h = data.hist[charts[(size_t)i]];
                uint32_t top = 1;
                for (int v = 1; v < 255; ++v) top = std::max(top, h[v]);
                for (int v = 0; v < 256; v += 64) {
                    const int x = cr.left() + v * (cr.width() - 1) / 255;
                    p.setPen(QColor(200, 110, 0));
                    p.drawLine(x, cr.top(), x, cr.bottom());
                }
                QPainterPath path;
                path.moveTo(cr.left(), cr.bottom());
                for (int v = 0; v < 256; ++v) {
                    const double x = cr.left() + v * (cr.width() - 1) / 255.0;
                    const double hgt = std::min(1.0, h[v] / (double)top) * (cr.height() - 1);
                    path.lineTo(x, cr.bottom() - hgt);
                }
                path.lineTo(cr.right(), cr.bottom());
                path.closeSubpath();
                p.fillPath(path, tints[(size_t)i]);
            }
            p.setPen(kScopeText);
            const bool lab = charts.front() >= 5;
            for (int v = 0; v <= 256; v += 64) {
                const int x = r.left() + std::min(255, v) * (r.width() - 1) / 255;
                p.drawText(QRect(x - 16, area.top(), 32, 12), Qt::AlignCenter, lab ? QString::number((int)std::lround(std::min(255, v) / 2.55)) : QString::number(std::min(255, v)));
            }
            double mean = 0, sd = 0;
            histStats(data.hist[charts.front()], mean, sd);
            if (lab) { mean /= 2.55; sd /= 2.55; }
            p.drawText(QRect(area.left(), r.bottom() + 3, area.width(), 14), Qt::AlignLeft | Qt::AlignVCenter,
                       QString("Mean: %1    Std Dev: %2").arg(mean, 0, 'f', 2).arg(sd, 0, 'f', 2));
            break;
        }
        }
        if (!readout.isEmpty()) {
            p.setPen(QColor(255, 255, 255));
            p.drawText(area.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, readout);
        }
    }
};

}  // namespace

struct VideoScopes::Impl {
    Playback* pb = nullptr;
    ScopeView* view = nullptr;
    QComboBox* choice = nullptr;
    QComboBox* mode = nullptr;
    QToolButton* whilePlaying = nullptr;
    QToolButton* settings = nullptr;
    QTimer* timer = nullptr;
    qint64 lastKey = -1;
    void take(const QImage& img)
    {
        view->data = analyse(img);
        view->haveData = !img.isNull();
        view->update();
    }
    void fillMode()
    {
        const QSignalBlocker b(mode);
        mode->clear();
        if (view->scope == Waveform) {
            mode->addItem("Composite", 1);
            mode->addItem("Luminance", 0);
            mode->setCurrentIndex(view->composite ? 0 : 1);
        } else if (view->scope == Histogram) {
            const char* names[] = {"Luminance", "Red", "Green", "Blue", "Alpha", "Luminance/R/G/B", "Lightness/a/b"};
            for (int k = 0; k < 7; ++k) mode->addItem(names[k], k);
            mode->setCurrentIndex(std::max(0, mode->findData(view->histogram)));
        }
        mode->setVisible(view->scope == Waveform || view->scope == Histogram);
    }
};

VideoScopes::VideoScopes(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->pb = playback;
    setObjectName("videoScopes");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    QWidget* bar = new QWidget(this);
    bar->setFixedHeight(29);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(4, 2, 4, 2);
    h->setSpacing(4);
    d->choice = new QComboBox(bar);
    d->choice->setObjectName("scopeChoice");
    d->choice->addItem("Vectorscope", (int)Vectorscope);
    d->choice->addItem("Waveform", (int)Waveform);
    d->choice->addItem("RGB Parade", (int)Parade);
    d->choice->addItem("Histogram", (int)Histogram);
    d->mode = new QComboBox(bar);
    d->mode->setObjectName("scopeMode");
    d->whilePlaying = new QToolButton(bar);
    d->whilePlaying->setObjectName("scopesWhilePlaying");
    d->whilePlaying->setText("Update Scopes While Playing");
    d->whilePlaying->setCheckable(true);
    d->whilePlaying->setToolTip("Update Scopes While Playing (otherwise they show the frame where playing stopped)");
    d->settings = new QToolButton(bar);
    d->settings->setObjectName("scopesSettings");
    d->settings->setText("Settings");
    d->settings->setToolTip("Video Scopes Settings: 7.5 IRE setup, Studio RGB");
    h->addWidget(d->choice);
    h->addWidget(d->mode);
    h->addWidget(d->whilePlaying);
    h->addWidget(d->settings);
    h->addStretch(1);
    v->addWidget(bar);
    d->view = new ScopeView(this);
    d->view->setObjectName("scopeView");
    v->addWidget(d->view, 1);
    {
        QSettings s;
        const int sc = s.value("scopes/scope", (int)Waveform).toInt();
        d->choice->setCurrentIndex(std::max(0, d->choice->findData(sc)));
        d->view->scope = (Scope)d->choice->currentData().toInt();
        d->whilePlaying->setChecked(s.value("scopes/whilePlaying", false).toBool());
        d->view->composite = s.value("scopes/composite", false).toBool();
        d->view->histogram = std::max(0, std::min(6, s.value("scopes/histogram", (int)HistLuminance).toInt()));
        d->view->setup75 = s.value("scopes/setup75", false).toBool();
        d->view->studioRgb = s.value("scopes/studioRgb", false).toBool();
    }
    d->fillMode();
    QObject::connect(d->choice, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { setScope((Scope)d->choice->currentData().toInt()); });
    QObject::connect(d->mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (d->view->scope == Waveform) setWaveformComposite(d->mode->currentData().toInt() == 1);
        else if (d->view->scope == Histogram) setHistogramType((HistogramType)d->mode->currentData().toInt());
    });
    QObject::connect(d->whilePlaying, &QToolButton::toggled, this, [](bool on) { QSettings().setValue("scopes/whilePlaying", on); });
    QObject::connect(d->settings, &QToolButton::clicked, this, [this] {
        QDialog dlg(this);
        dlg.setObjectName("videoScopesSettings");
        dlg.setWindowTitle("Video Scopes Settings");
        auto* lv = new QVBoxLayout(&dlg);
        auto* setup = new QCheckBox("7.5 IRE Setup", &dlg);
        setup->setObjectName("setup75");
        setup->setChecked(d->view->setup75);
        setup->setToolTip("Show black at 7.5 IRE in the waveform, as an external scope does with hardware that adds NTSC setup");
        auto* studio = new QCheckBox("Studio RGB (16 to 235)", &dlg);
        studio->setObjectName("studioRgb");
        studio->setChecked(d->view->studioRgb);
        studio->setToolTip("Read 16 as black and 235 as white (studio RGB) instead of 0 and 255");
        lv->addWidget(setup);
        lv->addWidget(studio);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        lv->addWidget(bb);
        QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() == QDialog::Accepted) setDisplaySettings(setup->isChecked(), studio->isChecked());
    });
    d->timer = new QTimer(this);
    d->timer->setInterval(100);
    QObject::connect(d->timer, &QTimer::timeout, this, [this] {
        if (!isVisible() || !d->pb) return;
        if (d->pb->playing() && !d->whilePlaying->isChecked()) return;
        const QImage img = d->pb->lastFrame();
        if (img.isNull() || img.cacheKey() == d->lastKey) return;
        d->lastKey = img.cacheKey();
        d->take(img);
    });
    d->timer->start();
}

VideoScopes::~VideoScopes() {}

void VideoScopes::setScope(Scope s)
{
    d->view->scope = s;
    const QSignalBlocker b(d->choice);
    d->choice->setCurrentIndex(std::max(0, d->choice->findData((int)s)));
    QSettings().setValue("scopes/scope", (int)s);
    d->fillMode();
    d->view->update();
}

VideoScopes::Scope VideoScopes::scope() const { return d->view->scope; }

void VideoScopes::setWaveformComposite(bool on)
{
    d->view->composite = on;
    QSettings().setValue("scopes/composite", on);
    d->fillMode();
    d->view->update();
}
bool VideoScopes::waveformComposite() const { return d->view->composite; }

void VideoScopes::setHistogramType(HistogramType t)
{
    d->view->histogram = std::max(0, std::min(6, (int)t));
    QSettings().setValue("scopes/histogram", d->view->histogram);
    d->fillMode();
    d->view->update();
}
VideoScopes::HistogramType VideoScopes::histogramType() const { return (HistogramType)d->view->histogram; }

void VideoScopes::setDisplaySettings(bool setup75, bool studioRgb)
{
    d->view->setup75 = setup75;
    d->view->studioRgb = studioRgb;
    QSettings s;
    s.setValue("scopes/setup75", setup75);
    s.setValue("scopes/studioRgb", studioRgb);
    d->view->update();
}
bool VideoScopes::setup75() const { return d->view->setup75; }
bool VideoScopes::studioRgb() const { return d->view->studioRgb; }
double VideoScopes::waveformIre(int level) const { return d->view->ire(level); }

void VideoScopes::histogramStats(HistogramType t, double& mean, double& stdDev) const
{
    const int chart = t == HistLuminanceRGB ? 0 : t == HistLab ? 5 : std::max(0, std::min(4, (int)t));
    histStats(d->view->data.hist[chart], mean, stdDev);
    if (t == HistLab) { mean /= 2.55; stdDev /= 2.55; }
}

void VideoScopes::setImage(const QImage& img)
{
    d->lastKey = img.cacheKey();
    d->take(img);
}

VideoScopes::Stats VideoScopes::stats() const { return d->view->data.stats; }

}  // namespace app
}  // namespace reno
