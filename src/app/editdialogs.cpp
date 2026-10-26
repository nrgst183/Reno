// The editing windows that work on one thing, laid out like VEGAS's: Event Pan/Crop (the media's image with the box of
// what shows, a side panel of numbers and a keyframe strip; non-modal, one per event), Track Motion (the frame with the
// box the track's image is put in, same panel and strip, plus the compositing mode; non-modal, one per track), and the
// modal Event Properties, Project Properties (Video, Audio, Ruler, Summary) and Preferences. Every change goes through
// AppState: a drag is one beginDrag / dragUpdate / endDrag (one undo step, the preview follows), anything else one
// edit().
#include "editdialogs.h"
#include "editdialogs_internal.h"
#include "vprefs.h"
#include "../engine/colormgmt.h"
#include "../engine/timefmt.h"
#include "../engine/stretch.h"
#include "playback.h"
#include "strconv.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <functional>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QShortcut>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStorageInfo>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#ifdef RENO_HAVE_QTMULTIMEDIA
#include <QAudioDevice>
#include <QMediaDevices>
#endif
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace reno {
namespace app {

const std::vector<CompositeMode>& compositeModes()
{
    static const std::vector<CompositeMode> modes = {
        {"srcalpha", "Source Alpha"}, {"3dsrcalpha", "3D Source Alpha"}, {"add", "Add"}, {"subtract", "Subtract"}, {"cut", "Cut"},
        {"multiply", "Multiply (Mask)"}, {"overlay", "Overlay"}, {"screen", "Screen"}, {"hardlight", "Hard Light"},
        {"dodge", "Dodge"}, {"burn", "Burn"}, {"darken", "Darken"}, {"lighten", "Lighten"},
        {"difference", "Difference"}, {"diffsquared", "Difference Squared"},
    };
    return modes;
}

namespace {

const double kPi = 3.14159265358979323846;


using namespace editdlg;

// ================================================================================== boxes and keyframes
// A box in a rectangle (Pan/Crop: in the media's image; Track Motion: in the frame): centre and size as fractions of the
// rectangle, turned clockwise by 'rot' degrees; a negative size is flipped.
struct Box {
    double time = 0;
    double cx = 0.5, cy = 0.5, w = 1.0, h = 1.0, rot = 0;
    int curve = 0;
    double smooth = 0;
    double px = 0, py = 0;
    double blur = 0.2, intensity = 0.5;
    uint32_t color = 0xFF000000;
    // Track Motion in 3D Source Alpha only: MotionKey's z, d, rotX, rotY, oriX/Y/Z, offX/Y/Z; 'rot' is its Z rotation
    double z = 0, d = 1.0, rx = 0, ry = 0, ox = 0, oy = 0, oz = 0, fx = 0, fy = 0, fz = 0;
    // Event Pan/Crop's Mask row only: which of the event's mask keys this one stands for; -1 a new one, with 'mask' its
    // shapes when it is a copy
    int src = -1;
    std::shared_ptr<const std::vector<MaskPath>> mask;
};

double ease(int curve, double s) { return keyCurveShape(curve, s); }

// The box at time t as the compositor works it out (keys sorted by time).
Box boxAt(const std::vector<Box>& keys, double t)
{
    if (keys.empty()) return Box();
    if (t <= keys.front().time) return keys.front();
    if (t >= keys.back().time) return keys.back();
    for (size_t k = 0; k + 1 < keys.size(); ++k) {
        const Box& a = keys[k];
        const Box& b = keys[k + 1];
        if (t < b.time) {
            const double span = b.time - a.time;
            const double f = span > 0 ? ease(a.curve, (t - a.time) / span) : 1.0;
            Box o;
            o.time = t;
            o.curve = a.curve;
            o.cx = a.cx + (b.cx - a.cx) * f;
            o.cy = a.cy + (b.cy - a.cy) * f;
            o.smooth = a.smooth;
            o.px = a.px + (b.px - a.px) * f;
            o.py = a.py + (b.py - a.py) * f;
            if (a.smooth > 0) {
                // VEGAS's Smoothness: the centre on a curve through the keys round these two, as the compositor has it
                const Box& p0 = k > 0 ? keys[k - 1] : a;
                const Box& p3 = k + 2 < keys.size() ? keys[k + 2] : b;
                smoothPath(a.smooth, p0.cx, p0.cy, a.cx, a.cy, b.cx, b.cy, p3.cx, p3.cy, f, o.cx, o.cy);
            }
            o.w = a.w + (b.w - a.w) * f;
            o.h = a.h + (b.h - a.h) * f;
            o.rot = a.rot + (b.rot - a.rot) * f;
            o.z = a.z + (b.z - a.z) * f;
            o.d = a.d + (b.d - a.d) * f;
            o.rx = a.rx + (b.rx - a.rx) * f;
            o.ry = a.ry + (b.ry - a.ry) * f;
            o.ox = a.ox + (b.ox - a.ox) * f;
            o.oy = a.oy + (b.oy - a.oy) * f;
            o.oz = a.oz + (b.oz - a.oz) * f;
            o.fx = a.fx + (b.fx - a.fx) * f;
            o.fy = a.fy + (b.fy - a.fy) * f;
            o.fz = a.fz + (b.fz - a.fz) * f;
            o.blur = a.blur + (b.blur - a.blur) * f;
            o.intensity = a.intensity + (b.intensity - a.intensity) * f;
            o.color = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                const double v = ((a.color >> sh) & 0xff) + ((double)((b.color >> sh) & 0xff) - ((a.color >> sh) & 0xff)) * f;
                o.color |= (uint32_t)std::min(255.0, std::max(0.0, std::floor(v + 0.5))) << sh;
            }
            return o;
        }
    }
    return keys.back();
}

void sortKeys(std::vector<Box>& keys)
{
    std::stable_sort(keys.begin(), keys.end(), [](const Box& a, const Box& b) { return a.time < b.time; });
}

int keyIndexAt(const std::vector<Box>& keys, double t, double tol)
{
    for (size_t k = 0; k < keys.size(); ++k)
        if (std::fabs(keys[k].time - t) <= tol) return (int)k;
    return -1;
}

// The key a change at time t goes to. With no keys or one, the settings are for the whole stretch (the single key is
// changed wherever the cursor is); with several, the key at the cursor (made if needed with the values it has there), as
// in VEGAS.
Box& keyForEdit(std::vector<Box>& keys, double t, double tol, double firstTime, const Box& def = Box())
{
    if (keys.empty()) {
        Box b = def;
        b.time = firstTime;
        keys.push_back(b);
    }
    if (keys.size() == 1) return keys[0];
    const int at = keyIndexAt(keys, t, tol);
    if (at >= 0) return keys[(size_t)at];
    Box b = boxAt(keys, t);
    b.time = t;
    keys.push_back(b);
    sortKeys(keys);
    return keys[(size_t)keyIndexAt(keys, t, 1e-12)];
}

// The box turned to angle 'rot' about its rotation centre (Box::px, py; its own centre when 0, 0): its centre goes round
// the rotation centre, which stays put. 'sw' x 'sh' is the rectangle's size in units of the same size across and down
// (pixels), where the turn is drawn.
Box turnedAbout(const Box& b, double rot, double sw, double sh)
{
    Box o = b;
    o.rot = rot;
    if (b.px == 0 && b.py == 0) return o;
    const double r = (rot - b.rot) * kPi / 180.0, c = std::cos(r), s = std::sin(r);
    const double vx = -b.px * sw, vy = -b.py * sh;
    const double nx = c * vx - s * vy, ny = s * vx + c * vy;
    const double pvx = b.cx + b.px, pvy = b.cy + b.py;
    o.cx = pvx + nx / std::max(1e-9, sw);
    o.cy = pvy + ny / std::max(1e-9, sh);
    o.px = pvx - o.cx;
    o.py = pvy - o.cy;
    return o;
}

// ---------------------------------------------------------------------------------- the box view
// The image (or frame) with the box over it. Drag inside the box to move it, its edges and corners to size it, just
// outside a corner to turn it. Lock aspect (default) keeps the box's shape; Shift does the opposite for one drag. Size
// about centre (default) keeps the centre fixed.
class BoxView : public QWidget {
public:
    std::function<Box()> current;
    std::function<void()> onBegin;
    std::function<void(const Box&)> onDrag;
    std::function<void()> onEnd;
    std::function<void(const QPoint& global)> onMenu;
    QSizeF space = QSizeF(16, 9);
    QImage picture;
    QString emptyText;
    bool cropStyle = true;
    bool lockAspect = true, aboutCentre = true, rotation = true;
    // VEGAS's other workspace switches: Enable Snapping (F8: the box's edges and centre snap to a grid of 16ths of the
    // rectangle, and its edges), Edit in Object Space (O: moving keeps to the box's own turned axes, so Prevent Movement
    // and Shift work along them), Prevent Movement X / Y (X, Y), Prevent Scaling X / Y (Shift+X, Shift+Y).
    bool snapping = false, objectSpace = false, noMoveX = false, noMoveY = false, noScaleX = false, noScaleY = false;
    int gridDivisions = 16;
    double zoom = 1.0;
    QPointF offset;
    std::function<void()> onZoom;
    bool pivots = false;
    std::function<std::vector<QPointF>()> path;
    // Event Pan/Crop's mask editing: drawn over everything, and given the mouse first (true: taken)
    std::function<void(QPainter&)> paintOver;
    std::function<bool(QMouseEvent*)> pressHook, moveHook, releaseHook, doubleClickHook;

    explicit BoxView(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumSize(360, 240);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setFocusPolicy(Qt::ClickFocus);
    }

    QRectF area() const
    {
        const double sw = std::max(1.0, space.width()), sh = std::max(1.0, space.height());
        const double s = zoom * 0.62 * std::min((width() - 20) / sw, (height() - 20) / sh);
        const double w = sw * s, h = sh * s;
        return QRectF((width() - w) / 2.0 + offset.x() * s, (height() - h) / 2.0 + offset.y() * s, w, h);
    }
    double gridStep() const { return 1.0 / std::max(1, std::min(256, gridDivisions)); }
    void publishArea() { setProperty("area", area()); }
    QPointF toView(double fx, double fy) const
    {
        const QRectF a = area();
        return QPointF(a.left() + fx * a.width(), a.top() + fy * a.height());
    }
    QPointF corner(const Box& b, int sx, int sy) const
    {
        const QRectF a = area();
        const QPointF c = toView(b.cx, b.cy);
        const double lx = sx * b.w * a.width() / 2, ly = sy * b.h * a.height() / 2;
        const double r = b.rot * kPi / 180.0;
        return QPointF(c.x() + lx * std::cos(r) - ly * std::sin(r), c.y() + lx * std::sin(r) + ly * std::cos(r));
    }

protected:
    enum Part { Nothing, Inside, Edge, Corner, Turn, Pivot, Pan };

    void paintEvent(QPaintEvent*) override
    {
        publishArea();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(24, 24, 26));
        p.setPen(QPen(QColor(40, 40, 43), 1));
        for (int x = 0; x < width(); x += 24) p.drawLine(x, 0, x, height());
        for (int y = 0; y < height(); y += 24) p.drawLine(0, y, width(), y);

        const QRectF a = area();
        if (cropStyle) {
            if (!picture.isNull()) p.drawImage(a, picture);
            else {
                p.fillRect(a, QColor(55, 55, 60));
                p.setPen(QColor(170, 170, 175));
                p.drawText(a, Qt::AlignCenter, emptyText);
            }
            if (snapping) {
                p.setPen(QPen(QColor(255, 255, 255, 40), 1));
                const double g = gridStep();
                for (double k = g; k < 1.0 - 1e-9; k += g) {
                    p.drawLine(QPointF(a.left() + a.width() * k, a.top()), QPointF(a.left() + a.width() * k, a.bottom()));
                    p.drawLine(QPointF(a.left(), a.top() + a.height() * k), QPointF(a.right(), a.top() + a.height() * k));
                }
            }
        } else {
            p.fillRect(a, QColor(34, 34, 37));
            if (snapping) {
                p.setPen(QPen(QColor(48, 48, 53), 1));
                const double g = gridStep();
                for (double k = g; k < 1.0 - 1e-9; k += g) {
                    p.drawLine(QPointF(a.left() + a.width() * k, a.top()), QPointF(a.left() + a.width() * k, a.bottom()));
                    p.drawLine(QPointF(a.left(), a.top() + a.height() * k), QPointF(a.right(), a.top() + a.height() * k));
                }
            }
            p.setPen(QPen(QColor(150, 150, 155), 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(a);
            p.setPen(QPen(QColor(60, 60, 64), 1));
            p.drawLine(QPointF(a.center().x(), a.top()), QPointF(a.center().x(), a.bottom()));
            p.drawLine(QPointF(a.left(), a.center().y()), QPointF(a.right(), a.center().y()));
        }
        if (!current) return;
        const Box b = current();
        QPolygonF poly;
        poly << corner(b, -1, -1) << corner(b, 1, -1) << corner(b, 1, 1) << corner(b, -1, 1);
        if (cropStyle) {
            QPainterPath outside;
            outside.addRect(QRectF(rect()));
            QPainterPath in;
            in.addPolygon(poly);
            in.closeSubpath();
            p.fillPath(outside.subtracted(in), QColor(0, 0, 0, 120));
        } else {
            QPainterPath in;
            in.addPolygon(poly);
            in.closeSubpath();
            p.fillPath(in, QColor(0, 122, 204, 40));
        }
        // the box: dashed, black under white so it shows on any image
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 200), 2.2));
        p.drawPolygon(poly);
        p.setPen(QPen(QColor(255, 255, 255), 1.2, Qt::DashLine));
        p.drawPolygon(poly);
        const QPointF c = toView(b.cx, b.cy);
        const double hx = std::fabs(b.w) * a.width() / 2, hy = std::fabs(b.h) * a.height() / 2;
        p.save();
        p.translate(c);
        p.rotate(b.rot);
        p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::DotLine));
        p.drawEllipse(QPointF(0, 0), hx, hy);
        p.scale(b.w < 0 ? -1 : 1, b.h < 0 ? -1 : 1);
        const double fh = std::max(10.0, std::min(hx, hy) * 0.9);
        QFont f = font();
        f.setPixelSize((int)fh);
        f.setBold(true);
        QPainterPath letter;
        letter.addText(0, 0, f, "F");
        const QRectF lb = letter.boundingRect();
        letter.translate(-lb.center());
        p.setPen(QPen(QColor(0, 0, 0, 150), 2));
        p.setBrush(QColor(255, 255, 255, 120));
        p.drawPath(letter);
        p.restore();
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(QColor(255, 255, 255));
        for (int sx = -1; sx <= 1; ++sx)
            for (int sy = -1; sy <= 1; ++sy) {
                if (sx == 0 && sy == 0) continue;
                const QRectF ar = area();
                const double lx = sx * b.w * ar.width() / 2, ly = sy * b.h * ar.height() / 2;
                const double r = b.rot * kPi / 180.0;
                const QPointF h(c.x() + lx * std::cos(r) - ly * std::sin(r), c.y() + lx * std::sin(r) + ly * std::cos(r));
                p.drawRect(QRectF(h.x() - 3, h.y() - 3, 6, 6));
            }
        if (path) {
            const std::vector<QPointF> pts = path();
            if (pts.size() >= 2) {
                QPolygonF line;
                for (const QPointF& f : pts) line << toView(f.x(), f.y());
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0, 0, 0, 150), 2.4));
                p.drawPolyline(line);
                p.setPen(QPen(QColor(255, 210, 90), 1.2, Qt::DotLine));
                p.drawPolyline(line);
            }
        }
        if (pivots) {
            const QPointF pv = toView(b.cx + b.px, b.cy + b.py);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 200), 3));
            p.drawEllipse(pv, 5, 5);
            p.setPen(QPen(QColor(255, 255, 255), 1.4));
            p.drawEllipse(pv, 5, 5);
            p.drawLine(QPointF(pv.x() - 8, pv.y()), QPointF(pv.x() + 8, pv.y()));
            p.drawLine(QPointF(pv.x(), pv.y() - 8), QPointF(pv.x(), pv.y() + 8));
        }
        if (paintOver) paintOver(p);
    }

    QPointF local(const Box& b, const QPointF& at) const
    {
        const QPointF c = toView(b.cx, b.cy);
        const double r = b.rot * kPi / 180.0;
        const double dx = at.x() - c.x(), dy = at.y() - c.y();
        return QPointF(dx * std::cos(r) + dy * std::sin(r), -dx * std::sin(r) + dy * std::cos(r));
    }

    Part partAt(const QPointF& at, int* sx, int* sy) const
    {
        if (!current) return Nothing;
        const Box b = current();
        const QRectF a = area();
        const QPointF l = local(b, at);
        const double hx = std::fabs(b.w) * a.width() / 2, hy = std::fabs(b.h) * a.height() / 2;
        const double tol = 6;
        const bool nearX = std::fabs(std::fabs(l.x()) - hx) <= tol, nearY = std::fabs(std::fabs(l.y()) - hy) <= tol;
        const bool inX = std::fabs(l.x()) <= hx + tol, inY = std::fabs(l.y()) <= hy + tol;
        *sx = l.x() < 0 ? -1 : 1;
        *sy = l.y() < 0 ? -1 : 1;
        // the rotation centre: grabbed where it is once moved off the box's centre; on the centre itself (where a drag
        // moves the box) with Ctrl held
        if (pivots && QLineF(at, toView(b.cx + b.px, b.cy + b.py)).length() <= 6.0 &&
            (b.px != 0 || b.py != 0 || (QApplication::keyboardModifiers() & Qt::ControlModifier)))
            return Pivot;
        if (nearX && nearY) return Corner;
        if (nearX && inY) { *sy = 0; return Edge; }
        if (nearY && inX) { *sx = 0; return Edge; }
        if (std::fabs(l.x()) < hx && std::fabs(l.y()) < hy) return Inside;
        if (rotation && std::fabs(l.x()) < hx + 26 && std::fabs(l.y()) < hy + 26 && std::fabs(l.x()) > hx - tol && std::fabs(l.y()) > hy - tol) return Turn;
        return Pan;
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (pressHook && pressHook(e)) return;
        if (e->button() == Qt::RightButton) {
            if (onMenu) onMenu(e->globalPosition().toPoint());
            return;
        }
        if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && current && partAt(e->position(), &sx_, &sy_) == Pan)) {
            part_ = Pan;
            press_ = e->position();
            panStart_ = offset;
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        if (e->button() != Qt::LeftButton || !current) return;
        part_ = partAt(e->position(), &sx_, &sy_);
        if (part_ == Nothing) return;
        start_ = current();
        press_ = e->position();
        if (onBegin) onBegin();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (part_ == Pan) {
            const double s = area().width() / std::max(1.0, space.width());
            offset = panStart_ + (e->position() - press_) / std::max(1e-9, s);
            publishArea();
            update();
            if (onZoom) onZoom();
            return;
        }
        if (part_ == Nothing && moveHook && moveHook(e)) return;
        if (part_ == Nothing || !(e->buttons() & Qt::LeftButton)) {
            int sx = 0, sy = 0;
            switch (partAt(e->position(), &sx, &sy)) {
            case Inside: setCursor(Qt::SizeAllCursor); break;
            case Edge: setCursor(sx ? Qt::SizeHorCursor : Qt::SizeVerCursor); break;
            case Corner: setCursor(sx * sy > 0 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor); break;
            case Turn: setCursor(Qt::OpenHandCursor); break;
            case Pivot: setCursor(Qt::CrossCursor); break;
            case Pan: setCursor(Qt::OpenHandCursor); break;
            default: unsetCursor(); break;
            }
            return;
        }
        if (onDrag) onDrag(dragged(e->position(), e->modifiers()));
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (part_ == Pan) {
            if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
                part_ = Nothing;
                setCursor(Qt::OpenHandCursor);
            }
            return;
        }
        if (part_ == Nothing && releaseHook && releaseHook(e)) return;
        if (e->button() != Qt::LeftButton || part_ == Nothing) return;
        part_ = Nothing;
        if (onEnd) onEnd();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (doubleClickHook && doubleClickHook(e)) return;
        QWidget::mouseDoubleClickEvent(e);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        const double f = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        zoom = std::min(8.0, std::max(0.2, zoom * f));
        publishArea();
        update();
        if (onZoom) onZoom();
    }
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        publishArea();
        update();
    }

