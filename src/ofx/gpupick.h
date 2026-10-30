// Which way a built-in effect with a Vulkan kernel is rendered: on the card or on the processor. A frame has to go up and come back, so a cheap effect on a big
// picture can be quicker on the CPU even where the kernel is much faster than the CPU code. Reno measures both, a few frames per effect at 1080p, once per
// device (card name + driver + Reno version; the result is kept in a file in Reno's data folder), on a background thread while the splash screen shows. Until it
// is there, and for an effect that was not measured, the GPU is used (the default of the Vulkan path). Only paths that give the same pixels are ever picked: an
// effect whose GPU picture differs from its CPU one in the measurement goes to the CPU (the CPU code is the reference).
//
// Preferences > Video "GPU effects": "Fastest" (this) or "Always the GPU" (setAuto(false), no choice made). "Off (CPU only)" of the GPU API stays the way to force the CPU.
#pragma once
#include "builtins_impl.h"
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace pick {

// On: effects measured to be quicker on the CPU use it. Off: every effect with a kernel uses the GPU (when Vulkan is on).
void setAuto(bool on);
bool isAuto();
// Whether this effect is to be tried on the GPU now (called for each frame; cheap).
bool useGpu(const Def& d);

struct Entry {
    std::string id;
    bool gpu = true;                // the choice
    double cpuMs = 0, gpuMs = 0;    // the slowest of the settings measured (what the choice was made on)
    int cases = 0;
    bool differs = false;           // the GPU picture was not the CPU's: CPU forced
};

// Starts the measurement on its own thread (nothing happens when Vulkan is off, there is no usable card, or a software implementation is the only one). Uses
// the saved result when it is for this device, Reno version ('renoVersion') and kernel revision, unless 'again'. Returns at once.
void startBackground(const std::string& renoVersion, bool again);
bool measuring();
// "Checking the graphics card..." while the background thread has not yet got an answer, else "".
std::string statusLine();
// Waits for the thread (at exit, and for the CLI).
void join();
// The measurement on this thread (reno_cli gpupick): prints one line per effect to 'log' (a FILE*, may be null). Returns the number of effects measured, -1 when the GPU is not usable.
int runNow(const std::string& renoVersion, bool again, void* log);
std::vector<Entry> results();
std::string deviceKey();

}  // namespace pick
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
