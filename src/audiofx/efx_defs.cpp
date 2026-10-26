// Settings of Reno's eFX effects: the 31 of each as the real ones list them (numbers, names, defaults, 0..1 meaning
// and wording), fitted to the real plug-ins' readings (each setting at 41 steps of 0..1; efx_test checks them all).
// "-" is a setting the effect does not use.
#include "efx.h"

namespace reno {
namespace efx {
namespace {

const char* const kLimiter0L[] = {"off", "on"};
const double kLimiter0T[] = {0.5};
const char* const kCompressor0L[] = {"off", "on"};
const double kCompressor0T[] = {0.5};
const char* const kCompressor10L[] = {"off", "on"};
const double kCompressor10T[] = {0.475};
const char* const kCompressor11L[] = {"off", "on"};
const double kCompressor11T[] = {0.475};
const char* const kCompressor15L[] = {"off", "lowpass"};
const double kCompressor15T[] = {0.975};
const char* const kCompressor16L[] = {"Int. Sidechain", "Ext. Sidechain"};
const double kCompressor16T[] = {0.975};
const char* const kCompressor21L[] = {"off", "on"};
const double kCompressor21T[] = {0.5};
const char* const kReverb0L[] = {"off", "on"};
const double kReverb0T[] = {0.5};
const char* const kReverb1L[] = {"Plate", "Room", "Hall", "Plate II Classic"};
const double kReverb1T[] = {0.325, 0.65, 0.975};
const char* const kTubeStage0L[] = {"off", "on"};
const double kTubeStage0T[] = {0.5};
const char* const kPhaser0L[] = {"off", "on"};
const double kPhaser0T[] = {0.5};
const char* const kPhaser1L[] = {"4 Stages", "8 Stages", "16 Stages"};
const double kPhaser1T[] = {0.475, 0.975};
const char* const kChorusFlanger0L[] = {"off", "on"};
const double kChorusFlanger0T[] = {0.5};
const char* const kChorusFlanger1L[] = {"Chorus", "Flanger", "Ensemble"};
const double kChorusFlanger1T[] = {0.475, 0.975};
const char* const kDeEsser0L[] = {"off", "on"};
const double kDeEsser0T[] = {0.5};
const char* const kDeEsser10L[] = {"off", "on"};
const double kDeEsser10T[] = {0.475};
const char* const kGate0L[] = {"Off", "On"};
const double kGate0T[] = {0.5};
const char* const kGate10L[] = {"Off", "On"};
const double kGate10T[] = {0.475};
const char* const kGate11L[] = {"Off", "On"};
const double kGate11T[] = {0.475};
const char* const kGate15L[] = {"off", "Lowpass", "Bandpass", "Highpass"};
const double kGate15T[] = {0.1, 0.15, 0.2};
const char* const kGate16L[] = {"Int. SideChain", "Ext. SideChain"};
const double kGate16T[] = {0.975};
const char* const kStereoDelay0L[] = {"off", "on"};
const double kStereoDelay0T[] = {0.5};
const char* const kStereoDelay1L[] = {"Digital", "Analog Tape", "Analog BBD"};
const double kStereoDelay1T[] = {0.475, 0.975};
const char* const kStereoDelay5L[] = {"1/32", "1/16 Tripl.", "1/16", "1/16 Dot.", "1/8 Tripl.", "1/8", "1/8 Dot.", "1/4 Tripl.", "1/4", "1/4 Dot.", "1/2 Bar", "1 Bar", "2 Bars"};
const double kStereoDelay5T[] = {0.025, 0.125, 0.2, 0.275, 0.375, 0.45, 0.525, 0.625, 0.7, 0.775, 0.875, 0.95};
const char* const kStereoDelay6L[] = {"1/32", "1/16 Tripl.", "1/16", "1/16 Dot.", "1/8 Tripl.", "1/8", "1/8 Dot.", "1/4 Tripl.", "1/4", "1/4 Dot.", "1/2 Bar", "1 Bar", "2 Bars"};
const double kStereoDelay6T[] = {0.025, 0.125, 0.2, 0.275, 0.375, 0.45, 0.525, 0.625, 0.7, 0.775, 0.875, 0.95};
const char* const kStereoDelay10L[] = {"off", "on"};
const double kStereoDelay10T[] = {0.25};
const char* const kStereoDelay22L[] = {"512", "2048", "4096", "8192"};
const double kStereoDelay22T[] = {0.15, 0.5, 0.825};
const char* const kTremoloPan0L[] = {"off", "on"};
const double kTremoloPan0T[] = {0.5};
const char* const kTremoloPan10L[] = {"off", "on"};
const double kTremoloPan10T[] = {0.5};
const char* const kVocalStrip0L[] = {"off", "on"};
const double kVocalStrip0T[] = {0.5};
const char* const kVocalStrip21L[] = {"off", "on"};
const double kVocalStrip21T[] = {0};

}  // namespace

std::vector<ParamDef> limiterDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kLimiter0L, 2, kLimiter0T);
    d[3] = pLin("Input", 0.5f, -24, 24, Fmt::DB);
    d[4] = pRat("Output", 0.971555f, -24, 0, 0.1428, Fmt::DB);
    d[5] = pRat("Threshold", 1.f, -30, 0, 0.6667, Fmt::DB);
    d[6] = pRat("Release", 0.15f, 1, 1000, 19.416, Fmt::MS);
    d[7] = pLin("Clip Gain", 0.5f, -12, 12, Fmt::DB);
    d[20] = pRat("Attack", 0.f, 1, 100, 9, Fmt::MS);
    d[21] = pLin("Adaptive Release", 1.f, 0, 100, Fmt::PCT);
    d[22] = pLin("Peak vs. RMS", 0.f, 0, 100, Fmt::PEAKRMS);
    d[23] = pFixed("Auto Makeup", 1.f, "On");
    d[24] = pLin("Input LowCut", 0.f, 0, 100, Fmt::HZOFF);
    d[25] = pRat("SC LowCut", 0.f, 0, 500, 4, Fmt::HZOFF);
    d[26] = pLin("Clip Shape", 1.f, 0, 100, Fmt::SOFTHARD);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

std::vector<ParamDef> compressorDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kCompressor0L, 2, kCompressor0T);
    d[3] = pGain("Input", 1.f);
    d[4] = pLin("Output", 0.5f, -48, 48, Fmt::DB);
    d[5] = pRat("SC Filter Freq", 0.f, 2, 1000, 9.15, Fmt::HZ);
    d[6] = pLin("Threshold", 1.f, -60, 0, Fmt::DB);
    d[7] = pRat("Ratio", 0.5f, 1.5, 50, 18.6, Fmt::RATIO);
    d[8] = pRat("Attack", 0.5f, 0.1, 150, 9, Fmt::MS);
    d[9] = pRat("Release", 0.5f, 25, 2500, 9, Fmt::MS);
    d[10] = pSteps("SC Listen", 0.f, kCompressor10L, 2, kCompressor10T);
    d[11] = pSteps("Soft Clip", 0.f, kCompressor11L, 2, kCompressor11T);
    d[15] = pSteps("SC Filter Type", 0.3333f, kCompressor15L, 2, kCompressor15T);
    d[16] = pSteps("Mode", 0.f, kCompressor16L, 2, kCompressor16T);
    d[20] = pRat("Adaptive Release", 0.472603f, 0, 10, 3, Fmt::NUM2);
    d[21] = pSteps("Auto Makeup", 0.f, kCompressor21L, 2, kCompressor21T);
    d[22] = pLin("Mix", 1.f, 0, 100, Fmt::PCT);
    d[30] = pLin("Version", 2.057f, 0, 0, Fmt::PCT);
    return d;
}

