// The pages of Reno's built-in audio effects: see fxpages.h.
#include "fxpages.h"
#include "../audiofx/vegasdistortion.h"
#include "../audiofx/vegaspack1.h"
#include "../audiofx/vegas_trkfx.h"
#include "../audiofx/vegaspack3_all.h"
#include "../audiofx/vegasfx_pack2.h"
#include "fxcurvegraph.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QGroupBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QLinearGradient>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPolygonF>
#include <QImage>
#include <QDir>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionButton>
#include <QSlider>
#include <QTimer>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace reno {
namespace app {

namespace {

const double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------------------------------ small widgets
// VEGAS's DirectX pages are Windows-classic: a trackbar is a thin sunken groove with a small grey thumb (style sheet on
// the slider, so it looks the same under either of Reno's looks).
class DxSlider : public QSlider {
public:
    DxSlider(Qt::Orientation o, QWidget* parent) : QSlider(o, parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setTickPosition(QSlider::NoTicks);
        const bool horizontal = o == Qt::Horizontal;
        setStyleSheet(horizontal
                          ? "QSlider::groove:horizontal { height: 2px; background: #9a9a9a; border: 1px solid #c8c8c8; border-top-color: #7a7a7a; border-left-color: #7a7a7a; }"
                            "QSlider::handle:horizontal { background: #f0f0f0; border: 1px solid #6c6c6c; border-bottom-color: #3c3c3c; border-right-color: #3c3c3c; width: 7px; margin: -7px 0; }"
                            "QSlider::handle:horizontal:hover { background: #ffffff; }"
                            "QSlider::groove:horizontal:disabled { background: #cfcfcf; border-color: #dcdcdc; }"
                            "QSlider::handle:horizontal:disabled { background: #e6e6e6; border-color: #b4b4b4; }"
                          : "QSlider::groove:vertical { width: 2px; background: #9a9a9a; border: 1px solid #c8c8c8; border-top-color: #7a7a7a; border-left-color: #7a7a7a; }"
                            "QSlider::handle:vertical { background: #f0f0f0; border: 1px solid #6c6c6c; border-bottom-color: #3c3c3c; border-right-color: #3c3c3c; height: 18px; margin: 0 -6px; }"
                            "QSlider::handle:vertical:hover { background: #ffffff; }"
                            "QSlider::groove:vertical:disabled { background: #cfcfcf; border-color: #dcdcdc; }"
                            "QSlider::handle:vertical:disabled { background: #e6e6e6; border-color: #b4b4b4; }");
    }

protected:
    void wheelEvent(QWheelEvent* e) override { e->ignore(); }
};

// A drawing in a white box with a grid. Mouse positions are passed on as fractions of the plot (x from the left, y from
// the bottom), outside 0..1 when the mouse is outside.
class Graph : public QWidget {
public:
    std::function<void(QPainter&, const QRectF& plot)> draw;
    std::function<void(const QPointF& at, int phase)> mouse;
    QMargins margins = QMargins(30, 6, 8, 18);

    explicit Graph(QWidget* parent) : QWidget(parent) {}
    QRectF plot() const { return QRectF(rect().marginsRemoved(margins)); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), QColor(240, 240, 240));    // (the page's own face: the application style sheet would give the palette the dark theme)
        const QRectF pl = plot();
        p.fillRect(pl, Qt::white);
        p.setFont(font());
        p.setPen(Qt::black);
        if (draw) draw(p, pl);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !mouse) return;
        down_ = true;
        mouse(frac(e->position()), 0);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (down_ && mouse) mouse(frac(e->position()), 1);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !down_) return;
        down_ = false;
        if (mouse) mouse(frac(e->position()), 2);
    }

private:
    QPointF frac(const QPointF& at) const
    {
        const QRectF pl = plot();
        return QPointF((at.x() - pl.left()) / std::max(1.0, pl.width()), 1.0 - (at.y() - pl.top()) / std::max(1.0, pl.height()));
    }
    bool down_ = false;
};

// VEGAS's envelope graph (Distortion): points joined by straight lines on a grid, levels in tenths of a dB from
// -840 (-Inf.) to 0 on both axes. Click the line to add a point (up to 32) and drag it; drag a point to move it
// between its neighbours (the end points only up and down); double-click or right-click a point to delete it.
class EnvelopeGraph : public QWidget {
public:
    std::vector<vegdist::Point> pts;
    std::function<void(int phase)> edited;      // 0 start of a drag, 1 during, 2 end (a delete: 0 then 2)
    QLabel* readout = nullptr;
    QMargins margins = QMargins(32, 4, 8, 16);
    size_t maxPts = (size_t)vegdist::kMaxPoints;                    // the most points the graph takes
    std::function<QString(const vegdist::Point&)> format;           // the text of the readout for a point (else "In .. dB, out .. dB")

    explicit EnvelopeGraph(QWidget* parent) : QWidget(parent) { setMouseTracking(true); }
    bool dragging() const { return drag_ >= 0; }

protected:
    QRectF plot() const { return QRectF(rect().marginsRemoved(margins)); }
    QPointF at(const vegdist::Point& p) const
    {
        const QRectF pl = plot();
        return QPointF(pl.left() + (p.x + 840) / 840.0 * pl.width(), pl.bottom() - (p.y + 840) / 840.0 * pl.height());
    }
    vegdist::Point level(const QPointF& q) const
    {
        const QRectF pl = plot();
        const double x = (q.x() - pl.left()) / std::max(1.0, pl.width()) * 840 - 840;
        const double y = (pl.bottom() - q.y()) / std::max(1.0, pl.height()) * 840 - 840;
        return vegdist::Point{(int)std::lround(std::max(-840.0, std::min(0.0, x))), (int)std::lround(std::max(-840.0, std::min(0.0, y)))};
    }
    int hit(const QPointF& q) const
    {
        for (size_t k = 0; k < pts.size(); ++k) {
            const QPointF c = at(pts[k]);
            if (std::fabs(q.x() - c.x()) <= 5 && std::fabs(q.y() - c.y()) <= 5) return (int)k;
        }
        return -1;
    }
    int onLine(const QPointF& q) const      // the segment under q (its first point), or -1
    {
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const QPointF a = at(pts[k]), b = at(pts[k + 1]);
            const QPointF d = b - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            double t = len2 > 0 ? ((q.x() - a.x()) * d.x() + (q.y() - a.y()) * d.y()) / len2 : 0;
            t = std::max(0.0, std::min(1.0, t));
            const QPointF c = a + t * d;
            if (std::hypot(q.x() - c.x(), q.y() - c.y()) <= 4) return (int)k;
        }
        return -1;
    }
    static QString db(int v) { return v <= -840 ? QString("-Inf.") : QString::number(v / 10.0, 'f', 1); }
    void show(const vegdist::Point& p)
    {
        if (!readout) return;
        readout->setText(format ? format(p) : QString("In %1 dB, out %2 dB").arg(db(p.x), db(p.y)));
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(240, 240, 240));    // (the page's own face: the application style sheet would give the palette the dark theme)
        const QRectF pl = plot();
        p.fillRect(pl, Qt::white);
        auto X = [&](double v) { return pl.left() + (v + 84) / 84 * pl.width(); };
        auto Y = [&](double v) { return pl.bottom() - (v + 84) / 84 * pl.height(); };
        p.setPen(QPen(QColor(70, 70, 70), 1));
        for (int k = 0; k <= 7; ++k) {
            const double v = -84 + k * 12;
            p.drawLine(QPointF(std::round(X(v)) + 0.5, pl.top()), QPointF(std::round(X(v)) + 0.5, pl.bottom()));
            p.drawLine(QPointF(pl.left(), std::round(Y(v)) + 0.5), QPointF(pl.right(), std::round(Y(v)) + 0.5));
        }
        p.setPen(Qt::black);
        for (int k = 0; k <= 7; ++k) {
            const double v = -84 + k * 12;
            const QString t = k == 0 ? QString("-Inf.") : QString::number((int)v);
            p.drawText(QRectF(X(v) - 20, pl.bottom() + 1, 40, 14), Qt::AlignHCenter | Qt::AlignTop, t);
            p.drawText(QRectF(0, Y(v) - 7, pl.left() - 2, 14), Qt::AlignRight | Qt::AlignVCenter, t);
        }
        p.setClipRect(pl.adjusted(-4, -4, 4, 4));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(Qt::black, 1));
        for (size_t k = 0; k + 1 < pts.size(); ++k) p.drawLine(at(pts[k]), at(pts[k + 1]));
        p.setRenderHint(QPainter::Antialiasing, false);
        for (size_t k = 0; k < pts.size(); ++k) {
            const QPointF c = at(pts[k]);
            p.setBrush((int)k == drag_ ? Qt::black : Qt::white);
            p.drawRect(QRectF(std::round(c.x()) - 2.5, std::round(c.y()) - 2.5, 5, 5));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const QPointF q = e->position();
        int k = hit(q);
        if (e->button() == Qt::RightButton) {
            remove(k);
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        if (k < 0) {
            const int seg = onLine(q);
            if (seg < 0 || pts.size() >= maxPts) return;
            vegdist::Point np = level(q);
            np.x = std::max(pts[(size_t)seg].x, std::min(pts[(size_t)seg + 1].x, np.x));
            pts.insert(pts.begin() + seg + 1, np);
            k = seg + 1;
        }
        drag_ = k;
        show(pts[(size_t)k]);
        if (edited) edited(0);
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (drag_ < 0) {
            const int k = hit(e->position());
            if (k >= 0) show(pts[(size_t)k]);
            else if (readout) readout->clear();
            setCursor(k >= 0 || onLine(e->position()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            return;
        }
        vegdist::Point np = level(e->position());
        const size_t k = (size_t)drag_;
        if (k == 0) np.x = vegdist::kInf;
        else if (k + 1 == pts.size()) np.x = 0;
        else np.x = std::max(pts[k - 1].x, std::min(pts[k + 1].x, np.x));
        if (np.x == pts[k].x && np.y == pts[k].y) return;
        pts[k] = np;
        show(np);
        if (edited) edited(1);
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || drag_ < 0) return;
        drag_ = -1;
        if (edited) edited(2);
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) remove(hit(e->position()));
    }
    void leaveEvent(QEvent*) override
    {
        if (drag_ < 0 && readout) readout->clear();
    }

private:
    void remove(int k)
    {
        if (k <= 0 || k + 1 >= (int)pts.size()) return;     // the end points stay
        drag_ = -1;
        pts.erase(pts.begin() + k);
        if (edited) {
            edited(0);
            edited(2);
        }
        update();
    }
    int drag_ = -1;
};

// 12345.6 as "12,345.6" (VEGAS's page edit boxes group thousands).
QString groupedNumber(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    const int dot = s.indexOf('.');
    const int end = dot < 0 ? (int)s.size() : dot;
    const int start = s.startsWith('-') ? 1 : 0;
    for (int i = end - 3; i > start; i -= 3) s.insert(i, ',');
    return s;
}

// A number in a range's caption, with no more digits than needed.
QString plainNumber(double v)
{
    QString s = QString::number(v, 'f', std::fabs(v - std::round(v)) < 1e-9 ? 0 : 2);
    return groupedNumber(s.toDouble(), std::fabs(v - std::round(v)) < 1e-9 ? 0 : 2);
}

bool parseNumber(QString t, double* out)
{
    t = t.trimmed();
    t.remove(',');
    int n = 0;
    while (n < t.size() && (t[n].isDigit() || t[n] == '.' || ((t[n] == '-' || t[n] == '+') && n == 0) || t[n] == 'e' || t[n] == 'E')) ++n;
    bool ok = false;
    const double v = t.left(n).toDouble(&ok);
    if (ok && std::isfinite(v)) {
        *out = v;
        return true;
    }
    return false;
}

// What a slider and its boxes show.
struct SMap {
    bool log = false;
    int dec = 1;
    bool inf = false;
    bool comma = true;
    bool noGap = false;     // a readout's unit follows the number with no space ("100%")
    double shown = 1;   // the boxes show the value times this (seconds for a setting kept in ms)
    int stepCount = 0;  // the slider's number of steps when it is not the usual 1000 (a trackbar of the plug-in that has one step per unit)
};


