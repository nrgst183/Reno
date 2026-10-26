// Project Media's Create Video Proxy and Match Project Video Settings (right-click a file), and image sequences
// (AppState::importImageSequence, as File > Import > Image Sequence), driven through the Project Media window without a screen.
//
//   media_tools_test <output folder>
#include "../../src/app/panels.h"
#include "../../src/engine/engine.h"
#include "uitestlib.h"
#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QTabWidget>
#include <QListWidget>
#include <QMenu>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <filesystem>

using namespace reno;
using namespace reno::app;
using uitest::check;

static QString gOut;

static bool makeVideo(const std::string& path)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = 1280;
    es.height = 720;
    es.fpsNum = 25;
    es.fpsDen = 1;
    std::string err;
    auto out = createMediaWriter(es, &err);
    if (!out) return false;
    for (int f = 0; f < 25; ++f) {
        ImagePtr img = Image::make(1280, 720, PixelDepth::Byte, false);
        for (int y = 0; y < 720; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < 1280; ++x, d += 4) { d[0] = (uint8_t)(x / 5); d[1] = (uint8_t)(y / 3); d[2] = (uint8_t)(f * 10); d[3] = 255; }
        }
        if (!out->writeVideo(*img)) return false;
    }
    return out->finish(&err);
}

// The Project Media list's row of media 'id' (in whichever view shows), selected.
static bool selectMedia(MediaBin& bin, uint64_t id, QWidget*& view, QPoint& pos)
{
    for (QTreeWidget* t : bin.findChildren<QTreeWidget*>()) {
        if (!t->isVisible() || !(t->contextMenuPolicy() == Qt::CustomContextMenu)) continue;
        for (QTreeWidgetItemIterator it(t); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toULongLong() == id || (*it)->data(0, Qt::UserRole + 1).toULongLong() == id) {
                t->clearSelection();
                (*it)->setSelected(true);
                t->setCurrentItem(*it);
                view = t;
                pos = t->visualItemRect(*it).center();
                return true;
            }
    }
    return false;
}

// Right-clicks there and picks 'text'; 'seen' gets the menu's items.
static bool menuPick(QWidget* tree, QPoint pos, const QString& text, QStringList* seen = nullptr, const QString& shot = QString())
{
    bool found = false;
    QTimer::singleShot(150, [&] {
        QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m)
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QMenu* mm = qobject_cast<QMenu*>(t))
                    if (mm->isVisible()) m = mm;
        if (!m) return;
        if (!shot.isEmpty()) m->grab().save(shot);
        QAction* pick = nullptr;
        for (QAction* a : m->actions()) {
            if (seen) *seen << a->text();
            if (!pick && a->text() == text && a->isEnabled()) pick = a;
        }
        if (pick) {
            found = true;
            pick->trigger();
        }
        m->close();
    });
    QTreeWidget* t = qobject_cast<QTreeWidget*>(tree);
    emit t->customContextMenuRequested(pos);
    uitest::settle(400);
    return found;
}

