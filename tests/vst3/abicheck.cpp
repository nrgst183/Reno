// Compares src/audiofx/vst3_abi.h with the official VST3 interface headers, mechanically.
// This file is the only one that needs the Steinberg headers; they are not part of Reno. See README.txt.
//
// What is compared:
//   - the 16 bytes of every interface id                       (when compiling, static_assert)
//   - every result code and every constant the host uses       (when compiling)
//   - size and alignment of every struct, offset and size of every member    (when compiling)
//   - the parameter and return types of every virtual function (when compiling)
//   - the position of every virtual function in its table, and the number of virtual functions of
//     every interface                                          (when run; only on platforms with the
//     usual Unix C++ object layout, where a pointer to a virtual member function holds its position)
// On Windows the last point cannot be read out of a running program in a portable way; there the
// tables the compiler lays out are compared with compare_vtables.py instead.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"

// Two names are macros in the official headers and constants in ours.
static const char* const kSdkAudioEffectClass = kVstAudioEffectClass;
static const char* const kSdkControllerClass = kVstComponentControllerClass;
#undef kVstAudioEffectClass
#undef kVstComponentControllerClass

#include "vst3_abi.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace S = Steinberg;
namespace SV = Steinberg::Vst;
namespace R = reno::vst3;

// ------------------------------------------------------------------ types made comparable
// Canon<T> rewrites a type so that the two sides can be compared with is_same: our structs and
// interfaces become the official ones, and integers become "an integer of this size and sign" (int64_t
// is not the same type as long long everywhere, but it is passed the same way).
template <int Bytes, bool Signed> struct IntOf;
template <class T> struct IsPlainInt : std::integral_constant<bool,
    std::is_integral<T>::value && !std::is_const<T>::value && !std::is_same<T, bool>::value && !std::is_same<T, char>::value &&
    !std::is_same<T, char16_t>::value && !std::is_same<T, char32_t>::value && !std::is_same<T, wchar_t>::value> {};

template <class T, class = void> struct Canon { typedef T type; };
template <class T> struct Canon<T, typename std::enable_if<IsPlainInt<T>::value>::type> { typedef IntOf<(int)sizeof(T), std::is_signed<T>::value> type; };
template <class T> struct Canon<T*, void> { typedef typename Canon<T>::type* type; };
template <class T> struct Canon<T&, void> { typedef typename Canon<T>::type& type; };
template <class T> struct Canon<const T, void> { typedef const typename Canon<T>::type type; };
template <class Ret, class C, class... A> struct Canon<Ret (PLUGIN_API C::*)(A...), void> {
    typedef typename Canon<Ret>::type (PLUGIN_API Canon<C>::type::*type)(typename Canon<A>::type...);
};
#define SAME_AS(Ours, Sdk) template <> struct Canon<Ours, void> { typedef Sdk type; };
SAME_AS(R::FUnknown, S::FUnknown)
SAME_AS(R::IPluginBase, S::IPluginBase)
SAME_AS(R::IPluginFactory, S::IPluginFactory)
SAME_AS(R::IPluginFactory2, S::IPluginFactory2)
SAME_AS(R::IPluginFactory3, S::IPluginFactory3)
SAME_AS(R::PFactoryInfo, S::PFactoryInfo)
SAME_AS(R::PClassInfo, S::PClassInfo)
SAME_AS(R::PClassInfo2, S::PClassInfo2)
SAME_AS(R::PClassInfoW, S::PClassInfoW)
SAME_AS(R::IBStream, S::IBStream)
SAME_AS(R::ViewRect, S::ViewRect)
SAME_AS(R::IPlugView, S::IPlugView)
SAME_AS(R::IPlugFrame, S::IPlugFrame)
SAME_AS(R::BusInfo, SV::BusInfo)
SAME_AS(R::RoutingInfo, SV::RoutingInfo)
SAME_AS(R::IComponent, SV::IComponent)
SAME_AS(R::ProcessSetup, SV::ProcessSetup)
SAME_AS(R::AudioBusBuffers, SV::AudioBusBuffers)
SAME_AS(R::ProcessData, SV::ProcessData)
SAME_AS(R::ProcessContext, SV::ProcessContext)
SAME_AS(R::IAudioProcessor, SV::IAudioProcessor)
SAME_AS(R::IParamValueQueue, SV::IParamValueQueue)
SAME_AS(R::IParameterChanges, SV::IParameterChanges)
SAME_AS(R::Event, SV::Event)
SAME_AS(R::IEventList, SV::IEventList)
SAME_AS(R::ParameterInfo, SV::ParameterInfo)
SAME_AS(R::IComponentHandler, SV::IComponentHandler)
SAME_AS(R::IEditController, SV::IEditController)
SAME_AS(R::IAttributeList, SV::IAttributeList)
SAME_AS(R::IMessage, SV::IMessage)
SAME_AS(R::IConnectionPoint, SV::IConnectionPoint)
SAME_AS(R::IHostApplication, SV::IHostApplication)
SAME_AS(R::IPlugInterfaceSupport, SV::IPlugInterfaceSupport)

