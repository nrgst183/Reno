// Options > Preferences against the VEGAS Pro 18 dialog (tests/ui/data/vegas_prefs.txt, from a capture of it): the tabs in VEGAS's
// order and names (Reno's own settings in a tab of their own at the end), and on each of VEGAS's tabs every control of the real
// dialog - kind, text, position and size - present in Reno's. Writes Reno's picture of every tab next to VEGAS's when the capture
// folder is given.
//
//   prefslayout_test <output folder> [capture folder with the VEGAS pictures]
//
// Build: uibuild prefslayout_test tests/ui/prefslayout_test.cpp (with the app objects, see tests/ui/prefsmore_test.cpp)
#include "../../src/app/editdialogs.h"
#include "../../src/app/mainwindow.h"
#include "layoutcmp.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QTextStream>

using namespace reno::app;
using uitest::check;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoPrefsLayoutTest");
    QApplication::setApplicationName("prefslayout_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("prefslayout");
    QString caps = args.size() > 2 && QFileInfo(args[2]).isDir() ? args[2] : QString();
    if (caps.isEmpty()) caps = QString::fromLocal8Bit(qgetenv("VEGAS_CAPTURE_DIR"));
    QDir().mkpath(out);
    const QString ref = QFileInfo(QString(__FILE__)).absolutePath() + "/data/vegas_prefs.txt";
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    QStringList report;
    QTimer::singleShot(400, [&] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(d && d->objectName() == "preferencesDialog", "the Preferences dialog opens");
        if (!d) return;
        QTabWidget* tabs = d->findChild<QTabWidget*>("tabs");
        const QStringList want = {"General", "Video", "Preview Device", "Audio", "Audio Device", "MIDI", "VST Effects", "Editing", "Display",
                                  "CD Settings", "Sync", "External Control && Automation", "Deprecated Features", "File I/O", "Reno"};
        QStringList have;
        for (int k = 0; tabs && k < tabs->count(); ++k) have << tabs->tabText(k);
        check(have == want, "VEGAS's 14 tabs in its order, then Reno's own", have.join(", ").toStdString());
        // Reno's tab rows are 8 px taller each than VEGAS's (easier to hit), so the dialog and everything under the tabs is 24 px lower
        const int taller = 24;
        check(d->width() == 433 && d->height() == 654 + taller, "the dialog is VEGAS's 435 x 686 with its frame (and taller tabs)",
              QString("%1x%2").arg(d->width()).arg(d->height()).toStdString());
        for (int k = 0; tabs && k < tabs->count() - 1; ++k) {
            tabs->setCurrentIndex(k);
            uitest::settle(60);
            QWidget* page = tabs->widget(k);
            const QString section = QString("tab %1 ").arg(k + 1) + tabs->tabText(k).replace("&&", "&");
            const auto wantCells = layoutcmp::readSection(ref, section);
            QWidget* area = page->findChild<QWidget*>("pageArea");
            const auto haveCells = layoutcmp::collect(area ? area : page, d, QPoint(1, 31 - taller));
            const auto res = layoutcmp::compare(wantCells, haveCells, 2);
            check(!wantCells.empty() && res.problems.isEmpty(), "tab " + tabs->tabText(k).toStdString() + ": every VEGAS control is there, in its place",
                  QString("%1 of %2: %3").arg(res.matched).arg(res.wanted).arg(res.problems.mid(0, 3).join("; ")).toStdString());
            check(res.extras.isEmpty(), "tab " + tabs->tabText(k).toStdString() + ": nothing but VEGAS's controls", res.extras.mid(0, 3).join("; ").toStdString());
            report << "== " + section << res.problems << QStringList("-- extras") << res.extras;
            const QImage shot = uitest::shot(d, QString("%1/prefs_%2.png").arg(out).arg(k + 1, 2, 10, QChar('0')));
            if (!caps.isEmpty()) {
                const QStringList pics = QDir(caps).entryList(QStringList() << QString("*Preferences*tab %1 *.png").arg(k + 1), QDir::Files);
                for (const QString& pic : pics)
                    if (!pic.contains("Browse") && !pic.contains("Advanced")) {
                        layoutcmp::sideBySide(shot, caps + "/" + pic, QString("%1/prefs_%2_vs_vegas.png").arg(out).arg(k + 1, 2, 10, QChar('0')));
                        break;
                    }
            }
        }
        QFile f(out + "/prefs_diff.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) QTextStream(&f) << report.join("\n") << "\n";
        d->reject();
    });
    runPreferences(nullptr);
    return uitest::summary();
}
