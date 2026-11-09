// Dialogs laid out by fixed positions (VEGAS's own sizes: Preferences, Advanced Audio Configuration, Project Properties...)
// fit the screen and can be resized: when shown, the dialog's contents go into a scroll area (scroll bars when the screen
// is smaller than the dialog), its bottom row of buttons stays in view under it, and the dialog is made no larger than the
// screen. Made larger, the contents stretch: drop-down lists, edit boxes, sliders, lists and pages that reach into the
// right half of their area grow by the extra width (what is to their right moves along), and the tallest list or page
// grows by the extra height (what is below it moves down). At the size it was made, nothing moves.
#include "dialogfit.h"
#include <QAbstractScrollArea>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontDialog>
#include <QFrame>
#include <QGroupBox>
#include <QInputDialog>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QWizard>
#include <algorithm>
#include <map>
#include <vector>

namespace reno {
namespace app {

namespace {

const char* const kFitted = "renoDialogFitted";

bool isLine(const QWidget* w)
{
    const QFrame* f = qobject_cast<const QFrame*>(w);
    return f && f->metaObject() == &QFrame::staticMetaObject && f->frameShape() == QFrame::HLine;
}

// A widget whose children are placed by hand (no layout) and that is a plain area of the dialog.
bool isArea(const QWidget* w)
{
    if (w->layout()) return false;
    const QMetaObject* m = w->metaObject();
    if (m != &QWidget::staticMetaObject && m != &QFrame::staticMetaObject && m != &QGroupBox::staticMetaObject) return false;
    for (QObject* c : w->children())
        if (c->isWidgetType() && !static_cast<QWidget*>(c)->isWindow()) return true;
    return false;
}

bool stretchesAcross(const QWidget* w)
{
    if (qobject_cast<const QComboBox*>(w) || qobject_cast<const QLineEdit*>(w) || qobject_cast<const QAbstractScrollArea*>(w) ||
        qobject_cast<const QTabWidget*>(w) || qobject_cast<const QProgressBar*>(w) || isLine(w) || isArea(w))
        return true;
    if (const QSlider* s = qobject_cast<const QSlider*>(w)) return s->orientation() == Qt::Horizontal;
    return false;
}

bool stretchesDown(const QWidget* w, int areaHeight)
{
    if (qobject_cast<const QTabWidget*>(w) || isArea(w)) return w->height() >= areaHeight * 6 / 10;
    if (qobject_cast<const QAbstractScrollArea*>(w)) return w->height() >= areaHeight * 3 / 10;
    return false;
}

// One area's children and what each does when the area changes size.
struct Area {
    QSize made;   // the area's size as the dialog was made
    struct Child {
        QPointer<QWidget> w;
        QRect made;
        bool widen = false, right = false, grow = false, down = false;
    };
    std::vector<Child> children;
};

class Stretcher : public QObject {
public:
    explicit Stretcher(QObject* parent) : QObject(parent) {}

