// Pictures of the main window set up like the VEGAS Pro 18 window the layout was matched against: one video and one audio track
// with three short events each (cross-fades between them), the audio track selected, cursor at two seconds, about 111 pixels a
// second, window 1920x1040 with its title bar.
//
//   look_shot <output folder> [file.ofx ...]
//
// Saves main.png (default layout), main_large.png (Large Preview Layout), main_small.png (1366x728) and keys.png (Help > Keyboard
// Shortcuts). Checks nothing. QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>

using namespace reno;
using namespace reno::app;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoLookShot");
    QApplication::setApplicationName("look_shot");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("look");
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, std::vector<std::string>());

    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t a = app.importMedia("synth:1", &err), b = app.importMedia("synth:2", &err), c = app.importMedia("synth:3", &err);
    uint64_t audioTrack = 0;
    app.edit("Look", [&](Project& p) {
        Track v, s;
        v.id = p.newId();
        v.type = TrackType::Video;
        s.id = p.newId();
        s.type = TrackType::Audio;
        audioTrack = s.id;
        const struct { uint64_t media; double start, length, offset; } parts[] = {{a, 0.0, 1.05, 0.0}, {b, 1.0, 0.95, 2.0}, {c, 1.75, 0.7, 4.0}};
        for (const auto& part : parts)
            for (Track* t : {&v, &s}) {
                Event e;
                e.id = p.newId();
                e.mediaId = part.media;
                e.start = part.start;
                e.length = part.length;
                e.offset = part.offset;
                e.name = p.findMedia(part.media)->name;
                t->events.push_back(e);
            }
        p.tracks.push_back(v);
        p.tracks.push_back(s);
    });
    app.currentTrack = audioTrack;
    app.setSelection(std::set<uint64_t>());

    MainWindow w;
    w.resize(1920, 1018);  // 1040 with the title bar
    w.show();
    uitest::settle(200);
    // about 111 pixels a second, from zero
    app.setLoop(0.0, 14.25);
    w.timeline()->zoomLarge(true);
    app.setLoop(0.0, 0.0);
    app.setCursor(2.08);
    uitest::settle(1200);
    uitest::shot(&w, out + "/main.png");
    w.applyLayout(MainWindow::LargePreviewLayout);
    uitest::settle(300);
    uitest::shot(&w, out + "/main_large.png");
    w.resize(1366, 728);
    w.applyLayout(MainWindow::DefaultLayout);
    uitest::settle(300);
    uitest::shot(&w, out + "/main_small.png");
    w.resize(1920, 1018);
    w.applyLayout(MainWindow::DefaultLayout);
    uitest::settle(200);
    // the menus, a tool tip, a message box: all dark
    for (QAction* m : w.menuBar()->actions())
        if (m->text().remove('&') == "View" && m->menu()) {
            QMenu* view = m->menu();
            view->popup(w.mapToGlobal(QPoint(40, 22)));
            uitest::settle(200);
            for (QAction* a : view->actions())
                if (a->menu() && a->text().contains("Layouts")) {
                    view->setActiveAction(a);
                    a->menu()->popup(view->mapToGlobal(QPoint(view->width(), 40)));
                    uitest::settle(200);
                    uitest::shot(a->menu(), out + "/menu_layouts.png");
                    a->menu()->close();
                }
            uitest::shot(view, out + "/menu_view.png");
            view->close();
        }
    {
        QMessageBox box(QMessageBox::Question, "Reno", "Save changes to the project?", QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, &w);
        box.show();
        uitest::settle(200);
        uitest::shot(&box, out + "/messagebox.png");
    }
    w.showShortcuts();
    uitest::settle(200);
    for (QWidget* t : QApplication::topLevelWidgets())
        if (t->objectName() == "shortcutsDialog" && t->isVisible()) uitest::shot(t, out + "/keys.png");
    return 0;
}
