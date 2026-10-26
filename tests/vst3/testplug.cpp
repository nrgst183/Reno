// A VST3 plug-in file for testing Reno's VST3 host (src/audiofx/vst3host.cpp). It is written against Reno's own
// declarations of the VST3 interfaces (src/audiofx/vst3_abi.h), not the Steinberg SDK, so it shows that the host and
// those declarations agree and that the host calls things in the order VST3 asks for. It cannot show that the
// declarations are right: that is abicheck.cpp's job.
//
// Three effects and the controller two of them share:
//   Reno Test Gain   stereo in (plus a stereo side-chain input bus), stereo out; out = in * gain.
//                    Its controller is a separate object; the two halves talk through IConnectionPoint.
//   Reno Test Mono   mono in, mono out, refuses to be anything else; same separate controller; the
//                    component has no IConnectionPoint. Reports a latency of 7 samples.
//   Reno Test Tone   an "instrument": no audio input, stereo out = 0.25 * gain; component and controller
//                    are one object; has no window.
// Parameters (the controller's order): Gain (id 100; 0..1 is a gain of 0..2), Secret (id 7, hidden),
// Page (id 200; lives only in the controller's state), Level (id 300; read-only, the peak of the last
// block, sent by the processor through the output parameter changes).
//
// The plug-in counts what the host does and everything it does wrong ("errors"); the test program reads the counts
// through RenoTestPlugGet and makes the plug-in act like a user in its window through RenoTestPlugPoke.
// Environment: RENO_TESTPLUG_FACTORY=1, 2 or 3 limits the factory to IPluginFactory, IPluginFactory2
// or IPluginFactory3 (the default).
#include "../../src/audiofx/vst3_abi.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace reno::vst3;

#if defined(_WIN32)
#  define TEST_EXPORT __declspec(dllexport)
static const char* const kWindowKind = "HWND";
#else
#  define TEST_EXPORT __attribute__((visibility("default")))
static const char* const kWindowKind = "X11EmbedWindowID";
#endif

namespace {

// ------------------------------------------------------------------ counts
struct Counts {
    std::atomic<int> live{0};               // plug-in objects that exist
    std::atomic<int> starts{0};             // ModuleEntry / InitDll calls
    std::atomic<int> factories{0};          // GetPluginFactory calls
    std::atomic<int> hostContexts{0};       // IPluginFactory3::setHostContext calls
    std::atomic<int> initialized{0}, terminated{0};
    std::atomic<int> connects{0}, disconnects{0};
    std::atomic<int> messages{0};           // messages that arrived whole
    std::atomic<int> componentStates{0};    // setComponentState calls
    std::atomic<int> errors{0};             // things the host did that VST3 does not allow
    std::atomic<int> blocks{0};             // process calls
    std::atomic<int> points{0};             // parameter changes that came in with blocks
    std::atomic<int> playing{0};            // the last block: transport playing
    std::atomic<int> position{0};           // the last block: project position in samples
    std::atomic<int> maxBlock{0};           // the biggest block seen
    std::atomic<int> sidechainOn{0};        // the side-chain bus was active when processing started
    std::atomic<int> viewsOpen{0};          // windows attached
    std::atomic<int> onSize{0};             // IPlugView::onSize calls
    std::atomic<int> arrangements{0};       // the surround kind: arrangements it took
    std::atomic<int> surroundBlocks{0};     // the surround kind: blocks processed in 5.1
    std::atomic<int> sizeWidth{0};          // the width the last onSize gave
    std::atomic<int> handlers{0};           // controllers that currently hold a component handler
};
Counts g;

void wrong(const char* what)
{
    ++g.errors;
    std::fprintf(stderr, "testplug: the host did something wrong: %s\n", what);
}

void put16(char16_t* dst, size_t room, const char* text)
{
    size_t k = 0;
    for (; text[k] && k + 1 < room; ++k) dst[k] = (char16_t)(unsigned char)text[k];
    dst[k] = 0;
}

void put8(char* dst, size_t room, const char* text)
{
    std::memset(dst, 0, room);
    std::strncpy(dst, text, room - 1);
}

const TUID kGainCid = RENO_VST3_ID_BYTES(0x52454E4F, 0x54455354, 0x00000001, 0x4741494E);
const TUID kMonoCid = RENO_VST3_ID_BYTES(0x52454E4F, 0x54455354, 0x00000002, 0x4D4F4E4F);
const TUID kToneCid = RENO_VST3_ID_BYTES(0x52454E4F, 0x54455354, 0x00000003, 0x544F4E45);
const TUID kSurroundCid = RENO_VST3_ID_BYTES(0x52454E4F, 0x54455354, 0x00000004, 0x35505431);
const TUID kControllerCid = RENO_VST3_ID_BYTES(0x52454E4F, 0x54455354, 0x000000FF, 0x4354524C);

enum Kind { kGainKind, kMonoKind, kToneKind, kControllerOnly, kSurroundKind };
enum : ParamID { kGainParam = 100, kSecretParam = 7, kPageParam = 200, kLevelParam = 300 };

class Plug;
class View;
std::vector<Plug*> gControllers;            // the controllers there are; RenoTestPlugPoke acts on the newest
View* gLastView = nullptr;                  // and on the window made last

// ------------------------------------------------------------------ the window (there is no real one)
class View final : public IPlugView {
public:
    explicit View(Plug* owner);
    ~View();

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        if (iidEqual(iid, FUnknown_iid) || iidEqual(iid, IPlugView_iid)) { addRef(); *obj = static_cast<IPlugView*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32_t PLUGIN_API addRef() override { return ++refs_; }
    uint32_t PLUGIN_API release() override
    {
        const uint32_t left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override { return (type && !std::strcmp(type, kWindowKind)) ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API attached(void* parent, FIDString type) override
    {
        if (!type || std::strcmp(type, kWindowKind)) { wrong("attached with the wrong kind of window"); return kResultFalse; }
        if (!frame_) wrong("attached before setFrame");
        if (attached_) wrong("attached twice");
        if (!parent) { wrong("attached to no window"); return kResultFalse; }
        if (parent == (void*)(size_t)0xBAD) return kResultFalse;        // (the test asks for a failure this way)
        attached_ = true;
        ++g.viewsOpen;
        return kResultOk;
    }
    tresult PLUGIN_API removed() override
    {
        if (!attached_) { wrong("removed without attached"); return kResultFalse; }
        attached_ = false;
        --g.viewsOpen;
        return kResultOk;
    }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16_t, int16_t, int16_t) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16_t, int16_t, int16_t) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect* size) override
    {
        if (!size) return kInvalidArgument;
        *size = size_;
        return kResultOk;
    }
    tresult PLUGIN_API onSize(ViewRect* newSize) override
    {
        if (!newSize) return kInvalidArgument;
        size_ = *newSize;
        ++g.onSize;
        g.sizeWidth = newSize->getWidth();
        return kResultOk;
    }
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override
    {
        if (frame) frame->addRef();
        if (frame_) frame_->release();
        frame_ = frame;
        return kResultOk;
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect*) override { return kResultOk; }

