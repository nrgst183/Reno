// VEGAS's Surround Panner: see surroundpanner.h.
#include "surroundpanner.h"
#include "toolwin.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QLabel>
#include <QLocale>
#include <QToolButton>
#include <QWidgetAction>
#include <QMouseEvent>
#include <QPainter>
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
    for (auto& b : p.inputBuses)
        if (b.id == id) return &b.surround;
    return nullptr;
}
const SurroundPan* surroundPanOf(const Project& p, uint64_t id) { return surroundPanOf(const_cast<Project&>(p), id); }

std::string surroundPanTypeOf(const Project& p, uint64_t id)
{
    for (const auto& t : p.tracks)
        if (t.id == id) return t.panType;
    for (const auto& b : p.buses)
        if (b.id == id) return b.panType;
    for (const auto& b : p.inputBuses)
        if (b.id == id) return b.panType;
    return std::string();
}

void setSurroundPanType(Project& p, uint64_t id, const std::string& type)
{
    const std::string v = type == "balance0" ? std::string() : type;
    for (auto& t : p.tracks)
        if (t.id == id) t.panType = v;
    for (auto& b : p.buses)
        if (b.id == id) b.panType = v;
    for (auto& b : p.inputBuses)
        if (b.id == id) b.panType = v;
}

const std::vector<std::pair<std::string, std::string>>& surroundPanTypes()
{
    static const std::vector<std::pair<std::string, std::string>> types = {
        {"add", "Add Channels"},          {"balance0", "Balance (0 dB center)"}, {"balance3", "Balance (-3 dB center)"},
        {"balance6", "Balance (-6 dB center)"}, {"power", "Constant Power"},     {"film", "Film"}};
    return types;
}

namespace {

const double kPi = 3.14159265358979323846;

// Where the speakers stand in the room (x, y): L, R, C, LFE (drawn at the rear middle; 5.1 has no speaker there), Ls, Rs.
const double kSpeakerX[6] = {-1, 1, 0, 0, -1, 1};
const double kSpeakerY[6] = {1, 1, 1, -1, -1, -1};

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
    // the window's room fills the field, the speakers standing in its corners and at its front middle (as VEGAS's)
    const double m = compact_ ? 4.0 : 10.0;
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
    if (!compact_) {
        // the window draws the five speakers (not the LFE, which has its own button) in the room's corners and at its
        // front middle, facing in
        if (ch == ChLFE) return QRectF();
        const QPointF c = toWidget(kSpeakerX[ch], kSpeakerY[ch]);
        const double s = 18.0, in = kSpeakerX[ch] == 0 ? 8.0 : 6.0;
        const QPointF d(kSpeakerX[ch] < 0 ? in : kSpeakerX[ch] > 0 ? -in : 0, kSpeakerY[ch] > 0 ? in : -in);
        return QRectF(c.x() + d.x() - s / 2, c.y() + d.y() - s / 2, s, s);
    }
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
    const SurroundPan* sp = get ? get() : nullptr;
    if (compact_) paintCompact(p, sp);
    else paintRoom(p, sp);
}

namespace {

// The level a speaker gets, as VEGAS writes it over the speaker: "-Inf." or dB with one decimal (in the system's way of
// writing numbers, as VEGAS does: "-7,4" with a decimal comma).
QString levelText(float g)
{
    if (g <= 1e-5f) return QString("-Inf.");
    double db = 20.0 * std::log10((double)g);
    if (std::fabs(db) < 0.05) db = 0.0;
    return QLocale().toString(db, 'f', 1);
}

// VEGAS's speaker: a small box and its cone, drawn facing down and turned to face 'to'.
void drawSpeaker(QPainter& p, const QRectF& r, const QPointF& to, bool muted)
{
    const QPointF c = r.center();
    const QPointF d = to - c;
    const double angle = std::atan2(-d.x(), d.y()) * 180.0 / kPi;
    p.save();
    p.translate(c);
    p.rotate(angle);
    const double s = r.width() / 18.0;
    p.scale(s, s);
    p.setPen(Qt::NoPen);
    p.setBrush(muted ? QColor(120, 120, 120) : QColor(240, 240, 240));
    p.drawRect(QRectF(-3.5, -7, 7, 4));
    const QPointF cone[4] = {QPointF(-3.5, -3), QPointF(3.5, -3), QPointF(7.5, 3.5), QPointF(-7.5, 3.5)};
    p.drawPolygon(cone, 4);
    p.restore();
    if (muted) {
        p.setPen(QPen(QColor(230, 70, 60), 2.0));
        p.drawLine(r.topLeft() + QPointF(3, 3), r.bottomRight() - QPointF(3, 3));
        p.drawLine(r.topRight() + QPointF(-3, 3), r.bottomLeft() + QPointF(3, -3));
    }
}

}  // namespace

