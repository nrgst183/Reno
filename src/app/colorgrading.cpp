// The Color Grading panel: see colorgrading.h.
#include "colorgrading.h"
#include "toolwin.h"
#include "../engine/builtinfx.h"
#include "../engine/catalog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QMenu>
#include <QButtonGroup>
#include <QImage>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace reno {
namespace app {

using namespace tw;

namespace {


// The colour a wheel point stands for: red at the top, green lower left, blue lower right (as the engine's wheels),
// fading to white in the middle.
QColor wheelColour(double angleDeg, double radius)
{
    double hue = std::fmod((angleDeg - 90.0) / 360.0 + 2.0, 1.0);
    return QColor::fromHsvF(hue, std::min(1.0, radius), 1.0);
}

// The wheel's point and master as the R, G, B amounts VEGAS shows beside it: master plus the point's push on each
// channel (engine wheelPush), and back.
void wheelToRgb(double x, double y, double master, double out[3])
{
    const double r = std::hypot(x, y), a = std::atan2(y, x);
    const double phase[3] = {90.0, 210.0, 330.0};
    for (int c = 0; c < 3; ++c) out[c] = master + (r < 1e-12 ? 0.0 : r * std::cos(a - phase[c] * 3.14159265358979323846 / 180.0));
}
void rgbToWheel(const double in[3], double* x, double* y, double* master)
{
    const double m = (in[0] + in[1] + in[2]) / 3.0;
    const double phase[3] = {90.0, 210.0, 330.0};
    double sx = 0, sy = 0;
    for (int c = 0; c < 3; ++c) {
        const double ph = phase[c] * 3.14159265358979323846 / 180.0;
        sx += (in[c] - m) * std::cos(ph);
        sy += (in[c] - m) * std::sin(ph);
    }
    sx *= 2.0 / 3.0;
    sy *= 2.0 / 3.0;
    const double r = std::hypot(sx, sy);
    if (r > 1) { sx /= r; sy /= r; }
    *x = sx;
    *y = sy;
    *master = m;
}

// A colour wheel as VEGAS 18 lays it out: name and reset arrow above, the wheel with the R, G, B boxes beside it, the Y
// (master) slider under it.
class GradeWheel : public QWidget {
public:
    GradeWheel(const QString& title, double lo, double hi, double neutral, QWidget* parent)
        : QWidget(parent), title_(title), lo_(lo), hi_(hi), neutral_(neutral)
    {
        setMinimumSize(150, 150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);
        v->addStretch(1);
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        row->addWidget(new QLabel("Y:", this));
        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setObjectName("wheelMaster");
        slider_->setRange(0, 1000);
        ybox_ = new QDoubleSpinBox(this);
        ybox_->setObjectName("wheelY");
        ybox_->setRange(lo, hi);
        ybox_->setDecimals(2);
        ybox_->setSingleStep(0.05);
        ybox_->setButtonSymbols(QAbstractSpinBox::NoButtons);
        ybox_->setKeyboardTracking(false);
        ybox_->setFixedWidth(46);
        row->addWidget(slider_, 1);
        row->addWidget(ybox_);
        v->addLayout(row);
        reset_ = new QToolButton(this);
        reset_->setObjectName("wheelReset");
        reset_->setText(QString::fromUtf8("\xe2\x86\xba"));
        reset_->setToolTip("Reset this wheel");
        reset_->setAutoRaise(true);
        reset_->setFixedSize(20, 18);
        const char* names[3] = {"R:", "G:", "B:"};
        for (int c = 0; c < 3; ++c) {
            lbl_[c] = new QLabel(names[c], this);
            box_[c] = new QDoubleSpinBox(this);
            box_[c]->setObjectName(QString("wheel%1").arg(QChar("RGB"[c])));
            box_[c]->setRange(lo - 1.0, hi + 1.0);
            box_[c]->setDecimals(3);
            box_[c]->setSingleStep(0.05);
            box_[c]->setButtonSymbols(QAbstractSpinBox::NoButtons);
            box_[c]->setKeyboardTracking(false);
            box_[c]->setFixedSize(46, 20);
            QObject::connect(box_[c], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, c](double val) {
                if (updating_) return;
                setRgb(c, val);
            });
        }
        QObject::connect(reset_, &QToolButton::clicked, this, [this] { resetToNeutral(); });
        QObject::connect(slider_, &QSlider::sliderPressed, this, [this] { if (onBegin) onBegin(); });
        QObject::connect(slider_, &QSlider::valueChanged, this, [this](int pos) {
            if (updating_) return;
            master = lo_ + (hi_ - lo_) * pos / 1000.0;
            sync(true);
            if (onChange) onChange(!slider_->isSliderDown());
        });
        QObject::connect(slider_, &QSlider::sliderReleased, this, [this] { if (onEnd) onEnd(); });
        QObject::connect(ybox_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double val) {
            if (updating_) return;
            master = val;
            sync(true);
            if (onChange) onChange(true);
        });
        sync(true);
    }
    double x = 0, y = 0, master = 0;
    std::function<void()> onBegin, onEnd;
    std::function<void(bool once)> onChange;

    void set(double nx, double ny, double nm)
    {
        x = nx;
        y = ny;
        master = nm;
        sync(true);
    }
    double rgb(int c) const
    {
        double v[3];
        wheelToRgb(x, y, master, v);
        return v[c];
    }
    void setRgb(int c, double value, bool once = true)
    {
        double v[3];
        wheelToRgb(x, y, master, v);
        v[c] = value;
        rgbToWheel(v, &x, &y, &master);
        master = std::min(hi_, std::max(lo_, master));
        sync(true);
        if (onChange) onChange(once);
    }
    double lo() const { return lo_; }
    double hi() const { return hi_; }
    void resetToNeutral()
    {
        x = y = 0;
        master = neutral_;
        sync(true);
        if (onChange) onChange(true);
    }

protected:
    QRectF wheelRect() const
    {
        const double h = height() - 34, w = width() - 62;
        const double s = std::max(20.0, std::min(w - 8, h - 16));
        return QRectF((w - s) / 2 + 2, 20, s, s);
    }
    void resizeEvent(QResizeEvent*) override
    {
        const QRectF r = wheelRect();
        const int bx = (int)std::lround(r.right()) + 8;
        reset_->move((int)std::lround(r.right()) - 18, 0);
        setProperty("wheelCx", r.center().x());
        setProperty("wheelCy", r.center().y());
        setProperty("wheelR", r.width() / 2);
        const int top = (int)std::lround(r.center().y()) - 34;
        for (int c = 0; c < 3; ++c) {
            lbl_[c]->move(bx, top + c * 24 + 2);
            box_[c]->move(bx + 14, top + c * 24);
        }
    }
    void showEvent(QShowEvent*) override { resizeEvent(nullptr); }
    void sync(bool boxes)
    {
        updating_ = true;
        slider_->setValue((int)std::lround((master - lo_) / (hi_ - lo_) * 1000.0));
        if (boxes) {
            ybox_->setValue(master);
            double v[3];
            wheelToRgb(x, y, master, v);
            for (int c = 0; c < 3; ++c) box_[c]->setValue(v[c]);
        } else {
            ybox_->setValue(master);
        }
        updating_ = false;
        update();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QColor(200, 200, 200));
        p.drawText(QRect(4, 0, std::max(10, width() - 90), 16), Qt::AlignLeft | Qt::AlignTop, title_);
        const QRectF r = wheelRect();
        QConicalGradient hues(r.center(), 0);
        for (int k = 0; k <= 12; ++k) hues.setColorAt(k / 12.0, wheelColour(k * 30.0, 1.0));
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
        const QPointF at(r.center().x() + x * r.width() / 2, r.center().y() - y * r.height() / 2);
        p.setPen(QPen(QColor(20, 20, 20), 2));
        p.setBrush(QColor(255, 255, 255));
        p.drawEllipse(at, 4.5, 4.5);
    }
    void move(const QPointF& pos)
    {
        const QRectF r = wheelRect();
        double nx = (pos.x() - r.center().x()) / (r.width() / 2), ny = -(pos.y() - r.center().y()) / (r.height() / 2);
        const double d = std::hypot(nx, ny);
        if (d > 1) { nx /= d; ny /= d; }
        x = nx;
        y = ny;
        sync(true);
        if (onChange) onChange(false);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const QRectF r = wheelRect();
        if (e->button() != Qt::LeftButton || QLineF(e->position(), r.center()).length() > r.width() / 2 + 6) return QWidget::mousePressEvent(e);
        held_ = true;
        if (onBegin) onBegin();
        move(e->position());
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (held_) move(e->position());
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!held_) return;
        held_ = false;
        if (onEnd) onEnd();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { resetToNeutral(); }

