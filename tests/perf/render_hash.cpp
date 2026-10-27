// render_hash: renders a project's sound and pictures the way playback asks for them and prints a hash of every piece,
// so two builds can be compared byte for byte.
//   render_hash [--audio-seconds S] [--frames N] [--seed N] project.veg...
// For each project: the sound of the whole project (up to S seconds, default 12) in 1024-sample blocks in order, then
// 40 blocks at random places (seeks, in a fixed order), and the same blocks again in another order from a pool that has
// seen them (the sound must be the same cached or not); N pictures (default 12) in order from the busiest stretch at
// the preview's half size and at full size, and at random places. Lines: "HASH <project> <what> <hex>".
#include "../../src/engine/engine.h"
#include "../../src/vegas/vegas.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>

using namespace reno;

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static uint64_t hashImage(uint64_t h, const ImagePtr& img)
{
    if (!img) return fnv(h, "null", 4);
    h = fnv(h, &img->w, sizeof img->w);
    h = fnv(h, &img->h, sizeof img->h);
    for (int y = 0; y < img->h; ++y) h = fnv(h, img->row(y), (size_t)img->w * img->pixelBytes());
    return h;
}

static double busiest(const Project& p, double span)
{
    const double fps = p.settings.fps();
    const int64_t n = (int64_t)std::ceil(p.length() * fps) + 1;
    std::vector<int> active((size_t)std::max<int64_t>(1, n), 0);
    for (const auto& t : p.tracks)
        for (const auto& e : t.events) {
            const int64_t a = std::max<int64_t>(0, (int64_t)std::floor(e.start * fps)), b = std::min<int64_t>(n - 1, (int64_t)std::ceil(e.end() * fps));
            for (int64_t f = a; f <= b; ++f) ++active[(size_t)f];
        }
    const int64_t w = std::max<int64_t>(1, (int64_t)(span * fps));
    int64_t best = 0, sum = 0, bestSum = -1;
    for (int64_t f = 0; f < n; ++f) {
        sum += active[(size_t)f];
        if (f >= w) sum -= active[(size_t)(f - w)];
        if (sum > bestSum) { bestSum = sum; best = std::max<int64_t>(0, f - w + 1); }
    }
    return best / fps;
}

static const char* gWavFile = nullptr;   // --wav file: the whole project's sound (sequential, from the start) as a 32-bit float wav, for comparing with VEGAS's renders
static bool gNoFx = false;               // --no-fx: the top-level project's audio effects bypassed (the nested ones keep theirs), as reno_cli --no-fx
static void writeWav(const char* path, int rate, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)l.size(), bytes = n * 8;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(3); u16(2); u32((uint32_t)rate); u32((uint32_t)rate * 8); u16(8); u16(32);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i) { std::fwrite(&l[i], 4, 1, f); std::fwrite(&r[i], 4, 1, f); }
    std::fclose(f);
}
static const char* gRawFile = nullptr;   // --raw file: the sequential audio as 32-bit floats (L block, R block, ...), to compare two runs by number

