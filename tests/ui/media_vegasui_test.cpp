// Project Media as VEGAS Pro 18 has it (manual pages 68-73, 80-83, 92-93), and the Compositors window: the bins tree
// (All Media, Media Bins, By Type, Tags, Smart Bins, Storyboard Bins), the Main Timeline storyboard (cells in time order,
// out of step after a timeline edit, Refresh Storyboard and automatic synchronizing, Alt+drag rearranging the timeline),
// the Media Tags panel (Add tags box, chips, Quick Tags) and View > Window > Compositors. Screenshots go to the output
// folder, to compare with the manual's pictures (p0031, p0093, p0070, p0080, p0081, p0035).
//
//   media_vegasui_test <output folder>
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/panels_storyboard.h"
#include "../../src/engine/engine.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <filesystem>

using namespace reno;
using namespace reno::app;
using uitest::check;

static QString gOut;

static QStringList topLevel(QTreeWidget* t)
{
    QStringList out;
    for (int k = 0; t && k < t->topLevelItemCount(); ++k) out << t->topLevelItem(k)->text(0);
    return out;
}

static QTreeWidgetItem* findItem(QTreeWidget* t, const QString& text)
{
    for (QTreeWidgetItemIterator it(t); *it; ++it)
        if ((*it)->text(0) == text) return *it;
    return nullptr;
}

static QStringList childrenOf(QTreeWidgetItem* it)
{
    QStringList out;
    for (int k = 0; it && k < it->childCount(); ++k) out << it->child(k)->text(0);
    return out;
}

// Video events of the clips' track (the video track with the most events), by start time: their media names.
static QStringList timelineOrder()
{
    QStringList out;
    const Project& p = AppState::get().project();
    const Track* most = nullptr;
    for (const Track& t : p.tracks)
        if (t.type == TrackType::Video && (!most || t.events.size() > most->events.size())) most = &t;
    for (const Track& t : p.tracks)
        if (&t == most) {
            std::vector<const Event*> ev;
            for (const Event& e : t.events) ev.push_back(&e);
            std::sort(ev.begin(), ev.end(), [](const Event* a, const Event* b) { return a->start < b->start; });
            for (const Event* e : ev)
                if (const MediaItem* m = p.findMedia(e->mediaId)) out << QString::fromStdString(m->name);
            break;
        }
    return out;
}

static bool selectInList(QTreeWidget* list, const std::vector<uint64_t>& ids)
{
    list->clearSelection();
    int n = 0;
    for (int k = 0; k < list->topLevelItemCount(); ++k)
        if (std::find(ids.begin(), ids.end(), list->topLevelItem(k)->data(0, Qt::UserRole).toULongLong()) != ids.end()) {
            list->topLevelItem(k)->setSelected(true);
            ++n;
        }
    uitest::settle(50);
    return n == (int)ids.size();
}

