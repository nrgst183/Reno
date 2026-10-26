// The panels docked round the timeline (preview, project media, plug-in lists) and the dialogs.
#pragma once
#include "appstate.h"
#include <QColor>
#include <QImage>
#include <QRectF>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

namespace reno {
namespace app {

class Playback;

// What is dragged between panels (QMimeData formats), as UTF-8 text:
extern const char* const kMimeMedia;        // media item: its id as a decimal number
extern const char* const kMimeVideoFx;      // video effect: "plug-in identifier\nlabel"
extern const char* const kMimeAudioFx;      // audio effect: AudioPluginDesc::toJson() text
extern const char* const kMimeGenerator;    // media generator: "plug-in identifier\nlabel"
extern const char* const kMimeTransition;   // transition: "plug-in identifier\nlabel"

// Media double-clicked in Project Media (mediaId) or the Explorer (path, mediaId 0): the main window shows it in the
// Trimmer. If unset, the media goes on the timeline at the cursor.
extern std::function<void(uint64_t mediaId, const QString& path)> openInTrimmer;
// Start / Stop Preview in the Explorer and Project Media: the main window plays the file in the Trimmer (start false:
// stops). Also a project file double-clicked in the Explorer opens.
extern std::function<void(uint64_t mediaId, const QString& path, bool start)> previewMedia;
extern std::function<void(const QString& path)> openProjectFile;
// Called after Project Media's Create Video Proxy made proxies (the main window shows the preview from them).
extern std::function<void()> onProxiesMade;

// VEGAS's preview quality (Draft, Preview, Good, Best) and size (Auto: the window's size; Full, Half, Quarter: of the project frame).
enum class PreviewQuality { Draft, Preview, Good, Best };
enum class PreviewSize { Auto, Full, Half, Quarter };

// The Video Preview window: a tool bar (project properties, video output effects, bypass, preview quality, overlays,
// snapshots), the image, and lines about project and preview sizes.
class PreviewWidget : public QWidget {
public:
    explicit PreviewWidget(Playback* playback, QWidget* parent = nullptr);
    ~PreviewWidget() override;
    // The tool bar's Video Output FX and Project Video Properties buttons.
    std::function<void(const FxTarget& target, uint64_t slotId)> onOpenFx;
    std::function<void()> onProjectProperties;
    void setQuality(PreviewQuality quality, PreviewSize size);     // remembered
    PreviewQuality quality() const;
    PreviewSize size() const;
    // Size the image is drawn at for the current quality, before fitting to the window (for tests).
    QSize previewSize() const;
    // The effect whose on-screen controls (OpenFX overlay) are drawn over the image and take the mouse: the one open in
    // the Video FX window
    // (slotId 0: none). Changes are undo steps. Hidden while playing, while effects are bypassed, and for an event's
    // effect when the cursor is
    // not over the event. "Show Video FX Overlays" in the Overlays menu turns them off.
    void setOverlayEffect(uint64_t slotId, const FxTarget& target);
    uint64_t overlayEffect() const;
    bool showOverlays() const;
    void setShowOverlays(bool on);      // remembered
    // For tests: whether the controls are in use and drawn; the widget the image is in; where a position in the effect's
    // canonical coordinates (its frame's pixels from the bottom left) is in that widget.
    bool overlayActive() const;
    bool overlayDrawn() const;
    QWidget* pictureWidget() const;
    // A picker over the image (Color Grading eyedroppers): while set, the mouse picks instead of drawing or moving. It
    // gets the state (0 moving, 1 pressed,
    // 2 dragged, 3 released), the area picked as fractions of the image from the top left (a point unless dragging) and
    // the image there (at most 256 pixels
    // a side). An empty function removes it.
    void setPicker(std::function<void(int state, const QRectF& area, const QImage& region)> fn);
    QPointF canonicalToView(double x, double y) const;

