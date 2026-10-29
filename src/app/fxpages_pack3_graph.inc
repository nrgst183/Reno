// The graph of the sfppack3_x64.dll effects that have one (Vibrato, Amplitude Modulation): points joined by straight lines
// on a grid, time across (0 to 1000 for one cycle of the modulation), a level up (a range of whole numbers). It is part of
// fxpages.cpp (included in its anonymous namespace). Click the line to add a point (up to the most the effect takes) and
// drag it; drag a point to move it between its neighbours (the end points only up and down); double-click or right-click
// a point to delete it.
class PointGraph : public QWidget {
public:
    struct P {
        int x = 0, y = 0;
    };
    std::vector<P> pts;
    int maxPoints = 32;
    int xMax = 1000;
    int yMin = -100, yMax = 100;
    int gridX = 10, gridY = 4;                  // grid lines across and up (the cells)
    std::function<void(int phase)> edited;      // 0 start of a drag, 1 during, 2 end (a delete: 0 then 2)
    std::function<QString(const P&)> describe;  // what the readout says about a point
    std::function<void()> leave;                // the mouse left (the readout goes back to its resting text)
    std::function<void(QPainter&, const QRectF&)> decorate;     // extra drawing over the plot (axis names)
    QMargins margins = QMargins(30, 4, 6, 4);

    explicit PointGraph(QWidget* parent) : QWidget(parent) { setMouseTracking(true); }
    bool dragging() const { return drag_ >= 0; }

protected:
    QRectF plot() const { return QRectF(rect().marginsRemoved(margins)); }
    QPointF at(const P& p) const
    {
        const QRectF pl = plot();
        return QPointF(pl.left() + (double)p.x / xMax * pl.width(), pl.bottom() - (double)(p.y - yMin) / (yMax - yMin) * pl.height());
    }
    P level(const QPointF& q) const
    {
        const QRectF pl = plot();
        const double x = (q.x() - pl.left()) / std::max(1.0, pl.width()) * xMax;
        const double y = yMin + (pl.bottom() - q.y()) / std::max(1.0, pl.height()) * (yMax - yMin);
        P p;
        p.x = (int)std::lround(std::max(0.0, std::min((double)xMax, x)));
        p.y = (int)std::lround(std::max((double)yMin, std::min((double)yMax, y)));
        return p;
    }
    int hit(const QPointF& q) const
    {
        for (size_t k = 0; k < pts.size(); ++k) {
            const QPointF c = at(pts[k]);
            if (std::fabs(q.x() - c.x()) <= 5 && std::fabs(q.y() - c.y()) <= 5) return (int)k;
        }
        return -1;
    }
    int onLine(const QPointF& q) const          // the segment under q (its first point), or -1
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
    void say(const P& p)
    {
        if (readout && describe) readout->setText(describe(p));
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(240, 240, 240));    // (the page's own face: the application style sheet would give the palette the dark theme)
        const QRectF pl = plot();
        p.fillRect(pl, Qt::white);
        p.setPen(QPen(QColor(160, 160, 160), 1));
        for (int k = 1; k < gridX; ++k) {
            const double x = std::round(pl.left() + pl.width() * k / gridX) + 0.5;
            p.drawLine(QPointF(x, pl.top()), QPointF(x, pl.bottom()));
        }
        for (int k = 1; k < gridY; ++k) {
            const double y = std::round(pl.top() + pl.height() * k / gridY) + 0.5;
            p.drawLine(QPointF(pl.left(), y), QPointF(pl.right(), y));
        }
        p.setPen(QPen(QColor(70, 70, 70), 1));
        p.drawRect(pl.adjusted(0.5, 0.5, -0.5, -0.5));
        if (decorate) decorate(p, pl);
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
            if (seg < 0 || pts.size() >= (size_t)maxPoints) return;
            P np = level(q);
            np.x = std::max(pts[(size_t)seg].x, std::min(pts[(size_t)seg + 1].x, np.x));
            pts.insert(pts.begin() + seg + 1, np);
            k = seg + 1;
        }
        drag_ = k;
        say(pts[(size_t)k]);
        if (edited) edited(0);
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (drag_ < 0) {
            const int k = hit(e->position());
            if (k >= 0) say(pts[(size_t)k]);
            else if (leave) leave();
            setCursor(k >= 0 || onLine(e->position()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            return;
        }
        P np = level(e->position());
        const size_t k = (size_t)drag_;
        if (k == 0) np.x = 0;
        else if (k + 1 == pts.size()) np.x = xMax;
        else np.x = std::max(pts[k - 1].x, std::min(pts[k + 1].x, np.x));
        if (np.x == pts[k].x && np.y == pts[k].y) return;
        pts[k] = np;
        say(np);
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
        if (drag_ < 0 && leave) leave();
    }

public:
    QLabel* readout = nullptr;

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