std::vector<ParamDef> reverbDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kReverb0L, 2, kReverb0T);
    d[1] = pSteps("Mode", 0.3333f, kReverb1L, 4, kReverb1T);
    d[3] = pGain("Input", 1.f);
    d[4] = pGain("Output", 1.f);
    d[5] = pLin("Size", 0.5f, 0, 100, Fmt::PCT);
    d[6] = pPow("Time", 0.5f, 0.3, 10, 3, Fmt::SEC);
    d[7] = pRat("Damping", 0.5f, 0, 100, 0.341, Fmt::PCT);
    d[8] = pLin("Modulation", 0.25f, 0, 100, Fmt::PCT);
    d[9] = pLin("Mix", 0.5f, 0, 100, Fmt::PCT);
    d[10] = pFixed("Sync", 0.f, "off");
    d[20] = pRat("Predelay", 0.095f, 0, 250, 5.6668, Fmt::MSP);
    d[21] = pLin("Diffusion", 1.f, 10, 100, Fmt::PCT);
    d[22] = pRat("Low Cut", 0.1429f, 50, 1000, 3, Fmt::HZ);
    d[30] = pLin("Version", 2.057f, -11, 0, Fmt::PCT);
    return d;
}

std::vector<ParamDef> tubeStageDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kTubeStage0L, 2, kTubeStage0T);
    d[3] = pLin("Input", 0.5f, -24, 24, Fmt::DB);
    d[4] = pLin("Output", 0.5f, -24, 24, Fmt::DB);
    d[5] = pLin("Pre EQ", 0.5f, -100, 100, Fmt::PCT);
    d[6] = pRat("Gain", 0.5003f, -6, 24, 4, Fmt::DB);
    d[7] = pLin("Post EQ", 0.5f, -100, 100, Fmt::PCT);
    d[10] = pFixed("Oversample", 1.f, "off");
    d[15] = pLin("Stages", 0.f, 100, 200, Fmt::PCT);
    d[16] = pLin("Class A || AB", 0.f, 0, 100, Fmt::PCT);
    d[20] = pLin("Tube Bias", 0.5f, -10, 10, Fmt::PCT1);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