private:
    Box dragged(const QPointF& at, Qt::KeyboardModifiers mods) const
    {
        Box b = start_;
        const QRectF a = area();
        const double r = start_.rot * kPi / 180.0;
        switch (part_) {
        case Inside: {
            double dx = at.x() - press_.x(), dy = at.y() - press_.y();
            // in object space the limits work along the box's own turned axes
            const double rr = objectSpace ? r : 0.0;
            double lx = dx * std::cos(rr) + dy * std::sin(rr), ly = -dx * std::sin(rr) + dy * std::cos(rr);
            if (mods & Qt::ShiftModifier) { if (std::fabs(lx) > std::fabs(ly)) ly = 0; else lx = 0; }
            if (noMoveX) lx = 0;
            if (noMoveY) ly = 0;
            dx = lx * std::cos(rr) - ly * std::sin(rr);
            dy = lx * std::sin(rr) + ly * std::cos(rr);
            b.cx = start_.cx + dx / a.width();
            b.cy = start_.cy + dy / a.height();
            if (snapping) snapMove(b, !(noMoveX && !objectSpace), !(noMoveY && !objectSpace));
            break;
        }
        case Turn: {
            const QPointF c = pivots ? toView(start_.cx + start_.px, start_.cy + start_.py) : toView(start_.cx, start_.cy);
            const double a0 = std::atan2(press_.y() - c.y(), press_.x() - c.x());
            const double a1 = std::atan2(at.y() - c.y(), at.x() - c.x());
            double d = (a1 - a0) * 180.0 / kPi;
            double rot = start_.rot + d;
            if (mods & Qt::ShiftModifier) rot = std::floor(rot / 15.0 + 0.5) * 15.0;
            b = turnedAbout(start_, rot, a.width(), a.height());
            break;
        }
        case Pivot: {
            double fx = (at.x() - a.left()) / std::max(1.0, a.width()), fy = (at.y() - a.top()) / std::max(1.0, a.height());
            if (snapping) {
                const double g = gridStep();
                fx = std::floor(fx / g + 0.5) * g;
                fy = std::floor(fy / g + 0.5) * g;
            }
            b.px = fx - start_.cx;
            b.py = fy - start_.cy;
            break;
        }
        case Edge: case Corner: {
            const QPointF l = local(start_, at);
            const double hx0 = std::fabs(start_.w) * a.width() / 2, hy0 = std::fabs(start_.h) * a.height() / 2;
            const bool lock = lockAspect != ((mods & Qt::ShiftModifier) != 0);
            double nhx = hx0, nhy = hy0, ox = 0, oy = 0;
            if (aboutCentre) {
                if (sx_) nhx = std::fabs(l.x());
                if (sy_) nhy = std::fabs(l.y());
            } else {
                if (sx_) { const double anchor = -sx_ * hx0; nhx = std::fabs(l.x() - anchor) / 2; ox = (l.x() + anchor) / 2; }
                if (sy_) { const double anchor = -sy_ * hy0; nhy = std::fabs(l.y() - anchor) / 2; oy = (l.y() + anchor) / 2; }
            }
            if (noScaleX) { nhx = hx0; ox = 0; }
            if (noScaleY) { nhy = hy0; oy = 0; }
            if (snapping && std::fabs(r) < 1e-9) {
                // the moving edge snaps (unturned boxes only: a turned box's edges are not on the grid's lines)
                if (sx_ && !noScaleX) nhx = snapHalf(nhx, ox, start_.cx, a.width(), aboutCentre ? 0 : sx_);
                if (sy_ && !noScaleY) nhy = snapHalf(nhy, oy, start_.cy, a.height(), aboutCentre ? 0 : sy_);
            }
            if (lock && hx0 > 0 && hy0 > 0 && !noScaleX && !noScaleY) {
                double s;
                if (sx_ && sy_) s = (nhx * hx0 + nhy * hy0) / (hx0 * hx0 + hy0 * hy0);
                else if (sx_) s = nhx / hx0;
                else s = nhy / hy0;
                nhx = hx0 * s;
                nhy = hy0 * s;
                if (!aboutCentre) {
                    if (sx_) ox = -sx_ * hx0 + sx_ * nhx;
                    if (sy_) oy = -sy_ * hy0 + sy_ * nhy;
                    // a corner keeps the opposite corner; an edge keeps the opposite edge, sizing the other way about the middle
                }
            }
            const double minW = 4.0;
            nhx = std::max(minW / 2, nhx);
            nhy = std::max(minW / 2, nhy);
            b.w = (start_.w < 0 ? -1 : 1) * 2 * nhx / a.width();
            b.h = (start_.h < 0 ? -1 : 1) * 2 * nhy / a.height();
            if (!aboutCentre) {
                // the centre moves by the turned offset
                const double dx = ox * std::cos(r) - oy * std::sin(r), dy = ox * std::sin(r) + oy * std::cos(r);
                b.cx = start_.cx + dx / a.width();
                b.cy = start_.cy + dy / a.height();
            }
            break;
        }
        default:
            break;
        }
        return b;
    }

    // Snapping a moved box: whichever of its left edge, centre and right edge (top, middle, bottom) is nearest a grid
    // line within 8 pixels goes onto it.
    void snapMove(Box& b, bool doX, bool doY) const
    {
        const QRectF a = area();
        const double step = gridStep();
        auto best = [step](double c, double half, double px) {
            double move = 0, dist = 1e9;
            for (double edge : {c - half, c, c + half}) {
                const double g = std::floor(edge / step + 0.5) * step;
                if (std::fabs(g - edge) * px < dist) { dist = std::fabs(g - edge) * px; move = g - edge; }
            }
            return dist <= 8.0 ? move : 0.0;
        };
        const double rr = b.rot * kPi / 180.0;
        const double hw = 0.5 * (std::fabs(b.w * std::cos(rr)) * a.width() + std::fabs(b.h * std::sin(rr)) * a.height()) / a.width();
        const double hh = 0.5 * (std::fabs(b.w * std::sin(rr)) * a.width() + std::fabs(b.h * std::cos(rr)) * a.height()) / a.height();
        if (doX) b.cx += best(b.cx, hw, a.width());
        if (doY) b.cy += best(b.cy, hh, a.height());
    }
    // A half size (view pixels) whose moving edge is snapped. 'side' 0: sized about the centre (both edges move); else
    // the side that moves (the other stays, and 'offset', the centre's move, follows).
    double snapHalf(double half, double& offset, double centre, double length, int side) const
    {
        const double edge = side == 0 ? centre + half / length : centre + (offset + side * half) / length;
        const double g = std::floor(edge / gridStep() + 0.5) * gridStep();
        if (std::fabs(g - edge) * length > 8.0) return half;
        const double d = (g - edge) * length;
        if (side == 0) return std::max(1.0, half + d);
        offset += d / 2;
        return std::max(1.0, half + side * d / 2);
    }

    Part part_ = Nothing;
    int sx_ = 0, sy_ = 0;
    Box start_;
    QPointF press_;
    QPointF panStart_;
};

// ---------------------------------------------------------------------------------- the keyframe strip
// A ruler with the keyframes on it and the window's time cursor. Each keyframe is a diamond in VEGAS's colour for its
// interpolation type (Linear grey, Fast green, Slow olive, Smooth lavender, Sharp pink, Hold dark red); selected ones
// are larger with a light outline. Click to move the cursor; click a keyframe to select it (Ctrl or Shift: add to or
// remove from the selection), drag to move the selected ones (Ctrl+drag: copies; Alt+drag: VEGAS's Change Keyframe
// Spacing); double-click adds one; right-click one for its menu.
class KeyStrip : public QWidget {
public:
    enum DragMode { MoveKeys = 0, SpaceKeys = 1, CopyKeys = 2 };
    std::function<std::vector<Box>()> keys;
    std::function<bool(double)> isSelected;
    double span = 1.0;
    double origin = 0;
    double fps = 30;
    double now = 0;
    std::function<void(double)> onSeek;
    std::function<void(int, Qt::KeyboardModifiers)> onKeyPress;
    // keyframe k dragged to a time: the selected keyframes move (or, CopyKeys, copies go) with it; SpaceKeys (Alt+drag):
    // the keyframes between it and the first one (the last, for the first) spread or close up in proportion, those
    // beyond it move with it
    std::function<void(int, double, DragMode)> onKeyDrag;
    std::function<void()> onKeyRelease;
    std::function<void(int, const QPoint&)> onKeyMenu;
    std::function<void(double)> onAddAt;
    std::function<void()> onDeleteKeys;
    std::function<void()> onEmptyClick;

    explicit KeyStrip(QWidget* parent) : QWidget(parent)
    {
        setObjectName("keyStrip");
        setMinimumHeight(54);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::ClickFocus);
    }
    int xOf(double t) const { return kMargin + (int)std::lround(t / std::max(1e-9, span) * (width() - 2 * kMargin)); }
    double timeAt(int x) const { return std::min(span, std::max(0.0, (double)(x - kMargin) / std::max(1, width() - 2 * kMargin) * span)); }
    int keyY() const { return 18 + (height() - 18) / 2; }
    static QColor curveColor(int curve)
    {
        switch (curve) {
        case KeyFast: return QColor(184, 255, 184);
        case KeySlow: return QColor(204, 196, 128);
        case KeySmooth: return QColor(196, 196, 255);
        case KeySharp: return QColor(240, 136, 204);
        case KeyHold: return QColor(160, 0, 0);
        default: return QColor(200, 200, 204);
        }
    }

protected:
    static const int kMargin = 10;

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(32, 32, 35));
        p.fillRect(QRect(0, 0, width(), 17), QColor(44, 44, 48));
        p.setPen(QColor(150, 150, 156));
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.85);
        p.setFont(f);
        const double pxPerSec = (width() - 2 * kMargin) / std::max(1e-9, span);
        double step = 1.0 / std::max(1.0, fps);
        const double nice[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
        if (pxPerSec * step < 70)
            for (double n : nice) { step = n; if (pxPerSec * n >= 70) break; }
        for (double t = 0; t <= span + 1e-9; t += step) {
            const int x = xOf(t);
            p.drawLine(x, 11, x, 17);
            p.drawText(QRect(x + 3, 0, 120, 13), Qt::AlignLeft | Qt::AlignVCenter, qs(timecode(origin + t, fps)));
        }
        p.setPen(QColor(62, 62, 66));
        p.drawLine(kMargin, keyY(), width() - kMargin, keyY());
        const std::vector<Box> ks = keys ? keys() : std::vector<Box>();
        for (size_t k = 0; k < ks.size(); ++k) {
            const QPointF c(xOf(ks[k].time), keyY());
            const bool atNow = std::fabs(ks[k].time - now) <= 0.5 / std::max(1.0, fps);
            const bool sel = isSelected && isSelected(ks[k].time);
            const double r = sel ? 7.5 : 6.0;
            p.setPen(sel ? QPen(QColor(255, 255, 255), 2.0) : QPen(atNow ? QColor(255, 196, 70) : QColor(20, 20, 20), atNow ? 2.0 : 1.0));
            p.setBrush(curveColor(ks[k].curve));
            const QPointF d[4] = {QPointF(c.x(), c.y() - r), QPointF(c.x() + r, c.y()), QPointF(c.x(), c.y() + r), QPointF(c.x() - r, c.y())};
            p.drawPolygon(d, 4);
        }
        const int x = xOf(now);
        p.setPen(QPen(QColor(255, 255, 255), 1));
        p.drawLine(x, 0, x, height());
        const QPointF tri[3] = {QPointF(x - 5, 0), QPointF(x + 5, 0), QPointF(x, 6)};
        p.setBrush(Qt::white);
        p.drawPolygon(tri, 3);
    }

    int keyAt(const QPoint& at) const
    {
        const std::vector<Box> ks = keys ? keys() : std::vector<Box>();
        for (size_t k = 0; k < ks.size(); ++k)
            if (std::abs(at.x() - xOf(ks[k].time)) <= 7 && std::abs(at.y() - keyY()) <= 9) return (int)k;
        return -1;
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const QPoint at = e->position().toPoint();
        const int k = keyAt(at);
        if (e->button() == Qt::RightButton) {
            if (k >= 0 && onKeyMenu) onKeyMenu(k, e->globalPosition().toPoint());
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        if (k >= 0) {
            dragKey_ = k;
            moved_ = false;
            mode_ = (e->modifiers() & Qt::AltModifier) ? SpaceKeys : (e->modifiers() & Qt::ControlModifier) ? CopyKeys : MoveKeys;
            pressX_ = at.x();
            const std::vector<Box> ks = keys();
            if (onSeek) onSeek(ks[(size_t)k].time);
            if (onKeyPress) onKeyPress(k, e->modifiers());
            return;
        }
        if (onEmptyClick) onEmptyClick();
        if (onSeek) onSeek(snap(timeAt(at.x())));
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const double t = snap(timeAt(e->position().toPoint().x()));
        if (dragKey_ >= 0) {
            // a Ctrl+click that does not move is a selection only: copies start once the mouse has gone a few pixels
            if (!moved_ && std::abs(e->position().toPoint().x() - pressX_) < 3) return;
            moved_ = true;
            if (onKeyDrag) onKeyDrag(dragKey_, t, mode_);
            if (onSeek) onSeek(t);
        } else if (onSeek) {
            onSeek(t);
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if (dragKey_ >= 0 && onKeyRelease) onKeyRelease();
        dragKey_ = -1;
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && keyAt(e->position().toPoint()) < 0 && onAddAt) onAddAt(snap(timeAt(e->position().toPoint().x())));
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete && onDeleteKeys) { onDeleteKeys(); e->accept(); return; }
        QWidget::keyPressEvent(e);
    }

private:
    double snap(double t) const { return fps > 0 ? std::floor(t * fps + 0.5) / fps : t; }
    int dragKey_ = -1;
    DragMode mode_ = MoveKeys;
    bool moved_ = false;
    int pressX_ = 0;
};

// A grid of named values in groups, like VEGAS's property list beside its Pan/Crop and Track Motion workspaces.
class PropPanel : public QWidget {
public:
    explicit PropPanel(QWidget* parent) : QWidget(parent)
    {
        setObjectName("propPanel");
        grid_ = new QGridLayout(this);
        grid_->setContentsMargins(0, 0, 0, 0);
        grid_->setHorizontalSpacing(8);
        grid_->setVerticalSpacing(3);
        grid_->setColumnStretch(1, 1);
        setFixedWidth(230);
    }
    QLabel* group(const QString& title)
    {
        QLabel* l = new QLabel(title, this);
        l->setObjectName("propGroup");
        QFont f = l->font();
        f.setBold(true);
        l->setFont(f);
        l->setAutoFillBackground(true);
        QPalette p = l->palette();
        p.setColor(QPalette::Window, QColor(58, 58, 63));
        l->setPalette(p);
        l->setContentsMargins(6, 3, 6, 3);
        grid_->addWidget(l, row_++, 0, 1, 2);
        return l;
    }
    QLabel* add(const QString& label, QWidget* w)
    {
        QLabel* l = new QLabel(label, this);
        l->setContentsMargins(12, 0, 0, 0);
        grid_->addWidget(l, row_, 0);
        grid_->addWidget(w, row_++, 1);
        return l;
    }
    void addWide(QWidget* w)
    {
        grid_->addWidget(w, row_++, 0, 1, 2);
    }
    void finish() { grid_->setRowStretch(row_, 1); }

private:
    QGridLayout* grid_;
    int row_ = 0;
};

QToolButton* toolButton(QWidget* parent, const QString& text, const QString& tip, const char* name, bool checkable = false)
{
    QToolButton* b = new QToolButton(parent);
    b->setObjectName(name);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setAutoRaise(false);
    return b;
}

// ---------------------------------------------------------------------------------- the 3D workspace
// VEGAS's 3D Track Motion workspace (Track Motion of a track in 3D Source Alpha): one to four panes (the Layout, keys 1
// to 6), each looking at the 3D space from a view angle: Camera (as the compositor draws it), Front, Back, Left, Right,
// Top, Bottom (flat views along the axes) or Perspective (from above, in front and to the right). A pane shows the frame
// (dashed), the camera and what it sees (outside the Camera view), the other 3D tracks (grey) and the track's box with
// its "F". Drag inside the box to move it along the pane's two axes, a corner to size it, just outside a corner to turn
// it about the pane's view axis (Shift: 15 degree steps); drag the empty space to scroll, the wheel zooms. The window's
// Prevent Movement / Scaling X, Y, Z switches and Lock Aspect Ratio apply. Right-click menu: Restore Box, Center,
// Rotation, Size; Flip; Restore View; Match Camera View.
enum Angle3d { CameraAngle = 0, FrontAngle, BackAngle, LeftAngle, RightAngle, TopAngle, BottomAngle, PerspectiveAngle };
const char* angleName(int a)
{
    static const char* names[] = {"Camera", "Front", "Back", "Left", "Right", "Top", "Bottom", "Perspective"};
    return names[std::min(7, std::max(0, a))];
}

MotionKey keyOfBox(const Box& b)
{
    MotionKey k;
    k.time = b.time; k.x = b.cx; k.y = b.cy; k.w = b.w; k.h = b.h; k.rotation = b.rot; k.curve = b.curve; k.smoothness = b.smooth;
    k.z = b.z; k.d = b.d; k.rotX = b.rx; k.rotY = b.ry; k.oriX = b.ox; k.oriY = b.oy; k.oriZ = b.oz; k.offX = b.fx; k.offY = b.fy; k.offZ = b.fz;
    return k;
}
Box boxOfKey(const MotionKey& k)
{
    Box b;
    b.time = k.time; b.cx = k.x; b.cy = k.y; b.w = k.w; b.h = k.h; b.rot = k.rotation; b.curve = k.curve; b.smooth = k.smoothness;
    b.z = k.z; b.d = k.d; b.rx = k.rotX; b.ry = k.rotY; b.ox = k.oriX; b.oy = k.oriY; b.oz = k.oriZ; b.fx = k.offX; b.fy = k.offY; b.fz = k.offZ;
    return b;
}

struct Scene3d {
    int W = 1920, H = 1080;
    double par = 1.0;
    std::vector<MotionKey> outer;
    std::vector<std::vector<MotionKey>> others;
};

struct Switches3d {
    bool rotation = true, lockAspect = true;
    bool noMove[3] = {false, false, false}, noScale[3] = {false, false, false};
};

class Pane3d : public QWidget {
public:
    int angle = CameraAngle;
    double zoom = 1.0;
    QPointF pan;
    std::function<Box()> current;
    std::function<Scene3d()> scene;
    std::function<Switches3d()> switches;
    std::function<void()> onBegin;
    std::function<void(const Box&)> onDrag;
    std::function<void()> onEnd;
    std::function<void(Pane3d*, const QPoint&)> onMenu;
    QComboBox* angleBox = nullptr;

    Pane3d(QWidget* parent, int a) : QWidget(parent), angle(a)
    {
        setObjectName("pane3d");
        setMouseTracking(true);
        setMinimumSize(150, 100);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setFocusPolicy(Qt::ClickFocus);
        angleBox = new QComboBox(this);
        angleBox->setObjectName("paneAngle");
        for (int k = 0; k < 8; ++k) angleBox->addItem(angleName(k), k);
        angleBox->setCurrentIndex(a);
        angleBox->setToolTip("The view angle of this pane");
        angleBox->setFocusPolicy(Qt::NoFocus);
        angleBox->move(4, 4);
        angleBox->adjustSize();
        QObject::connect(angleBox, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i < 0) return;
            angle = i;
            restoreView();
        });
    }
    void setAngle(int a)
    {
        QSignalBlocker b(angleBox);
        angleBox->setCurrentIndex(a);
        angle = a;
        restoreView();
    }
    void restoreView()
    {
        zoom = 1.0;
        pan = QPointF();
        update();
    }

    bool project(const double w[3], QPointF& out) const
    {
        const double Wd = sc_.W * sc_.par, D = trackMotion3dCamera(sc_.W, sc_.par);
        double a = 0, b = 0;
        const double x = w[0], y = w[1], z = w[2];
        switch (angle) {
        case CameraAngle: {
            const double zz = z + D;
            if (!(zz > 0.02 * D)) return false;
            a = x * D / zz;
            b = y * D / zz;
            break;
        }
        case FrontAngle: a = x; b = y; break;
        case BackAngle: a = -x; b = y; break;
        case LeftAngle: a = -z; b = y; break;
        case RightAngle: a = z; b = y; break;
        case TopAngle: a = x; b = -z; break;
        case BottomAngle: a = x; b = z; break;
        default: {
            const double yaw = 35 * kPi / 180, pitch = 25 * kPi / 180;
            const double e[3] = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
            const double f[3] = {-e[0], -e[1], -e[2]};
            double r[3] = {1 * f[2] - 0 * f[1], 0 * f[0] - 0 * f[2], 0 * f[1] - 1 * f[0]};
            const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            for (double& v : r) v /= rl;
            const double dn[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
            const double E = 2.6 * D;
            const double depth = x * f[0] + y * f[1] + z * f[2] + E;
            if (!(depth > 0.05 * E)) return false;
            a = (x * r[0] + y * r[1] + z * r[2]) * E / depth;
            b = (x * dn[0] + y * dn[1] + z * dn[2]) * E / depth;
            break;
        }
        }
        const double s = scale();
        out = QPointF(width() / 2.0 + pan.x() + a * s, height() / 2.0 + 10 + pan.y() + b * s);
        (void)Wd;
        return true;
    }
    double scale() const
    {
        const double Wd = std::max(1.0, sc_.W * sc_.par), H = std::max(1, sc_.H), D = trackMotion3dCamera(sc_.W, sc_.par);
        const double pw = std::max(40, width() - 16), ph = std::max(40, height() - 36);
        double s;
        if (angle == CameraAngle || angle == FrontAngle || angle == BackAngle) s = 0.8 * std::min(pw / Wd, ph / H);
        else if (angle == PerspectiveAngle) s = 0.62 * std::min(pw / Wd, ph / H);
        else s = 0.9 * std::min(pw, ph) / (2.4 * std::max(D, Wd));
        return s * zoom;
    }
    // Where the corners of an image placed by 'moves' are (top left, top right, bottom right, bottom left). False if any
    // is behind the camera (Camera view).
    bool corners(const std::vector<MotionKey>& moves, QPointF out[4]) const
    {
        static const double us[4] = {-0.5, 0.5, 0.5, -0.5}, vs[4] = {-0.5, -0.5, 0.5, 0.5};
        bool all = true;
        for (int i = 0; i < 4; ++i) {
            double w[3];
            trackMotion3dPoint(moves, us[i], vs[i], sc_.W, sc_.H, sc_.par, w);
            if (!project(w, out[i])) all = false;
        }
        return all;
    }
    std::vector<MotionKey> movesOf(const Box& b) const
    {
        std::vector<MotionKey> m;
        m.push_back(keyOfBox(b));
        m.insert(m.end(), sc_.outer.begin(), sc_.outer.end());
        return m;
    }
    bool centreOf(const Box& b, QPointF& out) const
    {
        double w[3];
        trackMotion3dPoint(movesOf(b), 0, 0, sc_.W, sc_.H, sc_.par, w);
        return project(w, out);
    }
protected:
    enum Part { Nothing, Inside, Corner, Turn, Scroll };

    void paintEvent(QPaintEvent*) override
    {
        sc_ = scene ? scene() : Scene3d();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(24, 24, 26));
        p.setPen(QPen(QColor(38, 38, 41), 1));
        for (int x = 0; x < width(); x += 24) p.drawLine(x, 0, x, height());
        for (int y = 0; y < height(); y += 24) p.drawLine(0, y, width(), y);
        QPointF fr[4];
        const bool frameOk = corners(std::vector<MotionKey>(), fr);
        // looking straight at the frame from the front or back, the camera is in the middle of it: not drawn
        if (angle != CameraAngle && angle != FrontAngle && angle != BackAngle) {
            double cam[3] = {0, 0, -trackMotion3dCamera(sc_.W, sc_.par)};
            QPointF c;
            if (project(cam, c)) {
                p.setPen(QPen(QColor(80, 130, 190, 160), 1));
                if (frameOk)
                    for (const QPointF& q : fr) p.drawLine(c, q);
                p.setBrush(QColor(80, 130, 190));
                p.drawEllipse(c, 5, 5);
                p.setPen(QColor(120, 165, 215));
                p.drawText(QRectF(c.x() + 8, c.y() - 8, 80, 16), Qt::AlignLeft | Qt::AlignVCenter, "Camera");
            }
        }
        if (frameOk) {
            QPolygonF f;
            for (const QPointF& q : fr) f << q;
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(150, 150, 155), 1, Qt::DashLine));
            p.drawPolygon(f);
        }
        for (const auto& m : sc_.others) {
            QPointF q[4];
            if (!corners(m, q)) continue;
            QPolygonF g;
            for (const QPointF& x : q) g << x;
            p.setPen(QPen(QColor(130, 130, 136, 170), 1));
            p.setBrush(QColor(130, 130, 136, 30));
            p.drawPolygon(g);
        }
        setProperty("angle", angle);
        if (!current) return;
        const Box b = current();
        QPointF q[4];
        if (!corners(movesOf(b), q)) return;
        for (int i = 0; i < 4; ++i) setProperty(qPrintable(QString("corner%1").arg(i)), q[i]);
        QPointF centre;
        if (centreOf(b, centre)) setProperty("boxCentre", centre);
        QPolygonF poly;
        for (const QPointF& x : q) poly << x;
        QPainterPath in;
        in.addPolygon(poly);
        in.closeSubpath();
        p.fillPath(in, QColor(0, 122, 204, 50));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 200), 2.2));
        p.drawPolygon(poly);
        p.setPen(QPen(QColor(255, 255, 255), 1.2, Qt::DashLine));
        p.drawPolygon(poly);
        // the "F" on the box, in its own plane (which way up and round it is, and whether its back is seen)
        QTransform onBox;
        QPolygonF unit;
        unit << QPointF(0, 0) << QPointF(1, 0) << QPointF(1, 1) << QPointF(0, 1);
        if (QTransform::quadToQuad(unit, poly, onBox)) {
            QPainterPath letter;
            QFont f = font();
            f.setPixelSize(40);
            f.setBold(true);
            letter.addText(0, 0, f, "F");
            const QRectF lb = letter.boundingRect();
            const double k = 0.45 / std::max(lb.width() / std::max(1e-9, std::fabs(b.w) * sc_.W * sc_.par),
                                              lb.height() / std::max(1e-9, std::fabs(b.h) * sc_.H));
            QTransform toUnit;
            toUnit.translate(0.5, 0.5);
            toUnit.scale(k / std::max(1e-9, std::fabs(b.w) * sc_.W * sc_.par), k / std::max(1e-9, std::fabs(b.h) * sc_.H));
            toUnit.translate(-lb.center().x(), -lb.center().y());
            p.save();
            p.setTransform(toUnit * onBox, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 120));
            p.drawPath(letter);
            p.restore();
        }
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(QColor(255, 255, 255));
        for (const QPointF& x : q) p.drawRect(QRectF(x.x() - 3, x.y() - 3, 6, 6));
        QPointF c;
        if (centreOf(b, c)) {
            p.setPen(QPen(QColor(255, 255, 255, 160), 1));
            p.drawLine(c - QPointF(5, 0), c + QPointF(5, 0));
            p.drawLine(c - QPointF(0, 5), c + QPointF(0, 5));
        }
    }

    Part partAt(const QPointF& at, int* corner) const
    {
        if (!current) return Nothing;
        const Box b = current();
        QPointF q[4];
        if (!corners(movesOf(b), q)) return Nothing;
        for (int i = 0; i < 4; ++i)
            if (QLineF(at, q[i]).length() <= 7) { *corner = i; return Corner; }
        QPolygonF poly;
        for (const QPointF& x : q) poly << x;
        if (poly.containsPoint(at, Qt::OddEvenFill)) return Inside;
        for (int i = 0; i < 4; ++i) {
            const QPointF a = q[i], e = q[(i + 1) % 4];
            const QPointF d = e - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            const double t = len2 > 0 ? std::min(1.0, std::max(0.0, ((at.x() - a.x()) * d.x() + (at.y() - a.y()) * d.y()) / len2)) : 0.0;
            if (QLineF(at, a + d * t).length() <= 5) return Inside;
        }
        const Switches3d s = switches ? switches() : Switches3d();
        if (s.rotation)
            for (int i = 0; i < 4; ++i)
                if (QLineF(at, q[i]).length() <= 26) { *corner = i; return Turn; }
        return Nothing;
    }

    void moveAxes(int& a, int& b) const
    {
        if (angle == TopAngle || angle == BottomAngle) { a = 0; b = 2; }
        else if (angle == LeftAngle || angle == RightAngle) { a = 2; b = 1; }
        else { a = 0; b = 1; }
    }
    static double& posField(Box& b, int axis) { return axis == 0 ? b.cx : axis == 1 ? b.cy : b.z; }
    static double& sizeField(Box& b, int axis) { return axis == 0 ? b.w : axis == 1 ? b.h : b.d; }
    static double sizeOf(const Box& b, int axis) { return axis == 0 ? b.w : axis == 1 ? b.h : b.d; }
    double& turnField(Box& b) const
    {
        if (angle == TopAngle || angle == BottomAngle) return b.ry;
        if (angle == LeftAngle || angle == RightAngle) return b.rx;
        return b.rot;
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        sc_ = scene ? scene() : Scene3d();
        if (e->button() == Qt::RightButton) {
            if (onMenu) onMenu(this, e->globalPosition().toPoint());
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        press_ = e->position();
        corner_ = 0;
        part_ = partAt(press_, &corner_);
        if (part_ == Nothing) {
            part_ = Scroll;
            panStart_ = pan;
            return;
        }
        start_ = current();
        centreOf(start_, centre_);
        if (part_ == Inside) {
            int a, b;
            moveAxes(a, b);
            const int ax[2] = {a, b};
            for (int i = 0; i < 2; ++i) {
                Box m = start_;
                posField(m, ax[i]) += 1e-3;
                QPointF c;
                centreOf(m, c);
                jac_[i] = (c - centre_) / 1e-3;
            }
        }
        if (part_ == Turn) {
            // which way the field turns the box on this pane
            static const double us[4] = {-0.5, 0.5, 0.5, -0.5}, vs[4] = {-0.5, -0.5, 0.5, 0.5};
            auto angleOf = [&](const Box& bx) {
                double w[3];
                trackMotion3dPoint(movesOf(bx), us[corner_], vs[corner_], sc_.W, sc_.H, sc_.par, w);
                QPointF q;
                project(w, q);
                QPointF c;
                centreOf(bx, c);
                return std::atan2(q.y() - c.y(), q.x() - c.x());
            };
            Box m = start_;
            turnField(m) += 1.0;
            double d = angleOf(m) - angleOf(start_);
            while (d > kPi) d -= 2 * kPi;
            while (d < -kPi) d += 2 * kPi;
            turnSign_ = d < 0 ? -1.0 : 1.0;
        }
        if (onBegin) onBegin();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || part_ == Nothing) {
            int c = 0;
            switch (partAt(e->position(), &c)) {
            case Inside: setCursor(Qt::SizeAllCursor); break;
            case Corner: setCursor(Qt::SizeFDiagCursor); break;
            case Turn: setCursor(Qt::OpenHandCursor); break;
            default: unsetCursor(); break;
            }
            return;
        }
        if (part_ == Scroll) {
            pan = panStart_ + (e->position() - press_);
            update();
            return;
        }
        if (onDrag) onDrag(dragged(e->position(), e->modifiers()));
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const Part was = part_;
        part_ = Nothing;
        if (was != Nothing && was != Scroll && onEnd) onEnd();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const double f = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        zoom = std::min(20.0, std::max(0.05, zoom * f));
        update();
    }