// The window's room, as VEGAS 18 draws it: dark, the grid in dashed lines, a grey disc at each speaker with the level
// it gets written in it, the pan point, the keyframes' path in small circles, and the pan type's name at the bottom.
void SurroundField::paintRoom(QPainter& p, const SurroundPan* sp)
{
    p.fillRect(rect(), QColor(38, 38, 38));
    const QRectF r = room();
    if (!sp) {
        p.setPen(text());
        p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, "No Panner Selected");
        return;
    }
    double x, y, c;
    surroundPointAt(*sp, AppState::get().cursor(), x, y, c);
    float g[6] = {0, 0, 0, 0, 0, 0};
    const bool panned = sp->route == "surround";
    if (panned) surroundGains(*sp, panType ? panType() : std::string(), x, y, c, g);
    else {
        if (sp->route == "front") g[ChL] = g[ChR] = 1;
        else if (sp->route == "rear") g[ChLs] = g[ChRs] = 1;
        else if (sp->route == "center") g[ChC] = 1;
        else if (sp->route == "lfe") g[ChLFE] = 1;
    }
    for (int ch = 0; ch < 6; ++ch)
        if ((sp->speakerMutes >> ch) & 1u) g[ch] = 0;
    // the discs: a lighter ring outside, the darker middle, and an arc that grows with the level
    const double R = r.width() * 0.25;
    QStringList levels;     // L, R, C, Ls, Rs as written (for tests)
    p.save();
    p.setClipRect(r);
    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    for (int ch = 0; ch < 6; ++ch) {
        if (ch == ChLFE) continue;
        const QPointF o = toWidget(kSpeakerX[ch], kSpeakerY[ch]);
        const double rad = ch == ChC ? R * 0.8 : R;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(92, 92, 92));
        p.drawEllipse(o, rad, rad);
        p.setBrush(QColor(68, 68, 68));
        p.drawEllipse(o, rad * 0.64, rad * 0.64);
        const double share = std::min(1.0, std::max(0.0, (double)g[ch]));
        if (share > 0.001) {
            p.setPen(QPen(QColor(200, 200, 200), 1.0));
            p.setBrush(Qt::NoBrush);
            const double a = rad * (0.64 + 0.36 * share);
            p.drawEllipse(o, a, a);
        }
        // the level, inside the disc toward the room's middle
        const QPointF in(kSpeakerX[ch] == 0 ? 0 : (kSpeakerX[ch] < 0 ? 1 : -1), kSpeakerY[ch] > 0 ? 1 : -1);
        const QPointF at = ch == ChC ? o + QPointF(0, rad * 0.45) : o + QPointF(in.x() * rad * 0.4, in.y() * rad * 0.36);
        p.setPen(QColor(240, 240, 240));
        p.drawText(QRectF(at.x() - 24, at.y() - 7, 48, 14), Qt::AlignCenter, levelText(g[ch]));
        levels << levelText(g[ch]);
    }
    // the grid: where left and right (and front and rear) differ by 6 dB, 0 dB and -6 dB
    p.setPen(QPen(QColor(110, 110, 110), 1, Qt::DashLine));
    for (double gx : {-2.0 / 3.0, 0.0, 2.0 / 3.0}) p.drawLine(toWidget(gx, 1), toWidget(gx, -1));
    for (double gy : {2.0 / 3.0, 0.2, -1.0 / 3.0}) p.drawLine(toWidget(-1, gy), toWidget(1, gy));
    // the keyframes' path
    if (sp->keys.size() > 1) {
        p.setPen(QPen(QColor(240, 140, 40), 1.2));
        p.setBrush(Qt::NoBrush);
        const double span = sp->keys.back().time - sp->keys.front().time;
        const int n = 40;
        for (int i = 0; i <= n; ++i) {
            double kx, ky, kc;
            surroundPointAt(*sp, sp->keys.front().time + span * i / n, kx, ky, kc);
            p.drawEllipse(toWidget(kx, ky), 2.2, 2.2);
        }
    }
    p.restore();
    for (int ch = 0; ch < 6; ++ch) {
        if (ch == ChLFE) continue;
        drawSpeaker(p, speakerRect(ch), r.center(), (sp->speakerMutes >> ch) & 1u);
    }
    const QPointF pt = toWidget(x, y);
    if (panned) {
        // the pan point: a red dot in a ring of small white dots
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255));
        for (int k = 0; k < 10; ++k) {
            const double a = k * kPi / 5.0;
            p.drawEllipse(pt + QPointF(6.5 * std::cos(a), 6.5 * std::sin(a)), 1.2, 1.2);
        }
        p.setBrush(hasFocus() && selected_ ? QColor(240, 60, 50) : QColor(200, 50, 45));
        p.setPen(QPen(QColor(255, 255, 255), 1.0));
        p.drawEllipse(pt, 3.6, 3.6);
    }
    // the pan type (or where the sound goes, when it is not panned) at the bottom
    QString name;
    if (panned) {
        const std::string type = panType ? panType() : std::string();
        const std::string key = type.empty() ? std::string("balance0") : type;
        for (const auto& t : surroundPanTypes())
            if (t.first == key) name = qs(t.second);
    } else {
        name = sp->route == "front" ? "Front Only" : sp->route == "rear" ? "Rear Only" : sp->route == "center" ? "Center Only" : "LFE Only";
    }
    QFont nf = font();
    nf.setPixelSize(11);
    p.setFont(nf);
    p.setPen(QColor(240, 240, 240));
    p.drawText(QRectF(r.left(), r.bottom() - 20, r.width(), 16), Qt::AlignCenter, name);
    setProperty("pointX", x);
    setProperty("pointY", y);
    setProperty("panTypeName", name);
    setProperty("levels", levels);
}

