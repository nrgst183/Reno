// VEGAS's time formats (Options > Ruler Format, the time display's and the ruler's Time Format menus): how a time is
// written on the ruler, in the time display and the readouts under the timeline, and how a typed time is read back.
#pragma once
#include "project.h"
#include <string>
#include <vector>

namespace reno {

struct TimeFormatInfo {
    const char* id;             // ProjectSettings::rulerFormat
    const char* menuText;       // VEGAS's menu item (with its & shortcut letter)
    int scriptValue;            // ScriptPortal.Vegas.RulerFormat
};

// VEGAS's formats in its menus' order.
const std::vector<TimeFormatInfo>& timeFormats();
// The format's entry (Time & Frames for an id that is not known).
const TimeFormatInfo& timeFormatInfo(const std::string& id);
// The id of ScriptPortal's RulerFormat value (Time & Frames for values Reno has no format for).
std::string timeFormatFromScript(int value);

// What the time display writes over the time: "SMPTE Drop (29.970 fps)", "Feet and Frames 35mm (16 fpf)", ...
std::string timeFormatTitle(const ProjectSettings& s, const std::string& format);

// 'shown' (the time as the ruler counts it: seconds from the project's start plus the ruler's start time) written in
// 'format'.
std::string formatTime(const ProjectSettings& s, double shown, const std::string& format);
// A time typed in 'format' (or in any of the usual forms: hh:mm:ss;ff, hh:mm:ss.sss, a number in the format's unit,
// feet+frames, measure.beat.tick) read back as the ruler counts it.
bool parseTimeText(const ProjectSettings& s, const std::string& text, const std::string& format, double* shown);

// The ruler's numbered marks: how many of the project's frames apart they are, so that a number has 'minPx' pixels at
// 'pps' pixels a second.
int64_t rulerStepFrames(double pps, double fps, double minPx);

// How many digits the frame part of hh:mm:ss:ff has for a counter going to 'nominal' frames a second: those of its last
// frame (nominal - 1), at least two - 2 up to 100 fps, 3 at 120 or 240 fps.
int frameDigits(int nominal);

// Frame counts and SMPTE drop-frame timecode (30000/1001 fps, frames ;00 and ;01 skipped every minute but each
// tenth): frame number -> the counter's frame number with the skipped ones counted, and back.
long long dropFrameDisplay(long long frame);
long long dropFrameFromDisplay(long long hours, long long minutes, long long seconds, long long frames);

}  // namespace reno
