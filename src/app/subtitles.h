// Insert > Insert Subtitles From File... / From Regions...: Titles & Text events made from the lines of a
// TXT or CSV file, or from region names, on a new top video track.
#pragma once
#include <string>
#include <vector>

class QWidget;

namespace reno {
namespace app {

struct SubtitleLine {
    double start = 0, length = 5.0;
    std::string text;
};

// The lines of a subtitle file. A line of "Timecode Start, Timecode End, Timecode Length, Subtitle Text"
// (tab- or comma-separated, quotes allowed) has its own place and length; any other line is a 5 second text.
std::vector<SubtitleLine> parseSubtitleText(const std::string& text, double fps);
std::vector<SubtitleLine> subtitlesFromRegions();

std::vector<std::string> subtitlePresetNames();
std::string defaultSubtitlePreset();
void setDefaultSubtitlePreset(const std::string& name);

// Puts the lines on a new top video track as Titles & Text events using the preset (empty: generator defaults),
// as one undo step "Insert Subtitles". Returns the number of events; 0 with *error if none.
int insertSubtitles(const std::vector<SubtitleLine>& lines, const std::string& preset, std::string* error);

bool runInsertSubtitlesFromFile(QWidget* parent);
bool runInsertSubtitlesFromRegions(QWidget* parent);

}  // namespace app
}  // namespace reno
