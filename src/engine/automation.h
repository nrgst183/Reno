// VEGAS's automation (manual: Automation and Scripting > Using automation), the parts that are plain data work: the
// track automation modes' names, writing a control's value into an envelope (at the cursor, or recorded while playing
// in Automation Write (Touch) and (Latch)), thinning envelope points, and VEGAS 22's Auto Ducking gain.
#pragma once
#include "project.h"
#include <string>
#include <vector>

namespace reno {

// The modes of a track's (or bus's) Automation Settings menu, in VEGAS's order, with VEGAS's names.
struct AutomationModeName { const char* key; const char* name; };
extern const AutomationModeName kAutomationModes[4];       // off, read, touch, latch

// What a control shows in automation mode: the envelope's value at t, or 'whenNone' without one.
double automationValue(const Envelopes& envs, const std::string& kind, uint64_t target, double t, double whenNone);

// Sets envelope (kind, target) to 'value' at time t, as VEGAS's controls do in automation mode: the point within
// 'near' seconds of t takes the value, else a point is added there (with the curve of the point before it, or 'curve'
// for the first).
void setEnvelopeAt(Envelopes& envs, const std::string& kind, uint64_t target, double t, double value, double near, int curve);

// Automation recording while playing (Automation Write (Touch) / (Latch)): the points a control writes between 'from'
// and the play position, put over the envelope as it was when recording began ('base').
struct AutomationTake {
    std::string kind;
    uint64_t target = 0;
    bool latch = false;
    std::vector<EnvPoint> base;             // the envelope's points before recording (empty: there was none)
    bool hadEnvelope = false;
    std::vector<EnvPoint> written;          // (time, value) in time order
    double latchTo = -1;                    // latch: where the last value holds to (the play position; -1: not yet)
    double whenNone = 0;                    // the setting where there was no envelope
    // Adds a written value at time t (one every 'step' seconds at most, so a long take stays thin; a value at an
    // earlier time than the last - playback went back - starts the take again from there).
    void write(double t, double value, double step);
    // The envelope's points with the take put over the base.
    std::vector<EnvPoint> result() const;
    // Puts the result into 'envs' (making the envelope if need be).
    void apply(Envelopes& envs) const;
};

// Thins an envelope's points between times 'from' and 'to' (VEGAS's Thin All Points / Thin Selected Points): a point
// is taken out where the line from the point kept before it to the point after it passes within 'tolerance' (in the
// envelope's own units) of it, and its curve and the next one's agree.
int thinEnvelope(Envelope& e, double from, double to, double tolerance);
// The tolerance thinning uses for an envelope of this kind (about 1% of its range, a tenth of a dB for volume).
double thinTolerance(const std::string& kind);

// Auto Ducking (Track::duckController / duckListener): the gain (factor) a listener track gets for a controller
// sound at 'levelDb' (its short-term level, dB FS): no change while it is quiet (below -45 dB), else 'floorDb' dB,
// and lower by 'peak' (0..1) of how far the sound is over the threshold (at 100% a -6 dB sound takes 39 dB more).
double autoDuckGain(double levelDb, double floorDb, double peak);

// The Auto Ducking envelope for a controller's sound: 'levelDb' are the controller's levels (dB FS, one per 'step'
// seconds from time t0).
std::vector<EnvPoint> duckPoints(const std::vector<float>& levelDb, double step, double t0, double floorDb, double peak);

}  // namespace reno