// The tick of a checked box: a small image made once in the temp folder (a style sheet can only take a file for it).
QString checkImage()
{
    static QString path;
    if (path.isEmpty()) {
        QImage im(11, 11, QImage::Format_ARGB32);
        im.fill(Qt::white);
        QPainter p(&im);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::black, 1.6));
        p.drawPolyline(QPolygonF({QPointF(2, 5.5), QPointF(4.5, 8), QPointF(9, 2.5)}));
        p.end();
        path = QDir::temp().filePath("reno_fx_check.png");
        im.save(path, "PNG");
    }
    return path;
}

typedef std::function<int()> Ix;
Ix fixed(int i) { return [i] { return i; }; }

// ------------------------------------------------------------------------------------------ the page
#include "fxpages_pack3_graph.inc"

// A level meter as VEGAS's pages have them: one or two bars (the channels) filled to a level, green into yellow into red,
// with a slow fall back. Level meters show dB between 'lo' and 0 (the bar grows up or to the right); a reduction meter
// shows how many dB are taken off, 0 .. -'lo' (the bar grows down or to the left of the full bar). The page feeds it.
class LevelMeter : public QWidget {
public:
    LevelMeter(QWidget* parent, bool vertical, int channels, double lo, bool reduction)
        : QWidget(parent), vertical_(vertical), reduction_(reduction), lo_(lo), shown_((size_t)std::max(1, channels), reduction ? 0.0 : lo)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    // a new reading in dB (reduction meters: a positive number of dB taken off); falls by 'fall' dB a call
    void feed(int ch, double db, double fall = 1.2)
    {
        if (ch < 0 || ch >= (int)shown_.size()) return;
        double& v = shown_[(size_t)ch];
        if (reduction_) v = std::max(db, v - fall);
        else v = std::max(db, v - fall);
        v = std::max(reduction_ ? 0.0 : lo_, std::min(reduction_ ? -lo_ : 0.0, v));
        update();
    }
    double shown(int ch) const { return ch >= 0 && ch < (int)shown_.size() ? shown_[(size_t)ch] : 0.0; }
    void clear()
    {
        for (double& v : shown_) v = reduction_ ? 0.0 : lo_;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(24, 24, 24));
        const int n = (int)shown_.size();
        const QRectF in = QRectF(rect()).adjusted(1, 1, -1, -1);
        for (int c = 0; c < n; ++c) {
            QRectF bar = in;
            if (vertical_) {
                const double w = in.width() / n;
                bar = QRectF(in.left() + c * w + 0.5, in.top(), w - 1, in.height());
            } else {
                const double h = in.height() / n;
                bar = QRectF(in.left(), in.top() + c * h + 0.5, in.width(), h - 1);
            }
            p.fillRect(bar, QColor(44, 44, 44));
            const double range = reduction_ ? -lo_ : -lo_;
            const double f = reduction_ ? shown_[(size_t)c] / range : (shown_[(size_t)c] - lo_) / range;
            const double fr = std::max(0.0, std::min(1.0, f));
            QRectF fill = bar;
            if (vertical_) {
                const double h = bar.height() * fr;
                fill = reduction_ ? QRectF(bar.left(), bar.top(), bar.width(), h) : QRectF(bar.left(), bar.bottom() - h, bar.width(), h);
            } else {
                const double w = bar.width() * fr;
                fill = reduction_ ? QRectF(bar.right() - w, bar.top(), w, bar.height()) : QRectF(bar.left(), bar.top(), w, bar.height());
            }
            // colours by position along the scale: green, yellow from -12 dB, red from -3 dB (reduction: all orange)
            if (reduction_) {
                p.fillRect(fill, QColor(230, 140, 30));
            } else {
                QLinearGradient g(vertical_ ? QPointF(0, bar.bottom()) : QPointF(bar.left(), 0),
                                  vertical_ ? QPointF(0, bar.top()) : QPointF(bar.right(), 0));
                g.setColorAt(0.0, QColor(30, 190, 60));
                g.setColorAt(std::max(0.0, std::min(1.0, (-12.0 - lo_) / range)), QColor(60, 200, 60));
                g.setColorAt(std::max(0.0, std::min(1.0, (-6.0 - lo_) / range)), QColor(230, 210, 40));
                g.setColorAt(1.0, QColor(235, 50, 40));
                p.fillRect(fill, g);
            }
        }
        p.setPen(QColor(90, 90, 90));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    bool vertical_, reduction_;
    double lo_;
    std::vector<double> shown_;
};

// The small up/down spinner some pages have beside a number box (VEGAS's own "SpinnerClass" control): two arrow buttons stacked, a click on the upper one
// adds a step to the setting, on the lower one takes one off.
class SpinArrows : public QWidget {
public:
    std::function<void(int)> step;     // +1 or -1
    explicit SpinArrows(QWidget* parent) : QWidget(parent) { setFocusPolicy(Qt::NoFocus); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const int h2 = height() / 2;
        for (int k = 0; k < 2; ++k) {
            QStyleOptionButton o;
            o.initFrom(this);
            o.rect = QRect(0, k ? h2 : 0, width(), k ? height() - h2 : h2);
            o.state = (isEnabled() ? QStyle::State_Enabled : QStyle::State_None) | (down_ == k ? QStyle::State_Sunken : QStyle::State_Raised);
            style()->drawControl(QStyle::CE_PushButton, &o, &p, this);
            QStyleOption a;
            a.initFrom(this);
            a.rect = o.rect.adjusted(1, 0, -1, 0);
            a.state = o.state;
            style()->drawPrimitive(k ? QStyle::PE_IndicatorArrowDown : QStyle::PE_IndicatorArrowUp, &a, &p, this);
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        down_ = e->position().y() < height() / 2 ? 0 : 1;
        update();
        if (step) step(down_ == 0 ? 1 : -1);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        down_ = -1;
        update();
    }

private:
    int down_ = -1;
};

class Page : public DxPage {
public:
    Page(QWidget* parent, const std::string& uid, const DxHost& host) : DxPage(parent), h_(host)
    {
        setObjectName("fxDxPage");
        setAutoFillBackground(true);
        QPalette pal;
        const QColor face(240, 240, 240);
        pal.setColor(QPalette::Window, face);
        pal.setColor(QPalette::WindowText, Qt::black);
        pal.setColor(QPalette::Base, Qt::white);
        pal.setColor(QPalette::AlternateBase, QColor(233, 233, 233));
        pal.setColor(QPalette::Text, Qt::black);
        pal.setColor(QPalette::Button, QColor(225, 225, 225));
        pal.setColor(QPalette::ButtonText, Qt::black);
        pal.setColor(QPalette::BrightText, Qt::white);
        pal.setColor(QPalette::Highlight, QColor(0, 120, 215));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Light, Qt::white);
        pal.setColor(QPalette::Midlight, QColor(227, 227, 227));
        pal.setColor(QPalette::Mid, QColor(160, 160, 160));
        pal.setColor(QPalette::Dark, QColor(105, 105, 105));
        pal.setColor(QPalette::Shadow, QColor(70, 70, 70));
        pal.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
        pal.setColor(QPalette::ToolTipBase, QColor(255, 255, 225));
        pal.setColor(QPalette::ToolTipText, Qt::black);
        for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) pal.setColor(QPalette::Disabled, r, QColor(109, 109, 109));
        pal.setColor(QPalette::Disabled, QPalette::Base, face);
        pal.setColor(QPalette::Disabled, QPalette::Button, QColor(204, 204, 204));
        setPalette(pal);
        // The application's dark look sets text colours by style sheet, which would override the palette: set them again here.
        setStyleSheet(QString("QLabel { background: transparent; } QLabel, QCheckBox, QRadioButton { color: black; } QLabel:disabled, QCheckBox:disabled, QRadioButton:disabled { color: #6d6d6d; } "
                      "QRadioButton::indicator { width: 11px; height: 11px; border-radius: 6px; border: 1px solid #333333; background: white; } QRadioButton::indicator:checked { background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 black, stop:0.45 black, stop:0.5 white, stop:1 white); } QRadioButton::indicator:disabled { border-color: #aaaaaa; background: #f0f0f0; } QCheckBox::indicator { width: 11px; height: 11px; border: 1px solid #333333; background: white; } QCheckBox::indicator:checked { image: url(%1); } QCheckBox::indicator:disabled { border-color: #aaaaaa; background: #f0f0f0; } QLineEdit { background: white; color: black; border: 1px solid #7a7a7a; selection-background-color: #0078d7; selection-color: white; } QLineEdit:disabled { background: #f0f0f0; color: #6d6d6d; border-color: #bcbcbc; }").arg(checkImage()));
        QFont f = QApplication::font();
#ifdef _WIN32
        f.setFamily("Segoe UI");
        f.setPointSizeF(9);
#else
        f.setPixelSize(12);
#endif
        setFont(f);

        root_ = new QVBoxLayout(this);
        root_->setContentsMargins(8, 6, 8, 8);
        root_->setSpacing(4);
        int width = 430;
        if (uid == "reno.distortion") { distortion(); width = 520; }
        else if (uid == "reno.vibrato") vibrato();
        else if (uid == "reno.pitchshift") pitchShift();
        else if (uid == "reno.timestretch") timeStretch(false);
        else if (uid == "reno.xptimestretch") timeStretch(true);
        else if (uid == "reno.chorus") chorus();
        else if (uid == "reno.flangewah") flangeWah();
        else if (uid == "reno.ampmod") ampMod();
        else if (uid == "reno.simpledelay") simpleDelay();
        else if (uid == "reno.multitapdelay") multiTap();
        else if (uid == "reno.reverb") reverb();
        else if (uid == "reno.moderneq") { modernEq(); width = 560; }
        else if (trkPage(uid, &width)) {}
        else if (uid == "reno.smooth") smoothEnhance();
        else if (uid == "reno.gapsnip") gapperSnipper();
        else if (xp2Page(uid, &width)) {}         // the ExpressFX reproductions of sfxpfx2 (fxpages_xp2.inc)
        else if (xp3Page(uid, &width)) {}          // the ExpressFX effects of sfxpfx3 (fxpages_xp3.inc)
        else if (pack2Page(uid, &width)) {}       // the sfppack2 / sfresfilter reproductions (fxpages_pack2.inc)
        else if (xp1Page(uid, &width)) {}         // the ExpressFX effects of sfxpfx1 (fxpages_xp1.inc)
        layout()->activate();
        const QSize s = sizeHint();
        setFixedSize(std::max(width, s.width()), s.height());
        syncAll();
    }

    void refresh() override { syncAll(); }

    // Meters: pollMeters() reads the plug-in's meters (about 25 times a second while the page is shown) and feeds the bars.
    void addMeterPoll(std::function<void()> f)
    {
        meterPolls_.push_back(std::move(f));
        if (!meterTimer_) {
            meterTimer_ = new QTimer(this);
            meterTimer_->setInterval(40);
            QObject::connect(meterTimer_, &QTimer::timeout, this, [this] { pollMeters(); });
            meterTimer_->start();
        }
    }
    void pollMeters()
    {
        if (!h_.meter || !isVisible()) return;
        for (auto& f : meterPolls_) f();
    }
    // linear peak -> dB for a meter ('lo' for silence)
    static double meterDb(float lin, double lo)
    {
        if (!(lin > 0.f)) return lo;
        return std::max(lo, 20.0 * std::log10((double)lin));
    }