    // Takes the sizes as they are now as the sizes the dialog was made for ('size': the area's, when it is not its own
    // yet: a tab widget's hidden pages are sized when first shown).
    void watch(QWidget* area, QSize size = QSize())
    {
        if (!area || areas_.count(area)) return;
        Area a;
        a.made = size.isValid() ? size : area->size();
        const int W = a.made.width(), H = a.made.height();
        std::vector<QWidget*> kids;
        for (QObject* o : area->children())
            if (o->isWidgetType() && !static_cast<QWidget*>(o)->isWindow() && !static_cast<QWidget*>(o)->isHidden()) kids.push_back(static_cast<QWidget*>(o));
        auto toRight = [&](QWidget* w) {
            std::vector<QWidget*> v;
            const QRect g = w->geometry();
            for (QWidget* s : kids)
                if (s != w && s->x() >= g.right() - 3 && s->y() < g.bottom() && s->geometry().bottom() > g.top()) v.push_back(s);
            return v;
        };
        // what moves right: not a stretching kind, in the right half, and only movers to its right (decided right to left)
        std::vector<QWidget*> order = kids;
        std::sort(order.begin(), order.end(), [](QWidget* x, QWidget* y) { return x->x() > y->x(); });
        std::map<QWidget*, bool> mover;
        for (QWidget* w : order) {
            bool m = !stretchesAcross(w) && w->x() > W / 2;
            for (QWidget* s : toRight(w)) m = m && mover[s];
            mover[w] = m;
        }
        // the one that grows down: the tallest list or page
        QWidget* tall = nullptr;
        for (QWidget* w : kids)
            if (stretchesDown(w, H) && (!tall || w->height() > tall->height())) tall = w;
        for (QWidget* w : kids) {
            Area::Child c;
            c.w = w;
            c.made = w->geometry();
            c.right = mover[w];
            if (!c.right && stretchesAcross(w) && w->geometry().right() > W / 2) {
                bool clear = true;
                for (QWidget* s : toRight(w)) clear = clear && mover[s];
                c.widen = clear;
            }
            c.grow = w == tall;
            c.down = tall && w != tall && w->y() >= tall->geometry().bottom() - 2 && w->x() < tall->geometry().right() && w->geometry().right() > tall->x();
            if (c.widen || c.right || c.grow || c.down) a.children.push_back(c);
            if (isArea(w)) watch(w);
            // a tab widget's pages are areas of their own
            if (QTabWidget* t = qobject_cast<QTabWidget*>(w)) {
                const QSize pageSize = t->currentWidget() ? t->currentWidget()->size() : QSize();
                for (int i = 0; i < t->count(); ++i)
                    if (QWidget* pg = t->widget(i)) {
                        if (isArea(pg)) watch(pg, pageSize);
                    }
            }
        }
        areas_[area] = a;
        area->installEventFilter(this);
    }

    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() != QEvent::Resize) return false;
        auto it = areas_.find(static_cast<QWidget*>(o));
        if (it == areas_.end()) return false;
        Area& a = it->second;
        // from the sizes as made (a page not yet shown is first told a size it never had, then its own), each child changed
        // only along what it follows, so where other code has put it otherwise stays
        const QSize now = static_cast<QResizeEvent*>(e)->size();
        const int dx = now.width() - a.made.width(), dy = now.height() - a.made.height();
        for (Area::Child& c : a.children) {
            if (!c.w) continue;
            QRect g = c.w->geometry();
            if (c.right) g.moveLeft(c.made.x() + dx);
            if (c.widen) g.setWidth(std::max(8, c.made.width() + dx));
            if (c.down) g.moveTop(c.made.y() + dy);
            if (c.grow) g.setHeight(std::max(8, c.made.height() + dy));
            if (g != c.w->geometry()) c.w->setGeometry(g);
        }
        return false;
    }

private:
    std::map<QWidget*, Area> areas_;
};

// The dialog's contents in a scroll area, its bottom row of buttons under it.
class FitDialog : public QObject {
public:
    FitDialog(QDialog* d) : QObject(d), d_(d)
    {
        const QSize made = d->size();
        // the bottom row: push buttons (and a rule just above them) within 44 px of the bottom
        int rowTop = made.height();
        for (QObject* o : d->children()) {
            QWidget* w = o->isWidgetType() ? static_cast<QWidget*>(o) : nullptr;
            if (!w || w->isWindow() || w->isHidden()) continue;
            if ((qobject_cast<QPushButton*>(w) || qobject_cast<QDialogButtonBox*>(w)) && w->geometry().bottom() >= made.height() - 16 &&
                w->y() >= made.height() - 44)
                rowTop = std::min(rowTop, w->y() - 6);
        }
        std::vector<QWidget*> all;
        for (QObject* o : d->children())
            if (o->isWidgetType() && !static_cast<QWidget*>(o)->isWindow()) all.push_back(static_cast<QWidget*>(o));
        scroll_ = new QScrollArea(d);
        scroll_->setObjectName("dialogFitScroll");
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setWidgetResizable(true);
        scroll_->viewport()->setAutoFillBackground(false);
        canvas_ = new QWidget;
        canvas_->setObjectName("dialogFitCanvas");
        canvas_->setAutoFillBackground(false);
        bar_ = new QWidget(d);
        bar_->setObjectName("dialogFitButtons");
        barH_ = made.height() - rowTop;
        for (QWidget* w : all) {
            const bool hidden = w->isHidden();
            const QRect g = w->geometry();
            const bool inRow = barH_ > 0 && g.y() >= rowTop && (qobject_cast<QPushButton*>(w) || qobject_cast<QDialogButtonBox*>(w) || isLine(w) ||
                                                                g.height() <= 24);
            if (inRow) {
                w->setParent(bar_);
                w->setGeometry(g.translated(0, -rowTop));
            } else {
                w->setParent(canvas_);
                w->setGeometry(g);
            }
            if (!hidden) w->show();
        }
        canvas_->setMinimumSize(made.width(), made.height() - barH_);
        canvas_->resize(canvas_->minimumSize());
        scroll_->setWidget(canvas_);
        canvas_->show();
        // made while the dialog is being shown, so shown by hand
        scroll_->show();
        bar_->setVisible(barH_ > 0);
        made_ = made;
        // the dialog: resizable, no larger than the screen
        d->setMinimumSize(std::min(made.width(), 240), std::min(made.height(), 160));
        d->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        d->setSizeGripEnabled(true);
        QSize s = made;
        if (QScreen* sc = d->screen()) {
            const QRect av = sc->availableGeometry();
            s = s.boundedTo(QSize(av.width() - 16, av.height() - 48));
        }
        d->resize(s);
        place();
        d->installEventFilter(this);
        // once the dialog is up and has laid itself out (its pages sized), its sizes are the ones it was made for
        QTimer::singleShot(0, this, [this] { start(); });
    }

    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == d_ && e->type() == QEvent::Resize) place();
        return false;
    }

