// VEGAS's Surround Panner: see surroundpanner.h.
#include "surroundpanner.h"
#include "toolwin.h"
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace reno {
namespace app {

using namespace tw;

SurroundPan* surroundPanOf(Project& p, uint64_t id)
{
    if (!id) return nullptr;
    for (auto& t : p.tracks)
        if (t.id == id) return t.type == TrackType::Audio ? &t.surround : nullptr;
    for (auto& b : p.buses)
        if (b.id == id) return &b.surround;
    return nullptr;
}
const SurroundPan* surroundPanOf(const Project& p, uint64_t id) { return surroundPanOf(const_cast<Project&>(p), id); }

namespace {

const double kPi = 3.14159265358979323846;

// Where the speakers stand in the room (x, y): L, R, C, LFE (drawn at the rear middle; 5.1 has no speaker there), Ls, Rs.
const double kSpeakerX[6] = {-1, 1, 0, 0, -1, 1};
const double kSpeakerY[6] = {1, 1, 1, -1, -1, -1};
const char* kSpeakerName[6] = {"L", "R", "C", "LFE", "Ls", "Rs"};

// Sets the pan point (x, y) in 'sp': the key at the cursor (created if missing) when it has keyframes, else the
// point itself. 'motion' keeps one axis where it is.
void placePoint(SurroundPan& sp, double x, double y, double cursor, double frame) { surroundPlacePoint(sp, x, y, cursor, frame); }

}  // namespace

SurroundField::SurroundField(bool compact, QWidget* parent) : QWidget(parent), compact_(compact)
{
    setFocusPolicy(compact ? Qt::ClickFocus : Qt::StrongFocus);
    setMouseTracking(false);
    if (compact) setFixedSize(44, 44);
    else setMinimumSize(180, 180);
    setObjectName(compact ? "surroundMini" : "surroundField");
}

QSize SurroundField::sizeHint() const { return compact_ ? QSize(44, 44) : QSize(260, 260); }

QRectF SurroundField::room() const
{
    const double m = compact_ ? 4.0 : 26.0;
    const double side = std::max(10.0, std::min(width(), height()) - 2 * m);
    return QRectF((width() - side) / 2.0, (height() - side) / 2.0, side, side);
}

QPointF SurroundField::toWidget(double x, double y) const
{
    const QRectF r = room();
    return QPointF(r.center().x() + x * r.width() / 2.0, r.center().y() - y * r.height() / 2.0);
}

void SurroundField::toRoom(const QPointF& w, double& x, double& y) const
{
    const QRectF r = room();
    x = (w.x() - r.center().x()) / (r.width() / 2.0);
    y = -(w.y() - r.center().y()) / (r.height() / 2.0);
}

QPointF SurroundField::point() const
{
    const SurroundPan* sp = get ? get() : nullptr;
    if (!sp) return QPointF(0, 0);
    double x, y, c;
    surroundPointAt(*sp, AppState::get().cursor(), x, y, c);
    return QPointF(x, y);
}

QPointF SurroundField::pointPos() const
{
    const QPointF p = point();
    return toWidget(p.x(), p.y());
}

QRectF SurroundField::speakerRect(int ch) const
{
    const QPointF c = toWidget(kSpeakerX[ch], kSpeakerY[ch]);
    const double s = compact_ ? 6.0 : 22.0;
    const double dx = kSpeakerX[ch] < 0 ? -s * 0.55 : (kSpeakerX[ch] > 0 ? s * 0.55 : 0);
    const double dy = kSpeakerY[ch] > 0 ? -s * 0.55 : s * 0.55;
    return QRectF(c.x() - s / 2 + dx, c.y() - s / 2 + dy, s, s);
}

void SurroundField::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), compact_ ? QColor(30, 30, 30) : gap());
    const SurroundPan* sp = get ? get() : nullptr;
    const QRectF r = room();
    p.setPen(QPen(QColor(90, 90, 90), 1));
    p.setBrush(QColor(22, 22, 24));
    p.drawRect(r);
    if (!sp) {
        if (!compact_) {
            p.setPen(text());
            p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, "No Panner Selected");
        }
        return;
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(60, 60, 64), 1));
    p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    if (!compact_) {
        for (double k : {0.5, 1.0}) p.drawEllipse(r.center(), r.width() / 2 * k, r.height() / 2 * k);
        p.drawEllipse(r.center(), r.width() / 2 * std::sqrt(2.0), r.height() / 2 * std::sqrt(2.0));
    }
    double x, y, c;
    surroundPointAt(*sp, AppState::get().cursor(), x, y, c);
    float g[6] = {0, 0, 0, 0, 0, 0};
    const bool panned = sp->route == "surround";
    if (panned) surroundGains(*sp, x, y, c, g);
    else {
        if (sp->route == "front") g[ChL] = g[ChR] = 1;
        else if (sp->route == "rear") g[ChLs] = g[ChRs] = 1;
        else if (sp->route == "center") g[ChC] = 1;
        else if (sp->route == "lfe") g[ChLFE] = 1;
    }
    for (int ch = 0; ch < 6; ++ch) {
        const QRectF s = speakerRect(ch);
        const bool muted = (sp->speakerMutes >> ch) & 1u;
        const double share = std::min(1.0, (double)g[ch]);
        const QColor lit = QColor::fromRgbF(0.25 + 0.5 * share * 0.0, 0.25 + 0.55 * share, 0.27 + 0.6 * share);
        p.setPen(QPen(muted ? QColor(120, 60, 60) : QColor(150, 150, 150), compact_ ? 0.8 : 1.2));
        p.setBrush(muted ? QColor(50, 30, 30) : (share > 0.001 ? QColor(accent().red(), accent().green(), accent().blue(), 60 + (int)(190 * share)) : QColor(45, 45, 48)));
        (void)lit;
        if (ch == ChLFE) p.drawEllipse(s.adjusted(1, 1, -1, -1));
        else p.drawRoundedRect(s, 2, 2);
        if (!compact_) {
            p.setPen(muted ? QColor(200, 110, 110) : text());
            QFont f = font();
            f.setPixelSize(9);
            p.setFont(f);
            p.drawText(s, Qt::AlignCenter, kSpeakerName[ch]);
        }
        if (muted) {
            p.setPen(QPen(QColor(230, 70, 60), compact_ ? 1.0 : 2.0));
            p.drawLine(s.topLeft(), s.bottomRight());
            p.drawLine(s.topRight(), s.bottomLeft());
        }
    }
    if (!compact_ && sp->keys.size() > 1) {
        QPainterPath path;
        for (size_t i = 0; i < sp->keys.size(); ++i) {
            const QPointF q = toWidget(sp->keys[i].x, sp->keys[i].y);
            if (i == 0) path.moveTo(q);
            else path.lineTo(q);
        }
        p.setPen(QPen(QColor(200, 170, 60, 160), 1, Qt::DashLine));
        p.drawPath(path);
        for (const auto& k : sp->keys) {
            const QPointF q = toWidget(k.x, k.y);
            p.fillRect(QRectF(q.x() - 2.5, q.y() - 2.5, 5, 5), QColor(220, 190, 70));
        }
    }
    const QPointF pt = toWidget(x, y);
    if (panned && !compact_) {
        for (int ch = 0; ch < 6; ++ch) {
            if (ch == ChLFE || g[ch] < 0.01f) continue;
            p.setPen(QPen(QColor(18, 132, 214, (int)(40 + 160 * g[ch])), 1));
            p.drawLine(pt, speakerRect(ch).center());
        }
    }
    if (panned) {
        const double rad = compact_ ? 3.0 : 7.0;
        p.setPen(QPen(QColor(255, 255, 255), compact_ ? 1.0 : 1.5));
        p.setBrush(hasFocus() && selected_ && !compact_ ? QColor(255, 200, 60) : QColor(240, 120, 30));
        p.drawEllipse(pt, rad, rad);
    }
    if (!compact_) {
        QFont f = font();
        f.setPixelSize(10);
        p.setFont(f);
        p.setPen(text());
        p.drawText(QRectF(r.left() + 14, height() - 17, r.width() / 2 - 26, 15), Qt::AlignLeft | Qt::AlignVCenter,
                   panned ? QString("X %1   Y %2").arg(x * 100, 0, 'f', 1).arg(y * 100, 0, 'f', 1)
                          : QString(sp->route == "front" ? "Front Only" : sp->route == "rear" ? "Rear Only" : sp->route == "center" ? "Center Only" : "LFE Only"));
    }
    setProperty("pointX", x);
    setProperty("pointY", y);
}