private:
    DxHost h_;
    std::vector<std::function<void()>> meterPolls_;
    QTimer* meterTimer_ = nullptr;
    QVBoxLayout* root_ = nullptr;
    bool updating_ = false;
    std::vector<std::function<void()>> sync_;
    std::vector<QWidget*> graphs_;

    struct Rng { double lo = 0, hi = 1; };
    Rng rng(int i) const
    {
        Rng r;
        if (!h_.range || !h_.range(i, &r.lo, &r.hi) || !(r.hi > r.lo)) { r.lo = 0; r.hi = 1; }
        return r;
    }
    double get(int i) const { return h_.get ? h_.get(i) : 0.0; }
    double clampTo(int i, double v) const
    {
        const Rng r = rng(i);
        return std::max(r.lo, std::min(r.hi, v));
    }
    void commit(int i, double v)
    {
        if (updating_ || !h_.set) return;
        h_.set(i, clampTo(i, v));
        syncAll();
    }
    void syncAll()
    {
        const bool was = updating_;
        updating_ = true;
        for (auto& f : sync_) f();
        for (QWidget* g : graphs_) g->update();
        updating_ = was;
    }
    void beginDrag(int i)
    {
        if (h_.begin) h_.begin(i);
    }
    void endDrag()
    {
        if (h_.end) h_.end();
    }

    int steps(const SMap& m, const Rng& r) const { if (m.stepCount > 0) return m.stepCount; return (m.dec == 0 && r.hi - r.lo <= 2000 && !m.log) ? (int)std::lround(r.hi - r.lo) : 1000; }
    int toPos(double v, const SMap& m, const Rng& r) const
    {
        double t;
        if (m.log && r.lo > 0) t = std::log(std::max(v, r.lo) / r.lo) / std::log(r.hi / r.lo);
        else t = (v - r.lo) / (r.hi - r.lo);
        return (int)std::lround(std::max(0.0, std::min(1.0, t)) * steps(m, r));
    }
    double fromPos(int pos, const SMap& m, const Rng& r) const
    {
        const double t = std::max(0.0, std::min(1.0, (double)pos / steps(m, r)));
        if (m.log && r.lo > 0) return r.lo * std::pow(r.hi / r.lo, t);
        return r.lo + t * (r.hi - r.lo);
    }
    QString edit(double v, const SMap& m, const Rng& r) const
    {
        if (m.inf && v <= r.lo + 0.05) return "-Inf.";
        v *= m.shown;
        return m.comma ? groupedNumber(v, m.dec) : QString::number(v, 'f', m.dec);
    }
    QString rangeText(int i, const SMap& m, const QString& unit) const
    {
        const Rng r = rng(i);
        const QString lo = m.inf ? QString("-Inf.") : plainNumber(r.lo);
        return QString("(%1 to %2%3)").arg(lo, plainNumber(r.hi), unit.isEmpty() ? QString() : " " + unit);
    }

    // ---- the controls
    // A slider for setting ix(), with an edit box and/or readout showing its value.
    DxSlider* slider(Qt::Orientation o, Ix ix, const SMap& m, const QString& name, QLineEdit* box, QLabel* readout, const QString& readUnit = QString())
    {
        DxSlider* s = new DxSlider(o, this);
        s->setObjectName(name);
        s->setRange(0, 1000);
        QObject::connect(s, &QSlider::valueChanged, s, [=](int pos) {
            if (updating_) return;
            commit(ix(), fromPos(pos, m, rng(ix())));
        });
        QObject::connect(s, &QSlider::sliderPressed, s, [=] { beginDrag(ix()); });
        QObject::connect(s, &QSlider::sliderReleased, s, [=] { endDrag(); });
        if (box) {
            box->setObjectName(name + "Edit");
            QObject::connect(box, &QLineEdit::editingFinished, box, [=] {
                if (updating_) return;
                const int i = ix();
                const Rng r = rng(i);
                if (box->text() == edit(get(i), m, r)) return;
                double v = 0;
                if (m.inf && box->text().trimmed().startsWith("-inf", Qt::CaseInsensitive)) v = r.lo;
                else if (!parseNumber(box->text(), &v)) { syncAll(); return; }
                else v /= m.shown;
                commit(i, v);
            });
        }
        sync_.push_back([=] {
            const int i = ix();
            const Rng r = rng(i);
            const double v = get(i);
            QSignalBlocker b(s);
            s->setRange(0, steps(m, r));
            if (!s->isSliderDown()) s->setValue(toPos(v, m, r));
            if (box && !box->hasFocus()) box->setText(edit(v, m, r));
            if (readout) readout->setText(m.inf && v <= r.lo + 0.05 ? QString("-Inf.") : QString::number(std::fabs(v) < 0.5 * std::pow(10.0, -m.dec) ? 0.0 : v, 'f', m.dec) + (readUnit.isEmpty() ? QString() : (m.noGap ? QString() : QString(" ")) + readUnit));
        });
        return s;
    }

    QLineEdit* editBox(int width = 58)
    {
        QLineEdit* e = new QLineEdit(this);
        e->setFixedSize(width, 21);
        e->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return e;
    }

    // VEGAS's faders have a label of one digit ("&1") in front of them that Alt+digit goes through; it is hidden behind the fader
    static void digitMnemonic(QWidget* fader, int index)
    {
        QLabel* l = new QLabel(QString("&%1").arg((index + 1) % 10), fader);
        l->setBuddy(fader);
        l->setAttribute(Qt::WA_TransparentForMouseEvents);
        l->setStyleSheet("background: transparent; color: transparent;");
        l->setGeometry(0, 0, 8, 8);
    }

    QLabel* text(const QString& t, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter)
    {
        QLabel* l = new QLabel(t, this);
        l->setTextFormat(Qt::RichText);
        l->setAlignment(a);
        return l;
    }

    QWidget* gainColumn(int i, const QString& name, bool inf = true, int colW = 84)
    {
        QWidget* col = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(col);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(3);
        SMap m;
        m.inf = inf;
        m.dec = 1;
        QLabel* readout = text(QString(), Qt::AlignHCenter | Qt::AlignVCenter);
        readout->setFixedHeight(16);
        v->addWidget(readout);
        DxSlider* s = slider(Qt::Vertical, fixed(i), m, QString("dxParam%1").arg(i), nullptr, readout, "dB");
        s->setFixedHeight(118);
        v->addWidget(s, 0, Qt::AlignHCenter);
        QLabel* cap = text(name + "<br>" + rangeText(i, m, "dB"), Qt::AlignHCenter | Qt::AlignTop);
        v->addWidget(cap);
        col->setFixedWidth(colW);
        return col;
    }

    QWidget* block(int i, const QString& name, const QString& unit, SMap m)
    {
        QWidget* w = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(4);
        QLabel* l = text(name + " " + rangeText(i, m, unit) + ":");
        top->addWidget(l, 1);
        QLineEdit* box = editBox(62);
        top->addWidget(box);
        v->addLayout(top);
        DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), box, nullptr);
        v->addWidget(s);
        l->setBuddy(s);
        return w;
    }

    QWidget* lineRow(int labelW, const Ix& ix, const QString& name, const SMap& m, const QString& objName, QCheckBox** check = nullptr, QLineEdit** boxOut = nullptr,
                     DxSlider** sliderOut = nullptr)
    {
        QWidget* w = new QWidget(this);
        QHBoxLayout* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        if (check) {
            *check = new QCheckBox(w);
            (*check)->setFixedWidth(16);
            h->addWidget(*check);
        }
        QLabel* l = text(name);
        l->setFixedWidth(labelW);
        h->addWidget(l);
        QLineEdit* box = editBox();
        DxSlider* s = slider(Qt::Horizontal, ix, m, objName, box, nullptr);
        l->setBuddy(s);
        h->addWidget(s, 1);
        h->addWidget(box);
        if (boxOut) *boxOut = box;
        if (sliderOut) *sliderOut = s;
        return w;
    }

    QCheckBox* checkBox(int i, const QString& label)
    {
        QString shown = label;
        shown.replace(QRegularExpression("<u>(.)</u>"), "&\\1");     // the mnemonic of a check box is an ampersand
        QCheckBox* c = new QCheckBox(shown, this);
        c->setObjectName(QString("dxParam%1").arg(i));
        QObject::connect(c, &QCheckBox::toggled, c, [=](bool on) { commit(i, on ? 1 : 0); });
        sync_.push_back([=] {
            QSignalBlocker b(c);
            c->setChecked(get(i) >= 0.5);
        });
        return c;
    }

    QComboBox* combo(const Ix& ix, const QStringList& items, const QString& name)
    {
        QComboBox* c = new QComboBox(this);
        c->setObjectName(name);
        c->addItems(items);
        QObject::connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), c, [=](int k) {
            if (k >= 0) commit(ix(), k);
        });
        sync_.push_back([=] {
            QSignalBlocker b(c);
            c->setCurrentIndex((int)std::lround(std::max(0.0, std::min((double)items.size() - 1, get(ix())))));
        });
        return c;
    }

    QWidget* radios(int i, const QStringList& items, bool vertical = false)
    {
        QWidget* w = new QWidget(this);
        QBoxLayout* h = vertical ? static_cast<QBoxLayout*>(new QVBoxLayout(w)) : static_cast<QBoxLayout*>(new QHBoxLayout(w));
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(vertical ? 2 : 14);
        QButtonGroup* g = new QButtonGroup(w);
        g->setExclusive(true);
        for (int k = 0; k < items.size(); ++k) {
            QRadioButton* r = new QRadioButton(items[k], w);
            r->setObjectName(QString("dxParam%1_%2").arg(i).arg(k));
            g->addButton(r, k);
            h->addWidget(r);
            QObject::connect(r, &QRadioButton::toggled, r, [=](bool on) {
                if (on) commit(i, k);
            });
        }
        if (!vertical) h->addStretch(1);
        sync_.push_back([=] {
            const int k = (int)std::lround(std::max(0.0, std::min((double)items.size() - 1, get(i))));
            if (QAbstractButton* b = g->button(k)) {
                QSignalBlocker blocker(b);
                b->setChecked(true);
            }
        });
        return w;
    }

    QPushButton* button(const QString& t)
    {
        QPushButton* b = new QPushButton(t, this);
        b->setFocusPolicy(Qt::TabFocus);
        return b;
    }

    Graph* graph(const QSize& size)
    {
        Graph* g = new Graph(this);
        g->setFixedSize(size);
        graphs_.push_back(g);
        return g;
    }

    QVBoxLayout* twoColumns(std::initializer_list<std::pair<int, const char*>> gains, int rightW = 300)
    {
        QHBoxLayout* h = new QHBoxLayout();
        h->setSpacing(6);
        QHBoxLayout* left = new QHBoxLayout();
        left->setSpacing(0);
        for (const auto& g : gains) left->addWidget(gainColumn(g.first, g.second));
        h->addLayout(left);
        QVBoxLayout* right = new QVBoxLayout();
        right->setSpacing(6);
        QWidget* holder = new QWidget(this);
        holder->setFixedWidth(rightW);
        QVBoxLayout* inner = new QVBoxLayout(holder);
        inner->setContentsMargins(0, 0, 0, 0);
        inner->setSpacing(7);
        right->addWidget(holder);
        h->addLayout(right, 1);
        root_->addLayout(h);
        return inner;
    }

    // ---- a page placed as the dialog places things (dialog units; the 14 units of the logo bar above the page are left out)
    struct Placed {
        QWidget* box = nullptr;
        static constexpr double kx = 1.75, ky = 1.875;
        static int px(int x) { return (int)std::lround(x * kx); }
        static int py(int y) { return (int)std::lround(y * ky); }
    };
    Placed placedPage(int dlgW, int dlgH)
    {
        Placed pl;
        pl.box = new QWidget(this);
        pl.box->setFixedSize(Placed::px(dlgW), Placed::py(dlgH - 14));
        root_->addWidget(pl.box);
        return pl;
    }
    template <class W>
    W* put(const Placed& pl, W* w, int x, int y, int cw, int ch)
    {
        w->setParent(pl.box);
        w->setGeometry(Placed::px(x), Placed::py(y - 14), Placed::px(cw), Placed::py(ch));
        static_cast<QWidget*>(w)->show();
        return w;
    }
    QLabel* putLabel(const Placed& pl, const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter)
    {
        return put(pl, text(t, a), x, y, cw, ch);
    }
    // A number box of the dialog's size. Boxes keep their own height (the style's), centred on the dialog's.
    QLineEdit* putEdit(const Placed& pl, int x, int y, int cw, int ch)
    {
        QLineEdit* e = new QLineEdit(this);
        e->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return put(pl, e, x, y, cw, ch);
    }
    // A gain fader the way the dialogs have them: the value above, the fader, the name and range below.
    DxSlider* putFader(const Placed& pl, int i, int fx, int fy, int fh, int rx, int rw, const QString& caption, int readX = -1, int readW = -1)
    {
        if (readX < 0) readX = rx - 8, readW = rw + 16;
        QLabel* rd = putLabel(pl, QString(), readX, fy - 9, readW, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        SMap m;
        m.inf = true;
        m.dec = 1;
        DxSlider* s = slider(Qt::Vertical, fixed(i), m, QString("dxParam%1").arg(i), nullptr, rd, "dB");
        put(pl, s, fx, fy, 10, fh);
        QLabel* cap = putLabel(pl, caption, rx - 8, fy + fh + 1, rw + 16, 16, Qt::AlignHCenter | Qt::AlignTop);   // (a little wider than the dialog's: the page's font may be wider)
        (void)cap;
        return s;
    }
    // A horizontal trackbar with the label above it (the label's mnemonic is the trackbar's), an optional number box and/or readout.
    DxSlider* putTrack(const Placed& pl, int i, const SMap& m, int sx, int sy, int sw, QLineEdit* box, QLabel* readout, QLabel* caption, const QString& readUnit = QString())
    {
        DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), box, readout, readUnit);
        put(pl, s, sx, sy, sw, 12);
        if (caption) caption->setBuddy(s);
        return s;
    }
    // The tempo-sync multiplier of a setting: a number box and the spinner beside it, which steps it by one.
    QLineEdit* putMultiplier(const Placed& pl, int i, int x, int y, int bw)
    {
        QLineEdit* box = valueBox(i, 3, 56, QString("dxParam%1Edit").arg(i));
        box->setMinimumSize(0, 0);
        box->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        put(pl, box, x, y, bw, 12);
        SpinArrows* sp = new SpinArrows(this);
        put(pl, sp, x + bw, y, 10, 12);
        sp->step = [this, i](int dir) {
            if (updating_) return;
            const Rng r = rng(i);
            commit(i, std::max(r.lo, std::min(r.hi, get(i) + dir)));
        };
        sync_.push_back([=] { sp->setEnabled(box->isEnabled()); });
        return box;
    }