// The small panner of track headers and mixer strips.
void SurroundField::paintCompact(QPainter& p, const SurroundPan* sp)
{
    p.fillRect(rect(), QColor(30, 30, 30));
    const QRectF r = room();
    p.setPen(QPen(QColor(90, 90, 90), 1));
    p.setBrush(QColor(22, 22, 24));
    p.drawRect(r);
    if (!sp) return;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(60, 60, 64), 1));
    p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    double x, y, c;
    surroundPointAt(*sp, AppState::get().cursor(), x, y, c);
    float g[6] = {0, 0, 0, 0, 0, 0};
    const bool panned = sp->route == "surround";
    if (panned) surroundGains(*sp, panType ? panType() : std::string(), x, y, c, g);
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
        p.setPen(QPen(muted ? QColor(120, 60, 60) : QColor(150, 150, 150), 0.8));
        p.setBrush(muted ? QColor(50, 30, 30) : (share > 0.001 ? QColor(accent().red(), accent().green(), accent().blue(), 60 + (int)(190 * share)) : QColor(45, 45, 48)));
        if (ch == ChLFE) p.drawEllipse(s.adjusted(1, 1, -1, -1));
        else p.drawRoundedRect(s, 2, 2);
        if (muted) {
            p.setPen(QPen(QColor(230, 70, 60), 1.0));
            p.drawLine(s.topLeft(), s.bottomRight());
            p.drawLine(s.topRight(), s.bottomLeft());
        }
    }
    if (panned) {
        p.setPen(QPen(QColor(255, 255, 255), 1.0));
        p.setBrush(QColor(240, 120, 30));
        p.drawEllipse(toWidget(x, y), 3.0, 3.0);
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
            if (!speakerRect(ch).isNull() && speakerRect(ch).adjusted(-2, -2, 2, 2).contains(pos)) {
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

void SurroundField::contextMenuEvent(QContextMenuEvent* e)
{
    if (!get || !get() || !panType || !setPanType) return;
    QMenu menu(this);
    QMenu* types = menu.addMenu("Pan Type");
    const std::string now = panType().empty() ? std::string("balance0") : panType();
    for (const auto& t : surroundPanTypes()) {
        QAction* a = types->addAction(qs(t.second));
        a->setCheckable(true);
        a->setChecked(t.first == now);
        const std::string key = t.first;
        QObject::connect(a, &QAction::triggered, this, [this, key] { setPanType(key); });
    }
    menu.exec(e->globalPos());
}

namespace {

// The Center fader goes from -60 dB (and -Inf at its bottom) to +12 dB, with 0.0 dB (no gain) in between.
const double kCenterTopDb = 12.0;

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
    if (std::fabs(db) < 0.04) return 1.0;      // the slider's step next to 0.0 dB is 0.0 dB
    return std::pow(10.0, db / 20.0);
}

// The tool bar's pictures, drawn here in VEGAS's manner: the track (its number and a speaker), the Move tool (the
// arrows of the motion allowed), and LFE.
QIcon trackIcon(const QString& label)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(26, 18) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    p.drawText(QRectF(0, 0, 11, 12), Qt::AlignLeft | Qt::AlignTop, label);
    p.fillRect(QRectF(10, 4, 16, 14), QColor(255, 255, 255));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(40, 40, 40));
    const QPointF cone[4] = {QPointF(16, 9), QPointF(16, 13), QPointF(21, 16), QPointF(21, 6)};
    p.drawRect(QRectF(13, 9, 3, 4));
    p.drawPolygon(cone, 4);
    return QIcon(pm);
}

