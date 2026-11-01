// The main window next to VEGAS's: the status bar's texts ("Complete: 00:00:00" at the left, "Record Time (2 channels): hh:mm:ss"
// at the right) and a picture of Reno's window at the size of a VEGAS screenshot with the tracks minimised, written next to the
// screenshot when VEGAS_MAIN_REF names one (a picture of a project: it is not kept in the repository).
//
//   mainwin_test <output folder>
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "layoutcmp.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QSettings>
#include <QStatusBar>

using namespace reno::app;
using uitest::check;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMainWinTest");
    QApplication::setApplicationName("mainwin_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("mainwin");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    uitest::buildDemoProject();
    MainWindow w;
    w.resize(1920, 1040);
    w.show();
    uitest::settle(400);
    QLabel* left = w.statusBar()->findChild<QLabel*>("statusText");
    QLabel* right = w.statusBar()->findChild<QLabel*>("diskSpace");
    check(left && left->text() == "Complete: 00:00:00", "the status bar's left text when idle", left ? left->text().toStdString() : "");
    check(right && right->text().startsWith("Record Time (2 channels): ") && right->text().contains(':'),
          "the status bar's right text: the record time the free space allows", right ? right->text().toStdString() : "");
    if (w.timeline()) w.timeline()->minimizeAllTracks();
    uitest::settle(200);
    const QImage shot = uitest::shot(&w, out + "/main_minimised.png");
    const QString ref = QString::fromLocal8Bit(qgetenv("VEGAS_MAIN_REF"));
    if (!ref.isEmpty() && QFileInfo::exists(ref)) layoutcmp::sideBySide(shot, ref, out + "/main_minimised_vs_vegas.png");
    return uitest::summary();
}