// ------------------------------------------------------------------ ids, compared while compiling
constexpr bool sameId(const char (&a)[16], const char (&b)[16])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] && a[5] == b[5] && a[6] == b[6] && a[7] == b[7] &&
           a[8] == b[8] && a[9] == b[9] && a[10] == b[10] && a[11] == b[11] && a[12] == b[12] && a[13] == b[13] && a[14] == b[14] && a[15] == b[15];
}
#define CHECK_ID(SdkNs, Name) static_assert(sameId(R::Name##_iid, SdkNs::Name##_iid), "interface id of " #Name);
CHECK_ID(S, FUnknown)
CHECK_ID(S, IPluginBase)
CHECK_ID(S, IPluginFactory)
CHECK_ID(S, IPluginFactory2)
CHECK_ID(S, IPluginFactory3)
CHECK_ID(S, IBStream)
CHECK_ID(S, IPlugView)
CHECK_ID(S, IPlugFrame)
CHECK_ID(SV, IComponent)
CHECK_ID(SV, IAudioProcessor)
CHECK_ID(SV, IParamValueQueue)
CHECK_ID(SV, IParameterChanges)
CHECK_ID(SV, IEventList)
CHECK_ID(SV, IComponentHandler)
CHECK_ID(SV, IEditController)
CHECK_ID(SV, IAttributeList)
CHECK_ID(SV, IMessage)
CHECK_ID(SV, IConnectionPoint)
CHECK_ID(SV, IHostApplication)
CHECK_ID(SV, IPlugInterfaceSupport)

// ------------------------------------------------------------------ basic types and constants
#define CHECK_TYPE(Ours, Sdk) static_assert(std::is_same<Canon<Ours>::type, Canon<Sdk>::type>::value, "type " #Ours);
CHECK_TYPE(R::TUID, S::TUID)
CHECK_TYPE(R::tresult, S::tresult)
CHECK_TYPE(R::TBool, S::TBool)
CHECK_TYPE(R::FIDString, S::FIDString)
CHECK_TYPE(R::TChar, SV::TChar)
CHECK_TYPE(R::String128, SV::String128)
CHECK_TYPE(R::MediaType, SV::MediaType)
CHECK_TYPE(R::BusDirection, SV::BusDirection)
CHECK_TYPE(R::BusType, SV::BusType)
CHECK_TYPE(R::IoMode, SV::IoMode)
CHECK_TYPE(R::UnitID, SV::UnitID)
CHECK_TYPE(R::ParamID, SV::ParamID)
CHECK_TYPE(R::ParamValue, SV::ParamValue)
CHECK_TYPE(R::SampleRate, SV::SampleRate)
CHECK_TYPE(R::Sample32, SV::Sample32)
CHECK_TYPE(R::Sample64, SV::Sample64)
CHECK_TYPE(R::TSamples, SV::TSamples)
CHECK_TYPE(R::TQuarterNotes, SV::TQuarterNotes)
CHECK_TYPE(R::Speaker, SV::Speaker)
CHECK_TYPE(R::SpeakerArrangement, SV::SpeakerArrangement)
CHECK_TYPE(R::IAttributeList::AttrID, SV::IAttributeList::AttrID)