private:
    QString title_;
    double lo_, hi_, neutral_;
    QSlider* slider_ = nullptr;
    QDoubleSpinBox* ybox_ = nullptr, *box_[3] = {};
    QLabel* lbl_[3] = {};
    QToolButton* reset_ = nullptr;
    bool updating_ = false, held_ = false;
};

// The curves: one channel's curve to edit, the others drawn faintly.
class GradeCurves : public QWidget {
public:
    explicit GradeCurves(QWidget* parent) : QWidget(parent)
    {
        setMinimumSize(180, 180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    std::vector<std::pair<double, double>> pts[6];
    int channel = 0;
    bool hsl = false;
    std::function<void()> onBegin, onEnd;
    std::function<void(bool once)> onChange;

    std::vector<std::pair<double, double>> shown(int k) const
    {
        if (pts[k].size() >= 2) return pts[k];
        if (hsl) return {{0.0, 0.5}, {1.0, 0.5}};
        return {{0.0, 0.0}, {1.0, 1.0}};
    }
    QRectF area() const { return QRectF(rect()).adjusted(6, 6, -6, -6); }
    QPointF toView(double x, double y) const
    {
        const QRectF a = area();
        return QPointF(a.left() + x * a.width(), a.bottom() - y * a.height());
    }
    std::pair<double, double> fromView(const QPointF& p) const
    {
        const QRectF a = area();
        return {std::min(1.0, std::max(0.0, (p.x() - a.left()) / a.width())), std::min(1.0, std::max(0.0, (a.bottom() - p.y()) / a.height()))};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), gap());
        const QRectF a = area();
        p.setPen(QPen(QColor(70, 70, 70), 1));
        for (int k = 0; k <= 4; ++k) {
            p.drawLine(QPointF(a.left() + a.width() * k / 4, a.top()), QPointF(a.left() + a.width() * k / 4, a.bottom()));
            p.drawLine(QPointF(a.left(), a.top() + a.height() * k / 4), QPointF(a.right(), a.top() + a.height() * k / 4));
        }
        const QColor cols[6] = {QColor(230, 230, 230), QColor(235, 80, 80), QColor(90, 210, 90), QColor(90, 140, 240), QColor(230, 200, 80), QColor(200, 120, 230)};
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k < (hsl ? 6 : 4); ++k) {
                if (hsl && k != channel) continue;
                if ((pass == 0) == (k == channel)) continue;
                const std::vector<std::pair<double, double>> c = shown(k);
                QPainterPath path;
                for (int i = 0; i <= 128; ++i) {
                    const double x = i / 128.0;
                    const QPointF v = toView(x, std::min(1.0, std::max(0.0, curveThrough(c, x))));
                    if (i == 0) path.moveTo(v);
                    else path.lineTo(v);
                }
                QColor col = cols[k];
                if (k != channel) col.setAlpha(90);
                p.setPen(QPen(col, k == channel ? 2.0 : 1.0));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
                if (k == channel) {
                    p.setBrush(col);
                    p.setPen(QPen(QColor(20, 20, 20), 1));
                    for (const auto& pt : c) p.drawEllipse(toView(pt.first, pt.second), 4, 4);
                }
            }
    }
    int pointNear(const QPointF& pos) const
    {
        const std::vector<std::pair<double, double>> c = shown(channel);
        for (size_t i = 0; i < c.size(); ++i)
            if (QLineF(pos, toView(c[i].first, c[i].second)).length() < 8) return (int)i;
        return -1;
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if (pts[channel].size() < 2) pts[channel] = shown(channel);
        held_ = pointNear(e->position());
        if (held_ < 0) {
            const auto at = fromView(e->position());
            pts[channel].push_back(at);
            std::sort(pts[channel].begin(), pts[channel].end());
            for (size_t i = 0; i < pts[channel].size(); ++i)
                if (pts[channel][i] == at) held_ = (int)i;
        }
        if (onBegin) onBegin();
        update();
        if (onChange) onChange(false);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (held_ < 0 || held_ >= (int)pts[channel].size()) return;
        auto at = fromView(e->position());
        auto& c = pts[channel];
        const double lo = held_ > 0 ? c[(size_t)held_ - 1].first + 0.01 : 0.0, hi = held_ + 1 < (int)c.size() ? c[(size_t)held_ + 1].first - 0.01 : 1.0;
        if (held_ == 0) at.first = 0.0;
        else if (held_ + 1 == (int)c.size()) at.first = 1.0;
        else at.first = std::min(hi, std::max(lo, at.first));
        c[(size_t)held_] = at;
        update();
        if (onChange) onChange(false);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (held_ < 0) return;
        held_ = -1;
        if (onEnd) onEnd();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        const int i = pointNear(e->position());
        auto& c = pts[channel];
        if (i <= 0 || i + 1 >= (int)c.size()) return;
        c.erase(c.begin() + i);
        held_ = -1;
        update();
        if (onChange) onChange(true);
    }

private:
    int held_ = -1;
};

const char* const kSliderNames[5] = {"exposure", "contrast", "saturation", "temperature", "tint"};

}

struct ColorGradingPanel::Impl {
    ColorGradingPanel* q = nullptr;
    QComboBox* applyBox = nullptr;
    QLabel* targetLbl = nullptr;
    QPushButton* bypass = nullptr, *pinBtn = nullptr;
    GradeWheel* wheels[4] = {};
    GradeCurves* curves = nullptr;
    QCheckBox* curveCheck[4] = {};
    Apply apply = SelectedEvent;
    FxTarget pinned;
    bool pinLock = false;
    FxTarget pinTarget;
    QSlider* chSlider[4][3] = {};
    QDoubleSpinBox* chBox[4][3] = {};
    ColorGrade g;
    bool dragging = false, updating = false;
    QTabWidget* sec1 = nullptr, *sec2 = nullptr;
    QLineEdit* lutFile[2] = {};
    QComboBox* lutInterp[2] = {};
    QSlider* lutSlider[2] = {};
    std::vector<std::function<void()>> refreshMore;
    GradeCurves* hslCurves = nullptr;
    QComboBox* lutBox[2] = {};
    QDoubleSpinBox* lutStrength[2] = {};
    QCheckBox* lutOn[2] = {};
    QPushButton* pickBtn[3] = {};
    QLabel* pickInfo = nullptr;
    int picking = 0;                                    // 0 none, 1 black point, 2 white point, 3 neutral tone
    std::unique_ptr<Listener> listener;

