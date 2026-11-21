// VEGAS 18's Recorded Files, Restore Backup, Selectively Paste Event Attributes and Edit Details: what they show and
// what their controls do. Pictures of each go into the output folder, to set beside the VEGAS manual's
// (p0100_x07362, p0059, VEGAS 23's p0394, p0032_x04424).
//
//   dlga_vegasui_test <output folder>
//
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/recorder.h"
#include "../../src/app/vegasextras.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <algorithm>
#include <cmath>
#include <memory>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

// Runs 'fn' on the dialog called 'name' once it is up (a modal dialog's own event loop runs it).
void whenShown(const char* name, std::function<void(QDialog*)> fn)
{
    auto tries = std::make_shared<int>(0);
    auto look = std::make_shared<std::function<void()>>();
    *look = [name, fn, tries, look] {
        for (QWidget* t : QApplication::topLevelWidgets())
            if (QDialog* d = qobject_cast<QDialog*>(t))
                if (d->isVisible() && d->objectName() == name) {
                    fn(d);
                    return;
                }
        if (++*tries < 100) QTimer::singleShot(30, *look);
    };
    QTimer::singleShot(30, *look);
}

// Answers the next question box with Yes.
void answerYes()
{
    QTimer::singleShot(60, [] {
        for (QWidget* t : QApplication::topLevelWidgets())
            if (QMessageBox* m = qobject_cast<QMessageBox*>(t))
                if (m->isVisible()) {
                    m->button(QMessageBox::Yes)->click();
                    return;
                }
        answerYes();
    });
}

void writeFile(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write("RIFF");
}

