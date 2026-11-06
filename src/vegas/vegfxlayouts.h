// Which settings of which OpenFX effects VEGAS keeps as whole numbers (choices, switches, counts) and which as doubles.
// A record VEGAS wrote says so for each setting it holds, but a setting Reno writes for an effect that has no record in
// the project (a project made from a .reno, a newly added effect) has only its value, and a whole number and a double
// of the same value are laid out differently. The table (vegfxlayouts.inc) is made from VEGAS's own records of the effects
// the sample projects use (tests/vegas/vegfxlayout_gen.cpp).
#pragma once
#include <string>

namespace reno {
namespace veg {

// 1: VEGAS keeps the setting as whole numbers; 0: as doubles; -1: not known.
int knownFxSettingIsWhole(const std::string& effectId, const std::string& setting);

}  // namespace veg
}  // namespace reno
