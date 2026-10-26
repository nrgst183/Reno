// Declarations shared by the mainwindow*.cpp files.
#pragma once
#include <QDialog>
#include "mainwindow.h"
#include <QActionGroup>
#include <QIcon>
#include <QMenu>
#include <QWidget>
#include <map>
#include <vector>

namespace reno {
namespace app {

class Playback;

enum class Ico {
    New, Open, Save, SaveAs, Render, Properties, Cut, Copy, Paste, Undo, Redo,
    ToolNormal, ToolEnvelope, ToolSelect, ToolZoom, Delete, Trim, TrimStart, TrimEnd, Split, Lock, IgnoreGroup, Ripple, Snap,
    Quantize, Marker, Region, AutoCrossfade, LockEnvelopes, Group, Ungroup, ClearGroup, Cursor, SelectionEnd, SelectionLength,
    Settings, Downmix, Dim, Mixer, Float, Close,
    ToolShuffle, ToolSlip, ToolSlide, ToolStretch, ToolSplitTrim,
    Tutorials, WhatsThis, ColorGrading, NestParent, NestOpen, NestCreate
};


// The drawn icon for one tool bar command.
QIcon toolbarIcon(Ico k);

// Version number saved window layouts carry (QMainWindow::saveState).
const int kLayoutVersion = 4;

// Main window actions by name, and the groups and menus built from them.
struct MainWindow::Actions {
    std::map<std::string, QAction*> all;
    QAction* get(const char* name) const
    {
        auto it = all.find(name);
        return it == all.end() ? nullptr : it->second;
    }
    QActionGroup* tools = nullptr;
    QActionGroup* ripple = nullptr;
    std::vector<QAction*> loadLayouts;
    QMenu* toolbars = nullptr;          // View > Toolbars
};

// Where the docking area is: over the timeline, or under it when Preferences > Display "Display timeline at bottom of
// main window" is cleared.
Qt::DockWidgetArea dockArea();

// Applies Preferences > Display's tab position and docking options.
void applyDockOptions(QMainWindow* w, const std::vector<QDockWidget*>& docks);

// Create Nested Timeline (Alt+C), Open Nested Timeline (Alt+N), Open Parent (Alt+P).
void createNestedTimeline(QWidget* parent);
void openNestedTimeline(QWidget* parent);
void openParentTimeline();

// Deletes the prerendered video files the project made ("Save active prerenders on project close").
void dropPrerenders(Playback* pb);

// Project Notes window (Alt+6) and Help > Keyboard Shortcuts dialog.
QWidget* makeNotesPanel(QWidget* parent);
QDialog* makeShortcutsDialog(QWidget* parent);

}  // namespace app
}  // namespace reno
