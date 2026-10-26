// Options > Customize Keyboard (see keyboard.h): the keymap logic, its files and the dialog.
#include "keyboard.h"
#include "mainwindow.h"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <set>

namespace reno {
namespace app {

// ============================================================================================== the map itself

namespace {

bool sameSet(const QList<QKeySequence>& a, const QList<QKeySequence>& b)
{
    if (a.size() != b.size()) return false;
    for (const QKeySequence& k : a)
        if (!b.contains(k)) return false;
    return true;
}

QString plainText(QString s)
{
    s.replace("&&", "\x01");
    s.remove('&');
    s.replace("\x01", "&");
    const int tab = s.indexOf('\t');
    if (tab >= 0) s.truncate(tab);
    return s.trimmed();
}

}  // namespace

int KeyMap::find(const QString& id) const
{
    for (size_t k = 0; k < commands.size(); ++k)
        if (commands[k].id == id) return (int)k;
    return -1;
}

QStringList KeyMap::ownersOf(const QKeySequence& key, const QString& except) const
{
    QStringList out;
    if (key.isEmpty()) return out;
    for (const KeyCommand& c : commands)
        if (c.id != except && c.keys.contains(key)) out << c.id;
    return out;
}

QStringList KeyMap::add(const QString& id, const QKeySequence& key)
{
    const int i = find(id);
    if (i < 0 || key.isEmpty()) return QStringList();
    const QStringList taken = ownersOf(key, id);
    for (const QString& o : taken) commands[(size_t)find(o)].keys.removeAll(key);
    if (!commands[(size_t)i].keys.contains(key)) commands[(size_t)i].keys << key;
    return taken;
}

QStringList KeyMap::replace(const QString& id, const QKeySequence& key)
{
    const int i = find(id);
    if (i < 0 || key.isEmpty()) return QStringList();
    const QStringList taken = ownersOf(key, id);
    for (const QString& o : taken) commands[(size_t)find(o)].keys.removeAll(key);
    commands[(size_t)i].keys = QList<QKeySequence>() << key;
    return taken;
}

bool KeyMap::remove(const QString& id, const QKeySequence& key)
{
    const int i = find(id);
    if (i < 0) return false;
    return commands[(size_t)i].keys.removeAll(key) > 0;
}

void KeyMap::clearKeys(const QString& id)
{
    const int i = find(id);
    if (i >= 0) commands[(size_t)i].keys.clear();
}

void KeyMap::resetToDefaults()
{
    for (KeyCommand& c : commands) c.keys = c.defaults;
}

bool KeyMap::isDefault() const
{
    for (const KeyCommand& c : commands)
        if (!sameSet(c.keys, c.defaults)) return false;
    return true;
}

bool KeyMap::sameKeys(const KeyMap& other) const
{
    for (const KeyCommand& c : commands) {
        const int j = other.find(c.id);
        if (j < 0 || !sameSet(c.keys, other.commands[(size_t)j].keys)) return false;
    }
    return true;
}

QString KeyMap::toIni(const QString& name) const
{
    QString out;
    out += "; Reno keyboard map (Options > Customize Keyboard): the keys that differ from Reno's defaults\r\n";
    out += "[Keyboard Map]\r\n";
    out += "Name=" + QString(name).remove('\r').remove('\n') + "\r\n";
    out += "[Global]\r\n";
    for (const KeyCommand& c : commands) {
        if (sameSet(c.keys, c.defaults)) continue;
        if (c.keys.isEmpty()) out += c.id + "=\r\n";
        for (const QKeySequence& k : c.keys) out += c.id + "=" + k.toString(QKeySequence::PortableText) + "\r\n";
    }
    return out;
}

bool KeyMap::fromIni(const QString& text, QString* name, QString* error, int* skipped)
{
    // read it all first: a file that is not a keyboard map changes nothing
    QString mapName, section;
    bool header = false;
    std::vector<std::pair<QString, QString>> entries;
    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(';') || line.startsWith('#')) continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            section = line.mid(1, line.size() - 2).trimmed();
            if (section.compare("Keyboard Map", Qt::CaseInsensitive) == 0 || section.compare("Global", Qt::CaseInsensitive) == 0) header = true;
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed(), value = line.mid(eq + 1).trimmed();
        if (section.compare("Keyboard Map", Qt::CaseInsensitive) == 0) {
            if (key.compare("Name", Qt::CaseInsensitive) == 0) mapName = value;
        } else if (section.compare("Global", Qt::CaseInsensitive) == 0) {
            entries.emplace_back(key, value);
        }
    }
    if (!header) {
        if (error) *error = "This file is not a keyboard map (no [Keyboard Map] or [Global] section).";
        return false;
    }
    resetToDefaults();
    std::set<QString> seen;
    int unknown = 0;
    std::set<QString> unknownIds;
    for (const auto& e : entries) {
        const int i = find(e.first);
        if (i < 0) {
            if (unknownIds.insert(e.first).second) ++unknown;
            continue;
        }
        if (seen.insert(e.first).second) commands[(size_t)i].keys.clear();
        if (e.second.isEmpty()) continue;
        const QKeySequence k = QKeySequence::fromString(e.second, QKeySequence::PortableText);
        if (k.isEmpty()) continue;
        // a key named twice in the file goes to the last command that names it
        for (KeyCommand& c : commands)
            if (c.id != e.first) c.keys.removeAll(k);
        if (!commands[(size_t)i].keys.contains(k)) commands[(size_t)i].keys << k;
    }
    if (name) *name = mapName;
    if (skipped) *skipped = unknown;
    return true;
}

