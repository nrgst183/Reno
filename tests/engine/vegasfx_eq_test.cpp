// VEGAS's Track EQ (sftrkfx1_x64.dll, CSfTEQ) as Reno's built-in "reno.trkeq" reproduces it: the filter's code
// against samples computed with the DLL's own code (run on Linux in a loader that maps the DLL) at 44100 and 48000 Hz,
// the saved form, the presets, a change of settings while running, settings of
// older Reno versions, the DirectX fallback and the way it is saved as VEGAS's.
//
//   vegasfx_eq_test
//
// The DLL is not needed to run this: the expected values are stored here.
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegas_trkfx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const int kFrames = 30000;
static float input(int k)       // exact pseudo-random numbers in -1..1
{
    uint32_t h = (uint32_t)k * 2654435761u;
    h ^= h >> 15;
    return (float)((int)(h & 0xffff) - 32768) / 32768.f;
}
// the test signal: bursts of different levels, 2000 frames each, stereo
static void makeInput(std::vector<float>& l, std::vector<float>& r)
{
    static const float amp[7] = {0.0005f, 0.02f, 0.5f, 0.0002f, 0.8f, 0.01f, 0.3f};
    l.resize(kFrames);
    r.resize(kFrames);
    for (int k = 0; k < kFrames; ++k) {
        const float a = amp[(k / 2000) % 7];
        l[(size_t)k] = a * input(2 * k);
        r[(size_t)k] = a * input(2 * k + 1);
    }
}
static uint32_t bitsOf(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
static uint32_t fnvInter(const std::vector<float>& l, const std::vector<float>& r)
{
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < l.size(); ++k)
        for (float f : {l[k], r[k]}) {
            const uint32_t b = bitsOf(f);
            for (int i = 0; i < 4; ++i) {
                h ^= (b >> (8 * i)) & 0xff;
                h *= 16777619u;
            }
        }
    return h;
}

struct Sample {
    int index;               // into the interleaved output (2 * frame + channel)
    uint32_t bits;
};
// The 32 settings as the DLL keeps them (gain, bandwidth, frequency, mode, roll-off, enable: four of each; selected band, a spare,
// six channel switches)
typedef int32_t Raw[32];
static vegeq::Settings settingsOf(const Raw& a)
{
    static_assert(sizeof(vegeq::Settings) == sizeof(Raw), "VEGAS Track EQ's settings are 128 bytes");
    vegeq::Settings s;
    std::memcpy(&s, a, sizeof s);
    return s;
}
struct Case {
    const char* name;
    int chunk;               // frames per call
    Raw s;
    uint32_t hash;           // FNV-1a over the 60000 output floats
    int changed;             // how many of them differ from the input
    std::vector<Sample> samples;
};

