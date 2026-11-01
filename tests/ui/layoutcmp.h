// Compares a Reno window's controls with the control list captured from the same window in VEGAS Pro 18
// (tests/ui/data/vegas_*.txt: lines "kind <tab> text <tab> x <tab> y <tab> w <tab> h" under "## <section>" headings, positions
// relative to the window's top left), and writes side-by-side pictures. Header-only, used by the interface tests.
#pragma once
#include <QAbstractButton>
#include <QCheckBox>
#include <QRadioButton>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QComboBox>
#include <QFile>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QTextDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QStringList>
#include <QToolButton>
#include <QTreeWidget>
#include <QGroupBox>
#include <vector>

namespace layoutcmp {

struct Cell {
    QString kind, text;
    QRect r;
};

// The cells under the "## <section>" heading that starts with 'section' (the capture's tab or dialog name).
inline std::vector<Cell> readSection(const QString& file, const QString& section)
{
    std::vector<Cell> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return out;
    bool in = false;
    for (const QString& ln : QString::fromUtf8(f.readAll()).split('\n')) {
        if (ln.startsWith("## ")) {
            in = ln.mid(3).trimmed().startsWith(section);
            continue;
        }
        if (!in || ln.isEmpty() || ln[0] == '#') continue;
        const QStringList p = ln.split('\t');
        if (p.size() < 6) continue;
        out.push_back({p[0], p[1], QRect(p[2].toInt(), p[3].toInt(), p[4].toInt(), p[5].toInt())});
    }
    return out;
}

inline bool isControl(const QWidget* w)
{
    return qobject_cast<const QAbstractButton*>(w) || qobject_cast<const QLabel*>(w) || qobject_cast<const QComboBox*>(w) ||
           qobject_cast<const QAbstractSlider*>(w) || qobject_cast<const QLineEdit*>(w) || qobject_cast<const QPlainTextEdit*>(w) ||
           qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QListWidget*>(w) || qobject_cast<const QTreeWidget*>(w) ||
           qobject_cast<const QGroupBox*>(w) || (qobject_cast<const QFrame*>(w) && static_cast<const QFrame*>(w)->frameShape() == QFrame::HLine);
}

// Reno's controls under 'page' in the window's coordinates ('origin' is the window, 'shift' what the title bar and frame add).
inline std::vector<Cell> collect(QWidget* page, QWidget* origin, QPoint shift)
{
    std::vector<Cell> out;
    for (QWidget* w : page->findChildren<QWidget*>()) {
        if (!w->isVisibleTo(page) || !isControl(w)) continue;
        bool inside = false;
        for (QWidget* a = w->parentWidget(); a && a != page; a = a->parentWidget())
            if (isControl(a) && !qobject_cast<QGroupBox*>(a)) inside = true;
        if (inside) continue;
        Cell c;
        c.r = QRect(w->mapTo(origin, QPoint(0, 0)) + shift, w->size());
        if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            c.kind = qobject_cast<QCheckBox*>(w) ? "check" : qobject_cast<QRadioButton*>(w) ? "radio" : qobject_cast<QToolButton*>(w) && b->text().isEmpty() ? "color" : "push";
            c.text = b->text();
        } else if (auto* l = qobject_cast<QLabel*>(w)) {
            c.kind = "label";
            c.text = l->text();
            if (l->textFormat() != Qt::PlainText && c.text.contains("<a ")) {   // a link: its words
                QTextDocument doc;
                doc.setHtml(c.text);
                c.text = doc.toPlainText();
            }
        } else if (auto* g = qobject_cast<QGroupBox*>(w)) {
            c.kind = "group";
            c.text = g->title();
        } else if (qobject_cast<QFrame*>(w) && !qobject_cast<QLabel*>(w) && !qobject_cast<QAbstractScrollArea*>(w)) {
            c.kind = "label";
        } else if (qobject_cast<QComboBox*>(w)) c.kind = "combo";
        else if (qobject_cast<QAbstractSlider*>(w)) c.kind = "slider";
        else if (qobject_cast<QAbstractSpinBox*>(w)) c.kind = "spinbox";
        else if (qobject_cast<QListWidget*>(w)) c.kind = "list";
        else if (qobject_cast<QTreeWidget*>(w)) c.kind = "list";
        else c.kind = "edit";
        out.push_back(c);
    }
    return out;
}

