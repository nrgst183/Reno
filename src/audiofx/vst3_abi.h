// The part of the VST3 binary interface that Reno's host uses, declared from scratch (building Reno needs no
// Steinberg SDK). The text is our own; names match the SDK because they are the API's names.
//
// Everything here must match the official headers bit for bit: virtual function order in each interface, struct
// members, the 16 bytes of each interface id, calling convention and result codes.
//
// Verification, against the official VST3 SDK 3.8.1 sources (github.com/steinbergmedia/vst3_pluginterfaces commit
// 4f547e8e, vst3_public_sdk commit 586dc5e6): the declarations were written from base/ (fplatform, ftypes, funknown,
// ipluginbase, ibstream, falignpush), vst/ (vsttypes, vstspeaker, ivstcomponent, ivstaudioprocessor,
// ivsteditcontroller, ivstparameterchanges, ivstprocesscontext, ivstevents, ivstmessage, ivstattributes,
// ivsthostapplication, ivstpluginterfacesupport), gui/iplugview.h and the public.sdk module entry points
// (dllmain.cpp, linuxmain.cpp, module_win32.cpp, module_linux.cpp), then checked:
//   1. mechanically with tests/vst3/abicheck.cpp, which includes the official headers next to this one and has the
//      compiler compare every id, result code, constant, struct size, alignment and member offset, and the
//      signature of every virtual function; function positions in each table are compared at run time (Linux) and
//      with tests/vst3/compare_vtables.py on clang's printed tables (Windows). Done for 64-bit Linux (GCC and
//      Clang, compiled and run), 64-bit and 32-bit Windows (clang with the Visual Studio headers: compiled and
//      tables compared, not run) and ARM64 Windows (compiled only). See tests/vst3/README.txt;
//   2. for funknown.h, ivstaudioprocessor.h and ivsteditcontroller.h, again against the master copies on GitHub;
//   3. by running Reno's host, which uses only this file, against plug-ins built from the SDK's sources (ADelay, mda
//      samples, HostChecker) on 64-bit Linux: tests/vst3/run_sdk.sh.
//
// Not checked: macOS (not a Reno platform; 32-bit macOS packs structs to 1 byte, which this file does not do);
// 32-bit Linux; cl.exe's own table layout (clang matches it, and these interfaces use single inheritance with no
// overloads); anything run on Windows; any plug-in other than the SDK samples.
#pragma once
#include <cstdint>
#include <cstddef>

// RENO_VST3_COM_STYLE: 1 on Windows (ids laid out like a Windows GUID, COM result codes), 0 elsewhere. Only
// overridable so tests can run the Windows flavour of the host on Linux; never in a real build.
#if defined(_WIN32)
#  ifndef RENO_VST3_COM_STYLE
#    define RENO_VST3_COM_STYLE 1
#  endif
#  ifndef PLUGIN_API
#    define PLUGIN_API __stdcall
#  endif
#else
#  ifndef RENO_VST3_COM_STYLE
#    define RENO_VST3_COM_STYLE 0
#  endif
#  ifndef PLUGIN_API
#    define PLUGIN_API
#  endif
#endif