    FxTarget currentTarget() const
    {
        AppState& app = AppState::get();
        if (pinLock && pinTarget.kind != FxTarget::None && app.chainOf(app.project(), pinTarget)) return pinTarget;
        if (pinned.kind != FxTarget::None && app.chainOf(app.project(), pinned)) return pinned;
        FxTarget t;
        if (apply == VideoOutput) {
            t.kind = FxTarget::VideoOut;
            return t;
        }
        if (apply == SelectedTrack) {
            FxTarget c;
            c.kind = FxTarget::Track;
            c.id = app.currentTrack;
            return app.targetIsVideo(c) ? c : t;
        }
        for (uint64_t id : app.selection()) {
            FxTarget c;
            c.kind = FxTarget::Event;
            c.id = id;
            if (app.targetIsVideo(c)) return c;
        }
        return t;
    }

    static const FxSlot* gradeSlot(const FxChain* c)
    {
        if (!c) return nullptr;
        const FxSlot* found = nullptr;
        for (const FxSlot& s : *c)
            if (isBuiltinVideo(s) && s.pluginId == kColorGradeId) found = &s;
        return found;
    }

    void load()
    {
        if (dragging) return;
        AppState& app = AppState::get();
        const FxTarget t = currentTarget();
        const FxSlot* s = gradeSlot(app.chainOf(app.project(), t));
        g = s ? ColorGrade::fromJson(s->state) : ColorGrade();
        updating = true;
        if (pinLock && !(pinTarget.kind != FxTarget::None && app.chainOf(app.project(), pinTarget))) {
            pinLock = false;
            pinBtn->setChecked(false);
        }
        targetLbl->setText(t.kind == FxTarget::None ? QString("(select a video event)") : qs(app.targetName(t)) + (s ? QString() : QString("  (not graded yet)")) + (pinLock ? QString("  (pinned)") : QString()));
        bypass->setChecked(s && s->bypass);
        bypass->setEnabled(s != nullptr);
        const ColorGrade::Wheel* w[4] = {&g.lift, &g.gamma, &g.gain, &g.offset};
        for (int k = 0; k < 4; ++k) wheels[k]->set(w[k]->x, w[k]->y, w[k]->master);
        syncChannels();
        for (int k = 0; k < 4; ++k) curves->pts[k] = g.curve[k];
        curves->update();
        for (auto& f : refreshMore) f();
        q->setEnabled(true);
        for (QWidget* wd : {(QWidget*)wheels[0], (QWidget*)wheels[1], (QWidget*)wheels[2], (QWidget*)wheels[3], (QWidget*)curves})
            wd->setEnabled(t.kind != FxTarget::None);
        updating = false;
    }

    void write(Project& p, const FxTarget& t, const Json& state) const
    {
        FxChain* c = AppState::get().chainOf(p, t);
        if (!c) return;
        FxSlot* s = nullptr;
        for (FxSlot& x : *c)
            if (isBuiltinVideo(x)) s = &x;
        if (!s) {
            c->push_back(makeBuiltinSlot(kColorGradeId, p.newId()));
            s = &c->back();
        }
        s->state = state;
    }

    void begin()
    {
        if (dragging || currentTarget().kind == FxTarget::None) return;
        AppState::get().beginDrag();
        dragging = true;
    }
    void change(bool once)
    {
        const FxTarget t = currentTarget();
        if (t.kind == FxTarget::None || updating) return;
        const Json state = g.toJson();
        AppState& app = AppState::get();
        if (dragging) app.dragUpdate([&](Project& p) { write(p, t, state); });
        else if (once) app.edit("Color Grading", [&](Project& p) { write(p, t, state); });
    }
    void end()
    {
        if (!dragging) return;
        dragging = false;
        AppState::get().endDrag("Color Grading");
        load();
    }

    void syncChannels()
    {
        const bool was = updating;
        updating = true;
        for (int k = 0; k < 4; ++k)
            for (int c = 0; c < 3; ++c) {
                if (!chSlider[k][c]) continue;
                const double lo = wheels[k]->lo() - 1.0, hi = wheels[k]->hi() + 1.0, v = wheels[k]->rgb(c);
                chBox[k][c]->setValue(v);
                chSlider[k][c]->setValue((int)std::lround((v - lo) / (hi - lo) * 1000.0));
            }
        updating = was;
    }
    void fromWheels()
    {
        ColorGrade::Wheel* w[4] = {&g.lift, &g.gamma, &g.gain, &g.offset};
        for (int k = 0; k < 4; ++k) {
            w[k]->x = wheels[k]->x;
            w[k]->y = wheels[k]->y;
            w[k]->master = wheels[k]->master;
        }
    }
    void setNamed(int k, double v)
    {
        double* f[5] = {&g.exposure, &g.contrast, &g.saturation, &g.temperature, &g.tint};
        *f[k] = v;
    }
};

