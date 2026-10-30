// Reno's built-in OpenFX plug-ins: VEGAS's stock media generators and the video effects VEGAS users rely on most, for when VEGAS's own plug-ins
// are not there (no VEGAS installed, or they do not load).
//
// They are ordinary OpenFX image effects compiled into Reno: the host "loads" them from the made-up path kBuiltinBinaryPath and they are described,
// instanced, keyframed, saved and rendered like any other plug-in. They carry the identifiers and parameter names of VEGAS's own
// (com.vegascreativesoftware:solidcolor, ...:gaussianblur ...), so a VEGAS project using them shows the same kind of frame without VEGAS. Where VEGAS's
// real plug-in is installed and works, it is used instead (the catalog hides the built-in).
//
// Each is Reno's approximation of VEGAS's effect; parameter names, choices and presets follow VEGAS Pro 18's (read from its OFX bundles' resource and
// preset files).
#pragma once
#include "ofxCore.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace reno {
namespace ofx {

// The path the built-in plug-ins are "loaded" from (Host::loadBinary knows it; it is no file).
extern const char* const kBuiltinBinaryPath;     // "builtin:reno"
bool isBuiltinPath(const std::string& path);
// Whether this built-in (by identifier) is a filter that renders on the Vulkan path now, so a frame handed to it may still be only on the card.
bool builtinGpuChainable(const std::string& id);

// The OpenFX entry points of the built-in "binary".
int builtinNumberOfPlugins();
OfxPlugin* builtinPlugin(int index);
OfxStatus builtinSetHost(const OfxHost* host);

// What the catalog lists for each built-in (without describing it through the host).
struct BuiltinInfo {
    std::string id, label, group;
    bool generator = false, filter = false, transition = false;
};
std::vector<BuiltinInfo> builtinInfos();

// The presets the built-ins come with, as an OfxPresetPackage (the format VEGAS's bundles keep theirs in).
const std::string& builtinPresetPackage();

// Text for the built-in Titles & Text generator. The engine has no font machinery of its own: the application sets
// a rasterizer (Qt's fonts) when it starts. Without one the generator draws only its background.
struct TextRequest {
    std::string text;
    std::string font = "Arial";
    double pixelSize = 48;
    bool bold = false, italic = false, underline = false;
    double tracking = 0;
    double lineSpacing = 1;
    int align = 1;
};
struct TextMask {
    int w = 0, h = 0;           // the block of text (all lines), top row first
    std::vector<uint8_t> a;     // coverage 0..255, w x h
};
using TextRasterizer = std::function<bool(const TextRequest&, TextMask&)>;
void setTextRasterizer(TextRasterizer fn);
bool rasterizeText(const TextRequest& req, TextMask& out);

// The text, font and style held in VEGAS's Titles & Text "Text" parameter (rich text: RTF); plain text is taken as it
// is. Exposed for the tests.
struct RichText {
    std::string text;
    std::string font;
    double points = 0;
    bool bold = false, italic = false, underline = false;
};
RichText parseRichText(const std::string& rtf);

}  // namespace ofx
}  // namespace reno
