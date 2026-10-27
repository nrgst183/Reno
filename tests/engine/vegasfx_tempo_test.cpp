// The project's tempo reaches the effects with tempo sync: Simple Delay with a quarter note delay is 0.5 s at 120 BPM, 2/3 s
// at 90 and a second at 60 (ProjectTempoScope is what the mixer puts around every effect chain), and a tempo that changes
// while the effect runs is taken up.
//
//   vegasfx_tempo_test
#include "../../src/audiofx/vegaspack1.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno;

static int gFail = 0;
static void check(bool ok, const char* what, const std::string& detail = std::string())
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++gFail;
}

// where the echo of an impulse comes, with the tempo of each block given by 'bpmAt' (by block number)
static long echoAt(double (*bpmAt)(int), int blocks)
{
    const vegpack1::Info* info = vegpack1::findInfo("reno.simpledelay");
    std::unique_ptr<AudioPlugin> p = vegpack1::create("reno.simpledelay");
    std::vector<int32_t> s(12, 0);
    s[1] = 500;
    s[3] = 0;
    s[4] = -800;
    s[6] = 13;          // quarter note
    s[7] = 1000;
    s[8] = 1;           // tempo sync
    s[10] = 1000;
    p->loadState(vegpack1::encode(*info, s.data()));
    p->activate(48000, 512);
    long found = -1;
    float best = 0;
    for (int b = 0; b < blocks; ++b) {
        std::vector<float> l(512, 0.f), r(512, 0.f);
        if (b == 0) l[0] = r[0] = 1.f;
        const ProjectTempoScope scope(bpmAt(b), 4);
        p->process(l.data(), r.data(), 512, (int64_t)b * 512, true);
        for (int k = 0; k < 512; ++k)
            if (std::fabs(l[k]) > best) {
                best = std::fabs(l[k]);
                found = (long)b * 512 + k;
            }
    }
    return found;
}

int main()
{
    const long at120 = echoAt([](int) { return 120.0; }, 200);
    const long at90 = echoAt([](int) { return 90.0; }, 200);
    const long at60 = echoAt([](int) { return 60.0; }, 200);
    check(at120 == 24000, "120 BPM: a quarter note is 24000 frames at 48 kHz", std::to_string(at120));
    check(at90 == 32000, "90 BPM: 32000 frames", std::to_string(at90));
    check(at60 == 48000, "60 BPM: 48000 frames", std::to_string(at60));
    // outside a mix the tempo is 120 BPM in 4/4
    check(projectTempo().bpm == 120.0 && projectTempo().beatsPerMeasure == 4, "default tempo");
    {
        const ProjectTempoScope a(100.0, 3);
        check(projectTempo().bpm == 100.0 && projectTempo().beatsPerMeasure == 3, "scope sets the tempo");
        {
            const ProjectTempoScope b(77.0, 5);
            check(projectTempo().bpm == 77.0, "scopes nest");
        }
        check(projectTempo().bpm == 100.0, "and give it back");
    }
    check(projectTempo().bpm == 120.0, "back to the default");
    std::printf("%s\n", gFail ? "FAILED" : "ALL PASSED");
    return gFail ? 1 : 0;
}