// The panel's pages as in VEGAS 18: section 1 (Input LUT | Color Wheels | Color Channels | Input/Output, then Reno's RL
// Color Wheels and Utilities), section 2 (Color Curves | HSL | Look LUT) and the Finishing column.
void ColorGradingPanel::buildPages(QHBoxLayout* root)
{
    Impl* impl = d.get();
    // A labelled slider with a box on one field of the grade (vertical as in VEGAS's Levels / Color Channels pages, or a
    // grid row). Double-clicking the label or slider resets the field to 'neutral'.
    auto makeField = [this, impl](const QString& label, const char* name, double* field, double lo, double hi, double neutral, int decimals, bool vertical) {
        QLabel* l = new QLabel(label, this);
        QSlider* sl = new QSlider(vertical ? Qt::Vertical : Qt::Horizontal, this);
        sl->setRange(0, 1000);
        QDoubleSpinBox* box = new QDoubleSpinBox(this);
        box->setObjectName(QString("gradeMoreBox_%1").arg(name));
        sl->setObjectName(QString("gradeMore_%1").arg(name));
        box->setRange(lo, hi);
        box->setDecimals(decimals);
        box->setSingleStep(decimals ? 0.05 : 1);
        box->setKeyboardTracking(false);
        if (vertical) box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        auto toPos = [lo, hi](double v) { return (int)std::lround((v - lo) / (hi - lo) * 1000.0); };
        QObject::connect(sl, &QSlider::sliderPressed, this, [impl] { impl->begin(); });
        QObject::connect(sl, &QSlider::valueChanged, this, [impl, sl, box, field, lo, hi](int pos) {
            if (impl->updating) return;
            const double v = lo + (hi - lo) * pos / 1000.0;
            impl->updating = true;
            box->setValue(v);
            impl->updating = false;
            *field = v;
            impl->change(!sl->isSliderDown());
        });
        QObject::connect(sl, &QSlider::sliderReleased, this, [impl] { impl->end(); });
        QObject::connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [impl, field](double v) {
            if (impl->updating) return;
            *field = v;
            impl->change(true);
        });
        d->refreshMore.push_back([sl, box, field, toPos] {
            box->setValue(*field);
            sl->setValue(toPos(*field));
        });
        for (QWidget* w : {(QWidget*)l, (QWidget*)sl}) {
            w->installEventFilter(this);
            w->setProperty("gradeResetBox", QVariant::fromValue<QObject*>(box));
            w->setProperty("gradeNeutral", neutral);
        }
        l->setToolTip("Double-click to reset");
        struct R { QLabel* l; QSlider* s; QDoubleSpinBox* b; };
        return R{l, sl, box};
    };
    auto vcolumn = [this](QLabel* l, QSlider* s, QDoubleSpinBox* b) {
        QWidget* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(2, 0, 2, 0);
        v->setSpacing(2);
        v->addWidget(s, 1, Qt::AlignHCenter);
        b->setFixedWidth(48);
        v->addWidget(b, 0, Qt::AlignHCenter);
        l->setAlignment(Qt::AlignHCenter);
        v->addWidget(l);
        return w;
    };
    auto hrow = [](QGridLayout* grid, int row, QLabel* l, QSlider* s, QDoubleSpinBox* b) {
        grid->addWidget(l, row, 0);
        grid->addWidget(s, row, 1);
        grid->addWidget(b, row, 2);
    };
    auto heading = [this](const QString& text) {
        QLabel* h = new QLabel(text, this);
        h->setAlignment(Qt::AlignHCenter);
        return h;
    };
    auto scrolled = [this](QWidget* page) {
        QScrollArea* sa = new QScrollArea(this);
        sa->setWidgetResizable(true);
        sa->setWidget(page);
        sa->setFrameShape(QFrame::NoFrame);
        return sa;
    };

    d->sec1 = new QTabWidget(this);
    d->sec1->setObjectName("gradeSection1");
    d->sec2 = new QTabWidget(this);
    d->sec2->setObjectName("gradeSection2");

    // --- a LUT page: "Camera LUT" (before the grade; Input LUT tab) or "Look LUT" (after it; section 2's last tab)
    auto lutPage = [this, impl, heading](bool camera) {
        const int w = camera ? 0 : 1;
        QWidget* page = new QWidget(this);
        auto* grid = new QGridLayout(page);
        grid->setHorizontalSpacing(8);
        grid->addWidget(heading(camera ? "Camera LUT" : "Look LUT"), 0, 0, 1, 4);
        QLineEdit* file = d->lutFile[w] = new QLineEdit(page);
        file->setObjectName(camera ? "gradeCameraLutFile" : "gradeLookLutFile");
        file->setReadOnly(true);
        QPushButton* browse = new QPushButton("Browse", page);
        browse->setObjectName(camera ? "gradeCameraLutBrowse" : "gradeLookLutBrowse");
        QComboBox* cb = d->lutBox[w] = new QComboBox(page);
        cb->setObjectName(camera ? "gradeCameraLut" : "gradeLookLut");
        cb->addItem("None", "");
        for (const LutPreset& lp : lutPresets())
            if (lp.camera == camera) cb->addItem(QString::fromStdString(lp.label), QString::fromStdString("preset:" + lp.id));
        QComboBox* interp = d->lutInterp[w] = new QComboBox(page);
        interp->setObjectName(camera ? "gradeCameraLutInterp" : "gradeLookLutInterp");
        interp->addItems({"Linear", "Tetrahedral"});
        QPushButton* rs = new QPushButton("Reset", page);
        rs->setObjectName(camera ? "gradeCameraLutReset" : "gradeLookLutReset");
        QSlider* st = d->lutSlider[w] = new QSlider(Qt::Horizontal, page);
        st->setRange(0, 100);
        st->setObjectName(camera ? "gradeCameraLutStrengthSlider" : "gradeLookLutStrengthSlider");
        QDoubleSpinBox* sb = d->lutStrength[w] = new QDoubleSpinBox(page);
        sb->setObjectName(camera ? "gradeCameraLutStrength" : "gradeLookLutStrength");
        sb->setRange(0, 1);
        sb->setDecimals(2);
        sb->setSingleStep(0.05);
        sb->setValue(1);
        sb->setKeyboardTracking(false);
        QCheckBox* on = d->lutOn[w] = new QCheckBox("Enable", page);
        on->setObjectName(camera ? "gradeCameraLutOn" : "gradeLookLutOn");
        on->setChecked(true);
        grid->addWidget(new QLabel("File:", page), 1, 0, Qt::AlignRight);
        grid->addWidget(file, 1, 1, 1, 2);
        grid->addWidget(browse, 1, 3);
        grid->addWidget(new QLabel("Preset:", page), 2, 0, Qt::AlignRight);
        grid->addWidget(cb, 2, 1, 1, 2);
        grid->addWidget(new QLabel("Interpolation:", page), 3, 0, Qt::AlignRight);
        grid->addWidget(interp, 3, 1);
        grid->addWidget(rs, 3, 3);
        grid->addWidget(new QLabel("Strength:", page), 4, 0, Qt::AlignRight);
        auto* sr = new QHBoxLayout();
        sr->addWidget(new QLabel("0", page));
        sr->addWidget(st, 1);
        sr->addWidget(new QLabel("1", page));
        sr->addWidget(sb);
        grid->addLayout(sr, 4, 1, 1, 2);
        grid->addWidget(on, 5, 3);
        grid->setColumnStretch(1, 1);
        grid->setRowStretch(6, 1);
        auto ref = [impl, camera]() -> LutRef& { return camera ? impl->g.cameraLut : impl->g.lookLut; };
        QObject::connect(browse, &QPushButton::clicked, this, [impl, this, ref] {
            const QString f = QFileDialog::getOpenFileName(this, "LUT", QSettings().value("colorGrading/lutFolder").toString(), "LUT (*.cube)");
            if (f.isEmpty()) return;
            QSettings().setValue("colorGrading/lutFolder", QFileInfo(f).absolutePath());
            ref().source = "file:" + f.toStdString();
            ref().enabled = true;
            impl->change(true);
            impl->load();
        });
        QObject::connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [impl, cb, ref](int i) {
            if (impl->updating || i < 0) return;
            ref().source = cb->itemData(i).toString().toStdString();
            ref().enabled = true;
            impl->change(true);
            impl->load();
        });
        QObject::connect(interp, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [impl, ref](int i) {
            if (impl->updating) return;
            ref().tetrahedral = i == 1;
            impl->change(true);
        });
        QObject::connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [impl, ref, st](double v) {
            if (impl->updating) return;
            ref().strength = v;
            impl->updating = true;
            st->setValue((int)std::lround(v * 100));
            impl->updating = false;
            impl->change(true);
        });
        QObject::connect(st, &QSlider::valueChanged, this, [impl, ref, sb](int v) {
            if (impl->updating) return;
            ref().strength = v / 100.0;
            impl->updating = true;
            sb->setValue(v / 100.0);
            impl->updating = false;
            impl->change(true);
        });
        QObject::connect(on, &QCheckBox::toggled, this, [impl, ref](bool v) {
            if (impl->updating) return;
            ref().enabled = v;
            impl->change(true);
        });
        QObject::connect(rs, &QPushButton::clicked, this, [impl, ref] {
            ref() = LutRef();
            impl->change(true);
            impl->load();
        });
        d->refreshMore.push_back([impl, cb, file, interp, st, sb, on, ref] {
            const LutRef& r = ref();
            const int at = cb->findData(QString::fromStdString(r.source));
            cb->setCurrentIndex(std::max(0, at));
            if (r.source.rfind("file:", 0) == 0) file->setText(QString::fromStdString(r.source.substr(5)));
            else file->setText(at > 0 ? cb->itemText(at) : QString());
            interp->setCurrentIndex(r.tetrahedral ? 1 : 0);
            st->setValue((int)std::lround(r.strength * 100));
            sb->setValue(r.strength);
            on->setChecked(r.enabled);
            (void)impl;
        });
        return page;
    };
    d->sec1->addTab(lutPage(true), "Input LUT");
    {
        QWidget* page = new QWidget(this);
        auto* row = new QHBoxLayout(page);
        const struct { const char* name; double lo, hi, neutral; } ws[4] = {
            {"Lift", -1.0, 1.0, 0.0}, {"Gamma", 0.1, 4.0, 1.0}, {"Gain", 0.0, 4.0, 1.0}, {"Offset", -1.0, 1.0, 0.0}};
        for (int k = 0; k < 4; ++k) {
            d->wheels[k] = new GradeWheel(ws[k].name, ws[k].lo, ws[k].hi, ws[k].neutral, this);
            d->wheels[k]->setObjectName(QString("gradeWheel%1").arg(ws[k].name));
            d->wheels[k]->onBegin = [impl] { impl->begin(); };
            d->wheels[k]->onEnd = [impl] { impl->end(); };
            d->wheels[k]->onChange = [impl](bool once) {
                impl->fromWheels();
                impl->syncChannels();
                impl->change(once);
            };
            row->addWidget(d->wheels[k]);
        }
        d->sec1->addTab(page, "Color Wheels");
    }
    {
        // Color Channels: each wheel's red, green and blue as vertical sliders (same numbers as the R, G, B boxes)
        QWidget* page = new QWidget(this);
        auto* row = new QHBoxLayout(page);
        const char* names[4] = {"Lift", "Gamma", "Gain", "Offset"};
        const char* chan[3] = {"R", "G", "B"};
        for (int k = 0; k < 4; ++k) {
            auto* col = new QVBoxLayout();
            col->addWidget(heading(names[k]));
            auto* sl3 = new QHBoxLayout();
            for (int c = 0; c < 3; ++c) {
                QSlider* sl = d->chSlider[k][c] = new QSlider(Qt::Vertical, this);
                sl->setRange(0, 1000);
                sl->setObjectName(QString("gradeChannel_%1_%2").arg(names[k]).arg(chan[c]));
                QDoubleSpinBox* box = d->chBox[k][c] = new QDoubleSpinBox(this);
                box->setObjectName(QString("gradeChannelBox_%1_%2").arg(names[k]).arg(chan[c]));
                const double lo = d->wheels[k]->lo() - 1.0, hi = d->wheels[k]->hi() + 1.0;
                box->setRange(lo, hi);
                box->setDecimals(3);
                box->setSingleStep(0.05);
                box->setKeyboardTracking(false);
                QLabel* l = new QLabel(chan[c], this);
                sl3->addWidget(vcolumn(l, sl, box));
                QObject::connect(sl, &QSlider::sliderPressed, this, [impl] { impl->begin(); });
                QObject::connect(sl, &QSlider::valueChanged, this, [impl, k, c, sl, lo, hi](int pos) {
                    if (impl->updating) return;
                    impl->wheels[k]->setRgb(c, lo + (hi - lo) * pos / 1000.0, !sl->isSliderDown());
                });
                QObject::connect(sl, &QSlider::sliderReleased, this, [impl] { impl->end(); });
                QObject::connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [impl, k, c](double v) {
                    if (impl->updating) return;
                    impl->wheels[k]->setRgb(c, v, true);
                });
                sl->installEventFilter(this);
                sl->setProperty("gradeResetBox", QVariant::fromValue<QObject*>(box));
                sl->setProperty("gradeNeutral", k == 1 || k == 2 ? 1.0 : 0.0);
            }
            col->addLayout(sl3, 1);
            row->addLayout(col, 1);
        }
        d->sec1->addTab(page, "Color Channels");
    }
    {
        // Input/Output: Levels (Input Min, Output Min, Input Max, Output Max) and the Independent Controls
        QWidget* page = new QWidget(this);
        auto* row = new QHBoxLayout(page);
        auto* lev = new QVBoxLayout();
        lev->addWidget(heading("Levels"));
        auto* levRow = new QHBoxLayout();
        const struct { const char* label; const char* name; double* f; double neutral; } ls[4] = {
            {"Input Min", "blackIn", &d->g.blackIn, 0}, {"Output Min", "blackOut", &d->g.blackOut, 0},
            {"Input Max", "whiteIn", &d->g.whiteIn, 1}, {"Output Max", "whiteOut", &d->g.whiteOut, 1}};
        for (auto& e : ls) {
            auto f = makeField(e.label, e.name, e.f, 0, 1, e.neutral, 2, true);
            levRow->addWidget(vcolumn(f.l, f.s, f.b));
        }
        lev->addLayout(levRow, 1);
        row->addLayout(lev, 4);
        auto* ind = new QVBoxLayout();
        ind->addWidget(heading("Independent Controls"));
        auto* indRow = new QHBoxLayout();
        const struct { const char* label; const char* name; double* f; double lo, hi, neutral; int dec; } is[5] = {
            {"Saturation", "saturation", &d->g.saturation, 0, 3, 1, 2}, {"Exposure", "exposure", &d->g.exposure, -5, 5, 0, 2},
            {"Contrast", "contrast", &d->g.contrast, 0, 3, 1, 2}, {"Temperature", "temperature", &d->g.temperature, -100, 100, 0, 0},
            {"Tint", "tint", &d->g.tint, -100, 100, 0, 0}};
        for (auto& e : is) {
            auto f = makeField(e.label, e.name, e.f, e.lo, e.hi, e.neutral, e.dec, true);
            indRow->addWidget(vcolumn(f.l, f.s, f.b));
        }
        ind->addLayout(indRow, 1);
        row->addLayout(ind, 5);
        d->sec1->addTab(page, "Input/Output");
    }
    // Reno's extra pages (VEGAS Pro 23): RL Color Wheels, Utilities
    {
        QWidget* page = new QWidget(this);
        auto* grid = new QGridLayout(page);
        grid->setVerticalSpacing(3);
        int row = 0;
        struct { const char* label; const char* name; ColorGrade::Wheel* w; } ws[4] = {
            {"Shadows", "shadows", &d->g.shadows}, {"Midtones", "midtones", &d->g.midtones}, {"Highlights", "highlights", &d->g.highlights}, {"Offset", "rlOffset", &d->g.rlOffset}};
        for (auto& e : ws) {
            const std::string n = e.name;
            for (int part = 0; part < 3; ++part) {
                const char* suffix[3] = {" X", " Y", " Luminance"};
                const char* key[3] = {"X", "Y", "Master"};
                double* f = part == 0 ? &e.w->x : part == 1 ? &e.w->y : &e.w->master;
                auto fld = makeField(QString::fromStdString(std::string(e.label) + suffix[part]), (n + key[part]).c_str(), f, -1, 1, 0, 2, false);
                hrow(grid, row++, fld.l, fld.s, fld.b);
            }
        }
        {
            auto f1 = makeField("Shadow / midtone threshold", "rlLow", &d->g.rlLow, 0, 1, 0.25, 2, false);
            hrow(grid, row++, f1.l, f1.s, f1.b);
            auto f2 = makeField("Midtone / highlight threshold", "rlHigh", &d->g.rlHigh, 0, 1, 0.75, 2, false);
            hrow(grid, row++, f2.l, f2.s, f2.b);
        }
        d->sec1->addTab(scrolled(page), "RL Color Wheels");
    }
    {
        QWidget* page = new QWidget(this);
        auto* grid = new QGridLayout(page);
        grid->setVerticalSpacing(3);
        int row = 0;
        const struct { const char* label; const char* name; double* f; double lo, hi, neutral; } us[3] = {
            {"Brightness", "brightness", &d->g.brightness, -1, 1, 0}, {"Contrast center", "contrastCenter", &d->g.contrastCenter, 0, 1, 0.5},
            {"Log exposure", "logExposure", &d->g.logExposure, -4, 4, 0}};
        for (auto& e : us) {
            auto f = makeField(e.label, e.name, e.f, e.lo, e.hi, e.neutral, 2, false);
            hrow(grid, row++, f.l, f.s, f.b);
        }
        auto* pk = new QHBoxLayout();
        const char* names[3] = {"Black Point", "White Point", "Select Neutral Tone"};
        for (int k = 0; k < 3; ++k) {
            QPushButton* b = d->pickBtn[k] = new QPushButton(names[k], page);
            b->setCheckable(true);
            b->setObjectName(QString("gradePick%1").arg(k));
            b->setToolTip("Click, then click or drag over the picture in the Video Preview window");
            pk->addWidget(b);
            QObject::connect(b, &QPushButton::toggled, this, [this, impl, k](bool on) {
                if (on) {
                    for (int j = 0; j < 3; ++j)
                        if (j != k && impl->pickBtn[j]->isChecked()) impl->pickBtn[j]->setChecked(false);
                    impl->picking = k + 1;
                    if (setPickerHook) setPickerHook([this](int state, const QRectF& area, const QImage& region) { pickedRegion(state, area, region); });
                } else if (impl->picking == k + 1) {
                    impl->picking = 0;
                    if (setPickerHook) setPickerHook({});
                }
            });
        }
        d->pickInfo = new QLabel(page);
        d->pickInfo->setObjectName("gradePickInfo");
        pk->addWidget(d->pickInfo, 1);
        grid->addLayout(pk, row++, 0, 1, 3);
        QPushButton* rs = new QPushButton("Reset Utilities", page);
        rs->setObjectName("gradeResetUtilities");
        grid->addWidget(rs, row++, 0, 1, 3);
        grid->setRowStretch(row, 1);
        QObject::connect(rs, &QPushButton::clicked, this, [impl] {
            ColorGrade n;
            ColorGrade& g = impl->g;
            g.brightness = n.brightness; g.contrastCenter = n.contrastCenter; g.logExposure = n.logExposure;
            impl->change(true);
            impl->load();
        });
        d->sec1->addTab(scrolled(page), "Utilities");
    }

    {
        QWidget* page = new QWidget(this);
        auto* lay = new QHBoxLayout(page);
        auto* side = new QVBoxLayout();
        auto* group = new QButtonGroup(page);
        group->setExclusive(true);
        const char* names[4] = {"Brightness", "Red", "Green", "Blue"};
        for (int k = 0; k < 4; ++k) {
            QCheckBox* c = d->curveCheck[k] = new QCheckBox(names[k], page);
            c->setObjectName(QString("gradeCurveChannel%1").arg(k));
            group->addButton(c, k);
            side->addWidget(c);
        }
        d->curveCheck[0]->setChecked(true);
        QToolButton* autoBtn = new QToolButton(page);
        autoBtn->setObjectName("gradeAutoColorBalance");
        autoBtn->setText("Auto Color Balance");
        autoBtn->setPopupMode(QToolButton::InstantPopup);
        QMenu* am = new QMenu(autoBtn);
        const char* autos[3] = {"Auto Color Balance", "Auto Black Point", "Auto White Point"};
        for (int k = 0; k < 3; ++k) {
            QAction* a = am->addAction(autos[k]);
            QObject::connect(a, &QAction::triggered, this, [this, k] { autoAdjust(k); });
        }
        autoBtn->setMenu(am);
        side->addSpacing(6);
        side->addWidget(autoBtn);
        QPushButton* rs = new QPushButton(QString::fromUtf8("\xe2\x86\xba Reset"), page);
        rs->setObjectName("gradeCurveReset");
        side->addWidget(rs);
        side->addStretch(1);
        lay->addLayout(side);
        d->curves = new GradeCurves(page);
        d->curves->setObjectName("gradeCurves");
        d->curves->onBegin = [impl] { impl->begin(); };
        d->curves->onEnd = [impl] { impl->end(); };
        d->curves->onChange = [impl](bool once) {
            for (int k = 0; k < 4; ++k) impl->g.curve[k] = impl->curves->pts[k];
            impl->change(once);
        };
        lay->addWidget(d->curves, 1);
        QObject::connect(group, QOverload<int>::of(&QButtonGroup::idClicked), this, [impl](int i) {
            impl->curves->channel = i;
            impl->curves->update();
        });
        QObject::connect(rs, &QPushButton::clicked, this, [impl] {
            const int k = impl->curves->channel;
            impl->curves->pts[k].clear();
            impl->g.curve[k].clear();
            impl->change(true);
            impl->load();
        });
        d->sec2->addTab(page, "Color Curves");
    }
    {
        // HSL: Hue, Saturation and Luminance, and (Reno's) the HSL curves
        QWidget* page = new QWidget(this);
        auto* lay = new QHBoxLayout(page);
        auto* grid = new QGridLayout();
        grid->setVerticalSpacing(3);
        const struct { const char* label; const char* name; double* f; double lo, hi, neutral; } hs[3] = {
            {"Hue", "hue", &d->g.hue, -1, 1, 0}, {"Saturation", "hslSaturation", &d->g.hslSaturation, 0, 3, 1}, {"Luminance", "luminance", &d->g.luminance, -1, 1, 0}};
        int row = 0;
        for (auto& e : hs) {
            auto f = makeField(e.label, e.name, e.f, e.lo, e.hi, e.neutral, 2, false);
            hrow(grid, row++, f.l, f.s, f.b);
        }
        QComboBox* kind = new QComboBox(page);
        kind->setObjectName("gradeHslCurveKind");
        kind->addItems({"Hue vs Hue", "Hue vs Sat", "Hue vs Lum", "Sat vs Sat", "Sat vs Lum", "Lum vs Sat"});
        QPushButton* rs = new QPushButton("Reset Curve", page);
        rs->setObjectName("gradeHslCurveReset");
        grid->addWidget(kind, row++, 0, 1, 3);
        grid->addWidget(rs, row++, 0, 1, 3);
        grid->setRowStretch(row, 1);
        lay->addLayout(grid, 1);
        d->hslCurves = new GradeCurves(page);
        d->hslCurves->setObjectName("gradeHslCurves");
        d->hslCurves->hsl = true;
        d->hslCurves->onBegin = [impl] { impl->begin(); };
        d->hslCurves->onEnd = [impl] { impl->end(); };
        d->hslCurves->onChange = [impl](bool once) {
            for (int k = 0; k < ColorGrade::HslCurveCount; ++k) impl->g.hslCurve[k] = impl->hslCurves->pts[k];
            impl->change(once);
        };
        lay->addWidget(d->hslCurves, 1);
        QObject::connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [impl](int i) {
            impl->hslCurves->channel = i;
            impl->hslCurves->update();
        });
        QObject::connect(rs, &QPushButton::clicked, this, [impl] {
            const int k = impl->hslCurves->channel;
            impl->hslCurves->pts[k].clear();
            impl->g.hslCurve[k].clear();
            impl->change(true);
            impl->load();
        });
        d->refreshMore.push_back([impl] {
            for (int k = 0; k < ColorGrade::HslCurveCount; ++k) impl->hslCurves->pts[k] = impl->g.hslCurve[k];
            impl->hslCurves->update();
        });
        d->sec2->addTab(page, "HSL");
    }
    d->sec2->addTab(lutPage(false), "Look LUT");
    d->sec1->setMinimumHeight(170);
    d->sec2->setMinimumHeight(170);

    auto* fin = new QVBoxLayout();
    fin->setSpacing(4);
    QLabel* fl = new QLabel("Finishing", this);
    fin->addWidget(fl);
    d->bypass = new QPushButton("Bypass Color Grading", this);
    d->bypass->setObjectName("gradeBypass");
    d->bypass->setCheckable(true);
    QPushButton* mask = new QPushButton(QString::fromUtf8("B\xc3\xa9zier Mask"), this);
    mask->setObjectName("gradeBezierMask");
    mask->setToolTip(QString::fromUtf8("Adds VEGAS's B\xc3\xa9zier Masking effect to what is graded and opens it"));
    QPushButton* broadcast = new QPushButton("Broadcast Colors", this);
    broadcast->setObjectName("gradeBroadcastColors");
    broadcast->setToolTip("Adds VEGAS's Broadcast Colors effect to what is graded and opens it");
    QPushButton* reset = new QPushButton("Reset All", this);
    reset->setObjectName("gradeReset");
    QPushButton* lut = new QPushButton("Export LUT", this);
    lut->setObjectName("gradeExportLut");
    lut->setToolTip("Saves the grade as a LUT file (.cube) to use elsewhere");
    d->pinBtn = new QPushButton("Pin/Unpin", this);
    d->pinBtn->setObjectName("gradePin");
    d->pinBtn->setCheckable(true);
    d->pinBtn->setToolTip("Pinned: keep grading the current target instead of following the selection");
    QPushButton* exitBtn = new QPushButton("Exit", this);
    exitBtn->setObjectName("gradeExit");
    exitBtn->setToolTip("Hides the panel (the grade stays on what it grades)");
    for (QPushButton* b : {d->bypass, mask, broadcast, reset, lut, d->pinBtn, exitBtn}) {
        b->setMinimumWidth(146);
        fin->addWidget(b);
    }
    fin->addStretch(1);
    QObject::connect(mask, &QPushButton::clicked, this, [this] { addPlugin("com.vegascreativesoftware:bzmasking"); });
    QObject::connect(broadcast, &QPushButton::clicked, this, [this] { addPlugin("com.vegascreativesoftware:broadcastcolors"); });
    QObject::connect(reset, &QPushButton::clicked, this, [this] { resetAll(); });
    QObject::connect(lut, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, "Export LUT", QSettings().value("colorGrading/lutFolder").toString(), "Cube LUT (*.cube)");
        if (f.isEmpty()) return;
        QSettings().setValue("colorGrading/lutFolder", QFileInfo(f).absolutePath());
        std::string err;
        if (!exportLut(f.toStdString(), &err)) QMessageBox::warning(this, "Export LUT", QString::fromStdString(err));
        else AppState::get().status("Exported the grade as " + f.toStdString());
    });
    QObject::connect(d->pinBtn, &QPushButton::toggled, this, [this](bool on) { setPinned(on); });
    QObject::connect(exitBtn, &QPushButton::clicked, this, [this] {
        for (QWidget* w = parentWidget(); w; w = w->parentWidget())
            if (QDockWidget* dw = qobject_cast<QDockWidget*>(w)) {
                dw->close();
                return;
            }
        hide();
    });
    root->addWidget(d->sec1, 6);
    root->addWidget(d->sec2, 3);
    root->addLayout(fin, 0);
}

