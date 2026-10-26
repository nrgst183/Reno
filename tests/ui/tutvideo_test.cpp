// The dialog for a file that could not be found (resolveMissingMedia: Search / Specify a new location / Ignore / Ignore all, as VEGAS
// words it), and Project Media's Select in Project Media List (an event's menu), as shown in the VEGAS Pro 18 beginner tutorial.
//
//   tutvideo_test <output folder>
//
// Saves missing_media.png. QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/vegasextras.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QRadioButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

static const MediaItem* mediaByName(const std::string& name)
{
    for (const MediaItem& m : st().project().media)
        if (m.name == name) return &m;
    return nullptr;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTutVideoTest");
    QApplication::setApplicationName("tutvideo_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("tutvideo_out");
    QDir().mkpath(out);
    uitest::setup({}, {});

    QTemporaryDir dir;
    check(dir.isValid(), "(a folder to work in)");
    // a project whose media are 'files' that are not where it says
    auto makeFile = [&](const QString& rel) {
        const QString f = dir.filePath(rel);
        QDir().mkpath(QFileInfo(f).absolutePath());
        QFile file(f);
        file.open(QIODevice::WriteOnly);
        file.write("x");
        return f;
    };
    auto missingProject = [&](const std::vector<std::string>& paths) {
        st().newProject();
        st().edit("Test Media", [&](Project& p) {
            uint64_t id = 9000;
            for (const std::string& path : paths) {
                MediaItem m;
                m.id = ++id;
                m.path = path;
                m.name = QFileInfo(QString::fromStdString(path)).fileName().toStdString();
                m.offline = true;
                p.media.push_back(m);
            }
        });
    };
    const QString found = makeFile("footage/sub/found.mp4");
    const QString chosen = makeFile("other/replacement.mp4");
    const std::string gone1 = "/no/such/folder/found.mp4", gone2 = "/no/such/folder/chosen.mp4", gone3 = "/no/such/folder/ignored.mp4";

    // ---- Search for missing file: found in a folder below the project's; the files chosen or ignored as asked
    {
        missingProject({gone1, gone2, gone3});
        // the project file's folder is where the search starts: save it there
        std::string err;
        QString asked;
        int calls = 0;
        const int n = resolveMissingMedia(nullptr, [&](const QString& path, QString* file) {
            ++calls;
            asked += path + ";";
            if (path.endsWith("found.mp4")) { return 0; }
            if (path.endsWith("chosen.mp4")) { *file = chosen; return 1; }
            return 2;
        });
        // no project file yet: the search looks in the media's own folder only, so 'found' stays offline
        check(calls == 3, "a dialog for each missing file", std::to_string(calls));
        check(n == 1 && mediaByName("chosen.mp4") && !mediaByName("chosen.mp4")->offline && mediaByName("chosen.mp4")->path == chosen.toStdString(),
              "Specify a new location or replacement file: the file chosen is used", std::to_string(n));
        check(mediaByName("ignored.mp4") && mediaByName("ignored.mp4")->offline, "Ignore missing file: it stays offline");
        check(mediaByName("found.mp4") && mediaByName("found.mp4")->offline, "(Search finds nothing where there is no such folder)");
        check(st().undoName() == "Relink Missing Media", "(one undo step)", st().undoName());
        st().undo();
        check(mediaByName("chosen.mp4")->offline, "(undone)");
    }
    // ---- Ignore all: no more questions
    {
        missingProject({gone1, gone2, gone3});
        int calls = 0;
        const int n = resolveMissingMedia(nullptr, [&](const QString&, QString*) { ++calls; return 3; });
        check(calls == 1 && n == 0, "Ignore all missing files: asked once, nothing changed", std::to_string(calls));
    }
    // ---- Search finds it next to where the media was
    {
        const QString nearF = makeFile("moved/nearF.mp4");
        // a media whose recorded path is in the same folder as another file of the name: here the path itself exists, so the search is for a file whose folder exists
        missingProject({(QFileInfo(nearF).absolutePath() + "/gone/nearF.mp4").toStdString()});
        const int n = resolveMissingMedia(nullptr, [&](const QString&, QString*) { return 0; });
        check(n == 0, "Search for missing file finds nothing that is not there", std::to_string(n));
        // the folder of the recorded path is searched below too
        QDir().mkpath(QFileInfo(nearF).absolutePath() + "/gone");
        makeFile("moved/gone/deep/nearF.mp4");
        missingProject({(QFileInfo(nearF).absolutePath() + "/gone/nearF.mp4").toStdString()});
        const int m = resolveMissingMedia(nullptr, [&](const QString&, QString*) { return 0; });
        check(m == 1 && mediaByName("nearF.mp4") && !mediaByName("nearF.mp4")->offline, "Search for missing file looks in the folders below the old one", std::to_string(m));
    }
    // ---- the dialog itself: VEGAS's words and choices (a picture too)
    {
        missingProject({gone1});
        bool seen = false;
        QTimer::singleShot(300, [&] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (w->objectName() != "missingMediaDialog" || !w->isVisible()) continue;
                seen = true;
                uitest::shot(w, out + "/missing_media.png");
                const QStringList want = {"Search for missing file", "Specify a new location or replacement file", "Recapture media",
                                          "Recapture all offline media", "Ignore missing file and leave it offline",
                                          "Ignore all missing files and leave them offline"};
                bool words = true;
                for (int i = 0; i < 6; ++i) {
                    auto* r = w->findChild<QRadioButton*>(QString("missingChoice%1").arg(i));
                    if (!r || r->text() != want[i]) words = false;
                }
                check(words, "the dialog's six choices, as VEGAS words them");
                check(w->findChild<QRadioButton*>("missingChoice0")->isChecked(), "Search for missing file is the default");
                check(!w->findChild<QRadioButton*>("missingChoice2")->isEnabled() && !w->findChild<QRadioButton*>("missingChoice3")->isEnabled(),
                      "Recapture (from tape) is there but not available");
                bool title = false;
                for (QLabel* l : w->findChildren<QLabel*>())
                    if (l->text() == "The following file could not be found in the specified location:") title = true;
                check(title, "its question");
                check(w->findChild<QLabel*>("missingPath")->text() == QString::fromStdString(gone1), "the path of the file is shown");
                w->findChild<QRadioButton*>("missingChoice4")->setChecked(true);
                QMetaObject::invokeMethod(w, "accept");
            }
        });
        const int n = resolveMissingMedia(nullptr);
        check(seen && n == 0 && mediaByName("found.mp4")->offline, "the dialog: Ignore missing file leaves it offline", std::to_string(n));
    }
    // ---- Select in Project Media List
    {
        uitest::buildDemoProject();
        MediaBin bin;
        bin.resize(500, 300);
        bin.show();
        uitest::settle(200);
        const std::vector<uint64_t> shown = bin.shownMedia();
        check(shown.size() >= 2, "(Project Media shows the demo media)", std::to_string(shown.size()));
        if (shown.size() >= 2) {
            bin.selectMedia({shown[1]});
            QTreeWidget* tree = bin.findChild<QTreeWidget*>("mediaList");
            check(tree && tree->selectedItems().size() == 1 && tree->selectedItems()[0]->data(0, Qt::UserRole).toULongLong() == shown[1],
                  "Select in Project Media List: that media selected there");
            bin.setSearch("zzzz-no-match");
            uitest::settle(100);
            bin.selectMedia({shown[0]});
            check(!bin.shownMedia().empty() && tree->selectedItems().size() == 1, "(a search that hides it is cleared)");
        }
    }
    return uitest::summary();
}
