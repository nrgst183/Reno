// The effects window: the chain of effects on one thing and the controls of the picked effect. The top row shows the chain as buttons
// (tick = on, drag to reorder) with Add / move / Remove beside it. Below, in a scroll area, is one "panel": an OpenFX
// effect (its own window if it has
// one, then a row per parameter), an audio plug-in (its own window, or a slider per parameter), or a line of text (empty
// chain, effect not loaded,
// event gone). Every change is an undo step: controls bracket each change (slider press to release, a click, a colour
// dialog, a burst of typing)
// with AppState::beginFxEdit / endFxEdit. Live plug-in instances belong to AppState's FxRuntime and can be destroyed
// under us (New Project), so a
// panel never trusts its pointer: before every use it asks the runtime again and checks it got the same object
// (OfxPanel::live, AudioPanel::live).
#include "../audiofx/vegaspack1.h"
#include "fxwindow.h"
#include "vprefs.h"
#include "fxlibrary.h"
#include "fxeditors.h"
#include "fxpages.h"
#include "panels.h"
#include "pluginchooser.h"
#include "playback.h"
#include "../audiofx/vegaspack3.h"
#include "../engine/audionews.h"
#include "../engine/fxnames.h"
#include "../ofx/vegas_ext.h"
#include "../audiofx/vegasfx_pack2.h"
#include "strconv.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QInputDialog>
#include <QKeyEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QSettings>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>

namespace reno {
namespace app {

namespace {

// Plug-ins put their own windows inside ours through a window handle, which only means something to them on Windows.
// (RENO_FX_HOST_PLUGIN_WINDOWS switches the same code on elsewhere so tests can use plug-ins that only pretend to make a
// window.)
#if defined(_WIN32) || defined(RENO_FX_HOST_PLUGIN_WINDOWS)
const bool kHostPluginWindows = true;
#else
const bool kHostPluginWindows = false;
#endif

// Positions as fractions of the frame: older OpenFX headers named these, current ones leave them out, but effects made
// for older hosts still use them.
const char* const kNormalisedXY = "OfxParamDoubleTypeNormalisedXY";
const char* const kNormalisedXYAbsolute = "OfxParamDoubleTypeNormalisedXYAbsolute";

const int kIndent = 14;
const int kSliderSteps = 1000;
const int kMaxAudioRows = 300;


// ------------------------------------------------------------------------------------------ small drawings
enum class Glyph { Left, Right, Remove, Reset, Add, First, Last };

QPixmap glyphPixmap(Glyph g, const QColor& colour)
{
    QPixmap pm(32, 32);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(colour, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (g) {
    case Glyph::Left: {
        const QPointF pts[3] = {QPointF(10, 3.5), QPointF(5.5, 8), QPointF(10, 12.5)};
        p.drawPolyline(pts, 3);
        break;
    }
    case Glyph::Right: {
        const QPointF pts[3] = {QPointF(6, 3.5), QPointF(10.5, 8), QPointF(6, 12.5)};
        p.drawPolyline(pts, 3);
        break;
    }
    case Glyph::First:
    case Glyph::Last: {
        const bool first = g == Glyph::First;
        const QPointF pts[3] = {QPointF(first ? 11 : 5, 3.5), QPointF(first ? 6.5 : 9.5, 8), QPointF(first ? 11 : 5, 12.5)};
        p.drawPolyline(pts, 3);
        p.drawLine(QPointF(first ? 4.5 : 11.5, 3.5), QPointF(first ? 4.5 : 11.5, 12.5));
        break;
    }
    case Glyph::Remove:
        p.drawLine(QPointF(4.5, 4.5), QPointF(11.5, 11.5));
        p.drawLine(QPointF(11.5, 4.5), QPointF(4.5, 11.5));
        break;
    case Glyph::Add:
        p.drawLine(QPointF(8, 3.5), QPointF(8, 12.5));
        p.drawLine(QPointF(3.5, 8), QPointF(12.5, 8));
        break;
    case Glyph::Reset: {
        p.drawArc(QRectF(3.5, 3.5, 9, 9), 60 * 16, 270 * 16);
        const QPointF head[3] = {QPointF(7.2, 1.6), QPointF(10.4, 3.9), QPointF(7.0, 5.8)};
        p.drawPolyline(head, 3);
        break;
    }
    }
    return pm;
}

QIcon glyphIcon(Glyph g, const QPalette& pal)
{
    QIcon icon;
    icon.addPixmap(glyphPixmap(g, pal.color(QPalette::Active, QPalette::ButtonText)), QIcon::Normal);
    icon.addPixmap(glyphPixmap(g, pal.color(QPalette::Disabled, QPalette::ButtonText)), QIcon::Disabled);
    return icon;
}

QIcon keyIcon(const QPalette& pal)
{
    QIcon icon;
    for (int on = 0; on < 2; ++on) {
        QPixmap pm(32, 32);
        pm.setDevicePixelRatio(2.0);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor line = on ? QColor(255, 196, 70) : pal.color(QPalette::Active, QPalette::ButtonText);
        p.setPen(QPen(line, 1.4));
        p.setBrush(on ? QBrush(QColor(255, 196, 70)) : QBrush(Qt::NoBrush));
        const QPointF pts[4] = {QPointF(8, 3), QPointF(13, 8), QPointF(8, 13), QPointF(3, 8)};
        p.drawPolygon(pts, 4);
        icon.addPixmap(pm, QIcon::Normal, on ? QIcon::On : QIcon::Off);
    }
    return icon;
}

// Save Preset (floppy disk), Delete Preset (red cross) and help ("?") icons.
QIcon presetIcon(const QPalette& pal, bool save)
{
    QIcon icon;
    for (int mode = 0; mode < 2; ++mode) {
        QPixmap pm(40, 40);
        pm.setDevicePixelRatio(2.0);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor off = pal.color(QPalette::Disabled, QPalette::ButtonText);
        if (save) {
            const QColor body = mode ? off : QColor(38, 112, 190), edge = mode ? off.darker(130) : QColor(14, 52, 100);
            p.setPen(QPen(edge, 1.0));
            p.setBrush(body);
            p.drawRoundedRect(QRectF(2.5, 2.5, 15, 15), 1.5, 1.5);
            p.setPen(Qt::NoPen);
            p.setBrush(mode ? QColor(150, 150, 150) : QColor(236, 240, 246));
            p.drawRect(QRectF(5, 3.5, 10, 5.5));
            p.setBrush(mode ? QColor(110, 110, 110) : QColor(26, 64, 120));
            p.drawRect(QRectF(11.2, 4.4, 2.2, 3.6));
            p.setBrush(mode ? QColor(150, 150, 150) : QColor(232, 236, 244));
            p.drawRect(QRectF(5, 11.5, 10, 5));
        } else {
            p.setPen(QPen(mode ? off : QColor(222, 84, 84), 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(4.5, 4.5), QPointF(15.5, 15.5));
            p.drawLine(QPointF(15.5, 4.5), QPointF(4.5, 15.5));
        }
        icon.addPixmap(pm, mode ? QIcon::Disabled : QIcon::Normal);
    }
    return icon;
}

QIcon helpIcon()
{
    QPixmap pm(40, 40);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 250, 250));
    p.drawEllipse(QRectF(2, 2, 16, 16));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(QColor(30, 30, 30));
    p.drawText(QRectF(2, 2, 16, 16), Qt::AlignCenter, "?");
    return QIcon(pm);
}

QIcon fxIcon(bool add)
{
    QIcon icon;
    for (int mode = 0; mode < 2; ++mode) {
        QPixmap pm(56, 44);
        pm.setDevicePixelRatio(2.0);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f("Times New Roman");
        f.setStyleHint(QFont::Serif);
        f.setItalic(true);
        f.setPixelSize(17);
        p.setFont(f);
        p.setPen(mode ? QColor(110, 110, 110) : QColor(228, 228, 228));
        p.drawText(QRectF(1, 0, 18, 22), Qt::AlignLeft | Qt::AlignVCenter, "fx");
        const QColor c = mode ? QColor(110, 110, 110) : (add ? QColor(84, 200, 84) : QColor(226, 88, 88));
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
        if (add) {
            p.drawLine(QPointF(22, 14), QPointF(27, 14));
            p.drawLine(QPointF(24.5, 11.5), QPointF(24.5, 16.5));
        } else {
            p.drawLine(QPointF(22, 11.5), QPointF(27, 16.5));
            p.drawLine(QPointF(27, 11.5), QPointF(22, 16.5));
        }
        icon.addPixmap(pm, mode ? QIcon::Disabled : QIcon::Normal);
    }
    return icon;
}

QColor chipColor(const Track& t, int index)
{
    if (t.color) return QColor((int)((t.color >> 16) & 0xff), (int)((t.color >> 8) & 0xff), (int)(t.color & 0xff));
    static const QColor colors[] = {QColor(89, 115, 168), QColor(124, 52, 65), QColor(150, 116, 52), QColor(70, 128, 74),
                                    QColor(52, 124, 132), QColor(118, 82, 158), QColor(160, 84, 116), QColor(108, 110, 64)};
    const QStringList own = vprefs::list("prefs/trackColors");
    if (own.size() == 8) {
        const QColor c(own[index % 8]);
        if (c.isValid()) return c;
    }
    return colors[(size_t)index % 8];
}

QToolButton* smallButton(QWidget* parent, const QIcon& icon, const QString& tip, const char* name)
{
    QToolButton* b = new QToolButton(parent);
    b->setObjectName(name);
    b->setIcon(icon);
    b->setIconSize(QSize(16, 16));
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

// ------------------------------------------------------------------------------------------ small widgets
// A label that elides its text with "...".
class ElideLabel : public QWidget {
public:
    explicit ElideLabel(QWidget* parent = nullptr) : QWidget(parent) { setObjectName("fxLabel"); }
    void setText(const QString& t)
    {
        if (t == text_) return;
        text_ = t;
        setAccessibleName(t);
        update();
    }
    const QString& text() const { return text_; }
    bool shortened() const { return fontMetrics().horizontalAdvance(text_) > width(); }
    QSize sizeHint() const override { return QSize(fontMetrics().horizontalAdvance(text_) + 2, fontMetrics().height() + 4); }
    QSize minimumSizeHint() const override { return QSize(24, fontMetrics().height() + 4); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(text_, Qt::ElideRight, width()));
    }

private:
    QString text_;
};

// Ignores the wheel until clicked, so scrolling the panel does not change settings.
class FxSlider : public QSlider {
public:
    std::function<void()> onDoubleClick;
    explicit FxSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumWidth(80);
    }

protected:
    void wheelEvent(QWheelEvent* e) override
    {
        if (hasFocus()) QSlider::wheelEvent(e);
        else e->ignore();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && onDoubleClick) {
            e->accept();
            onDoubleClick();
        } else {
            QSlider::mouseDoubleClickEvent(e);
        }
    }
};

class NumBox : public QDoubleSpinBox {
public:
    explicit NumBox(QWidget* parent) : QDoubleSpinBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setKeyboardTracking(false);
        setAccelerated(true);
    }
    bool typing() const { return hasFocus() && lineEdit()->isModified(); }
    // Sets the value unless it already shows it (keeps the caret).
    void show(double v)
    {
        const double eps = 0.5 * std::pow(10.0, -decimals());
        if (std::fabs(value() - v) < eps) return;
        QSignalBlocker b(this);
        setValue(v);
    }

protected:
    void wheelEvent(QWheelEvent* e) override
    {
        if (hasFocus()) QDoubleSpinBox::wheelEvent(e);
        else e->ignore();
    }
};

class FxCombo : public QComboBox {
public:
    explicit FxCombo(QWidget* parent) : QComboBox(parent) { setFocusPolicy(Qt::StrongFocus); }

protected:
    void wheelEvent(QWheelEvent* e) override
    {
        if (hasFocus()) QComboBox::wheelEvent(e);
        else e->ignore();
    }
};

class ColourButton : public QAbstractButton {
public:
    explicit ColourButton(QWidget* parent) : QAbstractButton(parent)
    {
        setFixedSize(56, 20);
        setCursor(Qt::PointingHandCursor);
    }
    void setColour(const QColor& c)
    {
        if (c == colour_) return;
        colour_ = c;
        update();
    }
    const QColor& colour() const { return colour_; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(0, 0, -1, -1);
        if (colour_.alpha() < 255) {
            p.setClipRect(r);
            for (int y = 0; y < height(); y += 5)
                for (int x = 0; x < width(); x += 5) p.fillRect(x, y, 5, 5, ((x / 5 + y / 5) & 1) ? QColor(150, 150, 150) : QColor(90, 90, 90));
            p.setClipping(false);
        }
        p.fillRect(r, colour_);
        p.setPen(hasFocus() ? palette().color(QPalette::Highlight) : palette().color(isEnabled() ? QPalette::Light : QPalette::Mid));
        p.drawRect(r);
        if (!isEnabled()) p.fillRect(r, QColor(45, 45, 48, 150));
    }

private:
    QColor colour_ = Qt::black;
};

// An angle dial (0 to the right, anticlockwise, as OpenFX). The value keeps whole turns; Shift drags in 15 degree steps.
class AngleDial : public QWidget {
public:
    std::function<void()> onPress, onRelease;
    std::function<void(double degrees)> onTurn;
    explicit AngleDial(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxDial");
        setFixedSize(26, 26);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
    }
    void setAngle(double a)
    {
        if (a == angle_) return;
        angle_ = a;
        update();
    }
    double angle() const { return angle_; }
    bool down() const { return down_; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(2.5, 2.5, -2.5, -2.5);
        p.setPen(QPen(palette().color(isEnabled() ? QPalette::Light : QPalette::Mid), 1.2));
        p.setBrush(palette().color(QPalette::Base));
        p.drawEllipse(r);
        const QPointF c = r.center();
        const double rad = angle_ * 3.14159265358979323846 / 180.0;
        const QPointF tip(c.x() + std::cos(rad) * r.width() * 0.5, c.y() - std::sin(rad) * r.height() * 0.5);
        p.setPen(QPen(isEnabled() ? QColor(255, 196, 70) : palette().color(QPalette::Mid), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, tip);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::ButtonText));
        p.drawEllipse(c, 1.8, 1.8);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        down_ = true;
        if (onPress) onPress();
        turnTo(e->position(), e->modifiers());
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (down_ && (e->buttons() & Qt::LeftButton)) turnTo(e->position(), e->modifiers());
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !down_) return;
        down_ = false;
        if (onRelease) onRelease();
    }

private:
    void turnTo(const QPointF& at, Qt::KeyboardModifiers mods)
    {
        const QPointF c = QRectF(rect()).center();
        const double dx = at.x() - c.x(), dy = c.y() - at.y();
        if (std::hypot(dx, dy) < 2.0) return;
        double want = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
        if (mods & Qt::ShiftModifier) want = std::floor(want / 15.0 + 0.5) * 15.0;
        double delta = std::fmod(want - angle_, 360.0);
        if (delta > 180.0) delta -= 360.0;
        if (delta < -180.0) delta += 360.0;
        if (std::fabs(delta) < 1e-9) return;
        if (onTurn) onTurn(angle_ + delta);
    }
    double angle_ = 0;
    bool down_ = false;
};

// A position pad: the frame with a dot. Values count up from the bottom (OpenFX's y goes up).
class XYPad : public QWidget {
public:
    std::function<void()> onPress, onRelease;
    std::function<void(double x, double y)> onMove;
    explicit XYPad(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxPad");
        setFixedSize(72, 42);
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::NoFocus);
    }
    void setRange(double x0, double x1, double y0, double y1)
    {
        x0_ = x0; x1_ = x1; y0_ = y0; y1_ = y1;
        update();
    }
    void setPoint(double x, double y)
    {
        if (x == x_ && y == y_) return;
        x_ = x;
        y_ = y;
        update();
    }
    bool down() const { return down_; }
    QPointF placeOf(double x, double y) const
    {
        const QRectF r = area();
        return QPointF(r.left() + (x - x0_) / (x1_ - x0_) * r.width(), r.bottom() - (y - y0_) / (y1_ - y0_) * r.height());
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = area();
        p.fillRect(rect(), palette().color(QPalette::Base));
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        p.setPen(QPen(palette().color(isEnabled() ? QPalette::Light : QPalette::Mid), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
        QPointF at = placeOf(x_, y_);
        at.setX(std::min(r.right(), std::max(r.left(), at.x())));
        at.setY(std::min(r.bottom(), std::max(r.top(), at.y())));
        const QColor dot = isEnabled() ? QColor(255, 196, 70) : palette().color(QPalette::Mid);
        p.setPen(QPen(dot, 1));
        p.drawLine(QPointF(at.x() - 5, at.y()), QPointF(at.x() + 5, at.y()));
        p.drawLine(QPointF(at.x(), at.y() - 5), QPointF(at.x(), at.y() + 5));
        p.setBrush(dot);
        p.drawEllipse(at, 2.2, 2.2);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        down_ = true;
        if (onPress) onPress();
        moveTo(e->position());
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (down_ && (e->buttons() & Qt::LeftButton)) moveTo(e->position());
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !down_) return;
        down_ = false;
        if (onRelease) onRelease();
    }

private:
    QRectF area() const { return QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5); }
    void moveTo(const QPointF& at)
    {
        const QRectF r = area();
        const double fx = std::min(1.0, std::max(0.0, (at.x() - r.left()) / r.width()));
        const double fy = std::min(1.0, std::max(0.0, (r.bottom() - at.y()) / r.height()));
        if (onMove) onMove(x0_ + fx * (x1_ - x0_), y0_ + fy * (y1_ - y0_));
    }
    double x0_ = 0, x1_ = 1, y0_ = 0, y1_ = 1, x_ = 0.5, y_ = 0.5;
    bool down_ = false;
};

class GroupHeader : public QAbstractButton {
public:
    explicit GroupHeader(QWidget* parent) : QAbstractButton(parent)
    {
        setCheckable(true);
        setChecked(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QFont f = font();
        f.setBold(true);
        setFont(f);
        setFocusPolicy(Qt::TabFocus);
    }
    QSize sizeHint() const override { return QSize(fontMetrics().horizontalAdvance(text()) + 30, fontMetrics().height() + 8); }
    QSize minimumSizeHint() const override { return QSize(60, fontMetrics().height() + 8); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QColor band = palette().color(QPalette::Mid);
        if (underMouse() && isEnabled()) band = band.lighter(125);
        p.fillRect(rect(), band);
        QStyleOption opt;
        opt.initFrom(this);
        opt.rect = QRect(4, (height() - 12) / 2, 12, 12);
        style()->drawPrimitive(isChecked() ? QStyle::PE_IndicatorArrowDown : QStyle::PE_IndicatorArrowRight, &opt, &p, this);
        p.setPen(palette().color(QPalette::WindowText));
        const QRect tr = rect().adjusted(22, 0, -4, 0);
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(text(), Qt::ElideRight, tr.width()));
        if (hasFocus()) {
            p.setPen(palette().color(QPalette::Highlight));
            p.drawRect(rect().adjusted(0, 0, -1, -1));
        }
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
};

class NativeHost : public QWidget {
public:
    std::function<void()> onPlaced;
    std::function<void()> onWindowLost;
    explicit NativeHost(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxPluginWindow");
        setAttribute(Qt::WA_NativeWindow);
    }
    void* handle() { return reinterpret_cast<void*>(static_cast<quintptr>(winId())); }
    double scale() const { return devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0; }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        if (onPlaced) onPlaced();
    }
    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        if (onPlaced) onPlaced();
    }
    bool event(QEvent* e) override
    {
        // Qt can replace the system window (when the window changes kind); the plug-in must be told first.
        const bool going = e->type() == QEvent::PlatformSurface &&
                           static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed;
        if ((going || e->type() == QEvent::WinIdChange) && onWindowLost) {
            const std::function<void()> fn = onWindowLost;
            onWindowLost = nullptr;
            fn();
        }
        return QWidget::event(e);
    }
};

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent, int hgap, int vgap) : QLayout(parent), hgap_(hgap), vgap_(vgap) { setContentsMargins(0, 0, 0, 0); }
    void setPad(int left, int right)
    {
        lp_ = left;
        rp_ = right;
    }
    ~FlowLayout() override
    {
        while (QLayoutItem* it = takeAt(0)) delete it;
    }
    void addItem(QLayoutItem* item) override { items_.append(item); }
    int count() const override { return (int)items_.size(); }
    QLayoutItem* itemAt(int k) const override { return (k >= 0 && k < items_.size()) ? items_.at(k) : nullptr; }
    QLayoutItem* takeAt(int k) override { return (k >= 0 && k < items_.size()) ? items_.takeAt(k) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return place(QRect(0, 0, w, 0), false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize s;
        for (QLayoutItem* it : items_) s = s.expandedTo(it->minimumSize());
        return s + QSize(lp_ + rp_, 0);
    }
    void setGeometry(const QRect& r) override
    {
        QLayout::setGeometry(r);
        place(r, true);
    }

private:
    int place(const QRect& r, bool move) const
    {
        int x = r.x() + lp_, y = r.y(), lineH = 0;
        for (QLayoutItem* it : items_) {
            if (it->isEmpty()) continue;
            const QSize sz = it->sizeHint();
            if (x > r.x() + lp_ && x + sz.width() > r.right() + 1 - rp_) {
                x = r.x() + lp_;
                y += lineH + vgap_;
                lineH = 0;
            }
            if (move) it->setGeometry(QRect(QPoint(x, y), sz));
            x += sz.width() + hgap_;
            lineH = std::max(lineH, sz.height());
        }
        return y + lineH - r.y();
    }
    QList<QLayoutItem*> items_;
    int hgap_, vgap_;
    int lp_ = 0, rp_ = 0;
};

class ChainButton : public QWidget {
public:
    uint64_t slotId = 0;
    QCheckBox* tick = nullptr;
    std::function<void()> onPick;
    std::function<void(const QPoint& globalPos, bool dropped)> onDrag;
    std::function<void(const QPoint& globalPos)> onMenu;

    explicit ChainButton(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxChainButton");
        setAttribute(Qt::WA_Hover);
        QHBoxLayout* h = new QHBoxLayout(this);
        h->setContentsMargins(6, 3, 9, 3);
        h->setSpacing(4);
        tick = new QCheckBox(this);
        tick->setObjectName("fxBypass");
        tick->setToolTip("Ticked: the effect is on. Clear the tick to bypass it.");
        tick->setFocusPolicy(Qt::NoFocus);
        h->addWidget(tick);
        warn_ = new QLabel(this);
        warn_->setObjectName("fxWarn");
        warn_->setAttribute(Qt::WA_TransparentForMouseEvents);
        warn_->setPixmap(warningPixmap());
        warn_->hide();
        h->addWidget(warn_);
        name_ = new QLabel(this);
        name_->setObjectName("fxChainName");
        name_->setTextFormat(Qt::PlainText);
        name_->setAttribute(Qt::WA_TransparentForMouseEvents);
        h->addWidget(name_);
        pre_ = new QLabel("pre", this);
        pre_->setObjectName("fxPrePanCrop");
        pre_->setToolTip("Applied before Event Pan/Crop (right-click to change)");
        pre_->setStyleSheet("QLabel { color: #9fc4ff; font-size: 8pt; }");
        pre_->setAttribute(Qt::WA_TransparentForMouseEvents);
        pre_->hide();
        h->addWidget(pre_);
    }

    void setBeforePanCrop(bool on)
    {
        if (pre_->isHidden() == on) pre_->setVisible(on);
    }
    bool beforePanCrop() const { return !pre_->isHidden(); }
    // applied after the track is composited (VEGAS's Composite button)
    void setAfterComposite(bool on)
    {
        if (on) {
            pre_->setText("post");
            pre_->setToolTip("Applied after the track is composited (right-click to change)");
        }
        if (pre_->isHidden() == on) pre_->setVisible(on);
    }

