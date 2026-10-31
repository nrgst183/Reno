// Main window layout and behaviour as in VEGAS Pro 18: the docking area over the timeline, track header and row sizes, buttons on
// headers and events, header column resizing, the bar under the timeline, the status bar, and the Preferences the timeline and
// preview follow.
//
//   look_test <output folder> [testfx.ofx ...]
//
// Build: uibuild look_test tests/ui/look_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QDockWidget>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

// The texts of the menu that pops up while 'open' runs (the menu is closed again).
QStringList menuTexts(std::function<void()> open, const QString& sub = QString())
{
    QStringList texts;
    QTimer::singleShot(150, [&] {
        QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m) return;
        QMenu* from = m;
        if (!sub.isEmpty())
            for (QAction* a : m->actions())
                if (a->menu() && a->text() == sub) from = a->menu();
        for (QAction* a : from->actions()) texts << a->text().split('\t')[0];
        m->close();
    });
    open();
    uitest::settle(50);
    return texts;
}

QRect inWindow(QWidget* w, QWidget* top) { return QRect(w->mapTo(top, QPoint(0, 0)), w->size()); }

// The tab in front in the main window's row of tabs that has a tab called 'any'.
QString frontTab(QWidget* w, const QString& any)
{
    for (QTabBar* tb : w->findChildren<QTabBar*>())
        for (int k = 0; k < tb->count(); ++k)
            if (tb->tabText(k) == any && tb->isVisible()) return tb->tabText(tb->currentIndex());
    return QString();
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoLookTest");
    QApplication::setApplicationName("look_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("look");
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, std::vector<std::string>());
    uitest::buildDemoProject();
    // the preview starts at the quality Preferences say
    st().prefs.previewQuality = "draft";

    MainWindow w;
    w.resize(1920, 1018);
    w.show();
    uitest::settle(400);
    TimelineWidget* tl = w.timeline();
    tl->zoomToFit();
    uitest::settle(100);
    uitest::shot(&w, out + "/look_main.png");

    // ---------------------------------------------------------------- the layout
    {
        QStringList menus;
        for (QAction* m : w.menuBar()->actions()) menus << m->text().remove('&');
        check(menus.join(",") == "File,Edit,View,Insert,Tools,Options,Help", "the menu bar", menus.join(",").toStdString());
        QDockWidget* preview = w.findChild<QDockWidget*>("dockPreview");
        QDockWidget* master = w.findChild<QDockWidget*>("dockMaster");
        QDockWidget* media = w.findChild<QDockWidget*>("dockMedia");
        const QRect tlr = inWindow(tl, &w);
        check(w.currentLayout() == "Default Layout", "Reno starts in the default layout", w.currentLayout().toStdString());
        check(preview && master && media && preview->isVisible() && master->isVisible() && media->isVisible(), "Project Media, the Video Preview and the Master Bus are shown");
        check(media && preview && master && media->geometry().right() < preview->geometry().left() && preview->geometry().right() < master->geometry().left() &&
                  media->geometry().left() < 10 && master->geometry().right() > w.width() - 10,
              "left to right: the windows' tabs, the preview, the Master Bus");
        check(preview && master && media && preview->geometry().bottom() < tlr.top() && master->geometry().bottom() < tlr.top() && media->geometry().bottom() < tlr.top(),
              "all over the timeline");
        check(master && master->width() >= 150 && master->width() < 200, "the Master Bus is narrow", strfmt("%d", master ? master->width() : 0));
        check(preview && media && std::abs(preview->width() - media->width()) < 40, "the tabs and the preview share the rest",
              strfmt("%d %d", media ? media->width() : 0, preview ? preview->width() : 0));
        check(tlr.top() > 420 && tlr.top() < 540, "the timeline starts a little under half way down", strfmt("%d", tlr.top()));
        // the tab group: Project Media in front, then the others as tabs along its bottom
        QStringList tabs;
        for (QDockWidget* d : w.tabifiedDockWidgets(media)) tabs << d->objectName();
        check(tabs.join(" ") == "dockExplorer dockVideoFx dockAudioFx dockGenerators dockTransitions", "the other windows share Project Media's place, in VEGAS's order",
              tabs.join(" ").toStdString());
        QStringList shown;
        for (QDockWidget* d : w.findChildren<QDockWidget*>())
            if (!d->isHidden()) shown << d->objectName();
        shown.sort();
        check(shown.join(" ") == "dockAudioFx dockExplorer dockGenerators dockMaster dockMedia dockPreview dockTransitions dockVideoFx",
              "Project Notes, the Mixing Console, Video Scopes and Edit Details are closed until asked for", shown.join(" ").toStdString());
        check(frontTab(&w, "Video FX") == "Project Media", "Project Media is the tab in front", frontTab(&w, "Video FX").toStdString());
        // the closed ones open there too
        for (const char* a : {"showNotes", "showMixer", "showScopes", "showDetails"})
            if (QAction* x = w.findChild<QAction*>(a)) x->trigger();
        uitest::settle(100);
        tabs.clear();
        for (QDockWidget* d : w.tabifiedDockWidgets(media)) tabs << d->objectName();
        check(tabs.contains("dockNotes") && tabs.contains("dockMixer") && tabs.contains("dockScopes") && tabs.contains("dockDetails"),
              "Project Notes, the Mixing Console, Video Scopes and Edit Details open as tabs there", tabs.join(" ").toStdString());
        uitest::shot(&w, out + "/look_details.png");
        QToolBar* main = w.findChild<QToolBar*>("mainToolBar");
        check(main && inWindow(main, &w).bottom() < preview->geometry().top(), "the main tool bar is under the menus");
        QWidget* tab = preview->findChild<QWidget*>("dockTabStrip");
        check(tab && tab->isVisible() && inWindow(tab, &w).top() > inWindow(w.preview(), &w).bottom() - 30, "the Video Preview's tab is at its bottom");
        QWidget* mediaTab = media->findChild<QWidget*>("dockTabStrip");
        check(mediaTab && !mediaTab->isVisible(), "the tab group shows the main window's tabs instead of its own");

        // the Large Preview Layout: the arrangement of the VEGAS 18 window
        w.applyLayout(MainWindow::LargePreviewLayout);
        uitest::settle(300);
        const QRect tl2 = inWindow(tl, &w);
        check(w.currentLayout() == "Large Preview Layout", "Alt+D, V: the Large Preview Layout");
        check(preview->isVisible() && master->isVisible() && !media->isVisible() && preview->geometry().right() < master->geometry().left(),
              "the preview and the Master Bus side by side, the other windows closed");
        check(preview->width() > 1300 && master->width() > 400 && master->width() < 560, "the preview takes most of the width",
              strfmt("%d %d", preview->width(), master->width()));
        check(tl2.top() > 560 && tl2.top() < 660, "the timeline starts a little more than half way down", strfmt("%d", tl2.top()));
        uitest::shot(&w, out + "/look_large.png");
        w.applyLayout(MainWindow::AudioMixingLayout);
        uitest::settle(300);
        QDockWidget* mixer = w.findChild<QDockWidget*>("dockMixer");
        check(mixer && mixer->isVisible() && frontTab(&w, "Project Media") == "Mixing Console" && mixer->width() > preview->width(), "Alt+D, A: the Mixing Console in front, wide",
              strfmt("%d %d %d", mixer->isVisible(), mixer->width(), preview->width()));
        w.restoreDefaultLayout();
        uitest::settle(300);
        check(w.currentLayout() == "Default Layout" && media->isVisible() && !mixer->isVisible(), "Alt+D, D: the default layout again");
        // the window's own menus and title bar strip are dark
        const QImage shot = w.grab().toImage();
        const QColor menuBar = shot.pixelColor(w.width() - 20, w.menuBar()->height() / 2);
        check(menuBar.lightness() < 70, "the menu bar is dark", menuBar.name().toStdString());
    }

    // ---------------------------------------------------------------- the timeline's sizes and buttons
    {
        const Project& p = st().project();
        uint64_t video = 0, audio = 0;
        for (const Track& t : p.tracks) {
            if (t.type == TrackType::Video && !video) video = t.id;
            if (t.type == TrackType::Audio && !audio) audio = t.id;
        }
        check(tl->headerWidth() == 161 && tl->xAt(tl->viewStartTime()) == 161, "the headers are 161 pixels wide (with the gap)", strfmt("%d", tl->headerWidth()));
        check(tl->trackRect(video).height() == 64 && tl->trackRect(audio).height() == 68, "video rows are 64 pixels, audio 68");
        check(tl->rulerRect().bottom() + 1 == tl->trackRect(p.tracks[0].id).top() && tl->rulerRect().height() == 16, "the ruler is 16 pixels, right over the tracks");
        check(tl->headerPartRect(video, TimelineWidget::PartFx).isEmpty() && !tl->headerPartRect(video, TimelineWidget::PartMute).isEmpty() &&
                  !tl->headerPartRect(video, TimelineWidget::PartSolo).isEmpty(),
              "at that width a header shows mute and solo only");
        // the header's menu button
        const QRect menu = tl->headerPartRect(audio, TimelineWidget::PartMenu);
        const QStringList trackMenu = menuTexts([&] { uitest::mouseClick(tl, menu.center()); });
        // VEGAS 18's hamburger menu: the buttons the header has no room for, then Edit Visible Button Set
        check(trackMenu.contains("Track FX") && trackMenu.contains("Arm for Record") && trackMenu.contains("Edit Visible Button Set...") && st().currentTrack == audio,
              "the strip's menu button opens the track's menu",
              trackMenu.join("|").toStdString());
        const QRect vmenu = tl->headerPartRect(video, TimelineWidget::PartMenu);
        const QStringList modes = menuTexts([&] { uitest::mouseClick(tl, vmenu.center()); }, "Compositing Mode");
        check(modes.contains("Source Alpha") && modes.contains("Add") && modes.size() > 5, "a video track's menu has the compositing modes", modes.join("|").toStdString());
        // the gap at the right of the headers resizes them
        const QPoint gap(158, tl->trackRect(video).center().y());
        uitest::mouseDrag(tl, gap, gap + QPoint(100, 0));
        check(tl->headerWidth() == 261 && !tl->headerPartRect(video, TimelineWidget::PartFx).isEmpty() && !tl->headerPartRect(video, TimelineWidget::PartMotion).isEmpty(),
              "dragging the gap widens the headers, which then show FX and Track Motion", strfmt("%d", tl->headerWidth()));
        QWidget* rate = w.findChild<QWidget*>("ratePanel");
        check(rate && rate->width() == 261, "the Rate panel under the headers follows their width");
        tl->setHeaderWidth(161);
        check(QSettings().value("timeline/headerWidth").toInt() == 161, "the width is kept for next time");
        // an event's menu button
        uint64_t ev = 0;
        for (const Track& t : p.tracks)
            if (t.id == video && !t.events.empty()) ev = t.events[0].id;
        const QRect em = tl->eventMenuRect(ev);
        check(!em.isEmpty() && !tl->eventButtonRect(ev, true).isEmpty() && tl->eventButtonRect(ev, true).right() < tl->eventButtonRect(ev, false).left() &&
                  tl->eventButtonRect(ev, false).right() < em.left(),
              "a video event has Pan/Crop, FX and menu buttons along its bottom right");
        const QStringList eventMenu = em.isEmpty() ? QStringList() : menuTexts([&] { uitest::mouseClick(tl, em.center()); });
        // VEGAS 18's hamburger menu for an event
        check(eventMenu.contains("Event FX") && eventMenu.contains("Freeze Frame at Cursor") && eventMenu.contains("Edit Visible Button Set..."), "the event's menu button opens its menu", eventMenu.join("|").toStdString());
        // the zoom buttons at the end of the scroll bar
        const double span = tl->viewEndTime() - tl->viewStartTime();
        if (QAbstractButton* in = tl->findChild<QAbstractButton*>("zoomInButton")) in->click();
        check(tl->viewEndTime() - tl->viewStartTime() < span * 0.8, "the + button beside the scroll bar zooms in");
        if (QAbstractButton* outB = tl->findChild<QAbstractButton*>("zoomOutButton")) outB->click();
        const int h0 = tl->trackRect(video).height();
        if (QAbstractButton* taller = tl->findChild<QAbstractButton*>("tallerTracksButton")) taller->click();
        check(tl->trackRect(video).height() > h0, "the + button under the vertical scroll bar makes the tracks taller");
        tl->resetTrackHeights();
        check(tl->trackRect(video).height() == h0, "and they go back to the default height");
        tl->zoomToFit();
    }

    // ---------------------------------------------------------------- under the timeline, and the status bar
    {
        QSlider* rate = w.findChild<QSlider*>("rateSlider");
        QLabel* rateLabel = w.findChild<QLabel*>("rateLabel");
        check(rate && rate->isEnabled() && rate->minimum() == -200 && rate->maximum() == 200 && rate->value() == 0 && rateLabel && rateLabel->text() == "Rate: 0.00",
              "the Rate slider (-2 .. 2) rests in the middle and says Rate: 0.00", rateLabel ? rateLabel->text().toStdString() : "");
        QAbstractButton* tool = w.findChild<QAbstractButton*>("editToolButton");
        check(tool && tool->isChecked(), "the normal edit tool is lit");
        QLabel* cursor = w.findChild<QLabel*>("cursorPosition");
        QLabel* end = w.findChild<QLabel*>("selectionEnd");
        QLabel* length = w.findChild<QLabel*>("selectionLength");
        st().setLoop(0, 0);
        st().setCursor(2.0);
        check(cursor && cursor->text() == "00:00:02:00" && end && end->text().isEmpty() && length && length->text().isEmpty(),
              "the cursor's time at the bottom right; no selection, no end or length", cursor ? cursor->text().toStdString() : "");
        st().setLoop(1.0, 3.5);
        check(end && end->text() == "00:00:03:15" && length && length->text() == "00:00:02:15", "with a time selection, its end and length",
              (end ? end->text() : QString()).toStdString() + " " + (length ? length->text() : QString()).toStdString());
        st().setLoop(0, 0);
        QLabel* disk = w.statusBar()->findChild<QLabel*>("diskSpace");
        check(disk && disk->text().contains("GB"), "the status bar tells the free disk space", disk ? disk->text().toStdString() : "");
        for (const char* name : {"toolEnvelope", "lockEnvelopes", "autoCrossfade"}) {
            QAction* a = w.findChild<QAction*>(name);
            check(a && a->isEnabled() && a->isCheckable(), std::string("working: ") + name);
        }
        // Lock works on the selected events: greyed while none is selected
        QAction* lock = w.findChild<QAction*>("lockEvent");
        check(lock && lock->isCheckable() && lock->isEnabled() == !st().selection().empty(), "working: lockEvent (with events selected)");
    }

    // ---------------------------------------------------------------- Preferences the timeline and the preview follow
    {
        check(w.preview()->quality() == PreviewQuality::Draft, "the preview starts at the quality Preferences give");
        Playback* pb = w.playback();
        // the cursor stays where playing stopped, if Preferences say so
        st().prefs.cursorFollowsPlayback = true;
        st().setCursor(1.0);
        pb->play();
        uitest::settle(300);
        stopPlayback(pb);
        check(!pb->playing() && st().cursor() > 1.1, "with the cursor following playback, stopping leaves it there", strfmt("%f", st().cursor()));
        st().prefs.cursorFollowsPlayback = false;
        st().setCursor(1.0);
        pb->play();
        uitest::settle(300);
        stopPlayback(pb);
        check(std::fabs(st().cursor() - 1.0) < 1e-9, "without, it goes back to where playing began");
        // snapping to the ruler's marks
        st().prefs.snapToGrid = true;
        st().notify(kPrefs);
        check(tl->options().snapToGrid, "Snap to Grid follows Preferences");
        st().prefs.snapToGrid = false;
        st().notify(kPrefs);
        // scrolling while playing: with None the view stays put
        for (int k = 0; k < 6; ++k) tl->zoomIn();
        st().prefs.scrollMode = "none";
        const double v0 = tl->viewStartTime();
        st().setCursor(tl->viewEndTime() - 0.05);
        pb->play();
        uitest::settle(600);
        check(std::fabs(tl->viewStartTime() - v0) < 1e-9, "with scrolling set to None the view does not follow playback");
        pb->pause();
        st().prefs.scrollMode = "page";
        st().setCursor(tl->viewEndTime() - 0.05);
        const double v1 = tl->viewStartTime();
        pb->play();
        uitest::settle(600);
        check(tl->viewStartTime() > v1, "with Page it turns the page", strfmt("%f %f", v1, tl->viewStartTime()));
        pb->pause();
        tl->zoomToFit();
    }
    uitest::settle(300);
    return uitest::summary();
}
