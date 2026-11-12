#include "touch_ui.h"
#if RENO_TOUCH_UI  // (the whole file: phones and tablets only)
#include "touch_widgets.h"
#include <QConicalGradient>
#include <QEasingCurve>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QVBoxLayout>
#include <cmath>

namespace reno {
namespace app {
namespace touch {

// ---------------------------------------------------------------- Button

Button::Button(Style style, Icon icon, const QString& text, QWidget* parent) : QAbstractButton(parent), style_(style), icon_(icon)
{
    setText(text);
    setAccessibleName(text);        // (screen readers: the tool bar's icons have their names)
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    // rows fill the width; chips share it (a row of them never makes a sheet wider than the screen)
    if (style == Row || style == Chip) setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    else setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (style == Chip) setCheckable(true);
}

void Button::setIconKind(Icon icon) { icon_ = icon; update(); }
void Button::setSubtitle(const QString& s) { subtitle_ = s; updateGeometry(); update(); }
void Button::setValueText(const QString& s) { value_ = s; update(); }
void Button::setChevron(bool on) { chevron_ = on; update(); }
void Button::setDanger(bool on) { danger_ = on; update(); }
void Button::setShowIcon(bool on) { showIcon_ = on; updateGeometry(); update(); }

QSize Button::sizeHint() const
{
    const QFontMetrics fm(uiFont(15, true));
    switch (style_) {
    case Bar: return QSize(kTarget, kTarget);
    case Tool: return QSize(std::max(64, QFontMetrics(uiFont(11)).horizontalAdvance(text()) + 16), kToolH);
    case Primary: return QSize(fm.horizontalAdvance(text()) + (showIcon_ ? 64 : 40), 40);
    case Chip: return QSize(std::max(48, QFontMetrics(uiFont(14, true)).horizontalAdvance(text()) + 20), 40);
    case Row: return QSize(200, subtitle_.isEmpty() ? 56 : 64);
    }
    return QSize(kTarget, kTarget);
}

void Button::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect();
    const bool down = isDown();
    const QColor fg = !isEnabled() ? color::faint : danger_ ? color::danger : color::text;
    switch (style_) {
    case Bar: {
        if (down || isChecked()) {
            p.setPen(Qt::NoPen);
            p.setBrush(isChecked() ? color::accent : color::raised);
            p.drawEllipse(r.center(), r.width() / 2 - 4, r.height() / 2 - 4);
        }
        drawIcon(p, icon_, QRectF(r.center().x() - 12, r.center().y() - 12, 24, 24), isChecked() ? Qt::white : fg);
        break;
    }
    case Tool: {
        if (down) {
            p.setPen(Qt::NoPen);
            p.setBrush(color::raised);
            p.drawRoundedRect(r.adjusted(4, 6, -4, -6), 10, 10);
        }
        const QColor c = isChecked() ? color::accentLight : fg;
        drawIcon(p, icon_, QRectF(r.center().x() - 12, r.top() + 13, 24, 24), c);
        p.setPen(c);
        p.setFont(uiFont(11));
        p.drawText(QRectF(r.left() + 2, r.top() + 41, r.width() - 4, 18), Qt::AlignHCenter | Qt::AlignTop,
                   p.fontMetrics().elidedText(text(), Qt::ElideRight, (int)r.width() - 4));
        break;
    }
    case Primary: {
        QColor bg = isEnabled() ? color::accent : color::raised;
        if (down) bg = bg.darker(120);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), r.height() / 2 - 1, r.height() / 2 - 1);
        p.setFont(uiFont(15, true));
        const int tw = p.fontMetrics().horizontalAdvance(text());
        const double iconW = showIcon_ ? 26 : 0;
        const double x = r.center().x() - (tw + iconW) / 2;
        if (showIcon_) drawIcon(p, icon_, QRectF(x, r.center().y() - 10, 20, 20), Qt::white);
        p.setPen(isEnabled() ? QColor(Qt::white) : color::faint);
        p.drawText(QRectF(x + iconW, r.top(), tw + 2, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text());
        break;
    }
    case Chip: {
        const bool on = isChecked();
        p.setPen(on ? Qt::NoPen : QPen(color::line, 1));
        p.setBrush(on ? color::accent : down ? color::raised : color::sheet);
        p.drawRoundedRect(r.adjusted(1, 4, -1, -4), (r.height() - 8) / 2, (r.height() - 8) / 2);
        p.setPen(on ? QColor(Qt::white) : fg);
        p.setFont(uiFont(14, true));
        p.drawText(r, Qt::AlignCenter, text());
        break;
    }
    case Row: {
        if (down) p.fillRect(r, color::raised);
        double x = 16;
        if (showIcon_) {
            drawIcon(p, icon_, QRectF(x, r.center().y() - 12, 24, 24), danger_ ? color::danger : isChecked() ? color::accentLight : color::dim);
            x += 40;
        }
        double right = r.right() - 16;
        if (chevron_) {
            drawIcon(p, Icon::Right, QRectF(right - 18, r.center().y() - 9, 18, 18), color::faint);
            right -= 26;
        }
        if (isCheckable() && isChecked() && !chevron_) {
            drawIcon(p, Icon::Check, QRectF(right - 22, r.center().y() - 11, 22, 22), color::accentLight);
            right -= 30;
        }
        if (!value_.isEmpty()) {
            p.setFont(uiFont(14));
            p.setPen(color::dim);
            const int vw = std::min(p.fontMetrics().horizontalAdvance(value_), (int)(r.width() * 0.4));
            p.drawText(QRectF(right - vw, r.top(), vw, r.height()), Qt::AlignVCenter | Qt::AlignRight,
                       p.fontMetrics().elidedText(value_, Qt::ElideRight, vw));
            right -= vw + 12;
        }
        p.setPen(fg);
        p.setFont(uiFont(15, false));
        if (subtitle_.isEmpty()) {
            p.drawText(QRectF(x, r.top(), right - x, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(text(), Qt::ElideRight, (int)(right - x)));
        } else {
            p.drawText(QRectF(x, r.top() + 10, right - x, 22), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(text(), Qt::ElideRight, (int)(right - x)));
            p.setFont(uiFont(12));
            p.setPen(color::dim);
            p.drawText(QRectF(x, r.top() + 32, right - x, 20), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(subtitle_, Qt::ElideRight, (int)(right - x)));
        }
        break;
    }
    }
}

// ---------------------------------------------------------------- Switch

Switch::Switch(const QString& text, QWidget* parent) : QAbstractButton(parent)
{
    setText(text);
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize Switch::sizeHint() const { return QSize(200, 56); }

void Switch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect();
    if (isDown()) p.fillRect(r, color::raised);
    p.setPen(isEnabled() ? color::text : color::faint);
    p.setFont(uiFont(15));
    p.drawText(r.adjusted(16, 0, -80, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
    const QRectF track(r.right() - 16 - 46, r.center().y() - 13, 46, 26);
    p.setPen(Qt::NoPen);
    p.setBrush(isChecked() ? color::accent : color::line);
    p.drawRoundedRect(track, 13, 13);
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(isChecked() ? track.right() - 13 : track.left() + 13, track.center().y()), 10, 10);
}

// ---------------------------------------------------------------- Slider

Slider::Slider(const QString& title, double lo, double hi, double value, QWidget* parent)
    : QWidget(parent), title_(title), lo_(lo), hi_(hi), value_(value), default_(value)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    format_ = [](double v) { return QString::number(v, 'f', 2); };
    toPos_ = [this](double v) { return (v - lo_) / std::max(1e-12, hi_ - lo_); };
    fromPos_ = [this](double f) { return lo_ + f * (hi_ - lo_); };
}

void Slider::setValue(double v)
{
    value_ = std::max(lo_, std::min(hi_, v));
    update();
}

void Slider::setMapping(std::function<double(double)> toPos, std::function<double(double)> fromPos)
{
    toPos_ = std::move(toPos);
    fromPos_ = std::move(fromPos);
    update();
}

QSize Slider::sizeHint() const { return QSize(240, 76); }

QRectF Slider::trackRect() const { return QRectF(24, 46, width() - 48, 6); }

double Slider::valueAtX(double x) const
{
    const QRectF t = trackRect();
    const double f = std::max(0.0, std::min(1.0, (x - t.left()) / std::max(1.0, t.width())));
    double v = fromPos_(f);
    if (step_ > 0) v = std::round(v / step_) * step_;
    return std::max(lo_, std::min(hi_, v));
}

void Slider::moveTo(double x)
{
    const double v = valueAtX(x);
    if (v == value_) return;
    value_ = v;
    update();
    if (onChange) onChange(value_);
}

void Slider::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(uiFont(14));
    p.setPen(isEnabled() ? color::text : color::faint);
    p.drawText(QRectF(24, 8, width() - 48, 24), Qt::AlignVCenter | Qt::AlignLeft, title_);
    p.setFont(uiFont(14, true));
    p.setPen(isEnabled() ? color::accentLight : color::faint);
    p.drawText(QRectF(24, 8, width() - 48, 24), Qt::AlignVCenter | Qt::AlignRight, format_(value_));
    const QRectF t = trackRect();
    p.setPen(Qt::NoPen);
    p.setBrush(color::line);
    p.drawRoundedRect(t, 3, 3);
    const double f = std::max(0.0, std::min(1.0, toPos_(value_)));
    const double fd = std::max(0.0, std::min(1.0, toPos_(default_)));
    // the part between the default and the value is filled (so a change from the default shows either way)
    const double a = t.left() + t.width() * std::min(f, fd), b = t.left() + t.width() * std::max(f, fd);
    p.setBrush(isEnabled() ? color::accent : color::faint);
    p.drawRoundedRect(QRectF(a, t.top(), std::max(0.0, b - a), t.height()), 3, 3);
    p.setBrush(color::faint);
    p.drawEllipse(QPointF(t.left() + t.width() * fd, t.center().y()), 2, 2);
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(t.left() + t.width() * f, t.center().y()), dragging_ ? 13 : 11, dragging_ ? 13 : 11);
}

