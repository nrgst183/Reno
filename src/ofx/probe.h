// Trying a plug-in file out: load it, describe every effect in it, make one, render a few frames. Used by the plug-in scan (which runs it in a separate process so a crashing plug-in cannot
// take Reno down) and by "reno_cli check".
#pragma once
#include "ofxhost.h"

namespace reno {
namespace ofx {

struct ProbeOptions {
    bool render = true;
    bool instance = true;
                                // instance (cheap, and where VEGAS's effects say what they miss), then let it go
    int width = 640, height = 360;
    int frames = 3;
};

// One JSON object per file: { "path", "ok", "error", "plugins": [ { "id", "label", "group", "contexts", "ok", "error",
//   "params", "rendered", "changedFrame", "msPerFrame", "suites": [...] } ] }
Json probeBinary(const std::string& path, const ProbeOptions& opt);

// A moving test frame (colour bars, a gradient, a box that moves with the frame number).
ImagePtr makeTestPattern(int w, int h, double frame);

}  // namespace ofx
}  // namespace reno