static void testBinsAndTags()
{
    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    std::vector<uint64_t> ids;
    for (int k = 1; k <= 6; ++k) ids.push_back(app.importMedia(strfmt("synth:%d", k), &err));
    app.edit("Tags", [&](Project& p) {
        p.findMedia(ids[1])->tags = {"Animals"};
        p.findMedia(ids[3])->tags = {"Landscape"};
        p.findMedia(ids[4])->tags = {"People", "Animals"};
    });
    MediaBin bin;
    bin.resize(660, 456);
    bin.setMediaTagsVisible(false);
    bin.setView(2);
    bin.show();
    uitest::settle(300);
    QTreeWidget* bins = bin.findChild<QTreeWidget*>("mediaBins");
    check(bins && topLevel(bins) == QStringList({"All Media", "Media Bins", "By Type", "Tags", "Smart Bins", "Storyboard Bins"}),
          "bins tree: All Media, Media Bins, By Type, Tags, Smart Bins, Storyboard Bins at the top (VEGAS 18)", topLevel(bins).join(", ").toStdString());
    check(bin.currentBin() == "all" && bins && bins->currentItem() && bins->currentItem()->text(0) == "All Media", "All Media is shown at first");
    check(childrenOf(findItem(bins, "Tags")) == QStringList({"Animals", "Landscape", "People"}), "Tags: a bin for each tag used",
          childrenOf(findItem(bins, "Tags")).join(", ").toStdString());
    check(childrenOf(findItem(bins, "By Type")).size() == 4, "By Type: Video, Audio, Images, Generated Media");
    check(childrenOf(findItem(bins, "Storyboard Bins")) == QStringList({"Main Timeline"}), "Storyboard Bins: Main Timeline");
    bin.showBin("tag:Animals");
    std::vector<uint64_t> shown = bin.shownMedia();
    check(shown.size() == 2 && shown[0] == ids[1] && shown[1] == ids[4], "a tag's bin shows the media with that tag", std::to_string(shown.size()));
    bin.showBin("bins");
    bin.newBin();
    check(bin.currentBin() == "bin:New Bin" && childrenOf(findItem(bins, "Media Bins")) == QStringList({"New Bin"}), "Create New Bin on Media Bins: the bin is under it",
          bin.currentBin().toStdString());
    bin.moveToBin("tag:Landscape", {ids[0]});
    check(app.project().findMedia(ids[0])->tags == std::vector<std::string>({"Landscape"}), "media dropped on a tag's bin gets the tag");
    bin.showBin("all");
    uitest::settle(600);
    uitest::shot(&bin, gOut + "/bins_p0031.png");

    // ---- Media Tags panel
    QToolButton* views = bin.findChild<QToolButton*>("mediaView");
    QStringList viewItems;
    QAction* toggle = nullptr;
    if (views && views->menu())
        for (QAction* a : views->menu()->actions()) {
            viewItems << a->text();
            if (a->text() == "Media Tags") toggle = a;
        }
    check(viewItems == QStringList({"List", "Details", "Thumbnails", "Media Tags"}) && toggle && toggle->isCheckable(),
          "Views menu: List, Details, Thumbnails, Media Tags (a toggle)", viewItems.join(", ").toStdString());
    MediaTagsPanel* tags = dynamic_cast<MediaTagsPanel*>(bin.findChild<QWidget*>("mediaTagsPanel"));
    check(tags && !tags->isVisible(), "the Media Tags panel is hidden at first");
    if (toggle) toggle->trigger();
    uitest::settle(50);
    check(tags && tags->isVisible() && bin.mediaTagsVisible(), "Views > Media Tags shows it");
    QLineEdit* add = bin.findChild<QLineEdit*>("addTags");
    check(add && !add->isEnabled() && add->placeholderText() == "Select files to edit tags", "with nothing selected the box says to select files");
    bin.setView(0);
    QTreeWidget* list = bin.findChild<QTreeWidget*>("mediaList");
    uitest::shot(&bin, gOut + "/tags_empty_p0093.png");
    check(list && selectInList(list, {ids[1], ids[4]}), "two media selected");
    check(tags && tags->chips() == QStringList({"Animals", "People*"}), "chips: the tags of the selected media (People only on some: partial)",
          tags ? tags->chips().join(", ").toStdString() : "");
    if (add && tags) {
        add->setFocus();
        add->setText("tracy");
        uitest::key(add, Qt::Key_Return);
        uitest::settle(50);
        check(app.project().findMedia(ids[1])->tags.size() == 2 && app.project().findMedia(ids[4])->tags.back() == "tracy", "Enter puts the typed tag on the selected media");
        check(tags->chips() == QStringList({"Animals", "tracy", "People*"}) || tags->chips().contains("tracy"), "and it shows as a chip", tags->chips().join(", ").toStdString());
        // Ctrl+Shift+1 in the box gives its text to Quick Tag 1
        add->setText("damon");
        uitest::key(add, Qt::Key_1, Qt::ControlModifier | Qt::ShiftModifier);
        check(MediaTagsPanel::quickTag(1) == "damon", "Ctrl+Shift+1 assigns the box's text to Ctrl+1", MediaTagsPanel::quickTag(1).toStdString());
        add->clear();
        QLabel* q1 = bin.findChild<QLabel*>("quickTag1");
        check(q1 && q1->text() == "damon", "the Quick Tags area shows it next to Ctrl+1");
        // Ctrl+1 in the media list puts it on the selected media
        uitest::key(list->viewport(), Qt::Key_1, Qt::ControlModifier);
        uitest::key(list, Qt::Key_1, Qt::ControlModifier);
        uitest::settle(50);
        const auto& t1 = app.project().findMedia(ids[1])->tags;
        check(std::find(t1.begin(), t1.end(), "damon") != t1.end(), "Ctrl+1 in the media list adds Quick Tag 1");
        MediaTagsPanel::setQuickTag(2, "tracy");
        MediaTagsPanel::setQuickTag(3, "michael");
        tags->setMedia({ids[1], ids[4]});
        uitest::settle(50);
        uitest::shot(&bin, gOut + "/tags_p0070.png");
        uitest::shot(tags, gOut + "/tags_panel_p0070.png");
        // the x of the "tracy" chip takes it off both
        int at = (int)tags->chips().indexOf("tracy");
        if (at >= 0) uitest::mouseClick(tags->chipArea(), tags->chipCloseRect(at).center());
        uitest::settle(50);
        const auto& t4 = app.project().findMedia(ids[4])->tags;
        check(at >= 0 && std::find(t4.begin(), t4.end(), "tracy") == t4.end() && !tags->chips().contains("tracy"), "the x on a chip removes the tag", tags->chips().join(", ").toStdString());
        // Backspace in the empty box removes the last chip's tag
        const int before = (int)tags->chips().size();
        add->setFocus();
        uitest::key(add, Qt::Key_Backspace);
        check((int)tags->chips().size() == before - 1, "Backspace in the empty box removes the last tag", tags->chips().join(", ").toStdString());
        check(childrenOf(findItem(bins, "Tags")).contains("damon"), "a new tag gets its bin under Tags");
    }
    bin.setMediaTagsVisible(false);
}