void Slider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    if (onBegin) onBegin();
    moveTo(e->position().x());
    update();
}

void Slider::mouseMoveEvent(QMouseEvent* e)
{
    if (dragging_) moveTo(e->position().x());
}

void Slider::mouseReleaseEvent(QMouseEvent*)
{
    if (!dragging_) return;
    dragging_ = false;
    update();
    if (onEnd) onEnd();
}

void Slider::mouseDoubleClickEvent(QMouseEvent* e)
{
    // the second tap of a double tap: back to the default (one undo step of its own)
    if (onBegin) onBegin();
    value_ = default_;
    update();
    if (onChange) onChange(value_);
    if (onEnd) onEnd();
    e->accept();
}

// ---------------------------------------------------------------- Swatches

namespace {
const QColor kSwatches[] = {QColor(255, 255, 255), QColor(0, 0, 0), QColor(229, 57, 53), QColor(255, 152, 0), QColor(255, 235, 59),
                            QColor(67, 160, 71), QColor(0, 188, 212), QColor(30, 136, 229), QColor(142, 36, 170), QColor(236, 64, 122)};
const int kSwatchCount = sizeof(kSwatches) / sizeof(kSwatches[0]);
}  // namespace

Swatches::Swatches(const QString& title, const QColor& value, QWidget* parent) : QWidget(parent), title_(title), value_(value)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Swatches::setValue(const QColor& c)
{
    value_ = c;
    update();
}

