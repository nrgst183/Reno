// Small dialogs against the VEGAS Pro 18 ones (tests/ui/data/vegas_dialogs.txt, from a capture of them): each is opened from its
// menu command, its size must be VEGAS's (window size less the frame) and every control of the real dialog - kind, text, position,
// size - must be in Reno's; nothing else may be in it. Writes Reno's picture next to VEGAS's when the capture folder is given.
//
//   dialogs_test <output folder> [capture folder with the VEGAS pictures]
#include "../../src/app/mainwindow.h"
#include "layoutcmp.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTextStream>
#include <QTabBar>
#include <QTimer>
#include "../../src/app/editdialogs.h"
#include "../../src/app/keyboard.h"

using namespace reno::app;
using uitest::check;

namespace {

QString key(QString t)
{
    QString k;
    for (QChar c : t)
        if (c.isLetterOrNumber()) k += c.toLower();
    return k;
}

QAction* find(QMenu* m, const QStringList& path, int at)
{
    for (QAction* a : m->actions()) {
        if (a->isSeparator() || key(a->text()) != key(path[at])) continue;
        if (at + 1 == path.size()) return a;
        if (a->menu()) {
            emit a->menu()->aboutToShow();
            if (QAction* r = find(a->menu(), path, at + 1)) return r;
        }
    }
    return nullptr;
}

QAction* item(MainWindow& w, const QString& path)
{
    const QStringList p = path.split('/');
    for (QAction* top : w.menuBar()->actions())
        if (top->menu() && key(top->text()) == key(p[0])) {
            emit top->menu()->aboutToShow();
            return find(top->menu(), p, 1);
        }
    return nullptr;
}

struct Case {
    QString menu, section, title, picture;
    int tab = -1;               // the tab to show first (the tab bar with this object name), if any
    QString tabBar;
};

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoDialogsTest");
    QApplication::setApplicationName("dialogs_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("dialogs");
    QString caps = args.size() > 2 && QFileInfo(args[2]).isDir() ? args[2] : QString();
    if (caps.isEmpty()) caps = QString::fromLocal8Bit(qgetenv("VEGAS_CAPTURE_DIR"));
    QDir().mkpath(out);
    const QString ref = QFileInfo(QString(__FILE__)).absolutePath() + "/data/vegas_dialogs.txt";
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState::get().newProject();
    MainWindow w;
    w.resize(1366, 760);
    w.show();
    uitest::settle(300);

    const std::vector<Case> cases = {
        {"Insert/Time...", "Insert Time", "Insert Time", "*Insert _ Time*.png"},
        {"View/Window Layouts/Save Layout As...", "Save Layout As", "Save Layout As", "*Save Layout As___ _Save Layout As_.png"},
        {"View/Window Layouts/Organize Layouts...", "Organize Layouts", "Organize Layouts", "*Organize Layouts___ _Organize Layouts_.png"},
        {"Options/Customize Timeline Toolbar...", "Customize Toolbar", "Customize Timeline Toolbar", "*Customize Timeline Toolbar*.png"},
        {"Options/Customize Keyboard...", "Customize Keyboard", "Customize Keyboard", "*Customize Keyboard___ _Customize Keyboard_.png"},
        {"@keymap", "Save Keyboard Map As", "Save Keyboard Map As", "*Save Keyboard Map As*.png"},
        {"@audioadvanced", "Advanced Audio Configuration", "Advanced Audio Configuration", "*Advanced Audio Configuration*.png"},
        {"Help/About Reno", "About General", "About Reno", "Help _ About*tab 1 General*.png", 0, "aboutTabs"},
        {"Help/About Reno", "About Computer", "About Reno", "Help _ About*tab 2 Computer*.png", 1, "aboutTabs"},
        {"Help/About Reno", "About Components", "About Reno", "Help _ About*tab 3 Components*.png", 2, "aboutTabs"},
        {"Help/About Reno", "About Multimedia Devices", "About Reno", "Help _ About*tab 4 Multimedia*.png", 3, "aboutTabs"},
        {"Help/About Reno", "About Copyrights and Patents", "About Reno", "Help _ About*tab 5 Copyrights*.png", 4, "aboutTabs"},
        {"Insert/Slideshow...", "Slideshow Pictures", "Slideshow Creator", "*Slideshow___ _ tab 1 Pictures*.png", 0, "slideshowTabs"},
        {"Insert/Slideshow...", "Slideshow Settings", "Slideshow Creator", "*Slideshow___ _ tab 2 Settings*.png", 1, "slideshowTabs"},
        {"Tools/Advanced Save...", "Advanced Save", "VEGAS Advanced Save Utility", "*Advanced Save___*.png"},
    };
    QStringList report;
    for (const Case& c : cases) {
        const QString name = c.section;
        const auto want = layoutcmp::readSection(ref, name);
        // the size is in the section heading: "## Insert Time (281x105)"
        QSize size;
        {
            QFile f(ref);
            if (f.open(QIODevice::ReadOnly))
                for (const QString& ln : QString::fromUtf8(f.readAll()).split('\n'))
                    if (ln.startsWith("## " + name + " (")) {
                        const QStringList wh = ln.mid(ln.indexOf('(') + 1).remove(')').split('x');
                        if (wh.size() == 2) size = QSize(wh[0].toInt(), wh[1].toInt());
                    }
        }
        QTimer::singleShot(500, [&] {
            QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(d != nullptr, name.toStdString() + ": the dialog opens");
            if (!d) return;
            if (c.tab >= 0)
                if (QTabBar* tb = d->findChild<QTabBar*>(c.tabBar)) tb->setCurrentIndex(c.tab);
            uitest::settle(50);
            check(d->windowTitle() == c.title, name.toStdString() + ": its title", d->windowTitle().toStdString());
            check(d->width() == size.width() - 2 && d->height() == size.height() - 32, name.toStdString() + ": VEGAS's size less the frame",
                  QString("%1x%2 want %3x%4").arg(d->width()).arg(d->height()).arg(size.width() - 2).arg(size.height() - 32).toStdString());
            const auto have = layoutcmp::collect(d, d, QPoint(1, 31));
            const auto res = layoutcmp::compare(want, have, 2);
            check(res.problems.isEmpty(), name.toStdString() + ": every VEGAS control is there, in its place",
                  QString("%1 of %2: %3").arg(res.matched).arg(res.wanted).arg(res.problems.mid(0, 4).join("; ")).toStdString());
            check(res.extras.isEmpty(), name.toStdString() + ": nothing but VEGAS's controls", res.extras.mid(0, 4).join("; ").toStdString());
            report << "== " + name << res.problems << QStringList("-- extras") << res.extras;
            const QString png = QString("%1/%2.png").arg(out).arg(key(name));
            const QImage shot = uitest::shot(d, png);
            if (!caps.isEmpty()) {
                const QStringList pics = QDir(caps).entryList(QStringList() << c.picture, QDir::Files);
                if (!pics.isEmpty()) layoutcmp::sideBySide(shot, caps + "/" + pics[0], QString("%1/%2_vs_vegas.png").arg(out).arg(key(name)));
            }
            d->reject();
        });
        if (c.menu == "@keymap") {
            askKeyboardMapName(&w, QString());
        } else if (c.menu == "@audioadvanced") {
            advancedAudioConfiguration(&w, "Speakers", "test-device");
        } else {
            QAction* a = item(w, c.menu);
            check(a != nullptr, c.menu.toStdString() + ": the command is there");
            if (a) a->trigger();
        }
        uitest::settle(100);
    }
    QFile f(out + "/dialogs_diff.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) QTextStream(&f) << report.join("\n") << "\n";
    return uitest::summary();
}