    void setState(const QString& name, bool on, bool picked, const QString& error)
    {
        if (name_->text() != name) name_->setText(name);
        if (tick->isChecked() != on) {
            QSignalBlocker b(tick);
            tick->setChecked(on);
        }
        const bool failed = !error.isEmpty();
        if (warn_->isHidden() == failed) warn_->setVisible(failed);
        const QString tip = failed ? name + "\n" + error : name;
        if (toolTip() != tip) setToolTip(tip);
        if (picked != picked_ || failed != failed_ || on != on_) {
            picked_ = picked;
            failed_ = failed;
            on_ = on;
            QPalette pal = QApplication::palette();
            QColor text = picked ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::ButtonText);
            if (!on) text = picked ? QColor(190, 205, 225) : pal.color(QPalette::Disabled, QPalette::ButtonText);
            pal.setColor(QPalette::WindowText, text);
            name_->setPalette(pal);
            update();
        }
    }
    QString name() const { return name_->text(); }
    bool picked() const { return picked_; }
    bool failed() const { return failed_; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        QColor fill = picked_ ? pal.color(QPalette::Highlight) : pal.color(QPalette::Button);
        if (underMouse() && !picked_) fill = fill.lighter(120);
        QColor edge = picked_ ? pal.color(QPalette::Highlight).lighter(130) : pal.color(QPalette::Light);
        if (failed_) edge = QColor(235, 150, 60);
        p.setPen(QPen(edge, 1));
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::RightButton) {
            if (onPick) onPick();
            if (onMenu) onMenu(e->globalPosition().toPoint());
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        pressed_ = true;
        dragging_ = false;
        pressPos_ = e->position().toPoint();
        if (onPick) onPick();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!pressed_ || !(e->buttons() & Qt::LeftButton)) return;
        if (!dragging_ && (e->position().toPoint() - pressPos_).manhattanLength() < QApplication::startDragDistance()) return;
        dragging_ = true;
        if (onDrag) onDrag(e->globalPosition().toPoint(), false);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const bool was = dragging_;
        pressed_ = dragging_ = false;
        if (was && onDrag) onDrag(e->globalPosition().toPoint(), true);
    }

private:
    QLabel* pre_ = nullptr;
    static QPixmap warningPixmap()
    {
        QPixmap pm(28, 28);
        pm.setDevicePixelRatio(2.0);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(240, 160, 50));
        const QPointF tri[3] = {QPointF(7, 1.5), QPointF(13, 12.5), QPointF(1, 12.5)};
        p.drawPolygon(tri, 3);
        p.setPen(QPen(QColor(40, 30, 10), 1.5));
        p.drawLine(QPointF(7, 5.2), QPointF(7, 8.6));
        p.drawPoint(QPointF(7, 10.8));
        return pm;
    }
    QLabel* warn_ = nullptr;
    QLabel* name_ = nullptr;
    bool picked_ = false, failed_ = false, on_ = true;
    bool pressed_ = false, dragging_ = false;
    QPoint pressPos_;
};

class ChainBar : public QWidget {
public:
    FlowLayout* flow = nullptr;
    explicit ChainBar(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxChain");
        flow = new FlowLayout(this, 8, 5);
        flow->setPad(12, 12);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    void setMarker(int x, int top, int bottom)
    {
        markerX_ = x;
        markerTop_ = top;
        markerBottom_ = bottom;
        update();
    }
    void clearMarker()
    {
        if (markerX_ < 0) return;
        markerX_ = -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        std::vector<QRect> rects;
        for (int k = 0; k < flow->count(); ++k)
            if (QLayoutItem* it = flow->itemAt(k))
                if (it->widget() && !it->isEmpty()) rects.push_back(it->geometry());
        if (!rects.empty()) {
            p.setRenderHint(QPainter::Antialiasing);
            const QColor line = palette().color(QPalette::WindowText).darker(125);
            p.setPen(QPen(line, 1));
            p.setBrush(Qt::NoBrush);
            const double r = 3.0;
            auto rowY = [](const QRect& q) { return q.center().y() + 0.5; };
            p.drawEllipse(QPointF(5.5, rowY(rects.front())), r, r);
            p.drawLine(QPointF(5.5 + r, rowY(rects.front())), QPointF(rects.front().left(), rowY(rects.front())));
            for (size_t k = 0; k + 1 < rects.size(); ++k)
                if (rects[k + 1].top() == rects[k].top()) p.drawLine(QPointF(rects[k].right() + 1, rowY(rects[k])), QPointF(rects[k + 1].left(), rowY(rects[k])));
            const double endX = std::min((double)width() - 5.5, (double)rects.back().right() + 1 + 8 + r);
            p.drawLine(QPointF(rects.back().right() + 1, rowY(rects.back())), QPointF(endX - r, rowY(rects.back())));
            p.drawEllipse(QPointF(endX, rowY(rects.back())), r, r);
        }
        if (markerX_ < 0) return;
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(QRect(markerX_ - 1, markerTop_, 2, markerBottom_ - markerTop_), palette().color(QPalette::BrightText));
    }

private:
    int markerX_ = -1, markerTop_ = 0, markerBottom_ = 0;
};

class TargetChip : public QWidget {
public:
    explicit TargetChip(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxTargetChip");
        setFixedSize(34, 20);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void setChip(const QColor& c, int number)
    {
        if (c == colour_ && number == number_) return;
        colour_ = c;
        number_ = number;
        setToolTip(number > 0 ? QString("Track %1").arg(number) : QString());
        update();
    }
    int number() const { return number_; }
    QColor colour() const { return colour_; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(colour_);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(3, 0, 14, 12), Qt::AlignLeft | Qt::AlignVCenter, QString::number(number_));
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        const QPointF tri[3] = {QPointF(14, 12), QPointF(9, 15.5), QPointF(14, 19)};
        p.drawPolygon(tri, 3);
    }

private:
    QColor colour_;
    int number_ = 0;
};

// ------------------------------------------------------------------------------------------ panels
// A change still going on when a panel closes counts as done; its undo step is made after the current event, since it notifies the whole interface.
void finishEditLater(uint64_t slotId, const std::string& what)
{
    if (!AppState::get().fxEditOpen(slotId)) return;
    AppState::post([slotId, what] { AppState::get().endFxEdit(slotId, what); });
}

// Panels are told to let go of their plug-in (shutDown) before their widgets are deleted, which is deferred.
class Panel : public QWidget {
public:
    uint64_t slotId = 0;
    std::string signature;
    std::function<void()> requestRebuild;

    explicit Panel(QWidget* parent) : QWidget(parent) {}
    virtual void shutDown() { closed_ = true; }
    virtual bool stale() const { return broken_; }
    virtual void refresh(bool cursorOnly) { (void)cursorOnly; }
    virtual void idle() {}
    virtual bool wantsIdle() const { return false; }
    // Called once the panel is showing, so a plug-in's own window finds a parent at its final size.
    virtual void start() {}

protected:
    bool closed_ = false, broken_ = false;
};

class MessagePanel : public Panel {
public:
    MessagePanel(QWidget* parent, const QString& title, const QString& detail) : Panel(parent)
    {
        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(24, 28, 24, 24);
        v->setSpacing(8);
        QLabel* t = new QLabel(title, this);
        t->setObjectName("fxMessage");
        t->setTextFormat(Qt::PlainText);
        t->setWordWrap(true);
        t->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        QFont f = t->font();
        f.setPointSizeF(f.pointSizeF() + 1.5);
        t->setFont(f);
        v->addWidget(t);
        if (!detail.isEmpty()) {
            QLabel* dl = new QLabel(detail, this);
            dl->setObjectName("fxMessageDetail");
            dl->setTextFormat(Qt::PlainText);
            dl->setWordWrap(true);
            dl->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            dl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            QPalette pal = dl->palette();
            pal.setColor(QPalette::WindowText, QColor(170, 170, 170));
            dl->setPalette(pal);
            v->addWidget(dl);
        }
        v->addStretch(1);
    }
};

// .......................................................................................... OpenFX
// One parameter's state copied out under the instance lock, so widgets update without holding up render threads.
struct Snap {
    ofx::Param* p = nullptr;
    ofx::ParamKind kind = ofx::ParamKind::Unknown;
    int dim = 0;
    std::string name, label, hint, parent, stringMode, str;
    bool secret = false, enabled = true, animates = false, groupOpen = true, mustExist = true;
    std::vector<double> v;
    size_t keys = 0;
    std::vector<std::string> choices, dimLabels;
    double lo[4] = {0, 0, 0, 0}, hi[4] = {0, 0, 0, 0}, dlo = 0, dhi = 0, inc = 1;
    int digits = 2;
    std::string doubleType;
    bool normalised = false;                // a position counted in fractions of the frame (else in canonical pixels)
    std::vector<double> keyTimes;
    bool interact = false;
    int prefSize[2] = {0, 0}, minSize[2] = {0, 0};
    double aspect = 0;
    const EditorSpec* editor = nullptr;
    bool member = false;                    // ... or this setting is one of those others: no row of its own
    std::string fileFilter;
};

bool isAngle(const Snap& s) { return s.kind == ofx::ParamKind::Double && s.doubleType == kOfxParamDoubleTypeAngle; }
bool isPosition(const Snap& s)
{
    if (s.kind != ofx::ParamKind::Double2D) return false;
    return s.doubleType == kOfxParamDoubleTypeXY || s.doubleType == kOfxParamDoubleTypeXYAbsolute ||
           s.doubleType == kNormalisedXY || s.doubleType == kNormalisedXYAbsolute;
}

bool wholeKind(ofx::ParamKind k)
{
    return k == ofx::ParamKind::Int || k == ofx::ParamKind::Int2D || k == ofx::ParamKind::Int3D;
}

// A slider range: both ends given, not the host's "no limit" default.
bool saneRange(double lo, double hi) { return std::isfinite(lo) && std::isfinite(hi) && hi > lo && lo > -1.0e9 && hi < 1.0e9; }

void snapshot(const ofx::Instance* inst, double time, std::vector<Snap>& out)
{
    out.clear();
    out.reserve(inst->params.params.size());
    const std::string pid = inst->plugin ? inst->plugin->info.id : std::string();
    for (const auto& up : inst->params.params) {
        const ofx::Param& p = *up;
        const ofx::PropertySet& pr = p.props;
        Snap s;
        s.p = up.get();
        s.kind = p.kind;
        s.dim = p.dim;
        s.name = p.name;
        if (p.kind != ofx::ParamKind::Group && p.kind != ofx::ParamKind::Page && inst->paramHasInteract(&p)) {
            s.interact = true;
            inst->paramInteractSize(&p, s.prefSize, s.minSize, &s.aspect);
        }
        s.editor = findEditorSpec(pid, p.name);
        s.member = !s.editor && isEditorMember(pid, p.name);
        const bool edited = s.editor || s.member;
        if (p.kind == ofx::ParamKind::Page || (p.kind == ofx::ParamKind::Unknown && !edited) ||
            (p.kind == ofx::ParamKind::Custom && !s.interact && !edited)) {
            out.push_back(std::move(s));
            continue;
        }
        s.label = p.label();
        if (s.editor && !s.editor->label.empty()) s.label = s.editor->label;
        s.hint = p.hint();
        s.parent = p.parent();
        s.secret = p.secret();
        // transition progress is set by the host; VEGAS does not show it
        if (p.name == kOfxImageEffectTransitionParamName && inst->context() == kOfxImageEffectContextTransition) s.secret = true;
        s.enabled = p.enabled();
        if (p.kind == ofx::ParamKind::Group) s.groupOpen = pr.getInt(kOfxParamPropGroupOpen, 0, 1) != 0;
        if (p.kind == ofx::ParamKind::String) {
            s.str = p.strAt(time);
            s.stringMode = pr.getString(kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);
            s.mustExist = pr.getInt(kOfxParamPropStringFilePathExists, 0, 1) != 0;
            if (const char* filter = filePickFilter(pid, p.name)) {
                s.stringMode = kOfxParamStringIsFilePath;
                s.mustExist = true;
                s.fileFilter = filter;
            }
        }
        if (p.kind == ofx::ParamKind::Custom) s.str = p.strAt(time);
        if (p.kind == ofx::ParamKind::Choice) s.choices = p.choices();
        if (p.dim > 0) {
            s.v = p.valueAt(time);
            s.v.resize((size_t)p.dim, 0.0);
            s.keys = p.keys.size();
            s.animates = pr.getInt(kOfxParamPropAnimates) != 0;
            for (int k = 0; k < p.dim && k < 4; ++k) {
                s.lo[k] = pr.getDouble(kOfxParamPropMin, k, -1.0e300);
                s.hi[k] = pr.getDouble(kOfxParamPropMax, k, 1.0e300);
            }
            s.dlo = pr.getDouble(kOfxParamPropDisplayMin, 0, -1.0e300);
            s.dhi = pr.getDouble(kOfxParamPropDisplayMax, 0, 1.0e300);
            s.inc = pr.getDouble(kOfxParamPropIncrement, 0, 1.0);
            s.digits = pr.getInt(kOfxParamPropDigits, 0, 2);
            s.doubleType = pr.getString(kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
            s.normalised = s.doubleType == kNormalisedXY || s.doubleType == kNormalisedXYAbsolute ||
                           pr.getString(kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesCanonical) == kOfxParamCoordinatesNormalised;
            for (const auto& k : p.keys) s.keyTimes.push_back(k.first);
            if (p.dim > 1)
                for (int k = 0; k < p.dim; ++k) s.dimLabels.push_back(pr.getString(kOfxParamPropDimensionLabel, k));
        }
        out.push_back(std::move(s));
    }
    // An editor whose anchor setting is missing (no VEGAS extensions): its first present setting stands in.
    for (const EditorSpec* spec : editorSpecsFor(pid)) {
        bool has = false;
        for (const Snap& sn : out) has = has || sn.editor == spec;
        if (has) continue;
        for (const std::string& name : spec->uses)
            for (Snap& sn : out)
                if (sn.name == name && sn.member && !has) {
                    sn.member = false;
                    sn.editor = spec;
                    sn.label = spec->label.empty() ? sn.label : spec->label;
                    has = true;
                }
    }
    for (Snap& sn : out) {
        if (!sn.editor || sn.editor->keyed.empty()) continue;
        sn.animates = true;
        sn.keys = 0;
        sn.keyTimes.clear();
        for (const std::string& name : sn.editor->keyed) {
            const ofx::Param* q = inst->params.find(name);
            if (!q) continue;
            sn.keys += q->keyCount();
            if (q->isText()) for (const auto& k : q->strKeys) sn.keyTimes.push_back(k.first);
            else for (const auto& k : q->keys) sn.keyTimes.push_back(k.first);
        }
        std::sort(sn.keyTimes.begin(), sn.keyTimes.end());
        sn.keyTimes.erase(std::unique(sn.keyTimes.begin(), sn.keyTimes.end()), sn.keyTimes.end());
    }
}

struct OfxRow {
    ofx::Param* param = nullptr;
    size_t index = 0;
    ofx::ParamKind kind = ofx::ParamKind::Unknown;
    int dim = 0;
    int group = -1;
    std::string stringMode;
    QWidget* row = nullptr;
    ElideLabel* label = nullptr;
    FxSlider* slider = nullptr;
    std::vector<NumBox*> boxes;
    std::vector<QLabel*> dimLabels;
    QCheckBox* check = nullptr;
    FxCombo* combo = nullptr;
    ColourButton* colour = nullptr;
    std::vector<NumBox*> colourBoxes;
    AngleDial* dial = nullptr;
    XYPad* pad = nullptr;
    QToolButton* prevKey = nullptr;
    QToolButton* nextKey = nullptr;
    std::vector<double> keyTimes;
    QLineEdit* line = nullptr;
    QPlainTextEdit* text = nullptr;
    QLabel* textLabel = nullptr;
    QPushButton* push = nullptr;
    QWidget* interact = nullptr;
    FxEditor* editor = nullptr;
    const EditorSpec* spec = nullptr;
    QString fileFilter;
    QToolButton* reset = nullptr;
    QToolButton* key = nullptr;
    QPointer<QColorDialog> dialog;
    std::vector<std::string> choices;
    bool ranged = false;
    double lo = 0, hi = 1;
    int steps = kSliderSteps;
    std::vector<double> rangeKey;
    size_t keys = 0;
    bool mustExist = true;

    double valueOfPos(int pos) const
    {
        double v = lo + (hi - lo) * (double)pos / (double)std::max(1, steps);
        if (boxes.empty()) return v;
        // to the number of decimals shown, so the slider gives 0.3 and not 0.30000000000000004
        const double m = std::pow(10.0, boxes[0]->decimals());
        v = std::floor(v * m + 0.5) / m;
        return std::min(hi, std::max(lo, v));
    }
    int posOf(double v) const
    {
        if (!(hi > lo)) return 0;
        const double t = (std::min(hi, std::max(lo, v)) - lo) / (hi - lo);
        return (int)std::floor(t * steps + 0.5);
    }
};

// A setting's control drawn by the effect itself (OpenFX custom parameter interact) via OpenGL into a frame shown here; mouse and keys go to the effect. Press to release is one undo step.
class ParamInteractView : public QWidget {
public:
    ParamInteractView(QWidget* parent, ofx::Param* p, const Snap& s, std::function<ofx::Instance*()> live, std::function<double()> time)
        : QWidget(parent), param_(p), live_(std::move(live)), time_(std::move(time))
    {
        setObjectName("fxInteract");
        setProperty("param", qs(s.name));
        setFocusPolicy(Qt::ClickFocus);
        setMouseTracking(true);
        pref_ = QSize(std::max(s.prefSize[0], s.minSize[0]), std::max(s.prefSize[1], s.minSize[1]));
        setMinimumSize(std::max(8, s.minSize[0]), std::max(8, s.minSize[1]));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(std::min(400, std::max(pref_.height(), s.minSize[1])));
    }
    ~ParamInteractView() override
    {
        if (ofx::Instance* inst = live_()) inst->disposeParamInteract(param_);
    }
    QSize sizeHint() const override { return pref_; }
    ofx::Param* param() const { return param_; }
    std::function<void()> onPress, onRelease;
    bool drawn() const { return !image_.isNull(); }

protected:
    ofx::Instance* ready()
    {
        ofx::Instance* inst = live_();
        if (!inst) return nullptr;
        if (!inst->paramInteractMade(param_) && (failed_ || !inst->createParamInteract(param_))) {
            failed_ = true;
            return nullptr;
        }
        return inst;
    }
    ofx::Instance::ParamInteractArgs args() const
    {
        ofx::Instance::ParamInteractArgs a;
        const qreal dpr = devicePixelRatioF();
        a.time = time_ ? time_() : 0.0;
        a.size[0] = (int)std::lround(width() * dpr);
        a.size[1] = (int)std::lround(height() * dpr);
        const QColor bg = palette().color(QPalette::Base);
        a.background[0] = bg.redF();
        a.background[1] = bg.greenF();
        a.background[2] = bg.blueF();
        return a;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        ofx::Instance* inst = ready();
        bool busy = false;
        if (!inst) image_ = QImage();
        else {
            std::unique_lock<std::recursive_mutex> lk(inst->mutex, std::try_to_lock);
            if (!lk.owns_lock()) {
                busy = true;
                QTimer::singleShot(30, this, [this] { update(); });
            } else {
                const ofx::Instance::ParamInteractArgs a = args();
                ofx::Param* prm = param_;
                image_ = drawWithGL(QSize(a.size[0], a.size[1]), palette().color(QPalette::Base), 0, a.size[0], 0, a.size[1],
                                    [inst, prm, &a] { inst->paramInteractDraw(prm, a); });
                glMissing_ = image_.isNull();
            }
        }
        if (busy && image_.isNull()) {
            p.fillRect(rect(), palette().color(QPalette::Base));
            return;
        }
        if (!image_.isNull()) {
            image_.setDevicePixelRatio(devicePixelRatioF());
            p.drawImage(QPoint(0, 0), image_);
        } else {
            p.fillRect(rect(), palette().color(QPalette::Base));
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect(), Qt::AlignCenter, failed_ ? QString("(the effect's control failed)") : glMissing_ ? QString("(drawn by the effect: needs OpenGL)") : QString());
        }
    }
    void pen(const char* action, const QPointF& at, double pressure)
    {
        ofx::Instance* inst = ready();
        if (!inst) return;
        const qreal dpr = devicePixelRatioF();
        if (inst->paramInteractPen(param_, action, args(), at.x() * dpr, (height() - at.y()) * dpr, pressure)) update();
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
        held_ = true;
        if (onPress) onPress();
        pen(kOfxInteractActionPenDown, e->position(), 1.0);
    }
    void mouseMoveEvent(QMouseEvent* e) override { pen(kOfxInteractActionPenMotion, e->position(), held_ ? 1.0 : 0.0); }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (!held_ || e->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(e);
        held_ = false;
        pen(kOfxInteractActionPenUp, e->position(), 0.0);
        if (onRelease) onRelease();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        ofx::Instance* inst = ready();
        int sym = 0;
        switch (e->key()) {
        case Qt::Key_Left: sym = 0xff51; break;
        case Qt::Key_Up: sym = 0xff52; break;
        case Qt::Key_Right: sym = 0xff53; break;
        case Qt::Key_Down: sym = 0xff54; break;
        default: {
            const QList<uint> t = e->text().toUcs4();
            if (!t.isEmpty() && t[0] >= 0x20) sym = t[0] < 0x100 ? (int)t[0] : (int)(0x01000000 | t[0]);
        }
        }
        if (!inst || !sym) return QWidget::keyPressEvent(e);
        if (onPress) onPress();
        const bool taken = inst->paramInteractKey(param_, e->isAutoRepeat() ? kOfxInteractActionKeyRepeat : kOfxInteractActionKeyDown, args(), sym, e->text().toStdString());
        if (onRelease) onRelease();
        if (taken) update();
        else QWidget::keyPressEvent(e);
    }
    void focusInEvent(QFocusEvent* e) override
    {
        if (ofx::Instance* inst = ready()) inst->paramInteractFocus(param_, true, args());
        QWidget::focusInEvent(e);
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        if (ofx::Instance* inst = live_())
            if (inst->paramInteractMade(param_)) inst->paramInteractFocus(param_, false, args());
        QWidget::focusOutEvent(e);
    }

private:
    ofx::Param* param_;
    std::function<ofx::Instance*()> live_;
    std::function<double()> time_;
    QSize pref_;
    QImage image_;
    bool held_ = false, failed_ = false, glMissing_ = false;
};

struct OfxGroup {
    ofx::Param* param = nullptr;
    size_t index = 0;
    std::string name;
    int parent = -1;
    int depth = 0;
    QWidget* box = nullptr;
    GroupHeader* header = nullptr;
    QWidget* body = nullptr;
    QVBoxLayout* bodyLayout = nullptr;
};

// Open/folded groups, kept by the window across effect picks.
typedef std::map<std::pair<uint64_t, std::string>, bool> GroupMemory;

class OfxPanel : public Panel {
public:
    OfxPanel(QWidget* parent, uint64_t slot, ofx::Instance* inst, std::function<double()> timeFn, std::function<void(double)> seekFn,
             GroupMemory* memory, Playback* playback)
        : Panel(parent), inst_(inst), time_(std::move(timeFn)), seek_(std::move(seekFn)), memory_(memory), playback_(playback)
    {
        slotId = slot;
        setObjectName("fxOfxPanel");
        top_ = new QVBoxLayout(this);
        top_->setContentsMargins(8, 8, 8, 8);
        top_->setSpacing(2);

        soon_ = new QTimer(this);
        soon_->setSingleShot(true);
        QObject::connect(soon_, &QTimer::timeout, this, [this] { refresh(false); });

        wait_ = new QTimer(this);
        wait_->setSingleShot(true);
        QObject::connect(wait_, &QTimer::timeout, this, [this] { tryBuild(); });

        typing_ = new QTimer(this);
        typing_->setSingleShot(true);
        typing_->setInterval(800);
        QObject::connect(typing_, &QTimer::timeout, this, [this] { endEdit(); });
    }

    void start() override { tryBuild(); }

    void shutDown() override
    {
        if (closed_) return;
        closed_ = true;
        *alive_ = false;
        soon_->stop();
        wait_->stop();
        typing_->stop();
        if (hasInteracts_) {
            ofx::Instance* now = live();
            if (now) {
                std::lock_guard<std::recursive_mutex> lk(now->mutex);
                now->onParamInteractRedraw = nullptr;
            }
            for (OfxRow& r : rows_)
                if (r.interact) {
                    if (now) now->disposeParamInteract(r.param);
                    delete r.interact;
                    r.interact = nullptr;
                }
        }
        for (OfxRow& r : rows_)
            if (r.dialog) delete r.dialog.data();
        finishEditLater(slotId, editName_);
        ofx::Instance* inst = live();
        if (host_) {
            host_->onPlaced = nullptr;
            host_->onWindowLost = nullptr;
            // if the instance has gone, it took its window with it when it was destroyed
            if (inst && ownWindow_) inst->disposeHwndInteract();
            ownWindow_ = false;
            delete host_;
            host_ = nullptr;
        }
        // this one waits for a render in progress to finish: the effect must hear the end of every edit it heard begin
        if (inst && editing_) inst->endEdit();
        editing_ = false;
    }

