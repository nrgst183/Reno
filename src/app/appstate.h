// Shared application state: the project being edited, undo, selection, cursor and live plug-in instances.
// Main (interface) thread only, except snapshot() and revision().
#pragma once
#include "../engine/engine.h"
#include "../engine/catalog.h"
#include <map>
#include <set>
#include <mutex>
#include <atomic>

class QTimer;

namespace reno {
namespace app {

// What changed, for listeners (or-ed together).
enum Change {
    kProject = 1,       // tracks, events, media, effect chains
    kSelection = 2,
    kCursor = 4,
    kPlayback = 8,      // started, stopped
    kCatalog = 16,      // the list of installed plug-ins
    kFxParams = 32,     // a setting of some effect (redraw only, no project change)
    kView = 64,         // zoom, scroll
    kFile = 128,        // the project's file name or saved/unsaved state
    kPrefs = 256,       // Options > Preferences changed (AppState::prefs)
    // media frames changed without a project change (e.g. a nested project finished rendering)
    kMediaPictures = 512
};

// Options > Preferences, kept in QSettings under "prefs/".
struct Preferences {
    std::string previewQuality = "preview";     // draft, preview, good, best
    int undoLevels = 200;                       // steps Undo remembers
    int autoSaveMinutes = 5;                    // auto-save interval, 0 = never
    double stillLength = 5.0;                   // seconds a still lasts on the timeline
    double generatedLength = 10.0;              // same for generated media
    bool cursorFollowsPlayback = false;         // on stop the cursor stays (else returns to where it started)
    std::string scrollMode = "page";            // page, smooth, none
    bool snapping = true;                       // Options > Enable Snapping
    bool snapToGrid = false;                    // also snap to the ruler's marks
    std::string ofxHostName = "com.vegascreativesoftware.vegas";    // host name reported to OpenFX effects
    bool vegasExtensions = true;                // VEGAS suites and properties (ofx::HostConfig)
    bool stubUnknownSuites = false;             // stand-ins for VEGAS suites Reno does not provide
    std::string audioDevice;                    // empty = system default
    // fold 5.1 down to stereo even if the sound card has six channels (Downmix Output)
    bool downmixOutput = false;
    // How multichannel files are put on the timeline: "tracks" (one mono event per channel, grouped), "pairs" (stereo pairs),
    // "mixdown" (one stereo event).
    std::string multichannelImport = "tracks";
    bool panMultichannelTracks = true;          // pan new tracks to their speaker
    std::string audioEditor;                    // program for Open in Audio Editor (Ctrl+E)
    double cutToOverlap = 1.0;                  // seconds: overlap made by Convert Cut to Transition
    std::string audioInputDevice;               // recording input (Qt Multimedia id); empty = system default
    std::string recordFolder;                   // empty = "Recorded" beside the project
    std::string prerenderFolder;                // empty = user data folder
    // Video preview: safe areas (percent of the frame) and grid divisions.
    double actionSafe = 10.0, titleSafe = 20.0;
    int gridColumns = 4, gridRows = 4;
    // Event thumbnails. videoThumbnails: "all", "headCenterTail", "headTail", "head", "none".
    // thumbnailNumbers: "none", "frames", "time", "timecode".
    std::string videoThumbnails = "all";
    std::string thumbnailNumbers = "none";
    // Preferences > Editing
    bool loopEvents = true;                     // loop new events past the media's end
    bool preservePitch = true;                  // keep pitch when stretching new audio events
    double cursorPreview = 1.0;                 // seconds Preview Cursor Position (Num 0) plays
    // Quickfade Audio Edits. Off by default: VEGAS renders show no fade at event edges.
    bool quickFadeEdits = false;
    double quickFadeMs = 10.0;
    // Project Properties' "Start all new projects with these settings" (Project JSON)
    std::string newProjectSettings;
    // Audio proxies (.sfap0) for nested projects, and VEGAS peak files (.sfk) beside the media.
    bool createAudioProxies = true;
    bool vegasPeakFiles = true;
    // Preferences > Video: render threads for playback (0 = automatic, 2) and Dynamic RAM Preview limit in MB.
    int renderThreads = 0;
    int ramPreviewMb = 1536;
    // Preferences > Editing:
    // time-selection envelope fades; align is "outside", "centered" or "inside"
    bool timeSelEnvFades = true;
    double timeSelEnvFadeLength = 0.1;
    std::string timeSelEnvFadeAlign = "outside";
    // overlap multiple selected media when added (by cutToOverlap with Automatic Crossfades)
    bool autoOverlapMedia = false;
    // no frame quantizing for audio-only edits
    bool noQuantizeAudioOnly = false;
    // smooth and thin automation data after recording or drawing
    bool thinAutomation = true;
    // Right mouse button switches to the Selection tool (VEGAS 22+). On: VEGAS 18 behaviour.
    bool rightMouseSelect = true;
    // Auto-save backups kept (1 = one file; more: <project>.reno.bak.1, .2...)
    int autoSaveKeep = 1;