static void testStoryboard()
{
    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    std::vector<uint64_t> media;
    for (int k = 1; k <= 5; ++k) media.push_back(app.importMedia(strfmt("synth:%d", k), &err));
    media.push_back(app.importMedia("synthpic:7", &err));
    // five clips of 4 seconds one after another on one track (their sound grouped below), and a picture on a track above
    double at = 0;
    for (int k = 0; k < 5; ++k) {
        const std::vector<uint64_t> made = app.addMediaToTimeline(media[(size_t)k], at);
        app.edit("Length", [&](Project& p) {
            for (uint64_t id : made)
                if (Event* e = p.findEvent(id)) e->length = 4.0;
        });
        at += 4.0;
    }
    app.addMediaToTimeline(media[5], 2.0);
    QSettings().setValue("media/storyboardAutoSync", false);
    MediaBin bin;
    bin.resize(481, 309);
    bin.show();
    uitest::settle(200);
    bin.showBin("storyboard:main");
    uitest::settle(400);
    StoryboardView* board = dynamic_cast<StoryboardView*>(bin.findChild<QWidget*>("storyboard"));
    check(board && board->isVisible() && !bin.findChild<QTreeWidget*>("mediaList")->isVisible(), "Main Timeline shows the storyboard instead of the list");
    std::vector<uint64_t> cells = bin.storyboardEvents();
    check(cells.size() == 6, "a cell per event (a clip's picture and sound are one)", std::to_string(cells.size()));
    QStringList names;
    for (uint64_t ev : cells)
        if (const Event* e = app.project().findEvent(ev)) names << QString::fromStdString(app.project().findMedia(e->mediaId)->name);
    check(names.size() == 6 && names[0] == "synth:1" && names[1] == "synthpic:7" && names[2] == "synth:2", "the cells are in time order", names.join(", ").toStdString());
    QToolButton* refresh = bin.findChild<QToolButton*>("refreshStoryboard");
    check(refresh && refresh->isVisible() && refresh->menu() && refresh->menu()->actions().size() == 1 &&
              refresh->menu()->actions()[0]->text() == "Automatically synchronize Main Timeline storyboard" && refresh->menu()->actions()[0]->isCheckable(),
          "Refresh Storyboard button, its arrow: Automatically synchronize Main Timeline storyboard");
    uitest::settle(600);
    uitest::shot(&bin, gOut + "/storyboard_p0080.png");

    // an edit on the timeline: out of step (red), and no changes until refreshed
    app.edit("Move", [&](Project& p) {
        for (Track& t : p.tracks)
            for (Event& e : t.events.mut())
                if (e.mediaId == media[5]) e.start = 30.0;
    });
    uitest::settle(50);
    check(!bin.storyboardInSync(), "a timeline edit leaves the storyboard out of step");
    uitest::shot(&bin, gOut + "/storyboard_outofstep_p0081.png");
    int asked = 0;
    board->askRefresh = [&] { ++asked; return true; };
    const bool moved = bin.moveStoryboardCell(3, 0);
    check(!moved && asked == 1 && bin.storyboardInSync(), "a change while out of step is refused, with the offer to refresh");
    cells = bin.storyboardEvents();
    check(cells.size() == 6 && app.project().findEvent(cells.back())->mediaId == media[5], "after the refresh the picture is the last cell");

    // Alt+drag the third cell (synth:3) onto the first: it goes first, the others move right and close its gap
    QWidget* canvas = board->canvas();
    const QPoint from = board->cellRect(2).center(), to = board->cellRect(0).topLeft() + QPoint(8, 20);
    uitest::mouseDrag(canvas, from, to, 8, Qt::AltModifier);
    uitest::settle(50);
    check(timelineOrder() == QStringList({"synth:3", "synth:1", "synth:2", "synth:4", "synth:5"}), "Alt+drag rearranges the events on the timeline",
          timelineOrder().join(", ").toStdString());
    bool contiguous = true;
    {
        double end = 0;
        for (const Track& t : app.project().tracks)
            if (t.type == TrackType::Video && t.events.size() == 5) {
                for (const Event& e : t.events) {
                    if (std::fabs(e.start - end) > 1e-6) contiguous = false;
                    end = e.start + e.length;
                }
            }
    }
    check(contiguous, "and leaves no gap and no overlap");
    bool audioFollows = true;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Audio)
            for (const Event& e : t.events)
                for (const Track& v : app.project().tracks)
                    if (v.type == TrackType::Video)
                        for (const Event& ve : v.events)
                            if (ve.group && ve.group == e.group && std::fabs(ve.start - e.start) > 1e-6) audioFollows = false;
    check(audioFollows, "the clips' sound moves with them");
    check(bin.storyboardInSync() && app.project().findEvent(bin.storyboardEvents()[0])->mediaId == media[2], "the storyboard shows the new order");
    // with automatic synchronizing, a timeline edit is shown at once
    bin.setStoryboardAutoSync(true);
    app.edit("Move", [&](Project& p) {
        for (Track& t : p.tracks)
            for (Event& e : t.events.mut())
                if (e.mediaId == media[5]) e.start = 0.5;
    });
    uitest::settle(50);
    check(bin.storyboardInSync() && app.project().findEvent(bin.storyboardEvents()[1])->mediaId == media[5], "Automatically synchronize: the storyboard follows the timeline");
    // media dropped onto the end goes after the last cell
    const size_t n = bin.storyboardEvents().size();
    board->placeMedia({media[0]}, 100, 0);
    check(bin.storyboardEvents().size() == n + 1 && app.project().findEvent(bin.storyboardEvents().back())->mediaId == media[0], "media dropped past the last cell is added at the end");
    bin.setStoryboardAutoSync(false);
    uitest::settle(400);
    uitest::shot(&bin, gOut + "/storyboard_after.png");
    app.newProject();
    uitest::settle(50);
    check(bin.storyboardInSync() && bin.storyboardEvents().empty(), "another project: its storyboard, in step");
}

