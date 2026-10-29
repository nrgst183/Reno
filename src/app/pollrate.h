// Slows a polling timer down when nothing is going on, and stops it while its window is hidden. Meters, the dashboard and
// the scopes read values a dozen times a second, but they only change while the project plays, records or is edited; an
// idle Reno should not wake up 200 times a second for nothing.
//
// Make the QTimer as before (fast interval, started), then keep a PollRate on the widget:
//   new PollRate(this, timer, 33);
// and end each tick with  rate->ticked(busy)  where 'busy' is true while the thing shown is moving (playing, a level above
// zero...). After 'quiet' ticks without it the timer runs every 'slowMs' (still looking, so a change that does not tell
// AppState is caught); any AppState change, and the widget being shown, put it back to the fast interval at once.
#pragma once
#include "appstate.h"
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWidget>

namespace reno {
namespace app {

class PollRate : public QObject {
public:
    PollRate(QWidget* watch, QTimer* timer, int fastMs, int slowMs = 500, int quietTicks = 25)
        : QObject(watch), timer_(timer), fast_(fastMs), slow_(slowMs), quietTicks_(quietTicks)
    {
        watch->installEventFilter(this);
        QPointer<PollRate> self(this);
        listener_ = AppState::get().addListener([self](int) {
            if (self) self->wake();
        });
        if (!watch->isVisible()) timer_->stop();        // started by the first Show
    }
    ~PollRate() override { AppState::get().removeListener(listener_); }

    // The end of a tick: is what is shown still moving?
    void ticked(bool busy)
    {
        if (busy) {
            quiet_ = 0;
            setInterval(fast_);
        } else if (++quiet_ >= quietTicks_) {
            setInterval(slow_);
        }
    }
    // Back to the fast interval (something changed).
    void wake()
    {
        if (QThread::currentThread() != timer_->thread()) {
            QPointer<QTimer> t = timer_;
            QTimer::singleShot(0, timer_, [this, t] { if (t) wake(); });
            return;
        }
        quiet_ = 0;
        setInterval(fast_);
    }

    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() == QEvent::Hide) {
            timer_->stop();
        } else if (e->type() == QEvent::Show) {
            quiet_ = 0;
            timer_->setInterval(fast_);
            timer_->start();
        }
        return false;
    }

private:
    void setInterval(int ms)
    {
        if (timer_->interval() != ms) timer_->setInterval(ms);
    }
    QTimer* timer_;
    int fast_, slow_, quietTicks_, quiet_ = 0, listener_ = 0;
};

}  // namespace app
}  // namespace reno