    void load();            // missing values keep defaults
    void save() const;
};

// Something that has an effect chain.
struct FxTarget {
    enum Kind { None, Event, Track, VideoOut, AudioOut, Bus,
                Transition,     // the transition where event 'id' overlaps the one before it
                Generator,      // generator of media item 'id'
                InputBus };     // input bus 'id'
    Kind kind = None;
    uint64_t id = 0;        // event, track, bus or media id
    bool operator==(const FxTarget& o) const { return kind == o.kind && id == o.id; }
};

// Where an envelope lives: a track, an event, a bus, the master bus or the video output.
struct EnvelopeOwner {
    enum Kind { None, Track, Event, Bus, Master, VideoOut };
    Kind kind = None;
    uint64_t id = 0;        // track, event or bus id
    bool operator==(const EnvelopeOwner& o) const { return kind == o.kind && id == o.id; }
    bool operator!=(const EnvelopeOwner& o) const { return !(*this == o); }
};

// One envelope: owner, kind, and for "send" the bus, for "fx" the slot and setting name.
struct EnvelopeRef {
    EnvelopeOwner owner;
    std::string kind;
    uint64_t target = 0;
    std::string param;
    bool valid() const { return owner.kind != EnvelopeOwner::None && !kind.empty(); }
    bool operator==(const EnvelopeRef& o) const { return owner == o.owner && kind == o.kind && target == o.target && param == o.param; }
    bool operator!=(const EnvelopeRef& o) const { return !(*this == o); }
};

// An envelope that can be added (Insert/Remove Envelope menus).
struct EnvelopeChoice {
    EnvelopeRef ref;
    std::string name;       // "Volume", "Pan", "Bus A", "Composite Level", "Reverb: Mix"...
};

class AppState {
public:
    static AppState& get();

    // ---- envelopes (each command is one undo step)
    static Envelopes* envelopesOf(Project& p, const EnvelopeOwner& o);
    static const Envelopes* envelopesOf(const Project& p, const EnvelopeOwner& o);
    static Envelope* findEnvelope(Project& p, const EnvelopeRef& r);
    static const Envelope* findEnvelope(const Project& p, const EnvelopeRef& r);
    // Starting value of a new envelope of this kind.
    static double envelopeDefault(const std::string& kind);
    // Curve of a new envelope point (VEGAS 18 defaults: smooth for audio/video, linear for fx, mute holds).
    static int envelopeDefaultCurve(const std::string& kind);
    // Envelopes 'o' can have, in VEGAS's order.
    std::vector<EnvelopeChoice> envelopeChoices(const EnvelopeOwner& o) const;
    // Adds an envelope (or shows it if hidden). False if it was already showing.
    bool insertEnvelope(const EnvelopeRef& r);
    void removeEnvelope(const EnvelopeRef& r);
    void showEnvelope(const EnvelopeRef& r, bool visible);
    // V and P: insert, show or hide the envelope.
    void toggleEnvelope(const EnvelopeRef& r);

