// The 32-bit VST2 bridge: the messages between Reno (64-bit) and reno_vst32 (the 32-bit host process).
//
// A 32-bit plug-in cannot be loaded into a 64-bit program, so Reno starts reno_vst32.exe, which loads the plug-in and
// is driven from Reno (vst32client.cpp) over two channels:
//   - a pair of pipes for control: open, describe, parameters, state (chunks), programs, the editor window, ...
//     Messages are framed (FrameHead + payload); every request gets one reply, in order; the server may also send
//     notifications (the plug-in's window wants another size, a setting moved in the plug-in's window).
//   - shared memory for audio: Reno writes a block of samples, wakes the server with an event, the server runs the
//     plug-in over it in place and wakes Reno with a second event. No pipe, no polling. The memory also carries the
//     parameters (set by Reno, read back from the plug-in) so automation never waits for a message.
// Both sides run on the same machine, so numbers use the machine's byte order.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace reno {
namespace vst32 {

const uint32_t kProtocolVersion = 1;

// ---------------------------------------------------------------- control messages
struct FrameHead {
    uint32_t size;      // bytes of payload that follow
    uint32_t op;        // an Op, with kReplyFlag set on a reply
    uint32_t seq;       // a request's number; its reply has the same (notifications: 0)
    uint32_t reserved;
};
const uint32_t kReplyFlag = 0x80000000u;
const uint32_t kMaxPayload = 256u * 1024u * 1024u;

enum Op : uint32_t {
    // requests (Reno -> server), each answered
    OpOpen = 1,             // path -> ok, error, name, vendor, uid, inputs, outputs, flags, params, programs, latency
    OpClose = 2,            // the server unloads the plug-in and exits
    OpActivate = 3,         // sampleRate, maxBlock -> ok
    OpDeactivate = 4,
    OpSetChannels = 5,      // n -> ok, channels now
    OpParamName = 6,        // index -> text
    OpParamText = 7,        // index -> text
    OpParamGet = 8,         // index -> value (for settings that do not fit in the shared memory)
    OpParamSet = 9,         // index, value
    OpSaveState = 10,       // -> the plug-in's state as Reno stores it (see AudioPlugin::saveState)
    OpLoadState = 11,       // state -> ok
    OpProgramCount = 12,
    OpProgramName = 13,     // index -> text
    OpProgramCurrent = 14,  // -> index
    OpProgramSet = 15,      // index -> ok
    OpSavePreset = 16,      // bank? -> bytes of an .fxp / .fxb
    OpLoadPreset = 17,      // bytes -> ok, error
    OpEditorOpen = 18,      // parent window (a handle of Reno's) -> ok, width, height
    OpEditorClose = 19,
    OpLatency = 20,         // -> samples
    OpPing = 21,
    OpEditorSize = 22,      // -> ok, width, height (what the open window needs now)
    // notifications (server -> Reno)
    NotifyHello = 100,      // the server is up: protocol version
    NotifyEditorResize = 101,   // width, height
    NotifyParamChanged = 102,   // index
};

// Writing and reading a message's payload.
class Buf {
public:
    Buf() {}
    explicit Buf(std::string bytes) : d_(std::move(bytes)) {}
    void u32(uint32_t v) { d_.append((const char*)&v, 4); }
    void i32(int32_t v) { d_.append((const char*)&v, 4); }
    void u64(uint64_t v) { d_.append((const char*)&v, 8); }
    void f32(float v) { d_.append((const char*)&v, 4); }
    void f64(double v) { d_.append((const char*)&v, 8); }
    void str(const std::string& s) { u32((uint32_t)s.size()); d_ += s; }
    uint32_t getU32() { uint32_t v = 0; take(&v, 4); return v; }
    int32_t getI32() { int32_t v = 0; take(&v, 4); return v; }
    uint64_t getU64() { uint64_t v = 0; take(&v, 8); return v; }
    float getF32() { float v = 0; take(&v, 4); return v; }
    double getF64() { double v = 0; take(&v, 8); return v; }
    std::string getStr()
    {
        const uint32_t n = getU32();
        if (!ok_ || n > d_.size() - pos_) { ok_ = false; return std::string(); }
        std::string s = d_.substr(pos_, n);
        pos_ += n;
        return s;
    }
    bool ok() const { return ok_; }                 // false once a read went past the end
    const std::string& bytes() const { return d_; }
private:
    void take(void* out, size_t n)
    {
        if (!ok_ || n > d_.size() - pos_) { ok_ = false; return; }
        std::memcpy(out, d_.data() + pos_, n);
        pos_ += n;
    }
    std::string d_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------- shared memory
const uint32_t kShmMagic = 0x32335652;      // "RV32"
const uint32_t kMaxChannels = 6;
const uint32_t kBlockCapacity = 16384;      // frames in one hand-over (longer stretches are passed in pieces)
const uint32_t kMaxParams = 4096;           // settings beyond these go through the pipe

struct ShmHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;                      // frames the audio area holds per channel
    uint32_t maxParams;
    unsigned char sems[2][64];              // (Linux) the two semaphores, request and response; unused on Windows (named events)
    std::atomic<uint32_t> quit;             // Reno asks the audio thread to end
    std::atomic<int32_t> latency;           // the plug-in's latency in samples, as of the last block
    uint32_t reqFrames, reqChannels, reqPlaying;
    uint32_t padA;                          // keeps the 64-bit number below at the same offset for 32- and 64-bit programs
    int64_t reqSamplePos;
    std::atomic<uint32_t> blocks;           // blocks the server has handled (for tests)
    std::atomic<uint32_t> numParams;        // setting count (the server fills it in at OpOpen)
    uint32_t pad[18];
};
static_assert(sizeof(ShmHeader) == 256, "the shared memory's header must be laid out the same in a 32-bit and a 64-bit program");
static_assert(sizeof(std::atomic<uint32_t>) == 4, "atomics in shared memory are plain 32-bit numbers");

// The audio area (kMaxChannels x capacity floats, one channel after another), then the three tables of settings.
inline size_t shmBytes()
{
    return sizeof(ShmHeader) + (size_t)kMaxChannels * kBlockCapacity * sizeof(float) + (size_t)3 * kMaxParams * 4;
}
inline float* shmAudio(void* base, uint32_t channel) { return (float*)((char*)base + sizeof(ShmHeader)) + (size_t)channel * kBlockCapacity; }
inline std::atomic<uint32_t>* shmParamIn(void* base)      // what Reno set (floats as bits)
{
    return (std::atomic<uint32_t>*)((char*)base + sizeof(ShmHeader) + (size_t)kMaxChannels * kBlockCapacity * sizeof(float));
}
inline std::atomic<uint32_t>* shmParamDirty(void* base) { return shmParamIn(base) + kMaxParams; }     // 1: set by Reno, not yet passed on
inline std::atomic<uint32_t>* shmParamOut(void* base) { return shmParamIn(base) + 2 * kMaxParams; }   // what the plug-in has

inline uint32_t floatBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
inline float bitsFloat(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

}  // namespace vst32
}  // namespace reno