// The pickers' mouse: the average colour of what is picked becomes the black point, white point or neutral tone.
void ColorGradingPanel::pickedRegion(int state, const QRectF&, const QImage& region)
{
    if (d->picking == 0 || region.isNull()) return;
    double r = 0, gr = 0, b = 0;
    long n = 0;
    for (int y = 0; y < region.height(); ++y)
        for (int x = 0; x < region.width(); ++x) {
            const QRgb c = region.pixel(x, y);
            r += qRed(c); gr += qGreen(c); b += qBlue(c);
            ++n;
        }
    if (!n) return;
    const QColor avg((int)std::lround(r / n), (int)std::lround(gr / n), (int)std::lround(b / n));
    d->pickInfo->setText(QString("R %1  G %2  B %3").arg(avg.red()).arg(avg.green()).arg(avg.blue()));
    d->pickInfo->setStyleSheet(QString("QLabel { border-left: 14px solid %1; padding-left: 4px; }").arg(avg.name()));
    if (state != 3) return;
    const float rgb[3] = {avg.redF(), avg.greenF(), avg.blueF()};
    if (d->picking == 1) d->g.setBlackPoint(rgb);
    else if (d->picking == 2) d->g.setWhitePoint(rgb);
    else d->g.setNeutralTone(rgb);
    d->change(true);
    d->load();
    const int k = d->picking - 1;
    d->picking = 0;
    d->pickBtn[k]->setChecked(false);
    if (setPickerHook) setPickerHook({});
}

