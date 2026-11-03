// What the time display over the track headers shows: the cursor time, or MIDI timecode / clock
// (Options > Timecode; midisync.h). Header-only.
#pragma once
#include <functional>
#include <string>

namespace reno {
namespace app {

enum class TimeDisplaySource { Cursor, MtcIn, MtcOut, ClockOut };

inline TimeDisplaySource& timeDisplaySource()
{
    static TimeDisplaySource s = TimeDisplaySource::Cursor;
    return s;
}

inline std::function<std::string(TimeDisplaySource)>& midiTimeDisplayText()
{
    static std::function<std::string(TimeDisplaySource)> f;
    return f;
}

// The words over the time display in place of the time format's name when it shows MIDI ("MTC Input Not Active"); empty
// when the format's name is shown.
inline std::function<std::string(TimeDisplaySource)>& midiTimeDisplayLabel()
{
    static std::function<std::string(TimeDisplaySource)> f;
    return f;
}

}  // namespace app
}  // namespace reno
