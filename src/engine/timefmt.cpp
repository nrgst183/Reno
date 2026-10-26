// VEGAS's time formats: writing times and reading typed ones. See timefmt.h.
#include "timefmt.h"
#include "../core/base.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace reno {

const std::vector<TimeFormatInfo>& timeFormats()
{
    // VEGAS's order and names, from its Ruler Format menu; the script values are ScriptPortal.Vegas.RulerFormat's.
    static const std::vector<TimeFormatInfo> list = {
        {"samples", "&Samples", 1},
        {"time", "&Time", 2},
        {"seconds", "Se&conds", 3},
        {"timecode", "Time && Fra&mes", 4},
        {"frames", "Absol&ute Frames", 5},
        {"measures", "Measures && &Beats", 6},
        {"feet16", "Feet and Frames &16mm (40 fpf)", 7},
        {"feet35", "Feet and Frames &35mm (16 fpf)", 8},
        {"smpteIVTC", "SMPTE Fi&lm Sync IVTC (23.976 fps, Video)", 9},
        {"smpte24", "SMPTE &Film Sync (24 fps, Film)", 10},
        {"smpteEBU", "SMPTE &EBU (25 fps, Video)", 11},
        {"smpteNonDrop", "SMPTE &Non-Drop (29.97 fps, Video)", 12},
        {"smpteDrop", "SMPTE &Drop (29.97 fps, Video)", 13},
        {"smpte30", "SM&PTE 30 (30 fps, Audio)", 14},
        {"cdtime", "&Audio CD Time", 15},
    };
    return list;
}

const TimeFormatInfo& timeFormatInfo(const std::string& id)
{
    for (const TimeFormatInfo& f : timeFormats())
        if (id == f.id) return f;
    return timeFormats()[3];
}

std::string timeFormatFromScript(int value)
{
    for (const TimeFormatInfo& f : timeFormats())
        if (f.scriptValue == value) return f.id;
    return "timecode";
}

namespace {

// The SMPTE-like formats: the frame rate they count at, the frames a second their counter goes to, and whether frame
// numbers are dropped.
struct Counter {
    double rate = 0;
    int nominal = 0;
    bool drop = false;
};

Counter counterOf(const ProjectSettings& s, const std::string& f)
{
    Counter c;
    if (f == "timecode") {
        c.rate = s.fps() > 0 ? s.fps() : 30.0;
        c.nominal = std::max(1, (int)std::lround(c.rate));
    } else if (f == "smpteIVTC") { c.rate = 24000.0 / 1001.0; c.nominal = 24; }
    else if (f == "smpte24") { c.rate = 24; c.nominal = 24; }
    else if (f == "smpteEBU") { c.rate = 25; c.nominal = 25; }
    else if (f == "smpteNonDrop") { c.rate = 30000.0 / 1001.0; c.nominal = 30; }
    else if (f == "smpteDrop") { c.rate = 30000.0 / 1001.0; c.nominal = 30; c.drop = true; }
    else if (f == "smpte30") { c.rate = 30; c.nominal = 30; }
    else if (f == "cdtime") { c.rate = 75; c.nominal = 75; }
    return c;
}

int feetFrames(const std::string& f) { return f == "feet16" ? 40 : f == "feet35" ? 16 : 0; }

double projectFps(const ProjectSettings& s) { return s.fps() > 0 ? s.fps() : 30.0; }

// The numbers in "12:34:56;07" or "1.2.300" (empty when something else is in it).
std::vector<double> numbers(const std::string& text, const char* separators, bool* ok)
{
    std::vector<double> out;
    std::string cur;
    *ok = true;
    auto flush = [&] {
        if (cur.empty()) { *ok = false; return; }
        char* end = nullptr;
        const double v = std::strtod(cur.c_str(), &end);
        if (!end || *end != 0 || !std::isfinite(v)) *ok = false;
        out.push_back(v);
        cur.clear();
    };
    for (char ch : text) {
        if (std::strchr(separators, ch)) flush();
        else cur += ch;
    }
    flush();
    return out;
}

}  // namespace