namespace reno {
namespace vst3 {

// ------------------------------------------------------------------ basic types
typedef char TUID[16];                          // an interface or class id
typedef int32_t tresult;
typedef uint8_t TBool;
typedef const char* FIDString;                  // 8-bit text, or (in createInstance) the 16 bytes of an id
typedef char16_t TChar;                         // UTF-16 on every platform
typedef TChar String128[128];

typedef int32_t MediaType;
typedef int32_t BusDirection;
typedef int32_t BusType;
typedef int32_t IoMode;
typedef int32_t UnitID;
typedef uint32_t ParamID;
typedef double ParamValue;
typedef double SampleRate;
typedef float Sample32;
typedef double Sample64;
typedef int64_t TSamples;
typedef double TQuarterNotes;
typedef uint64_t Speaker;
typedef uint64_t SpeakerArrangement;

// ------------------------------------------------------------------ result codes
#if RENO_VST3_COM_STYLE
constexpr tresult kNoInterface     = static_cast<tresult>(0x80004002u);    // E_NOINTERFACE
constexpr tresult kResultOk        = 0;                                    // S_OK
constexpr tresult kResultTrue      = kResultOk;
constexpr tresult kResultFalse     = 1;                                    // S_FALSE
constexpr tresult kInvalidArgument = static_cast<tresult>(0x80070057u);    // E_INVALIDARG
constexpr tresult kNotImplemented  = static_cast<tresult>(0x80004001u);    // E_NOTIMPL
constexpr tresult kInternalError   = static_cast<tresult>(0x80004005u);    // E_FAIL
constexpr tresult kNotInitialized  = static_cast<tresult>(0x8000FFFFu);    // E_UNEXPECTED
constexpr tresult kOutOfMemory     = static_cast<tresult>(0x8007000Eu);    // E_OUTOFMEMORY
#else
constexpr tresult kNoInterface     = -1;
constexpr tresult kResultOk        = 0;
constexpr tresult kResultTrue      = kResultOk;
constexpr tresult kResultFalse     = 1;
constexpr tresult kInvalidArgument = 2;
constexpr tresult kNotImplemented  = 3;
constexpr tresult kInternalError   = 4;
constexpr tresult kNotInitialized  = 5;
constexpr tresult kOutOfMemory     = 6;
#endif

// ------------------------------------------------------------------ interface ids
// An id is written as four 32-bit numbers a, b, c, d. Except on Windows its 16 bytes are those numbers, most
// significant byte first. On Windows the first eight bytes are stored like a GUID (a as a little-endian 32-bit
// number, then the upper and lower halves of b as little-endian 16-bit numbers); c and d stay big-endian.
#define RENO_VST3_BYTE(v, shift) static_cast<char>(static_cast<unsigned char>((static_cast<uint32_t>(v) >> (shift)) & 0xFFu))
#if RENO_VST3_COM_STYLE
#  define RENO_VST3_ID_BYTES(a, b, c, d) { \
        RENO_VST3_BYTE(a, 0),  RENO_VST3_BYTE(a, 8),  RENO_VST3_BYTE(a, 16), RENO_VST3_BYTE(a, 24), \
        RENO_VST3_BYTE(b, 16), RENO_VST3_BYTE(b, 24), RENO_VST3_BYTE(b, 0),  RENO_VST3_BYTE(b, 8),  \
        RENO_VST3_BYTE(c, 24), RENO_VST3_BYTE(c, 16), RENO_VST3_BYTE(c, 8),  RENO_VST3_BYTE(c, 0),  \
        RENO_VST3_BYTE(d, 24), RENO_VST3_BYTE(d, 16), RENO_VST3_BYTE(d, 8),  RENO_VST3_BYTE(d, 0) }
#else
#  define RENO_VST3_ID_BYTES(a, b, c, d) { \
        RENO_VST3_BYTE(a, 24), RENO_VST3_BYTE(a, 16), RENO_VST3_BYTE(a, 8),  RENO_VST3_BYTE(a, 0),  \
        RENO_VST3_BYTE(b, 24), RENO_VST3_BYTE(b, 16), RENO_VST3_BYTE(b, 8),  RENO_VST3_BYTE(b, 0),  \
        RENO_VST3_BYTE(c, 24), RENO_VST3_BYTE(c, 16), RENO_VST3_BYTE(c, 8),  RENO_VST3_BYTE(c, 0),  \
        RENO_VST3_BYTE(d, 24), RENO_VST3_BYTE(d, 16), RENO_VST3_BYTE(d, 8),  RENO_VST3_BYTE(d, 0) }
#endif
// The id of interface X is the constant X_iid.
#define RENO_VST3_IID(Interface, a, b, c, d) inline constexpr TUID Interface##_iid = RENO_VST3_ID_BYTES(a, b, c, d);

inline bool iidEqual(const char* x, const char* y)
{
    if (!x || !y) return false;
    for (int k = 0; k < 16; ++k) if (x[k] != y[k]) return false;
    return true;
}

// ------------------------------------------------------------------ the root interface
class FUnknown {
public:
    virtual tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) = 0;
    virtual uint32_t PLUGIN_API addRef() = 0;
    virtual uint32_t PLUGIN_API release() = 0;
};
RENO_VST3_IID(FUnknown, 0x00000000, 0x00000000, 0xC0000000, 0x00000046)

