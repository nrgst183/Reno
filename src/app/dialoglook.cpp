// The light dialog look (see dialoglook.h): Windows 10's palette and flat controls for the dialogs VEGAS draws that way.
#include "dialoglook.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QLineEdit>
#include <QObject>
#include <QPolygonF>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QStyleOptionFrame>
#include <QStyleOptionSlider>
#include <QStyleOptionSpinBox>
#include <QStyleOptionViewItem>

namespace reno {
namespace app {

namespace {

const char* const kDarkProperty = "renoDark";

// Windows 10 control colours.
const QColor kWindow(240, 240, 240), kBase(255, 255, 255), kText(0, 0, 0), kGray(109, 109, 109);
const QColor kButton(225, 225, 225), kButtonEdge(173, 173, 173), kButtonHot(229, 241, 251), kButtonHotEdge(0, 120, 215);
const QColor kButtonDown(204, 228, 247), kButtonDownEdge(0, 84, 153), kButtonOff(204, 204, 204), kButtonOffEdge(191, 191, 191);
const QColor kEditEdge(122, 122, 122), kListEdge(130, 135, 144), kBoxInk(51, 51, 51), kGroupEdge(220, 220, 220);

// The dialog's own style sheet over the dark one of the application: tabs, list headers and tool buttons.
const char* const kLightSheet =
    "QTabBar { background: transparent; }"
    "QTabBar::tab { background: #f0f0f0; color: #000000; border: 1px solid #d9d9d9; border-bottom: none; padding: 2px 6px 2px 6px; margin: 0px; }"
    "QTabBar::tab:selected { background: #ffffff; color: #000000; }"
    "QTabBar::tab:hover:!selected { background: #d8eaf9; color: #000000; }"
    "QTabBar::tab:disabled { color: #6d6d6d; }"
    "QTabWidget::pane { border: 1px solid #d9d9d9; background: #ffffff; }"
    "QHeaderView::section { background: #ffffff; color: #000000; border: none; border-right: 1px solid #e5e5e5; border-bottom: 1px solid #e5e5e5; padding: 2px 5px; }"
    "QToolButton { border: 1px solid transparent; border-radius: 0px; padding: 2px; color: #000000; background: transparent; }"
    "QToolButton:hover { background: #e5f1fb; border-color: #0078d7; }"
    "QToolButton:pressed { background: #cce4f7; border-color: #005499; }"
    "QToolButton:checked { background: #cce4f7; border-color: #005499; }"
    "QToolButton:disabled { color: #6d6d6d; }";

// The top-level window a widget belongs to, following parents through popups (a combo box's list belongs to its dialog).
QWidget* topOf(QWidget* w)
{
    // the nearest dialog that is a window of its own (a dialog is parented to the main window, so not simply the root)
    for (QWidget* x = w; x; x = x->parentWidget())
        if (x->isWindow() && qobject_cast<QDialog*>(x)) return x;
    return w->window();
}

class DialogLookFilter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() != QEvent::Polish || !o->isWidgetType()) return false;
        QWidget* w = static_cast<QWidget*>(o);
        QWidget* top = topOf(w);
        if (!qobject_cast<QDialog*>(top) || top->property(kDarkProperty).toBool()) return false;
        if (w == top) {
            if (w->property("renoLight").toBool()) return false;
            w->setProperty("renoLight", true);
            w->setPalette(lightDialogPalette());
            w->setStyleSheet(QString::fromLatin1(kLightSheet) + w->styleSheet());
            return false;
        }
        // the dialog's widgets: the style sheet of the application gave each its own copy of the dark palette when it was
        // polished, so the dialog's palette alone does not reach them
        if (w->palette().color(QPalette::Window).lightness() < 160 && !w->property("renoKeepPalette").toBool()) {
            const QColor text = w->palette().color(QPalette::WindowText);
            QPalette p = lightDialogPalette();
            if (w->testAttribute(Qt::WA_SetPalette) && text.lightness() > 128 && text.lightness() < 200 && text.saturation() < 40) {
                // a note in a dimmed grey keeps being dimmed
                p.setColor(QPalette::WindowText, QColor(109, 109, 109));
            }
            w->setPalette(p);
        }
        return false;
    }
};

