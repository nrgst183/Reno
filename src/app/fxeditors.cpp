// Editors of the effects window for VEGAS's own effects; see fxeditors.h.
#include "fxeditors.h"
#include "../core/base.h"
#include "../ofx/vegascurves.h"
#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace app {

namespace {

const double kPi = 3.14159265358979323846;

// A number box with no arrows, like the effects window's rows.
QDoubleSpinBox* numberBox(QWidget* parent, const char* name, double lo, double hi, int decimals, double step, int width = 56)
{
    QDoubleSpinBox* b = new QDoubleSpinBox(parent);
    b->setObjectName(name);
    b->setRange(lo, hi);
    b->setDecimals(decimals);
    b->setSingleStep(step);
    b->setButtonSymbols(QAbstractSpinBox::NoButtons);
    b->setKeyboardTracking(false);
    b->setFixedWidth(width);
    b->setFocusPolicy(Qt::StrongFocus);
    return b;
}

void showNumber(QDoubleSpinBox* b, double v)
{
    if (b->hasFocus()) {
        QLineEdit* le = b->findChild<QLineEdit*>();
        if (le && le->isModified()) return;
    }
    if (std::fabs(b->value() - v) < 0.5 * std::pow(10.0, -b->decimals())) return;
    QSignalBlocker bl(b);
    b->setValue(v);
}

const EdValue* edFind(const EdValues& v, const std::string& name)
{
    auto it = v.find(name);
    return (it != v.end() && it->second.present) ? &it->second : nullptr;
}

double numberOf(const EdValues& v, const std::string& name, double def)
{
    const EdValue* e = edFind(v, name);
    return (e && !e->v.empty()) ? e->v[0] : def;
}

// ------------------------------------------------------------------------------------------ the curve graph
// Up to four curves in a square, drawn as VEGAS does: dark box with grid, line for the curve, squares for points,
// handles on the picked point.
// Click to add a point; drag a point or handle; double-click a point (or Delete, or right button) removes it. Shift
// while dragging a handle lets it turn freely.
class CurveGraph : public QWidget {
public:
    struct Chan {
        QString name;
        QColor colour;
        ofx::BezCurve pts;
    };
    std::vector<Chan> chans;
    double range = 255;
    int edit = 0;
    bool anyChannel = false;
    bool showOthers = true;
    int selChan = 0, selPoint = -1;
    std::function<void()> onBegin, onEnd, onSelect;
    std::function<void(int chan)> onChange;

    explicit CurveGraph(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxCurveGraph");
        setFixedSize(256, 256);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(false);
    }
    bool dragging() const { return drag_ != None; }
    QRectF area() const { return QRectF(rect()).adjusted(8, 8, -8, -8); }
    QPointF toView(double x, double y) const
    {
        const QRectF a = area();
        return QPointF(a.left() + x / range * a.width(), a.bottom() - y / range * a.height());
    }
    void fromView(const QPointF& p, double* x, double* y) const
    {
        const QRectF a = area();
        *x = std::min(range, std::max(0.0, (p.x() - a.left()) / a.width() * range));
        *y = std::min(range, std::max(0.0, (a.bottom() - p.y()) / a.height() * range));
    }
    void select(int chan, int point)
    {
        if (chan == selChan && point == selPoint) return;
        selChan = chan;
        selPoint = point;
        update();
        if (onSelect) onSelect();
    }
    bool handlePos(int which, QPointF* at) const
    {
        if (selChan < 0 || selChan >= (int)chans.size() || selPoint < 0) return false;
        const ofx::BezCurve& c = chans[(size_t)selChan].pts;
        const size_t i = (size_t)selPoint;
        if (i >= c.size()) return false;
        double dx, dy;
        if (which == 0) {
            if (i == 0) return false;
            ofx::bezHandleIn(c, i, &dx, &dy);
        } else {
            if (i + 1 >= c.size()) return false;
            ofx::bezHandleOut(c, i, &dx, &dy);
        }
        *at = toView(c[i].x + dx, c[i].y + dy);
        return true;
    }
    void deleteSelected()
    {
        if (selChan < 0 || selChan >= (int)chans.size() || selPoint < 0) return;
        ofx::BezCurve& c = chans[(size_t)selChan].pts;
        if (!ofx::bezRemove(c, (size_t)selPoint)) return;
        const int ch = selChan;
        selPoint = -1;
        if (onBegin) onBegin();
        touched(ch);
        if (onEnd) onEnd();
        if (onSelect) onSelect();
        update();
    }
    void moveSelected(double x, double y)
    {
        if (selChan < 0 || selChan >= (int)chans.size() || selPoint < 0) return;
        ofx::BezCurve& c = chans[(size_t)selChan].pts;
        if ((size_t)selPoint >= c.size()) return;
        if (onBegin) onBegin();
        ofx::bezMove(c, (size_t)selPoint, x, y, range);
        touched(selChan);
        if (onEnd) onEnd();
        update();
    }
    QSize sizeHint() const override { return QSize(256, 256); }

protected:
    enum Drag { None, Point, HandleIn, HandleOut };

