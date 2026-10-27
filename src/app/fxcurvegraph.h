// A graph of points joined by straight lines on a grid, with draggable points, as VEGAS's envelope controls of its
// DirectX effects (Graphic EQ, Graphic Dynamics ...): click the line to add a point and drag it; drag a point to move
// it between its neighbours (the end points only up and down); double-click or right-click a point to delete it.
// The data are integers in the effect's own units (Graphic EQ: x in 1/1000 octaves above 20 Hz, y a slider position);
// the page says how they are drawn (a function that paints the grid and labels behind the line) and how they read.
#pragma once
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace reno {
namespace app {

class CurveGraph : public QWidget {
public:
    struct Pt {
        int x, y;
    };
    std::vector<Pt> pts;
    int xMin = 0, xMax = 10000, yMin = -500, yMax = 400;     // the data's range (the plot's edges)
    int maxPoints = 64;
    bool fixedEnds = true;                                   // the first and last point keep their x
    QMargins margins = QMargins(30, 4, 8, 16);
    std::function<void(int phase)> edited;                   // 0 start of a drag, 1 during, 2 end (a delete: 0 then 2)
    std::function<void(QPainter&, const CurveGraph&)> paintGrid;   // the grid and its labels (the plot is white already)
    std::function<QString(const Pt&)> describe;              // what the readout says of a point
    std::function<QString(const Pt&)> describeBare;          // ... of the mouse over the plot (null: nothing)
    std::function<void(const QString&)> readout;             // where that goes

    explicit CurveGraph(QWidget* parent) : QWidget(parent) { setMouseTracking(true); }
    bool dragging() const { return drag_ >= 0; }

    QRectF plot() const { return QRectF(rect().marginsRemoved(margins)); }
    QPointF at(double x, double y) const
    {
        const QRectF pl = plot();
        return QPointF(pl.left() + (x - xMin) / (double)(xMax - xMin) * pl.width(), pl.bottom() - (y - yMin) / (double)(yMax - yMin) * pl.height());
    }
    Pt value(const QPointF& q) const
    {
        const QRectF pl = plot();
        const double x = xMin + (q.x() - pl.left()) / std::max(1.0, pl.width()) * (xMax - xMin);
        const double y = yMin + (pl.bottom() - q.y()) / std::max(1.0, pl.height()) * (yMax - yMin);
        return Pt{(int)std::lround(std::max((double)xMin, std::min((double)xMax, x))), (int)std::lround(std::max((double)yMin, std::min((double)yMax, y)))};
    }
    // The index of the point at pixel q, or -1
    int hit(const QPointF& q) const
    {
        for (size_t k = 0; k < pts.size(); ++k) {
            const QPointF c = at(pts[k].x, pts[k].y);
            if (std::fabs(q.x() - c.x()) <= 5 && std::fabs(q.y() - c.y()) <= 5) return (int)k;
        }
        return -1;
    }
    // The segment under pixel q (its first point), or -1
    int onLine(const QPointF& q) const
    {
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const QPointF a = at(pts[k].x, pts[k].y), b = at(pts[k + 1].x, pts[k + 1].y);
            const QPointF d = b - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            double t = len2 > 0 ? ((q.x() - a.x()) * d.x() + (q.y() - a.y()) * d.y()) / len2 : 0;
            t = std::max(0.0, std::min(1.0, t));
            const QPointF c = a + t * d;
            if (std::hypot(q.x() - c.x(), q.y() - c.y()) <= 4) return (int)k;
        }
        return -1;
    }

protected:
    void show(const Pt& p)
    {
        if (readout && describe) readout(describe(p));
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        const QRectF pl = plot();
        p.fillRect(pl, Qt::white);
        if (paintGrid) paintGrid(p, *this);
        p.setClipRect(pl.adjusted(-4, -4, 4, 4));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(Qt::black, 1));
        for (size_t k = 0; k + 1 < pts.size(); ++k) p.drawLine(at(pts[k].x, pts[k].y), at(pts[k + 1].x, pts[k + 1].y));
        p.setRenderHint(QPainter::Antialiasing, false);
        for (size_t k = 0; k < pts.size(); ++k) {
            const QPointF c = at(pts[k].x, pts[k].y);
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
            if (seg < 0 || pts.size() >= (size_t)maxPoints) return;
            Pt np = value(q);
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
            else if (readout) {
                if (describeBare && plot().contains(e->position())) readout(describeBare(value(e->position())));
                else readout(QString());
            }
            setCursor(k >= 0 || onLine(e->position()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            return;
        }
        Pt np = value(e->position());
        const size_t k = (size_t)drag_;
        if (fixedEnds && k == 0) np.x = pts[0].x;
        else if (fixedEnds && k + 1 == pts.size()) np.x = pts[k].x;
        else {
            if (k > 0) np.x = std::max(pts[k - 1].x, np.x);
            if (k + 1 < pts.size()) np.x = std::min(pts[k + 1].x, np.x);
        }
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
        if (drag_ < 0 && readout) readout(QString());
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

}  // namespace app
}  // namespace reno