#define CHECK_VALUE(Ours, Sdk) static_assert((long long)(Ours) == (long long)(Sdk), "value of " #Ours);
CHECK_VALUE(R::kNoInterface, S::kNoInterface)
CHECK_VALUE(R::kResultOk, S::kResultOk)
CHECK_VALUE(R::kResultTrue, S::kResultTrue)
CHECK_VALUE(R::kResultFalse, S::kResultFalse)
CHECK_VALUE(R::kInvalidArgument, S::kInvalidArgument)
CHECK_VALUE(R::kNotImplemented, S::kNotImplemented)
CHECK_VALUE(R::kInternalError, S::kInternalError)
CHECK_VALUE(R::kNotInitialized, S::kNotInitialized)
CHECK_VALUE(R::kOutOfMemory, S::kOutOfMemory)
CHECK_VALUE(RENO_VST3_COM_STYLE, COM_COMPATIBLE)
CHECK_VALUE(R::PFactoryInfo::kUnicode, S::PFactoryInfo::kUnicode)
CHECK_VALUE(R::PFactoryInfo::kClassesDiscardable, S::PFactoryInfo::kClassesDiscardable)
CHECK_VALUE(R::PFactoryInfo::kComponentNonDiscardable, S::PFactoryInfo::kComponentNonDiscardable)
CHECK_VALUE(R::PClassInfo::kManyInstances, S::PClassInfo::kManyInstances)
CHECK_VALUE(R::IBStream::kIBSeekSet, S::IBStream::kIBSeekSet)
CHECK_VALUE(R::IBStream::kIBSeekCur, S::IBStream::kIBSeekCur)
CHECK_VALUE(R::IBStream::kIBSeekEnd, S::IBStream::kIBSeekEnd)
CHECK_VALUE(R::kAudio, SV::kAudio)
CHECK_VALUE(R::kEvent, SV::kEvent)
CHECK_VALUE(R::kInput, SV::kInput)
CHECK_VALUE(R::kOutput, SV::kOutput)
CHECK_VALUE(R::kMain, SV::kMain)
CHECK_VALUE(R::kAux, SV::kAux)
CHECK_VALUE(R::kSimple, SV::kSimple)
CHECK_VALUE(R::kAdvanced, SV::kAdvanced)
CHECK_VALUE(R::kOfflineProcessing, SV::kOfflineProcessing)
CHECK_VALUE(R::kSample32, SV::kSample32)
CHECK_VALUE(R::kSample64, SV::kSample64)
CHECK_VALUE(R::kRealtime, SV::kRealtime)
CHECK_VALUE(R::kPrefetch, SV::kPrefetch)
CHECK_VALUE(R::kOffline, SV::kOffline)
CHECK_VALUE(R::kSpeakerL, SV::kSpeakerL)
CHECK_VALUE(R::kSpeakerR, SV::kSpeakerR)
CHECK_VALUE(R::kSpeakerM, SV::kSpeakerM)
CHECK_VALUE(R::kSpeakerC, SV::kSpeakerC)
CHECK_VALUE(R::kSpeakerLfe, SV::kSpeakerLfe)
CHECK_VALUE(R::kSpeakerLs, SV::kSpeakerLs)
CHECK_VALUE(R::kSpeakerRs, SV::kSpeakerRs)
CHECK_VALUE(R::SpeakerArr::kEmpty, SV::SpeakerArr::kEmpty)
CHECK_VALUE(R::SpeakerArr::kMono, SV::SpeakerArr::kMono)
CHECK_VALUE(R::SpeakerArr::kStereo, SV::SpeakerArr::kStereo)
CHECK_VALUE(R::SpeakerArr::k51, SV::SpeakerArr::k51)
CHECK_VALUE(R::BusInfo::kDefaultActive, SV::BusInfo::kDefaultActive)
CHECK_VALUE(R::BusInfo::kIsControlVoltage, SV::BusInfo::kIsControlVoltage)
CHECK_VALUE(R::FrameRate::kPullDownRate, SV::FrameRate::kPullDownRate)
CHECK_VALUE(R::FrameRate::kDropRate, SV::FrameRate::kDropRate)
CHECK_VALUE(R::ProcessContext::kPlaying, SV::ProcessContext::kPlaying)
CHECK_VALUE(R::ProcessContext::kCycleActive, SV::ProcessContext::kCycleActive)
CHECK_VALUE(R::ProcessContext::kRecording, SV::ProcessContext::kRecording)
CHECK_VALUE(R::ProcessContext::kSystemTimeValid, SV::ProcessContext::kSystemTimeValid)
CHECK_VALUE(R::ProcessContext::kProjectTimeMusicValid, SV::ProcessContext::kProjectTimeMusicValid)
CHECK_VALUE(R::ProcessContext::kTempoValid, SV::ProcessContext::kTempoValid)
CHECK_VALUE(R::ProcessContext::kBarPositionValid, SV::ProcessContext::kBarPositionValid)
CHECK_VALUE(R::ProcessContext::kCycleValid, SV::ProcessContext::kCycleValid)
CHECK_VALUE(R::ProcessContext::kTimeSigValid, SV::ProcessContext::kTimeSigValid)
CHECK_VALUE(R::ProcessContext::kSmpteValid, SV::ProcessContext::kSmpteValid)
CHECK_VALUE(R::ProcessContext::kClockValid, SV::ProcessContext::kClockValid)
CHECK_VALUE(R::ProcessContext::kContTimeValid, SV::ProcessContext::kContTimeValid)
CHECK_VALUE(R::ProcessContext::kChordValid, SV::ProcessContext::kChordValid)
CHECK_VALUE(R::ParameterInfo::kNoFlags, SV::ParameterInfo::kNoFlags)
CHECK_VALUE(R::ParameterInfo::kCanAutomate, SV::ParameterInfo::kCanAutomate)
CHECK_VALUE(R::ParameterInfo::kIsReadOnly, SV::ParameterInfo::kIsReadOnly)
CHECK_VALUE(R::ParameterInfo::kIsWrapAround, SV::ParameterInfo::kIsWrapAround)
CHECK_VALUE(R::ParameterInfo::kIsList, SV::ParameterInfo::kIsList)
CHECK_VALUE(R::ParameterInfo::kIsHidden, SV::ParameterInfo::kIsHidden)
CHECK_VALUE(R::ParameterInfo::kIsProgramChange, SV::ParameterInfo::kIsProgramChange)
CHECK_VALUE(R::ParameterInfo::kIsBypass, SV::ParameterInfo::kIsBypass)
CHECK_VALUE(R::kReloadComponent, SV::kReloadComponent)
CHECK_VALUE(R::kIoChanged, SV::kIoChanged)
CHECK_VALUE(R::kParamValuesChanged, SV::kParamValuesChanged)
CHECK_VALUE(R::kLatencyChanged, SV::kLatencyChanged)
CHECK_VALUE(R::kParamTitlesChanged, SV::kParamTitlesChanged)
CHECK_VALUE(R::kMidiCCAssignmentChanged, SV::kMidiCCAssignmentChanged)
CHECK_VALUE(R::kNoteExpressionChanged, SV::kNoteExpressionChanged)
CHECK_VALUE(R::kIoTitlesChanged, SV::kIoTitlesChanged)
CHECK_VALUE(R::kPrefetchableSupportChanged, SV::kPrefetchableSupportChanged)
CHECK_VALUE(R::kRoutingInfoChanged, SV::kRoutingInfoChanged)
CHECK_VALUE(R::kKeyswitchChanged, SV::kKeyswitchChanged)
CHECK_VALUE(R::kParamIDMappingChanged, SV::kParamIDMappingChanged)

