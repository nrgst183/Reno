// VEGAS Pro 18's FX Automation Chooser (manual pages 242-244). Its look is measured from the manual's picture of it: a
// dark window 429 x 200, the effect chain across the top (the picked effect's button black, a ring at each end of the
// chain), the hint on a lighter strip, the list of settings with Windows's classic check boxes, and the four buttons on
// the right.
#include "fxautomation.h"
#include "appstate.h"
#include "dialoglook.h"
#include "uifont.h"
#include "../engine/fxnames.h"
#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace reno {
namespace app {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

const int kRowH = 16;
const int kBoxX = 12;       // the check box, from the row's left
const int kTextX = 34;      // the setting's name

// One effect of the chain and its settings that can have an envelope.
struct Plugin {
    uint64_t slotId = 0;
    QString label;
    std::vector<EnvelopeChoice> params;
    std::vector<QString> names;         // as listed: the setting's name without the effect's
    std::set<std::string> ticked;       // params ticked in the dialog (EnvelopeRef::param)
};

// The rows of the list: highlight, a classic Windows check box (white, sunken edge, black tick) and the name.
class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        return QSize(QStyledItemDelegate::sizeHint(o, i).width(), kRowH);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        p->save();
        const QRect r = o.rect;
        const bool sel = o.state & QStyle::State_Selected;
        if (sel) p->fillRect(r, QColor(123, 181, 254));
        const int bx = r.x() + kBoxX, by = r.y() + 1;
        p->fillRect(bx, by, 14, 14, QColor(254, 254, 254));
        p->setPen(QColor(160, 160, 160));
        p->drawLine(bx, by, bx + 12, by);
        p->drawLine(bx, by, bx, by + 12);
        p->setPen(QColor(106, 106, 106));
        p->drawLine(bx + 1, by + 1, bx + 11, by + 1);
        p->drawLine(bx + 1, by + 1, bx + 1, by + 11);
        p->setPen(QColor(228, 228, 228));
        p->drawLine(bx + 1, by + 12, bx + 12, by + 12);
        p->drawLine(bx + 12, by + 1, bx + 12, by + 12);
        if (i.data(Qt::CheckStateRole).toInt() == Qt::Checked) {
            // Windows's 7 x 7 tick
            static const char* const kTick[7] = {"......X", ".....XX", "X...XXX", "XX.XXX.", "XXXXX..", ".XXX...", "..X...."};
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 7; ++x)
                    if (kTick[y][x] == 'X') p->fillRect(bx + 3 + x, by + 4 + y, 1, 1, Qt::black);
        }
        p->setFont(o.font);
        p->setPen(QColor(254, 254, 254));
        const QRect tr(r.x() + kTextX, r.y(), r.width() - kTextX, r.height());
        p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, i.data(Qt::DisplayRole).toString());
        if (sel && (o.state & QStyle::State_HasFocus)) {
            QPen dots(Qt::black, 1, Qt::DotLine);
            p->setPen(dots);
            p->drawRect(QRect(r.x() + kTextX - 2, r.y(), r.width() - kTextX + 1, r.height() - 1));
        }
        p->restore();
    }

    // The box ticks on a click in it, a double click on the row, or the space bar.
    bool editorEvent(QEvent* e, QAbstractItemModel* m, const QStyleOptionViewItem& o, const QModelIndex& i) override
    {
        bool flip = false;
        if (e->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            const QRect box(o.rect.x() + kBoxX, o.rect.y(), 15, o.rect.height());
            flip = me->button() == Qt::LeftButton && box.contains(me->position().toPoint());
        } else if (e->type() == QEvent::MouseButtonDblClick) {
            flip = static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton;
        } else if (e->type() == QEvent::KeyPress) {
            flip = static_cast<QKeyEvent*>(e)->key() == Qt::Key_Space;
        }
        if (!flip) return false;
        const bool on = m->data(i, Qt::CheckStateRole).toInt() == Qt::Checked;
        m->setData(i, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
        return true;
    }
};

