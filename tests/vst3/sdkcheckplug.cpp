// A VST3 plug-in made with Steinberg's own SDK classes that watches what its host does to it.
// THIS FILE NEEDS THE STEINBERG VST3 SDK TO BUILD (like abicheck.cpp; see README.txt). Nothing in Reno depends on it.
//
// Reno's VST3 host is written against Reno's own declarations of the VST3 interfaces. This plug-in is the other side,
// built from the official ones: the SDK's AudioEffect and EditController classes, and the checks from Steinberg's
// "HostChecker" sample (public.sdk/samples/vst/hostchecker/source/hostcheck.cpp and friends), which look at every block
// the host hands over (bus buffers, process context, parameter changes, event list) plus the same bookkeeping of the
// order of calls as that sample's processor. So when Reno's host runs it, Steinberg's code reads Reno's structs and
// calls Reno's objects.
//
// The effect is a stereo gain (parameter "Gain", 0..1 is a gain of 0..2) with a "Bypass" switch, a read-only "Level"
// the processor reports (the loudest sample so far), a stereo side-chain input that is off by default, and a controller
// of its own. When an effect is terminated it prints what it noted, one line each:
//     SDKCHECK Error|Warn|Info  <Steinberg's text>  x<how often>
// and a last line "SDKCHECK done: <errors> error(s), <warnings> warning(s)".
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "base/source/fstreamer.h"
#include "hostcheck.h"                      // (Steinberg's, from the HostChecker sample)
#include <cstdio>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID kProcessorUID(0x52454E4F, 0x53444B43, 0x00000001, 0x50524F43);
const FUID kControllerUID(0x52454E4F, 0x53444B43, 0x00000002, 0x4354524C);
enum : ParamID { kGainId = 10, kBypassId = 11, kLevelId = 12 };