// ------------------------------------------------------------------ struct layouts
#define CHECK_STRUCT(Ours, Sdk) \
    static_assert(sizeof(Ours) == sizeof(Sdk), "size of " #Ours); \
    static_assert(alignof(Ours) == alignof(Sdk), "alignment of " #Ours);
// (the compiler's own offsetof: the one in the Windows headers is not a constant for every compiler)
#define OFFSET_OF(s, m) __builtin_offsetof(s, m)
#define CHECK_MEMBER(Ours, Sdk, m) \
    static_assert(OFFSET_OF(Ours, m) == OFFSET_OF(Sdk, m), "offset of " #Ours "::" #m); \
    static_assert(sizeof(((Ours*)nullptr)->m) == sizeof(((Sdk*)nullptr)->m), "size of " #Ours "::" #m); \
    static_assert(std::is_same<Canon<decltype(((Ours*)nullptr)->m)>::type, Canon<decltype(((Sdk*)nullptr)->m)>::type>::value, "type of " #Ours "::" #m);

#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Winvalid-offsetof"       // (some of the structs have constructors)
#endif

CHECK_STRUCT(R::PFactoryInfo, S::PFactoryInfo)
CHECK_MEMBER(R::PFactoryInfo, S::PFactoryInfo, vendor)
CHECK_MEMBER(R::PFactoryInfo, S::PFactoryInfo, url)
CHECK_MEMBER(R::PFactoryInfo, S::PFactoryInfo, email)
CHECK_MEMBER(R::PFactoryInfo, S::PFactoryInfo, flags)

CHECK_STRUCT(R::PClassInfo, S::PClassInfo)
CHECK_MEMBER(R::PClassInfo, S::PClassInfo, cid)
CHECK_MEMBER(R::PClassInfo, S::PClassInfo, cardinality)
CHECK_MEMBER(R::PClassInfo, S::PClassInfo, category)
CHECK_MEMBER(R::PClassInfo, S::PClassInfo, name)

CHECK_STRUCT(R::PClassInfo2, S::PClassInfo2)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, cid)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, cardinality)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, category)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, name)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, classFlags)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, subCategories)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, vendor)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, version)
CHECK_MEMBER(R::PClassInfo2, S::PClassInfo2, sdkVersion)

