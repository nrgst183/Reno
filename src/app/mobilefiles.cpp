#include "mobilefiles.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

namespace reno {
namespace app {

QString localFileFor(const QString& path)
{
#ifdef Q_OS_ANDROID
    if (!path.startsWith("content:", Qt::CaseInsensitive)) return path;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) return path;
    // the name the file has in the picker (Qt gives it for content:// files), else the last part of the address
    QString name = QFileInfo(in).fileName();
    if (name.isEmpty() || name.contains('%')) name = QUrl::fromPercentEncoding(QUrl(path).fileName().toUtf8());
    name.replace('/', '_').replace(':', '_');
    if (name.isEmpty()) name = "imported";
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Imported";
    QDir().mkpath(dir);
    const QString out = dir + "/" + name;
    if (QFileInfo(out).size() == in.size() && in.size() > 0) return out;
    QFile o(out + ".part");
    if (!o.open(QIODevice::WriteOnly)) return path;
    QByteArray buf;
    while (!(buf = in.read(1 << 20)).isEmpty())
        if (o.write(buf) != buf.size()) {
            o.remove();
            return path;
        }
    o.close();
    QFile::remove(out);
    return QFile::rename(out + ".part", out) ? out : path;
#else
    return path;
#endif
}

QStringList localFilesFor(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) out << localFileFor(p);
    return out;
}

}  // namespace app
}  // namespace reno