    void touched(int chan)
    {
        ofx::bezNormalise(chans[(size_t)chan].pts);
        if (onChange) onChange(chan);
    }
    int nearestCurve(const QPointF& at) const
    {
        if (!anyChannel) return edit;
        double x, y;
        fromView(at, &x, &y);
        int best = edit;
        double bestD = 1e300;
        for (size_t k = 0; k < chans.size(); ++k) {
            std::vector<std::pair<double, double>> poly;
            ofx::bezPolyline(chans[k].pts, 24, poly);
            for (size_t i = 0; i + 1 < poly.size(); ++i) {
                if (poly[i + 1].first < x || poly[i].first > x) continue;
                const double span = poly[i + 1].first - poly[i].first;
                const double yy = span > 1e-9 ? poly[i].second + (poly[i + 1].second - poly[i].second) * (x - poly[i].first) / span : poly[i].second;
                if (std::fabs(yy - y) < bestD) { bestD = std::fabs(yy - y); best = (int)k; }
            }
        }
        return best;
    }
    bool canEdit(int k) const { return anyChannel || k == edit; }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(20, 20, 22));
        const QRectF a = area();
        p.setPen(QPen(QColor(64, 64, 68), 1));
        for (int k = 0; k <= 4; ++k) {
            const double f = k / 4.0;
            p.drawLine(QPointF(a.left() + a.width() * f, a.top()), QPointF(a.left() + a.width() * f, a.bottom()));
            p.drawLine(QPointF(a.left(), a.top() + a.height() * f), QPointF(a.right(), a.top() + a.height() * f));
        }
        p.setPen(QPen(QColor(90, 90, 96), 1, Qt::DashLine));
        p.drawLine(toView(0, 0), toView(range, range));
        p.setClipRect(a.adjusted(-1, -1, 1, 1));
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t k = 0; k < chans.size(); ++k) {
                const bool current = (int)k == edit || anyChannel;
                if ((pass == 0) == current) continue;
                if (!current && !showOthers) continue;
                std::vector<std::pair<double, double>> poly;
                ofx::bezPolyline(chans[k].pts, 32, poly);
                QPainterPath path;
                for (size_t i = 0; i < poly.size(); ++i) {
                    const QPointF v = toView(poly[i].first, std::min(range, std::max(0.0, poly[i].second)));
                    if (i == 0) path.moveTo(v);
                    else path.lineTo(v);
                }
                QColor col = chans[k].colour;
                if (!current) col.setAlpha(110);
                p.setPen(QPen(col, current ? 2.0 : 1.0));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
                if (!current) continue;
                for (size_t i = 0; i < chans[k].pts.size(); ++i) {
                    const QPointF v = toView(chans[k].pts[i].x, chans[k].pts[i].y);
                    const bool picked = (int)k == selChan && (int)i == selPoint;
                    p.setPen(QPen(QColor(10, 10, 10), 1));
                    p.setBrush(picked ? QColor(255, 255, 255) : col);
                    p.drawRect(QRectF(v.x() - 3.5, v.y() - 3.5, 7, 7));
                }
            }
        }
        const QPointF at = selPointView();
        QPointF h;
        for (int which = 0; which < 2; ++which) {
            if (!handlePos(which, &h)) continue;
            p.setPen(QPen(QColor(200, 200, 205), 1));
            p.drawLine(at, h);
            p.setBrush(QColor(240, 240, 120));
            p.setPen(QPen(QColor(10, 10, 10), 1));
            p.drawEllipse(h, 3.5, 3.5);
        }
        p.setClipping(false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(hasFocus() ? QColor(120, 160, 230) : QColor(110, 110, 116), 1));
        p.drawRect(a);
    }
    QPointF selPointView() const
    {
        if (selChan < 0 || selChan >= (int)chans.size() || selPoint < 0 || selPoint >= (int)chans[(size_t)selChan].pts.size()) return QPointF();
        const ofx::BezPoint& pt = chans[(size_t)selChan].pts[(size_t)selPoint];
        return toView(pt.x, pt.y);
    }
    bool pointAt(const QPointF& at, int* chan, int* idx) const
    {
        std::vector<int> order;
        order.push_back(anyChannel ? selChan : edit);
        for (int k = 0; k < (int)chans.size(); ++k)
            if (canEdit(k) && k != order[0]) order.push_back(k);
        for (int k : order) {
            if (k < 0 || k >= (int)chans.size()) continue;
            const ofx::BezCurve& c = chans[(size_t)k].pts;
            for (size_t i = 0; i < c.size(); ++i)
                if (QLineF(at, toView(c[i].x, c[i].y)).length() <= 7.5) {
                    *chan = k;
                    *idx = (int)i;
                    return true;
                }
        }
        return false;
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        setFocus();
        const QPointF at = e->position();
        int ch = 0, idx = -1;
        if (e->button() == Qt::RightButton) {
            if (pointAt(at, &ch, &idx)) {
                select(ch, idx);
                deleteSelected();
            }
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        QPointF h;
        for (int which = 0; which < 2; ++which)
            if (handlePos(which, &h) && QLineF(at, h).length() <= 7.0) {
                drag_ = which == 0 ? HandleIn : HandleOut;
                if (onBegin) onBegin();
                return;
            }
        if (pointAt(at, &ch, &idx)) {
            select(ch, idx);
            drag_ = Point;
            if (onBegin) onBegin();
            return;
        }
        ch = nearestCurve(at);
        if (ch < 0 || ch >= (int)chans.size()) return;
        double x, y;
        fromView(at, &x, &y);
        if (onBegin) onBegin();
        const size_t i = ofx::bezInsert(chans[(size_t)ch].pts, x, y, range);
        selChan = ch;
        selPoint = (int)i;
        drag_ = Point;
        touched(ch);
        if (onSelect) onSelect();
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (drag_ == None || selChan < 0 || selChan >= (int)chans.size() || selPoint < 0) return;
        ofx::BezCurve& c = chans[(size_t)selChan].pts;
        const size_t i = (size_t)selPoint;
        if (i >= c.size()) return;
        double x, y;
        fromView(e->position(), &x, &y);
        if (drag_ == Point) {
            ofx::bezMove(c, i, x, y, range);
        } else {
            if (e->modifiers() & Qt::ShiftModifier) c[i].smooth = false;
            // the handle follows the mouse, not clamped to the box; only its length is limited by the gap
            const QRectF a = area();
            const double mx = (e->position().x() - a.left()) / a.width() * range, my = (a.bottom() - e->position().y()) / a.height() * range;
            if (drag_ == HandleIn) ofx::bezSetHandleIn(c, i, mx - c[i].x, my - c[i].y);
            else ofx::bezSetHandleOut(c, i, mx - c[i].x, my - c[i].y);
        }
        touched(selChan);
        if (onSelect) onSelect();
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (drag_ == None) return;
        drag_ = None;
        if (onEnd) onEnd();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        int ch = 0, idx = -1;
        if (e->button() == Qt::LeftButton && pointAt(e->position(), &ch, &idx)) {
            select(ch, idx);
            deleteSelected();
        }
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            deleteSelected();
            return;
        }
        if (selChan >= 0 && selChan < (int)chans.size() && selPoint >= 0 && (size_t)selPoint < chans[(size_t)selChan].pts.size()) {
            const ofx::BezPoint& pt = chans[(size_t)selChan].pts[(size_t)selPoint];
            const double step = range / 255.0 * (e->modifiers() & Qt::ShiftModifier ? 10 : 1);
            switch (e->key()) {
            case Qt::Key_Left: moveSelected(pt.x - step, pt.y); return;
            case Qt::Key_Right: moveSelected(pt.x + step, pt.y); return;
            case Qt::Key_Up: moveSelected(pt.x, pt.y + step); return;
            case Qt::Key_Down: moveSelected(pt.x, pt.y - step); return;
            default: break;
            }
        }
        QWidget::keyPressEvent(e);
    }
    void focusInEvent(QFocusEvent* e) override { update(); QWidget::focusInEvent(e); }
    void focusOutEvent(QFocusEvent* e) override { update(); QWidget::focusOutEvent(e); }

private:
    Drag drag_ = None;
};

const QColor kRed(235, 80, 80), kGreen(90, 205, 90), kBlue(95, 145, 245), kMaster(235, 235, 235);