void SurroundField::moveTo(double x, double y, bool drag)
{
    if (!get || !set || !get()) return;
    SurroundPan sp = *get();
    AppState& app = AppState::get();
    const double frame = 1.0 / std::max(1.0, app.project().settings.fps());
    placePoint(sp, x, y, app.cursor(), frame);
    std::function<void(Project&, const SurroundPan&)> fn = set;
    if (drag) app.dragUpdate([&](Project& p) { fn(p, sp); });
    else app.edit(undoName, [&](Project& p) { fn(p, sp); });
    update();
}

void SurroundField::mousePressEvent(QMouseEvent* e)
{
    const SurroundPan* sp = get ? get() : nullptr;
    if (!sp || e->button() != Qt::LeftButton) return;
    const QPointF pos = e->position();
    if (!compact_)
        for (int ch = 0; ch < 6; ++ch)
            if (speakerRect(ch).adjusted(-2, -2, 2, 2).contains(pos)) {
                SurroundPan n = *sp;
                n.speakerMutes ^= 1u << ch;
                std::function<void(Project&, const SurroundPan&)> fn = set;
                AppState::get().edit("Surround Pan Speaker Mutes", [&](Project& p) { fn(p, n); });
                update();
                return;
            }
    if (sp->route != "surround") return;
    selected_ = true;
    dragging_ = true;
    const QPointF pt = point();
    pressX_ = pt.x();
    pressY_ = pt.y();
    double x, y;
    toRoom(pos, x, y);
    pressRoom_ = QPointF(x, y);
    AppState::get().beginDrag();
    // a click away from the point moves it there (as VEGAS does); on it, the drag continues from where it is. The small
    // strip panner never jumps: a drag moves the point from where it was, and a double-click opens the window.
    if (!compact_ && std::hypot(pos.x() - pointPos().x(), pos.y() - pointPos().y()) > 9) {
        moveTo(x, y, true);
        pressX_ = std::clamp(x, -1.0, 1.0);
        pressY_ = std::clamp(y, -1.0, 1.0);
    }
    update();
}