    bool stale() const override { return broken_ || !live(); }

    void refresh(bool cursorOnly) override
    {
        if (closed_ || broken_ || !built_) return;
        if (cursorOnly && !anyKeys_) return;
        ofx::Instance* inst = live();
        if (!inst) {
            broken_ = true;
            if (requestRebuild) requestRebuild();
            return;
        }
        std::vector<Snap> snaps;
        {
            // A render holds this lock for a whole frame; retry shortly instead of freezing.
            std::unique_lock<std::recursive_mutex> lk(inst->mutex, std::try_to_lock);
            if (!lk.owns_lock()) {
                soon_->start(30);
                return;
            }
            snapshot(inst, time_(), snaps);
        }
        if (!sameShape(snaps)) {
            broken_ = true;
            if (requestRebuild) requestRebuild();
            return;
        }
        apply(snaps);
    }

private:
    // Controls are built from the instance's parameters under its lock. A render holds the lock for a whole frame (a chain's effects stay locked until the chain ends), so this says it is waiting and retries shortly.
    void tryBuild()
    {
        if (closed_ || built_) return;
        ofx::Instance* inst = AppState::get().fx.video(slotId);
        if (!inst || inst != inst_) {
            broken_ = true;
            if (requestRebuild) requestRebuild();
            return;
        }
        std::unique_lock<std::recursive_mutex> lk(inst->mutex, std::try_to_lock);
        if (!lk.owns_lock()) {
            if (!waiting_) {
                waiting_ = new QLabel("Waiting for the effect to finish drawing...", this);
                waiting_->setObjectName("fxWaiting");
                waiting_->setAlignment(Qt::AlignHCenter);
                top_->addSpacing(20);
                top_->addWidget(waiting_);
            }
            wait_->start(30);
            return;
        }
        std::vector<Snap> snaps;
        snapshot(inst, time_(), snaps);
        for (const Snap& s : snaps) params_.push_back(s.p);
        if (waiting_) {
            delete waiting_;
            waiting_ = nullptr;
            while (QLayoutItem* it = top_->takeAt(0)) delete it;
        }
        built_ = true;
        makeOwnWindow();
        build(snaps);
        bool anyShown = false;
        for (const Snap& sn : snaps)
            if (sn.editor || (!sn.secret && sn.kind != ofx::ParamKind::Page && sn.kind != ofx::ParamKind::Unknown && sn.kind != ofx::ParamKind::Group &&
                              (sn.kind != ofx::ParamKind::Custom || sn.interact))) anyShown = true;
        if (!anyShown && !host_) {
            QLabel* none = new QLabel("(No adjustable parameters)", this);
            none->setObjectName("fxNoParams");
            none->setEnabled(false);
            top_->addWidget(none);
        }
        top_->addStretch(1);
        apply(snaps);
        if (hasInteracts_) {
            std::shared_ptr<std::atomic<bool>> alive = alive_;
            inst->onParamInteractRedraw = [this, alive](ofx::Param* p) {
                AppState::post([this, alive, p] {
                    if (!*alive) return;
                    for (OfxRow& r : rows_)
                        if (r.interact && r.param == p) r.interact->update();
                });
            };
        }
        inst->beginEdit();
        editing_ = true;
    }

    ofx::Instance* live() const
    {
        ofx::Instance* now = AppState::get().fx.video(slotId);
        if (!now || now != inst_) return nullptr;
        if (!built_) return now;
        // a new instance could in theory get the old one's address: its parameters would not be the old ones too
        if (now->params.params.size() != params_.size()) return nullptr;
        for (size_t k = 0; k < params_.size(); ++k)
            if (now->params.params[k].get() != params_[k]) return nullptr;
        return now;
    }

    void makeOwnWindow()
    {
        if (!kHostPluginWindows || !inst_->hasHwndInteract()) return;
        host_ = new NativeHost(this);
        top_->addWidget(host_);
        int minW = 0, minH = 0, prefW = 0, prefH = 0;
        void* handle = host_->handle();
        if (!handle || !inst_->createHwndInteract(handle, &minW, &minH, &prefW, &prefH)) {
            delete host_;
            host_ = nullptr;
            return;
        }
        ownWindow_ = true;
        const double sc = host_->scale();
        const int wantH = prefH > 0 ? prefH : minH;
        int h = (int)std::ceil((wantH > 0 ? wantH : 48) / sc);
        h = std::max(h, (int)std::ceil(minH / sc));
        host_->setFixedHeight(std::min(std::max(h, 8), 900));
        host_->setMinimumWidth(std::min(std::max((int)std::ceil(minW / sc), 40), 600));
        host_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        host_->onPlaced = [this] { placeOwnWindow(); };
        host_->onWindowLost = [this] {
            // the system window the effect's window sat in is going: the effect must let go of it now
            if (ofx::Instance* inst = live()) inst->disposeHwndInteract();
            ownWindow_ = false;
            broken_ = true;
            if (requestRebuild) requestRebuild();
        };
        top_->addSpacing(6);
        placeOwnWindow();
    }