QString keyText(const QKeySequence& key)
{
    return key.toString(QKeySequence::PortableText);
}

// ============================================================================================== files and settings

QString keyMapFolder()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.reno";
    return QDir(dir).filePath("Keyboard Maps");
}

bool validKeyMapName(const QString& name)
{
    const QString n = name.trimmed();
    if (n.isEmpty() || n == "[Default]" || n.size() > 100) return false;
    for (QChar c : n)
        if (QString("\\/:*?\"<>|").contains(c) || c.unicode() < 32) return false;
    return !n.startsWith('.');
}

QString keyMapPath(const QString& name)
{
    return QDir(keyMapFolder()).filePath(name.trimmed() + ".ini");
}

namespace {

QString readText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024) return QString();
    return QString::fromUtf8(f.readAll());
}

bool writeText(const QString& path, const QString& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(text.toUtf8());
    return f.commit();
}

}  // namespace

QStringList savedKeyMaps()
{
    QStringList out;
    const QFileInfoList files = QDir(keyMapFolder()).entryInfoList(QStringList() << "*.ini", QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& f : files) out << f.completeBaseName();
    return out;
}

QString currentKeyMapName()
{
    const QString n = QSettings().value("keyboard/map").toString();
    return n.isEmpty() ? QString("[Default]") : n;
}

// ============================================================================================== the window's commands

namespace {

const char* kDefaultsProperty = "renoDefaultKeys";

// Every action in the menus, with its menu path.
void walkMenu(QMenu* menu, const QString& path, std::vector<std::pair<QAction*, QString>>& out, std::set<QMenu*>& seen)
{
    if (!menu || !seen.insert(menu).second) return;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) continue;
        if (a->menu()) walkMenu(a->menu(), path + " > " + plainText(a->text()), out, seen);
        else out.emplace_back(a, path);
    }
}

std::vector<std::pair<QAction*, QString>> commandActions(QMainWindow* window)
{
    std::vector<std::pair<QAction*, QString>> inMenus, out;
    std::set<QMenu*> seen;
    if (window->menuBar())
        for (QAction* top : window->menuBar()->actions())
            if (top->menu()) walkMenu(top->menu(), plainText(top->text()), inMenus, seen);
    std::set<QAction*> taken;
    std::set<QString> ids;
    auto take = [&](QAction* a, const QString& group) {
        if (taken.count(a) || a->objectName().isEmpty() || a->isSeparator() || a->menu()) return;
        if (ids.count(a->objectName())) return;     // two actions of one name: the first is the command
        taken.insert(a);
        ids.insert(a->objectName());
        out.emplace_back(a, group);
    };
    for (const auto& m : inMenus) take(m.first, m.second);
    for (QAction* a : window->actions()) take(a, "Other commands (no menu item)");
    return out;
}

QList<QKeySequence> defaultsOf(QAction* a)
{
    QVariant v = a->property(kDefaultsProperty);
    if (!v.isValid()) {
        a->setProperty(kDefaultsProperty, QVariant::fromValue(a->shortcuts()));
        return a->shortcuts();
    }
    return v.value<QList<QKeySequence>>();
}

}  // namespace

KeyMap windowKeyMap(QMainWindow* window)
{
    KeyMap map;
    if (!window) return map;
    for (const auto& ag : commandActions(window)) {
        KeyCommand c;
        c.id = ag.first->objectName();
        c.group = ag.second;
        c.text = plainText(ag.first->text());
        if (c.text.isEmpty()) c.text = c.id;
        c.defaults = defaultsOf(ag.first);
        c.keys = ag.first->shortcuts();
        map.commands.push_back(c);
    }
    return map;
}

void applyKeyMap(QMainWindow* window, const KeyMap& map)
{
    if (!window) return;
    for (const auto& ag : commandActions(window)) {
        const int i = map.find(ag.first->objectName());
        if (i < 0) continue;
        defaultsOf(ag.first);           // remembered before the keys change
        if (ag.first->shortcuts() != map.commands[(size_t)i].keys) ag.first->setShortcuts(map.commands[(size_t)i].keys);
    }
}

