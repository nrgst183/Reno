// Measures what a proxy setting costs to make and to read (the numbers behind VideoProxyOptions' defaults in
// src/media/proxy.cpp): time to make a proxy of a stretch of the file, its size, and the time of a read at a random place.
//
//   proxy_bench <video file> <scratch folder> [from seconds] [to seconds] [max width] [max height] [setting ...]
// A setting is "<key frame seconds>:<crf>:<encoder options with / for spaces>", e.g. 0.25:28:preset=ultrafast/tune=fastdecode;
// "orig" reads the original instead. Without settings, the usual set is measured.
#include "../../src/app/autoproxy.h"
#include "../../src/vegas/vegas.h"
#include "../../src/engine/profile.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>

using namespace reno;

static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void readCosts(const std::string& path, double from, double to, int w, int h, const char* label)
{
    std::string err;
    auto r = openMedia(path, &err);
    if (!r) { std::printf("  cannot open %s: %s\n", path.c_str(), err.c_str()); return; }
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> at(from + 1, to - 1);
    std::vector<double> ms;
    for (int k = 0; k < 80; ++k) {
        const double t = at(rng);
        const double a = nowMs();
        ImagePtr img = r->videoFrame(t, w, h);
        ms.push_back(nowMs() - a);
        if (!img) std::printf("  (no frame at %.2f)\n", t);
    }
    // short stretches of play after a cut: 5 frames in a row at a random place
    std::vector<double> run;
    for (int k = 0; k < 40; ++k) {
        const double t = at(rng);
        const double a = nowMs();
        for (int j = 0; j < 5; ++j) r->videoFrame(t + j / 25.0, w, h);
        run.push_back(nowMs() - a);
    }
    std::sort(ms.begin(), ms.end());
    std::sort(run.begin(), run.end());
    double sum = 0;
    for (double v : ms) sum += v;
    double rsum = 0;
    for (double v : run) rsum += v;
    std::printf("  %-10s random read: mean %6.1f ms  median %6.1f  p95 %6.1f  worst %6.1f | cut + 5 frames: mean %6.1f  worst %6.1f\n", label, sum / ms.size(), ms[ms.size() / 2],
                ms[ms.size() * 95 / 100], ms.back(), rsum / run.size(), run.back());
}

// proxy_bench --make <file> ...: makes each file's proxy as Reno does by itself (the defaults, one encoder thread), beside the file
// (or in the folder of RENO_PROXY_FOLDER), and says how long it took and how big it is.
static int makeAll(int argc, char** argv)
{
    if (std::getenv("RENO_PROXY_FOLDER")) setVideoProxyFolder(std::getenv("RENO_PROXY_FOLDER"));
    for (int k = 2; k < argc; ++k) {
        const std::string path = argv[k];
        std::string err;
        auto o = openMedia(path, &err);
        if (!o) { std::printf("cannot open %s: %s\n", path.c_str(), err.c_str()); continue; }
        const MediaInfo mi = o->info();
        o.reset();
        const VideoProxyAssessment a = assessVideoProxy(path, mi);
        std::printf("%s: %dx%d %s %.2f fps %.0f s; assessment: %s (%s)\n", pathFileName(path).c_str(), mi.width, mi.height, mi.videoCodec.c_str(), mi.fps, mi.duration,
                    a.worth ? "worth a proxy" : "no proxy", a.reason.c_str());
        VideoProxyOptions opt;
        opt.encoderThreads = 1;
        profile::reset();
        profile::setEnabled(true);
        const double t0 = nowMs();
        const bool madeOk = createVideoProxy(path, opt, nullptr, &err);
        const double took = (nowMs() - t0) / 1000;
        profile::setEnabled(false);
        if (!madeOk) { std::printf("  failed: %s\n", err.c_str()); continue; }
        for (const auto& row : profile::snapshot().media)
            if (row.path == path) std::printf("  reading the original: %llu seeks, %llu frames decoded (%llu of them again) for %.0f frames\n", (unsigned long long)row.seeks, (unsigned long long)row.decodes, (unsigned long long)row.again, mi.duration * mi.fps);
        std::printf("  proxy made in %.1f s (%.0f frames/s), %.1f MB (original %.1f MB)\n", took, mi.duration * mi.fps / took,
                    (double)std::filesystem::file_size(videoProxyPath(path)) / 1e6, (double)std::filesystem::file_size(path) / 1e6);
    }
    return 0;
}

