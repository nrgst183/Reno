// Text logs of Preferences > File I/O (Log Plugin Loading, Log Reads, Log Renders): Off, Basic or Detailed. Each is a file in the Logs
// folder of the data folder (plugins.log, reads.log, renders.log), one timed line per event; past 4 MB it is moved to
// <name>.1.log. Header-only.
#pragma once
#include "vprefs.h"
#include "../core/base.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTextStream>
#include <mutex>

namespace reno {
namespace app {
namespace logbook {

enum Level { Off = 0, Basic = 1, Detailed = 2 };
enum class Kind { Plugins, Reads, Renders };

inline const char* nameOf(Kind k) { return k == Kind::Plugins ? "plugins" : k == Kind::Reads ? "reads" : "renders"; }
inline QString folder() { return QString::fromStdString(pathJoin(userDataDir(), "Logs")); }
inline QString fileOf(Kind k) { return folder() + "/" + nameOf(k) + ".log"; }

inline Level level(Kind k)
{
    const QString key = k == Kind::Plugins ? "prefs/logPlugins" : k == Kind::Reads ? "prefs/logReads" : "prefs/logRenders";
    const QString t = vprefs::text(key);
    return t == "detailed" ? Detailed : t == "basic" ? Basic : Off;
}

// Writes a line if the log level is at least 'need' (Basic lines appear in Basic and Detailed).
inline void write(Kind k, Level need, const QString& text)
{
    if (level(k) < need) return;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lk(mutex);
    QDir().mkpath(folder());
    const QString file = fileOf(k);
    if (QFileInfo(file).size() > 4 * 1024 * 1024) {
        const QString old = folder() + "/" + nameOf(k) + ".1.log";
        QFile::remove(old);
        QFile::rename(file, old);
    }
    QFile f(file);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz") << "  " << text << "\n";
}

}  // namespace logbook
}  // namespace app
}  // namespace reno