std::vector<ParamDef> phaserDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kPhaser0L, 2, kPhaser0T);
    d[1] = pSteps("Mode", 0.f, kPhaser1L, 3, kPhaser1T);
    d[3] = pGain("Input", 1.f);
    d[4] = pGain("Output", 1.f);
    d[5] = pRat("Rate", 0.2f, 0.01, 15, 9, Fmt::HZR);
    d[6] = pLin("Depth", 0.5f, 0, 100, Fmt::PCT);
    d[7] = pRat("Phase Offset", 0.f, 0, 180, 3.004, Fmt::DEG);
    d[8] = pLin("Feedback", 0.f, 0, 100, Fmt::PCT);
    d[9] = pLin("Mix", 0.5f, 0, 100, Fmt::PCT);
    d[10] = pFixed("Sync", 0.f, "off");
    d[20] = pRat("Center Freq", 0.4354f, 50, 5000, 4, Fmt::HZ);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

std::vector<ParamDef> chorusFlangerDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kChorusFlanger0L, 2, kChorusFlanger0T);
    d[1] = pSteps("Mode", 0.f, kChorusFlanger1L, 3, kChorusFlanger1T);
    d[3] = pGain("Input", 1.f);
    d[4] = pGain("Output", 1.f);
    d[5] = pRat("Rate", 0.3294f, 0.01, 15, 7, Fmt::HZR);
    d[6] = pRat("Depth", 0.5f, 0, 100, 3.1, Fmt::PCT);
    d[7] = pRat("Phase Offset", 0.5f, 0, 180, 3.004, Fmt::DEG);
    d[8] = pLin("Feedback", 0.5f, -100, 100, Fmt::PCT);
    d[9] = pLin("Mix", 0.5f, 0, 100, Fmt::PCT);
    d[10] = pFixed("Sync", 0.f, "off");
    d[20] = pRat("Low Cut", 0.f, 20, 1000, 7, Fmt::HZ);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

std::vector<ParamDef> deEsserDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kDeEsser0L, 2, kDeEsser0T);
    d[3] = pLin("Input", 0.5f, -24, 24, Fmt::DB);
    d[4] = pLin("Output", 0.5f, -24, 24, Fmt::DB);
    d[5] = pLin("Filter Freq", 0.5f, 2000, 12000, Fmt::KHZ2);
    d[6] = pLin("Reduction", 0.1667f, 0, 36, Fmt::DB);
    d[10] = pSteps("Listen", 0.f, kDeEsser10L, 2, kDeEsser10T);
    d[20] = pRat("Filter Q", 0.2835f, 0.1, 2, 9, Fmt::NUM2);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

std::vector<ParamDef> gateDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kGate0L, 2, kGate0T);
    d[3] = pLin("Input", 0.5f, -24, 24, Fmt::DB);
    d[4] = pLin("Output", 0.5f, -24, 24, Fmt::DB);
    d[5] = pRat("SC Filter", 0.f, 20, 10000, 9, Fmt::HZ);
    d[6] = pRat("Threshold", 0.5f, -90, 0, 0.3637, Fmt::DB);
    d[7] = pRat("Range", 0.5f, 0, 90, 6.502, Fmt::DB);
    d[8] = pRat("Attack", 0.f, 0.05, 100, 9.05, Fmt::MS);
    d[9] = pRat("Release", 0.5f, 10, 2500, 9.361, Fmt::MS);
    d[10] = pSteps("SC Listen", 0.f, kGate10L, 2, kGate10T);
    d[11] = pSteps("Soft Knee", 0.f, kGate11L, 2, kGate11T);
    d[15] = pSteps("SC Filter Type", 0.3333f, kGate15L, 4, kGate15T);
    d[16] = pSteps("SC Mode", 0.f, kGate16L, 2, kGate16T);
    d[17] = pLin("Hold Time", 0.5f, 0, 200, Fmt::PCT);
    d[20] = pRat("Look Ahead", 0.5f, 0, 5, 9, Fmt::MSOFF);
    d[21] = pRat("HP Filter@Input", 0.2727f, 0, 500, 9, Fmt::HZOFF);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