int64_t rulerStepFrames(double pps, double fps, double minPx)
{
    if (!(fps > 0)) fps = 30.0;
    const int64_t nominal = std::max<int64_t>(1, std::llround(fps));
    std::vector<int64_t> steps;
    bool lost = false;
    for (int64_t f : {1, 2, 5, 10}) {
        if (f >= nominal) continue;
        if (nominal % f == 0) steps.push_back(f);
        else lost = true;
    }
    if (nominal % 2 == 0 && nominal / 2 > 10) steps.push_back(nominal / 2);
    if (lost) {
        steps.clear();
        for (int64_t f = 1; f <= nominal / 2; ++f)
            if (nominal % f == 0) steps.push_back(f);
    }
    for (int64_t s : {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 18000, 36000, 86400}) steps.push_back(s * nominal);
    for (int64_t s : steps)
        if ((double)s / fps * pps >= minPx) return s;
    return steps.back();
}

int frameDigits(int nominal)
{
    int digits = 1;
    for (long long n = std::max(1, nominal) - 1; n >= 10; n /= 10) ++digits;
    return std::max(2, digits);
}

long long dropFrameDisplay(long long frame)
{
    // 17982 frames in ten minutes (the first minute keeps all 1800 numbers, the nine others 1798)
    const long long d = frame / 17982, m = frame % 17982;
    return frame + 18 * d + (m > 1 ? 2 * ((m - 2) / 1798) : 0);
}

long long dropFrameFromDisplay(long long hours, long long minutes, long long seconds, long long frames)
{
    const long long totalMinutes = hours * 60 + minutes;
    return ((hours * 3600 + minutes * 60 + seconds) * 30 + frames) - 2 * (totalMinutes - totalMinutes / 10);
}

std::string timeFormatTitle(const ProjectSettings& s, const std::string& f)
{
    if (f == "samples") return strfmt("Samples (%d Hz)", s.sampleRate);
    if (f == "time") return "Time";
    if (f == "seconds") return "Time (Seconds)";
    if (f == "frames") return strfmt("Absolute Frames (%.3f fps)", projectFps(s));
    if (f == "measures") return strfmt("Measures and Beats (%g bpm)", s.tempo > 0 ? s.tempo : 120.0);
    if (f == "feet16") return "Feet and Frames 16mm (40 fpf)";
    if (f == "feet35") return "Feet and Frames 35mm (16 fpf)";
    if (f == "smpteIVTC") return "SMPTE Film Sync IVTC (23.976 fps)";
    if (f == "smpte24") return "SMPTE Film Sync (24 fps)";
    if (f == "smpteEBU") return "SMPTE EBU (25 fps)";
    if (f == "smpteNonDrop") return "SMPTE Non-Drop (29.970 fps)";
    if (f == "smpteDrop") return "SMPTE Drop (29.970 fps)";
    if (f == "smpte30") return "SMPTE 30 (30.000 fps)";
    if (f == "cdtime") return "Audio CD Time";
    // Time & Frames at the project's rate (Reno has always called it SMPTE)
    return strfmt("SMPTE (%.3f fps)", projectFps(s));
}

std::string formatTime(const ProjectSettings& s, double shown, const std::string& f)
{
    if (!std::isfinite(shown)) shown = 0;
    const char* sign = "";
    if (shown < 0 && f != "measures") {
        if (shown > -0.5 / std::max(1, s.sampleRate)) shown = 0;
        else { sign = "-"; shown = -shown; }
    }
    if (f == "seconds") return strfmt("%s%.3f", sign, shown);
    if (f == "samples") return strfmt("%s%lld", sign, (long long)std::llround(shown * s.sampleRate));
    if (f == "frames") return strfmt("%s%lld", sign, (long long)std::floor(shown * projectFps(s) + 1e-6));
    if (f == "time") {
        // hours:minutes:seconds.milliseconds
        long long ms = (long long)std::floor(shown * 1000.0 + 1e-6);
        const long long secs = ms / 1000;
        ms %= 1000;
        return strfmt("%s%02lld:%02lld:%02lld.%03lld", sign, secs / 3600, (secs / 60) % 60, secs % 60, ms);
    }
    if (f == "measures") {
        // measure.beat.ticks, counted from 1 (a beat has 1000 ticks here)
        const double tempo = s.tempo > 0 ? s.tempo : 120.0;
        const int perMeasure = std::max(1, s.beatsPerMeasure);
        const double beats = std::max(0.0, shown) * tempo / 60.0;
        const long long whole = (long long)std::floor(beats + 1e-9);
        const int ticks = std::min(999, (int)std::floor((beats - (double)whole) * 1000.0 + 1e-6));
        return strfmt("%lld.%lld.%03d", whole / perMeasure + 1, whole % perMeasure + 1, ticks);
    }
    if (const int fpf = feetFrames(f)) {
        // feet+frames of film, the frames counted at the project's rate
        const long long frames = (long long)std::floor(shown * projectFps(s) + 1e-6);
        return strfmt("%s%lld+%02lld", sign, frames / fpf, frames % fpf);
    }
    const Counter c = counterOf(s, f.empty() ? std::string("timecode") : f);
    if (c.nominal > 0) {
        long long frames = (long long)std::floor(shown * c.rate + 1e-6);
        if (c.drop) frames = dropFrameDisplay(frames);
        const long long ff = frames % c.nominal, sec = frames / c.nominal;
        return strfmt("%s%02lld:%02lld:%02lld%c%0*lld", sign, sec / 3600, (sec / 60) % 60, sec % 60, c.drop ? ';' : ':',
                      frameDigits(c.nominal), ff);
    }
    return strfmt("%s%.3f", sign, shown);
}

