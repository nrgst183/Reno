// Crash recovery, as in VEGAS: if an earlier run ended without closing and its auto-save copy (Preferences > General)
// was written after that run started, Reno asks VEGAS's question ("The last session did not complete properly. You
// can restore the autosaved version ...") and opens the copy untitled, so Save asks for a name.
//
// A crash is told from a run still going (e.g. a nested timeline's second Reno) by a record per run (<user
// data>/sessions/<pid>.ini: open project and auto-save copy) plus a lock file held while it runs. A clean exit
// removes both; a record whose lock is free belongs to a run that is gone.
#pragma once
#include <QDateTime>
#include <QString>
#include <functional>
#include <vector>

class QWidget;

namespace reno {
namespace app {

struct CrashedSession {
    QString record;
    QString project;
    QString autoSave;
    QDateTime started;
    bool recoverable = false;
};

namespace session {

// This run's record, in 'dir' (empty: <user data>/sessions). Kept current as projects are opened and saved.
void begin(const QString& dir = QString());
void end();
std::vector<CrashedSession> crashed(const QString& dir = QString());
void forget(const CrashedSession& s);
// Asks about the newest crashed run that can be recovered and on Yes calls 'restore' with its auto-save copy.
bool offerRecovery(QWidget* parent, std::function<bool(const CrashedSession&)> restore,
                   std::function<bool(const QString& question)> ask = nullptr, const QString& dir = QString());

}  // namespace session
}  // namespace app
}  // namespace reno
