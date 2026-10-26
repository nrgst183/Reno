// VEGAS's "CoreGraphics" video effects (Rays, Glint...) as a .veg stores them, and their OpenFX stand-ins in Reno.
//
// These are managed (.NET) objects, not OpenFX plug-ins. Their record in an event's (or track's) effect list is a
// chunk of the same kind as an OpenFX effect's (G::Fx) but with the object's class GUID instead of G::OfxEffect,
// the preset name, and the object as XML text once per keyframe (offsets from the record's start):
//   0x00 u32 0x38 (where the preset name starts)   0x04 class GUID
//   0x14 u32 bytes of the "static" block after the name (4 + XML length), 0 if none
//   0x18 u32 keyframe count   0x1c, 0x20, 0x24 u32 0   0x28 u32 2   0x2c u32 0x30e (flags; bit 0x80 is "apply before
//        pan/crop" as in the OpenFX record, see veglegacyfx.h; no Rays/Glint sample has it set)
//   0x30 u32 (4, 1 or 0; not worked out: of 10 sample records Rays 4, Glint 1 (7), 0 (2), 4 (1); the same chain
//        layout has 1 in two projects and 0 in a third, so it is not the chain position; the older records of
//        veglegacyfx.h have 0..7 there)   0x34 u32 preset name bytes
//   0x38 preset name (UTF-16LE with its 0)
//   the static block: u32 XML bytes, the XML (UTF-8, "<?xml version="1.0"?>" first, CRLF line ends)
//   per keyframe: u32 0x1c, i64 time (100 ns from the event's start; the last key of an animated effect sits a few
//   ticks before the event's end), 12 bytes (0 in every sample), u32 XML bytes + 4, u32 XML bytes, the XML.
// In the samples the static block (when present) equals the first keyframe's text. Every key holds the whole object,
// so a keyframed effect repeats all settings at every key (a Sparkle Glint in a sample project has four keys that
// differ only in <Hue>: 0, 360, 0, 360).
// The XML is an XmlSerializer dump: <Rays>, <Glint>, with <Enabled>, settings as 0..1 fractions (degrees for hue),
// and <Mask> (mode, type and the mask object, <Mask xsi:type="WipeMask">). Layouts and scales were read off the 37
// samples and checked against the presets VEGAS ships for the same effects as OpenFX plug-ins (Vfx1.ofx.bundle:
// "Luminance" for Rays, "Sparkle" for Glint); see tests/vegas/vegmanaged_test.cpp. The OpenFX ids are
// com.vegascreativesoftware:raysvelvetmatter and :glintvelvetmatter, which Reno has stand-ins for.
// The same dll holds the other four CoreGraphics video effects, which no sample has: Starburst, Fill Light, Defocus
// and Soft Contrast (class Softlight), with VEGAS's presets for them as XML text; together with the OpenFX presets
// they fix every scale (see effects() in vegmanaged.cpp and tests/vegas/vegmanaged2_test.cpp). The dll's other
// serialised classes (Desaturate, DirectionalBlur, DropShadow, GaussianBlur, Glow, HslAdjust, Opacity, Vignette)
// belong to the title generator.
#pragma once
#include "vegformat.h"
#include <string>
#include <vector>

namespace reno {
namespace veg {

struct ManagedFxKey {
    int64_t time = 0;               // 100 ns from the event's start
    std::string extra;              // the 12 bytes after the time, as they are
    std::string xml;
};

struct ManagedFxRecord {
    std::string classGuid;          // lower case, no braces
    std::string root;               // the XML's root element: "Rays"
    std::string preset;
    bool hasStatic = false;
    std::string staticXml;
    std::vector<ManagedFxKey> keys;
    size_t end = 0;                 // the byte after the record (without the padding)
};

// Whether a record of this kind (not OpenFX) starts at 'off': the header fits and XML follows the name.
bool isManagedFxAt(const Bytes& b, size_t off);
bool readManagedFx(const Bytes& b, size_t off, ManagedFxRecord& out);
// The bytes of 'rec' (unpadded) with the header and preset name of 'original' (the record it was read from at
// 'originalOff'). For a record as read, these are the original bytes.
std::string writeManagedFx(const Bytes& original, size_t originalOff, const ManagedFxRecord& rec);

// The OpenFX id of Reno's stand-in for the record's effect; empty if there is none.
std::string managedPluginId(const ManagedFxRecord& r);

// The record as Reno settings: {"plugin": id, "params": {...}} as Reno's OpenFX host loads. Key times are in frames
// at 'fps'. 'bypass' is set if the effect's own <Enabled> is off. 'problems' lists what could not be read.
bool managedFxState(const ManagedFxRecord& r, double fps, Json& state, bool& bypass, std::vector<std::string>& problems);

// The record 'orig' with the settings of 'state' (and 'bypass') put in. Unchanged parts stay byte for byte;
// keyframes follow the state's. 'warnings' lists what Reno could not save.
bool managedFxWith(const ManagedFxRecord& orig, const Json& state, bool bypass, double fps, ManagedFxRecord& out,
                   std::vector<std::string>& warnings);

}  // namespace veg
}  // namespace reno
