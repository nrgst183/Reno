// Editing windows for one thing: Event Pan/Crop, Track Motion, event properties, Project Properties and Preferences.
#pragma once
#include "appstate.h"
#include <QWidget>
#include <vector>

namespace reno {
namespace app {

class Playback;

// These open a window (non-modal where VEGAS's is: Pan/Crop, Track Motion) and return.
void openPanCrop(QWidget* parent, Playback* playback, uint64_t eventId);
void openTrackMotion(QWidget* parent, Playback* playback, uint64_t trackId);
// Parent Motion of a compositing parent (Parent Track Motion): same window, moving the whole group.
void openParentMotion(QWidget* parent, Playback* playback, uint64_t trackId);
// Modal; return when closed.
void runEventProperties(QWidget* parent, uint64_t eventId);
void runProjectPropertiesFull(QWidget* parent);
// Project Properties opened at its Audio tab (Master Bus window, Mixing Console).
void runProjectPropertiesAudio(QWidget* parent);
void runPreferences(QWidget* parent);

// VEGAS's compositing modes as Track::composite stores them ("srcalpha", "add"...), with menu names, in VEGAS's order.
struct CompositeMode {
    const char* id;
    const char* label;
};
const std::vector<CompositeMode>& compositeModes();

}  // namespace app
}  // namespace reno
