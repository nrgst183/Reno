// One run of a VEGAS script (Tools > Scripting > Run Script): the script's view of the project.
#pragma once
#include "project.h"
#include "scriptapi.h"
#include <map>
#include <set>
#include <tuple>

namespace reno {

// An installed plug-in as scripts see it (VEGAS's PlugInNode under Vegas.VideoFX, AudioFX, Transitions, Generators).
struct ScriptPlugin {
    enum Type { VideoFilter = 9, Transition = 11, Generator = 13, Audio = 2 };     // VEGAS's PlugInType numbers
    Type type = VideoFilter;
    std::string id;             // OpenFX identifier (video) or AudioPluginDesc::key() (audio)
    std::string name, group;
    AudioPluginDesc audio;      // (audio)
};

// An OpenFX effect's parameter as scripts see it (VEGAS's OFXParameter), read from the plug-in by the program.
struct ScriptOfxParam {
    std::string name, label, hint, parent;
    int type = 0;                               // VEGAS's OFXParameterType (11 Double, 8 Integer, 4 Boolean, 5 Choice...)
    int dim = 0;                                // how many numbers its value has (0: text, a group, a page, a button)
    bool whole = false;                         // integers, booleans and choices
    std::vector<double> def, min, max, displayMin, displayMax;
    std::string defText;
    std::vector<std::string> choices;
    bool enabled = true, secret = false, canAnimate = true, persist = true;
};
struct ScriptOfxDesc {
    std::string label, group, path;
    std::vector<ScriptOfxParam> params;         // in the order the effect defines them
};

// An OpenFX effect preset as scripts see it: its name and the effect settings it holds (FxSlot::state's form).
struct ScriptPreset {
    std::string name;
    Json state;
};

// Render As's formats and templates as scripts see them (VEGAS's Renderer and RenderTemplate).
struct ScriptRenderTemplate {
    std::string name, description, ext;     // ext: the file name extension where the template has its own ("" the format's)
    bool video = true, audio = true;
    int width = 0, height = 0;              // 0: the project's
    int fpsNum = 0, fpsDen = 1;
    int sampleRate = 0, channels = 2, bits = 16, videoKbps = 0, audioKbps = 0;
    double par = 1.0;
};
struct ScriptRenderer {
    std::string name, ext, description;
    bool hasVideo = true, hasAudio = true;
    std::vector<ScriptRenderTemplate> templates;
};
// One Vegas.Render call: format and template by their places in ScriptHost::renderers.
struct ScriptRenderRequest {
    int renderer = 0, tmpl = 0;
    std::string output;
    double from = 0, to = 0;               // seconds
    bool markers = false, useRotation = false;
};

// What the session needs from the program running it. Everything has a usable default (tests set what they need).
struct ScriptHost {
    std::string version = "Version 18.0 (Build 527) Reno";   // (VEGAS's form, which scripts parse)
    std::string appName = "Reno";
    std::string installDir, appDataDir, tempDir;
    std::string scriptFile, scriptArgs;
    std::string projectPath;                    // the project's file ("" untitled)
    // Reads a media file's description (for adding media). Default: none can be read.
    std::function<bool(const std::string& path, MediaInfo* info, std::string* error)> probe;
    std::vector<ScriptPlugin> plugins;
    ProjectSettings newSettings;                // what Vegas.NewProject starts with
    bool loopEvents = true;                     // new events loop (Preferences)
    double stillLength = 5.0;                   // what a still frame's Media.Length says
    // Vegas.SaveProject / Project.SaveProject: saves the project (as it is now in the script) at 'path'.
    std::function<bool(const Project& p, const std::string& path, std::string* error)> save;
    // Vegas.OpenProject: reads a project file into *p.
    std::function<bool(const std::string& path, Project* p, std::string* error)> open;
    // AudioEvent.Normalize: the loudest sample of an event's media between where it begins and ends (0: unknown).
    std::function<double(const Project& p, const Event& e)> eventPeak;
    std::function<void(const std::string&)> log;  // Vegas.DebugOut
    int64_t mainWindow = 0;                     // the main window's HWND (Windows), the owner of the script's dialogs
    // OFXEffect.Parameters: the parameters of the OpenFX effect 'pluginId' in 'context' ("Filter", "Transition",
    // "Generator").
    std::function<bool(const std::string& pluginId, const std::string& context, ScriptOfxDesc* out, std::string* error)> describeOfx;
    // Render As's formats (those the program can make). Default: none.
    std::function<std::vector<ScriptRenderer>()> renderers;
    // Vegas.Render: renders the project (as the script has it now) and returns VEGAS's RenderStatus (4 Complete, 5 Canceled,
    // 6 Failed with *error saying why).
    std::function<int(const Project& p, const ScriptRenderRequest& r, std::string* error)> render;
    // The presets of an OpenFX plug-in in 'context' ("Filter", "Transition", "Generator"), in VEGAS's order.
    std::function<std::vector<ScriptPreset>(const std::string& pluginId, const std::string& context)> presets;
};

class ScriptSession {
public:
    ScriptSession(const Project& p, ScriptHost host);
    ~ScriptSession();
    ScriptSession(const ScriptSession&) = delete;
    ScriptSession& operator=(const ScriptSession&) = delete;

