// The timeline: time display and track headers on the left, marker bar, time selection bar and ruler on top, events
// on tracks, scroll bars with VEGAS's zoom buttons at their ends.
#pragma once
#include "appstate.h"
#include <QRegion>
#include <QWidget>
#include <memory>
#include <set>

class QMenu;
class QDialog;
class QAction;

namespace reno {
namespace app {

class Playback;

// What a left-button drag does in the event area (VEGAS's edit tools). With the envelope tool only envelopes can be
// changed, and a drag across them selects their points. Shuffle, Slip, Slide, Time Stretch/Compress and Split Trim do
// with a plain drag what the normal tool does with keys held (Alt, Ctrl+Alt, Ctrl).
enum class EditTool { Normal, Selection, Zoom, Envelope, Shuffle, Slip, Slide, Stretch, SplitTrim };

enum class RippleMode {
    AffectedTracks,
    AffectedTracksAndMarkers,
    AllTracks
};

// Editing settings that live with the window, not the project (as in VEGAS, where they are tool bar switches). Change
// them through options() and call optionsChanged() afterwards.
struct TimelineOptions {
    EditTool tool = EditTool::Normal;
    bool quantize = true;
    bool showUnquantized = false;
    bool snapToGrid = false;
    bool snapToMarkers = true;
    bool ignoreGrouping = false;
    bool autoRipple = false;
    RippleMode ripple = RippleMode::AffectedTracks;
    bool showWaveforms = true;
    bool showEventButtons = true;
    bool eventHeaders = false;
    bool showEnvelopes = true;      // View > Show Envelopes (Ctrl+Shift+E): envelopes drawn and editable
    bool lockEnvelopes = true;
    bool showBusTracks = false;     // View > Show Bus Tracks (B): the video output, master and bus tracks under the tracks
    bool showFadeLengths = false;
    bool snapToAllEvents = true;    // Options > Snap to All Events (Ctrl+Shift+F8); off: only to events on the same tracks
    bool showTakeInfo = false;
    bool showMediaMarkers = false;
    // View > Video Event Edge Edit Frames (Ctrl+Shift+O): while an edge is trimmed or an event slipped or slid, the
    // preview shows the frame at that edge
    bool edgeFrames = true;
    bool slipAllTakes = false;
    bool multicam = false;
    bool showMarkerLabels = true;
    bool showFadeHandles = true;
    bool showGainHandle = true;
    bool showEventLength = false;
    // Options > Grid Spacing: what the grid lines (and Snap to Grid, Page Up / Down) go by: "ruler" (the ruler's
    // numbered marks), "minutes", "seconds", "milliseconds", "frames", "measures", or notes of the project's tempo.
    std::string gridSpacing = "ruler";
};

class TimelineWidget : public QWidget {
public:
    explicit TimelineWidget(Playback* playback, QWidget* parent = nullptr);
    ~TimelineWidget() override;

    // The user asked for the effects of something (an event's or a track's FX button, a menu, a dropped effect).
    // 'slotId' is the effect to show first (0: whichever).
    std::function<void(const FxTarget& target, uint64_t slotId)> onOpenFx;
    // An event's Pan/Crop button or menu item, a video track's Track Motion button, an event's Properties.
    std::function<void(uint64_t eventId)> onPanCrop;
    std::function<void(uint64_t trackId)> onTrackMotion;
    std::function<void(uint64_t eventId)> onEventProperties;
    std::function<void(uint64_t mediaId)> onSelectInMedia;
    std::function<void()> onOptionsChanged;
    // A video track's menu is being put together: more items can be added to it (the window adds the compositing
    // modes).
    std::function<void(QMenu* menu, uint64_t trackId)> onTrackMenu;
    // A video track's hamburger menu (and its right-click menu's tail): the window adds Compositing Mode and Make
    // Compositing Parent / Child (onTrackCompositing), and Set Default Track Properties (onTrackDefaults, after
    // Expand Track Layers as in VEGAS).
    std::function<void(QMenu* menu, uint64_t trackId)> onTrackCompositing;
    std::function<void(QMenu* menu, uint64_t trackId)> onTrackDefaults;
    // An audio track's Arm for Record button was clicked (the window's recorder arms it; without this set, the
    // track is just added to or taken out of AppState::armedTracks).
    std::function<void(uint64_t trackId)> onArmTrack;
    // A 5.1 project: the small surround panner in an audio track's header was double-clicked (the window shows the
    // Surround Panner for that track).
    std::function<void(uint64_t trackId)> onOpenSurround;
    // Tests: the right-drag's menu (Move Here / Copy Here / Shuffle Events / Cancel) is handed here instead of being
    // shown; return the item chosen (null: Cancel).
    std::function<QAction*(QMenu*)> rightDragMenu;