private:
    Box dragged(const QPointF& at, Qt::KeyboardModifiers mods) const
    {
        Box b = start_;
        const Switches3d s = switches ? switches() : Switches3d();
        switch (part_) {
        case Inside: {
            QPointF d = at - press_;
            if (mods & Qt::ShiftModifier) { if (std::fabs(d.x()) > std::fabs(d.y())) d.setY(0); else d.setX(0); }
            int a0, a1;
            moveAxes(a0, a1);
            const bool f0 = !s.noMove[a0], f1 = !s.noMove[a1];
            const QPointF j0 = jac_[0], j1 = jac_[1];
            double m0 = 0, m1 = 0;
            const double det = j0.x() * j1.y() - j1.x() * j0.y();
            if (f0 && f1 && std::fabs(det) > 1e-9) {
                m0 = (d.x() * j1.y() - j1.x() * d.y()) / det;
                m1 = (j0.x() * d.y() - d.x() * j0.y()) / det;
            } else {
                const double n0 = j0.x() * j0.x() + j0.y() * j0.y(), n1 = j1.x() * j1.x() + j1.y() * j1.y();
                if (f0 && n0 > 1e-12) m0 = (j0.x() * d.x() + j0.y() * d.y()) / n0;
                if (f1 && n1 > 1e-12) m1 = (j1.x() * d.x() + j1.y() * d.y()) / n1;
            }
            posField(b, a0) += m0;
            posField(b, a1) += m1;
            break;
        }
        case Corner: {
            const QPointF r0 = press_ - centre_, r1 = at - centre_;
            int a0, a1;
            moveAxes(a0, a1);
            const bool lock = s.lockAspect != ((mods & Qt::ShiftModifier) != 0);
            if (lock) {
                const double l0 = std::hypot(r0.x(), r0.y());
                if (l0 < 2) break;
                const double f = std::hypot(r1.x(), r1.y()) / l0;
                for (int k = 0; k < 3; ++k)
                    if (!s.noScale[k]) sizeField(b, k) = sizeOf(start_, k) * f;
            } else {
                if (std::fabs(r0.x()) > 3 && !s.noScale[a0]) sizeField(b, a0) = sizeOf(start_, a0) * std::fabs(r1.x()) / std::fabs(r0.x());
                if (std::fabs(r0.y()) > 3 && !s.noScale[a1]) sizeField(b, a1) = sizeOf(start_, a1) * std::fabs(r1.y()) / std::fabs(r0.y());
            }
            for (int k = 0; k < 3; ++k) {
                double& v = sizeField(b, k);
                if (std::fabs(v) < 1e-3) v = v < 0 ? -1e-3 : 1e-3;
            }
            break;
        }
        case Turn: {
            const double a0 = std::atan2(press_.y() - centre_.y(), press_.x() - centre_.x());
            const double a1 = std::atan2(at.y() - centre_.y(), at.x() - centre_.x());
            double v = turnField(b) + turnSign_ * (a1 - a0) * 180.0 / kPi;
            if (mods & Qt::ShiftModifier) v = std::floor(v / 15.0 + 0.5) * 15.0;
            turnField(b) = v;
            break;
        }
        default:
            break;
        }
        return b;
    }

    Scene3d sc_;
    Part part_ = Nothing;
    int corner_ = 0;
    Box start_;
    QPointF press_, centre_, panStart_;
    QPointF jac_[2];
    double turnSign_ = 1.0;
};

// The panes of the 3D workspace in a layout (VEGAS's Layout drop-down, keys 1 to 6): 1 one pane, 2 two side by side, 3
// two one above the other, 4 four, 5 a large pane with two beside it, 6 a large pane with two under it. Each layout
// remembers its panes' view angles while the window is open.
class View3d : public QWidget {
public:
    std::function<void(Pane3d*)> setUp;
    std::vector<Pane3d*> panes;

    explicit View3d(QWidget* parent) : QWidget(parent)
    {
        setObjectName("view3d");
        grid_ = new QGridLayout(this);
        grid_->setContentsMargins(0, 0, 0, 0);
        grid_->setSpacing(3);
        static const int defaults[7][4] = {
            {CameraAngle, TopAngle, LeftAngle, FrontAngle},
            {CameraAngle, TopAngle, LeftAngle, FrontAngle},
            {CameraAngle, TopAngle, LeftAngle, FrontAngle},
            {CameraAngle, TopAngle, LeftAngle, FrontAngle},
            {TopAngle, FrontAngle, LeftAngle, CameraAngle},
            {CameraAngle, TopAngle, LeftAngle, FrontAngle},
            {CameraAngle, TopAngle, LeftAngle, FrontAngle},
        };
        for (int l = 0; l < 7; ++l)
            for (int k = 0; k < 4; ++k) angles_[l][k] = defaults[l][k];
    }
    int layoutNumber() const { return layout_; }
    void setLayoutNumber(int n)
    {
        n = std::min(6, std::max(1, n));
        if (n == layout_ && !panes.empty()) return;
        for (size_t k = 0; k < panes.size() && layout_ >= 1; ++k) angles_[layout_][k] = panes[k]->angle;
        for (Pane3d* p : panes) {
            grid_->removeWidget(p);
            p->hide();
            p->setProperty("deleting", true);
            p->deleteLater();
        }
        panes.clear();
        layout_ = n;
        setProperty("layout", n);
        auto add = [&](int row, int col, int rs, int cs) {
            Pane3d* p = new Pane3d(this, angles_[n][panes.size()]);
            if (setUp) setUp(p);
            grid_->addWidget(p, row, col, rs, cs);
            panes.push_back(p);
        };
        for (int r = 0; r < 2; ++r) { grid_->setRowStretch(r, 0); grid_->setColumnStretch(r, 0); }
        switch (n) {
        case 1: add(0, 0, 1, 1); break;
        case 2: add(0, 0, 1, 1); add(0, 1, 1, 1); break;
        case 3: add(0, 0, 1, 1); add(1, 0, 1, 1); break;
        case 4: add(0, 0, 1, 1); add(0, 1, 1, 1); add(1, 0, 1, 1); add(1, 1, 1, 1); break;
        case 5: add(0, 0, 2, 1); add(0, 1, 1, 1); add(1, 1, 1, 1); grid_->setColumnStretch(0, 2); grid_->setColumnStretch(1, 1); break;
        default: add(0, 0, 1, 2); add(1, 0, 1, 1); add(1, 1, 1, 1); grid_->setRowStretch(0, 2); grid_->setRowStretch(1, 1); break;
        }
        if (n != 5) grid_->setColumnStretch(0, 1), grid_->setColumnStretch(1, n == 2 || n == 4 || n == 6 ? 1 : 0);
        if (n != 6) grid_->setRowStretch(0, 1), grid_->setRowStretch(1, n == 3 || n == 4 || n == 5 ? 1 : 0);
    }
    void updatePanes()
    {
        for (Pane3d* p : panes) p->update();
    }

private:
    QGridLayout* grid_;
    int layout_ = 0;
    int angles_[7][4];
};

// ---------------------------------------------------------------------------------- the box-editing windows
// What Pan/Crop and Track Motion share: the view, side panel, strip, keyframes and time cursor. A window works on a list
// of keys somewhere in the project (an event's pan/crop, a track's motion) through the virtual functions; its time is
// 'now_', seconds along the strip.
class BoxWindow : public QWidget {
public:
    BoxWindow(QWidget* parent, Playback* playback) : QWidget(parent, Qt::Window), playback_(playback)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        resize(980, 640);
    }
    ~BoxWindow() override
    {
        if (listener_) app_.removeListener(listener_);
        if (app_.dragging() && dragging_) app_.cancelDrag();
    }

