#pragma once
// Starting another program: an audio editor with a file, another Reno for a nested project, shutting the computer down
// after a render.
#include <QProcess>
#include <QString>
#include <QStringList>

namespace reno {
namespace app {

// Starts 'program' with 'args' and leaves it running. False if it could not be started.
inline bool startProgram(const QString& program, const QStringList& args)
{
    return QProcess::startDetached(program, args);
}

}  // namespace app
}  // namespace reno