QSize Swatches::sizeHint() const { return QSize(240, 84); }

// The dots share the width: the preset colours, then the "more" one.
QRectF Swatches::dot(int k) const
{
    const int n = kSwatchCount + 1;
    const double avail = width() - 32.0;
    const double step = avail / n;
    const double d = std::min(36.0, step - 6);
    return QRectF(16 + step * k + (step - d) / 2, 40, d, d);
}

void Swatches::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(uiFont(14));
    p.setPen(color::text);
    p.drawText(QRectF(24, 6, width() - 48, 26), Qt::AlignVCenter | Qt::AlignLeft, title_);
    p.setFont(uiFont(14, true));
    p.setPen(color::accentLight);
    p.drawText(QRectF(24, 6, width() - 48, 26), Qt::AlignVCenter | Qt::AlignRight, value_.name().toUpper());
    for (int k = 0; k < kSwatchCount; ++k) {
        const QRectF r = dot(k);
        const bool on = kSwatches[k].rgb() == value_.rgb();
        p.setPen(on ? QPen(color::accentLight, 3) : QPen(color::line, 1));
        p.setBrush(kSwatches[k]);
        p.drawEllipse(r.adjusted(2, 2, -2, -2));
    }
    // more: a colour wheel
    const QRectF r = dot(kSwatchCount).adjusted(2, 2, -2, -2);
    QConicalGradient g(r.center(), 0);
    for (int k = 0; k <= 6; ++k) g.setColorAt(k / 6.0, QColor::fromHsv((k * 60) % 360, 220, 255));
    p.setPen(QPen(color::line, 1));
    p.setBrush(g);
    p.drawEllipse(r);
}