// ------------------------------------------------------------------------------------------ curve editors
// Color Curves (one "Property" text with the four curves, shown channel in "SelectedChannel") and Color Grading's Master
// / Red / Green / Blue curves (a text each): the graph with channel pickers and the picked point's exact numbers.
class CurveEditor : public FxEditor {
public:
    CurveEditor(QWidget* parent, bool grading) : FxEditor(parent), grading_(grading)
    {
        setObjectName(grading ? "fxGradingCurves" : "fxColorCurves");
        QHBoxLayout* h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);
        graph_ = new CurveGraph(this);
        graph_->range = grading ? 1.0 : 255.0;
        graph_->chans.resize(4);
        const char* names[4] = {"Red", "Green", "Blue", "Master"};
        const QColor cols[4] = {kRed, kGreen, kBlue, kMaster};
        for (int k = 0; k < 4; ++k) {
            graph_->chans[(size_t)k].name = names[k];
            graph_->chans[(size_t)k].colour = cols[k];
            graph_->chans[(size_t)k].pts = ofx::bezStraight(graph_->range);
        }
        graph_->edit = 3;
        graph_->selChan = 3;
        h->addWidget(graph_, 0, Qt::AlignTop);

        QVBoxLayout* v = new QVBoxLayout();
        v->setSpacing(3);
        h->addLayout(v, 1);
        const int order[5] = {0, 1, 2, 3, 4};
        group_ = new QButtonGroup(this);
        const int n = grading ? 4 : 5;
        for (int k = 0; k < n; ++k) {
            const int ch = grading ? (k == 0 ? 3 : k - 1) : order[k];
            QRadioButton* r = new QRadioButton(ch == 4 ? QString("All") : QString::fromLatin1(names[ch]), this);
            r->setObjectName(QString("fxChannel%1").arg(ch));
            r->setProperty("channel", ch);
            group_->addButton(r, ch);
            v->addWidget(r);
        }
        if (!grading) {
            showAll_ = new QCheckBox("Show all channels", this);
            showAll_->setObjectName("fxShowAll");
            v->addSpacing(4);
            v->addWidget(showAll_);
        }
        v->addSpacing(6);
        QGridLayout* g = new QGridLayout();
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(3);
        g->addWidget(new QLabel("Input:", this), 0, 0);
        g->addWidget(new QLabel("Output:", this), 1, 0);
        in_ = numberBox(this, "fxCurveIn", 0, graph_->range, grading ? 3 : 1, grading ? 0.01 : 1.0);
        out_ = numberBox(this, "fxCurveOut", 0, graph_->range, grading ? 3 : 1, grading ? 0.01 : 1.0);
        g->addWidget(in_, 0, 1);
        g->addWidget(out_, 1, 1);
        g->setColumnStretch(2, 1);
        v->addLayout(g);
        reset_ = new QPushButton("Reset curve", this);
        reset_->setObjectName("fxCurveReset");
        v->addWidget(reset_, 0, Qt::AlignLeft);
        v->addStretch(1);

        graph_->onBegin = [this] { if (begin) begin(); };
        graph_->onEnd = [this] { if (end) end(); };
        graph_->onChange = [this](int ch) { send(ch); };
        graph_->onSelect = [this] { showPoint(); };
        QObject::connect(group_, &QButtonGroup::idClicked, this, [this](int ch) { channelPicked(ch); });
        if (showAll_)
            QObject::connect(showAll_, &QCheckBox::toggled, this, [this](bool on) {
                graph_->showOthers = on;
                graph_->update();
                if (!updating_ && setNumbers) setNumbers("ShowAllChannels", {on ? 1.0 : 0.0});
            });
        QObject::connect(in_, &QDoubleSpinBox::valueChanged, this, [this](double) { pointTyped(); });
        QObject::connect(out_, &QDoubleSpinBox::valueChanged, this, [this](double) { pointTyped(); });
        QObject::connect(reset_, &QPushButton::clicked, this, [this] { resetCurve(); });
        if (!grading) channelPicked(0, false);
        else channelPicked(3, false);
        showPoint();
    }

    CurveGraph* graph() const { return graph_; }

    void showValues(const EdValues& values) override
    {
        if (graph_->dragging()) return;
        updating_ = true;
        if (!grading_) {
            const EdValue* prop = edFind(values, "Property");
            const std::string text = prop ? prop->text : std::string();
            if (!shown_ || text != lastText_[0]) {
                ofx::BezCurve c[4];
                if (!ofx::decodeColorCurves(text, c))
                    for (int k = 0; k < 4; ++k) c[k] = ofx::bezStraight(graph_->range);
                for (int k = 0; k < 4; ++k) {
                    if (c[k].size() < 2) c[k] = ofx::bezStraight(graph_->range);
                    graph_->chans[(size_t)k].pts = c[k];
                }
                lastText_[0] = text;
                fix();
            }
            const int sel = (int)std::floor(numberOf(values, "SelectedChannel", 0) + 0.5);
            if (sel >= 0 && sel <= 4 && sel != channel_) channelPicked(sel, false);
            const bool all = numberOf(values, "ShowAllChannels", 1) != 0;
            if (showAll_ && showAll_->isChecked() != all) {
                QSignalBlocker b(showAll_);
                showAll_->setChecked(all);
            }
            graph_->showOthers = all;
        } else {
            static const char* const names[4] = {"Curve_R", "Curve_G", "Curve_B", "Curve_Y"};
            for (int k = 0; k < 4; ++k) {
                const EdValue* e = edFind(values, names[k]);
                const std::string text = e ? e->text : std::string();
                if (shown_ && text == lastText_[k]) continue;
                ofx::BezCurve c;
                if (!ofx::decodeGradingCurve(text, c) || c.size() < 2) c = ofx::bezStraight(graph_->range);
                graph_->chans[(size_t)k].pts = c;
                lastText_[k] = text;
            }
            fix();
        }
        shown_ = true;
        graph_->update();
        showPoint();
        updating_ = false;
    }

