// Options > Customize Keyboard (src/app/keyboard.*): the keyboard map logic alone (Add, Replace, Remove, conflicts, map files),
// then VEGAS's dialog driven through its buttons on the real main window: a command given another key, a key taken from the
// command that had it (with the warning), Locate, the filter and contexts, Save As / choosing / deleting maps, Export and Import,
// OK putting the keys in use (pressed through the window system) and the keys coming back in the next run.
//
//   keyboard_test <output folder>
//
// Build as keys_test (uibuild ... mainwindow, QtGui private include folders in EXTRA_CFLAGS). QT_QPA_PLATFORM=offscreen;
// point HOME at a disposable folder (the maps go in Reno's data folder).
#include "../../src/app/mainwindow.h"
#include "../../src/app/keyboard.h"
#include "../../src/app/smalldialogs.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QTreeWidget>
#include <qpa/qwindowsysteminterface.h>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString plain(QString t) { return t.split('\t')[0].remove('&'); }

void press(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QWindow* win = w->window()->windowHandle();
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyPress, key, mods, QString());
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyRelease, key, mods, QString());
    uitest::settle(30);
}

KeyMap smallMap()
{
    KeyMap m;
    auto cmd = [&](const char* id, const char* group, std::initializer_list<const char*> keys) {
        KeyCommand c;
        c.id = id;
        c.group = group;
        c.text = id;
        for (const char* k : keys) c.defaults << QKeySequence(QString::fromUtf8(k));
        c.keys = c.defaults;
        m.commands.push_back(c);
    };
    cmd("save", "File", {"Ctrl+S"});
    cmd("split", "Edit", {"S"});
    cmd("undo", "Edit", {"Ctrl+Z", "Alt+Backspace"});
    cmd("render", "File", {});
    cmd("layout", "View", {"Alt+D, D"});
    return m;
}

QList<QKeySequence> keysOf(const KeyMap& m, const char* id)
{
    const int i = m.find(id);
    return i < 0 ? QList<QKeySequence>() : m.commands[(size_t)i].keys;
}

QString keysStr(const QList<QKeySequence>& l)
{
    QStringList t;
    for (const QKeySequence& k : l) t << k.toString(QKeySequence::PortableText);
    return t.join(" | ");
}

QAction* windowAction(QMainWindow* w, const char* name)
{
    for (QAction* a : w->actions())
        if (a->objectName() == name) return a;
    return w->findChild<QAction*>(name);
}

void click(QDialog* d, const char* name)
{
    QPushButton* b = d->findChild<QPushButton*>(name);
    if (b && b->isEnabled()) b->click();
    else std::printf("  (button %s %s)\n", name, b ? "disabled" : "missing");
    uitest::settle(20);
}

void selectCommand(QDialog* d, const QString& id)
{
    QTreeWidget* t = d->findChild<QTreeWidget*>("commands");
    for (int g = 0; g < t->topLevelItemCount(); ++g)
        for (int c = 0; c < t->topLevelItem(g)->childCount(); ++c)
            if (t->topLevelItem(g)->child(c)->data(0, Qt::UserRole).toString() == id) t->setCurrentItem(t->topLevelItem(g)->child(c));
    uitest::settle(10);
}

QString currentId(QDialog* d)
{
    QTreeWidgetItem* it = d->findChild<QTreeWidget*>("commands")->currentItem();
    return it ? it->data(0, Qt::UserRole).toString() : QString();
}

void setKey(QDialog* d, const char* k)
{
    d->findChild<QKeySequenceEdit*>("shortcutKeys")->setKeySequence(QKeySequence(QString::fromUtf8(k)));
    uitest::settle(10);
}

