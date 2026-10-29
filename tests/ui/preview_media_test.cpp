// Video Preview options (channels, split screen, fit / project size, device aspect, tool bar and status bar, right-click menu),
// Project Media's bins, smart bins, views, search, subclips, properties, Replace and Remove All Unused Media, and the related
// Project Properties and Preferences. Saves pictures.
//
//   preview_media_test <output folder> [testfx.ofx ...]
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/appstate.h"
#include "../../src/app/editdialogs.h"
#include "../../src/app/mainwindow.h"  // so the stand-in checks link the windows the main window makes
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/theme.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

template <class T>
T* kid(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

// Calls step every 40 ms with the modal dialog (or popup menu) open; step returns true when it is done with it.
void drive(std::function<bool(QWidget*)> step, const std::string& what, int limitMs = 20000)
{
    auto* t = new QTimer(qApp);
    auto* clock = new QElapsedTimer();
    clock->start();
    QObject::connect(t, &QTimer::timeout, [=] {
        QWidget* m = QApplication::activeModalWidget();
        if (!m) m = QApplication::activePopupWidget();
        if (!m) return;
        if (step(m)) {
            t->stop();
            t->deleteLater();
            delete clock;
        } else if (clock->elapsed() > limitMs) {
            check(false, what + ": did not finish in time");
            t->stop();
            t->deleteLater();
            delete clock;
            m->close();
        }
    });
    t->start(40);
}

QRgb centre(QWidget* picture, const QRect& r)
{
    const QImage img = picture->grab().toImage();
    const QPoint c = r.center();
    return img.pixel(std::min(img.width() - 1, std::max(0, c.x())), std::min(img.height() - 1, std::max(0, c.y())));
}

QStringList menuTexts(QMenu* m)
{
    QStringList t;
    for (QAction* a : m->actions())
        if (!a->isSeparator()) t << a->text();
    return t;
}

void testPreview()
{
    AppState& app = AppState::get();
    app.setCursor(2.5);
    Playback& pb = *new Playback();  // never destroyed: its video thread may still post a frame
    PreviewWidget pv(&pb);
    pv.resize(840, 480);
    pv.show();
    QElapsedTimer first;
    first.start();
    do uitest::settle(50); while (pb.lastFrame().isNull() && first.elapsed() < 5000);
    pb.requestFrame();
    uitest::settle(400);
    QWidget* picture = pv.pictureWidget();
    const QRect r = pv.pictureRect();
    check(!r.isEmpty(), "preview: the picture has a place");
    const QRgb normal = centre(picture, r);

    // channels: red alone, then alpha as grey (the preview's pictures are opaque: white)
    pv.setChannel(1);
    uitest::settle(300);
    QRgb c = centre(picture, r);
    check(qGreen(c) == 0 && qBlue(c) == 0, "preview: Overlays > Red shows only the red channel", strfmt("%d %d %d (was %d %d %d)", qRed(c), qGreen(c), qBlue(c), qRed(normal), qGreen(normal), qBlue(normal)));
    uitest::shot(&pv, gOut + "/preview_red.png");
    pv.setChannel(5);
    uitest::settle(300);
    c = centre(picture, r);
    check(qRed(c) == qGreen(c) && qGreen(c) == qBlue(c) && std::abs(qGreen(c) - qGreen(normal)) <= 3, "preview: Green as Grayscale", strfmt("%d %d %d", qRed(c), qGreen(c), qBlue(c)));
    pv.setChannel(7);
    uitest::settle(300);
    c = centre(picture, r);
    check(qRed(c) == 255 && qGreen(c) == 255, "preview: Alpha as Grayscale: opaque is white");
    pv.setChannel(0);
    uitest::settle(300);
    check(centre(picture, r) == normal, "preview: back to the whole picture");

    // the Overlays menu lists the channels (VEGAS's)
    QStringList overlayItems;
    drive([&](QWidget* m) {
        auto* menu = qobject_cast<QMenu*>(m);
        if (!menu) return false;
        overlayItems = menuTexts(menu);
        uitest::shot(menu, gOut + "/preview_overlays_menu.png");
        menu->close();
        return true;
    }, "overlays menu");
    kid<QAbstractButton>(&pv, "overlays")->click();
    check(overlayItems.contains("Red as Grayscale") && overlayItems.contains("Alpha as Grayscale") && overlayItems.contains("Safe Areas") && overlayItems.contains("Grid"),
          "preview: the Overlays menu has the safe areas, the grid and the channels", overlayItems.join(", ").toStdString());

    // split screen, FX bypassed: the other picture arrives from its own thread
    pv.setSplitScreen(true, false);
    QElapsedTimer wait;
    wait.start();
    while (!pv.splitPictureReady() && wait.elapsed() < 5000) uitest::settle(50);
    check(pv.splitScreen() && pv.splitPictureReady(), "preview: Split-Screen View renders the frame without effects");
    check(kid<QAbstractButton>(&pv, "splitScreen")->isEnabled() && kid<QAbstractButton>(&pv, "splitScreen")->isChecked(), "preview: the split-screen button is on");
    uitest::shot(&pv, gOut + "/preview_split.png");
    // a drag over the picture chooses the area
    const QRect t = pv.pictureRect();
    uitest::mouseDrag(picture, QPoint(t.left() + t.width() / 4, t.top() + t.height() / 4), QPoint(t.left() + t.width() * 3 / 4, t.top() + t.height() * 3 / 4));
    const QRectF a = pv.splitArea();
    check(std::fabs(a.left() - 0.25) < 0.03 && std::fabs(a.right() - 0.75) < 0.03 && std::fabs(a.top() - 0.25) < 0.03, "preview: a drag over the picture chooses the split area",
          strfmt("%.3f %.3f %.3f %.3f", a.left(), a.top(), a.width(), a.height()));
    check(QSettings().value("preview/splitArea").toRectF() == a, "preview: the split area is remembered");
    uitest::shot(&pv, gOut + "/preview_split_area.png");
    // the clipboard's picture
    QImage red(64, 36, QImage::Format_ARGB32);
    red.fill(QColor(255, 0, 0));
    QGuiApplication::clipboard()->setImage(red);
    pv.setSplitScreen(true, true);
    uitest::settle(100);
    c = centre(picture, t);
    check(pv.splitClipboard() && qRed(c) > 200 && qGreen(c) < 40, "preview: split screen can show the clipboard's picture", strfmt("%d %d %d", qRed(c), qGreen(c), qBlue(c)));
    pv.setSplitArea(QRectF(0, 0, 0.5, 1));
    pv.setSplitScreen(false);
    check(!pv.splitScreen(), "preview: split screen off");

    // Scale Video to Fit Preview Window off: one preview pixel to one screen pixel
    pv.setQuality(PreviewQuality::Good, PreviewSize::Half);
    pv.setDisplayOptions(false, false, false);
    uitest::settle(100);
    check(pv.pictureRect().size() == QSize(960, 540), "preview: not scaled to fit, the picture is the preview's size (Good, Half: 960x540)",
          strfmt("%dx%d", pv.pictureRect().width(), pv.pictureRect().height()));
    uitest::shot(&pv, gOut + "/preview_not_fitted.png");
    pv.setDisplayOptions(false, true, false);
    pv.setQuality(PreviewQuality::Good, PreviewSize::Auto);
    uitest::settle(50);
    check(pv.pictureRect().width() <= 840, "preview: scaled to fit again");
    // Simulate Device Aspect Ratio: pixels twice as wide make the picture twice as wide for its height
    app.edit("Wide pixels", [](Project& p) { p.settings.pixelAspect = 2.0; });
    pv.setDisplayOptions(true, true, false);
    uitest::settle(100);
    const QRect wide = pv.pictureRect();
    check(std::fabs((double)wide.width() / wide.height() - 2.0 * 16.0 / 9.0) < 0.05, "preview: Simulate Device Aspect Ratio draws the pixels' shape",
          strfmt("%dx%d", wide.width(), wide.height()));
    app.undo();
    pv.setDisplayOptions(false, true, false);
    // Adjust Size and Quality for Optimal Playback: Preview while playing
    pv.setQuality(PreviewQuality::Best, PreviewSize::Full);
    pv.setDisplayOptions(false, true, true);
    pb.play();
    uitest::settle(100);
    const QSize playing = pv.previewSize();
    pb.pause();
    uitest::settle(100);
    check(playing.width() < 1920 && pv.previewSize() == QSize(1920, 1080), "preview: Adjust Size and Quality lowers the preview only while playing",
          strfmt("%dx%d / %dx%d", playing.width(), playing.height(), pv.previewSize().width(), pv.previewSize().height()));
    pv.setDisplayOptions(false, true, false);
    pv.setQuality(PreviewQuality::Good, PreviewSize::Auto);

    // tool bar and status bar
    pv.setToolbarVisible(false);
    check(kid<QWidget>(&pv, "previewToolBar")->isHidden(), "preview: Show Toolbar off hides the tool bar");
    pv.setStatusBarVisible(false);
    check(kid<QLabel>(&pv, "frameNumber")->isHidden() && kid<QLabel>(&pv, "projectFormat")->isHidden(), "preview: Show Status Bar off hides the lines under the picture");
    uitest::shot(&pv, gOut + "/preview_bare.png");
    pv.setToolbarVisible(true);
    pv.setStatusBarVisible(true);
    check(!kid<QWidget>(&pv, "previewToolBar")->isHidden() && !kid<QLabel>(&pv, "frameNumber")->isHidden(), "preview: and back");

    // the external monitor button
    int monitor = 0;
    pv.onExternalMonitor = [&] { ++monitor; };
    auto* mb = kid<QAbstractButton>(&pv, "externalMonitor");
    check(mb && mb->isEnabled(), "preview: Video Preview on External Monitor is available");
    mb->click();
    check(monitor == 1, "preview: the button opens the external monitor preview");

    // the right-click menu
    QStringList items;
    drive([&](QWidget* m) {
        auto* menu = qobject_cast<QMenu*>(m);
        if (!menu) return false;
        items = menuTexts(menu);
        uitest::shot(menu, gOut + "/preview_context_menu.png");
        menu->close();
        return true;
    }, "preview context menu");
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pv.pictureRect().center(), picture->mapToGlobal(pv.pictureRect().center()));
    QApplication::sendEvent(picture, &ev);
    for (const char* want : {"Simulate Device Aspect Ratio", "Scale Video to Fit Preview Window", "Adjust Size and Quality for Optimal Playback", "Show Toolbar",
                             "Show Status Bar", "Copy Snapshot to Clipboard", "Save Snapshot to File...", "Video Preview Preferences..."})
        check(items.contains(want), std::string("preview: the right-click menu has ") + want, items.join(", ").toStdString());

    // the grid follows Preferences > Video's divisions
    app.prefs.gridColumns = 3;
    app.prefs.gridRows = 2;
    QSettings().setValue("preview/grid", true);
    app.notify(kPrefs);
    uitest::settle(50);
    uitest::shot(&pv, gOut + "/preview_grid_3x2.png");
    app.prefs.gridColumns = 4;
    app.prefs.gridRows = 4;
    pb.pause();
    uitest::settle(300);
}