CHECK_STRUCT(R::PClassInfoW, S::PClassInfoW)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, cid)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, cardinality)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, category)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, name)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, classFlags)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, subCategories)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, vendor)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, version)
CHECK_MEMBER(R::PClassInfoW, S::PClassInfoW, sdkVersion)

CHECK_STRUCT(R::ViewRect, S::ViewRect)
CHECK_MEMBER(R::ViewRect, S::ViewRect, left)
CHECK_MEMBER(R::ViewRect, S::ViewRect, top)
CHECK_MEMBER(R::ViewRect, S::ViewRect, right)
CHECK_MEMBER(R::ViewRect, S::ViewRect, bottom)

CHECK_STRUCT(R::BusInfo, SV::BusInfo)
CHECK_MEMBER(R::BusInfo, SV::BusInfo, mediaType)
CHECK_MEMBER(R::BusInfo, SV::BusInfo, direction)
CHECK_MEMBER(R::BusInfo, SV::BusInfo, channelCount)
CHECK_MEMBER(R::BusInfo, SV::BusInfo, name)
CHECK_MEMBER(R::BusInfo, SV::BusInfo, busType)
CHECK_MEMBER(R::BusInfo, SV::BusInfo, flags)

CHECK_STRUCT(R::RoutingInfo, SV::RoutingInfo)
CHECK_MEMBER(R::RoutingInfo, SV::RoutingInfo, mediaType)
CHECK_MEMBER(R::RoutingInfo, SV::RoutingInfo, busIndex)
CHECK_MEMBER(R::RoutingInfo, SV::RoutingInfo, channel)

CHECK_STRUCT(R::ProcessSetup, SV::ProcessSetup)
CHECK_MEMBER(R::ProcessSetup, SV::ProcessSetup, processMode)
CHECK_MEMBER(R::ProcessSetup, SV::ProcessSetup, symbolicSampleSize)
CHECK_MEMBER(R::ProcessSetup, SV::ProcessSetup, maxSamplesPerBlock)
CHECK_MEMBER(R::ProcessSetup, SV::ProcessSetup, sampleRate)

CHECK_STRUCT(R::AudioBusBuffers, SV::AudioBusBuffers)
CHECK_MEMBER(R::AudioBusBuffers, SV::AudioBusBuffers, numChannels)
CHECK_MEMBER(R::AudioBusBuffers, SV::AudioBusBuffers, silenceFlags)
CHECK_MEMBER(R::AudioBusBuffers, SV::AudioBusBuffers, channelBuffers32)
CHECK_MEMBER(R::AudioBusBuffers, SV::AudioBusBuffers, channelBuffers64)

CHECK_STRUCT(R::ProcessData, SV::ProcessData)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, processMode)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, symbolicSampleSize)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, numSamples)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, numInputs)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, numOutputs)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, inputs)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, outputs)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, inputParameterChanges)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, outputParameterChanges)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, inputEvents)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, outputEvents)
CHECK_MEMBER(R::ProcessData, SV::ProcessData, processContext)

CHECK_STRUCT(R::FrameRate, SV::FrameRate)
CHECK_MEMBER(R::FrameRate, SV::FrameRate, framesPerSecond)
CHECK_MEMBER(R::FrameRate, SV::FrameRate, flags)

CHECK_STRUCT(R::Chord, SV::Chord)
CHECK_MEMBER(R::Chord, SV::Chord, keyNote)
CHECK_MEMBER(R::Chord, SV::Chord, rootNote)
CHECK_MEMBER(R::Chord, SV::Chord, chordMask)

