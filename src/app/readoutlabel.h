// A label that gets new text many times a second while playing (cursor time, frame number, fps). QLabel::setText
// makes the layouts up to the main window recompute, which was a large share of the interface thread's time. A
// settled label is fixed at its current size and takes new text of the same width without that.
#pragma once
#include <QElapsedTimer>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QLabel>
#include <QObject>
#include <QPointer>

namespace reno {
namespace app {

class ReadoutPin : public QObject {
public:
    ReadoutPin(QLabel* label, bool keepWidth) : QObject(label), label_(label), keepWidth_(keepWidth)
    {
        label->installEventFilter(this);
        parent_ = label->parentWidget();
        if (parent_) parent_->installEventFilter(this);
        loose_.start();
    }
    ~ReadoutPin() override
    {
        if (parent_) parent_->removeEventFilter(this);
    }

    static ReadoutPin* of(QLabel* label, bool keepWidth)
    {
        for (QObject* c : label->children())
            if (ReadoutPin* p = dynamic_cast<ReadoutPin*>(c)) return p;
        return new ReadoutPin(label, keepWidth);
    }

    void setText(const QString& text)
    {
        if (label_->text() == text) return;
        const int advance = advanceOf(text);
        const bool bothFull = !text.isEmpty() && !label_->text().isEmpty();
        if (pinned_ && advance == advance_ && bothFull) {
            label_->setText(text);
            return;
        }
        release();
        label_->setText(text);
        if (advance == advance_ && bothFull && loose_.elapsed() > 60 && label_->isVisible() && label_->height() > 0 && label_->parentWidget()) {
            if (keepWidth_) label_->setFixedHeight(label_->height());
            else label_->setFixedSize(label_->sizeHint().width(), label_->height());
            pinned_ = true;
            parentHeight_ = label_->parentWidget()->height();
            if (label_->parentWidget() != parent_) {
                if (parent_) parent_->removeEventFilter(this);
                parent_ = label_->parentWidget();
                parent_->installEventFilter(this);
            }
        } else if (advance != advance_ || !bothFull) {
            loose_.restart();
        }
        advance_ = advance;
    }

    void release()
    {
        if (!pinned_) return;
        pinned_ = false;
        loose_.restart();
        if (keepWidth_) {
            label_->setMinimumHeight(0);
            label_->setMaximumHeight(QWIDGETSIZE_MAX);
        } else {
            label_->setMinimumSize(0, 0);
            label_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == label_) {
            if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange || e->type() == QEvent::ApplicationFontChange || e->type() == QEvent::ParentChange)
                release();
        } else if (pinned_ && e->type() == QEvent::Resize && parent_ && parent_->height() != parentHeight_) {
            release();
        }
        return false;
    }

private:
    // Text width. In a font with equal-width digits (nearly all), a time or number is as wide as the same text with
    // every digit 0, so that width is cached instead of measuring each new text (shaping was most of a call's cost).
    int advanceOf(const QString& text)
    {
        const QFont& f = label_->font();
        if (!haveFont_ || f != font_) {
            font_ = f;
            haveFont_ = true;
            known_.clear();
            const QFontMetrics fm(f);
            const int zero = fm.horizontalAdvance(QChar('0'));
            tabular_ = true;
            for (char c = '1'; c <= '9'; ++c)
                if (fm.horizontalAdvance(QChar(c)) != zero) tabular_ = false;
        }
        if (!tabular_) return QFontMetrics(f).horizontalAdvance(text);
        QString key = text;
        for (QChar& c : key)
            if (c >= QChar('0') && c <= QChar('9')) c = QChar('0');
        auto it = known_.constFind(key);
        if (it != known_.constEnd()) return it.value();
        if (known_.size() > 64) known_.clear();
        const int w = QFontMetrics(f).horizontalAdvance(key);
        known_.insert(key, w);
        return w;
    }

    QLabel* label_;
    QPointer<QWidget> parent_;
    bool keepWidth_;
    bool pinned_ = false;
    int advance_ = -1, parentHeight_ = 0;
    QElapsedTimer loose_;
    QFont font_;
    bool haveFont_ = false, tabular_ = false;
    QHash<QString, int> known_;
};

inline void setReadoutText(QLabel* label, const QString& text, bool keepWidth = false) { ReadoutPin::of(label, keepWidth)->setText(text); }
inline void releaseReadout(QLabel* label)
{
    if (!label) return;
    for (QObject* c : label->children())
        if (ReadoutPin* p = dynamic_cast<ReadoutPin*>(c)) p->release();
}

}  // namespace app
}  // namespace reno
