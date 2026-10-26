// Getting work back after a crash: see recovery.h.
#include "recovery.h"
#include "appstate.h"
#include "../core/base.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QSettings>
#include <algorithm>
#include <memory>

namespace reno {
namespace app {
namespace session {

namespace {

struct Current {
    QString dir, record;
    std::unique_ptr<QLockFile> lock;
    QDateTime started;
    int listener = -1;
};
Current* cur = nullptr;

QString sessionsDir(const QString& dir)
{
    return dir.isEmpty() ? QDir(QString::fromStdString(userDataDir())).filePath("sessions") : dir;
}

QString lockOf(const QString& record) { return record.left(record.size() - 4) + ".lock"; }

void writeRecord()
{
    if (!cur) return;
    const AppState& app = AppState::get();
    QSettings r(cur->record, QSettings::IniFormat);
    r.setValue("session/pid", (qlonglong)QCoreApplication::applicationPid());
    r.setValue("session/started", cur->started.toString(Qt::ISODateWithMs));
    r.setValue("session/project", QString::fromStdString(app.path()));
    r.setValue("session/autoSave", QString::fromStdString(app.autoSavePath()));
    r.sync();
}

}  // namespace

void begin(const QString& dirIn)
{
    if (cur) return;
    const QString dir = sessionsDir(dirIn);
    QDir().mkpath(dir);
    cur = new Current();
    cur->dir = dir;
    cur->started = QDateTime::currentDateTime();
    const QString base = QDir(dir).filePath(QString("reno-%1").arg((qlonglong)QCoreApplication::applicationPid()));
    cur->record = base + ".ini";
    cur->lock.reset(new QLockFile(base + ".lock"));
    cur->lock->setStaleLockTime(0);
    cur->lock->tryLock(0);
    writeRecord();
    cur->listener = AppState::get().addListener([](int changes) {
        if (changes & kFile) writeRecord();
    });
}

void end()
{
    if (!cur) return;
    if (cur->listener >= 0) AppState::get().removeListener(cur->listener);
    QFile::remove(cur->record);
    cur->lock->unlock();
    QFile::remove(cur->lock->fileName());
    delete cur;
    cur = nullptr;
}

std::vector<CrashedSession> crashed(const QString& dirIn)
{
    std::vector<CrashedSession> out;
    const QDir dir(sessionsDir(dirIn));
    for (const QFileInfo& fi : dir.entryInfoList(QStringList() << "*.ini", QDir::Files)) {
        const QString record = fi.absoluteFilePath();
        if (cur && QFileInfo(cur->record).absoluteFilePath() == record) continue;
        QLockFile lock(lockOf(record));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) continue;
        lock.unlock();
        QSettings r(record, QSettings::IniFormat);
        CrashedSession s;
        s.record = record;
        s.project = r.value("session/project").toString();
        s.autoSave = r.value("session/autoSave").toString();
        s.started = QDateTime::fromString(r.value("session/started").toString(), Qt::ISODateWithMs);
        const QFileInfo copy(s.autoSave);
        s.recoverable = !s.autoSave.isEmpty() && copy.exists() && copy.size() > 0 &&
                        (!s.started.isValid() || copy.lastModified() >= s.started.addSecs(-1));
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const CrashedSession& a, const CrashedSession& b) { return a.started > b.started; });
    return out;
}

void forget(const CrashedSession& s)
{
    QFile::remove(s.record);
    QFile::remove(lockOf(s.record));
}

bool offerRecovery(QWidget* parent, std::function<bool(const CrashedSession&)> restore, std::function<bool(const QString&)> ask, const QString& dir)
{
    const std::vector<CrashedSession> list = crashed(dir);
    const CrashedSession* best = nullptr;
    for (const CrashedSession& s : list)
        if (s.recoverable) { best = &s; break; }
    bool restored = false;
    if (best) {
        // VEGAS's wording (vegas180.exe string table), plus which project
        QString q = "The last session did not complete properly.\n\n"
                    "You can restore the autosaved version of the project that was opened, but you should use the Save As "
                    "command to rename it immediately.\n\n";
        q += best->project.isEmpty() ? QString("Project: (untitled)\n") : QString("Project: %1\n").arg(QDir::toNativeSeparators(best->project));
        q += QString("Autosaved: %1\n\n").arg(QFileInfo(best->autoSave).lastModified().toString("yyyy-MM-dd hh:mm:ss"));
        q += "Would you like to restore the autosaved project?";
        bool yes = false;
        if (ask) yes = ask(q);
        else {
            QMessageBox box(QMessageBox::Question, QCoreApplication::applicationName().isEmpty() ? QString("Reno") : QCoreApplication::applicationName(),
                            q, QMessageBox::Yes | QMessageBox::No, parent);
            box.setObjectName("recoverProject");
            box.setDefaultButton(QMessageBox::Yes);
            yes = box.exec() == QMessageBox::Yes;
        }
        if (yes && restore) restored = restore(*best);
    }
    for (const CrashedSession& s : list) forget(s);
    return restored;
}

}  // namespace session
}  // namespace app
}  // namespace reno