// The strip the chain's buttons sit on: lighter than the window, a light line along its top, and a ring at each end of
// the chain joined to the buttons by short lines.
class ChainStrip : public QWidget {
public:
    std::vector<QToolButton*> buttons;
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(70, 70, 70));
        p.setPen(QColor(130, 130, 130));
        p.drawLine(0, 0, width() - 1, 0);
        if (buttons.empty()) return;
        const QColor ink(219, 219, 219);
        const int mid = 14;
        p.setPen(ink);
        int left = 4;
        for (QToolButton* b : buttons) {
            p.drawLine(left + 6, mid, b->x() - 1, mid);
            left = b->geometry().right() + 1 - 6;
        }
        const int last = buttons.back()->geometry().right() + 1;
        p.drawLine(last, mid, last + 5, mid);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(70, 70, 70));
        p.setPen(QPen(ink, 1.1));
        p.drawEllipse(QRectF(4.5, mid - 2.5, 6, 6));
        p.drawEllipse(QRectF(last + 5.5, mid - 2.5, 6, 6));
    }
};

class Chooser : public QDialog {
public:
    Chooser(QWidget* parent, uint64_t trackId) : QDialog(parent), trackId_(trackId)
    {
        setObjectName("fxAutomationChooser");
        makeDialogDark(this);
        setFont(vegasUiFont(11));
        setFixedSize(429, 200);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor(45, 45, 45));
        pal.setColor(QPalette::Base, QColor(45, 45, 45));
        for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text}) pal.setColor(r, QColor(254, 254, 254));
        setPalette(pal);
        setAutoFillBackground(true);
        load();

        strip_ = new ChainStrip(this);
        strip_->setObjectName("fxAutoChain");
        strip_->setGeometry(2, 2, 425, 28);
        group_ = new QButtonGroup(this);
        group_->setExclusive(true);
        int x = 15;
        const QFontMetrics fm(font());
        for (size_t k = 0; k < plugins_.size(); ++k) {
            QToolButton* b = new QToolButton(strip_);
            b->setObjectName("fxAutoPlugin");
            b->setProperty("slotId", QVariant::fromValue<qulonglong>(plugins_[k].slotId));
            b->setText(plugins_[k].label);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setStyleSheet("QToolButton { background: #5a5a5a; color: #dbdbdb; border: none; border-radius: 2px; padding: 0px; }"
                             "QToolButton:hover { background: #686868; }"
                             "QToolButton:checked { background: #101010; }");
            const int w = fm.horizontalAdvance(plugins_[k].label) + 7;
            b->setGeometry(x, 4, w, 20);
            x += w + 6;
            group_->addButton(b, (int)k);
            strip_->buttons.push_back(b);
        }
        QObject::connect(group_, &QButtonGroup::idClicked, this, [this](int k) { pick((size_t)k); });

        QLabel* hint = new QLabel("Select the plug-in parameters that you want to automate using track envelopes.", this);
        hint->setObjectName("fxAutoHint");
        hint->setGeometry(6, 34, 421, 13);
        hint->setStyleSheet("QLabel { background: #464646; border-top: 1px solid #828282; color: #dbdbdb; padding-left: 2px; }");

        list_ = new QListWidget(this);
        list_->setObjectName("fxAutoParams");
        list_->setGeometry(8, 62, 327, 114);
        list_->setItemDelegate(new RowDelegate(list_));
        list_->setUniformItemSizes(true);
        list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        list_->setStyleSheet("QListWidget { background: #2d2d2d; border: none; border-right: 1px solid #464646;"
                             " border-bottom: 1px solid #464646; outline: 0; }");
        QObject::connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
            if (cur_ >= plugins_.size()) return;
            const std::string param = it->data(Qt::UserRole).toString().toStdString();
            if (it->checkState() == Qt::Checked) plugins_[cur_].ticked.insert(param);
            else plugins_[cur_].ticked.erase(param);
        });

        const QString buttonLook = "QPushButton { color: black; border: 1px solid #707070; border-radius: 3px;"
                                   " background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f2f2f2, stop:0.49 #ebebeb,"
                                   " stop:0.5 #dddddd, stop:1 #cfcfcf); }"
                                   "QPushButton:hover { border-color: #3c7fb1; background: qlineargradient(x1:0, y1:0, x2:0,"
                                   " y2:1, stop:0 #eaf6fd, stop:0.49 #d9f0fc, stop:0.5 #bee6fd, stop:1 #a7d9f5); }"
                                   "QPushButton:pressed { border-color: #2c628b; background: #c4e5f6; }";
        auto button = [&](const char* name, const QString& text, int y) {
            QPushButton* b = new QPushButton(text, this);
            b->setObjectName(name);
            b->setGeometry(341, y, 80, 20);
            b->setStyleSheet(buttonLook);
            return b;
        };
        QPushButton* ok = button("fxAutoOk", "OK", 62);
        ok->setDefault(true);
        QPushButton* cancel = button("fxAutoCancel", "Cancel", 90);
        QPushButton* all = button("fxAutoSelectAll", "Select All", 122);
        QPushButton* none = button("fxAutoSelectNone", "Select None", 150);
        QObject::connect(ok, &QPushButton::clicked, this, &QDialog::accept);
        QObject::connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        QObject::connect(all, &QPushButton::clicked, this, [this] { tickAll(true); });
        QObject::connect(none, &QPushButton::clicked, this, [this] { tickAll(false); });

        if (!plugins_.empty()) {
            group_->button(0)->setChecked(true);
            pick(0);
        }
        list_->setFocus();
    }

    bool changed() const { return changed_; }

    // OK: an envelope for each setting newly ticked, none for each one cleared; all of it one undo step.
    void accept() override
    {
        AppState& app = AppState::get();
        size_t steps = 0;
        for (const Plugin& pl : plugins_)
            for (const EnvelopeChoice& c : pl.params) {
                const bool has = AppState::findEnvelope(app.project(), c.ref) != nullptr;
                const bool want = pl.ticked.count(c.ref.param) > 0;
                if (want && !has) {
                    if (app.insertEnvelope(c.ref)) ++steps;
                } else if (!want && has) {
                    app.removeEnvelope(c.ref);
                    ++steps;
                }
            }
        if (steps > 1) app.mergeUndo(std::min(steps, app.undoCount()), "FX Automation");
        changed_ = steps > 0;
        QDialog::accept();
    }

