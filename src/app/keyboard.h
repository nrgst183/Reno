// Options > Customize Keyboard, as in VEGAS Pro: every main window command can get other keys or none, and the set can be saved as a
// named keyboard map, chosen later, deleted, exported and imported. Keys in use are kept between runs.
// Changeable: main window commands (VEGAS's "Global" context). Keys the timeline, Trimmer and other docked windows
// handle themselves are
// listed but fixed; a Global key given to one of them takes it over (the dialog warns).
// Keyboard maps are text files (<data folder>/Keyboard Maps/<name>.ini) holding only what differs from the defaults:
//     [Keyboard Map]
//     Name=My keys
//     [Global]
//     split=S
//     split=Ctrl+K          (a second key for the same command)
//     render=               (no key at all)
// Commands in a map that this Reno does not have are skipped.
#pragma once
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

class QDialog;
class QMainWindow;
class QWidget;

namespace reno {
namespace app {

// One command that can have keys.
struct KeyCommand {
    QString id;                         // the action's object name ("split", "saveAs"...)
    QString group;                      // "Edit > Navigate", or "Other commands"
    QString text;                       // name without the & marks
    QList<QKeySequence> defaults;       // Reno's own keys
    QList<QKeySequence> keys;           // current keys
};

// A whole set of keys: the dialog's logic without the dialog (testable on its own).
class KeyMap {
public:
    std::vector<KeyCommand> commands;

    int find(const QString& id) const;  // -1 if no such command
    // Commands that have this key (except 'except').
    QStringList ownersOf(const QKeySequence& key, const QString& except = QString()) const;
    // Add: the command gets the key besides its others. Replace: the key becomes its only key. Either way the key is taken from any
    // other command that had it; those ids are returned. Nothing happens for an empty key or unknown command.
    QStringList add(const QString& id, const QKeySequence& key);
    QStringList replace(const QString& id, const QKeySequence& key);
    bool remove(const QString& id, const QKeySequence& key);
    void clearKeys(const QString& id);
    void resetToDefaults();
    bool isDefault() const;             // every command has its default keys
    bool sameKeys(const KeyMap& other) const;

    // The map file's text (only differences from defaults), and reading one back: keys reset to defaults first, then the
    // file's are applied.
    // Returns false (with 'error') if the text is not a keyboard map; 'skipped' counts unknown commands.
    QString toIni(const QString& name) const;
    bool fromIni(const QString& text, QString* name, QString* error = nullptr, int* skipped = nullptr);
};

// A key as the dialog shows it (Windows style: "Ctrl+Shift+S").
QString keyText(const QKeySequence& key);

// Folder of saved keyboard maps, and the maps in it (display names, sorted).
QString keyMapFolder();
QStringList savedKeyMaps();
QString keyMapPath(const QString& name);
bool validKeyMapName(const QString& name);

// The main window's commands as a KeyMap with their current keys. (The first call remembers each action's keys as its defaults.)
KeyMap windowKeyMap(QMainWindow* window);
// Gives the window's actions the map's keys.
void applyKeyMap(QMainWindow* window, const KeyMap& map);
// Called once the main window has all its commands: remembers defaults, applies saved keys and adds Options > Customize Keyboard...
void installKeyboardMap(QMainWindow* window);
// Re-applies the saved keys (after Options > Import Preferences): defaults if none.
void reloadKeyboardMap(QMainWindow* window);
// Name of the keyboard map in use ("[Default]" for Reno's own keys).
QString currentKeyMapName();
// Options > Customize Keyboard. Returns true if OK was pressed (new keys are then in use and saved).
bool runCustomizeKeyboard(QMainWindow* window);

// For tests: the dialog itself, not shown (OK applies and saves). Answers replace message and file boxes: 'answer' gets each
// Yes/No question, 'warning' each warning, 'name' is asked for Save As, 'file' for Import (false) or Export (true).
QDialog* makeCustomizeKeyboardDialog(QMainWindow* window);
struct KeyboardDialogHooks {
    std::function<bool(const QString&)> answer;
    std::function<void(const QString&)> warning;
    std::function<QString()> name;
    std::function<QString(bool save)> file;
};
extern KeyboardDialogHooks keyboardDialogHooks;

}  // namespace app
}  // namespace reno
