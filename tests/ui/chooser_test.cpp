// Plug-In Chooser (src/app/pluginchooser.cpp), plug-in packages and presets (src/app/fxlibrary.cpp), and the effects window's
// Plug-In Chain button, Preset row, transitions and media generators.
//
//   chooser_test <output folder> <testfx.ofx> [VEGAS PresetPackage.xml to read]
//
// The test effect is copied into a bundle with its own Presets/PresetPackage.xml. Prints PASS / FAIL lines and a summary.
#include "../../src/app/fxlibrary.h"
#include "../../src/app/fxthumbs.h"
#include "../../src/app/panels.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/core/base.h"
#include "../../src/engine/catalog.h"
#include "../../src/ofx/ofxhost.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QDropEvent>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>
#include <algorithm>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

const char* kBundlePresets = R"(<?xml version="1.0" encoding="UTF-8"?>
<OfxPresetPackage>
<OfxPreset plugin="reno.test.params" context="Filter" name="Bundle Preset A">
    <OfxPlugin>reno.test.params</OfxPlugin>
    <OfxPluginVersion>1 0</OfxPluginVersion>
    <OfxParamTypeInteger name="p_int">
      <OfxParamValue>7</OfxParamValue>
    </OfxParamTypeInteger>
    <OfxParamTypeDouble name="p_dbl">
      <OfxParamValue>0.25</OfxParamValue>
    </OfxParamTypeDouble>
    <OfxParamTypeRGBA name="p_rgba">
      <OfxParamValue>0.100000 0.200000 0.300000 0.400000</OfxParamValue>
    </OfxParamTypeRGBA>
    <OfxParamTypeString name="p_str">
      <OfxParamValue>from a preset</OfxParamValue>
    </OfxParamTypeString>
    <OfxParamTypeDouble name="no_such_param">
      <OfxParamValue>3</OfxParamValue>
    </OfxParamTypeDouble>
</OfxPreset>
<OfxPreset plugin="reno.test.params" context="Transition" name="Wrong Context">
    <OfxPlugin>reno.test.params</OfxPlugin>
</OfxPreset>
<OfxPreset plugin="reno.test.invert" context="Filter" name="Other Effect">
    <OfxPlugin>reno.test.invert</OfxPlugin>
</OfxPreset>
</OfxPresetPackage>
)";

// A VEGAS user preset (Documents\OFX Presets).
const char* kVegasUserPreset = R"(<?xml version="1.0" encoding="UTF-8"?>
<OfxPresetPackage><OfxPreset plugin="reno.test.params" context="Filter" name="Saved In VEGAS">
<OfxPlugin>reno.test.params</OfxPlugin><OfxPluginVersion>1 0</OfxPluginVersion>
<OfxParamTypeInteger name="p_int"><OfxParamValue>3</OfxParamValue></OfxParamTypeInteger>
</OfxPreset></OfxPresetPackage>)";

// Runs 'drive' on the next chooser that is shown (it must close it).
void onNextChooser(std::function<void(PluginChooser*)> drive)
{
    PluginChooser::onShown = [drive](PluginChooser* c) {
        const std::function<void(PluginChooser*)> run = drive;  // copied first: clearing onShown destroys this lambda
        PluginChooser::onShown = nullptr;
        run(c);
    };
}

QStringList chainNames(const FxChain* c)
{
    QStringList out;
    if (c)
        for (const FxSlot& s : *c) out << QString::fromStdString(s.name);
    return out;
}

std::string str(const QStringList& l) { return l.join(", ").toStdString(); }

double paramValue(uint64_t slot, const char* name, int k = 0)
{
    ofx::Instance* inst = AppState::get().fx.video(slot);
    if (!inst) return -999;
    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
    ofx::Param* p = inst->params.find(name);
    if (!p) return -998;
    const std::vector<double> v = p->valueAt(0);
    return k < (int)v.size() ? v[(size_t)k] : -997;
}