static void testCompositors()
{
    CompositorsWindow w;
    w.resize(488, 452);
    w.show();
    uitest::settle(100);
    const QStringList all = w.listed("all");
    check(all == QStringList({"Bump Map", "Displacement Map", "Height Map", "Layer Dimensionality"}), "Compositors: VEGAS's four compositing plug-ins under All",
          all.join(", ").toStdString());
    QTreeWidget* tree = w.findChild<QTreeWidget*>("compositorTree");
    check(tree && topLevel(tree) == QStringList({"All", "OFX", "32-bit floating point", "GPU Accelerated", "VEGAS", "Third Party"}), "folders as VEGAS's",
          topLevel(tree).join(", ").toStdString());
    check(w.listed("vegas").size() == 4 && w.listed("thirdparty").isEmpty(), "all of them under VEGAS, none under Third Party");
    QLineEdit* search = w.findChild<QLineEdit*>("searchBox");
    check(search && search->placeholderText() == "Search plug-ins", "the search box");
    check(w.pick("Bump Map") && w.presetCount() >= 1, "picking Bump Map shows its presets", std::to_string(w.presetCount()));
    check(w.description().startsWith("VEGAS Bump Map: OFX, 32-bit floating point, GPU Accelerated"), "the description line", w.description().toStdString());
    std::string activated;
    w.onActivated = [&](const std::string& data) { activated = data; };
    if (QTreeWidgetItem* it = tree ? tree->currentItem() : nullptr) emit tree->itemActivated(it, 0);
    check(activated == "com.vegascreativesoftware:bumpmap\nVEGAS Bump Map", "double-click: added as a video effect", activated);
    uitest::settle(1500);
    uitest::shot(&w, gOut + "/compositors_p0035.png");
    w.setSearch("height");
    check(w.listed("all") == QStringList({"Height Map"}), "searching", w.listed("all").join(", ").toStdString());
}