void chevron(QPainter* p, const QPointF& c, bool down, double a, const QColor& ink)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    p->setBrush(Qt::NoBrush);
    const double s = down ? 1.0 : -1.0;
    p->drawPolyline(QPolygonF() << QPointF(c.x() - a, c.y() - s * a * 0.5) << QPointF(c.x(), c.y() + s * a * 0.5)
                                << QPointF(c.x() + a, c.y() - s * a * 0.5));
    p->restore();
}

void box(QPainter* p, const QRect& r, const QColor& fill, const QColor& edge)
{
    p->fillRect(r, fill);
    p->setPen(edge);
    p->setBrush(Qt::NoBrush);
    p->drawRect(r.adjusted(0, 0, -1, -1));
}

void drawCheck(QPainter* p, const QRect& r0, bool on, bool partial, bool enabled, bool hot, bool down)
{
    const int s = 13;
    const QRect r(r0.center().x() - s / 2, r0.center().y() - s / 2, s, s);
    p->save();
    const QColor edge = !enabled ? QColor(188, 188, 188) : (hot || down) ? kButtonHotEdge : kBoxInk;
    box(p, r, down ? QColor(204, 228, 247) : kBase, edge);
    if (on || partial) {
        const QColor ink = enabled ? kBoxInk : QColor(131, 131, 131);
        if (partial && !on) {
            p->fillRect(r.adjusted(3, 3, -3, -3), ink);
        } else {
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
            p->drawPolyline(QPolygonF() << QPointF(r.left() + 3.0, r.top() + 6.5) << QPointF(r.left() + 5.3, r.top() + 9.0)
                                        << QPointF(r.left() + 9.7, r.top() + 3.5));
        }
    }
    p->restore();
}

void drawRadio(QPainter* p, const QRect& r0, bool on, bool enabled, bool hot, bool down)
{
    const int s = 13;
    const QRect r(r0.center().x() - s / 2, r0.center().y() - s / 2, s, s);
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const QColor edge = !enabled ? QColor(188, 188, 188) : (hot || down) ? kButtonHotEdge : kBoxInk;
    p->setPen(edge);
    p->setBrush(down ? QColor(204, 228, 247) : kBase);
    p->drawEllipse(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5));
    if (on) {
        p->setPen(Qt::NoPen);
        p->setBrush(enabled ? kBoxInk : QColor(131, 131, 131));
        p->drawEllipse(QRectF(r).adjusted(3.5, 3.5, -3.5, -3.5));
    }
    p->restore();
}

// A push button face: flat grey, blue edge when hot, pressed or the default button.
void drawButtonFace(QPainter* p, const QRect& r, const QStyleOption* o)
{
    const bool enabled = o->state & QStyle::State_Enabled, down = o->state & QStyle::State_Sunken, hot = o->state & QStyle::State_MouseOver;
    bool def = false;
    if (const auto* b = qstyleoption_cast<const QStyleOptionButton*>(o)) def = b->features & (QStyleOptionButton::DefaultButton | QStyleOptionButton::AutoDefaultButton) &&
                                                                           (b->features & QStyleOptionButton::DefaultButton);
    const bool checked = o->state & QStyle::State_On;
    QColor fill = kButton, edge = kButtonEdge;
    if (!enabled) { fill = kButtonOff; edge = kButtonOffEdge; }
    else if (down || checked) { fill = kButtonDown; edge = kButtonDownEdge; }
    else if (hot) { fill = kButtonHot; edge = kButtonHotEdge; }
    box(p, r, fill, edge);
    if (enabled && def && !down && !hot) {
        p->setPen(kButtonHotEdge);
        p->drawRect(r.adjusted(0, 0, -1, -1));
        p->drawRect(r.adjusted(1, 1, -2, -2));
    }
}

}  // namespace

void installDialogLook()
{
    static bool done = false;
    if (done) return;
    done = true;
    qApp->installEventFilter(new DialogLookFilter(qApp));
}

void makeDialogDark(QWidget* w)
{
    if (w) w->setProperty(kDarkProperty, true);
}

QPalette lightDialogPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, kWindow);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, kBase);
    p.setColor(QPalette::AlternateBase, QColor(245, 245, 245));
    p.setColor(QPalette::ToolTipBase, QColor(255, 255, 225));
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::Button, kButton);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, QColor(0, 120, 215));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0, 102, 204));
    p.setColor(QPalette::PlaceholderText, kGray);
    p.setColor(QPalette::Light, Qt::white);
    p.setColor(QPalette::Midlight, QColor(227, 227, 227));
    p.setColor(QPalette::Mid, QColor(160, 160, 160));
    p.setColor(QPalette::Dark, QColor(105, 105, 105));
    p.setColor(QPalette::Shadow, QColor(105, 105, 105));
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(QPalette::Disabled, r, kGray);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(204, 204, 204));
    p.setColor(QPalette::Disabled, QPalette::Base, kWindow);
    return p;
}