// ------------------------------------------------------------------ the module's factory
class IPluginBase : public FUnknown {
public:
    virtual tresult PLUGIN_API initialize(FUnknown* context) = 0;
    virtual tresult PLUGIN_API terminate() = 0;
};
RENO_VST3_IID(IPluginBase, 0x22888DDB, 0x156E45AE, 0x8358B348, 0x08190625)

struct PFactoryInfo {
    enum FactoryFlags { kNoFlags = 0, kClassesDiscardable = 1 << 0, kLicenseCheck = 1 << 1, kComponentNonDiscardable = 1 << 3, kUnicode = 1 << 4 };
    enum { kURLSize = 256, kEmailSize = 128, kNameSize = 64 };
    char vendor[kNameSize];
    char url[kURLSize];
    char email[kEmailSize];
    int32_t flags;
};

struct PClassInfo {
    enum ClassCardinality { kManyInstances = 0x7FFFFFFF };
    enum { kCategorySize = 32, kNameSize = 64 };
    TUID cid;
    int32_t cardinality;
    char category[kCategorySize];
    char name[kNameSize];
};

class IPluginFactory : public FUnknown {
public:
    virtual tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) = 0;
    virtual int32_t PLUGIN_API countClasses() = 0;
    virtual tresult PLUGIN_API getClassInfo(int32_t index, PClassInfo* info) = 0;
    virtual tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) = 0;    // both are 16-byte ids
};
RENO_VST3_IID(IPluginFactory, 0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F)

struct PClassInfo2 {
    TUID cid;
    int32_t cardinality;
    char category[PClassInfo::kCategorySize];
    char name[PClassInfo::kNameSize];
    enum { kVendorSize = 64, kVersionSize = 64, kSubCategoriesSize = 128 };
    uint32_t classFlags;
    char subCategories[kSubCategoriesSize];
    char vendor[kVendorSize];
    char version[kVersionSize];
    char sdkVersion[kVersionSize];
};

class IPluginFactory2 : public IPluginFactory {
public:
    virtual tresult PLUGIN_API getClassInfo2(int32_t index, PClassInfo2* info) = 0;
};
RENO_VST3_IID(IPluginFactory2, 0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB)

struct PClassInfoW {                            // PClassInfo2 with the readable texts in UTF-16
    TUID cid;
    int32_t cardinality;
    char category[PClassInfo::kCategorySize];
    char16_t name[PClassInfo::kNameSize];
    enum { kVendorSize = 64, kVersionSize = 64, kSubCategoriesSize = 128 };
    uint32_t classFlags;
    char subCategories[kSubCategoriesSize];
    char16_t vendor[kVendorSize];
    char16_t version[kVersionSize];
    char16_t sdkVersion[kVersionSize];
};

class IPluginFactory3 : public IPluginFactory2 {
public:
    virtual tresult PLUGIN_API getClassInfoUnicode(int32_t index, PClassInfoW* info) = 0;
    virtual tresult PLUGIN_API setHostContext(FUnknown* context) = 0;
};
RENO_VST3_IID(IPluginFactory3, 0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931)

// What a module exports. InitDll/ExitDll (Windows) are optional; ModuleEntry/ModuleExit (Linux) are expected.
extern "C" {
typedef IPluginFactory* (PLUGIN_API* GetFactoryProc)();         // "GetPluginFactory"
typedef bool (PLUGIN_API* InitDllProc)();                       // "InitDll", "ExitDll", "ModuleExit"
typedef bool (PLUGIN_API* ModuleEntryProc)(void*);              // "ModuleEntry" (given the library handle); macOS: "bundleEntry" (given a CFBundleRef)
}

constexpr const char* kVstAudioEffectClass = "Audio Module Class";             // PClassInfo::category of an effect or instrument
constexpr const char* kVstComponentControllerClass = "Component Controller Class";