    // ---- the project
    const Project& project() const { return project_; }
    std::shared_ptr<const Project> snapshot() const;        // any thread
    uint64_t revision() const { return revision_.load(); }  // any thread; changes with every project change

    // One undoable change: fn alters the project; 'what' names it in the Edit menu.
    void edit(const std::string& what, const std::function<void(Project&)>& fn);
    // edit() that also changes effect settings (FxSlot::state of 'fxSlots'); running plug-ins follow, undo/redo restore them.
    void editFx(const std::string& what, const std::function<void(Project&)>& fn, const std::vector<uint64_t>& fxSlots);
    // A drag: each dragUpdate restarts from the project at beginDrag; endDrag is one undo step.
    void beginDrag();
    void dragUpdate(const std::function<void(Project&)>& fn);
    void endDrag(const std::string& what);
    void cancelDrag();
    bool dragging() const { return dragging_; }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoName() const { return undo_.empty() ? std::string() : undo_.back().what; }
    std::string redoName() const { return redo_.empty() ? std::string() : redo_.back().what; }
    size_t undoCount() const { return undo_.size(); }        // for tests
    size_t redoCount() const { return redo_.size(); }
    // Names of the next 'most' Undo (Redo) steps, next first (history drop-downs).
    std::vector<std::string> undoNames(size_t most) const
    {
        std::vector<std::string> v;
        for (size_t k = undo_.size(); k-- > 0 && v.size() < most;) v.push_back(undo_[k].what);
        return v;
    }
    std::vector<std::string> redoNames(size_t most) const
    {
        std::vector<std::string> v;
        for (size_t k = redo_.size(); k-- > 0 && v.size() < most;) v.push_back(redo_[k].what);
        return v;
    }
    void undo();
    void redo();

    // ---- effect settings as undo steps
    // Settings changed in an effect's own controls are held by the live plug-in. The effects window brackets each
    // change with these; if the settings differ at the end it is one undo step.
    void beginFxEdit(uint64_t slotId);
    void endFxEdit(uint64_t slotId, const std::string& what);     // no-op if no edit was begun
    bool fxEditOpen(uint64_t slotId) const { return fxEdits_.count(slotId) != 0; }
    // One step for a change made in one go: begins, runs 'change', ends. Inside an open edit it just runs it.
    void fxEditStep(uint64_t slotId, const std::string& what, const std::function<void()>& change);
    // What the live plug-in would save now (null if none).
    Json fxLiveState(uint64_t slotId) const;

    // A new, empty project. 'vegasDefaults': VEGAS Pro 18's new-project settings (1920x1080, 29.970 fps...) unless
    // "Start all new projects with these settings" gives others. Without it, plain Project defaults (30 fps).
    void newProject(bool vegasDefaults = false);
    // Opens a .veg or .reno file (told apart by content). What Reno cannot use of a .veg is in openNotes().
    bool open(const std::string& path, std::string* error);
    // Saves as .veg if the path ends in .veg, else as Reno's own file. What a .veg could not hold is in saveWarnings().
    bool save(const std::string& path, std::string* error);
    const std::vector<std::string>& openNotes() const { return openNotes_; }
    const std::vector<std::string>& saveWarnings() const { return saveWarnings_; }
    const std::string& path() const { return path_; }
    // Goes up each time another project replaces this one (File > New, Open, Close): caches of the last one's frames
    // and pictures are let go then.
    uint64_t projectSerial() const { return projectSerial_; }
    bool dirty() const { return dirty_; }
    // Keeps the project but ties it to no file (restored from auto-save: Save asks for a name).
    void untieFromFile();

