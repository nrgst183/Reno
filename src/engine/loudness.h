// Loudness metering (ITU-R BS.1770-4, EBU R128, EBU Tech 3341/3342): K-weighted power in 400 ms blocks and 3 s
// windows, in LUFS. Used by the Loudness Meters window.
//   momentary      the last 400 ms
//   short-term     the last 3 s
//   integrated     everything since reset(), gated: blocks quieter than -70 LUFS, and then those more than 10 LU
//                  below the loudness of the rest, are left out
//   loudness range (LRA) the spread of the short-term loudness (10th to 95th percentile), gated at -70 LUFS and
//                  20 LU below
//   true peak      the highest sample after 4x oversampling, in dBTP
// Stereo: both sides weigh 1; 5.1 (processChannels: L R C LFE Ls Rs) as BS.1770 weighs it - the LFE left out, the
// surround channels 1.41 (+1.5 dB). Not thread safe (the caller locks).
#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include <string>

namespace reno {

class LoudnessMeter {
public:
    static constexpr double kSilence = -70.0;      // what the readings give when there is nothing to measure

    explicit LoudnessMeter(int sampleRate = 48000) { reset(sampleRate); }
    void reset(int sampleRate);
    void process(const float* left, const float* right, int frames);
    // 'channels' channels (1, 2 or 6: L R C LFE Ls Rs) at once.
    void processChannels(const float* const* ch, int channels, int frames);
    // VEGAS's True Peak Blocking Filter: the true peak is the larger of the signal's and of the signal with its DC
    // offset (and the asymmetry below 10 Hz) filtered out.
    void setPeakBlocking(bool on) { blocking_ = on; }
    void resetPeak()
    {
        peak_ = 0;
        for (int k = 0; k < 6; ++k) chPeak_[k] = curPeak_[k] = lastPeak_[k] = 0;
    }
    // One channel's true peak (dBTP): the highest since reset, and the highest in the last 100 ms (for a meter).
    double channelTruePeak(int ch) const;
    double recentTruePeak(int ch) const;

    double momentary() const;           // LUFS
    double shortTerm() const;
    double integrated() const;
    double range() const;               // LU
    double truePeak() const;            // dBTP
    double maxMomentary() const { return maxMomentary_; }
    double maxShortTerm() const { return maxShortTerm_; }
    double seconds() const { return (double)samples_ / rate_; }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        double run(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    double loudnessOf(double meanSquareSum) const;
    void endSubBlock();
    double peakOversampled(int ch, float x, std::vector<double>* hist = nullptr);

    int rate_ = 48000;
    Biquad shelf_[6], pass_[6];
    int64_t samples_ = 0;
    int subLen_ = 4800, subFill_ = 0;          // 100 ms pieces: four make a 400 ms block, thirty a short-term window
    double subSum_ = 0;                         // weighted squares of the piece being filled (both sides)
    std::deque<double> subs_;                   // the last 30 pieces' mean squares
    std::vector<double> blocks_;                // every 400 ms block's mean square (overlapping by 75%), for the integrated loudness
    std::vector<double> shortBlocks_;           // every short-term window's mean square, for the range
    double maxMomentary_ = kSilence, maxShortTerm_ = kSilence;
    // true peak: a polyphase 4x interpolator per side
    std::vector<double> taps_;                  // 4 phases x tapsPerPhase
    int tapsPerPhase_ = 12;
    std::vector<double> hist_[6];
    std::vector<double> histF_[6];              // (the same of the DC-blocked signal, with the blocking filter)
    bool blocking_ = false;
    double chPeak_[6] = {}, curPeak_[6] = {}, lastPeak_[6] = {};
    double dcX_[6] = {}, dcY_[6] = {};
    double peakOf(int ch, float x);
    int histPos_ = 0;
    double peak_ = 0;
};

// VEGAS's loudness log ("<name>_loud.txt", Render As's "Save loudness log file next to media file" and Tools >
// Generate Loudness Log).
struct LoudnessLogPoint {
    double time = 0, momentary = LoudnessMeter::kSilence, shortTerm = LoudnessMeter::kSilence;
};
std::string loudnessLogText(const std::string& name, const std::string& format, bool atsc, double from, double to, const LoudnessMeter& m,
                            const std::vector<LoudnessLogPoint>& points);

}  // namespace reno
