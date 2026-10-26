// Helpers shared by the panels_*.cpp files.
//
// No moc (the Windows build doesn't run it): connections are lambdas, widgets subclass virtuals.
#pragma once
#include "panels.h"
#include "appstate.h"
#include "toolwin.h"
#include "../media/media.h"
#include <QColor>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>

namespace reno {
namespace app {

using tw::Alive;
using tw::makeAlive;
using ListenerHandle = tw::Listener;

inline QString fpsText(double fps)
{
    QString t = QString::number(fps, 'f', 3);
    if (t.contains('.')) {
        while (t.endsWith('0')) t.chop(1);
        if (t.endsWith('.')) t.chop(1);
    }
    return t;
}

inline QColor mutedText(const QWidget* w) { return w->palette().color(QPalette::Disabled, QPalette::WindowText); }

// "*.mp4" ... for a kind of media (media.h's lists)
inline QStringList mediaGlobs(MediaKind kind)
{
    QStringList l;
    for (const std::string& e : mediaExtensions(kind)) l << "*." + QString::fromStdString(e);
    return l;
}

class Playback;

extern Playback* gPlayback;

}  // namespace app
}  // namespace reno
