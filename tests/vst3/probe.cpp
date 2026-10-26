// Tries Reno's audio plug-in host on a plug-in file from the command line: says what is in the file,
// makes every effect in it, lists its parameters, runs audio through it and saves and loads its state.
// For looking at how the host gets on with a plug-in somebody else made (it opens no window).
//
//     probe <plug-in file> [--set <parameter index>=<value 0..1>]... [--rate <sample rate>] [--block <frames>]
//           [--seconds <how long to run>] [--only <text the effect's name starts with>]
//
// The audio sent in is a single click (the first sample is 1, left and right) followed by silence, so
// that what comes out shows what the effect does: for each channel the peak, where the first sound
// is and where the loudest sample is.
#include "../../src/audiofx/audioplugin.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace reno;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: probe <plug-in file> [--set index=value]... [--rate hz] [--block frames] [--seconds s] [--only name]\n");
        return 2;
    }
    const std::string path = argv[1];
    std::vector<std::pair<int, float>> sets;
    double rate = 48000.0, seconds = 2.0;
    int block = 512;
    std::string only;
    for (int k = 2; k + 1 < argc; k += 2) {
        const std::string opt = argv[k], val = argv[k + 1];
        if (opt == "--set") {
            const size_t eq = val.find('=');
            if (eq != std::string::npos) sets.push_back({std::atoi(val.substr(0, eq).c_str()), (float)std::atof(val.substr(eq + 1).c_str())});
        } else if (opt == "--rate") rate = std::atof(val.c_str());
        else if (opt == "--block") block = std::atoi(val.c_str());
        else if (opt == "--seconds") seconds = std::atof(val.c_str());
        else if (opt == "--only") only = val;
    }

    std::string error;
    const std::vector<AudioPluginDesc> descs = describeAudioPluginFile(path, &error);
    if (descs.empty()) {
        std::printf("%s: %s\n", path.c_str(), error.empty() ? "nothing found in it" : error.c_str());
        return 1;
    }
    int problems = 0;
    for (const AudioPluginDesc& d : descs) {
        if (!only.empty() && d.name.compare(0, only.size(), only) != 0) continue;
        std::printf("== %s \"%s\" by \"%s\"  id %s  %s\n", d.format.c_str(), d.name.c_str(), d.vendor.c_str(), d.uid.c_str(), d.isInstrument ? "instrument" : "effect");
        error.clear();
        std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, &error);
        if (!p) {
            std::printf("   could not be made: %s\n", error.c_str());
            ++problems;
            continue;
        }
        std::printf("   channels in %d, out %d; latency %d samples; own window: %s\n", p->desc.numInputs, p->desc.numOutputs, p->latencySamples(),
                    p->hasEditor() ? "yes" : "no");
        const int count = p->paramCount();
        std::printf("   %d parameter(s)\n", count);
        for (int k = 0; k < count && k < 40; ++k)
            std::printf("     %2d  %-28s %.4f  \"%s\"\n", k, p->paramName(k).c_str(), p->paramGet(k), p->paramText(k).c_str());
        if (count > 40) std::printf("     ...\n");

        if (!p->activate(rate, block)) {
            std::printf("   activate failed\n");
            ++problems;
            continue;
        }
        for (const auto& s : sets) {
            p->paramSet(s.first, s.second);
            std::printf("   set parameter %d (%s) to %.4f: now %.4f \"%s\"\n", s.first, p->paramName(s.first).c_str(), s.second, p->paramGet(s.first),
                        p->paramText(s.first).c_str());
        }

        std::vector<float> atStart((size_t)count);
        for (int k = 0; k < count; ++k) atStart[(size_t)k] = p->paramGet(k);
        const int total = (int)(rate * seconds);
        std::vector<float> l((size_t)block), r((size_t)block);
        double peak[2] = {0.0, 0.0};
        long first[2] = {-1, -1}, loudest[2] = {-1, -1};
        bool finite = true;
        for (int done = 0; done < total; done += block) {
            const int n = std::min(block, total - done);
            std::fill(l.begin(), l.end(), 0.f);
            std::fill(r.begin(), r.end(), 0.f);
            if (done == 0) l[0] = r[0] = 1.f;
            p->process(l.data(), r.data(), n, done, true);
            p->idle();
            for (int c = 0; c < 2; ++c) {
                const std::vector<float>& ch = c == 0 ? l : r;
                for (int k = 0; k < n; ++k) {
                    const double v = std::fabs((double)ch[(size_t)k]);
                    if (!(v == v) || v > 1e6) finite = false;
                    if (v > 1e-6 && first[c] < 0) first[c] = done + k;
                    if (v > peak[c]) { peak[c] = v; loudest[c] = done + k; }
                }
            }
        }
        for (int c = 0; c < 2; ++c)
            std::printf("   click in, %s out: peak %.6f, first sound at sample %ld, loudest at sample %ld\n", c == 0 ? "left" : "right", peak[c], first[c], loudest[c]);
        for (int k = 0; k < count; ++k)             // (values the effect reports itself, meters and the like)
            if (std::fabs(p->paramGet(k) - atStart[(size_t)k]) > 1e-6f)
                std::printf("   parameter %d (%s) moved by itself while running: %.4f -> %.4f\n", k, p->paramName(k).c_str(), atStart[(size_t)k], p->paramGet(k));
        if (!finite) {
            std::printf("   PROBLEM: what came out is not sane audio\n");
            ++problems;
        }

        // state: save, change everything that can be changed, load, compare
        const std::string state = p->saveState();
        std::vector<float> before((size_t)count);
        for (int k = 0; k < count; ++k) before[(size_t)k] = p->paramGet(k);
        for (int k = 0; k < count; ++k) p->paramSet(k, before[(size_t)k] > 0.5f ? 0.f : 1.f);
        const bool loaded = p->loadState(state);
        int different = 0;
        for (int k = 0; k < count; ++k) {
            if (std::fabs(p->paramGet(k) - before[(size_t)k]) <= 1e-4f) continue;
            ++different;
            std::printf("   after loading the state parameter %d (%s) is %.4f, was %.4f\n", k, p->paramName(k).c_str(), p->paramGet(k), before[(size_t)k]);
        }
        std::printf("   state: %d characters; load %s; %d of %d parameter(s) not back where they were\n", (int)state.size(), loaded ? "ok" : "FAILED", different, count);
        if (!loaded) ++problems;
        p->deactivate();
        p.reset();
        std::printf("   closed\n");
    }
    std::printf("%d problem(s)\n", problems);
    return problems ? 1 : 0;
}