    // The table of functions for the script's side (valid as long as the session).
    const RenoScriptApi* api() { return &api_; }

    // ---- the script's state (set these before the run from the editor; read them after)
    Project project;
    std::set<uint64_t> selection;               // selected events
    std::set<uint64_t> selectedTracks;
    double cursor = 0;
    double selStart = 0, selLength = 0;         // the time selection (Reno's loop region)
    bool loopOn = false;
    bool bypassFx = false;

    // ---- what happened
    bool changed = false;                       // the project was changed
    bool viewChanged = false;                   // the selection, cursor or time selection was changed
    bool startedNew = false;                    // Vegas.NewProject was called (the project is a new one)
    std::string openedPath;                     // Vegas.OpenProject opened this file
    std::vector<std::string> savedPaths;        // where the project was saved during the run
    bool finished = false, failed = false;      // the script's side reported the end (finish)
    std::string message;                        // why it failed
    std::vector<std::string> output;            // DebugOut lines
    std::string undoLabel;                      // what the run's undo step is called (VEGAS's UndoBlock label; "" none given)
    bool cancelled = false;                     // UndoBlock.Cancel: the project is to be left as it was
    std::set<uint64_t> fxStatesChanged;         // effects whose settings (FxSlot::state) the script changed: the plug-ins
                                                // already running them must take the new settings

    // ---- the C functions' work (public for tests)
    int getI(uint64_t h, int prop, int64_t* out);
    int getD(uint64_t h, int prop, double* out);
    int getS(uint64_t h, int prop, std::string* out);
    int setI(uint64_t h, int prop, int64_t v);
    int setD(uint64_t h, int prop, double v);
    int setS(uint64_t h, int prop, const std::string& v);
    int list(uint64_t h, int which, std::vector<uint64_t>* out);
    int call(uint64_t h, int op, const std::vector<int64_t>& ia, const std::vector<double>& da, const std::string& s, uint64_t* out,
             double* dout);
    int kindOf(uint64_t h) const;
    const std::string& lastError() const { return error_; }
    int failWith(const std::string& why) { return fail(why); }     // (the C functions: a C++ exception becomes a failure)

    // The handle of an object (made if it has none yet).
    uint64_t trackHandle(uint64_t id) { return handle({RK_TRACK, id, 0, 0, ""}); }
    uint64_t eventHandle(uint64_t id) { return handle({RK_EVENT, id, 0, 0, ""}); }
    uint64_t mediaHandle(uint64_t id) { return handle({RK_MEDIA, id, 0, 0, ""}); }

    // VEGAS's numbers for Reno's names (exposed for tests).
    static int vegasCurve(int renoCurve);
    static int renoCurve(int vegasCurve);
    static int vegasComposite(const std::string& mode);
    static std::string renoComposite(int vegasMode);

private:
    struct Desc {
        int kind = RK_NONE;
        uint64_t a = 0, b = 0;      // ids (see handle kinds in scriptsession.cpp)
        int64_t c = 0;              // an index or a type
        std::string s;
        bool operator<(const Desc& o) const { return std::tie(kind, a, b, c, s) < std::tie(o.kind, o.a, o.b, o.c, o.s); }
    };
    uint64_t handle(const Desc& d);
    const Desc* desc(uint64_t h) const;
    int fail(const std::string& why);
    int unsupported();

    // lookups (null with the error set if the object is gone)
    Track* track(const Desc& d);
    Event* event(const Desc& d, Track** tr = nullptr);
    MediaItem* media(uint64_t id);
    Project::Marker* marker(const Desc& d);
    Bus* bus(const Desc& d);
    FxSlot* fx(const Desc& d, FxChain** chain = nullptr, int* index = nullptr);
    Envelopes* envelopesOf(int ownerKind, uint64_t ownerId);
    Envelope* envelope(const Desc& d);
    FxChain* chainOf(const Desc& owner);
    bool envOwnerOf(const Desc& d, int* ownerKind, uint64_t* ownerId);