void ColorGradingPanel::pickColour(int which, const QImage& region)
{
    if (which < 1 || which > 3) return;
    d->picking = which;
    pickedRegion(3, QRectF(), region);
}

void ColorGradingPanel::setField(const std::string& name, double v)
{
    ColorGrade& g = d->g;
    struct F { const char* n; double* p; };
    const F fs[] = {{"blackIn", &g.blackIn}, {"whiteIn", &g.whiteIn}, {"blackOut", &g.blackOut}, {"whiteOut", &g.whiteOut}, {"brightness", &g.brightness},
                    {"contrastCenter", &g.contrastCenter}, {"logExposure", &g.logExposure}, {"hue", &g.hue}, {"hslSaturation", &g.hslSaturation},
                    {"luminance", &g.luminance}, {"rlLow", &g.rlLow}, {"rlHigh", &g.rlHigh}};
    for (const F& f : fs)
        if (name == f.n) {
            *f.p = v;
            d->change(true);
            d->load();
        }
}

void ColorGradingPanel::setLut(bool camera, const std::string& source, double strength, bool tetrahedral)
{
    LutRef& r = camera ? d->g.cameraLut : d->g.lookLut;
    r.source = source;
    r.strength = strength;
    r.tetrahedral = tetrahedral;
    r.enabled = true;
    d->change(true);
    d->load();
}