private:
    // The track's audio effects in chain order, with their settings and which have an envelope now.
    void load()
    {
        AppState& app = AppState::get();
        const Project& p = app.project();
        int number = 0;
        for (size_t k = 0; k < p.tracks.size(); ++k)
            if (p.tracks[k].id == trackId_) {
                number = (int)k + 1;
                const Track& t = p.tracks[k];
                setWindowTitle(QString("FX Automation Chooser - %1").arg(t.name.empty() ? QString("Track %1").arg(number) : qs(t.name)));
                for (const FxSlot& s : t.fx) {
                    if (s.kind != "audio") continue;
                    Plugin pl;
                    pl.slotId = s.id;
                    pl.label = qs(fxSlotLabel(s));
                    plugins_.push_back(pl);
                }
            }
        if (!number) setWindowTitle("FX Automation Chooser");
        EnvelopeOwner o;
        o.kind = EnvelopeOwner::Track;
        o.id = trackId_;
        for (const EnvelopeChoice& c : app.envelopeChoices(o)) {
            if (c.ref.kind != "fx") continue;
            for (Plugin& pl : plugins_)
                if (pl.slotId == c.ref.target) {
                    // envelopeChoices names it "<effect>: <setting>"; the list shows the setting alone
                    const std::string prefix = pl.label.toStdString() + ": ";
                    pl.params.push_back(c);
                    pl.names.push_back(qs(c.name.compare(0, prefix.size(), prefix) == 0 ? c.name.substr(prefix.size()) : c.name));
                    if (AppState::findEnvelope(p, c.ref)) pl.ticked.insert(c.ref.param);
                }
        }
    }

    void pick(size_t k)
    {
        if (k >= plugins_.size()) return;
        cur_ = k;
        QSignalBlocker block(list_);
        list_->clear();
        const Plugin& pl = plugins_[k];
        for (size_t i = 0; i < pl.params.size(); ++i) {
            QListWidgetItem* it = new QListWidgetItem(pl.names[i], list_);
            it->setData(Qt::UserRole, qs(pl.params[i].ref.param));
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            it->setCheckState(pl.ticked.count(pl.params[i].ref.param) ? Qt::Checked : Qt::Unchecked);
        }
        if (list_->count()) list_->setCurrentRow(0);
    }

    void tickAll(bool on)
    {
        for (int i = 0; i < list_->count(); ++i) list_->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }

    uint64_t trackId_;
    std::vector<Plugin> plugins_;
    size_t cur_ = 0;
    ChainStrip* strip_ = nullptr;
    QButtonGroup* group_ = nullptr;
    QListWidget* list_ = nullptr;
    bool changed_ = false;
};

}  // namespace

QDialog* makeFxAutomationChooser(QWidget* parent, uint64_t trackId) { return new Chooser(parent, trackId); }

bool runFxAutomationChooser(QWidget* parent, uint64_t trackId)
{
    Chooser* d = new Chooser(parent, trackId);
    d->exec();
    const bool changed = d->result() == QDialog::Accepted && d->changed();
    delete d;
    return changed;
}

}  // namespace app
}  // namespace reno
