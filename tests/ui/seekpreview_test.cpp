// Moving the cursor about a big video with key frames seconds apart, stopped: the preview shows the nearest key frame at once and
// the exact picture after it (Playback, MediaReadHints::quick), and a cursor moved again before a picture is done stops the old
// decoding at once (MediaReadHints::cancel), so a few quick clicks cost one decode, not one each. The exact picture is the one a
// fresh render of the project gives.
//
//   seekpreview_test <video with key frames seconds apart, 30 s or longer>
// Build: uibuild seekpreview_test tests/ui/seekpreview_test.cpp
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <chrono>
#include <cmath>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Shown { double at; double seconds; QImage image; };

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    if (argc < 2) { printf("usage: seekpreview_test <video>\n"); return 2; }
    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t mid = app.importMedia(argv[1], &err);
    check(mid != 0, "the video is imported", err);
    double len = 0;
    app.edit("clip", [&](Project& p) {
        p.settings.width = 1280;
        p.settings.height = 720;
        p.settings.fpsNum = 60;
        p.settings.fpsDen = 1;
        const MediaItem* m = p.findMedia(mid);
        len = m ? m->info.duration : 0;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = mid;
        e.length = len;
        t.events.push_back(e);
        p.tracks.push_back(t);
    });
    Playback pb;
    pb.setPreviewSize(640, 360, false);
    std::vector<Shown> shown;
    pb.onFrame = [&](const QImage& img, double s, double) { shown.push_back({now(), s, img}); };
    auto waitFor = [&](double t, double limit) {
        // until a picture for time t has come and nothing is being rendered
        const double until = now() + limit;
        while (now() < until) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (pb.renderingFor() == 0.0 && !shown.empty() && std::fabs(shown.back().seconds - t) < 1e-9) {
                // the last picture rendered may still be on its way to the UI
                const double settle = now() + 0.1;
                while (now() < settle) { QCoreApplication::processEvents(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
                return true;
            }
        }
        return false;
    };

    app.setCursor(1.0);
    waitFor(1.0, 10);

    // ---- one click far in: a picture almost at once, the exact one after
    double firstWorst = 0, exactWorst = 0;
    bool exactRight = true, twoStep = false;
    const double places[] = {len * 0.47 + 0.123, len * 0.81 + 0.3, len * 0.15 + 0.7, len * 0.66 + 0.05};
    for (double t : places) {
        t = std::floor(t * 60) / 60 + 1.0 / 120;  // the middle of a frame
        shown.clear();
        const double a = now();
        app.setCursor(t);
        const bool done = waitFor(t, 20);
        if (!done || shown.empty()) { exactRight = false; continue; }
        firstWorst = std::max(firstWorst, shown.front().at - a);
        exactWorst = std::max(exactWorst, shown.back().at - a);
        if (shown.size() >= 2) twoStep = true;
        if (getenv("RENO_DEBUG_SEEK")) for (const Shown& x : shown) fprintf(stderr, "  shown %.3f at %.3f\n", x.seconds, x.at);
        printf("  %.2f s: first picture after %.0f ms, exact after %.0f ms (%zu pictures)\n", t, (shown.front().at - a) * 1000, (shown.back().at - a) * 1000, shown.size());
        // the last picture is exact: the same as a fresh preview of that place
        Playback other;
        other.setPreviewSize(640, 360, false);
        QImage want;
        other.onFrame = [&](const QImage& img, double s, double) { if (std::fabs(s - t) < 1e-9) want = img; };
        other.requestFrame();
        const double until = now() + 20;
        while (now() < until && (want.isNull() || other.renderingFor() > 0)) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (const double settle = now() + 0.1; now() < settle;) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (want.isNull() || want != shown.back().image) exactRight = false;
    }
    check(twoStep, "far into the file the preview shows the nearest key frame first, then the exact picture");
    check(exactRight, "the last picture shown is the exact one (the same as a fresh preview gives)");
    check(firstWorst < 0.25, "a picture comes almost at once after a click far into the file", std::to_string(firstWorst * 1000) + " ms");

    // ---- five clicks 40 ms apart: the old decodes give way, the last place's exact picture comes in about one decode
    {
        shown.clear();
        double t = 0;
        const double a = now();
        for (int k = 0; k < 5; ++k) {
            t = std::floor((len * (0.2 + 0.13 * k) + 3.3) * 60) / 60 + 1.0 / 120;
            app.setCursor(t);
            const double until = now() + 0.04;
            while (now() < until) { QCoreApplication::processEvents(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        }
        const bool done = waitFor(t, 30);
        const double total = now() - a;
        printf("  five clicks: the last exact picture after %.0f ms (one exact decode took at most %.0f ms)\n", total * 1000, exactWorst * 1000);
        check(done && total < 0.2 + exactWorst * 1.6, "five quick clicks cost about one decode, not five", std::to_string(total * 1000) + " ms");
    }
    return uitest::summary();
}
