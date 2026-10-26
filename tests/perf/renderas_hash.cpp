// renderas_hash: runs File > Render As (renderProjectAs) over the start of each project in several ways and prints a
// hash of every file made, so two builds can be compared byte for byte.
//   renderas_hash [--seconds S] [--out DIR] project.veg...
// Lines: "HASH <project> <variant> <hex>". Variants: video+sound on one worker, on three workers (RENO_RENDER_WORKERS),
// sound only, sound only with the loudness log, multiple mono files, channel mapping, two-pass (x264 if present).
#include "../../src/engine/engine.h"
#include "../../src/engine/renderas.h"
#include "../../src/vegas/vegas.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace reno;

static uint64_t hashFile(const std::string& path)
{
    uint64_t h = 1469598103934665603ull;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        unsigned char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ull; }
        std::fclose(f);
    } else {
        return 0;
    }
    return h;
}

int main(int argc, char** argv)
{
    double seconds = 2.0;
    std::string outDir = "/tmp";
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
        else files.push_back(argv[i]);
    }
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    for (const std::string& path : files) {
        Project p;
        std::string err;
        if (!loadProjectFile(path, p, &err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); continue; }
        const std::string name = pathFileName(path);
        struct Variant { const char* name; const char* workers; int kind; };
        const Variant variants[] = {{"av-1worker", "1", 0}, {"av-3workers", "3", 0}, {"audio", "1", 1}, {"audio-loudness", "1", 2},
                                    {"mono-files", "1", 3}, {"mapped", "1", 4}, {"twopass", "1", 5}};
        for (const Variant& v : variants) {
            setenv("RENO_RENDER_WORKERS", v.workers, 1);
            RenderSpec spec;
            spec.enc.audioFamily = "pcm";
            spec.width = 480; spec.height = 270;
            const bool soundOnly = v.kind >= 1 && v.kind <= 4;
            spec.enc.path = outDir + "/renderas_hash_" + v.name + (soundOnly ? ".wav" : ".avi");
            spec.enc.videoFamily = "rawvideo";
            if (soundOnly) spec.enc.video = false;
            if (v.kind == 2) spec.loudnessLog = outDir + "/renderas_hash_loudness.txt";
            if (v.kind == 3) spec.multipleMono = true;
            if (v.kind == 4) { RenderSpec::ChannelSource a, b; b.mono = true; spec.channelMap = {a, b}; }
            if (v.kind == 5) { spec.twoPass = true; spec.enc.videoFamily = "h264"; spec.enc.videoCodec = "libx264"; spec.enc.audioFamily = "aac"; }
            FxRuntime fx;
            fx.prepare(p);
            std::string e;
            std::string description;
            const bool ok = renderProjectAs(p, fx, spec, 0.0, seconds, [](const ExportProgress&) { return true; }, &e, MediaPool::Opener(), &description);
            if (!ok) { std::printf("HASH %s %s FAILED (%s)\n", name.c_str(), v.name, e.c_str()); continue; }
            if (v.kind == 3) {
                for (const std::string& f : multipleMonoPaths(spec.enc.path, AudioEngine::projectChannels(p)))
                    std::printf("HASH %s %s %s %016llx\n", name.c_str(), v.name, pathFileName(f).c_str(), (unsigned long long)hashFile(f));
            } else if (v.kind == 5) {
                // an encoder's output is not byte-stable across runs of the same build: just say it worked
                std::printf("HASH %s %s ok %s\n", name.c_str(), v.name, description.c_str());
            } else {
                std::printf("HASH %s %s %016llx\n", name.c_str(), v.name, (unsigned long long)hashFile(spec.enc.path));
            }
            if (v.kind == 2) std::printf("HASH %s %s loudnesslog %016llx\n", name.c_str(), v.name, (unsigned long long)hashFile(spec.loudnessLog));
            std::fflush(stdout);
        }
    }
    return 0;
}
