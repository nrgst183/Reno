// Reno's versions of the DirectX effects in VEGAS's sfppack1_x64.dll: Simple Delay, Multi-Tap Delay, Chorus, Pitch
// Shift, Reverb and Time Stretch. Each keeps VEGAS's settings, presets and saved form (what its IPersistStream writes:
// u32 length, a GUID, the plug-in's own settings structure), and its sound is the plug-in's own code translated line by
// line (same double math in the same order), so a project made with the VEGAS effect sounds the same here and
// the effect saves back as the VEGAS one.
//
// One generic plug-in class (vegaspack1.cpp) serves all of them; an effect is a table of its settings (the way VEGAS
// describes them to a host: name, range, default, unit, where in the structure) plus an Engine with the DSP.
#pragma once
#include "audioplugin.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reno {
namespace vegpack1 {

// What the host knows about the project's tempo (a "Tempo sync" setting uses it). flags as in VEGAS's block: 1 = bpm is
// valid, 4 = the time signature (num / den) is valid.
struct Tempo {
    uint32_t flags = 5;
    double bpm = 120.0;
    int32_t num = 4, den = 4;
};
Tempo tempo();
void setTempo(double bpm, int beatsPerMeasure);     // the project's tempo, for every pack1 effect

enum Kind { Num, Bool, Choice, Dbl };

// One automatable setting, in the order VEGAS lists them. The plug-in stores it as an int at settings[slot]: the value
// times 'scale' (dB in tenths: 10; Hz: 1; seconds in tenths: 10...).
struct ParamDesc {
    const char* name;
    double lo, hi, def;
    const char* unit;                // "dB", "ms"...; "" for none
    int slot;
    int scale;
    Kind kind;
    const char* choices;             // Choice: names separated by commas (Dbl: a double kept in slot and slot + 1; the setting is that double divided by 'scale')
    int decimals;                    // digits shown after the point
    bool infAtMin;                   // the lowest value reads "-Inf."
    const char* atMaxText = nullptr; // the highest value reads this instead of its number ("Inf.:1")
};

struct Preset {
    const char* name;
    std::vector<int32_t> v;          // the settings structure
};

// The sound of an effect. setup() is the plug-in's "apply the settings" routine: keep = false is a fresh start (silent
// buffers), keep = true a change of settings in the middle of the stream (buffers kept unless their size changes).
class Engine {
public:
    virtual ~Engine() {}
    virtual void setup(const int32_t* settings, uint32_t sampleRate, bool keep) = 0;
    virtual void process(float* left, float* right, int frames) = 0;
    // Frames the sound comes out later than it went in with these settings (an effect that holds the sound back); the host
    // plays the track earlier by as much. Asked before the engine has seen the settings.
    virtual int latency(const int32_t* settings, uint32_t sampleRate) const { (void)settings; (void)sampleRate; return 0; }
    // Effects that make the sound longer or shorter (AudioPlugin::processWhole): a fresh engine, just set up, takes the
    // whole sound and gives everything it delivers.
    virtual bool changesLength(const int32_t* settings) const { (void)settings; return false; }
    virtual bool whole(const float* inL, const float* inR, int64_t frames, std::vector<float>& outL, std::vector<float>& outR)
    {
        (void)inL; (void)inR; (void)frames; (void)outL; (void)outR;
        return false;
    }
};

struct Info {
    const char* uid;                 // Reno's id ("reno.simpledelay")
    const char* name;                // "Simple Delay"
    const char* clsid;               // VEGAS's DirectX class id, lower case with dashes
    uint8_t persistGuid[16];         // written before the settings
    int nInts;                       // size of the settings structure in ints
    std::vector<ParamDesc> params;
    std::vector<Preset> presets;     // VEGAS's own, "(Default)" first
    std::unique_ptr<Engine> (*make)();
    int bypassSlot;                  // -1: none
    void (*derive)(int32_t* settings, int slot) = nullptr;   // after a setting was changed by hand: updates the ones that follow from it (Pitch Shift's ratio)
};

const Info* findInfo(const std::string& uid);
const std::vector<const Info*>& allInfos();

// "S:" + base64 of u32 length, GUID, settings (what Reno stores for a DirectX effect); decode checks the GUID.
std::string encode(const Info& info, const int32_t* settings);
bool decode(const Info& info, const std::string& state, std::vector<int32_t>* settings);

// The Reno effect for a DirectX class id ("dshow:{...}" uid or a bare id), "" for none; and the other way.
std::string uidForDx(const std::string& dxUid);
std::string dxForUid(const std::string& uid);

std::unique_ptr<AudioPlugin> create(const std::string& uid);

}  // namespace vegpack1
}  // namespace reno