private:
    void fix()
    {
        if (graph_->selChan < 0 || graph_->selChan >= 4) graph_->selChan = graph_->edit;
        if (graph_->selPoint >= (int)graph_->chans[(size_t)graph_->selChan].pts.size()) graph_->selPoint = -1;
    }
    void channelPicked(int ch, bool tell_ = true)
    {
        channel_ = ch;
        QSignalBlocker b(group_);
        if (QAbstractButton* r = group_->button(ch)) r->setChecked(true);
        graph_->anyChannel = ch == 4;
        graph_->edit = ch == 4 ? 3 : ch;
        graph_->selChan = graph_->edit;
        graph_->selPoint = -1;
        graph_->update();
        showPoint();
        if (tell_ && !grading_ && !updating_ && setNumbers) setNumbers("SelectedChannel", {(double)ch});
    }
    void showPoint()
    {
        const bool have = graph_->selChan >= 0 && graph_->selChan < 4 && graph_->selPoint >= 0 &&
                          (size_t)graph_->selPoint < graph_->chans[(size_t)graph_->selChan].pts.size();
        in_->setEnabled(have);
        out_->setEnabled(have);
        QSignalBlocker b1(in_), b2(out_);
        if (have) {
            const ofx::BezPoint& p = graph_->chans[(size_t)graph_->selChan].pts[(size_t)graph_->selPoint];
            showNumber(in_, p.x);
            showNumber(out_, p.y);
        }
    }
    void pointTyped()
    {
        if (updating_) return;
        graph_->moveSelected(in_->value(), out_->value());
        showPoint();
    }
    void resetCurve()
    {
        const int ch = graph_->anyChannel ? graph_->selChan : graph_->edit;
        if (ch < 0 || ch >= 4) return;
        if (begin) begin();
        graph_->chans[(size_t)ch].pts = ofx::bezStraight(graph_->range);
        graph_->selPoint = -1;
        send(ch);
        if (end) end();
        graph_->update();
        showPoint();
    }
    void send(int ch)
    {
        if (grading_) {
            static const char* const names[4] = {"Curve_R", "Curve_G", "Curve_B", "Curve_Y"};
            const std::string text = ofx::encodeGradingCurve(graph_->chans[(size_t)ch].pts);
            lastText_[ch] = text;
            if (setText) setText(names[ch], text);
        } else {
            ofx::BezCurve c[4];
            for (int k = 0; k < 4; ++k) c[k] = graph_->chans[(size_t)k].pts;
            const std::string text = ofx::encodeColorCurves(c);
            lastText_[0] = text;
            if (setText) setText("Property", text);
        }
    }

    bool grading_;
    CurveGraph* graph_ = nullptr;
    QButtonGroup* group_ = nullptr;
    QCheckBox* showAll_ = nullptr;
    QDoubleSpinBox *in_ = nullptr, *out_ = nullptr;
    QPushButton* reset_ = nullptr;
    int channel_ = 0;
    bool updating_ = false, shown_ = false;
    std::string lastText_[4];
};

// ------------------------------------------------------------------------------------------ colour wheels
// A Lift / Gamma / Gain / Offset wheel as the effect keeps it: R, G, B amounts, overall amount Y and chroma pair U, V.
// The dot is the R, G, B amounts' difference from neutral: red top, green lower left, blue lower right; boxes below give
// exact amounts.
class WheelView : public QWidget {
public:
    double x = 0, y = 0;
    bool vectorscope = false;
    std::function<void()> onBegin, onEnd, onReset;
    std::function<void(double x, double y)> onMove;

    explicit WheelView(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxWheel");
        setFixedSize(132, 132);
    }
    QRectF disc() const { return QRectF(rect()).adjusted(5, 5, -5, -5); }
    QPointF dotView() const
    {
        const QRectF r = disc();
        return QPointF(r.center().x() + x * r.width() / 2, r.center().y() - y * r.height() / 2);
    }
    bool dragging() const { return held_; }

protected:
    QColor hueAt(double angle, double radius) const
    {
        if (vectorscope) {
            const double a = angle * kPi / 180.0, u = 0.45 * std::cos(a), v = 0.45 * std::sin(a), yy = 0.6;
            const double r = yy + v / 0.877, b = yy + u / 0.492, g = (yy - 0.299 * r - 0.114 * b) / 0.587;
            auto c = [](double q) { return std::min(1.0, std::max(0.0, q)); };
            return QColor::fromRgbF(c(r), c(g), c(b));
        }
        const double hue = std::fmod((angle - 90.0) / 360.0 + 2.0, 1.0);
        return QColor::fromHsvF(hue, std::min(1.0, radius), 1.0);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = disc();
        QConicalGradient hues(r.center(), 0);
        for (int k = 0; k <= 12; ++k) hues.setColorAt(k / 12.0, hueAt(k * 30.0, 1.0));
        p.setPen(QPen(QColor(20, 20, 20), 1));
        p.setBrush(hues);
        p.drawEllipse(r);
        QRadialGradient white(r.center(), r.width() / 2);
        white.setColorAt(0, QColor(255, 255, 255, 255));
        white.setColorAt(1, QColor(255, 255, 255, 0));
        p.setBrush(white);
        p.setPen(Qt::NoPen);
        p.drawEllipse(r);
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawEllipse(r);
        p.setPen(QPen(QColor(255, 255, 255, 80), 1));
        p.drawLine(QPointF(r.center().x(), r.top() + 4), QPointF(r.center().x(), r.bottom() - 4));
        p.drawLine(QPointF(r.left() + 4, r.center().y()), QPointF(r.right() - 4, r.center().y()));
        p.setPen(QPen(QColor(20, 20, 20), 2));
        p.setBrush(QColor(255, 255, 255));
        p.drawEllipse(dotView(), 4.5, 4.5);
    }
    void moveTo(const QPointF& pos)
    {
        const QRectF r = disc();
        double nx = (pos.x() - r.center().x()) / (r.width() / 2), ny = -(pos.y() - r.center().y()) / (r.height() / 2);
        const double d = std::hypot(nx, ny);
        if (d > 1) { nx /= d; ny /= d; }
        x = nx;
        y = ny;
        update();
        if (onMove) onMove(nx, ny);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        held_ = true;
        if (onBegin) onBegin();
        moveTo(e->position());
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (held_) moveTo(e->position());
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!held_) return;
        held_ = false;
        if (onEnd) onEnd();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        if (onReset) onReset();
    }

private:
    bool held_ = false;
};

class WheelEditor : public FxEditor {
public:
    WheelEditor(QWidget* parent, const std::string& base) : FxEditor(parent), base_(base)
    {
        setObjectName("fxWheelEditor");
        setProperty("wheel", QString::fromStdString(base));
        neutral_ = (base == "Gamma" || base == "Gain") ? 1.0 : 0.0;
        for (int k = 0; k < 4; ++k) {
            lo_[k] = neutral_ == 1.0 ? 0.0 : -1.0;
            hi_[k] = neutral_ == 1.0 ? 4.0 : 1.0;
        }
        reach_ = neutral_ == 1.0 ? 1.0 : 0.5;
        QHBoxLayout* h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);
        wheel_ = new WheelView(this);
        h->addWidget(wheel_, 0, Qt::AlignTop);
        QGridLayout* g = new QGridLayout();
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(3);
        h->addLayout(g, 1);
        const char* names[4] = {"R:", "G:", "B:", "Y:"};
        for (int k = 0; k < 4; ++k) {
            g->addWidget(new QLabel(names[k], this), k, 0);
            box_[k] = numberBox(this, k < 3 ? (k == 0 ? "fxWheelR" : k == 1 ? "fxWheelG" : "fxWheelB") : "fxWheelY", lo_[k], hi_[k], 3, 0.05, 60);
            g->addWidget(box_[k], k, 1);
        }
        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setObjectName("fxWheelMaster");
        slider_->setRange(0, 1000);
        g->addWidget(slider_, 3, 2);
        reset_ = new QPushButton("Reset", this);
        reset_->setObjectName("fxWheelReset");
        g->addWidget(reset_, 0, 2, Qt::AlignLeft);
        g->setColumnStretch(2, 1);

