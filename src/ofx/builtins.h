// Reno's built-in OpenFX plug-ins: VEGAS's stock media generators and the video effects VEGAS users rely on most, for when VEGAS's own plug-ins
// are not there (no VEGAS installed, or they do not load).
//
// They are ordinary OpenFX image effects compiled into Reno: the host "loads" them from the made-up path kBuiltinBinaryPath and they are described,
// instanced, keyframed, saved and rendered like any other plug-in. They carry the identifiers and parameter names of VEGAS's own
// (com.vegascreativesoftware:solidcolor, ...:gaussianblur ...), so a VEGAS project using them shows the same kind of frame without VEGAS. By default they
// are always used for VEGAS's identifiers and VEGAS's own plug-in bundles are not loaded (Host::skipVegasBundles).
//
// Each is Reno's approximation of VEGAS's effect; parameter names, choices and presets follow VEGAS Pro 18's (read from its OFX bundles' resource and
// preset files).
#pragma once
#include "ofxCore.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace reno {
namespace ofx {

// The path the built-in plug-ins are "loaded" from (Host::loadBinary knows it; it is no file).
extern const char* const kBuiltinBinaryPath;     // "builtin:reno"
bool isBuiltinPath(const std::string& path);
// Whether this built-in (by identifier) is a filter that renders on the Vulkan path now, so a frame handed to it may still be only on the card.
bool builtinGpuChainable(const std::string& id);
// VEGAS's "GPU acceleration" (Preferences > Video; the host's HostConfig::gpu). On (the default, as in the user's VEGAS), the built-in copies of VEGAS's effects that have an OpenCL
// processor in VEGAS draw as VEGAS's OpenCL kernels do (float); off, as VEGAS's CPU code does. Host::applyConfig passes the setting on.
namespace builtin {
void setVegasGpuAcceleration(bool on);
bool vegasGpuAcceleration();
}

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

// Fonts for the generator's own text layout (vfxtitles.h): outlines and metrics in em units. On Windows the engine reads them from GDI itself; elsewhere the
// application sets a source (Qt's fonts) when it starts.
struct TextSeg {
    char op = 'L';                      // 'M' move, 'L' line, 'C' cubic (three points), 'Z' close the figure
    float p[6] = {0, 0, 0, 0, 0, 0};    // points (x, y) in em units, y up
};
struct TextGlyph {
    double adv = 0;                     // advance in ems
    std::vector<TextSeg> segs;
};
struct TextFace {
    double ascent = 0.9, descent = 0.2, lineGap = 0;    // ems (hhea)
    virtual bool glyph(uint32_t codePoint, TextGlyph& out) = 0;
    virtual double kern(uint32_t, uint32_t) { return 0; }      // kerning between a pair, in ems
    virtual ~TextFace() {}
};
using TextFaceSource = std::function<std::shared_ptr<TextFace>(const std::string& family, bool bold, bool italic)>;
void setTextFaceSource(TextFaceSource fn);
std::shared_ptr<TextFace> textFaceFromSource(const std::string& family, bool bold, bool italic);

// The text, font and style held in VEGAS's Titles & Text "Text" parameter (rich text: RTF); plain text is taken as it
// is. Exposed for the tests.
struct RichText {
    std::string text;
    std::string font;
    double points = 0;
    bool bold = false, italic = false, underline = false;
    int align = 0;                      // \ql 0, \qc 1, \qr 2
    // The character formatting inside the text: one span per stretch of equal formatting, covering the whole text in order
    // (empty for plain text). points 0 = not set by the span.
    struct Span { size_t start = 0, len = 0; double points = 0; bool bold = false, italic = false; std::string font; };
    std::vector<Span> spans;
};
RichText parseRichText(const std::string& rtf);

}  // namespace ofx
}  // namespace reno
