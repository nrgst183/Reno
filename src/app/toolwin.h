// Helpers shared by the tool windows (Trimmer, Loudness Meters, Plug-In Manager, Dashboard...):
// a liveness flag, a self-removing listener, string conversions and VEGAS's dark colours.
#pragma once
#include "appstate.h"
#include <QColor>
#include <QString>
#include <atomic>
#include <memory>

namespace reno {
namespace app {
namespace tw {

using Alive = std::shared_ptr<std::atomic<bool>>;
inline Alive makeAlive() { return std::make_shared<std::atomic<bool>>(true); }

struct Listener {
    int id = 0;
    explicit Listener(std::function<void(int)> fn) { id = AppState::get().addListener(std::move(fn)); }
    ~Listener() { AppState::get().removeListener(id); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
};

inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline std::string ss(const QString& s) { return s.toStdString(); }

inline QColor gap() { return QColor(35, 35, 35); }
inline QColor dark() { return QColor(45, 45, 45); }
inline QColor bar() { return QColor(55, 55, 55); }
inline QColor tool() { return QColor(70, 70, 70); }
inline QColor text() { return QColor(220, 220, 220); }
inline QColor accent() { return QColor(18, 132, 214); }

inline std::unique_ptr<MediaReader> openReader(const std::string& path, std::string* error)
{
    AppState& app = AppState::get();
    if (app.mediaOpener) return app.mediaOpener(path, error);
    return openMediaOrProject(path, error);
}

}  // namespace tw
}  // namespace app
}  // namespace reno