    void placeOwnWindow()
    {
        if (closed_ || !host_ || !ownWindow_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        const double sc = host_->scale();
        inst->moveHwndInteract(0, 0, (int)std::floor(host_->width() * sc + 0.5), (int)std::floor(host_->height() * sc + 0.5));
    }

    static bool hasRow(const Snap& s)
    {
        if (s.member) return false;                 // a setting of someone else's editor
        if (s.editor) return true;
        if (s.interact && s.kind != ofx::ParamKind::Group && s.kind != ofx::ParamKind::Page) return true;
        switch (s.kind) {
        case ofx::ParamKind::Group: case ofx::ParamKind::Page: case ofx::ParamKind::Custom: case ofx::ParamKind::Unknown: return false;
        default: return true;
        }
    }

    void build(const std::vector<Snap>& snaps)
    {
        std::map<std::string, int> groupByName;
        for (size_t i = 0; i < snaps.size(); ++i) {
            if (snaps[i].kind != ofx::ParamKind::Group) continue;
            OfxGroup g;
            g.param = snaps[i].p;
            g.index = i;
            g.name = snaps[i].name;
            groupByName[g.name] = (int)groups_.size();
            groups_.push_back(g);
        }
        auto groupOf = [&](const Snap& s) -> int {
            auto it = groupByName.find(s.parent);
            return (s.parent.empty() || it == groupByName.end()) ? -1 : it->second;
        };
        for (size_t g = 0; g < groups_.size(); ++g) {
            groups_[g].parent = groupOf(snaps[groups_[g].index]);
            if (groups_[g].parent == (int)g) groups_[g].parent = -1;
        }
        // groups that contain each other (a mistake in the effect) are moved to the top level
        for (size_t g = 0; g < groups_.size(); ++g) {
            int depth = 0;
            for (int up = groups_[g].parent; up >= 0; up = groups_[(size_t)up].parent)
                if (++depth > (int)groups_.size()) break;
            if (depth > (int)groups_.size()) { groups_[g].parent = -1; depth = 0; }
            groups_[g].depth = depth;
        }
        for (size_t g = 0; g < groups_.size(); ++g) {
            int depth = 0;
            for (int up = groups_[g].parent; up >= 0 && depth <= (int)groups_.size(); up = groups_[(size_t)up].parent) ++depth;
            groups_[g].depth = depth;
        }

        const QFontMetrics fm(font());
        int widest = 0;
        for (const Snap& s : snaps) {
            if (!hasRow(s) || s.kind == ofx::ParamKind::PushButton) continue;
            const int g = groupOf(s);
            const int depth = g >= 0 ? groups_[(size_t)g].depth + 1 : 0;
            widest = std::max(widest, fm.horizontalAdvance(qs(s.label)) + depth * kIndent);
        }
        labelWidth_ = std::min(std::max(widest + 10, 70), 190);

        for (OfxGroup& g : groups_) makeGroup(g, snaps[g.index]);
        rows_.reserve(snaps.size());
        for (size_t i = 0; i < snaps.size(); ++i) {
            const Snap& s = snaps[i];
            const int parent = s.kind == ofx::ParamKind::Group ? groups_[(size_t)groupByName[s.name]].parent : groupOf(s);
            QVBoxLayout* into = parent >= 0 ? groups_[(size_t)parent].bodyLayout : top_;
            if (s.kind == ofx::ParamKind::Group) {
                into->addWidget(groups_[(size_t)groupByName[s.name]].box);
            } else if (hasRow(s)) {
                makeRow(s, i, parent);
                into->addWidget(rows_.back().row);
            }
        }
    }

    void makeGroup(OfxGroup& g, const Snap& s)
    {
        g.box = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(g.box);
        v->setContentsMargins(0, 3, 0, 3);
        v->setSpacing(2);
        g.header = new GroupHeader(g.box);
        g.header->setObjectName("fxGroup");
        g.header->setProperty("param", qs(g.name));
        v->addWidget(g.header);
        g.body = new QWidget(g.box);
        g.bodyLayout = new QVBoxLayout(g.body);
        g.bodyLayout->setContentsMargins(kIndent, 0, 0, 0);
        g.bodyLayout->setSpacing(2);
        v->addWidget(g.body);
        bool open = s.groupOpen;
        if (memory_) {
            auto it = memory_->find(std::make_pair(slotId, g.name));
            if (it != memory_->end()) open = it->second;
        }
        g.header->setChecked(open);
        g.body->setVisible(open);
        QWidget* body = g.body;
        const std::string name = g.name;
        QObject::connect(g.header, &QAbstractButton::toggled, this, [this, body, name](bool on) {
            body->setVisible(on);
            if (memory_) (*memory_)[std::make_pair(slotId, name)] = on;
        });
    }

    NumBox* makeBox(OfxRow& r, QWidget* parent, size_t ri)
    {
        NumBox* b = new NumBox(parent);
        b->setObjectName("fxBox");
        QObject::connect(b, &QDoubleSpinBox::valueChanged, r.row, [this, ri](double) { boxEdited(ri); });
        r.boxes.push_back(b);
        return b;
    }

    void makeRow(const Snap& s, size_t index, int group)
    {
        typedef ofx::ParamKind K;
        const size_t ri = rows_.size();
        rows_.push_back(OfxRow());
        OfxRow& r = rows_.back();
        r.param = s.p;
        r.index = index;
        r.kind = s.kind;
        r.dim = s.dim;
        r.group = group;
        r.stringMode = s.stringMode;
        r.fileFilter = QString::fromStdString(s.fileFilter);
        r.spec = s.editor;
        const int depth = group >= 0 ? groups_[(size_t)group].depth + 1 : 0;

        r.row = new QWidget(this);
        r.row->setObjectName("fxRow");
        r.row->setProperty("param", qs(s.name));
        QHBoxLayout* h = new QHBoxLayout(r.row);
        h->setContentsMargins(0, 1, 0, 1);
        h->setSpacing(6);
        r.label = new ElideLabel(r.row);
        r.label->setFixedWidth(std::max(40, labelWidth_ - depth * kIndent));
        h->addWidget(r.label);

        bool canReset = false;
        if (s.interact) {
            ParamInteractView* v = new ParamInteractView(r.row, s.p, s, [this] { return live(); }, time_);
            v->onPress = [this, ri] { beginEdit(ri, "Change"); };
            v->onRelease = [this] { endEdit(); };
            r.interact = v;
            h->addWidget(v, 1);
            canReset = s.dim > 0 || s.kind == K::String || s.kind == K::Custom;
            hasInteracts_ = true;
        }
        if (s.editor && !s.interact) {
            if (FxEditor* ed = makeEditor(*s.editor, r.row)) {
                r.editor = ed;
                ed->begin = [this, ri] { beginEdit(ri, "Change"); };
                ed->end = [this] { endEdit(); };
                ed->setText = [this, ri](const std::string& n, const std::string& t) { editorText(ri, n, t); };
                ed->setNumbers = [this, ri](const std::string& n, const std::vector<double>& v) { editorNumbers(ri, n, v); };
                ed->previewFrame = [this] { return playback_ ? playback_->lastFrame() : QImage(); };
                ed->tell = [this](const std::string& m) { QMessageBox::information(window(), "Reno", qs(m)); };
                h->addWidget(ed, 1);
                h->setAlignment(r.label, Qt::AlignTop);
                canReset = !s.editor->keyed.empty();
            }
        }
        switch ((s.interact || r.editor) ? K::Unknown : s.kind) {
        case K::Double: case K::Int: {
            if (isAngle(s)) {
                r.dial = new AngleDial(r.row);
                r.dial->onPress = [this, ri] { beginEdit(ri, "Change"); };
                r.dial->onRelease = [this] { endEdit(); };
                r.dial->onTurn = [this, ri](double deg) { dialTurned(ri, deg); };
                h->addWidget(r.dial);
                NumBox* b = makeBox(r, r.row, ri);
                b->setSuffix(QString::fromUtf8("\xc2\xb0"));
                b->setFixedWidth(84);
                h->addWidget(b);
                h->addStretch(1);
            } else {
                r.slider = new FxSlider(r.row);
                r.slider->setObjectName("fxSlider");
                r.slider->onDoubleClick = [this, ri] { resetRow(ri); };
                QObject::connect(r.slider, &QSlider::valueChanged, r.row, [this, ri](int pos) { sliderMoved(ri, pos); });
                QObject::connect(r.slider, &QSlider::sliderPressed, r.row, [this, ri] { beginEdit(ri, "Change"); });
                QObject::connect(r.slider, &QSlider::sliderReleased, r.row, [this] { endEdit(); });
                h->addWidget(r.slider, 1);
                NumBox* b = makeBox(r, r.row, ri);
                b->setFixedWidth(84);
                h->addWidget(b);
                h->addStretch(0);
            }
            canReset = true;
            break;
        }
        case K::Bool:
            r.check = new QCheckBox(r.row);
            r.check->setObjectName("fxCheck");
            QObject::connect(r.check, &QCheckBox::toggled, r.row, [this, ri](bool on) {
                if (!updating_) setValues(ri, std::vector<double>(1, on ? 1.0 : 0.0));
            });
            h->addWidget(r.check);
            h->addStretch(1);
            canReset = true;
            break;
        case K::Choice:
            r.combo = new FxCombo(r.row);
            r.combo->setObjectName("fxCombo");
            r.combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            r.combo->setMinimumContentsLength(12);
            QObject::connect(r.combo, &QComboBox::currentIndexChanged, r.row, [this, ri](int at) {
                if (!updating_ && at >= 0) setValues(ri, std::vector<double>(1, (double)at));
            });
            h->addWidget(r.combo, 1);
            canReset = true;
            break;
        case K::RGB: case K::RGBA:
            r.colour = new ColourButton(r.row);
            r.colour->setObjectName("fxColour");
            QObject::connect(r.colour, &QAbstractButton::clicked, r.row, [this, ri] { pickColour(ri); });
            h->addWidget(r.colour);
            for (int k = 0; k < (s.kind == K::RGBA ? 4 : 3); ++k) {
                NumBox* b = new NumBox(r.row);
                b->setObjectName("fxColourBox");
                b->setDecimals(3);
                b->setSingleStep(0.01);
                b->setRange(0.0, 1.0);
                b->setButtonSymbols(QAbstractSpinBox::NoButtons);
                b->setFixedWidth(46);
                b->setToolTip(QStringList({"Red", "Green", "Blue", "Alpha (opacity)"}).at(k));
                QObject::connect(b, &QDoubleSpinBox::valueChanged, r.row, [this, ri](double) { colourBoxEdited(ri); });
                r.colourBoxes.push_back(b);
                h->addWidget(b);
            }
            h->addStretch(1);
            canReset = true;
            break;
        case K::Double2D: case K::Double3D: case K::Int2D: case K::Int3D:
            if (isPosition(s)) {
                r.pad = new XYPad(r.row);
                r.pad->onPress = [this, ri] { beginEdit(ri, "Move"); };
                r.pad->onRelease = [this] { endEdit(); };
                r.pad->onMove = [this, ri](double x, double y) { padMoved(ri, x, y); };
                h->addWidget(r.pad);
            }
            for (int k = 0; k < s.dim; ++k) {
                QLabel* dl = new QLabel(r.row);
                dl->setTextFormat(Qt::PlainText);
                r.dimLabels.push_back(dl);
                h->addWidget(dl);
                NumBox* b = makeBox(r, r.row, ri);
                b->setMinimumWidth(56);
                h->addWidget(b, 1);
            }
            canReset = true;
            break;
        case K::String:
            if (s.stringMode == kOfxParamStringIsLabel) {
                r.textLabel = new QLabel(r.row);
                r.textLabel->setObjectName("fxTextLabel");
                r.textLabel->setTextFormat(Qt::PlainText);
                r.textLabel->setWordWrap(true);
                r.textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
                h->addWidget(r.textLabel, 1);
            } else if (s.stringMode == kOfxParamStringIsMultiLine || s.stringMode == kOfxParamStringIsRichTextFormat) {
                r.text = new QPlainTextEdit(r.row);
                r.text->setObjectName("fxText");
                r.text->setTabChangesFocus(true);
                r.text->setFixedHeight(QFontMetrics(r.text->font()).lineSpacing() * 4 + 12);
                h->setAlignment(r.label, Qt::AlignTop);
                QPlainTextEdit* edit = r.text;
                QObject::connect(r.text, &QPlainTextEdit::textChanged, r.row, [this, ri, edit] {
                    if (!updating_) setString(ri, edit->toPlainText());
                });
                h->addWidget(r.text, 1);
                canReset = true;
            } else {
                r.line = new QLineEdit(r.row);
                r.line->setObjectName("fxLine");
                QObject::connect(r.line, &QLineEdit::textEdited, r.row, [this, ri](const QString& t) {
                    if (!updating_) setString(ri, t);
                });
                QObject::connect(r.line, &QLineEdit::editingFinished, r.row, [this] { endEdit(); });
                h->addWidget(r.line, 1);
                if (s.stringMode == kOfxParamStringIsFilePath || s.stringMode == kOfxParamStringIsDirectoryPath) {
                    QPushButton* browse = new QPushButton("Browse...", r.row);
                    browse->setObjectName("fxBrowse");
                    QObject::connect(browse, &QPushButton::clicked, r.row, [this, ri] { browse_(ri); });
                    h->addWidget(browse);
                }
                canReset = true;
            }
            break;
        case K::PushButton:
            r.push = new QPushButton(r.row);
            r.push->setObjectName("fxPush");
            QObject::connect(r.push, &QPushButton::clicked, r.row, [this, ri] { pushed(ri); });
            h->addWidget(r.push);
            h->addStretch(1);
            break;
        default:
            break;
        }

        if (canReset) {
            r.reset = smallButton(r.row, glyphIcon(Glyph::Reset, palette()), "Back to the default", "fxReset");
            r.reset->setFixedSize(22, 22);
            QObject::connect(r.reset, &QToolButton::clicked, r.row, [this, ri] { resetRow(ri); });
            h->addWidget(r.reset);
            if (r.editor) h->setAlignment(r.reset, Qt::AlignTop);
        } else {
            h->addSpacing(22 + h->spacing());
        }
        if (s.animates && (s.dim > 0 || r.editor)) {
            r.prevKey = smallButton(r.row, glyphIcon(Glyph::Left, palette()), "Go to the previous keyframe", "fxPrevKey");
            r.nextKey = smallButton(r.row, glyphIcon(Glyph::Right, palette()), "Go to the next keyframe", "fxNextKey");
            for (QToolButton* b : {r.prevKey, r.nextKey}) {
                b->setFixedSize(16, 22);
                b->setIconSize(QSize(12, 12));
                h->addWidget(b);
                if (r.editor) h->setAlignment(b, Qt::AlignTop);
            }
            h->setSpacing(6);
            QObject::connect(r.prevKey, &QToolButton::clicked, r.row, [this, ri] { goToKey(ri, -1); });
            QObject::connect(r.nextKey, &QToolButton::clicked, r.row, [this, ri] { goToKey(ri, 1); });
            r.key = smallButton(r.row, keyIcon(palette()), QString(), "fxKey");
            r.key->setCheckable(true);
            r.key->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            r.key->setFixedSize(46, 22);
            QObject::connect(r.key, &QToolButton::toggled, r.row, [this, ri](bool on) { keyToggled(ri, on); });
            h->addWidget(r.key);
            if (r.editor) h->setAlignment(r.key, Qt::AlignTop);
        } else {
            h->addSpacing(16 + 16 + 46 + 3 * h->spacing());
        }
    }

    bool sameShape(const std::vector<Snap>& snaps) const
    {
        if (snaps.size() != params_.size()) return false;
        for (const OfxRow& r : rows_) {
            const Snap& s = snaps[r.index];
            if (s.p != r.param || s.kind != r.kind || s.stringMode != r.stringMode) return false;
            const int g = r.group;
            if ((g >= 0 ? groups_[(size_t)g].name : std::string()) != groupNameFor(s.parent)) return false;
        }
        return true;
    }

    std::string groupNameFor(const std::string& parent) const
    {
        for (const OfxGroup& g : groups_)
            if (g.name == parent) return parent;
        return std::string();
    }

    void setUpNumbers(OfxRow& r, const Snap& s)
    {
        std::vector<double> key;
        for (int k = 0; k < s.dim && k < 4; ++k) { key.push_back(s.lo[k]); key.push_back(s.hi[k]); }
        key.push_back(s.dlo);
        key.push_back(s.dhi);
        key.push_back(s.inc);
        key.push_back(s.digits);
        if (r.ranged && key == r.rangeKey) return;
        r.ranged = true;
        r.rangeKey = key;

        const bool whole = wholeKind(r.kind);
        const double big = whole ? 2147483647.0 : 1.0e9;
        int decimals = whole ? 0 : std::min(std::max(s.digits, 0), 8);
        double step = whole ? 1.0 : (s.inc > 0 && std::isfinite(s.inc) ? s.inc : 1.0);

        if (r.slider) {
            double lo = s.dlo, hi = s.dhi;
            if (!saneRange(lo, hi)) { lo = s.lo[0]; hi = s.hi[0]; }
            const bool ok = saneRange(lo, hi);
            if (ok) {
                if (saneRange(s.lo[0], s.hi[0]) && std::max(lo, s.lo[0]) < std::min(hi, s.hi[0])) {
                    lo = std::max(lo, s.lo[0]);
                    hi = std::min(hi, s.hi[0]);
                }
                int steps = kSliderSteps;
                if (whole) {
                    steps = (hi - lo) <= 20000 ? (int)std::floor(hi - lo + 0.5) : 10000;
                } else {
                    const double n = (hi - lo) / step;
                    if (n >= 50 && n <= 20000 && std::fabs(n - std::floor(n + 0.5)) < 1e-6) steps = (int)std::floor(n + 0.5);
                    else step = (hi - lo) / 100.0;
                    const double per = (hi - lo) / steps;
                    const int need = (int)std::ceil(-std::log10(per) - 1e-9);
                    decimals = std::max(decimals, std::min(std::max(need, 0), 6));
                }
                r.lo = lo;
                r.hi = hi;
                r.steps = std::max(1, steps);
                QSignalBlocker b(r.slider);
                r.slider->setRange(0, r.steps);
                r.slider->setSingleStep(1);
                r.slider->setPageStep(std::max(1, r.steps / 10));
            }
            if (r.slider->isHidden() == ok) r.slider->setVisible(ok);
            if (!r.boxes.empty()) {
                if (ok) r.boxes[0]->setFixedWidth(84);
                else { r.boxes[0]->setMinimumWidth(84); r.boxes[0]->setMaximumWidth(160); }
            }
        }
        for (size_t k = 0; k < r.boxes.size(); ++k) {
            const double lo = (std::isfinite(s.lo[k]) && s.lo[k] > -big) ? s.lo[k] : -big;
            const double hi = (std::isfinite(s.hi[k]) && s.hi[k] < big) ? s.hi[k] : big;
            double st = step;
            if (!whole && hi - lo < 1.0e8 && st >= (hi - lo)) st = (hi - lo) / 100.0;
            QSignalBlocker b(r.boxes[k]);
            r.boxes[k]->setDecimals(decimals);
            r.boxes[k]->setRange(lo, std::max(lo, hi));
            r.boxes[k]->setSingleStep(st > 0 ? st : 1.0);
        }
    }

    static QColor toColour(const std::vector<double>& v, bool alpha)
    {
        auto c = [&](size_t k, double def) { return (float)std::min(1.0, std::max(0.0, k < v.size() ? v[k] : def)); };
        return QColor::fromRgbF(c(0, 0), c(1, 0), c(2, 0), alpha ? c(3, 1) : 1.0f);
    }

    void applyRow(OfxRow& r, const Snap& s)
    {
        typedef ofx::ParamKind K;
        const QString label = qs(s.label);
        if (r.push) {
            if (r.push->text() != label) r.push->setText(label);
        } else {
            r.label->setText(label);
        }
        QString tip = qs(s.hint);
        if (!r.push && r.label->shortened()) tip = tip.isEmpty() ? label : label + "\n" + tip;
        if (r.row->toolTip() != tip) r.row->setToolTip(tip);
        if (r.row->isEnabled() != s.enabled) r.row->setEnabled(s.enabled);

        switch ((r.interact || r.editor) ? K::Unknown : r.kind) {
        case K::Double: case K::Int:
            setUpNumbers(r, s);
            if (!r.boxes[0]->typing()) r.boxes[0]->show(s.v[0]);
            if (r.dial && !r.dial->down()) r.dial->setAngle(s.v[0]);
            if (r.slider && !r.slider->isHidden() && !r.slider->isSliderDown()) {
                const int pos = r.posOf(s.v[0]);
                if (r.slider->value() != pos) {
                    QSignalBlocker b(r.slider);
                    r.slider->setValue(pos);
                }
            }
            break;
        case K::Bool:
            if (r.check->isChecked() != (s.v[0] != 0.0)) {
                QSignalBlocker b(r.check);
                r.check->setChecked(s.v[0] != 0.0);
            }
            break;
        case K::Choice: {
            if (r.combo->view() && r.combo->view()->isVisible()) break;
            QSignalBlocker b(r.combo);
            if (s.choices != r.choices || r.combo->count() != (int)s.choices.size()) {
                r.choices = s.choices;
                r.combo->clear();
                for (const std::string& c : s.choices) r.combo->addItem(qs(c));
            }
            const int at = (int)std::floor(s.v[0] + 0.5);
            const int want = (at >= 0 && at < r.combo->count()) ? at : -1;
            if (r.combo->currentIndex() != want) r.combo->setCurrentIndex(want);
            break;
        }
        case K::RGB: case K::RGBA: {
            const bool alpha = r.kind == K::RGBA;
            r.colour->setColour(toColour(s.v, alpha));
            for (size_t k = 0; k < r.colourBoxes.size() && k < s.v.size(); ++k) {
                NumBox* b = r.colourBoxes[k];
                const double hi = std::isfinite(s.hi[k]) && s.hi[k] > 1.0 && s.hi[k] < 1.0e6 ? s.hi[k] : 1.0;
                if (b->maximum() != hi) { QSignalBlocker bl(b); b->setMaximum(hi); }
                if (!b->typing()) b->show(s.v[k]);
            }
            break;
        }
        case K::Double2D: case K::Double3D: case K::Int2D: case K::Int3D:
            setUpNumbers(r, s);
            for (size_t k = 0; k < r.boxes.size(); ++k) {
                if (!r.boxes[k]->typing()) r.boxes[k]->show(s.v[k]);
                const QString dl = k < s.dimLabels.size() ? qs(s.dimLabels[k]) : QString();
                if (r.dimLabels[k]->text() != dl) r.dimLabels[k]->setText(dl);
            }
            if (r.pad) {
                const ProjectSettings& ps = AppState::get().project().settings;
                const double fw = s.normalised ? 1.0 : ps.width * ps.pixelAspect, fh = s.normalised ? 1.0 : ps.height;
                r.pad->setRange(0, fw, 0, fh);
                if (!r.pad->down()) r.pad->setPoint(s.v[0], s.v[1]);
            }
            break;
        case K::String: {
            const QString t = qs(s.str);
            r.mustExist = s.mustExist;
            if (r.textLabel) {
                if (r.textLabel->text() != t) r.textLabel->setText(t);
            } else if (r.text) {
                if (r.text->toPlainText() != t) {
                    QSignalBlocker b(r.text);
                    r.text->setPlainText(t);
                }
            } else if (r.line) {
                if (r.line->text() != t) r.line->setText(t);
            }
            break;
        }
        default:
            break;
        }

        if (r.key) {
            r.keyTimes = s.keyTimes;
            const double now = time_();
            bool before = false, after = false;
            for (double kt : s.keyTimes) {
                if (kt < now - 1e-6) before = true;
                if (kt > now + 1e-6) after = true;
            }
            if (r.prevKey->isEnabled() != before) r.prevKey->setEnabled(before);
            if (r.nextKey->isEnabled() != after) r.nextKey->setEnabled(after);
            r.keys = s.keys;
            const bool on = s.keys > 0;
            if (r.key->isChecked() != on) {
                QSignalBlocker b(r.key);
                r.key->setChecked(on);
            }
            const QString count = on ? QString::number((qulonglong)s.keys) : QString();
            if (r.key->text() != count) r.key->setText(count);
            const QString kt = on ? QString("%1 keyframe%2. Changes set a keyframe at the cursor.\nClick to remove the keyframes and keep the current value.")
                                        .arg((qulonglong)s.keys).arg(s.keys == 1 ? "" : "s")
                                  : QString("Keyframes are off: changes apply to the whole effect.\nClick to make changes set keyframes at the cursor.");
            if (r.key->toolTip() != kt) r.key->setToolTip(kt);
        }
    }

    void apply(const std::vector<Snap>& snaps)
    {
        updating_ = true;
        anyKeys_ = false;
        std::vector<char> used(groups_.size(), 0);
        std::map<std::string, size_t> byName;
        for (OfxRow& r : rows_) {
            const Snap& s = snaps[r.index];
            applyRow(r, s);
            if (r.interact) r.interact->update();
            if (r.editor && r.spec) {
                if (byName.empty())
                    for (size_t k = 0; k < snaps.size(); ++k) byName[snaps[k].name] = k;
                EdValues vals;
                for (const std::string& n : r.spec->uses) {
                    auto it = byName.find(n);
                    if (it == byName.end()) continue;
                    const Snap& m = snaps[it->second];
                    EdValue e;
                    e.present = true;
                    e.text = m.str;
                    e.v = m.v;
                    e.lo = m.lo[0];
                    e.hi = m.hi[0];
                    e.enabled = m.enabled;
                    vals[n] = e;
                }
                r.editor->showValues(vals);
            }
            if (s.keys > 0) anyKeys_ = true;
            const bool hide = s.secret && !s.editor;
            if (r.row->isHidden() != hide) r.row->setVisible(!hide);
            if (!hide)
                for (int g = r.group, n = 0; g >= 0 && n <= (int)groups_.size(); g = groups_[(size_t)g].parent, ++n) used[(size_t)g] = 1;
        }
        for (size_t k = 0; k < groups_.size(); ++k) {
            OfxGroup& g = groups_[k];
            const Snap& s = snaps[g.index];
            const QString label = qs(s.label);
            if (g.header->text() != label) { g.header->setText(label); g.header->update(); }
            const QString tip = qs(s.hint);
            if (g.header->toolTip() != tip) g.header->setToolTip(tip);
            if (g.body->isEnabled() != s.enabled) g.body->setEnabled(s.enabled);
            const bool show = !s.secret && used[k];
            if (g.box->isHidden() == show) g.box->setVisible(show);
        }
        updating_ = false;
    }

    // ---- the user changed something; undo: each change is one step (AppState keeps the effect's settings from before it)
    std::string nameFor(size_t ri, const char* verb) const
    {
        const QString label = rows_[ri].push ? rows_[ri].push->text() : rows_[ri].label->text();
        return std::string(verb) + " \"" + s8(label.isEmpty() ? QString("Setting") : label) + "\"";
    }

    void beginEdit(size_t ri, const char* verb)
    {
        if (closed_) return;
        AppState& app = AppState::get();
        if (app.fxEditOpen(slotId)) return;
        editName_ = nameFor(ri, verb);
        app.beginFxEdit(slotId);
    }

    void endEdit()
    {
        typing_->stop();
        AppState& app = AppState::get();
        if (app.fxEditOpen(slotId)) app.endFxEdit(slotId, editName_);
    }

    // A change made in one go: its own undo step, unless it is part of a change already going on.
    void oneStep(size_t ri, const char* verb, const std::function<void()>& fn)
    {
        AppState& app = AppState::get();
        if (app.fxEditOpen(slotId)) { fn(); return; }
        editName_ = nameFor(ri, verb);
        app.fxEditStep(slotId, editName_, fn);
    }

    void setValues(size_t ri, const std::vector<double>& v)
    {
        if (closed_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        OfxRow& r = rows_[ri];
        oneStep(ri, "Change", [&] { inst->userSetValue(r.param, v, time_(), r.key && r.key->isChecked()); });
        soon_->start(0);
    }

    void setString(size_t ri, const QString& t)
    {
        if (closed_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        beginEdit(ri, "Type in");
        inst->userSetString(rows_[ri].param, s8(t), time_());
        typing_->start();
        soon_->start(0);
    }

    void dialTurned(size_t ri, double degrees)
    {
        if (updating_ || closed_) return;
        OfxRow& r = rows_[ri];
        NumBox* b = r.boxes[0];
        const double v = std::min(b->maximum(), std::max(b->minimum(), degrees));
        r.dial->setAngle(v);
        {
            QSignalBlocker bl(b);
            b->setValue(v);
        }
        setValues(ri, std::vector<double>(1, v));
    }

    void padMoved(size_t ri, double x, double y)
    {
        if (updating_ || closed_) return;
        OfxRow& r = rows_[ri];
        std::vector<double> v;
        const double xy[2] = {x, y};
        for (size_t k = 0; k < r.boxes.size(); ++k) {
            double val = k < 2 ? xy[k] : r.boxes[k]->value();
            val = std::min(r.boxes[k]->maximum(), std::max(r.boxes[k]->minimum(), val));
            const double m = std::pow(10.0, r.boxes[k]->decimals());
            val = std::floor(val * m + 0.5) / m;
            QSignalBlocker bl(r.boxes[k]);
            r.boxes[k]->setValue(val);
            v.push_back(val);
        }
        r.pad->setPoint(v[0], v.size() > 1 ? v[1] : 0.0);
        setValues(ri, v);
    }

    void colourBoxEdited(size_t ri)
    {
        if (updating_ || closed_) return;
        OfxRow& r = rows_[ri];
        std::vector<double> v;
        for (NumBox* b : r.colourBoxes) v.push_back(b->value());
        r.colour->setColour(toColour(v, r.kind == ofx::ParamKind::RGBA));
        setValues(ri, v);
    }

    void goToKey(size_t ri, int dir)
    {
        if (closed_ || !seek_) return;
        const double now = time_();
        double best = 0;
        bool found = false;
        for (double kt : rows_[ri].keyTimes) {
            if (dir < 0 && kt < now - 1e-6 && (!found || kt > best)) { best = kt; found = true; }
            if (dir > 0 && kt > now + 1e-6 && (!found || kt < best)) { best = kt; found = true; }
        }
        if (found) seek_(best);
    }

    void sliderMoved(size_t ri, int pos)
    {
        if (updating_ || closed_) return;
        OfxRow& r = rows_[ri];
        const double v = r.valueOfPos(pos);
        {
            QSignalBlocker b(r.boxes[0]);
            r.boxes[0]->setValue(v);
        }
        setValues(ri, std::vector<double>(1, v));
    }

    void boxEdited(size_t ri)
    {
        if (updating_ || closed_) return;
        OfxRow& r = rows_[ri];
        std::vector<double> v;
        for (NumBox* b : r.boxes) v.push_back(b->value());
        if (r.slider && !r.slider->isSliderDown()) {
            QSignalBlocker b(r.slider);
            r.slider->setValue(r.posOf(v[0]));
        }
        setValues(ri, v);
    }

    void keyToggled(size_t ri, bool on)
    {
        if (updating_ || closed_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        OfxRow& r = rows_[ri];
        const double t = time_();
        if (r.editor && r.spec) {
            oneStep(ri, on ? "Add Keyframe to" : "Remove Keyframes from", [&] {
                for (const std::string& n : r.spec->keyed) {
                    ofx::Param* q = inst->params.find(n);
                    if (!q) continue;
                    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                    if (!on) inst->userDeleteKeys(q, t);
                    else if (q->isText()) inst->userSetString(q, q->strAt(t), t, true);
                    else inst->userSetValue(q, q->valueAt(t), t, true);
                }
            });
            refresh(false);
            return;
        }
        if (on) {
            std::vector<double> v;
            {
                std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                v = r.param->valueAt(t);
            }
            oneStep(ri, "Add Keyframe to", [&] { inst->userSetValue(r.param, v, t, true); });
        } else {
            oneStep(ri, "Remove Keyframes from", [&] { inst->userDeleteKeys(r.param, t); });
        }
        refresh(false);
    }

    void resetRow(size_t ri)
    {
        if (closed_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        OfxRow& r = rows_[ri];
        if (!r.reset) return;
        const double t = time_();
        endEdit();
        if (r.editor && r.spec) {
            const bool key = r.key && r.key->isChecked();
            oneStep(ri, "Reset", [&] {
                for (const std::string& n : r.spec->keyed) {
                    ofx::Param* q = inst->params.find(n);
                    if (!q) continue;
                    if (q->isText()) {
                        inst->userSetString(q, q->props.getString(kOfxParamPropDefault), t, key);
                    } else {
                        std::vector<double> def;
                        for (int k = 0; k < q->dim; ++k) def.push_back(q->props.getDouble(kOfxParamPropDefault, k));
                        inst->userSetValue(q, def, t, key);
                    }
                }
            });
        } else if (r.kind == ofx::ParamKind::String) {
            std::string def;
            {
                std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                def = r.param->props.getString(kOfxParamPropDefault);
            }
            oneStep(ri, "Reset", [&] { inst->userSetString(r.param, def, t); });
        } else {
            std::vector<double> def;
            {
                std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                for (int k = 0; k < r.dim; ++k) def.push_back(r.param->props.getDouble(kOfxParamPropDefault, k));
            }
            oneStep(ri, "Reset", [&] { inst->userSetValue(r.param, def, t, r.key && r.key->isChecked()); });
        }
        refresh(false);
    }

    void pushed(size_t ri)
    {
        if (closed_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        oneStep(ri, "Press", [&] { inst->userPushButton(rows_[ri].param, time_()); });
        if (playback_) playback_->requestFrame();
        soon_->start(0);
    }

    bool keyedSetting(const OfxRow& r, const std::string& name) const
    {
        if (!r.key || !r.key->isChecked() || !r.spec) return false;
        for (const std::string& n : r.spec->keyed)
            if (n == name) return true;
        return false;
    }
    void editorText(size_t ri, const std::string& name, const std::string& text)
    {
        if (closed_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        OfxRow& r = rows_[ri];
        ofx::Param* q = inst->params.find(name);
        if (!q) return;
        const bool key = keyedSetting(r, name);
        oneStep(ri, "Change", [&] { inst->userSetString(q, text, time_(), key); });
        soon_->start(0);
    }
    void editorNumbers(size_t ri, const std::string& name, const std::vector<double>& v)
    {
        if (closed_) return;
        ofx::Instance* inst = live();
        if (!inst) return;
        OfxRow& r = rows_[ri];
        ofx::Param* q = inst->params.find(name);
        if (!q || q->dim == 0) return;
        const bool key = keyedSetting(r, name);
        oneStep(ri, "Change", [&] { inst->userSetValue(q, v, time_(), key); });
        soon_->start(0);
    }

    void pickColour(size_t ri)
    {
        if (closed_) return;
        OfxRow& r = rows_[ri];
        if (r.dialog) {
            r.dialog->raise();
            r.dialog->activateWindow();
            return;
        }
        const bool alpha = r.kind == ofx::ParamKind::RGBA;
        const QColor before = r.colour->colour();
        // Qt's own colour dialog: it reports the colour while choosing (preview follows) and handles see-through colours.
        QColorDialog* dlg = new QColorDialog(before, r.colour);
        dlg->setObjectName("fxColourDialog");
        dlg->setWindowTitle(r.label->text());
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setOption(QColorDialog::DontUseNativeDialog, true);
        dlg->setOption(QColorDialog::ShowAlphaChannel, alpha);
        r.dialog = dlg;
        endEdit();
        beginEdit(ri, "Change");
        auto set = [this, ri, alpha](const QColor& c) {
            std::vector<double> v = {c.redF(), c.greenF(), c.blueF()};
            if (alpha) v.push_back(c.alphaF());
            setValues(ri, v);
        };
        QObject::connect(dlg, &QColorDialog::currentColorChanged, r.row, [set](const QColor& c) { set(c); });
        QObject::connect(dlg, &QDialog::rejected, r.row, [set, before] { set(before); });
        QObject::connect(dlg, &QDialog::finished, r.row, [this](int) { endEdit(); });
        dlg->show();
    }

    void browse_(size_t ri)
    {
        if (closed_) return;
        OfxRow& r = rows_[ri];
        if (!r.line) return;
        const QString title = r.label->text();
        QString got;
        if (r.stringMode == kOfxParamStringIsDirectoryPath) got = QFileDialog::getExistingDirectory(window(), title, r.line->text());
        else if (r.mustExist) got = fileChooser()(window(), title, r.line->text(), r.fileFilter);
        else got = QFileDialog::getSaveFileName(window(), title, r.line->text());
        if (got.isEmpty() || closed_) return;
        r.line->setText(got);
        setString(ri, got);
        if (!r.fileFilter.isEmpty()) {
            if (ofx::Instance* inst = live())
                if (ofx::Param* nm = inst->params.find("LUTName"))
                    if (nm->kind == ofx::ParamKind::String && inst->plugin && inst->plugin->info.id == "com.vegascreativesoftware:lutfilter")
                        inst->userSetString(nm, s8(QFileInfo(got).completeBaseName()), time_());
        }
        endEdit();
    }

    ofx::Instance* inst_;
    std::vector<ofx::Param*> params_;
    std::function<double()> time_;
    std::function<void(double)> seek_;
    QTimer* typing_ = nullptr;
    std::string editName_;
    GroupMemory* memory_;
    Playback* playback_;
    QVBoxLayout* top_ = nullptr;
    QTimer* soon_ = nullptr;
    QTimer* wait_ = nullptr;
    QLabel* waiting_ = nullptr;
    NativeHost* host_ = nullptr;
    bool built_ = false, ownWindow_ = false, editing_ = false, updating_ = false, anyKeys_ = false;
    bool hasInteracts_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    int labelWidth_ = 120;
    std::vector<OfxRow> rows_;
    std::vector<OfxGroup> groups_;
};

// .......................................................................................... audio plug-ins
class AudioPanel : public Panel {
public:
    std::function<void()> onEditorSize;
    QString title;
    std::function<bool()> isBypassed;
    std::function<void(bool)> setBypassed;

    AudioPanel(QWidget* parent, uint64_t slot, AudioPlugin* plugin, bool wantEditor, bool wantPage = false)
        : Panel(parent), plugin_(plugin), wantEditor_(wantEditor), wantPage_(wantPage)
    {
        slotId = slot;
        setObjectName("fxAudioPanel");
        top_ = new QVBoxLayout(this);
        top_->setContentsMargins(kFrameLeft, kFrameTop, kFrameRight, kFrameBottom);
        top_->setSpacing(0);
    }

    void start() override
    {
        if (started_ || closed_) return;
        started_ = true;
        if (!live()) return;
        if (wantPage_ && buildPage()) return;
        if (wantEditor_ && openEditor()) return;
        buildSliders();
    }

    void shutDown() override
    {
        if (closed_) return;
        closed_ = true;
        finishEditLater(slotId, editName_);
        AudioPlugin* plugin = live();
        if (host_) {
            host_->onWindowLost = nullptr;
            if (plugin && editorOpen_) plugin->closeEditor();
            editorOpen_ = false;
            delete host_;
            host_ = nullptr;
        }
    }

    bool stale() const override { return broken_ || !live(); }
    bool editorOpen() const { return editorOpen_; }
    bool wantsIdle() const override { return editorOpen_ && !closed_; }
    QSize editorSize() const { return editorSize_ + QSize(2 + kFrameLeft + kFrameRight, 2 + kTitleH + kFrameTop + kFrameBottom); }

    void idle() override
    {
        if (closed_ || !editorOpen_) return;
        if (AudioPlugin* plugin = live()) plugin->idle();
        checkStopped();
    }

    // A 32-bit plug-in whose bridge process crashed or hung is silent; the window says so until playback restarts it.
    void checkStopped()
    {
        if (closed_ || !nameLabel_) return;
        AudioPlugin* plugin = live();
        if (!plugin) return;
        const std::string why = plugin->stoppedReason();
        const bool stopped = !why.empty();
        if (stopped == shownStopped_) return;
        shownStopped_ = stopped;
        nameLabel_->setText(stopped ? title + " (stopped)" : title);
        nameLabel_->setToolTip(stopped ? qs(why) : QString());
        if (stopped && host_ && bodyLayout_) {
            if (!stoppedNote_) {
                stoppedNote_ = new QLabel(body_);
                stoppedNote_->setWordWrap(true);
                stoppedNote_->setAlignment(Qt::AlignCenter);
                stoppedNote_->setMargin(12);
                bodyLayout_->insertWidget(0, stoppedNote_, 0, Qt::AlignLeft | Qt::AlignTop);
            }
            stoppedNote_->setText(qs(why));
            stoppedNote_->setFixedSize(editorSize_);
            stoppedNote_->show();
            host_->hide();
        } else if (!stopped && stoppedNote_) {
            stoppedNote_->hide();
            if (host_) host_->show();
            if (editorOpen_) {
                broken_ = true;
                if (requestRebuild) requestRebuild();
            }
        }
    }

    void refresh(bool cursorOnly) override
    {
        checkStopped();
        if (!closed_ && bypassBtn_ && isBypassed) {
            const bool by = isBypassed();
            if (bypassBtn_->isChecked() != by) {
                QSignalBlocker b(bypassBtn_);
                bypassBtn_->setChecked(by);
            }
        }
        if (cursorOnly || closed_ || broken_ || editorOpen_) return;
        AudioPlugin* plugin = live();
        if (!plugin) return;
        if (plugin->paramCount() != count_) {
            broken_ = true;
            if (requestRebuild) requestRebuild();
            return;
        }
        if (page_) page_->refresh();
        for (size_t k = 0; k < rows_.size(); ++k) showRow(plugin, k);
    }

    void paramChangedByPlugin(const AudioPlugin* from, int index)
    {
        if (closed_ || editorOpen_ || from != plugin_) return;
        AudioPlugin* plugin = live();
        if (!plugin) return;
        if (index < 0 || page_) refresh(false);
        else if ((size_t)index < rows_.size()) showRow(plugin, (size_t)index);
    }

    void editorResized(const AudioPlugin* from, int w, int h)
    {
        if (closed_ || !editorOpen_ || !host_ || from != plugin_ || w <= 0 || h <= 0) return;
        setEditorSize(w, h);
        if (onEditorSize) onEditorSize();
    }

private:
    AudioPlugin* live() const
    {
        AudioPlugin* now = AppState::get().fx.audio(slotId);
        return (now && now == plugin_) ? now : nullptr;
    }

    bool openEditor()
    {
        if (!kHostPluginWindows || !plugin_->hasEditor()) return false;
        makeFrame(true);
        host_ = new NativeHost(body_);
        bodyLayout_->addWidget(host_, 0, Qt::AlignLeft | Qt::AlignTop);
        void* handle = host_->handle();
        int w = 0, h = 0;
        if (!handle || !plugin_->openEditor(handle, &w, &h)) {
            delete host_;
            host_ = nullptr;
            frame_->deleteLater();
            frame_ = nullptr;
            bypassBtn_ = nullptr;
            nameLabel_ = nullptr;
            return false;
        }
        editorOpen_ = true;
        setEditorSize(w > 0 ? w : 400, h > 0 ? h : 300);
        host_->onWindowLost = [this] {
            if (AudioPlugin* plugin = live()) plugin->closeEditor();
            editorOpen_ = false;
            broken_ = true;
            if (requestRebuild) requestRebuild();
        };
        top_->addStretch(1);
        return true;
    }

    void setEditorSize(int w, int h)
    {
        const double sc = host_->scale();
        editorSize_ = QSize(std::max(16, (int)std::ceil(w / sc)), std::max(16, (int)std::ceil(h / sc)));
        host_->setFixedSize(editorSize_);
        if (frame_) frame_->setFixedSize(editorSize_ + QSize(2, 2 + kTitleH));
    }

    // VEGAS's frame round a plug-in's page: light grey with a darker title bar (name, bypass, About, ?). 'hug': sized to its content; otherwise fills the panel.
    void makeFrame(bool hug)
    {
        frame_ = new QFrame(this);
        frame_->setObjectName("fxFrame");
        frame_->setFrameShape(QFrame::NoFrame);
        frame_->setStyleSheet("QFrame#fxFrame { border: 1px solid #5c5c5c; background: transparent; }");
        QVBoxLayout* fl = new QVBoxLayout(frame_);
        fl->setContentsMargins(1, 1, 1, 1);
        fl->setSpacing(0);
        QWidget* bar = new TitleBar(frame_);
        bar->setObjectName("fxTitleBar");
        bar->setFixedHeight(kTitleH);
        QHBoxLayout* bl = new QHBoxLayout(bar);
        bl->setContentsMargins(6, 0, 4, 0);
        bl->setSpacing(4);
        QLabel* name = new QLabel(title, bar);
        name->setObjectName("fxTitleName");
        name->setTextFormat(Qt::PlainText);
        nameLabel_ = name;
        QFont f = name->font();
        f.setBold(true);
        name->setFont(f);
        QPalette white = name->palette();
        white.setColor(QPalette::WindowText, Qt::white);
        name->setPalette(white);
        bl->addWidget(name, 1);
        bypassBtn_ = new QToolButton(bar);
        bypassBtn_->setObjectName("fxFrameBypass");
        bypassBtn_->setAutoRaise(true);
        bypassBtn_->setCheckable(true);
        bypassBtn_->setFixedSize(18, 16);
        bypassBtn_->setFocusPolicy(Qt::NoFocus);
        bypassBtn_->setToolTip("Bypass this effect");
        {
            QIcon icon;
            for (int on = 0; on < 2; ++on) {
                QPixmap pm(36, 36);
                pm.setDevicePixelRatio(2.0);
                pm.fill(Qt::transparent);
                QPainter p(&pm);
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(QPen(on ? QColor(255, 224, 224) : QColor(36, 36, 36), 1.6));
                p.setBrush(on ? QBrush(QColor(200, 50, 50)) : QBrush(Qt::NoBrush));
                p.drawEllipse(QRectF(3, 3, 12, 12));
                p.drawLine(QPointF(5, 13), QPointF(13, 5));
                icon.addPixmap(pm, QIcon::Normal, on ? QIcon::On : QIcon::Off);
            }
            bypassBtn_->setIcon(icon);
            bypassBtn_->setIconSize(QSize(14, 14));
        }
        QObject::connect(bypassBtn_, &QToolButton::clicked, bypassBtn_, [this](bool on) {
            if (setBypassed) setBypassed(on);
        });
        bl->addWidget(bypassBtn_);
        QToolButton* about = new QToolButton(bar);
        about->setObjectName("fxFrameAbout");
        about->setText("About");
        about->setAutoRaise(true);
        about->setFocusPolicy(Qt::NoFocus);
        QPalette ap = about->palette();
        ap.setColor(QPalette::ButtonText, Qt::white);
        about->setPalette(ap);
        QToolButton* help = new QToolButton(bar);
        help->setObjectName("fxFrameHelp");
        help->setText("?");
        help->setAutoRaise(true);
        help->setFocusPolicy(Qt::NoFocus);
        help->setPalette(ap);
        for (QToolButton* b : {about, help}) {
            b->setFixedHeight(16);
            b->setStyleSheet("QToolButton { color: white; border: none; padding: 0px 2px; background: transparent; } QToolButton:hover { background: rgba(255,255,255,60); }");
            b->setToolTip("About this effect");
            bl->addWidget(b);
            QObject::connect(b, &QToolButton::clicked, b, [this] { about_(); });
        }
        fl->addWidget(bar);
        body_ = new QWidget(frame_);
        body_->setObjectName("fxFrameBody");
        bodyLayout_ = new QVBoxLayout(body_);
        bodyLayout_->setContentsMargins(0, 0, 0, 0);
        bodyLayout_->setSpacing(0);
        fl->addWidget(body_, 1);
        if (hug) top_->addWidget(frame_, 0, Qt::AlignLeft | Qt::AlignTop);
        else top_->addWidget(frame_, 1);
        if (isBypassed) {
            QSignalBlocker b(bypassBtn_);
            bypassBtn_->setChecked(isBypassed());
        }
    }

    void about_()
    {
        const AudioPluginDesc& d = plugin_->desc;
        QString text = QString("<b>%1</b><br>").arg(title.toHtmlEscaped());
        if (builtinPageUid(d) == "reno.distortion")
            text += "Reno's version of VEGAS Distortion: the same settings and the same sound. It saves as VEGAS Distortion, and "
                    "projects with VEGAS Distortion use it where VEGAS's own is not installed.";
        else if (const vegpack3::Entry* e = vegpack3::entryForBuiltin(builtinPageUid(d)))
            text += QString("Reno's version of %1: the same settings and the same sound. It saves as %1, and "
                            "projects with %1 use it where VEGAS's own is not installed.").arg(QString::fromUtf8(e->dxName));
        else if (const vegpack1::Info* pi = vegpack1::findInfo(builtinPageUid(d)))
            text += QString("Reno's version of VEGAS %1: the same settings and the same sound. It saves as VEGAS %1, and "
                            "projects with VEGAS %1 use it where VEGAS's own is not installed.").arg(QString::fromUtf8(pi->name));
        else if (!pack2VegasName(builtinPageUid(d)).empty())
            text += qs("Reno's version of " + pack2VegasName(builtinPageUid(d)) + ": the same settings and the same sound. It saves as " +
                       pack2VegasName(builtinPageUid(d)) + ", and projects with it use it where VEGAS's own is not installed.");
        else if (d.format == "BUILTIN")
            text += "A Reno audio effect that stands in for VEGAS's DirectX effect of the same name, with similar settings. "
                    "Projects made with the VEGAS effect use it when VEGAS's own is not installed. Its sound is Reno's own.";
        else {
            text += QString("%1 plug-in%2").arg(qs(d.format), qs(audioPluginKindNote(d)));
            if (!d.vendor.empty()) text += QString("<br>%1").arg(qs(d.vendor).toHtmlEscaped());
        }
        QMessageBox::about(window(), title, text);
    }

    bool buildPage()
    {
        if (!hasBuiltinPage(builtinPageUid(plugin_->desc))) return false;
        count_ = plugin_->paramCount();
        makeFrame(true);
        DxHost host;
        host.range = [this](int i, double* lo, double* hi) {
            AudioPlugin* p = live();
            return p && p->paramRange(i, lo, hi);
        };
        host.get = [this](int i) {
            AudioPlugin* p = live();
            double lo = 0, hi = 1;
            if (!p || !p->paramRange(i, &lo, &hi)) return 0.0;
            return lo + (double)p->paramGet(i) * (hi - lo);
        };
        host.set = [this](int i, double v) {
            AudioPlugin* p = live();
            double lo = 0, hi = 1;
            if (closed_ || !p || !p->paramRange(i, &lo, &hi)) return;
            const float n = (float)((v - lo) / (hi - lo));
            AppState& app = AppState::get();
            if (app.fxEditOpen(slotId)) p->paramSet(i, n);
            else app.fxEditStep(slotId, "Change \"" + p->paramName(i) + "\"", [&] { p->paramSet(i, n); });
        };
        host.name = [this](int i) {
            AudioPlugin* p = live();
            return p ? p->paramName(i) : std::string();
        };
        host.begin = [this](int i) {
            AudioPlugin* p = live();
            if (closed_ || !p || AppState::get().fxEditOpen(slotId)) return;
            editName_ = "Change \"" + p->paramName(i) + "\"";
            AppState::get().beginFxEdit(slotId);
        };
        host.end = [this] {
            if (AppState::get().fxEditOpen(slotId)) AppState::get().endFxEdit(slotId, editName_);
        };
        host.state = [this] {
            AudioPlugin* p = live();
            return p ? p->saveState() : std::string();
        };
        host.setState = [this](const std::string& st, const std::string& what) {
            AudioPlugin* p = live();
            if (closed_ || !p) return;
            AppState& app = AppState::get();
            if (app.fxEditOpen(slotId)) p->loadState(st);
            else app.fxEditStep(slotId, what, [&] { p->loadState(st); });
        };
        host.beginEdit = [this](const std::string& what) {
            AudioPlugin* p = live();
            if (closed_ || !p || AppState::get().fxEditOpen(slotId)) return;
            editName_ = what;
            AppState::get().beginFxEdit(slotId);
        };
        page_ = makeBuiltinPage(body_, builtinPageUid(plugin_->desc), host);
        if (!page_) {
            frame_->deleteLater();
            frame_ = nullptr;
            bypassBtn_ = nullptr;
            nameLabel_ = nullptr;
            return false;
        }
        bodyLayout_->addWidget(page_);
        frame_->setFixedSize(page_->width() + 2, page_->height() + 2 + kTitleH);
        top_->addStretch(1);
        return true;
    }

    void buildSliders()
    {
        makeFrame(false);
        QVBoxLayout* top_ = bodyLayout_;
        top_->setContentsMargins(8, 8, 8, 8);
        top_->setSpacing(2);
        count_ = plugin_->paramCount();
        const int shown = std::min(count_, kMaxAudioRows);
        const QFontMetrics fm(font());
        int widest = 0;
        std::vector<QString> names;
        for (int k = 0; k < shown; ++k) {
            names.push_back(qs(plugin_->paramName(k)).trimmed());
            if (names.back().isEmpty()) names.back() = QString("Setting %1").arg(k + 1);
            widest = std::max(widest, fm.horizontalAdvance(names.back()));
        }
        const int labelW = std::min(std::max(widest + 10, 70), 190);
        if (shown == 0) {
            QLabel* none = new QLabel("This effect has no settings to change.", this);
            none->setObjectName("fxMessage");
            none->setAlignment(Qt::AlignHCenter);
            top_->addSpacing(20);
            top_->addWidget(none);
        }
        rows_.reserve((size_t)shown);
        for (int k = 0; k < shown; ++k) {
            ARow r;
            r.row = new QWidget(this);
            r.row->setObjectName("fxAudioRow");
            r.row->setProperty("index", k);
            QHBoxLayout* h = new QHBoxLayout(r.row);
            h->setContentsMargins(0, 3, 0, 3);
            h->setSpacing(6);
            r.label = new ElideLabel(r.row);
            r.label->setText(names[(size_t)k]);
            r.label->setFixedWidth(labelW);
            h->addWidget(r.label);
            r.slider = new FxSlider(r.row);
            r.slider->setObjectName("fxAudioSlider");
            r.slider->setRange(0, kSliderSteps);
            r.slider->setPageStep(kSliderSteps / 10);
            h->addWidget(r.slider, 1);
            r.text = new ElideLabel(r.row);
            r.text->setObjectName("fxAudioText");
            r.text->setFixedWidth(96);
            h->addWidget(r.text);
            const size_t ri = rows_.size();
            QObject::connect(r.slider, &QSlider::valueChanged, r.row, [this, ri](int pos) { sliderMoved(ri, pos); });
            QObject::connect(r.slider, &QSlider::sliderPressed, r.row, [this, ri] {
                if (closed_ || AppState::get().fxEditOpen(slotId)) return;
                editName_ = nameFor(ri);
                AppState::get().beginFxEdit(slotId);
            });
            QObject::connect(r.slider, &QSlider::sliderReleased, r.row, [this] {
                if (AppState::get().fxEditOpen(slotId)) AppState::get().endFxEdit(slotId, editName_);
            });
            rows_.push_back(r);
            top_->addWidget(r.row);
        }
        if (count_ > shown) {
            QLabel* more = new QLabel(QString("Showing the first %1 of this effect's %2 settings.").arg(shown).arg(count_), this);
            more->setObjectName("fxAudioMore");
            top_->addSpacing(6);
            top_->addWidget(more);
        }
        top_->addStretch(1);
        for (size_t k = 0; k < rows_.size(); ++k) showRow(plugin_, k);
    }

    void showRow(AudioPlugin* plugin, size_t k)
    {
        ARow& r = rows_[k];
        updating_ = true;
        if (!r.slider->isSliderDown()) {
            const float v = plugin->paramGet((int)k);
            const int pos = (int)std::floor(std::min(1.f, std::max(0.f, v)) * kSliderSteps + 0.5f);
            if (r.slider->value() != pos) r.slider->setValue(pos);
        }
        const QString t = qs(plugin->paramText((int)k)).trimmed();
        r.text->setText(t);
        if (r.text->toolTip() != t) r.text->setToolTip(t);
        updating_ = false;
    }

    std::string nameFor(size_t ri) const { return "Change \"" + s8(rows_[ri].label->text()) + "\""; }

    void sliderMoved(size_t ri, int pos)
    {
        if (updating_ || closed_) return;
        AudioPlugin* plugin = live();
        if (!plugin) return;
        // inside a drag this is part of its step; a click on the groove or a key press is a step of its own
        AppState& app = AppState::get();
        if (app.fxEditOpen(slotId)) plugin->paramSet((int)ri, (float)pos / (float)kSliderSteps);
        else app.fxEditStep(slotId, nameFor(ri), [&] { plugin->paramSet((int)ri, (float)pos / (float)kSliderSteps); });
        const QString t = qs(plugin->paramText((int)ri)).trimmed();
        rows_[ri].text->setText(t);
        rows_[ri].text->setToolTip(t);
    }

    struct ARow {
        QWidget* row = nullptr;
        ElideLabel* label = nullptr;
        FxSlider* slider = nullptr;
        ElideLabel* text = nullptr;
    };
    class TitleBar : public QWidget {
    public:
        explicit TitleBar(QWidget* parent) : QWidget(parent) {}

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter p(this);
            QLinearGradient g(0, 0, 0, height());
            g.setColorAt(0, QColor(160, 160, 160));
            g.setColorAt(1, QColor(135, 135, 135));
            p.fillRect(rect(), g);
        }
    };
    static constexpr int kTitleH = 20;
    static constexpr int kFrameLeft = 10, kFrameTop = 6, kFrameRight = 10, kFrameBottom = 10;
    AudioPlugin* plugin_;
    std::string editName_;
    bool wantEditor_ = false, wantPage_ = false, started_ = false;
    QVBoxLayout* top_ = nullptr;
    QFrame* frame_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
    QToolButton* bypassBtn_ = nullptr;
    QLabel* nameLabel_ = nullptr;
    QLabel* stoppedNote_ = nullptr;
    bool shownStopped_ = false;
    DxPage* page_ = nullptr;
    NativeHost* host_ = nullptr;
    bool editorOpen_ = false, updating_ = false;
    QSize editorSize_;
    int count_ = 0;
    std::vector<ARow> rows_;
};

}

// ------------------------------------------------------------------------------------------ the window
// .......................................................................................... the keyframe strip
// VEGAS's keyframe controller under an OpenFX effect's controls (the Animate button): a small timeline as long as what
// the effect is on (an event's effects: the event; a transition: the overlap; a track's or the output's: the project),
// with a row of keyframes for the whole effect, one per group of settings and one per setting that can change over time.
// A click puts the cursor there; a keyframe is picked with a click and dragged to another time (on a group's or the
// effect's row, all keyframes at that time move together); a double-click on a row adds a keyframe (the value the
// setting has there); the right-click menu sets how the value goes on to the next keyframe (Linear, Fast, Slow, Smooth,
// Sharp, Hold) or deletes it; Delete deletes the picked one. The buttons under it go to the first / previous / next /
// last keyframe and add or delete one at the cursor. Each change is one undo step. Times are the effect's frames (from
// the event's start for event effects).
class KeyStrip : public QWidget {
public:
    std::function<ofx::Instance*()> live;
    std::function<double()> now;
    std::function<void(double)> seek;
    std::function<double()> span;
    std::function<double()> rate;
    uint64_t slotId = 0;

    struct Row {
        QString label;
        int depth = 0;
        std::vector<ofx::Param*> params;
        std::map<double, int> keys;         // time -> curve (-1: settings with different curves there)
    };

    explicit KeyStrip(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxKeyStrip");
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        setMinimumHeight(kRuler + 3 * kRowH + 2);
    }

    const std::vector<Row>& rows() const { return rows_; }
    int selectedRow() const { return selRow_; }
    double selectedTime() const { return selTime_; }
    void select(int row, double time) { selRow_ = row; selTime_ = time; update(); }

    void refresh()
    {
        ofx::Instance* inst = live ? live() : nullptr;
        if (!inst) {
            rows_.clear();
            update();
            return;
        }
        std::unique_lock<std::recursive_mutex> lk(inst->mutex, std::try_to_lock);
        if (!lk.owns_lock()) {
            QTimer::singleShot(40, this, [this] { refresh(); });
            return;
        }
        std::vector<Row> rows;
        Row all;
        all.label = "All settings";
        std::vector<std::string> groupNames;
        std::map<std::string, Row> groups;
        std::vector<Row> loose;
        std::map<std::string, std::vector<Row>> inGroup;
        std::map<std::string, std::string> groupLabel;
        for (const auto& up : inst->params.params) {
            ofx::Param* p = up.get();
            if (p->kind == ofx::ParamKind::Group) groupLabel[p->name] = p->label();
            if (p->dim <= 0 || p->secret() || p->kind == ofx::ParamKind::PushButton) continue;
            if (!p->props.getInt(kOfxParamPropAnimates)) continue;
            if (p->name == kOfxImageEffectTransitionParamName && inst->context() == kOfxImageEffectContextTransition) continue;
            Row r;
            r.label = qs(p->label());
            r.depth = 2;
            r.params.push_back(p);
            for (const auto& k : p->keys) r.keys[k.first] = p->keyCurve(k.first);
            all.params.push_back(p);
            const std::string parent = p->parent();
            if (parent.empty()) loose.push_back(r);
            else {
                if (!inGroup.count(parent)) groupNames.push_back(parent);
                inGroup[parent].push_back(r);
            }
        }
        auto merge = [](Row& into, const Row& from) {
            for (const auto& k : from.keys) {
                auto it = into.keys.find(k.first);
                if (it == into.keys.end()) into.keys[k.first] = k.second;
                else if (it->second != k.second) it->second = -1;
            }
        };
        for (const Row& r : loose) merge(all, r);
        for (const auto& g : inGroup) for (const Row& r : g.second) merge(all, r);
        rows.push_back(all);
        for (Row r : loose) {
            r.depth = 1;
            rows.push_back(r);
        }
        for (const std::string& g : groupNames) {
            Row gr;
            gr.label = qs(groupLabel.count(g) && !groupLabel[g].empty() ? groupLabel[g] : g);
            gr.depth = 1;
            for (const Row& r : inGroup[g]) {
                merge(gr, r);
                gr.params.push_back(r.params[0]);
            }
            rows.push_back(gr);
            for (const Row& r : inGroup[g]) rows.push_back(r);
        }
        if (all.params.empty()) rows.clear();
        lk.unlock();
        rows_ = rows;
        if (selRow_ >= (int)rows_.size() || (selRow_ >= 0 && !rows_[(size_t)selRow_].keys.count(selTime_))) selRow_ = -1;
        setMinimumHeight(kRuler + (int)std::max<size_t>(rows_.size(), 1) * kRowH + 2);
        updateGeometry();
        update();
    }

    std::vector<double> allTimes() const
    {
        std::vector<double> v;
        if (!rows_.empty()) for (const auto& k : rows_[0].keys) v.push_back(k.first);
        return v;
    }

    void moveKeys(int row, double time, double to)
    {
        ofx::Instance* inst = live ? live() : nullptr;
        if (!inst || row < 0 || row >= (int)rows_.size()) return;
        for (ofx::Param* p : rows_[(size_t)row].params) inst->userMoveKey(p, time, to);
    }

    void setCurve(int row, double time, int curve)
    {
        ofx::Instance* inst = live ? live() : nullptr;
        if (!inst || row < 0 || row >= (int)rows_.size()) return;
        const std::vector<ofx::Param*> ps = rows_[(size_t)row].params;
        AppState::get().fxEditStep(slotId, "Keyframe Interpolation", [&] { for (ofx::Param* p : ps) inst->userSetKeyCurve(p, time, curve); });
        refresh();
    }

    void deleteKeys(int row, double time)
    {
        ofx::Instance* inst = live ? live() : nullptr;
        if (!inst || row < 0 || row >= (int)rows_.size()) return;
        const std::vector<ofx::Param*> ps = rows_[(size_t)row].params;
        AppState::get().fxEditStep(slotId, "Delete Keyframe", [&] { for (ofx::Param* p : ps) inst->userDeleteKey(p, time); });
        if (selRow_ == row && selTime_ == time) selRow_ = -1;
        refresh();
    }

    void addKeys(int row, double time)
    {
        ofx::Instance* inst = live ? live() : nullptr;
        if (!inst || row < 0 || row >= (int)rows_.size()) return;
        const std::vector<ofx::Param*> ps = rows_[(size_t)row].params;
        AppState::get().fxEditStep(slotId, "Add Keyframe", [&] {
            for (ofx::Param* p : ps) {
                std::vector<double> v;
                {
                    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                    v = p->valueAt(time);
                }
                inst->userSetValue(p, v, time, true);
            }
        });
        selRow_ = row;
        selTime_ = time;
        refresh();
    }

    double xOf(double frames) const
    {
        const double len = std::max(1.0, span ? span() : 1.0);
        return kLabelW + 6 + (width() - kLabelW - 12) * (frames / len);
    }
    double timeAt(double x) const
    {
        const double len = std::max(1.0, span ? span() : 1.0);
        double t = (x - kLabelW - 6) / std::max(1.0, (double)(width() - kLabelW - 12)) * len;
        t = std::floor(t + 0.5);
        return std::min(len, std::max(0.0, t));
    }
    int rowAt(double y) const
    {
        if (y < kRuler) return -2;
        const int r = (int)((y - kRuler) / kRowH);
        return r < (int)rows_.size() ? r : -1;
    }
    QPoint keyPoint(int row, double frames) const { return QPoint((int)std::lround(xOf(frames)), kRuler + row * kRowH + kRowH / 2); }

    static constexpr int kRuler = 18, kRowH = 18, kLabelW = 150;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Base));
        const QColor text = pal.color(QPalette::Text), dim = pal.color(QPalette::Disabled, QPalette::Text);
        const double len = std::max(1.0, span ? span() : 1.0), fps = rate ? std::max(1.0, rate()) : 30.0;
        p.fillRect(QRect(0, 0, width(), kRuler), pal.color(QPalette::Window).darker(115));
        p.setPen(dim);
        QFont f = font();
        f.setPointSizeF(std::max(6.5, f.pointSizeF() - 1.5));
        p.setFont(f);
        const double pxPerSec = (width() - kLabelW - 12) / (len / fps);
        double step = 1.0;
        while (pxPerSec * step < 46 && step < 3600) step *= step < 10 ? 2.0 : 3.0;
        for (double sec = 0; sec * fps <= len + 1e-9; sec += step) {
            const int x = (int)std::lround(xOf(sec * fps));
            p.drawLine(x, kRuler - 5, x, kRuler - 1);
            p.drawText(QRect(x + 2, 0, 60, kRuler - 2), Qt::AlignLeft | Qt::AlignVCenter, QString("%1:%2").arg((int)sec / 60).arg((int)sec % 60, 2, 10, QChar('0')));
        }
        for (size_t r = 0; r < rows_.size(); ++r) {
            const Row& row = rows_[r];
            const int y = kRuler + (int)r * kRowH;
            if (r % 2) p.fillRect(QRect(0, y, width(), kRowH), pal.color(QPalette::AlternateBase));
            p.setPen(row.depth == 2 ? dim : text);
            QFont rf = font();
            rf.setBold(row.depth < 2);
            p.setFont(rf);
            p.drawText(QRect(6 + row.depth * 10, y, kLabelW - 10 - row.depth * 10, kRowH), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(rf).elidedText(row.label, Qt::ElideRight, kLabelW - 10 - row.depth * 10));
            p.setPen(QPen(pal.color(QPalette::Mid), 1));
            p.drawLine(QPointF(xOf(0), y + kRowH / 2.0), QPointF(xOf(len), y + kRowH / 2.0));
            for (const auto& k : row.keys) {
                const QPoint c = keyPoint((int)r, k.first);
                const bool sel = (int)r == selRow_ && k.first == selTime_;
                QPolygonF d;
                const double s = row.depth == 2 ? 4.5 : 5.5;
                d << QPointF(c.x(), c.y() - s) << QPointF(c.x() + s, c.y()) << QPointF(c.x(), c.y() + s) << QPointF(c.x() - s, c.y());
                p.setPen(QPen(sel ? QColor(255, 170, 40) : QColor(20, 20, 20), sel ? 2.0 : 1.0));
                p.setBrush(curveColour(k.second));
                p.drawPolygon(d);
            }
        }
        if (rows_.empty()) {
            p.setPen(dim);
            p.drawText(rect().adjusted(8, kRuler, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, "This effect has no settings that can change over time.");
        }
        p.setPen(pal.color(QPalette::Mid));
        p.drawLine(kLabelW, 0, kLabelW, height());
        if (now) {
            const double t = now();
            if (t >= 0 && t <= len + 1e-9) {
                const int x = (int)std::lround(xOf(t));
                p.setPen(QPen(QColor(230, 230, 230), 1));
                p.drawLine(x, 0, x, height());
            }
        }
    }

    static QColor curveColour(int curve)
    {
        switch (curve) {
        case ofx::Param::KeyFast: return QColor(110, 200, 110);
        case ofx::Param::KeySlow: return QColor(110, 160, 230);
        case ofx::Param::KeySmooth: return QColor(230, 210, 90);
        case ofx::Param::KeySharp: return QColor(200, 120, 220);
        case ofx::Param::KeyHold: return QColor(225, 90, 80);
        case -1: return QColor(150, 150, 150);
        default: return QColor(235, 235, 235);
        }
    }

    double keyUnder(int row, double x) const
    {
        if (row < 0 || row >= (int)rows_.size()) return -1;
        double best = -1, bestD = 7.0;
        for (const auto& k : rows_[(size_t)row].keys) {
            const double d = std::fabs(xOf(k.first) - x);
            if (d <= bestD) { bestD = d; best = k.first; }
        }
        return best;
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const QPointF pos = e->position();
        if (pos.x() < kLabelW) {
            const int r = rowAt(pos.y());
            if (r >= 0) { selRow_ = r; selTime_ = -1; update(); }
            return;
        }
        const int r = rowAt(pos.y());
        const double k = keyUnder(r, pos.x());
        if (e->button() == Qt::RightButton) {
            if (k >= 0) {
                selRow_ = r;
                selTime_ = k;
                update();
                keyMenu(r, k, e->globalPosition().toPoint());
            }
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        if (k >= 0) {
            selRow_ = r;
            selTime_ = k;
            dragging_ = true;
            dragFrom_ = dragNow_ = k;
            AppState::get().beginFxEdit(slotId);
            if (seek) seek(k);
        } else {
            if (r >= 0) selRow_ = r;
            selTime_ = -1;
            if (seek) seek(timeAt(pos.x()));
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!dragging_) {
            const int r = rowAt(e->position().y());
            setCursor(keyUnder(r, e->position().x()) >= 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
            return;
        }
        const double t = timeAt(e->position().x());
        if (t == dragNow_) return;
        // not onto another keyframe of the same row: that one would be lost
        if (selRow_ >= 0 && selRow_ < (int)rows_.size() && rows_[(size_t)selRow_].keys.count(t)) return;
        moveKeys(selRow_, dragNow_, t);
        dragNow_ = t;
        selTime_ = t;
        refresh();
        selTime_ = t;
        if (seek) seek(t);
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!dragging_) return;
        dragging_ = false;
        AppState::get().endFxEdit(slotId, "Move Keyframe");
        refresh();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        const int r = rowAt(e->position().y());
        if (r < 0 || e->position().x() < kLabelW) return;
        if (keyUnder(r, e->position().x()) >= 0) return;
        addKeys(r, timeAt(e->position().x()));
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && selRow_ >= 0 && selTime_ >= 0) {
            deleteKeys(selRow_, selTime_);
            return;
        }
        QWidget::keyPressEvent(e);
    }

    void keyMenu(int row, double time, const QPoint& at)
    {
        QMenu m(this);
        const int now = rows_[(size_t)row].keys.count(time) ? rows_[(size_t)row].keys.at(time) : 0;
        const struct { const char* text; int curve; } kinds[] = {{"&Linear", ofx::Param::KeyLinear}, {"&Fast", ofx::Param::KeyFast},
                                                                 {"&Slow", ofx::Param::KeySlow}, {"S&mooth", ofx::Param::KeySmooth},
                                                                 {"S&harp", ofx::Param::KeySharp}, {"H&old", ofx::Param::KeyHold}};
        for (const auto& k : kinds) {
            QAction* a = m.addAction(k.text);
            a->setCheckable(true);
            a->setChecked(now == k.curve);
            const int c = k.curve;
            QObject::connect(a, &QAction::triggered, this, [this, row, time, c] { setCurve(row, time, c); });
        }
        m.addSeparator();
        QObject::connect(m.addAction("&Delete"), &QAction::triggered, this, [this, row, time] { deleteKeys(row, time); });
        if (onMenu) onMenu(&m);
        else m.exec(at);
    }

public:
    std::function<void(QMenu*)> onMenu;

private:
    std::vector<Row> rows_;
    int selRow_ = -1;
    double selTime_ = -1;
    bool dragging_ = false;
    double dragFrom_ = 0, dragNow_ = 0;
};

class KeyPanel : public QWidget {
public:
    KeyStrip* strip = nullptr;
    QToolButton *first = nullptr, *prev = nullptr, *add = nullptr, *del = nullptr, *next = nullptr, *last = nullptr;
    QLabel* where = nullptr;

    explicit KeyPanel(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxKeyPanel");
        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);
        strip = new KeyStrip(this);
        QScrollArea* sa = new QScrollArea(this);
        sa->setObjectName("fxKeyScroll");
        sa->setWidgetResizable(true);
        sa->setWidget(strip);
        sa->setFrameShape(QFrame::StyledPanel);
        sa->setMinimumHeight(KeyStrip::kRuler + 4 * KeyStrip::kRowH + 4);
        v->addWidget(sa, 1);
        QHBoxLayout* h = new QHBoxLayout();
        h->setSpacing(1);
        const QPalette pal = palette();
        first = smallButton(this, glyphIcon(Glyph::First, pal), "First keyframe", "fxKeyFirst");
        prev = smallButton(this, glyphIcon(Glyph::Left, pal), "Previous keyframe", "fxKeyPrev");
        add = smallButton(this, glyphIcon(Glyph::Add, pal), "Create keyframe at the cursor (for the picked row's settings)", "fxKeyAdd");
        del = smallButton(this, glyphIcon(Glyph::Remove, pal), "Delete the picked keyframe", "fxKeyDelete");
        next = smallButton(this, glyphIcon(Glyph::Right, pal), "Next keyframe", "fxKeyNext");
        last = smallButton(this, glyphIcon(Glyph::Last, pal), "Last keyframe", "fxKeyLast");
        for (QToolButton* b : {first, prev, add, del, next, last}) h->addWidget(b);
        where = new QLabel(this);
        where->setObjectName("fxKeyTime");
        h->addSpacing(8);
        h->addWidget(where, 1);
        v->addLayout(h);
        QObject::connect(first, &QToolButton::clicked, this, [this] { go(-2); });
        QObject::connect(prev, &QToolButton::clicked, this, [this] { go(-1); });
        QObject::connect(next, &QToolButton::clicked, this, [this] { go(1); });
        QObject::connect(last, &QToolButton::clicked, this, [this] { go(2); });
        QObject::connect(add, &QToolButton::clicked, this, [this] {
            if (!strip->now || strip->rows().empty()) return;
            strip->addKeys(std::max(0, strip->selectedRow()), std::floor(strip->now() + 0.5));
        });
        QObject::connect(del, &QToolButton::clicked, this, [this] {
            int r = strip->selectedRow();
            double t = strip->selectedTime();
            if (t < 0 && strip->now) {
                r = std::max(0, r);
                t = std::floor(strip->now() + 0.5);
            }
            if (r >= 0 && r < (int)strip->rows().size() && strip->rows()[(size_t)r].keys.count(t)) strip->deleteKeys(r, t);
        });
    }

    void go(int how)
    {
        const std::vector<double> ts = strip->allTimes();
        if (ts.empty() || !strip->now || !strip->seek) return;
        const double t = strip->now();
        double to = -1;
        if (how == -2) to = ts.front();
        else if (how == 2) to = ts.back();
        else if (how == -1) { for (double x : ts) if (x < t - 1e-6) to = x; }
        else { for (double x : ts) if (x > t + 1e-6) { to = x; break; } }
        if (to >= 0) {
            strip->select(0, to);
            strip->seek(to);
        }
    }

    void refresh()
    {
        strip->refresh();
        if (strip->now && strip->rate) {
            const double f = strip->now(), fps = std::max(1.0, strip->rate());
            const double sec = f / fps;
            where->setText(QString("%1:%2.%3  (frame %4)").arg((int)sec / 60).arg((int)sec % 60, 2, 10, QChar('0'))
                               .arg((int)std::lround((sec - std::floor(sec)) * 1000), 3, 10, QChar('0')).arg((long long)std::lround(f)));
        }
    }
};

struct FxWindow::Impl {
    FxWindow* q = nullptr;
    Playback* playback = nullptr;
    AppState& app = AppState::get();
    int listener = 0;

    FxTarget target;
    bool targetLost = false;
    uint64_t picked = 0;
    std::vector<uint64_t> order;

    ChainBar* chainBar = nullptr;
    TargetChip* chip = nullptr;
    QLabel* targetName = nullptr;
    QToolButton* presetHelp = nullptr;
    std::map<uint64_t, ChainButton*> buttons;
    QToolButton *modeBtn = nullptr, *addBtn = nullptr, *leftBtn = nullptr, *rightBtn = nullptr, *removeBtn = nullptr;
    QToolButton* chainBtn = nullptr;
    QWidget* presetRow = nullptr;
    QComboBox* presetBox = nullptr;
    QToolButton *presetSave = nullptr, *presetDelete = nullptr;
    std::vector<OfxPreset> presets;
    uint64_t presetsSlot = 0;
    // an audio plug-in's Preset list: its own programs, then the user's presets (presetBox's data: the index here)
    struct AudioItem { int program = -1; AudioPreset preset; };
    std::vector<AudioItem> audioItems;
    bool presetsAudio = false;
    QToolButton* presetFile = nullptr;
    QToolButton* bypassAuto = nullptr;
    QToolButton* animateBtn = nullptr;
    QToolButton* adjustLenBtn = nullptr;
    QToolButton* progressBtn = nullptr;
    KeyPanel* keys = nullptr;
    mutable FxChain single;
    QMenu* addMenu = nullptr;
    std::vector<VideoFxEntry> menuVideo;
    std::vector<AudioFxEntry> menuAudio;
    QLabel* banner = nullptr;
    QScrollArea* scroll = nullptr;
    QWidget* holder = nullptr;
    QVBoxLayout* holderLayout = nullptr;
    Panel* panel = nullptr;
    QTimer* slowTimer = nullptr;
    QTimer* idleTimer = nullptr;
    bool rebuildQueued = false;

    bool preferSliders = false;
    std::set<uint64_t> editorFailed;
    QSize sizeBeforeEditor;
    GroupMemory groupMemory;

    // Apply Non-Real-Time Event FX's Take FX window: only these effects of the event's chain are shown
    std::vector<uint64_t> onlySlots;
    mutable FxChain onlyChain;

    const FxChain* chain() const
    {
        if (target.kind == FxTarget::None) return nullptr;
        if (!onlySlots.empty() && target.kind == FxTarget::Event) {
            const FxChain* all = app.chainOf(app.project(), target);
            if (!all) return nullptr;
            onlyChain.clear();
            for (const FxSlot& s : *all)
                if (std::find(onlySlots.begin(), onlySlots.end(), s.id) != onlySlots.end()) onlyChain.push_back(s);
            return &onlyChain;
        }
        if (singleTarget()) {
            // a transition or a generator: its one effect (none: a transition that is a plain cross-fade)
            const Project& p = app.project();
            const FxSlot* s = nullptr;
            if (target.kind == FxTarget::Transition) {
                const Event* e = p.findEvent(target.id);
                if (!e) return nullptr;
                s = &e->transition;
            } else {
                const MediaItem* m = p.findMedia(target.id);
                if (!m || !m->generated) return nullptr;
                s = &m->generator;
            }
            single.clear();
            if (!s->kind.empty()) single.push_back(*s);
            return &single;
        }
        return app.chainOf(app.project(), target);
    }
    bool singleTarget() const { return target.kind == FxTarget::Transition || target.kind == FxTarget::Generator; }
    bool isVideo() const { return singleTarget() || app.targetIsVideo(target); }

    const FxSlot* slot(uint64_t id) const
    {
        const FxChain* c = chain();
        if (!c || !id) return nullptr;
        for (const FxSlot& s : *c)
            if (s.id == id) return &s;
        return nullptr;
    }

    int indexOf(uint64_t id) const
    {
        const FxChain* c = chain();
        if (!c) return -1;
        for (size_t k = 0; k < c->size(); ++k)
            if ((*c)[k].id == id) return (int)k;
        return -1;
    }

    // The time effects are told, in frames, as the engine works it out when it draws the image: from the start of the
    // event for an event's effects, from the start of the project otherwise.
    double fxTime() const
    {
        const Project& p = app.project();
        const double fps = p.settings.fps();
        double t = p.frameTime(p.frameAt(app.cursor()));
        if (target.kind == FxTarget::Event)
            if (const Event* e = p.findEvent(target.id)) t -= e->start;
        if (t < 0) t = 0;
        const double f = t * fps, r = std::floor(f + 0.5);
        return std::fabs(f - r) < 1e-4 ? r : f;
    }

    void seekFx(double frames)
    {
        const Project& p = app.project();
        double t = frames / p.settings.fps();
        if (target.kind == FxTarget::Event)
            if (const Event* e = p.findEvent(target.id)) t += e->start;
        app.setCursor(t);
    }

    QString title() const
    {
        if (target.kind == FxTarget::None) return "Effects";
        if (target.kind == FxTarget::Transition) {
            const FxChain* c = chain();
            return c && !c->empty() ? QString("Transition: %1").arg(qs(fxSlotLabel(c->front()))) : QString("Transition");
        }
        if (target.kind == FxTarget::Generator) {
            const MediaItem* m = app.project().findMedia(target.id);
            return QString("Video Media Generator: %1").arg(m ? qs(m->name) : QString());
        }
        const bool video = app.targetIsVideo(target);
        const QString name = qs(app.targetName(target));
        const int colon = (int)name.indexOf(": ");
        switch (target.kind) {
        case FxTarget::Event: return QString(video ? "Video Event FX: %1" : "Audio Event FX: %1").arg(colon >= 0 ? name.mid(colon + 2) : name);
        case FxTarget::Track: return QString(video ? "Video Track FX: %1" : "Audio Track FX: %1").arg(colon >= 0 ? name.mid(colon + 2) : name);
        default: return name + " FX";
        }
    }

    void updateHeader()
    {
        const Project& p = app.project();
        QString name;
        QColor colour;
        int number = 0;
        auto trackOf = [&](uint64_t id) {
            for (size_t k = 0; k < p.tracks.size(); ++k)
                if (p.tracks[k].id == id) { number = (int)k + 1; colour = chipColor(p.tracks[k], (int)k); return &p.tracks[k]; }
            return (const Track*)nullptr;
        };
        if (target.kind == FxTarget::Event) {
            if (const Event* e = p.findEvent(target.id)) {
                for (const Track& t : p.tracks) {
                    bool in = false;
                    for (const Event& ev : t.events) if (ev.id == e->id) { in = true; break; }
                    if (in) { trackOf(t.id); break; }
                }
                name = qs(e->name);
                if (name.isEmpty())
                    if (const MediaItem* m = p.findMedia(e->mediaId)) name = qs(m->name);
            }
        } else if (target.kind == FxTarget::Track) {
            if (const Track* t = trackOf(target.id)) name = t->name.empty() ? QString("Track %1").arg(number) : qs(t->name);
        } else if (target.kind != FxTarget::None) {
            name = singleTarget() ? title() : qs(app.targetName(target));
        }
        chip->setChip(colour, number);
        if (chip->isHidden() == (number > 0)) chip->setVisible(number > 0);
        if (targetName->text() != name) targetName->setText(name);
    }

    void syncChain()
    {
        const FxChain* c = chain();
        if (target.kind != FxTarget::None && !c) {
            target = FxTarget();
            targetLost = true;
            c = nullptr;
        }
        q->setWindowTitle(title());
        updateHeader();

        if (c && picked && indexOf(picked) < 0) {
            size_t was = 0;
            for (size_t k = 0; k < order.size(); ++k)
                if (order[k] == picked) was = k;
            picked = c->empty() ? 0 : (*c)[std::min(was, c->size() - 1)].id;
        }
        if (!c) picked = 0;

        if (!groupMemory.empty() || !editorFailed.empty()) {
            std::set<uint64_t> ids;
            app.project().forEachFx([&](const FxSlot& s) { ids.insert(s.id); });
            for (auto it = groupMemory.begin(); it != groupMemory.end();) {
                if (!ids.count(it->first.first)) it = groupMemory.erase(it);
                else ++it;
            }
            for (auto it = editorFailed.begin(); it != editorFailed.end();) {
                if (!ids.count(*it)) it = editorFailed.erase(it);
                else ++it;
            }
        }

        std::vector<uint64_t> now;
        if (c)
            for (const FxSlot& s : *c) now.push_back(s.id);
        // most changes to the project (every step of a drag in the timeline) leave the chain as it was
        if (now == order && buttons.size() == now.size()) {
            updateStatus();
            return;
        }
        order = now;

        while (QLayoutItem* it = chainBar->flow->takeAt(0)) delete it;
        std::map<uint64_t, ChainButton*> keep;
        if (c) {
            for (const FxSlot& s : *c) {
                ChainButton* b = nullptr;
                auto it = buttons.find(s.id);
                if (it != buttons.end()) {
                    b = it->second;
                    buttons.erase(it);
                } else {
                    b = new ChainButton(chainBar);
                    b->slotId = s.id;
                    b->setProperty("slotId", (qulonglong)s.id);
                    const uint64_t id = s.id;
                    b->onPick = [this, id] { pick(id); };
                    b->onDrag = [this, id](const QPoint& at, bool dropped) { dragTo(id, at, dropped); };
                    QObject::connect(b->tick, &QCheckBox::toggled, q, [this, id](bool on) { app.setFxBypass(target, id, !on); });
                    b->onMenu = [this, id](const QPoint& at) { chainMenu(id, at); };
                    b->show();
                }
                keep[s.id] = b;
                chainBar->flow->addWidget(b);
            }
        }
        for (auto& kv : buttons) {
            kv.second->hide();
            kv.second->deleteLater();
        }
        buttons.swap(keep);
        chainBar->flow->invalidate();
        chainBar->updateGeometry();
        updateStatus();

    }

    void updateStatus()
    {
        const FxChain* c = chain();
        QString pickedError;
        bool pickedLive = false;
        if (c) {
            for (const FxSlot& s : *c) {
                auto it = buttons.find(s.id);
                if (it == buttons.end()) continue;
                const QString err = qs(app.fx.error(s.id));
                QString name = qs(fxSlotLabel(s));
                if (name.isEmpty()) name = "Effect";
                it->second->setState(name, !s.bypass, s.id == picked, err);
                if (it->second->tick->isHidden() != singleTarget()) it->second->tick->setVisible(!singleTarget());
                if (target.kind == FxTarget::Track) it->second->setAfterComposite(s.afterComposite && s.kind == "ofx");
                else it->second->setBeforePanCrop(target.kind == FxTarget::Event && s.beforePanCrop && s.kind == "ofx");
                if (s.id == picked) {
                    pickedError = err;
                    pickedLive = s.kind == "builtin" ? true : (s.kind == "ofx" ? app.fx.video(s.id) != nullptr : app.fx.audio(s.id) != nullptr);
                }
            }
        }
        const int at = indexOf(picked);
        const int n = c ? (int)c->size() : 0;
        const bool many = !singleTarget();
        addBtn->setEnabled(c != nullptr && many);
        removeBtn->setEnabled(at >= 0 && (many || target.kind == FxTarget::Transition));
        leftBtn->setEnabled(at > 0 && many);
        rightBtn->setEnabled(at >= 0 && at < n - 1 && many);
        for (QToolButton* b : {addBtn, leftBtn, rightBtn})
            if (b->isHidden() == many) b->setVisible(many);
        chainBtn->setEnabled(c != nullptr);
        if (adjustLenBtn) {
            bool slow = false;
            if (c && target.kind == FxTarget::Event)
                for (const FxSlot& s : *c)
                    if (s.id == picked && s.kind == "ofx" && (s.pluginId == kSlowMotionId || s.pluginId == kTimeWarpId)) slow = true;
            if (adjustLenBtn->isHidden() == slow) adjustLenBtn->setVisible(slow);
        }
        if (progressBtn) {
            const bool tr = target.kind == FxTarget::Transition;
            if (progressBtn->isHidden() == tr) progressBtn->setVisible(tr);
            if (tr) {
                EnvelopeRef r;
                r.owner.kind = EnvelopeOwner::Event;
                r.owner.id = target.id;
                r.kind = "transition";
                const bool has = AppState::findEnvelope(app.project(), r) != nullptr;
                if (progressBtn->isChecked() != has) progressBtn->setChecked(has);
            }
        }
        const QString chainTip = many ? QString("Plug-In Chain...") : QString("Replace Plug-In...");
        if (chainBtn->toolTip() != chainTip) chainBtn->setToolTip(chainTip);
        syncPresets();

        // a line under the chain for what the user should know: the effect is there but failed while drawing (one that
        // could not be made at all says so where its controls would be), or everything is switched off
        QString text;
        bool bad = false;
        if (pickedLive && !pickedError.isEmpty()) {
            text = "This effect had a problem and was skipped: " + pickedError;
            bad = true;
        } else if (c && app.bypassAllFx && isVideo()) {
            text = "All video effects are switched off for now (Tools > Bypass All Video FX).";
        }
        if (banner->text() != text) banner->setText(text);
        if (banner->property("bad").toBool() != bad || banner->styleSheet().isEmpty()) {
            banner->setProperty("bad", bad);
            banner->setStyleSheet(bad ? "QLabel { background: #5c2f2f; color: #ffdcdc; border: 1px solid #8f4a4a; border-radius: 3px; padding: 4px 8px; }"
                                      : "QLabel { background: #3d3d33; color: #e6e2c8; border: 1px solid #66664d; border-radius: 3px; padding: 4px 8px; }");
        }
        if (banner->isHidden() != text.isEmpty()) banner->setVisible(!text.isEmpty());
    }

    // Right-click on an OpenFX effect: its About and Help (VEGAS's effects show them by asking the host back, see
    // installEffectRequests), and for an effect of a video event whether it is applied before Event Pan/Crop. VEGAS
    // keeps those at the start of the chain, so the effect moves to the end of that run (or just after it) as it changes
    // sides.
    void chainMenu(uint64_t id, const QPoint& at)
    {
        const FxChain* c = chain();
        const int i = indexOf(id);
        if (!c || i < 0 || (*c)[(size_t)i].kind != "ofx") return;
        QMenu menu;
        QAction* pre = nullptr;
        QAction* post = nullptr;
        if (target.kind == FxTarget::Event && app.targetIsVideo(target)) {
            pre = menu.addAction("Apply Before Event Pan/Crop");
            pre->setObjectName("fxBeforePanCrop");
            pre->setCheckable(true);
            pre->setChecked((*c)[(size_t)i].beforePanCrop);
            menu.addSeparator();
        }
        if (target.kind == FxTarget::Track && app.targetIsVideo(target)) {
            post = menu.addAction("Apply After Track Compositing");
            post->setObjectName("fxAfterComposite");
            post->setCheckable(true);
            post->setChecked((*c)[(size_t)i].afterComposite);
            menu.addSeparator();
        }
        ofx::Instance* inst = app.fx.video(id);
        QAction* about = menu.addAction("About...");
        about->setObjectName("fxAbout");
        QAction* help = menu.addAction("Help");
        help->setObjectName("fxHelp");
        about->setEnabled(inst != nullptr);
        help->setEnabled(inst != nullptr);
        QAction* got = menu.exec(at);
        if (!got) return;
        if (got == pre) { setBeforePanCrop(id, pre->isChecked()); return; }
        if (got == post) { setAfterComposite(id, post->isChecked()); return; }
        inst = app.fx.video(id);
        if (!inst || (got != about && got != help)) return;
        const bool answered = got == about ? inst->invokeAbout() : inst->invokeHelp();
        if (!answered) showEffectInfo(inst->plugin ? inst->plugin->info : ofx::PluginInfo(), got == help);
    }

public:
    // What Reno shows for an effect's About or Help: what the effect says about itself.
    static void showEffectInfo(const ofx::PluginInfo& pi, bool help)
    {
        QString text = QString("<b>%1</b><br>%2 %3.%4").arg(qs(pi.label).toHtmlEscaped(), qs(pi.id).toHtmlEscaped()).arg(pi.versionMajor).arg(pi.versionMinor);
        if (!pi.grouping.empty()) text += "<br>" + qs(pi.grouping).toHtmlEscaped();
        if (!pi.description.empty()) text += "<br><br>" + qs(pi.description).toHtmlEscaped();
        if (help) text += "<br><br>Reno has no help pages for this effect.";
        if (!pi.binaryPath.empty()) text += "<br><br><small>" + qs(pi.binaryPath).toHtmlEscaped() + "</small>";
        QMessageBox box(QMessageBox::Information, help ? QString("Help: %1").arg(qs(pi.label)) : QString("About %1").arg(qs(pi.label)), text);
        box.setTextFormat(Qt::RichText);
        box.setObjectName(help ? "fxHelpBox" : "fxAboutBox");
        box.exec();
    }
    // VEGAS's own effects answer About and Help by asking the host to show them (OfxVegasEffectSuite); the request can
    // come from any thread, so it is shown later on the interface thread.
    static void installEffectRequests()
    {
        ofx::Host::get().onEffectRequest = [](ofx::EffectBase* e, const std::string& action) {
            if (!e || !e->plugin) return;
            const ofx::PluginInfo pi = e->plugin->info;
            const bool help = action == kVegasActionShowEffectHelp;
            QMetaObject::invokeMethod(qApp, [pi, help] { showEffectInfo(pi, help); }, Qt::QueuedConnection);
        };
    }

private:

public:
    void setBeforePanCrop(uint64_t id, bool on)
    {
        const uint64_t eventId = target.id;
        app.edit(on ? "Effect Before Pan/Crop" : "Effect After Pan/Crop", [&](Project& p) {
            Event* e = p.findEvent(eventId);
            if (!e) return;
            for (size_t k = 0; k < e->fx.size(); ++k) {
                if (e->fx[k].id != id) continue;
                FxSlot s = e->fx[k];
                s.beforePanCrop = on;
                e->fx.erase(e->fx.begin() + (std::ptrdiff_t)k);
                size_t before = 0;
                while (before < e->fx.size() && e->fx[before].beforePanCrop) ++before;
                e->fx.insert(e->fx.begin() + (std::ptrdiff_t)before, s);
                return;
            }
        });
    }

    // A video track's effect before or after the track is composited (Track Motion, composite level and mode). The pre
    // ones are kept first in the chain, so the effect moves to the end of that run as it changes sides.
    void setAfterComposite(uint64_t id, bool on)
    {
        const uint64_t trackId = target.id;
        app.edit(on ? "Effect After Compositing" : "Effect Before Compositing", [&](Project& p) {
            Track* t = p.findTrack(trackId);
            if (!t) return;
            for (size_t k = 0; k < t->fx.size(); ++k) {
                if (t->fx[k].id != id) continue;
                FxSlot s = t->fx[k];
                s.afterComposite = on;
                t->fx.erase(t->fx.begin() + (std::ptrdiff_t)k);
                size_t before = 0;
                while (before < t->fx.size() && !t->fx[before].afterComposite) ++before;
                t->fx.insert(t->fx.begin() + (std::ptrdiff_t)before, s);
                return;
            }
        });
    }

    void pick(uint64_t id)
    {
        if (indexOf(id) < 0) return;
        picked = id;
        updateStatus();
        rebuildPanel(false);
    }

    void dragTo(uint64_t id, const QPoint& globalPos, bool dropped)
    {
        const FxChain* c = chain();
        const int from = indexOf(id);
        if (!c || from < 0) { chainBar->clearMarker(); return; }
        const QPoint at = chainBar->mapFromGlobal(globalPos);
        int gap = (int)c->size();
        int markX = 0, markTop = 0, markBottom = 0;
        bool found = false;
        int lastOnLine = -1;
        for (size_t k = 0; k < c->size(); ++k) {
            auto it = buttons.find((*c)[k].id);
            if (it == buttons.end()) continue;
            const QRect g = it->second->geometry();
            const bool below = at.y() > g.bottom() + 2;
            if (below) continue;
            if (at.y() < g.top() - 2 && lastOnLine >= 0) break;
            lastOnLine = (int)k;
            if (at.x() < g.center().x()) {
                gap = (int)k;
                markX = g.left() - 3;
                markTop = g.top();
                markBottom = g.bottom() + 1;
                found = true;
                break;
            }
        }
        if (!found && lastOnLine >= 0) {
            gap = lastOnLine + 1;
            const QRect g = buttons[(*c)[(size_t)lastOnLine].id]->geometry();
            markX = g.right() + 4;
            markTop = g.top();
            markBottom = g.bottom() + 1;
            found = true;
        }
        const int to = gap > from ? gap - 1 : gap;
        if (!dropped) {
            if (found && to != from) chainBar->setMarker(std::max(1, markX), markTop, markBottom);
            else chainBar->clearMarker();
            return;
        }
        chainBar->clearMarker();
        if (!found || to == from) return;
        if (to - from == 1 || to - from == -1) {
            app.moveFx(target, id, to - from);
        } else {
            // further than next door: one step for Undo, not one per place moved
            const FxTarget t = target;
            app.edit("Reorder Effects", [&](Project& p) {
                FxChain* ch = app.chainOf(p, t);
                if (!ch || from >= (int)ch->size() || to >= (int)ch->size()) return;
                FxSlot s = (*ch)[(size_t)from];
                ch->erase(ch->begin() + from);
                ch->insert(ch->begin() + to, s);
            });
        }
    }

    void openChooser()
    {
        if (!chain()) return;
        if (!singleTarget()) {
            uint64_t first = 0;
            if (!choosePluginsFor(q, target, &first)) return;
            if (first) pick(first);
            else syncChain();
            return;
        }
        const bool transition = target.kind == FxTarget::Transition;
        PluginChooser dlg(transition ? ChooserKind::Transition : ChooserKind::Generator,
                          transition ? "Plug-In Chooser - Transition" : "Plug-In Chooser - Video Media Generator", FxChain(), q);
        if (dlg.exec() != QDialog::Accepted) return;
        FxSlot s;
        if (!dlg.picked(&s) || s.pluginId.empty()) return;
        replaceSingle(s);
    }

    void replaceSingle(const FxSlot& with)
    {
        if (target.kind == FxTarget::Transition) {
            app.setTransition(target.id, with.pluginId, fxSlotLabel(with));
        } else if (target.kind == FxTarget::Generator) {
            const uint64_t media = target.id;
            app.edit("Replace Media Generator", [&](Project& p) {
                for (MediaItem& m : p.media)
                    if (m.id == media && m.generated) {
                        const bool keepName = m.name == m.generator.name;
                        m.generator = with;
                        m.generator.id = p.newId();
                        m.generator.kind = "ofx";
                        m.generator.veg.reset();
                        if (keepName) m.name = with.name;
                    }
            });
        }
        picked = 0;
        order.clear();
        if (const FxChain* c = chain())
            if (!c->empty()) picked = c->front().id;
        syncChain();
        rebuildPanel(false);
    }

    void removePicked()
    {
        if (!picked) return;
        if (target.kind == FxTarget::Transition) app.setTransition(target.id, std::string(), std::string());
        else if (!singleTarget()) app.removeFx(target, picked);
    }

    ofx::Instance* pickedOfx() const
    {
        const FxSlot* s = slot(picked);
        return s && s->kind == "ofx" ? app.fx.video(s->id) : nullptr;
    }

    AudioPlugin* pickedAudio() const
    {
        const FxSlot* s = slot(picked);
        return s && s->kind == "audio" ? app.fx.audio(s->id) : nullptr;
    }

    // VEGAS's Bypass FX Automation: shown for an audio effect on a track, a bus or the master; usable while the owner
    // has automation envelopes for it; down: the envelopes are not followed (FxSlot::bypassAutomation).
    void syncBypassAutomation()
    {
        if (!bypassAuto) return;
        const FxSlot* s = slot(picked);
        const bool audio = s && s->kind == "audio" && (target.kind == FxTarget::Track || target.kind == FxTarget::Bus || target.kind == FxTarget::AudioOut);
        bypassAuto->setVisible(audio);
        if (!audio) return;
        const Project& p = app.project();
        const Envelopes* envs = nullptr;
        if (target.kind == FxTarget::AudioOut) envs = &p.masterEnvelopes;
        else if (target.kind == FxTarget::Bus) {
            if (const reno::Bus* b = p.findBus(target.id)) envs = &b->envelopes;
        } else {
            for (const Track& t : p.tracks)
                if (t.id == target.id) envs = &t.envelopes;
        }
        bool has = false;
        if (envs)
            for (const Envelope& e : *envs) has = has || (e.kind == "fx" && e.target == s->id && !e.points.empty());
        const QSignalBlocker b(bypassAuto);
        bypassAuto->setEnabled(has);
        bypassAuto->setChecked(has && s->bypassAutomation);
        bypassAuto->setToolTip(has ? "Bypass FX Automation: when down, the effect's automation envelopes are ignored"
                                   : "Bypass FX Automation: the effect has no automation envelopes (Insert/Remove Envelope > FX Automation)");
    }

    void syncPresets()
    {
        ofx::Instance* inst = pickedOfx();
        AudioPlugin* audio = inst ? nullptr : pickedAudio();
        const bool show = inst != nullptr || audio != nullptr;
        if (presetRow->isHidden() == show) presetRow->setVisible(show);
        presetFile->setVisible(audio && audio->desc.format == "VST2");
        syncBypassAutomation();
        animateBtn->setVisible(inst != nullptr);
        syncKeys();
        if (!show) {
            presetsSlot = 0;
            return;
        }
        if (presetsSlot == picked) return;
        loadPresets(QString());
    }

    void loadPresets(const QString& select)
    {
        ofx::Instance* inst = pickedOfx();
        AudioPlugin* audio = inst ? nullptr : pickedAudio();
        if (!inst && !audio) return;
        presetsSlot = picked;
        presetsAudio = audio != nullptr;
        QSignalBlocker b(presetBox);
        presetBox->clear();
        if (audio) {
            audioItems.clear();
            const FxSlot* sl = slot(picked);
            const int n = std::min(audio->programCount(), 1024);
            for (int k = 0; k < n; ++k) {
                std::string name = audio->programName(k);
                if (name.empty()) name = strfmt("Program %d", k + 1);
                AudioItem it;
                it.program = k;
                it.preset.name = name;
                audioItems.push_back(it);
                presetBox->addItem(qs(name), (int)audioItems.size() - 1);
            }
            bool sep = false;
            for (const AudioPreset& pr : audioPresetsFor(sl ? sl->audio.key() : std::string(), sl ? sl->audio.name : std::string())) {
                if (!sep && presetBox->count() > 0) presetBox->insertSeparator(presetBox->count());
                sep = true;
                AudioItem it;
                it.preset = pr;
                audioItems.push_back(it);
                presetBox->addItem(qs(pr.name), (int)audioItems.size() - 1);
            }
            presetBox->setEditText(QString());
            presetBox->setCurrentIndex(-1);
            if (!select.isEmpty()) {
                const int i = presetBox->findText(select);
                if (i >= 0) presetBox->setCurrentIndex(i);
            }
            presetBox->lineEdit()->setPlaceholderText("(Untitled)");
            updatePresetButtons();
            return;
        }
        // read without the instance's lock: a render may be holding it, and these never change once it is made
        const FxSlot* sl = slot(picked);
        const std::string id = sl ? sl->pluginId : std::string();
        const std::string context = presetContext(inst->context());
        presets = app::presetsFor(id, context);
        presetBox->addItem("(Default)");
        bool userStarted = false;
        for (size_t k = 0; k < presets.size(); ++k) {
            if (presets[k].user && !userStarted && k > 0) presetBox->insertSeparator(presetBox->count());
            userStarted = userStarted || presets[k].user;
            presetBox->addItem(qs(presets[k].name), (int)k);
        }
        presetBox->setEditText(QString());
        presetBox->setCurrentIndex(-1);
        if (!select.isEmpty()) {
            const int i = presetBox->findText(select);
            if (i >= 0) presetBox->setCurrentIndex(i);
        }
        presetBox->lineEdit()->setPlaceholderText("(Untitled)");
        updatePresetButtons();
    }

    void updatePresetButtons()
    {
        const int i = presetBox->currentIndex();
        const QVariant v = i >= 0 ? presetBox->itemData(i) : QVariant();
        if (presetsAudio) {
            const bool user = v.isValid() && (size_t)v.toInt() < audioItems.size() && audioItems[(size_t)v.toInt()].program < 0;
            presetDelete->setEnabled(user);
            presetSave->setEnabled(pickedAudio() != nullptr);
            return;
        }
        const bool user = v.isValid() && (size_t)v.toInt() < presets.size() && presets[(size_t)v.toInt()].user && !presets[(size_t)v.toInt()].vegas;
        presetDelete->setEnabled(user);
        presetSave->setEnabled(pickedOfx() != nullptr);
    }

    void presetChosen(int index)
    {
        if (presetsAudio) {
            AudioPlugin* audio = pickedAudio();
            const QVariant v = index >= 0 ? presetBox->itemData(index) : QVariant();
            if (!audio || !v.isValid() || (size_t)v.toInt() >= audioItems.size()) return;
            const AudioItem it = audioItems[(size_t)v.toInt()];
            app.fxEditStep(picked, "Preset " + it.preset.name, [&] {
                if (it.program >= 0) audio->setProgram(it.program);
                else audio->loadState(it.preset.state);
            });
            updatePresetButtons();
            if (panel) panel->refresh(false);
            return;
        }
        ofx::Instance* inst = pickedOfx();
        if (!inst || index < 0) return;
        const QVariant v = presetBox->itemData(index);
        const double t = fxTime();
        const uint64_t id = picked;
        if (!v.isValid()) {
            if (presetBox->itemText(index) != "(Default)") return;
            app.fxEditStep(id, "Preset (Default)", [&] { applyDefaults(inst, t); });
        } else {
            const size_t k = (size_t)v.toInt();
            if (k >= presets.size()) return;
            const OfxPreset pr = presets[k];
            app.fxEditStep(id, "Preset " + pr.name, [&] { applyPreset(inst, pr, t); });
        }
        updatePresetButtons();
        if (panel) panel->refresh(false);
        syncKeys();
    }

    QString askPresetName()
    {
        QString name = presetBox->currentText().trimmed();
        if (name.isEmpty() || name == "(Default)") {
            bool ok = false;
            name = QInputDialog::getText(q, "Save Preset", "Preset name:", QLineEdit::Normal, QString(), &ok).trimmed();
            if (!ok) return QString();
        }
        return name;
    }

    void savePresetNow()
    {
        if (presetsAudio) {
            AudioPlugin* audio = pickedAudio();
            const FxSlot* sl = slot(picked);
            if (!audio || !sl) return;
            const QString name = askPresetName();
            if (name.isEmpty()) return;
            for (const AudioItem& it : audioItems)
                if (it.program >= 0 && qs(it.preset.name) == name) {
                    QMessageBox::warning(q, "Save Preset", QString("'%1' is one of the plug-in's own programs. Enter a different name.").arg(name));
                    return;
                }
            AudioPreset pr;
            pr.name = s8(name);
            pr.pluginKey = sl->audio.key();
            pr.state = audio->saveState();
            std::string error;
            if (!saveAudioPreset(pr, sl->audio.name, &error)) {
                QMessageBox::warning(q, "Save Preset", qs(error));
                return;
            }
            app.status("Saved the preset \"" + pr.name + "\".");
            loadPresets(name);
            return;
        }
        ofx::Instance* inst = pickedOfx();
        if (!inst) return;
        const QString name = askPresetName();
        if (name.isEmpty()) return;
        std::string error;
        if (!saveUserPreset(capturePreset(inst, fxTime(), s8(name)), &error)) {
            QMessageBox::warning(q, "Save Preset", qs(error));
            return;
        }
        app.status("Saved the preset \"" + s8(name) + "\".");
        loadPresets(name);
    }

    void deletePresetNow()
    {
        const int i = presetBox->currentIndex();
        const QVariant v = i >= 0 ? presetBox->itemData(i) : QVariant();
        if (presetsAudio) {
            if (!v.isValid() || (size_t)v.toInt() >= audioItems.size() || audioItems[(size_t)v.toInt()].program >= 0) return;
            std::string error;
            if (!deleteAudioPreset(audioItems[(size_t)v.toInt()].preset, &error)) {
                QMessageBox::warning(q, "Delete Preset", qs(error));
                return;
            }
            loadPresets(QString());
            return;
        }
        if (!v.isValid() || (size_t)v.toInt() >= presets.size()) return;
        const OfxPreset pr = presets[(size_t)v.toInt()];
        std::string error;
        if (!deleteUserPreset(pr, &error)) {
            QMessageBox::warning(q, "Delete Preset", qs(error));
            return;
        }
        loadPresets(QString());
    }

    bool loadVstFile(QString path)
    {
        AudioPlugin* audio = pickedAudio();
        if (!audio) return false;
        if (path.isEmpty()) {
            path = QFileDialog::getOpenFileName(q, "Load VST Preset", QSettings().value("fxWindow/presetFolder").toString(), "VST presets (*.fxp *.fxb);;All files (*)");
            if (!path.isEmpty()) QSettings().setValue("fxWindow/presetFolder", QFileInfo(path).absolutePath());
        }
        if (path.isEmpty()) return false;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || f.size() > 256 * 1024 * 1024) {
            QMessageBox::warning(q, "Load VST Preset", "Could not read " + path);
            return false;
        }
        const std::string bytes = f.readAll().toStdString();
        std::string error;
        bool ok = false;
        app.fxEditStep(picked, "Load " + s8(QFileInfo(path).fileName()), [&] { ok = audio->loadVstPresetFile(bytes, &error); });
        if (!ok) {
            QMessageBox::warning(q, "Load VST Preset", QString("%1 could not be loaded: %2.").arg(QFileInfo(path).fileName(), qs(error)));
            return false;
        }
        loadPresets(QString());
        if (panel) panel->refresh(false);
        return true;
    }

    bool saveVstFile(bool bank, QString path)
    {
        AudioPlugin* audio = pickedAudio();
        if (!audio) return false;
        if (path.isEmpty()) {
            path = QFileDialog::getSaveFileName(q, bank ? "Save VST Bank" : "Save VST Program", QSettings().value("fxWindow/presetFolder").toString(),
                                                bank ? "VST banks (*.fxb)" : "VST programs (*.fxp)");
            if (!path.isEmpty()) QSettings().setValue("fxWindow/presetFolder", QFileInfo(path).absolutePath());
        }
        if (path.isEmpty()) return false;
        const std::string bytes = audio->saveVstPresetFile(bank);
        QFile f(path);
        if (bytes.empty() || !f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes.data(), (qint64)bytes.size()) != (qint64)bytes.size()) {
            QMessageBox::warning(q, bank ? "Save VST Bank" : "Save VST Program", bytes.empty() ? "The plug-in gave nothing to save." : "Could not write " + path);
            return false;
        }
        app.status("Saved " + s8(path));
        return true;
    }

