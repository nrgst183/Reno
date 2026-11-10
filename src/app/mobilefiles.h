// Files chosen on Android: its file dialogs give content:// addresses, which Qt can read but FFmpeg and the project reader
// cannot open by name (mobilefiles.cpp).
#pragma once
#include <QString>
#include <QStringList>

namespace reno {
namespace app {

// A file the program can open by name: on Android a content:// file is copied into the app's own "Imported" folder (once:
// a copy already there with the same size is used again) and that copy's path is returned; elsewhere, and for ordinary
// paths, the path itself.
QString localFileFor(const QString& path);
QStringList localFilesFor(const QStringList& paths);

}  // namespace app
}  // namespace reno