void Swatches::mouseReleaseEvent(QMouseEvent* e)
{
    for (int k = 0; k <= kSwatchCount; ++k)
        if (dot(k).adjusted(-4, -6, 4, 6).contains(e->position())) {
            if (k == kSwatchCount) {
                if (onMore) onMore();
            } else {
                value_ = kSwatches[k];
                update();
                if (onPick) onPick(value_);
            }
            return;
        }
}

QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    QLabel* l = new QLabel(text.toUpper(), parent);
    l->setFont(uiFont(11, true));
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, color::dim);
    l->setPalette(pal);
    l->setContentsMargins(16, 14, 16, 4);
    return l;
}

// ---------------------------------------------------------------- Sheet

void makeFlickable(QScrollArea* area)
{
    QScroller::grabGesture(area->viewport(), QScroller::LeftMouseButtonGesture);
    QScroller* s = QScroller::scroller(area->viewport());
    QScrollerProperties sp = s->scrollerProperties();
    // presses reach the rows at once (no delay before a tap shows), and a drag only scrolls once it is plainly a drag
    sp.setScrollMetric(QScrollerProperties::MousePressEventDelay, 0.0);
    sp.setScrollMetric(QScrollerProperties::DragStartDistance, 0.004);
    sp.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    sp.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootWhenScrollable);
    sp.setScrollMetric(QScrollerProperties::OvershootDragResistanceFactor, 0.4);
    sp.setScrollMetric(QScrollerProperties::OvershootScrollDistanceFactor, 0.1);
    sp.setScrollMetric(QScrollerProperties::AxisLockThreshold, 0.6);
    s->setScrollerProperties(sp);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

Sheet::Sheet(const QString& title, QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    outer_ = new QVBoxLayout(this);
    outer_->setContentsMargins(0, 18, 0, 0);
    outer_->setSpacing(0);
    header_ = new QWidget(this);
    auto* hl = new QHBoxLayout(header_);
    hl->setContentsMargins(16, 0, 4, 4);
    hl->setSpacing(4);
    title_ = new QLabel(title, header_);
    title_->setFont(uiFont(17, true));
    QPalette pal = title_->palette();
    pal.setColor(QPalette::WindowText, color::text);
    title_->setPalette(pal);
    hl->addWidget(title_, 1);
    Button* closeB = new Button(Button::Bar, Icon::Close, QString(), header_);
    closeB->setObjectName("sheetClose");
    QObject::connect(closeB, &QAbstractButton::clicked, this, [this] { close(); });
    hl->addWidget(closeB);
    outer_->addWidget(header_);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    content_ = new QWidget();
    body_ = new QVBoxLayout(content_);
    body_->setContentsMargins(0, 0, 0, 16);
    body_->setSpacing(0);
    scroll_->setWidget(content_);
    makeFlickable(scroll_);
    outer_->addWidget(scroll_, 1);
}

