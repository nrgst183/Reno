// Commands for the main window's menus from the manual's previewing, captioning, color, disc and tape chapters:
//   File > Import > Closed Captioning...       caption files (.scc .srt .smi .rt .txt .mcc .sub) as 608CC1.. command markers, at their
//                                              timecodes less the ruler offset
//   File > Export > XDCAM Disc..., View > Window > XDCAM Explorer, Tools > Burn Disc > ..., Print Video to Tape... (not available: need hardware)
//   Tools > Relink Wireless Adapter Media...   proxy files (C0001S03.MP4, *_proxy.*) replaced by the full-resolution files in a folder
//   Tools > Lay Out Audio CD from Events       CD track regions over the audio events (touching events: one track)
//   Tools > Generate Loudness Log              the selection's (or timeline's) loudness into <project>_loud.txt
//   Tools > Scripting > Promote Media Closed Captioning, Export Closed Captioning for DVD Architect / QuickTime / Windows Media Player / YouTube
//   Options > Mute All Audio, Mute All Video, Loudness Meters >, External Control (not available)
#pragma once
#include "../engine/captions.h"
#include <QString>
#include <string>

namespace reno {
namespace app {

class MainWindow;

void addOutputCommands(MainWindow* w);

// The commands' work, for the menus and tests (each one undo step where it changes the project).
// Caption markers from a file: how many were added (0 with *error when none).
int importClosedCaptions(const std::string& path, std::string* error);
// The project's captions into a file (and the file it links to, for SAMI and QuickTime).
bool exportClosedCaptions(const std::string& path, CaptionFormat format, std::string* error);
// The selected events' media captions (the .scc beside each file, as VEGAS keeps them) as command markers on the timeline: how many.
int promoteMediaClosedCaptions();
// CD track regions over the audio events: how many.
int layOutAudioCdFromEvents();
// The proxy files among the project's media and how many have their full-resolution file in 'folder'; those are relinked
// when 'relink'.
int wirelessProxyCount();
int relinkWirelessMedia(const std::string& folder, bool relink = true);
// Writes the loudness of the time selection (or timeline) to 'path' (empty: <project>_loud.txt beside the project, else
// Documents); the path written is in *written.
bool generateLoudnessLog(const std::string& path, std::string* written, std::string* error);

}  // namespace app
}  // namespace reno