// Labels show their mnemonic only when they have a control to give the focus to, so their texts compare without the &.
inline QString norm(const QString& t, bool label = false) { return label ? QString(t).remove('&').trimmed() : t.trimmed(); }

inline bool hasText(const QString& kind) { return kind == "label" || kind == "check" || kind == "radio" || kind == "push" || kind == "group"; }

inline bool near(const QRect& a, const QRect& b, int tol, bool growable = false)
{
    // a text that does not fit in its cell with the platform's font may be given more width (never less)
    if (growable) return std::abs(a.x() - b.x()) <= tol && std::abs(a.y() - b.y()) <= tol && std::abs(a.height() - b.height()) <= tol && a.width() >= b.width() - tol && a.width() <= b.width() + 140;
    return std::abs(a.x() - b.x()) <= tol && std::abs(a.y() - b.y()) <= tol && std::abs(a.width() - b.width()) <= tol && std::abs(a.height() - b.height()) <= tol;
}

struct Result {
    int wanted = 0, matched = 0;
    QStringList problems;  // one line each: what VEGAS has that Reno does not match
    QStringList extras;    // Reno controls VEGAS has no cell for
};

// Every wanted cell needs a Reno control of the same kind (and text, where VEGAS shows one) in the same place (within 'tol' pixels).
inline Result compare(const std::vector<Cell>& want, std::vector<Cell> have, int tol = 2)
{
    Result res;
    std::vector<bool> taken(have.size(), false);
    // edit boxes with spin arrows: a Reno spin box is VEGAS's edit box and its spin cell together
    for (size_t i = 0; i < want.size(); ++i) {
        const Cell& w = want[i];
        if (w.kind == "spin" || w.kind == "pane") continue;
        ++res.wanted;
        QRect wr = w.r;
        QString kind = w.kind == "listview" ? QString("list") : w.kind;
        if (w.kind == "edit" && i + 1 < want.size() && want[i + 1].kind == "spin" && want[i + 1].r.x() == w.r.right() + 1) {
            wr = w.r.united(want[i + 1].r);
            kind = "spinbox";
        }
        int found = -1;
        for (size_t k = 0; k < have.size() && found < 0; ++k) {
            if (taken[k] || have[k].kind != kind) continue;
            if (hasText(kind) && !w.text.isEmpty() && w.text != "<value>" && norm(have[k].text, kind == "label") != norm(w.text, kind == "label")) continue;
            if (near(have[k].r, wr, tol, kind == "label" || kind == "check" || kind == "radio")) found = (int)k;
        }
        if (found >= 0) {
            taken[(size_t)found] = true;
            ++res.matched;
            continue;
        }
        QString near_;
        for (size_t k = 0; k < have.size(); ++k)
            if (have[k].kind == kind && (w.text.isEmpty() || w.text == "<value>" || norm(have[k].text, kind == "label") == norm(w.text, kind == "label")) &&
                std::abs(have[k].r.y() - wr.y()) < 40)
                near_ = QString(" (Reno has it at %1,%2 %3x%4)").arg(have[k].r.x()).arg(have[k].r.y()).arg(have[k].r.width()).arg(have[k].r.height());
        res.problems << QString("%1 \"%2\" %3,%4 %5x%6 not matched%7").arg(kind, w.text).arg(wr.x()).arg(wr.y()).arg(wr.width()).arg(wr.height()).arg(near_);
    }
    for (size_t k = 0; k < have.size(); ++k)
        if (!taken[k]) res.extras << QString("%1 \"%2\" at %3,%4 %5x%6").arg(have[k].kind, have[k].text).arg(have[k].r.x()).arg(have[k].r.y()).arg(have[k].r.width()).arg(have[k].r.height());
    return res;
}

// Reno's picture on the left and VEGAS's on the right, side by side.
inline void sideBySide(const QImage& reno, const QString& vegasPng, const QString& out)
{
    const QImage v(vegasPng);
    QImage img(reno.width() + v.width() + 12, std::max(reno.height(), v.height()), QImage::Format_RGB32);
    img.fill(QColor(255, 0, 255));
    QPainter p(&img);
    p.drawImage(0, 0, reno);
    if (!v.isNull()) p.drawImage(reno.width() + 12, 0, v);
    p.end();
    img.save(out);
}

}  // namespace layoutcmp