    // ---- media
    // Adds a file to the media (or finds it). 0 with *error if it cannot be read.
    uint64_t importMedia(const std::string& path, std::string* error);
    // Numbered frames from 'firstFile' as one video at 'fps' (Import > Image Sequence). 0 with *error if none.
    uint64_t importImageSequence(const std::string& firstFile, double fps, std::string* error);
    // Adds media made by an OpenFX generator.
    uint64_t addGeneratedMedia(const std::string& pluginId, const std::string& label);
    // Puts media on the timeline at 'time': frame on a video track, sound on an audio track (under 'trackHint' if
    // it suits, else the first free ones, else new ones). One undo step. Returns the new event ids.
    std::vector<uint64_t> addMediaToTimeline(uint64_t mediaId, double time, uint64_t trackHint = 0);
    uint64_t addTrack(TrackType type, int index = -1);
    // Called by addTrack for each new track, inside its edit (Set Default Track Properties).
    std::function<void(Project&, Track&)> onNewTrack;
    void removeTrack(uint64_t trackId);
    // How media files are opened (tests substitute made-up media).
    MediaPool::Opener mediaOpener;
    // Opens Project Media's Properties for a media (set by the Project Media panel; empty without one).
    std::function<void(uint64_t mediaId)> showMediaProperties;

    // ---- effects
    FxChain* chainOf(Project& p, const FxTarget& t) const;
    const FxChain* chainOf(const Project& p, const FxTarget& t) const;
    std::string targetName(const FxTarget& t) const;        // "Event: clip.mp4", "Track 2", "Video Output"
    bool targetIsVideo(const FxTarget& t) const;
    uint64_t addVideoFx(const FxTarget& t, const std::string& pluginId, const std::string& label);
    uint64_t addAudioFx(const FxTarget& t, const AudioPluginDesc& desc);
    void removeFx(const FxTarget& t, uint64_t slotId);
    void moveFx(const FxTarget& t, uint64_t slotId, int delta);
    void setFxBypass(const FxTarget& t, uint64_t slotId, bool bypass);
    void setTransition(uint64_t eventId, const std::string& pluginId, const std::string& label);   // empty id = plain cross-fade
    FxRuntime fx;

    // ---- selection, cursor, loop region
    const std::set<uint64_t>& selection() const { return selection_; }
    void setSelection(const std::set<uint64_t>& ids);
    void selectOnly(uint64_t eventId);
    uint64_t currentTrack = 0;
    double cursor() const { return cursor_; }
    void setCursor(double seconds);                         // clamped to >= 0
    double loopStart = 0, loopEnd = 0;                      // loopEnd <= loopStart: no region
    bool loopOn = false;
    void setLoop(double a, double b);
    bool snapping = true;
    bool bypassAllFx = false;
    // Options > Mute All Audio / Mute All Video: skipped in playback and display, not in renders or the project.
    bool muteAllAudio = false;
    bool muteAllVideo = false;
    // Options > Bypass All Audio FX: playback skips every audio effect. Read by the audio thread.
    //
    std::atomic<bool> bypassAudioFx{false};
    // Audio tracks armed for recording. Not part of the project, not an undo step; notify kSelection after changing.
    std::set<uint64_t> armedTracks;
    // Part of a media the next addMediaToTimeline() uses (Trimmer selection, media seconds in..out); used once.
    // mediaId 0: none. 'streams': 0 both, 1 frame only, 2 sound only.
    struct MediaRange { uint64_t mediaId = 0; double in = 0, out = 0; int streams = 0; };
    MediaRange nextAddRange;

    // ---- editing commands that work on the selection
    void splitAtCursor();
    // Detect Scenes: timeline times where the picture of a video event cuts (sensitivity 0..100), and a split at those times.
    std::vector<double> detectScenes(uint64_t eventId, int sensitivity = 50);
    int splitAtScenes(int sensitivity = 50);
    void deleteSelection();
    void copySelection();
    void cutSelection();
    void pasteAtCursor();
    void groupSelection(bool group);
    bool hasClipboard() const { return !clipboard_.empty(); }
    // Edit > Switches > Lock: locked events cannot be moved, trimmed, split or deleted.
    void setLocked(const std::set<uint64_t>& ids, bool locked);
    // Ids among 'ids' that are locked.
    std::set<uint64_t> lockedAmong(const std::set<uint64_t>& ids) const;

