// Analysis of music for the editor (VEGAS 22's Beat Detection and Tempo Detection). Plain data in, plain
// data out: the interface reads the sound of a file and shows the result.
#pragma once
#include <vector>

namespace reno {

// The low and low-mid part of the sound (below about 1.5 kHz, where kick drums, bass lines and the body of a snare
// are) as a loudness curve in dB, one value per 'hop' seconds: what both detectors look at.
std::vector<float> lowBandLevels(const std::vector<float>& mono, int sampleRate, double hop);

// Beat Detection: the times (seconds from the start of 'mono') of the highlights - syncopated hits, drum rolls,
// "interesting" events in the low-mid and low end.
std::vector<double> detectBeats(const std::vector<float>& mono, int sampleRate, double sensitivity);

// Tempo Detection: the tempo in beats per minute (the one a listener would tap, 70..180 preferred), 0 if the sound is
// too short or has no pulse.
double detectTempo(const std::vector<float>& mono, int sampleRate, double* confidence = nullptr);

}  // namespace reno