// computed with the DLL's own settings reader, band builder and filter for the input above
static const std::vector<Case> k44100 = {
    {"(Default)", 512, {0, 0, 0, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x2bd1eda0u, 0, {}},
    {"Default all parameters", 512, {0, 0, 0, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x2bd1eda0u, 0, {}},
    {"15 dB notch centered at 1000 Hz", 512, {0, 0, -150, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 1, 0, 2, 0, 1, 1, 1, 1, 1, 1}, 0x88d1b794u, 59999, {{1, 0xb97c04c9}, {10000, 0xbe682bff}, {20000, 0x3cdab128}, {30000, 0x396afcf5}, {40000, 0xbd011bf0}, {50000, 0x3ba515d6}}},
    {"60 Hz hum notch using four stacked filters", 512, {-250, -250, -250, -250, 3, 3, 3, 3, 60, 60, 60, 60, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xdb491450u, 59999, {{1, 0xb96f1d4a}, {10000, 0xbe83a21a}, {20000, 0x3a6e0d37}, {30000, 0xb882a843}, {40000, 0xbaba496a}, {50000, 0x3b8033e0}}},
    {"Add air (3 dB peak at 15 kHz)", 512, {0, 0, 0, 30, 10, 10, 10, 7, 100, 300, 1000, 15000, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0x0a4d4f37u, 59999, {{1, 0xb9421148}, {10000, 0xbe5ada89}, {20000, 0x3ca554f7}, {30000, 0x39632393}, {40000, 0xbd2c4fc0}, {50000, 0x3bedc72c}}},
    {"Add air (boost above 16k by 6 dB)", 512, {0, 0, 0, 60, 10, 10, 10, 10, 100, 300, 1000, 16000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0x2da5c754u, 59998, {{2, 0x39638df1}, {10001, 0x3f1902d9}, {20001, 0xbea2c19f}, {30001, 0xb9c88972}, {40000, 0x3da39c23}, {50000, 0x3c1f91d5}}},
    {"Anti-DTMF Talkoff", 512, {-250, -250, -250, -250, 3, 3, 3, 3, 1209, 1336, 1447, 1633, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x77f33b3cu, 59999, {{1, 0xb98268dc}, {10000, 0xbe3b8ddf}, {20000, 0xbe027b0e}, {30000, 0x393c41f6}, {40000, 0xbb2e41ae}, {50000, 0x3bc1cbe7}}},
    {"Boost bass frequencies below 250 Hz by 4 dB", 512, {40, 0, 0, 0, 10, 10, 10, 10, 250, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0x669d9d86u, 59998, {{2, 0x38426af5}, {10001, 0x3ec3b0fc}, {20001, 0x3c964feb}, {30001, 0xb8c9f6d6}, {40000, 0x3b248fb7}, {50000, 0x3b9a1304}}},
    {"Boost frequencies above 7 kHz", 512, {0, 0, 0, 30, 10, 10, 10, 10, 100, 300, 1000, 7000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0x767de681u, 59998, {{2, 0x3948fe5e}, {10001, 0x3ef7d86d}, {20001, 0xbd46d0e8}, {30001, 0xb9340cb7}, {40000, 0x3c346ba9}, {50000, 0x3bf5b934}}},
    {"Fletcher-Munson curve (low volume compensation)", 512, {60, 0, -120, 60, 10, 10, 10, 10, 200, 300, 4000, 8000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x373e001au, 59999, {{1, 0xb9896a97}, {10000, 0xbf097b87}, {20000, 0xbe839eb6}, {30000, 0x39d314be}, {40000, 0x3db90716}, {50000, 0x3c53820d}}},
    {"Hiss cut (cut above 8 kHz by 6 dB)", 512, {0, 0, 0, -60, 10, 10, 10, 10, 100, 300, 1000, 8000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0xc5321247u, 59998, {{2, 0xb8fbcb87}, {10001, 0x3e859ffd}, {20001, 0x3d399fd8}, {30001, 0x37ca68fc}, {40000, 0xbc656884}, {50000, 0x3aa63ea6}}},
    {"Remove low rumble below 80 Hz", 512, {-250, 0, 0, 0, 10, 10, 10, 10, 80, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0x2ac0fa21u, 59998, {{2, 0x383f5b82}, {10001, 0x3ec42094}, {20001, 0xbd016d13}, {30001, 0xb8af2c3d}, {40000, 0x3ba712ef}, {50000, 0x3ba02270}}},
    {"Remove very low and inaudible frequencies below 20 Hz", 512, {-250, 0, 0, 0, 10, 10, 10, 10, 20, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0x6587e2e1u, 59998, {{2, 0x38402c99}, {10001, 0x3ec2eaa8}, {20001, 0xbca43e82}, {30001, 0xb8d2ce19}, {40000, 0x3c0e3d1c}, {50000, 0x3bb962a7}}},
    {"All four bands boosted", 512, {60, -40, 90, 50, 10, 5, 20, 10, 120, 600, 2500, 9000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x3e060f7du, 59999, {{1, 0xb8f838c3}, {10000, 0xbec49926}, {20000, 0x3e738276}, {30000, 0x39ebfae9}, {40000, 0xbe109cbc}, {50000, 0x3bded682}}},
    {"Four-stage shelves", 512, {100, 0, 0, -120, 10, 10, 10, 10, 200, 300, 1000, 6000, 1, 2, 2, 3, 4, 1, 1, 4, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xa079565du, 60000, {{0, 0xba041625}, {10000, 0xbe2e7edf}, {20000, 0x3e067271}, {30000, 0x384ac852}, {40000, 0xbd25fc3e}, {50000, 0xbaf84881}}},
    {"Three-stage and two-stage shelves", 512, {-80, 0, 0, 70, 10, 10, 10, 10, 500, 300, 1000, 3000, 1, 2, 2, 3, 3, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x0e4a167fu, 60000, {{0, 0xba01a2f6}, {10000, 0xbf17e56d}, {20000, 0xbebc91f3}, {30000, 0x3a126413}, {40000, 0x3d293d2c}, {50000, 0x3c84e475}}},
    {"Narrow and wide bells", 512, {90, -90, 150, -30, 1, 40, 3, 15, 80, 15000, 440, 2000, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xc2f58a33u, 59999, {{1, 0xb9bf8774}, {10000, 0xbe81dd26}, {20000, 0x3d0c8943}, {30000, 0x36c9f387}, {40000, 0xbce756fc}, {50000, 0x3b0dbe54}}},
    {"Disabled band with a gain", 512, {60, 60, 60, 60, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0x43996a62u, 59999, {{1, 0xb969cc05}, {10000, 0xbe926ed9}, {20000, 0x3cc6b5de}, {30000, 0x39806fb3}, {40000, 0x3b8da29f}, {50000, 0x3ba41640}}},
    {"Frequency at the top of the range", 512, {60, 60, 60, 60, 10, 10, 10, 10, 20, 20000, 19000, 20000, 3, 2, 2, 1, 2, 1, 1, 3, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x79e33f40u, 60000, {{0, 0xba4c48f4}, {10000, 0xbf16a344}, {20000, 0x3ee13df0}, {30000, 0x3b1796f0}, {40000, 0x3d34b554}, {50000, 0x3d9a73e6}}},
    {"Odd blocks: Fletcher-Munson", 481, {60, 0, -120, 60, 10, 10, 10, 10, 200, 300, 4000, 8000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x373e001au, 59999, {{1, 0xb9896a97}, {10000, 0xbf097b87}, {20000, 0xbe839eb6}, {30000, 0x39d314be}, {40000, 0x3db90716}, {50000, 0x3c53820d}}},
    {"Odd blocks: stacked notches", 37, {-250, -250, -250, -250, 3, 3, 3, 3, 60, 60, 60, 60, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xdb491450u, 59999, {{1, 0xb96f1d4a}, {10000, 0xbe83a21a}, {20000, 0x3a6e0d37}, {30000, 0xb882a843}, {40000, 0xbaba496a}, {50000, 0x3b8033e0}}},
    {"Odd blocks: shelves and bells", 1000, {100, 40, -60, 70, 10, 8, 12, 10, 150, 700, 3100, 6500, 1, 2, 2, 3, 3, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x896d0d8bu, 60000, {{0, 0xba03eaac}, {10000, 0xbf406fb3}, {20000, 0xbf16eb85}, {30000, 0x3a11adde}, {40000, 0x3dbe0006}, {50000, 0x3c88ce73}}},
};
static const std::vector<Case> k48000 = {
    {"(Default)", 512, {0, 0, 0, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x2bd1eda0u, 0, {}},
    {"Default all parameters", 512, {0, 0, 0, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x2bd1eda0u, 0, {}},
    {"15 dB notch centered at 1000 Hz", 512, {0, 0, -150, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 1, 0, 2, 0, 1, 1, 1, 1, 1, 1}, 0x6438f94eu, 59999, {{1, 0xb97aefb4}, {10000, 0xbe6e8129}, {20000, 0x3d178074}, {30000, 0x396c13e1}, {40000, 0xbd08ac01}, {50000, 0x3ba30104}}},
    {"60 Hz hum notch using four stacked filters", 512, {-250, -250, -250, -250, 3, 3, 3, 3, 60, 60, 60, 60, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x97faf444u, 59999, {{1, 0xb96f0827}, {10000, 0xbe83ac0e}, {20000, 0x3a95816d}, {30000, 0x3980c967}, {40000, 0xba94adb3}, {50000, 0x3babd43f}}},
    {"Add air (3 dB peak at 15 kHz)", 512, {0, 0, 0, 30, 10, 10, 10, 7, 100, 300, 1000, 15000, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0x57c93d61u, 59999, {{1, 0xb94552a6}, {10000, 0xbe5a4ff8}, {20000, 0x3d5540d7}, {30000, 0x3953024f}, {40000, 0xbd859c39}, {50000, 0x3bf36f90}}},
    {"Add air (boost above 16k by 6 dB)", 512, {0, 0, 0, 60, 10, 10, 10, 10, 100, 300, 1000, 16000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0x82e404c0u, 59998, {{2, 0x398050f1}, {10001, 0x3f1bc2a4}, {20001, 0xbe8f8ef9}, {30001, 0xb9c6b717}, {40000, 0x3d8b4aeb}, {50000, 0x3c2aaa20}}},
    {"Anti-DTMF Talkoff", 512, {-250, -250, -250, -250, 3, 3, 3, 3, 1209, 1336, 1447, 1633, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xd6444e20u, 59999, {{1, 0xb981878b}, {10000, 0xbe414332}, {20000, 0xbd83e790}, {30000, 0x394f73ae}, {40000, 0xbcad558b}, {50000, 0x3bb6953f}}},
    {"Boost bass frequencies below 250 Hz by 4 dB", 512, {40, 0, 0, 0, 10, 10, 10, 10, 250, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0x5dcda000u, 59998, {{2, 0x384242a3}, {10001, 0x3ec36ce9}, {20001, 0x3c8959ad}, {30001, 0xb8c8a54f}, {40000, 0x3b04de21}, {50000, 0x3b99d3df}}},
    {"Boost frequencies above 7 kHz", 512, {0, 0, 0, 30, 10, 10, 10, 10, 100, 300, 1000, 7000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0x0dce7192u, 59998, {{2, 0x394e424f}, {10001, 0x3ef8dde7}, {20001, 0xbd3a7ebf}, {30001, 0xb92e812d}, {40000, 0x3c2b69f0}, {50000, 0x3bf3fbfd}}},
    {"Fletcher-Munson curve (low volume compensation)", 512, {60, 0, -120, 60, 10, 10, 10, 10, 200, 300, 4000, 8000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x3e02f8b3u, 59999, {{1, 0xb988230a}, {10000, 0xbf0c10a1}, {20000, 0xbe7e2212}, {30000, 0x39d2a55d}, {40000, 0x3db02522}, {50000, 0x3c4e237b}}},
    {"Hiss cut (cut above 8 kHz by 6 dB)", 512, {0, 0, 0, -60, 10, 10, 10, 10, 100, 300, 1000, 8000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 1, 1, 1, 1, 1, 1}, 0x35d87f60u, 59998, {{2, 0xb904e694}, {10001, 0x3e84a1a0}, {20001, 0x3d243d48}, {30001, 0x37967329}, {40000, 0xbc59f2a7}, {50000, 0x3aaccfc4}}},
    {"Remove low rumble below 80 Hz", 512, {-250, 0, 0, 0, 10, 10, 10, 10, 80, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0xd5a502bcu, 59998, {{2, 0x383f7213}, {10001, 0x3ec4011f}, {20001, 0xbcf9e322}, {30001, 0xb8b0329c}, {40000, 0x3bb4ff95}, {50000, 0x3ba04e4d}}},
    {"Remove very low and inaudible frequencies below 20 Hz", 512, {-250, 0, 0, 0, 10, 10, 10, 10, 20, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0x1cac96e6u, 59998, {{2, 0x38403249}, {10001, 0x3ec2eb81}, {20001, 0xbca0b62c}, {30001, 0xb8ea6187}, {40000, 0x3c0faf8c}, {50000, 0x3bc0532a}}},
    {"All four bands boosted", 512, {60, -40, 90, 50, 10, 5, 20, 10, 120, 600, 2500, 9000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x52296d56u, 59999, {{1, 0xb903b017}, {10000, 0xbec7293a}, {20000, 0x3e7c02fa}, {30000, 0x39ef9266}, {40000, 0xbe0fcac1}, {50000, 0x3be04933}}},
    {"Four-stage shelves", 512, {100, 0, 0, -120, 10, 10, 10, 10, 200, 300, 1000, 6000, 1, 2, 2, 3, 4, 1, 1, 4, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xe82bb1c4u, 60000, {{0, 0xba040119}, {10000, 0xbe2c825c}, {20000, 0x3df6ad1f}, {30000, 0x384c0b8f}, {40000, 0xbd29fdfa}, {50000, 0xbae1143b}}},
    {"Three-stage and two-stage shelves", 512, {-80, 0, 0, 70, 10, 10, 10, 10, 500, 300, 1000, 3000, 1, 2, 2, 3, 3, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xd23f18feu, 60000, {{0, 0xba01c052}, {10000, 0xbf17e36d}, {20000, 0xbeb40131}, {30000, 0x3a125718}, {40000, 0x3d215434}, {50000, 0x3c82d6b9}}},
    {"Narrow and wide bells", 512, {90, -90, 150, -30, 1, 40, 3, 15, 80, 15000, 440, 2000, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x29660f58u, 59999, {{1, 0xb9c0a1e7}, {10000, 0xbe66f4d0}, {20000, 0x3b09ff1d}, {30000, 0x3a478eba}, {40000, 0xbd374f8f}, {50000, 0x3b390f52}}},
    {"Disabled band with a gain", 512, {60, 60, 60, 60, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 0xd9c2a0e9u, 59999, {{1, 0xb96a2439}, {10000, 0xbe919bab}, {20000, 0x3cbdf606}, {30000, 0x398194a9}, {40000, 0x3baf716c}, {50000, 0x3ba4e095}}},
    {"Frequency at the top of the range", 512, {60, 60, 60, 60, 10, 10, 10, 10, 20, 20000, 19000, 20000, 3, 2, 2, 1, 2, 1, 1, 3, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x21a7d0d1u, 60000, {{0, 0xba45756f}, {10000, 0xbf2121ea}, {20000, 0x3f24c34c}, {30000, 0x3b0ef72e}, {40000, 0xbdf9fb11}, {50000, 0x3d98fc2d}}},
    {"Odd blocks: Fletcher-Munson", 481, {60, 0, -120, 60, 10, 10, 10, 10, 200, 300, 4000, 8000, 1, 2, 2, 3, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x3e02f8b3u, 59999, {{1, 0xb988230a}, {10000, 0xbf0c10a1}, {20000, 0xbe7e2212}, {30000, 0x39d2a55d}, {40000, 0x3db02522}, {50000, 0x3c4e237b}}},
    {"Odd blocks: stacked notches", 37, {-250, -250, -250, -250, 3, 3, 3, 3, 60, 60, 60, 60, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0x97faf444u, 59999, {{1, 0xb96f0827}, {10000, 0xbe83ac0e}, {20000, 0x3a95816d}, {30000, 0x3980c967}, {40000, 0xba94adb3}, {50000, 0x3babd43f}}},
    {"Odd blocks: shelves and bells", 1000, {100, 40, -60, 70, 10, 8, 12, 10, 150, 700, 3100, 6500, 1, 2, 2, 3, 3, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0xe0018162u, 60000, {{0, 0xba03d928}, {10000, 0xbf3e4ab6}, {20000, 0xbf14d212}, {30000, 0x3a0eef52}, {40000, 0x3daf0014}, {50000, 0x3c89a528}}},
};
// a change of settings in the middle (at frame 15360); keep: the filters keep the sound they hold (VEGAS does that while playing)
struct SwitchCase {
    const char* name;
    Raw s1, s2;
    int keep;
    uint32_t hash;
    int changed;
    std::vector<Sample> samples;
};
static const std::vector<SwitchCase> kSwitch44100 = {
    {"15 dB notch, then air boost", {0, 0, -150, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1}, {60, 0, -150, 40, 10, 10, 10, 10, 100, 300, 1500, 9000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 1, 0x78787e1du, 59999, {{1, 0xb97c04c9}, {10000, 0xbe682bff}, {20000, 0x3cdab128}, {30000, 0x396afcf5}, {40000, 0x3d49416f}, {50000, 0x3c20e798}}},
    {"Shelves, then a bell in their place", {60, 0, 0, 40, 10, 10, 10, 10, 150, 300, 1000, 6000, 1, 2, 2, 3, 3, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, {0, -50, 70, 0, 10, 10, 10, 10, 150, 500, 1000, 6000, 1, 2, 2, 3, 3, 1, 1, 2, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 1, 0x5927b247u, 60000, {{0, 0xba038781}, {10000, 0xbeec845f}, {20000, 0xbe5d88c6}, {30000, 0x39c6a84b}, {40000, 0x3caecde2}, {50000, 0x3b8f2eb0}}},
    {"Same bands, other gains (state not kept)", {60, 60, 60, 60, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, {-30, 30, -30, 30, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0, 0xb4473951u, 60000, {{0, 0xba035f68}, {10000, 0xbf25b636}, {20000, 0xbeaee97c}, {30000, 0x3a08273d}, {40000, 0x3c384be9}, {50000, 0x3c101a4b}}},
};
static const std::vector<SwitchCase> kSwitch48000 = {
    {"15 dB notch, then air boost", {0, 0, -150, 0, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1}, {60, 0, -150, 40, 10, 10, 10, 10, 100, 300, 1500, 9000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 1, 0xd909dc84u, 59999, {{1, 0xb97aefb4}, {10000, 0xbe6e8129}, {20000, 0x3d178074}, {30000, 0x396c13e1}, {40000, 0x3ce2d1af}, {50000, 0x3c24a4be}}},
    {"Shelves, then a bell in their place", {60, 0, 0, 40, 10, 10, 10, 10, 150, 300, 1000, 6000, 1, 2, 2, 3, 3, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, {0, -50, 70, 0, 10, 10, 10, 10, 150, 500, 1000, 6000, 1, 2, 2, 3, 3, 1, 1, 2, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 1, 0xb0435510u, 60000, {{0, 0xba037e07}, {10000, 0xbeeb505b}, {20000, 0xbe57a33b}, {30000, 0x39c543c1}, {40000, 0x3ce4246c}, {50000, 0x3b908965}}},
    {"Same bands, other gains (state not kept)", {60, 60, 60, 60, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, {-30, 30, -30, 30, 10, 10, 10, 10, 100, 300, 1000, 5000, 1, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1}, 0, 0x75f6636au, 60000, {{0, 0xba03592d}, {10000, 0xbf230d83}, {20000, 0xbeacfa22}, {30000, 0x3a074fd1}, {40000, 0x3c2a8c24}, {50000, 0x3c0f8125}}},
};

static bool matches(uint32_t hash, int changedWant, const std::vector<Sample>& samples, const std::vector<float>& l, const std::vector<float>& r, const std::vector<float>& l0,
                    const std::vector<float>& r0, std::string* why)
{
    char b[160];
    if (fnvInter(l, r) != hash) {
        std::snprintf(b, sizeof b, "hash %08x, expected %08x", fnvInter(l, r), hash);
        *why = b;
        return false;
    }
    int changed = 0;
    for (size_t k = 0; k < l.size(); ++k) changed += (bitsOf(l[k]) != bitsOf(l0[k])) + (bitsOf(r[k]) != bitsOf(r0[k]));
    if (changed != changedWant) {
        *why = "changed samples " + std::to_string(changed) + ", expected " + std::to_string(changedWant);
        return false;
    }
    for (const Sample& s : samples) {
        const float f = (s.index & 1) ? r[(size_t)(s.index / 2)] : l[(size_t)(s.index / 2)];
        if (bitsOf(f) != s.bits) {
            std::snprintf(b, sizeof b, "sample %d is %08x, expected %08x", s.index, bitsOf(f), s.bits);
            *why = b;
            return false;
        }
    }
    return true;
}

static void testKernel(const std::vector<Case>& cases, int rate)
{
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : cases) {
        vegeq::Eq eq;
        eq.prepare(settingsOf(c.s), rate, false);
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += c.chunk) eq.process(&l[(size_t)at], &r[(size_t)at], std::min(c.chunk, kFrames - at));
        std::string why;
        check(matches(c.hash, c.changed, c.samples, l, r, l0, r0, &why), strfmt("EQ %s at %d Hz: all 60000 samples as the DLL's", c.name, rate), why);
    }
}

static void testSwitchKernel(const std::vector<SwitchCase>& cases, int rate)
{
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const SwitchCase& c : cases) {
        vegeq::Eq eq;
        eq.prepare(settingsOf(c.s1), rate, false);
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) {
            if (at == 15360) eq.prepare(settingsOf(c.s2), rate, c.keep != 0);
            eq.process(&l[(size_t)at], &r[(size_t)at], std::min(512, kFrames - at));
        }
        std::string why;
        check(matches(c.hash, c.changed, c.samples, l, r, l0, r0, &why), strfmt("EQ switch '%s' at %d Hz: as the DLL's", c.name, rate), why);
    }
}

static std::unique_ptr<AudioPlugin> make(const std::string& format, const std::string& uid, double rate)
{
    AudioPluginDesc d;
    d.format = format;
    d.uid = uid;
    std::string err;
    std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, &err);
    if (p) p->activate(rate, 1024);
    return p;
}

static void testPlugin(const std::vector<Case>& cases, const std::vector<SwitchCase>& switches, int rate)
{
    const std::string tag = strfmt(" at %d Hz", rate);
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : cases) {
        auto p = make("BUILTIN", "reno.trkeq", rate);
        if (!p) { check(false, "the built-in Track EQ is made" + tag); return; }
        p->loadState(vegeq::encode(settingsOf(c.s)));
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += c.chunk) p->process(&l[(size_t)at], &r[(size_t)at], std::min(c.chunk, kFrames - at), at, true);
        std::string why;
        check(matches(c.hash, c.changed, c.samples, l, r, l0, r0, &why), strfmt("plug-in %s%s: the DLL's samples", c.name, tag.c_str()), why);
    }
    // settings changed while it runs: by loading another saved form between two blocks (the sections keep what they hold)
    for (const SwitchCase& c : switches) {
        if (!c.keep) continue;
        auto p = make("BUILTIN", "reno.trkeq", rate);
        p->loadState(vegeq::encode(settingsOf(c.s1)));
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) {
            if (at == 15360) p->loadState(vegeq::encode(settingsOf(c.s2)));
            p->process(&l[(size_t)at], &r[(size_t)at], std::min(512, kFrames - at), at, true);
        }
        std::string why;
        check(matches(c.hash, c.changed, c.samples, l, r, l0, r0, &why), strfmt("plug-in switch '%s'%s: the DLL's samples", c.name, tag.c_str()), why);
    }
    // a jump in time (seek) restarts it: the same input again from the start gives the same output
    {
        auto p = make("BUILTIN", "reno.trkeq", rate);
        p->loadState(vegeq::encode(settingsOf(cases[3].s)));
        std::vector<float> l = l0, r = r0;
        p->process(l.data(), r.data(), 2048, 0, true);
        std::vector<float> l2 = l0, r2 = r0;
        p->process(l2.data(), r2.data(), 2048, 0, true);      // position 0 again: a jump back
        bool same = true;
        for (int k = 0; k < 2048; ++k) same = same && bitsOf(l[(size_t)k]) == bitsOf(l2[(size_t)k]) && bitsOf(r[(size_t)k]) == bitsOf(r2[(size_t)k]);
        check(same, "going back to the start restarts the EQ" + tag);
    }
}

static void testSettings()
{
    vegeq::Settings d;
    const std::string st = vegeq::encode(d);
    const std::string raw = base64Decode(st.substr(2));
    check(raw.size() == 148 && (uint8_t)raw[0] == 0x94 && std::memcmp(raw.data() + 4, vegeq::persistGuid(), 16) == 0, "the saved form is 148 bytes: length 0x94, the plug-in's GUID, 128 bytes of settings");
    int32_t f0, f1, f2, f3;
    std::memcpy(&f0, raw.data() + 20 + 8 * 4, 4);
    std::memcpy(&f1, raw.data() + 20 + 9 * 4, 4);
    std::memcpy(&f2, raw.data() + 20 + 10 * 4, 4);
    std::memcpy(&f3, raw.data() + 20 + 11 * 4, 4);
    check(f0 == 100 && f1 == 300 && f2 == 1000 && f3 == 5000, "  the defaults: bands at 100, 300, 1000 and 5000 Hz");
    vegeq::Settings s;
    check(vegeq::decode(st, &s) && vegeq::encode(s) == st, "  and reads back to the same bytes");
    check(vegeq::presets().size() == 13, "thirteen presets (the two defaults and eleven more)");
    check(std::string(vegeq::presets()[3].name) == "60 Hz hum notch using four stacked filters" && vegeq::presets()[3].s.freq[2] == 60 && vegeq::presets()[3].s.bw[0] == 3,
          "  60 Hz hum notch: four bells at 60 Hz, bandwidth 0.3");
    check(std::string(vegeq::presets()[12].name) == "Remove very low and inaudible frequencies below 20 Hz" && vegeq::presets()[12].s.freq[0] == 20, "  Remove very low and inaudible frequencies below 20 Hz");
    vegeq::Settings w;
    for (int b = 0; b < 4; ++b) {
        w.gain[b] = 9999;
        w.bw[b] = -5;
        w.freq[b] = 5;
        w.type[b] = 9;
        w.rolloff[b] = 0;
        w.enable[b] = 7;
    }
    w.selected = 9;
    w.channel[5] = 3;
    vegeq::sanitize(w);
    check(w.gain[0] == 150 && w.bw[0] == 1 && w.freq[0] == 20 && w.type[0] == 3 && w.rolloff[0] == 1 && w.enable[0] == 1 && w.selected == 3 && w.channel[5] == 1,
          "out of range values are limited to the dialog's ranges");
    check(!vegeq::decode("S:AAAA", &s) && !vegeq::decode("garbage", &s), "a short or foreign form is refused");

    auto p = make("BUILTIN", "reno.trkeq", 48000);
    check(p != nullptr, "the built-in Track EQ is made");
    if (!p) return;
    check(p->paramCount() == 30 && p->paramName(0) == "Band 1 Frequency" && p->paramName(4) == "Band 1 Gain" && p->paramName(11) == "Band 4 Bandwidth" && p->paramName(12) == "Band 1 Rolloff" &&
              p->paramName(19) == "Band 4 Mode" && p->paramName(20) == "Band 1 Enable" && p->paramName(24) == "Left Enable" && p->paramName(29) == "LFE Enable",
          "thirty settings: four bands of six, and the six channel switches");
    double lo = 0, hi = 0;
    check(p->paramRange(0, &lo, &hi) && lo == 20 && hi == 20000, "frequency 20..20000 Hz");
    check(p->paramRange(9, &lo, &hi) && lo == 0.1 && hi == 4, "bandwidth 0.1..4 octaves");
    check(p->paramRange(13, &lo, &hi) && lo == 1 && hi == 4, "roll-off 1..4");
    check(p->saveState() == st, "the default plug-in saves as the default form");
    check(p->programCount() == 13 && p->programName(0) == "(Default)" && p->programName(2) == "15 dB notch centered at 1000 Hz", "VEGAS's thirteen presets");
    for (int i = 0; i < 13; ++i) {
        const bool ok = p->setProgram(i) && p->saveState() == vegeq::encode(vegeq::presets()[(size_t)i].s) && p->currentProgram() == i;
        check(ok, strfmt("preset %d (%s) saves as VEGAS's form", i, vegeq::presets()[(size_t)i].name));
    }
    p->setProgram(2);
    check(p->paramText(2) == "1000 Hz" && p->paramText(6) == "-15.0 dB", "  band 3 at 1000 Hz, -15.0 dB: " + p->paramText(2) + ", " + p->paramText(6));
    check(p->loadState("reno-builtin-audio 1\nBand 1 Gain=6\n") && p->saveState() == st, "settings of an older Reno load as the defaults");
    check(!p->loadState("garbage"), "unknown text is refused");

    // the response (what the page's graph draws): a bell of +6 dB at 1000 Hz, a notch, a shelf
    {
        vegeq::Settings b;
        b.gain[2] = 60;
        b.bw[2] = 10;
        b.freq[2] = 1000;
        b.type[2] = 2;
        for (int k : {0, 1, 3}) b.gain[k] = 0;
        vegeq::Eq eq;
        eq.prepare(b, 48000, false);
        const double at1k = eq.responseDb(1000), far = eq.responseDb(20);
        check(std::fabs(at1k - 6.0) < 0.1 && std::fabs(far) < 0.5, strfmt("the graph's curve: a +6 dB bell at 1000 Hz reads %.3f dB there and %.3f dB at 20 Hz", at1k, far));
        b.gain[2] = 0;
        b.gain[0] = 60;
        b.freq[0] = 200;
        b.type[0] = 1;
        b.rolloff[0] = 1;
        eq.prepare(b, 48000, false);
        check(eq.responseDb(20) > 5.5 && eq.responseDb(20000) < 0.5, strfmt("  a +6 dB low shelf at 200 Hz: %.2f dB at 20 Hz, %.2f dB at 20 kHz", eq.responseDb(20), eq.responseDb(20000)));
    }
}

static void testDxFallback()
{
    const std::string dx = "dshow:{8CB69A0A-10E8-11D2-9B89-00104B8D13C2}";
    check(builtinUidForDx(dx) == "reno.trkeq", "the DirectX Track EQ stands for the built-in one");
    check(dxUidForBuiltin("reno.trkeq") == dx, "the built-in one is saved as the DirectX Track EQ");
    AudioPluginDesc d;
    d.format = "DX";
    d.uid = dx;
    d.name = "VEGAS Track EQ";
    std::string err;
    auto p = createAudioPlugin(d, &err);      // no DirectX here (or no VEGAS): Reno's version
    check(p != nullptr, "a DX Track EQ slot makes a plug-in", err);
    if (p) {
        check(p->desc.format == "DX" && p->desc.uid == dx, "  that keeps the slot's description");
        check(builtinPageUid(p->desc) == "reno.trkeq" || p->desc.vendor != kBuiltinAudioVendor, "  and shows the Track EQ page");
    }
}

int main()
{
    testKernel(k44100, 44100);
    testKernel(k48000, 48000);
    testSwitchKernel(kSwitch44100, 44100);
    testSwitchKernel(kSwitch48000, 48000);
    testPlugin(k44100, kSwitch44100, 44100);
    testPlugin(k48000, kSwitch48000, 48000);
    testSettings();
    testDxFallback();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
