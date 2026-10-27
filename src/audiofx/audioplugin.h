// Audio effect plug-ins, whatever their kind (VST2, VST3, DirectX), behind one interface.
#pragma once
#include "../core/base.h"
#include "../core/json.h"
#include <vector>

namespace reno {

struct AudioPluginDesc {
    std::string format;         // "VST2", "VST3", "DX"
    std::string path;           // the file (VST2, VST3); empty for DirectX
    std::string uid;            // VST2: the four-character id as a number; VST3: the class id; DX: the CLSID
    std::string name, vendor;
    int numInputs = 2, numOutputs = 2;
    bool hasEditor = false;
    bool isInstrument = false;
    bool bridged32 = false;     // a 32-bit VST2 plug-in, run in a process of its own (reno_vst32.exe, see vst32client.cpp)

    std::string key() const { return format + "|" + path + "|" + uid; }       // what a project stores
    Json toJson() const;
    static AudioPluginDesc fromJson(const Json& j);
};

// The tempo and time signature of the project whose sound is being mixed, for the effects with tempo sync (VEGAS's plug-ins
// ask the host for them). The mixer puts them here (ProjectTempoScope, per thread) around every effect chain it runs;
// outside a mix they are 120 beats a minute in 4/4. Read it inside process().
struct ProjectTempo {
    double bpm = 120.0;
    int beatsPerMeasure = 4;
    int beatUnit = 4;
    bool operator==(const ProjectTempo& o) const { return bpm == o.bpm && beatsPerMeasure == o.beatsPerMeasure && beatUnit == o.beatUnit; }
    bool operator!=(const ProjectTempo& o) const { return !(*this == o); }
};
ProjectTempo projectTempo();
void setProjectTempo(double bpm, int beatsPerMeasure, int beatUnit = 4);     // this thread's, until the next call
class ProjectTempoScope {
public:
    ProjectTempoScope(double bpm, int beatsPerMeasure, int beatUnit = 4);
    ~ProjectTempoScope();
    ProjectTempoScope(const ProjectTempoScope&) = delete;
    ProjectTempoScope& operator=(const ProjectTempoScope&) = delete;
private:
    ProjectTempo prev_;
};

class AudioPlugin {
public:
    virtual ~AudioPlugin() {}
    AudioPluginDesc desc;

    // Audio. 'io' is stereo, processed in place. Call activate() before the first process(); may be called again
    // when the sample rate changes.
    virtual bool activate(double sampleRate, int maxBlock) = 0;
    virtual void deactivate() = 0;
    virtual void process(float* left, float* right, int frames, int64_t samplePos, bool playing) = 0;
    virtual int latencySamples() { return 0; }

    // Effects that make the sound longer or shorter (Time Stretch, Gapper/Snipper, Vibrato): a track's effect chain gives
    // and takes blocks of the same size, so process() lines what they deliver up in a queue. Tools > Audio > Apply
    // Non-Real-Time Event FX has no such limit: processWhole() runs a whole sound (from its start to its end) through a
    // fresh run of the effect, with its current settings, and returns everything the effect delivers, however long that
    // is. False (and nothing done) for the effects that give back what they are given. changesLength() says whether the
    // effect is of the first kind.
    virtual bool changesLength() { return false; }
    virtual bool processWhole(const float* inL, const float* inR, int64_t frames, std::vector<float>& outL, std::vector<float>& outR)
    {
        (void)inL; (void)inR; (void)frames; (void)outL; (void)outR;
        return false;
    }

    // Multichannel processing for a 5.1 project's master (L R C LFE Ls Rs, VST's and FFmpeg's 5.1 order).
    // setChannels(6) asks for six channels in and out as one stream (VST3: kSpeakerArr51 on the main buses; VST2: six
    // inputs and outputs) and returns whether the plug-in accepts; setChannels(2) goes back to stereo. It waits
    // for process() on another thread and restarts the plug-in. channels() is the current count (2 or 6).
    // processChannels() takes 'channels' buffers in place; a stereo plug-in gets the first two. DirectX plug-ins
    // stay stereo.
    virtual bool setChannels(int channels) { return channels == 2; }
    virtual int channels() const { return 2; }
    virtual void processChannels(float* const* io, int channels, int frames, int64_t samplePos, bool playing)
    {
        if (channels >= 2) process(io[0], io[1], frames, samplePos, playing);
    }

