// View > Window Layouts (src/app/mainwindow.cpp): the built-in layouts, saving and recalling by name and number, Save Layout As,
// Organize Layouts, the layout in use kept between runs, and starting in the default layout after an older kind of layout was saved.
//
//   layouts_test <output folder>
//
// Build: uibuild layouts_test tests/ui/layouts_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
#include "../../src/app/mainwindow.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QDockWidget* dock(MainWindow& w, const char* name) { return w.findChild<QDockWidget*>(name); }
bool shown(MainWindow& w, const char* name) { QDockWidget* d = dock(w, name); return d && !d->isHidden(); }

// Default layout: Project Media (tab group) left of the preview; Large Preview: Project Media closed.
bool looksDefault(MainWindow& w) { return shown(w, "dockMedia") && dock(w, "dockMedia")->geometry().right() < dock(w, "dockPreview")->geometry().left(); }
bool looksLarge(MainWindow& w) { return !shown(w, "dockMedia") && shown(w, "dockPreview") && dock(w, "dockPreview")->width() > 1300; }

std::string names()
{
    std::string s;
    for (const MainWindow::SavedLayout& l : MainWindow::savedLayoutList()) s += (s.empty() ? "" : " | ") + l.name.toStdString() + strfmt(":%d", l.key);
    return s;
}

