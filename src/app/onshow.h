#pragma once
// Running something when a widget is shown: the windows that fill themselves only once they are first seen (lists of
// plug-ins, Color Grading's pages) do that work there rather than when they are made.
#include <QEvent>
#include <QObject>
#include <functional>
#include <utility>

namespace reno {
namespace app {

// Calls 'fn' each time 'w' is shown.
inline void callWhenShown(QObject* w, std::function<void()> fn)
{
    struct ShowFilter : QObject {
        std::function<void()> fn;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() == QEvent::Show) fn();
            return false;
        }
    };
    auto* filter = new ShowFilter();
    filter->fn = std::move(fn);
    filter->setParent(w);
    w->installEventFilter(filter);
}

}  // namespace app
}  // namespace reno