int visibleCommands(QDialog* d)
{
    QTreeWidget* t = d->findChild<QTreeWidget*>("commands");
    int n = 0;
    for (int g = 0; g < t->topLevelItemCount(); ++g) n += t->topLevelItem(g)->childCount();
    return n;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoKeyboardTest");
    QApplication::setApplicationName("keyboard_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("keyboard");
    QDir().mkpath(out);
    QDir(keyMapFolder()).removeRecursively();
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    // ------------------------------------------------------------------ the map on its own
    {
        KeyMap m = smallMap();
        check(m.isDefault(), "a new map has the default keys");
        QStringList taken = m.add("split", QKeySequence("Ctrl+K"));
        check(taken.isEmpty() && keysStr(keysOf(m, "split")) == "S | Ctrl+K", "Add keeps the command's key and adds the new one", keysStr(keysOf(m, "split")).toStdString());
        taken = m.add("split", QKeySequence("Ctrl+S"));
        check(taken == QStringList() << "save" && keysOf(m, "save").isEmpty() && keysOf(m, "split").contains(QKeySequence("Ctrl+S")),
              "a key added to one command is taken from the command that had it", taken.join(",").toStdString());
        check(m.ownersOf(QKeySequence("Ctrl+S")) == QStringList() << "split", "ownersOf finds the one command with a key");
        taken = m.replace("undo", QKeySequence("Ctrl+K"));
        check(keysStr(keysOf(m, "undo")) == "Ctrl+K" && taken == QStringList() << "split" && !keysOf(m, "split").contains(QKeySequence("Ctrl+K")),
              "Replace makes the key the command's only one (and takes it from the other)", keysStr(keysOf(m, "undo")).toStdString());
        check(m.remove("split", QKeySequence("S")) && !m.remove("split", QKeySequence("X")), "Remove takes a key away (and says when there was none)");
        check(m.add("nosuch", QKeySequence("Q")).isEmpty() && m.ownersOf(QKeySequence("Q")).isEmpty(), "an unknown command gets nothing");
        check(m.add("split", QKeySequence()).isEmpty() && !m.isDefault(), "an empty key does nothing; the map is no longer the default");
        m.add("render", QKeySequence("Ctrl+M"));
        m.add("layout", QKeySequence("Alt+D, L"));
        const QString ini = m.toIni("My keys");
        check(ini.contains("[Keyboard Map]") && ini.contains("Name=My keys") && ini.contains("save=\r\n") && ini.contains("render=Ctrl+M") &&
                  ini.contains("layout=Alt+D, D") && ini.contains("layout=Alt+D, L") && !ini.contains("\nundo=Alt"),
              "the map file holds only what differs from the defaults", ini.toStdString());
        KeyMap back = smallMap();
        QString name;
        int skipped = -1;
        check(back.fromIni(ini + "nosuchcommand=F5\r\n", &name, nullptr, &skipped) && name == "My keys" && skipped == 1, "a map file reads back (unknown commands skipped)",
              QString("%1 %2").arg(name).arg(skipped).toStdString());
        check(back.sameKeys(m) && m.sameKeys(back), "reading it back gives the same keys", keysStr(keysOf(back, "split")).toStdString());
        KeyMap bad = back;
        QString err;
        check(!bad.fromIni("hello\nthis=is not a map\n", nullptr, &err) && bad.sameKeys(back) && !err.isEmpty(), "a file that is not a map is refused and changes nothing");
        KeyMap dup = smallMap();
        dup.fromIni("[Global]\nsplit=Ctrl+S\n", nullptr);
        check(keysOf(dup, "save").isEmpty() && keysOf(dup, "split") == QList<QKeySequence>() << QKeySequence("Ctrl+S"),
              "a map giving a default key to another command takes it from its default owner");
        check(validKeyMapName("Editing keys") && !validKeyMapName("[Default]") && !validKeyMapName("a/b") && !validKeyMapName("  "), "map names are checked");
    }

    // ------------------------------------------------------------------ the main window
    MainWindow* w = new MainWindow();
    w->resize(1400, 860);
    w->show();
    uitest::settle(300);
    {
        QMenu* options = nullptr;
        for (QAction* a : w->menuBar()->actions())
            if (plain(a->text()) == "Options") options = a->menu();
        QStringList items;
        if (options)
            for (QAction* a : options->actions()) items << (a->isSeparator() ? QString("-") : plain(a->text()));
        const int ck = items.indexOf("Customize Keyboard..."), pr = items.indexOf("Preferences...");
        check(ck >= 0 && pr > ck, "Options has Customize Keyboard... before Preferences (as VEGAS)", items.join(",").toStdString());
        const KeyMap m = windowKeyMap(w);
        const int split = m.find("split");
        check(m.commands.size() > 200, "every command of the window is in the map", std::to_string(m.commands.size()));
        check(split >= 0 && m.commands[(size_t)split].defaults == QList<QKeySequence>() << QKeySequence("S") && m.commands[(size_t)split].group.startsWith("Edit"),
              "Split is there, in the Edit menu, with its key S", split >= 0 ? m.commands[(size_t)split].group.toStdString() : "");
        check(m.isDefault() && currentKeyMapName() == "[Default]", "with nothing saved, the keys are VEGAS's defaults");
        std::set<QString> ids;
        bool unique = true;
        for (const KeyCommand& c : m.commands) unique = unique && ids.insert(c.id).second;
        check(unique, "each command appears once");
    }

    // ------------------------------------------------------------------ the dialog
    QStringList questions, warnings;
    bool answer = true;
    QString nameToGive, fileToGive;
    keyboardDialogHooks.answer = [&](const QString& q) { questions << q; return answer; };
    keyboardDialogHooks.warning = [&](const QString& t) { warnings << t; };
    keyboardDialogHooks.name = [&] { return nameToGive; };
    keyboardDialogHooks.file = [&](bool) { return fileToGive; };
    QDialog* d = makeCustomizeKeyboardDialog(w);
    d->show();
    uitest::settle(100);
    {
        QComboBox* maps = d->findChild<QComboBox*>("keyboardMap");
        check(maps && maps->currentText() == "[Default]" && maps->count() == 1, "the map list starts with [Default] alone");
        check(!d->findChild<QPushButton*>("deleteMap")->isEnabled(), "the default map cannot be deleted");
        QTabBar* ctx = d->findChild<QTabBar*>("contexts");
        QStringList tabs;
        for (int k = 0; k < ctx->count(); ++k) tabs << ctx->tabText(k);
        check(tabs.size() >= 3 && tabs[0] == "Global" && tabs.contains("Track View") && tabs.contains("Trimmer"), "contexts: Global, Track View, Trimmer...", tabs.join(",").toStdString());
        const int all = visibleCommands(d);
        d->findChild<QLineEdit*>("filter")->setText("ripple");
        uitest::settle(20);
        const int some = visibleCommands(d);
        check(some > 0 && some < all / 10, "Show commands containing narrows the list", QString("%1 of %2").arg(some).arg(all).toStdString());
        d->findChild<QLineEdit*>("filter")->clear();
        ctx->setCurrentIndex(tabs.indexOf("Track View"));
        uitest::settle(20);
        check(visibleCommands(d) > 5 && !d->findChild<QPushButton*>("addKey")->isEnabled(), "Track View lists the timeline's keys (fixed: nothing to add)");
        uitest::shot(d, out + "/customize_keyboard_trackview.png");
        ctx->setCurrentIndex(0);
        uitest::settle(20);

        // Split gets Ctrl+K as well as S
        selectCommand(d, "split");
        check(currentId(d) == "split" && d->findChild<QKeySequenceEdit*>("shortcutKeys")->keySequence() == QKeySequence("S"),
              "choosing a command shows its key");
        setKey(d, "Ctrl+K");
        check(d->findChild<QTreeWidget*>("assignedTo")->topLevelItemCount() == 0, "Ctrl+K is free");
        click(d, "addKey");
        KeyMap now = windowKeyMap(w);
        check(questions.isEmpty(), "a free key is added without a question");
        // the dialog works on its own copy; the window keeps its keys until OK
        check(keysOf(now, "split") == QList<QKeySequence>() << QKeySequence("S"), "the window's keys stay until OK");

        // Ctrl+S is Save's: the list says so, Locate finds it, and Add asks before taking it
        setKey(d, "Ctrl+S");
        QTreeWidget* own = d->findChild<QTreeWidget*>("assignedTo");
        check(own->topLevelItemCount() == 1 && own->topLevelItem(0)->text(1).contains("Save"), "Shortcut currently assigned to names Save",
              own->topLevelItemCount() ? own->topLevelItem(0)->text(1).toStdString() : "");
        click(d, "locate");
        check(currentId(d) == "save", "Locate selects the command that has the key", currentId(d).toStdString());
        selectCommand(d, "split");
        setKey(d, "Ctrl+S");
        answer = false;
        click(d, "addKey");
        check(questions.size() == 1 && questions[0].contains("Ctrl+S") && questions[0].contains("Save"), "taking a key that is in use asks first",
              questions.join("|").toStdString());
        uitest::shot(d, out + "/customize_keyboard.png");
        answer = true;
        click(d, "addKey");
        setKey(d, "S");
        click(d, "removeKey");  // Split keeps Ctrl+K and Ctrl+S
        // a key the timeline acts on: I (Set In while the timeline has the keyboard)
        selectCommand(d, "render");
        setKey(d, "I");
        questions.clear();
        click(d, "replaceKey");
        check(questions.size() == 1 && questions[0].contains("fixed"), "giving a timeline key to a window command warns that it takes it over", questions.join("|").toStdString());
        // and Remove: the render command loses its I again
        click(d, "removeKey");
        selectCommand(d, "undo");
        setKey(d, "");
        click(d, "removeKey");  // empty box: all of Undo's keys
        selectCommand(d, "undo");
        click(d, "resetCommand");   // Reset gives them back
        QTreeWidgetItem* undoRow = d->findChild<QTreeWidget*>("commands")->currentItem();
        check(undoRow && undoRow->text(1) == "Ctrl+Z or Alt+Backspace", "Remove with an empty box clears a command; Reset gives its keys back",
              undoRow ? undoRow->text(1).toStdString() : "");

        // ---- Save As, a second map, choosing, deleting
        nameToGive = "[Default]";
        click(d, "saveMapAs");
        check(!warnings.isEmpty() && warnings.last().contains("cannot overwrite the default"), "Save As refuses [Default]");
        nameToGive = "bad/name";
        click(d, "saveMapAs");
        check(warnings.last().contains("invalid characters"), "Save As refuses a name with invalid characters");
        nameToGive = "My keys";
        click(d, "saveMapAs");
        check(QFileInfo::exists(keyMapPath("My keys")) && maps->currentText() == "My keys", "Save As writes the map and chooses it", maps->currentText().toStdString());
        // choosing [Default] with nothing changed since the save: no question, the keys go back
        questions.clear();
        maps->setCurrentIndex(maps->findText("[Default]"));
        emit maps->activated(maps->currentIndex());
        uitest::settle(20);
        selectCommand(d, "split");
        check(questions.isEmpty() && d->findChild<QTreeWidget*>("commands")->currentItem()->text(1) == "S", "choosing [Default] brings the default keys back");
        // a change, then choosing My keys: asked (No keeps the change)
        setKey(d, "Ctrl+J");
        click(d, "addKey");
        answer = false;
        maps->setCurrentIndex(maps->findText("My keys"));
        emit maps->activated(maps->currentIndex());
        uitest::settle(20);
        check(questions.size() == 1 && questions[0].contains("discard") && maps->currentText() == "[Default]", "choosing another map with changes asks (No keeps them)");
        answer = true;
        maps->setCurrentIndex(maps->findText("My keys"));
        emit maps->activated(maps->currentIndex());
        uitest::settle(20);
        selectCommand(d, "split");
        check(d->findChild<QTreeWidget*>("commands")->currentItem()->text(1) == "Ctrl+K or Ctrl+S", "choosing My keys loads it",
              d->findChild<QTreeWidget*>("commands")->currentItem()->text(1).toStdString());

        // ---- Export, delete, Import
        fileToGive = out + "/exported keys.ini";
        QFile::remove(fileToGive);
        click(d, "exportMap");
        check(QFileInfo::exists(fileToGive), "Export writes the map to a file");
        click(d, "deleteMap");
        check(!QFileInfo::exists(keyMapPath("My keys")) && maps->currentText() == "[Default]" && maps->count() == 1, "Delete removes the saved map");
        {
            QFile f(out + "/not a map.ini");
            if (f.open(QIODevice::WriteOnly)) f.write("just text\n");
        }
        fileToGive = out + "/not a map.ini";
        warnings.clear();
        click(d, "importMap");
        check(warnings.size() == 1 && maps->count() == 1, "Import refuses a file that is not a keyboard map");
        fileToGive = out + "/exported keys.ini";
        click(d, "importMap");
        selectCommand(d, "split");
        check(maps->currentText() == "My keys" && QFileInfo::exists(keyMapPath("My keys")) &&
                  d->findChild<QTreeWidget*>("commands")->currentItem()->text(1) == "Ctrl+K or Ctrl+S",
              "Import copies the map in, under its own name, and chooses it", maps->currentText().toStdString());
        uitest::shot(d, out + "/customize_keyboard_imported.png");

        // ---- OK: the keys are in use
        int splits = 0, saves = 0;
        QObject::connect(windowAction(w, "split"), &QAction::triggered, [&] { ++splits; });
        QObject::connect(windowAction(w, "save"), &QAction::triggered, [&] { ++saves; });
        QDialogButtonBox* bb = d->findChild<QDialogButtonBox*>();
        bb->button(QDialogButtonBox::Ok)->click();
        uitest::settle(50);
        check(d->result() == QDialog::Accepted, "OK closes the dialog");
        check(windowAction(w, "split")->shortcuts() == QList<QKeySequence>() << QKeySequence("Ctrl+K") << QKeySequence("Ctrl+S") &&
                  windowAction(w, "save")->shortcuts().isEmpty(),
              "OK gives the window's commands the new keys", keysStr(windowAction(w, "split")->shortcuts()).toStdString());
        check(currentKeyMapName() == "My keys", "the map in use is remembered");
        w->activateWindow();
        w->timeline()->setFocus();
        uitest::settle(50);
        press(w->timeline(), Qt::Key_K, Qt::ControlModifier);
        press(w->timeline(), Qt::Key_S, Qt::ControlModifier);
        press(w->timeline(), Qt::Key_S);
        check(splits == 2 && saves == 0, "Ctrl+K and Ctrl+S now split; S no longer does; Save has no key",
              QString("splits %1 saves %2").arg(splits).arg(saves).toStdString());
        delete d;
    }
    delete w;
    uitest::settle(50);

    // ------------------------------------------------------------------ the next run keeps them
    {
        MainWindow w2;
        w2.show();
        uitest::settle(200);
        check(windowAction(&w2, "split")->shortcuts() == QList<QKeySequence>() << QKeySequence("Ctrl+K") << QKeySequence("Ctrl+S") &&
                  windowAction(&w2, "save")->shortcuts().isEmpty(),
              "the next run starts with the keys chosen", keysStr(windowAction(&w2, "split")->shortcuts()).toStdString());
        const KeyMap m = windowKeyMap(&w2);
        const int split = m.find("split");
        check(split >= 0 && m.commands[(size_t)split].defaults == QList<QKeySequence>() << QKeySequence("S"), "... and still knows the defaults");
        // Options > Export Preferences: the keys and the preferences go to a file
        AppState::get().prefs.undoLevels = 77;
        AppState::get().applyPreferences();
        const QString prefsFile = out + "/exported prefs.ini";
        const int written = exportPreferences(prefsFile);
        check(written > 3 && QFileInfo::exists(prefsFile), "Export Preferences writes the settings to a file", std::to_string(written));
        {
            QStringList items;
            for (QAction* a : w2.menuBar()->actions())
                if (plain(a->text()) == "Options")
                    for (QAction* o : a->menu()->actions()) items << o->objectName();
            const int ck = items.indexOf("customizeKeyboard"), ex = items.indexOf("exportPreferences"), im = items.indexOf("importPreferences"),
                      pr = items.indexOf("preferences");
            check(ck >= 0 && ex > ck && im > ex && pr > im, "Options: Customize Keyboard, Export / Import Preferences, Preferences (VEGAS's order)", items.join(",").toStdString());
        }
        // back to [Default] through the dialog
        QDialog* d2 = makeCustomizeKeyboardDialog(&w2);
        QComboBox* maps = d2->findChild<QComboBox*>("keyboardMap");
        check(maps->currentText() == "My keys", "the dialog opens on the map in use");
        maps->setCurrentIndex(maps->findText("[Default]"));
        emit maps->activated(maps->currentIndex());
        d2->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        uitest::settle(20);
        check(windowAction(&w2, "split")->shortcuts() == QList<QKeySequence>() << QKeySequence("S") &&
                  windowAction(&w2, "save")->shortcuts() == QList<QKeySequence>() << QKeySequence("Ctrl+S") && !QSettings().contains("keyboard/bindings"),
              "[Default] and OK restore VEGAS's keys");
        delete d2;
        // Import Preferences brings the keys and the preferences back
        AppState::get().prefs.undoLevels = 200;
        AppState::get().applyPreferences();
        QString err;
        check(importPreferences(&w2, out + "/not a map.ini", &err) < 0 && !err.isEmpty(), "Import Preferences refuses a file that is not one");
        const int read = importPreferences(&w2, prefsFile, &err);
        check(read == written && AppState::get().prefs.undoLevels == 77 &&
                  windowAction(&w2, "split")->shortcuts() == QList<QKeySequence>() << QKeySequence("Ctrl+K") << QKeySequence("Ctrl+S") &&
                  currentKeyMapName() == "My keys",
              "Import Preferences puts the preferences and the keys back at once", std::to_string(read));
    }
    keyboardDialogHooks = KeyboardDialogHooks();
    return uitest::summary();
}