// ------------------------------------------------------------------ streams (plug-in state)
class IBStream : public FUnknown {
public:
    enum IStreamSeekMode { kIBSeekSet = 0, kIBSeekCur, kIBSeekEnd };
    virtual tresult PLUGIN_API read(void* buffer, int32_t numBytes, int32_t* numBytesRead = nullptr) = 0;
    virtual tresult PLUGIN_API write(void* buffer, int32_t numBytes, int32_t* numBytesWritten = nullptr) = 0;
    virtual tresult PLUGIN_API seek(int64_t pos, int32_t mode, int64_t* result = nullptr) = 0;
    virtual tresult PLUGIN_API tell(int64_t* pos) = 0;
};
RENO_VST3_IID(IBStream, 0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B)

// ------------------------------------------------------------------ the plug-in's window
struct ViewRect {
    ViewRect(int32_t l = 0, int32_t t = 0, int32_t r = 0, int32_t b = 0) : left(l), top(t), right(r), bottom(b) {}
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t getWidth() const { return right - left; }
    int32_t getHeight() const { return bottom - top; }
};

constexpr FIDString kPlatformTypeHWND = "HWND";
constexpr FIDString kPlatformTypeX11EmbedWindowID = "X11EmbedWindowID";
constexpr FIDString kPlatformTypeNSView = "NSView";                  // macOS: the parent is an NSView*

class IPlugFrame;

class IPlugView : public FUnknown {
public:
    virtual tresult PLUGIN_API isPlatformTypeSupported(FIDString type) = 0;
    virtual tresult PLUGIN_API attached(void* parent, FIDString type) = 0;
    virtual tresult PLUGIN_API removed() = 0;
    virtual tresult PLUGIN_API onWheel(float distance) = 0;
    virtual tresult PLUGIN_API onKeyDown(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult PLUGIN_API onKeyUp(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult PLUGIN_API getSize(ViewRect* size) = 0;
    virtual tresult PLUGIN_API onSize(ViewRect* newSize) = 0;
    virtual tresult PLUGIN_API onFocus(TBool state) = 0;
    virtual tresult PLUGIN_API setFrame(IPlugFrame* frame) = 0;
    virtual tresult PLUGIN_API canResize() = 0;
    virtual tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) = 0;
};
RENO_VST3_IID(IPlugView, 0x5BC32507, 0xD06049EA, 0xA6151B52, 0x2B755B29)

class IPlugFrame : public FUnknown {            // implemented by the host
public:
    virtual tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) = 0;
};
RENO_VST3_IID(IPlugFrame, 0x367FAF01, 0xAFA94693, 0x8D4DA2A0, 0xED0882A3)

// ------------------------------------------------------------------ everything below is packed as in the SDK's vst/ headers
#if defined(_WIN64) || defined(_M_ARM64) || defined(__LP64__)
#  pragma pack(push, 16)
#else
#  pragma pack(push, 8)
#endif

enum MediaTypes { kAudio = 0, kEvent, kNumMediaTypes };
enum BusDirections { kInput = 0, kOutput };
enum BusTypes { kMain = 0, kAux };
enum IoModes { kSimple = 0, kAdvanced, kOfflineProcessing };
enum SymbolicSampleSizes { kSample32 = 0, kSample64 };
enum ProcessModes { kRealtime = 0, kPrefetch, kOffline };

constexpr Speaker kSpeakerL = 1u << 0;
constexpr Speaker kSpeakerR = 1u << 1;
constexpr Speaker kSpeakerC = 1u << 2;
constexpr Speaker kSpeakerLfe = 1u << 3;
constexpr Speaker kSpeakerLs = 1u << 4;
constexpr Speaker kSpeakerRs = 1u << 5;
constexpr Speaker kSpeakerM = 1u << 19;
namespace SpeakerArr {
constexpr SpeakerArrangement kEmpty = 0;
constexpr SpeakerArrangement kMono = kSpeakerM;
constexpr SpeakerArrangement kStereo = kSpeakerL | kSpeakerR;
constexpr SpeakerArrangement k51 = kSpeakerL | kSpeakerR | kSpeakerC | kSpeakerLfe | kSpeakerLs | kSpeakerRs;    // channels in this order
}