QIcon moveIcon(const std::string& motion)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(16, 16) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink(240, 240, 240);
    p.setPen(QPen(ink, 1.6));
    p.setBrush(ink);
    const bool across = motion != "y", along = motion != "x";
    if (across) {
        p.drawLine(QPointF(2.5, 8), QPointF(13.5, 8));
        const QPointF l[3] = {QPointF(1, 8), QPointF(4.5, 5), QPointF(4.5, 11)};
        const QPointF r[3] = {QPointF(15, 8), QPointF(11.5, 5), QPointF(11.5, 11)};
        p.drawPolygon(l, 3);
        p.drawPolygon(r, 3);
    }
    if (along) {
        p.drawLine(QPointF(8, 2.5), QPointF(8, 13.5));
        const QPointF u[3] = {QPointF(8, 1), QPointF(5, 4.5), QPointF(11, 4.5)};
        const QPointF d[3] = {QPointF(8, 15), QPointF(5, 11.5), QPointF(11, 11.5)};
        p.drawPolygon(u, 3);
        p.drawPolygon(d, 3);
    }
    return QIcon(pm);
}

QIcon lfeIcon()
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(20, 16) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(240, 240, 240));
    p.drawRoundedRect(QRectF(0.5, 3.5, 19, 9), 1.5, 1.5);
    QFont f = p.font();
    f.setPixelSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(30, 30, 30));
    p.drawText(QRectF(0, 3, 20, 10), Qt::AlignCenter, "LFE");
    return QIcon(pm);
}

// VEGAS's fader rows under the room: the name in a sunken box, the fader, the value in a box.
QLabel* boxedLabel(const QString& text, QWidget* parent)
{
    QLabel* l = new QLabel(text, parent);
    l->setStyleSheet("QLabel { background: #2b2b2b; border: 1px solid #1e1e1e; border-right-color: #5a5a5a; border-bottom-color: #5a5a5a;"
                     " padding: 0px 3px; color: #e6e6e6; }");
    return l;
}

}  // namespace

struct SurroundPannerWindow::Impl {
    SurroundPannerWindow* w = nullptr;
    uint64_t pinned = 0;
    bool stale = false;
    // The tool bar: whose panner (its menu lists the others), the Move tool and LFE.
    QToolButton* targetBtn = nullptr;
    QToolButton* moveBtn = nullptr;
    QToolButton* lfeBtn = nullptr;
    QMenu* moveMenu = nullptr;
    // What the menus set is kept in these (not shown: VEGAS 18 has no such boxes), so a setting changes the same way
    // from a menu, a key or a script.
    QComboBox* target = nullptr;
    QComboBox* route = nullptr;
    QComboBox* panTypeBox = nullptr;
    QComboBox* motion = nullptr;
    QCheckBox* lfeOn = nullptr;
    QPushButton* addKey = nullptr;
    QPushButton* delKey = nullptr;
    QPushButton* clearKeys = nullptr;
    QLabel* message = nullptr;
    SurroundField* field = nullptr;
    LabeledSlider center, smooth, lfe;
    QWidget* lfeRow = nullptr;          // the LFE send's level, on the Move tool's menu
    QWidgetAction* lfeAction = nullptr; // (the window's, so clearing the menu keeps it)
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