#include "fxpages_vegastrk.inc"

    static QRectF dot(const QPointF& c, double r) { return QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r); }

    // ============================================================================================ Distortion
    // VEGAS's Distortion page: the graph is the transfer curve (level in against level out, -Inf. (-84 dB) to 0 dB)
    // for one polarity of the wave; Pos/Neg switches polarity and the mode makes the other one from it.
    vegdist::Settings distSettings() const
    {
        vegdist::Settings s;
        if (h_.state) vegdist::decode(h_.state(), &s);
        return s;
    }
    void distStore(const vegdist::Settings& s, const std::string& what)
    {
        if (updating_ || !h_.setState) return;
        h_.setState(vegdist::encode(s), what);
        syncAll();
    }

    // Dialog 5001 of sfppack3 (285x221), placed as the dialog places things: the Dry out and Wet out faders, the graph with its marks around it, the
    // polarity mode, Pos/Neg and Reset, and the slew rate and low-pass start with a check box, a trackbar and a box each.
    void distortion()
    {
        const Placed pl = placedPage(285, 221);
        {
            SMap m;
            m.inf = true;
            m.dec = 1;
            QLabel* r4 = putLabel(pl, QString(), 15, 17, 28, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            DxSlider* s4 = slider(Qt::Vertical, fixed(4), m, "dxParam4", nullptr, r4, "dB");
            put(pl, s4, 24, 26, 10, 71);
            putLabel(pl, "<u>D</u>ry out<br>(-Inf. to 0 dB)", 2, 98, 46, 16, Qt::AlignHCenter | Qt::AlignTop)->setBuddy(s4);
            QLabel* r5 = putLabel(pl, QString(), 58, 17, 28, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            DxSlider* s5 = slider(Qt::Vertical, fixed(5), m, "dxParam5", nullptr, r5, "dB");
            put(pl, s5, 67, 26, 10, 71);
            putLabel(pl, "<u>W</u>et out<br>(-Inf. to 24 dB)", 49, 98, 49, 16, Qt::AlignHCenter | Qt::AlignTop)->setBuddy(s5);
        }
        QLabel* gl = putLabel(pl, "<u>G</u>raph:", 108, 17, 26, 8);
        QLabel* readout = putLabel(pl, QString(), 147, 17, 132, 8, Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        EnvelopeGraph* g = new EnvelopeGraph(this);
        g->setObjectName("dxGraph");
        put(pl, g, 93, 24, 191, 146);
        g->margins = QMargins(Placed::px(15), Placed::py(2), Placed::px(4), Placed::py(9));
        gl->setBuddy(g);
        graphs_.push_back(g);
        g->readout = readout;
        g->edited = [this, g](int phase) {
            vegdist::Settings s = distSettings();
            vegdist::Point* to = s.editPositive ? s.pos : s.neg;
            int32_t& n = s.editPositive ? s.posCount : s.negCount;
            n = (int32_t)g->pts.size();
            for (int k = 0; k < n; ++k) to[k] = g->pts[(size_t)k];
            vegdist::applyMode(s);
            if (phase == 0 && h_.beginEdit) h_.beginEdit("Edit Distortion graph");
            distStore(s, "Edit Distortion graph");
            if (phase == 2) endDrag();
        };
        sync_.push_back([this, g] {
            if (g->dragging()) return;
            const vegdist::Settings s = distSettings();
            const vegdist::Point* from = s.editPositive ? s.pos : s.neg;
            const int n = s.editPositive ? s.posCount : s.negCount;
            g->pts.assign(from, from + n);
        });

        QLabel* ml = putLabel(pl, "Graph p<u>o</u>larity edit mode:", 5, 173, 95, 8);
        QComboBox* c = new QComboBox(this);
        c->setObjectName("dxPolarityMode");
        c->addItems({"Individual", "Synchronize", "Mirror Y", "Mirror X", "Mirror X/Y"});
        put(pl, c, 106, 171, 75, 12);
        ml->setBuddy(c);
        QObject::connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), c, [this](int k) {
            if (k < 0) return;
            vegdist::Settings s = distSettings();
            s.mode = k;
            vegdist::applyMode(s);
            distStore(s, "Change \"Graph polarity edit mode\"");
        });
        // (the button shows the polarity the next press switches to)
        QPushButton* pn = button("Nega&tive");
        pn->setObjectName("dxPosNeg");
        put(pl, pn, 186, 170, 45, 14);
        QObject::connect(pn, &QPushButton::clicked, pn, [this] {
            vegdist::Settings s = distSettings();
            s.editPositive = !s.editPositive;
            distStore(s, "Change polarity edited");
        });
        QPushButton* reset = button("R&eset");
        reset->setObjectName("dxReset");
        put(pl, reset, 235, 170, 45, 14);
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            vegdist::Settings s = distSettings();
            vegdist::Point* to = s.editPositive ? s.pos : s.neg;
            (s.editPositive ? s.posCount : s.negCount) = 2;
            to[0] = vegdist::Point{vegdist::kInf, vegdist::kInf};
            to[1] = vegdist::Point{0, 0};
            vegdist::applyMode(s);
            distStore(s, "Reset Distortion graph");
        });
        sync_.push_back([=] {
            const vegdist::Settings s = distSettings();
            QSignalBlocker b(c);
            c->setCurrentIndex(s.mode);
            pn->setText(s.editPositive ? "Nega&tive" : "Posi&tive");
        });

        SMap slew;
        slew.dec = 0;
        QCheckBox* sc = new QCheckBox("S&lew rate (1 to 100):", this);
        sc->setObjectName("dxParam0");
        put(pl, sc, 5, 190, 95, 10);
        QObject::connect(sc, &QCheckBox::toggled, sc, [this](bool on) { commit(0, on ? 1 : 0); });
        QLineEdit* slewBox = putEdit(pl, 236, 188, 44, 12);
        DxSlider* slewSlider = slider(Qt::Horizontal, fixed(1), slew, "dxParam1", slewBox, nullptr);
        put(pl, slewSlider, 106, 188, 128, 12);
        SMap lpm;
        lpm.log = true;
        lpm.dec = 0;
        QCheckBox* lc = new QCheckBox("Low-pass start &freq. (Hz):", this);
        lc->setObjectName("dxParam2");
        put(pl, lc, 5, 204, 95, 10);
        QObject::connect(lc, &QCheckBox::toggled, lc, [this](bool on) { commit(2, on ? 1 : 0); });
        QLineEdit* lpBox = putEdit(pl, 236, 203, 44, 12);
        DxSlider* lpSlider = slider(Qt::Horizontal, fixed(3), lpm, "dxParam3", lpBox, nullptr);
        put(pl, lpSlider, 106, 203, 128, 12);
        sync_.push_back([=] {
            QSignalBlocker b1(sc), b2(lc);
            const bool slewOn = get(0) >= 0.5, lpOn = get(2) >= 0.5;
            sc->setChecked(slewOn);
            slewSlider->setEnabled(slewOn);
            slewBox->setEnabled(slewOn);
            lc->setChecked(lpOn);
            lpSlider->setEnabled(lpOn);
            lpBox->setEnabled(lpOn);
        });
    }

    // ============================================================================================ Vibrato
    // Vibrato: the graph is the pitch over one turn of the modulation (semitones, up to the setting's 2); its handle pulls the depth.
    static double waveAt(int w, double ph)
    {
        ph -= std::floor(ph);
        if (w == 1) return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
        if (w == 2) return ph < 0.5 ? 1 : -1;
        return std::sin(2 * kPi * ph);
    }

