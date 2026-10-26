// Small widgets shared by the editdialogs*.cpp files.
#pragma once
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <cmath>
#include <utility>
#include <vector>

namespace reno {
namespace app {
namespace editdlg {

// A label in the grey used for explanations under controls.
inline QLabel* note(const QString& text, QWidget* parent)
{
    QLabel* l = new QLabel(text, parent);
    l->setObjectName("note");
    l->setWordWrap(true);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, QColor(160, 160, 166));
    l->setPalette(p);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.92);
    l->setFont(f);
    return l;
}

// A number box that takes a typed value only on Enter and ignores the mouse wheel until clicked.
class Spin : public QDoubleSpinBox {
public:
    explicit Spin(QWidget* parent, double lo, double hi, int decimals, double step, const char* name) : QDoubleSpinBox(parent)
    {
        setObjectName(name);
        setRange(lo, hi);
        setDecimals(decimals);
        setSingleStep(step);
        setKeyboardTracking(false);
        setFocusPolicy(Qt::StrongFocus);
        setAccelerated(true);
    }
    // Shows a value silently (only if it differs, so typing is not disturbed).
    void showValue(double v)
    {
        if (hasFocus() && lineEdit()->isModified()) return;
        if (std::fabs(value() - v) < 0.5 * std::pow(10.0, -decimals())) return;
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

inline QComboBox* combo(QWidget* parent, const char* name, const std::vector<std::pair<QString, QString>>& items)
{
    QComboBox* c = new QComboBox(parent);
    c->setObjectName(name);
    for (const auto& it : items) c->addItem(it.first, it.second);
    return c;
}

inline void pickData(QComboBox* c, const QString& data)
{
    const int i = c->findData(data);
    QSignalBlocker b(c);
    c->setCurrentIndex(i >= 0 ? i : 0);
}

}  // namespace editdlg
}  // namespace app
}  // namespace reno