struct BusInfo {
    MediaType mediaType;
    BusDirection direction;
    int32_t channelCount;
    String128 name;
    BusType busType;
    uint32_t flags;
    enum BusFlags { kDefaultActive = 1 << 0, kIsControlVoltage = 1 << 1 };
};

struct RoutingInfo {
    MediaType mediaType;
    int32_t busIndex;
    int32_t channel;
};

class IComponent : public IPluginBase {
public:
    virtual tresult PLUGIN_API getControllerClassId(TUID classId) = 0;
    virtual tresult PLUGIN_API setIoMode(IoMode mode) = 0;
    virtual int32_t PLUGIN_API getBusCount(MediaType type, BusDirection dir) = 0;
    virtual tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32_t index, BusInfo& bus) = 0;
    virtual tresult PLUGIN_API getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) = 0;
    virtual tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32_t index, TBool state) = 0;
    virtual tresult PLUGIN_API setActive(TBool state) = 0;
    virtual tresult PLUGIN_API setState(IBStream* state) = 0;
    virtual tresult PLUGIN_API getState(IBStream* state) = 0;
};
RENO_VST3_IID(IComponent, 0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802)

// ------------------------------------------------------------------ audio processing
struct ProcessSetup {
    int32_t processMode;
    int32_t symbolicSampleSize;
    int32_t maxSamplesPerBlock;
    SampleRate sampleRate;
};

struct AudioBusBuffers {
    int32_t numChannels = 0;
    uint64_t silenceFlags = 0;                  // bit n set: channel n is all zero
    union {
        Sample32** channelBuffers32;
        Sample64** channelBuffers64 = nullptr;
    };
};

class IParameterChanges;
class IEventList;
struct ProcessContext;

struct ProcessData {
    int32_t processMode = 0;
    int32_t symbolicSampleSize = kSample32;
    int32_t numSamples = 0;
    int32_t numInputs = 0;                      // number of input buses
    int32_t numOutputs = 0;                     // number of output buses
    AudioBusBuffers* inputs = nullptr;
    AudioBusBuffers* outputs = nullptr;
    IParameterChanges* inputParameterChanges = nullptr;
    IParameterChanges* outputParameterChanges = nullptr;
    IEventList* inputEvents = nullptr;
    IEventList* outputEvents = nullptr;
    ProcessContext* processContext = nullptr;
};

class IAudioProcessor : public FUnknown {
public:
    virtual tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32_t numIns, SpeakerArrangement* outputs, int32_t numOuts) = 0;
    virtual tresult PLUGIN_API getBusArrangement(BusDirection dir, int32_t index, SpeakerArrangement& arr) = 0;
    virtual tresult PLUGIN_API canProcessSampleSize(int32_t symbolicSampleSize) = 0;
    virtual uint32_t PLUGIN_API getLatencySamples() = 0;
    virtual tresult PLUGIN_API setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult PLUGIN_API setProcessing(TBool state) = 0;
    virtual tresult PLUGIN_API process(ProcessData& data) = 0;
    virtual uint32_t PLUGIN_API getTailSamples() = 0;
};
RENO_VST3_IID(IAudioProcessor, 0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D)

struct FrameRate {
    enum FrameRateFlags { kPullDownRate = 1 << 0, kDropRate = 1 << 1 };
    uint32_t framesPerSecond;
    uint32_t flags;
};

struct Chord {
    uint8_t keyNote;
    uint8_t rootNote;
    int16_t chordMask;
};

struct ProcessContext {                         // where the transport is, for one block
    enum StatesAndFlags {
        kPlaying = 1 << 1, kCycleActive = 1 << 2, kRecording = 1 << 3,
        kSystemTimeValid = 1 << 8, kProjectTimeMusicValid = 1 << 9, kTempoValid = 1 << 10, kBarPositionValid = 1 << 11,
        kCycleValid = 1 << 12, kTimeSigValid = 1 << 13, kSmpteValid = 1 << 14, kClockValid = 1 << 15,
        kContTimeValid = 1 << 17, kChordValid = 1 << 18
    };
    uint32_t state;
    double sampleRate;
    TSamples projectTimeSamples;
    int64_t systemTime;
    TSamples continousTimeSamples;              // spelled as in the SDK
    TQuarterNotes projectTimeMusic;
    TQuarterNotes barPositionMusic;
    TQuarterNotes cycleStartMusic;
    TQuarterNotes cycleEndMusic;
    double tempo;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    Chord chord;
    int32_t smpteOffsetSubframes;
    FrameRate frameRate;
    int32_t samplesToNextClock;
};

