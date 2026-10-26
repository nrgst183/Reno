// Audio commands that have windows of their own: File > Real-Time Render, Tools > Audio > Apply Non-Real-Time Event FX,
// Remove Assignable FX, Auto Ducking, Beat Detection and Tempo Detection.
// The analysis is in engine/audioanalysis.* and engine/automation.*; this is the dialogs and glue.
#pragma once
#include "appstate.h"
#include <functional>
#include <set>
#include <string>
#include <vector>

class QWidget;
class QMenu;

namespace reno {
namespace app {

// File > Real-Time Render: asks for a .wav file and renders the project's sound into it. (VEGAS plays the project to record
// input buses; Reno has none, so this is a normal render.) False if nothing was written.
bool runRealTimeRender(QWidget* parent);
// The same without asking (tests). False with *error on failure.
bool realTimeRenderTo(const std::string& path, std::string* error);

// Tools > Audio > Apply Non-Real-Time Event FX (also the event's right-click menu): for the selected audio events (active takes)
// shows the Plug-In Chooser, the Take FX window, then the Apply Non Real-Time FX window (name, folder, format, template) and a
// progress window. Each event's sound goes through the effects into a new file "<media name> Take <n>" (n from 2, never over
// an existing file, beside that take's media), which is added to Project Media and becomes the event's active take. The
// effects are not kept on the event; its fades, volume and envelopes stay. One undo step; cancel or failure leaves the
// project unchanged.
struct NrtHooks {
    // Preview: (true, seconds) plays from the event's start through the effects, (false, 0) stops.
    std::function<void(bool play, double from)> preview;
};
void runNonRealTimeEventFx(QWidget* parent, const NrtHooks& hooks = NrtHooks());

// Format and template of the new files: a Render As format key ("wav", "w64", "aiff", "flac") and a template name
// (empty: project rate, 24 bit, stereo).
struct NrtFormat {
    std::string format = "wav";
    std::string templateName;
};
// Formats offered (key, VEGAS's name) and a format's templates (default first).
std::vector<std::pair<std::string, std::string>> nrtFormats();
std::vector<std::string> nrtTemplates(const std::string& format);
// Default folder for new files: the event's media folder, else the project's.
std::string nrtDefaultFolder(const Project& p, uint64_t eventId);
// Next file for a new take: "<media name> Take <n>.<ext>" in 'folder', n the first number with no file, media item or take of that
// name and not in 'reserved'.
struct NrtFile { std::string path, takeName; };
NrtFile nextNrtFile(const Project& p, uint64_t eventId, const std::string& folder, const std::string& ext, const std::set<std::string>& reserved = {});

struct NrtOptions {
    NrtFormat format;
    std::string folder;             // empty: nrtDefaultFolder
    std::string fileName;           // one event only: file name without extension
    std::string exactPath;          // one event only: exact file (overwritten if present)
    QWidget* parent = nullptr;      // progress window with Cancel
    std::function<bool(double)> progress;       // 0..1 over all events; false stops
};
// The rendering step (also used by tests): each event's sound is run through 'chain' (live project slots) into a new file that
// becomes the active take; 'chain' is removed from the events. One undo step. On failure returns false with *error ("stopped"
// if cancelled), leaves the project unchanged and deletes the files made.
bool applyNonRealTimeFxToEvents(const std::vector<uint64_t>& eventIds, const FxChain& chain, const NrtOptions& opt, std::string* error);
// The same for one event into 'path' (format by extension).
bool applyNonRealTimeFx(uint64_t eventId, const FxChain& chain, const std::string& path, std::string* error);

// Tools > Audio > Remove Assignable FX: submenu of assignable FX chains; choosing one deletes it and its sends. One undo step.
void fillRemoveAssignableMenu(QMenu* m);

// ---- Auto Ducking
// Analyses the controller track's sound from 'from' to 'to' (whole track if to <= from) into its "duck" envelope,
// replacing that range.
// The track becomes a controller. False with *error.
bool analyzeAutoDucking(uint64_t controllerTrack, double from, double to, std::string* error);
void showAutoDuckingDialog(QWidget* parent, uint64_t trackId);
void setAutoDuckingListener(uint64_t trackId, bool on);
void removeAutoDuckingController(uint64_t trackId);
// Re-Analyze Auto Ducking for one event of a controller track.
bool reanalyzeAutoDucking(uint64_t eventId, std::string* error);

// ---- Beat and Tempo Detection
// Mono sound of a media file from 'from' to 'to' seconds (0, 0: all, up to 'maxSeconds') at 'rate'.
bool readMonoSound(const MediaItem& media, double from, double to, int rate, std::vector<float>& out, std::string* error, double maxSeconds = 1800.0);
// Media markers from a Beat Detection run (named "Beat"; earlier ones replaced).
void setBeatMarkers(uint64_t mediaId, const std::vector<double>& times);
void showBeatDetectionDialog(QWidget* parent, uint64_t mediaId);
void showTempoDetectionDialog(QWidget* parent, uint64_t mediaId);
// Applies a tempo as Tempo Detection's checkboxes say.
struct TempoChoices { bool setTempo = true, rulerMeasures = true, metronome = false, snapToGrid = true; };
void applyDetectedTempo(double bpm, const TempoChoices& c);

}  // namespace app
}  // namespace reno