        wheel_->onBegin = [this] { if (begin) begin(); };
        wheel_->onEnd = [this] { if (end) end(); };
        wheel_->onMove = [this](double x, double y) { dotMoved(x, y); };
        wheel_->onReset = [this] { resetAll(); };
        for (int k = 0; k < 4; ++k)
            QObject::connect(box_[k], &QDoubleSpinBox::valueChanged, this, [this, k](double v) {
                if (updating_) return;
                sendOne(k, v);
            });
        QObject::connect(slider_, &QSlider::sliderPressed, this, [this] { if (begin) begin(); });
        QObject::connect(slider_, &QSlider::sliderReleased, this, [this] { if (end) end(); });
        QObject::connect(slider_, &QSlider::valueChanged, this, [this](int pos) {
            if (updating_) return;
            const double v = lo_[3] + (hi_[3] - lo_[3]) * pos / 1000.0;
            QSignalBlocker b(box_[3]);
            box_[3]->setValue(v);
            sendOne(3, v);
        });
        QObject::connect(reset_, &QPushButton::clicked, this, [this] { resetAll(); });
    }

    WheelView* wheel() const { return wheel_; }

    void showValues(const EdValues& values) override
    {
        if (wheel_->dragging()) return;
        updating_ = true;
        static const char* const sfx[4] = {"_R", "_G", "_B", "_Y"};
        for (int k = 0; k < 4; ++k) {
            const EdValue* e = edFind(values, base_ + sfx[k]);
            if (e) {
                if (std::isfinite(e->lo) && std::isfinite(e->hi) && e->hi > e->lo && e->lo > -1e8 && e->hi < 1e8) {
                    lo_[k] = e->lo;
                    hi_[k] = e->hi;
                    box_[k]->setRange(e->lo, e->hi);
                }
            }
            val_[k] = e && !e->v.empty() ? e->v[0] : (k == 3 ? neutral_ : neutral_);
            showNumber(box_[k], val_[k]);
            box_[k]->setEnabled(e == nullptr || e->enabled);
        }
        slider_->setValue(hi_[3] > lo_[3] ? (int)std::lround((val_[3] - lo_[3]) / (hi_[3] - lo_[3]) * 1000.0) : 0);
        placeDot();
        updating_ = false;
    }

private:
    static constexpr double kPhase[3] = {90.0, 210.0, 330.0};

    void placeDot()
    {
        double sx = 0, sy = 0, mean = 0;
        for (int c = 0; c < 3; ++c) mean += (val_[c] - neutral_) / 3.0;
        for (int c = 0; c < 3; ++c) {
            const double ph = kPhase[c] * kPi / 180.0;
            sx += (val_[c] - neutral_ - mean) * std::cos(ph);
            sy += (val_[c] - neutral_ - mean) * std::sin(ph);
        }
        sx *= 2.0 / 3.0 / reach_;
        sy *= 2.0 / 3.0 / reach_;
        const double r = std::hypot(sx, sy);
        if (r > 1) { sx /= r; sy /= r; }
        wheel_->x = sx;
        wheel_->y = sy;
        wheel_->update();
    }
    void dotMoved(double x, double y)
    {
        double mean = 0;
        for (int c = 0; c < 3; ++c) mean += (val_[c] - neutral_) / 3.0;
        const double r = std::hypot(x, y), a = std::atan2(y, x);
        for (int c = 0; c < 3; ++c) {
            double v = neutral_ + mean + (r < 1e-12 ? 0.0 : r * reach_ * std::cos(a - kPhase[c] * kPi / 180.0));
            v = std::min(hi_[c], std::max(lo_[c], v));
            val_[c] = v;
        }
        updating_ = true;
        for (int c = 0; c < 3; ++c) showNumber(box_[c], val_[c]);
        updating_ = false;
        static const char* const sfx[3] = {"_R", "_G", "_B"};
        for (int c = 0; c < 3; ++c)
            if (setNumbers) setNumbers(base_ + sfx[c], {val_[c]});
    }
    void sendOne(int k, double v)
    {
        static const char* const sfx[4] = {"_R", "_G", "_B", "_Y"};
        val_[k] = v;
        if (k < 3) placeDot();
        if (setNumbers) setNumbers(base_ + sfx[k], {v});
    }
    void resetAll()
    {
        if (begin) begin();
        static const char* const sfx[6] = {"_R", "_G", "_B", "_Y", "_U", "_V"};
        for (int k = 0; k < 6; ++k) {
            const double v = (k < 4) ? neutral_ : 0.0;
            if (k < 4) val_[k] = v;
            if (setNumbers) setNumbers(base_ + sfx[k], {v});
        }
        updating_ = true;
        for (int k = 0; k < 4; ++k) showNumber(box_[k], val_[k]);
        slider_->setValue(hi_[3] > lo_[3] ? (int)std::lround((val_[3] - lo_[3]) / (hi_[3] - lo_[3]) * 1000.0) : 0);
        updating_ = false;
        placeDot();
        if (end) end();
    }

    std::string base_;
    double neutral_ = 0, reach_ = 1;
    WheelView* wheel_ = nullptr;
    QDoubleSpinBox* box_[4] = {};
    QSlider* slider_ = nullptr;
    QPushButton* reset_ = nullptr;
    double val_[4] = {0, 0, 0, 0}, lo_[4] = {-1, -1, -1, -1}, hi_[4] = {1, 1, 1, 1};
    bool updating_ = false;
};

