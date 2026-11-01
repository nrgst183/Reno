// "reno_cli vegasfx": checks which of VEGAS's own OpenFX effects work in Reno and how close Reno's built-in versions are.
//
// For every effect identifier that Reno has as a built-in and finds as a real plug-in on this machine, and for every VEGAS plug-in with no built-in, the
// real plug-in is run in a process of its own (a crash or hang cannot stop the run): loaded, an instance made, a few frames of a test picture rendered at
// the defaults, with every setting at its smallest / middle / largest value one at a time, and with the plug-in's presets. The same is rendered with
// Reno's built-in and the two are compared. Output: vegasfx_report.txt and .json and pictures of the worst cases (test picture, real, built-in,
// difference). Without VEGAS's plug-ins on the machine (Linux, macOS) only the built-ins are tried.
//
// The pieces run as reno_cli commands (plugincheck.cpp sends them here):
//   vegasfx      the whole run (runVegasFxReport)
//   vegasfxrun   one effect in one context from one source, in the process the run started for it
//   vegasfxlist  the effects in one plug-in file
//   vegasfxdescribe  --out FILE [--only TEXT] [--size WxH]: every built-in effect, per context, with its settings (type, size, default, allowed and shown
//                ranges, choices) and presets (JSON), for planning a capture (tools/effect_capture_compare.py --plan)
//
// A scenario given to vegasfxrun (--scenarios-in) is { "name", "kind", "frames", "set": { setting: [numbers] | "text" } } and may also give:
//   "a" / "b"            a raw picture file (width x height x 4 bytes, premultiplied BGRA, bottom row first) for the effect's source / a transition's "to" clip
//   "seqA" / "seqB"      a list of such files, one per frame (a moving clip)
//   "times"              the effect's time for each frame (default 10, 11, ...); "timeline": the position on the timeline (OfxPropVegasTimeCode) for each frame
//   "progress"           a transition's progress (0..1) for each frame (default: evenly spread over the frames)
// "frames" grows to the longest of these lists. tools/effect_capture_compare.py makes these from the manifest of tools/scripts/"Reno Effect Capture.cs".
#pragma once
#include "ofxhost.h"

namespace reno {
namespace ofx {

struct CompareOptions {
    std::string outDir = ".";
    int width = 320, height = 180;
    int frames = 3;
    std::string only;
    std::vector<std::string> pluginPaths;
    bool quick = false;
    bool stubs = false;
    bool all = false;
    int maxParams = 12;
    int timeoutSec = 300;
    std::vector<std::string> resourceDirs;
    bool paramsOnly = false;
    // Keep every picture VEGAS's own plug-in made: <outDir>/reference/<effect>_<context>/ holds the raw frames (<n>_<frame>.bgra: premultiplied BGRA, bottom row
    // first, width x height), real.json (each test's settings) and the test pictures it was given (source_<frame>.bgra).
    bool keepReal = false;
    bool gpu = true;
};

// The whole run. The exit code (0: the report was written).
int runVegasFxReport(const CompareOptions& opt);

// "vegasfxrun" / "vegasfxlist": the child processes of the run (not meant to be typed).
int runVegasFxChild(const std::string& cmd, const std::vector<std::string>& args);

// The parameter lists of the built-ins against VEGAS's own bundles' resource files (Contents/Resources/<name>.xml: every effect's
// identifier, and per context each setting's type, name, label and choices). 'dirs' are folders of bundles (or bundles). Fills
// 'out' (JSON: per effect the differences) and 'lines' (the text of the report); returns how many built-ins differ.
int compareParameterLists(const std::vector<std::string>& dirs, Json& out, std::vector<std::string>& lines);

// The pieces the tests use ---------------------------------------------------------------------------------------

// A setting of a preset: numbers, or text for String / Custom settings.
struct PresetValue {
    std::string name;
    std::vector<double> numbers;
    std::string text;
    bool isText = false;
};
struct PresetEntry {
    std::string plugin, context, name;
    std::vector<PresetValue> values;
};
// Reads the presets out of an OfxPresetPackage's text (no XML library: the format is regular).
std::vector<PresetEntry> parsePresetPackage(const std::string& xml);

// A PNG file (8 bits, RGB, stored uncompressed: big, but needs no library) from top-down RGB bytes.
std::string encodePng(int w, int h, const std::vector<uint8_t>& rgb);

}  // namespace ofx
}  // namespace reno