protected:
    virtual bool exists() const = 0;
    virtual std::vector<Box> readKeys(const Project& p) const = 0;
    virtual void writeKeys(Project& p, const std::vector<Box>& keys) const = 0;
    virtual double span() const = 0;
    virtual double startOnTimeline() const = 0;
    virtual double firstKeyTime() const { return 0; }
    virtual QSizeF space() const = 0;
    virtual std::string undoName() const = 0;
    virtual QString windowName() const = 0;
    virtual void refreshExtra() {}
    virtual void pictureChanged() {}
    virtual bool smoothable() const { return true; }
    virtual bool hasPivot() const { return false; }
    virtual bool hasEditGroup() const { return false; }

    AppState& app_ = AppState::get();
    Playback* playback_;
    BoxView* view_ = nullptr;
    QHBoxLayout* middle_ = nullptr;
    KeyStrip* strip_ = nullptr;
    PropPanel* panel_ = nullptr;
    QHBoxLayout* toolbar_ = nullptr;
    QToolButton *lockBtn_ = nullptr, *centreBtn_ = nullptr, *rotateBtn_ = nullptr, *syncBtn_ = nullptr;
    QToolButton *snapBtn_ = nullptr, *objectBtn_ = nullptr, *moveXBtn_ = nullptr, *moveYBtn_ = nullptr, *scaleXBtn_ = nullptr, *scaleYBtn_ = nullptr;
    Spin *wBox_ = nullptr, *hBox_ = nullptr, *xBox_ = nullptr, *yBox_ = nullptr, *rotBox_ = nullptr, *zoomBox_ = nullptr, *smoothBox_ = nullptr;
    Spin *pivotXBox_ = nullptr, *pivotYBox_ = nullptr, *offXBox_ = nullptr, *offYBox_ = nullptr, *gridBox_ = nullptr;
    QComboBox *snapEdit_ = nullptr, *lockEdit_ = nullptr, *centreEdit_ = nullptr, *moveEdit_ = nullptr;
    QComboBox* presetBox_ = nullptr;
    QToolButton *savePreset_ = nullptr, *delPreset_ = nullptr;
    std::string presetsFor_;
    QComboBox* curveBox_ = nullptr;
    std::vector<double> sel_;
    double pressTime_ = 0;
    bool copyDrag_ = false;
    bool dragMoved_ = false;
    double ctrlToggle_ = -1;
    double dragDelta_ = 0;
    QLabel *timeLabel_ = nullptr, *hint_ = nullptr;
    QToolButton *addKey_ = nullptr, *delKey_ = nullptr, *firstKey_ = nullptr, *prevKey_ = nullptr, *nextKey_ = nullptr, *lastKey_ = nullptr;
    double now_ = 0;
    bool dragging_ = false;
    int listener_ = 0;

    double fps() const { return app_.project().settings.fps(); }
    double tol() const { return 0.5 / std::max(1.0, fps()); }
    std::vector<Box> keys() const { std::vector<Box> k = readKeys(app_.project()); sortKeys(k); return k; }
    Box boxNow() const
    {
        const std::vector<Box> k = keys();
        return k.empty() ? defaultBox(now_) : boxAt(k, now_);
    }
    // The box a first keyframe starts as (at time t).
    virtual Box defaultBox(double) const { return Box(); }

    // Builds the controls; the subclass adds its own to the toolbar and panel between buildTop and buildRest.
    void buildTop()
    {
        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(6);
        toolbar_ = new QHBoxLayout();
        toolbar_->setSpacing(6);
        v->addLayout(toolbar_);
        QHBoxLayout* middle = new QHBoxLayout();
        middle->setSpacing(8);
        panel_ = new PropPanel(this);
        // in a scroll area: Track Motion's 3D settings make a long list
        QScrollArea* panelScroll = new QScrollArea(this);
        panelScroll->setObjectName("propScroll");
        panelScroll->setFrameShape(QFrame::NoFrame);
        panelScroll->setWidget(panel_);
        panelScroll->setWidgetResizable(true);
        panelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        panelScroll->setFixedWidth(panel_->width() + 14);
        middle->addWidget(panelScroll);
        middle_ = middle;
        view_ = new BoxView(this);
        view_->setObjectName("boxView");
        middle->addWidget(view_, 1);
        v->addLayout(middle, 1);

        rotateBtn_ = toolButton(this, "Rotation", "Enable Rotation (Shift+F8): dragging just outside a corner turns the box", "enableRotation", true);
        rotateBtn_->setChecked(true);
        snapBtn_ = toolButton(this, "Snap", "Enable Snapping (F8): the box's edges and centre snap to the grid", "enableSnapping", true);
        objectBtn_ = toolButton(this, "Object Space", "Edit in Object Space (O): moving keeps to the box's own (turned) axes", "objectSpace", true);
        moveXBtn_ = toolButton(this, "Lock X", "Prevent Movement (X): the box moves only up and down", "preventMoveX", true);
        moveYBtn_ = toolButton(this, "Lock Y", "Prevent Movement (Y): the box moves only across", "preventMoveY", true);
        lockBtn_ = toolButton(this, "Lock Aspect", "Lock Aspect Ratio (A): sizing keeps the box's shape (hold Shift to reverse it for one drag)", "lockAspect", true);
        lockBtn_->setChecked(true);
        centreBtn_ = toolButton(this, "Size About Center", "Scale About Center (C): sizing keeps the box's centre where it is", "sizeAboutCenter", true);
        centreBtn_->setChecked(true);
        scaleXBtn_ = toolButton(this, "Fix Width", "Prevent Scaling (Shift+X): the box's width stays", "preventScaleX", true);
        scaleYBtn_ = toolButton(this, "Fix Height", "Prevent Scaling (Shift+Y): the box's height stays", "preventScaleY", true);
        for (QToolButton* b : {rotateBtn_, snapBtn_, objectBtn_, moveXBtn_, moveYBtn_, lockBtn_, centreBtn_, scaleXBtn_, scaleYBtn_}) {
            b->setFocusPolicy(Qt::NoFocus);
            toolbar_->addWidget(b);
        }
        auto flag = [this](QToolButton* b, bool BoxView::*field) {
            QObject::connect(b, &QToolButton::toggled, this, [this, field](bool on) {
                view_->*field = on;
                view_->update();
            });
        };
        flag(lockBtn_, &BoxView::lockAspect);
        flag(centreBtn_, &BoxView::aboutCentre);
        flag(rotateBtn_, &BoxView::rotation);
        flag(snapBtn_, &BoxView::snapping);
        flag(objectBtn_, &BoxView::objectSpace);
        flag(moveXBtn_, &BoxView::noMoveX);
        flag(moveYBtn_, &BoxView::noMoveY);
        flag(scaleXBtn_, &BoxView::noScaleX);
        flag(scaleYBtn_, &BoxView::noScaleY);
        toolbar_->addStretch(1);

        panel_->group("Position");
        wBox_ = new Spin(panel_, 1, 1.0e6, 1, 1, "boxWidth");
        hBox_ = new Spin(panel_, 1, 1.0e6, 1, 1, "boxHeight");
        xBox_ = new Spin(panel_, -1.0e6, 1.0e6, 1, 1, "boxX");
        yBox_ = new Spin(panel_, -1.0e6, 1.0e6, 1, 1, "boxY");
        panel_->add("Width", wBox_);
        panel_->add("Height", hBox_);
        panel_->add("X Center", xBox_);
        panel_->add("Y Center", yBox_);
        panel_->group("Rotation");
        rotBox_ = new Spin(panel_, -36000, 36000, 1, 1, "boxAngle");
        rotBox_->setSuffix(QString::fromUtf8("\xc2\xb0"));
        panel_->add("Angle", rotBox_);
        if (hasPivot()) {
            pivotXBox_ = new Spin(panel_, -1.0e6, 1.0e6, 1, 1, "pivotX");
            pivotYBox_ = new Spin(panel_, -1.0e6, 1.0e6, 1, 1, "pivotY");
            pivotXBox_->setToolTip("The point the box rotates about (drag the circle in the workspace; Ctrl+drag moves it off the box centre)");
            pivotYBox_->setToolTip(pivotXBox_->toolTip());
            panel_->add("X Center", pivotXBox_);
            panel_->add("Y Center", pivotYBox_);
            QObject::connect(pivotXBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { numberEdited(5, v); });
            QObject::connect(pivotYBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { numberEdited(6, v); });
            view_->pivots = true;
        }
        panel_->group("Keyframe Interpolation");
        smoothBox_ = new Spin(panel_, 0, 100, 1, 1, "keySmoothness");
        smoothBox_->setToolTip("Smoothness: 0 moves the centre in a straight line to the next keyframe, higher values curve through the surrounding keyframes");
        panel_->add("Smoothness", smoothBox_);
        curveBox_ = combo(panel_, "keyCurve", curveItems());
        curveBox_->setToolTip("How values change from the keyframe at the cursor to the next one");
        panel_->add("Type", curveBox_);

        QObject::connect(wBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { numberEdited(0, v); });
        QObject::connect(hBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { numberEdited(1, v); });
        QObject::connect(xBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { numberEdited(2, v); });
        QObject::connect(yBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { numberEdited(3, v); });
        QObject::connect(rotBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { numberEdited(4, v); });
        QObject::connect(curveBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i < 0) return;
            const int c = curveBox_->itemData(i).toInt();
            const int k = keyIndexAt(keys(), now_, tol());
            if (k < 0) return;
            setCurveOf(k, c);
        });
        QObject::connect(smoothBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            changeBox([v](Box& b) { b.smooth = std::min(100.0, std::max(0.0, v)); }, "Keyframe Smoothness");
        });

        view_->current = [this] { return boxNow(); };
        view_->onBegin = [this] {
            app_.beginDrag();
            dragging_ = true;
        };
        view_->onDrag = [this](const Box& b) {
            if (!dragging_) return;
            const double t = now_, tl = tol(), first = firstKeyTime();
            const Box def = defaultBox(first);
            app_.dragUpdate([this, b, t, tl, first, def](Project& p) {
                std::vector<Box> ks = readKeys(p);
                sortKeys(ks);
                Box& k = keyForEdit(ks, t, tl, first, def);
                setPlace(k, b);
                writeKeys(p, ks);
            });
        };
        view_->onEnd = [this] {
            if (!dragging_) return;
            dragging_ = false;
            app_.endDrag(undoName());
        };
        view_->onZoom = [this] {
            zoomBox_->showValue(view_->zoom * 100.0);
            if (offXBox_) offXBox_->showValue(view_->offset.x());
            if (offYBox_) offYBox_->showValue(view_->offset.y());
        };
    }

    void buildRest()
    {
        panel_->group("Workspace");
        zoomBox_ = new Spin(panel_, 20, 800, 0, 10, "workspaceZoom");
        zoomBox_->setSuffix(" %");
        zoomBox_->setValue(100);
        panel_->add("Zoom", zoomBox_);
        QObject::connect(zoomBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            view_->zoom = v / 100.0;
            view_->publishArea();
            view_->update();
        });
        // VEGAS's X / Y offset (the workspace moved, in the rectangle's pixels; dragging the empty workspace does it
        // too) and Grid spacing (the grid's divisions, also what snapping goes to)
        offXBox_ = new Spin(panel_, -1.0e6, 1.0e6, 0, 10, "workspaceOffsetX");
        offYBox_ = new Spin(panel_, -1.0e6, 1.0e6, 0, 10, "workspaceOffsetY");
        offXBox_->setToolTip("Moves the workspace view left (negative) or right (positive); you can also drag the empty workspace");
        offYBox_->setToolTip("Moves the workspace's view up (negative) or down (positive)");
        panel_->add("X offset", offXBox_);
        panel_->add("Y offset", offYBox_);
        gridBox_ = new Spin(panel_, 1, 256, 0, 1, "gridSpacing");
        gridBox_->setValue(view_->gridDivisions);
        gridBox_->setToolTip("Number of grid divisions across and down (used by Enable Snapping)");
        panel_->add("Grid spacing", gridBox_);
        QObject::connect(offXBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            view_->offset.setX(v);
            view_->publishArea();
            view_->update();
        });
        QObject::connect(offYBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            view_->offset.setY(v);
            view_->publishArea();
            view_->update();
        });
        QObject::connect(gridBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            view_->gridDivisions = (int)std::lround(v);
            view_->update();
        });
        if (hasEditGroup()) buildEditGroup();
        hint_ = note(QString(), panel_);
        hint_->setObjectName("boxHint");
        hint_->setContentsMargins(6, 10, 6, 0);
        panel_->addWide(hint_);
        panel_->finish();

        QVBoxLayout* v = static_cast<QVBoxLayout*>(layout());
        strip_ = new KeyStrip(this);
        v->addWidget(strip_);
        QHBoxLayout* keysRow = new QHBoxLayout();
        keysRow->setSpacing(4);
        firstKey_ = toolButton(this, "|<", "First keyframe", "firstKey");
        prevKey_ = toolButton(this, "<", "Previous keyframe", "prevKey");
        nextKey_ = toolButton(this, ">", "Next keyframe", "nextKey");
        lastKey_ = toolButton(this, ">|", "Last keyframe", "lastKey");
        addKey_ = toolButton(this, "Add Keyframe", "Add a keyframe at the cursor", "addKey");
        delKey_ = toolButton(this, "Delete Keyframe", "Delete the keyframe at the cursor", "deleteKey");
        syncBtn_ = toolButton(this, "Sync Cursor", "The cursor here and the timeline's cursor move together", "syncCursor", true);
        syncBtn_->setChecked(true);
        for (QToolButton* b : {firstKey_, prevKey_, nextKey_, lastKey_}) keysRow->addWidget(b);
        keysRow->addSpacing(10);
        keysRow->addWidget(addKey_);
        keysRow->addWidget(delKey_);
        keysRow->addSpacing(10);
        keysRow->addWidget(syncBtn_);
        // VEGAS's Play and Stop under the keyframes: the project played from the cursor (the window's own when it
        // follows the timeline's), to see the motion as set now
        QToolButton* playBtn = toolButton(this, "Play", "Play from the cursor, to see the motion as it is set now", "boxPlay");
        QToolButton* stopBtn = toolButton(this, "Stop", "Stop playing (back to where play began)", "boxStop");
        for (QToolButton* b : {playBtn, stopBtn}) {
            b->setFocusPolicy(Qt::NoFocus);
            keysRow->addWidget(b);
        }
        QObject::connect(playBtn, &QToolButton::clicked, this, [this] {
            if (!playback_ || playback_->playing()) return;
            const double t = app_.cursor() - startOnTimeline();
            if (t < 0 || t >= span()) app_.setCursor(startOnTimeline());
            playback_->play();
        });
        QObject::connect(stopBtn, &QToolButton::clicked, this, [this] { if (playback_) playback_->stop(); });
        keysRow->addStretch(1);
        timeLabel_ = new QLabel(this);
        timeLabel_->setObjectName("boxTime");
        QFont mono("DejaVu Sans Mono");
        mono.setStyleHint(QFont::Monospace);
        timeLabel_->setFont(mono);
        keysRow->addWidget(timeLabel_);
        v->addLayout(keysRow);

        strip_->keys = [this] { return keys(); };
        strip_->onSeek = [this](double t) { seek(t); };
        strip_->onAddAt = [this](double t) { seek(t); addKeyAtNow(); };
        strip_->isSelected = [this](double t) { return isSel(t); };
        strip_->onKeyPress = [this](int k, Qt::KeyboardModifiers mods) {
            const std::vector<Box> ks = keys();
            if (k < 0 || k >= (int)ks.size()) return;
            const double t = ks[(size_t)k].time;
            const bool add = (mods & (Qt::ControlModifier | Qt::ShiftModifier)) != 0 && !(mods & Qt::AltModifier);
            if ((mods & Qt::ShiftModifier) && !(mods & Qt::ControlModifier) && !sel_.empty()) {
                const double from = sel_.back();
                for (const Box& b : ks)
                    if (b.time >= std::min(from, t) - tol() && b.time <= std::max(from, t) + tol() && !isSel(b.time)) sel_.push_back(b.time);
            } else if (add) {
                if (!isSel(t)) sel_.push_back(t);
                else if (!(mods & Qt::ControlModifier)) unselect(t);
                else ctrlToggle_ = t;
            } else if (!isSel(t)) {
                sel_.assign(1, t);
            }
            pressTime_ = t;
            strip_->update();
            app_.beginDrag();
            dragging_ = true;
            dragMoved_ = false;
        };
        strip_->onKeyDrag = [this](int k, double t, KeyStrip::DragMode mode) {
            if (!dragging_) return;
            dragMoved_ = true;
            ctrlToggle_ = -1;
            copyDrag_ = mode == KeyStrip::CopyKeys;
            const std::vector<double> sel = sel_;
            const double from = pressTime_;
            app_.dragUpdate([this, k, t, mode, sel, from](Project& p) {
                std::vector<Box> ks = readKeys(p);
                sortKeys(ks);
                if (k >= (int)ks.size()) return;
                if (mode == KeyStrip::SpaceKeys && ks.size() >= 2) {
                    respace(ks, k, t);
                    writeKeys(p, ks);
                    return;
                }
                if (!moveSelected(ks, sel, t - from, mode == KeyStrip::CopyKeys)) return;
                writeKeys(p, ks);
            });
            dragDelta_ = t - from;
        };
        strip_->onKeyRelease = [this] {
            if (!dragging_) return;
            dragging_ = false;
            if (!dragMoved_) {
                app_.cancelDrag();
                if (ctrlToggle_ >= 0) unselect(ctrlToggle_);
                ctrlToggle_ = -1;
                strip_->update();
                return;
            }
            app_.endDrag(copyDrag_ ? "Copy Keyframes" : "Move Keyframe");
            for (double& x : sel_) x += dragDelta_;
            dragDelta_ = 0;
            strip_->update();
        };
        strip_->onKeyMenu = [this](int k, const QPoint& at) { keyMenu(k, at); };
        strip_->onDeleteKeys = [this] { deleteSelectedKeys(); };
        strip_->onEmptyClick = [this] { sel_.clear(); };
        QObject::connect(addKey_, &QToolButton::clicked, this, [this] { addKeyAtNow(); });
        QObject::connect(delKey_, &QToolButton::clicked, this, [this] { deleteKeyAtNow(); });
        QObject::connect(firstKey_, &QToolButton::clicked, this, [this] { const auto k = keys(); if (!k.empty()) seek(k.front().time); });
        QObject::connect(lastKey_, &QToolButton::clicked, this, [this] { const auto k = keys(); if (!k.empty()) seek(k.back().time); });
        QObject::connect(prevKey_, &QToolButton::clicked, this, [this] { stepKey(-1); });
        QObject::connect(nextKey_, &QToolButton::clicked, this, [this] { stepKey(1); });
        QObject::connect(syncBtn_, &QToolButton::toggled, this, [this](bool on) {
            if (on) followCursor();
        });
        view_->onMenu = [this](const QPoint& at) { boxMenu(at); };
        view_->path = [this] {
            std::vector<QPointF> pts;
            if (!smoothable()) return pts;
            const std::vector<Box> ks = keys();
            if (ks.size() < 2) return pts;
            for (size_t i = 0; i + 1 < ks.size(); ++i)
                for (int j = 0; j < 24; ++j) {
                    const Box b = boxAt(ks, ks[i].time + (ks[i + 1].time - ks[i].time) * j / 24.0);
                    pts.push_back(QPointF(b.cx, b.cy));
                }
            pts.push_back(QPointF(ks.back().cx, ks.back().cy));
            return pts;
        };

        listener_ = app_.addListener([this](int changes) { changed(changes); });
        makeShortcuts();
        view_->setFocus();
        followCursor();
        refresh();
    }

    virtual std::vector<std::pair<QString, std::function<void(Box&)>>> builtinPresets() const { return {}; }
    virtual const char* presetObjectName() const = 0;
    virtual std::string presetUndoPrefix() const = 0;
    QString presetGroup() const { return "KeyframePresets/" + QString::fromStdString(clipKind()); }
    static Json boxToPreset(const Box& b)
    {
        Json j = Json::object();
        j["cx"] = b.cx; j["cy"] = b.cy; j["w"] = b.w; j["h"] = b.h; j["rot"] = b.rot; j["curve"] = b.curve; j["smooth"] = b.smooth;
        j["px"] = b.px; j["py"] = b.py; j["blur"] = b.blur; j["intensity"] = b.intensity; j["color"] = (double)b.color;
        j["z"] = b.z; j["d"] = b.d; j["rx"] = b.rx; j["ry"] = b.ry; j["ox"] = b.ox; j["oy"] = b.oy; j["oz"] = b.oz;
        j["fx"] = b.fx; j["fy"] = b.fy; j["fz"] = b.fz;
        return j;
    }
    static void presetToBox(const Json& j, Box& b)
    {
        auto num = [&j](const char* k, double& v) {
            const double x = j.get(k).asDouble(v);
            if (std::isfinite(x)) v = x;
        };
        num("cx", b.cx); num("cy", b.cy); num("w", b.w); num("h", b.h); num("rot", b.rot); num("smooth", b.smooth);
        num("px", b.px); num("py", b.py); num("blur", b.blur); num("intensity", b.intensity);
        num("z", b.z); num("d", b.d); num("rx", b.rx); num("ry", b.ry); num("ox", b.ox); num("oy", b.oy); num("oz", b.oz);
        num("fx", b.fx); num("fy", b.fy); num("fz", b.fz);
        b.curve = std::max(0, std::min(5, j.get("curve").asInt(b.curve)));
        b.smooth = std::min(100.0, std::max(0.0, b.smooth));
        if (j.has("color")) b.color = (uint32_t)j.get("color").asDouble((double)b.color);
        if (std::fabs(b.w) < 1e-6) b.w = 1;
        if (std::fabs(b.h) < 1e-6) b.h = 1;
    }
    virtual Json presetNow() const { return boxToPreset(boxNow()); }
    virtual void applyPreset(const Json& j, const std::string& undo)
    {
        applyAtCursor([j](Box& b) { presetToBox(j, b); }, undo);
    }
    // VEGAS's way with a preset: the keyframe at the cursor is changed, or one is made there (the first of all goes
    // where the first keyframe goes).
    void applyAtCursor(const std::function<void(Box&)>& fn, const std::string& undo)
    {
        const double t = now_, tl = tol(), first = firstKeyTime();
        const Box def = defaultBox(first);
        editKeys(undo, [fn, t, tl, first, def](std::vector<Box>& ks) {
            if (ks.empty()) {
                Box b = def;
                b.time = first;
                fn(b);
                b.time = first;
                ks.push_back(b);
                return;
            }
            int at = keyIndexAt(ks, t, tl);
            if (at < 0) {
                Box b = boxAt(ks, t);
                b.time = t;
                b.src = -1;
                ks.push_back(b);
                sortKeys(ks);
                at = keyIndexAt(ks, t, 1e-12);
            }
            if (at < 0) return;
            Box& k = ks[(size_t)at];
            const double kt = k.time;
            fn(k);
            k.time = kt;
        });
    }
    std::vector<std::pair<QString, QString>> presetList() const
    {
        std::vector<std::pair<QString, QString>> v;
        const auto b = builtinPresets();
        for (size_t i = 0; i < b.size(); ++i) v.push_back({b[i].first, "b" + QString::number(i)});
        QSettings st;
        st.beginGroup(presetGroup());
        QStringList names = st.childKeys();
        names.sort(Qt::CaseInsensitive);
        for (const QString& n : names) v.push_back({QByteArray::fromPercentEncoding(n.toUtf8()), "u" + QByteArray::fromPercentEncoding(n.toUtf8())});
        st.endGroup();
        return v;
    }
    void fillPresets()
    {
        if (!presetBox_) return;
        const QString typed = presetBox_->currentText();
        QSignalBlocker sb(presetBox_);
        presetBox_->clear();
        presetBox_->addItem(QString(), QString());
        for (const auto& p : presetList()) presetBox_->addItem(p.first, p.second);
        presetBox_->setCurrentIndex(0);
        presetBox_->setEditText(typed);
        presetsFor_ = clipKind();
    }
    void buildPresets()
    {
        presetBox_ = new QComboBox(this);
        presetBox_->setObjectName(presetObjectName());
        presetBox_->setEditable(true);
        presetBox_->setInsertPolicy(QComboBox::NoInsert);
        presetBox_->setMinimumWidth(170);
        presetBox_->lineEdit()->setPlaceholderText("(Choose a preset)");
        presetBox_->setToolTip("Pick a preset to apply at the cursor (a keyframe is added if needed), or type a name and click Save Preset");
        savePreset_ = toolButton(this, "Save", "Save Preset: save the settings at the cursor under the name in the Preset box", "savePreset");
        delPreset_ = toolButton(this, "Delete", "Delete Preset: the saved preset named in the Preset box", "deletePreset");
        for (QToolButton* b : {savePreset_, delPreset_}) b->setFocusPolicy(Qt::NoFocus);
        toolbar_->insertWidget(0, new QLabel("Preset:", this));
        toolbar_->insertWidget(1, presetBox_);
        toolbar_->insertWidget(2, savePreset_);
        toolbar_->insertWidget(3, delPreset_);
        toolbar_->insertSpacing(4, 12);
        fillPresets();
        QObject::connect(presetBox_, &QComboBox::activated, this, [this](int i) {
            const QString id = presetBox_->itemData(i).toString();
            const QString label = presetBox_->itemText(i);
            if (id.startsWith("b")) {
                const auto ps = builtinPresets();
                const int k = id.mid(1).toInt();
                if (k >= 0 && k < (int)ps.size()) applyAtCursor(ps[(size_t)k].second, presetUndoPrefix() + ": " + s8(label));
            } else if (id.startsWith("u")) {
                QSettings st;
                st.beginGroup(presetGroup());
                const QString text = st.value(QString::fromUtf8(QUrl::toPercentEncoding(id.mid(1)))).toString();
                st.endGroup();
                Json j;
                if (Json::parse(s8(text), j)) applyPreset(j, presetUndoPrefix() + ": " + s8(label));
            }
        });
        QObject::connect(savePreset_, &QToolButton::clicked, this, [this] { savePresetAs(presetBox_->currentText().trimmed()); });
        QObject::connect(delPreset_, &QToolButton::clicked, this, [this] { deletePreset(presetBox_->currentText().trimmed()); });
    }

public:
    bool savePresetAs(const QString& name)
    {
        if (name.isEmpty()) {
            app_.status("Type a name for the preset in the Preset box first.");
            return false;
        }
        QSettings st;
        st.beginGroup(presetGroup());
        st.setValue(QString::fromUtf8(QUrl::toPercentEncoding(name)), qs(presetNow().dump()));
        st.endGroup();
        fillPresets();
        presetBox_->setEditText(name);
        app_.status(s8("Saved the preset \"" + name + "\"."));
        return true;
    }
    bool deletePreset(const QString& name)
    {
        QSettings st;
        st.beginGroup(presetGroup());
        const QString key = QString::fromUtf8(QUrl::toPercentEncoding(name));
        const bool had = !name.isEmpty() && st.contains(key);
        if (had) st.remove(key);
        st.endGroup();
        if (!had) {
            app_.status(name.isEmpty() ? "Choose the preset to delete first." : "Only presets you saved can be deleted.");
            return false;
        }
        fillPresets();
        presetBox_->setEditText(QString());
        return true;
    }