void testMediaBin()
{
    AppState& app = AppState::get();
    MediaBin bin;
    bin.resize(900, 420);
    bin.show();
    uitest::settle(300);
    const int all = (int)app.project().media.size();
    check((int)bin.shownMedia().size() == all, "media: Project Media shows everything", std::to_string(bin.shownMedia().size()));
    check(kid<QTreeWidget>(&bin, "mediaBins") && kid<QTreeWidget>(&bin, "mediaBins")->topLevelItem(0)->text(0) == "Project Media", "media: the bins tree starts at Project Media");
    uitest::shot(&bin, gOut + "/media_detailed.png");

    // bins
    bin.newBin();
    uitest::settle(50);
    check(app.project().mediaBins.size() == 1 && app.project().mediaBins[0] == "New Bin" && bin.currentBin() == "bin:New Bin" && bin.shownMedia().empty(),
          "media: Create New Bin makes an empty bin and shows it");
    const uint64_t m0 = app.project().media[0].id, m1 = app.project().media[1].id;
    bin.moveToBin("bin:New Bin", {m0, m1});
    uitest::settle(50);
    check(app.project().findMedia(m0)->bin == "New Bin" && bin.shownMedia().size() == 2, "media: media moved into the bin shows there", std::to_string(bin.shownMedia().size()));
    bin.newBin();       // inside it
    uitest::settle(50);
    check(app.project().mediaBins.size() == 2 && app.project().mediaBins[1] == "New Bin/New Bin", "media: a bin inside a bin");
    uitest::shot(&bin, gOut + "/media_bins.png");
    bin.deleteCurrentBin();
    check(app.project().mediaBins.size() == 1, "media: Delete removes the bin");
    // a drop on a bin: what a drag from the list carries
    bin.showBin("all");
    uitest::settle(50);
    QTreeWidget* list = kid<QTreeWidget>(&bin, "mediaList");
    QMimeData* mime = list->model()->mimeData(QModelIndexList() << list->model()->index(2, 0) << list->model()->index(3, 0));
    check(mime && mime->hasFormat("application/x-reno-media-list") && QString::fromUtf8(mime->data("application/x-reno-media-list")).split(' ').size() == 2,
          "media: a drag of several media carries them all (for a bin)");
    delete mime;
    // by type
    bin.showBin("type:audio");
    uitest::settle(50);
    bool audioOnly = !bin.shownMedia().empty();
    for (uint64_t id : bin.shownMedia()) audioOnly = audioOnly && !app.project().findMedia(id)->info.hasVideo;
    check(audioOnly, "media: By Type > Audio shows the sound files");
    bin.showBin("type:image");
    uitest::settle(50);
    check(bin.shownMedia().size() == 1 && app.project().findMedia(bin.shownMedia()[0])->info.isStill, "media: By Type > Images shows the pictures");
    // search and smart bins
    bin.showBin("all");
    bin.setSearch("picture");
    uitest::settle(50);
    check(bin.shownMedia().size() == 1, "media: the search box finds by type and name", std::to_string(bin.shownMedia().size()));
    bin.saveSmartBin("Pictures");
    uitest::settle(50);
    check(app.project().smartBins.size() == 1 && bin.currentBin() == "smart:Pictures" && bin.shownMedia().size() == 1, "media: Save as Smart Bin keeps the search as a bin");
    uitest::shot(&bin, gOut + "/media_smart_bin.png");
    bin.deleteCurrentBin();
    check(app.project().smartBins.empty(), "media: a smart bin can be deleted");
    bin.showBin("all");

    // views
    bin.setView(2);
    uitest::settle(300);
    QListWidget* tiles = kid<QListWidget>(&bin, "mediaThumbnails");
    check(tiles && tiles->isVisible() && tiles->count() == all && !list->isVisible(), "media: Thumbnails view", std::to_string(tiles ? tiles->count() : -1));
    uitest::shot(&bin, gOut + "/media_thumbnails.png");
    bin.setView(1);
    uitest::settle(100);
    check(list->isVisible() && list->isColumnHidden(1), "media: List view shows the names only");
    uitest::shot(&bin, gOut + "/media_list.png");
    bin.setView(0);
    check(!list->isColumnHidden(4) && list->headerItem()->text(4) == "Tags", "media: Detailed view has the columns");

    // tags and comment through Properties
    list->clearSelection();
    list->topLevelItem(0)->setSelected(true);
    QString streams;
    drive([&](QWidget* m) {
        if (m->objectName() != "mediaProperties") return false;
        kid<QLineEdit>(m, "mediaTags")->setText("interview, day one");
        kid<QPlainTextEdit>(m, "mediaComment")->setPlainText("the good take");
        auto* tabs = kid<QTabWidget>(m, "mediaTabs");
        tabs->setCurrentIndex(1);
        uitest::shot(m, gOut + "/media_properties.png");
        streams = QString::number(tabs->count());
        kid<QPushButton>(m, "ok")->click();
        return true;
    }, "media properties");
    kid<QAbstractButton>(&bin, "mediaProperties")->click();
    const MediaItem* mi = app.project().findMedia(list->topLevelItem(0)->data(0, Qt::UserRole).toULongLong());
    check(mi && mi->tags.size() == 2 && mi->tags[1] == "day one" && mi->comment == "the good take", "media: Properties edits the tags and the comment");
    uitest::settle(50);
    check(list->topLevelItem(0)->text(4) == "interview, day one", "media: the Tags column shows them");
    bin.setSearch("interview");
    uitest::settle(50);
    check(bin.shownMedia().size() == 1, "media: the search finds tags");
    bin.setSearch("");

    // subclips
    const MediaItem* video = nullptr;
    for (const auto& m : app.project().media)
        if (m.info.hasVideo && !m.info.isStill && !m.generated) { video = &m; break; }
    check(video != nullptr, "media: there is a video file");
    if (video) {
        // by id: making subclips adds media, which can move the project's media list
        const uint64_t videoId = video->id;
        video = nullptr;
        const uint64_t sub = createSubclip(videoId, 2.0, 5.0, "Middle part");
        const MediaItem* s = app.project().findMedia(sub);
        check(s && std::fabs(s->subclipStart - 2.0) < 1e-9 && std::fabs(s->subclipLength - 3.0) < 1e-9 && std::fabs(s->info.duration - 3.0) < 1e-9 && s->name == "Middle part",
              "media: Create Subclip makes media of that part");
        const std::vector<uint64_t> made = app.addMediaToTimeline(sub, 20.0, 0);
        const Event* e = made.empty() ? nullptr : app.project().findEvent(made[0]);
        check(e && std::fabs(e->offset - 2.0) < 1e-6 && std::fabs(e->length - 3.0) < 0.05, "media: a subclip put on the timeline starts at its part", e ? strfmt("%.3f + %.3f", e->offset, e->length) : "none");
        const uint64_t subsub = createSubclip(sub, 1.0, 10.0, "");
        const MediaItem* ss = app.project().findMedia(subsub);
        check(ss && std::fabs(ss->subclipStart - 3.0) < 1e-9 && std::fabs(ss->subclipLength - 2.0) < 1e-9, "media: a subclip of a subclip stays inside it",
              ss ? strfmt("%.3f + %.3f", ss->subclipStart, ss->subclipLength) : "none");
        check(createSubclip(videoId, 3.0, 3.0, "x") == 0, "media: an empty range makes no subclip");
        // the Trimmer's selection, through the menu's Create Subclip
        trimmerSelection = [vid = videoId](uint64_t& m, double& in, double& out) { m = vid; in = 1.0; out = 1.5; return true; };
        uitest::settle(100);
        QStringList menuItems;
        bool asked = false;
        openInTrimmer = [](uint64_t, const QString&) {};
        drive([&](QWidget* m) {
            auto* menu = qobject_cast<QMenu*>(m);
            if (!menu) return false;
            menuItems = menuTexts(menu);
            uitest::shot(menu, gOut + "/media_menu.png");
            for (QAction* a : menu->actions())
                if (a->text().startsWith("Create Subclip")) {
                    // the name is asked for in a dialog of its own, answered by a second driver
                    drive([&](QWidget* d) {
                        auto* dlg = qobject_cast<QDialog*>(d);
                        if (!dlg) return false;
                        if (auto* e = dlg->findChild<QLineEdit*>()) e->setText("From the Trimmer");
                        asked = true;
                        dlg->accept();
                        return true;
                    }, "subclip name");
                    a->trigger();
                }
            menu->close();
            return true;
        }, "media menu subclip");
        list->clearSelection();
        list->topLevelItem(0)->setSelected(true);
        const QPoint at = list->visualItemRect(list->topLevelItem(0)).center();
        emit list->customContextMenuRequested(at);
        uitest::settle(300);
        for (const char* want : {"Open in Trimmer", "Select Timeline Events", "Explore Containing Folder", "Create Subclip from Trimmer Selection...", "Remove from Current Bin",
                                 "Remove from Project", "Rename", "Replace...", "Refresh", "Remove All Unused Media", "Properties..."})
            check(menuItems.contains(want), std::string("media: the right-click menu has ") + want, menuItems.join(", ").toStdString());
        bool fromTrimmer = false;
        for (const auto& m : app.project().media) fromTrimmer = fromTrimmer || (m.name == "From the Trimmer" && std::fabs(m.subclipLength - 0.5) < 1e-9);
        check(asked && fromTrimmer, "media: Create Subclip takes the Trimmer's selection");
        trimmerSelection = nullptr;
        openInTrimmer = nullptr;
    }

    // the project file keeps bins, tags, comments and subclips
    {
        Project copy;
        std::string err;
        check(copy.fromJson(app.project().toJson(), &err), "media: the project reads back", err);
        bool same = copy.mediaBins == app.project().mediaBins && copy.media.size() == app.project().media.size();
        for (size_t k = 0; same && k < copy.media.size(); ++k) {
            const MediaItem &x = copy.media[k], &y = app.project().media[k];
            same = x.bin == y.bin && x.tags == y.tags && x.comment == y.comment && x.subclipStart == y.subclipStart && x.subclipLength == y.subclipLength;
        }
        check(same, "media: bins, tags, comments and subclips are saved with the project (.reno)");
    }

    // Select Timeline Events
    bin.selectTimelineEvents({m0});
    bool allOfIt = !app.selection().empty();
    for (uint64_t id : app.selection()) allOfIt = allOfIt && app.project().findEvent(id)->mediaId == m0;
    check(allOfIt, "media: Select Timeline Events selects the events that use it", std::to_string(app.selection().size()));

    // Replace: every event that used it shows the new file
    std::string err;
    const uint64_t extra = app.importMedia("synthaudio:3", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(extra, 30.0, 0);
    check(bin.replaceMedia(extra, "synth:4"), "media: Replace takes another file");
    const MediaItem* rep = app.project().findMedia(extra);
    check(rep && rep->path == "synth:4" && rep->info.hasVideo && !placed.empty() && app.project().findEvent(placed[0])->mediaId == extra, "media: Replace keeps the events and changes the file");
    app.undo();
    app.undo();
    // Remove All Unused Media
    app.importMedia("synth:9", &err);
    app.importMedia("synthpic:9", &err);
    const size_t before = app.project().media.size();
    const int gone = bin.removeUnusedMedia();
    uitest::settle(50);
    std::set<uint64_t> used;
    for (const auto& t : app.project().tracks)
        for (const auto& e : t.events) used.insert(e.mediaId);
    bool onlyUsed = true;
    for (const auto& m : app.project().media) onlyUsed = onlyUsed && used.count(m.id);
    check(gone >= 2 && app.project().media.size() == before - (size_t)gone && onlyUsed, "media: Remove All Unused Media leaves only what events use", std::to_string(gone));
    app.undo();
    uitest::shot(&bin, gOut + "/media_after.png");
}

void testPropertiesAndPreferences()
{
    AppState& app = AppState::get();
    // Project Properties: output rotation, folders, start new projects with these settings
    drive([&](QWidget* m) {
        if (m->objectName() != "projectPropertiesDialog") return false;
        auto* rot = kid<QComboBox>(m, "outputRotation");
        rot->setCurrentIndex(rot->findData("90"));
        kid<QLineEdit>(m, "recordFolder")->setText(gOut + "/recordings");
        check(kid<QLabel>(m, "recordFolderSpace")->text().startsWith("Free storage space"), "properties: the free space in the folder is shown");
        kid<QCheckBox>(m, "startAllNewProjects")->setChecked(true);
        uitest::shot(m, gOut + "/project_properties_video.png");
        kid<QPushButton>(m, "ok")->click();
        return true;
    }, "project properties");
    runProjectPropertiesFull(nullptr);
    check(app.project().settings.outputRotation == 90, "properties: Output rotation is set");
    check(QDir::fromNativeSeparators(QString::fromStdString(app.prefs.recordFolder)) == gOut + "/recordings", "properties: the recorded files folder is set");
    check(!app.prefs.newProjectSettings.empty(), "properties: Start all new projects with these settings keeps them");

    // Preferences: preview display options and editing
    drive([&](QWidget* m) {
        if (m->objectName() != "preferencesDialog") return false;
        kid<QDoubleSpinBox>(m, "actionSafe")->setValue(15);
        kid<QSpinBox>(m, "gridColumns")->setValue(6);
        kid<QCheckBox>(m, "loopEvents")->setChecked(false);
        kid<QCheckBox>(m, "preservePitch")->setChecked(true);
        kid<QDoubleSpinBox>(m, "cursorPreview")->setValue(2.5);
        kid<QCheckBox>(m, "displayAtProjectSize")->setChecked(true);
        auto* tabs = kid<QTabWidget>(m, "tabs");
        for (int k = 0; k < tabs->count(); ++k)
            if (tabs->tabText(k) == "Video") tabs->setCurrentIndex(k);
        uitest::shot(m, gOut + "/preferences_video.png");
        kid<QPushButton>(m, "ok")->click();
        return true;
    }, "preferences");
    runPreferences(nullptr);
    check(app.prefs.actionSafe == 15 && app.prefs.gridColumns == 6 && !app.prefs.loopEvents && app.prefs.preservePitch && std::fabs(app.prefs.cursorPreview - 2.5) < 1e-9,
          "preferences: the new Video and Editing settings are kept");
    check(!QSettings().value("preview/scaleToFit", true).toBool(), "preferences: Display at project size is the preview's not-fitted view");
    {
        Preferences again;
        again.load();
        check(again.actionSafe == 15 && again.gridColumns == 6 && !again.loopEvents && std::fabs(again.cursorPreview - 2.5) < 1e-9, "preferences: and saved");
    }
    std::string err;
    const uint64_t snd = app.importMedia("synthaudio:5", &err);
    const std::vector<uint64_t> made = app.addMediaToTimeline(snd, 40.0, 0);
    const Event* e = made.empty() ? nullptr : app.project().findEvent(made[0]);
    check(e && !e->loop && e->keepPitch, "preferences: new events take Enable looping and Preserve pitch");
    app.prefs = Preferences();
    app.prefs.newProjectSettings = Project().toJson().dump();
    {
        Project d;
        d.settings.width = 1280;
        d.settings.height = 720;
        d.settings.outputRotation = 270;
        app.prefs.newProjectSettings = d.toJson().dump();
    }
    app.newProject();
    check(app.project().settings.width == 1280 && app.project().settings.outputRotation == 270, "properties: a new project starts with the kept settings");
    app.prefs.newProjectSettings.clear();
    app.newProject();
    check(app.project().settings.width == 1920 && app.project().settings.outputRotation == 0, "properties: ... and with Reno's own when none are kept");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoPreviewMediaTest");
    QApplication::setApplicationName("preview_media_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: preview_media_test <output folder> [file.ofx ...]"); return 2; }
    gOut = QDir(args[1]).absolutePath();
    QDir().mkpath(gOut);
    QSettings().clear();
    std::vector<std::string> ofxFiles;
    for (int k = 2; k < args.size(); ++k) ofxFiles.push_back(args[k].toStdString());
    uitest::setup(ofxFiles, std::vector<std::string>());
    uitest::buildDemoProject();

    testPreview();
    testMediaBin();
    testPropertiesAndPreferences();
    const int rc = uitest::summary();
    std::fflush(stdout);
    std::_Exit(rc);
}