    // Parameters, all 0..1.
    virtual int paramCount() { return 0; }
    virtual std::string paramName(int) { return std::string(); }
    virtual std::string paramText(int) { return std::string(); }        // the value as the plug-in words it ("-6 dB")
    virtual float paramGet(int) { return 0.f; }
    virtual void paramSet(int, float) {}
    // The real range a setting's 0..1 stands for (dB, Hz, ms for the built-in effects' pages). False where the
    // plug-in does not say.
    virtual bool paramRange(int, double* /*lo*/, double* /*hi*/) { return false; }

    // Everything needed to restore the plug-in (stored in the project as text).
    virtual std::string saveState() { return std::string(); }
    virtual bool loadState(const std::string&) { return false; }

    // The plug-in's own programs (VST2 programs; shown in the effects window's Preset list).
    virtual int programCount() { return 0; }
    virtual std::string programName(int) { return std::string(); }
    virtual int currentProgram() { return -1; }
    virtual bool setProgram(int) { return false; }
    // VST preset files: one program (.fxp) or the whole bank (.fxb), as other VST hosts read and write them. Empty /
    // false (with *error set) where the plug-in has none (VST3, DirectX) or the file is not for it.
    virtual std::string saveVstPresetFile(bool bank) { (void)bank; return std::string(); }
    virtual bool loadVstPresetFile(const std::string& bytes, std::string* error) { (void)bytes; if (error) *error = "this kind of plug-in has no .fxp / .fxb files"; return false; }

    // The plug-in's own window, put inside 'parentWindow' (an HWND on Windows).
    virtual bool hasEditor() { return false; }
    virtual bool openEditor(void* /*parentWindow*/, int* /*width*/, int* /*height*/) { return false; }
    virtual void closeEditor() {}
    virtual void idle() {}                                              // call ~20 times a second while the window is open

    // Level meters a plug-in's page shows (Track Compressor, Multi-Band Dynamics). meterTake(i) is the biggest value
    // since the last call (peaks: linear amplitude; gain meters: the smallest gain, 1 = untouched), 0 when there was
    // no sound; it may be called from the interface thread while the sound runs.
    virtual int meterCount() { return 0; }
    virtual float meterTake(int /*i*/) { return 0.f; }

    // Non-empty once the plug-in has stopped working and passes sound through: why. E.g. a 32-bit plug-in's bridge
    // process crashed or hung; it restarts when playback next starts.
    virtual std::string stoppedReason() { return std::string(); }

