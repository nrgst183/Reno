// Project Media's Media Tags panel (see panels_storyboard.h).
#include "panels_storyboard.h"
#include "panels_internal.h"
#include "strconv.h"
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <map>

namespace reno {
namespace app {

namespace {

const int kChipH = 18;

// The 1 .. 9 of Ctrl+1 .. Ctrl+9 (with Shift too, whatever the keyboard makes of Shift+digit); 0 for other keys.
int digitOf(const QKeyEvent* e)
{
    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9) return e->key() - Qt::Key_0;
    const quint32 vk = e->nativeVirtualKey();
    if (vk >= 0x31 && vk <= 0x39) return (int)(vk - 0x30);
    static const int shifted[] = {Qt::Key_Exclam, Qt::Key_At, Qt::Key_NumberSign, Qt::Key_Dollar, Qt::Key_Percent,
                                  Qt::Key_AsciiCircum, Qt::Key_Ampersand, Qt::Key_Asterisk, Qt::Key_ParenLeft};
    for (int k = 0; k < 9; ++k)
        if (e->key() == shifted[k]) return k + 1;
    return 0;
}

struct Chip {
    QString tag;
    bool partial = false;
    QRect rect, close;
};

// The chips of the Add tags box, with the box's text field after them.
class ChipArea : public QWidget {
public:
    explicit ChipArea(QWidget* parent) : QWidget(parent)
    {
        setObjectName("mediaTagChips");
        setMinimumHeight(kChipH + 8);
        edit = new QLineEdit(this);
        edit->setObjectName("addTags");
        edit->setFrame(false);
        QPalette pal = edit->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        edit->setPalette(pal);
        QFont f = edit->font();
        f.setItalic(true);
        edit->setFont(f);
    }
    QLineEdit* edit = nullptr;
    std::vector<Chip> chips;
    std::function<void(const QString&)> onRemove;