class IParamValueQueue : public FUnknown {      // the values one parameter takes during a block
public:
    virtual ParamID PLUGIN_API getParameterId() = 0;
    virtual int32_t PLUGIN_API getPointCount() = 0;
    virtual tresult PLUGIN_API getPoint(int32_t index, int32_t& sampleOffset, ParamValue& value) = 0;
    virtual tresult PLUGIN_API addPoint(int32_t sampleOffset, ParamValue value, int32_t& index) = 0;
};
RENO_VST3_IID(IParamValueQueue, 0x01263A18, 0xED074F6F, 0x98C9D356, 0x4686F9BA)

class IParameterChanges : public FUnknown {     // one queue per parameter that changes during a block
public:
    virtual int32_t PLUGIN_API getParameterCount() = 0;
    virtual IParamValueQueue* PLUGIN_API getParameterData(int32_t index) = 0;
    virtual IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32_t& index) = 0;
};
RENO_VST3_IID(IParameterChanges, 0xA4779663, 0x0BB64A56, 0xB44384A8, 0x466FEB9D)

struct Event;                                   // notes etc.; Reno sends none, so the layout is not declared

class IEventList : public FUnknown {
public:
    virtual int32_t PLUGIN_API getEventCount() = 0;
    virtual tresult PLUGIN_API getEvent(int32_t index, Event& e) = 0;
    virtual tresult PLUGIN_API addEvent(Event& e) = 0;
};
RENO_VST3_IID(IEventList, 0x3A2C4214, 0x346349FE, 0xB2C4F397, 0xB9695A44)

// ------------------------------------------------------------------ parameters and the controller half of a plug-in
struct ParameterInfo {
    ParamID id;
    String128 title;
    String128 shortTitle;
    String128 units;
    int32_t stepCount;
    ParamValue defaultNormalizedValue;
    UnitID unitId;
    int32_t flags;
    enum ParameterFlags : int32_t {
        kNoFlags = 0, kCanAutomate = 1 << 0, kIsReadOnly = 1 << 1, kIsWrapAround = 1 << 2, kIsList = 1 << 3, kIsHidden = 1 << 4,
        kIsProgramChange = 1 << 15, kIsBypass = 1 << 16
    };
};

enum RestartFlags : int32_t {
    kReloadComponent = 1 << 0, kIoChanged = 1 << 1, kParamValuesChanged = 1 << 2, kLatencyChanged = 1 << 3, kParamTitlesChanged = 1 << 4,
    kMidiCCAssignmentChanged = 1 << 5, kNoteExpressionChanged = 1 << 6, kIoTitlesChanged = 1 << 7, kPrefetchableSupportChanged = 1 << 8,
    kRoutingInfoChanged = 1 << 9, kKeyswitchChanged = 1 << 10, kParamIDMappingChanged = 1 << 11
};

class IComponentHandler : public FUnknown {     // implemented by the host: the plug-in's window reports edits here
public:
    virtual tresult PLUGIN_API beginEdit(ParamID id) = 0;
    virtual tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) = 0;
    virtual tresult PLUGIN_API endEdit(ParamID id) = 0;
    virtual tresult PLUGIN_API restartComponent(int32_t flags) = 0;
};
RENO_VST3_IID(IComponentHandler, 0x93A0BEA3, 0x0BD045DB, 0x8E890B0C, 0xC1E46AC6)

namespace ViewType {
constexpr FIDString kEditor = "editor";
}