#include "fxpages_pack3.inc"

    // VEGAS's Time Stretch page (dialog 1001): the Mode, the final length as a percentage with its slider, the Input format
    // (Percentage, Tempo or Time) which turns that same value into a final tempo or a final time worked out from the initial
    // tempo or initial time, and the percentage of the original it comes to. The initial time is kept in samples at 44100
    // a second, as the dialog does without an input pin.
    void timeStretch(bool express = false)
    {
        const int W = express ? 245 : 225;
        const double kx = 1.75, ky = 1.875;
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(W * kx), (int)std::lround((115 - 14) * ky));
        root_->addWidget(box);
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 14) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        auto label = [=](const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter) {
            QLabel* l = text(t, a);
            put(l, x, y, cw, ch);
            return l;
        };

        const std::string uid = express ? "reno.xptimestretch" : "reno.timestretch";
        QLabel* ml = label("<u>M</u>ode:", 5, 19, 24, 8);
        QComboBox* mode = combo(fixed(1), choicesOf(uid.c_str(), 1), "dxParam1");
        put(mode, express ? 60 : 40, 17, 180, 14);
        ml->setBuddy(mode);

        QLabel* fl = label(QString(), 5, 36, 158, 8);
        SMap pm;
        pm.dec = 1;
        pm.comma = false;
        DxSlider* pS = slider(Qt::Horizontal, fixed(0), pm, "dxParam0", nullptr, nullptr);
        put(pS, 5, 48, W - 10, 12);
        fl->setBuddy(pS);
        QLineEdit* fBox = editBox((int)std::lround(55 * kx));
        fBox->setObjectName("dxParam0Edit");
        put(fBox, W - 60, 34, 55, 12);

        QFrame* ln = new QFrame(this);
        ln->setFrameShape(QFrame::HLine);
        ln->setFrameShadow(QFrame::Sunken);
        put(ln, 5, 64, W - 10, 2);

        QLabel* tl = label("Input forma<u>t</u>:", 5, 71, 110, 8);
        QComboBox* fmt = combo(fixed(4), choicesOf(uid.c_str(), 4), "dxParam4");
        put(fmt, W - (express ? 95 : 95), 69, 90, 14);
        tl->setBuddy(fmt);
        QLabel* il = label(QString(), 5, 87, 140, 8);
        QLineEdit* iBox = editBox((int)std::lround(55 * kx));
        iBox->setObjectName("dxParam2Edit");
        put(iBox, W - 60, 85, 55, 12);
        il->setBuddy(iBox);
        label("Percent of original:", 5, 102, 116, 8);
        QLabel* pct = label(QString(), W - 47, 102, 42, 8, Qt::AlignRight | Qt::AlignVCenter);

        const double kRate = 44100.0;
        auto clock = [](double sec) {
            sec = std::max(0.0, sec);
            const long long ms = (long long)std::floor(sec * 1000.0 + 0.5);
            return QString("%1:%2:%3.%4").arg(ms / 3600000, 2, 10, QChar('0')).arg(ms / 60000 % 60, 2, 10, QChar('0')).arg(ms / 1000 % 60, 2, 10, QChar('0')).arg(ms % 1000, 3, 10, QChar('0'));
        };
        auto parseClock = [](const QString& t, double* out) {
            double sec = 0;
            const QStringList parts = t.trimmed().split(':');
            if (parts.isEmpty() || parts.size() > 3) return false;
            for (const QString& p : parts) {
                double v = 0;
                if (!parseNumber(p, &v) || v < 0) return false;
                sec = sec * 60.0 + v;
            }
            *out = sec;
            return true;
        };
        // the final length edit, in whichever unit the Input format asks for
        QObject::connect(fBox, &QLineEdit::editingFinished, fBox, [=] {
            if (updating_) return;
            const int f = (int)std::lround(get(4));
            const double ini = get(3) / kRate, tempo = get(2);
            double v = 0;
            double p = 0;
            bool ok = false;
            if (f == 1) {
                ok = parseNumber(fBox->text().trimmed(), &v) && v > 0;
                p = ok ? tempo * 100.0 / v : 0;
            } else if (f == 2) {
                ok = parseClock(fBox->text(), &v) && ini > 0;
                p = ok ? v / ini * 100.0 : 0;
            } else {
                ok = parseNumber(fBox->text().trimmed(), &v);
                p = v;
            }
            if (!ok) { syncAll(); return; }
            commit(0, std::min(500.0, std::max(50.0, p)));
        });
        // the initial tempo or the initial time
        QObject::connect(iBox, &QLineEdit::editingFinished, iBox, [=] {
            if (updating_) return;
            const int f = (int)std::lround(get(4));
            double v = 0;
            if (f == 1) {
                if (!parseNumber(iBox->text().trimmed(), &v) || v < 0 || v > 500) { syncAll(); return; }
                commit(2, v);
            } else if (f == 2) {
                if (!parseClock(iBox->text(), &v)) { syncAll(); return; }
                commit(3, std::min(793800000.0, v * kRate));
            } else {
                syncAll();
            }
        });
        sync_.push_back([=] {
            const int f = (int)std::lround(get(4));
            const double p = get(0), tempo = get(2), tm = get(3) / kRate;
            iBox->setEnabled(f != 0);
            if (f == 1) {
                const double lo = tempo * 0.2, hi = tempo * 2.0;
                fl->setText(QString("<u>F</u>inal Tempo (%1 to %2 bpm):").arg(lo, 0, 'f', 2).arg(hi, 0, 'f', 2));
                il->setText("<u>I</u>nitial Tempo (bpm):");
                if (!fBox->hasFocus()) fBox->setText(QString::number(tempo * 100.0 / p, 'f', 2));
                if (!iBox->hasFocus()) iBox->setText(QString::number(tempo, 'f', 2));
            } else if (f == 2) {
                fl->setText(QString("<u>F</u>inal Time (%1 to %2):").arg(clock(tm * 0.5), clock(tm * 5.0)));
                il->setText("<u>I</u>nitial Time (hr:mn:sc.xxx):");
                if (!fBox->hasFocus()) fBox->setText(clock(tm * p / 100.0));
                if (!iBox->hasFocus()) iBox->setText(clock(tm));
            } else {
                fl->setText("<u>F</u>inal Percentage  (50.0 to 500.0 %):");
                il->setText("Unavailable with Percentage");
                if (!fBox->hasFocus()) fBox->setText(QString::number(p, 'f', 1));
                iBox->setText(QString::number(tempo, 'f', 2));
            }
            pct->setText(QString::number(p, 'f', 0) + " %");
        });
    }

    // ============================================================================================ the other effects
    // VEGAS's Pitch Shift page (dialog 2001): Semitones and Cents (slider and box each), Accuracy (1 to 3), the anti-alias
    // filter, Preserve duration, the Mode, and the Initial time with the Final time, the musical equivalent and the
    // transposition ratio it works out. Preserve duration limits the pitch to 12 semitones either way; the anti-alias
    // filter is for a higher pitch only.
    void pitchShift()
    {
        const double kx = 1.75, ky = 1.875;
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(210 * kx), (int)std::lround((183 - 14) * ky));
        root_->addWidget(box);
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 14) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        auto label = [=](const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter) {
            QLabel* l = text(t, a);
            put(l, x, y, cw, ch);
            return l;
        };
        auto line = [=](int x, int y, int cw) {
            QFrame* f = new QFrame(this);
            f->setFrameShape(QFrame::HLine);
            f->setFrameShadow(QFrame::Sunken);
            put(f, x, y, cw, 2);
        };

        QLabel* sl = label("Se<u>m</u>itones to shift pitch by (-50 to 50):", 5, 19, 145, 8);
        QLineEdit* sBox = editBox(52);
        SMap sm;
        sm.dec = 0;
        sm.comma = false;
        DxSlider* sS = slider(Qt::Horizontal, fixed(0), sm, "dxParam0", sBox, nullptr);
        put(sS, 5, 31, 200, 12);
        put(sBox, 165, 17, 40, 12);
        sl->setBuddy(sS);

        QLabel* cl = label("<u>C</u>ents to shift pitch by (-100.0 to 100.0):", 5, 47, 158, 8);
        QLineEdit* cBox = editBox(52);
        SMap cm;
        cm.dec = 1;
        cm.comma = false;
        DxSlider* cS = slider(Qt::Horizontal, fixed(1), cm, "dxParam1", cBox, nullptr);
        put(cS, 5, 59, 200, 12);
        put(cBox, 165, 45, 40, 12);
        cl->setBuddy(cS);

        QLabel* al = label("Accurac<u>y</u> (1 low to 3 high):", 5, 75, 98, 8);
        label("Low", 104, 75, 14, 8);
        QLabel* aRead = label(QString(), 197, 75, 8, 8);
        SMap am;
        am.dec = 0;
        DxSlider* aS = slider(Qt::Horizontal, fixed(2), am, "dxParam2", nullptr, aRead);
        put(aS, 121, 73, 42, 12);
        label("High", 165, 75, 16, 8);
        al->setBuddy(aS);

        QCheckBox* anti = checkBox(3, "Apply an anti-alias <u>f</u>ilter during pitch shift");
        put(anti, 5, 87, 160, 10);
        QCheckBox* keep = checkBox(4, "Preserve <u>d</u>uration");
        put(keep, 5, 100, 74, 10);
        line(85, 105, 120);

        QLabel* ml = label("M<u>o</u>de:", 5, 113, 25, 8);
        QComboBox* mode = combo(fixed(5), choicesOf("reno.pitchshift", 5), "dxParam5");
        put(mode, 35, 111, 170, 14);
        ml->setBuddy(mode);
        line(5, 128, 200);

        QLabel* il = label("Initia<u>l</u> time (hr:mn:sc.xxx):", 5, 135, 125, 8);
        QLineEdit* iBox = editBox((int)std::lround(70 * kx));
        iBox->setObjectName("dxParam6Edit");
        put(iBox, 135, 133, 70, 12);
        il->setBuddy(iBox);
        label("Final time (hr:mn:sc.xxx):", 5, 148, 140, 8);
        QLabel* fin = label("00:00:00", 145, 148, 60, 8);
        label("Musical equivalent:", 5, 159, 125, 8);
        QLabel* mus = label(QString(), 95, 159, 110, 8, Qt::AlignRight | Qt::AlignVCenter);   // wider than the dialog's: "Up Major 3rd + 20 cents"
        label("Transposition ratio:", 5, 170, 125, 8);
        QLabel* rat = label(QString(), 145, 170, 60, 8);

        // the time is kept as a count of samples at 44100 a second, as the dialog does without an input
        const double kRate = 44100.0;
        auto clock = [](double sec) {
            sec = std::max(0.0, sec);
            const long long ms = (long long)std::floor(sec * 1000.0 + 0.5);
            return QString("%1:%2:%3.%4").arg(ms / 3600000, 2, 10, QChar('0')).arg(ms / 60000 % 60, 2, 10, QChar('0')).arg(ms / 1000 % 60, 2, 10, QChar('0')).arg(ms % 1000, 3, 10, QChar('0'));
        };
        QObject::connect(iBox, &QLineEdit::editingFinished, iBox, [=] {
            if (updating_) return;
            double sec = 0;
            const QStringList parts = iBox->text().trimmed().split(':');
            bool ok = !parts.isEmpty() && parts.size() <= 3;
            for (const QString& p : parts) {
                double v = 0;
                if (!parseNumber(p, &v) || v < 0) { ok = false; break; }
                sec = sec * 60.0 + v;
            }
            if (!ok) { syncAll(); return; }
            commit(6, sec * kRate);
        });
        static const char* const kInt[13] = {"", "Minor 2nd", "Major 2nd", "Minor 3rd", "Major 3rd", "4th", "Tritone", "5th", "Minor 6th", "Major 6th", "Minor 7th", "Major 7th", "Octave"};
        sync_.push_back([=] {
            const int semis = (int)std::lround(get(0));
            const double cents = get(1);
            const bool preserve = get(4) >= 0.5;
            const double ratio = std::pow(2.0, (cents / 100.0 + semis) / 12.0);
            // Preserve duration works up to an octave either way, the filter only for a higher pitch
            keep->setEnabled(semis >= -12 && semis <= 12);
            anti->setEnabled(ratio > 1.0);
            sl->setText(preserve ? "Se<u>m</u>itones to shift pitch by (-12 to 12):" : "Se<u>m</u>itones to shift pitch by (-50 to 50):");
            if (preserve && !updating_ && (semis > 12 || semis < -12) && h_.set) h_.set(0, semis > 12 ? 12 : -12);
            if (!iBox->hasFocus()) iBox->setText(clock(get(6) / kRate));
            fin->setText(clock(get(6) / kRate / ratio));
            rat->setText(QString::number(ratio, 'g', 5));
            // "Up Major 3rd + 20 cents": the nearest interval, then what is left in cents
            const double total = semis + cents / 100.0;
            QString m;
            if (std::fabs(total) >= 0.005) {
                const double a = std::fabs(total);
                int whole = (int)std::floor(a + 0.005);
                int rest = (int)std::lround((a - whole) * 100.0);
                if (rest >= 100) { ++whole; rest = 0; }
                m = total > 0 ? "Up" : "Down";
                const int oct = whole / 12, within = whole % 12;
                if (oct > 0) m += QString(" %1 octave%2").arg(oct).arg(oct > 1 ? "s" : "");
                if (within > 0) m += QString(" ") + kInt[within];
                if (rest > 0) m += QString(" + %1 cents").arg(rest);
            }
            mus->setText(m);
        });
    }

    // VEGAS's Chorus page (dialog 6001): Input gain / Dry out / Chorus out faders, the two phase inversions, Chorus
    // size (1 to 3), Modulation rate (or, with Tempo sync, the Modulation period as a note times a multiplier), Depth,
    // Feedback, Delay, and the high-frequency attenuation with its frequency. Placed as the dialog places things.
    void chorus()
    {
        const Placed pl = placedPage(272, 224);
        putFader(pl, 9, 23, 26, 70, 5, 45, "<u>I</u>nput gain<br>(-Inf. to 0 dB)");
        putFader(pl, 10, 69, 26, 70, 51, 45, "Dr<u>y</u> out<br>(-Inf. to 0 dB)");
        putFader(pl, 11, 114, 26, 70, 97, 45, "<u>C</u>horus out<br>(-Inf. to 0 dB)");
        put(pl, checkBox(5, "In<u>v</u>ert chorus phase"), 145, 48, 107, 10);
        put(pl, checkBox(6, "Invert feedbac<u>k</u> phase"), 145, 61, 110, 10);

        QLabel* sizeCap = putLabel(pl, "Chorus si<u>z</u>e (1 to 3):", 145, 74, 71, 8);
        QLabel* sizeRead = putLabel(pl, QString(), 247, 74, 10, 8, Qt::AlignLeft | Qt::AlignVCenter);
        SMap size;
        size.dec = 0;
        putTrack(pl, 4, size, 218, 73, 23, nullptr, sizeRead, sizeCap);

        QLabel* rateCap = putLabel(pl, "Modulation ra<u>t</u>e (0.001 to 20.0 Hz):", 5, 120, 130, 8);
        QLineEdit* rateBox = putEdit(pl, 237, 118, 30, 12);
        SMap rate;
        rate.dec = 3;
        rate.log = true;
        DxSlider* rateS = putTrack(pl, 0, rate, 145, 118, 90, rateBox, nullptr, rateCap);

        QLabel* depthCap = putLabel(pl, "Modulation d<u>e</u>pth (1 to 100 %):", 5, 135, 130, 8);
        QLabel* depthRead = putLabel(pl, QString(), 237, 135, 30, 8, Qt::AlignLeft | Qt::AlignVCenter);
        SMap pc;
        pc.dec = 0;
        putTrack(pl, 1, pc, 145, 133, 90, nullptr, depthRead, depthCap, "%");

        QLabel* fbCap = putLabel(pl, "<u>F</u>eedback (0 to 100 %):", 5, 150, 130, 8);
        QLabel* fbRead = putLabel(pl, QString(), 237, 150, 30, 8, Qt::AlignLeft | Qt::AlignVCenter);
        putTrack(pl, 3, pc, 145, 148, 90, nullptr, fbRead, fbCap, "%");

        QLabel* delCap = putLabel(pl, "Chorus out <u>d</u>elay (0.1 to 100.0 ms):", 5, 165, 130, 8);
        QLineEdit* delBox = putEdit(pl, 237, 163, 30, 12);
        SMap dl;
        dl.dec = 1;
        putTrack(pl, 2, dl, 145, 163, 90, delBox, nullptr, delCap);

        QCheckBox* hf = put(pl, checkBox(7, "Attenuate high frequencies ab<u>o</u>ve (Hz):"), 5, 179, 138, 10);
        QLineEdit* hfBox = putEdit(pl, 237, 178, 30, 12);
        SMap hz;
        hz.dec = 0;
        hz.log = true;
        DxSlider* hfS = putTrack(pl, 8, hz, 145, 178, 90, hfBox, nullptr, nullptr);

        QCheckBox* ts = put(pl, checkBox(15, "Tempo <u>s</u>ync"), 5, 194, 54, 10);
        QLabel* perCap = putLabel(pl, "Modulation <u>p</u>eriod:", 5, 208, 130, 8);
        QLineEdit* mult = putMultiplier(pl, 14, 145, 206, 35);
        perCap->setBuddy(mult);
        QComboBox* note = put(pl, combo(fixed(13), choicesOf("reno.chorus", 13), "dxParam13"), 193, 206, 74, 14);
        (void)ts;
        sync_.push_back([=] {
            const bool tsOn = get(15) >= 0.5;
            rateCap->setEnabled(!tsOn);
            rateBox->setEnabled(!tsOn);
            rateS->setEnabled(!tsOn);
            perCap->setEnabled(tsOn);
            mult->setEnabled(tsOn);
            note->setEnabled(tsOn);
            const bool hfOn = get(7) >= 0.5;
            hfS->setEnabled(hfOn);
            hfBox->setEnabled(hfOn);
            (void)hf;
        });
    }

    // ============================================================================================ pack1 helpers
    QStringList choicesOf(const char* uid, int param) const
    {
        QStringList l;
        const vegpack1::Info* info = vegpack1::findInfo(uid);
        if (!info || param < 0 || param >= (int)info->params.size()) return l;
        const QString all = QString::fromUtf8(info->params[(size_t)param].choices);
        return all.split(all.contains('|') ? '|' : ',');
    }
    // A plain edit box for setting i (shown with 'dec' digits); returns it, the caller places it.
    QLineEdit* valueBox(int i, int dec, int width, const QString& name)
    {
        QLineEdit* e = editBox(width);
        e->setObjectName(name);
        QObject::connect(e, &QLineEdit::editingFinished, e, [=] {
            if (updating_) return;
            double v = 0;
            if (!parseNumber(e->text(), &v)) { syncAll(); return; }
            if (std::fabs(v - get(i)) > 1e-9) commit(i, v);
            else syncAll();
        });
        sync_.push_back([=] {
            if (!e->hasFocus()) e->setText(QString::number(get(i), 'f', dec));
        });
        return e;
    }

    // ============================================================================================ Simple Delay
    // VEGAS's Simple Delay page (dialog 3001): Dry out and Delay out faders, Delay time (seconds) with a Tempo sync
    // delay (note and multiplier), Multiple delays (Feedback) with Decay time and its own Tempo sync. Placed as the dialog places things.
    void simpleDelay()
    {
        const Placed pl = placedPage(257, 208);
        putFader(pl, 3, 23, 26, 160, 5, 45, "Dr<u>y</u> out<br>(-Inf. to 0 dB)");
        putFader(pl, 4, 69, 26, 160, 51, 45, "<u>D</u>elay out<br>(-Inf. to 0 dB)");

        std::vector<QWidget*> delayWays, tempoDelay, decayPlain, decayTempo;
        auto timeRow = [&](int i, const QString& label, double shown, int dec, int capY, int boxY, int sliderY, std::vector<QWidget*>& group) {
            QLabel* l = putLabel(pl, label, 102, capY, 118, 8);
            SMap m;
            m.dec = dec;
            m.shown = shown;
            m.comma = false;
            QLineEdit* box = putEdit(pl, 220, boxY, 32, 12);
            if (shown != 1) box->setProperty("shown", true);
            DxSlider* s = putTrack(pl, i, m, 102, sliderY, 150, box, nullptr, l);
            group.push_back(l);
            group.push_back(box);
            group.push_back(s);
        };
        timeRow(0, "De<u>l</u>ay time (0.001 to 5.0 seconds):", 0.001, 3, 25, 23, 39, delayWays);
        put(pl, checkBox(8, "<u>T</u>empo sync delay"), 102, 55, 73, 10);
        QLineEdit* mult = putMultiplier(pl, 7, 102, 69, 36);
        QComboBox* note = put(pl, combo(fixed(6), choicesOf("reno.simpledelay", 6), "dxParam6"), 151, 69, 101, 14);
        tempoDelay = {mult, note};
        put(pl, checkBox(2, "<u>M</u>ultiple delays (Feedback)"), 102, 107, 110, 10);
        timeRow(1, "De<u>c</u>ay time (0.1 to 20.0 seconds):", 1.0, 1, 123, 121, 137, decayPlain);
        QCheckBox* tsd = put(pl, checkBox(11, "Tempo <u>s</u>ync decay"), 102, 153, 75, 10);
        QLineEdit* dmult = putMultiplier(pl, 10, 102, 167, 36);
        QComboBox* dnote = put(pl, combo(fixed(9), choicesOf("reno.simpledelay", 9), "dxParam9"), 151, 167, 101, 14);
        decayTempo = {dmult, dnote};
        sync_.push_back([=] {
            const bool tsOn = get(8) >= 0.5, multiOn = get(2) >= 0.5, tsdOn = get(11) >= 0.5;
            for (QWidget* w : delayWays) w->setEnabled(!tsOn);
            for (QWidget* w : tempoDelay) w->setEnabled(tsOn);
            tsd->setEnabled(multiOn);
            for (QWidget* w : decayPlain) w->setEnabled(multiOn && !tsdOn);
            for (QWidget* w : decayTempo) w->setEnabled(multiOn && tsdOn);
        });
    }

    // ============================================================================================ Multi-Tap Delay
    // VEGAS's Multi-Tap Delay page (dialog 4001): Input gain / Dry out / Wet out faders, modulation and feedback sliders,
    // the low-pass, the number of taps, the current tap (1 to 8) with its gain, delay and pan, and a graph of the taps
    // (across: delay, up and down: gain) that picks and pulls them. Placed as the dialog places things (dialog units).
    int tap_ = 0;
    int tapRes_ = -1;      // index into kTapRes: the graph's width in ms
    static constexpr int kTapRes[4] = {500, 1000, 3000, 5000};

    void multiTap()
    {
        const double kx = 1.75, ky = 1.875;
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(290 * kx), (int)std::lround((262 - 14) * ky));
        root_->addWidget(box);
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 14) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        auto label = [=](const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter) {
            QLabel* l = text(t, a);
            put(l, x, y, cw, ch);
            return l;
        };
        auto fader = [=](Ix ix, const QString& name, int x, int y, int ch, int readX, int readY, const QString& unit, const SMap& m, const QString& cap, int capY, int capH) {
            QLabel* r = label(QString(), readX, readY, 45, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            DxSlider* s = slider(Qt::Vertical, ix, m, name, nullptr, r, unit);
            put(s, x, y, 10, ch);
            label(cap, readX, capY, 52, capH, Qt::AlignHCenter | Qt::AlignTop);
        };
        SMap db;
        db.inf = true;
        fader(fixed(0), "dxParam0", 23, 26, 70, 5, 17, "dB", db, "Inpu<u>t</u> gain<br>(-Inf. to 0 dB)", 97, 16);
        fader(fixed(1), "dxParam1", 69, 26, 70, 51, 17, "dB", db, "Dr<u>y</u> out<br>(-Inf. to 0 dB)", 97, 16);
        fader(fixed(2), "dxParam2", 114, 26, 70, 97, 17, "dB", db, "<u>W</u>et out<br>(-Inf. to 0 dB)", 97, 16);

        auto bar = [=](int i, const QString& cap, int ly, int sy, int dec, const QString& unit) {
            label(cap, 150, ly, 100, 8);
            QLabel* r = label(QString(), 255, ly, 30, 8, Qt::AlignRight | Qt::AlignVCenter);
            SMap m;
            m.dec = dec;
            DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), nullptr, r, unit);
            put(s, 150, sy, 135, 12);
        };
        bar(3, "<u>M</u>od. rate (0.1 to 10.0 Hz):", 20, 30, 1, "Hz");
        bar(4, "M<u>o</u>d. depth (0 to 100 %):", 48, 58, 0, "%");
        bar(5, "<u>F</u>eedback (0 to 150 %):", 77, 86, 0, "%");

        QCheckBox* lp = checkBox(6, "<u>L</u>ow-pass start freq. (Hz):");
        put(lp, 150, 106, 100, 10);
        SMap lm;
        lm.log = true;
        lm.dec = 0;
        lm.comma = false;
        QLineEdit* lpBox = editBox(52);
        DxSlider* lpS = slider(Qt::Horizontal, fixed(7), lm, "dxParam7", lpBox, nullptr);
        put(lpS, 150, 118, 135, 12);
        put(lpBox, 255, 105, 30, 12);
        sync_.push_back([=] {
            const bool on = get(6) >= 0.5;
            lpS->setEnabled(on);
            lpBox->setEnabled(on);
        });

        label("Numb<u>e</u>r of taps (1 to 8):", 9, 120, 80, 8);
        QLabel* nt = label(QString(), 126, 120, 8, 8, Qt::AlignRight | Qt::AlignVCenter);
        SMap nm;
        nm.dec = 0;
        put(slider(Qt::Horizontal, fixed(8), nm, "dxParam8", nullptr, nt), 91, 118, 31, 12);

        label("<u>C</u>urrent tap:", 9, 135, 42, 8);
        static const int bx[8] = {54, 75, 96, 117, 138, 159, 180, 203};
        std::vector<QRadioButton*> tapBtn;
        for (int k = 0; k < 8; ++k) {
            QRadioButton* b = new QRadioButton(QString("&%1").arg(k + 1), this);
            b->setObjectName(QString("dxTap%1").arg(k));
            put(b, bx[k], 135, 20, 10);
            QObject::connect(b, &QPushButton::clicked, b, [this, k] {
                tap_ = k;
                syncAll();
            });
            tapBtn.push_back(b);
        }

        // the current tap's gain, delay and pan
        const Ix gainIx = [this] { return 17 + tap_; }, delayIx = [this] { return 9 + tap_; }, panIx = [this] { return 25 + tap_; };
        SMap pc;
        pc.dec = 0;
        fader(gainIx, "dxTapGain", 23, 157, 76, 5, 148, "%", pc, "Tap <u>g</u>ain<br>(-100 to 100 %)", 235, 16);
        label("<u>D</u>elay (1 to 2,500 ms):", 84, 228, 80, 8);
        QLineEdit* dBox = editBox(52);
        DxSlider* dS = slider(Qt::Horizontal, delayIx, pc, "dxTapDelay", dBox, nullptr);
        put(dS, 84, 239, 135, 12);
        put(dBox, 179, 226, 30, 12);
        label("Pan:", 225, 228, 17, 8);
        QLabel* pr = label(QString(), 249, 228, 25, 8, Qt::AlignRight | Qt::AlignVCenter);
        put(slider(Qt::Horizontal, panIx, pc, "dxTapPan", nullptr, pr, "%"), 225, 239, 49, 12);
        label("L", 225, 252, 8, 8);
        label("R", 266, 252, 8, 8, Qt::AlignRight | Qt::AlignVCenter);

        // the graph of the taps
        label("100 %", 57, 150, 25, 8, Qt::AlignRight | Qt::AlignVCenter);
        label("Gain", 57, 174, 25, 8, Qt::AlignRight | Qt::AlignVCenter);
        label("-100 %", 57, 200, 25, 8, Qt::AlignRight | Qt::AlignVCenter);
        label("0 ms", 83, 211, 17, 8);
        label("Graph Resolut<u>i</u>on (Delay ms):", 123, 211, 101, 8);
        QComboBox* res = new QComboBox(this);
        res->setObjectName("dxTapRes");
        for (int r : kTapRes) res->addItem(QString::number(r));
        put(res, 226, 209, 48, 14);
        // start with the narrowest graph that holds every tap
        {
            int far = 0;
            const int n = (int)std::lround(get(8));
            for (int k = 0; k < n; ++k) far = std::max(far, (int)std::lround(get(9 + k)));
            tapRes_ = 3;
            for (int r = 3; r >= 0; --r)
                if (far <= kTapRes[r]) tapRes_ = r;
        }
        Graph* g = new Graph(this);
        g->setObjectName("dxTapGraph");
        g->margins = QMargins(0, 0, 0, 0);
        graphs_.push_back(g);
        put(g, 84, 151, 190, 56);
        g->draw = [this](QPainter& p, const QRectF& pl) {
            p.setPen(QColor(200, 200, 200));
            p.drawLine(QPointF(pl.left(), pl.center().y()), QPointF(pl.right(), pl.center().y()));
            p.setPen(QColor(140, 140, 140));
            p.drawRect(pl.adjusted(0, 0, -1, -1));
            const int n = (int)std::lround(get(8));
            const double span = kTapRes[std::max(0, std::min(3, tapRes_))];
            for (int k = n - 1; k >= 0; --k) {
                const double d = get(9 + k), gn = get(17 + k);
                const double x = pl.left() + std::max(0.0, std::min(1.0, d / span)) * (pl.width() - 1);
                const double y = pl.center().y() - gn / 100.0 * (pl.height() / 2 - 1);
                p.setPen(QPen(k == tap_ ? QColor(200, 30, 30) : QColor(30, 60, 160), k == tap_ ? 2 : 1));
                p.drawLine(QPointF(x, pl.center().y()), QPointF(x, y));
                p.fillRect(dot(QPointF(x, y), 2.5), k == tap_ ? QColor(200, 30, 30) : QColor(30, 60, 160));
            }
        };
        g->mouse = [this, g](const QPointF& at, int phase) {
            const int n = (int)std::lround(get(8));
            const double span = kTapRes[std::max(0, std::min(3, tapRes_))];
            if (phase == 0) {
                int best = -1;
                double bestD = 1e9;
                const QRectF pl = g->plot();
                for (int k = 0; k < n; ++k) {
                    const double x = std::max(0.0, std::min(1.0, get(9 + k) / span)) * pl.width();
                    const double d = std::fabs(at.x() * pl.width() - x);
                    if (d < bestD) { bestD = d; best = k; }
                }
                if (best < 0 || bestD > 9) { tapDrag_ = false; return; }
                tap_ = best;
                tapDrag_ = true;
                beginDrag(9 + tap_);
                syncAll();
                return;
            }
            if (!tapDrag_) return;
            if (phase == 1) {
                const double d = std::round(std::max(0.0, std::min(1.0, at.x())) * span);
                commit(9 + tap_, std::max(1.0, d));
                commit(17 + tap_, std::round((std::max(0.0, std::min(1.0, at.y())) - 0.5) * 200.0));
            } else {
                tapDrag_ = false;
                endDrag();
            }
        };
        QObject::connect(res, QOverload<int>::of(&QComboBox::currentIndexChanged), res, [this](int k) {
            if (k < 0 || updating_) return;
            tapRes_ = k;
            syncAll();
        });
        sync_.push_back([=] {
            const int n = std::max(1, (int)std::lround(get(8)));
            if (tap_ >= n) tap_ = n - 1;
            for (int k = 0; k < 8; ++k) {
                QSignalBlocker b(tapBtn[(size_t)k]);
                tapBtn[(size_t)k]->setChecked(k == tap_);
                tapBtn[(size_t)k]->setEnabled(k < n);
            }
            QSignalBlocker b(res);
            res->setCurrentIndex(std::max(0, tapRes_));
        });
    }
    bool tapDrag_ = false;

    // VEGAS's Reverb page (dialog 5001): the Reverberation mode and Early reflection style lists, Dry out / Reverb out /
    // Early out faders, Decay time and Pre-delay (Off at 0), and the two attenuation filters (each with its own check
    // box, slider and frequency box). Placed as the dialog places things (dialog units).
    void reverb()
    {
        const double kx = 1.75, ky = 1.875;
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(254 * kx), (int)std::lround((193 - 14) * ky));
        root_->addWidget(box);
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 14) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        auto label = [=](const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter) {
            QLabel* l = text(t, a);
            put(l, x, y, cw, ch);
            return l;
        };
        static const char* const kModes[19] = {"Rich hall", "Wide open hall", "Concert hall", "Deep hall", "Long hall", "Medium hall 1",
                                               "Medium hall 2", "Warm space", "Cavernous space", "Bright corridor", "Bright small room",
                                               "Bright medium room", "Medium room", "Subtle ambience", "Live ambience", "Warm ambience",
                                               "Plate 1", "Plate 2, ringy", "Metal tank"};
        static const char* const kStyles[10] = {"Mode 1  (6  ms)", "Mode 2  (8  ms)", "Mode 3  (10 ms)", "Mode 4  (16 ms)", "Mode 5  (20 ms)",
                                                "Mode 6  (36 ms)", "Mode 7  (8  ms, pan right)", "Mode 8  (8  ms, pan left)",
                                                "Mode 9  (16 ms, pan right)", "Mode 10 (16 ms, pan left)"};
        QStringList modes, styles;
        for (const char* m : kModes) modes << m;
        for (const char* m : kStyles) styles << m;

        QLabel* ml = label("Reverbera<u>t</u>ion mode:", 5, 19, 68, 8);
        QComboBox* mode = combo(fixed(2), modes, "dxParam2");
        put(mode, 74, 17, 175, 14);
        ml->setBuddy(mode);

        auto fader = [=](int i, int x, int readX, const QString& cap) {
            QLabel* rd = label(QString(), readX, 34, 45, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            SMap m;
            m.inf = true;
            m.dec = 0;
            DxSlider* s = slider(Qt::Vertical, fixed(i), m, QString("dxParam%1").arg(i), nullptr, rd, "dB");
            put(s, x, 42, 10, 70);
            label(cap, readX, 114, 45, 16, Qt::AlignHCenter | Qt::AlignTop);
        };
        fader(8, 19, 1, "<u>D</u>ry out<br>(-Inf. to 0 dB)");
        fader(9, 65, 47, "Re<u>v</u>erb out<br>(-Inf. to 0 dB)");
        fader(10, 110, 93, "<u>E</u>arly out<br>(-Inf. to 0 dB)");

        QLabel* sl = label("Early re<u>f</u>lection style:", 139, 40, 80, 8);
        QComboBox* style = combo(fixed(3), styles, "dxParam3");
        put(style, 139, 50, 110, 14);
        sl->setBuddy(style);

        QLabel* dl = label("De<u>c</u>ay time (0.5 to 5.0 secs.):", 139, 69, 96, 8);
        QLabel* dr = label(QString(), 235, 69, 14, 8, Qt::AlignRight | Qt::AlignVCenter);
        SMap dm;
        dm.dec = 1;
        DxSlider* ds = slider(Qt::Horizontal, fixed(1), dm, "dxParam1", nullptr, dr);
        put(ds, 139, 78, 110, 12);
        dl->setBuddy(ds);

        QLabel* pl = label("Pre-dela<u>y</u> (Off to 200 ms):", 139, 97, 90, 8);
        QLabel* pr = label(QString(), 235, 97, 14, 8, Qt::AlignRight | Qt::AlignVCenter);
        SMap pm;
        pm.dec = 0;
        DxSlider* ps = slider(Qt::Horizontal, fixed(0), pm, "dxParam0", nullptr, nullptr);
        put(ps, 139, 107, 110, 12);
        pl->setBuddy(ps);
        sync_.push_back([=] { pr->setText(get(0) < 0.5 ? QString("Off") : QString::number((int)std::lround(get(0)))); });

        QCheckBox* bass = checkBox(4, "Attenuate bass freqs. belo<u>w</u> (10 to 5,000 Hz):");
        put(bass, 5, 135, 165, 10);
        SMap bm;
        bm.log = true;
        bm.dec = 0;
        bm.comma = false;
        QLineEdit* bBox = editBox(52);
        DxSlider* bS = slider(Qt::Horizontal, fixed(5), bm, "dxParam5", bBox, nullptr);
        put(bS, 5, 146, 200, 12);
        put(bBox, 209, 146, 40, 12);

        QCheckBox* high = checkBox(6, "Attenuate high freqs. ab<u>o</u>ve (100 to 15,000 Hz):");
        put(high, 5, 163, 173, 10);
        QLineEdit* hBox = editBox(52);
        DxSlider* hS = slider(Qt::Horizontal, fixed(7), bm, "dxParam7", hBox, nullptr);
        put(hS, 5, 176, 200, 12);
        put(hBox, 209, 176, 40, 12);
        sync_.push_back([=] {
            const bool b = get(4) >= 0.5, h = get(6) >= 0.5;
            bS->setEnabled(b);
            bBox->setEnabled(b);
            hS->setEnabled(h);
            hBox->setEnabled(h);
        });
    }

    // ============================================================================================ Modern Equalizer
    // Modern Equalizer: eight bands, each Enabled, Type, Frequency, Gain, Q, Channel, Slope (setting band * 7 + that),
    // then Global Scale, Mono Below, Stereo Width, Output Gain (56..59). The graph is their combined response; a band's
    // point is pulled with the mouse (frequency across, gain up and down).
    int band_ = 0;
    int eq(int k) const { return band_ * 7 + k; }

    // Response of one band at 'freq' in dB (same filters as the effect: biquads; 12 dB per octave is one, 24 two, up to three).
    double bandDb(int b, double freq, double scale) const
    {
        const int o = b * 7;
        if (get(o) < 0.5) return 0.0;
        const int type = (int)std::lround(get(o + 1));
        const int slope = (int)std::lround(get(o + 6));
        int stages = (type <= 1 || type == 4) ? std::max(1, (slope + 2) / 2) : 1;
        stages = std::min(stages, 3);
        const double sr = 48000.0, f0 = std::max(10.0, std::min(get(o + 2), sr * 0.45));
        double db = 0;
        for (int st = 0; st < stages; ++st) {
            const double Q = std::max(0.05, st == 0 ? get(o + 4) : 0.71), gain = get(o + 3) * scale;
            const double w0 = 2 * kPi * f0 / sr, cs = std::cos(w0), sn = std::sin(w0), A = std::pow(10.0, gain / 40.0);
            const double alpha = sn / (2 * Q), sa = 2 * std::sqrt(A) * (sn / 2 * std::sqrt(2.0));
            double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
            switch (type) {
            case 0: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
            case 1: b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
            case 2:
                b0 = A * ((A + 1) - (A - 1) * cs + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sa);
                a0 = (A + 1) + (A - 1) * cs + sa; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sa;
                break;
            case 3:
                b0 = A * ((A + 1) + (A - 1) * cs + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sa);
                a0 = (A + 1) - (A - 1) * cs + sa; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sa;
                break;
            case 4: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
            default: b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A; break;
            }
            const double w = 2 * kPi * std::max(1.0, std::min(freq, sr * 0.499)) / sr;
            const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
            const std::complex<double> num = b0 + b1 * z1 + b2 * z2, den = a0 + a1 * z1 + a2 * z2;
            const double mag = std::abs(num) / std::max(1e-12, std::abs(den));
            db += 20.0 * std::log10(std::max(1e-9, mag));
        }
        return db;
    }

    void modernEq()
    {
        Graph* g = graph(QSize(544, 224));
        g->margins = QMargins(32, 6, 8, 16);
        g->setObjectName("dxEqGraph");
        auto fx = [](const QRectF& pl, double hz) { return pl.left() + std::log(hz / 20.0) / std::log(1000.0) * pl.width(); };
        auto gy = [](const QRectF& pl, double db) { return pl.center().y() - db / 24.0 * (pl.height() / 2); };
        g->draw = [this, fx, gy](QPainter& p, const QRectF& pl) {
            p.setPen(QColor(205, 205, 205));
            for (int db = -24; db <= 24; db += 6) p.drawLine(QPointF(pl.left(), std::round(gy(pl, db)) + 0.5), QPointF(pl.right(), std::round(gy(pl, db)) + 0.5));
            static const double marks[] = {20, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 2000, 3000, 4000, 5000,
                                           6000, 7000, 8000, 9000, 10000, 20000};
            for (double hz : marks) p.drawLine(QPointF(std::round(fx(pl, hz)) + 0.5, pl.top()), QPointF(std::round(fx(pl, hz)) + 0.5, pl.bottom()));
            p.setPen(QColor(110, 110, 110));
            for (int db : {-24, -12, 0, 12, 24}) p.drawLine(QPointF(pl.left(), std::round(gy(pl, db)) + 0.5), QPointF(pl.right(), std::round(gy(pl, db)) + 0.5));
            for (double hz : {100.0, 1000.0, 10000.0}) p.drawLine(QPointF(std::round(fx(pl, hz)) + 0.5, pl.top()), QPointF(std::round(fx(pl, hz)) + 0.5, pl.bottom()));
            p.setPen(Qt::black);
            for (int db : {-24, -12, 0, 12, 24}) p.drawText(QRectF(0, gy(pl, db) - 7, pl.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db));
            for (const auto& m : {std::make_pair(20.0, "20"), std::make_pair(100.0, "100"), std::make_pair(1000.0, "1k"), std::make_pair(10000.0, "10k"), std::make_pair(20000.0, "20k")})
                p.drawText(QRectF(fx(pl, m.first) - 20, pl.bottom() + 1, 40, 14), Qt::AlignHCenter | Qt::AlignTop, m.second);
            p.setPen(QColor(60, 60, 60));
            p.setBrush(Qt::NoBrush);
            p.drawRect(pl.adjusted(0.5, 0.5, -0.5, -0.5));
            const double scale = get(56) / 100.0;
            QPainterPath path;
            const int n = std::max(2, (int)pl.width());
            for (int k = 0; k <= n; ++k) {
                const double hz = 20.0 * std::pow(1000.0, (double)k / n);
                double db = 0;
                for (int b = 0; b < 8; ++b) db += bandDb(b, hz, scale);
                db = std::max(-24.0, std::min(24.0, db));
                const QPointF pt(pl.left() + k * pl.width() / n, gy(pl, db));
                if (k == 0) path.moveTo(pt);
                else path.lineTo(pt);
            }
            p.setClipRect(pl.adjusted(1, 1, -1, -1));
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(0, 110, 210), 1.6));
            p.drawPath(path);
            p.setClipping(false);
            for (int b = 0; b < 8; ++b) {
                if (get(b * 7) < 0.5) continue;
                const int type = (int)std::lround(get(b * 7 + 1));
                const bool hasGain = type == 2 || type == 3 || type == 5;
                const QPointF c(fx(pl, std::max(20.0, std::min(20000.0, get(b * 7 + 2)))), gy(pl, hasGain ? std::max(-24.0, std::min(24.0, get(b * 7 + 3) * scale)) : 0.0));
                p.setPen(QPen(Qt::black, b == band_ ? 2 : 1));
                p.setBrush(b == band_ ? QColor(255, 214, 90) : QColor(255, 255, 255));
                p.drawEllipse(dot(c, 7.5));
                p.setPen(Qt::black);
                p.drawText(dot(c, 7.5), Qt::AlignCenter, QString::number(b + 1));
            }
        };
        g->mouse = [this, g, fx, gy](const QPointF& at, int phase) {
            const QRectF pl = g->plot();
            const QPointF px(pl.left() + at.x() * pl.width(), pl.bottom() - at.y() * pl.height());
            if (phase == 0) {
                int best = -1;
                double bestD = 11;
                const double scale = get(56) / 100.0;
                for (int b = 0; b < 8; ++b) {
                    if (get(b * 7) < 0.5) continue;
                    const int type = (int)std::lround(get(b * 7 + 1));
                    const bool hasGain = type == 2 || type == 3 || type == 5;
                    const QPointF c(fx(pl, std::max(20.0, std::min(20000.0, get(b * 7 + 2)))), gy(pl, hasGain ? std::max(-24.0, std::min(24.0, get(b * 7 + 3) * scale)) : 0.0));
                    const double d = std::hypot(c.x() - px.x(), c.y() - px.y());
                    if (d < bestD) { bestD = d; best = b; }
                }
                if (best < 0) return;
                dragBand_ = best;
                selectBand(best);
                beginDrag(best * 7 + 2);
                return;
            }
            if (dragBand_ < 0) return;
            if (phase == 1) {
                const int o = dragBand_ * 7;
                const double hz = 20.0 * std::pow(1000.0, std::max(0.0, std::min(1.0, at.x())));
                commit(o + 2, std::round(hz));
                const int type = (int)std::lround(get(o + 1));
                if (type == 2 || type == 3 || type == 5) {
                    const double scale = std::max(0.01, std::fabs(get(56) / 100.0));
                    commit(o + 3, std::round((at.y() - 0.5) * 2 * 24.0 / scale * 10) / 10.0);
                }
            } else {
                dragBand_ = -1;
                endDrag();
            }
        };
        root_->addWidget(g);

        QHBoxLayout* bands = new QHBoxLayout();
        bands->setSpacing(4);
        bands->addWidget(text("Band:"));
        bandButtons_ = new QButtonGroup(this);
        for (int b = 0; b < 8; ++b) {
            QPushButton* pb = new QPushButton(QString::number(b + 1), this);
            pb->setObjectName(QString("dxEqBand%1").arg(b));
            pb->setCheckable(true);
            pb->setFixedSize(28, 22);
            pb->setFocusPolicy(Qt::TabFocus);
            bandButtons_->addButton(pb, b);
            bands->addWidget(pb);
            QObject::connect(pb, &QPushButton::clicked, pb, [this, b] { selectBand(b); });
        }
        bands->addSpacing(10);
        QCheckBox* en = new QCheckBox("&Enabled", this);
        en->setObjectName("dxEqEnabled");
        QObject::connect(en, &QCheckBox::toggled, en, [this](bool on) { commit(eq(0), on ? 1 : 0); });
        sync_.push_back([=] {
            QSignalBlocker b(en);
            en->setChecked(get(eq(0)) >= 0.5);
            for (int k = 0; k < 8; ++k) {
                QAbstractButton* pb = bandButtons_->button(k);
                QSignalBlocker bb(pb);
                pb->setChecked(k == band_);
                QFont f = pb->font();
                f.setBold(get(k * 7) >= 0.5);
                pb->setFont(f);
            }
        });
        bands->addWidget(en);
        bands->addStretch(1);
        root_->addLayout(bands);

        QHBoxLayout* kinds = new QHBoxLayout();
        kinds->setSpacing(6);
        kinds->addWidget(text("Type:"));
        kinds->addWidget(combo([this] { return eq(1); }, {"High Pass", "Low Pass", "Low Shelving", "High Shelving", "Band Pass", "Peaking"}, "dxEqType"));
        kinds->addSpacing(6);
        kinds->addWidget(text("Channel:"));
        kinds->addWidget(combo([this] { return eq(5); }, {"Stereo", "Left", "Right", "Mid", "Side"}, "dxEqChannel"));
        kinds->addSpacing(6);
        kinds->addWidget(text("Slope:"));
        kinds->addWidget(combo([this] { return eq(6); }, {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct"}, "dxEqSlope"));
        kinds->addStretch(1);
        root_->addLayout(kinds);

        SMap freq;
        freq.log = true;
        freq.dec = 0;
        root_->addWidget(lineRow(120, [this] { return eq(2); }, "<u>F</u>requency (Hz):", freq, "dxEqFrequency"));
        SMap gain;
        gain.dec = 1;
        root_->addWidget(lineRow(120, [this] { return eq(3); }, "<u>G</u>ain (dB):", gain, "dxEqGain"));
        SMap q;
        q.dec = 2;
        q.log = true;
        root_->addWidget(lineRow(120, [this] { return eq(4); }, "<u>Q</u>:", q, "dxEqQ"));
        QFrame* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        root_->addWidget(line);
        SMap pc;
        pc.dec = 0;
        root_->addWidget(lineRow(120, fixed(56), "Global <u>s</u>cale (%):", pc, "dxParam56"));
        root_->addWidget(lineRow(120, fixed(57), "<u>M</u>ono below (Hz):", pc, "dxParam57"));
        root_->addWidget(lineRow(120, fixed(58), "Stereo <u>w</u>idth (%):", pc, "dxParam58"));
        SMap og;
        og.dec = 1;
        root_->addWidget(lineRow(120, fixed(59), "<u>O</u>utput gain (dB):", og, "dxParam59"));
    }

    void selectBand(int b)
    {
        if (b < 0 || b > 7 || b == band_) return;
        band_ = b;
        syncAll();
    }
    int dragBand_ = -1;
    QButtonGroup* bandButtons_ = nullptr;

#include "fxpages_pack2.inc"
#include "fxpages_xp1.inc"
#include "fxpages_xp2.inc"
#include "fxpages_xp3.inc"
};

const char* const kUids[] = {"reno.xptimestretch", "reno.timestretch", "reno.distortion", "reno.vibrato", "reno.pitchshift", "reno.chorus", "reno.flangewah",
                             "reno.ampmod", "reno.simpledelay", "reno.reverb", "reno.moderneq", "reno.smooth", "reno.gapsnip", "reno.multitapdelay"};

}

bool hasBuiltinPage(const std::string& uid)
{
    if (vegtrk::hasPage(uid)) return true;
    for (const char* u : kUids)
        if (uid == u) return true;
    return Page::pack2Has(uid) || Page::xp1Has(uid) || Page::xp2Has(uid) || Page::xp3Has(uid);
}

DxPage* makeBuiltinPage(QWidget* parent, const std::string& uid, const DxHost& host)
{
    if (!hasBuiltinPage(uid)) return nullptr;
    return new Page(parent, uid, host);
}

}
}