protected:
    // VEGAS's Edit heading (Event Pan/Crop): Snap to grid, Lock aspect, Scale about center and Allow movement as
    // drop-downs, the same switches as the tool bar's, kept in step.
    void buildEditGroup()
    {
        panel_->group("Edit");
        snapEdit_ = combo(panel_, "editSnap", {{"No", "0"}, {"Yes", "1"}});
        lockEdit_ = combo(panel_, "editLockAspect", {{"Yes", "1"}, {"No", "0"}});
        centreEdit_ = combo(panel_, "editScaleCenter", {{"Yes", "1"}, {"No", "0"}});
        moveEdit_ = combo(panel_, "editMovement", {{"Unconstrained", "free"}, {"Horizontal", "x"}, {"Vertical", "y"}});
        snapEdit_->setToolTip("Yes: the box's edges snap to the workspace grid when it is moved or sized");
        lockEdit_->setToolTip("Yes: the box keeps its shape when it is sized");
        centreEdit_->setToolTip("Yes: dragging an edge keeps the box centre; No: the opposite side stays");
        moveEdit_->setToolTip("Which ways the box can be moved");
        panel_->add("Snap to grid", snapEdit_);
        panel_->add("Lock aspect", lockEdit_);
        panel_->add("Scale about center", centreEdit_);
        panel_->add("Allow movement", moveEdit_);
        auto tie = [this](QComboBox* c, QToolButton* b, bool yesIsOn) {
            QObject::connect(c, &QComboBox::activated, this, [c, b, yesIsOn](int) { b->setChecked((c->currentData().toString() == "1") == yesIsOn); });
            QObject::connect(b, &QToolButton::toggled, this, [this] { syncEditGroup(); });
        };
        tie(snapEdit_, snapBtn_, true);
        tie(lockEdit_, lockBtn_, true);
        tie(centreEdit_, centreBtn_, true);
        QObject::connect(moveEdit_, &QComboBox::activated, this, [this](int) {
            const QString m = moveEdit_->currentData().toString();
            moveXBtn_->setChecked(m == "y");
            moveYBtn_->setChecked(m == "x");
        });
        QObject::connect(moveXBtn_, &QToolButton::toggled, this, [this] { syncEditGroup(); });
        QObject::connect(moveYBtn_, &QToolButton::toggled, this, [this] { syncEditGroup(); });
        syncEditGroup();
    }
    void syncEditGroup()
    {
        if (!snapEdit_) return;
        pickData(snapEdit_, snapBtn_->isChecked() ? "1" : "0");
        pickData(lockEdit_, lockBtn_->isChecked() ? "1" : "0");
        pickData(centreEdit_, centreBtn_->isChecked() ? "1" : "0");
        const bool nx = moveXBtn_->isChecked(), ny = moveYBtn_->isChecked();
        pickData(moveEdit_, nx == ny ? "free" : ny ? "x" : "y");
    }

    void followCursor()
    {
        now_ = std::min(span(), std::max(0.0, app_.cursor() - startOnTimeline()));
    }
    void seek(double t)
    {
        now_ = std::min(span(), std::max(0.0, t));
        if (syncBtn_->isChecked()) app_.setCursor(startOnTimeline() + now_);
        refresh();
    }
    void stepKey(int dir)
    {
        const std::vector<Box> ks = keys();
        for (size_t k = 0; k < ks.size(); ++k) {
            const Box& b = ks[dir > 0 ? k : ks.size() - 1 - k];
            if (dir > 0 && b.time > now_ + tol()) { seek(b.time); return; }
            if (dir < 0 && b.time < now_ - tol()) { seek(b.time); return; }
        }
    }

    // Alt+drag of keyframe k to time t (VEGAS's Change Keyframe Spacing): the keyframes between it and the anchor (the
    // first keyframe; the last when k is the first) are spread out or closed up in proportion, and those on the far side
    // of k move with it, so the spacing changes while the order and anchor stay. Kept a frame apart and inside the
    // strip.
    void respace(std::vector<Box>& ks, int k, double t) const
    {
        const int n = (int)ks.size();
        const int anchor = k == 0 ? n - 1 : 0;
        const double a = ks[(size_t)anchor].time, old = ks[(size_t)k].time;
        const double dir = k > anchor ? 1.0 : -1.0;
        const double frame = 1.0 / std::max(1.0, fps());
        const int between = std::abs(k - anchor);
        double lo = a + dir * frame * between;
        double beyond = 0;
        for (int j = 0; j < n; ++j)
            if ((j - k) * dir > 0) beyond = std::max(beyond, std::fabs(ks[(size_t)j].time - old));
        double tt = t;
        if (dir > 0) tt = std::min(std::max(tt, lo), span() - beyond);
        else tt = std::max(std::min(tt, lo), beyond);
        const double f = std::fabs(old - a) > 1e-12 ? (tt - a) / (old - a) : 1.0;
        for (int j = 0; j < n; ++j) {
            const double d = (j - anchor) * dir;
            if (d <= 0) continue;
            if ((j - k) * dir <= 0) ks[(size_t)j].time = a + (ks[(size_t)j].time - a) * f;
            else ks[(size_t)j].time += tt - old;
        }
        sortKeys(ks);
    }

    // The place of a box (everything a drag in a workspace changes) put into a key.
    static void setPlace(Box& k, const Box& b)
    {
        k.cx = b.cx; k.cy = b.cy; k.w = b.w; k.h = b.h; k.rot = b.rot; k.px = b.px; k.py = b.py;
        k.z = b.z; k.d = b.d; k.rx = b.rx; k.ry = b.ry; k.ox = b.ox; k.oy = b.oy; k.oz = b.oz; k.fx = b.fx; k.fy = b.fy; k.fz = b.fz;
    }
    void beginBoxDrag()
    {
        app_.beginDrag();
        dragging_ = true;
    }
    void boxDragged(const Box& b)
    {
        if (!dragging_) return;
        const double t = now_, tl = tol(), first = firstKeyTime();
        const Box def = defaultBox(first);
        app_.dragUpdate([this, b, t, tl, first, def](Project& p) {
            std::vector<Box> ks = readKeys(p);
            sortKeys(ks);
            Box& k = keyForEdit(ks, t, tl, first, def);
            setPlace(k, b);
            writeKeys(p, ks);
        });
    }
    void endBoxDrag()
    {
        if (!dragging_) return;
        dragging_ = false;
        app_.endDrag(undoName());
    }

    void editKeys(const std::string& what, const std::function<void(std::vector<Box>&)>& fn)
    {
        app_.edit(what, [this, fn](Project& p) {
            std::vector<Box> ks = readKeys(p);
            sortKeys(ks);
            fn(ks);
            sortKeys(ks);
            writeKeys(p, ks);
        });
    }

    void changeBox(const std::function<void(Box&)>& fn, const std::string& what = std::string())
    {
        const double t = now_, tl = tol(), first = firstKeyTime();
        const Box def = defaultBox(first);
        editKeys(what.empty() ? undoName() : what, [fn, t, tl, first, def](std::vector<Box>& ks) { fn(keyForEdit(ks, t, tl, first, def)); });
    }

    void numberEdited(int which, double v)
    {
        const QSizeF sp = space();
        changeBox([which, v, sp](Box& b) {
            switch (which) {
            case 0: b.w = (b.w < 0 ? -1 : 1) * v / sp.width(); break;
            case 1: b.h = (b.h < 0 ? -1 : 1) * v / sp.height(); break;
            case 2: b.cx = v / sp.width(); break;
            case 3: b.cy = v / sp.height(); break;
            case 5: b.px = v / sp.width() - b.cx; break;
            case 6: b.py = v / sp.height() - b.cy; break;
            default: b = turnedAbout(b, v, sp.width(), sp.height()); break;
            }
        });
    }

    void addKeyAtNow()
    {
        const double t = now_, tl = tol(), first = firstKeyTime();
        const Box def = defaultBox(first);
        const int newCurve = clipKind() == "mask" ? -1 : vprefs::keyCurveCode("prefs/curveVideoMotion");
        const double newSmooth = vprefs::real(clipKind() == "pancrop" ? "prefs/panCropSmoothness" : "prefs/motionSmoothness");
        const bool canSmooth = smoothable();
        editKeys("Add Keyframe", [t, tl, first, def, newCurve, newSmooth, canSmooth](std::vector<Box>& ks) {
            if (ks.empty()) {
                Box b = def;
                b.time = first;
                ks.push_back(b);
            }
            if (keyIndexAt(ks, t, tl) >= 0) return;
            Box b = boxAt(ks, t);
            b.time = t;
            if (newCurve >= 0) b.curve = newCurve;
            if (canSmooth) b.smooth = newSmooth;
            ks.push_back(b);
        });
    }

    void deleteKeyAtNow()
    {
        const int k = keyIndexAt(keys(), now_, tol());
        if (k < 0) return;
        editKeys("Delete Keyframe", [k](std::vector<Box>& ks) { if (k < (int)ks.size()) ks.erase(ks.begin() + k); });
    }

    static std::vector<std::pair<QString, QString>> curveItems()
    {
        return {{"Linear", "0"}, {"Fast", "3"}, {"Slow", "4"}, {"Smooth", "2"}, {"Sharp", "5"}, {"Hold", "1"}};
    }

    bool isSel(double t) const
    {
        for (double x : sel_)
            if (std::fabs(x - t) <= tol()) return true;
        return false;
    }
    void unselect(double t)
    {
        sel_.erase(std::remove_if(sel_.begin(), sel_.end(), [&](double x) { return std::fabs(x - t) <= tol(); }), sel_.end());
    }
    std::vector<int> targetsOf(const std::vector<Box>& ks, int k) const
    {
        std::vector<int> out;
        if (k >= 0 && k < (int)ks.size() && isSel(ks[(size_t)k].time)) {
            for (size_t i = 0; i < ks.size(); ++i)
                if (isSel(ks[i].time)) out.push_back((int)i);
        } else if (k >= 0 && k < (int)ks.size()) {
            out.push_back(k);
        }
        return out;
    }
    // The keys at the times 'sel' moved by 'delta' (or, copying, copies put there, over any key already at a time they
    // land on). False (nothing done) if a moved key would land outside the strip or on a key that stays.
    bool moveSelected(std::vector<Box>& ks, const std::vector<double>& sel, double delta, bool copy) const
    {
        const double tl = tol();
        auto chosen = [&](double t) {
            for (double x : sel)
                if (std::fabs(x - t) <= tl) return true;
            return false;
        };
        std::vector<Box> moved, rest;
        for (const Box& b : ks) {
            if (chosen(b.time)) {
                Box m = b;
                m.time = b.time + delta;
                if (m.time < -1e-9 || m.time > span() + 1e-9) return false;
                m.time = std::min(span(), std::max(0.0, m.time));
                moved.push_back(m);
                if (copy) rest.push_back(b);
            } else {
                rest.push_back(b);
            }
        }
        if (moved.empty()) return false;
        if (!copy) {
            for (const Box& m : moved)
                for (const Box& r : rest)
                    if (std::fabs(m.time - r.time) < tl) return false;
        } else {
            for (const Box& m : moved)
                rest.erase(std::remove_if(rest.begin(), rest.end(), [&](const Box& r) { return std::fabs(m.time - r.time) < tl; }), rest.end());
            for (Box& m : moved) m.src = -1;
        }
        rest.insert(rest.end(), moved.begin(), moved.end());
        ks = rest;
        sortKeys(ks);
        return true;
    }
    void setCurveOf(int k, int c)
    {
        const std::vector<int> idx = targetsOf(keys(), k);
        if (idx.empty()) return;
        editKeys("Keyframe Interpolation", [idx, c](std::vector<Box>& v) {
            for (int i : idx)
                if (i < (int)v.size()) v[(size_t)i].curve = c;
        });
    }
    void deleteSelectedKeys()
    {
        const std::vector<Box> ks = keys();
        std::vector<double> times;
        for (const Box& b : ks)
            if (isSel(b.time)) times.push_back(b.time);
        if (times.empty()) {
            deleteKeyAtNow();
            return;
        }
        const double tl = tol();
        editKeys(times.size() > 1 ? "Delete Keyframes" : "Delete Keyframe", [times, tl](std::vector<Box>& v) {
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Box& b) {
                for (double t : times)
                    if (std::fabs(b.time - t) <= tl) return true;
                return false;
            }), v.end());
        });
        sel_.clear();
    }

    struct KeyClip {
        std::string kind;
        std::vector<Box> keys;
    };
    static KeyClip& keyClip()
    {
        static KeyClip c;
        return c;
    }
    virtual std::string clipKind() const = 0;
    void copyKeys(int k)
    {
        const std::vector<Box> ks = keys();
        std::vector<int> idx = targetsOf(ks, k);
        if (k < 0)
            for (size_t i = 0; i < ks.size(); ++i)
                if (isSel(ks[i].time)) idx.push_back((int)i);
        if (idx.empty()) {
            const int at = keyIndexAt(ks, now_, tol());
            if (at >= 0) idx.push_back(at);
        }
        if (idx.empty()) return;
        KeyClip c;
        c.kind = clipKind();
        const double t0 = ks[(size_t)idx.front()].time;
        for (int i : idx) {
            Box b = ks[(size_t)i];
            b.time -= t0;
            c.keys.push_back(b);
        }
        keyClip() = c;
        app_.status(strfmt("%d keyframe(s) copied.", (int)c.keys.size()));
    }
    bool canPaste() const { return !keyClip().keys.empty() && keyClip().kind == clipKind(); }
    void pasteKeys()
    {
        if (!canPaste()) return;
        const std::vector<Box> clip = keyClip().keys;
        const double at = now_, tl = tol(), end = span();
        editKeys("Paste Keyframes", [clip, at, tl, end](std::vector<Box>& v) {
            for (Box b : clip) {
                b.time += at;
                if (b.time > end + 1e-9) continue;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const Box& o) { return std::fabs(o.time - b.time) <= tl; }), v.end());
                b.src = -1;
                v.push_back(b);
            }
        });
        sel_.clear();
        for (const Box& b : clip) sel_.push_back(b.time + at);
    }

    void keyMenu(int k, const QPoint& at)
    {
        QMenu m(this);
        m.setObjectName("keyMenu");
        const std::vector<Box> ks = keys();
        if (k >= (int)ks.size()) return;
        if (!isSel(ks[(size_t)k].time)) sel_.assign(1, ks[(size_t)k].time);
        strip_->update();
        for (const auto& it : curveItems()) {
            const int c = it.second.toInt();
            QAction* a = m.addAction(it.first);
            a->setCheckable(true);
            a->setChecked(ks[(size_t)k].curve == c);
            QObject::connect(a, &QAction::triggered, this, [this, k, c] { setCurveOf(k, c); });
        }
        m.addSeparator();
        QObject::connect(m.addAction("Copy\tCtrl+C"), &QAction::triggered, this, [this, k] { copyKeys(k); });
        QAction* paste = m.addAction("Paste\tCtrl+V");
        paste->setEnabled(canPaste());
        QObject::connect(paste, &QAction::triggered, this, [this] { pasteKeys(); });
        QObject::connect(m.addAction("Delete\tDel"), &QAction::triggered, this, [this] { deleteSelectedKeys(); });
        m.addSeparator();
        QObject::connect(m.addAction("Select All"), &QAction::triggered, this, [this] {
            sel_.clear();
            for (const Box& b : keys()) sel_.push_back(b.time);
            strip_->update();
        });
        m.exec(at);
    }

    void boxMenu(const QPoint& at)
    {
        QMenu m(this);
        m.setObjectName("boxMenu");
        for (const auto& item : boxActions()) {
            QAction* a = m.addAction(item.first);
            const std::function<void(Box&)> fn = item.second;
            const QString label = item.first;
            QObject::connect(a, &QAction::triggered, this, [this, fn, label] { changeBox(fn, undoName() + ": " + s8(label)); });
        }
        m.exec(at);
    }

public:
    virtual std::vector<std::pair<QString, std::function<void(Box&)>>> boxActions() const
    {
        std::vector<std::pair<QString, std::function<void(Box&)>>> v;
        v.push_back({"Restore", [](Box& b) { const double t = b.time; const int c = b.curve; b = Box(); b.time = t; b.curve = c; }});
        v.push_back({"Center", [](Box& b) { b.cx = 0.5; b.cy = 0.5; }});
        v.push_back({"Flip Horizontal", [](Box& b) { b.w = -b.w; }});
        v.push_back({"Flip Vertical", [](Box& b) { b.h = -b.h; }});
        return v;
    }

    // The window's keys, as VEGAS's (the toolbar's switches): A Lock Aspect Ratio, C Scale About Center, O Edit in
    // Object Space, X / Y Prevent Movement, Shift+X / Shift+Y Prevent Scaling, F8 Enable Snapping, Shift+F8 Enable
    // Rotation. True if it was one of them.
    bool windowKey(int key, Qt::KeyboardModifiers mods)
    {
        const Qt::KeyboardModifiers m = mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        // the keyframes: Ctrl+Left / Ctrl+Right to the previous / next one, Ctrl+C / Ctrl+V copy and paste them, Delete
        // (on the strip) deletes the selected ones
        if (m == Qt::ControlModifier) {
            switch (key) {
            case Qt::Key_Left: stepKey(-1); return true;
            case Qt::Key_Right: stepKey(1); return true;
            case Qt::Key_C: copyKeys(-1); return true;
            case Qt::Key_V: pasteKeys(); return true;
            default: break;
            }
        }
        if (key == Qt::Key_Delete && m == Qt::NoModifier && strip_ && strip_->hasFocus()) {
            deleteSelectedKeys();
            return true;
        }
        if (extraKey(key, mods)) return true;
        QToolButton* b = switchFor(key, mods);
        if (!b) return false;
        b->toggle();
        return true;
    }

protected:
    virtual bool extraKey(int, Qt::KeyboardModifiers) { return false; }
    virtual QStringList extraKeys() const { return QStringList(); }
    QToolButton* switchFor(int key, Qt::KeyboardModifiers mods) const
    {
        const Qt::KeyboardModifiers m = mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        const bool plain = m == Qt::NoModifier, shift = m == Qt::ShiftModifier;
        switch (key) {
        case Qt::Key_A: return plain ? lockBtn_ : nullptr;
        case Qt::Key_C: return plain ? centreBtn_ : nullptr;
        case Qt::Key_O: return plain ? objectBtn_ : nullptr;
        case Qt::Key_X: return plain ? moveXBtn_ : shift ? scaleXBtn_ : nullptr;
        case Qt::Key_Y: return plain ? moveYBtn_ : shift ? scaleYBtn_ : nullptr;
        case Qt::Key_F8: return plain ? snapBtn_ : shift ? rotateBtn_ : nullptr;
        default: return nullptr;
        }
    }
    // The keys are the window's shortcuts (anywhere in it, except while a number is being typed: the field keeps them);
    // a key that reaches the window itself does the same.
    void makeShortcuts()
    {
        QStringList keys = {"A", "C", "O", "X", "Y", "Shift+X", "Shift+Y", "F8", "Shift+F8", "Ctrl+Left", "Ctrl+Right", "Ctrl+C", "Ctrl+V", "Del"};
        keys += extraKeys();
        for (const QString& k : keys) {
            const QKeySequence seq(k);
            QShortcut* sc = new QShortcut(seq, this);
            sc->setContext(Qt::WindowShortcut);
            sc->setAutoRepeat(false);
            const int key = seq[0].key();
            const Qt::KeyboardModifiers mods = seq[0].keyboardModifiers();
            QObject::connect(sc, &QShortcut::activated, this, [this, key, mods] { windowKey(key, mods); });
        }
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (windowKey(e->key(), e->modifiers())) e->accept();
        else QWidget::keyPressEvent(e);
    }

    void changed(int changes)
    {
        if (changes & kProject) {
            if (!exists()) { close(); return; }
        }
        if ((changes & kCursor) && syncBtn_->isChecked() && !dragging_) followCursor();
        if (changes & (kProject | kCursor)) refresh();
    }

    void refresh()
    {
        if (!exists()) return;
        setWindowTitle(windowName());
        strip_->span = std::max(1e-3, span());
        strip_->origin = startOnTimeline();
        strip_->fps = fps();
        strip_->now = now_;
        strip_->update();
        view_->space = space();
        view_->publishArea();
        strip_->setProperty("span", strip_->span);
        const Box b = boxNow();
        const QSizeF sp = space();
        wBox_->showValue(std::fabs(b.w) * sp.width());
        hBox_->showValue(std::fabs(b.h) * sp.height());
        xBox_->showValue(b.cx * sp.width());
        yBox_->showValue(b.cy * sp.height());
        rotBox_->showValue(b.rot);
        if (pivotXBox_) {
            pivotXBox_->showValue((b.cx + b.px) * sp.width());
            pivotYBox_->showValue((b.cy + b.py) * sp.height());
        }
        const std::vector<Box> ks = keys();
        const int k = keyIndexAt(ks, now_, tol());
        curveBox_->setEnabled(k >= 0);
        if (k >= 0) pickData(curveBox_, QString::number(ks[(size_t)k].curve));
        smoothBox_->setEnabled(smoothable());
        smoothBox_->showValue(b.smooth);
        delKey_->setEnabled(k >= 0);
        // selected keyframes that are gone are forgotten, not while a drag is moving them about
        if (!dragging_) sel_.erase(std::remove_if(sel_.begin(), sel_.end(), [&](double t) { return keyIndexAt(ks, t, tol()) < 0; }), sel_.end());
        bool before = false, after = false;
        for (const Box& x : ks) {
            if (x.time < now_ - tol()) before = true;
            if (x.time > now_ + tol()) after = true;
        }
        firstKey_->setEnabled(before);
        prevKey_->setEnabled(before);
        nextKey_->setEnabled(after);
        lastKey_->setEnabled(after);
        timeLabel_->setText(qs(timecode(startOnTimeline() + now_, fps())));
        hint_->setText(ks.size() <= 1 ? "One keyframe: the settings apply throughout. Add another keyframe to animate."
                                      : QString("%1 keyframes: changes go to the keyframe at the cursor (added if needed).").arg(ks.size()));
        refreshExtra();
        if (presetBox_ && presetsFor_ != clipKind()) fillPresets();
        pictureChanged();
        view_->update();
    }
};

// ---------------------------------------------------------------------------------- Event Pan/Crop
class PanCropWindow : public BoxWindow {
public:
    PanCropWindow(QWidget* parent, Playback* playback, uint64_t eventId) : BoxWindow(parent, playback), eventId_(eventId)
    {
        setObjectName("panCropWindow");
        pool_.setOpener(app_.mediaOpener);
        buildTop();
        view_->cropStyle = true;
        view_->emptyText = "No picture";

        buildPresets();

        panel_->group("Source");
        keepAspect_ = combo(panel_, "maintainAspect", {{"Yes", "1"}, {"No", "0"}});
        keepAspect_->setToolTip("Yes: keep the picture's shape, with bars where shapes differ.\nNo: stretch it to fill the frame.");
        panel_->add("Maintain aspect", keepAspect_);
        QObject::connect(keepAspect_, &QComboBox::currentIndexChanged, this, [this](int i) {
            const bool on = i == 0;
            const Event* e = app_.project().findEvent(eventId_);
            if (!e || e->keepAspect == on) return;
            const uint64_t id = eventId_;
            app_.edit("Maintain Aspect Ratio", [id, on](Project& p) { if (Event* ev = p.findEvent(id)) ev->keepAspect = on; });
        });
        stretch_ = combo(panel_, "stretchToFill", {{"No", "0"}, {"Yes", "1"}});
        stretch_->setToolTip("Yes: the picture fills the frame (anything outside it is cut off).\n"
                             "No: it fits inside the frame, with bars where shapes differ.");
        panel_->add("Stretch to fill frame", stretch_);
        QObject::connect(stretch_, &QComboBox::activated, this, [this](int i) {
            const bool on = i == 1;
            const Event* e = app_.project().findEvent(eventId_);
            if (!e || e->stretchToFill == on) return;
            const uint64_t id = eventId_;
            app_.edit("Stretch to Fill Frame", [id, on](Project& p) { if (Event* ev = p.findEvent(id)) ev->stretchToFill = on; });
        });
        buildMaskPanel();
        buildRest();
        buildMaskTools();
    }


    std::vector<std::pair<QString, std::function<void(Box&)>>> boxActions() const override
    {
        auto v = BoxWindow::boxActions();
        const double media = space().width() / std::max(1.0, space().height());
        const ProjectSettings& s = app_.project().settings;
        const double out = s.width * s.pixelAspect / std::max(1, s.height);
        v.push_back({"Match Output Aspect", [media, out](Box& b) { matchAspect(b, out, media); }});
        v.push_back({"Match Source Aspect", [media](Box& b) { matchAspect(b, media, media); }});
        return v;
    }

private:
    static void matchAspect(Box& b, double aspect, double media)
    {
        const double sw = b.w < 0 ? -1 : 1, sh = b.h < 0 ? -1 : 1;
        double h = std::fabs(b.h), w = h * aspect / media;
        if (w > 1.0 && std::fabs(b.w) <= 1.0) { w = std::fabs(b.w); h = w * media / aspect; }
        b.w = sw * w;
        b.h = sh * h;
    }