    int headerWidth() const;
    void setHeaderWidth(int width);
    std::function<void(int width)> onHeaderWidthChanged;

    TimelineOptions& options();
    void optionsChanged();

    // Repainting. These stand in for QWidget's own so the cached picture of the timeline (see paintEvent) is dropped
    // whenever anything but the cursor asks for a repaint. The cursor asks through updateCursor: the strips where it was.
    void update();
    void update(const QRect& r);
    void update(const QRegion& r);
    void update(int x, int y, int w, int h);
    void scroll(int dx, int dy);
    void scroll(int dx, int dy, const QRect& r);
    void updateCursor(const QRegion& r);
    int layerPaints() const;

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomLarge(bool in);
    void scrollToCursor();
    void centerOnCursor();
    double viewStartTime() const;
    double viewEndTime() const;

    void deleteSelection();
    void cutSelection();
    void pasteInsert();             // pastes at the cursor and pushes later events on those tracks along (two undo steps)
    void paste();
    // Edit > Copy / Paste with the Envelope tool (VEGAS's): the selected envelope's points in the time selection
    // (or its selected points) copied; pasted into the selected envelope at the cursor. False when it does not apply.
    bool copyEnvelopePoints();
    bool pasteEnvelopePoints();
    bool hasEnvelopeClip() const;
    void trimStartToCursor();
    void trimEndToCursor();
    void trimToTimeSelection();
    void nudgeSelection(int frames, int pixels, int tracks);
    void insertMarker();
    void insertRegion();
    void goToMarker(int number);
    // The cursor to the previous (-1) or next (+1): marker or region end; event edge; edit point (event edges and
    // fade ends); edge of a selected event. Each falls back to the project's start or end.
    void cursorToMarker(int dir);
    void cursorToEventEdge(int dir);
    void cursorToEditPoint(int dir);
    void cursorToSelectedEdge(int dir);
    void timeSelectSelectedEvent(int dir);
    void renameSelected();
    void clearGroups();
    void selectGroups();
    void changeTrackHeights(int dir);
    void toggleMinimizeTrack();
    void toggleMaximizeTrack();
    void resetTrackHeights();
    // Set Time at Cursor (VEGAS): a box in the ruler to type the time the cursor is to read; the ruler's start moves
    // to make it so (one undo step)
    void setTimeAtCursor();
    void zoomToFrames();
    bool cancelMouseDrag();

    // ---- keyboard trimming (VEGAS's edge-trimming mode). [ / ] (or Num 7 / Num 9) with an event selected select its
    // start or end for trimming (again: the previous / next edge on that track); with nothing selected they move the
    // cursor to the previous / next event edge. Then Num 1 / 3 trim by a frame, Num 4 / 6 by a pixel; Ctrl
    // time-stretches, Alt slip-trims, Ctrl+Alt slides the edit, Shift ignores grouping. With no edge selected the
    // same keys move or stretch the selected events. Num 5 (Expanded Edit Mode) works on both sides of the edit.
    void selectEdge(int dir);
    void toggleExpandedEdit();
    void exitEditMode();
    bool edgeTrimming() const;
    uint64_t trimEvent() const;
    int trimSide() const;
    void keypadEdit(int frames, int pixels, Qt::KeyboardModifiers mods);
    void jog(int dir);

    // ---- more editing commands (each one undo step)
    // Alt+S: the time selection taken out of the selected events, the rest joined with a five-frame cross-fade (VEGAS
    // 18's Smart Split)
    void smartSplit();
    void convertCutToTransition(int kind);
    void convertTransitionToCut();
    void postEditRipple(RippleMode mode);
    void selectLoopRegion();
    void setLocked(bool locked);
    void insertCdTrack();
    void insertCdIndex();
    void cursorToCd(int dir, bool tracks);
    void insertCommandMarker(const std::string& command, const std::string& parameter);
    // a command marker with its comment, at 'at' (seconds); and Command Properties on an existing one (its
    // marker tag's Edit... or a double-click on it)
    void insertCommandMarker(const std::string& command, const std::string& parameter, const std::string& comment, double at);
    void editCommandMarker(uint64_t markerId);
    void rebuildPeaks();
    void addTakesToSelected(const std::vector<uint64_t>& media);

    void toggleTrackList();
    bool trackListFocused() const;
    void setTrackListFocus(bool on);
    void changeTrackLevel(int dir, bool pan);
    std::set<uint64_t> selectedTracks() const;
    void setSelectedTracks(const std::set<uint64_t>& ids);
    void groupTracks(bool group);
    void collapseTrackGroup(bool collapse);
    void muteSoloTrackGroup(bool solo, bool exclusive);

