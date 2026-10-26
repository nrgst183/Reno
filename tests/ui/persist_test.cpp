// Settings kept between runs (QSettings, "Reno"): the timeline's switches (quantize, snapping, snap to grid and markers, auto
// ripple and its mode, grouping, envelopes, bus tracks, waveforms, event buttons and headers, grid spacing...), Automatic
// Crossfades, Quickfade, Loop Playback, track header width, the Video Preview's quality, size and switches, the Preferences
// dialog's settings, the window's size and layout, the effects window's size, the folder of the last project and recent projects
// are changed in one main window, which is then destroyed without being closed (as a crash would leave it: what is saved must
// have been saved as it changed), and a new main window must have them all back. Also: settings saved under one key and read
// from another (Snap to Grid and Preferences' "Snap to the ruler's marks", F8 and "Enable snapping") agree, and a Preferences
// change does not put a switch back to an old value.
//
//   persist_test <output folder>
//
// Build: uibuild persist_test tests/ui/persist_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QScreen>
#include <QDockWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QAction* action(MainWindow& w, const char* name)
{
    for (QAction* a : w.findChildren<QAction*>())
        if (a->objectName() == name) return a;
    return nullptr;
}
bool checked(MainWindow& w, const char* name) { QAction* a = action(w, name); return a && a->isChecked(); }
void setChecked(MainWindow& w, const char* name, bool on)
{
    QAction* a = action(w, name);
    if (!a) { check(false, std::string("there is an action ") + name); return; }
    if (a->isChecked() != on) a->trigger();
}

// Runs 'fn' on the modal dialog 'open' shows, then presses its OK button.
void withDialog(std::function<void()> open, std::function<void(QDialog*)> fn)
{
    QTimer::singleShot(300, [fn] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!d) return;
        fn(d);
        for (QPushButton* b : d->findChildren<QPushButton*>())
            if (b->text() == "OK") { b->click(); return; }
        d->reject();
    });
    open();
}

std::set<std::string> shownDocks(MainWindow& w)
{
    std::set<std::string> s;
    for (QDockWidget* d : w.findChildren<QDockWidget*>())
        if (!d->isHidden()) s.insert(d->objectName().toStdString());
    return s;
}

}  // namespace