int main(int argc, char** argv)
{
    double seconds = 12;
    int frames = 12;
    unsigned seed = 5;
    bool noVideo = false, noAudio = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--audio-seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-video")) noVideo = true;
        else if (!std::strcmp(argv[i], "--raw") && i + 1 < argc) gRawFile = argv[++i];
        else if (!std::strcmp(argv[i], "--no-audio")) noAudio = true;
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) gWavFile = argv[++i];
        else if (!std::strcmp(argv[i], "--no-fx")) gNoFx = true;
        else if (!std::strcmp(argv[i], "--no-proxy")) setAudioProxyOptions(false, false);      // nested projects' sound mixed live (a proxy being built in the background makes runs differ)
        else if (!std::strcmp(argv[i], "--no-cache")) {
#ifndef RENO_NO_PREFETCH
            AudioEngine::setEventCacheLimit(0);      // the mixer makes every event as it plays, block by block (no event cache)
#endif
        }
        else files.push_back(argv[i]);
    }
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    for (const std::string& path : files) {
        Project p;
        std::string err;
        if (!loadProjectFile(path, p, &err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); continue; }
        std::string name = pathFileName(path);
        FxRuntime rt;
        rt.prepare(p);
        const int rate = p.settings.sampleRate;
        const int block = 1024;
        if (gWavFile) {
            // the whole project, once, from its start
            MediaPool pool;
            AudioEngine ae(pool, rt);
            ae.bypassFx = gNoFx;
            FinalRenderScope final;
            const int64_t total = (int64_t)std::floor(p.length() * rate + 1e-6);
            std::vector<float> wl, wr, l(block), r(block);
            for (int64_t at = 0; at < total; at += block) {
                const int n = (int)std::min<int64_t>(block, total - at);
                ae.render(p, at, n, l.data(), r.data(), true);
                wl.insert(wl.end(), l.begin(), l.begin() + n);
                wr.insert(wr.end(), r.begin(), r.begin() + n);
            }
            writeWav(gWavFile, rate, wl, wr);
            std::printf("WAV %s %lld samples\n", name.c_str(), (long long)total);
            std::fflush(stdout);
            rt.clear();
            continue;
        }
        if (!noAudio) {
            const int64_t total = std::min<int64_t>((int64_t)std::ceil(p.length() * rate), (int64_t)(seconds * rate));
            const int blocks = (int)((total + block - 1) / block);
            std::vector<float> l(block), r(block);
            MediaPool pool;
            AudioEngine ae(pool, rt);
            uint64_t h = 1469598103934665603ull;
            FILE* raw = gRawFile ? std::fopen(gRawFile, "wb") : nullptr;
            for (int k = 0; k < blocks; ++k) {
                ae.render(p, (int64_t)k * block, block, l.data(), r.data(), true);
                if (raw) { std::fwrite(l.data(), sizeof(float), block, raw); std::fwrite(r.data(), sizeof(float), block, raw); }
                h = fnv(h, l.data(), block * sizeof(float));
                h = fnv(h, r.data(), block * sizeof(float));
            }
            if (raw) std::fclose(raw);
            std::printf("HASH %s audio-sequential %016llx (%d blocks)\n", name.c_str(), (unsigned long long)h, blocks);
            // random seeks, each from a fresh engine and pool (as after a stop) ...
            std::mt19937 rng(seed);
            std::vector<int64_t> at;
            for (int k = 0; k < 40; ++k) at.push_back((int64_t)(std::uniform_real_distribution<double>(0, std::max(0.1, p.length() - 0.1))(rng) * rate));
            {
                uint64_t hs = 1469598103934665603ull;
                for (int64_t s : at) {
                    MediaPool pool2;
                    AudioEngine ae2(pool2, rt);
                    ae2.render(p, s, block, l.data(), r.data(), true);
                    hs = fnv(hs, l.data(), block * sizeof(float));
                    hs = fnv(hs, r.data(), block * sizeof(float));
                }
                std::printf("HASH %s audio-seek-fresh %016llx\n", name.c_str(), (unsigned long long)hs);
            }
            // ... and from one engine that has been playing and seeking about (readers and caches warm)
            {
                MediaPool pool3;
                AudioEngine ae3(pool3, rt);
                uint64_t hs = 1469598103934665603ull;
                for (int pass = 0; pass < 2; ++pass)
                    for (int64_t s : at) {
                        // a few blocks in a row from each place
                        for (int k = 0; k < 3; ++k) {
                            ae3.render(p, s + (int64_t)k * block, block, l.data(), r.data(), true);
                            if (pass == 1 || k == 0) {
                                hs = fnv(hs, l.data(), block * sizeof(float));
                                hs = fnv(hs, r.data(), block * sizeof(float));
                            }
                        }
                    }
                std::printf("HASH %s audio-seek-warm %016llx\n", name.c_str(), (unsigned long long)hs);
            }
        }
        if (!noVideo) {
            const double from = busiest(p, 1.0);
            const double fps = p.settings.fps();
            for (double scale : {0.5, 1.0}) {
                MediaPool pool;
                VideoEngine ve(pool, rt);
                FrameOptions fo;
                fo.scale = scale;
                uint64_t h = 1469598103934665603ull;
                for (int k = 0; k < frames; ++k) h = hashImage(h, ve.render(p, from + k / fps, fo));
                std::printf("HASH %s video-seq-scale%.1f %016llx\n", name.c_str(), scale, (unsigned long long)h);
            }
            {
                std::mt19937 rng(seed);
                std::vector<double> at;
                for (int k = 0; k < frames; ++k) at.push_back(std::uniform_real_distribution<double>(0, std::max(0.1, p.length() - 0.05))(rng));
                MediaPool pool;
                VideoEngine ve(pool, rt);
                FrameOptions fo;
                uint64_t h = 1469598103934665603ull;
                for (double t : at) h = hashImage(h, ve.render(p, t, fo));
                std::printf("HASH %s video-seek-warm %016llx\n", name.c_str(), (unsigned long long)h);
                uint64_t hc = 1469598103934665603ull;
                for (double t : at) {
                    MediaPool pool2;
                    VideoEngine ve2(pool2, rt);
                    hc = hashImage(hc, ve2.render(p, t, fo));
                }
                std::printf("HASH %s video-seek-cold %016llx\n", name.c_str(), (unsigned long long)hc);
            }
        }
        std::fflush(stdout);
        rt.clear();
    }
    return 0;
}