static void writePicture(const std::string& path, int level)
{
    std::string s = "P6\n32 18\n255\n";
    for (int k = 0; k < 32 * 18; ++k) { s += (char)level; s += (char)(255 - level); s += (char)level; }
    writeFile(path, s);
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("media_tools_test");
    QApplication::setStyle("Fusion");
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("media_tools_out");
    std::filesystem::create_directories(gOut.toStdString());
    uitest::setup({}, {});
    AppState& app = AppState::get();
    app.newProject();
    const std::string video = gOut.toStdString() + "/clip 720p.mp4";
    std::filesystem::remove(videoProxyPath(video));
    check(makeVideo(video), "setup: a 1280 x 720, 25 fps video");
    std::string err;
    const uint64_t id = app.importMedia(video, &err);
    check(id != 0, "setup: it is in Project Media", err);

    MediaBin bin;
    bin.resize(700, 400);
    bin.show();
    uitest::settle(300);
    int proxiesMade = 0;
    onProxiesMade = [&] { ++proxiesMade; };
    QWidget* view = nullptr;
    QPoint pos;
    check(selectMedia(bin, id, view, pos), "the file is listed and selected");

    // ---- Create Video Proxy
    if (view) {
        QStringList seen;
        const bool picked = menuPick(view, pos, "Create Video Proxy", &seen, gOut + "/media_menu.png");
        check(seen.contains("Create Video Proxy") && seen.contains("Match Project Video Settings"), "the right-click menu has Create Video Proxy and Match Project Video Settings",
              seen.join(" | ").toStdString());
        for (int k = 0; k < 100 && !hasVideoProxy(video); ++k) uitest::settle(100);
        check(picked && hasVideoProxy(video), "Create Video Proxy makes the <file>.sfvp0 proxy");
        check(proxiesMade == 1, "and tells the main window (the preview shows it)");
        auto r = openMedia(videoProxyPath(video), &err);
        check(r && r->info().width == 960 && r->info().height == 540, "the proxy is 960 x 540", r ? strfmt("%dx%d", r->info().width, r->info().height) : err);
    }
    uitest::shot(&bin, gOut + "/media_bin.png");

    // ---- Match Project Video Settings
    check(app.project().settings.width == 1920 && std::fabs(app.project().settings.fps() - 25) > 0.5, "setup: a new project is 1920 x 1080, not 25 fps",
          strfmt("%dx%d %.3f", app.project().settings.width, app.project().settings.height, app.project().settings.fps()));
    if (selectMedia(bin, id, view, pos)) {
        menuPick(view, pos, "Match Project Video Settings");
        const ProjectSettings& s = app.project().settings;
        check(s.width == 1280 && s.height == 720 && s.fpsNum == 25 && s.fpsDen == 1, "Match Project Video Settings: 1280 x 720 at 25 fps",
              strfmt("%dx%d %d/%d", s.width, s.height, s.fpsNum, s.fpsDen));
        app.undo();
        check(app.project().settings.width == 1920, "one undo step");
    }

    // ---- Properties > Media > Alpha channel
    if (selectMedia(bin, id, view, pos)) {
        bool sawCombo = false;
        QTimer::singleShot(700, [&] {
            for (QWidget* t : QApplication::topLevelWidgets()) {
                if (t->objectName() != "mediaProperties" || !t->isVisible()) continue;
                if (auto* tabs = t->findChild<QTabWidget*>("mediaTabs")) tabs->setCurrentIndex(1);
                if (QComboBox* c = t->findChild<QComboBox*>("mediaAlpha")) {
                    sawCombo = c->count() == 4 && c->currentData().toString().isEmpty();
                    c->setCurrentIndex(c->findData("premultiplied"));
                    uitest::settle(50);
                    uitest::shot(t, gOut + "/media_properties_alpha.png");
                }
                if (QPushButton* ok = t->findChild<QPushButton*>("ok")) ok->click();
            }
        });
        menuPick(view, pos, "Properties...");
        uitest::settle(900);
        const MediaItem* m = app.project().findMedia(id);
        check(sawCombo && m && m->alphaMode == "premultiplied", "Properties > Media > Alpha channel (Undefined at first) set to Premultiplied",
              m ? m->alphaMode : "");
        app.undo();
        check(app.project().findMedia(id) && app.project().findMedia(id)->alphaMode.empty(), "one undo step");
    }

    // ---- Properties > Media > Field order, Pixel aspect ratio, Rotation (the file says progressive, square pixels, not turned)
    if (selectMedia(bin, id, view, pos)) {
        bool sawBoxes = false;
        QTimer::singleShot(700, [&] {
            for (QWidget* t : QApplication::topLevelWidgets()) {
                if (t->objectName() != "mediaProperties" || !t->isVisible()) continue;
                if (auto* tabs = t->findChild<QTabWidget*>("mediaTabs")) tabs->setCurrentIndex(1);
                QComboBox* f = t->findChild<QComboBox*>("mediaFieldOrder");
                QComboBox* pa = t->findChild<QComboBox*>("mediaPixelAspect");
                QComboBox* ro = t->findChild<QComboBox*>("mediaRotation");
                if (f && pa && ro) {
                    sawBoxes = f->currentData().toString() == "none" && std::fabs(pa->currentData().toDouble() - 1.0) < 0.001 && ro->currentData().toInt() == 0;
                    f->setCurrentIndex(f->findData("upper"));
                    pa->setCurrentIndex(pa->findData(0.9091));
                    ro->setCurrentIndex(ro->findData(90));
                    uitest::settle(50);
                    uitest::shot(t, gOut + "/media_properties_overrides.png");
                }
                if (QPushButton* ok = t->findChild<QPushButton*>("ok")) ok->click();
            }
        });
        menuPick(view, pos, "Properties...");
        uitest::settle(900);
        const MediaItem* m = app.project().findMedia(id);
        check(sawBoxes && m && m->fieldOrderOverride == "upper" && std::fabs(m->pixelAspectOverride - 0.9091) < 1e-6 && m->rotationOverride == 90,
              "Properties > Media: Field order, Pixel aspect ratio and Rotation start as the file says and can be set",
              m ? strfmt("%s %.4f %d", m->fieldOrderOverride.c_str(), m->pixelAspectOverride, m->rotationOverride) : "");
        // back to what the file says: nothing is overridden
        QTimer::singleShot(700, [&] {
            for (QWidget* t : QApplication::topLevelWidgets()) {
                if (t->objectName() != "mediaProperties" || !t->isVisible()) continue;
                if (QComboBox* f = t->findChild<QComboBox*>("mediaFieldOrder")) f->setCurrentIndex(f->findData("none"));
                if (QComboBox* pa = t->findChild<QComboBox*>("mediaPixelAspect")) pa->setCurrentIndex(pa->findData(1.0));
                if (QComboBox* ro = t->findChild<QComboBox*>("mediaRotation")) ro->setCurrentIndex(ro->findData(0));
                if (QPushButton* ok = t->findChild<QPushButton*>("ok")) ok->click();
            }
        });
        menuPick(view, pos, "Properties...");
        uitest::settle(900);
        m = app.project().findMedia(id);
        check(m && m->fieldOrderOverride.empty() && m->pixelAspectOverride == 0 && m->rotationOverride == 0, "set back to what the file says: no override kept",
              m ? strfmt("%s %.4f %d", m->fieldOrderOverride.c_str(), m->pixelAspectOverride, m->rotationOverride) : "");
        app.undo();
        app.undo();
    }

    // ---- an image sequence
    const std::string dir = gOut.toStdString() + "/frames";
    std::filesystem::create_directories(dir);
    for (int k = 1; k <= 12; ++k) writePicture(dir + strfmt("/shot_%03d.ppm", k), k * 20);
    const uint64_t seq = app.importImageSequence(dir + "/shot_001.ppm", 12, &err);
    const MediaItem* sm = app.project().findMedia(seq);
    check(sm && sm->sequenceFps == 12 && sm->name == "shot_###.ppm" && sm->info.hasVideo && !sm->info.isStill && std::fabs(sm->info.duration - 1.0) < 0.1,
          "an image sequence: one video named shot_###.ppm, 12 pictures at 12 fps = 1 s", sm ? strfmt("%s %.3f s", sm->name.c_str(), sm->info.duration) : err);
    check(app.importImageSequence(dir + "/shot_001.ppm", 12, &err) == seq, "imported again: the same media");
    check(app.importImageSequence(dir + "/shot_012.ppm", 12, &err) == 0 && !err.empty(), "the last picture alone is no sequence", err);
    uitest::settle(300);
    bool listed = false;
    for (QTreeWidget* t : bin.findChildren<QTreeWidget*>())
        for (QTreeWidgetItemIterator it(t); *it; ++it) listed = listed || (*it)->text(0) == "shot_###.ppm";
    check(listed, "and Project Media lists it");
    uitest::shot(&bin, gOut + "/media_bin_sequence.png");
    std::filesystem::remove(videoProxyPath(video));
    onProxiesMade = nullptr;
    return uitest::summary();
}