bool parseTimeText(const ProjectSettings& s, const std::string& textIn, const std::string& f, double* shown)
{
    std::string text;
    for (char ch : textIn)
        if (!std::isspace((unsigned char)ch)) text += ch;
    if (text.empty()) return false;
    double sign = 1;
    if (text[0] == '-' || text[0] == '+') {
        if (text[0] == '-') sign = -1;
        text.erase(0, 1);
        if (text.empty()) return false;
    }
    bool ok = false;
    double v = 0;
    const double fps = projectFps(s);
    if (text.find('+') != std::string::npos) {
        // feet+frames (16 frames a foot unless the ruler is in 16mm feet)
        const std::vector<double> n = numbers(text, "+", &ok);
        if (!ok || n.size() != 2) return false;
        const int fpf = feetFrames(f) ? feetFrames(f) : 16;
        v = (n[0] * fpf + n[1]) / fps;
    } else if (f == "measures" && text.find(':') == std::string::npos && text.find(';') == std::string::npos &&
               std::count(text.begin(), text.end(), '.') >= 1) {
        // measure.beat[.tick]: counted from 1
        const std::vector<double> n = numbers(text, ".", &ok);
        if (!ok || n.size() > 3) return false;
        const double tempo = s.tempo > 0 ? s.tempo : 120.0;
        const int perMeasure = std::max(1, s.beatsPerMeasure);
        const double beats = (n[0] - 1) * perMeasure + (n.size() > 1 ? n[1] - 1 : 0) + (n.size() > 2 ? n[2] / 1000.0 : 0);
        v = std::max(0.0, beats) * 60.0 / tempo;
    } else if (text.find(':') != std::string::npos || text.find(';') != std::string::npos) {
        // hh:mm:ss;ff (frames: three separators, or a ';'), or hh:mm:ss.sss
        const bool framesLast = std::count(text.begin(), text.end(), ':') + std::count(text.begin(), text.end(), ';') >= 3 ||
                                text.find(';') != std::string::npos;
        std::vector<double> n = numbers(text, ":;", &ok);
        if (!ok || n.size() > 4) return false;
        Counter c = counterOf(s, f);
        if (c.nominal == 0) {
            c.rate = fps;
            c.nominal = std::max(1, (int)std::lround(fps));
        }
        double frames = 0;
        if (framesLast) {
            frames = n.back();
            n.pop_back();
            if (frames < 0) return false;
        }
        double secs = 0;
        for (double x : n) secs = secs * 60.0 + x;
        if (framesLast && c.drop && n.size() >= 1 && std::floor(secs) == secs) {
            const long long total = (long long)secs;
            v = (double)dropFrameFromDisplay(total / 3600, (total / 60) % 60, total % 60, (long long)frames) / c.rate;
        } else if (framesLast) {
            // the counter's seconds are nominal ones: count frames, then time them at the real rate
            v = (secs * c.nominal + frames) / c.rate;
        } else {
            v = secs;
        }
    } else {
        // a plain number in the format's unit (seconds for the clock-like ones)
        const std::vector<double> n = numbers(text, "", &ok);
        if (!ok || n.size() != 1) return false;
        if (f == "samples") v = n[0] / std::max(1, s.sampleRate);
        else if (f == "frames") v = n[0] / fps;
        else if (feetFrames(f)) v = n[0] * feetFrames(f) / fps;
        else if (f == "measures") v = std::max(0.0, n[0] - 1) * std::max(1, s.beatsPerMeasure) * 60.0 / (s.tempo > 0 ? s.tempo : 120.0);
        else v = n[0];
    }
    if (!std::isfinite(v)) return false;
    *shown = sign * v;
    return true;
}

}  // namespace reno