class IEditController : public IPluginBase {
public:
    virtual tresult PLUGIN_API setComponentState(IBStream* state) = 0;
    virtual tresult PLUGIN_API setState(IBStream* state) = 0;
    virtual tresult PLUGIN_API getState(IBStream* state) = 0;
    virtual int32_t PLUGIN_API getParameterCount() = 0;
    virtual tresult PLUGIN_API getParameterInfo(int32_t paramIndex, ParameterInfo& info) = 0;
    virtual tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) = 0;
    virtual tresult PLUGIN_API getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) = 0;
    virtual ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue valueNormalized) = 0;
    virtual ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plainValue) = 0;
    virtual ParamValue PLUGIN_API getParamNormalized(ParamID id) = 0;
    virtual tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) = 0;
    virtual tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) = 0;
    virtual IPlugView* PLUGIN_API createView(FIDString name) = 0;
};
RENO_VST3_IID(IEditController, 0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E)

// ------------------------------------------------------------------ messages between the two halves of a plug-in
class IAttributeList : public FUnknown {        // implemented by the host
public:
    typedef const char* AttrID;
    virtual tresult PLUGIN_API setInt(AttrID id, int64_t value) = 0;
    virtual tresult PLUGIN_API getInt(AttrID id, int64_t& value) = 0;
    virtual tresult PLUGIN_API setFloat(AttrID id, double value) = 0;
    virtual tresult PLUGIN_API getFloat(AttrID id, double& value) = 0;
    virtual tresult PLUGIN_API setString(AttrID id, const TChar* string) = 0;
    virtual tresult PLUGIN_API getString(AttrID id, TChar* string, uint32_t sizeInBytes) = 0;
    virtual tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32_t sizeInBytes) = 0;
    virtual tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32_t& sizeInBytes) = 0;
};
RENO_VST3_IID(IAttributeList, 0x1E5F0AEB, 0xCC7F4533, 0xA2544011, 0x38AD5EE4)

class IMessage : public FUnknown {              // implemented by the host (made through IHostApplication::createInstance)
public:
    virtual FIDString PLUGIN_API getMessageID() = 0;
    virtual void PLUGIN_API setMessageID(FIDString id) = 0;
    virtual IAttributeList* PLUGIN_API getAttributes() = 0;
};
RENO_VST3_IID(IMessage, 0x936F033B, 0xC6C047DB, 0xBB0882F8, 0x13C1E613)

class IConnectionPoint : public FUnknown {
public:
    virtual tresult PLUGIN_API connect(IConnectionPoint* other) = 0;
    virtual tresult PLUGIN_API disconnect(IConnectionPoint* other) = 0;
    virtual tresult PLUGIN_API notify(IMessage* message) = 0;
};
RENO_VST3_IID(IConnectionPoint, 0x70A4156F, 0x6E6E4026, 0x989148BF, 0xAA60D8D1)

#pragma pack(pop)

// ------------------------------------------------------------------ what the host hands a plug-in when it starts it
class IHostApplication : public FUnknown {
public:
    virtual tresult PLUGIN_API getName(String128 name) = 0;
    virtual tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void** obj) = 0;
};
RENO_VST3_IID(IHostApplication, 0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5)

class IPlugInterfaceSupport : public FUnknown {     // also the host's: which of a plug-in's interfaces the host uses
public:
    virtual tresult PLUGIN_API isPlugInterfaceSupported(const TUID _iid) = 0;
};
RENO_VST3_IID(IPlugInterfaceSupport, 0x4FB58B9E, 0x9EAA4E0F, 0xAB361C1C, 0xCCB56FEA)

// ------------------------------------------------------------------ sizes, as a safety net (64-bit builds)
#if defined(_WIN64) || defined(__LP64__)
static_assert(sizeof(PFactoryInfo) == 452 && sizeof(PClassInfo) == 116 && sizeof(PClassInfo2) == 440 && sizeof(PClassInfoW) == 696, "VST3 factory struct layout");
static_assert(sizeof(BusInfo) == 276 && sizeof(RoutingInfo) == 12 && sizeof(ViewRect) == 16, "VST3 struct layout");
static_assert(sizeof(ProcessSetup) == 24 && sizeof(AudioBusBuffers) == 24 && sizeof(ProcessData) == 80, "VST3 process struct layout");
static_assert(sizeof(ProcessContext) == 112 && sizeof(ParameterInfo) == 792, "VST3 struct layout");
#endif

}  // namespace vst3
}  // namespace reno