bool isLightPalette(const QPalette& p)
{
    return p.color(QPalette::Window).lightness() > 160;
}

bool lightDrawPrimitive(const QStyle* style, QStyle::PrimitiveElement pe, const QStyleOption* o, QPainter* p, const QWidget* w)
{
    (void)style;
    if (!o) return false;
    if (!isLightPalette(o->palette)) {
        // VEGAS's dark dialogs draw check boxes and radio buttons as white boxes with a dark mark (greyed when off-limits)
        const bool box = pe == QStyle::PE_IndicatorCheckBox, radio = pe == QStyle::PE_IndicatorRadioButton;
        if (!(box || radio) || !w) return false;
        bool dark = false;
        for (const QWidget* x = w; x; x = x->parentWidget()) dark = dark || x->property(kDarkProperty).toBool();
        if (!dark) return false;
        const bool on = o->state & QStyle::State_Enabled, checked = o->state & QStyle::State_On, hot = o->state & QStyle::State_MouseOver;
        const QRectF r(QRectF(o->rect).center().x() - 6.5, QRectF(o->rect).center().y() - 6.5, 13, 13);
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(on ? (hot ? QColor(18, 176, 255) : QColor(200, 200, 200)) : QColor(90, 90, 90));
        p->setBrush(on ? QColor(250, 250, 250) : QColor(70, 70, 70));
        if (box) p->drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5)); else p->drawEllipse(r.adjusted(0.5, 0.5, -0.5, -0.5));
        if (checked) {
            const QColor ink = on ? QColor(30, 30, 30) : QColor(120, 120, 120);
            if (box) {
                p->setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
                p->setBrush(Qt::NoBrush);
                p->drawPolyline(QPolygonF() << QPointF(r.left() + 3.0, r.top() + 6.5) << QPointF(r.left() + 5.3, r.top() + 9.0) << QPointF(r.left() + 9.7, r.top() + 3.5));
            } else {
                p->setPen(Qt::NoPen);
                p->setBrush(ink);
                p->drawEllipse(r.adjusted(3.5, 3.5, -3.5, -3.5));
            }
        }
        p->restore();
        return true;
    }
    using S = QStyle;
    const bool enabled = o->state & S::State_Enabled, hot = o->state & S::State_MouseOver, down = o->state & S::State_Sunken;
    switch (pe) {
    case S::PE_PanelButtonCommand:
        drawButtonFace(p, o->rect, o);
        return true;
    case S::PE_PanelButtonTool:
        // tool buttons are the style sheet's
        return false;
    case S::PE_FrameDefaultButton:
        return true;
    case S::PE_FrameLineEdit:
        p->save();
        p->setPen(enabled ? kEditEdge : QColor(204, 204, 204));
        p->setBrush(Qt::NoBrush);
        p->drawRect(o->rect.adjusted(0, 0, -1, -1));
        p->restore();
        return true;
    case S::PE_PanelLineEdit: {
        // inside a combo box or spin box the outer control draws the frame
        const bool inner = w && w->parentWidget() && (qobject_cast<const QComboBox*>(w->parentWidget()) || qobject_cast<const QAbstractSpinBox*>(w->parentWidget()));
        if (inner) {
            p->fillRect(o->rect, o->palette.color(enabled ? QPalette::Base : QPalette::Window));
            return true;
        }
        const auto* f = qstyleoption_cast<const QStyleOptionFrame*>(o);
        p->fillRect(o->rect, o->palette.color(enabled ? QPalette::Base : QPalette::Window));
        if (!f || f->lineWidth > 0) {
            p->save();
            p->setPen(enabled ? kEditEdge : QColor(204, 204, 204));
            p->setBrush(Qt::NoBrush);
            p->drawRect(o->rect.adjusted(0, 0, -1, -1));
            p->restore();
        }
        return true;
    }
    case S::PE_Frame:
    case S::PE_FrameTabWidget: {
        if (pe == S::PE_FrameTabWidget) return false;
        const auto* f = qstyleoption_cast<const QStyleOptionFrame*>(o);
        if (f && f->frameShape != QFrame::StyledPanel && f->frameShape != QFrame::Box && f->frameShape != QFrame::Panel) return false;
        p->save();
        p->setPen(kListEdge);
        p->setBrush(Qt::NoBrush);
        p->drawRect(o->rect.adjusted(0, 0, -1, -1));
        p->restore();
        return true;
    }
    case S::PE_FrameGroupBox:
        p->save();
        p->setPen(kGroupEdge);
        p->setBrush(Qt::NoBrush);
        p->drawRect(o->rect.adjusted(0, 0, -1, -1));
        p->restore();
        return true;
    case S::PE_IndicatorCheckBox:
    case S::PE_IndicatorItemViewItemCheck:
        drawCheck(p, o->rect, o->state & S::State_On, o->state & S::State_NoChange, enabled, hot, down);
        return true;
    case S::PE_IndicatorRadioButton:
        drawRadio(p, o->rect, o->state & S::State_On, enabled, hot, down);
        return true;
    case S::PE_IndicatorArrowDown:
        chevron(p, QRectF(o->rect).center(), true, 3.5, enabled ? QColor(80, 80, 80) : QColor(160, 160, 160));
        return true;
    case S::PE_IndicatorArrowUp:
        chevron(p, QRectF(o->rect).center(), false, 3.5, enabled ? QColor(80, 80, 80) : QColor(160, 160, 160));
        return true;
    default: return false;
    }
}