    // VEGAS-style presets: the whole image, cropping to fill a frame of another shape, fitting inside one (letterbox /
    // pillarbox), zooming, flipping.
    const char* presetObjectName() const override { return "panCropPreset"; }
    Json presetNow() const override
    {
        if (row_ != MaskRow) return BoxWindow::presetNow();
        Json j = Json::object();
        Json ps = Json::array();
        for (const MaskPath& m : pathsNow()) {
            Json jp = Json::object();
            Json pts = Json::array();
            for (const MaskPoint& q : m.points) {
                Json a = Json::array();
                for (double v : {q.x, q.y, q.inX, q.inY, q.outX, q.outY}) a.push(v);
                pts.push(a);
            }
            jp["points"] = pts;
            jp["closed"] = m.closed;
            jp["negative"] = m.negative;
            jp["enabled"] = m.enabled;
            jp["antiAlias"] = m.antiAlias;
            jp["featherType"] = m.featherType;
            jp["feather"] = m.feather;
            jp["opacity"] = m.opacity;
            ps.push(jp);
        }
        j["paths"] = ps;
        return j;
    }
    void applyPreset(const Json& j, const std::string& undo) override
    {
        if (row_ != MaskRow) {
            BoxWindow::applyPreset(j, undo);
            return;
        }
        std::vector<MaskPath> paths;
        for (const Json& jp : j.get("paths").items()) {
            MaskPath m;
            for (const Json& a : jp.get("points").items()) {
                MaskPoint q;
                q.x = a.at(0).asDouble(0.5); q.y = a.at(1).asDouble(0.5);
                q.inX = a.at(2).asDouble(); q.inY = a.at(3).asDouble(); q.outX = a.at(4).asDouble(); q.outY = a.at(5).asDouble();
                if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.inX) && std::isfinite(q.inY) && std::isfinite(q.outX) && std::isfinite(q.outY))
                    m.points.push_back(q);
            }
            m.closed = jp.get("closed").asBool(true);
            m.negative = jp.get("negative").asBool();
            m.enabled = jp.get("enabled").asBool(true);
            m.antiAlias = jp.get("antiAlias").asBool(true);
            m.featherType = std::max(0, std::min(3, jp.get("featherType").asInt(2)));
            m.feather = std::min(100.0, std::max(0.0, jp.get("feather").asDouble()));
            m.opacity = std::min(1.0, std::max(0.0, jp.get("opacity").asDouble(1.0)));
            if (m.points.size() >= 3) paths.push_back(m);
        }
        auto shared = std::make_shared<const std::vector<MaskPath>>(paths);
        // the key at the cursor gets these shapes; writeKeys keeps 'mask' for a key with no source
        applyAtCursor([shared](Box& b) { b.mask = shared; b.src = -2; }, undo);
        const uint64_t id = eventId_;
        app_.edit("Mask On", [id](Project& p) { if (Event* ev = p.findEvent(id)) ev->maskOn = true; });
    }
    std::string presetUndoPrefix() const override { return row_ == MaskRow ? "Pan/Crop Mask Preset" : "Pan/Crop Preset"; }
    std::vector<std::pair<QString, std::function<void(Box&)>>> builtinPresets() const override
    {
        if (row_ == MaskRow) return {};
        const double media = space().width() / std::max(1.0, space().height());
        std::vector<std::pair<QString, std::function<void(Box&)>>> v;
        v.push_back({"(Default) Whole picture", [](Box& b) { const double t = b.time; const int c = b.curve; b = Box(); b.time = t; b.curve = c; }});
        // the largest box of a shape inside the image (crop to fill), or the smallest round all of it (fit)
        auto crop = [media](double aspect) {
            return [media, aspect](Box& b) {
                b.cx = b.cy = 0.5; b.rot = 0;
                if (aspect > media) { b.w = 1; b.h = media / aspect; } else { b.h = 1; b.w = aspect / media; }
            };
        };
        auto fit = [media](double aspect) {
            return [media, aspect](Box& b) {
                b.cx = b.cy = 0.5; b.rot = 0;
                if (aspect > media) { b.h = 1; b.w = aspect / media; } else { b.w = 1; b.h = media / aspect; }
            };
        };
        v.push_back({"Fill 16:9 (crop 4:3 top and bottom)", crop(16.0 / 9.0)});
        v.push_back({"Fill 4:3 (crop 16:9 sides)", crop(4.0 / 3.0)});
        v.push_back({"Fit in 16:9 (pillarbox)", fit(16.0 / 9.0)});
        v.push_back({"Fit in 4:3 (letterbox)", fit(4.0 / 3.0)});
        v.push_back({"Zoom 2x", [](Box& b) { b.w = (b.w < 0 ? -0.5 : 0.5); b.h = (b.h < 0 ? -0.5 : 0.5); }});
        v.push_back({"Flip Horizontal", [](Box& b) { b.w = -b.w; }});
        v.push_back({"Flip Vertical", [](Box& b) { b.h = -b.h; }});
        return v;
    }

    const Event* theEvent() const { return app_.project().findEvent(eventId_); }
    const MediaItem* media() const
    {
        const Event* e = theEvent();
        return e ? app_.project().findMedia(e->mediaId) : nullptr;
    }

    bool exists() const override { return theEvent() != nullptr; }
    std::vector<Box> readKeys(const Project& p) const override
    {
        std::vector<Box> v;
        if (row_ == MaskRow) {
            // the Mask row: a box per mask key, standing for it by its index (only the time and curve are used)
            if (const Event* e = p.findEvent(eventId_))
                for (size_t i = 0; i < e->mask.size(); ++i) {
                    Box b;
                    b.time = e->mask[i].time;
                    b.curve = e->mask[i].curve;
                    b.src = (int)i;
                    b.mask = std::make_shared<const std::vector<MaskPath>>(e->mask[i].paths);
                    v.push_back(b);
                }
            return v;
        }
        if (const Event* e = p.findEvent(eventId_))
            for (const PanCropKey& k : e->panCrop) {
                Box b;
                b.time = k.time; b.cx = k.cx; b.cy = k.cy; b.w = k.w; b.h = k.h; b.rot = k.rotation; b.curve = k.curve;
                b.smooth = k.smoothness; b.px = k.pivotX; b.py = k.pivotY;
                v.push_back(b);
            }
        return v;
    }
    void writeKeys(Project& p, const std::vector<Box>& keys) const override
    {
        Event* e = p.findEvent(eventId_);
        if (!e) return;
        if (row_ == MaskRow) {
            // keys kept, moved, deleted or added on the strip: a new one has the mask as it is at its time
            const std::vector<MaskKey> old = e->mask;
            e->mask.clear();
            for (const Box& b : keys) {
                MaskKey k;
                k.time = b.time;
                k.curve = b.curve;
                // a key that was there keeps its shapes; a copy or a preset (src < 0 with 'mask') has its own
                k.paths = b.src >= 0 && b.src < (int)old.size() ? old[(size_t)b.src].paths : b.mask ? *b.mask : maskAt(old, b.time);
                e->mask.push_back(k);
            }
            return;
        }
        e->panCrop.clear();
        for (const Box& b : keys) {
            PanCropKey k;
            k.time = b.time; k.cx = b.cx; k.cy = b.cy; k.w = b.w; k.h = b.h; k.rotation = b.rot; k.curve = b.curve;
            k.smoothness = b.smooth; k.pivotX = b.px; k.pivotY = b.py;
            e->panCrop.push_back(k);
        }
    }
    double span() const override { const Event* e = theEvent(); return e ? e->length : 1.0; }
    double startOnTimeline() const override { const Event* e = theEvent(); return e ? e->start : 0.0; }
    QSizeF space() const override
    {
        const MediaItem* m = media();
        const ProjectSettings& s = app_.project().settings;
        if (m && !m->generated && m->info.width > 0 && m->info.height > 0) return QSizeF(m->info.width, m->info.height);
        return QSizeF(s.width, s.height);
    }
    std::string undoName() const override { return row_ == MaskRow ? "Event Pan/Crop Mask" : "Event Pan/Crop"; }
    std::string clipKind() const override { return row_ == MaskRow ? "mask" : "pancrop"; }
    bool smoothable() const override { return row_ == PositionRow; }
    bool hasPivot() const override { return true; }
    bool hasEditGroup() const override { return true; }
    QString windowName() const override
    {
        const Event* e = theEvent();
        return QString("Event Pan/Crop: %1").arg(e ? qs(e->name) : QString());
    }

    void refreshExtra() override
    {
        if (const Event* e = theEvent()) {
            pickData(keepAspect_, e->keepAspect ? "1" : "0");
            pickData(stretch_, e->stretchToFill ? "1" : "0");
        }
        refreshMask();
    }

    // ================ the mask (VEGAS's Mask row and tools)
    // VEGAS's Pan/Crop mask: the Mask check box and keyframe row under the workspace, the mask tools (Normal Edit,
    // Anchor Creation, Anchor Deletion, Split Tangent, Rectangle, Oval), and the Mask / Path properties. Shapes are kept
    // as Bezier paths in the media's image (see MaskKey); the Mask row's keyframes animate them.
    enum Row { PositionRow = 0, MaskRow = 1 };
    enum Tool { ToolNormal = 0, ToolAnchor, ToolDelete, ToolTangent, ToolRect, ToolOval };
    enum Drag { DragNone, DragPoint, DragIn, DragOut, DragPath, DragNewPoint, DragShape, DragScale, DragRotate };

    void buildMaskPanel()
    {
        panel_->group("Mask");
        applyFx_ = combo(panel_, "maskApplyToFx", {{"No", "0"}, {"Yes", "1"}});
        applyFx_->setToolTip("No: the mask cuts the event's picture (transparent outside it).\n"
                             "Yes: the event's effects apply only inside the mask.");
        panel_->add("Apply to FX", applyFx_);
        QObject::connect(applyFx_, &QComboBox::activated, this, [this](int i) {
            const bool on = i == 1;
            const uint64_t id = eventId_;
            app_.edit("Mask: Apply to FX", [id, on](Project& p) { if (Event* ev = p.findEvent(id)) ev->maskApplyToFx = on; });
        });
        pathGroup_ = panel_->group("Path");
        modeBox_ = combo(panel_, "maskMode", {{"Positive", "0"}, {"Negative", "1"}});
        modeBox_->setToolTip("Positive: the inside of the path shows. Negative: the outside shows.");
        panel_->add("Mode", modeBox_);
        aaBox_ = combo(panel_, "maskAntiAlias", {{"Yes", "1"}, {"No", "0"}});
        panel_->add("Anti alias", aaBox_);
        opacityBox_ = new Spin(panel_, 0, 100, 1, 1, "maskOpacity");
        opacityBox_->setSuffix(" %");
        panel_->add("Opacity", opacityBox_);
        featherTypeBox_ = combo(panel_, "maskFeatherType", {{"None", "0"}, {"In", "1"}, {"Out", "2"}, {"Both", "3"}});
        featherTypeBox_->setToolTip("Which side of the path's edge is softened");
        panel_->add("Feather type", featherTypeBox_);
        featherBox_ = new Spin(panel_, 0, 100, 1, 1, "maskFeather");
        featherBox_->setSuffix(" %");
        featherBox_->setToolTip("How soft the edge is (100%: a quarter of the frame's shorter side)");
        panel_->add("Feather", featherBox_);
        QObject::connect(modeBox_, &QComboBox::activated, this, [this](int i) { editPath("Mask Path: Mode", [i](MaskPath& m) { m.negative = i == 1; }); });
        QObject::connect(aaBox_, &QComboBox::activated, this, [this](int i) { editPath("Mask Path: Anti Alias", [i](MaskPath& m) { m.antiAlias = i == 0; }); });
        QObject::connect(featherTypeBox_, &QComboBox::activated, this, [this](int i) { editPath("Mask Path: Feather Type", [i](MaskPath& m) { m.featherType = i; }); });
        QObject::connect(opacityBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { editPath("Mask Path: Opacity", [v](MaskPath& m) { m.opacity = v / 100.0; }); });
        QObject::connect(featherBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) { editPath("Mask Path: Feather", [v](MaskPath& m) { m.feather = v; }); });
    }

    void buildMaskTools()
    {
        const struct { const char* text; const char* tip; const char* name; } tools[] = {
            {"Normal", "Normal Edit Tool: moves the box (Position row) or a mask's points, handles and paths (Mask row)", "toolNormal"},
            {"Anchor", "Anchor Creation Tool: click to add points to a Bezier mask (drag to pull out handles); click the first point to close it", "toolAnchor"},
            {"Delete Anchor", "Anchor Deletion Tool: click a point to take it out of its path", "toolAnchorDelete"},
            {"Split Tangent", "Split Tangent Tool: drag a handle without moving the other one (drag a point to pull out new handles)", "toolSplitTangent"},
            {"Rectangle", "Rectangle or Square Mask Creation Tool: drag to draw (Shift: a square, Ctrl: from the centre)", "toolRectMask"},
            {"Oval", "Oval or Circle Mask Creation Tool: drag to draw (Shift: a circle, Ctrl: from the centre)", "toolOvalMask"},
        };
        QVBoxLayout* column = new QVBoxLayout();
        column->setSpacing(3);
        for (int i = 0; i < 6; ++i) {
            QToolButton* b = toolButton(this, tools[i].text, tools[i].tip, tools[i].name, true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setChecked(i == 0);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            toolBtns_[i] = b;
            column->addWidget(b);
            QObject::connect(b, &QToolButton::clicked, this, [this, i] { setTool((Tool)i); });
        }
        column->addStretch(1);
        if (middle_) middle_->insertLayout(1, column);
        QVBoxLayout* v = static_cast<QVBoxLayout*>(layout());
        QHBoxLayout* keysRow = v && v->count() > 0 ? qobject_cast<QHBoxLayout*>(v->itemAt(v->count() - 1)->layout()) : nullptr;
        rowBox_ = combo(this, "keyRow", {{"Position", "0"}, {"Mask", "1"}});
        rowBox_->setToolTip("The keyframe row to show and edit: the box (Position) or the mask shapes (Mask)");
        rowBox_->setFocusPolicy(Qt::NoFocus);
        maskCheck_ = new QCheckBox("Mask", this);
        maskCheck_->setObjectName("maskSwitch");
        maskCheck_->setToolTip("Mask on: the event's picture (or its effects, with Apply to FX) is cut to the mask shapes");
        maskCheck_->setFocusPolicy(Qt::NoFocus);
        if (keysRow) {
            keysRow->insertWidget(0, rowBox_);
            keysRow->insertWidget(1, maskCheck_);
            keysRow->insertSpacing(2, 12);
        }
        QObject::connect(rowBox_, &QComboBox::activated, this, [this](int i) { setRow(i == 1 ? MaskRow : PositionRow); });
        QObject::connect(maskCheck_, &QCheckBox::clicked, this, [this](bool on) {
            const uint64_t id = eventId_;
            app_.edit(on ? "Mask On" : "Mask Off", [id, on](Project& p) { if (Event* ev = p.findEvent(id)) ev->maskOn = on; });
            if (on) setRow(MaskRow);
        });
        view_->paintOver = [this](QPainter& p) { paintMask(p); };
        view_->pressHook = [this](QMouseEvent* e) { return maskPress(e); };
        view_->moveHook = [this](QMouseEvent* e) { return maskMove(e); };
        view_->releaseHook = [this](QMouseEvent* e) { return maskRelease(e); };
        view_->doubleClickHook = [this](QMouseEvent* e) {
            if (row_ != MaskRow || e->button() != Qt::LeftButton) return false;
            if (tool_ == ToolAnchor) finishOpenPath();
            else maskPress(e);
            return true;
        };
        refresh();
    }

public:
    void setRow(Row r)
    {
        if (r != MaskRow) finishOpenPath();
        row_ = r;
        if (r == PositionRow && tool_ != ToolNormal) setTool(ToolNormal);
        if (rowBox_) pickData(rowBox_, QString::number((int)r));
        refresh();
    }
    void setTool(Tool t)
    {
        if (t != ToolAnchor) finishOpenPath();
        tool_ = t;
        for (int i = 0; i < 6; ++i)
            if (toolBtns_[i]) toolBtns_[i]->setChecked(i == (int)t);
        if (t != ToolNormal && row_ != MaskRow) setRow(MaskRow);
        view_->setCursor(t == ToolNormal ? Qt::ArrowCursor : Qt::CrossCursor);
        view_->update();
    }

private:
    double maskTol() const { return tol(); }
    std::vector<MaskPath> pathsNow() const
    {
        const Event* e = theEvent();
        return e ? maskAt(e->mask, now_) : std::vector<MaskPath>();
    }
    static MaskKey& maskKeyForEdit(Event& e, double t, double tl)
    {
        if (e.mask.empty()) e.mask.push_back(MaskKey());
        if (e.mask.size() == 1) return e.mask[0];
        std::stable_sort(e.mask.begin(), e.mask.end(), [](const MaskKey& a, const MaskKey& b) { return a.time < b.time; });
        for (MaskKey& k : e.mask)
            if (std::fabs(k.time - t) <= tl) return k;
        MaskKey k;
        k.time = t;
        k.paths = maskAt(e.mask, t);
        for (const MaskKey& o : e.mask)
            if (o.time <= t) k.curve = o.curve;
        e.mask.push_back(k);
        std::stable_sort(e.mask.begin(), e.mask.end(), [](const MaskKey& a, const MaskKey& b) { return a.time < b.time; });
        for (MaskKey& m : e.mask)
            if (std::fabs(m.time - t) <= 1e-12) return m;
        return e.mask.back();
    }
    void setPathsInDrag(const std::vector<MaskPath>& paths)
    {
        const uint64_t id = eventId_;
        const double t = now_, tl = maskTol();
        app_.dragUpdate([id, t, tl, paths](Project& p) {
            Event* e = p.findEvent(id);
            if (!e) return;
            e->maskOn = true;
            maskKeyForEdit(*e, t, tl).paths = paths;
        });
    }
    void editPaths(const std::string& what, const std::function<void(std::vector<MaskPath>&)>& fn)
    {
        const uint64_t id = eventId_;
        const double t = now_, tl = maskTol();
        app_.edit(what, [id, t, tl, fn](Project& p) {
            Event* e = p.findEvent(id);
            if (!e) return;
            fn(maskKeyForEdit(*e, t, tl).paths);
        });
    }
    void editPath(const std::string& what, const std::function<void(MaskPath&)>& fn)
    {
        if (refreshingMask_) return;
        const int sel = selPath_;
        if (sel < 0 || sel >= (int)pathsNow().size()) return;
        editPaths(what, [sel, fn](std::vector<MaskPath>& ps) { if (sel < (int)ps.size()) fn(ps[(size_t)sel]); });
    }

    QPointF fracOf(const QPointF& v) const
    {
        const QRectF a = view_->area();
        return QPointF((v.x() - a.left()) / std::max(1.0, a.width()), (v.y() - a.top()) / std::max(1.0, a.height()));
    }
    QPointF viewOf(double x, double y) const { return view_->toView(x, y); }
    QPainterPath viewPath(const MaskPath& m) const
    {
        QPainterPath pp;
        const size_t n = m.points.size();
        if (n == 0) return pp;
        pp.moveTo(viewOf(m.points[0].x, m.points[0].y));
        const size_t segs = m.closed ? n : n - 1;
        for (size_t i = 0; i < segs; ++i) {
            const MaskPoint& a = m.points[i];
            const MaskPoint& b = m.points[(i + 1) % n];
            pp.cubicTo(viewOf(a.x + a.outX, a.y + a.outY), viewOf(b.x + b.inX, b.y + b.inY), viewOf(b.x, b.y));
        }
        if (m.closed) pp.closeSubpath();
        return pp;
    }

    void paintMask(QPainter& p)
    {
        const Event* e = theEvent();
        if (!e || (row_ != MaskRow && !e->maskOn)) return;
        const std::vector<MaskPath> ps = pathsNow();
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        for (size_t i = 0; i < ps.size(); ++i) {
            const MaskPath& m = ps[i];
            const bool sel = (int)i == selPath_ && row_ == MaskRow;
            const QColor col = !m.enabled ? QColor(150, 150, 150) : m.negative ? QColor(255, 90, 80) : QColor(255, 214, 60);
            const QPainterPath pp = viewPath(m);
            if (m.closed && e->maskOn) p.fillPath(pp, QColor(col.red(), col.green(), col.blue(), sel ? 46 : 26));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 170), sel ? 3.0 : 2.4));
            p.drawPath(pp);
            p.setPen(QPen(col, sel ? 1.6 : 1.1, m.closed ? Qt::SolidLine : Qt::DashLine));
            p.drawPath(pp);
            if (row_ != MaskRow) continue;
            QRectF tb;
            if (sel && transformBox(ps, tb)) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(220, 220, 225, 170), 1, Qt::DashLine));
                p.drawRect(tb);
                p.drawLine(QPointF(tb.center().x(), tb.top()), rotateHandle(tb));
                p.setPen(QPen(Qt::black, 1));
                p.setBrush(QColor(255, 214, 60));
                for (int c = 0; c < 4; ++c) p.drawRect(QRectF(boxCorner(tb, c) - QPointF(3.5, 3.5), QSizeF(7, 7)));
                p.drawEllipse(rotateHandle(tb), 4.5, 4.5);
            }
            for (size_t j = 0; j < m.points.size(); ++j) {
                const MaskPoint& q = m.points[j];
                const QPointF c = viewOf(q.x, q.y);
                const bool selPt = sel && (int)j == selPoint_;
                if (selPt || (sel && tool_ == ToolTangent)) {
                    p.setPen(QPen(QColor(220, 220, 225), 1));
                    for (const QPointF& h : {viewOf(q.x + q.inX, q.y + q.inY), viewOf(q.x + q.outX, q.y + q.outY)}) {
                        if (QLineF(h, c).length() < 0.5) continue;
                        p.drawLine(c, h);
                        p.setBrush(QColor(60, 160, 255));
                        p.drawEllipse(h, 3.5, 3.5);
                        p.setBrush(Qt::NoBrush);
                    }
                }
                p.setPen(QPen(Qt::black, 1));
                p.setBrush(selPt ? QColor(60, 160, 255) : (j == 0 && !m.closed ? QColor(120, 255, 120) : QColor(255, 255, 255)));
                p.drawRect(QRectF(c.x() - 3.5, c.y() - 3.5, 7, 7));
            }
        }
        p.restore();
    }

    // The selected shape's transform box (VEGAS's handles round a selected mask): its points' bounds in the view, a
    // little larger so the corners do not sit on the shape's own points; the rotation handle is above its top.
    bool transformBox(const std::vector<MaskPath>& ps, QRectF& box) const
    {
        if (row_ != MaskRow || tool_ != ToolNormal || selPath_ < 0 || selPath_ >= (int)ps.size() || ps[(size_t)selPath_].points.empty()) return false;
        double l = 1e9, t = 1e9, r = -1e9, b = -1e9;
        for (const MaskPoint& q : ps[(size_t)selPath_].points) {
            const QPointF v = viewOf(q.x, q.y);
            l = std::min(l, v.x()); r = std::max(r, v.x()); t = std::min(t, v.y()); b = std::max(b, v.y());
        }
        box = QRectF(QPointF(l, t), QPointF(r, b)).adjusted(-9, -9, 9, 9);
        return true;
    }
    static QPointF boxCorner(const QRectF& b, int c) { return c == 0 ? b.topLeft() : c == 1 ? b.topRight() : c == 2 ? b.bottomRight() : b.bottomLeft(); }
    static QPointF rotateHandle(const QRectF& b) { return QPointF(b.center().x(), b.top() - 18); }

    Drag hitTest(const QPointF& at, const std::vector<MaskPath>& ps, int& path, int& point) const
    {
        path = point = -1;
        const double tolPx = 7.0;
        if (selPath_ >= 0 && selPath_ < (int)ps.size() && selPoint_ >= 0 && selPoint_ < (int)ps[(size_t)selPath_].points.size()) {
            const MaskPoint& q = ps[(size_t)selPath_].points[(size_t)selPoint_];
            const QPointF c = viewOf(q.x, q.y);
            const QPointF hin = viewOf(q.x + q.inX, q.y + q.inY), hout = viewOf(q.x + q.outX, q.y + q.outY);
            if (QLineF(hout, c).length() > 0.5 && QLineF(hout, at).length() <= tolPx) { path = selPath_; point = selPoint_; return DragOut; }
            if (QLineF(hin, c).length() > 0.5 && QLineF(hin, at).length() <= tolPx) { path = selPath_; point = selPoint_; return DragIn; }
        }
        QRectF tb;
        if (transformBox(ps, tb)) {
            if (QLineF(rotateHandle(tb), at).length() <= tolPx) { path = selPath_; return DragRotate; }
            for (int c = 0; c < 4; ++c)
                if (QLineF(boxCorner(tb, c), at).length() <= tolPx) { path = selPath_; point = -1; scaleCorner_ = c; return DragScale; }
        }
        for (int i = (int)ps.size() - 1; i >= 0; --i)
            for (size_t j = 0; j < ps[(size_t)i].points.size(); ++j) {
                const MaskPoint& q = ps[(size_t)i].points[j];
                if (QLineF(viewOf(q.x, q.y), at).length() <= tolPx) { path = i; point = (int)j; return DragPoint; }
            }
        for (int i = (int)ps.size() - 1; i >= 0; --i)
            if (ps[(size_t)i].closed && viewPath(ps[(size_t)i]).contains(at)) { path = i; return DragPath; }
        return DragNone;
    }

    int openPathIndex(const std::vector<MaskPath>& ps) const
    {
        for (size_t i = 0; i < ps.size(); ++i)
            if (!ps[i].closed) return (int)i;
        return -1;
    }
    void finishOpenPath()
    {
        const Event* e = theEvent();
        if (!e || !view_) return;
        bool any = false;
        for (const MaskKey& k : e->mask)
            for (const MaskPath& m : k.paths)
                if (!m.closed) any = true;
        if (!any) return;
        const uint64_t id = eventId_;
        app_.edit("Mask Path", [id](Project& p) {
            Event* ev = p.findEvent(id);
            if (!ev) return;
            for (MaskKey& k : ev->mask)
                for (size_t i = k.paths.size(); i-- > 0;) {
                    if (k.paths[i].closed) continue;
                    if (k.paths[i].points.size() >= 3) k.paths[i].closed = true;
                    else k.paths.erase(k.paths.begin() + (long)i);
                }
        });
    }

    bool maskPress(QMouseEvent* e)
    {
        if (row_ != MaskRow) return false;
        const Event* ev = theEvent();
        if (!ev) return true;
        const QPointF at = e->position();
        std::vector<MaskPath> ps = pathsNow();
        if (e->button() == Qt::RightButton) {
            int path = -1, point = -1;
            if (hitTest(at, ps, path, point) != DragNone) { selPath_ = path; selPoint_ = point; }
            maskMenu(e->globalPosition().toPoint());
            return true;
        }
        if (e->button() != Qt::LeftButton) return true;
        view_->setFocus();
        start_ = ps;
        pressFrac_ = fracOf(at);
        press_ = at;
        drag_ = DragNone;
        const bool alt = (e->modifiers() & Qt::AltModifier) != 0;
        switch (tool_) {
        case ToolNormal: case ToolTangent: {
            int path = -1, point = -1;
            const Drag d = hitTest(at, ps, path, point);
            if (tool_ == ToolNormal && (e->modifiers() & Qt::AltModifier) && (e->modifiers() & Qt::ShiftModifier)) {
                selPath_ = path >= 0 && path != selPath_ ? path : -1;
                selPoint_ = -1;
                refresh();
                return true;
            }
            selPath_ = path;
            selPoint_ = point;
            if (d == DragNone) { refresh(); return true; }
            drag_ = d;
            dupDrag_ = d == DragPath && (e->modifiers() & Qt::ControlModifier);
            if (d == DragScale || d == DragRotate) {
                QRectF tb;
                transformBox(ps, tb);
                startBox_ = tb;
                dragName_ = d == DragScale ? "Scale Mask" : "Rotate Mask";
                break;
            }
            if (tool_ == ToolTangent && d == DragPoint) drag_ = DragNewPoint;
            mirror_ = tool_ == ToolNormal && !alt;
            dragName_ = d == DragPath ? (dupDrag_ ? "Duplicate Mask" : "Move Mask") : d == DragPoint ? "Move Mask Point" : "Mask Point Handles";
            break;
        }
        case ToolDelete: {
            int path = -1, point = -1;
            if (hitTest(at, ps, path, point) == DragPoint) {
                selPath_ = -1;
                selPoint_ = -1;
                editPaths("Delete Mask Point", [path, point](std::vector<MaskPath>& v) {
                    if (path >= (int)v.size() || point >= (int)v[(size_t)path].points.size()) return;
                    MaskPath& m = v[(size_t)path];
                    m.points.erase(m.points.begin() + point);
                    if (m.points.size() < (m.closed ? 3u : 1u)) v.erase(v.begin() + path);
                });
            }
            return true;
        }
        case ToolAnchor: {
            int open = openPathIndex(ps);
            if (open >= 0 && ps[(size_t)open].points.size() >= 3) {
                const MaskPoint& f = ps[(size_t)open].points[0];
                if (QLineF(viewOf(f.x, f.y), at).length() <= 7.0) {
                    editPaths("Close Mask Path", [open](std::vector<MaskPath>& v) { if (open < (int)v.size()) v[(size_t)open].closed = true; });
                    selPath_ = open;
                    selPoint_ = -1;
                    return true;
                }
            }
            MaskPoint q;
            q.x = pressFrac_.x();
            q.y = pressFrac_.y();
            if (open < 0) {
                MaskPath m;
                m.closed = false;
                ps.push_back(m);
                open = (int)ps.size() - 1;
            }
            ps[(size_t)open].points.push_back(q);
            selPath_ = open;
            selPoint_ = (int)ps[(size_t)open].points.size() - 1;
            start_ = ps;
            drag_ = DragNewPoint;
            mirror_ = true;
            dragName_ = "Add Mask Point";
            app_.beginDrag();
            dragging_ = true;
            setPathsInDrag(ps);
            return true;
        }
        case ToolRect: case ToolOval: {
            drag_ = DragShape;
            dragName_ = tool_ == ToolRect ? "Rectangle Mask" : "Oval Mask";
            selPath_ = (int)ps.size();
            selPoint_ = -1;
            break;
        }
        }
        if (drag_ != DragNone) {
            app_.beginDrag();
            dragging_ = true;
        }
        return true;
    }

    bool maskMove(QMouseEvent* e)
    {
        if (row_ != MaskRow) return false;
        if (drag_ == DragNone || !dragging_ || !(e->buttons() & Qt::LeftButton)) return true;
        const QPointF f = fracOf(e->position());
        const double dx = f.x() - pressFrac_.x(), dy = f.y() - pressFrac_.y();
        std::vector<MaskPath> ps = start_;
        const bool shift = (e->modifiers() & Qt::ShiftModifier) != 0, ctrl = (e->modifiers() & Qt::ControlModifier) != 0;
        if (drag_ == DragShape) {
            const QSizeF sp = space();
            const double aspect = sp.width() / std::max(1.0, sp.height());
            double x0 = pressFrac_.x(), y0 = pressFrac_.y(), w = dx, h = dy;
            if (shift) {
                const double side = std::max(std::fabs(w) * aspect, std::fabs(h));
                w = (w < 0 ? -1 : 1) * side / aspect;
                h = (h < 0 ? -1 : 1) * side;
            }
            double l = std::min(x0, x0 + w), t = std::min(y0, y0 + h), r = std::max(x0, x0 + w), b = std::max(y0, y0 + h);
            if (ctrl) { l = x0 - std::fabs(w); r = x0 + std::fabs(w); t = y0 - std::fabs(h); b = y0 + std::fabs(h); }
            if (r - l < 1e-4 || b - t < 1e-4) return true;
            const bool alt = (e->modifiers() & Qt::AltModifier) != 0;
            if (tool_ == ToolRect) ps.push_back(alt ? roundRectPath(l, t, r, b, sp.width(), sp.height()) : rectPath(l, t, r, b));
            else ps.push_back(alt ? halfOvalPath(l, t, r, b) : ovalPath(l, t, r, b));
            setPathsInDrag(ps);
            return true;
        }
        if (selPath_ < 0 || selPath_ >= (int)ps.size()) return true;
        if (drag_ == DragPath && dupDrag_) {
            ps.push_back(ps[(size_t)selPath_]);
        }
        MaskPath& m = drag_ == DragPath && dupDrag_ ? ps.back() : ps[(size_t)selPath_];
        if (drag_ == DragScale || drag_ == DragRotate) {
            transformMask(m, e->position(), shift, ctrl);
        } else if (drag_ == DragPath) {
            double mx = dx, my = dy;
            if (shift) { if (std::fabs(mx) > std::fabs(my)) my = 0; else mx = 0; }
            for (MaskPoint& q : m.points) { q.x += mx; q.y += my; }
        } else if (selPoint_ >= 0 && selPoint_ < (int)m.points.size()) {
            MaskPoint& q = m.points[(size_t)selPoint_];
            if (drag_ == DragPoint) {
                q.x += dx;
                q.y += dy;
            } else if (drag_ == DragNewPoint) {
                q.outX = f.x() - q.x;
                q.outY = f.y() - q.y;
                q.inX = -q.outX;
                q.inY = -q.outY;
            } else {
                double& hx = drag_ == DragOut ? q.outX : q.inX;
                double& hy = drag_ == DragOut ? q.outY : q.inY;
                hx += dx;
                hy += dy;
                if (mirror_) {
                    // the other handle stays opposite (a smooth point), keeping its own length
                    double& ox = drag_ == DragOut ? q.inX : q.outX;
                    double& oy = drag_ == DragOut ? q.inY : q.outY;
                    const double len = std::hypot(ox, oy), hl = std::hypot(hx, hy);
                    if (hl > 1e-9 && len > 1e-9) { ox = -hx / hl * len; oy = -hy / hl * len; }
                }
            }
        }
        setPathsInDrag(ps);
        return true;
    }

    // The shape scaled (a corner of its transform box dragged: the opposite corner stays, or with Ctrl the centre; Shift
    // keeps its proportions) or turned (the handle above: about the box's centre, Shift in 15 degree steps), from where
    // it was when the drag began.
    void transformMask(MaskPath& m, const QPointF& at, bool shift, bool ctrl)
    {
        const QRectF a = view_->area();
        const double aw = std::max(1.0, a.width()), ah = std::max(1.0, a.height());
        const QRectF& b = startBox_;
        if (drag_ == DragRotate) {
            const QPointF c = b.center();
            double d = (std::atan2(at.y() - c.y(), at.x() - c.x()) - std::atan2(press_.y() - c.y(), press_.x() - c.x())) * 180.0 / kPi;
            if (shift) d = std::floor(d / 15.0 + 0.5) * 15.0;
            const double r = d * kPi / 180.0, cs = std::cos(r), sn = std::sin(r);
            const QPointF cf = fracOf(c);
            auto turn = [&](double& x, double& y, bool vec) {
                const double px = (x - (vec ? 0 : cf.x())) * aw, py = (y - (vec ? 0 : cf.y())) * ah;
                x = (vec ? 0 : cf.x()) + (cs * px - sn * py) / aw;
                y = (vec ? 0 : cf.y()) + (sn * px + cs * py) / ah;
            };
            for (MaskPoint& q : m.points) {
                turn(q.x, q.y, false);
                turn(q.inX, q.inY, true);
                turn(q.outX, q.outY, true);
            }
            return;
        }
        const QPointF corner = boxCorner(b, scaleCorner_);
        const QPointF anchor = ctrl ? b.center() : boxCorner(b, (scaleCorner_ + 2) % 4);
        const double ox = corner.x() - anchor.x(), oy = corner.y() - anchor.y();
        double sx = std::fabs(ox) > 1e-6 ? (at.x() - anchor.x()) / ox : 1.0;
        double sy = std::fabs(oy) > 1e-6 ? (at.y() - anchor.y()) / oy : 1.0;
        if (shift) sx = sy = std::fabs(sx) > std::fabs(sy) ? sx : sy;
        if (std::fabs(sx) < 0.02) sx = sx < 0 ? -0.02 : 0.02;
        if (std::fabs(sy) < 0.02) sy = sy < 0 ? -0.02 : 0.02;
        const QPointF af = fracOf(anchor);
        for (MaskPoint& q : m.points) {
            q.x = af.x() + (q.x - af.x()) * sx;
            q.y = af.y() + (q.y - af.y()) * sy;
            q.inX *= sx; q.outX *= sx;
            q.inY *= sy; q.outY *= sy;
        }
    }

    bool maskRelease(QMouseEvent* e)
    {
        if (row_ != MaskRow) return false;
        if (e->button() != Qt::LeftButton || drag_ == DragNone) return true;
        if (drag_ == DragPath && dupDrag_ && dragging_) selPath_ = (int)pathsNow().size() - 1;
        drag_ = DragNone;
        dupDrag_ = false;
        if (dragging_) {
            dragging_ = false;
            app_.endDrag(dragName_);
        }
        return true;
    }

    static MaskPath rectPath(double l, double t, double r, double b)
    {
        MaskPath m;
        for (const auto& c : {std::make_pair(l, t), std::make_pair(r, t), std::make_pair(r, b), std::make_pair(l, b)}) {
            MaskPoint q;
            q.x = c.first;
            q.y = c.second;
            m.points.push_back(q);
        }
        return m;
    }
    static MaskPath roundRectPath(double l, double t, double r, double b, double sw, double sh)
    {
        const double R = 0.2 * std::min((r - l) * sw, (b - t) * sh), rx = R / std::max(1.0, sw), ry = R / std::max(1.0, sh), k = 0.5523;
        MaskPath m;
        auto pt = [](double x, double y, double ix, double iy, double ox, double oy) {
            MaskPoint q;
            q.x = x; q.y = y; q.inX = ix; q.inY = iy; q.outX = ox; q.outY = oy;
            return q;
        };
        m.points.push_back(pt(l + rx, t, -k * rx, 0, 0, 0));
        m.points.push_back(pt(r - rx, t, 0, 0, k * rx, 0));
        m.points.push_back(pt(r, t + ry, 0, -k * ry, 0, 0));
        m.points.push_back(pt(r, b - ry, 0, 0, 0, k * ry));
        m.points.push_back(pt(r - rx, b, k * rx, 0, 0, 0));
        m.points.push_back(pt(l + rx, b, 0, 0, -k * rx, 0));
        m.points.push_back(pt(l, b - ry, 0, k * ry, 0, 0));
        m.points.push_back(pt(l, t + ry, 0, 0, 0, -k * ry));
        return m;
    }
    static MaskPath halfOvalPath(double l, double t, double r, double b)
    {
        const double cx = (l + r) / 2, rx = (r - l) / 2, ry = b - t, k = 0.5523;
        MaskPath m;
        MaskPoint a, top, c;
        a.x = l; a.y = b; a.outX = 0; a.outY = -k * ry;
        top.x = cx; top.y = t; top.inX = -k * rx; top.outX = k * rx;
        c.x = r; c.y = b; c.inX = 0; c.inY = -k * ry;
        m.points = {a, top, c};
        return m;
    }
    static MaskPath ovalPath(double l, double t, double r, double b)
    {
        // four points with the handles of a circle's Bezier (0.5523 of the radius)
        const double cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2, k = 0.5523;
        MaskPath m;
        auto pt = [](double x, double y, double ix, double iy) {
            MaskPoint q;
            q.x = x; q.y = y; q.inX = ix; q.inY = iy; q.outX = -ix; q.outY = -iy;
            return q;
        };
        m.points.push_back(pt(cx, cy - ry, -k * rx, 0));
        m.points.push_back(pt(cx + rx, cy, 0, -k * ry));
        m.points.push_back(pt(cx, cy + ry, k * rx, 0));
        m.points.push_back(pt(cx - rx, cy, 0, k * ry));
        return m;
    }

    void maskMenu(const QPoint& at)
    {
        QMenu m(this);
        m.setObjectName("maskMenu");
        const int sel = selPath_;
        const bool has = sel >= 0 && sel < (int)pathsNow().size();
        auto add = [&](const QString& text, bool on, const std::string& undo, std::function<void(std::vector<MaskPath>&)> fn) {
            QAction* a = m.addAction(text);
            a->setEnabled(on);
            QObject::connect(a, &QAction::triggered, this, [this, undo, fn] { editPaths(undo, fn); });
        };
        add("Delete", has, "Delete Mask", [sel](std::vector<MaskPath>& v) { if (sel < (int)v.size()) v.erase(v.begin() + sel); });
        add("Duplicate", has, "Duplicate Mask", [sel](std::vector<MaskPath>& v) {
            if (sel >= (int)v.size()) return;
            MaskPath c = v[(size_t)sel];
            for (MaskPoint& q : c.points) { q.x += 0.03; q.y += 0.03; }
            v.push_back(c);
        });
        add("Flip Horizontal", has, "Flip Mask Horizontal", [sel](std::vector<MaskPath>& v) {
            if (sel >= (int)v.size()) return;
            double lo = 1e9, hi = -1e9;
            for (const MaskPoint& q : v[(size_t)sel].points) { lo = std::min(lo, q.x); hi = std::max(hi, q.x); }
            for (MaskPoint& q : v[(size_t)sel].points) { q.x = lo + hi - q.x; q.inX = -q.inX; q.outX = -q.outX; }
        });
        add("Flip Vertical", has, "Flip Mask Vertical", [sel](std::vector<MaskPath>& v) {
            if (sel >= (int)v.size()) return;
            double lo = 1e9, hi = -1e9;
            for (const MaskPoint& q : v[(size_t)sel].points) { lo = std::min(lo, q.y); hi = std::max(hi, q.y); }
            for (MaskPoint& q : v[(size_t)sel].points) { q.y = lo + hi - q.y; q.inY = -q.inY; q.outY = -q.outY; }
        });
        m.addSeparator();
        add("Positive", has, "Mask Path: Mode", [sel](std::vector<MaskPath>& v) { if (sel < (int)v.size()) v[(size_t)sel].negative = false; });
        add("Negative", has, "Mask Path: Mode", [sel](std::vector<MaskPath>& v) { if (sel < (int)v.size()) v[(size_t)sel].negative = true; });
        add("Enabled", has, "Mask Path: Enable", [sel](std::vector<MaskPath>& v) { if (sel < (int)v.size()) v[(size_t)sel].enabled = !v[(size_t)sel].enabled; });
        if (has) {
            m.actions().last()->setCheckable(true);
            m.actions().last()->setChecked(pathsNow()[(size_t)sel].enabled);
        }
        m.addSeparator();
        add("Reset Mask", !pathsNow().empty(), "Reset Mask", [](std::vector<MaskPath>& v) { v.clear(); });
        m.exec(at);
    }

    void refreshMask()
    {
        if (!rowBox_) return;
        const Event* e = theEvent();
        if (!e) return;
        refreshingMask_ = true;
        maskCheck_->setChecked(e->maskOn);
        pickData(rowBox_, QString::number((int)row_));
        pickData(applyFx_, e->maskApplyToFx ? "1" : "0");
        const std::vector<MaskPath> ps = pathsNow();
        if (selPath_ >= (int)ps.size()) { selPath_ = -1; selPoint_ = -1; }
        const bool has = selPath_ >= 0;
        for (QWidget* w : std::initializer_list<QWidget*>{modeBox_, aaBox_, opacityBox_, featherTypeBox_, featherBox_}) w->setEnabled(has);
        if (has) {
            const MaskPath& m = ps[(size_t)selPath_];
            pickData(modeBox_, m.negative ? "1" : "0");
            pickData(aaBox_, m.antiAlias ? "1" : "0");
            pickData(featherTypeBox_, QString::number(m.featherType));
            opacityBox_->showValue(m.opacity * 100.0);
            featherBox_->showValue(m.feather);
        }
        const bool pos = row_ == PositionRow;
        for (QWidget* w : std::initializer_list<QWidget*>{wBox_, hBox_, xBox_, yBox_, rotBox_, pivotXBox_, pivotYBox_}) if (w) w->setEnabled(pos);
        if (row_ == MaskRow && hint_)
            hint_->setText(ps.empty() ? "Mask row: draw a shape with the Rectangle, Oval or Anchor tool."
                                      : QString("Mask row: %1 shape(s). Drag points, handles or a whole shape with the Normal tool; right-click for more.").arg(ps.size()));
        refreshingMask_ = false;
    }

