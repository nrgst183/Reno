// Automation controls: the Automation Settings button of a track, bus or mixer strip with its modes (Off, Read, Write (Touch),
// Write (Latch)) and what the volume, pan, level and mute controls do in each. Data work is in engine/automation.*.
#pragma once
#include "appstate.h"
#include "../engine/automation.h"
#include <functional>
#include <string>
#include <vector>

namespace reno {
namespace app {

// What the controls ask the main window about playback (unset: nothing playing).
struct AutomationHooks {
    static std::function<bool()> playing;
    static std::function<double()> position;       // playback position (seconds)
    static std::function<void()> wake;             // a take began: the main window starts calling automationTick
};
// The time a control shows and writes at: playback position while playing, else the cursor.
double automationNow();

// The owner's settings (track or bus; others have none).
bool automationControlsOn(const Project& p, const EnvelopeOwner& o);
std::string automationModeOf(const Project& p, const EnvelopeOwner& o);
// Automation Settings button: on or off for these owners (one undo step); and their mode.
void setAutomationControls(AppState& app, const std::vector<EnvelopeOwner>& owners, bool on);
void setAutomationMode(AppState& app, const std::vector<EnvelopeOwner>& owners, const std::string& mode);
// VEGAS's names: "Off", "Read", "Automation Write (Touch)", "Automation Write (Latch)".
const char* automationModeName(const std::string& mode);

enum class ControlUse {
    Trim,       // control sets the track's (or bus's) own level, pan or mute
    AutoValue,  // mode Off, or Read with no envelope: sets the track's automation value (Track::autoLevel...)
    Follow,     // Read with an envelope: control shows it, cannot be adjusted
    Write,      // Touch or Latch: control edits the envelope at the cursor
};
ControlUse controlUse(const Project& p, const EnvelopeOwner& o, const std::string& kind, uint64_t target = 0);

// The value a control shows: the envelope's at time t in automation mode if there is one, else 'trim'.
double controlShown(const Project& p, const EnvelopeOwner& o, const std::string& kind, uint64_t target, double trim, double t);

// A control was moved to 'value' (envelope units). Write mode puts it in the envelope and returns true; Follow returns true and does
// nothing; Trim returns false (caller sets the trim). Called inside a drag update.
bool controlMoved(Project& p, const EnvelopeOwner& o, const std::string& kind, uint64_t target, double value);
// Control released: Touch stops writing (the old envelope returns), Latch continues until playback stops.
void controlReleased(AppState& app);
// Polled by a timer: Latch writes the last value during playback and finishes when it stops.
bool automationTick(AppState& app);      // false: no take is going, the caller can stop calling

}  // namespace app
}  // namespace reno