    double keySpan() const
    {
        const Project& p = app.project();
        const double fps = p.settings.fps();
        double secs = std::max(10.0, p.length());
        if (target.kind == FxTarget::Event)
            if (const Event* e = p.findEvent(target.id)) secs = e->length;
        return std::max(1.0, std::floor(secs * fps + 0.5));
    }

    void syncKeys()
    {
        const bool show = animateBtn->isChecked() && pickedOfx() != nullptr && q->isVisible();
        if (keys->isHidden() == show) keys->setVisible(show);
        if (!show) return;
        keys->strip->slotId = picked;
        keys->refresh();
    }

    void fillAddMenu()
    {
        addMenu->clear();
        // clear() leaves the group menus made last time: they belong to the menu, not to their own entries
        for (QMenu* sub : addMenu->findChildren<QMenu*>(Qt::FindDirectChildrenOnly)) delete sub;
        menuVideo.clear();
        menuAudio.clear();
        if (!chain()) {
            addMenu->addAction("Nothing to add effects to")->setEnabled(false);
            return;
        }
        struct Item { QString group, label; int index; };
        std::vector<Item> items;
        const bool video = isVideo();
        if (video) {
            menuVideo = Catalog::get().video();
            for (size_t k = 0; k < menuVideo.size(); ++k) {
                const VideoFxEntry& e = menuVideo[k];
                if (!e.ok || !e.filter) continue;
                items.push_back({qs(e.group.empty() ? std::string("Other") : e.group), qs(e.label.empty() ? e.id : e.label), (int)k});
            }
        } else {
            menuAudio = Catalog::get().audio();
            for (size_t k = 0; k < menuAudio.size(); ++k) {
                const AudioFxEntry& e = menuAudio[k];
                if (!e.ok || e.desc.isInstrument || !vprefs::audioPluginShown(e.desc.path)) continue;
                const std::string group = !e.desc.vendor.empty() ? e.desc.vendor : (!e.desc.format.empty() ? e.desc.format : std::string("Other"));
                items.push_back({qs(group), qs(e.desc.name.empty() ? pathFileName(e.desc.path) : e.desc.name), (int)k});
            }
        }
        if (items.empty()) {
            addMenu->addAction(video ? "No video effects were found on this computer" : "No audio effects were found on this computer")->setEnabled(false);
            addMenu->addAction("(Options > Look for New Plug-Ins searches again)")->setEnabled(false);
            return;
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            const int g = QString::compare(a.group, b.group, Qt::CaseInsensitive);
            return g != 0 ? g < 0 : QString::compare(a.label, b.label, Qt::CaseInsensitive) < 0;
        });
        std::set<QString> groups;
        for (const Item& it : items) groups.insert(it.group);
        const bool flat = groups.size() <= 1 || items.size() <= 14;
        QMenu* sub = nullptr;
        QString current;
        for (size_t k = 0; k < items.size(); ++k) {
            const Item& it = items[k];
            if (k == 0 || it.group != current) {
                current = it.group;
                if (flat) {
                    if (k > 0) addMenu->addSeparator();
                } else {
                    sub = addMenu->addMenu(QString(it.group).replace("&", "&&"));
                }
            }
            QAction* a = (flat ? addMenu : sub)->addAction(QString(it.label).replace("&", "&&"));
            a->setObjectName("fxAddAction");
            a->setData(it.index);
            if (flat && groups.size() > 1) a->setToolTip(it.group);
            const int index = it.index;
            QObject::connect(a, &QAction::triggered, q, [this, index, video] { addChosen(index, video); });
        }
    }

