// Time-stretching and pitch-shifting of an event's sound (Audio Event Properties > Time stretch / pitch shift).
//   None      speed and pitch change together (the media is just read at another sample rate)
//   Classic   VEGAS's splicing stretcher with its 19 presets "A01. Music 1" .. "A19. Drums (tiny echo)"; the numbers
//             are VEGAS's own (table in vegas180k.dll)
//   élastique zplane's library is not available; Reno has its own algorithms for "pro" (phase vocoder), "efficient"
//             (cheaper vocoder), "soloist-mono" and "soloist-speech" (pitch-synchronous overlap-add)
//
// A stretcher changes the length by 'alpha' (output samples per input sample). Pitch is normally changed before it by
// reading the media at another sample rate; a stretcher that shifts pitch itself (soloist modes with formants kept)
// wants input at the original pitch and is told the pitch factor. "preserve formants" keeps the spectral envelope and
// "formant" moves it by that many semitones.
//
// The stretcher pulls input in order through 'pull' and keeps its state between calls; output does not depend on block
// sizes. Output sample u comes from around input sample u / alpha.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace reno {

// Input samples [index, index + count) into l and r. Indexes can be negative (before the event's media start).
using StretchPull = std::function<void(int64_t index, int count, float* l, float* r)>;

struct StretchParams {
    std::string method = "elastique";   // "elastique" or "classic" ("none" needs no stretcher)
    std::string mode = "pro";           // elastique: pro, efficient, soloist-mono, soloist-speech; classic: A01..A19
    int rate = 48000;                   // sample rate (input and output)
    double alpha = 1.0;                 // output samples per input sample (2: twice as long)
    double pitch = 1.0;                 // frequency factor the stretcher applies itself (see stretcherShiftsPitch)
    double resampled = 1.0;             // the frequency factor already applied to the input by resampling it
    bool preserveFormants = false;      // keep the formants where they were before any pitch change ...
    double formant = 0;                 // ... and then move them by this many semitones
    double delay = 0;                   // the sound comes this many output samples later (earlier if negative): see elastiqueDelaySamples
    int64_t inputEnd = -1;              // the input's length in samples if it is known and does not loop (-1: unknown); Classic
                                        // does not search for a better splice where the search would run past it
};

class TimeStretcher {
public:
    virtual ~TimeStretcher() {}
    // n output samples into l and r (stereo).
    virtual void process(float* l, float* r, int n, const StretchPull& pull) = 0;
};

// Whether the method and mode change the formants at all with these settings (VEGAS: élastique Pro and Soloist
// (Monophonic) with "Preserve formants" on; the shift itself only with Pro).
bool stretchKeepsFormants(const std::string& method, const std::string& mode, bool preserveFormants);
// Whether the stretcher wants its input at the original pitch and changes the pitch itself (true for the soloist
// modes when formants are kept, and always for soloist-speech, whose grains keep the voice's formants).
bool stretcherShiftsPitch(const std::string& method, const std::string& mode, bool preserveFormants);
// The stretcher for these settings (an unknown method or mode gets élastique Pro / Classic A03, VEGAS's defaults).
std::unique_ptr<TimeStretcher> makeStretcher(const StretchParams& p);

// Where VEGAS's élastique puts a sound whose pitch is shifted by 'semitones' against an ideal shifter, which makes
// the sound at the very time the stretch says (as Reno's stretchers do): that many output samples later (negative:
// earlier).
double elastiqueDelaySamples(const std::string& mode, double semitones);

// VEGAS's Classic presets, with the numbers VEGAS uses for them.
struct ClassicPreset {
    const char* id;             // "A01"
    const char* name;           // "A01. Music 1 (minimum flange, may echo)" as VEGAS lists it
    int halfSpliceMs;           // a splice is twice this long
    int searchMs;               // how far either way to look for the best place to splice
    int searchStep;             // looking at every n-th sample (at 44.1 kHz; "fast" presets look at fewer)
    int crossfadeMs;            // the crossfade between splices
};
const std::vector<ClassicPreset>& classicPresets();
const ClassicPreset* classicPreset(const std::string& id);

// The élastique modes: id ("pro") and VEGAS's name ("Pro").
const std::vector<std::pair<std::string, std::string>>& elastiqueModes();

}  // namespace reno