protected:
    bool extraKey(int key, Qt::KeyboardModifiers mods) override
    {
        if (row_ != MaskRow || (mods & (Qt::ControlModifier | Qt::AltModifier))) return false;
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter) {
            finishOpenPath();
            return true;
        }
        if (key == Qt::Key_Delete) {
            const int sel = selPath_, pt = selPoint_;
            if (sel < 0) return true;
            editPaths("Delete Mask", [sel, pt](std::vector<MaskPath>& v) {
                if (sel >= (int)v.size()) return;
                MaskPath& m = v[(size_t)sel];
                if (pt >= 0 && pt < (int)m.points.size() && m.points.size() > 3) m.points.erase(m.points.begin() + pt);
                else v.erase(v.begin() + sel);
            });
            selPoint_ = -1;
            return true;
        }
        return false;
    }
    QStringList extraKeys() const override { return {"Esc", "Return"}; }
private:

    void pictureChanged() override
    {
        const Event* e = theEvent();
        const MediaItem* m = media();
        if (!e || !m) return;
        const double f = fps();
        const double t = e->start + now_;
        double mt = eventMediaTime(*e, m, e->reverse ? t + 1.0 / f : t);
        const int64_t frame = (int64_t)std::floor(mt * 1000.0 + 0.5);
        const std::string key = std::to_string(m->id) + ":" + std::to_string(frame);
        if (key == pictureKey_) return;
        pictureKey_ = key;
        QImage img;
        if (m->generated) {
            if (ofx::Instance* gen = app_.fx.video(m->generator.id)) {
                ofx::RenderOptions ro;
                std::string err;
                ImagePtr pic = gen->render(std::floor(mt * f + 0.5), ro, &err);
                if (pic) img = toQImage(*pic);
            }
        } else if (m->info.hasVideo && !m->offline) {
            ImagePtr pic = pool_.videoFrame(eventId_ ^ 0x4000000000000000ull, *m, mt, 960, 540);
            if (pic) img = toQImage(*pic);
        }
        view_->picture = img;
        view_->emptyText = m->offline ? "Media offline" : "No picture";
    }

    uint64_t eventId_;
    MediaPool pool_;
    std::string pictureKey_;
    QComboBox *keepAspect_ = nullptr, *stretch_ = nullptr;
    Row row_ = PositionRow;
    Tool tool_ = ToolNormal;
    Drag drag_ = DragNone;
    bool mirror_ = true, refreshingMask_ = false, dupDrag_ = false;
    int selPath_ = -1, selPoint_ = -1;
    mutable int scaleCorner_ = 0;
    QRectF startBox_;
    QPointF press_;
    std::vector<MaskPath> start_;
    QPointF pressFrac_;
    std::string dragName_;
    QToolButton* toolBtns_[6] = {};
    QComboBox *rowBox_ = nullptr, *applyFx_ = nullptr, *modeBox_ = nullptr, *aaBox_ = nullptr, *featherTypeBox_ = nullptr;
    QCheckBox* maskCheck_ = nullptr;
    QLabel* pathGroup_ = nullptr;
    Spin *opacityBox_ = nullptr, *featherBox_ = nullptr;
};

// ---------------------------------------------------------------------------------- Track Motion
// VEGAS's Track Motion with its keyframe rows: Position (the image's box), 2D Shadow and 2D Glow (a blurred copy of its
// shape in one colour laid under it; their boxes are shown and edited where they fall in the frame, kept in the project
// relative to the image's box). The row chosen under the strip is what the workspace, panel and strip work on. With the
// track in 3D Source Alpha the window is VEGAS's 3D Track Motion: the 3D workspace (View3d) in place of the flat one,
// the Layout drop-down (keys 1 to 6), Prevent Movement Z (Z) and Prevent Scaling Z (Shift+Z), and the 3D values in the
// panel (Position Z, Depth, Rotation X / Y, Orientation, Rotation Offset); 2D Shadow and Glow are 2D only. The same
// window edits a compositing parent's Parent Motion ('parent'): its keys move the whole group, and its compositing
// drop-down is the Parent Composite Mode.
class TrackMotionWindow : public BoxWindow {
public:
    enum Row { PositionRow = 0, ShadowRow = 1, GlowRow = 2 };

