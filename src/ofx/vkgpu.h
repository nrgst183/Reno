// Reno's Vulkan compute path for the built-in video effects (see src/ofx/vkeffects.cpp for the effects' side).
//
// A frame goes up, a few compute passes run over it, the result comes back - or stays on the card when the next built-in effect on the same event or track
// takes that frame as its input (see FrameIO::inSerial). Any failure (no Vulkan loader, driver or compute queue, lost device, out of memory, picture too big)
// makes run() return false and the caller renders on the CPU. Nothing here throws or asserts on a driver's answer.
#pragma once
#include "vkplan.h"
#include <cstdint>
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
};
struct RunStats {
    double gpuMs = 0;
    double totalMs = 0;
    bool inputStayed = false;
    int passes = 0;
};
// Runs the plan on the frame. False: could not (nothing was written to 'out'); the caller then does it on the CPU.
bool run(const Plan& plan, FrameIO& io, RunStats* stats = nullptr);
// How many runs went to the card, how many of those found their input already there (chains), how many could not be done.
struct Counters { uint64_t runs = 0, inputsStayed = 0, failed = 0; };
Counters counters();
// Forgets the frame kept on the card for chains (a different frame size, a project closed...).
void dropResident();

// The shaders the program carries (names), for the tests and vkcheck.
std::vector<std::string> shaderNames();
bool hasShader(const std::string& name);

}  // namespace gpu
}  // namespace reno
