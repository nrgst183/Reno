// VEGAS's older video effects (neither OpenFX nor XML objects) as a .veg stores them, and their mapping to Reno's
// built-in effects of the same names: Swirl, Pinch/Punch, Wave, TV Simulator and Light Rays.
//
// These are the pre-OpenFX (DirectX-era) plug-ins, kept by class GUID with settings as plain numbers. The record in
// an event's (or track's) effect list is a chunk of the same kind as an OpenFX effect's (G::Fx), laid out as
// vegmanaged.h describes for the XML effects but with doubles instead of XML (offsets from the record's start):
//   0x00 u32 0x38   0x04 class GUID
//   0x14 u32 bytes of the static block (the effect's settings as it starts: 8 bytes per setting, no length in front)
//   0x18 u32 keyframe count   0x1c, 0x20, 0x24 u32 0   0x28 u32 2
//   0x2c u32 flags: 0x30c for most of these (0x10e for Light Rays; 0x30e for the XML ones). Bit 0x80 is "apply
//        before pan/crop": across all 291 chains of two or more effects in the 37 samples (63 with an effect before
//        pan/crop) such effects are always first in the chain. Bits 0x10 (every TV Look) and 0x40 (one record) are
//        unexplained.
//   0x30 u32 (0..7; not worked out)   0x34 u32 preset name bytes   0x38 preset name (UTF-16LE with its 0)
//   the static block
//   per keyframe: u32 0x1c, i64 time (100 ns from the event's start), 12 bytes (0 in every sample), u32 settings
//   bytes (8 per setting), the settings as f64.
// The static block is the effect's defaults (Swirl and Pinch/Punch: Amount 1, size 1 by 1; Wave: "Reset to None"
// holds the "Large" preset); the keys carry what the event uses. The record ends at the last key; the chunk pads
// to a multiple of 8.
//
// Which setting is where, from comparing the sample numbers with VEGAS's OpenFX presets of the same names
// (Vfx1.ofx.bundle's PresetPackage.xml; units are the OpenFX ones):
//   Swirl, Pinch/Punch (6): Amount, X, Y, Horizontal size, Vertical size, Proportional (a 64-bit whole number; 1
//     reads as the double 5e-324). X and Y are taken as the centre's offset from the middle of the frame, -1..1
//     across, y up like the OpenFX Center. GUESSED (no sample moves it far).
//   Wave (6): Vertical waves, Horizontal waves, Vertical amplitude, Horizontal amplitude, Vertical phase, Horizontal
//     phase. The static block equals the OpenFX preset "Large" (5.4, 5.4, 0.05, 0.05, 0, 0); the preset name is
//     "Reset to None".
//   Light Rays (13): Light source x and y, Strength, Sensitivity, Bound radius (a 64-bit whole number), Radius X,
//     Radius Y, Feather, Tint colour r g b, Blend, Noise. The static block equals the OpenFX preset "Weak Light
//     Rays".
//   TV Simulator (8): Detail zoom, Aperture grill, Interlacing, Line sync, Vertical sync, Scan phasing,
//     Phosphorescence, Static. Detail zoom is the magnification itself (1 = none), so the built-in's zoom z is
//     (number - 1) / 3 (it magnifies by 1 + 3 z) (GUESSED); the other seven are the OpenFX numbers as they are. That
//     includes Line sync and Vertical sync (1 = in sync): the coverage run's "TV Look" record holds 1, 1 for them, as the
//     OpenFX preset of that name does, and Reno's picture with them taken so matches VEGAS's snapshot of it (mean 0.06 levels;
//     taken as 1 - number it was far off). The older samples' static block of 0 for both is VEGAS's unused default, not what
//     the keys use (keys hold 1.0 and 0.43..0.75).
// Not mapped yet (the same record frame, class GUIDs and flags 0x30e / 0x21c / 0x304; read from the coverage run's presets, all with one key equal to the
// static block, flags stored as 64-bit whole numbers, 1 = 5e-324):
//   Deform {f4205ab4-e8c1-468f-9ad2-4feb8df04538}, 14 doubles: 0 Amount, 1 Center Image (flag), 4 Shear Horizontal, 5 Shear Vertical, 6 Squeeze Top (the other
//     slots are 0 in "Reset to None", "Shear Horizontally", "Shear Vertically" and "Squeeze Top"; which are Squeeze Left / Right / Bottom and the Curves is not shown).
//   Film Effects {d9a65ba3-0d06-11d2-9aac-00a0c99b12c5}, 35 doubles: 0 Grain, 1 Convert To Greyscale (flag), 2 Tint, 3..5 tint colour blue, green, red, 6 always 1.0, then
//     seven groups of four from 7 (a 32-bit-float number as a double, the second, a third, an on flag): vfxfilm.h's Preset table holds the same numbers.
//   Gradient Map {300f2f5f-d7c7-48be-b796-90fc740b654e}, 38 doubles: 0 Amount (1), 1 a 64-bit mode word, the gradient stops from 2 as 32-bit floats packed in doubles
//     ("Night Vision" holds 0.41, 0.0078 ..., not worked out).
// Bypass: bit 0x01 of the word at 0x2c is taken as "switched off", as in the OpenFX record. No sample has it set:
// inferred, not seen.
#pragma once
#include "vegformat.h"
#include <string>
#include <vector>

namespace reno {
namespace veg {

struct LegacyFxKey {
    int64_t time = 0;               // 100 ns from the event's start
    std::string extra;              // the 12 bytes after the time, as they are
    std::vector<double> v;
};

struct LegacyFxRecord {
    std::string classGuid;          // lower case, no braces
    std::string preset;
    uint32_t flags = 0;             // the word at 0x2c
    std::vector<double> statik;     // the static block
    std::vector<LegacyFxKey> keys;
    size_t end = 0;                 // the byte after the record (without the padding)
};

// Whether a record of one of these effects (a class known here) starts at 'off'.
bool isLegacyFxAt(const Bytes& b, size_t off);
bool readLegacyFx(const Bytes& b, size_t off, LegacyFxRecord& out);
// The bytes of 'rec' (unpadded) with the header and preset name of 'original' (the record it was read from at
// 'originalOff'). For a record as read, these are the original bytes.
std::string writeLegacyFx(const Bytes& original, size_t originalOff, const LegacyFxRecord& rec);

// The OpenFX id of Reno's built-in for the effect in the record.
std::string legacyPluginId(const LegacyFxRecord& r);

// The record as Reno settings: {"plugin": id, "params": {...}} as Reno's OpenFX host loads. Key times are in frames
// at 'fps'.
bool legacyFxState(const LegacyFxRecord& r, double fps, Json& state, std::vector<std::string>& problems);

// The record 'orig' with the settings of 'state' put in. Unchanged parts stay bit for bit; keyframes follow the
// state's. 'warnings' lists what Reno could not save.
bool legacyFxWith(const LegacyFxRecord& orig, const Json& state, double fps, LegacyFxRecord& out, std::vector<std::string>& warnings);

// "Apply before pan/crop" of a non-OpenFX record (these and the XML ones of vegmanaged.h): bit 0x80 of the word at
// 0x2c. Not available from any VEGAS export (scripting lists only OpenFX effects); inferred from the order of chains.
bool recordBeforePanCrop(const Bytes& b, size_t off);
void setRecordBeforePanCrop(std::string& record, bool on);
// Bypass of the older records: bit 0x01 of the same word, as in the OpenFX record. INFERRED (none of the 27 sample
// records is switched off).
bool legacyBypass(const LegacyFxRecord& r);
void setLegacyBypass(std::string& record, bool on);

}  // namespace veg
}  // namespace reno
