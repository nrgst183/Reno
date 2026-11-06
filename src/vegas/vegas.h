// Reading and writing VEGAS Pro projects (.veg). Reading (vegas.cpp) keeps what VEGAS wrote for every object;
// writing (vegwrite.cpp) changes only what was edited, so VEGAS Pro opens the result.
#pragma once
#include "../engine/project.h"

namespace reno {

struct VegasImportResult {
    bool ok = false;
    std::string error;                  // why it failed (ok == false)
    std::vector<std::string> notes;     // what Reno cannot use, in plain words, one per line
};

// Reads a VEGAS project file into a Reno project.
VegasImportResult importVegasProject(const std::string& path, Project& out);

// True if the file looks like a VEGAS project (by its contents, not its name).
bool isVegasProject(const std::string& path);

// Opens a project of either kind, VEGAS (.veg) or Reno's own (.reno), detected by contents.
bool loadProjectFile(const std::string& path, Project& out, std::string* error);
// Same, also returning what Reno cannot use of a VEGAS project (empty for .reno files).
bool loadProjectFileWithNotes(const std::string& path, Project& out, std::string* error, std::vector<std::string>* notes);

// Frame size, frame rate, length (end of the last event) and whether there are frames and sound, for a .veg used
// as media in another project.
bool vegasProjectMediaInfo(const std::string& path, MediaInfo& info, std::string* error);

struct VegasSaveResult {
    bool ok = false;
    std::string error;                  // why it failed (ok == false)
    std::vector<std::string> warnings;  // what the .veg cannot hold (left out), in plain words, one per line
};

// Writes the project as a VEGAS Pro 18 project. Written to a temporary file next to 'path', the old file is kept
// as "<path>.bak" (as VEGAS does), then the new one replaces it. On success the project is tied to the written
// file (Project::veg and the objects' origins) so the next save starts from it.
VegasSaveResult saveVegasProject(Project& p, const std::string& path);

// The curve an event's cross-fade-in lets the event before it fade out by (VEGAS's FadeIn.ReciprocalCurve, which a .veg
// always holds): the event's own when given, else - as the mixer does - the fade-out curve of the earlier event it
// overlaps, else the mirror of its fade-in. Used by the .veg writer and by the model dump, so both say what the mixer plays.
int effectiveFadeInRecipCurve(const Track& t, const Event& e);

// Video effects and transitions of VEGAS's older kinds (not OpenFX: Swirl, Film Effects, 3D Blinds...) as the .veg holds them: the class GUID
// (upper case, no braces), the preset name, the switches in the record, and for effects Reno cannot host (kept in the file, not in
// the chain) how many of the chain's effects come before it. For the model dump (src/cli/modeldump.cpp).
struct VegOlderFx {
    std::string classGuid, preset;
    bool bypass = false, beforePanCrop = false;
    size_t before = 0;
};
// True (and 'out' filled) if the slot was read from a record of the older kind.
bool vegOlderFxOf(const FxSlot& s, VegOlderFx& out);
// The settings record of one of VEGAS's older (DirectX-era) generators, Credit Roll or Color Gradient, which Reno hosts as its own
// built-in without reading the record's settings: class GUID and preset name, for the model dump.
bool vegLegacyGeneratorOf(const MediaItem& m, VegOlderFx& out);
// The older effects of a track's / event's / the video output's chain that Reno cannot host.
std::vector<VegOlderFx> vegHiddenFxOf(const Track& t);
std::vector<VegOlderFx> vegHiddenFxOf(const Event& e);
std::vector<VegOlderFx> vegHiddenVideoOutFxOf(const Project& p);

// The bytes saveVegasProject would write, without writing (for tests). 'path' is where the file would go
// (VEGAS stores the project path in it).
std::string writeVegasData(const Project& p, const std::string& path, std::vector<std::string>* warnings);

}  // namespace reno
