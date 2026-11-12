// The main window, laid out like VEGAS Pro's: menu and main tool bar on top, docking area (default: tab group at the
// left with Project Media,
// Explorer, Video FX, Audio FX, Media Generators, Transitions and others when opened; Video Preview beside it; narrow
// Master Bus at the
// right; other layouts in View > Window Layouts), timeline with transport and edit tool bar below, status bar. Also the
// keyboard shortcuts (VEGAS defaults).
#pragma once
#include "appstate.h"
#include "playback.h"
#include "panels.h"
#include <QMainWindow>
#include <map>
#include <vector>

class QAction;
class QLabel;
class QDockWidget;
class QMenu;
class QTimer;

namespace reno {
namespace app {

class TimelineWidget;
class TouchShell;
class FxWindow;
class VegasCommandBuilder;

// One line of the keyboard shortcut list (Help > Keyboard Shortcuts).
struct ShortcutInfo {
    enum Kind {
        Window,         // window command: works wherever the focus is in the window (except while typing)
        Timeline,       // key the timeline handles when focused
        Panel,          // key a docked window handles when focused ('menu' names it)
        NotAvailable    // VEGAS has it; Reno cannot yet ('note' says why)
    };
    const char* menu;
    const char* command;
    const char* keys;       // as shown; alternatives joined by " or ", sequences by ", "
    Kind kind;
    const char* note;
};
const std::vector<ShortcutInfo>& shortcutList();

class MainWindow : public QMainWindow {
public:
    MainWindow();
    ~MainWindow() override;

    Playback* playback() { return playback_.get(); }
    TimelineWidget* timeline() { return timeline_; }
    FxWindow* fxWindow() { return fxWindow_; }
    PreviewWidget* preview() { return preview_; }
    void openFx(const FxTarget& target, uint64_t slotId);
    // A Reno project, or a VEGAS project (.veg) brought in as a new, unsaved project.
    bool openProject(const QString& path);
    bool offerRecovery();                       // after a crash: restore the auto-saved project? (recovery.h)
    bool copyMediaWithProject(const QString& folder);   // Save As's "Copy media with project"
    QString projectFolder() const;
    void showShortcuts();               // Help > Keyboard Shortcuts

    // View > Window Layouts, as in VEGAS. Built in: Default Layout (Alt+D, D), Large Preview Layout (Alt+D, V; the
    // arrangement of the user's
    // VEGAS 18 window) and Audio Mixing Layout (Alt+D, A). Saved layouts have a name and optionally a number:
    // Ctrl+Alt+D, n saves as layout n,
    // Alt+D, n recalls it. Kept in QSettings "windowLayouts", along with the layout in use when Reno closed.
    enum BuiltinLayout { DefaultLayout, LargePreviewLayout, AudioMixingLayout };
    struct SavedLayout {
        QString name;
        int key = -1;                   // 0..9, or -1 for none
        QByteArray state;               // QMainWindow::saveState
        int version = 0;                // state version
    };
    void applyLayout(BuiltinLayout which);
    void restoreDefaultLayout() { applyLayout(DefaultLayout); }
    bool recallLayout(int key);         // false if no layout has that number
    bool recallLayout(const QString& name);
    void saveLayout(const QString& name, int key);  // replaces one of the same name and takes the number from any other
    static std::vector<SavedLayout> savedLayoutList();
    static void setSavedLayoutList(const std::vector<SavedLayout>& list);
    QString currentLayout() const { return currentLayout_; }   // name of the layout in use ("" if changed by hand)
    static QString builtinLayoutName(BuiltinLayout which);

