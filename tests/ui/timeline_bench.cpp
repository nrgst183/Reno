// How long the timeline takes to draw big projects (thousands of tiny events), zoomed out to the whole project and zoomed in, with
// stand-in media (every file the project names is played by one of the clips in the media folder, as perf_bench does), and how long
// an edit (a step of undo) and an undo take. Prints "BENCH <project> <measure> <value> <unit>" lines.
//
//   timeline_bench <media folder> project.veg...
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/timeline.h"
#include "../../src/app/appstate.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <cstdio>

using namespace reno;
using namespace reno::app;

static void report(const std::string& name, const char* what, double v, const char* unit)
{
    std::printf("BENCH %-28s %-26s %10.3f %s\n", name.substr(0, 28).c_str(), what, v, unit);
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: timeline_bench <media folder> project.veg...\n");
        return 2;
    }
    uitest::setup({}, {});
    const std::string dir = argv[1];
    AppState& app = AppState::get();
    // every file becomes a stand-in of its kind
    app.mediaOpener = [dir](const std::string& path, std::string* err) -> std::unique_ptr<MediaReader> {
        const std::string low = toLower(path);
        std::string n = "clip720.mp4";
        if (endsWithNoCase(low, ".png") || endsWithNoCase(low, ".jpg") || endsWithNoCase(low, ".bmp") || endsWithNoCase(low, ".gif")) n = "still.png";
        else if (endsWithNoCase(low, ".wav") || endsWithNoCase(low, ".mp3") || endsWithNoCase(low, ".ogg") || endsWithNoCase(low, ".flac")) n = "tone.wav";
        return openMedia(dir + "/" + n, err);
    };
    Playback playback;
    TimelineWidget tl(&playback);
    tl.resize(1600, 700);
    tl.show();
    uitest::settle(200);
    for (int i = 2; i < argc; ++i) {
        const std::string path = argv[i];
        std::string name = pathFileName(path);
        std::string err;
        if (!app.open(path, &err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            continue;
        }
        tl.zoomToFit();
        uitest::settle(300);
        QPixmap pm(tl.size());
        auto paintMs = [&](int n) {
            QElapsedTimer t;
            t.start();
            for (int k = 0; k < n; ++k) tl.render(&pm);
            return (double)t.nsecsElapsed() / 1.0e6 / n;
        };
        paintMs(1);
        uitest::settle(1500);  // thumbnails and waveforms are read in the background
        report(name, "paint zoomed out", paintMs(10), "ms");
        for (int k = 0; k < 6; ++k) tl.zoomIn();
        uitest::settle(1500);
        report(name, "paint zoomed in", paintMs(10), "ms");
        tl.zoomToFit();
        // an edit (one undo step), an undo and a redo
        QElapsedTimer t;
        t.start();
        const int n = 10;
        for (int k = 0; k < n; ++k) app.edit("Bench", [](Project& p) { p.masterVolume = p.masterVolume == 1.0 ? 0.5 : 1.0; });
        report(name, "edit (undo step)", (double)t.nsecsElapsed() / 1.0e6 / n, "ms");
        t.restart();
        for (int k = 0; k < n; ++k) app.undo();
        report(name, "undo", (double)t.nsecsElapsed() / 1.0e6 / n, "ms");
        t.restart();
        for (int k = 0; k < n; ++k) app.redo();
        report(name, "redo", (double)t.nsecsElapsed() / 1.0e6 / n, "ms");
    }
    return 0;
}