std::string paramText(uint64_t slot, const char* name)
{
    ofx::Instance* inst = AppState::get().fx.video(slot);
    if (!inst) return "?";
    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
    ofx::Param* p = inst->params.find(name);
    return p ? p->str : std::string("?");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoChooserTest");
    QApplication::setApplicationName("chooser_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) { qWarning("usage: chooser_test <output folder> <testfx.ofx> [PresetPackage.xml]"); return 2; }
    gOut = QDir(args[1]).absolutePath();
    QDir(gOut).removeRecursively();
    QDir().mkpath(gOut);

    // test effect in its own bundle, with presets
    const QString bundle = gOut + "/Test.ofx.bundle/Contents";
    QDir().mkpath(bundle + "/Linux-x86-64");
    QDir().mkpath(bundle + "/Presets");
    const QString binary = bundle + "/Linux-x86-64/testfx.ofx";
    QFile::copy(args[2], binary);
    writeFile((bundle + "/Presets/PresetPackage.xml").toStdString(), kBundlePresets);
    setUserPresetDir((gOut + "/user presets").toStdString());
    setVegasPresetDir((gOut + "/Documents/OFX Presets").toStdString());
    QDir().mkpath(gOut + "/Documents/OFX Presets/reno.test.params/Filter");
    writeFile((gOut + "/Documents/OFX Presets/reno.test.params/Filter/Saved In VEGAS.xml").toStdString(), kVegasUserPreset);
    setPackageRoot((gOut + "/packages").toStdString());
    std::vector<std::string> vsts;
    for (int k = 3; k + 1 < args.size(); ++k)
        if (args[k] == "--vst") vsts.push_back(args[k + 1].toStdString());
    uitest::setup({binary.toStdString()}, vsts);
    AppState& app = AppState::get();

    // ================================================================================ preset files
    {
        std::vector<OfxPreset> ps = parsePresets(kBundlePresets);
        check(ps.size() == 3, "presets: three read from a package", std::to_string(ps.size()));
        if (ps.size() == 3) {
            check(ps[0].name == "Bundle Preset A" && ps[0].pluginId == "reno.test.params" && ps[0].context == "Filter" && ps[0].version == "1 0", "presets: name, plug-in, context and version");
            check(ps[0].values.size() == 5 && ps[0].values[2].numbers.size() == 4 && std::fabs(ps[0].values[2].numbers[3] - 0.4) < 1e-9, "presets: an RGBA value is four numbers");
            check(ps[0].values[3].text == "from a preset", "presets: a string value is text");
            // written and read back
            const std::vector<OfxPreset> again = parsePresets(writePreset(ps[0]));
            check(again.size() == 1 && again[0].values.size() == 5 && again[0].values[3].text == "from a preset" && again[0].values[0].numbers == std::vector<double>{7},
                  "presets: written as VEGAS writes them and read back");
        }
        check(parsePresets("<OfxPresetPackage><OfxPreset plugin=\"x\" name=\"a\"><OfxParamTypeDouble name=\"q\"><OfxParamValue>nan junk").size() == 0,
              "presets: broken XML gives no half-read preset");
        check(parsePresets("").empty() && parsePresets("not xml at all").empty(), "presets: nonsense gives none");
        const std::vector<OfxPreset> forParams = presetsFor("reno.test.params", "Filter");
        QStringList names;
        for (const OfxPreset& p : forParams) names << QString::fromStdString(p.name);
        check(names == QStringList({"Bundle Preset A", "Saved In VEGAS"}), "presets: the effect's own (its bundle) then VEGAS's user presets, by plug-in and context", str(names));
        if (args.size() > 3 && !args[3].startsWith("--")) {
            // VEGAS Pro 18's own OpenFX presets
            std::string xml;
            readFile(args[3].toStdString(), xml);
            const std::vector<OfxPreset> v = parsePresets(xml);
            size_t values = 0;
            for (const OfxPreset& p : v) values += p.values.size();
            check(v.size() > 600 && values > 4000, "presets: VEGAS's Vfx1 PresetPackage.xml reads", std::to_string(v.size()) + " presets, " + std::to_string(values) + " values");
        }
        std::string error;
        OfxPreset clash = forParams.empty() ? OfxPreset() : forParams[0];
        clash.user = true;
        check(!saveUserPreset(clash, &error) && error.find("part of the plug-in") != std::string::npos, "presets: a user preset cannot take the name of one of the effect's own", error);
        check(!deleteUserPreset(forParams[0], &error), "presets: the effect's own cannot be deleted");
        if (forParams.size() > 1) check(!deleteUserPreset(forParams[1], &error), "presets: VEGAS's are left alone", error);
        check(safeFileName("a/b:c*?") == "a_b_c__" && safeFileName("  ") == "Untitled", "file names made safe");
    }

    // ================================================================================ the chooser on its own
    {
        PluginChooser c(ChooserKind::VideoFx, "Plug-In Chooser - Video Event FX", FxChain());
        c.show();
        uitest::settle(100);
        const QStringList f = c.folders();
        check(f.contains("FX") && f.contains("VEGAS") && f.contains("Third Party") && f.contains("OFX") && f.contains("Reno Test") &&
                  f.contains("Filter Packages") && !f.contains("All"),
              "chooser: VEGAS's folders (FX, then Filter Packages, OFX, the OFX groupings, Third Party, VEGAS)", str(f));
        QTreeWidget* tree = c.findChild<QTreeWidget*>("chooserFolders");
        check(tree && tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->text(0) == "FX", "chooser: FX at the top of the folders");
        {
            QStringList kids;
            for (int k = 0; tree && k < tree->topLevelItem(0)->childCount(); ++k) kids << tree->topLevelItem(0)->child(k)->text(0);
            QStringList sorted = kids;
            std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
            check(!kids.isEmpty() && kids == sorted, "chooser: FX's folders in alphabetical order, as VEGAS has them", str(kids));
        }
        check(c.openFolder("All") && c.listed().contains("Test Invert") && c.listed().contains("Test Parameters") && !c.listed().contains("Test Generator") &&
                  !c.listed().contains("Test Dissolve"),
              "chooser: FX (the top) lists the video effects only", str(c.listed()));
        check(c.listedFolders().contains("OFX") && c.listedFolders().contains("Filter Packages") && c.listedFolders().contains("VEGAS"),
              "chooser: ...after its folders, as VEGAS lists them", str(c.listedFolders()));
        {
            // VEGAS look: big window, list in columns running down then across, buttons down the right
            QListWidget* list = c.findChild<QListWidget*>("chooserList");
            QComboBox* pathBox = c.findChild<QComboBox*>("chooserPath");
            QPushButton* okb = c.findChild<QPushButton*>("chooserOk");
            QWidget* chainBox = c.findChild<QWidget*>("chooserChainBox");
            const QSize room = c.screen() ? c.screen()->availableGeometry().size() - QSize(40, 80) : QSize(1340, 760);
            check(c.width() >= std::min(1300, room.width()) && c.height() >= std::min(700, room.height()), "chooser: opens big, as VEGAS's does",
                  strfmt("%dx%d (room for %dx%d)", c.width(), c.height(), room.width(), room.height()));
            check(list && list->viewMode() == QListView::ListMode && list->flow() == QListView::TopToBottom && list->isWrapping(),
                  "chooser: the list flows top to bottom, then left to right, in columns");
            check(list && list->item(0) && list->item(0)->data(Qt::UserRole + 1).toInt() < 0, "chooser: folders first in the list");
            check(pathBox && pathBox->currentText().trimmed() == "FX", "chooser: the path box shows the folder", pathBox ? pathBox->currentText().toStdString() : "");
            check(okb && list && okb->mapTo(&c, QPoint(0, 0)).x() > list->mapTo(&c, QPoint(list->width(), 0)).x(), "chooser: OK down the right");
            check(chainBox && list && chainBox->mapTo(&c, QPoint(0, 0)).y() < list->mapTo(&c, QPoint(0, 0)).y(), "chooser: the chain across the top");
            for (const char* n : {"chooserUp", "chooserNewFolder", "chooserDelete", "chooserView", "chooserUndo", "chooserRedo"})
                check(c.findChild<QToolButton*>(n) != nullptr, std::string("chooser: the tool ") + n);
            for (const char* n : {"chooserOk", "chooserCancel", "chooserAdd", "chooserRemove", "chooserSaveAs", "chooserSaveLut"})
                check(c.findChild<QPushButton*>(n) != nullptr, std::string("chooser: the button ") + n);
            // double-click on a folder opens it
            for (int k = 0; list && k < list->count(); ++k)
                if (list->item(k)->text() == "OFX") {
                    emit list->itemActivated(list->item(k));
                    break;
                }
            check(tree->currentItem() && tree->currentItem()->text(0) == "OFX" && pathBox->currentText().trimmed() == "OFX", "chooser: a folder in the list opens with a double-click");
            c.findChild<QToolButton*>("chooserUp")->click();
            check(tree->currentItem() && tree->currentItem()->text(0) == "FX", "chooser: Up One Level back to FX");
            // the path box goes to a folder
            const int at = pathBox->findText("Third Party", Qt::MatchContains);
            if (at >= 0) {
                pathBox->setCurrentIndex(at);
                emit pathBox->activated(at);
            }
            check(tree->currentItem() && tree->currentItem()->text(0) == "Third Party", "chooser: the path box opens a folder");
            uitest::shot(&c, gOut + "/chooser_vegas_look.png");
            c.openFolder("All");
            // list longer than the column: plug-ins continue in the next column, list scrolls sideways
            const QSize was = c.size();
            c.resize(1300, 360);
            uitest::settle(100);
            QListWidgetItem* firstItem = list->item(0);
            QListWidgetItem* lastItem = list->item(list->count() - 1);
            check(firstItem && lastItem && list->visualItemRect(lastItem).x() > list->visualItemRect(firstItem).x() + 50 &&
                      list->visualItemRect(list->item(1)).y() > list->visualItemRect(firstItem).y(),
                  "chooser: down the first column, then on in the next");
            uitest::shot(&c, gOut + "/chooser_columns.png");
            c.resize(was);
            uitest::settle(50);
        }
        c.openFolder("VEGAS");
        check(!c.listed().contains("Test Invert") && c.listed().contains("Invert"), "chooser: the test effects are not VEGAS's own (VEGAS's are: Reno's built-in stand-ins)", str(c.listed()));
        c.openFolder("Third Party");
        check(c.listed().contains("Test Invert"), "chooser: ...they are Third Party");
        c.openFolder("All");
        c.findChild<QLineEdit*>("chooserSearch")->setText("inv");
        check(c.listed() == QStringList({"Invert", "Test Invert"}), "chooser: search", str(c.listed()));
        c.findChild<QLineEdit*>("chooserSearch")->clear();
        QPushButton* ok = c.findChild<QPushButton*>("chooserOk");
        QLabel* none = c.findChild<QLabel*>("chooserChainEmpty");
        check(none && none->isVisible() && none->text() == "No plug-ins in chain", "chooser: an empty chain says so");
        check(c.addByLabel("Test Invert") && c.addByLabel("Test Parameters"), "chooser: Add");
        FxChain added = c.chain();
        check(chainNames(&added) == QStringList({"Test Invert", "Test Parameters"}), "chooser: the chain in the order added", str(chainNames(&added)));
        check(!none->isVisible(), "chooser: the empty-chain line goes");
        // the last added is picked: move it left, then remove it
        c.findChild<QToolButton*>("chooserLeft")->click();
        FxChain ch = c.chain();
        check(chainNames(&ch) == QStringList({"Test Parameters", "Test Invert"}), "chooser: Move Plug-In Left", str(chainNames(&ch)));
        check(!c.findChild<QToolButton*>("chooserLeft")->isEnabled() && c.findChild<QToolButton*>("chooserRight")->isEnabled(), "chooser: the moves follow the place");
        uitest::shot(&c, gOut + "/chooser_video.png");
        // package of the two
        check(c.savePackageAs("Two Effects", nullptr), "chooser: Save As... saves a package");
        check(QFile::exists(gOut + "/packages/Video FX/Two Effects.json"), "chooser: ...in the packages folder");
        c.openFolder("Filter Packages");
        check(c.listed() == QStringList({"Two Effects"}), "chooser: Filter Packages lists it", str(c.listed()));
        uitest::shot(&c, gOut + "/chooser_packages.png");
        c.findChild<QPushButton*>("chooserRemove")->click();
        ch = c.chain();
        check(chainNames(&ch) == QStringList({"Test Invert"}), "chooser: Remove Selected Plug-In", str(chainNames(&ch)));
        // chain undo and redo
        c.findChild<QToolButton*>("chooserUndo")->click();
        ch = c.chain();
        check(chainNames(&ch) == QStringList({"Test Parameters", "Test Invert"}), "chooser: the chain's Undo", str(chainNames(&ch)));
        c.findChild<QToolButton*>("chooserRedo")->click();
        ch = c.chain();
        check(chainNames(&ch) == QStringList({"Test Invert"}), "chooser: the chain's Redo", str(chainNames(&ch)));
        // double-click adds a plug-in
        {
            c.openFolder("All");
            QListWidget* list = c.findChild<QListWidget*>("chooserList");
            for (int k = 0; k < list->count(); ++k)
                if (list->item(k)->text() == "Test Parameters") {
                    emit list->itemActivated(list->item(k));
                    break;
                }
            ch = c.chain();
            check(chainNames(&ch) == QStringList({"Test Invert", "Test Parameters"}), "chooser: a double-click adds the plug-in to the chain", str(chainNames(&ch)));
            c.findChild<QToolButton*>("chooserUndo")->click();
        }
        c.addByLabel("Two Effects");
        ch = c.chain();
        check(chainNames(&ch) == QStringList({"Test Invert", "Test Parameters", "Test Invert"}), "chooser: adding a package adds its effects", str(chainNames(&ch)));
        check(ok->isEnabled(), "chooser: OK");
        // Up One Level from a grouping goes to OFX
        c.openFolder("Reno Test");
        c.findChild<QToolButton*>("chooserUp")->click();
        QTreeWidget* folders = c.findChild<QTreeWidget*>("chooserFolders");
        check(folders->currentItem() && folders->currentItem()->text(0) == "FX", "chooser: Up One Level", folders->currentItem() ? folders->currentItem()->text(0).toStdString() : "");
        // drag a plug-in from the list into the chain, before the first
        {
            c.openFolder("All");
            QListWidget* list = c.findChild<QListWidget*>("chooserList");
            QWidget* row = c.findChild<QWidget*>("chooserChain");
            check(row && row->acceptDrops(), "chooser: the chain row takes drops");
            int number = -1;
            for (int k = 0; k < list->count(); ++k)
                if (list->item(k)->text() == "Test Misc") number = list->item(k)->data(Qt::UserRole + 1).toInt();
            QMimeData md;
            md.setData("application/x-reno-chooser-plugin", QByteArray::number(number));
            QDragEnterEvent enter(QPoint(2, 10), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(row, &enter);
            QDropEvent drop(QPointF(2, 10), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(row, &drop);
            ch = c.chain();
            check(!ch.empty() && ch.front().name == "Test Misc" && ch.size() == 4, "chooser: a plug-in dropped into the chain goes where it is dropped", str(chainNames(&ch)));
            // and a chain plug-in dragged to the end
            QMimeData mv;
            mv.setData("application/x-reno-chooser-chain", QByteArray::number(0));
            QDragEnterEvent enter2(QPoint(row->width() - 4, 10), Qt::MoveAction, &mv, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(row, &enter2);
            QDropEvent move(QPointF(row->width() - 4, 10), Qt::MoveAction, &mv, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(row, &move);
            ch = c.chain();
            check(ch.size() == 4 && ch.back().name == "Test Misc", "chooser: dragging a plug-in along the chain reorders it", str(chainNames(&ch)));
            QMimeData junk;
            junk.setData("application/x-reno-chooser-plugin", QByteArray("99999"));
            QDragEnterEvent enter3(QPoint(2, 10), Qt::CopyAction, &junk, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(row, &enter3);
            QDropEvent bad(QPointF(2, 10), Qt::CopyAction, &junk, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(row, &bad);
            check(c.chain().size() == 4, "chooser: a drop of nothing known is ignored");
        }
        c.findChild<QToolButton*>("chooserView")->click();
        c.openFolder("All");
        uitest::shot(&c, gOut + "/chooser_icons.png");
        check(c.findChild<QListWidget*>("chooserList")->viewMode() == QListView::IconMode, "chooser: the large icon view");
        c.close();
    }
    {
        PluginChooser g(ChooserKind::Generator, "Plug-In Chooser - New Media Generator", FxChain());
        g.show();
        uitest::settle(50);
        g.openFolder("All");
        check(g.listed().contains("Test Generator") && g.listed().contains("Solid Color") && !g.listed().contains("Test Invert"), "generator chooser: generators only", str(g.listed()));
        check(!g.findChild<QWidget*>("chooserChainBox")->isVisibleTo(&g) && !g.findChild<QPushButton*>("chooserAdd")->isVisibleTo(&g), "generator chooser: one plug-in, no chain");
        check(g.folders().contains("Media Generator Packages"), "generator chooser: Media Generator Packages", str(g.folders()));
        uitest::shot(&g, gOut + "/chooser_generator.png");
        PluginChooser t(ChooserKind::Transition, "Plug-In Chooser - Transition", FxChain());
        t.openFolder("All");
        // Reno's built-in transitions are listed with the scanned ones
        check(t.listed().contains("Test Dissolve") && t.listed().contains("Cross Effect") && !t.listed().contains("Test Invert") && !t.listed().contains("Test Generator"),
              "transition chooser: transitions only", str(t.listed()));
    }
    if (!vsts.empty()) {
        PluginChooser a(ChooserKind::AudioFx, "Plug-In Chooser - Audio Track FX", FxChain());
        a.show();
        uitest::settle(50);
        const QStringList f = a.folders();
        check(f.contains("FX") && f.contains("VST2") && f.contains("FX Packages") && f.contains("Third Party"), "audio chooser: formats, makers, FX Packages", str(f));
        a.openFolder("All");
        check(!a.listed().isEmpty() && !a.listed().contains("Test Invert"), "audio chooser: audio plug-ins only", str(a.listed()));
        uitest::shot(&a, gOut + "/chooser_audio.png");
    }
    {
        FxPackage p;
        check(!packageFromText("{}", p) && !packageFromText("junk", p), "packages: bad text is refused");
        check(packageFromText("{\"reno\":\"plug-in package\",\"name\":\"x\",\"plugins\":[{\"kind\":\"ofx\"},5,{\"kind\":\"bad\"}]}", p), "packages: a package with bad entries reads");
        check(p.chain.empty(), "packages: entries without a plug-in are left out");
    }

    // ================================================================================ the Video FX window: presets and packages
    std::string presetDrop;
    {
        FxBrowser b(FxBrowser::VideoFx);
        b.resize(420, 640);
        b.show();
        uitest::settle(100);
        QTreeWidget* tree = b.findChild<QTreeWidget*>("fxTree");
        QListWidget* thumbs = b.findChild<QListWidget*>("fxPresetThumbs");
        check(tree && thumbs, "Video FX window: the plug-ins and the presets list");
        QTreeWidgetItem* params = nullptr;
        QTreeWidgetItem* packs = nullptr;
        QTreeWidgetItem* invert = nullptr;
        if (tree)
            for (int g = 0; g < tree->topLevelItemCount(); ++g) {
                QTreeWidgetItem* grp = tree->topLevelItem(g);
                if (grp->text(0).startsWith("Filter Packages")) packs = grp;
                for (int k = 0; k < grp->childCount(); ++k) {
                    if (grp->child(k)->text(0) == "Test Parameters") params = grp->child(k);
                    if (grp->child(k)->text(0) == "Test Invert") invert = grp->child(k);
                }
            }
        check(packs && packs->childCount() == 1 && packs->child(0)->text(0) == "Two Effects" && tree->indexOfTopLevelItem(packs) == tree->topLevelItemCount() - 1,
              "Video FX window: Filter Packages, last");
        if (params && thumbs) {
            tree->setCurrentItem(params);
            uitest::settle(600);
            QStringList names;
            for (int k = 0; k < thumbs->count(); ++k) names << thumbs->item(k)->text();
            check(names == QStringList({"(Default)", "Bundle Preset A", "Saved In VEGAS"}), "Video FX window: the picked effect's presets", str(names));
            const QImage def = fxThumbnail("reno.test.params", "Filter", Json(), QSize(96, 54), 0.5);
            const std::vector<OfxPreset> ps = presetsFor("reno.test.params", "Filter");
            const QImage a = ps.empty() ? QImage() : fxThumbnail("reno.test.params", "Filter", presetToState(ps[0]), QSize(96, 54), 0.5);
            check(!def.isNull() && !a.isNull() && def.size() == QSize(96, 54), "thumbnails: drawn by the effect");
            check(def != a, "thumbnails: a preset's differs from the defaults'");
            uitest::shot(&b, gOut + "/videofx_presets.png");
            def.save(gOut + "/thumb_default.png");
            a.save(gOut + "/thumb_presetA.png");
            presetDrop = thumbs->count() > 1 ? thumbs->item(1)->data(Qt::UserRole).toString().toStdString() : std::string();
            check(isFxPackageData(presetDrop), "thumbnails: a preset carries a package of the effect with its settings");
        }
        if (invert && thumbs) {
            tree->setCurrentItem(invert);
            uitest::settle(300);
            check(thumbs->count() == 2 && thumbs->item(0)->text() == "(Default)" && thumbs->item(1)->text() == "Other Effect",
                  "Video FX window: another effect, its presets from the same bundle");
            const QImage inv = fxThumbnail("reno.test.invert", "Filter", Json(), QSize(96, 54), 0.5);
            const QImage pic = samplePicture(QSize(96, 54), 2.0, 0);
            const QRgb x = inv.isNull() ? 0 : inv.pixel(10, 5), y = pic.pixel(10, 5);
            check(!inv.isNull() && std::abs(qRed(x) - (255 - qRed(y))) < 8 && std::abs(qBlue(x) - (255 - qBlue(y))) < 8, "thumbnails: Invert inverts the test picture",
                  strfmt("%08x vs %08x", (unsigned)x, (unsigned)y));
        }
    }
    {
        std::string error;
        const QImage bad = fxThumbnail("no.such.plugin", "Filter", Json(), QSize(96, 54), 0.5, &error);
        check(bad.isNull() && !error.empty(), "thumbnails: a plug-in that is not there gives none", error);
        PluginChooser t(ChooserKind::Transition, "x", FxChain());
        const QImage tr0 = fxThumbnail("reno.test.transition", "Transition", Json(), QSize(96, 54), 0.0);
        const QImage tr1 = fxThumbnail("reno.test.transition", "Transition", Json(), QSize(96, 54), 1.0);
        check(!tr0.isNull() && !tr1.isNull() && tr0 != tr1, "thumbnails: a transition goes from one picture to the other");
        tr0.save(gOut + "/thumb_transition0.png");
        tr1.save(gOut + "/thumb_transition1.png");
    }

    // ================================================================================ in the editor
    uitest::buildDemoProject();
    MainWindow w;
    w.resize(1500, 900);
    w.show();
    uitest::settle(300);
    FxWindow* fxw = w.fxWindow();
    // a video event with no effects
    std::vector<uint64_t> bare;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Video)
            for (const Event& e : t.events)
                if (e.fx.empty()) bare.push_back(e.id);
    const uint64_t ev = bare.size() > 0 ? bare[0] : 0, ev2 = bare.size() > 1 ? bare[1] : 0;
    check(ev && ev2, "editor: two video events without effects");
    FxTarget target;
    target.kind = FxTarget::Event;
    target.id = ev;

    // Cancel: nothing added, the effects window not opened
    fxw->hide();
    bool shown = false;
    QString title;
    onNextChooser([&](PluginChooser* c) {
        shown = true;
        title = c->windowTitle();
        c->reject();
    });
    w.openFx(target, 0);
    uitest::settle(100);
    check(shown && title.startsWith("Plug-In Chooser - Video Event FX"), "FX button on an event without effects: the Plug-In Chooser", title.toStdString());
    check(!fxw->isVisible() && app.chainOf(app.project(), target)->empty(), "Cancel: no effects, no effects window");

    // OK with two effects: the effects window shows the chain
    onNextChooser([&](PluginChooser* c) {
        c->openFolder("All");
        c->addByLabel("Test Invert");
        c->addByLabel("Test Parameters");
        uitest::shot(c, gOut + "/chooser_event.png");
        c->findChild<QPushButton*>("chooserOk")->click();
    });
    w.openFx(target, 0);
    uitest::settle(200);
    const FxChain* chain = app.chainOf(app.project(), target);
    check(chainNames(chain) == QStringList({"Test Invert", "Test Parameters"}), "OK: the chain is on the event", str(chainNames(chain)));
    check(fxw->isVisible() && fxw->target() == target, "OK: then the effects window, showing it");
    check(app.undoName() == "Plug-In Chain", "one undo step", app.undoName());
    uitest::shot(fxw, gOut + "/fxwindow_after_chooser.png");

    // with effects the FX button goes straight to the window
    shown = false;
    PluginChooser::onShown = [&](PluginChooser* c) { shown = true; c->reject(); };
    w.openFx(target, 0);
    uitest::settle(50);
    check(!shown, "FX button on an event with effects: straight to the effects window");
    PluginChooser::onShown = nullptr;

    // the effects window's Plug-In Chain button: the chooser with the chain in it
    QToolButton* chainBtn = fxw->findChild<QToolButton*>("fxPluginChain");
    check(chainBtn && chainBtn->isVisible() && chainBtn->isEnabled(), "effects window: the Plug-In Chain button");
    QStringList seen;
    onNextChooser([&](PluginChooser* c) {
        FxChain ch = c->chain();
        seen = chainNames(&ch);
        c->findChild<QPushButton*>("chooserRemove")->click();  // the first is picked
        c->findChild<QPushButton*>("chooserOk")->click();
    });
    if (chainBtn) chainBtn->click();
    uitest::settle(150);
    chain = app.chainOf(app.project(), target);
    check(seen == QStringList({"Test Invert", "Test Parameters"}), "Plug-In Chain: the chooser starts with the chain", str(seen));
    check(chainNames(chain) == QStringList({"Test Parameters"}), "Plug-In Chain: what OK leaves is the chain", str(chainNames(chain)));
    const uint64_t paramsSlot = chain && !chain->empty() ? chain->front().id : 0;
    app.undo();
    uitest::settle(100);
    check(chainNames(app.chainOf(app.project(), target)) == QStringList({"Test Invert", "Test Parameters"}), "Plug-In Chain: undone in one step");
    app.redo();
    uitest::settle(100);

    // ================================================================================ presets in the effects window
    fxw->showTarget(target, paramsSlot);
    uitest::settle(200);
    QComboBox* preset = fxw->findChild<QComboBox*>("fxPreset");
    QWidget* presetRow = fxw->findChild<QWidget*>("fxPresetRow");
    check(preset && presetRow && presetRow->isVisible(), "presets: the Preset row over an OpenFX effect");
    if (preset) {
        QStringList items;
        for (int k = 0; k < preset->count(); ++k)
            if (!preset->itemText(k).isEmpty()) items << preset->itemText(k);
        check(items == QStringList({"(Default)", "Bundle Preset A", "Saved In VEGAS"}), "presets: (Default), the effect's own, the user's", str(items));
        const int a = preset->findText("Bundle Preset A");
        preset->setCurrentIndex(a);
        preset->activated(a);
        uitest::settle(150);
        check(paramValue(paramsSlot, "p_int") == 7 && std::fabs(paramValue(paramsSlot, "p_dbl") - 0.25) < 1e-9 && std::fabs(paramValue(paramsSlot, "p_rgba", 2) - 0.3) < 1e-6 &&
                  paramText(paramsSlot, "p_str") == "from a preset",
              "presets: choosing one sets the effect's values", strfmt("p_int %g p_dbl %g", paramValue(paramsSlot, "p_int"), paramValue(paramsSlot, "p_dbl")));
        check(app.undoName() == "Preset Bundle Preset A", "presets: one undo step", app.undoName());
        uitest::shot(fxw, gOut + "/fxwindow_preset.png");
        app.undo();
        uitest::settle(100);
        check(paramValue(paramsSlot, "p_int") != 7, "presets: undone");
        app.redo();
        uitest::settle(100);
        // save the settings as a user preset
        preset->setEditText("My Look");
        fxw->findChild<QToolButton*>("fxPresetSave")->click();
        uitest::settle(100);
        check(QFile::exists(gOut + "/user presets/reno.test.params/Filter/My Look.xml"), "presets: Save Preset writes the user's preset");
        check(preset->findText("My Look") >= 0 && preset->currentText() == "My Look", "presets: ...and lists it");
        check(fxw->findChild<QToolButton*>("fxPresetDelete")->isEnabled(), "presets: a user preset can be deleted");
        // (Default) restores the defaults
        const int def = preset->findText("(Default)");
        preset->setCurrentIndex(def);
        preset->activated(def);
        uitest::settle(100);
        check(paramValue(paramsSlot, "p_int") != 7 && paramText(paramsSlot, "p_str") != "from a preset", "presets: (Default)");
        const int mine = preset->findText("My Look");
        preset->setCurrentIndex(mine);
        preset->activated(mine);
        uitest::settle(100);
        check(paramValue(paramsSlot, "p_int") == 7, "presets: the saved preset brings its values back");
        fxw->findChild<QToolButton*>("fxPresetDelete")->click();
        uitest::settle(100);
        check(!QFile::exists(gOut + "/user presets/reno.test.params/Filter/My Look.xml") && preset->findText("My Look") < 0, "presets: Delete Preset");
        const int vegas = preset->findText("Saved In VEGAS");
        preset->setCurrentIndex(vegas);
        check(!fxw->findChild<QToolButton*>("fxPresetDelete")->isEnabled(), "presets: VEGAS's own user presets are not deleted from Reno");
    }

    // ================================================================================ Insert > Generated Media
    {
        const size_t mediaBefore = app.project().media.size();
        onNextChooser([&](PluginChooser* c) {
            title = c->windowTitle();
            c->openFolder("All");
            c->addByLabel("Test Generator");
            c->findChild<QPushButton*>("chooserOk")->click();
        });
        QAction* gen = w.findChild<QAction*>("generatedMedia");
        check(gen != nullptr, "Insert > Generated Media is there");
        if (gen) gen->trigger();
        uitest::settle(250);
        check(title == "Plug-In Chooser - New Media Generator", "Generated Media: VEGAS's New Media Generator chooser", title.toStdString());
        check(app.project().media.size() == mediaBefore + 1 && app.project().media.back().generated && app.project().media.back().generator.pluginId == "reno.test.generator",
              "Generated Media: the media is made");
        check(!app.selection().empty(), "Generated Media: put on the timeline (and selected)");
        check(fxw->isVisible() && fxw->target().kind == FxTarget::Generator && fxw->windowTitle() == "Video Media Generator: Test Generator",
              "Generated Media: its Media Generator window", fxw->windowTitle().toStdString());
        check(!fxw->findChild<QToolButton*>("fxAdd")->isVisible() && fxw->findChild<QToolButton*>("fxPluginChain")->toolTip() == "Replace Plug-In...",
              "Media Generator window: one plug-in (Replace Plug-In instead of Add)");
        uitest::shot(fxw, gOut + "/fxwindow_generator.png");
    }

    // ================================================================================ a transition's window
    uint64_t evx = 0;
    {
        // two clips overlapping by two seconds on their own track
        std::string err;
        const uint64_t vtrack = app.addTrack(TrackType::Video);
        const uint64_t m = app.importMedia("synth:3", &err);
        app.addMediaToTimeline(m, 40.0, vtrack);
        app.edit("Overlap", [&](Project& p) {
            for (Track& tr : p.tracks)
                if (tr.id == vtrack && !tr.events.empty()) {
                    Event c = tr.events.front();
                    c.id = p.newId();
                    c.group = 0;
                    c.start += 8.0;
                    tr.events.push_back(c);
                }
        });
    }
    for (const Track& tr : app.project().tracks)
        if (tr.type == TrackType::Video)
            for (const Event& b : tr.events)
                for (const Event& a : tr.events)
                    if (a.id != b.id && a.start < b.start && a.end() > b.start && a.end() < b.end()) evx = b.id;
    check(evx != 0, "editor: an event overlapping the one before it");
    {
        app.setTransition(evx, "reno.test.transition", "Test Dissolve");
        FxTarget tt;
        tt.kind = FxTarget::Transition;
        tt.id = evx;
        w.openFx(tt, 0);
        uitest::settle(200);
        check(fxw->windowTitle() == "Transition: Test Dissolve", "transition window: its title", fxw->windowTitle().toStdString());
        check(fxw->findChild<QWidget*>("fxOfxPanel") != nullptr, "transition window: the transition's controls");
        uitest::shot(fxw, gOut + "/fxwindow_transition.png");
        // Transition Progress envelope: offered on an event with a transition, 0% to 100% over the overlap
        EnvelopeOwner eo;
        eo.kind = EnvelopeOwner::Event;
        eo.id = evx;
        bool offered = false;
        for (const EnvelopeChoice& c : app.envelopeChoices(eo))
            if (c.ref.kind == "transition" && c.name == "Transition Progress") offered = true;
        check(offered, "Transition Progress envelope: offered for an event with a transition");
        EnvelopeRef er;
        er.owner = eo;
        er.kind = "transition";
        check(app.insertEnvelope(er), "Transition Progress envelope: inserted");
        const Envelope* env = AppState::findEnvelope(app.project(), er);
        const Event* e2 = app.project().findEvent(evx);
        double overlapEnd = 0;
        for (const Track& tr : app.project().tracks)
            for (const Event& o : tr.events)
                if (e2 && o.id != evx && o.start < e2->start && o.end() > e2->start) overlapEnd = std::max(overlapEnd, o.end());
        check(env && env->points.size() == 2 && env->points[0].value == 0.0 && env->points[1].value == 1.0 && e2 &&
                  std::fabs(env->points[1].time - (overlapEnd - e2->start)) < 1e-9,
              "Transition Progress envelope: 0% at the start of the overlap, 100% at its end",
              env ? strfmt("%d points", (int)env->points.size()) : std::string("none"));
        app.undo();
        fxw->findChild<QToolButton*>("fxRemove")->click();
        uitest::settle(150);
        const Event* e = app.project().findEvent(evx);
        check(e && e->transition.kind.empty(), "transition window: Remove makes it a plain cross-fade again");
        bool found = false;
        for (QLabel* l : fxw->findChildren<QLabel*>("fxMessage"))
            if (l->isVisible() && l->text().contains("cross-fade")) found = true;
        check(found, "transition window: says so");
    }
    // ================================================================================ a preset dropped on an event
    if (!presetDrop.empty()) {
        FxTarget t3;
        t3.kind = FxTarget::Event;
        t3.id = ev2;
        const uint64_t s3 = addFxData(t3, presetDrop);
        uitest::settle(150);
        check(s3 && paramValue(s3, "p_int") == 7 && paramText(s3, "p_str") == "from a preset", "a preset's thumbnail dropped on an event: the effect with the preset's settings");
        check(app.undoName() == "Add Effect", "...one undo step", app.undoName());
    }
    return uitest::summary();
}