    // Windows and tools added by mainwindow_tools.cpp (Trimmer, recording, Loudness Meters...); see mainwindow_tools.h.
    struct Tools;
    Tools* tools() { return tools_.get(); }
    // An AppState listener of something added to this window from outside its class (it uses the window or its
    // playback): removed as the window's destructor begins, before any of what it uses is gone.
    void ownListener(int id) { ownedListeners_.push_back(id); }

protected:
    void closeEvent(QCloseEvent*) override;
    void showEvent(QShowEvent*) override;

private:
    struct Actions;
    class ActionBuilder;
    void buildActions();
    void addFileActions(ActionBuilder& b);
    void addEditActions(ActionBuilder& b);
    void addNavigationActions(ActionBuilder& b);
    void addGroupToolAndPitchActions(ActionBuilder& b);
    void addViewActions(ActionBuilder& b);
    void addInsertActions(ActionBuilder& b);
    void addToolsActions(ActionBuilder& b);
    void addOptionsActions(ActionBuilder& b);
    void addEnvelopeActions(ActionBuilder& b);
    void addTrackActions(ActionBuilder& b);
    void addPlaybackActions(ActionBuilder& b);
    void addMoreEditingActions(ActionBuilder& b);
    void addHelpActions(ActionBuilder& b);
    void connectTimeline();
    void buildStatusBar();
    void startTimers();
    void prefsChanged();
    void restoreWindowLayout();
    void watchLayoutChanges();
    void buildMenus();
    void buildFileMenu();
    void buildEditMenu();
    void buildViewMenu();
    void buildInsertMenu();
    void buildToolsMenu();
    void buildOptionsMenu();
    void buildHelpMenu();
    void fillRecentProjects(QMenu* file, QAction* recentStart);
    void updateSwitchesMenu(QMenu* sw);
    void buildToolBars();
    void buildDocks();
    QWidget* buildBottomBar();
    void refresh(int changes);
    bool maybeSave();                   // false if the user cancelled
    bool saveProject(bool as);
    void saveVersion();
    void browserActivated(FxBrowser::Kind kind, const std::string& data);
    QDockWidget* dock(const QString& title, const QString& name, QWidget* w);
    // The same, the window made by 'make' as a child of the dock's page (a window made elsewhere and moved in is styled twice).
    QDockWidget* dock(const QString& title, const QString& name, const std::function<QWidget*(QWidget*)>& make);
    void updateDockTabs();              // a docked window's own tab shows while it is alone in its place
    void compositingMenu(QMenu* menu, uint64_t trackId);    // added to a video track's menu
    void showDiskSpace();               // status bar's right-hand side
    void restyle();                     // colours of the window's own bars
    void stepTrack(int dir);            // Alt+Shift+Up / Down: previous or next track becomes selected
    void showDock(QDockWidget* d);      // shown, raised in its tabs and focused
    void cycleFocus(int dir);           // F6 / Shift+F6
    void goToDialog(bool selectionEnd); // Ctrl+G / Ctrl+Shift+G
    void typedTime(int what, const QString& text);  // a time typed in place: 0 cursor, 1 selection end, 2 selection length
    void pasteRepeat();
    void changePitch(double semitones, bool reset);
    void muteSolo(bool solo, bool exclusive);
    void shuttle(int dir);              // J (-1), K (0), L (+1)
    void connectRatePanel();            // the Rate slider controls playback
    void updateRate();                  // and shows the playing rate
    void buildRamPreview();             // Shift+B
    void renderToNewTrack();            // Ctrl+M
    void syncOptions();                 // tool bars and menus follow the timeline's options
    void fillLayoutsMenu(QMenu* menu);
    void saveLayoutAsDialog();
    void organizeLayouts();
    void reloadLayout();                // the layout in use, as saved
    void chooseTakeDialog(bool remove); // Edit > Take > Choose Active... / Delete...
    void insertCommandDialog();         // Insert > Command... (C)
    void showHelp();                    // Help > Contents and Index (F1)
public:
    // Tools > Audio > Open in Audio Editor (Ctrl+E): the event's media in the program set in Preferences (asked the
    // first time). False if nothing started.
    bool openInAudioEditor(uint64_t eventId);
private:
    void buildToolWindows();            // (mainwindow_tools.cpp)
    void addToolWindow(QDockWidget* d, const char* actionName, const char* key);
    void setUpTrimmer(Tools* t);
    void setUpRecording(Tools* t);
    void setUpSurroundPanner(Tools* t);
    void setUpExternalMonitor(Tools* t);
    void setUpColorGrading(Tools* t);
    void setUpMidiSync(Tools* t);
    void setUpPrerender(Tools* t);
    void addAdjustmentTrackCommand(Tools* t);
    // VEGAS Pro 18's menus (mainwindow_menus.cpp): commands Reno lacked, and every menu in VEGAS's order and names.
    void buildVegasMenus();
    void addVegasFileCommands(VegasCommandBuilder& cmd);
    void addVegasEditCommands(VegasCommandBuilder& cmd);
    void addVegasViewCommands(VegasCommandBuilder& cmd);
    void addVegasInsertCommands(VegasCommandBuilder& cmd);
    void addVegasToolsCommands(VegasCommandBuilder& cmd);
    void addVegasOptionsCommands(VegasCommandBuilder& cmd);
    void arrangeVegasMenus(VegasCommandBuilder& cmd);
    void arrangeVegasFileMenu(QMenu* menu);
    void arrangeVegasEditMenu(QMenu* menu, VegasCommandBuilder& cmd);
    void arrangeVegasViewMenu(QMenu* menu);
    void arrangeVegasInsertMenu(QMenu* menu);
    void arrangeVegasToolsMenu(QMenu* menu);
    void arrangeVegasOptionsMenu(QMenu* menu);
    void addVegasAudioCommands(VegasCommandBuilder& cmd);
    void showMediaFx();
    void addMissingStreams();
    void openCopyInAudioEditor();
    void createEventSubclip();
    void updateVegasCommands();         // their ticks and greying for the current selection and project
    void selectivePasteDialog();        // Edit > Selectively Paste Event Attributes...
public:
    QAction* vegasAction(const char* name) const;   // one of the window's commands by object name (null: none)
    // The touch interface (touch_shell.h) in place of the menus, tool bars and docked windows, or back
    // (mainwindow_touch.cpp; View > Touch Interface).
    void setTouchMode(bool on);
    bool touchMode() const { return touch_ != nullptr; }
    TouchShell* touchShell() const { return touch_; }
    bool maybeSaveChanges() { return maybeSave(); }    // asks to save a changed project; false if the user cancelled
private:
    void addTouchAction();
    TouchShell* touch_ = nullptr;
    QWidget* desktopCentral_ = nullptr;         // the timeline area, while the touch interface is in its place
    QByteArray touchSavedState_;
    std::vector<QWidget*> touchHidden_;

