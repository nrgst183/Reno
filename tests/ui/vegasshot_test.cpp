// Pictures of the VEGAS look: main window with a demo project (default, Large Preview and Audio Mixing layouts, a selected
// event, a small window), every menu, the effects window, Keyboard Shortcuts, and every page of Options > Preferences.
// Saves PNGs only (checks nothing); used to compare the look before and after a change to the interface code.
//
//   vegasshot_test <output folder> [testfx.ofx ...]
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/fxwindow.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

using namespace reno;
using namespace reno::app;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoVegasShot");
    QApplication::setApplicationName("vegasshot_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("vegasshot");
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, std::vector<std::string>());
    uitest::buildDemoProject();

    MainWindow w;
    w.resize(1600, 900);
    w.show();
    w.timeline()->zoomToFit();
    AppState::get().setCursor(2.5);
    uitest::settle(600);
    uitest::shot(&w, out + "/main.png");

    // an event selected, the second track current
    {
        const Project& p = AppState::get().project();
        std::set<uint64_t> sel;
        if (!p.tracks.empty() && !p.tracks[0].events.empty()) sel.insert(p.tracks[0].events[0].id);
        AppState::get().setSelection(sel);
        if (p.tracks.size() > 1) AppState::get().currentTrack = p.tracks[1].id;
        AppState::get().notify(kSelection);
        uitest::settle(300);
        uitest::shot(&w, out + "/main_selected.png");
    }
    w.applyLayout(MainWindow::LargePreviewLayout);
    uitest::settle(300);
    uitest::shot(&w, out + "/main_large.png");
    w.applyLayout(MainWindow::AudioMixingLayout);
    uitest::settle(300);
    uitest::shot(&w, out + "/main_mixing.png");
    w.applyLayout(MainWindow::DefaultLayout);
    w.resize(1100, 640);
    uitest::settle(300);
    uitest::shot(&w, out + "/main_small.png");
    w.resize(1600, 900);
    uitest::settle(300);

    // every menu
    int n = 0;
    for (QAction* m : w.menuBar()->actions())
        if (m->menu()) {
            QMenu* menu = m->menu();
            menu->popup(w.mapToGlobal(QPoint(40, 22)));
            uitest::settle(150);
            uitest::shot(menu, out + "/menu_" + QString::number(n++) + ".png");
            menu->close();
        }

    // the effects window
    const Project& p = AppState::get().project();
    for (const auto& t : p.tracks)
        for (const auto& e : t.events)
            if (!e.fx.empty()) {
                FxTarget target;
                target.kind = FxTarget::Event;
                target.id = e.id;
                w.openFx(target, e.fx[0].id);
                uitest::settle(300);
                uitest::shot(w.fxWindow(), out + "/fxwindow.png");
                w.fxWindow()->hide();
                goto fxdone;
            }
fxdone:

    // Options > Preferences: every page
    {
        QAction* act = nullptr;
        for (QAction* a : w.findChildren<QAction*>())
            if (a->objectName() == "preferences") act = a;
        QTimer::singleShot(400, [&] {
            QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!d) return;
            uitest::shot(d, out + "/prefs_0.png");
            int page = 1;
            for (QTabWidget* tw : d->findChildren<QTabWidget*>())
                for (int i = 0; i < tw->count(); ++i) {
                    tw->setCurrentIndex(i);
                    uitest::settle(80);
                    uitest::shot(d, out + "/prefs_" + QString::number(page++) + ".png");
                    // the part of a long page below the window's edge
                    for (QScrollArea* sa : tw->widget(i)->findChildren<QScrollArea*>()) {
                        sa->verticalScrollBar()->setValue(sa->verticalScrollBar()->maximum());
                        uitest::settle(80);
                        uitest::shot(d, out + "/prefs_" + QString::number(page - 1) + "_bottom.png");
                        sa->verticalScrollBar()->setValue(0);
                    }
                }
            for (QListWidget* lw : d->findChildren<QListWidget*>())
                for (int i = 0; i < lw->count() && i < 40; ++i) {
                    lw->setCurrentRow(i);
                    uitest::settle(80);
                    uitest::shot(d, out + "/prefs_" + QString::number(page++) + ".png");
                }
            for (QTreeWidget* tw : d->findChildren<QTreeWidget*>())
                for (int i = 0; i < tw->topLevelItemCount() && i < 40; ++i) {
                    tw->setCurrentItem(tw->topLevelItem(i));
                    uitest::settle(80);
                    uitest::shot(d, out + "/prefs_" + QString::number(page++) + ".png");
                }
            d->reject();
        });
        if (act) act->trigger();
    }

    w.showShortcuts();
    uitest::settle(200);
    for (QWidget* t : QApplication::topLevelWidgets())
        if (t->objectName() == "shortcutsDialog" && t->isVisible()) uitest::shot(t, out + "/keys.png");
    return 0;
}