    void addChosen(int index, bool video)
    {
        if (!chain() || index < 0) return;
        uint64_t id = 0;
        if (video) {
            if ((size_t)index >= menuVideo.size()) return;
            const VideoFxEntry e = menuVideo[(size_t)index];
            id = app.addVideoFx(target, e.id, e.label.empty() ? e.id : e.label);
        } else {
            if ((size_t)index >= menuAudio.size()) return;
            const AudioPluginDesc desc = menuAudio[(size_t)index].desc;
            id = app.addAudioFx(target, desc);
        }
        if (id) pick(id);
    }

    // ---- the panel
    // What the panel should be showing, as a short tag (compared with what the panel that is up says it shows).
    std::string wantedSignature() const
    {
        if (target.kind == FxTarget::None) return targetLost ? "gone" : "none";
        const FxChain* c = chain();
        if (!c) return "gone";
        if (c->empty()) return "empty";
        const FxSlot* s = slot(picked);
        if (!s) return "unpicked";
        const std::string id = std::to_string(s->id);
        if (s->kind == "builtin") return "builtin:" + id;
        if (s->kind == "ofx") {
            if (!app.fx.video(s->id)) return "failed:" + id + ":" + app.fx.error(s->id);
            return "ofx:" + id;
        }
        AudioPlugin* plugin = app.fx.audio(s->id);
        if (!plugin) return "failed:" + id + ":" + app.fx.error(s->id);
        const bool editor = kHostPluginWindows && plugin->hasEditor() && !preferSliders && !editorFailed.count(s->id);
        const bool page = !preferSliders && hasBuiltinPage(builtinPageUid(plugin->desc));
        return (editor ? "audio-window:" : page ? "audio-page:" : "audio-sliders:") + id;
    }