    std::function<void(int w, int h)> onEditorResize;                   // the plug-in asked for a different window size
    std::function<void(int index)> onParamChangedByPlugin;              // moved in the plug-in's own window
};

// Reads what is in a plug-in file (loads it). One file can hold several plug-ins (VST3).
std::vector<AudioPluginDesc> describeAudioPluginFile(const std::string& path, std::string* error = nullptr);
// Makes an instance.
std::unique_ptr<AudioPlugin> createAudioPlugin(const AudioPluginDesc& desc, std::string* error = nullptr);
// Folders audio plug-ins are normally installed in on this machine, and the plug-in files in them.
std::vector<std::string> audioPluginSearchDirs();
std::vector<std::string> findAudioPluginFiles(const std::vector<std::string>& dirs);
// Folders VEGAS Pro searches for VST plug-ins beyond Windows' usual places: "MAGIX Plugins" in each installed VEGAS
// (its eFX effects are in MAGIX Plugins\essentialFX\x64) and the folders in VEGAS's Preferences > VST Effects.
// Part of audioPluginSearchDirs(). Empty where VEGAS is not installed (Linux).
std::vector<std::string> vegasPluginDirs();
// For an effect that is not installed: which file it is and where it is normally found, for the effects VEGAS Pro
// brings ("sfppack3_x64.dll, one of VEGAS's DirectX plug-ins in ...\\Shared Plug-Ins\\Audio_x64"); "" for any other.
std::string audioPluginFileHint(const AudioPluginDesc& desc);
// A VST2 id as its four characters ("MX23"), or the number where they are not printable.
std::string vstIdText(const std::string& uid);
// DirectX audio effects registered on this machine (Windows only; empty elsewhere).
std::vector<AudioPluginDesc> listDirectXAudioPlugins();

// 32-bit VST2 plug-ins cannot be loaded into this 64-bit program; they run in reno_vst32.exe, a 32-bit host process
// beside Reno, driven over shared memory and pipes (vst32client.cpp, src/vst32/vst32server.cpp).
// vst32Wanted: the file is a 32-bit VST2 plug-in (an x86 DLL exporting VSTPluginMain / main), or RENO_VST32_FORCE=1
// is set (every VST2 file goes through the bridge, to test it with 64-bit plug-ins).
bool vst32Wanted(const std::string& path);
std::vector<AudioPluginDesc> vst32Describe(const std::string& path, std::string* error);
std::unique_ptr<AudioPlugin> vst32Create(const AudioPluginDesc& desc, std::string* error);
// Where reno_vst32 is (beside the running program; RENO_VST32_SERVER overrides); "" if none.
std::string vst32ServerPath();
// The machine type of a PE executable (0x14c: x86, 0x8664: x64; 0: not a PE), and whether it exports a VST2 entry
// point (VSTPluginMain, main, MAIN).
struct PeKind { int machine = 0; bool exportsVst = false; bool exportsKnown = false; };
PeKind inspectPeFile(const std::string& path);
// " (32-bit, bridged)" for a plug-in run through the bridge, otherwise "".
std::string audioPluginKindNote(const AudioPluginDesc& desc);

// the individual hosts
std::vector<AudioPluginDesc> vst2Describe(const std::string& path, std::string* error);
std::unique_ptr<AudioPlugin> vst2Create(const AudioPluginDesc& desc, std::string* error);
std::vector<AudioPluginDesc> vst3Describe(const std::string& path, std::string* error);
std::unique_ptr<AudioPlugin> vst3Create(const AudioPluginDesc& desc, std::string* error);
std::unique_ptr<AudioPlugin> dxCreate(const AudioPluginDesc& desc, std::string* error);
// Reno's built-in audio effects (builtinaudio.cpp; format "BUILTIN", vendor kBuiltinAudioVendor): stand-ins for the
// VEGAS DirectX effects used most: Pitch Shift, Chorus, Flange/Wah-wah, Distortion, Vibrato, Amplitude Modulation,
// Simple Delay, Reverb. Always listed.
std::vector<AudioPluginDesc> builtinAudioPlugins();
std::unique_ptr<AudioPlugin> builtinAudioCreate(const AudioPluginDesc& desc, std::string* error);
extern const char* const kBuiltinAudioVendor;
// Reno's version of a VEGAS DirectX effect it reproduces exactly (now: Distortion), used for a "DX" slot when the
// DirectX one cannot be made (not Windows, VEGAS not installed). The plug-in keeps the slot's DX description with
// vendor kBuiltinAudioVendor. Empty uid: none.
std::string builtinUidForDx(const std::string& dxUid);
std::string dxUidForBuiltin(const std::string& builtinUid);     // the other way: "dshow:{...}" or ""
// The built-in effect whose page shows this plug-in ("reno.distortion"...), or "" for one with no page.
std::string builtinPageUid(const AudioPluginDesc& desc);

// Reno's versions of VEGAS's MAGIX essentialFX effects (efx.h; Limiter, Compressor, Reverb, Tube Stage, Phaser,
// Chorus/Flanger). The real ones are VST2 plug-ins that only run unrestricted inside VEGAS, so a slot whose VST2 id
// is one of theirs gets Reno's version (same 31 settings, same saved form, still a VST2 slot with the same id)
// unless the preference "VEGAS's eFX effects" says to use the installed plug-ins.
bool efxPreferBuiltin();
void setEfxPreferBuiltin(bool on);
bool efxReproducedId(const std::string& vst2Uid);                   // 'MX2A' (as a number) and the other five ids
std::unique_ptr<AudioPlugin> efxCreate(const AudioPluginDesc& desc, std::string* error);
bool efxIsReproduction(const AudioPluginDesc& desc);                // a plug-in efxCreate made
extern const char* const kEfxVendor;

}  // namespace reno