    void place()
    {
        QFontMetrics fm(font());
        int x = 4;
        const int y = (height() - kChipH) / 2;
        for (Chip& c : chips) {
            const int w = fm.horizontalAdvance(c.tag) + 10 + 14;
            c.rect = QRect(x, y, w, kChipH);
            c.close = QRect(c.rect.right() - 14, y + 3, 12, 12);
            x += w + 4;
        }
        edit->setGeometry(x, (height() - edit->sizeHint().height()) / 2, std::max(60, width() - x - 4), edit->sizeHint().height());
        update();
    }

protected:
    void resizeEvent(QResizeEvent*) override { place(); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (const Chip& c : chips) {
            QPainterPath path;
            path.addRoundedRect(QRectF(c.rect).adjusted(0.5, 0.5, -0.5, -0.5), kChipH / 2.0, kChipH / 2.0);
            p.setPen(QPen(QColor(150, 150, 150), 1));
            if (c.partial) {
                // only some of the selected media have it: the chip is filled at its left only
                p.setBrush(Qt::NoBrush);
                p.save();
                p.setClipRect(QRect(c.rect.left(), c.rect.top(), c.rect.width() / 2, c.rect.height()));
                p.fillPath(path, QColor(235, 235, 235));
                p.restore();
                p.drawPath(path);
            } else {
                p.setBrush(QColor(190, 190, 190));
                p.drawPath(path);
            }
            p.setPen(c.partial ? QColor(220, 220, 220) : QColor(30, 30, 30));
            if (c.partial) {
                p.save();
                p.setClipRect(QRect(c.rect.left(), c.rect.top(), c.rect.width() / 2, c.rect.height()));
                p.setPen(QColor(30, 30, 30));
                p.drawText(c.rect.adjusted(7, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, c.tag);
                p.restore();
                p.save();
                p.setClipRect(QRect(c.rect.left() + c.rect.width() / 2, c.rect.top(), c.rect.width(), c.rect.height()));
                p.drawText(c.rect.adjusted(7, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, c.tag);
                p.restore();
            } else {
                p.drawText(c.rect.adjusted(7, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, c.tag);
            }
            // the x in a circle
            p.setPen(QPen(QColor(90, 90, 90), 1));
            p.setBrush(QColor(225, 225, 225));
            p.drawEllipse(QRectF(c.close).adjusted(0.5, 0.5, -0.5, -0.5));
            const QPointF m = QRectF(c.close).center();
            p.setPen(QPen(QColor(70, 70, 70), 1.3));
            p.drawLine(m + QPointF(-2.5, -2.5), m + QPointF(2.5, 2.5));
            p.drawLine(m + QPointF(2.5, -2.5), m + QPointF(-2.5, 2.5));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        for (const Chip& c : chips)
            if (c.close.adjusted(-2, -2, 2, 2).contains(e->position().toPoint())) {
                const QString tag = c.tag;
                if (onRemove) onRemove(tag);
                return;
            }
        edit->setFocus();
    }
};

// Ctrl+1 .. Ctrl+9 in the watched widgets (taken before any shortcut of the main window gets them).
class KeyWatch : public QObject {
public:
    KeyWatch(QObject* parent, std::function<bool(int)> fn) : QObject(parent), fn_(std::move(fn)) {}

protected:
    bool eventFilter(QObject* o, QEvent* ev) override
    {
        if (ev->type() == QEvent::ShortcutOverride || ev->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(ev);
            const Qt::KeyboardModifiers mods = k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
            const int n = mods == Qt::ControlModifier ? digitOf(k) : 0;
            if (n > 0 && !QSettings().value(QString("media/quickTag%1").arg(n)).toString().isEmpty()) {
                if (ev->type() == QEvent::ShortcutOverride) {
                    k->accept();
                    return true;
                }
                return fn_(n);
            }
        }
        return QObject::eventFilter(o, ev);
    }

private:
    std::function<bool(int)> fn_;
};

}  // namespace

struct MediaTagsPanel::Impl {
    MediaTagsPanel* owner = nullptr;
    ChipArea* chips = nullptr;
    QFrame* box = nullptr;
    QWidget* quick = nullptr;
    QLabel* quickLabels[9] = {};
    std::vector<uint64_t> media;
    KeyWatch* keys = nullptr;

    void fillChips()
    {
        const Project& p = AppState::get().project();
        std::map<std::string, int> count;
        std::vector<std::string> order;
        int n = 0;
        for (uint64_t id : media) {
            const MediaItem* m = p.findMedia(id);
            if (!m) continue;
            ++n;
            for (const std::string& t : m->tags) {
                if (!count[t]++) order.push_back(t);
            }
        }
        chips->chips.clear();
        for (const std::string& t : order) {
            Chip c;
            c.tag = qs(t);
            c.partial = count[t] < n;
            chips->chips.push_back(c);
        }
        chips->edit->setEnabled(n > 0);
        chips->edit->setPlaceholderText(n > 0 ? QString("Add tags") : QString("Select files to edit tags"));
        chips->place();
    }

    void fillQuick()
    {
        for (int k = 0; k < 9; ++k) quickLabels[k]->setText(quickTag(k + 1));
    }

    void changeTags(const QString& what, const QString& tag, bool add)
    {
        const std::string t = tag.trimmed().toStdString();
        if (t.empty() || media.empty()) return;
        const std::vector<uint64_t> ids = media;
        bool needed = false;
        for (uint64_t id : ids)
            if (const MediaItem* m = AppState::get().project().findMedia(id)) {
                const bool has = std::find(m->tags.begin(), m->tags.end(), t) != m->tags.end();
                if (has != add) needed = true;
            }
        if (!needed) return;
        AppState::get().edit(what.toStdString(), [&](Project& p) {
            for (uint64_t id : ids)
                if (MediaItem* m = p.findMedia(id)) {
                    auto at = std::find(m->tags.begin(), m->tags.end(), t);
                    if (add && at == m->tags.end()) m->tags.push_back(t);
                    else if (!add && at != m->tags.end()) m->tags.erase(at);
                }
        });
        fillChips();
    }
};

MediaTagsPanel::MediaTagsPanel(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->owner = this;
    Impl* impl = d.get();
    setObjectName("mediaTagsPanel");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 2, 4, 2);
    root->setSpacing(3);
    d->box = new QFrame(this);
    d->box->setObjectName("addTagsBox");
    d->box->setFrameShape(QFrame::NoFrame);
    auto* bl = new QVBoxLayout(d->box);
    bl->setContentsMargins(0, 0, 0, 0);
    d->chips = new ChipArea(d->box);
    bl->addWidget(d->chips);
    root->addWidget(d->box);
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(2);
    QPalette lp = line->palette();
    lp.setColor(QPalette::WindowText, QColor(120, 120, 120));
    line->setPalette(lp);
    root->addWidget(line);

    d->quick = new QWidget(this);
    d->quick->setObjectName("quickTags");
    d->quick->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* grid = new QGridLayout(d->quick);
    grid->setContentsMargins(2, 0, 2, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(0);
    for (int k = 0; k < 9; ++k) {
        const int col = k / 3, row = k % 3;
        auto* key = new QLabel(QString("Ctrl+%1").arg(k + 1), d->quick);
        d->quickLabels[k] = new QLabel(d->quick);
        d->quickLabels[k]->setObjectName(QString("quickTag%1").arg(k + 1));
        grid->addWidget(key, row, col * 2);
        grid->addWidget(d->quickLabels[k], row, col * 2 + 1);
        grid->setColumnStretch(col * 2 + 1, 1);
    }
    root->addWidget(d->quick);

    d->chips->onRemove = [impl](const QString& tag) { impl->changeTags("Remove Media Tag", tag, false); };
    QLineEdit* edit = d->chips->edit;
    QObject::connect(edit, &QLineEdit::returnPressed, this, [impl, edit] {
        const QString t = edit->text().trimmed();
        if (t.isEmpty()) return;
        impl->changeTags("Add Media Tag", t, true);
        edit->clear();
    });
    QObject::connect(d->quick, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.setObjectName("quickTagsMenu");
        menu.addAction("Edit Quick Tags...", [this] { editQuickTags(); });
        menu.exec(d->quick->mapToGlobal(pos));
    });
    // the box: Backspace when empty takes off the last tag; Ctrl+Shift+1 .. 9 gives the text to that Quick Tag
    class BoxKeys : public QObject {
    public:
        BoxKeys(QObject* parent, Impl* impl) : QObject(parent), impl_(impl) {}
    protected:
        bool eventFilter(QObject* o, QEvent* ev) override
        {
            if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::ShortcutOverride) return QObject::eventFilter(o, ev);
            auto* k = static_cast<QKeyEvent*>(ev);
            QLineEdit* edit = impl_->chips->edit;
            const Qt::KeyboardModifiers mods = k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
            const int n = digitOf(k);
            if (n > 0 && mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
                if (ev->type() == QEvent::ShortcutOverride) { k->accept(); return true; }
                if (!edit->text().trimmed().isEmpty()) {
                    setQuickTag(n, edit->text().trimmed());
                    impl_->fillQuick();
                }
                return true;
            }
            if (ev->type() == QEvent::KeyPress && k->key() == Qt::Key_Backspace && mods == Qt::NoModifier && edit->text().isEmpty() &&
                !impl_->chips->chips.empty()) {
                impl_->changeTags("Remove Media Tag", impl_->chips->chips.back().tag, false);
                return true;
            }
            return QObject::eventFilter(o, ev);
        }
    private:
        Impl* impl_;
    };
    edit->installEventFilter(new BoxKeys(this, impl));
    d->keys = new KeyWatch(this, [this](int n) { return applyQuickTag(n); });
    edit->installEventFilter(d->keys);
    d->fillQuick();
    d->fillChips();
}

MediaTagsPanel::~MediaTagsPanel() {}

void MediaTagsPanel::setMedia(const std::vector<uint64_t>& ids)
{
    d->media = ids;
    d->fillQuick();
    d->fillChips();
}

void MediaTagsPanel::refresh()
{
    std::vector<uint64_t> still;
    for (uint64_t id : d->media)
        if (AppState::get().project().findMedia(id)) still.push_back(id);
    d->media = still;
    d->fillQuick();
    d->fillChips();
}

void MediaTagsPanel::addTag(const QString& tag) { d->changeTags("Add Media Tag", tag, true); }
void MediaTagsPanel::removeTag(const QString& tag) { d->changeTags("Remove Media Tag", tag, false); }

QString MediaTagsPanel::quickTag(int n) { return n >= 1 && n <= 9 ? QSettings().value(QString("media/quickTag%1").arg(n)).toString() : QString(); }
void MediaTagsPanel::setQuickTag(int n, const QString& tag)
{
    if (n >= 1 && n <= 9) QSettings().setValue(QString("media/quickTag%1").arg(n), tag.trimmed());
}

bool MediaTagsPanel::applyQuickTag(int n)
{
    const QString t = quickTag(n);
    if (t.isEmpty() || d->media.empty()) return false;
    d->changeTags("Add Media Tag", t, true);
    return true;
}

void MediaTagsPanel::editQuickTags()
{
    QDialog dlg(window());
    dlg.setObjectName("editQuickTags");
    dlg.setWindowTitle("Edit Quick Tags");
    auto* form = new QFormLayout(&dlg);
    QLineEdit* edits[9];
    for (int k = 0; k < 9; ++k) {
        edits[k] = new QLineEdit(quickTag(k + 1), &dlg);
        edits[k]->setObjectName(QString("quickTagEdit%1").arg(k + 1));
        form->addRow(QString("Ctrl+%1:").arg(k + 1), edits[k]);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    for (int k = 0; k < 9; ++k) setQuickTag(k + 1, edits[k]->text());
    d->fillQuick();
}

void MediaTagsPanel::watchKeys(QWidget* w)
{
    if (w) w->installEventFilter(d->keys);
}

QStringList MediaTagsPanel::chips() const
{
    QStringList out;
    for (const Chip& c : d->chips->chips) out << (c.partial ? c.tag + "*" : c.tag);
    return out;
}

QRect MediaTagsPanel::chipCloseRect(int index) const
{
    return index >= 0 && (size_t)index < d->chips->chips.size() ? d->chips->chips[(size_t)index].close : QRect();
}

QWidget* MediaTagsPanel::chipArea() const { return d->chips; }

}  // namespace app
}  // namespace reno