void Sheet::setTitle(const QString& title) { title_->setText(title); }

void Sheet::addHeaderButton(QAbstractButton* b)
{
    auto* hl = static_cast<QHBoxLayout*>(header_->layout());
    b->setParent(header_);
    hl->insertWidget(hl->count() - 1, b);
}

void Sheet::setScrolls(bool on)
{
    if (on) return;
    // the content laid out in the sheet itself (a list with its own scrolling, or a few controls)
    scroll_->takeWidget();
    outer_->removeWidget(scroll_);
    scroll_->deleteLater();
    outer_->addWidget(content_, 1);
}

int Sheet::preferredHeight(int width) const
{
    body_->activate();
    const int h = body_->hasHeightForWidth() ? body_->totalHeightForWidth(width) : body_->totalSizeHint().height();
    return outer_->contentsMargins().top() + header_->sizeHint().height() + h + 8;
}

void Sheet::close()
{
    if (closing_) return;
    closing_ = true;
    // slides out, then goes
    auto* a = new QPropertyAnimation(this, "pos", this);
    a->setDuration(160);
    a->setEasingCurve(QEasingCurve::InCubic);
    a->setStartValue(pos());
    a->setEndValue(sideways ? QPoint(parentWidget()->width(), y()) : QPoint(x(), parentWidget()->height()));
    std::function<void()> done = onClosed;
    QObject::connect(a, &QPropertyAnimation::finished, this, [this, done] {
        hide();
        if (done) done();
        deleteLater();
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void Sheet::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect();
    QPainterPath path;
    if (sideways) path.addRect(r);
    else {
        // rounded top corners only
        const double rad = 14;
        path.moveTo(r.left(), r.bottom());
        path.lineTo(r.left(), r.top() + rad);
        path.quadTo(r.left(), r.top(), r.left() + rad, r.top());
        path.lineTo(r.right() - rad, r.top());
        path.quadTo(r.right(), r.top(), r.right(), r.top() + rad);
        path.lineTo(r.right(), r.bottom());
        path.closeSubpath();
    }
    p.fillPath(path, color::sheet);
    if (sideways) p.fillRect(QRectF(0, 0, 1, r.height()), color::line);
    else {
        // the grip
        p.setPen(Qt::NoPen);
        p.setBrush(color::line);
        p.drawRoundedRect(QRectF(r.center().x() - 18, 7, 36, 4), 2, 2);
    }
}

// A drag down from the top of the sheet (above its rows) pulls it down; far enough, it closes.
void Sheet::mousePressEvent(QMouseEvent* e)
{
    if (sideways || e->position().y() > header_->geometry().bottom()) return;
    grabbing_ = true;
    grabY_ = e->globalPosition().y();
    grabTop_ = y();
}

void Sheet::mouseMoveEvent(QMouseEvent* e)
{
    if (!grabbing_) return;
    move(x(), grabTop_ + std::max(0, (int)(e->globalPosition().y() - grabY_)));
}

void Sheet::mouseReleaseEvent(QMouseEvent* e)
{
    if (!grabbing_) return;
    grabbing_ = false;
    if (e->globalPosition().y() - grabY_ > std::min(120, height() / 4)) close();
    else move(x(), grabTop_);
}

}  // namespace touch
}  // namespace app
}  // namespace reno
#endif  // RENO_TOUCH_UI
