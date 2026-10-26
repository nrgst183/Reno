// Fills a window again only when worth it. Windows showing the whole project (Edit Details...) would otherwise refill on every change,
// even hidden. request() marks a fill due; it runs once the event loop gets to it (one fill for many changes), a few times a second
// during a timeline drag, and not at all while hidden until shown. flush() fills at once if due.
#pragma once
#include "appstate.h"
#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

namespace reno {
namespace app {

class LazyRefresh : public QObject {
public:
    LazyRefresh(QWidget* w, std::function<void()> fill) : QObject(w), w_(w), fill_(std::move(fill))
    {
        w->installEventFilter(this);
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { flush(); });
    }

    // Something the window shows changed.
    void request()
    {
        pending_ = true;
        schedule();
    }

    // Fills the window now if a fill is due.
    void flush()
    {
        timer_.stop();
        if (!pending_) return;
        pending_ = false;
        if (fill_) fill_();
    }

    // The window was just filled anyway: nothing is due.
    void cancel()
    {
        timer_.stop();
        pending_ = false;
    }

    bool pending() const { return pending_; }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == w_ && e->type() == QEvent::Show && pending_) schedule();
        return false;
    }

private:
    void schedule()
    {
        if (!w_ || !w_->isVisible()) return;          // filled when shown
        const int delay = AppState::get().dragging() ? 200 : 0;
        if (timer_.isActive() && timer_.remainingTime() <= delay) return;
        timer_.start(delay);
    }

    QPointer<QWidget> w_;
    std::function<void()> fill_;
    QTimer timer_;
    bool pending_ = false;
};

}  // namespace app
}  // namespace reno