    // ---- takes (Edit > Take)
    static std::vector<Take> takesOf(const Event& e);           // active one at e.activeTake
    static void setActiveTake(Event& e, int index);             // out of range does nothing
    // T / Shift+T: next (+1) or previous (-1) take of the selected events and their groups. One undo step; false if none has another.
    bool stepTakes(const std::set<uint64_t>& ids, int dir);
    // Makes take 'index' (0-based) active. False if no event has it.
    bool chooseTake(const std::set<uint64_t>& ids, int index, const std::string& what = "Choose Take");
    // Adds media as more takes (each from the media's start; active take stays). Only media of the track's stream kind.
    // Returns the count.
    int addTakes(uint64_t eventId, const std::vector<uint64_t>& media);
    bool deleteActiveTake(uint64_t eventId);                   // next take becomes active; not the only take
    bool deleteTakes(uint64_t eventId, const std::set<int>& indices);   // by index; at least one is kept
    void renameActiveTake(uint64_t eventId, const std::string& name);

    // ---- Edit menu commands (each one undo step)
    void undoAll();                 // Edit > Undo All (each step can be redone)
    // Merges the last 'count' undo steps into one called 'what' (commands made of several edits).
    void mergeUndo(size_t count, const std::string& what);
    // Drops undo steps above the first 'count' without leaving them to Redo, and sets the changed mark to 'dirty'
    // (for commands that were cancelled, e.g. Apply Non-Real-Time Event FX).
    void discardUndoTo(size_t count, bool dirty);
    void clearEditHistory();        // Edit > Clear Edit History
    // What Paste Event Attributes copies (Selectively Paste ticks them).
    enum EventAttr {
        AttrFx = 1,             // effects (Event FX)
        AttrPanCrop = 2,        // Event Pan/Crop (video)
        AttrEnvelopes = 4,      // envelopes
        AttrFadeIn = 8,         // fade-in length and curve
        AttrFadeOut = 16,       // fade-out length and curve
        AttrLevel = 32,         // opacity (video) or volume (audio)
        AttrRate = 64,          // playback rate, reverse
        AttrSwitches = 128,     // mute, loop, maintain aspect ratio, reduce flicker, invert phase, normalize
        AttrPitch = 256,        // audio: pitch, stretch settings, channels
        AttrAll = 511
    };
    const Event* clipboardEvent() const;    // first event copied (null if none)
    // Paste Event Attributes (all) / Selectively Paste: clipboard event's attributes onto these events (what suits
    // each). False if nothing pasted.
    bool pasteEventAttributes(const std::set<uint64_t>& ids, int attrs);
    // Event Heal: rejoins split events that still follow on in their media (selected, with neighbours and groups). Returns joins.
    int healEvents(const std::set<uint64_t>& ids);
    // Close Gaps: moves each selected event (and its group) back to where the previous one ends. Returns count moved.
    int closeGaps(const std::set<uint64_t>& ids);
    // Insert > Time: 'length' seconds of nothing at 'at'; later events, points, markers and regions move (an event
    // across it is split).
    void insertTime(double at, double length);
    // Insert > Empty Event over [start, start + length). Returns its id (0: none).
    uint64_t insertEmptyEvent(uint64_t trackId, double start, double length, bool adjustment = false);   // adjustment: Insert > Adjustment Event
    // Clean Project Media: removes media no event or take uses. Returns count.
    int cleanProjectMedia();
    // Duplicate Track: copy with events, effects, envelopes, put under it. Returns the copy's id.
    uint64_t duplicateTrack(uint64_t trackId);
    // Select Events to End: events on tracks 'ids' from the earliest onwards. Select All After Cursor: events ending
    // after 'time' (on 'onlyTrack' or all). Caller selects.
    std::set<uint64_t> eventsToEnd(const std::set<uint64_t>& ids) const;
    std::set<uint64_t> eventsAfter(double time, uint64_t onlyTrack = 0) const;
    // Switches > Normalize: on, measures each audio event's peak between its start and end and sets the gain to the
    // normalize level; off, back to its own level.
    void setNormalize(const std::set<uint64_t>& ids, bool on);
    // Auto Normalize: Normalize re-worked on every trim, split or stretch (same undo step). Off leaves Normalize on.
    void setAutoNormalize(const std::set<uint64_t>& ids, bool on);
    static double normalizeLevel();         // -0.1 dB as a gain (VEGAS default)
    // Loudest sample of an audio event's media between its start and end (0 if unreadable).
    double eventPeak(const Event& e) const;

