// A scroll bar that caches its image. Qt's Fusion style draws a scroll bar from hundreds of small shapes (about 4 ms for a
// timeline-wide one) and the timeline repaints constantly, so the bar is drawn into an image and reused until its value, range,
// size, state, palette or style changes.
#pragma once
#include <QCursor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <functional>

namespace reno {
namespace app {

class CachedScrollBar : public QScrollBar {
public:
    explicit CachedScrollBar(Qt::Orientation o, QWidget* parent = nullptr) : QScrollBar(o, parent) { setMouseTracking(true); }

    // VEGAS's scroll bars: dragging an end of the scroll box also zooms, and a double-click zooms out to everything. Off
    // until onEdgeDrag is set.
    // onEdgeDrag(phase, edge, pos, grooveLen): phase 0 pressed, 1 moved, 2 released; edge 0 start, 1 far end; pos =
    // wanted end position in
    // pixels from the groove start (grab offset already removed).
    std::function<void(int phase, int edge, double pos, double grooveLen)> onEdgeDrag;
    std::function<void()> onDoubleClick;

    // Handle highlight: 0 none, 1 mouse over, 2 held; and the part under the mouse: 0 none, 1 body, 2 start grip, 3 far grip.
    int lit() const { return litLevel(); }
    int hoverPart() const { return hover_; }

protected:
    // Scroll box and groove in the bar's pixels along its direction (a, b: start, end).
    bool boxExtent(double& groove0, double& grooveLen, double& boxA, double& boxB) const
    {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const bool vert = orientation() == Qt::Vertical;
        const QRect g = style()->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarGroove, this);
        const QRect s = style()->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarSlider, this);
        groove0 = vert ? g.top() : g.left();
        grooveLen = vert ? g.height() : g.width();
        if (grooveLen < 8) return false;
        // nothing to scroll: the box fills the groove
        boxA = minimum() == maximum() ? groove0 : (vert ? s.top() : s.left());
        boxB = minimum() == maximum() ? groove0 + grooveLen : (vert ? s.bottom() + 1 : s.right() + 1);
        return true;
    }
    // Which end of the scroll box 'pos' is at: 0, 1 or -1. The grip is the ridged part at each end (and 2 px outside); a
    // small box keeps its middle third for dragging.
    int edgeAt(double pos, double* endPos = nullptr) const
    {
        if (!onEdgeDrag) return -1;
        double g0, gl, a, b;
        if (!boxExtent(g0, gl, a, b)) return -1;
        const double zone = std::min(9.0, (b - a) / 3.0);
        if (pos >= a - 2 && pos < a + zone) {
            if (endPos) *endPos = a;
            return 0;
        }
        if (pos >= b - zone && pos < b + 2) {
            if (endPos) *endPos = b;
            return 1;
        }
        return -1;
    }
    // Which part of the handle 'pos' is over: 0 none, 1 body, 2 start grip, 3 far grip.
    int partAt(double pos) const
    {
        double g0, gl, a, b;
        if (!boxExtent(g0, gl, a, b)) return 0;
        if (onEdgeDrag) {
            const int e = edgeAt(pos);
            if (e >= 0) return 2 + e;
        }
        return pos >= a && pos < b ? 1 : 0;
    }
    int litLevel() const { return edgeDrag_ >= 0 || isSliderDown() ? 2 : (hover_ != 0 ? 1 : 0); }
    void setHover(int part)
    {
        if (part == hover_) return;
        hover_ = part;
        update();
    }
    double along(const QPointF& p) const { return orientation() == Qt::Vertical ? p.y() : p.x(); }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && onEdgeDrag) {
            double endPos = 0;
            const int edge = edgeAt(along(e->position()), &endPos);
            if (edge >= 0) {
                edgeDrag_ = edge;
                grab_ = along(e->position()) - endPos;
                setCursor(orientation() == Qt::Vertical ? Qt::SizeVerCursor : Qt::SizeHorCursor);
                sendEdge(0, e);
                e->accept();
                update();
                return;
            }
        }
        QScrollBar::mousePressEvent(e);
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (edgeDrag_ >= 0) {
            sendEdge(1, e);
            e->accept();
            return;
        }
        if (onEdgeDrag && !(e->buttons() & Qt::LeftButton)) {
            const bool vert = orientation() == Qt::Vertical;
            const int edge = edgeAt(along(e->position()));
            setCursor(edge >= 0 ? (vert ? Qt::SizeVerCursor : Qt::SizeHorCursor) : Qt::ArrowCursor);
        }
        // a held handle stays lit wherever the pointer goes
        if (!isSliderDown()) setHover(partAt(along(e->position())));
        QScrollBar::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (edgeDrag_ >= 0 && e->button() == Qt::LeftButton) {
            sendEdge(2, e);
            edgeDrag_ = -1;
            // pointer where it was released: lit if still over the handle
            setHover(rect().contains(e->position().toPoint()) ? partAt(along(e->position())) : 0);
            e->accept();
            update();
            return;
        }
        QScrollBar::mouseReleaseEvent(e);
        setHover(rect().contains(e->position().toPoint()) ? partAt(along(e->position())) : 0);
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (onDoubleClick && e->button() == Qt::LeftButton) {
            // on the scroll box or groove, not the arrow buttons
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QStyle::SubControl sc = style()->hitTestComplexControl(QStyle::CC_ScrollBar, &opt, e->position().toPoint(), this);
            if (sc == QStyle::SC_ScrollBarSlider || sc == QStyle::SC_ScrollBarAddPage || sc == QStyle::SC_ScrollBarSubPage || sc == QStyle::SC_ScrollBarGroove) {
                onDoubleClick();
                e->accept();
                return;
            }
        }
        QScrollBar::mouseDoubleClickEvent(e);
    }
    void enterEvent(QEnterEvent* e) override
    {
        QScrollBar::enterEvent(e);
        if (edgeDrag_ < 0 && !isSliderDown()) setHover(partAt(along(e->position())));
    }
    void leaveEvent(QEvent* e) override
    {
        if (edgeDrag_ < 0 && onEdgeDrag) setCursor(Qt::ArrowCursor);
        QScrollBar::leaveEvent(e);
        setHover(0);
    }
    void sendEdge(int phase, QMouseEvent* e)
    {
        double g0, gl, a, b;
        if (!boxExtent(g0, gl, a, b)) return;
        onEdgeDrag(phase, edgeDrag_, along(e->position()) - grab_ - g0, gl);
    }

    // VEGAS's grips: three short ridges at each end of the scroll box
    void paintGrips()
    {
        if (!onEdgeDrag) return;
        double g0, gl, a, b;
        if (!boxExtent(g0, gl, a, b) || b - a < 16) return;
        const bool vert = orientation() == Qt::Vertical;
        const int thick = vert ? width() : height();
        const int len = std::max(4, thick / 2 - 1), off = (thick - len) / 2;
        QPainter p(this);
        const int lv = litLevel();
        const QColor dark = palette().color(QPalette::Shadow);
        const QColor light = lv == 0 ? palette().color(QPalette::Light) : blend(palette().color(QPalette::Light), Qt::white, lv == 2 ? 0.9 : 0.6);
        for (int end = 0; end < 2; ++end)
            for (int k = 0; k < 3; ++k) {
                const int at = end == 0 ? (int)a + 3 + 2 * k : (int)b - 4 - 2 * k;
                if (vert) {
                    p.fillRect(off, at, len, 1, dark);
                    p.fillRect(off, at + 1, len, 1, light);
                } else {
                    p.fillRect(at, off, 1, len, dark);
                    p.fillRect(at + 1, off, 1, len, light);
                }
            }
    }

    static QColor blend(const QColor& a, const QColor& b, double t)
    {
        return QColor(int(a.red() + (b.red() - a.red()) * t + 0.5), int(a.green() + (b.green() - a.green()) * t + 0.5),
                      int(a.blue() + (b.blue() - a.blue()) * t + 0.5), a.alpha());
    }

    // Lights the handle: face lightened (darkened on a light handle), more under the grip the mouse is on, more when
    // held. Drawn over the style's output so it looks the same in every theme.
    void paintLit(QPainter& pp, const QStyleOptionSlider& opt, int level, int part) const
    {
        double g0, gl, a, b;
        if (!boxExtent(g0, gl, a, b)) return;
        const bool vert = orientation() == Qt::Vertical;
        const QColor face = palette().color(QPalette::Button);
        const bool darkFace = face.lightness() < 140;
        auto tint = [&](int alpha) { return darkFace ? QColor(255, 255, 255, alpha) : QColor(0, 0, 0, std::max(10, alpha * 2 / 3)); };
        auto rectOf = [&](double x0, double x1) {
            return vert ? QRectF(1, x0, opt.rect.width() - 2, x1 - x0) : QRectF(x0, 1, x1 - x0, opt.rect.height() - 2);
        };
        pp.fillRect(rectOf(a, b), tint(level == 2 ? 80 : 46));
        if (part >= 2) {
            const double zone = std::min(9.0, (b - a) / 3.0);
            pp.fillRect(part == 2 ? rectOf(a, a + zone) : rectOf(b - zone, b), tint(level == 2 ? 70 : 50));
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        opt.subControls = QStyle::SC_All;
        const qreal dpr = devicePixelRatioF();
        Key k;
        k.w = opt.rect.width();
        k.h = opt.rect.height();
        k.dpr = dpr;
        k.state = (int)opt.state;
        k.active = (int)opt.activeSubControls;
        k.minimum = opt.minimum;
        k.maximum = opt.maximum;
        k.position = opt.sliderPosition;
        k.value = opt.sliderValue;
        k.pageStep = opt.pageStep;
        k.upsideDown = opt.upsideDown;
        k.vertical = opt.orientation == Qt::Vertical;
        k.palette = opt.palette.cacheKey();
        k.style = style();
        k.layoutRtl = opt.direction == Qt::RightToLeft;
        k.lit = litLevel();
        k.part = k.lit == 0 ? 0 : hover_;
        k.grips = onEdgeDrag ? 1 : 0;
        if (pix_.isNull() || !(k == key_)) {
            // drawn as QScrollBar::paintEvent does, onto an image
            QImage im((QSize(k.w, k.h) * dpr).expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
            im.setDevicePixelRatio(dpr);
            im.fill(Qt::transparent);
            {
                QPainter pp(&im);
                style()->drawComplexControl(QStyle::CC_ScrollBar, &opt, &pp, this);
                if (k.lit > 0) paintLit(pp, opt, k.lit, k.part);
            }
            pix_ = QPixmap::fromImage(im);
            key_ = k;
        }
        {
            QPainter p(this);
            p.drawPixmap(0, 0, pix_);
        }
        paintGrips();
    }

private:
    struct Key {
        int w = -1, h = -1;
        qreal dpr = 0;
        int state = 0, active = 0, minimum = 0, maximum = 0, position = 0, value = 0, pageStep = 0;
        bool upsideDown = false, vertical = false, layoutRtl = false;
        qint64 palette = 0;
        int lit = 0, part = 0, grips = 0;     // highlight level, part under the mouse, whether it has grips
        const QStyle* style = nullptr;
        bool operator==(const Key& o) const
        {
            return w == o.w && h == o.h && dpr == o.dpr && state == o.state && active == o.active && minimum == o.minimum && maximum == o.maximum &&
                   position == o.position && value == o.value && pageStep == o.pageStep && upsideDown == o.upsideDown && vertical == o.vertical &&
                   layoutRtl == o.layoutRtl && palette == o.palette && style == o.style && lit == o.lit && part == o.part && grips == o.grips;
        }
    };
    Key key_;
    QPixmap pix_;
    int hover_ = 0;         // part of the handle under the mouse (partAt), 0 = none
    int edgeDrag_ = -1;     // end of the scroll box being dragged, or -1
    double grab_ = 0;       // pointer's distance from that end when pressed
};

}  // namespace app
}  // namespace reno