int main(int argc, char** argv)
{
    // offscreen, a screen as big as the user's: the offscreen platform's own is 800x600, which a restored window would be fitted into
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") {
        const QString cfg = QDir::temp().absoluteFilePath("persist_test_screen.json");
        QFile f(cfg);
        if (f.open(QIODevice::WriteOnly))
            f.write(R"({"screens": [{"name": "Screen", "x": 0, "y": 0, "width": 1920, "height": 1080, "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]})");
        f.close();
        qputenv("QT_QPA_PLATFORM", ("offscreen:configfile=" + cfg).toUtf8());
    }
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoPersistTest");
    QApplication::setApplicationName("persist_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("persistout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.newProject();
    uitest::buildDemoProject();
    const QString projectPath = QDir(out).absoluteFilePath("persist.veg");
    {
        std::string err;
        check(app.save(projectPath.toStdString(), &err), "the demo project saved", err);
    }

    // ---------------------------------------------------------------- the first run: everything changed
    std::set<std::string> docksBefore;
    QSize sizeBefore;
    {
        MainWindow* w = new MainWindow();
        w->resize(1500, 900);
        w->show();
        uitest::settle(400);
        check(w->openProject(projectPath), "the project opened (File's recent projects)");
        uitest::settle(200);
        TimelineWidget* tl = w->timeline();
        // the timeline's switches (from their menu items, as the user would)
        setChecked(*w, "quantize", false);
        setChecked(*w, "snapping", false);
        setChecked(*w, "snapGrid", true);
        setChecked(*w, "snapMarkers", false);
        setChecked(*w, "autoRipple", true);
        if (QAction* a = action(*w, "rippleAll")) a->trigger();
        setChecked(*w, "ignoreGrouping", true);
        setChecked(*w, "lockEnvelopes", false);
        setChecked(*w, "showEnvelopes", false);
        setChecked(*w, "showBusTracks", true);
        setChecked(*w, "showFadeLengths", true);
        setChecked(*w, "showWaveforms", false);
        setChecked(*w, "showEventButtons", false);
        setChecked(*w, "eventHeaders", true);
        setChecked(*w, "snapAllEvents", false);
        setChecked(*w, "showTakeInfo", true);
        setChecked(*w, "edgeFrames", false);
        setChecked(*w, "autoCrossfade", false);
        setChecked(*w, "quickFade", false);
        setChecked(*w, "loop", true);
        if (QAction* a = action(*w, "showEventLength")) a->trigger();
        // the time format (Options > Time Format): new projects start with it
        if (QAction* a = action(*w, "rulerFrames")) a->trigger();
        tl->options().gridSpacing = "frames";
        tl->optionsChanged();
        tl->setHeaderWidth(310);
        // the Video Preview
        PreviewWidget* pv = w->preview();
        pv->setQuality(PreviewQuality::Best, PreviewSize::Half);
        pv->setShowOverlays(false);
        pv->setToolbarVisible(false);
        pv->setStatusBarVisible(false);
        pv->setSplitArea(QRectF(0.1, 0.2, 0.4, 0.5));
        pv->setDisplayOptions(true, false, true);
        // the Preferences dialog (several of its tabs)
        withDialog([&] { action(*w, "preferences")->trigger(); }, [](QDialog* d) {
            if (QSpinBox* s = d->findChild<QSpinBox*>("undoLevels")) s->setValue(77);
            if (QSpinBox* s = d->findChild<QSpinBox*>("autoSaveMinutes")) s->setValue(9);
            if (QCheckBox* c = d->findChild<QCheckBox*>("cursorFollowsPlayback")) c->setChecked(true);
            if (QComboBox* c = d->findChild<QComboBox*>("scrollMode")) c->setCurrentIndex(c->findData("smooth"));
            if (QCheckBox* c = d->findChild<QCheckBox*>("loopEvents")) c->setChecked(false);
            if (QSpinBox* s = d->findChild<QSpinBox*>("gridColumns")) s->setValue(6);
            if (QComboBox* c = d->findChild<QComboBox*>("videoThumbnails")) c->setCurrentIndex(c->findData("headTail"));
            if (QCheckBox* c = d->findChild<QCheckBox*>("panMultichannelTracks")) c->setChecked(false);
            if (QSpinBox* s = d->findChild<QSpinBox*>("renderThreads")) s->setValue(3);
            if (QSpinBox* s = d->findChild<QSpinBox*>("ramPreviewMb")) s->setValue(777);
        });
        uitest::settle(200);
  // Preferences applied: the switches set above are not put back by it
        check(!checked(*w, "snapping") && checked(*w, "snapGrid") && !checked(*w, "quickFade"),
              "OK in Preferences leaves Enable Snapping, Snap to Grid and Quickfade as they were switched");
        // the window: its size and another layout, the effects window's size
        w->applyLayout(MainWindow::AudioMixingLayout);
        w->resize(1320, 840);
        FxTarget vo;
        vo.kind = FxTarget::VideoOut;
        w->fxWindow()->showTarget(vo, 0);
        w->fxWindow()->show();
        uitest::settle(200);
        w->fxWindow()->resize(640, 520);
        uitest::settle(900);  // a moment after the last change, the layout is saved
        docksBefore = shownDocks(*w);
        sizeBefore = w->size();
        // saved already, before any closing (a crash now would keep them)
        QSettings s;
        check(s.contains("window/geometry") && s.value("window/layout").toString() == "Audio Mixing Layout" && s.contains("fxWindow/geometry") &&
                  s.value("timeline/quantize", true).toBool() == false && s.value("preview/size").toInt() == (int)PreviewSize::Half &&
                  s.value("prefs/undoLevels").toInt() == 77 && s.value("options/loopPlayback").toBool(),
              "everything is in QSettings as soon as it changed, before the window is closed");
        uitest::shot(w, out + "/persist_before.png");
        delete w;  // no closeEvent: as if Reno had crashed
    }

    // what a new run of Reno starts with in memory: the defaults, before the saved settings are read
    app.snapping = true;
    app.loopOn = false;
    app.setAutoCrossfades(true);
    app.prefs = Preferences();
    app.loadPreferences();
    app.newProject(true);
    uitest::settle(100);

    // ---------------------------------------------------------------- the second run: all of it back
    {
        MainWindow* w = new MainWindow();
        w->show();
        uitest::settle(600);
        TimelineWidget* tl = w->timeline();
        const TimelineOptions& o = tl->options();
        check(!o.quantize && !app.snapping && o.snapToGrid && !o.snapToMarkers && o.autoRipple && o.ripple == RippleMode::AllTracks && o.ignoreGrouping,
              "the timeline's editing switches are back (quantize, snapping, snap to grid and markers, auto ripple and its mode, grouping)");
        check(!o.lockEnvelopes && !o.showEnvelopes && o.showBusTracks && o.showFadeLengths && !o.showWaveforms && !o.showEventButtons && o.eventHeaders &&
                  !o.snapToAllEvents && o.showTakeInfo && !o.edgeFrames && o.showEventLength && o.gridSpacing == "frames",
              "and its view switches (envelopes, bus tracks, fade lengths, waveforms, event buttons and headers, take info, edge frames, event length, grid spacing)");
        check(!checked(*w, "quantize") && !checked(*w, "snapping") && checked(*w, "snapGrid") && checked(*w, "autoRipple") && checked(*w, "rippleAll") &&
                  !checked(*w, "showWaveforms") && checked(*w, "showBusTracks"),
              "the menus show them");
        check(!app.autoCrossfades() && !app.prefs.quickFadeEdits && app.loopOn && !checked(*w, "autoCrossfade") && checked(*w, "loop") && !checked(*w, "quickFade"),
              "Automatic Crossfades, Quickfade Audio Edits and Loop Playback are back");
        check(tl->headerWidth() == 310, "the track header width is back", strfmt("%d", tl->headerWidth()));
        PreviewWidget* pv = w->preview();
        check(pv->quality() == PreviewQuality::Best && pv->size() == PreviewSize::Half, "the preview's quality and size are back (Best, Half)");
        check(!pv->showOverlays() && !pv->toolbarVisible() && !pv->statusBarVisible() && pv->splitArea() == QRectF(0.1, 0.2, 0.4, 0.5) &&
                  pv->simulateAspect() && !pv->scaleToFit() && pv->adjustForPlayback(),
              "and its switches (effects' controls, tool bar, status lines, split area, display options)");
        const Preferences& pr = app.prefs;
        check(pr.undoLevels == 77 && pr.autoSaveMinutes == 9 && pr.cursorFollowsPlayback && pr.scrollMode == "smooth" && !pr.loopEvents && pr.gridColumns == 6 &&
                  pr.videoThumbnails == "headTail" && !pr.panMultichannelTracks && pr.previewQuality == "best",
              "the Preferences dialog's settings are back (General, Editing, Video, Audio)");
        check(pr.renderThreads == 3 && pr.ramPreviewMb == 777, "and Video's rendering threads and Dynamic RAM Preview size",
              strfmt("%d %d", pr.renderThreads, pr.ramPreviewMb));
        check(app.project().settings.rulerFormat == "frames", "a new project starts with the time format chosen last", app.project().settings.rulerFormat);
        check(w->currentLayout() == "Audio Mixing Layout" && shownDocks(*w) == docksBefore, "the window layout is back (the same docked windows showing)",
              w->currentLayout().toStdString());
        const QRect screen = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->availableGeometry() : QRect();
        check(w->size() == sizeBefore, "and the window's size", strfmt("%dx%d, was %dx%d (screen %dx%d)", w->width(), w->height(), sizeBefore.width(),
                                                                     sizeBefore.height(), screen.width(), screen.height()));
        FxTarget vo;
        vo.kind = FxTarget::VideoOut;
        w->fxWindow()->showTarget(vo, 0);
        w->fxWindow()->show();
        uitest::settle(200);
        check(w->fxWindow()->size() == QSize(640, 520), "the effects window opens at the size it had",
              strfmt("%dx%d", w->fxWindow()->width(), w->fxWindow()->height()));
        w->fxWindow()->hide();
        const QStringList recent = QSettings().value("recentProjects").toStringList();
        check(!recent.isEmpty() && recent.first() == projectPath && w->projectFolder() == QFileInfo(projectPath).absolutePath(),
              "the recent projects and the folder the Open dialog starts in are kept");
        // Preferences shows the switches as they are (one setting, two places)
        bool snapBox = true, gridBox = false;
        withDialog([&] { action(*w, "preferences")->trigger(); }, [&](QDialog* d) {
            if (QCheckBox* c = d->findChild<QCheckBox*>("snapping")) snapBox = c->isChecked();
            if (QCheckBox* c = d->findChild<QCheckBox*>("snapToGrid")) gridBox = c->isChecked();
        });
        uitest::settle(100);
        check(!snapBox && gridBox, "Preferences' Enable snapping and Snap to the ruler's marks show the switches as they were left");
        // switching one back, from the menu, is kept too (and Preferences after it does not undo it)
        setChecked(*w, "snapGrid", false);
        setChecked(*w, "snapping", true);
        app.prefs.cutToOverlap = 2.0;
        app.applyPreferences();
        uitest::settle(100);
        check(!tl->options().snapToGrid && app.snapping && !QSettings().value("prefs/snapToGrid").toBool() && QSettings().value("prefs/snapping").toBool(),
              "Snap to Grid and Enable Snapping switched in the menu stay so through a change of Preferences, and are saved");
        uitest::shot(w, out + "/persist_after.png");
        delete w;
    }
    return uitest::summary();
}
