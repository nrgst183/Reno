// VEGAS's ACES color management (manual: "S-Log and ACES workflow"), worked out without OpenColorIO.
#pragma once
#include "../core/base.h"
#include "project.h"
#include <string>
#include <vector>

namespace reno {


struct ColorSpaceChoice {
    std::string id;             // what is kept (MediaItem::colorSpace, ProjectSettings::viewTransform, a template's colorSpace)
    std::string label;          // as VEGAS lists it
};

// Media Properties > Color space (the first is Default, id ""), as VEGAS lists them for an ACES 1.0 or 0.7 project.
const std::vector<ColorSpaceChoice>& mediaColorSpaces(const std::string& acesVersion);
// Project Properties > View transform (the first is Off, id "off").
const std::vector<ColorSpaceChoice>& viewTransforms(const std::string& acesVersion);
// The Custom Template's Project tab > Color space (the first is Default, id "").
const std::vector<ColorSpaceChoice>& renderColorSpaces();
// The label of an id in any of the lists (the id itself if it is in none).
std::string colorSpaceLabel(const std::string& id);

// Whether the project composites in linear light (32-bit floating point full range with compositing gamma 1.000, or a view transform).
bool linearLightProject(const ProjectSettings& s);
// Whether the project's pixel format is one of the 32-bit floating point ones (pixelFormat "32bit": video levels or full range).
bool floatProject(const ProjectSettings& s);

// What the frame of a view transform / output color space is coded as, for the file's colour tags: "pq" (ST 2084) or
// "hlg" (ARIB STD-B67) with the colour primaries "bt2020" or "p3d65"; transfer "" for the others (SDR: untagged).
struct HdrCoding {
    std::string transfer;       // "", "pq", "hlg"
    std::string primaries;      // "bt2020", "p3d65"
};
HdrCoding hdrCodingOf(const std::string& colorSpaceId);

// The choices of Project Properties > Video > Master Display: "custom" (Custom display: the values as set) and some
// common mastering displays.
std::vector<std::string> masterDisplayPresetIds();
std::string masterDisplayPresetLabel(const std::string& id);
// The preset's values (the id "custom" or an unknown one gives 'current' back unchanged).
MasterDisplay masterDisplayPreset(const std::string& id, const MasterDisplay& current);

// A frame (any depth; a Float frame comes back, premultiplied) brought from the color space to scene-linear.
ImagePtr toSceneLinear(const ImagePtr& img, const std::string& colorSpace);
// A Float frame (scene-linear, premultiplied) brought in place to what a view transform or output color space
// makes of it for a display or a file.
void fromSceneLinear(Image& img, const std::string& transform);

// One straight colour, for tests.
void colorToSceneLinear(float rgb[3], const std::string& colorSpace);
void colorFromSceneLinear(float rgb[3], const std::string& transform);

}  // namespace reno