    TrackMotionWindow(QWidget* parent, Playback* playback, uint64_t trackId, bool parentMotion = false)
        : BoxWindow(parent, playback), trackId_(trackId), parent_(parentMotion)
    {
        setObjectName(parent_ ? "parentMotionWindow" : "trackMotionWindow");
        buildTop();
        view_->cropStyle = false;
        mode_ = new QComboBox(this);
        mode_->setObjectName("compositeMode");
        for (const CompositeMode& m : compositeModes()) mode_->addItem(m.label, QString(m.id));
        mode_->setToolTip(parent_ ? "How this track and its compositing children are laid over the tracks below"
                                  : "How this track is laid over the tracks below it");
        toolbar_->insertWidget(0, new QLabel(parent_ ? "Parent Composite Mode:" : "Compositing Mode:", this));
        toolbar_->insertWidget(1, mode_);
        toolbar_->insertSpacing(2, 12);
        QObject::connect(mode_, &QComboBox::currentIndexChanged, this, [this](int) {
            const std::string want = s8(mode_->currentData().toString());
            const Track* t = track();
            if (!t || modeOf(*t) == want) return;
            const uint64_t id = trackId_;
            const bool par = parent_;
            app_.edit(par ? "Parent Composite Mode" : "Compositing Mode", [id, want, par](Project& p) {
                if (Track* tr = p.findTrack(id)) (par ? tr->parentComposite : tr->composite) = want;
            });
        });

        layoutLabel_ = new QLabel("Layout:", this);
        layoutBox_ = combo(this, "layout3d", {{"1  Single Pane", "1"}, {"2  Split - Vertical", "2"}, {"3  Split - Horizontal", "3"},
                                              {"4  Four Panes", "4"}, {"5  Large Pane, Two Beside", "5"}, {"6  Large Pane, Two Below", "6"}});
        layoutBox_->setToolTip("Number and arrangement of 3D workspace panes (keys 1 to 6)");
        layoutBox_->setFocusPolicy(Qt::NoFocus);
        toolbar_->insertWidget(3, layoutLabel_);
        toolbar_->insertWidget(4, layoutBox_);
        toolbar_->insertSpacing(5, 12);
        moveZBtn_ = toolButton(this, "Lock Z", "Prevent Movement (Z): the box does not move nearer or further", "preventMoveZ", true);
        scaleZBtn_ = toolButton(this, "Fix Depth", "Prevent Scaling (Shift+Z): the box's depth stays", "preventScaleZ", true);
        for (QToolButton* b : {moveZBtn_, scaleZBtn_}) b->setFocusPolicy(Qt::NoFocus);
        toolbar_->insertWidget(toolbar_->indexOf(moveYBtn_) + 1, moveZBtn_);
        toolbar_->insertWidget(toolbar_->indexOf(scaleYBtn_) + 1, scaleZBtn_);
        view3d_ = new View3d(this);
        view3d_->setUp = [this](Pane3d* p) {
            p->current = [this] { return boxNow(); };
            p->scene = [this] { return scene3d(); };
            p->switches = [this] { return switches3d(); };
            p->onBegin = [this] { beginBoxDrag(); };
            p->onDrag = [this](const Box& b) { boxDragged(b); };
            p->onEnd = [this] { endBoxDrag(); };
            p->onMenu = [this](Pane3d* pane, const QPoint& at) { paneMenu(pane, at); };
        };
        view3d_->setLayoutNumber(1);
        view3d_->hide();
        middle_->addWidget(view3d_, 1);
        QObject::connect(layoutBox_, &QComboBox::activated, this, [this](int i) { setLayout3d(i + 1); });

        std::vector<QWidget*>& d3 = rows3d_;
        auto spin = [&](const char* name, double lo, double hi, int decimals, const QString& label, const QString& suffix) {
            Spin* sp = new Spin(panel_, lo, hi, decimals, 1, name);
            if (!suffix.isEmpty()) sp->setSuffix(suffix);
            d3.push_back(panel_->add(label, sp));
            d3.push_back(sp);
            return sp;
        };
        const QString deg = QString::fromUtf8("\xc2\xb0");
        d3.push_back(panel_->group("3D Position"));
        zBox_ = spin("boxZ", -1.0e6, 1.0e6, 1, "Z Center", QString());
        zBox_->setToolTip("Distance of the picture's centre from the frame plane towards the camera, in pixels (negative: further away)");
        depthBox_ = spin("boxDepth", 1, 1.0e6, 1, "Depth", QString());
        d3.push_back(panel_->group("3D Rotation"));
        rxBox_ = spin("boxRotX", -36000, 36000, 1, "X", deg);
        ryBox_ = spin("boxRotY", -36000, 36000, 1, "Y", deg);
        QLabel* zNote = new QLabel("the Angle above", panel_);
        d3.push_back(panel_->add("Z", zNote));
        d3.push_back(zNote);
        d3.push_back(panel_->group("Orientation"));
        oxBox_ = spin("boxOriX", -36000, 36000, 1, "X", deg);
        oyBox_ = spin("boxOriY", -36000, 36000, 1, "Y", deg);
        ozBox_ = spin("boxOriZ", -36000, 36000, 1, "Z", deg);
        d3.push_back(panel_->group("Rotation Offset"));
        fxBox_ = spin("boxOffX", -1.0e6, 1.0e6, 1, "X", QString());
        fyBox_ = spin("boxOffY", -1.0e6, 1.0e6, 1, "Y", QString());
        fzBox_ = spin("boxOffZ", -1.0e6, 1.0e6, 1, "Z", QString());
        const std::pair<Spin*, int> fields[] = {{zBox_, 0}, {depthBox_, 1}, {rxBox_, 2}, {ryBox_, 3}, {oxBox_, 4}, {oyBox_, 5},
                                                {ozBox_, 6}, {fxBox_, 7}, {fyBox_, 8}, {fzBox_, 9}};
        for (const auto& f : fields) {
            const int which = f.second;
            QObject::connect(f.first, &QDoubleSpinBox::valueChanged, this, [this, which](double v) { number3dEdited(which, v); });
        }

        rows2d_.push_back(panel_->group("2D Shadow / Glow"));
        effectOn_ = new QCheckBox("On", panel_);
        effectOn_->setObjectName("effectOn");
        rows2d_.push_back(panel_->add("Shadow / Glow", effectOn_));
        rows2d_.push_back(effectOn_);
        blurBox_ = new Spin(panel_, 0, 100, 0, 5, "effectBlur");
        blurBox_->setSuffix(" %");
        rows2d_.push_back(panel_->add("Blur", blurBox_));
        rows2d_.push_back(blurBox_);
        intensityBox_ = new Spin(panel_, 0, 100, 0, 5, "effectIntensity");
        intensityBox_->setSuffix(" %");
        rows2d_.push_back(panel_->add("Intensity", intensityBox_));
        rows2d_.push_back(intensityBox_);
        colorBtn_ = new QToolButton(panel_);
        colorBtn_->setObjectName("effectColor");
        colorBtn_->setToolTip("The colour of the shadow or the glow");
        colorBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        rows2d_.push_back(panel_->add("Color", colorBtn_));
        rows2d_.push_back(colorBtn_);
        QObject::connect(effectOn_, &QCheckBox::toggled, this, [this](bool on) { setEffectOn(on); });
        QObject::connect(blurBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (row_ != PositionRow) changeBox([v](Box& b) { b.blur = v / 100.0; });
        });
        QObject::connect(intensityBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (row_ != PositionRow) changeBox([v](Box& b) { b.intensity = v / 100.0; });
        });
        QObject::connect(colorBtn_, &QToolButton::clicked, this, [this] {
            const Box b = boxNow();
            const QColor c = QColorDialog::getColor(QColor::fromRgba(b.color), this, "2D Shadow / Glow Color", QColorDialog::ShowAlphaChannel);
            if (c.isValid()) setEffectColor(c.rgba());
        });
        buildRest();

        QHBoxLayout* keysRow = nullptr;
        if (QVBoxLayout* v = static_cast<QVBoxLayout*>(layout()))
            keysRow = qobject_cast<QHBoxLayout*>(v->itemAt(v->count() - 1)->layout());
        const char* names[3] = {"Position", "2D Shadow", "2D Glow"};
        const char* ids[3] = {"rowPosition", "rowShadow", "rowGlow"};
        for (int r = 0; r < 3; ++r) {
            rows_[r] = toolButton(this, names[r], QString("Show and edit the %1 keyframes").arg(names[r]), ids[r], true);
            rows_[r]->setFocusPolicy(Qt::NoFocus);
            if (keysRow) keysRow->insertWidget(r, rows_[r]);
            QObject::connect(rows_[r], &QToolButton::clicked, this, [this, r] { setRow((Row)r); });
        }
        if (keysRow) keysRow->insertSpacing(3, 12);
        rows_[0]->setChecked(true);
        buildPresets();
        shownOnce_ = false;
        refresh();
    }


    // The right-click menu: Restore, Center, the flips, and VEGAS's shapes: the output's (the frame's), the source
    // media's (the event under the cursor on the track, else its first one) and a square, each keeping the height.
    std::vector<std::pair<QString, std::function<void(Box&)>>> boxActions() const override
    {
        auto v = BoxWindow::boxActions();
        const QSizeF sp = space();
        const double sw = std::max(1.0, sp.width()), sh = std::max(1.0, sp.height());
        double source = sw / sh;
        if (const Track* t = track()) {
            const Project& p = app_.project();
            const double now = startOnTimeline() + now_;
            const Event* pick = nullptr;
            for (const Event& e : t->events) {
                if (!pick) pick = &e;
                if (e.start <= now && now < e.end()) { pick = &e; break; }
            }
            const MediaItem* m = pick ? p.findMedia(pick->mediaId) : nullptr;
            if (m && !m->generated && m->info.width > 0 && m->info.height > 0) source = (double)m->info.width / m->info.height;
        }
        auto shape = [sw, sh](double aspect) {
            return [sw, sh, aspect](Box& b) {
                const double sx = b.w < 0 ? -1 : 1;
                b.w = sx * std::fabs(b.h) * aspect * sh / sw;
            };
        };
        v.push_back({"Match Output Aspect", shape(sw / sh)});
        v.push_back({"Match Source Aspect", shape(source)});
        v.push_back({"Square Aspect", shape(1.0)});
        return v;
    }

    void setRow(Row r)
    {
        if (parent_ || is3d()) r = PositionRow;
        row_ = r;
        for (int k = 0; k < 3; ++k) rows_[k]->setChecked(k == (int)r);
        view_->rotation = r == PositionRow && rotateBtn_->isChecked();
        setProperty("row", (int)r);
        refresh();
    }

    void setLayout3d(int n)
    {
        view3d_->setLayoutNumber(n);
        QSignalBlocker b(layoutBox_);
        layoutBox_->setCurrentIndex(view3d_->layoutNumber() - 1);
        setProperty("layout3d", view3d_->layoutNumber());
    }

private:
    const Track* track() const { return const_cast<Project&>(app_.project()).findTrack(trackId_); }
    bool exists() const override { return track() != nullptr; }
    const std::string& modeOf(const Track& t) const { return parent_ ? t.parentComposite : t.composite; }
    const std::vector<MotionKey>& motionOf(const Track& t) const { return parent_ ? t.parentMotion : t.motion; }
    bool is3d() const
    {
        const Track* t = track();
        return t && modeOf(*t) == "3dsrcalpha";
    }

    static Box motionBox(const Track& t, double time)
    {
        std::vector<Box> v;
        for (const MotionKey& k : t.motion) v.push_back(boxOfKey(k));
        sortKeys(v);
        return boxAt(v, time);
    }
    const std::vector<GlowKey>* glowKeys(const Track& t) const { return row_ == ShadowRow ? &t.shadow2d : &t.glow2d; }

    std::vector<Box> readKeys(const Project& p) const override
    {
        std::vector<Box> v;
        const Track* t = const_cast<Project&>(p).findTrack(trackId_);
        if (!t) return v;
        if (row_ == PositionRow) {
            for (const MotionKey& k : motionOf(*t)) v.push_back(boxOfKey(k));
            return v;
        }
        for (const GlowKey& g : *glowKeys(*t)) {
            const Box m = motionBox(*t, g.time);
            Box b;
            b.time = g.time; b.curve = g.curve;
            b.cx = m.cx + g.x; b.cy = m.cy + g.y; b.w = m.w * g.w; b.h = m.h * g.h; b.rot = m.rot;
            b.blur = g.blur; b.intensity = g.intensity; b.color = g.color;
            v.push_back(b);
        }
        return v;
    }
    void writeKeys(Project& p, const std::vector<Box>& keys) const override
    {
        Track* t = p.findTrack(trackId_);
        if (!t) return;
        if (row_ == PositionRow) {
            std::vector<MotionKey>& out = parent_ ? t->parentMotion : t->motion;
            out.clear();
            for (const Box& b : keys) out.push_back(keyOfBox(b));
            return;
        }
        std::vector<GlowKey> out;
        for (const Box& b : keys) {
            const Box m = motionBox(*t, b.time);
            GlowKey g;
            g.time = b.time; g.curve = b.curve;
            g.x = b.cx - m.cx; g.y = b.cy - m.cy;
            g.w = std::fabs(m.w) > 1e-9 ? b.w / m.w : 1.0;
            g.h = std::fabs(m.h) > 1e-9 ? b.h / m.h : 1.0;
            g.blur = std::min(1.0, std::max(0.0, b.blur));
            g.intensity = std::min(1.0, std::max(0.0, b.intensity));
            g.color = b.color;
            out.push_back(g);
        }
        (row_ == ShadowRow ? t->shadow2d : t->glow2d) = out;
    }
    Box defaultBox(double time) const override
    {
        if (row_ == PositionRow) return Box();
        const Track* t = track();
        const Box m = t ? motionBox(*t, time) : Box();
        GlowKey g;
        if (row_ == ShadowRow) {
            g.x = 0.02;
            g.y = 0.035;
        } else {
            g.w = g.h = 1.06;
            g.color = 0xFFFFFFFF;
            g.intensity = 0.6;
        }
        Box b = m;
        b.time = time;
        b.cx = m.cx + g.x; b.cy = m.cy + g.y; b.w = m.w * g.w; b.h = m.h * g.h;
        b.blur = g.blur; b.intensity = g.intensity; b.color = g.color; b.curve = 0;
        return b;
    }
    double span() const override { return std::max(10.0, app_.project().length() + 5.0); }
    double startOnTimeline() const override { return 0; }
    QSizeF space() const override
    {
        const ProjectSettings& s = app_.project().settings;
        return QSizeF(s.width * s.pixelAspect, s.height);
    }
    std::string undoName() const override
    {
        if (parent_) return "Parent Motion";
        return row_ == ShadowRow ? "2D Shadow" : row_ == GlowRow ? "2D Glow" : "Track Motion";
    }
    std::string clipKind() const override { return row_ == ShadowRow ? "shadow" : row_ == GlowRow ? "glow" : is3d() ? "motion3d" : "motion"; }
    const char* presetObjectName() const override { return "motionPreset"; }
    std::string presetUndoPrefix() const override { return undoName() + " Preset"; }
    std::vector<std::pair<QString, std::function<void(Box&)>>> builtinPresets() const override
    {
        if (row_ != PositionRow) return {};
        std::vector<std::pair<QString, std::function<void(Box&)>>> v;
        v.push_back({"(Default)", [](Box& b) { const int c = b.curve; const double s = b.smooth; b = Box(); b.curve = c; b.smooth = s; }});
        const struct { const char* name; double x, y; } pip[] = {
            {"Picture in picture, top left", 0.2, 0.2}, {"Picture in picture, top right", 0.8, 0.2},
            {"Picture in picture, bottom left", 0.2, 0.8}, {"Picture in picture, bottom right", 0.8, 0.8}};
        for (const auto& c : pip) {
            const double x = c.x, y = c.y;
            v.push_back({c.name, [x, y](Box& b) { b.cx = x; b.cy = y; b.w = b.w < 0 ? -1.0 / 3.0 : 1.0 / 3.0; b.h = b.h < 0 ? -1.0 / 3.0 : 1.0 / 3.0; b.rot = 0; }});
        }
        return v;
    }
    bool smoothable() const override { return row_ == PositionRow; }
    QString windowName() const override
    {
        const Project& p = app_.project();
        const QString what = parent_ ? "Parent Track Motion" : "Track Motion";
        for (size_t k = 0; k < p.tracks.size(); ++k)
            if (p.tracks[k].id == trackId_)
                return QString("%1: %2").arg(what, p.tracks[k].name.empty() ? QString("Track %1").arg(k + 1) : qs(p.tracks[k].name));
        return what;
    }

    // ---- 3D
    // VEGAS's 3D keys: 1 to 6 the layouts, Z / Shift+Z Prevent Movement / Prevent Scaling Z (only in 3D).
    QStringList extraKeys() const override { return {"1", "2", "3", "4", "5", "6", "Z", "Shift+Z"}; }
    bool extraKey(int key, Qt::KeyboardModifiers mods) override
    {
        if (!is3d()) return false;
        const Qt::KeyboardModifiers m = mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if (key >= Qt::Key_1 && key <= Qt::Key_6 && m == Qt::NoModifier) {
            setLayout3d(key - Qt::Key_0);
            return true;
        }
        if (key == Qt::Key_Z && m == Qt::NoModifier) { moveZBtn_->toggle(); return true; }
        if (key == Qt::Key_Z && m == Qt::ShiftModifier) { scaleZBtn_->toggle(); return true; }
        return false;
    }
    Switches3d switches3d() const
    {
        Switches3d s;
        s.rotation = rotateBtn_->isChecked();
        s.lockAspect = lockBtn_->isChecked();
        s.noMove[0] = moveXBtn_->isChecked();
        s.noMove[1] = moveYBtn_->isChecked();
        s.noMove[2] = moveZBtn_->isChecked();
        s.noScale[0] = scaleXBtn_->isChecked();
        s.noScale[1] = scaleYBtn_->isChecked();
        s.noScale[2] = scaleZBtn_->isChecked();
        return s;
    }
    Scene3d scene3d() const
    {
        Scene3d sc;
        const Project& p = app_.project();
        sc.W = std::max(1, p.settings.width);
        sc.H = std::max(1, p.settings.height);
        sc.par = p.settings.pixelAspect > 0.01 && p.settings.pixelAspect < 100 ? p.settings.pixelAspect : 1.0;
        const double t = startOnTimeline() + now_;
        std::vector<const Track*> vt;
        for (const Track& tr : p.tracks)
            if (tr.type == TrackType::Video) vt.push_back(&tr);
        auto level = [&](size_t i) { return std::max(0, vt[i]->compositeLevel); };
        auto outer = [&](size_t i) {
            std::vector<MotionKey> m;
            int l = level(i);
            for (size_t j = i; j-- > 0 && l > 0;) {
                if (level(j) >= l) continue;
                if (vt[j]->parentComposite != "3dsrcalpha") break;
                m.push_back(motionKeyAt(vt[j]->parentMotion, t));
                l = level(j);
            }
            return m;
        };
        for (size_t i = 0; i < vt.size(); ++i) {
            if (vt[i]->id == trackId_) {
                sc.outer = outer(i);
                continue;
            }
            const bool hasChildren = i + 1 < vt.size() && level(i + 1) > level(i);
            if (hasChildren || vt[i]->adjustment || vt[i]->composite != "3dsrcalpha") continue;
            std::vector<MotionKey> m;
            m.push_back(motionKeyAt(vt[i]->motion, t));
            const std::vector<MotionKey> o = outer(i);
            m.insert(m.end(), o.begin(), o.end());
            sc.others.push_back(m);
        }
        return sc;
    }
    void number3dEdited(int which, double v)
    {
        const double wd = std::max(1.0, space().width());
        changeBox([which, v, wd](Box& b) {
            switch (which) {
            case 0: b.z = v / wd; break;
            case 1: b.d = v / wd; break;
            case 2: b.rx = v; break;
            case 3: b.ry = v; break;
            case 4: b.ox = v; break;
            case 5: b.oy = v; break;
            case 6: b.oz = v; break;
            case 7: b.fx = v / wd; break;
            case 8: b.fy = v / wd; break;
            default: b.fz = v / wd; break;
            }
        });
    }
    // The 3D panes' right-click menu: VEGAS's Restore Box / Center / Rotation / Size, the flips, Restore View and Match Camera View.
    void paneMenu(Pane3d* pane, const QPoint& at)
    {
        QMenu m(this);
        m.setObjectName("paneMenu");
        QPointer<Pane3d> pp(pane);
        auto change = [](const QString& label, std::function<void(Box&)> fn) { return std::make_pair(label, fn); };
        const std::vector<std::pair<QString, std::function<void(Box&)>>> items = {
            change("Restore Box", [](Box& b) { const double t = b.time; const int c = b.curve; b = Box(); b.time = t; b.curve = c; }),
            change("Restore Center", [](Box& b) { b.cx = 0.5; b.cy = 0.5; b.z = 0; }),
            change("Restore Rotation", [](Box& b) { b.rot = 0; b.rx = 0; b.ry = 0; b.ox = b.oy = b.oz = 0; b.fx = b.fy = b.fz = 0; }),
            change("Restore Size", [](Box& b) { b.w = b.w < 0 ? -1 : 1; b.h = b.h < 0 ? -1 : 1; b.d = 1; }),
            change("Flip Horizontal", [](Box& b) { b.w = -b.w; }),
            change("Flip Vertical", [](Box& b) { b.h = -b.h; }),
        };
        for (const auto& it : items) {
            const std::function<void(Box&)> fn = it.second;
            const QString label = it.first;
            QObject::connect(m.addAction(label), &QAction::triggered, this, [this, fn, label] { changeBox(fn, undoName() + ": " + s8(label)); });
        }
        m.addSeparator();
        QObject::connect(m.addAction("Restore View"), &QAction::triggered, this, [pp] { if (pp) pp->restoreView(); });
        QObject::connect(m.addAction("Match Camera View"), &QAction::triggered, this, [pp] { if (pp) pp->setAngle(CameraAngle); });
        m.exec(at);
    }

    void refreshExtra() override
    {
        const Track* t = track();
        if (!t) return;
        pickData(mode_, qs(modeOf(*t)));
        const bool d3 = is3d();
        if (d3 != shown3d_ || !shownOnce_) {
            shown3d_ = d3;
            shownOnce_ = true;
            if (d3 && row_ != PositionRow) setRow(PositionRow);
            view_->setVisible(!d3);
            view3d_->setVisible(d3);
            layoutLabel_->setVisible(d3);
            layoutBox_->setVisible(d3);
            moveZBtn_->setVisible(d3);
            scaleZBtn_->setVisible(d3);
            snapBtn_->setEnabled(!d3);
            objectBtn_->setEnabled(!d3);
            centreBtn_->setEnabled(!d3);
            for (QWidget* w : rows3d_) if (w) w->setVisible(d3);
            for (QWidget* w : rows2d_) if (w) w->setVisible(!d3 && !parent_);
            for (int k = 1; k < 3; ++k) if (rows_[k]) rows_[k]->setVisible(!d3 && !parent_);
            setProperty("mode3d", d3);
        }
        if (d3) {
            const Box b = boxNow();
            const double wd = std::max(1.0, space().width());
            zBox_->showValue(b.z * wd);
            depthBox_->showValue(std::fabs(b.d) * wd);
            rxBox_->showValue(b.rx);
            ryBox_->showValue(b.ry);
            oxBox_->showValue(b.ox);
            oyBox_->showValue(b.oy);
            ozBox_->showValue(b.oz);
            fxBox_->showValue(b.fx * wd);
            fyBox_->showValue(b.fy * wd);
            fzBox_->showValue(b.fz * wd);
            view3d_->updatePanes();
        }
        const bool fxRow = row_ != PositionRow;
        const bool on = fxRow && !glowKeys(*t)->empty();
        {
            const QSignalBlocker b(effectOn_);
            effectOn_->setChecked(on);
        }
        effectOn_->setEnabled(fxRow);
        blurBox_->setEnabled(on);
        intensityBox_->setEnabled(on);
        colorBtn_->setEnabled(on);
        for (int k = 1; k < 3 && rows_[k]; ++k) {
            const bool has = k == 1 ? !t->shadow2d.empty() : !t->glow2d.empty();
            rows_[k]->setText(QString(k == 1 ? "2D Shadow" : "2D Glow") + (has ? QString::fromUtf8(" \xe2\x9c\x93") : QString()));
        }
        if (on) {
            const Box b = boxNow();
            blurBox_->showValue(b.blur * 100.0);
            intensityBox_->showValue(b.intensity * 100.0);
            const QColor c = QColor::fromRgba(b.color);
            colorBtn_->setText(c.name().toUpper());
            colorBtn_->setStyleSheet(QString("QToolButton { background: %1; color: %2; }").arg(c.name(), c.lightness() > 128 ? "black" : "white"));
        } else {
            colorBtn_->setText(QString());
            colorBtn_->setStyleSheet(QString());
        }
    }

public:
    void setEffectOn(bool on)
    {
        if (row_ == PositionRow) return;
        const Box def = defaultBox(0.0);
        const Row r = row_;
        editKeys(on ? (r == ShadowRow ? "2D Shadow On" : "2D Glow On") : (r == ShadowRow ? "2D Shadow Off" : "2D Glow Off"), [on, def](std::vector<Box>& ks) {
            ks.clear();
            if (on) ks.push_back(def);
        });
    }
    void setEffectColor(uint32_t argb)
    {
        if (row_ != PositionRow) changeBox([argb](Box& b) { b.color = argb; });
    }

private:
    uint64_t trackId_;
    bool parent_ = false;
    QComboBox* mode_ = nullptr;
    Row row_ = PositionRow;
    QToolButton* rows_[3] = {nullptr, nullptr, nullptr};
    QCheckBox* effectOn_ = nullptr;
    Spin *blurBox_ = nullptr, *intensityBox_ = nullptr;
    QToolButton* colorBtn_ = nullptr;
    View3d* view3d_ = nullptr;
    QLabel* layoutLabel_ = nullptr;
    QComboBox* layoutBox_ = nullptr;
    QToolButton *moveZBtn_ = nullptr, *scaleZBtn_ = nullptr;
    Spin *zBox_ = nullptr, *depthBox_ = nullptr, *rxBox_ = nullptr, *ryBox_ = nullptr, *oxBox_ = nullptr, *oyBox_ = nullptr, *ozBox_ = nullptr;
    Spin *fxBox_ = nullptr, *fyBox_ = nullptr, *fzBox_ = nullptr;
    std::vector<QWidget*> rows3d_, rows2d_;
    bool shown3d_ = false, shownOnce_ = false;
};

std::map<uint64_t, QPointer<QWidget>>& panCropWindows()
{
    static std::map<uint64_t, QPointer<QWidget>> all;
    return all;
}
std::map<uint64_t, QPointer<QWidget>>& motionWindows()
{
    static std::map<uint64_t, QPointer<QWidget>> all;
    return all;
}

void raise(QWidget* w)
{
    w->show();
    w->raise();
    w->activateWindow();
}

}

// ================================================================================== entry points
void openPanCrop(QWidget* parent, Playback* playback, uint64_t eventId)
{
    const Track* tr = nullptr;
    if (!AppState::get().project().findEvent(eventId, &tr) || !tr || tr->type != TrackType::Video) return;
    auto& all = panCropWindows();
    auto it = all.find(eventId);
    if (it != all.end() && it->second) { raise(it->second); return; }
    PanCropWindow* w = new PanCropWindow(parent, playback, eventId);
    all[eventId] = w;
    raise(w);
}

std::map<uint64_t, QPointer<QWidget>>& parentMotionWindows()
{
    static std::map<uint64_t, QPointer<QWidget>> all;
    return all;
}

void openParentMotion(QWidget* parent, Playback* playback, uint64_t trackId)
{
    Track* tr = const_cast<Project&>(AppState::get().project()).findTrack(trackId);
    if (!tr || tr->type != TrackType::Video) return;
    auto& all = parentMotionWindows();
    auto it = all.find(trackId);
    if (it != all.end() && it->second) { raise(it->second); return; }
    TrackMotionWindow* w = new TrackMotionWindow(parent, playback, trackId, true);
    all[trackId] = w;
    raise(w);
}

void openTrackMotion(QWidget* parent, Playback* playback, uint64_t trackId)
{
    Track* tr = const_cast<Project&>(AppState::get().project()).findTrack(trackId);
    if (!tr || tr->type != TrackType::Video) return;
    auto& all = motionWindows();
    auto it = all.find(trackId);
    if (it != all.end() && it->second) { raise(it->second); return; }
    TrackMotionWindow* w = new TrackMotionWindow(parent, playback, trackId);
    all[trackId] = w;
    raise(w);
}

}
}
