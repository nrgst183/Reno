// vegnormalize: Edit > Switches > Normalize in a .veg. VEGAS keeps a normalised event's measured gain apart from its
// own level (a flag and a second gain in the event's record); Reno reads them into Event::normalize and
// Event::normalizeGain and writes them back when they change.
//
//   vegnormalize [folder with the sample projects]     (default $RENO_SAMPLES)
#include "../../src/vegas/vegas.h"
#include <cmath>
#include <cstdio>
#include <string>
#include "../testdata.h"

using namespace reno;

static int passed = 0, failed = 0;
static void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? passed : failed)++;
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    if (testDataMissing(dir)) return 0;
    // a sample with a normalised sound event on an audio track (the first found; the project is saved to /tmp, never changed)
    std::string src;
    size_t ti = 0, ei = 0;
    Project p;
    for (const std::string& f : testVegFiles(dir)) {
        Project cand;
        if (!importVegasProject(f, cand).ok) continue;
        for (size_t t = 0; src.empty() && t < cand.tracks.size(); ++t)
            for (size_t e = 0; src.empty() && e < cand.tracks[t].events.size(); ++e)
                if (cand.tracks[t].type == TrackType::Audio && cand.tracks[t].events[e].normalize) { src = f; ti = t; ei = e; }
        if (!src.empty()) { p = cand; break; }
    }
    if (src.empty()) { std::printf("SKIP: no sample project has a normalised sound event\n"); return 0; }
    check(true, "a sample with a normalised sound event opens");
    Event& a = p.tracks[ti].events[ei];
    check(a.normalize && a.normalizeGain > 0, "its sound event is normalised: a normalising gain is kept apart from its level");
    const double origGain = a.gain, origNormalize = a.normalizeGain;
    (void)origNormalize;
    (void)origGain;
    const std::string out = "/tmp/vegnormalize_test.veg";
    // switched off: the flag goes, the event's own level stays
    a.normalize = false;
    a.gain = 0.5;
    check(saveVegasProject(p, out).ok, "saved with Normalize off and the level at 0.5");
    Project q;
    check(importVegasProject(out, q).ok && q.tracks.size() == p.tracks.size() && q.tracks[ti].events.size() == p.tracks[ti].events.size(), "read back");
    if (q.tracks.size() == p.tracks.size() && q.tracks[ti].events.size() == p.tracks[ti].events.size()) {
        const Event& b = q.tracks[ti].events[ei];
        check(!b.normalize && std::fabs(b.gain - 0.5) < 1e-4, "Normalize off, level 0.5");
        // switched on again with a new measured gain
        q.tracks[ti].events[ei].normalize = true;
        q.tracks[ti].events[ei].normalizeGain = 2.25;
        check(saveVegasProject(q, out).ok, "saved with Normalize on (2.25)");
        Project r;
        check(importVegasProject(out, r).ok && r.tracks.size() == q.tracks.size() && r.tracks[ti].events.size() == q.tracks[ti].events.size(), "read back again");
        if (r.tracks.size() == q.tracks.size() && r.tracks[ti].events.size() == q.tracks[ti].events.size()) {
            const Event& c = r.tracks[ti].events[ei];
            check(c.normalize && std::fabs(c.normalizeGain - 2.25) < 1e-4 && std::fabs(c.gain - 0.5) < 1e-4, "Normalize on, normalising gain 2.25, level still 0.5");
        }
    }
    std::remove(out.c_str());
    std::remove((out + ".bak").c_str());
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