private:
    void start()
    {
        if (!d_ || stretch_) return;
        stretch_ = new Stretcher(this);
        stretch_->watch(canvas_);
        rowKids_.clear();
        for (QObject* c : bar_->children())
            if (c->isWidgetType()) {
                QWidget* w = static_cast<QWidget*>(c);
                rowKids_.push_back({w, isLine(w) ? 2 : w->x() > made_.width() / 3 ? 1 : 0});
            }
        rowLast_ = bar_->width();
    }
    void place()
    {
        const QSize s = d_->size();
        scroll_->setGeometry(0, 0, s.width(), s.height() - barH_);
        bar_->setGeometry(0, s.height() - barH_, s.width(), barH_);
        if (rowLast_ >= 0) {
            const int dx = s.width() - rowLast_;
            rowLast_ = s.width();
            for (auto& k : rowKids_) {
                if (!k.first) continue;
                if (k.second == 1) k.first->move(k.first->x() + dx, k.first->y());
                if (k.second == 2) k.first->resize(std::max(8, k.first->width() + dx), k.first->height());
            }
        }
    }

    QPointer<QDialog> d_;
    QScrollArea* scroll_ = nullptr;
    QWidget* canvas_ = nullptr;
    QWidget* bar_ = nullptr;
    int barH_ = 0;
    QSize made_;
    Stretcher* stretch_ = nullptr;
    int rowLast_ = -1;
    std::vector<std::pair<QPointer<QWidget>, int>> rowKids_;
};

class FitFilter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() != QEvent::Show) return false;
        QDialog* d = qobject_cast<QDialog*>(o);
        if (!d || !d->isWindow() || d->property(kFitted).isValid()) return false;
        d->setProperty(kFitted, true);
        if (qobject_cast<QMessageBox*>(d) || qobject_cast<QFileDialog*>(d) || qobject_cast<QColorDialog*>(d) || qobject_cast<QFontDialog*>(d) ||
            qobject_cast<QInputDialog*>(d) || qobject_cast<QProgressDialog*>(d) || qobject_cast<QWizard*>(d) || d->property("renoNoFit").toBool())
            return false;
        // only dialogs placed by hand at a fixed size; one with a layout sizes itself
        if (d->layout() || d->minimumSize() != d->maximumSize() || d->width() < 120 || d->height() < 80) {
            // still no taller or wider than the screen
            if (QScreen* sc = d->screen()) {
                const QRect av = sc->availableGeometry();
                if (d->height() > av.height() - 48 || d->width() > av.width() - 16) {
                    d->setMinimumSize(0, 0);
                    d->resize(d->size().boundedTo(QSize(av.width() - 16, av.height() - 48)));
                }
            }
            return false;
        }
        new FitDialog(d);
        return false;
    }
};

}  // namespace

void installDialogFit()
{
    static bool done = false;
    if (done) return;
    done = true;
    qApp->installEventFilter(new FitFilter(qApp));
}

}  // namespace app
}  // namespace reno
