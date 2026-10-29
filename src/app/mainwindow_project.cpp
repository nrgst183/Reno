// Opening, saving and closing projects: File > Open / Save / Save Version / Save As, recovery, Copy Media With Project.
#include "audioproxyui.h"
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "vprefs.h"
#include "prefsapply.h"
#include "vegasextras.h"
#include "man2dialogs.h"
#include "outputcmds.h"
#include "startup.h"
#include "recovery.h"
#include "timeline.h"
#include "fxwindow.h"
#include "mixer.h"
#include "editdialogs.h"
#include "mainwindow_tools.h"
#include "keyboard.h"
#include "trackdefaults.h"
#include "smalldialogs.h"
#include <QGuiApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QCheckBox>
#include <QGridLayout>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <set>

namespace reno {
namespace app {

QString MainWindow::projectFolder() const
{
    const std::string p = AppState::get().path();
    if (!p.empty()) return QFileInfo(QString::fromStdString(p)).absolutePath();
    return QSettings().value("lastProjectFolder").toString();
}

// Puts a project file at the top of File's recent projects list.
static void rememberProject(const QString& path)
{
    QSettings q;
    QStringList recent = q.value("recentProjects").toStringList();
    const QString abs = QFileInfo(path).absoluteFilePath();
    recent.removeAll(abs);
    recent.prepend(abs);
    while (recent.size() > 9) recent.removeLast();
    q.setValue("recentProjects", recent);
    q.setValue("lastProjectFolder", QFileInfo(abs).absolutePath());
}

// A window with a list of lines that stays until closed (what Reno cannot use of a project, what a save left out).
static void showLines(QWidget* parent, const char* objectName, const QString& title, const QString& head, const std::vector<std::string>& lines)
{
    QDialog* d = new QDialog(parent);
    d->setObjectName(objectName);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(title);
    d->resize(640, 380);
    auto* lay = new QVBoxLayout(d);
    auto* top = new QLabel(head, d);
    top->setWordWrap(true);
    lay->addWidget(top);
    auto* text = new QPlainTextEdit(d);
    text->setReadOnly(true);
    QStringList l;
    for (const std::string& n : lines) l << QString::fromStdString(n);
    text->setPlainText(l.join("\n"));
    lay->addWidget(text, 1);
    auto* ok = new QPushButton("OK", d);
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(ok);
    lay->addLayout(row);
    QObject::connect(ok, &QPushButton::clicked, d, [d] { d->close(); });
    d->show();
}

// Preferences > General, "Save active prerenders on project close" cleared: the prerendered video files are deleted when
// a project closes.
void dropPrerenders(Playback* pb)
{
    if (pb && !vprefs::on("prefs/savePrerenders")) pb->clearPrerendered(true);
}

// Opens a project. A VEGAS project (.veg) opens as it is and Save writes back to it. Older .reno files open too.
bool MainWindow::openProject(const QString& path)
{
    playback_->pause();
    AppState& app = AppState::get();
    std::string err;
    // A media file (File > Open, command line, a file dropped on reno.exe) goes into the open project on the timeline at
    // the cursor, as VEGAS does. A file that is not a project is tried as media too, below.
    auto openAsMedia = [&]() {
        const uint64_t id = app.importMedia(path.toStdString(), &err);
        if (!id) {
            QMessageBox::warning(this, "Open", QString("%1\n\n%2").arg(path, QString::fromStdString(err.empty() ? "It is not a project or a media file Reno can read." : err)));
            return false;
        }
        std::set<uint64_t> made;
        for (uint64_t e : app.addMediaToTimeline(id, app.cursor(), app.currentTrack)) made.insert(e);
        if (!made.empty()) app.setSelection(made);
        app.status("Opened " + path.toStdString() + " at the cursor.");
        return true;
    };
    if (!isProjectFile(path.toStdString()) && hasMediaExtension(path.toStdString())) return openAsMedia();
    // the plug-ins it uses are made as it opens, with names and progress shown if slow (startup.h)
    std::unique_ptr<PluginLoadProgress> loading(new PluginLoadProgress(this, "Opening " + QFileInfo(path).fileName()));
    const bool ok = app.open(path.toStdString(), &err);
    loading.reset();
    if (!ok) {
        std::string projectErr = err;
        err.clear();
        if (!isProjectFile(path.toStdString()) && openMedia(path.toStdString())) return openAsMedia();
        QMessageBox::warning(this, "Open Project", QString("%1\n\n%2").arg(path, QString::fromStdString(projectErr)));
        return false;
    }
    dropPrerenders(playback_.get());
    if (vprefs::on("prefs/warnMissingPlugins")) {
        const std::vector<std::string> missing = prefsapply::missingPlugins(app.project());
        if (!missing.empty()) {
            QStringList names;
            for (const std::string& n : missing) names << QString::fromStdString(n);
            app.status(strfmt("%d plug-in%s this project uses %s not installed: %s", (int)missing.size(), missing.size() == 1 ? "" : "s", missing.size() == 1 ? "is" : "are",
                              names.join(", ").toStdString().c_str()));
            if (QGuiApplication::platformName() != "offscreen")
                QMessageBox::information(this, "Open Project", QString("These plug-ins are used by %1 but are not installed or did not load:\n\n%2\n\nTheir effects are left out.")
                                                                  .arg(QFileInfo(path).fileName(), names.join("\n")));
        }
    }
    rememberProject(path);
    timeline_->zoomToFit();
    // VEGAS's "could not be found in the specified location" dialog for each missing file (not in headless test runs)
    if (QGuiApplication::platformName() != "offscreen") resolveMissingMedia(this);
    const std::vector<std::string>& notes = app.openNotes();
    app.status("Opened " + path.toStdString() + (notes.empty() ? std::string() : strfmt(" (%d things Reno shows differently from VEGAS)", (int)notes.size())));
    if (!notes.empty())
        showLines(this, "importNotes", "Opened with Differences",
                  QString("%1 is open. Reno cannot use everything in it; what it cannot use is kept in the project and saved back "
                          "as it was:").arg(QFileInfo(path).fileName()),
                  notes);
    return true;
}

// Save As's "Copy media with project": every media file the project uses (images, sound, video, nested projects; not
// generated media) is copied into 'folder' and the project made to use the copies. One undo step. A file already in the
// folder stays; a different file of the same name gets a number added to the copy's name. False if a copy could not be
// made (nothing changes then).
bool MainWindow::copyMediaWithProject(const QString& folder)
{
    AppState& app = AppState::get();
    const QDir dir(folder);
    std::map<std::string, std::string> moved;
    std::vector<std::pair<QString, QString>> todo;
    for (const MediaItem& m : app.project().media) {
        if (m.generated || m.path.empty() || m.path.compare(0, 6, "synth:") == 0 || moved.count(m.path)) continue;
        const QFileInfo src(QString::fromStdString(m.path));
        if (!src.exists()) continue;
        if (QFileInfo(src.absolutePath()) == QFileInfo(dir.absolutePath())) continue;
        QString target = dir.filePath(src.fileName());
        for (int n = 2; QFileInfo::exists(target); ++n) {
            const QFileInfo t(target);
            if (t.size() == src.size() && t.lastModified() >= src.lastModified()) break;
            target = dir.filePath(QString("%1 (%2)%3").arg(src.completeBaseName()).arg(n).arg(src.suffix().isEmpty() ? QString() : "." + src.suffix()));
        }
        moved[m.path] = target.toStdString();
        todo.push_back({src.absoluteFilePath(), target});
    }
    if (moved.empty()) return true;
    QProgressDialog progress("Copying media...", "Cancel", 0, (int)todo.size(), this);
    progress.setObjectName("copyMediaProgress");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    int k = 0;
    for (const auto& t : todo) {
        progress.setValue(k++);
        progress.setLabelText(QString("Copying %1...").arg(QFileInfo(t.first).fileName()));
        if (progress.wasCanceled()) return false;
        if (QFileInfo::exists(t.second)) continue;
        if (!QFile::copy(t.first, t.second)) {
            QMessageBox::warning(this, "Copy Media with Project", QString("%1 could not be copied to %2.").arg(t.first, folder));
            return false;
        }
    }
    progress.setValue((int)todo.size());
    app.edit("Copy Media with Project", [&](Project& p) {
        for (MediaItem& m : p.media) {
            auto it = moved.find(m.path);
            if (it != moved.end()) m.path = it->second;
        }
    });
    app.status(strfmt("%d media file%s copied to %s.", (int)todo.size(), todo.size() == 1 ? "" : "s", folder.toStdString().c_str()));
    return true;
}

// After a crash: VEGAS's question, and the auto-saved copy opened untitled (recovery.h). True if it was.
bool MainWindow::offerRecovery()
{
    return session::offerRecovery(this, [this](const CrashedSession& s) {
        playback_->pause();
        AppState& app = AppState::get();
        std::string err;
        std::unique_ptr<PluginLoadProgress> loading(new PluginLoadProgress(this, "Restoring the auto-saved project"));
        const bool ok = app.open(s.autoSave.toStdString(), &err);
        loading.reset();
        if (!ok) {
            QMessageBox::warning(this, "Restore Project", QString("%1\n\n%2").arg(s.autoSave, QString::fromStdString(err)));
            return false;
        }
        app.untieFromFile();
        timeline_->zoomToFit();
        app.status("Restored the auto-saved project" + (s.project.isEmpty() ? std::string() : " " + s.project.toStdString()) +
                   ": use File > Save As to give it a name.");
        return true;
    });
}

// Saves the project. A new project and Save As are saved as VEGAS Pro projects (.veg) unless a Reno project (.reno) is
// asked for; a project opened from a .reno is saved back to it.
bool MainWindow::saveProject(bool as)
{
    AppState& app = AppState::get();
    QString path = QString::fromStdString(app.path());
    if (as || path.isEmpty()) {
        const QString veg = "VEGAS Pro projects (*.veg)", reno = "Reno projects (*.reno)";
        QString selected = veg;
        QString suggestion = path.isEmpty() ? QDir(projectFolder()).filePath("Untitled.veg")
                                            : QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".veg";
        bool copyMedia = false;
        if (as) {
            // VEGAS's Save As has "Copy media with project" under the file list: Qt's own file dialog, which can take a check box
            QFileDialog dlg(this, "Save Project As", suggestion, veg + ";;" + reno);
            dlg.setObjectName("saveAsDialog");
            dlg.setAcceptMode(QFileDialog::AcceptSave);
            dlg.setOption(QFileDialog::DontUseNativeDialog, true);
            dlg.selectFile(suggestion);
            QCheckBox* copy = new QCheckBox("Copy media with project", &dlg);
            copy->setObjectName("copyMedia");
            copy->setToolTip("The media files the project uses are copied into the folder the project is saved in, and the project "
                             "uses the copies (files already there are not copied again)");
            if (QGridLayout* g = qobject_cast<QGridLayout*>(dlg.layout())) g->addWidget(copy, g->rowCount(), 0, 1, g->columnCount());
            if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty()) return false;
            path = dlg.selectedFiles().first();
            selected = dlg.selectedNameFilter();
            copyMedia = copy->isChecked();
        } else {
            path = QFileDialog::getSaveFileName(this, "Save Project", suggestion, veg + ";;" + reno, &selected);
        }
        if (path.isEmpty()) return false;
        if (QFileInfo(path).suffix().isEmpty()) path += selected == reno ? ".reno" : ".veg";
        if (copyMedia && !copyMediaWithProject(QFileInfo(path).absolutePath())) return false;
    }
    std::string err;
    if (!app.save(path.toStdString(), &err)) {
        QMessageBox::warning(this, "Save Project", QString("%1\n\n%2").arg(path, QString::fromStdString(err)));
        return false;
    }
    rememberProject(path);
    const std::vector<std::string>& lost = app.saveWarnings();
    app.status("Saved " + path.toStdString() + (lost.empty() ? std::string() : strfmt(" (%d things could not be saved in it)", (int)lost.size())));
    if (!lost.empty())
        showLines(this, "saveWarnings", "Saved with Changes",
                  QString("%1 was saved, but a VEGAS project cannot hold everything Reno has (or Reno cannot write it yet). Left out:")
                      .arg(QFileInfo(path).fileName()),
                  lost);
    return true;
}

// Saves under the next free numbered name (project_01.veg, project_02.veg...) and carries on in that file.
void MainWindow::saveVersion()
{
    AppState& app = AppState::get();
    if (app.path().empty()) { saveProject(true); return; }
    const QFileInfo fi(QString::fromStdString(app.path()));
    QString base = fi.completeBaseName();
    int n = 1;
    const QRegularExpressionMatch m = QRegularExpression("^(.*)_(\\d+)$").match(base);
    if (m.hasMatch()) {
        base = m.captured(1);
        n = m.captured(2).toInt() + 1;
    }
    QString path;
    for (;; ++n) {
        path = fi.dir().filePath(QString("%1_%2.%3").arg(base).arg(n, 2, 10, QChar('0')).arg(fi.suffix().isEmpty() ? QString("veg") : fi.suffix()));
        if (!QFileInfo::exists(path)) break;
    }
    std::string err;
    if (!app.save(path.toStdString(), &err)) {
        QMessageBox::warning(this, "Save Version", QString("%1\n\n%2").arg(path, QString::fromStdString(err)));
        return;
    }
    rememberProject(path);
    app.status("Saved " + path.toStdString());
}

bool MainWindow::maybeSave()
{
    AppState& app = AppState::get();
    if (!app.dirty()) return true;
    const QString name = app.path().empty() ? QString("Untitled") : QFileInfo(QString::fromStdString(app.path())).fileName();
    const auto r = QMessageBox::question(this, "Reno", QString("Save changes to %1?").arg(name), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return saveProject(false);
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    // Preferences > Reno > Confirmations, "Ask to save changes when leaving" cleared: nothing is asked and the changes
    // are kept in the auto-save file beside the project
    if (!vprefs::on("prefs/confirmExit") && AppState::get().dirty()) AppState::get().autoSaveNow();
    else if (!maybeSave()) { e->ignore(); return; }
    // for Preferences > General "Automatically open last project on startup"
    QSettings().setValue("prefs/projectOpenAtExit", QString::fromStdString(AppState::get().path()));
    playback_->pause();
    if (layoutSaver_) layoutSaver_->stop();
    saveWindowLayout();
    fxWindow_->close();
    stopAudioProxies();
    e->accept();
}

}
}