void ColorGradingPanel::setHslCurve(int kind, const std::vector<std::pair<double, double>>& points)
{
    if (kind < 0 || kind >= ColorGrade::HslCurveCount) return;
    d->g.hslCurve[kind] = points;
    d->change(true);
    d->load();
}

ColorGradingPanel::ColorGradingPanel(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->q = this;
    setObjectName("colorGrading");
    Impl* impl = d.get();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 4, 6, 6);
    root->setSpacing(4);
    // top: what is graded (VEGAS grades the selected event; Reno can also grade a track or the video output)
    auto* top = new QHBoxLayout();
    d->applyBox = new QComboBox(this);
    d->applyBox->setObjectName("gradeApplyTo");
    d->applyBox->addItem("Selected Event");
    d->applyBox->addItem("Selected Track");
    d->applyBox->addItem("Video Output");
    d->targetLbl = new QLabel(this);
    d->targetLbl->setObjectName("gradeTarget");
    top->addWidget(new QLabel("Grade:", this));
    top->addWidget(d->applyBox);
    top->addWidget(d->targetLbl, 1);
    root->addLayout(top);
    auto* main = new QHBoxLayout();
    main->setSpacing(8);
    buildPages(main);
    root->addLayout(main, 1);
    d->sec1->setCurrentIndex(1);

    QObject::connect(d->applyBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { setApplyTo((Apply)i); });
    QObject::connect(d->bypass, &QPushButton::toggled, this, [impl](bool on) {
        if (impl->updating) return;
        AppState& app = AppState::get();
        const FxTarget t = impl->currentTarget();
        const FxSlot* s = Impl::gradeSlot(app.chainOf(app.project(), t));
        if (s) app.setFxBypass(t, s->id, on);
    });
    d->listener.reset(new Listener([impl](int changes) {
        if (changes & kSelection) impl->pinned = FxTarget();
        if (changes & (kProject | kSelection)) impl->load();
    }));
    d->apply = (Apply)std::min(2, std::max(0, QSettings().value("colorGrading/applyTo", 0).toInt()));
    d->applyBox->setCurrentIndex((int)d->apply);
    d->load();
}

