// Controls of the touch interface: buttons and list rows sized for fingers, a slider with a large thumb that shows its
// value, a switch, and the sheet that slides up from the bottom (or in from the side on a wide screen) to hold them.
// They draw themselves, so they look the same on every platform and ignore the desktop's VEGAS-sized style.
#pragma once
#include "touch_style.h"
#include <QAbstractButton>
#include <QWidget>
#include <functional>

class QVBoxLayout;
class QScrollArea;
class QLabel;
class QVariantAnimation;

namespace reno {
namespace app {
namespace touch {

class Button : public QAbstractButton {
public:
    enum Style {
        Bar,        // a round icon button (top bar, transport)
        Tool,       // icon over a short label (the tool bar)
        Primary,    // a filled accent pill with text (Export, Add)
        Row,        // a list row: icon, title and subtitle at the left, a value or chevron at the right
        Chip        // a small rounded choice (speed presets); checkable
    };
    Button(Style style, Icon icon, const QString& text, QWidget* parent = nullptr);
    void setIconKind(Icon icon);
    void setSubtitle(const QString& s);
    void setValueText(const QString& s);        // Row: shown at the right
    void setChevron(bool on);                   // Row: an arrow at the right (opens something)
    void setDanger(bool on);                    // drawn red (Delete)
    void setShowIcon(bool on);
    Style style() const { return style_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return style_ == Chip ? QSize(40, 40) : sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Style style_;
    Icon icon_;
    bool showIcon_ = true;
    QString subtitle_, value_;
    bool chevron_ = false, danger_ = false;
};

// A row with a title and a switch at the right; the whole row toggles it.
class Switch : public QAbstractButton {
public:
    Switch(const QString& text, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
};

// A slider for a value: title and the value as text above, the track below. Dragging changes the value from where the
// finger is (the thumb doesn't have to be hit); a double tap puts back the default. 'toPos' and 'fromPos' map the value
// to the track (0..1), so a scale can be in decibels or logarithmic.
class Slider : public QWidget {
public:
    Slider(const QString& title, double lo, double hi, double value, QWidget* parent = nullptr);
    void setValue(double v);                    // no callbacks
    double value() const { return value_; }
    void setDefault(double v) { default_ = v; }
    void setFormat(std::function<QString(double)> fn) { format_ = std::move(fn); update(); }
    void setMapping(std::function<double(double)> toPos, std::function<double(double)> fromPos);
    void setStep(double step) { step_ = step; }     // values are rounded to it (0: not)
    std::function<void()> onBegin;              // a drag starts (one undo step until onEnd)
    std::function<void(double)> onChange;
    std::function<void()> onEnd;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QRectF trackRect() const;
    double valueAtX(double x) const;
    void moveTo(double x);
    QString title_;
    double lo_, hi_, value_, default_;
    double step_ = 0;
    bool dragging_ = false;
    std::function<QString(double)> format_;
    std::function<double(double)> toPos_, fromPos_;
};

// A colour setting: its name and value, and a row of colour dots to tap; the last dot opens the full colour dialog.
class Swatches : public QWidget {
public:
    Swatches(const QString& title, const QColor& value, QWidget* parent = nullptr);
    void setValue(const QColor& c);
    std::function<void(const QColor&)> onPick;     // a dot was tapped
    std::function<void()> onMore;                   // the "more colours" dot
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QRectF dot(int k) const;
    QString title_;
    QColor value_;
};

// A small heading inside a sheet.
QLabel* sectionLabel(const QString& text, QWidget* parent);

// The panel that holds a tool's controls. The shell places it (see TouchShell::placeSheet) and shows it sliding in;
// it closes from its close button, a swipe down on its grip, or a tap outside it.
class Sheet : public QWidget {
public:
    Sheet(const QString& title, QWidget* parent);
    QVBoxLayout* body() const { return body_; }
    void setTitle(const QString& title);
    void addHeaderButton(QAbstractButton* b);   // left of the close button
    void setScrolls(bool on);                   // content in a scroll area (default) or laid out directly
    int preferredHeight(int width) const;
    std::function<void()> onClosed;             // after it is gone (it deletes itself)
    void close();
    bool sideways = false;                      // placed at the side: no grip, square corners
    // A modal sheet (lists, menus) closes when tapped outside; one that adjusts the selection (speed, volume) leaves
    // the timeline usable beside it and closes when the selection changes.
    bool modal = true;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QLabel* title_;
    QWidget* header_;
    QWidget* content_;
    QScrollArea* scroll_;
    QVBoxLayout* body_;
    QVBoxLayout* outer_;
    bool grabbing_ = false;
    double grabY_ = 0;
    int grabTop_ = 0;
    bool closing_ = false;
};

// Kinetic scrolling for a scroll area driven by a finger (and a mouse on a desktop).
void makeFlickable(QScrollArea* area);

}  // namespace touch
}  // namespace app
}  // namespace reno