void SurroundField::mouseMoveEvent(QMouseEvent* e)
{
    if (!dragging_) return;
    double x, y;
    toRoom(e->position(), x, y);
    const Qt::KeyboardModifiers m = e->modifiers();
    double nx = pressX_ + (x - pressRoom_.x()), ny = pressY_ + (y - pressRoom_.y());
    if (m & Qt::ControlModifier) {
        nx = pressX_ + (x - pressRoom_.x()) * 0.1;
        ny = pressY_ + (y - pressRoom_.y()) * 0.1;
    }
    const bool shift = m & Qt::ShiftModifier, alt = m & Qt::AltModifier;
    if (alt) {
        const double radius = shift ? 1.0 : std::hypot(pressX_, pressY_);
        const double a = std::atan2(ny, nx);
        nx = radius * std::cos(a);
        ny = radius * std::sin(a);
    } else if (shift) {
        const double len = std::hypot(pressX_, pressY_);
        if (len > 1e-6) {
            const double ux = pressX_ / len, uy = pressY_ / len;
            const double t = nx * ux + ny * uy;
            nx = t * ux;
            ny = t * uy;
        }
    }
    moveTo(nx, ny, true);
}

void SurroundField::mouseReleaseEvent(QMouseEvent*)
{
    if (!dragging_) return;
    dragging_ = false;
    AppState::get().endDrag(undoName);
    update();
}

void SurroundField::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (compact_ && onOpenWindow) {
        onOpenWindow();
        return;
    }
    if (!compact_ && e->button() == Qt::LeftButton) moveTo(0, 0, false);
}

