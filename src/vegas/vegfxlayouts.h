// Which settings of which OpenFX effects VEGAS keeps as whole numbers (choices, switches, counts) and which as doubles.
// A record VEGAS wrote says so for each setting it holds, but a setting Reno writes for an effect that has no record in
// the project (a project made from a .reno, a newly added effect) has only its value, and a whole number and a double
// of the same value are laid out differently. The table (vegfxlayouts.inc) is made from VEGAS's own records of the effects
// the sample projects use (tests/vegas/vegfxlayout_gen.cpp).
#pragma once
#include <functional>
#include <string>

namespace reno {
namespace veg {

// 1: VEGAS keeps the setting as whole numbers; 0: as doubles; -1: not known. Where the table does not say, the
// program's own answer (setFxSettingKindSource) is asked.
int knownFxSettingIsWhole(const std::string& effectId, const std::string& setting);
// The program's answer for settings the table does not have: the effect's plug-in says what kind each setting is (a
// live instance of it, made for the project being saved). 1 whole numbers, 0 not, -1 not known. Main thread.
void setFxSettingKindSource(std::function<int(const std::string& effectId, const std::string& setting)> fn);

}  // namespace veg
}  // namespace reno
