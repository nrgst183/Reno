// reverse_end_test: where VEGAS mirrors a reversed event (a reversed subclip), and the importer's use of it.
//
//   reverse_end_test [folder]      (default $RENO_SAMPLES; it is only read)
//
// A reversed event is a subclip with its reverse flag; the event's offset counts from the subclip's END. When the
// recorded subclip is only a little longer than the picture (628.2457 s against 628.2333 s of video, for one .mp4 file
// of the sample projects), VEGAS mirrors about the end of the picture. reversedClipEnd decides that; it is checked with
// made-up file information, then the reversed events of every sample project are checked to never read past the picture's end.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegorigin.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main(int argc, char** argv)
{
    MediaInfo v;
    v.hasVideo = true; v.fps = 30.0; v.videoDuration = 628.2333;
    // the subclip runs 12 ms past the picture (under 1.5 frames of 33 ms) -> the picture's end
    check(near(reversedClipEnd(0.0, 628.2457251, &v), 628.2333), "slightly longer than the picture: mirrors at the picture's end");
    // a subclip that stops before the picture's end stays
    check(near(reversedClipEnd(10.0, 100.0, &v), 110.0), "inside the picture: unchanged");
    // far longer (an audio take of a file with a shorter picture): unchanged
    check(near(reversedClipEnd(0.0, 640.0, &v), 640.0), "far longer than the picture: unchanged");
    // exactly 1.5 frames over is still clamped, a little more is not
    check(near(reversedClipEnd(0.0, 628.2333 + 1.5 / 30.0, &v), 628.2333), "1.5 frames over: clamped");
    check(near(reversedClipEnd(0.0, 628.2333 + 1.6 / 30.0, &v), 628.2333 + 1.6 / 30.0), "1.6 frames over: unchanged");
    // no information, a sound file, a still, no length known
    check(near(reversedClipEnd(0.0, 628.2457251, nullptr), 628.2457251), "no file information: unchanged");
    MediaInfo a; a.hasVideo = false; a.videoDuration = 0;
    check(near(reversedClipEnd(0.0, 5.0, &a), 5.0), "sound file: unchanged");
    MediaInfo s = v; s.isStill = true;
    check(near(reversedClipEnd(0.0, 628.2457251, &s), 628.2457251), "still: unchanged");
    MediaInfo u = v; u.videoDuration = 0;
    check(near(reversedClipEnd(0.0, 628.2457251, &u), 628.2457251), "picture length unknown: unchanged");
    check(reversedClipEnd(5.0, -1.0, nullptr) >= 5.0, "never before the subclip's start");

    const std::string folder = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    int checkedEvents = 0, projects = 0;
    bool all = true;
    for (const std::string& veg : testVegFiles(folder)) {
        Project p;
        if (!importVegasProject(veg, p).ok) continue;
        ++projects;
        for (const auto& t : p.tracks)
            for (const auto& e : t.events) {
                if (!e.reverse) continue;
                const MediaItem* m = p.findMedia(e.mediaId);
                if (!m || m->offline || !m->info.hasVideo || m->info.isStill || m->info.videoDuration <= 0) continue;
                ++checkedEvents;
                const double top = e.offset + e.length * e.rate;     // where the backwards reading starts
                if (top > m->info.videoDuration + 0.0015) { all = false; std::printf("  %s: event at %.3f reads from %.4f, picture ends %.4f\n", veg.c_str(), e.start, top, m->info.videoDuration); }
            }
    }
    if (!projects) std::printf("SKIP the sample projects (none under %s)\n", folder.c_str());
    else if (!checkedEvents) std::printf("SKIP the sample projects' reversed events (their reversed media is missing)\n");
    else check(all, "samples: no reversed event with a picture reads from past the picture's end");
    std::printf("%d passed, %d failed (%d reversed events checked)\n", gPass, gFail, checkedEvents);
    return gFail ? 1 : 0;
}