// Runs 'fn' on the dialog that opens modally next (from within 'open').
void withDialog(std::function<void()> open, std::function<void(QDialog*)> fn)
{
    QTimer::singleShot(200, [fn] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!d) return;
        fn(d);
    });
    open();
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoLayoutsTest");
    QApplication::setApplicationName("layouts_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("layoutsout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState::get().newProject();

    // ---------------------------------------------------------------- a Reno that saved the old layout
    {
        // an older Reno's saved layout: the screenshot arrangement, saved as version 3
        MainWindow old;
        old.resize(1920, 1018);
        old.show();
        uitest::settle(200);
        old.applyLayout(MainWindow::LargePreviewLayout);
        uitest::settle(200);
        QSettings s;
        s.setValue("window/state", old.saveState(3));
        s.setValue("layouts/2", old.saveState(3));
        s.remove("windowLayouts");
    }
    {
        MainWindow w;
        w.resize(1920, 1018);
        w.show();
        uitest::settle(300);
        check(w.currentLayout() == "Default Layout" && looksDefault(w), "after an older Reno's layout, this one starts in the default layout once",
              w.currentLayout().toStdString());
        // the older Reno's numbered layouts are kept, by number
        check(names() == "Layout 2:2", "an older Reno's numbered layouts are kept", names());
        check(w.recallLayout(2) && looksLarge(w) && w.currentLayout() == "Layout 2", "and Alt+D, 2 still brings that one back");
        check(!QSettings().contains("layouts/2"), "(and the old keys are tidied away)");
        // the layout in use is kept when Reno is closed
        w.applyLayout(MainWindow::LargePreviewLayout);
        uitest::settle(200);
        w.close();
    }
    {
        MainWindow w;
        w.resize(1920, 1018);
        w.show();
        uitest::settle(300);
        check(w.currentLayout() == "Large Preview Layout" && looksLarge(w), "the layout in use when Reno closed is there when it starts again",
              w.currentLayout().toStdString());

        // ---------------------------------------------------------------- numbered layouts
        w.restoreDefaultLayout();
        uitest::settle(200);
        QAction* save4 = w.findChild<QAction*>("saveLayout4");
        QAction* load4 = w.findChild<QAction*>("loadLayout4");
        check(save4 && load4 && save4->shortcut().toString() == "Ctrl+Alt+D, 4" && load4->shortcut().toString() == "Alt+D, 4",
              "Ctrl+Alt+D, 4 saves and Alt+D, 4 recalls (VEGAS's keys)");
        if (QAction* x = w.findChild<QAction*>("showMixer")) x->trigger();
        uitest::settle(100);
        if (save4) save4->trigger();
        check(names() == "Layout 2:2 | Layout 4:4" && w.currentLayout() == "Layout 4", "Ctrl+Alt+D, 4 saves the window as \"Layout 4\"", names());
        w.restoreDefaultLayout();
        uitest::settle(200);
        check(!shown(w, "dockMixer"), "(the default layout has the Mixing Console closed)");
        if (load4) load4->trigger();
        uitest::settle(200);
        check(shown(w, "dockMixer") && w.currentLayout() == "Layout 4", "Alt+D, 4 brings it back");
        check(!w.recallLayout(7), "a number with nothing saved on it does nothing");
        // saved again under a number another layout had: that one loses the number
        w.saveLayout("Mine", 4);
        check(names() == "Layout 2:2 | Layout 4:-1 | Mine:4", "a number belongs to one layout", names());

        // ---------------------------------------------------------------- the menu
        QMenu* menu = w.findChild<QMenu*>("layoutsMenu");
        if (menu) Q_EMIT menu->aboutToShow();
        QStringList items;
        if (menu)
            for (QAction* a : menu->actions())
                if (!a->isSeparator()) items << a->text().replace('&', "") + (a->isChecked() ? "*" : "");
        check(items.join("|") == "Default Layout|Large Preview Layout|Audio Mixing Layout|Layout 2\tAlt+D, 2|Layout 4|Mine\tAlt+D, 4*|Save Layout As...|Save Layout|"
                                 "Reload Selected Layout|Organize Layouts...",
              "View > Window Layouts: the built-in layouts, the saved ones (a bullet at the one in use), the commands", items.join("|").toStdString());
        QAction* defaultLayout = w.findChild<QAction*>("defaultLayout");
        QAction* large = w.findChild<QAction*>("largePreviewLayout");
        QAction* mixing = w.findChild<QAction*>("audioMixingLayout");
        check(defaultLayout && defaultLayout->shortcut().toString() == "Alt+D, D" && large && large->shortcut().toString() == "Alt+D, V" && mixing &&
                  mixing->shortcut().toString() == "Alt+D, A",
              "the built-in layouts' keys: Alt+D, D / V / A");
        // Save Layout (the one in use, again) and Reload Selected Layout
        w.restoreDefaultLayout();
        uitest::settle(100);
        w.recallLayout(QString("Mine"));
        uitest::settle(100);
        dock(w, "dockMixer")->close();
        if (QAction* x = w.findChild<QAction*>("reloadLayout")) x->trigger();
        uitest::settle(100);
        check(shown(w, "dockMixer"), "Reload Selected Layout puts the layout in use back as it was saved");
        dock(w, "dockMixer")->close();
        if (QAction* x = w.findChild<QAction*>("saveCurrentLayout")) x->trigger();
        w.restoreDefaultLayout();
        w.recallLayout(QString("Mine"));
        uitest::settle(100);
        check(!shown(w, "dockMixer") && names() == "Layout 2:2 | Layout 4:-1 | Mine:4", "Save Layout keeps the change in the layout in use", names());

        // ---------------------------------------------------------------- Save Layout As
        withDialog([&] { if (QAction* x = w.findChild<QAction*>("saveLayoutAs")) x->trigger(); },
                   [](QDialog* d) {
                       QLineEdit* name = d->findChild<QLineEdit*>("layoutName");
                       QComboBox* key = d->findChild<QComboBox*>("layoutKey");
                       if (name) name->setText("Editing");
                       if (key) key->setCurrentIndex(key->findData(5));
                       d->accept();
                   });
        uitest::settle(100);
        check(names() == "Layout 2:2 | Layout 4:-1 | Mine:4 | Editing:5" && w.currentLayout() == "Editing", "Save Layout As: a name and a number", names());

        // ---------------------------------------------------------------- Organize Layouts
        withDialog([&] { if (QAction* x = w.findChild<QAction*>("organizeLayouts")) x->trigger(); },
                   [](QDialog* d) {
                       QTreeWidget* list = d->findChild<QTreeWidget*>("layoutList");
                       QComboBox* key = d->findChild<QComboBox*>("layoutKey");
                       QPushButton* del = d->findChild<QPushButton*>("layoutDelete");
                       if (!list || !key || !del) { d->reject(); return; }
                       // "Layout 4" goes; "Editing" moves to 9
                       for (int k = 0; k < list->topLevelItemCount(); ++k)
                           if (list->topLevelItem(k)->text(0) == "Layout 4") list->setCurrentItem(list->topLevelItem(k));
                       del->click();
                       for (int k = 0; k < list->topLevelItemCount(); ++k)
                           if (list->topLevelItem(k)->text(0) == "Editing") list->setCurrentItem(list->topLevelItem(k));
                       key->setCurrentIndex(key->findData(9));
                       d->accept();
                   });
        uitest::settle(100);
        check(names() == "Layout 2:2 | Mine:4 | Editing:9", "Organize Layouts: delete one, give another a new number", names());
        uitest::shot(&w, out + "/layouts_window.png");
        w.restoreDefaultLayout();
        uitest::settle(100);
        w.close();
    }
    {
        MainWindow w;
        w.resize(1366, 728);
        w.show();
        uitest::settle(300);
        check(names() == "Layout 2:2 | Mine:4 | Editing:9" && w.currentLayout() == "Default Layout" && looksDefault(w),
              "the saved layouts and the one in use are there next time", names());
        QDockWidget* master = dock(w, "dockMaster");
        check(master && master->width() < 200 && master->geometry().right() > w.width() - 10, "at 1366x728 the Master Bus is still narrow at the right",
              strfmt("%d", master ? master->width() : 0));
        uitest::shot(&w, out + "/layouts_small.png");
    }
    return uitest::summary();
}