void SurroundField::wheelEvent(QWheelEvent* e)
{
    const SurroundPan* sp = get ? get() : nullptr;
    if (!sp || sp->route != "surround") return;
    const double steps = (e->angleDelta().y() + e->angleDelta().x()) / 120.0;
    const Qt::KeyboardModifiers m = e->modifiers();
    const double step = (m & Qt::ControlModifier ? 0.01 : 0.05) * steps;
    const QPointF pt = point();
    double x = pt.x(), y = pt.y();
    if (m & Qt::AltModifier) {
        const double radius = (m & Qt::ShiftModifier) ? 1.0 : std::hypot(x, y);
        const double a = std::atan2(y, x) - step * kPi;
        x = radius * std::cos(a);
        y = radius * std::sin(a);
    } else if (m & Qt::ShiftModifier) x += step;
    else y += step;
    moveTo(x, y, false);
    e->accept();
}

bool SurroundField::handleKey(int key, Qt::KeyboardModifiers mods)
{
    const SurroundPan* sp = get ? get() : nullptr;
    if (!sp || sp->route != "surround") return false;
    const bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
    const double step = ctrl ? 0.01 : 0.05;
    const QPointF pt = point();
    double x = pt.x(), y = pt.y();
    if (mods & Qt::KeypadModifier && key >= Qt::Key_1 && key <= Qt::Key_9) {
        const int n = key - Qt::Key_1;
        x = (double)(n % 3) - 1.0;
        y = (double)(n / 3) - 1.0;
        if (ctrl && x != 0 && y != 0) {
            x *= std::sqrt(0.5);
            y *= std::sqrt(0.5);
        }
        moveTo(x, y, false);
        return true;
    }
    switch (key) {
    case Qt::Key_Up: y += step; break;
    case Qt::Key_Down: y -= step; break;
    case Qt::Key_Left: x -= step; break;
    case Qt::Key_Right: x += step; break;
    case Qt::Key_PageUp: if (shift) x += step; else y += step; break;
    case Qt::Key_PageDown: if (shift) x -= step; else y -= step; break;
    default: return false;
    }
    moveTo(x, y, false);
    return true;
}

void SurroundField::keyPressEvent(QKeyEvent* e)
{
    if (handleKey(e->key(), e->modifiers())) return;
    QWidget::keyPressEvent(e);
}

namespace {

struct LabeledSlider {
    QSlider* slider = nullptr;
    QLabel* value = nullptr;
};

QString dbText(double gain)
{
    if (gain <= 1e-6) return QString("-Inf");
    const double db = 20.0 * std::log10(gain);
    return QString("%1 dB").arg(db, 0, 'f', 1);
}
int gainToSlider(double gain, double topDb)
{
    if (gain <= 1e-6) return 0;
    const double db = 20.0 * std::log10(gain);
    return (int)std::lround(std::clamp((db + 60.0) / (topDb + 60.0), 0.0, 1.0) * 999.0) + 1;
}
double sliderToGain(int v, double topDb)
{
    if (v <= 0) return 0.0;
    const double db = -60.0 + (topDb + 60.0) * (v - 1) / 999.0;
    return std::pow(10.0, db / 20.0);
}

}  // namespace

struct SurroundPannerWindow::Impl {
    SurroundPannerWindow* w = nullptr;
    uint64_t pinned = 0;
    bool stale = false;
    QComboBox* target = nullptr;
    QComboBox* route = nullptr;
    QComboBox* motion = nullptr;
    QLabel* message = nullptr;
    SurroundField* field = nullptr;
    LabeledSlider center, smooth, lfe;
    QCheckBox* lfeOn = nullptr;
    QPushButton* addKey = nullptr;
    QPushButton* delKey = nullptr;
    QPushButton* clearKeys = nullptr;
    QWidget* controls = nullptr;
    std::unique_ptr<Listener> listener;

    uint64_t shown() const
    {
        const Project& p = AppState::get().project();
        if (pinned && surroundPanOf(p, pinned)) return pinned;
        const uint64_t cur = AppState::get().currentTrack;
        if (surroundPanOf(p, cur)) return cur;
        return 0;
    }
    const SurroundPan* pan() const { return surroundPanOf(AppState::get().project(), shown()); }
    void change(const char* undo, const std::function<void(SurroundPan&)>& fn)
    {
        const uint64_t id = shown();
        if (!id) return;
        AppState::get().edit(undo, [&](Project& p) {
            if (SurroundPan* sp = surroundPanOf(p, id)) fn(*sp);
        });
    }