QStringList headers(QTreeWidget* t)
{
    QStringList out;
    for (int c = 0; c < t->columnCount(); ++c) out << t->headerItem()->text(c);
    return out;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoDlgaTest");
    QApplication::setApplicationName("dlga_vegasui_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = QDir(args.size() > 1 ? args[1] : QString("dlgaout")).absolutePath();
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    // ------------------------------------------------------------------ Recorded Files
    {
        const QString folder = out + "/recorded";
        QDir(folder).removeRecursively();
        const QString f1 = folder + "/Voice - 1.wav", f2 = folder + "/Voice - 2.wav";
        writeFile(f1);
        writeFile(f2);
        vprefs::set("prefs/promptKeepRecorded", true);
        bool looked = false;
        whenShown("recordedFilesDialog", [&](QDialog* d) {
            looked = true;
            QLabel* where = d->findChild<QLabel*>("recordedFolder");
            QListWidget* list = d->findChild<QListWidget*>("recordedFiles");
            QPushButton* del = d->findChild<QPushButton*>("delete");
            QPushButton* ren = d->findChild<QPushButton*>("rename");
            QPushButton* delAll = d->findChild<QPushButton*>("deleteAll");
            QPushButton* done = d->findChild<QPushButton*>("done");
            QCheckBox* again = d->findChild<QCheckBox*>("showAfterRecording");
            check(where && where->toolTip() == QDir::toNativeSeparators(folder + "/") && where->text().endsWith("recorded\\"), "Recorded Files: the folder above the list",
                  where ? where->text().toStdString() : "");
            check(list && list->count() == 2 && list->item(0)->text() == "Voice - 1.wav" && list->item(1)->text() == "Voice - 2.wav",
                  "the files by name alone");
            check(del && ren && delAll && done && again, "Delete, Rename, Delete All, the check box and Done are there");
            if (!(list && del && ren && delAll && done && again)) { d->reject(); return; }
            check(del->text() == "&Delete" && ren->text() == "&Rename" && delAll->text() == "Delete &All" && done->text() == "Do&ne" &&
                      again->text() == "&Show after every recording session",
                  "VEGAS's texts (Rename without dots)");
            check(del->x() < ren->x() && ren->geometry().right() + 40 < delAll->x() && delAll->geometry().right() > d->width() - 20,
                  "Delete and Rename on the left, Delete All at the right");
            check(again->y() > delAll->y() && done->y() > delAll->y() && done->geometry().right() > d->width() - 20 && again->x() < 20,
                  "the check box and Done on the row below");
            check(again->isChecked(), "the check box is Prompt to keep files after recording");
            uitest::shot(d, out + "/recorded_files.png");
            // Rename: the name is typed in the list
            list->item(1)->setText("Take Two");
            uitest::settle(20);
            check(QFileInfo::exists(folder + "/Take Two.wav") && !QFileInfo::exists(f2) && list->item(1)->text() == "Take Two.wav",
                  "Rename: the file is renamed (keeping its extension)");
            // Delete: the selected file goes
            list->clearSelection();
            list->item(0)->setSelected(true);
            list->setCurrentRow(0);
            answerYes();
            del->click();
            uitest::settle(20);
            check(!QFileInfo::exists(f1) && list->count() == 1, "Delete removes the selected file");
            again->setChecked(false);
            done->click();
        });
        const QList<QPair<int, QString>> keep = recordedFilesDialog({f1, f2});
        check(looked, "the Recorded Files dialog was shown");
        check(keep.size() == 1 && keep[0].first == 1 && QFileInfo(keep[0].second).fileName() == "Take Two.wav", "it returns the file kept, under its new name");
        check(!vprefs::on("prefs/promptKeepRecorded"), "the check box cleared turns off Prompt to keep files after recording");
        vprefs::set("prefs/promptKeepRecorded", true);
    }

    // ------------------------------------------------------------------ Restore Backup
    {
        const QString base = out + "/backups";
        QDir(base).removeRecursively();
        QSettings().setValue("backup/location", "folder");
        QSettings().setValue("backup/folder", base);
        st().newProject();
        writeFile(base + "/minutes/Untitled_2026-11-20_10-00-00.veg");
        writeFile(base + "/hours/welcome_2026-11-19_09-00-00.veg");
        bool looked = false;
        whenShown("restoreBackupDialog", [&](QDialog* d) {
            looked = true;
            QPushButton* all = d->findChild<QPushButton*>("allBackups");
            QPushButton* current = d->findChild<QPushButton*>("currentProjectOnly");
            QTreeWidget* list = d->findChild<QTreeWidget*>("backupList");
            QCheckBox* inst = d->findChild<QCheckBox*>("openNewInstance");
            QPushButton* cancel = d->findChild<QPushButton*>("cancel");
            QPushButton* del = d->findChild<QPushButton*>("deleteSelected");
            QPushButton* restore = d->findChild<QPushButton*>("restoreSelected");
            check(all && current && list && inst && cancel && del && restore, "Restore Backup: its controls are there");
            if (!(all && current && list && inst && cancel && del && restore)) { d->reject(); return; }
            check(all->isCheckable() && all->isChecked() && current->isCheckable() && !current->isChecked() && all->x() < current->x() &&
                      all->y() < list->y(),
                  "All Backups / Current Project Only: two buttons above the list, All Backups on");
            check(headers(list) == QStringList({"Backup Name", "Backup Type", "Time of Backup"}), "the columns: Backup Name, Backup Type, Time of Backup",
                  headers(list).join(", ").toStdString());
            check(list->topLevelItemCount() == 2, "every backup is listed", std::to_string(list->topLevelItemCount()));
            bool minor = false, major = false;
            for (int r = 0; r < list->topLevelItemCount(); ++r) {
                const QTreeWidgetItem* it = list->topLevelItem(r);
                minor = minor || (it->text(0) == "Untitled_2026-11-20_10-00-00" && it->text(1) == "Minor backup" && !it->text(2).isEmpty());
                major = major || (it->text(0) == "welcome_2026-11-19_09-00-00" && it->text(1) == "Major backup");
            }
            check(minor && major, "names without .veg, the type (minor: by minutes, major: by hours) and the time");
            check(inst->text() == "Open in new instance of Reno" && inst->y() > list->geometry().bottom() && inst->x() < 20,
                  "the new instance check box under the list at the left");
            // (where they are in the dialog: the dialog look may put a bottom row of buttons on a footer of its own)
            const QRect c = QRect(cancel->mapTo(d, QPoint()), cancel->size()), x = QRect(del->mapTo(d, QPoint()), del->size()),
                        r = QRect(restore->mapTo(d, QPoint()), restore->size());
            check(c.x() < x.x() && x.x() < r.x() && c.y() == r.y() && c.y() > inst->y() && r.right() > d->width() - 20 && cancel->text() == "Cancel" && del->text() == "Delete Selected" &&
                      restore->text() == "Restore Selected",
                  "Cancel, Delete Selected, Restore Selected at the bottom right",
                  QString("%1 %2 %3 %4").arg(c.x()).arg(r.x()).arg(c.y()).arg(d->width()).toStdString());
            uitest::shot(d, out + "/restore_backup.png");
            current->click();
            uitest::settle(20);
            check(list->topLevelItemCount() == 1 && list->topLevelItem(0)->text(0).startsWith("Untitled"), "Current Project Only: this project's backups");
            all->click();
            uitest::settle(20);
            check(list->topLevelItemCount() == 2 && !current->isChecked(), "All Backups again");
            for (int r = 0; r < list->topLevelItemCount(); ++r) list->topLevelItem(r)->setSelected(list->topLevelItem(r)->text(0).startsWith("welcome"));
            answerYes();
            del->click();
            uitest::settle(20);
            check(list->topLevelItemCount() == 1 && !QFileInfo::exists(base + "/hours/welcome_2026-11-19_09-00-00.veg"), "Delete Selected deletes the backup");
            cancel->click();
        });
        restoreBackupDialog(nullptr);
        check(looked, "the Restore Backup dialog was shown");
        QSettings().remove("backup/location");
        QSettings().remove("backup/folder");
    }

    // ------------------------------------------------------------------ Selectively Paste Event Attributes
    uitest::buildDemoProject();
    MainWindow w;
    w.resize(1400, 860);
    w.show();
    uitest::settle(300);
    {
        AppState& app = st();
        std::vector<uint64_t> video, audio;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events) (t.type == TrackType::Video ? video : audio).push_back(e.id);
        check(video.size() >= 2 && audio.size() >= 2, "set up: video and audio events");
        if (video.size() >= 2 && audio.size() >= 2) {
            const uint64_t src = video[0], dst = video[1];
            app.edit("Set Up", [&](Project& p) {
                Event* e = p.findEvent(src);
                e->fadeIn = 0.5;
                e->fadeInCurve = CurveFast;
                e->fadeOut = 0.25;
                e->fadeOutCurve = CurveSlow;
                e->mute = true;
                Event* d = p.findEvent(dst);
                d->fadeIn = 0;
                d->fadeOut = 0;
                d->fadeInCurve = d->fadeOutCurve = CurveLinear;
                d->mute = false;
            });
            app.selectOnly(src);
            app.copySelection();
            app.selectOnly(dst);
            uitest::settle(50);
            QAction* act = w.findChild<QAction*>("selectivePaste");
            check(act != nullptr, "Edit > Selectively Paste Event Attributes is there");
            bool looked = false;
            const QStringList videoItems = {"Event switches", "Video event playback rate and undersample rate", "Video event effects and effect keyframes",
                                            "Video event pan/crop settings", "Video event velocity envelopes", "Pre transition type",
                                            "Post transition type", "Pre transition length", "Post transition length"};
            whenShown("selectivePasteDialog", [&](QDialog* d) {
                looked = true;
                QLabel* text = d->findChild<QLabel*>("selectivePasteText");
                check(text && text->text().startsWith("Select the attribute(s) that you want to paste into the target event.") && text->y() < 40,
                      "the sentence at the top");
                QStringList names;
                bool anyTicked = false;
                std::vector<QCheckBox*> boxes;
                for (QCheckBox* b : d->findChildren<QCheckBox*>()) boxes.push_back(b);
                std::sort(boxes.begin(), boxes.end(), [](QCheckBox* a, QCheckBox* b) { return a->y() < b->y(); });
                for (QCheckBox* b : boxes) {
                    names << b->text();
                    anyTicked = anyTicked || b->isChecked();
                }
                check(names == videoItems, "a video event's attributes, in VEGAS's order", names.join(" | ").toStdString());
                check(!anyTicked, "none ticked to begin with");
                QPushButton* all = d->findChild<QPushButton*>("selectAll");
                QPushButton* none = d->findChild<QPushButton*>("clearAll");
                QPushButton* ok = d->findChild<QPushButton*>("ok");
                check(all && none && ok && all->x() > 300 && none->y() > all->y() && all->text() == "Select All" && none->text() == "Clear All",
                      "Select All and Clear All at the right");
                if (!(all && none && ok)) { d->reject(); return; }
                uitest::shot(d, out + "/selectively_paste.png");
                all->click();
                bool allOn = true;
                for (QCheckBox* b : boxes) allOn = allOn && b->isChecked();
                check(allOn, "Select All ticks every box");
                none->click();
                bool allOff = true;
                for (QCheckBox* b : boxes) allOff = allOff && !b->isChecked();
                check(allOff, "Clear All clears them");
                for (QCheckBox* b : boxes) b->setChecked(b->text() == "Pre transition length" || b->text() == "Post transition type");
                ok->click();
            });
            if (act) {
                act->setEnabled(true);
                act->trigger();
            }
            uitest::settle(50);
            check(looked, "the dialog was shown");
            const Event* d = app.project().findEvent(dst);
            check(d && near(d->fadeIn, 0.5) && d->fadeInCurve == CurveLinear && near(d->fadeOut, 0) && d->fadeOutCurve == CurveSlow && !d->mute,
                  "only the ticked attributes are pasted: the pre transition's length, the post transition's type");
            check(app.undoName() == "Selectively Paste Event Attributes", "in one undo step", app.undoName());
            app.undo();
            // an audio event: its pitch shift instead of the video items
            app.selectOnly(audio[0]);
            app.copySelection();
            app.selectOnly(audio[1]);
            looked = false;
            whenShown("selectivePasteDialog", [&](QDialog* dlg) {
                looked = true;
                QStringList names;
                for (QCheckBox* b : dlg->findChildren<QCheckBox*>()) names << b->text();
                check(names.contains("Audio event pitch shift") && names.contains("Event switches") && names.contains("Post transition length") &&
                          !names.contains("Video event pan/crop settings"),
                      "an audio event: Audio event pitch shift, no video items", names.join(" | ").toStdString());
                dlg->reject();
            });
            if (act) {
                act->setEnabled(true);
                act->trigger();
            }
            uitest::settle(50);
            check(looked, "... shown for an audio event");
        }
    }

    // ------------------------------------------------------------------ Edit Details
    {
        AppState& app = st();
        uitest::buildDemoProject();
        EditDetails details;
        details.resize(1500, 260);
        details.show();
        uitest::settle(80);
        QComboBox* show = details.findChild<QComboBox*>("detailsShow");
        QComboBox* preset = details.findChild<QComboBox*>("detailsPreset");
        QToolButton* save = details.findChild<QToolButton*>("detailsPresetSave");
        QToolButton* remove = details.findChild<QToolButton*>("detailsPresetDelete");
        QTreeWidget* table = details.findChild<QTreeWidget*>("detailsTable");
        check(show && preset && save && remove && table, "Edit Details: Show, the preset box, Save and Delete, the table");
        if (show && preset && save && remove && table) {
            QStringList items;
            for (int i = 0; i < show->count(); ++i) items << show->itemText(i);
            check(items == QStringList({"Audio CD Track List", "Commands", "Events", "Markers", "Regions", "Selected Events"}),
                  "Show: VEGAS's list", items.join(", ").toStdString());
            check(show->currentText() == "Events", "Events to begin with");
            check(show->x() < preset->x() && preset->x() < save->x() && save->x() < remove->x() && preset->isEditable(), "Show, Preset, Save, Delete in a row");
            const QStringList want = {"Track", "Start", "End", "Length", "Number of Takes", "Active Take Name", "Take Start", "Timecode In", "Timecode Out",
                                      "File Path", "Tape Name", "Select", "Mute", "Loop", "Lock", "Normalize", "Snap Offset"};
            check(headers(table) == want, "the Events columns", headers(table).join(", ").toStdString());
            int events = 0;
            for (const Track& t : app.project().tracks) events += (int)t.events.size();
            check(details.rows() == events, "every event");
            const uint64_t e0 = app.project().tracks[0].events[0].id;
            const QString eid = QString("e%1").arg(e0);
            QTreeWidgetItem* row = nullptr;
            for (int r = 0; r < table->topLevelItemCount(); ++r)
                if (table->topLevelItem(r)->data(0, Qt::UserRole).toString() == eid) row = table->topLevelItem(r);
            check(row && row->text(0) == "1" && row->text(4) == "1" && row->checkState(13) == Qt::Checked && row->checkState(12) == Qt::Unchecked,
                  "Track is the number alone, one take, Loop ticked, Mute not", row ? (row->text(0) + " " + row->text(4)).toStdString() : "");
            uitest::shot(&details, out + "/edit_details.png");
            auto rowOf = [&](const QString& id) -> QTreeWidgetItem* {
                for (int r = 0; r < table->topLevelItemCount(); ++r)
                    if (table->topLevelItem(r)->data(0, Qt::UserRole).toString() == id) return table->topLevelItem(r);
                return nullptr;
            };
            auto tick = [&](int column, bool on) {
                if (QTreeWidgetItem* it = rowOf(eid)) it->setCheckState(column, on ? Qt::Checked : Qt::Unchecked);
                uitest::settle(40);
            };
            tick(12, true);
            check(app.project().findEvent(e0)->mute, "Mute ticked mutes the event");
            tick(13, false);
            check(!app.project().findEvent(e0)->loop, "Loop cleared");
            tick(14, true);
            check(app.project().findEvent(e0)->locked, "Lock ticked locks it");
            app.setSelection({});
            uitest::settle(40);
            tick(11, true);
            check(app.selection().count(e0) == 1, "Select ticked selects it");
            check(rowOf(eid) && rowOf(eid)->checkState(11) == Qt::Checked, "... and stays ticked");
            if (QTreeWidgetItem* it = rowOf(eid)) it->setText(16, "00:00:01:00");
            uitest::settle(40);
            check(near(app.project().findEvent(e0)->snapOffset, 1.0), "Snap Offset typed in", std::to_string(app.project().findEvent(e0)->snapOffset));
            if (QTreeWidgetItem* it = rowOf(eid)) it->setText(6, "00:00:02:00");
            uitest::settle(40);
            check(near(app.project().findEvent(e0)->offset, 2.0), "Take Start typed in");
            if (QTreeWidgetItem* it = rowOf(eid)) it->setText(5, "Opening");
            uitest::settle(40);
            check(app.project().findEvent(e0)->name == "Opening" && rowOf(eid) && rowOf(eid)->text(5) == "Opening", "Active Take Name renames it");
            // a video event cannot be normalized: the box goes back
            tick(15, true);
            check(!app.project().findEvent(e0)->normalize && rowOf(eid) && rowOf(eid)->checkState(15) == Qt::Unchecked, "Normalize stays off for video");
            uitest::shot(&details, out + "/edit_details_changed.png");
            // presets: the view by name
            table->header()->resizeSection(5, 240);
            preset->setEditText("Wide names");
            save->click();
            uitest::settle(20);
            check(preset->findText("Wide names") >= 0 && QSettings().contains("editDetails/presets/Wide names/show"), "Save keeps the view under its name");
            details.setShow(EditDetails::Markers);
            table->header()->resizeSection(0, 100);
            preset->setCurrentIndex(preset->findText("Wide names"));
            emit preset->activated(preset->findText("Wide names"));
            uitest::settle(20);
            check(show->currentText() == "Events" && table->header()->sectionSize(5) == 240, "choosing the preset brings the view back");
            remove->click();
            uitest::settle(20);
            check(preset->findText("Wide names") < 0 && !QSettings().contains("editDetails/presets/Wide names/show"), "Delete removes it");
            details.setShow(EditDetails::SelectedEvents);
            check(details.rows() == (int)app.selection().size(), "Selected Events");
        }
    }
    return uitest::summary();
}