// ------------------------------------------------------------------------------------------ Color Corrector's wheels
// One wheel setting of Color Corrector (Low, Mid, High) or Color Corrector (Secondary) (Chrominance): the angle round
// the wheel in degrees
// (vectorscope: 0 towards blue at the right, 103 red at the upper left, 241 green) and the amount, distance from the middle (rim = 1).
class PolarWheelEditor : public FxEditor {
public:
    PolarWheelEditor(QWidget* parent, const std::string& name) : FxEditor(parent), name_(name)
    {
        setObjectName("fxPolarWheelEditor");
        QHBoxLayout* h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);
        wheel_ = new WheelView(this);
        wheel_->vectorscope = true;
        h->addWidget(wheel_, 0, Qt::AlignTop);
        QGridLayout* g = new QGridLayout();
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(3);
        h->addLayout(g, 1);
        g->addWidget(new QLabel("Angle:", this), 0, 0);
        g->addWidget(new QLabel("Amount:", this), 1, 0);
        angle_ = numberBox(this, "fxWheelAngle", -360, 360, 1, 1.0, 64);
        amount_ = numberBox(this, "fxWheelAmount", -360, 360, 3, 0.05, 64);
        g->addWidget(angle_, 0, 1);
        g->addWidget(amount_, 1, 1);
        QPushButton* reset = new QPushButton("Reset", this);
        reset->setObjectName("fxWheelReset");
        g->addWidget(reset, 0, 2, Qt::AlignLeft);
        g->setColumnStretch(3, 1);
        wheel_->onBegin = [this] { if (begin) begin(); };
        wheel_->onEnd = [this] { if (end) end(); };
        wheel_->onMove = [this](double x, double y) {
            double a = std::atan2(y, x) * 180.0 / kPi;
            if (a < 0) a += 360.0;
            angle_v_ = a;
            amount_v_ = std::hypot(x, y);
            updating_ = true;
            showNumber(angle_, a);
            showNumber(amount_, amount_v_);
            updating_ = false;
            if (setNumbers) setNumbers(name_, {angle_v_, amount_v_});
        };
        wheel_->onReset = [this] { resetWheel(); };
        QObject::connect(reset, &QPushButton::clicked, this, [this] { resetWheel(); });
        for (QDoubleSpinBox* b : {angle_, amount_})
            QObject::connect(b, &QDoubleSpinBox::valueChanged, this, [this](double) {
                if (updating_) return;
                angle_v_ = angle_->value();
                amount_v_ = amount_->value();
                place();
                if (setNumbers) setNumbers(name_, {angle_v_, amount_v_});
            });
    }

    WheelView* wheel() const { return wheel_; }

    void showValues(const EdValues& values) override
    {
        if (wheel_->dragging()) return;
        const EdValue* e = edFind(values, name_);
        if (!e || e->v.size() < 2) return;
        updating_ = true;
        angle_v_ = e->v[0];
        amount_v_ = e->v[1];
        showNumber(angle_, angle_v_);
        showNumber(amount_, amount_v_);
        updating_ = false;
        place();
    }

private:
    void place()
    {
        const double a = angle_v_ * kPi / 180.0, r = std::min(1.0, std::max(0.0, amount_v_));
        wheel_->x = r * std::cos(a);
        wheel_->y = r * std::sin(a);
        wheel_->update();
    }
    void resetWheel()
    {
        if (begin) begin();
        angle_v_ = amount_v_ = 0;
        updating_ = true;
        showNumber(angle_, 0);
        showNumber(amount_, 0);
        updating_ = false;
        place();
        if (setNumbers) setNumbers(name_, {0.0, 0.0});
        if (end) end();
    }

    std::string name_;
    WheelView* wheel_ = nullptr;
    QDoubleSpinBox *angle_ = nullptr, *amount_ = nullptr;
    double angle_v_ = 0, amount_v_ = 0;
    bool updating_ = false;
};

// ------------------------------------------------------------------------------------------ a LUT of Color Grading
class LutEditor : public FxEditor {
public:
    LutEditor(QWidget* parent, int slot) : FxEditor(parent), slot_(std::to_string(slot))
    {
        setObjectName("fxLutEditor");
        setProperty("slot", slot);
        QGridLayout* g = new QGridLayout(this);
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(3);
        enable_ = new QCheckBox("Use", this);
        enable_->setObjectName("fxLutEnable");
        file_ = new QLineEdit(this);
        file_->setObjectName("fxLutFile");
        file_->setPlaceholderText("LUT file (.cube)");
        QPushButton* browse = new QPushButton("Browse...", this);
        browse->setObjectName("fxLutBrowse");
        g->addWidget(enable_, 0, 0);
        g->addWidget(file_, 0, 1);
        g->addWidget(browse, 0, 2);
        QHBoxLayout* h = new QHBoxLayout();
        h->setSpacing(6);
        h->addWidget(new QLabel("Strength:", this));
        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setObjectName("fxLutStrength");
        slider_->setRange(0, 1000);
        h->addWidget(slider_, 1);
        box_ = numberBox(this, "fxLutStrengthBox", 0, 1, 2, 0.05);
        h->addWidget(box_);
        h->addSpacing(8);
        h->addWidget(new QLabel("Interpolation:", this));
        interp_ = new QComboBox(this);
        interp_->setObjectName("fxLutInterpolation");
        interp_->addItem("Linear");
        interp_->addItem("Tetrahedral");
        h->addWidget(interp_);
        g->addLayout(h, 1, 0, 1, 3);
        g->setColumnStretch(1, 1);

        QObject::connect(enable_, &QCheckBox::toggled, this, [this](bool on) {
            if (!updating_ && setNumbers) setNumbers("LUTEnable_" + slot_, {on ? 1.0 : 0.0});
        });
        QObject::connect(file_, &QLineEdit::editingFinished, this, [this] { fileTyped(); });
        QObject::connect(browse, &QPushButton::clicked, this, [this] {
            const QString got = fileChooser()(window(), "LUT", file_->text(), "LUT files (*.cube *.3dl *.lut);;All files (*)");
            if (got.isEmpty()) return;
            file_->setText(got);
            fileTyped();
        });
        QObject::connect(slider_, &QSlider::sliderPressed, this, [this] { if (begin) begin(); });
        QObject::connect(slider_, &QSlider::sliderReleased, this, [this] { if (end) end(); });
        QObject::connect(slider_, &QSlider::valueChanged, this, [this](int pos) {
            if (updating_) return;
            const double v = pos / 1000.0;
            QSignalBlocker b(box_);
            box_->setValue(v);
            if (setNumbers) setNumbers("LUTGain_" + slot_, {v});
        });
        QObject::connect(box_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (updating_) return;
            QSignalBlocker b(slider_);
            slider_->setValue((int)std::lround(v * 1000.0));
            if (setNumbers) setNumbers("LUTGain_" + slot_, {v});
        });
        QObject::connect(interp_, &QComboBox::activated, this, [this](int at) {
            if (!updating_ && setNumbers) setNumbers("LUTInterpolation_" + slot_, {(double)at});
        });
    }

    void showValues(const EdValues& values) override
    {
        updating_ = true;
        const bool on = numberOf(values, "LUTEnable_" + slot_, 0) != 0;
        if (enable_->isChecked() != on) {
            QSignalBlocker b(enable_);
            enable_->setChecked(on);
        }
        const EdValue* f = edFind(values, "LUTFilename_" + slot_);
        const QString t = QString::fromStdString(f ? f->text : std::string());
        if (!file_->hasFocus() && file_->text() != t) file_->setText(t);
        const double gain = numberOf(values, "LUTGain_" + slot_, 1);
        showNumber(box_, gain);
        if (!slider_->isSliderDown()) {
            QSignalBlocker b(slider_);
            slider_->setValue((int)std::lround(gain * 1000.0));
        }
        const int mode = (int)std::floor(numberOf(values, "LUTInterpolation_" + slot_, 0) + 0.5);
        if (interp_->currentIndex() != mode && mode >= 0 && mode < interp_->count()) {
            QSignalBlocker b(interp_);
            interp_->setCurrentIndex(mode);
        }
        updating_ = false;
    }

