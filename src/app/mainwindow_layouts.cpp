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
#include <QDockWidget>
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

// Save Layout As: a name, and the number key to recall it (VEGAS's Shortcut list).
void MainWindow::saveLayoutAsDialog()
{
    QDialog dlg(this);
    dlg.setObjectName("saveLayoutDialog");
    dlg.setWindowTitle("Save Layout As");
    auto* v = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QLineEdit* name = new QLineEdit(&dlg);
    name->setObjectName("layoutName");
    const std::vector<SavedLayout> list = savedLayoutList();
    name->setText(QString("Layout %1").arg(list.size() + 1));
    name->selectAll();
    QComboBox* key = new QComboBox(&dlg);
    key->setObjectName("layoutKey");
    key->addItem("None", -1);
    int firstFree = -1;
    for (int n = 0; n < 10; ++n) {
        QString owner;
        for (const SavedLayout& l : list)
            if (l.key == n) owner = l.name;
        key->addItem(owner.isEmpty() ? QString("Alt+D, %1").arg(n) : QString("Alt+D, %1 (now %2)").arg(n).arg(owner), n);
        if (owner.isEmpty() && firstFree < 0) firstFree = n;
    }
    key->setCurrentIndex(firstFree + 1);
    form->addRow("Name:", name);
    form->addRow("Shortcut:", key);
    v->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
    QString n = name->text().trimmed();
    for (int k = 0; k < 3; ++k)
        if (n == builtinLayoutName((BuiltinLayout)k)) n += " (saved)";     // the built-in names stay the built-in layouts'
    saveLayout(n, key->currentData().toInt());
    AppState::get().status(strfmt("Window layout \"%s\" saved.", n.toStdString().c_str()));
}

// Organize Layouts: saved layouts to bring back, rename, give a number key or delete.
void MainWindow::organizeLayouts()
{
    QDialog dlg(this);
    dlg.setObjectName("organizeLayoutsDialog");
    dlg.setWindowTitle("Organize Layouts");
    dlg.resize(460, 300);
    auto* h = new QHBoxLayout(&dlg);
    QTreeWidget* tree = new QTreeWidget(&dlg);
    tree->setObjectName("layoutList");
    tree->setRootIsDecorated(false);
    tree->setHeaderLabels(QStringList() << "Layout" << "Shortcut");
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    h->addWidget(tree, 1);
    auto* side = new QVBoxLayout();
    QPushButton* activate = new QPushButton("&Activate", &dlg);
    QPushButton* rename = new QPushButton("&Rename...", &dlg);
    QComboBox* key = new QComboBox(&dlg);
    QPushButton* remove = new QPushButton("&Delete", &dlg);
    QPushButton* close = new QPushButton("Close", &dlg);
    activate->setObjectName("layoutActivate");
    rename->setObjectName("layoutRename");
    key->setObjectName("layoutKey");
    remove->setObjectName("layoutDelete");
    key->addItem("No shortcut", -1);
    for (int n = 0; n < 10; ++n) key->addItem(QString("Alt+D, %1").arg(n), n);
    for (QWidget* w : {(QWidget*)activate, (QWidget*)rename, (QWidget*)key, (QWidget*)remove}) side->addWidget(w);
    side->addStretch(1);
    side->addWidget(close);
    h->addLayout(side);
    std::vector<SavedLayout> list = savedLayoutList();
    bool filling = false;
    auto fill = [&] {
        filling = true;
        const int row = std::max(0, tree->indexOfTopLevelItem(tree->currentItem()));
        tree->clear();
        for (const SavedLayout& l : list)
            new QTreeWidgetItem(tree, QStringList() << l.name << (l.key >= 0 ? QString("Alt+D, %1").arg(l.key) : QString()));
        if (!list.empty()) tree->setCurrentItem(tree->topLevelItem(std::min(row, (int)list.size() - 1)));
        const bool any = !list.empty();
        for (QWidget* w : {(QWidget*)activate, (QWidget*)rename, (QWidget*)key, (QWidget*)remove}) w->setEnabled(any);
        if (any) key->setCurrentIndex(list[(size_t)tree->indexOfTopLevelItem(tree->currentItem())].key + 1);
        filling = false;
    };
    auto current = [&]() -> SavedLayout* {
        const int r = tree->indexOfTopLevelItem(tree->currentItem());
        return r >= 0 && r < (int)list.size() ? &list[(size_t)r] : nullptr;
    };
    fill();
    QObject::connect(tree, &QTreeWidget::currentItemChanged, &dlg, [&] {
        if (filling) return;
        if (SavedLayout* l = current()) {
            filling = true;
            key->setCurrentIndex(l->key + 1);
            filling = false;
        }
    });
    QObject::connect(key, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [&](int) {
        SavedLayout* l = current();
        if (filling || !l) return;
        const int k = key->currentData().toInt();
        for (SavedLayout& o : list)
            if (&o != l && k >= 0 && o.key == k) o.key = -1;
        l->key = k;
        setSavedLayoutList(list);
        fill();
    });
    auto activateCurrent = [&] {
        if (SavedLayout* l = current()) recallLayout(l->name);
    };
    QObject::connect(activate, &QPushButton::clicked, &dlg, activateCurrent);
    QObject::connect(tree, &QTreeWidget::itemDoubleClicked, &dlg, activateCurrent);
    QObject::connect(rename, &QPushButton::clicked, &dlg, [&] {
        SavedLayout* l = current();
        if (!l) return;
        bool ok = false;
        const QString n = QInputDialog::getText(&dlg, "Rename Layout", "Name:", QLineEdit::Normal, l->name, &ok).trimmed();
        if (!ok || n.isEmpty() || n == l->name) return;
        for (const SavedLayout& o : list)
            if (o.name == n) return;
        if (currentLayout_ == l->name) currentLayout_ = n;
        l->name = n;
        setSavedLayoutList(list);
        fill();
    });
    QObject::connect(remove, &QPushButton::clicked, &dlg, [&] {
        SavedLayout* l = current();
        if (!l) return;
        if (currentLayout_ == l->name) currentLayout_.clear();
        list.erase(list.begin() + (l - list.data()));
        setSavedLayoutList(list);
        fill();
    });
    QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    dlg.exec();
}

}  // namespace app
}  // namespace reno