void installKeyboardMap(QMainWindow* window)
{
    if (!window) return;
    // Options > Customize Keyboard..., where VEGAS has it (after the toolbars' customizing, before Preferences)
    QAction* cust = new QAction("Customize &Keyboard...", window);
    cust->setObjectName("customizeKeyboard");
    cust->setStatusTip("Allows you to edit keyboard shortcuts.");
    QObject::connect(cust, &QAction::triggered, window, [window] { runCustomizeKeyboard(window); });
    window->addAction(cust);
    if (window->menuBar())
        for (QAction* top : window->menuBar()->actions()) {
            QMenu* m = top->menu();
            if (!m || plainText(top->text()) != "Options") continue;
            QAction* prefs = nullptr;
            for (QAction* a : m->actions())
                if (a->objectName() == "preferences") prefs = a;
            if (prefs) {
                m->insertAction(prefs, cust);
                m->insertSeparator(prefs);
            } else {
                m->addSeparator();
                m->addAction(cust);
            }
        }
    KeyMap map = windowKeyMap(window);  // remembers every command's default keys
    const QString saved = QSettings().value("keyboard/bindings").toString();
    if (!saved.isEmpty() && map.fromIni(saved, nullptr)) applyKeyMap(window, map);
}

void reloadKeyboardMap(QMainWindow* window)
{
    if (!window) return;
    KeyMap map = windowKeyMap(window);
    map.resetToDefaults();
    const QString saved = QSettings().value("keyboard/bindings").toString();
    if (!saved.isEmpty()) map.fromIni(saved, nullptr);
    applyKeyMap(window, map);
}

// ============================================================================================== keys the window does not own

namespace {

// Keys the timeline and docked windows handle themselves (from the shortcut table), by VEGAS's context.
struct FixedKey {
    QString context;        // "Track View", "Trimmer"...
    QString command;
    QString keysText;       // as the table writes them
    QList<QKeySequence> keys;   // those readable as single keys
};

const std::vector<FixedKey>& fixedKeys()
{
    static std::vector<FixedKey> list;
    if (!list.empty()) return list;
    for (const ShortcutInfo& s : shortcutList()) {
        if (s.kind != ShortcutInfo::Timeline && s.kind != ShortcutInfo::Panel) continue;
        const QString menu = QString::fromUtf8(s.menu);
        if (menu == "Mouse" || !s.keys || !*s.keys) continue;
        FixedKey f;
        f.context = s.kind == ShortcutInfo::Timeline ? QString("Track View") : menu;
        f.command = QString::fromUtf8(s.command);
        f.keysText = QString::fromUtf8(s.keys);
        for (const QString& alt : f.keysText.split(" or ")) {
            const QString t = alt.trimmed();
            const QKeySequence k = QKeySequence::fromString(t, QKeySequence::PortableText);
            if (k.count() == 1 && k.toString(QKeySequence::PortableText).compare(t, Qt::CaseInsensitive) == 0) f.keys << k;
        }
        list.push_back(f);
    }
    return list;
}

}  // namespace

// ============================================================================================== the dialog

KeyboardDialogHooks keyboardDialogHooks;