private:
    void fileTyped()
    {
        if (updating_) return;
        if (begin) begin();
        if (setText) setText("LUTFilename_" + slot_, file_->text().toStdString());
        if (!file_->text().isEmpty() && !enable_->isChecked() && setNumbers) setNumbers("LUTEnable_" + slot_, {1.0});
        if (end) end();
    }

    std::string slot_;
    QCheckBox* enable_ = nullptr;
    QLineEdit* file_ = nullptr;
    QSlider* slider_ = nullptr;
    QDoubleSpinBox* box_ = nullptr;
    QComboBox* interp_ = nullptr;
    bool updating_ = false;
};

// ------------------------------------------------------------------------------------------ Color Match
float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f; }
float labF(float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; }
float labFinv(float t) { const float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.f / 116.f) / 7.787f; }

void rgbToLab(float r, float g, float b, float lab[3])
{
    const float lr = srgbToLinear(r), lg = srgbToLinear(g), lb = srgbToLinear(b);
    const float X = (0.4124564f * lr + 0.3575761f * lg + 0.1804375f * lb) / 0.95047f;
    const float Y = 0.2126729f * lr + 0.7151522f * lg + 0.0721750f * lb;
    const float Z = (0.0193339f * lr + 0.1191920f * lg + 0.9503041f * lb) / 1.08883f;
    const float fx = labF(X), fy = labF(Y), fz = labF(Z);
    lab[0] = 116.f * fy - 16.f;
    lab[1] = 500.f * (fx - fy);
    lab[2] = 200.f * (fy - fz);
}

QColor labToColour(double L, double a, double b)
{
    L = std::min(100.0, std::max(0.0, L));
    const float fy = (float)((L + 16.0) / 116.0), fx = fy + (float)a / 500.f, fz = fy - (float)b / 200.f;
    const float X = labFinv(fx) * 0.95047f, Y = labFinv(fy), Z = labFinv(fz) * 1.08883f;
    const float lr = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
    const float lg = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
    const float lb = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;
    auto c = [](float v) { return std::min(1.f, std::max(0.f, linearToSrgb(std::min(1.f, std::max(0.f, v))))); };
    return QColor::fromRgbF(c(lr), c(lg), c(lb));
}

// What a picture stands for in Color Match: its mean colour and spread as swatches (only the statistics are kept in the
// effect, not the pictures).
class MatchSwatch : public QWidget {
public:
    explicit MatchSwatch(QWidget* parent) : QWidget(parent) { setFixedSize(112, 48); }
    void setStats(bool captured, const double* s)
    {
        captured_ = captured;
        if (captured)
            for (int k = 0; k < 6; ++k) s_[k] = s[k];
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(0, 0, -1, -1);
        if (!captured_) {
            p.fillRect(r, palette().color(QPalette::Base));
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(r, Qt::AlignCenter, "(no picture)");
        } else {
            const double d[3] = {-1.0, 0.0, 1.0};
            for (int k = 0; k < 3; ++k)
                p.fillRect(QRect(r.left() + k * (r.width() + 1) / 3, r.top(), (r.width() + 1) / 3 + 1, r.height()),
                           labToColour(s_[0] + d[k] * s_[1], s_[2] + d[k] * s_[3] * 0.5, s_[4] + d[k] * s_[5] * 0.5));
        }
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(r);
    }

private:
    bool captured_ = false;
    double s_[6] = {0, 0, 0, 0, 0, 0};
};

class ColorMatchEditor : public FxEditor {
public:
    explicit ColorMatchEditor(QWidget* parent) : FxEditor(parent)
    {
        setObjectName("fxColorMatch");
        QHBoxLayout* h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(16);
        for (int k = 0; k < 2; ++k) {
            QVBoxLayout* v = new QVBoxLayout();
            v->setSpacing(3);
            h->addLayout(v);
            v->addWidget(new QLabel(k == 0 ? "Source image" : "Target image", this));
            swatch_[k] = new MatchSwatch(this);
            swatch_[k]->setObjectName(k == 0 ? "fxMatchSource" : "fxMatchTarget");
            v->addWidget(swatch_[k]);
            QPushButton* load = new QPushButton("Load...", this);
            load->setObjectName(k == 0 ? "fxMatchSourceLoad" : "fxMatchTargetLoad");
            grab_[k] = new QPushButton("Grab from preview", this);
            grab_[k]->setObjectName(k == 0 ? "fxMatchSourceGrab" : "fxMatchTargetGrab");
            clear_[k] = new QPushButton("Clear", this);
            clear_[k]->setObjectName(k == 0 ? "fxMatchSourceClear" : "fxMatchTargetClear");
            v->addWidget(load);
            v->addWidget(grab_[k]);
            v->addWidget(clear_[k]);
            QObject::connect(load, &QPushButton::clicked, this, [this, k] {
                const QString got = fileChooser()(window(), k == 0 ? "Source image" : "Target image", QString(),
                                                  "Pictures (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.gif *.webp);;All files (*)");
                if (!got.isEmpty()) loadFile(k, got);
            });
            QObject::connect(grab_[k], &QPushButton::clicked, this, [this, k] {
                const QImage img = previewFrame ? previewFrame() : QImage();
                if (img.isNull()) {
                    if (tell) tell("There is no picture in the preview to take colours from.");
                    return;
                }
                capture(k, img);
            });
            QObject::connect(clear_[k], &QPushButton::clicked, this, [this, k] { clearSide(k); });
        }
        h->addStretch(1);
    }

    // Takes the colours of a picture file for the source (0) or target (1); false if it cannot be read.
    bool loadFile(int side, const QString& path)
    {
        QImage img(path);
        if (img.isNull()) {
            if (tell) tell("The picture could not be read: " + path.toStdString());
            return false;
        }
        return capture(side, img);
    }

    void showValues(const EdValues& values) override
    {
        const EdValue* d = edFind(values, "internaldata");
        data_ = d ? d->text : std::string();
        double st[12] = {0};
        const bool ok = readStats(st);
        const bool cap[2] = {numberOf(values, "SourceCaptured", 0) != 0, numberOf(values, "TargetCaptured", 0) != 0};
        for (int k = 0; k < 2; ++k) {
            swatch_[k]->setStats(cap[k] && ok, st + 6 * k);
            clear_[k]->setEnabled(cap[k]);
        }
    }

private:
    bool readStats(double st[12]) const
    {
        const std::string b = base64Decode(data_);
        if (b.size() < 12 * sizeof(double)) return false;
        for (int i = 0; i < 12; ++i) {
            std::memcpy(&st[i], b.data() + (size_t)i * 8, 8);
            if (!std::isfinite(st[i])) return false;
        }
        return true;
    }
    void write(int side, const double* six, bool captured)
    {
        std::string b = base64Decode(data_);
        if (b.size() < 12 * sizeof(double)) b.assign(12 * sizeof(double), '\0');
        std::memcpy(&b[(size_t)side * 6 * 8], six, 6 * sizeof(double));
        data_ = base64Encode(b.data(), b.size());
        if (begin) begin();
        if (setText) setText("internaldata", data_);
        if (setNumbers) setNumbers(side == 0 ? "SourceCaptured" : "TargetCaptured", {captured ? 1.0 : 0.0});
        if (end) end();
    }
    bool capture(int side, const QImage& img)
    {
        double s[6];
        if (!colorMatchStatistics(img, s)) {
            if (tell) tell("The picture has no colours to take.");
            return false;
        }
        write(side, s, true);
        return true;
    }
    void clearSide(int side)
    {
        const double zero[6] = {0, 0, 0, 0, 0, 0};
        write(side, zero, false);
    }

