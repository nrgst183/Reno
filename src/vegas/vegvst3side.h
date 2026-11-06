// VST3 plug-ins next to a .veg file.
//
// VEGAS Pro has no VST3 host: vegas180.exe / vegas180k.dll / sfvstwrap.dll (the VST wrapper) hold no VST3 code or
// strings, only the VST2 wrapper (FxCk/FPCh programs, .fxp/.fxb), and the VEGAS Pro 23 manual speaks only of "DirectX
// and VST" effects. So no .veg can hold a VST3 plug-in, and Reno must not invent a record VEGAS would not write.
// What Reno does instead, so that a VST3 effect still survives Save and Open of a .veg:
//   - the .veg itself is written exactly as before (the VST3 effects are left out of it; VEGAS opens it as a project
//     without them), and
//   - the VST3 effects (plug-in, its state exactly as Reno's host saved it, name, bypass) go to a small JSON file beside
//     it, "<project>.veg.reno-vst3", with where each one sat: which chain (track, event, bus or master), its position
//     in that chain. Opening the .veg in Reno puts them back. VEGAS ignores the file.
// The file is a convenience that can go stale (the .veg edited in VEGAS, moved or copied without it): effects whose
// track or event can no longer be found are not put back and Reno says so; nothing in the .veg is ever changed by it.
// A plug-in Reno cannot load stays in the file and in the project with its state untouched, and is written back as it was.
#pragma once
#include "../engine/project.h"
#include <string>
#include <vector>

namespace reno {

// Path of the side file of a .veg.
std::string vst3SidePath(const std::string& vegPath);

// How many VST3 audio effects the project has (all chains).
int countVst3(const Project& p);

// Writes the side file for the VST3 audio effects in 'p' (every chain: tracks, events, buses, master). With none, an
// old side file is removed. A failure is added to 'warnings' (the .veg is not affected). Returns how many effects
// were written.
int writeVst3Side(const Project& p, const std::string& vegPath, std::vector<std::string>* warnings);

// Puts the effects of the side file back into 'p' (just read from the .veg at 'vegPath'). Adds a line to 'notes' for
// what was restored and for what could not be placed. Returns how many were restored. No side file: 0, no notes.
int readVst3Side(const std::string& vegPath, Project& p, std::vector<std::string>* notes);

}  // namespace reno