    int eventProp(const Desc& d, int prop, int64_t* i, double* x, std::string* s, const int64_t* si, const double* sx, const std::string* ss);
    int takeOp(Event& e, int op, const std::vector<int64_t>& ia, const std::string& s, uint64_t* out);
    uint64_t addFx(FxChain& chain, int pluginIndex, int at, bool video);
    double takeOffset(const Event& e, const MediaItem* m) const;
    void setTakeOffset(Event& e, const MediaItem* m, double vegasOffset);
    double mediaLength(const MediaItem& m) const;
    int markerKind(const Project::Marker& m) const;
    std::vector<Project::Marker*> markersOfKind(int kind);
    std::vector<uint64_t> groupIds() const;

    // scriptsession_ext.cpp: the project's HDR and stereoscopic 3D settings, track groups and media bins. Each returns
    // kNotMine when the object or property is not one of these, so the callers carry on.
    static const int kNotMine = -1000;
    int extGetI(const Desc& d, int prop, int64_t* out);
    int extGetD(const Desc& d, int prop, double* out);
    int extGetS(const Desc& d, int prop, std::string* out);
    int extSetI(const Desc& d, int prop, int64_t v);
    int extSetD(const Desc& d, int prop, double v);
    int extSetS(const Desc& d, int prop, const std::string& v);
    int extList(const Desc& d, int which, std::vector<uint64_t>* out);
    int extCall(const Desc& d, int op, const std::vector<int64_t>& ia, const std::vector<double>& da, const std::string& s, uint64_t* out, double* dout);
    const std::vector<ScriptPreset>& presetsOf(size_t pluginIndex);
    const std::vector<ScriptRenderer>& renderersOf();
    std::vector<ScriptRenderer> renderers_;
    bool renderersRead_ = false;
    std::map<size_t, std::vector<ScriptPreset>> presets_;      // by plug-in (read once per run)
    std::vector<uint64_t> trackGroupIds() const;
    std::vector<std::string> binPaths() const;           // every bin's path (the ones kept, the ones media are in, their parents)
    bool binExists(const std::string& path) const;
    void touch() { changed = true; }

    // OpenFX parameters (scriptsession.cpp, "OpenFX parameters")
    struct OfxValue {
        std::vector<double> v;                          // the value (numbers)
        std::string s;                                  // the value (text)
        std::map<double, std::vector<double>> keys;     // keyframes by time in frames from the start of what the effect is on
        std::map<double, int> curves;                   // their interpolation (Reno's KeyCurve), when not linear
        std::map<double, std::string> textKeys;
    };
    const ScriptOfxDesc* ofxDesc(const FxSlot& s, std::string* error);
    const ScriptOfxParam* ofxParam(const Desc& d, FxSlot** slot);
    OfxValue ofxRead(const FxSlot& s, const ScriptOfxParam& p) const;
    void ofxWrite(FxSlot& s, const ScriptOfxParam& p, const OfxValue& v);
    double ofxFps() const;
    std::vector<double> ofxKeyTimes(const OfxValue& v, const ScriptOfxParam& p) const;
    int ofxGetI(const Desc& d, int prop, int64_t* out);
    int ofxGetD(const Desc& d, int prop, double* out);
    int ofxGetS(const Desc& d, int prop, std::string* out);
    int ofxSetI(const Desc& d, int prop, int64_t v);
    int ofxSetD(const Desc& d, int prop, double v);
    int ofxSetS(const Desc& d, int prop, const std::string& v);
    int ofxList(const Desc& d, int which, std::vector<uint64_t>* out);
    int ofxCall(const Desc& d, int op, const std::vector<int64_t>& ia, const std::vector<double>& da, const std::string& s, uint64_t* out,
                double* dout);
    std::map<std::string, ScriptOfxDesc> ofxDescs_;    // by "<plug-in>|<context>" (read once per run)
    std::map<uint64_t, double> ofxTime_;               // OFXEffect.CurrentTime of each effect (seconds)

    ScriptHost host_;
    RenoScriptApi api_{};
    std::vector<Desc> descs_;               // handle - 1 -> what it is
    std::map<Desc, uint64_t> handles_;
    std::set<uint64_t> emptyGroups_;        // groups made by the script with no events yet
    std::set<uint64_t> reversedMedia_;      // subclips the script made reversed (VEGAS's Subclip IsReversed)
    std::string error_;
};

}  // namespace reno
