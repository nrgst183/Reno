// Window layouts: the built-in ones, saved and numbered layouts, and the Layouts menu.
#include "audioproxyui.h"
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "vprefs.h"
#include "prefsapply.h"
#include "vegasextras.h"
#include "man2dialogs.h"
#include "outputcmds.h"
#include "startup.h"
#include "recovery.h"
#include "../engine/timefmt.h"
#include "pluginchooser.h"
#include "timeline.h"
#include "fxwindow.h"
#include "mixer.h"
#include "editdialogs.h"
#include "mainwindow_tools.h"
#include "../vegas/vegas.h"
#include "renderdialog.h"
#include "keyboard.h"
#include "trackdefaults.h"
#include "smalldialogs.h"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include "automationctl.h"
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace reno {
namespace app {

// ================================================================================================ window layouts
QString MainWindow::builtinLayoutName(BuiltinLayout which)
{
    switch (which) {
    case LargePreviewLayout: return "Large Preview Layout";
    case AudioMixingLayout: return "Audio Mixing Layout";
    default: return "Default Layout";
    }
}

void MainWindow::applyLayout(BuiltinLayout which)
{
    if (timelineMaximized_) {
        timelineMaximized_ = false;     // so unticking Maximize Timeline does not bring back what was there before
        a_->get("maximizeTimeline")->setChecked(false);
    }
    auto named = [this](const char* n) { return findChild<QDockWidget*>(n); };
    QDockWidget *media = named("dockMedia"), *preview = named("dockPreview"), *master = named("dockMaster"), *mixer = mixerDock_;
    // the windows that share the tab group at the left, in tab order
    std::vector<QDockWidget*> group;
    for (const char* n : {"dockMedia", "dockExplorer", "dockVideoFx", "dockAudioFx", "dockGenerators", "dockTransitions", "dockNotes", "dockMixer",
                          "dockScopes", "dockDetails"})
        if (QDockWidget* d = named(n)) group.push_back(d);
    // (any window added later that is not in the list goes there too)
    for (QDockWidget* d : docks_)
        if (d != preview && d != master && std::find(group.begin(), group.end(), d) == group.end()) group.push_back(d);
    for (QDockWidget* d : docks_) {
        d->setFloating(false);
        removeDockWidget(d);
    }
    // the window's own size decides the sizes, as when they are given (it may still change as it is shown, e.g. maximized)
    auto W = [this] { return std::max(800, width()); };
    auto H = [this] { return std::max(500, height()); };
    std::set<QDockWidget*> shown;
    QDockWidget* front = nullptr;
    std::function<void()> sizes;
    if (which == LargePreviewLayout) {
        // the user's VEGAS 18 window: preview over most of the width, Master Bus at its right; other windows are tabs
        // beside the preview, closed until asked for
        addDockWidget(dockArea(), preview);
        splitDockWidget(preview, master, Qt::Horizontal);
        for (QDockWidget* d : group) tabifyDockWidget(preview, d);
        shown = {preview, master};
        front = preview;
        sizes = [this, preview, master, W, H] {
            const int m = W() * 477 / 1920;
            resizeDocks({preview, master}, {W() - m - 7, m}, Qt::Horizontal);
            resizeDocks({preview}, {H() * 540 / 1018}, Qt::Vertical);
        };
    } else {
        // Default: tab group at the top left (Project Media in front), Video Preview at its right, Master Bus narrow at
        // the far right. Audio Mixing: the same with the Mixing Console in front, wider.
        if (which == AudioMixingLayout) {
            group.erase(std::find(group.begin(), group.end(), mixer));
            group.insert(group.begin(), mixer);
        }
        QDockWidget* first = group.front();
        addDockWidget(dockArea(), first);
        splitDockWidget(first, preview, Qt::Horizontal);
        splitDockWidget(preview, master, Qt::Horizontal);
        for (size_t k = 1; k < group.size(); ++k) tabifyDockWidget(first, group[k]);
        // open at first: Project Media and the plug-in lists; the others are tabs there once asked for
        const std::set<QString> open = {"dockMedia", "dockExplorer", "dockVideoFx", "dockAudioFx", "dockGenerators", "dockTransitions"};
        for (QDockWidget* d : group)
            if (open.count(d->objectName()) || (d == mixer && which == AudioMixingLayout)) shown.insert(d);
        shown.insert(preview);
        shown.insert(master);
        front = which == AudioMixingLayout ? mixer : media;
        const bool mixing = which == AudioMixingLayout;
        sizes = [this, first, preview, master, W, H, mixing] {
            const int m = 168;              // Master Bus: its meters and a little room
            const int rest = W() - m - 14;
            const int left = mixing ? rest * 62 / 100 : rest / 2;
            resizeDocks({first, preview, master}, {left, rest - left, m}, Qt::Horizontal);
            resizeDocks({preview}, {H() * 43 / 100}, Qt::Vertical);
        };
    }
    for (QDockWidget* d : docks_) d->setVisible(shown.count(d) > 0);
    if (front) front->raise();
    // sizes are given again once the main window has laid the windows out; until then the docking area may not take them all
    sizes();
    QTimer::singleShot(0, this, [sizes, front] {
        sizes();
        if (front) front->raise();          // a window just opened among tabs comes to the front only once they are made
    });
    currentLayout_ = builtinLayoutName(which);
    updateDockTabs();
}

std::vector<MainWindow::SavedLayout> MainWindow::savedLayoutList()
{
    QSettings s;
    std::vector<SavedLayout> list;
    if (!s.contains("windowLayouts/size")) {
        // layouts an older Reno saved on the number keys
        for (int n = 0; n < 10; ++n) {
            const QString key = QString("layouts/%1").arg(n);
            if (!s.contains(key)) continue;
            SavedLayout l;
            l.name = QString("Layout %1").arg(n);
            l.key = n;
            l.state = s.value(key).toByteArray();
            l.version = 3;
            list.push_back(l);
        }
        if (!list.empty()) {
            setSavedLayoutList(list);
            s.remove("layouts");
        }
        return list;
    }
    const int n = s.beginReadArray("windowLayouts");
    for (int k = 0; k < n; ++k) {
        s.setArrayIndex(k);
        SavedLayout l;
        l.name = s.value("name").toString();
        l.key = s.value("key", -1).toInt();
        l.state = s.value("state").toByteArray();
        l.version = s.value("version").toInt();
        if (!l.name.isEmpty()) list.push_back(l);
    }
    s.endArray();
    return list;
}

void MainWindow::setSavedLayoutList(const std::vector<SavedLayout>& list)
{
    QSettings s;
    s.remove("windowLayouts");
    s.beginWriteArray("windowLayouts", (int)list.size());
    for (int k = 0; k < (int)list.size(); ++k) {
        s.setArrayIndex(k);
        s.setValue("name", list[(size_t)k].name);
        s.setValue("key", list[(size_t)k].key);
        s.setValue("state", list[(size_t)k].state);
        s.setValue("version", list[(size_t)k].version);
    }
    s.endArray();
}

void MainWindow::saveLayout(const QString& name, int key)
{
    std::vector<SavedLayout> list = savedLayoutList();
    SavedLayout* mine = nullptr;
    for (SavedLayout& l : list) {
        if (l.name == name) mine = &l;
        else if (key >= 0 && l.key == key) l.key = -1;      // a number belongs to one layout
    }
    if (!mine) {
        list.push_back(SavedLayout());
        mine = &list.back();
        mine->name = name;
    }
    mine->key = key;
    mine->state = saveState(kLayoutVersion);
    mine->version = kLayoutVersion;
    setSavedLayoutList(list);
    currentLayout_ = name;
}

bool MainWindow::recallLayout(const QString& name)
{
    for (int k = 0; k < 3; ++k)
        if (name == builtinLayoutName((BuiltinLayout)k)) {
            applyLayout((BuiltinLayout)k);
            return true;
        }
    for (const SavedLayout& l : savedLayoutList()) {
        if (l.name != name) continue;
        if (timelineMaximized_) {
            timelineMaximized_ = false;
            a_->get("maximizeTimeline")->setChecked(false);
        }
        if (!restoreState(l.state, l.version)) {
            AppState::get().status(strfmt("The window layout \"%s\" could not be brought back.", l.name.toStdString().c_str()));
            return false;
        }
        currentLayout_ = l.name;
        updateDockTabs();
        return true;
    }
    return false;
}

bool MainWindow::recallLayout(int key)
{
    for (const SavedLayout& l : savedLayoutList())
        if (l.key == key) return recallLayout(l.name);
    return false;
}

void MainWindow::reloadLayout()
{
    if (currentLayout_.isEmpty() || !recallLayout(currentLayout_)) applyLayout(DefaultLayout);
}

// View > Window Layouts: built-in layouts, saved ones (a bullet at the one in use), and the commands.
void MainWindow::fillLayoutsMenu(QMenu* menu)
{
    menu->clear();
    auto bullet = [this](QAction* x, const QString& name) {
        x->setCheckable(true);
        x->setChecked(name == currentLayout_);
    };
    const char* builtins[] = {"defaultLayout", "largePreviewLayout", "audioMixingLayout"};
    for (int k = 0; k < 3; ++k) {
        QAction* x = a_->get(builtins[k]);
        bullet(x, builtinLayoutName((BuiltinLayout)k));
        menu->addAction(x);
    }
    const std::vector<SavedLayout> list = savedLayoutList();
    if (!list.empty()) menu->addSeparator();
    for (const SavedLayout& l : list) {
        // the number keys belong to hidden actions that work whether the menu is open or not; the menu only shows them
        QAction* x = menu->addAction(l.key >= 0 ? QString("%1\tAlt+D, %2").arg(l.name).arg(l.key) : l.name);
        bullet(x, l.name);
        const QString name = l.name;
        QObject::connect(x, &QAction::triggered, this, [this, name] { recallLayout(name); });
    }
    menu->addSeparator();
    bool saved = false;
    for (const SavedLayout& l : list) saved = saved || l.name == currentLayout_;
    a_->get("saveCurrentLayout")->setEnabled(saved);
    menu->addActions({a_->get("saveLayoutAs"), a_->get("saveCurrentLayout"), a_->get("reloadLayout"), a_->get("organizeLayouts")});
}

namespace {
// VEGAS's dialogs are placed by the capture's coordinates, which count from the window's top left with its frame (1 px at the
// sides, 31 px of title bar on top).
void placeAt(QWidget* w, int x, int y, int cw, int ch) { w->setGeometry(x - 1, y - 31, cw, ch); }
void vegasFrame(QDialog& d, int w, int h)
{
    QFont f = d.font();
    f.setPixelSize(12);
    d.setFont(f);
    d.setFixedSize(w - 2, h - 31 - 1);
}
QString layoutFolderText()
{
    return QDir::toNativeSeparators(QFileInfo(QSettings().fileName()).absolutePath() + "/");
}
QPushButton* standInButton(QWidget* parent, const QString& text, int x, int y, int w)
{
    QPushButton* b = new QPushButton(text, parent);
    placeAt(b, x, y, w, 23);
    b->setEnabled(false);
    b->setToolTip("VEGAS keeps its layouts in files in a folder; Reno keeps them in its settings.");
    return b;
}
QString slotText(int n, const QString& owner) { return QString("%1 %2").arg(n).arg(owner.isEmpty() ? QString("<EMPTY>") : owner); }
}  // namespace

// Save Layout As: a name, and the number key to recall it (VEGAS's Shortcut list). 632 x 159 like VEGAS's.
void MainWindow::saveLayoutAsDialog()
{
    QDialog dlg(this);
    dlg.setObjectName("saveLayoutDialog");
    dlg.setWindowTitle("Save Layout As");
    vegasFrame(dlg, 632, 159);
    const std::vector<SavedLayout> list = savedLayoutList();
    QLabel* nameLabel = new QLabel("Name:", &dlg);
    placeAt(nameLabel, 18, 62, 40, 13);
    QLineEdit* name = new QLineEdit(&dlg);
    name->setObjectName("layoutName");
    placeAt(name, 57, 57, 233, 23);
    name->setText(QString("Layout %1").arg(list.size() + 1));
    name->selectAll();
    QLabel* keyLabel = new QLabel("Shortcut:", &dlg);
    placeAt(keyLabel, 300, 60, 52, 13);
    QComboBox* key = new QComboBox(&dlg);
    key->setObjectName("layoutKey");
    placeAt(key, 354, 57, 269, 21);
    int firstFree = 0;
    bool freeFound = false;
    for (int n = 0; n < 10; ++n) {
        QString owner;
        for (const SavedLayout& l : list)
            if (l.key == n) owner = l.name;
        key->addItem(slotText(n, owner), n);
        if (owner.isEmpty() && !freeFound) { firstFree = n; freeFound = true; }
    }
    key->setCurrentIndex(firstFree);
    QLabel* folderLabel = new QLabel("Folder:", &dlg);
    placeAt(folderLabel, 16, 98, 40, 13);
    QLineEdit* folder = new QLineEdit(layoutFolderText(), &dlg);
    folder->setObjectName("layoutFolder");
    folder->setReadOnly(true);
    placeAt(folder, 57, 94, 488, 23);
    standInButton(&dlg, "Browse...", 550, 94, 75)->setObjectName("layoutBrowse");
    QPushButton* ok = new QPushButton("OK", &dlg);
    placeAt(ok, 237, 127, 75, 23);
    ok->setDefault(true);
    QPushButton* cancel = new QPushButton("Cancel", &dlg);
    placeAt(cancel, 324, 127, 75, 23);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
    QString n = name->text().trimmed();
    for (int k = 0; k < 3; ++k)
        if (n == builtinLayoutName((BuiltinLayout)k)) n += " (saved)";     // the built-in names stay the built-in layouts'
    saveLayout(n, key->currentData().toInt());
    AppState::get().status(strfmt("Window layout \"%s\" saved.", n.toStdString().c_str()));
}

// Organize Layouts (596 x 372 like VEGAS's): the layouts that exist on the left, the ten number-key slotList ("Current layouts in
// menu") on the right; Assign puts the chosen layout in the chosen slot. The changes are kept with OK.
void MainWindow::organizeLayouts()
{
    QDialog dlg(this);
    dlg.setObjectName("organizeLayoutsDialog");
    dlg.setWindowTitle("Organize Layouts");
    vegasFrame(dlg, 596, 372);
    QLabel* l1 = new QLabel("Current layout folder:", &dlg);
    placeAt(l1, 13, 41, 190, 13);
    QLineEdit* folder = new QLineEdit(layoutFolderText(), &dlg);
    folder->setObjectName("layoutFolder");
    folder->setReadOnly(true);
    placeAt(folder, 13, 59, 488, 23);
    standInButton(&dlg, "Browse...", 510, 59, 75)->setObjectName("layoutBrowse");
    QLabel* l2 = new QLabel("Available layouts in current folder:", &dlg);
    placeAt(l2, 13, 94, 250, 13);
    QTreeWidget* tree = new QTreeWidget(&dlg);
    tree->setObjectName("layoutList");
    tree->setRootIsDecorated(false);
    tree->setHeaderHidden(true);
    placeAt(tree, 13, 112, 195, 176);
    QLabel* l3 = new QLabel("Current layouts in menu:", &dlg);
    placeAt(l3, 306, 94, 195, 13);
    QListWidget* slotList = new QListWidget(&dlg);
    slotList->setObjectName("layoutSlots");
    placeAt(slotList, 306, 112, 195, 176);
    QPushButton* assign = new QPushButton("&Assign ->", &dlg);
    assign->setObjectName("layoutAssign");
    placeAt(assign, 219, 177, 75, 23);
    QPushButton* up = new QPushButton("Move &Up", &dlg);
    up->setObjectName("layoutUp");
    placeAt(up, 510, 111, 75, 23);
    QPushButton* down = new QPushButton("Move &Down", &dlg);
    down->setObjectName("layoutDown");
    placeAt(down, 510, 142, 75, 23);
    QPushButton* clear = new QPushButton("&Clear", &dlg);
    clear->setObjectName("layoutClear");
    placeAt(clear, 510, 190, 75, 23);
    QPushButton* activate = new QPushButton("Activate", &dlg);
    activate->setObjectName("layoutActivate");
    placeAt(activate, 510, 220, 75, 23);
    QPushButton* remove = new QPushButton("Delete Layout", &dlg);
    remove->setObjectName("layoutDelete");
    placeAt(remove, 13, 294, 195, 23);
    standInButton(&dlg, "Explore Containing Folder...", 306, 294, 195)->setObjectName("layoutExplore");
    QPushButton* ok = new QPushButton("OK", &dlg);
    placeAt(ok, 429, 337, 75, 23);
    ok->setDefault(true);
    QPushButton* cancel = new QPushButton("Cancel", &dlg);
    placeAt(cancel, 510, 337, 75, 23);

    std::vector<SavedLayout> list = savedLayoutList();
    std::vector<QString> deleted;
    auto slotOwner = [&](int n) {
        for (const SavedLayout& l : list)
            if (l.key == n) return l.name;
        return QString();
    };
    auto availableRow = [&]() { return tree->indexOfTopLevelItem(tree->currentItem()); };
    auto update = [&] {
        const bool any = availableRow() >= 0 && availableRow() < (int)list.size();
        const int s = slotList->currentRow();
        const bool filled = s >= 0 && !slotOwner(s).isEmpty();
        assign->setEnabled(any && s >= 0);
        remove->setEnabled(any);
        clear->setEnabled(filled);
        up->setEnabled(filled && s > 0);
        down->setEnabled(filled && s < 9);
        activate->setEnabled(any || filled);
    };
    auto fill = [&] {
        const int row = std::max(0, availableRow());
        const int srow = slotList->currentRow();
        tree->clear();
        for (const SavedLayout& l : list) new QTreeWidgetItem(tree, QStringList() << l.name);
        if (!list.empty()) tree->setCurrentItem(tree->topLevelItem(std::min(row, (int)list.size() - 1)));
        slotList->clear();
        for (int n = 0; n < 10; ++n) slotList->addItem(slotText(n, slotOwner(n)));
        slotList->setCurrentRow(srow >= 0 ? srow : 0);
        update();
    };
    fill();
    QObject::connect(tree, &QTreeWidget::currentItemChanged, &dlg, [&] { update(); });
    QObject::connect(slotList, &QListWidget::currentRowChanged, &dlg, [&] { update(); });
    QObject::connect(assign, &QPushButton::clicked, &dlg, [&] {
        const int r = availableRow(), s = slotList->currentRow();
        if (r < 0 || r >= (int)list.size() || s < 0) return;
        for (SavedLayout& o : list)
            if (o.key == s) o.key = -1;
        list[(size_t)r].key = s;
        fill();
    });
    QObject::connect(clear, &QPushButton::clicked, &dlg, [&] {
        const int s = slotList->currentRow();
        for (SavedLayout& o : list)
            if (o.key == s) o.key = -1;
        fill();
    });
    auto move = [&](int by) {
        const int s = slotList->currentRow(), t = s + by;
        if (s < 0 || t < 0 || t > 9) return;
        for (SavedLayout& o : list) {
            if (o.key == s) o.key = t;
            else if (o.key == t) o.key = s;
        }
        slotList->setCurrentRow(t);
        fill();
    };
    QObject::connect(up, &QPushButton::clicked, &dlg, [&] { move(-1); });
    QObject::connect(down, &QPushButton::clicked, &dlg, [&] { move(1); });
    QObject::connect(remove, &QPushButton::clicked, &dlg, [&] {
        const int r = availableRow();
        if (r < 0 || r >= (int)list.size()) return;
        deleted.push_back(list[(size_t)r].name);
        list.erase(list.begin() + r);
        fill();
    });
    auto activateCurrent = [&] {
        QString n;
        if (QTreeWidgetItem* it = tree->currentItem()) n = it->text(0);
        if (n.isEmpty()) n = slotOwner(slotList->currentRow());
        if (!n.isEmpty()) {
            setSavedLayoutList(list);   // what was changed so far counts, so the layout can be found
            recallLayout(n);
        }
    };
    QObject::connect(activate, &QPushButton::clicked, &dlg, activateCurrent);
    QObject::connect(tree, &QTreeWidget::itemDoubleClicked, &dlg, activateCurrent);
    QObject::connect(slotList, &QListWidget::itemDoubleClicked, &dlg, activateCurrent);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    for (const QString& d : deleted)
        if (currentLayout_ == d) currentLayout_.clear();
    setSavedLayoutList(list);
}

}  // namespace app
}  // namespace reno