    void dropPanel()
    {
        idleTimer->stop();
        if (!panel) return;
        Panel* old = panel;
        panel = nullptr;
        if (dynamic_cast<OfxPanel*>(old) && q->onEffectShown) q->onEffectShown(0, FxTarget());
        old->requestRebuild = nullptr;
        old->shutDown();
        // Its widgets are deleted after the current event, since the click that got us here may have come from one of
        // them. Meanwhile it is only hidden, not reparented: moving widgets with native windows is slow and some
        // plug-ins notice.
        holderLayout->removeWidget(old);
        old->hide();
        old->deleteLater();
    }

    void rebuildPanel(bool force)
    {
        if (!q->isVisible()) {
            dropPanel();
            return;
        }
        const std::string want = wantedSignature();
        if (!force && panel && panel->signature == want && !panel->stale()) return;
        dropPanel();

        const bool fitted = sizeBeforeEditor.isValid();
        const FxSlot* s = slot(picked);
        Panel* made = nullptr;
        bool audioWindow = false;
        if (want.compare(0, 4, "ofx:") == 0 && s) {
            ofx::Instance* inst = app.fx.video(s->id);
            made = new OfxPanel(holder, s->id, inst, [this] { return fxTime(); }, [this](double frames) { seekFx(frames); }, &groupMemory, playback);
        } else if (want.compare(0, 6, "audio-") == 0 && s) {
            AudioPlugin* plugin = app.fx.audio(s->id);
            hookAudio(plugin);
            const bool wantWindow = want.compare(0, 13, "audio-window:") == 0;
            AudioPanel* ap = new AudioPanel(holder, s->id, plugin, wantWindow, want.compare(0, 11, "audio-page:") == 0);
            ap->onEditorSize = [this] { fitToEditor(); };
            ap->title = qs(fxSlotLabel(*s));
            const uint64_t sid = s->id;
            ap->isBypassed = [this, sid] {
                const FxSlot* sl = slot(sid);
                return sl && sl->bypass;
            };
            ap->setBypassed = [this, sid](bool on) { app.setFxBypass(target, sid, on); };
            made = ap;
        } else if (want.compare(0, 8, "builtin:") == 0 && s) {
            MessagePanel* mp = new MessagePanel(holder, qs(fxSlotLabel(*s)),
                                                "This effect is built into Reno: its settings are in the Color Grading panel (Alt+G).");
            QPushButton* open = new QPushButton("Open the Color Grading Panel", mp);
            open->setObjectName("fxOpenBuiltin");
            const uint64_t id = s->id;
            QObject::connect(open, &QPushButton::clicked, mp, [this, id] { if (q->onOpenBuiltin) q->onOpenBuiltin(id, target); });
            if (QVBoxLayout* v = qobject_cast<QVBoxLayout*>(mp->layout())) v->insertWidget(v->count() - 1, open, 0, Qt::AlignHCenter);
            made = mp;
        } else if (want.compare(0, 7, "failed:") == 0 && s) {
            const QString name = qs(fxSlotLabel(*s));
            const QString why = qs(app.fx.error(s->id));
            made = new MessagePanel(holder, QString("\"%1\" could not be loaded.").arg(name),
                                    (why.isEmpty() ? QString() : why + "\n\n") +
                                        "It stays in the chain with its settings, in case the project is opened where the effect is installed. "
                                        "Use Remove to take it out.");
        } else if (want == "empty" && target.kind == FxTarget::Transition) {
            made = new MessagePanel(holder, "This transition is a plain cross-fade.", "Click the Plug-In Chain button to choose a transition plug-in for it.");
        } else if (want == "empty") {
            made = new MessagePanel(holder, "No effects here yet.", "Click Plug-In Chain (or Add) to choose some. Effects are applied in order, left to right.");
        } else if (want == "gone") {
            made = new MessagePanel(holder, "What these effects were on is no longer in the project.", QString());
        } else {
            made = new MessagePanel(holder, "Nothing to show.", "Open the effects of an event, a track or an output to see them here.");
        }
        panel = made;
        panel->requestRebuild = [this] { queueRebuild(); };
        if (dynamic_cast<OfxPanel*>(made) && q->onEffectShown) q->onEffectShown(s->id, target);
        holderLayout->addWidget(panel);
        panel->show();
        q->layout()->activate();
        panel->start();
        if (AudioPanel* ap = dynamic_cast<AudioPanel*>(panel)) audioWindow = ap->editorOpen();
        // an audio plug-in whose window will not open gets sliders: remember, so as not to try every time
        if (want.compare(0, 13, "audio-window:") == 0 && !audioWindow) {
            editorFailed.insert(s->id);
            panel->signature = "audio-sliders:" + want.substr(13);
        } else {
            panel->signature = want;
        }
        if (panel->wantsIdle()) idleTimer->start();

        bool choice = false;
        if (s && s->kind == "audio")
            if (AudioPlugin* plugin = app.fx.audio(s->id)) {
                if (kHostPluginWindows) choice = plugin->hasEditor() && !editorFailed.count(s->id);
                if (hasBuiltinPage(builtinPageUid(plugin->desc))) choice = true;
            }
        if (modeBtn->isHidden() == choice) modeBtn->setVisible(choice);

        if (audioWindow) {
            if (!fitted) sizeBeforeEditor = q->size();
            fitToEditor();
            QTimer::singleShot(0, q, [this] { fitToEditor(); });           // q as context: not after the window has gone
        } else if (fitted) {
            q->resize(sizeBeforeEditor);
            sizeBeforeEditor = QSize();
        }
        updateStatus();
    }

