// OpenFX image-effect host: loads .ofx files, describes their effects, makes instances and renders frames.
// It presents itself to plug-ins the way VEGAS Pro does (host name, blue-first 8-bit frames, the VEGAS suites, the effect's own
// window at the top of its panel), because VEGAS and NewBlue effects check for it. All of it can be switched off in HostConfig.
#pragma once
#include "../core/base.h"
#include "../core/json.h"
#include "../core/dynlib.h"
#include "ofxCore.h"
#include "ofxProperty.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "rendergate.h"
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace reno {
namespace ofx {

// ------------------------------------------------------------------ property sets
enum class PType { None, Int, Double, String, Pointer };

struct Property {
    PType type = PType::None;
    std::vector<int> i;
    std::vector<double> d;
    std::vector<std::string> s;
    std::vector<void*> p;
    int dim() const;
};

class PropertySet {
public:
    static const uint32_t kMagic = 0x50724f70;
    uint32_t magic = kMagic;
    void* owner = nullptr;                                        // whatever this set belongs to (not copied)
    std::function<void(const std::string& name)> onChanged;

    PropertySet() {}
    PropertySet(const PropertySet& o);
    PropertySet& operator=(const PropertySet& o);
    ~PropertySet() { magic = 0; }

    void setInt(const std::string& name, int v, int idx = 0);
    void setDouble(const std::string& name, double v, int idx = 0);
    void setString(const std::string& name, const std::string& v, int idx = 0);
    void setPointer(const std::string& name, void* v, int idx = 0);
    int getInt(const std::string& name, int idx = 0, int def = 0) const;
    double getDouble(const std::string& name, int idx = 0, double def = 0) const;
    std::string getString(const std::string& name, int idx = 0, const std::string& def = std::string()) const;
    void* getPointer(const std::string& name, int idx = 0) const;
    int dimension(const std::string& name) const;
    bool has(const std::string& name) const;
    void erase(const std::string& name);
    void resetDim(const std::string& name, int n);

    Property* find(const std::string& name);
    const Property* find(const std::string& name) const;
    Property& ensure(const std::string& name, PType t);
    std::vector<std::string> names() const;

private:
    std::unordered_map<std::string, Property> props_;
};

// ------------------------------------------------------------------ parameters
enum class ParamKind { Int, Double, Bool, Choice, RGBA, RGB, Double2D, Int2D, Double3D, Int3D, String, Custom, Group, Page, PushButton, Unknown };

class ParamSet;
class EffectBase;

struct Param {
    static const uint32_t kMagic = 0x5061726d;
    uint32_t magic = kMagic;
    ParamSet* set = nullptr;
    std::string name, type;
    ParamKind kind = ParamKind::Unknown;
    int dim = 0;
    PropertySet props;
    std::vector<double> value;
    std::string str;                                // String / Custom
    std::map<double, std::vector<double>> keys;
    std::map<double, std::string> strKeys;
    // Interpolation from a keyframe to the next (VEGAS's types; keys not listed: Linear). Values are the engine's fade curves (src/engine/project.h):
    // 0 Linear, 1 Fast, 2 Slow, 3 Smooth, 4 Sharp, 5 Hold.
    enum KeyCurve { KeyLinear = 0, KeyFast = 1, KeySlow = 2, KeySmooth = 3, KeySharp = 4, KeyHold = 5 };
    std::map<double, int> keyCurves;
    static std::atomic<int>& newKeyCurve()
    {
        static std::atomic<int> c{0};
        return c;
    }
    int keyCurve(double time) const { auto it = keyCurves.find(time); return it == keyCurves.end() ? KeyLinear : it->second; }

    bool hasValue() const { return dim > 0 || kind == ParamKind::String || kind == ParamKind::Custom; }
    bool isWhole() const { return kind == ParamKind::Int || kind == ParamKind::Bool || kind == ParamKind::Choice || kind == ParamKind::Int2D || kind == ParamKind::Int3D; }
    std::vector<double> valueAt(double time) const;
    std::string strAt(double time) const;
    bool isText() const { return kind == ParamKind::String || kind == ParamKind::Custom; }
    size_t keyCount() const { return isText() ? strKeys.size() : keys.size(); }
    std::string label() const;
    std::string hint() const { return props.getString(kOfxParamPropHint); }
    std::string parent() const { return props.getString(kOfxParamPropParent); }
    bool secret() const { return props.getInt(kOfxParamPropSecret) != 0; }
    bool enabled() const { return props.getInt(kOfxParamPropEnabled, 0, 1) != 0; }
    std::vector<std::string> choices() const;
    ~Param() { magic = 0; }
};

class ParamSet {
public:
    static const uint32_t kMagic = 0x50536574;
    uint32_t magic = kMagic;
    EffectBase* owner = nullptr;
    PropertySet props;
    std::vector<std::unique_ptr<Param>> params;

    Param* define(const std::string& type, const std::string& name);
    Param* find(const std::string& name) const;
    void cloneFrom(const ParamSet& o);
    ~ParamSet() { magic = 0; }

private:
    std::unordered_map<std::string, Param*> byName_;
};

// ------------------------------------------------------------------ effects
class Plugin;
class Instance;
struct HwndInteractImpl;

struct ClipDesc {
    std::string name;
    PropertySet props;
};

struct ClipInst {
    static const uint32_t kMagic = 0x436c6970;
    uint32_t magic = kMagic;
    Instance* inst = nullptr;
    std::string name;
    PropertySet props;
    bool isOutput = false;
    bool optional = false;
    std::function<ImagePtr(double time)> source;
    ~ClipInst() { magic = 0; }
};

class EffectBase {
public:
    static const uint32_t kMagic = 0x45666678;
    uint32_t magic = kMagic;
    bool isInstance = false;
    Plugin* plugin = nullptr;
    PropertySet props;
    ParamSet params;
    EffectBase() { params.owner = this; }
    virtual ~EffectBase() { magic = 0; }
};

class Descriptor : public EffectBase {
public:
    std::vector<std::unique_ptr<ClipDesc>> clips;
    ClipDesc* findClip(const std::string& name) const;
};

struct ImageHandle {
    PropertySet props;
    ImagePtr img;
    bool released = false;
};

struct InstanceSetup {
    int width = 1920, height = 1080;
    double fps = 30.0;
    double durationFrames = 1.0e6;
    std::string vegasContext;           // one of kVegasContext...; empty = unknown
};

struct RenderOptions {
    bool interactive = true;
    bool draft = false;
    PixelDepth depth = PixelDepth::Byte;
    // Position on the timeline in project frames (VEGAS's OfxPropVegasTimeCode); below 0: same as the effect's own time.
    double timelineTime = -1;
    // Set by another thread when the frame being drawn is no longer wanted; the effect's abort() reports it. Null: never.
    const std::atomic<bool>* abort = nullptr;
    // Size of the frames the effect works on, in pixels (0: the project's size). Honoured only for effects that support multi-resolution
    // (Instance::scalable()); the others work at the project's size and the caller stretches their frames. Input frames from the sources must be
    // this size; the render scale told to the effect is width / project width.
    int width = 0, height = 0;
    // Also give effects WITHOUT multi-resolution support frames of width x height (a preview): they are told the project is that size at render
    // scale 1, and canonical coordinates (X / Y / XY double settings) are scaled to it as they read them, as VEGAS does in smaller previews.
    // A final render leaves it off.
    bool previewSizeForAll = false;
};

class Instance : public EffectBase {
public:
    ~Instance() override;

    const std::string& context() const { return context_; }
    bool scalable() const;
    void setProjectSizeProps(int w, int h);
    double renderScale[2] = {1.0, 1.0};
    int canonW = 0, canonH = 0;
    double canonScale[2] = {1.0, 1.0};
    int canonicalWidth() const { return canonW > 0 ? canonW : setup.width; }
    int canonicalHeight() const { return canonH > 0 ? canonH : setup.height; }
    PixelDepth depth() const { return depth_; }

    void setClipSource(const std::string& clip, std::function<ImagePtr(double time)> fn);
    ClipInst* findClip(const std::string& name) const;
    std::vector<std::string> inputClipNames() const;

    // Renders one frame (time in frames). Returns null and sets *error if the effect failed.
    // If the effect says it changes nothing at this time, the source frame is returned as it is.
    ImagePtr render(double time, const RenderOptions& opt, std::string* error = nullptr);

    // Changes made by the user (the effect is told, as OpenFX requires). 'time' is where the cursor is, in frames.
    void userSetValue(Param* p, const std::vector<double>& v, double time, bool setKey = false);
    void userSetString(Param* p, const std::string& s, double time, bool setKey = false);
    void userPushButton(Param* p, double time);
    void userDeleteKeys(Param* p, double time);
    // The keyframe strip of the effects window (VEGAS's Animate): one keyframe of a number setting moved to another
    // time (one already there is replaced), its interpolation to the next key set (Param::KeyCurve), one keyframe
    // deleted (the last one gone, the setting keeps its value). False if there is no key at 'time'.
    bool userMoveKey(Param* p, double time, double to);
    bool userSetKeyCurve(Param* p, double time, int curve);
    bool userDeleteKey(Param* p, double time);
    void beginEdit();
    void endEdit();

    Json saveState();
    void loadState(const Json& state);

    // Called (on whatever thread made the change) when a parameter's value or one of its properties changed.
    std::function<void(Param* p, const std::string& what)> onParamChanged;
    std::atomic<uint64_t> revision{1};

    // The effect's own window at the top of its panel, if it supplies one (VEGAS's "HWnd interact").
    bool hasHwndInteract() const;
    bool createHwndInteract(void* parentWindow, int* minW, int* minH, int* prefW, int* prefH);
    void moveHwndInteract(int x, int y, int w, int h);
    void disposeHwndInteract();
    // The effect's "About" and "Help" (VEGAS extension). False if the effect has none. VEGAS's own effects answer
    // by asking the host to show them (OfxVegasEffectSuite: Host::onEffectRequest).
    bool invokeAbout();
    bool invokeHelp();

    // The effect's on-screen controls over the preview (OpenFX overlay interact): the host draws the frame, makes an OpenGL context current with the
    // projection set so canonical coordinates (project frame pixels from the bottom left) land on the frame, and asks the effect to draw; mouse and key
    // events over the frame go to it first. Settings it changes reach onParamChanged as usual. Runs on the interface thread with the instance locked.
    struct OverlayArgs {
        double time = 0;
        double renderScale[2] = {1, 1};
        double pixelScale[2] = {1, 1};
        int viewport[2] = {0, 0};
        double background[3] = {0, 0, 0};
    };
    bool hasOverlay() const;
    bool createOverlay();
    bool overlayMade() const { return overlay_ != nullptr; }
    void disposeOverlay();
    // Each returns true if the effect took the event (kOfxStatOK); false if it ignored it or failed. An effect
    // whose overlay fails (kOfxStatFailed, kOfxStatErrFatal or a crash) has it disposed of, as OpenFX asks.
    bool overlayDraw(const OverlayArgs& a);
    bool overlayPen(const char* action, const OverlayArgs& a, double x, double y, double pressure, int viewX, int viewY);
    bool overlayKey(const char* action, const OverlayArgs& a, int keySym, const std::string& keyString);
    bool overlayFocus(bool gained, const OverlayArgs& a);
    PropertySet* overlayProps();                // the overlay's own property set (null if it is not made)
    // The effect asked for its overlay to be drawn again (the interact suite's interactRedraw). May be called on
    // any thread.
    std::function<void()> onOverlayRedraw;

    // A parameter's own control drawn by the effect (OpenFX custom parameter interact, kOfxParamPropInteractV1), shown in the effects window in place
    // of the usual control. The OpenGL context is current with the projection set to the control's pixels ((0, 0) at its bottom left); mouse and key
    // events over the control go to it. Interface thread, instance locked. A failing interact is disposed of.
    struct ParamInteractArgs {
        double time = 0;                        // in frames
        int size[2] = {0, 0};
        double pixelScale[2] = {1, 1};
        double background[3] = {0, 0, 0};
    };
    bool paramHasInteract(const Param* p) const;
    void paramInteractSize(const Param* p, int preferred[2], int minimum[2], double* aspect) const;
    bool createParamInteract(Param* p);
    bool paramInteractMade(const Param* p) const;
    void disposeParamInteract(Param* p);
    bool paramInteractDraw(Param* p, const ParamInteractArgs& a);
    bool paramInteractPen(Param* p, const char* action, const ParamInteractArgs& a, double x, double y, double pressure);
    bool paramInteractKey(Param* p, const char* action, const ParamInteractArgs& a, int keySym, const std::string& keyString);
    bool paramInteractFocus(Param* p, bool gained, const ParamInteractArgs& a);
    // The effect asked for a parameter's control to be drawn again. May be called on any thread.
    std::function<void(Param* p)> onParamInteractRedraw;

    std::recursive_mutex mutex;
    // The OpenGL render going on (gpurender.h's GpuRender), null otherwise: what the OpenGL render suite's functions find.
    std::atomic<void*> gpuFrame{nullptr};
    double currentTime = 0;
    InstanceSetup setup;

    std::vector<std::unique_ptr<ClipInst>> clips;
    std::vector<std::unique_ptr<ImageHandle>> liveImages;
    std::mutex imageMutex;
    ImagePtr outputImage;
    std::atomic<bool> abort_{false};
    std::atomic<const std::atomic<bool>*> renderAbort{nullptr};
    bool aborted() const { const std::atomic<bool>* a = renderAbort.load(); return abort_.load() || (a && a->load()); }
    int changeDepth = 0;
    bool clipPrefsDirty = true;
    std::string persistentMessage;

private:
    friend class Plugin;
    std::string context_;
    PixelDepth depth_ = PixelDepth::Byte;
    PixelDepth preferred_ = PixelDepth::Byte;
    bool created_ = false;
    std::unique_ptr<HwndInteractImpl> hwnd_;
    std::unique_ptr<HwndInteractImpl> overlay_, overlayDesc_;
    OfxStatus overlayCall(const char* action, PropertySet* in);
    std::map<const Param*, std::unique_ptr<HwndInteractImpl>> paramInteracts_;
    OfxStatus paramInteractCall(Param* p, const char* action, PropertySet* in);
    void runClipPreferences();
    void changed(Param* p, const char* reason, double time);
    void setupClipProps();
};

struct PluginInfo {
    std::string id;
    int versionMajor = 0, versionMinor = 0;
    std::string label, grouping, description;
    std::string binaryPath, bundlePath;
    std::vector<std::string> contexts;
    std::vector<std::string> depths;
    bool hasHwndInteract = false;
    bool hasOverlay = false;
    bool multiResolution = false;
    // The GUIDs of the older VEGAS plug-ins this effect takes the place of (kVegasPropUpliftGUID), as it gave them.
    std::vector<std::string> upliftGuids;
};

class Binary;

class Plugin {
public:
    Binary* binary = nullptr;
    OfxPlugin* ofx = nullptr;
    PluginInfo info;
    bool loaded = false, failed = false, crashed = false;
    std::atomic<bool> glFailed{false};
    // It answered kOfxStatErrMissingHostFeature when described in a context or made: it loads but cannot be used here, and a built-in stand-in of
    // the same identifier is preferred (Host::findPlugin).
    bool unusable = false;      // loaded / failed change under loadMutex; use load() to read them while another thread may be loading
    std::string error;
    std::vector<std::string> suiteLog;
    std::vector<std::string> missingLog;
    // Last time the plug-in answered kOfxStatErrMissingHostFeature: the action, and the last suite and property it asked for that Reno could not give. Empty if never.
    std::string missingFeature;
    std::string explainStatus(OfxStatus st) const;

    bool load(std::string* err = nullptr);
    Descriptor* describeInContext(const std::string& context, std::string* err = nullptr);
    std::unique_ptr<Instance> createInstance(const std::string& context, const InstanceSetup& setup, std::string* err = nullptr);
    bool supportsContext(const std::string& context) const;
    std::string filterContext() const;

    OfxStatus call(const char* action, const void* handle, PropertySet* in, PropertySet* out);
    std::unique_ptr<Descriptor> desc;
    std::map<std::string, std::unique_ptr<Descriptor>> contextDescs;
    RenderGate renderGate;
    std::recursive_mutex loadMutex;                 // describing is done once, by one thread
    std::mutex suiteLogMutex;                       // guards suiteLog
    int safety = 1;
};

class Binary {
public:
    std::string path, bundlePath;
    DynLib lib;
    bool builtin = false;               // Reno's own plug-ins (builtins.h), not a file
    std::vector<std::unique_ptr<Plugin>> plugins;
};

struct HostConfig {
    // VEGAS Pro 18's host name (vegas180.exe's CSvfxPropertySetHost). A dozen of its stock effects (Test Pattern, Border, Crop, Picture in Picture,
    // Channel Blend, LUT Filter, ...) only declare how they can be used under this name; VEGAS 13 and older ("com.sonycreativesoftware.vegas") can be
    // chosen in Preferences for older third-party effects.
    std::string hostName = "com.vegascreativesoftware.vegas";
    std::string hostLabel = "Reno";
    int versionMajor = 18, versionMinor = 0;
    bool vegasExtensions = true;        // the VEGAS suites, properties and blue-first 8-bit frames
    bool stubUnknownSuites = false;
    // VEGAS's own effects (isVegasOwnEffect) get the multithread suite as VEGAS Pro 18 has it (work done on one thread), not Reno's real multi-threaded
    // one (some of them draw wrongly when split)
    bool vegasThreading = true;
    // Render effects with OpenGL on the graphics card where they can, as VEGAS does (Preferences > Video: "GPU acceleration of video processing";
    // reno_cli --gpu on|off): the host then reports kOfxImageEffectPropOpenGLRenderSupported "true" when a context can be made (call applyConfig after changing).
    bool gpu = true;
    // For VEGAS's own effects (isVegasOwnEffect) that Reno has a built-in reproduction of: true (default) always uses the built-in and ignores VEGAS's
    // installed plug-in; false uses VEGAS's plug-in when installed and working. Effects Reno has no version of always use the installed plug-in.
    // Preferences > Video, reno_cli --vegas-fx real|builtin. Effects already made keep their choice until FxRuntime::prepare makes them again.
    bool preferBuiltinVegasFx = true;
    // Previews at a smaller size: effects without multi-resolution work at the preview's size too, as VEGAS does (RenderOptions::previewSizeForAll).
    // False (Preferences > Video, reno_cli --full-size-fx): they work at the project's size in previews as well.
    bool previewSizeForAll = true;
    void* mainWindow = nullptr;
    bool verbose = false;
};

class Host {
public:
    static Host& get();
    HostConfig config;
    PropertySet hostProps;
    OfxHost ofxHost;

    void applyConfig();
    Binary* loadBinary(const std::string& path, std::string* err = nullptr);
    Plugin* findPlugin(const std::string& id) const;
    std::vector<Plugin*> allPlugins() const;

    // The folders effects are looked for in on this machine (the standard OpenFX folders, OFX_PLUGIN_PATH,
    // and the OFX folders of any VEGAS installs found), and the .ofx files in them.
    static std::vector<std::string> defaultSearchDirs();
    static std::vector<std::string> findBinaries(const std::vector<std::string>& dirs);
    // Program folders of VEGAS installs (their effects need DLLs from there).
    static std::vector<std::string> vegasProgramDirs();

    // An effect asked the host to show its Help or About (OfxVegasEffectSuite; 'action' is kVegasActionShowEffectHelp or ...ShowEffectAbout; 'effect' is its
    // instance or descriptor). Called on the effect's calling thread. Without a handler the request is only logged.
    std::function<void(EffectBase* effect, const std::string& action)> onEffectRequest;

    static thread_local Plugin* currentPlugin;  // the plug-in this thread is loading (for the suite log)
    mutable std::mutex mutex;
    std::mutex loadBinaryMutex;         // one binary is loaded by one thread at a time

private:
    Host();
    std::vector<std::unique_ptr<Binary>> binaries_;
};

const char* statusName(OfxStatus s);
// Whether an effect identifier is one of VEGAS's own effects (VEGAS's, Sony's and MAGIX's stock OpenFX plug-ins).
bool isVegasOwnEffect(const std::string& id);

}  // namespace ofx
}  // namespace reno