static void testWindowMenu()
{
    AppState::get().newProject();
    MainWindow w;
    w.resize(1400, 860);
    w.show();
    uitest::settle(300);
    QAction* item = nullptr;
    for (QAction* top : w.menuBar()->actions())
        if (top->text().remove('&') == "View" && top->menu()) {
            emit top->menu()->aboutToShow();
            for (QAction* a : top->menu()->actions())
                if (a->menu() && a->text().remove('&') == "Window") {
                    emit a->menu()->aboutToShow();
                    for (QAction* b : a->menu()->actions())
                        if (b->text().split('\t')[0].remove('&') == "Compositors") item = b;
                }
        }
    check(item && item->isEnabled() && !item->property("vegasUnavailable").toBool(), "View > Window > Compositors is a real item, not a placeholder");
    check(item && item->shortcut().isEmpty() && !item->text().contains('\t'), "with no shortcut (as VEGAS 18)");
    QDockWidget* dock = w.findChild<QDockWidget*>("dockCompositors");
    if (item) item->trigger();
    uitest::settle(300);
    check(dock && dock->isVisible() && dynamic_cast<CompositorsWindow*>(dock->findChild<QWidget*>("compositors")), "it opens the Compositors window");
    if (dock) uitest::shot(dock, gOut + "/compositors_dock.png");
    uitest::shot(&w, gOut + "/mainwindow_compositors.png");
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("media_vegasui_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("media_vegasui_out");
    std::filesystem::create_directories(gOut.toStdString());
    uitest::setup({}, {});
    testBinsAndTags();
    testStoryboard();
    testCompositors();
    testWindowMenu();
    return uitest::summary();
}