    void createMulticamTrack();
    bool multicamCut(int take, bool crossfade);
    std::function<void(uint64_t eventId)> onAudioEditor;

    // ---- envelopes
    // VEGAS's V and P on the selected track: the envelope ("volume", "pan"; on a video track "volume" is its
    // composite level) is inserted, shown if hidden, or hidden if showing. Shift+V / Shift+P remove it.
    void toggleTrackEnvelope(const std::string& kind);
    void removeTrackEnvelope(const std::string& kind);
    // E / Shift+E: the next / previous envelope of the selected track becomes the one in front (drawn over the
    // others, so its points can be reached).
    void cycleEnvelope(int dir);
    void fillEnvelopeMenu(QMenu* menu, bool video);

    // ---- VEGAS's commands for its menus (also in the main window's menus)
    // The right-click menus show some of the main window's commands (Open in Trimmer, Selectively Paste Event
    // Attributes, Loop Playback...): commandAction gives the main window's QAction by object name (null: leave the
    // item out). onBeforeMenu lets it bring their ticks up to date first.
    std::function<QAction*(const char* name)> commandAction;
    std::function<void()> onBeforeMenu;
    // Fills 'menu' with what a right-click at 'pos' (in the timeline) would show, without showing it. False if nothing.
    bool buildContextMenu(QMenu* menu, const QPoint& pos);
    void selectEventsToEnd();
    void selectAllAfterCursor(uint64_t onlyTrack = 0);
    void setSelectionToView();
    void setSelectionToProject();
    void minimizeAllTracks();
    void insertEmptyEvent();
    void insertAdjustmentEvent();
    void fadeColorDialog(uint64_t trackId, bool top);
    void duplicateTrack(uint64_t trackId);
    QPoint envelopePointPos(const EnvelopeRef& ref, int index) const;
    QRect busTrackRect(const EnvelopeOwner& owner) const;
    EnvelopeRef selectedEnvelope() const;
    std::vector<int> selectedEnvelopePoints() const;

    double timeAt(int x) const;
    int xAt(double seconds) const;
    QRect trackRect(uint64_t trackId) const;
    QRect headerRect(uint64_t trackId) const;
    enum HeaderPart { PartMenu, PartName, PartMotion, PartFx, PartMute, PartSolo, PartLevel, PartPan, PartArm, PartSurround,
                      PartAuto, PartFade, PartPhase, PartBus };
    QRect eventRectOf(uint64_t eventId) const;
    QRect headerPartRect(uint64_t trackId, HeaderPart part) const;
    QRect eventButtonRect(uint64_t eventId, bool panCrop) const;
    QRect eventMenuRect(uint64_t eventId) const;
    // The hamburger menus without showing them; the event header (its title bar, empty if it has none); the Edit
    // Visible Button Set dialog ('run' is given the dialog to accept or reject); Expand Track Layers' sub-lanes (an
    // event's: 0 the A roll, 1 the B roll, -1 if its track is not expanded).
    void buildTrackHamburger(QMenu* menu, uint64_t trackId);
    void buildEventHamburger(QMenu* menu, uint64_t eventId);
    QRect eventHeaderRect(uint64_t eventId) const;
    void editVisibleButtons(bool video, const std::function<void(QDialog*)>& run);
    bool headerButtonHidden(bool video, const QString& key) const;
    bool trackLayersExpanded(uint64_t trackId) const;
    int eventLayer(uint64_t eventId) const;
    QRect markerTagRect(uint64_t markerId) const;
    QRect loopBarRect() const;
    QString loopLengthLabel() const;
    QRect markerBarRect() const;
    QRect rulerRect() const;
    QRect timeDisplayRect() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void leaveEvent(QEvent*) override;
    bool focusNextPrevChild(bool next) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// The time in the project's ruler format (timecode, seconds, frames, samples or measures and beats), counted from the
// project's ruler start. Used by the timeline and the time readouts around it.
std::string rulerTime(const Project& p, double seconds);
// A time typed in the project's ruler format (or hh:mm:ss;ff, hh:mm:ss.sss, a plain number of seconds...) as seconds
// from the project's start (the ruler's start taken off). False if it can't be read.
bool parseRulerTime(const Project& p, const QString& text, double* seconds);

// VEGAS edits times and names in place: a box over 'r' of 'parent' with 'text' in it (all selected). Enter calls 'done'
// with the contents; Esc or clicking elsewhere drops it. The box deletes itself.
void editTextInPlace(QWidget* parent, const QRect& r, const QString& text, std::function<void(const QString&)> done);

}  // namespace app
}  // namespace reno