    void refresh()
    {
        const Project& p = AppState::get().project();
        const bool surround = p.settings.surround();
        {
            const QSignalBlocker b(target);
            target->clear();
            target->addItem("(the current track)", QVariant::fromValue<qulonglong>(0));
            int n = 0;
            for (const auto& t : p.tracks) {
                if (t.type != TrackType::Audio) continue;
                ++n;
                target->addItem(QString("Track %1%2").arg(n).arg(t.name.empty() ? QString() : ": " + qs(t.name)), QVariant::fromValue<qulonglong>(t.id));
            }
            for (size_t i = 0; i < p.buses.size(); ++i)
                target->addItem(p.buses[i].name.empty() ? QString("Bus %1").arg(QChar('A' + (int)(i % 26))) : qs(p.buses[i].name),
                                QVariant::fromValue<qulonglong>(p.buses[i].id));
            target->setCurrentIndex(std::max(0, target->findData(QVariant::fromValue<qulonglong>(pinned))));
        }
        const SurroundPan* sp = pan();
        message->setVisible(!surround || !sp);
        message->setText(!surround ? "No Surround Project\n\nProject Properties > Audio > Master bus mode: 5.1 Surround"
                                   : "No Panner Selected\n\nClick an audio track (or pick a track or bus above)");
        field->setVisible(surround && sp);
        controls->setEnabled(surround && sp);
        if (sp) {
            const QSignalBlocker b1(route), b2(motion), b3(center.slider), b4(smooth.slider), b5(lfe.slider), b6(lfeOn);
            route->setCurrentIndex(std::max(0, route->findData(qs(sp->route))));
            motion->setCurrentIndex(std::max(0, motion->findData(qs(sp->motion))));
            double x, y, c;
            surroundPointAt(*sp, AppState::get().cursor(), x, y, c);
            center.slider->setValue(gainToSlider(c, 0.0));
            center.value->setText(dbText(c));
            smooth.slider->setValue((int)std::lround(sp->smoothness * 100));
            smooth.value->setText(QString("%1 %").arg((int)std::lround(sp->smoothness * 100)));
            lfeOn->setChecked(sp->lfe);
            lfe.slider->setValue(gainToSlider(sp->lfeLevel, 12.0));
            lfe.value->setText(dbText(sp->lfeLevel));
            lfe.slider->setEnabled(sp->lfe);
            delKey->setEnabled(!sp->keys.empty());
            clearKeys->setEnabled(!sp->keys.empty());
            w->setProperty("keys", (int)sp->keys.size());
        }
        field->update();
    }
};

