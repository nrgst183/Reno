// Text conversions the window files share: std::string <-> QString (UTF-8 both ways) and file sizes in words.
#pragma once
#include <QString>
#include <string>

namespace reno {
namespace app {

inline QString qs(const std::string& s) { return QString::fromUtf8(s.data(), (qsizetype)s.size()); }
inline std::string s8(const QString& s) { return s.toStdString(); }

inline QString sizeText(qint64 bytes)
{
    if (bytes < 1024) return QString("%1 bytes").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

}  // namespace app
}  // namespace reno
