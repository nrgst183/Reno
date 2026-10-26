// Thumbnails and waveforms kept on disk between sessions.
// VEGAS keeps a file's waveform in a .sfk file beside it; so does Reno (timeline.cpp's loader, media/vegasfiles.h),
// falling back on its own
// cache where the folder is not writable and for waveforms it does not make VEGAS's way (more than two channels).
// Reno keeps those and the timeline thumbnails (for a nested project, rendering it with all its effects) in its data
// folder (cache/peaks,
// cache/thumbs), one file each, named by a hash of the file's path, size and modification time (for a project file also
// those of the projects
// it nests: engine.h's mediaSignature) and the request. A resaved file gets new ones; old ones are cleared oldest first
// when the folder
// passes its limit. Everything here runs on the timeline's loader thread.
#pragma once
#include "../engine/engine.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace reno {
namespace app {
namespace mediacache {

// Whether waveforms are kept as VEGAS keeps them, in .sfk peak files beside the media (read if VEGAS made them; written
// where the folder is writable, else in Reno's cache). Preferences; on by default.
inline std::atomic<bool>& vegasPeakFiles()
{
    static std::atomic<bool> on{true};
    return on;
}

inline QString folder(const char* kind)
{
    return QString::fromStdString(pathJoin(pathJoin(userDataDir(), "cache"), kind));
}

inline QString fileFor(const char* kind, const std::string& key, const char* ext)
{
    const QByteArray h = QCryptographicHash::hash(QByteArray::fromStdString(key), QCryptographicHash::Sha1).toHex();
    return folder(kind) + "/" + QString::fromLatin1(h) + ext;
}

// mediaSignature, remembered for a few seconds (asked for every thumbnail of a file)
class Signatures {
public:
    std::string get(const std::string& path)
    {
        const auto now = std::chrono::steady_clock::now();
        auto it = known_.find(path);
        if (it != known_.end() && now - it->second.at < std::chrono::seconds(5)) return it->second.sig;
        Known k;
        k.sig = mediaSignature(path);
        k.at = now;
        known_[path] = k;
        return k.sig;
    }

private:
    struct Known { std::string sig; std::chrono::steady_clock::time_point at; };
    std::map<std::string, Known> known_;
};

inline bool read(const QString& file, QByteArray& out)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    out = f.readAll();
    return true;
}

// written whole or not at all: another Reno may be reading the folder
inline void write(const QString& file, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(data);
    f.commit();
}

inline bool readImage(const QString& file, QImage& out)
{
    if (!QFileInfo::exists(file)) return false;
    QImage img;
    if (!img.load(file, "PNG")) return false;
    out = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return !out.isNull();
}

inline void writeImage(const QString& file, const QImage& img)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return;
    if (img.save(&f, "PNG", 10)) f.commit();
    else f.cancelWriting();
}

// Clears the oldest files from a cache folder once it holds more than 'most' bytes (down to three quarters).
inline void trim(const char* kind, qint64 most)
{
    QDir d(folder(kind));
    if (!d.exists()) return;
    const QFileInfoList files = d.entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo& f : files) total += f.size();
    if (total <= most) return;
    for (const QFileInfo& f : files) {
        if (total <= most * 3 / 4) break;
        total -= f.size();
        QFile::remove(f.absoluteFilePath());
    }
}

}  // namespace mediacache
}  // namespace app
}  // namespace reno