std::vector<ParamDef> stereoDelayDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kStereoDelay0L, 2, kStereoDelay0T);
    d[1] = pSteps("Mode", 0.f, kStereoDelay1L, 3, kStereoDelay1T);
    d[3] = pGain("Input", 1.f);
    d[4] = pGain("Output", 1.f);
    d[5] = pSteps("Delay L", 0.4167f, kStereoDelay5L, 13, kStereoDelay5T);
    d[6] = pSteps("Delay R", 0.4167f, kStereoDelay6L, 13, kStereoDelay6T);
    d[7] = pLin("Damping", 0.5f, 0, 100, Fmt::PCT);
    d[8] = pLin("Feedback", 0.5f, -100, 100, Fmt::PINGDUAL);
    d[9] = pLin("Mix", 0.5f, 0, 100, Fmt::PCT);
    d[10] = pSteps("Sync", 1.f, kStereoDelay10L, 2, kStereoDelay10T);
    d[20] = pRat("Feedback LowCut", 0.075f, 10, 1000, 4, Fmt::HZ);
    d[21] = pRat("Tape/BBD Noise", 0.f, -100, -40, 2, Fmt::DB);
    d[22] = pSteps("BBD Stages", 0.6667f, kStereoDelay22L, 4, kStereoDelay22T);
    d[23] = pLin("BBD Compander", 0.5f, 0, 100, Fmt::PCT);
    d[24] = pRat("BBD Clock Drift", 0.06f, 0, 100, 3.1, Fmt::PCT);
    d[30] = pLin("Version", 2.057f, -11, 0, Fmt::PCT);
    return d;
}

std::vector<ParamDef> tremoloPanDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kTremoloPan0L, 2, kTremoloPan0T);
    d[3] = pLin("Input", 0.5f, -24, 24, Fmt::DB);
    d[4] = pLin("Output", 0.5f, -24, 24, Fmt::DB);
    d[5] = pPow("Rate", 0.1158f, 0.1, 100, 4, Fmt::HZSP);
    d[6] = pLin("Shape", 0.f, 0, 100, Fmt::PCT);
    d[7] = pRat("Phase", 0.f, 0, 180, 3.004, Fmt::DEG);
    d[8] = pRat("Depth", 1.f, 0, 100, 3.1, Fmt::PCT);
    d[10] = pSteps("Sync", 0.f, kTremoloPan10L, 2, kTremoloPan10T);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

std::vector<ParamDef> vocalStripDefs()
{
    std::vector<ParamDef> d(kParams);
    d[0] = pSteps("Active", 1.f, kVocalStrip0L, 2, kVocalStrip0T);
    d[3] = pLin("Input", 0.5f, -24, 24, Fmt::DB);
    d[4] = pLin("Output", 0.5f, -24, 24, Fmt::DB);
    d[5] = pRat("Highpass", 0.f, 20, 500, 5, Fmt::HZ);
    d[6] = pRat("Gate Threshold", 0.f, -90, 0, 0.3637, Fmt::DB);
    d[7] = pLin("DeEsser", 0.f, 0, 36, Fmt::DB);
    d[8] = pLin("Compression", 0.f, 0, 48, Fmt::DB);
    d[9] = pLin("Tone", 0.5f, -100, 100, Fmt::PCT);
    d[20] = pLin("DeEss DetFreq", 0.375f, 4000, 12000, Fmt::KHZ2);
    d[21] = pSteps("CompAutoMakeup", 1.f, kVocalStrip21L, 2, kVocalStrip21T);
    d[22] = pRat("Comp Attack", 0.1389f, 0.2, 10, 3, Fmt::MS);
    d[23] = pRat("Comp Adapt.Rel.", 0.6667f, 0, 10, 3, Fmt::NUM2);
    d[30] = pLin("Version", 2.057f, 100, 1000, Fmt::PCT);
    return d;
}

}  // namespace efx
}  // namespace reno