SurroundPannerWindow::SurroundPannerWindow(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->w = this;
    setObjectName("surroundPanner");
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, dark());
    pal.setColor(QPalette::WindowText, text());
    setPalette(pal);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);

    QHBoxLayout* top = new QHBoxLayout();
    d->target = new QComboBox(this);
    d->target->setObjectName("surroundTarget");
    d->target->setToolTip("Whose surround panner is shown: the current track, or a track or bus of your choice");
    top->addWidget(d->target, 1);
    d->route = new QComboBox(this);
    d->route->setObjectName("surroundRoute");
    for (auto r : {std::make_pair("Surround Pan", "surround"), std::make_pair("Front Only", "front"), std::make_pair("Center Only", "center"),
                   std::make_pair("LFE Only", "lfe"), std::make_pair("Rear Only", "rear")})
        d->route->addItem(r.first, QString(r.second));
    d->route->setToolTip("Where the sound goes: panned by the point, or straight to the front pair, the centre, the LFE or the rear pair");
    top->addWidget(d->route);
    v->addLayout(top);

    d->message = new QLabel(this);
    d->message->setAlignment(Qt::AlignCenter);
    d->message->setWordWrap(true);
    d->message->setObjectName("surroundMessage");
    v->addWidget(d->message, 1);
    d->field = new SurroundField(false, this);
    d->field->get = [this]() { return d->pan(); };
    d->field->set = [this](Project& p, const SurroundPan& sp) {
        if (SurroundPan* to = surroundPanOf(p, d->shown())) *to = sp;
    };
    d->field->setToolTip("Drag the point (Shift: along the line through the middle, Alt: round its circle, Alt+Shift: round the largest circle, "
                         "Ctrl: finer); arrows, Page Up/Down, the wheel and the keypad move it; click a speaker to mute it");
    v->addWidget(d->field, 4);

    d->controls = new QWidget(this);
    QGridLayout* g = new QGridLayout(d->controls);
    g->setContentsMargins(0, 0, 0, 0);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(2);
    auto slider = [&](LabeledSlider& s, const char* label, const char* name, int lo, int hi, int row) {
        g->addWidget(new QLabel(label, d->controls), row, 0);
        s.slider = new QSlider(Qt::Horizontal, d->controls);
        s.slider->setObjectName(name);
        s.slider->setRange(lo, hi);
        g->addWidget(s.slider, row, 1);
        s.value = new QLabel(d->controls);
        s.value->setMinimumWidth(56);
        g->addWidget(s.value, row, 2);
    };
    slider(d->center, "Center:", "surroundCenter", 0, 1000, 0);
    slider(d->smooth, "Smoothness:", "surroundSmoothness", 0, 100, 1);
    d->lfeOn = new QCheckBox("Enable &LFE", d->controls);
    d->lfeOn->setObjectName("surroundLfe");
    g->addWidget(d->lfeOn, 2, 0);
    d->lfe.slider = new QSlider(Qt::Horizontal, d->controls);
    d->lfe.slider->setObjectName("surroundLfeLevel");
    d->lfe.slider->setRange(0, 1000);
    g->addWidget(d->lfe.slider, 2, 1);
    d->lfe.value = new QLabel(d->controls);
    g->addWidget(d->lfe.value, 2, 2);
    g->addWidget(new QLabel("Motion:", d->controls), 3, 0);
    d->motion = new QComboBox(d->controls);
    d->motion->setObjectName("surroundMotion");
    d->motion->addItem("Move Freely (X or Y)", QString("free"));
    d->motion->addItem("Move left/right only (X)", QString("x"));
    d->motion->addItem("Move front/back only (Y)", QString("y"));
    g->addWidget(d->motion, 3, 1, 1, 2);
    QHBoxLayout* keys = new QHBoxLayout();
    d->addKey = new QPushButton("Add Keyframe", d->controls);
    d->addKey->setObjectName("surroundAddKey");
    d->addKey->setToolTip("A Surround Pan keyframe at the cursor, where the point is now (moving the point then changes the keyframe at the cursor)");
    d->delKey = new QPushButton("Delete Keyframe", d->controls);
    d->delKey->setObjectName("surroundDeleteKey");
    d->clearKeys = new QPushButton("Clear", d->controls);
    d->clearKeys->setToolTip("Delete every keyframe (the point stays where it is at the cursor)");
    keys->addWidget(d->addKey);
    keys->addWidget(d->delKey);
    keys->addWidget(d->clearKeys);
    g->addLayout(keys, 4, 0, 1, 3);
    v->addWidget(d->controls);

    QObject::connect(d->target, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        d->pinned = d->target->currentData().toULongLong();
        d->refresh();
    });
    QObject::connect(d->route, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const std::string r = ss(d->route->currentData().toString());
        d->change("Surround Pan", [&](SurroundPan& sp) { sp.route = r; });
    });
    QObject::connect(d->motion, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const std::string m = ss(d->motion->currentData().toString());
        d->change("Surround Pan Motion", [&](SurroundPan& sp) { sp.motion = m; });
    });
    auto dragSlider = [this](QSlider* s, const char* undo, std::function<void(SurroundPan&, int)> fn) {
        QObject::connect(s, &QSlider::sliderPressed, this, [] { AppState::get().beginDrag(); });
        QObject::connect(s, &QSlider::sliderReleased, this, [undo] { AppState::get().endDrag(undo); });
        QObject::connect(s, &QSlider::valueChanged, this, [this, s, undo, fn](int v) {
            const uint64_t id = d->shown();
            auto apply = [&](Project& p) {
                if (SurroundPan* sp = surroundPanOf(p, id)) fn(*sp, v);
            };
            if (s->isSliderDown()) AppState::get().dragUpdate(apply);
            else AppState::get().edit(undo, apply);
        });
    };
    dragSlider(d->center.slider, "Surround Pan Center", [](SurroundPan& sp, int v) {
        const double c = sliderToGain(v, 0.0);
        if (sp.keys.empty()) sp.center = c;
        else {
            const double t = AppState::get().cursor();
            double x, y, cc;
            surroundPointAt(sp, t, x, y, cc);
            const std::string m = sp.motion;
            sp.motion = "free";
            placePoint(sp, x, y, t, 1.0 / std::max(1.0, AppState::get().project().settings.fps()));
            sp.motion = m;
            for (auto& k : sp.keys)
                if (std::fabs(k.time - t) <= 0.5 / std::max(1.0, AppState::get().project().settings.fps())) k.center = c;
        }
    });
    dragSlider(d->smooth.slider, "Surround Pan Smoothness", [](SurroundPan& sp, int v) { sp.smoothness = v / 100.0; });
    dragSlider(d->lfe.slider, "Surround Pan LFE", [](SurroundPan& sp, int v) { sp.lfeLevel = sliderToGain(v, 12.0); });
    QObject::connect(d->lfeOn, &QCheckBox::toggled, this, [this](bool on) { d->change("Enable LFE", [&](SurroundPan& sp) { sp.lfe = on; }); });
    QObject::connect(d->addKey, &QPushButton::clicked, this, [this] {
        const double t = AppState::get().cursor();
        const double frame = 1.0 / std::max(1.0, AppState::get().project().settings.fps());
        d->change("Surround Pan Keyframe", [&](SurroundPan& sp) {
            if (sp.keys.empty()) {
                SurroundKey k;
                k.time = t;
                k.x = sp.x;
                k.y = sp.y;
                k.center = sp.center;
                sp.keys.push_back(k);
                return;
            }
            double x, y, c;
            surroundPointAt(sp, t, x, y, c);
            const std::string m = sp.motion;
            sp.motion = "free";
            placePoint(sp, x, y, t, frame);
            sp.motion = m;
        });
    });
    QObject::connect(d->delKey, &QPushButton::clicked, this, [this] {
        const double t = AppState::get().cursor();
        d->change("Delete Surround Pan Keyframe", [&](SurroundPan& sp) {
            if (sp.keys.empty()) return;
            size_t best = 0;
            for (size_t i = 1; i < sp.keys.size(); ++i)
                if (std::fabs(sp.keys[i].time - t) < std::fabs(sp.keys[best].time - t)) best = i;
            if (sp.keys.size() == 1) {
                sp.x = sp.keys[0].x;
                sp.y = sp.keys[0].y;
                sp.center = sp.keys[0].center;
            }
            sp.keys.erase(sp.keys.begin() + (std::ptrdiff_t)best);
        });
    });
    QObject::connect(d->clearKeys, &QPushButton::clicked, this, [this] {
        const double t = AppState::get().cursor();
        d->change("Clear Surround Pan Keyframes", [&](SurroundPan& sp) {
            surroundPointAt(sp, t, sp.x, sp.y, sp.center);
            sp.keys.clear();
        });
    });
    d->listener.reset(new Listener([this](int changes) {
        if (!(changes & (kProject | kSelection | kCursor | kFile))) return;
        if (isVisible()) d->refresh();
        else d->stale = true;
    }));
    d->refresh();
}

SurroundPannerWindow::~SurroundPannerWindow() = default;

void SurroundPannerWindow::showPanner(uint64_t id)
{
    d->pinned = id;
    d->stale = false;
    d->refresh();
}
uint64_t SurroundPannerWindow::shownId() const { return d->shown(); }
SurroundField* SurroundPannerWindow::field() const { return d->field; }
bool SurroundPannerWindow::handleKey(int key, Qt::KeyboardModifiers mods) { return d->field->handleKey(key, mods); }

void SurroundPannerWindow::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (d->stale) {
        d->stale = false;
        d->refresh();
    }
}

void SurroundPannerWindow::keyPressEvent(QKeyEvent* e)
{
    if (handleKey(e->key(), e->modifiers())) return;
    QWidget::keyPressEvent(e);
}

}  // namespace app
}  // namespace reno