ColorGradingPanel::~ColorGradingPanel()
{
    if (d->dragging) AppState::get().cancelDrag();
}

void ColorGradingPanel::setApplyTo(Apply a)
{
    d->apply = a;
    d->pinned = FxTarget();
    if (d->pinLock) d->pinBtn->setChecked(false);
    QSettings().setValue("colorGrading/applyTo", (int)a);
    if (d->applyBox->currentIndex() != (int)a) d->applyBox->setCurrentIndex((int)a);
    d->load();
}

ColorGradingPanel::Apply ColorGradingPanel::applyTo() const { return d->apply; }
FxTarget ColorGradingPanel::target() const { return d->currentTarget(); }

uint64_t ColorGradingPanel::slot() const
{
    AppState& app = AppState::get();
    const FxSlot* s = Impl::gradeSlot(app.chainOf(app.project(), d->currentTarget()));
    return s ? s->id : 0;
}

void ColorGradingPanel::showSlot(uint64_t, const FxTarget& target)
{
    d->pinned = target;
    d->load();
}

void ColorGradingPanel::setValue(const std::string& name, double v)
{
    for (int k = 0; k < 5; ++k)
        if (name == kSliderNames[k]) {
            d->setNamed(k, v);
            d->change(true);
            d->load();
        }
}

void ColorGradingPanel::setWheel(const std::string& name, double x, double y, double master)
{
    const char* names[4] = {"lift", "gamma", "gain", "offset"};
    for (int k = 0; k < 4; ++k)
        if (name == names[k]) {
            d->wheels[k]->set(x, y, master);
            d->fromWheels();
            d->change(true);
            d->load();
        }
}

void ColorGradingPanel::setCurve(int channel, const std::vector<std::pair<double, double>>& points)
{
    if (channel < 0 || channel > 3) return;
    d->g.curve[channel] = points;
    d->change(true);
    d->load();
}

void ColorGradingPanel::resetAll()
{
    d->g = ColorGrade();
    d->change(true);
    d->load();
}

// Pin/Unpin: pinned, the panel keeps grading what it is grading whatever is selected next.
void ColorGradingPanel::setPinned(bool on)
{
    if (on == d->pinLock) return;
    if (on) {
        d->pinTarget = d->currentTarget();
        if (d->pinTarget.kind == FxTarget::None) {
            d->pinBtn->setChecked(false);
            return;
        }
        d->pinLock = true;
    } else {
        d->pinLock = false;
        d->pinTarget = FxTarget();
    }
    if (d->pinBtn->isChecked() != on) d->pinBtn->setChecked(on);
    d->load();
}
bool ColorGradingPanel::pinned() const { return d->pinLock; }

// Auto Color Balance (0: the image's average colour becomes neutral), Auto Black Point / White Point (1, 2: the darkest
// / brightest fifth of a percent of pixels become black / white), from the image frameSource gives.
void ColorGradingPanel::autoAdjust(int kind)
{
    if (!frameSource) return;
    const QImage img = frameSource();
    if (img.isNull() || d->currentTarget().kind == FxTarget::None) return;
    const QImage src = img.convertToFormat(QImage::Format_RGB32);
    std::vector<std::pair<int, QRgb>> px;
    const int stepY = std::max(1, src.height() / 90), stepX = std::max(1, src.width() / 160);
    for (int y = 0; y < src.height(); y += stepY)
        for (int x = 0; x < src.width(); x += stepX) {
            const QRgb c = src.pixel(x, y);
            px.push_back({(qRed(c) * 3 + qGreen(c) * 6 + qBlue(c)) / 10, c});
        }
    if (px.empty()) return;
    std::sort(px.begin(), px.end(), [](const std::pair<int, QRgb>& a, const std::pair<int, QRgb>& b) { return a.first < b.first; });
    size_t from = 0, to = px.size();
    if (kind == 1) to = std::max<size_t>(1, px.size() / 50);
    else if (kind == 2) from = px.size() - std::max<size_t>(1, px.size() / 50);
    double r = 0, g = 0, b = 0;
    for (size_t i = from; i < to; ++i) {
        r += qRed(px[i].second);
        g += qGreen(px[i].second);
        b += qBlue(px[i].second);
    }
    const double n = (double)(to - from);
    QImage one(1, 1, QImage::Format_RGB32);
    one.setPixel(0, 0, qRgb((int)std::lround(r / n), (int)std::lround(g / n), (int)std::lround(b / n)));
    pickColour(kind == 0 ? 3 : kind, one);
}

Json ColorGradingPanel::state() const { return d->g.toJson(); }
QWidget* ColorGradingPanel::wheel(int k) const { return k >= 0 && k < 4 ? d->wheels[k] : nullptr; }
QWidget* ColorGradingPanel::curveEditor() const { return d->curves; }

bool ColorGradingPanel::eventFilter(QObject* o, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonDblClick && o->property("gradeResetBox").isValid()) {
        if (QDoubleSpinBox* b = qobject_cast<QDoubleSpinBox*>(o->property("gradeResetBox").value<QObject*>())) {
            const double n = o->property("gradeNeutral").toDouble();
            b->setValue(n);
        }
        return true;
    }
    return QWidget::eventFilter(o, e);
}

bool ColorGradingPanel::exportLut(const std::string& path, std::string* error) const
{
    ColorGrade g = d->g;
    g.prepare();
    const int n = 33;
    std::string text = "# Made by Reno's Color Grading panel\nTITLE \"Color Grading\"\nLUT_3D_SIZE 33\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    text.reserve(text.size() + (size_t)n * n * n * 30);
    // the .cube order: red changes fastest, then green, then blue
    for (int b = 0; b < n; ++b)
        for (int gi = 0; gi < n; ++gi)
            for (int r = 0; r < n; ++r) {
                float c[3] = {(float)r / (n - 1), (float)gi / (n - 1), (float)b / (n - 1)};
                g.apply(c);
                for (float& v : c)
                    if (!std::isfinite(v)) v = 0;
                text += strfmt("%.6f %.6f %.6f\n", c[0], c[1], c[2]);
            }
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.data(), (qint64)text.size()) != (qint64)text.size()) {
        if (error) *error = "Could not write " + path;
        return false;
    }
    return true;
}

bool ColorGradingPanel::addPlugin(const std::string& pluginId)
{
    AppState& app = AppState::get();
    const FxTarget t = d->currentTarget();
    if (t.kind == FxTarget::None) {
        app.status("Select a video event to grade first.");
        return false;
    }
    VideoFxEntry e;
    if (!Catalog::get().findVideo(pluginId, &e) || !e.ok) {
        app.status("VEGAS's " + std::string(pluginId.find("bzmask") != std::string::npos ? "B\xc3\xa9zier Masking" : "Broadcast Colors") +
                   " effect is not installed on this computer.");
        return false;
    }
    const uint64_t slot = app.addVideoFx(t, pluginId, e.label);
    if (slot && onOpenFx) onOpenFx(t, slot);
    return slot != 0;
}

}
}