    void queueRebuild()
    {
        if (rebuildQueued) return;
        rebuildQueued = true;
        QTimer::singleShot(0, q, [this] {               // q as context: not after the window has gone
            rebuildQueued = false;
            rebuildPanel(false);
        });
    }

    void fitToEditor()
    {
        AudioPanel* ap = dynamic_cast<AudioPanel*>(panel);
        if (!ap || !ap->editorOpen()) return;
        const QSize want = ap->editorSize();
        if (!want.isValid()) return;
        if (q->layout()) q->layout()->activate();
        const int frame = 2 * scroll->frameWidth();
        const QSize around(q->width() - scroll->width() + frame, q->height() - scroll->height() + frame);
        QSize size = want + around;
        if (QScreen* screen = q->screen()) {
            const QSize most = screen->availableGeometry().size() - QSize(24, 60);
            size = size.boundedTo(most);
        }
        size = size.expandedTo(q->minimumSize());
        if (size != q->size()) q->resize(size);
    }

    // Audio plug-ins report when a setting is changed from their side and when their window wants another size, possibly
    // from another thread and after this window is gone. The news is passed to the interface thread and to whichever
    // window is then showing that plug-in (the pointer is only compared, never used, until a panel has checked it is
    // still the live one). Plug-ins deliver it through FxRuntime (set up before any thread uses it, see audionews.h).
    void hookAudio(AudioPlugin*)
    {
        static bool hooked = false;
        if (hooked) return;
        hooked = true;
        setAudioPluginNews(
            [](AudioPlugin* plugin, int index) {
                AppState::post([plugin, index] {
                    for (Impl* im : windows())
                        if (AudioPanel* ap = dynamic_cast<AudioPanel*>(im->panel)) ap->paramChangedByPlugin(plugin, index);
                });
            },
            [](AudioPlugin* plugin, int width, int height) {
                AppState::post([plugin, width, height] {
                    for (Impl* im : windows())
                        if (AudioPanel* ap = dynamic_cast<AudioPanel*>(im->panel)) ap->editorResized(plugin, width, height);
                });
            });
    }

    // The effects windows that exist (normally one). Only touched on the interface thread. (Never destroyed: news from a
    // plug-in can still be on its way when the program ends.)
    static std::set<Impl*>& windows()
    {
        static std::set<Impl*>* all = new std::set<Impl*>();
        return *all;
    }

    void changed(int changes)
    {
        if (changes & kProject) {
            syncChain();
            rebuildPanel(false);
            syncBypassAutomation();
        }
        if (!q->isVisible()) return;
        if (changes & (kFxParams | kProject)) {
            updateStatus();
            if (panel) panel->refresh(false);
            if (keys->isVisible()) keys->refresh();
        } else if (changes & kCursor) {
            if (panel) panel->refresh(true);
            if (keys->isVisible()) keys->refresh();
        }
    }

    // A few times a second while the window is up: what plug-ins change without saying (which settings are greyed or
    // hidden, their labels, errors from drawing, audio plug-ins' values).
    void slowTick()
    {
        if (!q->isVisible()) return;
        if (panel && panel->signature != wantedSignature()) { rebuildPanel(false); return; }
        updateStatus();
        if (panel) panel->refresh(false);
    }
};

FxWindow::FxWindow(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->q = this;
    d->playback = playback;
    Impl::installEffectRequests();
    setObjectName("FxWindow");
    setWindowTitle("Effects");
    setMinimumSize(360, 220);

    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(6);

    // VEGAS's header: the track's chip and the name of the event or track, then (at the right) the Plug-In Chain ("fx"
    // with a plus) and Remove Plug-In ("fx" with a cross) buttons.
    QHBoxLayout* top = new QHBoxLayout();
    top->setSpacing(6);
    d->chip = new TargetChip(this);
    d->chip->hide();
    top->addWidget(d->chip);
    d->targetName = new QLabel(this);
    d->targetName->setObjectName("fxTargetName");
    d->targetName->setTextFormat(Qt::PlainText);
    top->addWidget(d->targetName);
    top->addStretch(1);
    d->modeBtn = new QToolButton(this);
    d->modeBtn->setObjectName("fxAudioMode");
    d->modeBtn->setText("Sliders");
    d->modeBtn->setCheckable(true);
    d->modeBtn->setToolTip("Show plain sliders instead of the effect's own page or window");
    d->modeBtn->hide();
    top->addWidget(d->modeBtn);
    d->addBtn = new QToolButton(this);
    d->addBtn->setObjectName("fxAdd");
    d->addBtn->setText("Add");
    d->addBtn->setIcon(glyphIcon(Glyph::Add, palette()));
    d->addBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    d->addBtn->setPopupMode(QToolButton::InstantPopup);
    d->addBtn->setToolTip("Add an effect to the end of the chain");
    d->addMenu = new QMenu(d->addBtn);
    d->addMenu->setObjectName("fxAddMenu");
    d->addMenu->setToolTipsVisible(true);
    d->addBtn->setMenu(d->addMenu);
    top->addWidget(d->addBtn);
    d->leftBtn = smallButton(this, glyphIcon(Glyph::Left, palette()), "Move the picked effect earlier in the chain", "fxMoveLeft");
    d->rightBtn = smallButton(this, glyphIcon(Glyph::Right, palette()), "Move the picked effect later in the chain", "fxMoveRight");
    for (QToolButton* b : {d->leftBtn, d->rightBtn}) {
        b->setAutoRaise(false);
        top->addWidget(b);
    }
    d->chainBtn = new QToolButton(this);
    d->chainBtn->setObjectName("fxPluginChain");
    d->chainBtn->setIcon(fxIcon(true));
    d->chainBtn->setIconSize(QSize(28, 22));
    d->chainBtn->setAutoRaise(true);
    d->chainBtn->setToolTip("Plug-In Chain...");
    d->chainBtn->setFocusPolicy(Qt::NoFocus);
    top->addWidget(d->chainBtn);
    d->removeBtn = smallButton(this, fxIcon(false), "Remove the picked effect", "fxRemove");
    d->removeBtn->setIconSize(QSize(28, 22));
    top->addWidget(d->removeBtn);
    v->addLayout(top);
    d->chainBar = new ChainBar(this);
    v->addWidget(d->chainBar);

    d->presetRow = new QWidget(this);
    d->presetRow->setObjectName("fxPresetRow");
    QHBoxLayout* pr = new QHBoxLayout(d->presetRow);
    pr->setContentsMargins(0, 0, 0, 0);
    pr->setSpacing(4);
    QLabel* presetLabel = new QLabel("Preset:", d->presetRow);
    pr->addWidget(presetLabel);
    d->presetBox = new QComboBox(d->presetRow);
    d->presetBox->setObjectName("fxPreset");
    d->presetBox->setEditable(true);
    d->presetBox->setInsertPolicy(QComboBox::NoInsert);
    d->presetBox->setMinimumWidth(220);
    d->presetBox->setMaximumWidth(340);
    d->presetBox->setMaxVisibleItems(24);
    d->presetBox->setToolTip("The effect's presets and your own. Type a name and click Save Preset to save the current settings.");
    presetLabel->setBuddy(d->presetBox);
    pr->addWidget(d->presetBox, 1);
    d->presetSave = smallButton(d->presetRow, presetIcon(palette(), true), "Save Preset", "fxPresetSave");
    d->presetDelete = smallButton(d->presetRow, presetIcon(palette(), false), "Delete Preset", "fxPresetDelete");
    for (QToolButton* b : {d->presetSave, d->presetDelete}) b->setIconSize(QSize(20, 20));
    pr->addWidget(d->presetSave);
    pr->addWidget(d->presetDelete);
    d->presetHelp = smallButton(d->presetRow, helpIcon(), "Help on presets", "fxPresetHelp");
    d->presetHelp->setIconSize(QSize(20, 20));
    pr->addWidget(d->presetHelp);
    QObject::connect(d->presetHelp, &QToolButton::clicked, this, [this] {
        QMessageBox::information(this, "Presets",
                                 "A preset is a saved set of the effect's settings. Pick one from the list to use it. To save the current "
                                 "settings, type a name in the list and click Save Preset (the disk). Delete Preset (the red cross) removes "
                                 "a preset of your own.");
    });
    d->presetFile = new QToolButton(d->presetRow);
    d->presetFile->setObjectName("fxPresetFile");
    d->presetFile->setText("...");
    d->presetFile->setToolTip("VST preset files: load a program (.fxp) or bank (.fxb), save the program or the whole bank");
    d->presetFile->setPopupMode(QToolButton::InstantPopup);
    d->presetFile->setFocusPolicy(Qt::NoFocus);
    {
        QMenu* m = new QMenu(d->presetFile);
        Impl* im = d.get();
        QObject::connect(m->addAction("&Load Program or Bank (.fxp, .fxb)..."), &QAction::triggered, this, [im] { im->loadVstFile(QString()); });
        QObject::connect(m->addAction("Save &Program As (.fxp)..."), &QAction::triggered, this, [im] { im->saveVstFile(false, QString()); });
        QObject::connect(m->addAction("Save &Bank As (.fxb)..."), &QAction::triggered, this, [im] { im->saveVstFile(true, QString()); });
        d->presetFile->setMenu(m);
    }
    d->presetFile->hide();
    pr->addWidget(d->presetFile);
    d->bypassAuto = new QToolButton(d->presetRow);
    d->bypassAuto->setObjectName("fxBypassAutomation");
    d->bypassAuto->setText("Bypass FX Automation");
    d->bypassAuto->setCheckable(true);
    d->bypassAuto->setFocusPolicy(Qt::NoFocus);
    d->bypassAuto->hide();
    pr->addWidget(d->bypassAuto);
    {
        Impl* im = d.get();
        QObject::connect(d->bypassAuto, &QToolButton::clicked, this, [im](bool on) {
            const uint64_t id = im->picked;
            const FxTarget t = im->target;
            AppState::get().edit(on ? "Bypass FX Automation" : "Follow FX Automation", [&](Project& p) {
                if (FxChain* c = AppState::get().chainOf(p, t))
                    for (FxSlot& s : *c)
                        if (s.id == id) s.bypassAutomation = on;
            });
        });
    }
    pr->addStretch(0);
    // VEGAS's Transition Progress button (a transition's window): puts the Transition Progress envelope on the event, to
    // hold, reverse or repeat the transition; pressed again it takes the envelope off
    d->progressBtn = new QToolButton(d->presetRow);
    d->progressBtn->setObjectName("fxTransitionProgress");
    d->progressBtn->setText("Transition Progress");
    d->progressBtn->setCheckable(true);
    d->progressBtn->setFocusPolicy(Qt::NoFocus);
    d->progressBtn->setToolTip("Add the Transition Progress envelope (click again to remove it); its points hold, reverse or repeat the transition");
    d->progressBtn->hide();
    pr->addWidget(d->progressBtn);
    {
        Impl* im = d.get();
        QObject::connect(d->progressBtn, &QToolButton::clicked, this, [im](bool on) {
            if (im->target.kind != FxTarget::Transition) return;
            EnvelopeRef r;
            r.owner.kind = EnvelopeOwner::Event;
            r.owner.id = im->target.id;
            r.kind = "transition";
            if (on) { if (!im->app.insertEnvelope(r)) im->app.showEnvelope(r, true); }
            else im->app.removeEnvelope(r);
        });
    }
    d->adjustLenBtn = new QToolButton(d->presetRow);
    d->adjustLenBtn->setObjectName("fxAdjustEventLength");
    d->adjustLenBtn->setText("Adjust Event Length");
    d->adjustLenBtn->setFocusPolicy(Qt::NoFocus);
    d->adjustLenBtn->setToolTip("Stretch the event to the Slow Motion playback rate so no frames are cut off");
    d->adjustLenBtn->hide();
    pr->addWidget(d->adjustLenBtn);
    {
        Impl* im = d.get();
        QObject::connect(d->adjustLenBtn, &QToolButton::clicked, this, [im] {
            if (im->target.kind != FxTarget::Event) return;
            const uint64_t id = im->target.id;
            const Event* ev = im->app.project().findEvent(id);
            if (!ev) return;
            const double rate = eventFxRate(*ev);
            if (!(rate > 0)) return;
            const double len = ev->length / rate;
            im->app.edit("Adjust Event Length", [&](Project& p) {
                if (Event* o = p.findEvent(id)) o->length = len;
            });
        });
    }
    d->animateBtn = new QToolButton(d->presetRow);
    d->animateBtn->setObjectName("fxAnimate");
    d->animateBtn->setText("Animate");
    d->animateBtn->setCheckable(true);
    d->animateBtn->setFocusPolicy(Qt::NoFocus);
    d->animateBtn->setToolTip("Show the keyframes of the effect's settings over time, under its controls");
    d->animateBtn->setChecked(QSettings().value("fxWindow/animate", false).toBool());
    pr->addWidget(d->animateBtn);
    // VEGAS's Play and Stop in its effect windows: the project played from the cursor with the effect as it is set now,
    // and stopped (back to where play began).
    for (int k = 0; k < 2; ++k) {
        QToolButton* b = new QToolButton(d->presetRow);
        b->setObjectName(k == 0 ? "fxPlay" : "fxStop");
        b->setText(k == 0 ? "Play" : "Stop");
        b->setToolTip(k == 0 ? "Play from the cursor, to see the effect as it is set now" : "Stop playing (back to where play began)");
        b->setFocusPolicy(Qt::NoFocus);
        pr->addWidget(b);
        Impl* im = d.get();
        QObject::connect(b, &QToolButton::clicked, this, [im, k] {
            if (!im->playback) return;
            if (k == 0) { if (!im->playback->playing()) im->playback->play(); }
            else im->playback->stop();
        });
    }
    d->presetRow->hide();
    v->addWidget(d->presetRow);

    d->banner = new QLabel(this);
    d->banner->setObjectName("fxBanner");
    d->banner->setTextFormat(Qt::PlainText);
    d->banner->setWordWrap(true);
    d->banner->hide();
    v->addWidget(d->banner);

    d->scroll = new QScrollArea(this);
    d->scroll->setObjectName("fxScroll");
    d->scroll->setWidgetResizable(true);
    d->holder = new QWidget();
    d->holder->setObjectName("fxHolder");
    d->holderLayout = new QVBoxLayout(d->holder);
    d->holderLayout->setContentsMargins(0, 0, 0, 0);
    d->holderLayout->setSpacing(0);
    d->scroll->setWidget(d->holder);
    v->addWidget(d->scroll, 1);

    d->keys = new KeyPanel(this);
    d->keys->setMaximumHeight(240);
    d->keys->hide();
    v->addWidget(d->keys);
    {
        Impl* im = d.get();
        KeyStrip* ks = d->keys->strip;
        ks->live = [im] { return im->pickedOfx(); };
        ks->now = [im] { return im->fxTime(); };
        ks->seek = [im](double frames) { im->seekFx(frames); };
        ks->span = [im] { return im->keySpan(); };
        ks->rate = [im] { return im->app.project().settings.fps(); };
        QObject::connect(d->animateBtn, &QToolButton::toggled, this, [im](bool on) {
            QSettings().setValue("fxWindow/animate", on);
            im->syncKeys();
        });
    }

    Impl* im = d.get();
    QObject::connect(d->addMenu, &QMenu::aboutToShow, this, [im] { im->fillAddMenu(); });
    QObject::connect(d->leftBtn, &QToolButton::clicked, this, [im] { if (im->picked) im->app.moveFx(im->target, im->picked, -1); });
    QObject::connect(d->rightBtn, &QToolButton::clicked, this, [im] { if (im->picked) im->app.moveFx(im->target, im->picked, 1); });
    QObject::connect(d->removeBtn, &QToolButton::clicked, this, [im] { im->removePicked(); });
    QObject::connect(d->chainBtn, &QToolButton::clicked, this, [im] { im->openChooser(); });
    QObject::connect(d->presetBox, QOverload<int>::of(&QComboBox::activated), this, [im](int i) { im->presetChosen(i); });
    QObject::connect(d->presetBox, &QComboBox::currentIndexChanged, this, [im](int) { im->updatePresetButtons(); });
    QObject::connect(d->presetSave, &QToolButton::clicked, this, [im] { im->savePresetNow(); });
    QObject::connect(d->presetDelete, &QToolButton::clicked, this, [im] { im->deletePresetNow(); });
    QObject::connect(d->modeBtn, &QToolButton::toggled, this, [im](bool on) {
        im->preferSliders = on;
        im->rebuildPanel(false);
    });

    d->slowTimer = new QTimer(this);
    d->slowTimer->setInterval(300);
    QObject::connect(d->slowTimer, &QTimer::timeout, this, [im] { im->slowTick(); });
    d->idleTimer = new QTimer(this);
    d->idleTimer->setInterval(50);
    QObject::connect(d->idleTimer, &QTimer::timeout, this, [im] {
        if (im->panel && im->panel->wantsIdle()) im->panel->idle();
        else im->idleTimer->stop();
    });

    d->listener = d->app.addListener([im](int changes) { im->changed(changes); });
    Impl::windows().insert(im);
    d->syncChain();
}

FxWindow::~FxWindow()
{
    d->app.removeListener(d->listener);
    Impl::windows().erase(d.get());
    d->slowTimer->stop();
    d->idleTimer->stop();
    if (d->panel) {
        d->panel->requestRebuild = nullptr;
        d->panel->shutDown();
        delete d->panel;
        d->panel = nullptr;
    }
}

void FxWindow::setEffectBeforePanCrop(uint64_t slotId, bool before)
{
    if (d->target.kind == FxTarget::Event) d->setBeforePanCrop(slotId, before);
}

void FxWindow::showTarget(const FxTarget& target, uint64_t slotId)
{
    d->target = target;
    d->targetLost = false;
    d->picked = 0;
    d->order.clear();
    if (const FxChain* c = d->chain()) {
        for (const FxSlot& s : *c)
            if (s.id == slotId) d->picked = slotId;
        if (!d->picked && !c->empty()) d->picked = c->back().id;
    } else {
        d->target = FxTarget();
    }
    d->syncChain();
    d->rebuildPanel(false);
}

FxTarget FxWindow::target() const { return d->target; }

void FxWindow::setOnlySlots(const std::vector<uint64_t>& slotIds, bool lockChain)
{
    d->onlySlots = slotIds;
    if (lockChain)
        for (QToolButton* b : {d->addBtn, d->leftBtn, d->rightBtn, d->chainBtn, d->removeBtn}) b->hide();
    d->syncChain();
    d->rebuildPanel(true);
}

void FxWindow::closeEvent(QCloseEvent* e)
{
    d->dropPanel();
    QWidget::closeEvent(e);
}

void FxWindow::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    d->syncChain();
    d->rebuildPanel(false);
    d->slowTimer->start();
}

void FxWindow::hideEvent(QHideEvent* e)
{
    d->slowTimer->stop();
    d->dropPanel();
    QWidget::hideEvent(e);
}

}
}