namespace {

// VEGAS's Save Keyboard Map As: "Save this keyboard map as:".
QString askMapName(QWidget* parent, const QString& start)
{
    QDialog d(parent);
    d.setObjectName("saveKeyboardMapAs");
    d.setWindowTitle("Save Keyboard Map As");
    QVBoxLayout* v = new QVBoxLayout(&d);
    v->addWidget(new QLabel("Save this keyboard map as:", &d));
    QLineEdit* name = new QLineEdit(start, &d);
    name->setObjectName("mapName");
    name->selectAll();
    v->addWidget(name);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    d.resize(360, d.sizeHint().height());
    if (d.exec() != QDialog::Accepted) return QString();
    return name->text().trimmed();
}

class CustomizeKeyboardDialog : public QDialog {
public:
    CustomizeKeyboardDialog(QMainWindow* window) : QDialog(window), window_(window)
    {
        setObjectName("customizeKeyboard");
        setWindowTitle("Customize Keyboard");
        work_ = windowKeyMap(window);
        mapName_ = currentKeyMapName();
        loaded_ = work_;
        // what the map of that name holds, so a change to another map can warn that edits would be lost
        {
            KeyMap fromFile = work_;
            if (mapName_ == "[Default]") fromFile.resetToDefaults();
            else if (!fromFile.fromIni(readText(keyMapPath(mapName_)), nullptr)) fromFile = work_;
            loaded_ = fromFile;
        }

        QVBoxLayout* v = new QVBoxLayout(this);
        // ---- Keyboard map: [maps] Save As... Delete (and Reno's Import... / Export...)
        v->addWidget(new QLabel("Keyboard &map:", this));
        QHBoxLayout* mapRow = new QHBoxLayout();
        maps_ = new QComboBox(this);
        maps_->setObjectName("keyboardMap");
        maps_->setMinimumWidth(220);
        static_cast<QLabel*>(v->itemAt(0)->widget())->setBuddy(maps_);
        mapRow->addWidget(maps_, 1);
        saveAs_ = button("Save As...", "saveMapAs", mapRow);
        delete_ = button("&Delete", "deleteMap", mapRow);
        import_ = button("Import...", "importMap", mapRow);
        export_ = button("Export...", "exportMap", mapRow);
        v->addLayout(mapRow);

        // ---- contexts (VEGAS's buttons above the list), filter and commands
        contexts_ = new QTabBar(this);
        contexts_->setObjectName("contexts");
        contexts_->setExpanding(false);
        contexts_->addTab("Global");
        std::vector<QString> names;
        for (const FixedKey& f : fixedKeys())
            if (std::find(names.begin(), names.end(), f.context) == names.end()) names.push_back(f.context);
        for (const QString& n : names) contexts_->addTab(n);
        v->addWidget(contexts_);
        QLabel* fl = new QLabel("Show &commands containing:", this);
        v->addWidget(fl);
        filter_ = new QLineEdit(this);
        filter_->setObjectName("filter");
        filter_->setClearButtonEnabled(true);
        fl->setBuddy(filter_);
        v->addWidget(filter_);
        tree_ = new QTreeWidget(this);
        tree_->setObjectName("commands");
        tree_->setColumnCount(2);
        tree_->setHeaderLabels(QStringList() << "Command" << "Shortcut keys");
        tree_->setRootIsDecorated(true);
        tree_->setUniformRowHeights(true);
        tree_->header()->setStretchLastSection(true);
        tree_->setColumnWidth(0, 330);
        v->addWidget(tree_, 1);
        note_ = new QLabel(this);
        note_->setObjectName("contextNote");
        note_->setWordWrap(true);
        v->addWidget(note_);

        // ---- Shortcut keys: [   ] Add Replace Remove (and Reno's Reset)
        QHBoxLayout* keyRow = new QHBoxLayout();
        QLabel* kl = new QLabel("&Shortcut keys:", this);
        keyRow->addWidget(kl);
        keyEdit_ = new QKeySequenceEdit(this);
        keyEdit_->setObjectName("shortcutKeys");
        keyEdit_->setMaximumSequenceLength(2);      // VEGAS's own keys go up to two in a row: Alt+D, D
        keyEdit_->setClearButtonEnabled(true);
        kl->setBuddy(keyEdit_);
        keyRow->addWidget(keyEdit_, 1);
        add_ = button("&Add", "addKey", keyRow);
        replace_ = button("Replace", "replaceKey", keyRow);
        remove_ = button("&Remove", "removeKey", keyRow);
        reset_ = button("Reset", "resetCommand", keyRow);
        reset_->setToolTip("Gives the selected command its default keys back");
        v->addLayout(keyRow);

        // ---- Shortcut currently assigned to: commands that have the key typed, Locate
        v->addWidget(new QLabel("Shortcut c&urrently assigned to:", this));
        QHBoxLayout* ownRow = new QHBoxLayout();
        owners_ = new QTreeWidget(this);
        owners_->setObjectName("assignedTo");
        owners_->setColumnCount(3);
        owners_->setHeaderLabels(QStringList() << "Context" << "Command" << "Keys");
        owners_->setRootIsDecorated(false);
        owners_->setColumnWidth(0, 90);
        owners_->setColumnWidth(1, 300);
        owners_->setFixedHeight(84);
        static_cast<QLabel*>(v->itemAt(v->count() - 1)->widget())->setBuddy(owners_);
        ownRow->addWidget(owners_, 1);
        QVBoxLayout* locCol = new QVBoxLayout();
        locate_ = new QPushButton("Locate", this);
        locate_->setObjectName("locate");
        locCol->addWidget(locate_);
        locCol->addStretch(1);
        ownRow->addLayout(locCol);
        v->addLayout(ownRow);

        QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        QObject::connect(bb, &QDialogButtonBox::accepted, this, [this] { finish(); });
        QObject::connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        v->addWidget(bb);

        fillMaps();
        fillTree();
        QObject::connect(maps_, &QComboBox::activated, this, [this](int) { chooseMap(maps_->currentText()); });
        QObject::connect(saveAs_, &QPushButton::clicked, this, [this] { saveAs(); });
        QObject::connect(delete_, &QPushButton::clicked, this, [this] { deleteMap(); });
        QObject::connect(import_, &QPushButton::clicked, this, [this] { importMap(QString()); });
        QObject::connect(export_, &QPushButton::clicked, this, [this] { exportMap(QString()); });
        QObject::connect(contexts_, &QTabBar::currentChanged, this, [this](int) { fillTree(); });
        QObject::connect(filter_, &QLineEdit::textChanged, this, [this](const QString&) { fillTree(); });
        QObject::connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { selected(); });
        QObject::connect(keyEdit_, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence&) { showOwners(); });
        QObject::connect(add_, &QPushButton::clicked, this, [this] { assign(false); });
        QObject::connect(replace_, &QPushButton::clicked, this, [this] { assign(true); });
        QObject::connect(remove_, &QPushButton::clicked, this, [this] { removeKey(); });
        QObject::connect(reset_, &QPushButton::clicked, this, [this] { resetCommand(); });
        QObject::connect(locate_, &QPushButton::clicked, this, [this] { locate(); });
        QObject::connect(owners_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) { locate(); });
        updateButtons();
        resize(640, 640);
    }

    // ---- for tests and the code below
    KeyMap& work() { return work_; }
    QString mapName() const { return mapName_; }
    bool selectCommand(const QString& id)
    {
        if (contexts_->currentIndex() != 0) contexts_->setCurrentIndex(0);
        if (!filter_->text().isEmpty()) filter_->clear();
        for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
            QTreeWidgetItem* gi = tree_->topLevelItem(g);
            for (int c = 0; c < gi->childCount(); ++c)
                if (gi->child(c)->data(0, Qt::UserRole).toString() == id) {
                    tree_->setCurrentItem(gi->child(c));
                    tree_->scrollToItem(gi->child(c));
                    return true;
                }
        }
        return false;
    }

    void chooseMap(const QString& name)
    {
        if (name == mapName_ && work_.sameKeys(loaded_)) return;
        if (!work_.sameKeys(loaded_) &&
            !ask("Selecting another map will discard your changes to the current one.\nDiscard changes?")) {
            maps_->setCurrentText(mapName_);
            return;
        }
        KeyMap m = work_;
        if (name == "[Default]") m.resetToDefaults();
        else {
            QString error;
            if (!m.fromIni(readText(keyMapPath(name)), nullptr, &error)) {
                warn(QString("The keyboard map \"%1\" could not be read. %2").arg(name, error));
                maps_->setCurrentText(mapName_);
                return;
            }
        }
        work_ = m;
        loaded_ = m;
        mapName_ = name;
        fillMaps();
        fillTree();
    }

    bool saveAs(const QString& given = QString())
    {
        QString name = given;
        if (name.isEmpty()) name = keyboardDialogHooks.name ? keyboardDialogHooks.name() : askMapName(this, mapName_ == "[Default]" ? QString() : mapName_);
        if (name.isEmpty()) return false;
        if (name == "[Default]") {
            warn("You cannot overwrite the default keyboard map.\nPlease enter another name.");
            return false;
        }
        if (!validKeyMapName(name)) {
            warn("The name is empty or contains invalid characters.");
            return false;
        }
        if (QFileInfo::exists(keyMapPath(name)) && !ask("A map with this name already exists.\nOverwrite it?")) return false;
        if (!writeText(keyMapPath(name), work_.toIni(name))) {
            warn(QString("The keyboard map could not be saved to %1.").arg(QDir::toNativeSeparators(keyMapPath(name))));
            return false;
        }
        mapName_ = name;
        loaded_ = work_;
        fillMaps();
        return true;
    }

    bool deleteMap()
    {
        if (mapName_ == "[Default]") return false;
        if (!ask(QString("Delete the keyboard map \"%1\"?").arg(mapName_))) return false;
        QFile::remove(keyMapPath(mapName_));
        // the keys stay as they are; they are just no longer a saved map
        mapName_ = "[Default]";
        loaded_ = work_;
        loaded_.resetToDefaults();
        fillMaps();
        return true;
    }

    // Import: a map file is copied into the maps folder and chosen.
    bool importMap(QString path)
    {
        if (path.isEmpty() && keyboardDialogHooks.file) path = keyboardDialogHooks.file(false);
        if (path.isEmpty() && !keyboardDialogHooks.file)
        {
            // last used folder, kept between runs
            path = QFileDialog::getOpenFileName(this, "Import Keyboard Map", QSettings().value("keyboard/folder", QDir::homePath()).toString(),
                                                "Keyboard maps (*.ini);;All files (*)");
            if (!path.isEmpty()) QSettings().setValue("keyboard/folder", QFileInfo(path).absolutePath());
        }
        if (path.isEmpty()) return false;
        const QString text = readText(path);
        KeyMap probe = work_;
        QString name, error;
        int skipped = 0;
        if (!probe.fromIni(text, &name, &error, &skipped)) {
            warn(QString("%1 could not be imported. %2").arg(QFileInfo(path).fileName(), error));
            return false;
        }
        if (!validKeyMapName(name)) name = QFileInfo(path).completeBaseName();
        if (!validKeyMapName(name)) name = "Imported";
        if (!work_.sameKeys(loaded_) &&
            !ask("Selecting another map will discard your changes to the current one.\nDiscard changes?"))
            return false;
        if (QFileInfo::exists(keyMapPath(name)) && !ask(QString("A map named \"%1\" already exists.\nDo you want to overwrite the existing map?").arg(name)))
            return false;
        if (!writeText(keyMapPath(name), probe.toIni(name))) {
            warn("The keyboard map could not be copied into " + QDir::toNativeSeparators(keyMapFolder()) + ".");
            return false;
        }
        work_ = probe;
        loaded_ = probe;
        mapName_ = name;
        fillMaps();
        fillTree();
        if (skipped) warn(QString("%1 command%2 in the file %3 not in Reno and %4 left out.")
                              .arg(skipped).arg(skipped == 1 ? "" : "s", skipped == 1 ? "is" : "are", skipped == 1 ? "was" : "were"));
        return true;
    }

    bool exportMap(QString path)
    {
        if (path.isEmpty() && keyboardDialogHooks.file) path = keyboardDialogHooks.file(true);
        if (path.isEmpty() && !keyboardDialogHooks.file) {
            const QString start = QDir(QSettings().value("keyboard/folder", QDir::homePath()).toString())
                                      .filePath((mapName_ == "[Default]" ? QString("Reno keyboard") : mapName_) + ".ini");
            path = QFileDialog::getSaveFileName(this, "Export Keyboard Map", start, "Keyboard maps (*.ini)");
            if (!path.isEmpty()) QSettings().setValue("keyboard/folder", QFileInfo(path).absolutePath());
        }
        if (path.isEmpty()) return false;
        if (!path.endsWith(".ini", Qt::CaseInsensitive)) path += ".ini";
        const QString name = mapName_ == "[Default]" ? QFileInfo(path).completeBaseName() : mapName_;
        if (!writeText(path, work_.toIni(name))) {
            warn("The keyboard map could not be written to " + QDir::toNativeSeparators(path) + ".");
            return false;
        }
        return true;
    }

    // Add (keep the command's other keys) or Replace (the key becomes its only one).
    bool assign(bool replaceKeys)
    {
        const QString id = currentId();
        const QKeySequence key = keyEdit_->keySequence();
        if (id.isEmpty() || key.isEmpty()) return false;
        const QStringList others = work_.ownersOf(key, id);
        QStringList fixed;
        for (const FixedKey& f : fixedKeys())
            if (f.keys.contains(key)) fixed << QString("%1: %2").arg(f.context, f.command);
        if (!others.isEmpty() || !fixed.isEmpty()) {
            QString msg = QString("%1 is already assigned to:\n").arg(keyText(key));
            for (const QString& o : others) msg += "    " + commandName(o) + "\n";
            for (const QString& f : fixed) msg += "    " + f + " (fixed in Reno)\n";
            if (!others.isEmpty()) msg += QString("\nGive it to %1 instead?").arg(commandName(id));
            else msg += QString("\nGiven to %1, the key does that wherever the focus is in the window instead. Assign it?").arg(commandName(id));
            if (!ask(msg)) return false;
        }
        if (replaceKeys) work_.replace(id, key);
        else work_.add(id, key);
        refreshKeys();
        return true;
    }

    // Remove: the key in the box from the selected command, or (box empty) all its keys.
    bool removeKey()
    {
        const QString id = currentId();
        if (id.isEmpty()) return false;
        const QKeySequence key = keyEdit_->keySequence();
        bool done;
        if (key.isEmpty()) {
            done = work_.find(id) >= 0 && !work_.commands[(size_t)work_.find(id)].keys.isEmpty();
            work_.clearKeys(id);
        } else done = work_.remove(id, key);
        refreshKeys();
        return done;
    }

    void resetCommand()
    {
        const QString id = currentId();
        const int i = work_.find(id);
        if (i < 0) return;
        const QList<QKeySequence> def = work_.commands[(size_t)i].defaults;
        work_.clearKeys(id);
        for (const QKeySequence& k : def) work_.add(id, k);
        refreshKeys();
    }

    void setKey(const QKeySequence& k)
    {
        keyEdit_->setKeySequence(k);
        showOwners();
    }
    QTreeWidget* ownersList() { return owners_; }
    QTreeWidget* commandTree() { return tree_; }
    QComboBox* mapsBox() { return maps_; }
    QTabBar* contextBar() { return contexts_; }
    QLineEdit* filterBox() { return filter_; }

    void locate()
    {
        QTreeWidgetItem* it = owners_->currentItem();
        if (!it && owners_->topLevelItemCount()) it = owners_->topLevelItem(0);
        if (!it) return;
        const QString id = it->data(0, Qt::UserRole).toString();
        if (!id.isEmpty()) {
            selectCommand(id);
            return;
        }
        // a fixed key: show its context
        const QString ctx = it->text(0), cmd = it->text(1);
        for (int k = 0; k < contexts_->count(); ++k)
            if (contexts_->tabText(k) == ctx) contexts_->setCurrentIndex(k);
        filter_->clear();
        for (int g = 0; g < tree_->topLevelItemCount(); ++g)
            for (int c = 0; c < tree_->topLevelItem(g)->childCount(); ++c)
                if (tree_->topLevelItem(g)->child(c)->text(0) == cmd) tree_->setCurrentItem(tree_->topLevelItem(g)->child(c));
    }

    void finish()
    {
        applyKeyMap(window_, work_);
        QSettings s;
        s.setValue("keyboard/map", mapName_ == "[Default]" ? QString() : mapName_);
        if (work_.isDefault()) s.remove("keyboard/bindings");
        else s.setValue("keyboard/bindings", work_.toIni(mapName_));
        accept();
    }

