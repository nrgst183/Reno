// The parameters and presets of the VEGAS stock effects Reno has stand-ins for, taken from VEGAS Pro 18's OpenFX resource and preset files by tools/gen_vfxdata.py (builtins_vfxdata.cpp is generated).
#pragma once
#include "builtins_impl.h"

namespace reno {
namespace ofx {
namespace builtin {

// One effect: its identifier, label, context ("Filter", "Transition", "General") and parameters as text,
// "kind|name|label|default...;..." with kind D double (default|min|max), I integer, B boolean, C choice (default|options
// separated by commas), K RGBA, R RGB, P position, S string.
struct VfxSpec { const char* id; const char* label; const char* context; const char* params; };
extern const VfxSpec kVfxSpecs[];
extern const int kVfxSpecCount;
// VEGAS's presets for them, as pieces of an OfxPresetPackage's text.
extern const char* const kVfxPresetChunks[];
extern const int kVfxPresetChunkCount;

// A built-in effect described by the spec for 'id' (with the parameters of VEGAS's own), drawn by 'fn'. A transition
// when the spec's context says so. Unknown ids give an empty definition (id empty).
Def vfxDef(const std::string& id, void (*fn)(Ctx&));
// Sets a double or integer parameter's default and (when given) range in a definition made by vfxDef.
void vfxTune(Def& d, const char* name, double def, double lo = NAN, double hi = NAN);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
