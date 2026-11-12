// The touch shell's parts, shared by touch_shell.cpp (layout, bars, tools) and touch_sheets.cpp (the sheets tools open).
#pragma once
#include "appstate.h"
#include "mainwindow.h"
#include "playback.h"
#include "touch_timeline.h"
#include "touch_widgets.h"
#include "touch_shell.h"
#include <QBoxLayout>
#include <QElapsedTimer>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <set>
#include <vector>

namespace reno {
namespace app {

// An export going on (touch_export.cpp): the render runs on a thread; these are read on both.
struct ExportJob {
    QString path, format;
    std::atomic<double> fraction{0};
    std::atomic<bool> cancel{false}, done{false};
    bool ok = false;                        // (set before 'done')
    std::string error;
    QElapsedTimer clock;
    std::function<void()> onProgress, onDone;   // called on the interface's thread
};

struct TouchShellImpl {
    TouchShell* q;
    MainWindow* mw;
    AppState& app = AppState::get();
    Playback* pb;
    int listener = 0;

    // ---- parts
    QWidget* topBar = nullptr;
    touch::Button* menuB = nullptr;
    QLabel* title = nullptr;
    touch::Button* undoB = nullptr;
    touch::Button* redoB = nullptr;
    touch::Button* exportB = nullptr;
    QWidget* previewBox = nullptr;          // black area the preview sits in
    QPointer<QWidget> preview;              // the main window's Video Preview, while it is here
    QWidget* previewHome = nullptr;         // where it came from
    QObject* pictureTap = nullptr;          // a tap on the picture plays or pauses
    QWidget* transport = nullptr;
    QLabel* timeText = nullptr;
    touch::Button* playB = nullptr;
    touch::Button* fullB = nullptr;
    TouchTimeline* timeline = nullptr;
    QScrollArea* toolScroll = nullptr;
    QWidget* tools = nullptr;
    QWidget* backdrop = nullptr;            // catches a tap outside an open sheet
    QWidget* toast = nullptr;               // short messages
    std::function<void(const std::string&)> oldStatus;     // the desktop window's status line (kept, and given back)
    QPointer<touch::Sheet> sheet;
    QTimer* clock = nullptr;                // the time while playing
    bool fullPreview = false;
    QString toolsKey;                       // what the tool bar was made for
    std::shared_ptr<ExportJob> exportJob;
    std::thread exportThread;
    std::map<QString, std::function<void()>> toolFns;
    QStringList toolOrder;

    TouchShellImpl(TouchShell* shell, MainWindow* window);

    // ---- touch_shell.cpp
    void build();
    void buildTopBar();
    void buildTransport();
    void layout();
    void placeSheet(touch::Sheet* s, bool animate);
    void attachPreview();
    void detachPreview();
    void changed(int changes);
    void refreshTitle();
    void refreshTime();
    void refreshTools(bool force = false);
    void addTool(QBoxLayout* row, const QString& name, touch::Icon icon, const QString& label, std::function<void()> fn, bool danger = false);
    void togglePlay();
    void say(const QString& text);          // a short message (a toast)
    void setFullPreview(bool on);
    bool runAction(const char* name);       // one of the main window's commands
    const Event* soleEvent(const Track** track = nullptr) const;   // the selected event, when there is one
    std::set<uint64_t> selectionWithGroups() const;
    bool selectionHasVideo() const;     // (counting the events grouped with the selected ones)
    bool selectionHasAudio() const;

    // ---- touch_sheets.cpp
    touch::Sheet* newSheet(const QString& title);
    void projectMenu();
    // ---- touch_export.cpp
    void exportSheet();
    void startExport(bool soundOnly, int shortSide, const QString& folder, const QString& stem);
    void exportProgressSheet();
    void stopExport();
    // ---- touch_sheets.cpp
    void addMedia(bool audioOnly);
    void addTrackSheet();
    void trackSheet(uint64_t trackId);
    void speedSheet();
    void levelSheet();
    void fadeSheet();
    void textSheet(bool editSelected);
    void transitionSheet();
    void fxSheet(const FxTarget& target);
    void addFxSheet(const FxTarget& target);
    void effectSheet(const FxTarget& target, uint64_t slotId);
    void moreSheet();
    void commandsSheet();
    void duplicateSelection();
    void toggleOnSelection(const char* what, bool Event::*field);
    void addMarker();
};

}  // namespace app
}  // namespace reno