private:
    QPushButton* button(const QString& text, const char* name, QHBoxLayout* row)
    {
        QPushButton* b = new QPushButton(text, this);
        b->setObjectName(name);
        b->setAutoDefault(false);
        row->addWidget(b);
        return b;
    }

    bool ask(const QString& text)
    {
        if (keyboardDialogHooks.answer) return keyboardDialogHooks.answer(text);
        return QMessageBox::question(this, "Customize Keyboard", text, QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes;
    }
    void warn(const QString& text)
    {
        if (keyboardDialogHooks.warning) {
            keyboardDialogHooks.warning(text);
            return;
        }
        QMessageBox::warning(this, "Customize Keyboard", text);
    }

    QString commandName(const QString& id) const
    {
        const int i = work_.find(id);
        if (i < 0) return id;
        return work_.commands[(size_t)i].group + " > " + work_.commands[(size_t)i].text;
    }

    QString currentId() const
    {
        if (contexts_->currentIndex() != 0) return QString();
        QTreeWidgetItem* it = tree_->currentItem();
        return it ? it->data(0, Qt::UserRole).toString() : QString();
    }

    static QString keysText(const QList<QKeySequence>& keys)
    {
        QStringList t;
        for (const QKeySequence& k : keys) t << keyText(k);
        return t.join(" or ");
    }

    void fillMaps()
    {
        maps_->clear();
        maps_->addItem("[Default]");
        for (const QString& n : savedKeyMaps()) maps_->addItem(n);
        if (maps_->findText(mapName_) < 0) mapName_ = "[Default]";
        maps_->setCurrentText(mapName_);
        updateButtons();
    }

    void fillTree()
    {
        const QString keep = currentId();
        const QString f = filter_->text().trimmed();
        auto matches = [&f](const QString& a, const QString& b, const QString& c) {
            return f.isEmpty() || a.contains(f, Qt::CaseInsensitive) || b.contains(f, Qt::CaseInsensitive) || c.contains(f, Qt::CaseInsensitive);
        };
        tree_->clear();
        const bool global = contexts_->currentIndex() <= 0;
        std::map<QString, QTreeWidgetItem*> groups;
        std::vector<QString> order;
        auto group = [&](const QString& g) {
            auto it = groups.find(g);
            if (it != groups.end()) return it->second;
            QTreeWidgetItem* gi = new QTreeWidgetItem(QStringList() << g);
            QFont bold = gi->font(0);
            bold.setBold(true);
            gi->setFont(0, bold);
            gi->setFlags(Qt::ItemIsEnabled);
            groups[g] = gi;
            order.push_back(g);
            return gi;
        };
        if (global) {
            for (const KeyCommand& c : work_.commands) {
                const QString keys = keysText(c.keys);
                if (!matches(c.text, c.group, keys)) continue;
                QTreeWidgetItem* it = new QTreeWidgetItem(group(c.group), QStringList() << c.text << keys);
                it->setData(0, Qt::UserRole, c.id);
                if (!sameSet(c.keys, c.defaults)) {
                    QFont fo = it->font(1);
                    fo.setItalic(true);
                    it->setFont(1, fo);
                    it->setToolTip(1, "Changed (default: " + (c.defaults.isEmpty() ? QString("none") : keysText(c.defaults)) + ")");
                }
            }
            note_->setText("The main window's commands: their keys work wherever the focus is in Reno's window.");
        } else {
            const QString ctx = contexts_->tabText(contexts_->currentIndex());
            for (const FixedKey& k : fixedKeys()) {
                if (k.context != ctx || !matches(k.command, k.context, k.keysText)) continue;
                QTreeWidgetItem* it = new QTreeWidgetItem(group(ctx), QStringList() << k.command << k.keysText);
                it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            }
            note_->setText(QString("Keys %1 acts on while it has the keyboard. They are fixed in Reno (VEGAS's defaults) and "
                                   "cannot be changed here; a Global key given the same key takes it over.")
                               .arg(ctx == "Track View" ? QString("the timeline") : "the " + ctx));
        }
        for (const QString& g : order) tree_->addTopLevelItem(groups[g]);
        tree_->expandAll();
        // contexts without a match are greyed, as VEGAS's buttons are
        for (int k = 0; k < contexts_->count(); ++k) {
            bool any = false;
            if (k == 0) {
                for (const KeyCommand& c : work_.commands) any = any || matches(c.text, c.group, keysText(c.keys));
            } else {
                for (const FixedKey& fk : fixedKeys())
                    any = any || (fk.context == contexts_->tabText(k) && matches(fk.command, fk.context, fk.keysText));
            }
            contexts_->setTabTextColor(k, any ? QColor() : palette().color(QPalette::Disabled, QPalette::WindowText));
        }
        if (!keep.isEmpty() && global)
            for (int g = 0; g < tree_->topLevelItemCount(); ++g)
                for (int c = 0; c < tree_->topLevelItem(g)->childCount(); ++c)
                    if (tree_->topLevelItem(g)->child(c)->data(0, Qt::UserRole).toString() == keep) tree_->setCurrentItem(tree_->topLevelItem(g)->child(c));
        updateButtons();
    }

    // after keys changed: row key texts, the map's "changed" state, the owners
    void refreshKeys()
    {
        for (int g = 0; g < tree_->topLevelItemCount(); ++g)
            for (int c = 0; c < tree_->topLevelItem(g)->childCount(); ++c) {
                QTreeWidgetItem* it = tree_->topLevelItem(g)->child(c);
                const int i = work_.find(it->data(0, Qt::UserRole).toString());
                if (i < 0) continue;
                const KeyCommand& kc = work_.commands[(size_t)i];
                it->setText(1, keysText(kc.keys));
                QFont fo = it->font(1);
                fo.setItalic(!sameSet(kc.keys, kc.defaults));
                it->setFont(1, fo);
                it->setToolTip(1, sameSet(kc.keys, kc.defaults) ? QString()
                                                                : "Changed (default: " + (kc.defaults.isEmpty() ? QString("none") : keysText(kc.defaults)) + ")");
            }
        showOwners();
        updateButtons();
    }

    void selected()
    {
        const QString id = currentId();
        const int i = work_.find(id);
        if (i >= 0 && !work_.commands[(size_t)i].keys.isEmpty()) keyEdit_->setKeySequence(work_.commands[(size_t)i].keys.first());
        else if (i >= 0) keyEdit_->clear();
        showOwners();
        updateButtons();
    }

    void showOwners()
    {
        owners_->clear();
        const QKeySequence key = keyEdit_->keySequence();
        if (!key.isEmpty()) {
            for (const KeyCommand& c : work_.commands)
                if (c.keys.contains(key)) {
                    QTreeWidgetItem* it = new QTreeWidgetItem(QStringList() << "Global" << c.group + " > " + c.text << keysText(c.keys));
                    it->setData(0, Qt::UserRole, c.id);
                    owners_->addTopLevelItem(it);
                }
            for (const FixedKey& f : fixedKeys())
                if (f.keys.contains(key)) owners_->addTopLevelItem(new QTreeWidgetItem(QStringList() << f.context << f.command << f.keysText + " (fixed)"));
        }
        if (owners_->topLevelItemCount()) owners_->setCurrentItem(owners_->topLevelItem(0));
        updateButtons();
    }

    void updateButtons()
    {
        if (!add_) return;
        const bool cmd = !currentId().isEmpty();
        const bool key = !keyEdit_->keySequence().isEmpty();
        add_->setEnabled(cmd && key);
        replace_->setEnabled(cmd && key);
        remove_->setEnabled(cmd);
        const int i = work_.find(currentId());
        reset_->setEnabled(i >= 0 && !sameSet(work_.commands[(size_t)i].keys, work_.commands[(size_t)i].defaults));
        delete_->setEnabled(mapName_ != "[Default]");
        locate_->setEnabled(owners_->topLevelItemCount() > 0);
        keyEdit_->setEnabled(contexts_->currentIndex() == 0);
    }

    QMainWindow* window_;
    KeyMap work_, loaded_;
    QString mapName_;
    QComboBox* maps_ = nullptr;
    QPushButton *saveAs_ = nullptr, *delete_ = nullptr, *import_ = nullptr, *export_ = nullptr;
    QTabBar* contexts_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* note_ = nullptr;
    QKeySequenceEdit* keyEdit_ = nullptr;
    QPushButton *add_ = nullptr, *replace_ = nullptr, *remove_ = nullptr, *reset_ = nullptr, *locate_ = nullptr;
    QTreeWidget* owners_ = nullptr;
};

}  // namespace

bool runCustomizeKeyboard(QMainWindow* window)
{
    CustomizeKeyboardDialog d(window);
    return d.exec() == QDialog::Accepted;
}

QDialog* makeCustomizeKeyboardDialog(QMainWindow* window)
{
    return new CustomizeKeyboardDialog(window);
}

}  // namespace app
}  // namespace reno
