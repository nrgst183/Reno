// Video Scopes analyse the picture on the interface thread (every 100 ms while it changes), so one analysis must stay short. Times
// setImage() on a 1280x720 picture and prints what the scopes then report (stats and the histograms' mean / deviation for each
// type), which must not change when the analysis is made faster: "SCOPES <number>" lines are compared between builds.
//
//   scope_perf_test <output folder>
#include "../../src/app/panels.h"
#include "uitestlib.h"
#include <QApplication>
#include <QImage>
#include <QSettings>
#include <cstdio>
#include <ctime>

using namespace reno::app;
using uitest::check;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoScopePerfTest");
    QApplication::setApplicationName("scope_perf_test");
    QSettings().clear();
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    QImage img(1280, 720, QImage::Format_RGB32);
    uint32_t seed = 12345;
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = (QRgb*)img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            seed = seed * 1664525u + 1013904223u;
            line[x] = qRgb((x * 255 / img.width() + (seed >> 28)) & 255, (y * 255 / img.height() + (seed >> 24)) & 255, ((x + y) * 3 + (seed >> 20)) & 255);
        }
    }
    VideoScopes scopes(nullptr);
    scopes.resize(500, 300);
    scopes.setImage(img);
    // the thread's own CPU time: the machine may be busy with other work
    auto cpuMs = [] { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6; };
    const double c0 = cpuMs();
    const int rounds = 10;
    for (int k = 0; k < rounds; ++k) scopes.setImage(img);
    const double ms = (cpuMs() - c0) / rounds;
    std::printf("analysis of a 1280x720 picture: %.1f ms of CPU\n", ms);
    const VideoScopes::Stats s = scopes.stats();
    std::printf("SCOPES stats %d %.9g %.9g %d %d\n", s.pixels, s.meanLuma, s.stdDevLuma, s.minLuma, s.maxLuma);
    for (int type = 0; type <= 6; ++type) {
        double mean = 0, sd = 0;
        scopes.histogramStats((VideoScopes::HistogramType)type, mean, sd);
        std::printf("SCOPES hist %d %.9g %.9g\n", type, mean, sd);
    }
    check(ms < 100, "one analysis takes under 100 ms of CPU", std::to_string(ms) + " ms");
    return uitest::summary();
}
