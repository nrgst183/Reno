#pragma once
// Starting another program: an audio editor with a file, another Reno for a nested project, shutting the computer down
// after a render. iOS starts no other programs (Qt has no QProcess there), so there it reports that it could not.
#include <QProcess>
#include <QString>
#include <QStringList>

namespace reno {
namespace app {

// Starts 'program' with 'args' and leaves it running. False if it could not be started.
inline bool startProgram(const QString& program, const QStringList& args)
{
#if QT_CONFIG(process)
    return QProcess::startDetached(program, args);
#else
    (void)program;
    (void)args;
    return false;
#endif
}

}  // namespace app
}  // namespace reno