CHECK_STRUCT(R::ProcessContext, SV::ProcessContext)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, state)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, sampleRate)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, projectTimeSamples)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, systemTime)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, continousTimeSamples)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, projectTimeMusic)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, barPositionMusic)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, cycleStartMusic)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, cycleEndMusic)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, tempo)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, timeSigNumerator)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, timeSigDenominator)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, smpteOffsetSubframes)
CHECK_MEMBER(R::ProcessContext, SV::ProcessContext, samplesToNextClock)
static_assert(OFFSET_OF(R::ProcessContext, chord) == OFFSET_OF(SV::ProcessContext, chord) && sizeof(R::Chord) == sizeof(SV::Chord), "ProcessContext::chord");
static_assert(OFFSET_OF(R::ProcessContext, frameRate) == OFFSET_OF(SV::ProcessContext, frameRate) && sizeof(R::FrameRate) == sizeof(SV::FrameRate), "ProcessContext::frameRate");

CHECK_STRUCT(R::ParameterInfo, SV::ParameterInfo)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, id)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, title)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, shortTitle)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, units)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, stepCount)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, defaultNormalizedValue)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, unitId)
CHECK_MEMBER(R::ParameterInfo, SV::ParameterInfo, flags)

// ------------------------------------------------------------------ virtual functions
// Signature: compared while compiling. Position in the table: compared when run.
static int failures = 0;
static int checked = 0;

// The position of a virtual function, read out of a pointer to it (-1 where that cannot be done).
template <class F> static long slotOf(F f)
{
#if defined(__GNUC__) && !defined(_WIN32)
    struct Rep { std::ptrdiff_t ptr, adj; } rep;
    static_assert(sizeof(F) == sizeof(Rep), "member function pointer shape");
    std::memcpy(&rep, &f, sizeof(rep));
    if (!(rep.ptr & 1)) return -2;                      // not virtual
    return (long)((rep.ptr - 1) / (std::ptrdiff_t)sizeof(void*));
#else
    (void)f;
    return -1;
#endif
}

static void slotCheck(const char* what, long ours, long sdk)
{
    ++checked;
    if (ours == -1 && sdk == -1) return;                // cannot be read here
    if (ours != sdk || ours < 0) { ++failures; std::printf("MISMATCH  %-52s ours %ld, official %ld\n", what, ours, sdk); }
    else std::printf("ok   slot %2ld  %s\n", ours, what);
}