bool lightDrawComplex(const QStyle* style, QStyle::ComplexControl cc, const QStyleOptionComplex* o, QPainter* p, const QWidget* w)
{
    if (!o || !isLightPalette(o->palette)) return false;
    using S = QStyle;
    const bool enabled = o->state & S::State_Enabled, hot = o->state & S::State_MouseOver;
    if (cc == S::CC_ComboBox) {
        const auto* c = qstyleoption_cast<const QStyleOptionComboBox*>(o);
        if (!c) return false;
        const QRect arrow = style->subControlRect(cc, o, S::SC_ComboBoxArrow, w);
        if (c->editable) {
            p->fillRect(o->rect, o->palette.color(enabled ? QPalette::Base : QPalette::Window));
            p->setPen(enabled ? kEditEdge : QColor(204, 204, 204));
            p->setBrush(Qt::NoBrush);
            p->drawRect(o->rect.adjusted(0, 0, -1, -1));
            chevron(p, QRectF(arrow).center(), true, 3.5, enabled ? QColor(80, 80, 80) : QColor(160, 160, 160));
        } else {
            // Windows 10's drop-down list: a flat grey button with the arrow at the right
            const bool open = o->state & S::State_On;
            box(p, o->rect, !enabled ? QColor(240, 240, 240) : open ? kButtonDown : hot ? kButtonHot : kButton,
                !enabled ? QColor(217, 217, 217) : open ? kButtonDownEdge : hot ? kButtonHotEdge : kButtonEdge);
            chevron(p, QRectF(arrow).center(), true, 3.5, enabled ? QColor(60, 60, 60) : QColor(160, 160, 160));
        }
        return true;
    }
    if (cc == S::CC_SpinBox) {
        const auto* sb = qstyleoption_cast<const QStyleOptionSpinBox*>(o);
        if (!sb) return false;
        p->fillRect(o->rect, o->palette.color(enabled ? QPalette::Base : QPalette::Window));
        p->setPen(enabled ? kEditEdge : QColor(204, 204, 204));
        p->setBrush(Qt::NoBrush);
        p->drawRect(o->rect.adjusted(0, 0, -1, -1));
        for (S::SubControl sc : {S::SC_SpinBoxUp, S::SC_SpinBoxDown}) {
            const QRect r = style->subControlRect(cc, o, sc, w).adjusted(0, 0, -1, 0);
            const bool active = (sb->activeSubControls & sc);
            const bool pressed = active && (o->state & S::State_Sunken);
            p->fillRect(r.adjusted(0, sc == S::SC_SpinBoxUp ? 1 : 0, 0, sc == S::SC_SpinBoxUp ? 0 : -1), pressed ? kButtonDown : active && hot ? kButtonHot : QColor(240, 240, 240));
            const QColor ink = enabled ? QColor(60, 60, 60) : QColor(160, 160, 160);
            chevron(p, QRectF(r).center(), sc == S::SC_SpinBoxDown, 2.6, ink);
        }
        return true;
    }
    if (cc == S::CC_ScrollBar) {
        const auto* s = qstyleoption_cast<const QStyleOptionSlider*>(o);
        if (!s) return false;
        const bool horizontal = s->orientation == Qt::Horizontal;
        p->save();
        p->fillRect(s->rect, QColor(240, 240, 240));
        for (S::SubControl sc : {S::SC_ScrollBarSubLine, S::SC_ScrollBarAddLine}) {
            const QRect r = style->subControlRect(cc, s, sc, w);
            if (r.isEmpty()) continue;
            const bool active = (s->activeSubControls & sc);
            if (active && (s->state & S::State_Sunken)) p->fillRect(r, QColor(96, 96, 96));
            else if (active && hot) p->fillRect(r, QColor(218, 218, 218));
            const QPointF c = QRectF(r).center();
            const bool back = sc == S::SC_ScrollBarSubLine;
            const QColor ink = (active && (s->state & S::State_Sunken)) ? Qt::white : enabled ? QColor(96, 96, 96) : QColor(163, 163, 163);
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
            p->setBrush(Qt::NoBrush);
            const double a = 3.0;
            QPolygonF line;
            if (horizontal) {
                const double d = back ? 1 : -1;
                line << QPointF(c.x() + d * a * 0.5, c.y() - a) << QPointF(c.x() - d * a * 0.5, c.y()) << QPointF(c.x() + d * a * 0.5, c.y() + a);
            } else {
                const double d = back ? 1 : -1;
                line << QPointF(c.x() - a, c.y() + d * a * 0.5) << QPointF(c.x(), c.y() - d * a * 0.5) << QPointF(c.x() + a, c.y() + d * a * 0.5);
            }
            p->drawPolyline(line);
            p->setRenderHint(QPainter::Antialiasing, false);
        }
        const QRect h = style->subControlRect(cc, s, S::SC_ScrollBarSlider, w);
        if (!h.isEmpty()) {
            const bool active = (s->activeSubControls & S::SC_ScrollBarSlider);
            const QColor c = !enabled ? QColor(240, 240, 240) : active && (s->state & S::State_Sunken) ? QColor(96, 96, 96) : active && hot ? QColor(166, 166, 166) : QColor(205, 205, 205);
            p->fillRect(horizontal ? h.adjusted(0, 2, 0, -2) : h.adjusted(2, 0, -2, 0), c);
        }
        p->restore();
        return true;
    }
    if (cc == S::CC_Slider) {
        const auto* s = qstyleoption_cast<const QStyleOptionSlider*>(o);
        if (!s) return false;
        const bool horizontal = s->orientation == Qt::Horizontal;
        const QRect groove = style->subControlRect(cc, s, S::SC_SliderGroove, w);
        const QRect handle = style->subControlRect(cc, s, S::SC_SliderHandle, w);
        p->save();
        // Windows' track: a sunken line, dark above (left), light below
        if (horizontal) {
            const int y = groove.center().y();
            p->fillRect(QRect(groove.left(), y - 1, groove.width(), 1), QColor(160, 160, 160));
            p->fillRect(QRect(groove.left(), y, groove.width(), 1), QColor(255, 255, 255));
            p->fillRect(QRect(groove.left(), y + 1, groove.width(), 1), QColor(227, 227, 227));
        } else {
            const int x = groove.center().x();
            p->fillRect(QRect(x - 1, groove.top(), 1, groove.height()), QColor(160, 160, 160));
            p->fillRect(QRect(x, groove.top(), 1, groove.height()), QColor(255, 255, 255));
            p->fillRect(QRect(x + 1, groove.top(), 1, groove.height()), QColor(227, 227, 227));
        }
        const bool down = s->state & S::State_Sunken;
        box(p, handle, down ? QColor(190, 190, 190) : QColor(235, 235, 235), QColor(120, 120, 120));
        p->setPen(QColor(255, 255, 255));
        p->drawLine(handle.left() + 1, handle.top() + 1, handle.right() - 2, handle.top() + 1);
        p->drawLine(handle.left() + 1, handle.top() + 1, handle.left() + 1, handle.bottom() - 2);
        p->restore();
        return true;
    }
    return false;
}

}  // namespace app
}  // namespace reno
