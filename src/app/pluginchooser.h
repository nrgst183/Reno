// VEGAS's Plug-In Chooser: lists every plug-in of one kind in folders (FX at the top with every plug-in, then
// packages, Third Party, VEGAS, OFX or the audio formats, OpenFX groupings or audio makers, and the user's own
// folders). A chain of effects is built in the bar across the top and saved as a package (Save As...); OK applies it.
// VEGAS opens it from an FX button with no effects yet, from the effects window's "Plug-In Chain" button, and for
// Insert > Generated Media.
#pragma once
#include "appstate.h"
#include <QDialog>
#include <functional>
#include <memory>

class QMenu;
class QAction;
class QTreeWidget;
class QListWidget;

namespace reno {
namespace app {

enum class ChooserKind { VideoFx, AudioFx, Transition, Generator };

class PluginChooser : public QDialog {
public:
    // 'chain': what the target has now (its slots keep their ids; their states are what a saved package holds).
    // Transitions and generators pick one plug-in: no chain row, and OK takes the plug-in picked.
    PluginChooser(ChooserKind kind, const QString& title, const FxChain& chain, QWidget* parent = nullptr);
    ~PluginChooser() override;

    // The chain as it is in the window: existing slots keep their ids; new ones have id 0 (and the settings a package
    // gave them in 'state').
    FxChain chain() const;
    bool picked(FxSlot* out) const;

    // For tests: the folder shown (its listed name, or "All" for the top one), the plug-ins listed (labels), the folders
    // listed before them (the folder's own subfolders, as VEGAS lists them), adding one by label.
    bool openFolder(const QString& name);
    QStringList listed() const;
    QStringList listedFolders() const;
    bool addByLabel(const QString& label);
    QStringList folders() const;

    static std::function<void(PluginChooser*)> onShown;
    // Tests: answers the "Save Plug-In Package" question instead of the dialog (empty: cancelled).
    static std::function<QString(const QStringList& existing)> packageNameForTests;
    // Tests: the right-click menus of the folders and list are handed here instead of being shown (return the item to
    // act on, or null), and requested names (new folder, rename) are answered here (empty: cancelled).
    static std::function<QAction*(QMenu*)> menuForTests;
    static std::function<QString(const QString& suggested)> folderNameForTests;
    // The user's own folders (Create New Folder in the folder tree's right-click menu): created, renamed, deleted, and
    // the listed entry 'label' put in one (like the list's "Add to Folder"). False if it can't be done.
    bool createFolder(const QString& name);
    bool renameFolder(const QString& from, const QString& to);
    bool deleteFolder(const QString& name);
    bool addToFolder(const QString& label, const QString& folder);
    QTreeWidget* folderTree() const;
    QListWidget* choiceList() const;
    bool savePackageAs(const QString& name, std::string* error);

protected:
    void showEvent(QShowEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

ChooserKind chooserKindFor(const FxTarget& t);
// "Video Event FX", "Audio Track FX", "Video Output FX"... (VEGAS's names for the chains)
QString chainTitle(const FxTarget& t);

// Opens the chooser on a target's chain; on OK the chain becomes the choice, as one undo step. Returns true on OK
// (with the first effect added in *firstNew, if any).
bool choosePluginsFor(QWidget* parent, const FxTarget& target, uint64_t* firstNew = nullptr);
// Makes 'chosen' the target's chain (one undo step "Plug-In Chain"): slots whose id the chain has stay as they are;
// others are added with new ids. Returns the id of the first one added (0 if none).
uint64_t applyChain(const FxTarget& target, const FxChain& chosen, const std::string& what = "Plug-In Chain");

// VEGAS's right-click menu of an FX button (track header, bus, console): Enable All, Bypass All and Delete All for
// the target's chain (each one undo step). Items are appended to 'm'.
void fillChainMenu(QMenu* m, const FxTarget& target);
// The audio plug-ins as a menu with submenus by maker (VEGAS's Add New Insert FX menu); 'chosen' is called with the
// plug-in picked. Plug-ins that don't work are left out.
void fillAudioPluginMenu(QMenu* m, const std::function<void(const AudioPluginDesc&)>& chosen);
uint64_t addChainToTarget(const FxTarget& target, const FxChain& add);

// What the Video FX / Transitions / Media Generators windows hand out for a package or preset (drag data,
// double-click argument): a package's text (fxlibrary's packageToText; a preset is a one-effect package with its
// settings). Adds it to 'target' (one undo step) and returns the first new effect; 0 if 'data' is not such a package.
uint64_t addFxData(const FxTarget& target, const std::string& data);
bool isFxPackageData(const std::string& data);
// Generated media made by the generator a package or preset names, with its settings (one undo step). 0 if none.
uint64_t makeGeneratedMediaFromData(const std::string& data);
uint64_t makeGeneratedMedia(const FxSlot& generator);

// What an FX button does first: with no effects on 'target', the chooser. Returns true if the effects window should
// not be opened after all (Cancel, or OK with no plug-ins); else *slotId is the first effect added.
bool pluginChooserFirst(QWidget* parent, const FxTarget& target, uint64_t* slotId);
// Insert > Generated Media: the chooser, then the media on the timeline at the cursor (in the selected track if it is
// a video track) and its Media Generator window ('openFx' with the generator as its target).
void insertGeneratedMedia(QWidget* parent, const std::function<void(const FxTarget&)>& openFx);

// The chooser for one generator ("Plug-In Chooser - New Media Generator"), then the generated media is created (one
// undo step). Returns the new media's id (0 if cancelled).
uint64_t chooseGeneratedMedia(QWidget* parent);

}  // namespace app
}  // namespace reno
