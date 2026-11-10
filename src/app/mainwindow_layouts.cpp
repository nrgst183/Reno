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

namespace {

// Organize Layouts (596 x 372 like VEGAS's): the layouts that exist on the left, the ten number-key slots ("Current
// layouts in menu") on the right; Assign puts the chosen layout in the chosen slot. The changes are made to a copy of
// the list, which OK keeps; Activate recalls a layout at once.
class OrganizeLayoutsDialog : public QDialog {
public:
    using Activate = std::function<void(const std::vector<MainWindow::SavedLayout>& list, const QString& name)>;

    OrganizeLayoutsDialog(QWidget* parent, Activate activate) : QDialog(parent), list_(MainWindow::savedLayoutList()), activate_(std::move(activate))
    {
        setObjectName("organizeLayoutsDialog");
        setWindowTitle("Organize Layouts");
        vegasFrame(*this, 596, 372);
        buildControls();
        fill();
        QObject::connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { updateButtons(); });
        QObject::connect(slots_, &QListWidget::currentRowChanged, this, [this] { updateButtons(); });
        QObject::connect(assign_, &QPushButton::clicked, this, [this] { assignToSlot(); });
        QObject::connect(clear_, &QPushButton::clicked, this, [this] { clearSlot(); });
        QObject::connect(up_, &QPushButton::clicked, this, [this] { moveSlot(-1); });
        QObject::connect(down_, &QPushButton::clicked, this, [this] { moveSlot(1); });
        QObject::connect(remove_, &QPushButton::clicked, this, [this] { deleteLayout(); });
        QObject::connect(activateBtn_, &QPushButton::clicked, this, [this] { activateCurrent(); });
        QObject::connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this] { activateCurrent(); });
        QObject::connect(slots_, &QListWidget::itemDoubleClicked, this, [this] { activateCurrent(); });
    }

    const std::vector<MainWindow::SavedLayout>& list() const { return list_; }
    const std::vector<QString>& deleted() const { return deleted_; }