    // The track's number (or the bus's letter) as the tool bar's first button shows it.
    QString shownLabel() const
    {
        const Project& p = AppState::get().project();
        const uint64_t id = shown();
        for (size_t i = 0; i < p.tracks.size(); ++i)
            if (p.tracks[i].id == id) return QString::number(i + 1);
        for (size_t i = 0; i < p.buses.size(); ++i)
            if (p.buses[i].id == id) return QString(QChar('A' + (int)(i % 26)));
        return QString();
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
                ++n;
                if (t.type != TrackType::Audio) continue;
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
                                   : "No Panner Selected\n\nClick an audio track (or pick a track or bus on the first button's menu)");
        field->setVisible(surround && sp);
        controls->setEnabled(surround && sp);
        for (QToolButton* b : {moveBtn, lfeBtn}) b->setEnabled(surround && sp);
        targetBtn->setIcon(trackIcon(sp ? shownLabel() : QString()));
        const int at = target->findData(QVariant::fromValue<qulonglong>(shown()));
        targetBtn->setToolTip(sp && at >= 0 ? QString("The panner of %1 (click to show another track's or a bus's)").arg(target->itemText(at))
                                            : QString("Pick the track or bus whose panner is shown"));
        if (sp) {
            const QSignalBlocker b1(route), b2(motion), b3(center.slider), b4(smooth.slider), b5(lfe.slider), b6(lfeOn), b7(panTypeBox), b8(lfeBtn);
            {
                const std::string type = surroundPanTypeOf(p, shown());
                panTypeBox->setCurrentIndex(std::max(0, panTypeBox->findData(qs(type.empty() ? std::string("balance0") : type))));
            }
            route->setCurrentIndex(std::max(0, route->findData(qs(sp->route))));
            motion->setCurrentIndex(std::max(0, motion->findData(qs(sp->motion))));
            moveBtn->setIcon(moveIcon(sp->motion));
            moveBtn->setProperty("motion", qs(sp->motion));
            lfeBtn->setChecked(sp->route == "lfe");
            double x, y, c;
            surroundPointAt(*sp, AppState::get().cursor(), x, y, c);
            center.slider->setValue(gainToSlider(c, kCenterTopDb));
            center.value->setText(dbText(c));
            double smoothNow = sp->smoothness;
            for (const auto& k : sp->keys)
                if (std::fabs(k.time - AppState::get().cursor()) <= 0.5 / std::max(1.0, p.settings.fps()) && k.smoothness >= 0) smoothNow = k.smoothness;
            smooth.slider->setValue((int)std::lround(smoothNow * 100));
            smooth.value->setText(QString("%1 %").arg((int)std::lround(smoothNow * 100)));
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

    // The settings the menus work through (not shown: see above).
    void buildState()
    {
        target = new QComboBox(w);
        target->setObjectName("surroundTarget");
        route = new QComboBox(w);
        route->setObjectName("surroundRoute");
        for (auto r : {std::make_pair("Surround Pan", "surround"), std::make_pair("Front Only", "front"), std::make_pair("Center Only", "center"),
                       std::make_pair("LFE Only", "lfe"), std::make_pair("Rear Only", "rear")})
            route->addItem(r.first, QString(r.second));
        panTypeBox = new QComboBox(w);
        panTypeBox->setObjectName("surroundPanType");
        for (const auto& t : surroundPanTypes()) panTypeBox->addItem(qs(t.second), qs(t.first));
        motion = new QComboBox(w);
        motion->setObjectName("surroundMotion");
        motion->addItem("Move Freely", QString("free"));
        motion->addItem("Move Left/Right Only", QString("x"));
        motion->addItem("Move Front/Back Only", QString("y"));
        lfeOn = new QCheckBox("Enable &LFE", w);
        lfeOn->setObjectName("surroundLfe");
        addKey = new QPushButton("Add Keyframe", w);
        addKey->setObjectName("surroundAddKey");
        delKey = new QPushButton("Delete Keyframe", w);
        delKey->setObjectName("surroundDeleteKey");
        clearKeys = new QPushButton("Clear Keyframes", w);
        clearKeys->setObjectName("surroundClearKeys");
        for (QWidget* x : std::initializer_list<QWidget*>{target, route, panTypeBox, motion, lfeOn, addKey, delKey, clearKeys}) x->hide();
    }

    // VEGAS 18's tool bar: the track, the Move tool (with its menu) and LFE.
    void buildToolbar(QVBoxLayout* v)
    {
        QWidget* bar = new QWidget(w);
        bar->setObjectName("surroundToolbar");
        bar->setAutoFillBackground(true);
        QPalette bp = bar->palette();
        bp.setColor(QPalette::Window, QColor(52, 52, 52));
        bar->setPalette(bp);
        // (the Move tool keeps the style's own drawing, for its arrow)
        bar->setStyleSheet("QToolButton#surroundTargetButton, QToolButton#surroundLfeButton { background: transparent; border: 1px solid transparent;"
                           " border-radius: 2px; padding: 1px; }"
                           "QToolButton#surroundTargetButton:hover, QToolButton#surroundLfeButton:hover { background: rgba(255, 255, 255, 30); }"
                           "QToolButton#surroundLfeButton:checked { background: #12b0ff; border-color: #12b0ff; }"
                           "QToolButton#surroundTargetButton::menu-indicator { image: none; width: 0px; }");
        QHBoxLayout* h = new QHBoxLayout(bar);
        h->setContentsMargins(4, 2, 4, 2);
        h->setSpacing(6);
        targetBtn = new QToolButton(bar);
        targetBtn->setObjectName("surroundTargetButton");
        targetBtn->setIconSize(QSize(26, 18));
        targetBtn->setPopupMode(QToolButton::InstantPopup);
        QMenu* targets = new QMenu(targetBtn);
        targets->setObjectName("surroundTargetMenu");
        targetBtn->setMenu(targets);
        QObject::connect(targets, &QMenu::aboutToShow, w, [this, targets] {
            targets->clear();
            for (int i = 0; i < target->count(); ++i) {
                QAction* a = targets->addAction(target->itemText(i));
                a->setCheckable(true);
                a->setChecked(i == target->currentIndex());
                QObject::connect(a, &QAction::triggered, w, [this, i] { target->setCurrentIndex(i); });
            }
        });
        moveBtn = new QToolButton(bar);
        moveBtn->setObjectName("surroundMoveTool");
        moveBtn->setIconSize(QSize(16, 16));
        moveBtn->setPopupMode(QToolButton::MenuButtonPopup);
        moveBtn->setAutoRaise(true);
        moveBtn->setToolTip("Move Freely, Move Left/Right Only, Move Front/Back Only (a click goes to the next); the arrow: the pan type, "
                            "where the sound goes, the LFE send and the keyframes");
        moveMenu = new QMenu(moveBtn);
        moveMenu->setObjectName("surroundMoveMenu");
        moveBtn->setMenu(moveMenu);
        QObject::connect(moveMenu, &QMenu::aboutToShow, w, [this] { fillMoveMenu(moveMenu); });
        lfeBtn = new QToolButton(bar);
        lfeBtn->setObjectName("surroundLfeButton");
        lfeBtn->setCheckable(true);
        lfeBtn->setIcon(lfeIcon());
        lfeBtn->setIconSize(QSize(20, 16));
        lfeBtn->setToolTip("LFE: send this sound to the LFE channel only (click again for surround panning)");
        h->addWidget(targetBtn);
        h->addWidget(moveBtn);
        h->addWidget(lfeBtn);
        h->addStretch(1);
        v->addWidget(bar);
    }

    // The Move tool's menu: the motion, then what VEGAS 18 keeps elsewhere (the pan type, on the room's shortcut menu;
    // where the sound goes and the LFE send, on the track's panner; the keyframes, on the timeline).
    void fillMoveMenu(QMenu* m)
    {
        m->clear();
        auto radio = [&](QMenu* in, QComboBox* box, int i) {
            QAction* a = in->addAction(box->itemText(i));
            a->setCheckable(true);
            a->setChecked(box->currentIndex() == i);
            QObject::connect(a, &QAction::triggered, w, [box, i] { box->setCurrentIndex(i); });
            return a;
        };
        for (int i = 0; i < motion->count(); ++i) radio(m, motion, i);
        m->addSeparator();
        QMenu* types = m->addMenu("Pan Type");
        for (int i = 0; i < panTypeBox->count(); ++i) radio(types, panTypeBox, i);
        QMenu* routes = m->addMenu("Route");
        for (int i = 0; i < route->count(); ++i) radio(routes, route, i);
        m->addSeparator();
        QAction* send = m->addAction("Enable LFE");
        send->setCheckable(true);
        send->setChecked(lfeOn->isChecked());
        QObject::connect(send, &QAction::triggered, w, [this](bool on) { lfeOn->setChecked(on); });
        lfeRow->setEnabled(lfeOn->isChecked());
        m->addAction(lfeAction);
        m->addSeparator();
        QObject::connect(m->addAction("Add Keyframe"), &QAction::triggered, addKey, &QPushButton::click);
        QAction* del = m->addAction("Delete Keyframe");
        del->setEnabled(delKey->isEnabled());
        QObject::connect(del, &QAction::triggered, delKey, &QPushButton::click);
        QAction* clear = m->addAction("Clear Keyframes");
        clear->setEnabled(clearKeys->isEnabled());
        QObject::connect(clear, &QAction::triggered, clearKeys, &QPushButton::click);
    }

    // The panner itself (or a message saying why there is none).
    void buildField(QVBoxLayout* v)
    {
        message = new QLabel(w);
        message->setAlignment(Qt::AlignCenter);
        message->setWordWrap(true);
        message->setObjectName("surroundMessage");
        v->addWidget(message, 1);
        field = new SurroundField(false, w);
        field->get = [this]() { return pan(); };
        field->panType = [this]() { return surroundPanTypeOf(AppState::get().project(), shown()); };
        field->setPanType = [this](const std::string& type) {
            const uint64_t id = shown();
            AppState::get().edit("Pan Type", [&](Project& p) { setSurroundPanType(p, id, type); });
        };
        field->set = [this](Project& p, const SurroundPan& sp) {
            if (SurroundPan* to = surroundPanOf(p, shown())) *to = sp;
        };
        field->setToolTip("Drag the point (Shift: along the line through the middle, Alt: around its circle, Alt+Shift: around the largest circle, "
                             "Ctrl: finer); arrows, Page Up/Down, the wheel and the keypad move it; click a speaker to mute it");
        v->addWidget(field, 4);
    }

    // Under the panner: VEGAS's Center and Smoothness faders. (The LFE send's level is on the Move tool's menu.)
    void buildControls(QVBoxLayout* v)
    {
        controls = new QWidget(w);
        QGridLayout* g = new QGridLayout(controls);
        g->setContentsMargins(4, 2, 4, 4);
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(4);
        auto slider = [&](LabeledSlider& s, const char* label, const char* name, int lo, int hi, int row) {
            g->addWidget(boxedLabel(label, controls), row, 0);
            s.slider = new QSlider(Qt::Horizontal, controls);
            s.slider->setObjectName(name);
            s.slider->setRange(lo, hi);
            g->addWidget(s.slider, row, 1);
            s.value = boxedLabel(QString(), controls);
            s.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            s.value->setMinimumWidth(52);
            g->addWidget(s.value, row, 2);
        };
        slider(center, "Center:", "surroundCenter", 0, 1000, 0);   // 0.0 dB (no gain) is where the fader sits by default
        slider(smooth, "Smoothness:", "surroundSmoothness", 0, 100, 1);
        g->setColumnStretch(1, 1);
        v->addWidget(controls);
        // the LFE send's level, shown on the Move tool's menu
        lfeRow = new QWidget();
        lfeRow->setObjectName("surroundLfeRow");
        QHBoxLayout* lh = new QHBoxLayout(lfeRow);
        lh->setContentsMargins(24, 2, 8, 2);
        lh->addWidget(new QLabel("LFE level:", lfeRow));
        lfe.slider = new QSlider(Qt::Horizontal, lfeRow);
        lfe.slider->setObjectName("surroundLfeLevel");
        lfe.slider->setRange(0, 1000);
        lfe.slider->setMinimumWidth(110);
        lh->addWidget(lfe.slider, 1);
        lfe.value = new QLabel(lfeRow);
        lfe.value->setMinimumWidth(52);
        lh->addWidget(lfe.value);
        lfeAction = new QWidgetAction(w);
        lfeAction->setDefaultWidget(lfeRow);
    }

    // What the row's and the controls' settings change.
    void connectControls()
    {
        QObject::connect(target, QOverload<int>::of(&QComboBox::currentIndexChanged), w, [this](int) {
            pinned = target->currentData().toULongLong();
            refresh();
        });
        QObject::connect(route, QOverload<int>::of(&QComboBox::currentIndexChanged), w, [this](int) {
            const std::string r = ss(route->currentData().toString());
            change("Surround Pan", [&](SurroundPan& sp) { sp.route = r; });
        });
        QObject::connect(panTypeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), w, [this](int) {
            const std::string type = ss(panTypeBox->currentData().toString());
            const uint64_t id = shown();
            if (!id) return;
            AppState::get().edit("Pan Type", [&](Project& p) { setSurroundPanType(p, id, type); });
        });
        QObject::connect(motion, QOverload<int>::of(&QComboBox::currentIndexChanged), w, [this](int) {
            const std::string m = ss(motion->currentData().toString());
            change("Surround Pan Motion", [&](SurroundPan& sp) { sp.motion = m; });
        });
        auto dragSlider = [this](QSlider* s, const char* undo, std::function<void(SurroundPan&, int)> fn) {
            QObject::connect(s, &QSlider::sliderPressed, w, [] { AppState::get().beginDrag(); });
            QObject::connect(s, &QSlider::sliderReleased, w, [undo] { AppState::get().endDrag(undo); });
            QObject::connect(s, &QSlider::valueChanged, w, [this, s, undo, fn](int v) {
                const uint64_t id = shown();
                auto apply = [&](Project& p) {
                    if (SurroundPan* sp = surroundPanOf(p, id)) fn(*sp, v);
                };
                if (s->isSliderDown()) AppState::get().dragUpdate(apply);
                else AppState::get().edit(undo, apply);
            });
        };
        dragSlider(center.slider, "Surround Pan Center", [](SurroundPan& sp, int v) {
            const double c = sliderToGain(v, kCenterTopDb);
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
        dragSlider(smooth.slider, "Surround Pan Smoothness", [](SurroundPan& sp, int v) {
            // with keyframes the key at the cursor gets its own (the curve of the path to the next key), else the panner
            const double t = AppState::get().cursor();
            const double half = 0.5 / std::max(1.0, AppState::get().project().settings.fps());
            for (auto& k : sp.keys)
                if (std::fabs(k.time - t) <= half) {
                    k.smoothness = v / 100.0;
                    return;
                }
            sp.smoothness = v / 100.0;
        });
        dragSlider(lfe.slider, "Surround Pan LFE", [](SurroundPan& sp, int v) { sp.lfeLevel = sliderToGain(v, 12.0); });
        QObject::connect(lfeOn, &QCheckBox::toggled, w, [this](bool on) { change("Enable LFE", [&](SurroundPan& sp) { sp.lfe = on; }); });
        QObject::connect(addKey, &QPushButton::clicked, w, [this] { addKeyframeAtCursor(); });
        QObject::connect(delKey, &QPushButton::clicked, w, [this] { deleteNearestKeyframe(); });
        QObject::connect(clearKeys, &QPushButton::clicked, w, [this] { clearKeyframes(); });
        // the Move tool: a click goes on to the next motion (VEGAS: Move Freely, Left/Right Only, Front/Back Only)
        QObject::connect(moveBtn, &QToolButton::clicked, w, [this] {
            if (motion->count() > 0) motion->setCurrentIndex((motion->currentIndex() + 1) % motion->count());
        });
        QObject::connect(lfeBtn, &QToolButton::toggled, w, [this](bool on) {
            route->setCurrentIndex(std::max(0, route->findData(QString(on ? "lfe" : "surround"))));
        });
        QObject::connect(lfeOn, &QCheckBox::toggled, w, [this](bool on) { lfeRow->setEnabled(on); });
    }

    // Add Keyframe: a keyframe at the cursor where the point is now (the first one takes the panner's point).
    void addKeyframeAtCursor()
    {
        const double t = AppState::get().cursor();
        const double frame = 1.0 / std::max(1.0, AppState::get().project().settings.fps());
        change("Surround Pan Keyframe", [&](SurroundPan& sp) {
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
    }

    // Delete Keyframe: the keyframe nearest the cursor; the last one leaves its point as the panner's.
    void deleteNearestKeyframe()
    {
        const double t = AppState::get().cursor();
        change("Delete Surround Pan Keyframe", [&](SurroundPan& sp) {
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
    }

    // Clear: every keyframe goes, the point staying where it is at the cursor.
    void clearKeyframes()
    {
        const double t = AppState::get().cursor();
        change("Clear Surround Pan Keyframes", [&](SurroundPan& sp) {
            surroundPointAt(sp, t, sp.x, sp.y, sp.center);
            sp.keys.clear();
        });
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
    v->setContentsMargins(0, 0, 0, 2);
    v->setSpacing(2);

    d->buildState();
    d->buildToolbar(v);
    d->buildField(v);
    d->buildControls(v);
    d->connectControls();
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