    std::unique_ptr<Playback> playback_;
    TimelineWidget* timeline_ = nullptr;
    FxWindow* fxWindow_ = nullptr;
    MediaBin* mediaBin_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    QLabel* statusText_ = nullptr;
    QLabel* diskText_ = nullptr;
    QLabel *cursorText_ = nullptr, *selEnd_ = nullptr, *selLength_ = nullptr;
    QWidget* ratePanel_ = nullptr;      // under the track headers, as wide as they are
    bool shuttleKeyHeld_ = false;       // unused: K is a press, not a hold
    bool jogging_ = false;
    bool slowMode_ = false;             // K was pressed: J / L now step the rate by a quarter
    double slowRate_ = 0;               // rate slow mode last set
    QObject* shuttleFilter_ = nullptr;
    static constexpr double kJogRate = 0.25;
    QDockWidget* mixerDock_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QWidget* shortcutsDialog_ = nullptr;
    QString currentLayout_;
    bool firstShow_ = true;             // layout to start with is arranged once the window has its size
    bool startLayoutRestored_ = false;  // layout in use at last close was restored
    std::vector<QDockWidget*> docks_;
    bool timelineMaximized_ = false;
    QByteArray beforeMaximize_;
    std::map<int, std::vector<QDockWidget*>> hiddenByArea_;   // View > Show / Hide the Top / Bottom / Right Docking Area
    // Window size and place, docked layout and layout name are saved shortly after each change, not only at close, so a
    // crash keeps them.
    QTimer* layoutSaver_ = nullptr;
    void saveWindowLayout();
    // Backspace: earlier time selections (newest last)
    std::vector<std::pair<double, double>> selectionHistory_;
    std::pair<double, double> lastSelection_{0, 0};
    bool restoringSelection_ = false;
    int listener_ = 0;
    std::vector<int> ownedListeners_;   // ownListener()
    std::unique_ptr<Actions> a_;
    std::unique_ptr<Tools> tools_;
};

// Dark colours for the whole application (VEGAS Pro 18's scheme).
void applyTheme();

// Puts the menu bar's menus in VEGAS Pro 18's order and wording (menuconform.cpp).
void conformMenusToVegas(QMenuBar* bar);

}  // namespace app
}  // namespace reno
