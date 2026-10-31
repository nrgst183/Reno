// Reno's Vulkan compute path for the built-in video effects (see src/ofx/vkeffects.cpp for the effects' side).
//
// A frame goes up, a few compute passes run over it, the result comes back - or stays on the card when the next built-in effect on the same event or track
// takes that frame as its input (see FrameIO::inSerial). Any failure (no Vulkan loader, driver or compute queue, lost device, out of memory, picture too big)
// makes run() return false and the caller renders on the CPU. Nothing here throws or asserts on a driver's answer.
#pragma once
#include "vkplan.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reno {
namespace gpu {

struct DeviceInfo {
    int index = 0;
    std::string name;
    uint32_t vendorId = 0, deviceId = 0, apiVersion = 0, driverVersion = 0;
    int type = 0;
    bool software = false;              // a CPU implementation (SwiftShader, llvmpipe, WARP)
    uint64_t localMemoryBytes = 0;
    uint32_t computeQueueFamily = 0xffffffffu;
    bool usable = false;
    uint32_t maxStorageRange = 0, maxPushConstants = 0, maxGroupInvocations = 0;
    bool timestamps = false;
    // Float controls (Vulkan 1.2 / VK_KHR_shader_float_controls), as the driver reports them: fcKnown is false when they could not be asked (a Vulkan 1.0/1.1 device).
    bool fcUseRte = false, fcUseDenorm = false, fcUseSzinf = false;      // the controls put into every kernel
    bool fcKnown = false, fcRteF32 = false, fcDenormPreserveF32 = false, fcDenormFlushF32 = false, fcRtzF32 = false, fcSignedZeroInfNanF32 = false;
    int fcDenormIndependence = -1, fcRoundingIndependence = -1;      // 0 = 32-bit only, 1 = all, 2 = none
    bool floatControlsUsed = false;     // every kernel is made with RoundingModeRTE and DenormPreserve for 32-bit floats
    std::string note;                   // why it is not usable
    std::string typeName() const;
    std::string apiVersionText() const;
};

// Whether the built-in effects may use the GPU (Preferences > Video "GPU API", reno_cli --gpu-api). Off at start: nothing touches Vulkan until it is
// switched on and a frame is first asked for.
void setEnabled(bool on);
bool enabled();
// Which device to use: -1 = the best (a discrete GPU first, then integrated, then any), else an index of listDevices().
// Taking effect at once (the device is closed and made again on the next use). Also read from the environment variable RENO_VK_DEVICE (an index or part of a
// device name) when nothing was chosen.
void selectDevice(int index);

// All Vulkan devices the driver offers (makes a short-lived instance; empty and 'why' set when there is no Vulkan at all).
std::vector<DeviceInfo> listDevices(std::string* why = nullptr);

// Makes the device if it is not there yet. True when frames can be run. 'why' says what failed otherwise (also kept in status()).
bool ready(std::string* why = nullptr);
bool deviceLost();
// "Vulkan: <device>" or the reason there is none.
std::string status();
// Why the last run() on this thread failed (the check that stopped it), or "".
std::string lastRunFailure();
DeviceInfo currentDevice();
// Closes everything (the device is made again on the next use); the pipeline cache is written first.
void shutdown();

// One frame in and out, as the host holds it: 8-bit (BGRA or RGBA, bottom row first) or float RGBA, rows 'rowBytes' apart.
struct FrameIO {
    uint32_t w = 0, h = 0;
    bool isFloat = false;
    bool bgra = true;                   // 8-bit byte order
    const uint8_t* in = nullptr;
    size_t inRowBytes = 0;
    uint8_t* out = nullptr;             // where the result goes
    size_t outRowBytes = 0;
    // Chains: a frame made by run() carries the number given as outSerial (0: none). If the next run's input is the frame with that number (unchanged since
    // - the caller vouches for that), it is not sent again.
    uint64_t inSerial = 0, outSerial = 0;
    bool noInput = false;               // a generator: there is no input frame (buffer 0 of the plan is not filled)
    // Leave the download out: the result stays on the card, registered under outSerial (which must not be 0), and 'out' is NOT written. The caller must
    // then hold pendingHold(outSerial) with the frame, and call materialize() before anything reads 'out'. The next run() whose inSerial is that number
    // takes the frame from the card (the same 8-bit values the download would have given, so a chain gives the pictures one-by-one running gives).
    // Ignored (the frame is downloaded as usual) when too many frames are waiting.
    bool deferOut = false;
};
struct RunStats {
    double gpuMs = 0;                   // the kernels (the unpack, the passes, the pack), as the card's timestamps say
    double totalMs = 0;
    // where the rest of totalMs goes (0 when not measured): copying into the staging memory, recording, the card copying up / down (timestamps), the
    // whole submit-to-fence wait, copying out of the staging memory
    double fillMs = 0, recordMs = 0, copyUpMs = 0, copyDownMs = 0, waitMs = 0, readMs = 0;
    bool inputStayed = false;
    bool deferred = false;              // FrameIO::deferOut was done
    int passes = 0;
};
// Runs the plan on the frame. False: could not (nothing was written to 'out'); the caller then does it on the CPU.
bool run(const Plan& plan, FrameIO& io, RunStats* stats = nullptr);
// How many runs went to the card, how many of those found their input already there (chains), how many could not be done.
struct Counters { uint64_t runs = 0, inputsStayed = 0, failed = 0, deferred = 0; };
// Whether chained built-in effects may leave frames on the card (FrameIO::deferOut). On unless RENO_VK_NO_DEFER is set; the tests switch it to compare.
void setChainDeferral(bool on);
Counters counters();
// ---- frames left on the card (FrameIO::deferOut)
// While one of these lives on a thread, the built-in effect rendered on that thread (builtins.cpp's gpuRender) is asked to leave its download out. The host
// sets it around an effect's render when the next effect of the chain takes the frame from the card.
class DeferOutput {
public:
    explicit DeferOutput(bool on);
    ~DeferOutput();
    DeferOutput(const DeferOutput&) = delete;
    DeferOutput& operator=(const DeferOutput&) = delete;
private:
    bool prev_;
};
bool deferOutputRequested();
bool isPending(uint64_t serial);
// A holder to keep with the frame (Image::gpuHold): the card's copy is freed when the last reference goes. Null if nothing waits under that number.
std::shared_ptr<void> pendingHold(uint64_t serial);
// Downloads the frame waiting under 'serial' into dst (rows rowBytes apart, as the frame was given as 'out'). False when there is none or the card fails.
bool materialize(uint64_t serial, uint8_t* dst, size_t rowBytes);

// Forgets the frame kept on the card for chains (a different frame size, a project closed...).
void dropResident();

// The shaders the program carries (names), for the tests and vkcheck.
std::vector<std::string> shaderNames();
bool hasShader(const std::string& name);

}  // namespace gpu
}  // namespace reno