    // (the user drags the window's corner)
    bool askForSize(int w, int h)
    {
        if (!frame_ || !attached_) return false;
        ViewRect r(0, 0, w, h);
        return frame_->resizeView(this, &r) == kResultOk;
    }

private:
    std::atomic<uint32_t> refs_{1};
    Plug* owner_;
    IPlugFrame* frame_ = nullptr;
    bool attached_ = false;
    ViewRect size_{0, 0, 320, 200};
};

// ------------------------------------------------------------------ an effect, a controller, or both in one
class Plug final : public IComponent, public IAudioProcessor, public IEditController, public IConnectionPoint {
public:
    explicit Plug(Kind kind) : kind_(kind)
    {
        ++g.live;
        isComponent_ = kind != kControllerOnly;
        isController_ = kind == kControllerOnly || kind == kToneKind;
        if (isController_) gControllers.push_back(this);
    }
    ~Plug()
    {
        if (started_) wrong("released without terminate");
        if (handler_) wrong("released while still holding the component handler");
        if (peer_) wrong("released while still connected");
        for (size_t k = 0; k < gControllers.size(); ++k)
            if (gControllers[k] == this) { gControllers.erase(gControllers.begin() + (std::ptrdiff_t)k); break; }
        --g.live;
    }

    // ---- FUnknown
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (iidEqual(iid, FUnknown_iid) || iidEqual(iid, IPluginBase_iid)) {
            if (isComponent_) *obj = static_cast<IPluginBase*>(static_cast<IComponent*>(this));
            else *obj = static_cast<IPluginBase*>(static_cast<IEditController*>(this));
        } else if (isComponent_ && iidEqual(iid, IComponent_iid)) *obj = static_cast<IComponent*>(this);
        else if (isComponent_ && iidEqual(iid, IAudioProcessor_iid)) *obj = static_cast<IAudioProcessor*>(this);
        else if (isController_ && iidEqual(iid, IEditController_iid)) *obj = static_cast<IEditController*>(this);
        else if ((kind_ == kGainKind || kind_ == kControllerOnly) && iidEqual(iid, IConnectionPoint_iid)) *obj = static_cast<IConnectionPoint*>(this);
        if (!*obj) return kNoInterface;
        ++refs_;
        return kResultOk;
    }
    uint32_t PLUGIN_API addRef() override { return ++refs_; }
    uint32_t PLUGIN_API release() override
    {
        const uint32_t left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    // ---- IPluginBase
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        if (started_) { wrong("initialize twice"); return kResultFalse; }
        if (!context) { wrong("initialize without a host"); return kResultFalse; }
        void* obj = nullptr;
        if (context->queryInterface(IHostApplication_iid, &obj) != kResultOk || !obj) { wrong("the host is no IHostApplication"); return kResultFalse; }
        host_ = static_cast<IHostApplication*>(obj);
        String128 name;
        std::memset(name, 0, sizeof(name));
        const char16_t expected[] = u"Reno";
        if (host_->getName(name) != kResultOk || std::memcmp(name, expected, sizeof(expected)) != 0) wrong("the host has the wrong name");
        void* nothing = (void*)this;
        TUID bogus;
        std::memcpy(bogus, kGainCid, sizeof(TUID));
        if (context->queryInterface(bogus, &nothing) == kResultOk || nothing) wrong("the host claims an interface it cannot have");
        // the host says which plug-in interfaces it uses, and is one object whichever way it is asked for
        obj = nullptr;
        if (context->queryInterface(IPlugInterfaceSupport_iid, &obj) != kResultOk || !obj) wrong("the host has no IPlugInterfaceSupport");
        else {
            IPlugInterfaceSupport* support = static_cast<IPlugInterfaceSupport*>(obj);
            if (support->isPlugInterfaceSupported(IComponent_iid) != kResultTrue || support->isPlugInterfaceSupported(IEditController_iid) != kResultTrue)
                wrong("the host says it does not use IComponent or IEditController");
            if (support->isPlugInterfaceSupported(bogus) == kResultTrue) wrong("the host says it uses an interface that does not exist");
            void* unknown = nullptr;
            void* again = nullptr;
            if (support->queryInterface(FUnknown_iid, &unknown) != kResultOk || !unknown ||
                static_cast<FUnknown*>(unknown)->queryInterface(IHostApplication_iid, &again) != kResultOk || again != (void*)host_)
                wrong("the host is not the same object when asked for in a roundabout way");
            if (again) static_cast<FUnknown*>(again)->release();
            if (unknown) static_cast<FUnknown*>(unknown)->release();
            support->release();
        }
        started_ = true;
        ++g.initialized;
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override
    {
        if (!started_) { wrong("terminate without initialize"); return kResultFalse; }
        if (active_) wrong("terminate while active");
        if (g.viewsOpen > 0 && isController_) wrong("terminate while the window is open");
        if (host_) host_->release();
        host_ = nullptr;
        started_ = false;
        ++g.terminated;
        return kResultOk;
    }

    // ---- IComponent
    int inBuses() const { return kind_ == kGainKind ? 2 : (kind_ == kMonoKind || kind_ == kSurroundKind ? 1 : 0); }
    int channels(BusDirection dir, int index) const
    {
        if (kind_ == kMonoKind) return 1;
        if (kind_ == kSurroundKind) return arr_ == SpeakerArr::k51 ? 6 : 2;
        (void)dir;
        (void)index;
        return 2;
    }

    tresult PLUGIN_API getControllerClassId(TUID classId) override
    {
        if (kind_ == kToneKind) return kNotImplemented;
        std::memcpy(classId, kControllerCid, sizeof(TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(IoMode) override { return kNotImplemented; }
    int32_t PLUGIN_API getBusCount(MediaType type, BusDirection dir) override
    {
        if (type == kAudio) return dir == kInput ? inBuses() : 1;
        return (kind_ == kToneKind && dir == kInput) ? 1 : 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32_t index, BusInfo& bus) override
    {
        if (index < 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
        std::memset(&bus, 0, sizeof(bus));
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == kAudio ? channels(dir, index) : 1;
        put16(bus.name, 128, type != kAudio ? "Notes" : (dir == kOutput ? "Output" : (index == 0 ? "Input" : "Sidechain")));
        bus.busType = index == 0 ? kMain : kAux;
        bus.flags = index == 0 ? (uint32_t)BusInfo::kDefaultActive : 0u;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32_t index, TBool state) override
    {
        if (active_) wrong("activateBus while active");
        if (index < 0 || index >= getBusCount(type, dir)) { wrong("activateBus for a bus that is not there"); return kInvalidArgument; }
        if (type == kAudio && index < 2) busOn_[dir == kInput ? 0 : 1][index] = state != 0;
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override
    {
        if (!started_) wrong("setActive before initialize");
        if ((state != 0) == active_) wrong("setActive with the state it already has");
        if (state) {
            if (!setUp_) wrong("setActive before setupProcessing");
            if (!busOn_[1][0]) wrong("the main output bus was not activated");
            if (inBuses() > 0 && !busOn_[0][0]) wrong("the main input bus was not activated");
            g.sidechainOn = busOn_[0][1] ? 1 : 0;
            lastContinuous_ = 0;
        } else if (processing_) {
            wrong("setActive(false) while still processing");
        }
        active_ = state != 0;
        return kResultOk;
    }

    // ---- state (one pair of functions serves both halves; what is written depends on which half this is)
    tresult PLUGIN_API getState(IBStream* state) override
    {
        if (!state) { wrong("getState without a stream"); return kInvalidArgument; }
        const char* magic = kind_ == kToneKind ? "RTS1" : (isComponent_ ? "RTC1" : "RTE1");
        int32_t done = 0;
        if (state->write((void*)magic, 4, &done) != kResultOk || done != 4) return kResultFalse;
        if (isComponent_) {
            double v = gain_.load();
            if (state->write(&v, sizeof(v), &done) != kResultOk) return kResultFalse;
        }
        if (isController_) {
            double v = page_;
            if (state->write(&v, sizeof(v), &done) != kResultOk) return kResultFalse;
        }
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* state) override
    {
        if (!state) { wrong("setState without a stream"); return kInvalidArgument; }
        char magic[4];
        int32_t got = 0;
        const char* expected = kind_ == kToneKind ? "RTS1" : (isComponent_ ? "RTC1" : "RTE1");
        if (state->read(magic, 4, &got) != kResultOk || got != 4 || std::memcmp(magic, expected, 4) != 0) return kResultFalse;
        double v = 0.0;
        if (isComponent_) {
            if (state->read(&v, sizeof(v), &got) != kResultOk || got != (int32_t)sizeof(v)) return kResultFalse;
            gain_ = v;
            if (isController_) shownGain_ = v / 2.0;
        }
        if (isController_) {
            if (state->read(&v, sizeof(v), &got) != kResultOk || got != (int32_t)sizeof(v)) return kResultFalse;
            page_ = v;
        }
        // reading on must give nothing, and seeking must work
        int64_t where = -1, end = -1;
        char extra = 0;
        if (state->read(&extra, 1, &got) != kResultOk || got != 0) wrong("the state stream gives more than was stored");
        if (state->tell(&where) != kResultOk || state->seek(0, IBStream::kIBSeekEnd, &end) != kResultOk || where != end) wrong("the state stream's tell/seek do not agree");
        return kResultOk;
    }

    // ---- IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32_t numIns, SpeakerArrangement* outputs, int32_t numOuts) override
    {
        if (active_) wrong("setBusArrangements while active");
        if (numIns != inBuses() || numOuts != 1) { wrong("setBusArrangements with the wrong number of buses"); return kResultFalse; }
        if ((numIns > 0 && !inputs) || !outputs) { wrong("setBusArrangements without arrangements"); return kResultFalse; }
        if (kind_ == kSurroundKind) {
            // stereo or 5.1, the same in and out; anything else is refused (and it stays as it was)
            if (inputs[0] == outputs[0] && (inputs[0] == SpeakerArr::kStereo || inputs[0] == SpeakerArr::k51)) {
                arr_ = inputs[0];
                ++g.arrangements;
                return kResultTrue;
            }
            return kResultFalse;
        }
        const SpeakerArrangement mine = kind_ == kMonoKind ? SpeakerArr::kMono : SpeakerArr::kStereo;
        if (kind_ == kGainKind && inputs[1] != SpeakerArr::kStereo) wrong("the side-chain bus was not left as it was");
        bool same = outputs[0] == mine;
        if (numIns > 0 && inputs[0] != mine) same = false;
        return same ? kResultTrue : kResultFalse;       // (this plug-in never changes what it is)
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32_t index, SpeakerArrangement& arr) override
    {
        if (index < 0 || index >= getBusCount(kAudio, dir)) return kInvalidArgument;
        arr = kind_ == kMonoKind ? SpeakerArr::kMono : (kind_ == kSurroundKind ? arr_ : SpeakerArr::kStereo);
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32_t size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32_t PLUGIN_API getLatencySamples() override { return kind_ == kMonoKind ? 7u : 0u; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override
    {
        if (active_) wrong("setupProcessing while active");
        if (setup.symbolicSampleSize != kSample32) wrong("setupProcessing: not 32-bit float");
        if (setup.processMode != kRealtime) wrong("setupProcessing: not realtime");
        if (setup.maxSamplesPerBlock <= 0 || !(setup.sampleRate > 0.0)) wrong("setupProcessing: no block size or sample rate");
        setup_ = setup;
        setUp_ = true;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override
    {
        if (state && !active_) wrong("setProcessing(true) while not active");
        processing_ = state != 0;
        return kNotImplemented;                         // (what the SDK's base class answers; a host must not mind)
    }
    tresult PLUGIN_API process(ProcessData& data) override
    {
        ++g.blocks;
        if (!active_ || !processing_) { wrong("process while not active and processing"); return kResultFalse; }
        if (data.symbolicSampleSize != kSample32) wrong("process: not 32-bit float");
        if (data.numSamples <= 0 || data.numSamples > setup_.maxSamplesPerBlock) wrong("process: more samples than the block size given");
        if (data.numSamples > g.maxBlock) g.maxBlock = data.numSamples;
        if (data.numInputs != inBuses() || data.numOutputs != 1) { wrong("process: wrong number of buses"); return kResultFalse; }
        if ((data.numInputs > 0 && !data.inputs) || !data.outputs) { wrong("process: no bus buffers"); return kResultFalse; }
        for (int b = 0; b < data.numInputs; ++b) {
            if (data.inputs[b].numChannels != channels(kInput, b)) { wrong("process: an input bus has the wrong number of channels"); return kResultFalse; }
            if (!data.inputs[b].channelBuffers32) { wrong("process: an input bus has no channel list"); return kResultFalse; }
        }
        if (data.outputs[0].numChannels != channels(kOutput, 0) || !data.outputs[0].channelBuffers32) { wrong("process: bad output bus"); return kResultFalse; }
        if (!data.processContext) wrong("process: no context");
        else {
            const ProcessContext& pc = *data.processContext;
            if (pc.sampleRate != setup_.sampleRate) wrong("process: the context has another sample rate");
            if (!(pc.state & ProcessContext::kTempoValid) || !(pc.tempo > 0.0)) wrong("process: no tempo");
            if (pc.continousTimeSamples < lastContinuous_) wrong("process: continuous time went backwards");
            lastContinuous_ = pc.continousTimeSamples;
            g.playing = (pc.state & ProcessContext::kPlaying) ? 1 : 0;
            g.position = (int)pc.projectTimeSamples;
        }
        if (data.inputEvents && data.inputEvents->getEventCount() != 0) wrong("process: events out of nowhere");

        // parameter changes
        if (IParameterChanges* changes = data.inputParameterChanges) {
            const int32_t n = changes->getParameterCount();
            for (int32_t k = 0; k < n; ++k) {
                IParamValueQueue* q = changes->getParameterData(k);
                if (!q) { wrong("process: a parameter queue is missing"); continue; }
                const int32_t count = q->getPointCount();
                if (count <= 0) { wrong("process: a parameter queue with nothing in it"); continue; }
                int32_t offset = -1;
                ParamValue value = -1.0;
                if (q->getPoint(count - 1, offset, value) != kResultOk) { wrong("process: getPoint failed"); continue; }
                if (offset < 0 || offset >= data.numSamples || value < 0.0 || value > 1.0) wrong("process: a parameter change out of range");
                ++g.points;
                if (q->getParameterId() == kGainParam) gain_ = value * 2.0;
            }
            if (changes->getParameterData(n) != nullptr) wrong("process: a parameter queue beyond the last");
        } else {
            wrong("process: no input parameter changes");
        }

        // A message sent from here must not get to the controller (sending one here is not allowed; a host
        // that passed it on would be calling the controller on the audio thread).
        if (peer_ && host_ && !triedAudioMessage_) {
            triedAudioMessage_ = true;
            TUID id;
            std::memcpy(id, IMessage_iid, sizeof(TUID));
            void* obj = nullptr;
            if (host_->createInstance(id, id, &obj) == kResultOk && obj) {
                IMessage* m = static_cast<IMessage*>(obj);
                m->setMessageID("from the audio thread");
                peer_->notify(m);
                m->release();
            }
        }

        // audio
        const float gain = (float)gain_.load();
        const int n = data.numSamples;
        float peak = 0.f;
        float** out = data.outputs[0].channelBuffers32;
        const int outCh = data.outputs[0].numChannels;
        for (int c = 0; c < outCh; ++c) {
            if (!out[c]) { wrong("process: an output channel has no buffer"); return kResultFalse; }
            const float* in = nullptr;
            if (kind_ != kToneKind) {
                in = data.inputs[0].channelBuffers32[c];
                if (!in) { wrong("process: an input channel has no buffer"); return kResultFalse; }
                if (in == out[c]) wrong("process: input and output share a buffer (the host said it keeps them apart)");
            }
            for (int k = 0; k < n; ++k) {
                const float v = (in ? in[k] : 0.25f) * gain;
                out[c][k] = v;
                if (std::fabs(v) > peak) peak = std::fabs(v);
            }
        }
        if (kind_ == kSurroundKind && outCh == 6) {
            // 5.1: the LFE also gets a quarter of the front pair (bass management) - only a plug-in that sees all six
            // channels at once can do that, so a test can tell it from three stereo instances
            const float* const* in = data.inputs[0].channelBuffers32;
            for (int k = 0; k < n; ++k) out[3][k] += 0.25f * gain * (in[0][k] + in[1][k]);
            g.surroundBlocks++;
        }
        if (kind_ == kGainKind) {
            // the side-chain bus is off: what is there, if anything, must be silence and marked as such
            const AudioBusBuffers& side = data.inputs[1];
            if ((side.silenceFlags & 3u) != 3u) wrong("process: the unused side-chain is not marked silent");
            for (int c = 0; c < 2; ++c)
                if (const float* s = side.channelBuffers32[c])
                    for (int k = 0; k < n; ++k)
                        if (s[k] != 0.f) { wrong("process: the unused side-chain is not silent"); break; }
            if (gain == 0.f) {
                // A plug-in is allowed to say "silent" - this one, badly, leaves rubbish in the buffers when it does.
                for (int c = 0; c < outCh; ++c)
                    for (int k = 0; k < n; ++k) out[c][k] = 0.5f;
                data.outputs[0].silenceFlags = 3u;
            }
        }
        if (data.outputParameterChanges) {
            int32_t index = 0;
            if (IParamValueQueue* q = data.outputParameterChanges->addParameterData(kLevelParam, index)) {
                int32_t at = 0;
                q->addPoint(0, peak > 1.f ? 1.0 : (ParamValue)peak, at);
            } else {
                wrong("process: could not add an output parameter change");
            }
        }
        return kResultOk;
    }
    uint32_t PLUGIN_API getTailSamples() override { return 0; }

    // ---- IEditController
    tresult PLUGIN_API setComponentState(IBStream* state) override
    {
        if (!state) { wrong("setComponentState without a stream"); return kInvalidArgument; }
        ++g.componentStates;
        char magic[4];
        int32_t got = 0;
        int64_t where = -1;
        double v = 0.0;
        if (state->tell(&where) != kResultOk || where != 0) wrong("setComponentState: the stream does not start at the beginning");
        if (state->read(magic, 4, &got) != kResultOk || got != 4) return kResultFalse;
        if (std::memcmp(magic, "RTC1", 4) != 0 && std::memcmp(magic, "RTS1", 4) != 0) return kResultFalse;
        if (state->read(&v, sizeof(v), &got) != kResultOk || got != (int32_t)sizeof(v)) return kResultFalse;
        shownGain_ = v / 2.0;
        return kResultOk;
    }
    int32_t PLUGIN_API getParameterCount() override { return 4; }
    tresult PLUGIN_API getParameterInfo(int32_t index, ParameterInfo& info) override
    {
        if (index < 0 || index >= 4) return kInvalidArgument;
        std::memset(&info, 0, sizeof(info));
        if (index == 0) {
            info.id = kGainParam;
            put16(info.title, 128, "Gain");
            put16(info.units, 128, "x");
            info.defaultNormalizedValue = 0.5;
            info.flags = ParameterInfo::kCanAutomate;
        } else if (index == 1) {
            info.id = kSecretParam;
            put16(info.title, 128, "Secret");
            info.flags = ParameterInfo::kIsHidden;
        } else if (index == 2) {
            info.id = kPageParam;
            put16(info.shortTitle, 128, "Page");            // (no long title: the host falls back on the short one)
            info.stepCount = 3;
        } else {
            info.id = kLevelParam;
            put16(info.title, 128, "Level");
            info.flags = ParameterInfo::kIsReadOnly;
        }
        return kResultOk;
    }
    double* shown(ParamID id)
    {
        if (id == kGainParam) return &shownGain_;
        if (id == kSecretParam) return &secret_;
        if (id == kPageParam) return &page_;
        if (id == kLevelParam) return &level_;
        return nullptr;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 string) override
    {
        char text[64];
        if (id == kGainParam) std::snprintf(text, sizeof(text), "%.2f", value * 2.0);
        else if (id == kPageParam) std::snprintf(text, sizeof(text), "Page %d", (int)std::floor(value * 3.0 + 0.5) + 1);
        else if (id == kLevelParam) return kResultFalse;        // (the host has to word this one itself)
        else return kInvalidArgument;
        put16(string, 128, text);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) override { return id == kGainParam ? v * 2.0 : v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue v) override { return id == kGainParam ? v / 2.0 : v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override
    {
        const double* v = shown(id);
        return v ? *v : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
    {
        double* v = shown(id);
        if (!v) return kInvalidArgument;
        *v = value;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override
    {
        if (handler) handler->addRef();
        if (handler && !handler_) ++g.handlers;
        if (!handler && handler_) --g.handlers;
        if (handler_) handler_->release();
        handler_ = handler;
        if (handler) {
            void* obj = (void*)this;
            if (handler->queryInterface(IPlugFrame_iid, &obj) == kResultOk || obj) wrong("the component handler claims to be something else as well");
        }
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override
    {
        if (!name || std::strcmp(name, "editor") != 0 || kind_ == kToneKind) return nullptr;
        return new View(this);
    }

    // ---- IConnectionPoint
    tresult PLUGIN_API connect(IConnectionPoint* other) override
    {
        if (!other) { wrong("connect to nothing"); return kInvalidArgument; }
        if (peer_) wrong("connect twice");
        if (!started_) wrong("connect before initialize");
        other->addRef();
        peer_ = other;
        ++g.connects;
        if (!isComponent_ && host_) sayHello();
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint* other) override
    {
        if (!peer_ || other != peer_) { wrong("disconnect from somebody else"); return kResultFalse; }
        peer_->release();
        peer_ = nullptr;
        ++g.disconnects;
        return kResultOk;
    }
    tresult PLUGIN_API notify(IMessage* message) override
    {
        if (!message) return kInvalidArgument;
        FIDString id = message->getMessageID();
        IAttributeList* list = message->getAttributes();
        if (id && !std::strcmp(id, "from the audio thread")) { wrong("a message sent from inside process was passed on"); return kResultFalse; }
        if (!id || std::strcmp(id, "hello") != 0 || !list) return kResultFalse;
        int64_t number = 0;
        double real = 0.0;
        TChar text[8];
        TChar tiny[2] = {u'?', u'?'};
        const void* bytes = nullptr;
        uint32_t size = 0;
        const char16_t expected[] = u"Reno";
        bool ok = list->getInt("answer", number) == kResultOk && number == 42;
        ok = ok && list->getFloat("half", real) == kResultOk && real == 0.5;
        ok = ok && list->getString("name", text, sizeof(text)) == kResultOk && std::memcmp(text, expected, sizeof(expected)) == 0;
        ok = ok && list->getString("name", tiny, sizeof(tiny)) == kResultOk && tiny[0] == u'R' && tiny[1] == 0;     // cut short, still ended
        ok = ok && list->getBinary("blob", bytes, size) == kResultOk && size == 3 && bytes && std::memcmp(bytes, "\x01\x02\x03", 3) == 0;
        ok = ok && list->getInt("name", number) != kResultOk;       // (a text is not a number)
        ok = ok && list->getInt("nothing", number) != kResultOk;
        if (ok) ++g.messages;
        else wrong("a message did not arrive whole");
        return kResultOk;
    }

    // The controller greets the component with a message made by the host.
    void sayHello()
    {
        TUID id;
        std::memcpy(id, IMessage_iid, sizeof(TUID));
        void* obj = nullptr;
        if (host_->createInstance(id, id, &obj) != kResultOk || !obj) { wrong("the host cannot make a message"); return; }
        IMessage* m = static_cast<IMessage*>(obj);
        if (m->getMessageID() != nullptr) wrong("a new message already has an id");
        m->setMessageID("hello");
        IAttributeList* list = m->getAttributes();
        if (!list) wrong("a message without attributes");
        else {
            list->setInt("answer", 41);
            list->setInt("answer", 42);
            list->setFloat("half", 0.5);
            list->setString("name", u"Reno");
            list->setBinary("blob", "\x01\x02\x03", 3);
        }
        peer_->notify(m);
        if (m->release() != 0) wrong("somebody kept the message");
    }

    // (the user moves the gain knob in the window)
    bool userMovesGain(double value)
    {
        if (!handler_) return false;
        shownGain_ = value;
        bool ok = handler_->beginEdit(kGainParam) == kResultOk;
        ok = handler_->performEdit(kGainParam, value) == kResultOk && ok;
        ok = handler_->endEdit(kGainParam) == kResultOk && ok;
        return ok;
    }
    // (the user picks a preset in the window: several values change at once)
    bool userPicksPreset(double value)
    {
        if (!handler_) return false;
        shownGain_ = value;
        page_ = value;
        return handler_->restartComponent(kParamValuesChanged) == kResultOk;
    }

private:
    std::atomic<uint32_t> refs_{1};
    Kind kind_;
    SpeakerArrangement arr_ = SpeakerArr::kStereo;     // (the surround kind: what its buses are now)
    bool isComponent_ = false, isController_ = false;
    bool started_ = false, setUp_ = false, active_ = false, processing_ = false, triedAudioMessage_ = false;
    bool busOn_[2][2] = {{false, false}, {false, false}};
    IHostApplication* host_ = nullptr;
    IConnectionPoint* peer_ = nullptr;
    IComponentHandler* handler_ = nullptr;
    ProcessSetup setup_{};
    int64_t lastContinuous_ = 0;
    std::atomic<double> gain_{1.0};         // the processor's
    double shownGain_ = 0.5;                // the controller's (0..1)
    double secret_ = 0.0, page_ = 0.0, level_ = 0.0;
};

View::View(Plug* owner) : owner_(owner)
{
    ++g.live;
    owner_->addRef();
    gLastView = this;
}

View::~View()
{
    if (attached_) wrong("the window was released while attached");
    if (frame_) wrong("the window was released while it still had its frame");
    if (gLastView == this) gLastView = nullptr;
    owner_->release();
    --g.live;
}

// ------------------------------------------------------------------ the factory
int factoryLevel()
{
    const char* e = std::getenv("RENO_TESTPLUG_FACTORY");
    const int level = e ? std::atoi(e) : 3;
    return level < 1 || level > 3 ? 3 : level;
}

struct ClassRow {
    const char* cid;
    const char* category;
    const char* name;
    const char16_t* wideName;
    const char* subCategories;
    const char* vendor;
    Kind kind;
};
const ClassRow kClasses[] = {
    {kGainCid, "Audio Module Class", "Reno Test Gain", u"Reno Test Gain", "Fx", "", kGainKind},
    {kControllerCid, "Component Controller Class", "Reno Test Controller", u"Reno Test Controller", "", "", kControllerOnly},
    {kMonoCid, "Audio Module Class", "Reno Test Mono", u"Reno Test Mono", "Fx|Mono", "Mono Maker", kMonoKind},
    {kToneCid, "Audio Module Class", "Reno Test Tone", u"Reno Test Töne \U0001F3B5", "Instrument|Synth", "", kToneKind},
    // (only with RENO_TESTPLUG_SURROUND=1: a stereo or 5.1 gain, for the 5.1 master's six-channel plug-ins)
    {kSurroundCid, "Audio Module Class", "Reno Test Surround", u"Reno Test Surround", "Fx|Surround", "", kSurroundKind},
};
int classCountNow()
{
    const char* e = std::getenv("RENO_TESTPLUG_SURROUND");
    const int all = (int)(sizeof(kClasses) / sizeof(kClasses[0]));
    return (e && *e == '1') ? all : all - 1;
}
#define kClassCount classCountNow()

class Factory final : public IPluginFactory3 {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        const int level = factoryLevel();
        if (iidEqual(iid, FUnknown_iid) || iidEqual(iid, IPluginFactory_iid) || (level >= 2 && iidEqual(iid, IPluginFactory2_iid)) ||
            (level >= 3 && iidEqual(iid, IPluginFactory3_iid))) {
            *obj = static_cast<IPluginFactory3*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32_t PLUGIN_API addRef() override { return 2; }
    uint32_t PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override
    {
        if (!info) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        put8(info->vendor, sizeof(info->vendor), "Reno Tests");
        put8(info->url, sizeof(info->url), "https://example.invalid/reno");
        put8(info->email, sizeof(info->email), "mailto:nobody@example.invalid");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32_t PLUGIN_API countClasses() override { return kClassCount; }
    tresult PLUGIN_API getClassInfo(int32_t index, PClassInfo* info) override
    {
        if (index < 0 || index >= kClassCount || !info) return kInvalidArgument;
        const ClassRow& row = kClasses[index];
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, row.cid, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        put8(info->category, sizeof(info->category), row.category);
        put8(info->name, sizeof(info->name), row.name);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (!cid || !iid) return kInvalidArgument;
        for (const ClassRow& row : kClasses) {
            if (!iidEqual(cid, row.cid)) continue;
            Plug* p = new Plug(row.kind);
            IPluginBase* base = row.kind == kControllerOnly ? static_cast<IPluginBase*>(static_cast<IEditController*>(p))
                                                            : static_cast<IPluginBase*>(static_cast<IComponent*>(p));
            const tresult r = base->queryInterface(iid, obj);
            base->release();
            return r;
        }
        return kNoInterface;
    }
    tresult PLUGIN_API getClassInfo2(int32_t index, PClassInfo2* info) override
    {
        if (index < 0 || index >= kClassCount || !info) return kInvalidArgument;
        const ClassRow& row = kClasses[index];
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, row.cid, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        put8(info->category, sizeof(info->category), row.category);
        put8(info->name, sizeof(info->name), row.name);
        put8(info->subCategories, sizeof(info->subCategories), row.subCategories);
        put8(info->vendor, sizeof(info->vendor), row.vendor);
        put8(info->version, sizeof(info->version), "1.0.0");
        put8(info->sdkVersion, sizeof(info->sdkVersion), "VST 3.8.1");
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfoUnicode(int32_t index, PClassInfoW* info) override
    {
        if (index < 0 || index >= kClassCount || !info) return kInvalidArgument;
        const ClassRow& row = kClasses[index];
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, row.cid, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        put8(info->category, sizeof(info->category), row.category);
        size_t n = 0;
        while (row.wideName[n] && n + 1 < sizeof(info->name) / sizeof(info->name[0])) { info->name[n] = row.wideName[n]; ++n; }
        put8(info->subCategories, sizeof(info->subCategories), row.subCategories);
        put16(info->vendor, sizeof(info->vendor) / sizeof(info->vendor[0]), row.vendor);
        put16(info->version, sizeof(info->version) / sizeof(info->version[0]), "1.0.0");
        put16(info->sdkVersion, sizeof(info->sdkVersion) / sizeof(info->sdkVersion[0]), "VST 3.8.1");
        return kResultOk;
    }
    tresult PLUGIN_API setHostContext(FUnknown* context) override
    {
        if (!context) wrong("setHostContext without a host");
        ++g.hostContexts;
        return kResultOk;
    }
};

Factory gFactory;

}  // namespace

// ------------------------------------------------------------------ what the file exports
extern "C" {

TEST_EXPORT IPluginFactory* PLUGIN_API GetPluginFactory()
{
    if (g.starts == 0) wrong("GetPluginFactory before the start-up function");
    ++g.factories;
    return &gFactory;
}

#if defined(_WIN32)
TEST_EXPORT bool PLUGIN_API InitDll()
{
    ++g.starts;
    return true;
}
TEST_EXPORT bool PLUGIN_API ExitDll()
{
    --g.starts;
    return true;
}
#else
TEST_EXPORT bool ModuleEntry(void* handle)
{
    if (!handle) wrong("ModuleEntry without the library handle");
    ++g.starts;
    return true;
}
TEST_EXPORT bool ModuleExit()
{
    --g.starts;
    return true;
}
#endif

// For the test program: one of the counts by name; -1 for a name there is no count for.
TEST_EXPORT int RenoTestPlugGet(const char* what)
{
    struct Row {
        const char* name;
        std::atomic<int>* value;
    };
    const Row rows[] = {
        {"live", &g.live}, {"starts", &g.starts}, {"factories", &g.factories}, {"hostContexts", &g.hostContexts},
        {"initialized", &g.initialized}, {"terminated", &g.terminated}, {"connects", &g.connects}, {"disconnects", &g.disconnects},
        {"messages", &g.messages}, {"componentStates", &g.componentStates}, {"errors", &g.errors}, {"blocks", &g.blocks},
        {"points", &g.points}, {"playing", &g.playing}, {"position", &g.position}, {"maxBlock", &g.maxBlock},
        {"sidechainOn", &g.sidechainOn}, {"viewsOpen", &g.viewsOpen}, {"onSize", &g.onSize}, {"arrangements", &g.arrangements}, {"surroundBlocks", &g.surroundBlocks}, {"sizeWidth", &g.sizeWidth},
        {"handlers", &g.handlers},
    };
    for (const Row& row : rows)
        if (what && !std::strcmp(what, row.name)) return row.value->load();
    return -1;
}

// For the test program: act like a user in the window of the newest controller. 1 if it went through.
TEST_EXPORT int RenoTestPlugPoke(const char* what, double value)
{
    if (!what) return 0;
    Plug* controller = gControllers.empty() ? nullptr : gControllers.back();
    if (!std::strcmp(what, "gain")) return (controller && controller->userMovesGain(value)) ? 1 : 0;
    if (!std::strcmp(what, "preset")) return (controller && controller->userPicksPreset(value)) ? 1 : 0;
    if (!std::strcmp(what, "resize")) return (gLastView && gLastView->askForSize((int)value, 240)) ? 1 : 0;
    return 0;
}

}  // extern "C"