private:
    void buildControls()
    {
        QLabel* l1 = new QLabel("Current layout folder:", this);
        placeAt(l1, 13, 41, 190, 13);
        QLineEdit* folder = new QLineEdit(layoutFolderText(), this);
        folder->setObjectName("layoutFolder");
        folder->setReadOnly(true);
        placeAt(folder, 13, 59, 488, 23);
        standInButton(this, "Browse...", 510, 59, 75)->setObjectName("layoutBrowse");
        QLabel* l2 = new QLabel("Available layouts in current folder:", this);
        placeAt(l2, 13, 94, 250, 13);
        tree_ = new QTreeWidget(this);
        tree_->setObjectName("layoutList");
        tree_->setRootIsDecorated(false);
        tree_->setHeaderHidden(true);
        placeAt(tree_, 13, 112, 195, 176);
        QLabel* l3 = new QLabel("Current layouts in menu:", this);
        placeAt(l3, 306, 94, 195, 13);
        slots_ = new QListWidget(this);
        slots_->setObjectName("layoutSlots");
        placeAt(slots_, 306, 112, 195, 176);
        assign_ = button("&Assign ->", "layoutAssign", 219, 177, 75);
        up_ = button("Move &Up", "layoutUp", 510, 111, 75);
        down_ = button("Move &Down", "layoutDown", 510, 142, 75);
        clear_ = button("&Clear", "layoutClear", 510, 190, 75);
        activateBtn_ = button("Activate", "layoutActivate", 510, 220, 75);
        remove_ = button("Delete Layout", "layoutDelete", 13, 294, 195);
        standInButton(this, "Explore Containing Folder...", 306, 294, 195)->setObjectName("layoutExplore");
        QPushButton* ok = new QPushButton("OK", this);
        placeAt(ok, 429, 337, 75, 23);
        ok->setDefault(true);
        QPushButton* cancel = new QPushButton("Cancel", this);
        placeAt(cancel, 510, 337, 75, 23);
        QObject::connect(ok, &QPushButton::clicked, this, &QDialog::accept);
        QObject::connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    }

    QPushButton* button(const QString& text, const char* name, int x, int y, int w)
    {
        QPushButton* b = new QPushButton(text, this);
        b->setObjectName(name);
        placeAt(b, x, y, w, 23);
        return b;
    }

    // The name of the layout in menu slot n ("" if the slot is empty).
    QString slotOwner(int n) const
    {
        for (const MainWindow::SavedLayout& l : list_)
            if (l.key == n) return l.name;
        return QString();
    }

    int availableRow() const { return tree_->indexOfTopLevelItem(tree_->currentItem()); }

    void updateButtons()
    {
        const bool any = availableRow() >= 0 && availableRow() < (int)list_.size();
        const int s = slots_->currentRow();
        const bool filled = s >= 0 && !slotOwner(s).isEmpty();
        assign_->setEnabled(any && s >= 0);
        remove_->setEnabled(any);
        clear_->setEnabled(filled);
        up_->setEnabled(filled && s > 0);
        down_->setEnabled(filled && s < 9);
        activateBtn_->setEnabled(any || filled);
    }

    // Both lists again from list_, keeping the rows picked.
    void fill()
    {
        const int row = std::max(0, availableRow());
        const int srow = slots_->currentRow();
        tree_->clear();
        for (const MainWindow::SavedLayout& l : list_) new QTreeWidgetItem(tree_, QStringList() << l.name);
        if (!list_.empty()) tree_->setCurrentItem(tree_->topLevelItem(std::min(row, (int)list_.size() - 1)));
        slots_->clear();
        for (int n = 0; n < 10; ++n) slots_->addItem(slotText(n, slotOwner(n)));
        slots_->setCurrentRow(srow >= 0 ? srow : 0);
        updateButtons();
    }

    void emptySlot(int s)
    {
        for (MainWindow::SavedLayout& o : list_)
            if (o.key == s) o.key = -1;
    }

    // Assign: the picked layout goes in the picked slot (out of any other).
    void assignToSlot()
    {
        const int r = availableRow(), s = slots_->currentRow();
        if (r < 0 || r >= (int)list_.size() || s < 0) return;
        emptySlot(s);
        list_[(size_t)r].key = s;
        fill();
    }

    void clearSlot()
    {
        emptySlot(slots_->currentRow());
        fill();
    }

    // Move Up / Move Down: the slot's layout swaps places with the one above or below.
    void moveSlot(int by)
    {
        const int s = slots_->currentRow(), t = s + by;
        if (s < 0 || t < 0 || t > 9) return;
        for (MainWindow::SavedLayout& o : list_) {
            if (o.key == s) o.key = t;
            else if (o.key == t) o.key = s;
        }
        slots_->setCurrentRow(t);
        fill();
    }

    void deleteLayout()
    {
        const int r = availableRow();
        if (r < 0 || r >= (int)list_.size()) return;
        deleted_.push_back(list_[(size_t)r].name);
        list_.erase(list_.begin() + r);
        fill();
    }

    // Activate (or a double-click): the picked layout, else the picked slot's.
    void activateCurrent()
    {
        QString n;
        if (QTreeWidgetItem* it = tree_->currentItem()) n = it->text(0);
        if (n.isEmpty()) n = slotOwner(slots_->currentRow());
        if (!n.isEmpty()) activate_(list_, n);
    }

    std::vector<MainWindow::SavedLayout> list_;
    std::vector<QString> deleted_;
    Activate activate_;
    QTreeWidget* tree_ = nullptr;
    QListWidget* slots_ = nullptr;
    QPushButton *assign_ = nullptr, *up_ = nullptr, *down_ = nullptr, *clear_ = nullptr, *activateBtn_ = nullptr, *remove_ = nullptr;
};

}  // namespace

void MainWindow::organizeLayouts()
{
    OrganizeLayoutsDialog dlg(this, [this](const std::vector<SavedLayout>& list, const QString& name) {
        setSavedLayoutList(list);   // what was changed so far counts, so the layout can be found
        recallLayout(name);
    });
    if (dlg.exec() != QDialog::Accepted) return;
    for (const QString& d : dlg.deleted())
        if (currentLayout_ == d) currentLayout_.clear();
    setSavedLayoutList(dlg.list());
}

}  // namespace app
}  // namespace reno