    MatchSwatch* swatch_[2] = {};
    QPushButton *grab_[2] = {}, *clear_[2] = {};
    std::string data_;
};

// ------------------------------------------------------------------------------------------ the list of editors
std::vector<EditorSpec> makeSpecs()
{
    std::vector<EditorSpec> v;
    {
        EditorSpec s;
        s.plugin = "com.vegascreativesoftware:colorcurves";
        s.anchor = "Property";
        s.kind = "colorcurves";
        s.label = "Curves";
        s.uses = {"Property", "SelectedChannel", "ShowAllChannels"};
        s.keyed = {"Property"};
        v.push_back(s);
    }
    {
        EditorSpec s;
        s.plugin = "com.vegascreativesoftware:colorgrading";
        s.anchor = "Curve_R";
        s.kind = "gradingcurves";
        s.label = "Curves";
        s.uses = {"Curve_R", "Curve_G", "Curve_B", "Curve_Y"};
        s.keyed = s.uses;
        v.push_back(s);
        const char* groups[4] = {"Lift", "Gamma", "Gain", "Offset"};
        for (const char* g : groups) {
            EditorSpec w;
            w.plugin = s.plugin;
            w.anchor = std::string(g) + "_R";
            w.kind = "wheel";
            w.label = g;
            for (const char* c : {"R", "G", "B", "Y", "U", "V"}) w.uses.push_back(std::string(g) + "_" + c);
            w.keyed = w.uses;
            v.push_back(w);
        }
        for (int i = 0; i < 2; ++i) {
            const std::string sx = "_" + std::to_string(i);
            EditorSpec l;
            l.plugin = s.plugin;
            l.anchor = "LUTEnable" + sx;
            l.kind = "lutslot";
            l.label = i == 0 ? "LUT 1" : "LUT 2";
            l.uses = {"LUTEnable" + sx, "LUTInterpolation" + sx, "LUTGain" + sx, "LUTFilename" + sx};
            l.keyed = {"LUTGain" + sx};
            v.push_back(l);
        }
    }
    {
        const struct { const char* plugin; const char* name; } wheels[] = {{"com.vegascreativesoftware:colorcorrector", "Low"},
                                                                          {"com.vegascreativesoftware:colorcorrector", "Mid"},
                                                                          {"com.vegascreativesoftware:colorcorrector", "High"},
                                                                          {"com.vegascreativesoftware:colorcorrectorsecondary", "Chrominance"}};
        for (const auto& w : wheels) {
            EditorSpec s;
            s.plugin = w.plugin;
            s.anchor = w.name;
            s.kind = "polarwheel";
            s.uses = {w.name};
            s.keyed = s.uses;
            v.push_back(s);
        }
    }
    {
        EditorSpec s;
        s.plugin = "com.vegascreativesoftware:colormatch";
        s.anchor = "ImageSource";
        s.kind = "colormatch";
        s.label = "Images";
        s.uses = {"ImageSource", "ImageTarget", "ImageResult", "SourceCaptured", "TargetCaptured", "internaldata"};
        s.keyed = {};
        v.push_back(s);
    }
    return v;
}

const std::vector<EditorSpec>& specs()
{
    static const std::vector<EditorSpec> s = makeSpecs();
    return s;
}

}

const EditorSpec* findEditorSpec(const std::string& plugin, const std::string& param)
{
    for (const EditorSpec& s : specs())
        if (s.anchor == param && s.plugin == plugin) return &s;
    return nullptr;
}

std::vector<const EditorSpec*> editorSpecsFor(const std::string& plugin)
{
    std::vector<const EditorSpec*> v;
    for (const EditorSpec& s : specs())
        if (s.plugin == plugin) v.push_back(&s);
    return v;
}

bool isEditorMember(const std::string& plugin, const std::string& param)
{
    for (const EditorSpec& s : specs()) {
        if (s.plugin != plugin || s.anchor == param) continue;
        for (const std::string& u : s.uses)
            if (u == param) return true;
    }
    return false;
}

FxEditor* makeEditor(const EditorSpec& s, QWidget* parent)
{
    if (s.kind == "colorcurves") return new CurveEditor(parent, false);
    if (s.kind == "gradingcurves") return new CurveEditor(parent, true);
    if (s.kind == "wheel") return new WheelEditor(parent, s.label);
    if (s.kind == "polarwheel") return new PolarWheelEditor(parent, s.anchor);
    if (s.kind == "lutslot") return new LutEditor(parent, s.anchor.empty() ? 0 : (s.anchor.back() == '1' ? 1 : 0));
    if (s.kind == "colormatch") return new ColorMatchEditor(parent);
    return nullptr;
}

FileChooser& fileChooser()
{
    static FileChooser c = [](QWidget* parent, const QString& title, const QString& startAt, const QString& filter) {
        return QFileDialog::getOpenFileName(parent, title, startAt, filter);
    };
    return c;
}

const char* filePickFilter(const std::string& plugin, const std::string& param)
{
    if (plugin == "com.vegascreativesoftware:lutfilter" && param == "LUTFilename") return "LUT files (*.cube *.3dl *.lut);;All files (*)";
    return nullptr;
}

bool colorMatchStatistics(const QImage& image, double out[6])
{
    if (image.isNull()) return false;
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    const int W = img.width(), H = img.height();
    if (W < 1 || H < 1) return false;
    const int gx = 160, gy = 90;
    double sum[3] = {0, 0, 0}, sq[3] = {0, 0, 0};
    long cnt = 0;
    for (int j = 0; j < gy; ++j)
        for (int i = 0; i < gx; ++i) {
            const int x = std::min(W - 1, (int)((i + 0.5) * W / gx)), y = std::min(H - 1, (int)((j + 0.5) * H / gy));
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) == 0) continue;
            float lab[3];
            rgbToLab(qRed(c) / 255.f, qGreen(c) / 255.f, qBlue(c) / 255.f, lab);
            for (int k = 0; k < 3; ++k) { sum[k] += lab[k]; sq[k] += (double)lab[k] * lab[k]; }
            ++cnt;
        }
    if (cnt < 4) return false;
    for (int k = 0; k < 3; ++k) {
        out[2 * k] = sum[k] / cnt;
        out[2 * k + 1] = std::sqrt(std::max(0.0, sq[k] / cnt - out[2 * k] * out[2 * k]));
    }
    return true;
}

}
}