    // The tool bar's Video Preview on External Monitor button, and the right-click menu's Video Preview Preferences.
    std::function<void()> onExternalMonitor;
    std::function<void()> onPreferences;
    // Overlays menu channel views: 0 whole image; 1, 2, 3 red, green, blue alone; 4, 5, 6 the same as grey; 7 alpha as
    // grey (not remembered, as in VEGAS).
    void setChannel(int channel);
    // Overlays > Closed Captioning: caption channel shown over the frame (1..4: CC1..CC4; 0 none); the 360 mode of a 360
    // project and where its camera looks;
    // transport buttons shown under the frame ("record", "loop", "playFromStart", "play", "pause", "stop", "goToStart",
    // "goToEnd", "previousFrame", "nextFrame": Edit Visible Button Set)
    void setCaptionChannel(int channel);
    int captionChannel() const;
    void set360Mode(bool on);
    bool mode360() const;
    void set360View(double yaw, double pitch, double fov);
    QStringList visibleTransportButtons() const;
    void setVisibleTransportButtons(const QStringList& set);
    int channel() const;
    // Split-Screen View: part of the image (fractions from the top left; drag to choose) shows the frame without effects
    // (FX Bypassed) or the clipboard image. Area and source are remembered.
    void setSplitScreen(bool on, bool clipboard = false);
    bool splitScreen() const;
    bool splitClipboard() const;
    void setSplitArea(const QRectF& area);
    QRectF splitArea() const;
    bool splitPictureReady() const;     // for tests: the other image has arrived
    // Right-click menu: Simulate Device Aspect Ratio, Scale Video to Fit Preview Window, Adjust Size and Quality for
    // Optimal Playback, Show Toolbar, Show Status Bar (all remembered).
    void setDisplayOptions(bool simulateAspect, bool scaleToFit, bool adjustForPlayback);
    bool simulateAspect() const;
    bool scaleToFit() const;
    bool adjustForPlayback() const;
    void setToolbarVisible(bool on);
    bool toolbarVisible() const;
    void setStatusBarVisible(bool on);
    bool statusBarVisible() const;
    QRect pictureRect() const;          // where the image is drawn in pictureWidget()
    // Shows another image than the timeline's until the timeline's next one (the Trimmer's, with Show Video in Preview
    // Window); an empty image shows the timeline's last again.
    void showPicture(const QImage& img);
protected:
    void showEvent(QShowEvent* e) override;     // asks for a first image
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// VEGAS's Video Scopes window (View > Window > Video Scopes, Ctrl+Alt+2): the preview image as a vectorscope, waveform
// (luma, 0 to 100 percent of
// computer RGB), RGB parade or histogram (with mean and spread). While playing it keeps the frame where playing began
// unless "Update Scopes While Playing" is on.
class VideoScopes : public QWidget {
public:
    enum Scope { Vectorscope, Waveform, Parade, Histogram };
    explicit VideoScopes(Playback* playback, QWidget* parent = nullptr);
    ~VideoScopes() override;
    void setScope(Scope s);             // remembered
    Scope scope() const;
    // Waveform Composite (luma with the chroma's swing) or Luminance; the histogram chart; the Settings dialog's 7.5 IRE
    // Setup and Studio RGB (16 to 235), which change the waveform's IRE of an 8-bit level.
    void setWaveformComposite(bool on);
    bool waveformComposite() const;
    enum HistogramType { HistLuminance, HistRed, HistGreen, HistBlue, HistAlpha, HistLuminanceRGB, HistLab };
    void setHistogramType(HistogramType t);
    HistogramType histogramType() const;
    void setDisplaySettings(bool setup75, bool studioRgb);
    bool setup75() const;
    bool studioRgb() const;
    double waveformIre(int level) const;
    void histogramStats(HistogramType t, double& mean, double& stdDev) const;      // of what was measured last
    // For tests: measure this image now, and what was measured (luma in 0..255).
    void setImage(const QImage& img);
    struct Stats {
        int pixels = 0;
        double meanLuma = 0, stdDevLuma = 0;
        int minLuma = 0, maxLuma = 0;
    };
    Stats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// VEGAS's Explorer (View > Window > Explorer, Alt+1): folders on this computer and the media files in them. Drag files
// to the timeline or Project Media; a double-click puts one on the timeline at the cursor.
class MediaExplorer : public QWidget {
public:
    explicit MediaExplorer(QWidget* parent = nullptr);
    ~MediaExplorer() override;
    void setFolder(const QString& path);            // remembered
    QString folder() const;
    QStringList visibleFiles() const;               // names in the file list (for tests; it fills in as it reads)
    // Adds the files to Project Media and, with toTimeline, puts each on the timeline at the cursor.
    void addFiles(const QStringList& paths, bool toTimeline);
    // VEGAS's tool bar: Back / Forward, views ("details", "list", "thumbnail"), Favorites, Delete
    void goHistory(int dir);
    void setView(const QString& view);
    void addFavorite(const QString& folder);
    QStringList favorites() const;
    void deleteSelected();
private:
    void applyViews();
    void showDetails(const QString& file);
    struct Impl;
    std::unique_ptr<Impl> d;
};

// VEGAS's Edit Details (View > Window > Edit Details, Ctrl+Alt+4): the project's events, markers, regions or media as a
// table. A click on a row selects it and moves the cursor.
class EditDetails : public QWidget {
public:
    // (Commands: command markers, including closed captions; AudioCdTrackList: the CD layout's tracks and indexes)
    enum Show { Events, SelectedEvents, Markers, Regions, Media, Commands, AudioCdTrackList };
    explicit EditDetails(QWidget* parent = nullptr);
    ~EditDetails() override;
    void setShow(Show what);
    // Ctrl+K in the Commands list (caption timing): the selected command moves to the cursor (playback position while
    // playing) and the next is selected. False if none selected.
    bool insertSelectedCommandAtCursor();
    // The selected rows as tab-separated text (Ctrl+C), and command markers made from such text (Paste in the Commands
    // list: time, command, parameter[, comment] per line). Returns how many were made.
    QString selectedText() const;
    int pasteCommands(const QString& text);
    int rows() const;                               // for tests
    QString cell(int row, int column) const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// The transport bar under the timeline: Record, Loop Playback, Play From Start, Play, Pause, Stop, Go To Start, Go To
// End, Previous Frame, Next Frame.
class TransportBar : public QWidget {
public:
    explicit TransportBar(Playback* playback, QWidget* parent = nullptr);
    ~TransportBar() override;
    // A button moved the cursor (the timeline brings it into view).
    std::function<void()> onCursorMoved;
    // The Record button: enabled once something records (the main window's recorder); checked while recording.
    void setRecordHandler(std::function<void()> fn);
    void setRecording(bool on);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Stops playback (Space, Esc, Stop buttons): the cursor returns to where playing began, or stays if Preferences say so
// (cursorFollowsPlayback).
void stopPlayback(Playback* pb);

// VEGAS's Project Media window: bins at the left (Project Media, bins from Create New Bin, smart bins that show a saved
// search, media by type), the bin's
// media at the right as detailed list, list or thumbnails, a search box (names, tags, comments, types), Import Media,
// Remove All Unused Media and Properties.
// The right-click menu has Open in Trimmer, Add to Timeline, Select Timeline Events, Explore Containing Folder, Create
// Subclip, Remove from Current Bin,
// Remove from Project, Rename, Replace, Refresh and Properties.
class MediaBin : public QWidget {
public:
    explicit MediaBin(QWidget* parent = nullptr);
    ~MediaBin() override;
    void importFiles();                     // file dialog
    void importImageSequence();             // File > Import > Image Sequence: a frame, then the frame rate
    // (also for tests)
    void setView(int view);                 // 0 detailed, 1 list, 2 thumbnails (remembered)
    int view() const;
    void showBin(const QString& key);       // "all", "bin:<path>", "smart:<name>", "type:video|audio|image|generated"
    QString currentBin() const;
    void setSearch(const QString& text);
    std::vector<uint64_t> shownMedia() const;
    int removeUnusedMedia();                // returns how many were removed
    void newBin();                          // inside the bin shown (if it is one); then shows it
    void moveToBin(const QString& key, const std::vector<uint64_t>& ids);   // key "all": out of any bin
    void saveSmartBin(const QString& name); // the search box's words as a smart bin
    void deleteCurrentBin();
    bool replaceMedia(uint64_t id, const QString& file);
    void refreshMedia(const std::vector<uint64_t>& ids);
    void selectTimelineEvents(const std::vector<uint64_t>& ids);
    void selectMedia(const std::vector<uint64_t>& ids);     // event menu > Select in Project Media List: these items selected (and shown)
    // VEGAS's Add At Project Frame Rate, and Swap Media (a folder of files with the same names)
    void addAtProjectFrameRate(const std::vector<uint64_t>& ids);
    int swapMedia(const QString& folder);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// The Trimmer's selection for Create Subclip (set by the main window): the media and seconds into the file. False when
// nothing is selected.
extern std::function<bool(uint64_t& mediaId, double& in, double& out)> trimmerSelection;
// Create Subclip: a new media item for 'in' .. 'out' seconds of the media (of the part, for a subclip), named so (empty:
// "<name> Subclip"). Returns its id, 0 if it cannot be made (stills, generated media, empty range).
uint64_t createSubclip(uint64_t mediaId, double in, double out, const std::string& name);

// A list of installed plug-ins of one kind, with a search box. Items can be dragged onto the timeline or double-clicked (onActivated).
class FxBrowser : public QWidget {
public:
    enum Kind { VideoFx, AudioFx, Generators, Transitions };
    explicit FxBrowser(Kind kind, QWidget* parent = nullptr);
    ~FxBrowser() override;
    // Double-click: for video/audio effects the argument is the same text a drag carries (kMime... formats).
    std::function<void(Kind kind, const std::string& data)> onActivated;
    // VEGAS 18's favourites and categories ("all", "favorites", "thirdparty", "group:<name>"); also for tests
    void setFavorite(const std::string& pluginId, bool on);
    void setCategory(const QString& key);
    int listedPlugins() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Draws with OpenGL into an image of 'pixels' (the offscreen context for the preview's overlays): the whole image is the
// viewport, cleared to 'background', with the projection mapping left..right and bottom..top onto it; then draw() runs
// (an effect drawing its control). Null if there is no OpenGL.
QImage drawWithGL(const QSize& pixels, const QColor& background, double left, double right, double bottom, double top,
                  const std::function<void()>& draw);

// Dialogs. Each returns when closed.
void runRenderDialog(QWidget* parent);                  // "Render As": pick a file and format, render with progress
void runProjectProperties(QWidget* parent);             // size, frame rate, sample rate (short form)
void runPluginScan(QWidget* parent, bool full);         // looks for plug-ins, with progress (full: retry all)
// Shows a background plug-in scan in the status bar (after the splash screen's Skip, or Run in Background in Options >
// Look for New Plug-Ins) and tells the lists when done. 'owner' keeps the timer.
void watchBackgroundScan(QWidget* owner);
void showAbout(QWidget* parent);

}  // namespace app
}  // namespace reno