// ------------------------------------------------------------------ the half that does the audio
class CheckProcessor : public AudioEffect {
public:
    CheckProcessor() { setControllerClass(kControllerUID); }
    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new CheckProcessor()); }

    void note(int32 id) { check.getEventLogger().addLogEvent(id); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE
    {
        const tresult result = AudioEffect::initialize(context);
        if (result != kResultOk) return result;
        if (state != State::kUninitialized) note(kLogIdInvalidStateInitializedMissing);
        state = State::kInitialized;
        addAudioInput(STR16("Audio Input"), SpeakerArr::kStereo);
        addAudioInput(STR16("Aux Input"), SpeakerArr::kStereo, kAux, 0);
        addAudioOutput(STR16("Audio Output"), SpeakerArr::kStereo);
        addEventInput(STR16("Event Input"), 1);
        check.setComponent(this);
        check.addParameter(kGainId);
        check.addParameter(kBypassId);

        if (auto support = U::cast<IPlugInterfaceSupport>(context)) {
            note(kLogIdIPlugInterfaceSupportSupported);
            if (support->isPlugInterfaceSupported(IComponent::iid) != kResultTrue || support->isPlugInterfaceSupported(IAudioProcessor::iid) != kResultTrue)
                std::printf("SDKCHECK Error  the host's IPlugInterfaceSupport does not know IComponent / IAudioProcessor\n");
        } else {
            note(kLogIdIPlugInterfaceSupportNotSupported);
        }
        // the host context has to be one object whichever interface it is asked for through
        if (auto app = U::cast<IHostApplication>(context)) {
            String128 name = {};
            if (app->getName(name) != kResultOk || name[0] == 0) std::printf("SDKCHECK Error  IHostApplication::getName gave nothing\n");
            if (auto unknown = U::cast<FUnknown>(app)) {
                if (!U::cast<IHostApplication>(unknown)) note(kLogWrongCOMBehaviorFUnknown1);
            } else {
                note(kLogWrongCOMBehaviorFUnknown2);
            }
        } else {
            std::printf("SDKCHECK Error  the host context is no IHostApplication\n");
        }
        return kResultOk;
    }

    tresult PLUGIN_API terminate() SMTG_OVERRIDE
    {
        if (state != State::kInitialized && state != State::kSetupDone) std::printf("SDKCHECK Error  terminate called while still active\n");
        state = State::kUninitialized;
        report();
        return AudioEffect::terminate();
    }

    tresult PLUGIN_API connect(IConnectionPoint* other) SMTG_OVERRIDE
    {
        // (a host is advised to put something of its own between the two halves)
        if (U::cast<IEditController>(other)) note(kLogIdProcessorControllerConnection);
        return AudioEffect::connect(other);
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) SMTG_OVERRIDE
    {
        if (state != State::kInitialized && state != State::kSetupDone) note(kLogIdInvalidStateInitializedMissing);
        state = State::kSetupDone;
        check.setProcessSetup(setup);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool on) SMTG_OVERRIDE
    {
        if (state == State::kProcessing) note(kLogIdInvalidStateSetActiveWrong);
        checkLatencyCall = true;
        if (!on) {
            if (state == State::kSetupDone) note(kLogIdsetActiveFalseRedundant);
            state = State::kSetupDone;
            latencyAskedAfterActive = false;
        } else {
            setActiveCalled = true;
            if (state == State::kActivated) note(kLogIdsetActiveTrueRedundant);
            else if (state != State::kSetupDone) note(kLogIdInvalidStateSetupMissing);
            state = State::kActivated;
        }
        return AudioEffect::setActive(on);
    }

    tresult PLUGIN_API setProcessing(TBool on) SMTG_OVERRIDE
    {
        if (on) {
            if (state != State::kActivated) note(kLogIdInvalidStateSetProcessingWrong);
            if (state == State::kProcessing) note(kLogIdsetProcessingTrueRedundant);
            state = State::kProcessing;
        } else {
            if (state != State::kProcessing) note(kLogIdsetProcessingFalseRedundant);
            state = State::kActivated;
        }
        return kResultOk;
    }

    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE
    {
        checkLatencyCall = true;
        latencyAsked = true;
        if (setActiveCalled) latencyAskedAfterActive = true;
        note(kLogIdGetLatencySamples);
        return 0;
    }

    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool on) SMTG_OVERRIDE
    {
        if (type == kAudio && dir == kInput) {
            if (index < 0 || index >= static_cast<int32>(getBusList(kAudio, kInput)->size())) note(kLogIdInvalidActivateAuxBus);
            else if (index > 0 && on) note(kLogIdActivateAuxBus);
        }
        const tresult result = AudioEffect::activateBus(type, dir, index, on);
        if (result == kResultTrue && type == kAudio) {
            if (auto list = getBusList(type, dir)) {
                int32 lastActive = -1;
                for (int32 k = static_cast<int32>(list->size()) - 1; k >= 0; --k)
                    if (list->at(k)->isActive()) { lastActive = k; break; }
                (dir == kInput ? minInputs : minOutputs) = lastActive + 1;
            }
        }
        return result;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE
    {
        note(kLogIdSetBusArrangements);
        if (state != State::kInitialized && state != State::kSetupDone) std::printf("SDKCHECK Error  setBusArrangements called while active\n");
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE
    {
        check.validate(data, minInputs, minOutputs);             // Steinberg's look at everything in 'data'
        if (state != State::kProcessing) note(kLogIdInvalidStateProcessingMissing);
        if (setActiveCalled) {
            setActiveCalled = false;
            note(kLogIdSetActiveCalledSupported);
        }
        if (checkLatencyCall) {
            checkLatencyCall = false;
            if (latencyAsked) {
                if (!latencyAskedAfterActive) note(kLogIdGetLatencyCalledbeforeSetActive);
            } else {
                note(kLogIdgetLatencyNotCalled);
            }
        }
        if (data.processMode == kRealtime) note(kLogIdProcessModeRealtimeSupported);
        if (data.processContext) {
            const uint32 flags = data.processContext->state;
            if (flags & ProcessContext::kPlaying) note(kLogIdProcessContextPlayingSupported);
            if (flags & ProcessContext::kContTimeValid) note(kLogIdProcessContextContTimeSupported);
            if (flags & ProcessContext::kProjectTimeMusicValid) note(kLogIdProcessContextTimeMusicSupported);
            if (flags & ProcessContext::kBarPositionValid) note(kLogIdProcessContextBarPositionSupported);
            if (flags & ProcessContext::kTempoValid) note(kLogIdProcessContextTempoSupported);
            if (flags & ProcessContext::kTimeSigValid) note(kLogIdProcessContextTimeSigSupported);
        }

        if (IParameterChanges* changes = data.inputParameterChanges) {
            const int32 count = changes->getParameterCount();
            for (int32 k = 0; k < count; ++k) {
                IParamValueQueue* queue = changes->getParameterData(k);
                if (!queue) continue;
                ParamValue value = 0.0;
                int32 offset = 0;
                if (queue->getPoint(queue->getPointCount() - 1, offset, value) != kResultTrue) continue;
                if (queue->getParameterId() == kGainId) gain = value * 2.0;
                else if (queue->getParameterId() == kBypassId) bypass = value > 0.5;
            }
        }
        if (data.numSamples <= 0 || data.numOutputs < 1 || !data.outputs) return kResultOk;
        if (data.numInputs > 1 && data.inputs[1].silenceFlags != 0) note(kLogIdSilentFlagsSCSupported);
        const int32 channels = data.outputs[0].numChannels;
        double peak = 0.0;
        for (int32 c = 0; c < channels; ++c) {
            Sample32* out = data.outputs[0].channelBuffers32[c];
            const Sample32* in = (data.numInputs > 0 && c < data.inputs[0].numChannels) ? data.inputs[0].channelBuffers32[c] : nullptr;
            if (!out) continue;
            for (int32 k = 0; k < data.numSamples; ++k) {
                out[k] = in ? static_cast<Sample32>(in[k] * (bypass ? 1.0 : gain)) : 0.f;
                if (out[k] > peak) peak = out[k];
                if (-out[k] > peak) peak = -out[k];
            }
        }
        // how loud it was at its loudest so far, for the controller (a value going out of the processor)
        if (peak > loudest) {
            loudest = peak;
            if (data.outputParameterChanges) {
                int32 index = 0;
                if (IParamValueQueue* queue = data.outputParameterChanges->addParameterData(kLevelId, index)) {
                    int32 at = 0;
                    if (queue->addPoint(0, loudest > 1.0 ? 1.0 : loudest, at) != kResultOk) std::printf("SDKCHECK Error  the host's IParamValueQueue::addPoint failed\n");
                } else {
                    std::printf("SDKCHECK Error  the host's IParameterChanges::addParameterData gave no queue\n");
                }
            }
        }
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* stream) SMTG_OVERRIDE
    {
        if (!stream) return kInvalidArgument;
        IBStreamer s(stream, kLittleEndian);
        double g = 1.0;
        int32 b = 0;
        if (!s.readDouble(g) || !s.readInt32(b)) return kResultFalse;
        gain = g;
        bypass = b != 0;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* stream) SMTG_OVERRIDE
    {
        if (!stream) return kInvalidArgument;
        IBStreamer s(stream, kLittleEndian);
        s.writeDouble(gain);
        s.writeInt32(bypass ? 1 : 0);
        return kResultOk;
    }

    // What Steinberg's checks and the bookkeeping above noted. "Info" lines only say what the host makes use of.
    void report()
    {
        int errors = 0, warnings = 0;
        for (const LogEvent& e : check.getEventLogs()) {
            if (e.count <= 0 || e.id < 0 || e.id >= kNumLogEvents) continue;
            const char* severity = logEventSeverity[e.id];
            if (!std::strcmp(severity, LOG_ERR)) ++errors;
            else if (!std::strcmp(severity, LOG_WARN)) ++warnings;
            std::printf("SDKCHECK %-5s %s  x%lld\n", severity, logEventDescriptions[e.id], static_cast<long long>(e.count));
        }
        std::printf("SDKCHECK done: %d error(s), %d warning(s)\n", errors, warnings);
        std::fflush(stdout);
    }

private:
    enum class State { kUninitialized, kInitialized, kSetupDone, kActivated, kProcessing };
    State state = State::kUninitialized;
    HostCheck check;
    int32 minInputs = 0, minOutputs = 0;
    bool setActiveCalled = false, checkLatencyCall = false, latencyAsked = false, latencyAskedAfterActive = false;
    double gain = 1.0, loudest = 0.0;
    bool bypass = false;
};

// ------------------------------------------------------------------ the half with the parameters
class CheckController : public EditController {
public:
    static FUnknown* createInstance(void*) { return static_cast<IEditController*>(new CheckController()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE
    {
        const tresult result = EditController::initialize(context);
        if (result != kResultOk) return result;
        parameters.addParameter(STR16("Gain"), STR16("x"), 0, 0.5, ParameterInfo::kCanAutomate, kGainId);
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, kBypassId);
        parameters.addParameter(STR16("Level"), nullptr, 0, 0.0, ParameterInfo::kIsReadOnly, kLevelId);
        return kResultOk;
    }

    tresult PLUGIN_API setComponentState(IBStream* stream) SMTG_OVERRIDE
    {
        if (!stream) return kInvalidArgument;
        IBStreamer s(stream, kLittleEndian);
        double g = 1.0;
        int32 b = 0;
        if (!s.readDouble(g) || !s.readInt32(b)) return kResultFalse;
        setParamNormalized(kGainId, g / 2.0);
        setParamNormalized(kBypassId, b ? 1.0 : 0.0);
        return kResultOk;
    }

    tresult PLUGIN_API connect(IConnectionPoint* other) SMTG_OVERRIDE
    {
        if (U::cast<IAudioProcessor>(other)) std::printf("SDKCHECK Warn  the controller is connected straight to the processor\n");
        // a message for the processor, made by the host
        if (IPtr<IMessage> message = owned(allocateMessage())) {
            message->setMessageID("hello");
            if (IAttributeList* attributes = message->getAttributes()) {
                attributes->setInt("number", 7);
                int64 back = 0;
                if (attributes->getInt("number", back) != kResultOk || back != 7) std::printf("SDKCHECK Error  the host's IAttributeList does not give back what was put in\n");
                String128 text = {};
                attributes->setString("text", STR16("Reno"));
                if (attributes->getString("text", text, sizeof(text)) != kResultOk || text[0] != u'R' || text[3] != u'o' || text[4] != 0)
                    std::printf("SDKCHECK Error  the host's IAttributeList does not give back the text that was put in\n");
            } else {
                std::printf("SDKCHECK Error  the host's IMessage has no attribute list\n");
            }
        } else {
            std::printf("SDKCHECK Error  the host cannot make an IMessage\n");
        }
        return EditController::connect(other);
    }
};

}  // namespace

BEGIN_FACTORY_DEF("Reno Tests", "https://example.invalid/reno", "mailto:nobody@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(kProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "Reno SDK Check", Vst::kDistributable, "Fx", "1.0.0",
           kVstVersionString, CheckProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(kControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass, "Reno SDK Check Controller", 0, "", "1.0.0",
           kVstVersionString, CheckController::createInstance)

END_FACTORY