    // Automatic Crossfades (Ctrl+Shift+X). Not an undo step, not saved with the project (Project::autoCrossfades).
    bool autoCrossfades() const { return autoCrossfades_; }
    void setAutoCrossfades(bool on);

    // ---- listeners
    int addListener(std::function<void(int changes)> fn);
    void removeListener(int id);
    void notify(int changes);
    // Runs fn soon on the main thread (any thread may call).
    static void post(std::function<void()> fn);

    // ---- preferences
    Preferences prefs;
    // Reads preferences (QSettings) and applies them. Done on first use if the application has a name by then; otherwise
    // call after QApplication is set up.
    void loadPreferences();
    // Applies 'prefs' (undo limit, auto-save, OpenFX host switches), saves them, notifies kPrefs.
    void applyPreferences();
    // Auto-save copy: <project>.reno.bak beside the project, or in the user data folder if untitled.
    std::string autoSavePath() const;
    // Writes the auto-save copy now if anything is unsaved. False with *error on failure.
    bool autoSaveNow(std::string* error = nullptr);
    // Timer version for big projects, written on a thread. False: not handled; use autoSaveNow.
    bool autoSaveInBackground();

    // Status bar messages.
    std::function<void(const std::string&)> onStatus;
    void status(const std::string& s) { if (onStatus) onStatus(s); }

private:
    AppState();
    struct Step {
        std::string what;
        Project before;
        std::vector<uint64_t> fxSlots;      // effects restored from the project on undo/redo
    };
    void commit(const std::string& what, Project before);
    void refreshAutoNormalize(const Project& before);
    void trimUndo();
    void restoreFxStates(const std::vector<uint64_t>& ids);
    std::map<uint64_t, Json> fxEdits_;      // edits begun: state before
    QTimer* autoSaveTimer_ = nullptr;
    void afterChange(int extra = 0);
    Project project_;
    mutable std::mutex snapMutex_;
    std::shared_ptr<const Project> snapshot_;
    std::atomic<uint64_t> revision_{1};
    std::vector<Step> undo_, redo_;
    bool autoCrossfades_ = true;
    bool dragging_ = false;
    bool dragUpdated_ = false;
    static bool plainlyDifferent(const Project& a, const Project& b);
    Project dragBase_;
    std::string path_;
    std::vector<std::string> openNotes_, saveWarnings_;
    bool dirty_ = false;
    std::set<uint64_t> selection_;
    double cursor_ = 0;
    struct ClipItem { Event event; TrackType type; int trackOffset; };
    std::vector<ClipItem> clipboard_;
    double clipboardStart_ = 0;
    std::vector<std::pair<int, std::function<void(int)>>> listeners_;
    int nextListener_ = 1;
    int notifyDepth_ = 0;
    int pending_ = 0;
    uint64_t projectSerial_ = 0;
};

// An event's start moved by 'delta': its envelope points move back by as much so they stay put on the timeline.
void shiftEventEnvelopes(Event& e, double delta);
// Same for Event Pan/Crop keyframes and mask keyframes when a split makes the event start later.
void shiftEventKeyframes(Event& e, double delta);

// "00:00:12;15"-style time (hours:minutes:seconds;frames).
std::string timecode(double seconds, double fps);

}  // namespace app
}  // namespace reno
