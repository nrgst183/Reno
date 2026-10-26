// Private to the media code (ffmpeg_reader.cpp, hwdecode.cpp and the hwdecode test): decoding video on the graphics
// card through FFmpeg's hwaccels. The public settings are in media.h (setHardwareDecoding...).
#pragma once
#include "ffmpeg_common.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <memory>
#include <string>
#include <vector>

namespace reno {
namespace hw {

enum class Platform { Windows, Linux, Mac, Other };
Platform currentPlatform();

// What a decoder opened on the card needs to remember (the decoder's opaque pointer points here).
struct State {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;   // the card's frame format for this decoder
    bool emulated = false;                      // tests: a software decoder whose frames are treated as the card's
    bool unsupported = false;                   // FFmpeg could not set the card up for this stream (get_format saw no card format)
    long received = 0;                          // frames this decoder has given
    std::string name;                           // "d3d11va"...
};

// ---- decisions that need no card (tested without one)
// The device kinds to try, in order, for a setting: 'mode' off gives none; 'api' names one (empty: the platform's order);
// only kinds in 'available' (what this FFmpeg has) are kept.
std::vector<AVHWDeviceType> candidateTypes(HwDecodeMode mode, const std::string& api, Platform platform, const std::vector<AVHWDeviceType>& available);
// Whether a stream is worth sending to a card and one is likely to take it: the codec (H.264, HEVC, VP9, AV1, MPEG-2,
// VC-1), 4:2:0 frames of 8 bits (10 for HEVC, VP9 and AV1), not too small.
bool codecEligible(AVCodecID codec, int profile, int pixFormat, int width, int height, std::string* why = nullptr);
const char* typeName(AVHWDeviceType t);
AVHWDeviceType typeFromName(const std::string& name);
std::vector<AVHWDeviceType> availableTypes();       // what this FFmpeg build knows (av_hwdevice_iterate_types)

// ---- devices and sessions
// A new reference to the process's one device of this kind (made on first use); null if it cannot be made (remembered, so
// that a machine without one is not tried again for every file until the settings change).
AVBufferRef* sharedDevice(AVHWDeviceType type);
// A place among the files decoding on the card at once; null (nothing taken) when the limit is reached.
std::shared_ptr<void> acquireSession();
int activeSessions();

// Sets 'ctx' up to decode on the card if the setting, the stream and the machine allow it: fills 'state', takes a
// session into 'token'.
bool attach(AVCodecContext* ctx, const AVCodec* codec, const AVCodecParameters* par, State& state, std::shared_ptr<void>& token);

// Whether a frame from a decoder with this state is in the card's memory (and so must be copied before use).
bool isOnCard(const State& state, const AVFrame* f);
// Marks a frame just received from the decoder (tests: emulated decoders mark every frame).
void markReceived(const State& state, AVFrame* f);
// Copies a frame from the card's memory into ordinary memory, in place (props and time stamps kept). Counted in
// transfers().
bool toSystemMemory(const State& state, AVFrame* f);

// ---- tests: pretend. With emulation on, attach() opens the ordinary software decoder but treats its frames as being
// on a card: toSystemMemory() really copies them (and counts), so the reader's decisions about WHEN to copy can be tested on
// a machine without a card. failTransferAfter / failDecodeAfter (-1: never) make the n-th copy / decoded frame fail.
// semiPlanar: the copy comes out as a card gives it, NV12 (8-bit 4:2:0) or P010 (10-bit), not as the decoder made it.
void setEmulation(bool on, int failTransferAfter = -1, int failDecodeAfter = -1, bool semiPlanar = false);
bool emulationFailsDecode();                         // true once when failDecodeAfter frames have gone by
long transfers();                                     // frames copied off a card (real or emulated) since startup
void resetCounters();

}  // namespace hw
}  // namespace reno