// proxy_bench --plan <project.veg> [--make]: which files the automatic proxies would make for the project, in order (and, with
// --make, makes them as they would, saying how long each takes).
static int planProject(int argc, char** argv)
{
    if (std::getenv("RENO_PROXY_FOLDER")) setVideoProxyFolder(std::getenv("RENO_PROXY_FOLDER"));
    Project p;
    std::string err;
    if (!loadProjectFileWithNotes(argv[2], p, &err, nullptr)) { std::printf("cannot open %s: %s\n", argv[2], err.c_str()); return 1; }
    const double t0 = nowMs();
    app::autoproxy::PlanOptions opt;
    const auto found = app::autoproxy::plan(p, [](const std::string& path, Project& out) { std::string e; return loadProjectFileWithNotes(path, out, &e, nullptr); }, opt);
    std::printf("plan for %s (%.0f ms): %d file(s)\n", pathFileName(argv[2]).c_str(), nowMs() - t0, (int)found.size());
    for (const auto& c : found)
        std::printf("  %-60.60s score %8.1f events %4d shortest %.2f s: %s\n", pathFileName(c.path).c_str(), c.score, c.events, c.shortest, c.why.reason.c_str());
    if (argc > 3 && std::string(argv[3]) == "--make") {
        std::vector<char*> args = {argv[0], const_cast<char*>("--make")};
        for (const auto& c : found) args.push_back(strdup(c.path.c_str()));
        makeAll((int)args.size(), args.data());
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 2 && std::string(argv[1]) == "--plan") return planProject(argc, argv);
    if (argc > 2 && std::string(argv[1]) == "--make") return makeAll(argc, argv);
    if (argc < 3) { std::printf("usage: proxy_bench <video> <scratch folder> [from] [to] [maxW] [maxH] [settings...]\n"); return 2; }
    const std::string video = argv[1], dir = argv[2];
    const double from = argc > 3 ? std::atof(argv[3]) : 60, to = argc > 4 ? std::atof(argv[4]) : 90;
    const int mw = argc > 5 ? std::atoi(argv[5]) : 960, mh = argc > 6 ? std::atoi(argv[6]) : 540;
    std::vector<std::string> settings;
    for (int k = 7; k < argc; ++k) settings.push_back(argv[k]);
    if (settings.empty())
        settings = {"orig", "0.5:26:preset=veryfast", "0.5:28:preset=ultrafast/tune=fastdecode", "0.25:28:preset=ultrafast/tune=fastdecode", "0.1:28:preset=ultrafast/tune=fastdecode",
                    "0.001:28:preset=ultrafast/tune=fastdecode"};
    std::filesystem::create_directories(dir);
    std::string err;
    auto o = openMedia(video, &err);
    if (!o) { std::printf("cannot open: %s\n", err.c_str()); return 1; }
    std::printf("%s: %dx%d %s %.2f fps, %.0f s; stretch %.0f-%.0f s, box %dx%d\n", pathFileName(video).c_str(), o->info().width, o->info().height, o->info().videoCodec.c_str(), o->info().fps,
                o->info().duration, from, to, mw, mh);
    o.reset();
    for (const std::string& s : settings) {
        if (s == "orig") {
            std::printf("original\n");
            readCosts(video, from, to, mw, mh, "orig");
            continue;
        }
        VideoProxyOptions opt;
        opt.maxWidth = mw;
        opt.maxHeight = mh;
        opt.announce = false;
        opt.fromSeconds = from;
        opt.toSeconds = to;
        const size_t c1 = s.find(':'), c2 = s.find(':', c1 + 1);
        opt.keyframeSeconds = std::atof(s.substr(0, c1).c_str());
        opt.quality = std::atoi(s.substr(c1 + 1, c2 - c1 - 1).c_str());
        opt.encoderOptions = s.substr(c2 + 1);
        for (char& c : opt.encoderOptions) if (c == '/') c = ' ';
        opt.outputPath = dir + "/bench.mp4";
        const int th = std::getenv("ENCTHREADS") ? std::atoi(std::getenv("ENCTHREADS")) : 0;
        opt.encoderThreads = th;
        const double a = nowMs();
        const bool ok = createVideoProxy(video, opt, nullptr, &err);
        const double took = nowMs() - a;
        if (!ok) { std::printf("%s: failed: %s\n", s.c_str(), err.c_str()); continue; }
        auto p = openMedia(opt.outputPath, &err);
        const double frames = (to - from) * 25;
        std::printf("%s\n  made in %.1f s (%.0f frames/s), %.2f MB (%.0f kB/s of video), %dx%d\n", s.c_str(), took / 1000, frames / (took / 1000),
                    (double)std::filesystem::file_size(opt.outputPath) / 1e6, (double)std::filesystem::file_size(opt.outputPath) / 1e3 / (to - from), p ? p->info().width : 0, p ? p->info().height : 0);
        p.reset();
        // (read at the same times: the proxy covers from..to, so its own times start at 0)
        readCosts(opt.outputPath, 0, to - from, mw, mh, "proxy");
        std::filesystem::remove(opt.outputPath);
    }
    return 0;
}