#define CHECK_METHOD(Ours, Sdk, m) \
    static_assert(std::is_same<Canon<decltype(&Ours::m)>::type, Canon<decltype(&Sdk::m)>::type>::value, "signature of " #Ours "::" #m); \
    slotCheck(#Ours "::" #m, slotOf(&Ours::m), slotOf(&Sdk::m));

// One more virtual function added after an interface's own lands right after its last one, so its
// position is the number of virtual functions the interface has.
template <class Base> struct OneMore : Base { virtual void renoOneMore() {} };
#define CHECK_COUNT(Ours, Sdk) slotCheck(#Ours " (number of virtual functions)", slotOf(&OneMore<Ours>::renoOneMore), slotOf(&OneMore<Sdk>::renoOneMore));

int main()
{
    CHECK_METHOD(R::FUnknown, S::FUnknown, queryInterface)
    CHECK_METHOD(R::FUnknown, S::FUnknown, addRef)
    CHECK_METHOD(R::FUnknown, S::FUnknown, release)
    CHECK_COUNT(R::FUnknown, S::FUnknown)

    CHECK_METHOD(R::IPluginBase, S::IPluginBase, initialize)
    CHECK_METHOD(R::IPluginBase, S::IPluginBase, terminate)
    CHECK_COUNT(R::IPluginBase, S::IPluginBase)

    CHECK_METHOD(R::IPluginFactory, S::IPluginFactory, getFactoryInfo)
    CHECK_METHOD(R::IPluginFactory, S::IPluginFactory, countClasses)
    CHECK_METHOD(R::IPluginFactory, S::IPluginFactory, getClassInfo)
    CHECK_METHOD(R::IPluginFactory, S::IPluginFactory, createInstance)
    CHECK_COUNT(R::IPluginFactory, S::IPluginFactory)

    CHECK_METHOD(R::IPluginFactory2, S::IPluginFactory2, getClassInfo2)
    CHECK_COUNT(R::IPluginFactory2, S::IPluginFactory2)

    CHECK_METHOD(R::IPluginFactory3, S::IPluginFactory3, getClassInfoUnicode)
    CHECK_METHOD(R::IPluginFactory3, S::IPluginFactory3, setHostContext)
    CHECK_COUNT(R::IPluginFactory3, S::IPluginFactory3)

    CHECK_METHOD(R::IBStream, S::IBStream, read)
    CHECK_METHOD(R::IBStream, S::IBStream, write)
    CHECK_METHOD(R::IBStream, S::IBStream, seek)
    CHECK_METHOD(R::IBStream, S::IBStream, tell)
    CHECK_COUNT(R::IBStream, S::IBStream)

    CHECK_METHOD(R::IPlugView, S::IPlugView, isPlatformTypeSupported)
    CHECK_METHOD(R::IPlugView, S::IPlugView, attached)
    CHECK_METHOD(R::IPlugView, S::IPlugView, removed)
    CHECK_METHOD(R::IPlugView, S::IPlugView, onWheel)
    CHECK_METHOD(R::IPlugView, S::IPlugView, onKeyDown)
    CHECK_METHOD(R::IPlugView, S::IPlugView, onKeyUp)
    CHECK_METHOD(R::IPlugView, S::IPlugView, getSize)
    CHECK_METHOD(R::IPlugView, S::IPlugView, onSize)
    CHECK_METHOD(R::IPlugView, S::IPlugView, onFocus)
    CHECK_METHOD(R::IPlugView, S::IPlugView, setFrame)
    CHECK_METHOD(R::IPlugView, S::IPlugView, canResize)
    CHECK_METHOD(R::IPlugView, S::IPlugView, checkSizeConstraint)
    CHECK_COUNT(R::IPlugView, S::IPlugView)

    CHECK_METHOD(R::IPlugFrame, S::IPlugFrame, resizeView)
    CHECK_COUNT(R::IPlugFrame, S::IPlugFrame)

    CHECK_METHOD(R::IComponent, SV::IComponent, getControllerClassId)
    CHECK_METHOD(R::IComponent, SV::IComponent, setIoMode)
    CHECK_METHOD(R::IComponent, SV::IComponent, getBusCount)
    CHECK_METHOD(R::IComponent, SV::IComponent, getBusInfo)
    CHECK_METHOD(R::IComponent, SV::IComponent, getRoutingInfo)
    CHECK_METHOD(R::IComponent, SV::IComponent, activateBus)
    CHECK_METHOD(R::IComponent, SV::IComponent, setActive)
    CHECK_METHOD(R::IComponent, SV::IComponent, setState)
    CHECK_METHOD(R::IComponent, SV::IComponent, getState)
    CHECK_COUNT(R::IComponent, SV::IComponent)

    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, setBusArrangements)
    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, getBusArrangement)
    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, canProcessSampleSize)
    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, getLatencySamples)
    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, setupProcessing)
    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, setProcessing)
    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, process)
    CHECK_METHOD(R::IAudioProcessor, SV::IAudioProcessor, getTailSamples)
    CHECK_COUNT(R::IAudioProcessor, SV::IAudioProcessor)

    CHECK_METHOD(R::IParamValueQueue, SV::IParamValueQueue, getParameterId)
    CHECK_METHOD(R::IParamValueQueue, SV::IParamValueQueue, getPointCount)
    CHECK_METHOD(R::IParamValueQueue, SV::IParamValueQueue, getPoint)
    CHECK_METHOD(R::IParamValueQueue, SV::IParamValueQueue, addPoint)
    CHECK_COUNT(R::IParamValueQueue, SV::IParamValueQueue)

    CHECK_METHOD(R::IParameterChanges, SV::IParameterChanges, getParameterCount)
    CHECK_METHOD(R::IParameterChanges, SV::IParameterChanges, getParameterData)
    CHECK_METHOD(R::IParameterChanges, SV::IParameterChanges, addParameterData)
    CHECK_COUNT(R::IParameterChanges, SV::IParameterChanges)

    CHECK_METHOD(R::IEventList, SV::IEventList, getEventCount)
    CHECK_METHOD(R::IEventList, SV::IEventList, getEvent)
    CHECK_METHOD(R::IEventList, SV::IEventList, addEvent)
    CHECK_COUNT(R::IEventList, SV::IEventList)

    CHECK_METHOD(R::IComponentHandler, SV::IComponentHandler, beginEdit)
    CHECK_METHOD(R::IComponentHandler, SV::IComponentHandler, performEdit)
    CHECK_METHOD(R::IComponentHandler, SV::IComponentHandler, endEdit)
    CHECK_METHOD(R::IComponentHandler, SV::IComponentHandler, restartComponent)
    CHECK_COUNT(R::IComponentHandler, SV::IComponentHandler)

    CHECK_METHOD(R::IEditController, SV::IEditController, setComponentState)
    CHECK_METHOD(R::IEditController, SV::IEditController, setState)
    CHECK_METHOD(R::IEditController, SV::IEditController, getState)
    CHECK_METHOD(R::IEditController, SV::IEditController, getParameterCount)
    CHECK_METHOD(R::IEditController, SV::IEditController, getParameterInfo)
    CHECK_METHOD(R::IEditController, SV::IEditController, getParamStringByValue)
    CHECK_METHOD(R::IEditController, SV::IEditController, getParamValueByString)
    CHECK_METHOD(R::IEditController, SV::IEditController, normalizedParamToPlain)
    CHECK_METHOD(R::IEditController, SV::IEditController, plainParamToNormalized)
    CHECK_METHOD(R::IEditController, SV::IEditController, getParamNormalized)
    CHECK_METHOD(R::IEditController, SV::IEditController, setParamNormalized)
    CHECK_METHOD(R::IEditController, SV::IEditController, setComponentHandler)
    CHECK_METHOD(R::IEditController, SV::IEditController, createView)
    CHECK_COUNT(R::IEditController, SV::IEditController)

    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, setInt)
    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, getInt)
    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, setFloat)
    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, getFloat)
    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, setString)
    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, getString)
    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, setBinary)
    CHECK_METHOD(R::IAttributeList, SV::IAttributeList, getBinary)
    CHECK_COUNT(R::IAttributeList, SV::IAttributeList)

    CHECK_METHOD(R::IMessage, SV::IMessage, getMessageID)
    CHECK_METHOD(R::IMessage, SV::IMessage, setMessageID)
    CHECK_METHOD(R::IMessage, SV::IMessage, getAttributes)
    CHECK_COUNT(R::IMessage, SV::IMessage)

    CHECK_METHOD(R::IConnectionPoint, SV::IConnectionPoint, connect)
    CHECK_METHOD(R::IConnectionPoint, SV::IConnectionPoint, disconnect)
    CHECK_METHOD(R::IConnectionPoint, SV::IConnectionPoint, notify)
    CHECK_COUNT(R::IConnectionPoint, SV::IConnectionPoint)

    CHECK_METHOD(R::IHostApplication, SV::IHostApplication, getName)
    CHECK_METHOD(R::IHostApplication, SV::IHostApplication, createInstance)
    CHECK_COUNT(R::IHostApplication, SV::IHostApplication)

    CHECK_METHOD(R::IPlugInterfaceSupport, SV::IPlugInterfaceSupport, isPlugInterfaceSupported)
    CHECK_COUNT(R::IPlugInterfaceSupport, SV::IPlugInterfaceSupport)

    // texts
    auto text = [](const char* what, const char* ours, const char* sdk) {
        ++checked;
        if (std::strcmp(ours, sdk) != 0) { ++failures; std::printf("MISMATCH  %s: ours \"%s\", official \"%s\"\n", what, ours, sdk); }
        else std::printf("ok   text     %s = \"%s\"\n", what, ours);
    };
    text("kVstAudioEffectClass", R::kVstAudioEffectClass, kSdkAudioEffectClass);
    text("kVstComponentControllerClass", R::kVstComponentControllerClass, kSdkControllerClass);
    text("kPlatformTypeHWND", R::kPlatformTypeHWND, S::kPlatformTypeHWND);
    text("kPlatformTypeX11EmbedWindowID", R::kPlatformTypeX11EmbedWindowID, S::kPlatformTypeX11EmbedWindowID);
    text("ViewType::kEditor", R::ViewType::kEditor, SV::ViewType::kEditor);

    const bool slotsReadable = slotOf(&R::FUnknown::addRef) != -1;
    std::printf("\n%d checks, %d mismatches (ids, constants, struct layouts and signatures were checked by the compiler)\n", checked, failures);
    if (!slotsReadable) std::printf("positions of virtual functions could NOT be read on this platform; use compare_vtables.py\n");
    std::printf("pointer size %d, struct packing as in pluginterfaces/base/falignpush.h, %s ids and result codes\n", (int)sizeof(void*),
                RENO_VST3_COM_STYLE ? "Windows (COM) style" : "plain");
    return failures ? 1 : 0;
}
