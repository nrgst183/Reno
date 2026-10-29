// vegshort: lists the events of .veg projects that are shorter than a limit (default 100 ms) - where they sit on the
// frame grid and the sample grid - so that very short events can be looked at.
//   vegshort [--ms N] project.veg...
#include "../../src/vegas/vegas.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace reno;

int main(int argc, char** argv)
{
    double limit = 0.1;
    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--ms") && a + 1 < argc) { limit = std::atof(argv[++a]) / 1000.0; continue; }
        Project p;
        const VegasImportResult r = importVegasProject(argv[a], p);
        if (!r.ok) { std::printf("%s: FAILED %s\n", argv[a], r.error.c_str()); continue; }
        const double fps = p.settings.fps();
        const int sr = p.settings.sampleRate;
        int n = 0, total = 0, subframe = 0, offFrame = 0;
        double minLen = 1e9;
        for (const auto& t : p.tracks)
            for (const auto& e : t.events) {
                ++total;
                if (e.length >= limit) continue;
                ++n;
                minLen = std::min(minLen, e.length);
                const double fs = e.start * fps;
                if (e.length * fps < 1) ++subframe;
                if (std::fabs(fs - std::round(fs)) > 1e-3) ++offFrame;
                if (n <= 12)
                    std::printf("  %s track %llu start %.7f (%.4f fr, %.3f smp) len %.7f (%.4f fr, %.3f smp) off %.7f rate %.3f fade %.3f/%.3f %s\n",
                                t.type == TrackType::Audio ? "A" : "V", (unsigned long long)t.id, e.start, fs, e.start * sr, e.length, e.length * fps,
                                e.length * sr, e.offset, e.rate, e.fadeIn, e.fadeOut, e.loop ? "loop" : "");
            }
        std::printf("%s: %.3f fps %d Hz, %d events, %d shorter than %.0f ms (%d under one frame, %d starting off the frame grid), shortest %.6f s\n", argv[a], fps, sr,
                    total, n, limit * 1000, subframe, offFrame, n ? minLen : 0.0);
    }
    return 0;
}
