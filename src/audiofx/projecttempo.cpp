// The project tempo the mixer tells the effects (see ProjectTempo in audioplugin.h).
#include "audioplugin.h"

namespace reno {

// ------------------------------------------------------------------------------------------ project tempo
namespace {
thread_local ProjectTempo t_tempo;
}

ProjectTempo projectTempo() { return t_tempo; }

void setProjectTempo(double bpm, int beatsPerMeasure, int beatUnit)
{
    ProjectTempo t;
    if (bpm >= 1.0 && bpm <= 1000.0) t.bpm = bpm;
    if (beatsPerMeasure >= 1 && beatsPerMeasure <= 64) t.beatsPerMeasure = beatsPerMeasure;
    if (beatUnit == 1 || beatUnit == 2 || beatUnit == 4 || beatUnit == 8 || beatUnit == 16) t.beatUnit = beatUnit;
    t_tempo = t;
}

ProjectTempoScope::ProjectTempoScope(double bpm, int beatsPerMeasure, int beatUnit) : prev_(t_tempo) { setProjectTempo(bpm, beatsPerMeasure, beatUnit); }
ProjectTempoScope::~ProjectTempoScope() { t_tempo = prev_; }

}  // namespace reno
