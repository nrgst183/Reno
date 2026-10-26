// Mixing on several processors: a project with many tracks, buses, envelopes, stretched / pitched / reversed events,
// built-in effects and a VST effect is mixed serially and in parallel; every sample and meter peak must match.
// Also prints a hash of the mix (build with -DREF_ENGINE for an engine without the parallelMix switch).
//
//   audio_parallel_test [a VST2 effect] [--bench]
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include "../../src/audiofx/audioplugin.h"
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const double kPi = 3.14159265358979323846;

// Made-up media "tone:<hz>": a tone with a slow wobble, 20 s, 48 kHz (or "tone44:<hz>" at 44.1 kHz so that it is resampled)
class Synth : public MediaReader {
public:
    MediaInfo mi;
    double freq = 220;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const double t = (double)(start + k) / sampleRate;
            left[k] = (float)(0.2 * std::sin(2.0 * kPi * freq * t) + 0.05 * std::sin(2.0 * kPi * 3.1 * t));
            right[k] = (float)(0.2 * std::sin(2.0 * kPi * freq * 1.01 * t));
        }
        return true;
    }
};
static std::unique_ptr<MediaReader> synthOpen(const std::string& path, std::string* error)
{
    std::unique_ptr<Synth> r(new Synth());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 20;
    r->mi.sampleRate = path.compare(0, 6, "tone44") == 0 ? 44100 : 48000;
    r->mi.channels = 2;
    const size_t c = path.find(':');
    if (c == std::string::npos) { if (error) *error = "unknown"; return nullptr; }
    r->freq = std::atof(path.c_str() + c + 1);
    return std::unique_ptr<MediaReader>(r.release());
}

static EnvPoint pt(double t, double v)
{
    EnvPoint p;
    p.time = t;
    p.value = v;
    return p;
}
static Envelope env(const std::string& kind, std::vector<EnvPoint> pts, uint64_t target = 0)
{
    Envelope e;
    e.kind = kind;
    e.target = target;
    e.points = std::move(pts);
    return e;
}

struct Rig {
    FxRuntime fx;
    MediaPool pool;
    AudioEngine audio;
    Rig() : audio(pool, fx) { pool.setOpener(synthOpen); }
};

static FxSlot slotOf(Project& p, const AudioPluginDesc& d)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.audio = d;
    s.name = d.name;
    return s;
}

// A project of 'nTracks' audio tracks and 'nBuses' buses.
static Project makeProject(int nTracks, int nBuses, bool surround, const std::vector<AudioPluginDesc>& builtin, const AudioPluginDesc* vst)
{
    Project p;
    p.nextId = 10000;
    p.settings.sampleRate = 48000;
    if (surround) p.settings.audioChannels = 6;
    uint64_t media[3];
    for (int i = 0; i < 3; ++i) {
        MediaItem m;
        m.id = p.newId();
        m.path = std::string(i == 2 ? "tone44:" : "tone:") + std::to_string(110 * (i + 1));
        m.name = m.path;
        std::string e;
        m.info = synthOpen(m.path, &e)->info();
        p.media.push_back(m);
        media[i] = m.id;
    }
    for (int b = 0; b < nBuses; ++b) {
        Bus bus;
        bus.id = p.newId();
        bus.name = "bus" + std::to_string(b);
        bus.level = 0.8 + 0.05 * b;
        bus.pan = b == 1 ? 0.3 : 0;
        // a chain: 3 -> 2 -> 1 -> master; the others go to the master or to the first
        if (b > 0 && b < 3) bus.output = p.buses[(size_t)b - 1].id;
        if (b >= 3) bus.output = p.buses[(size_t)(b % 2)].id;
        if (!builtin.empty() && b != 2) bus.fx.push_back(slotOf(p, builtin[(size_t)b % builtin.size()]));
        if (vst && b == 1) bus.fx.push_back(slotOf(p, *vst));
        if (b == 3) bus.fxPostFader = true;
        if (b == 4) bus.envelopes.push_back(env("volume", {pt(0, 1.0), pt(6, 0.4)}));
        if (surround && b == 0) bus.surround.x = -0.5;
        p.buses.push_back(bus);
    }
    for (int i = 0; i < nTracks; ++i) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        t.level = 0.5 + 0.02 * (i % 7);
        t.pan = (i % 5 - 2) * 0.2;
        if (nBuses > 0 && i % 3 != 0) t.output = p.buses[(size_t)(i % nBuses)].id;
        if (nBuses > 0 && i % 4 == 1) {
            Send s;
            s.bus = p.buses[(size_t)((i + 1) % nBuses)].id;
            s.level = 0.3;
            s.preFader = i % 8 == 1;
            t.sends.push_back(s);
        }
        if (nBuses > 1 && i % 6 == 2) t.envelopes.push_back(env("send", {pt(0, 0.2), pt(8, 0.9)}, p.buses[0].id));
        if (i % 5 == 3) t.envelopes.push_back(env("volume", {pt(0, 0.4), pt(4, 1.0), pt(9, 0.6)}));
        if (i % 7 == 4) t.envelopes.push_back(env("pan", {pt(0, -0.5), pt(7, 0.5)}));
        if (i % 11 == 5) t.mute = true;
        if (i % 13 == 6) t.panType = "constantPower";
        if (i % 9 == 0) t.invertPhase = true;
        if (!builtin.empty() && i % 2 == 0) t.fx.push_back(slotOf(p, builtin[(size_t)i % builtin.size()]));
        if (vst && i % 6 == 3) t.fx.push_back(slotOf(p, *vst));
        if (surround) t.surround.x = ((i % 7) - 3) / 4.0;
        for (int k = 0; k < 3; ++k) {
            Event e;
            e.id = p.newId();
            e.mediaId = media[(i + k) % 3];
            e.start = k * 3.0 + (i % 4) * 0.13;
            e.length = 3.4;
            e.offset = 0.5 * (i % 5);
            e.gain = 0.7 + 0.05 * (k + i % 3);
            if (k == 1) e.fadeIn = 0.4;
            if (k == 0) e.fadeOut = 0.3;
            if (i % 5 == 2 && k == 1) e.rate = 1.25;                         // stretched
            if (i % 8 == 3 && k == 2) e.pitch = 3;                           // pitched
            if (i % 9 == 4 && k == 0) e.reverse = true;
            if (i % 6 == 5 && k == 2) e.fx.push_back(slotOf(p, builtin.empty() ? AudioPluginDesc() : builtin[0]));
            if (e.fx.size() && builtin.empty()) e.fx.clear();
            t.events.push_back(e);
        }
        t.sortEvents();
        p.tracks.push_back(t);
    }
    return p;
}

struct Result {
    std::vector<float> l, r;
    std::vector<AudioEngine::StripPeak> peaks;
    double seconds = 0, mainCpu = 0, allCpu = 0;       // wall time, the calling thread's and the whole process's processor time
    int width = 0;
    uint64_t hash = 14695981039346656037ull;
};

static void hashIn(uint64_t& h, const void* d, size_t n)
{
    const unsigned char* b = (const unsigned char*)d;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
}

// Mixes 'blocks' blocks of 'frames' samples from 'from' seconds (with one jump back in the middle); mode: 0 serial (the
// switch off), n > 0 that many threads
static Result mixProject(Project p, int mode, int blocks, int frames, double from, bool channels6 = false)
{
    Rig rig;
#ifndef REF_ENGINE
    rig.audio.parallelMix = mode != 0;
    rig.audio.mixThreads = mode;
#endif
    rig.fx.prepare(p);
    Result res;
    std::vector<float> l((size_t)frames), r((size_t)frames), c[6];
    for (auto& v : c) v.resize((size_t)frames);
    float* six[6];
    for (int k = 0; k < 6; ++k) six[k] = c[k].data();
    const int64_t s0 = (int64_t)(from * 48000);
    auto cpu = [](clockid_t id) { timespec ts; clock_gettime(id, &ts); return (double)ts.tv_sec + ts.tv_nsec * 1e-9; };
    const double m0 = cpu(CLOCK_THREAD_CPUTIME_ID), a0 = cpu(CLOCK_PROCESS_CPUTIME_ID);
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
        int64_t at = s0 + (int64_t)b * frames;
        if (b >= blocks / 2) at = s0 + (int64_t)(b - blocks / 2) * frames + 2 * 48000;      // a jump
        if (channels6) {
            rig.audio.renderChannels(p, at, frames, six, true);
            for (int k = 0; k < 6; ++k) {
                res.l.insert(res.l.end(), c[k].begin(), c[k].end());
                hashIn(res.hash, c[k].data(), (size_t)frames * 4);
            }
        } else {
            rig.audio.render(p, at, frames, l.data(), r.data(), true);
            res.l.insert(res.l.end(), l.begin(), l.end());
            res.r.insert(res.r.end(), r.begin(), r.end());
            hashIn(res.hash, l.data(), (size_t)frames * 4);
            hashIn(res.hash, r.data(), (size_t)frames * 4);
        }
        std::vector<AudioEngine::StripPeak> pk;
        rig.audio.stripPeaks(pk);
        for (const auto& x : pk) {
            hashIn(res.hash, &x.id, 8);
            hashIn(res.hash, &x.left, 4);
            hashIn(res.hash, &x.right, 4);
        }
        if (b == blocks - 1) res.peaks = pk;
#ifndef REF_ENGINE
        res.width = std::max(res.width, rig.audio.lastMixWidth);
#endif
    }
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    res.mainCpu = cpu(CLOCK_THREAD_CPUTIME_ID) - m0;
    res.allCpu = cpu(CLOCK_PROCESS_CPUTIME_ID) - a0;
    return res;
}

static bool same(const Result& a, const Result& b)
{
    if (a.l.size() != b.l.size() || a.r.size() != b.r.size() || a.peaks.size() != b.peaks.size()) return false;
    if (std::memcmp(a.l.data(), b.l.data(), a.l.size() * 4) != 0) return false;
    if (!a.r.empty() && std::memcmp(a.r.data(), b.r.data(), a.r.size() * 4) != 0) return false;
    for (size_t i = 0; i < a.peaks.size(); ++i)
        if (a.peaks[i].id != b.peaks[i].id || std::memcmp(&a.peaks[i].left, &b.peaks[i].left, 8) != 0) return false;
    return a.hash == b.hash;
}

int main(int argc, char** argv)
{
    std::string vstFile;
    bool bench = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--bench") bench = true;
        else vstFile = argv[i];
    }
    const std::vector<AudioPluginDesc> builtin = builtinAudioPlugins();
    AudioPluginDesc vst;
    bool haveVst = false;
    if (!vstFile.empty()) {
        std::string err;
        std::vector<AudioPluginDesc> d = describeAudioPluginFile(vstFile, &err);
        if (d.empty()) check(false, "the VST effect is recognised", err);
        else { vst = d[0]; haveVst = true; }
    }
    check(!builtin.empty(), "built-in effects exist", std::to_string(builtin.size()));
    const AudioPluginDesc* vp = haveVst ? &vst : nullptr;

    // ---- the same mix, one thread or several (stereo)
    {
        const Project p = makeProject(24, 5, false, builtin, vp);
        const Result a = mixProject(p, 0, 40, 1024, 0.5);
        const Result a2 = mixProject(p, 0, 40, 1024, 0.5);
        check(same(a, a2), "stereo 24 tracks, 5 buses: the mix is repeatable (serial twice)");
        double peak = 0;
        for (float v : a.l) peak = std::max(peak, (double)std::fabs(v));
        check(peak > 0.05 && peak < 50, "and it is not silent", std::to_string(peak));
        std::printf("HASH stereo %016llx\n", (unsigned long long)a.hash);
#ifndef REF_ENGINE
        for (int n : {1, 2, 3, 8}) {
            const Result b = mixProject(p, n, 40, 1024, 0.5);
            check(same(a, b), "stereo 24 tracks, 5 buses: " + std::to_string(n) + " threads give the very same samples and peaks",
                  "width " + std::to_string(b.width));
        }
        const Result c = mixProject(p, 0, 7, 4096, 0.2);
        const Result d = mixProject(p, 3, 7, 4096, 0.2);
        check(same(c, d), "stereo: bigger blocks too");
        const Result e = mixProject(p, 0, 50, 64, 0.2);
        const Result f = mixProject(p, 3, 50, 64, 0.2);
        check(same(e, f), "stereo: tiny blocks too");
        // two tracks naming the same plug-in slot would share one instance: such a project is mixed on one thread
        {
            Project q = makeProject(24, 0, false, builtin, vp);
            int from = -1, to = -1;
            for (size_t i = 0; i < q.tracks.size(); ++i)
                if (q.tracks[i].type == TrackType::Audio && !q.tracks[i].fx.empty()) { if (from < 0) from = (int)i; else if (to < 0) to = (int)i; }
            if (from >= 0 && to >= 0) {
                q.tracks[(size_t)to].fx.push_back(q.tracks[(size_t)from].fx[0]);
                const Result g = mixProject(q, 0, 20, 1024, 0.5);
                const Result h = mixProject(q, 4, 20, 1024, 0.5);
                check(same(g, h) && h.width == 1, "a plug-in slot shared by two tracks: mixed on one thread", "width " + std::to_string(h.width));
            }
        }
#endif
    }
    // ---- 5.1
    {
        const Project p = makeProject(20, 4, true, builtin, vp);
        const Result a = mixProject(p, 0, 30, 1024, 0.5, true);
        std::printf("HASH surround %016llx\n", (unsigned long long)a.hash);
#ifndef REF_ENGINE
        const Result b = mixProject(p, 4, 30, 1024, 0.5, true);
        check(same(a, b), "5.1 project, 20 tracks, 4 buses: the same six channels with several threads", "width " + std::to_string(b.width));
        double peak = 0;
        for (float v : a.l) peak = std::max(peak, (double)std::fabs(v));
        check(peak > 0.02, "and it is not silent", std::to_string(peak));
#endif
    }
    // ---- solo, a muted bus
    {
        Project p = makeProject(16, 4, false, builtin, vp);
        p.tracks[3].solo = true;
        p.buses[2].mute = true;
        const Result a = mixProject(p, 0, 20, 1024, 0.5);
        std::printf("HASH solo %016llx\n", (unsigned long long)a.hash);
#ifndef REF_ENGINE
        const Result b = mixProject(p, 4, 20, 1024, 0.5);
        check(same(a, b), "solo track and a muted bus: the same");
        p.buses[1].solo = true;
        const Result c = mixProject(p, 0, 20, 1024, 0.5);
        const Result d = mixProject(p, 4, 20, 1024, 0.5);
        check(same(c, d), "solo bus: the same");
#endif
    }
    // ---- no buses, one track, none
    {
        const Project p = makeProject(12, 0, false, builtin, vp);
        const Result a = mixProject(p, 0, 20, 1024, 0.5);
        std::printf("HASH nobus %016llx\n", (unsigned long long)a.hash);
#ifndef REF_ENGINE
        check(same(a, mixProject(p, 4, 20, 1024, 0.5)), "no buses: the same");
        const Project one = makeProject(1, 1, false, builtin, vp);
        check(same(mixProject(one, 0, 10, 1024, 0.5), mixProject(one, 4, 10, 1024, 0.5)), "one track: the same");
        const Project none = makeProject(0, 2, false, builtin, vp);
        check(same(mixProject(none, 0, 5, 1024, 0.5), mixProject(none, 4, 5, 1024, 0.5)), "no tracks: the same");
#endif
    }
    // ---- the bus tap (Render As's multichannel mapping) gets the same sound in the same order
    {
#ifndef REF_ENGINE
        const Project p = makeProject(16, 4, false, builtin, vp);
        uint64_t h[2] = {14695981039346656037ull, 14695981039346656037ull};
        for (int mode = 0; mode < 2; ++mode) {
            Rig rig;
            rig.audio.parallelMix = mode != 0;
            rig.audio.mixThreads = mode ? 4 : 0;
            uint64_t& hh = h[mode];
            rig.audio.busTap = [&](uint64_t id, const float* l, const float* r, int n) {
                hashIn(hh, &id, 8);
                hashIn(hh, l, (size_t)n * 4);
                hashIn(hh, r, (size_t)n * 4);
            };
            Project q = makeProject(24, 0, false, builtin, vp);
            rig.fx.prepare(q);
            std::vector<float> l(1024), r(1024);
            for (int b = 0; b < 20; ++b) rig.audio.render(q, 24000 + (int64_t)b * 1024, 1024, l.data(), r.data(), true);
        }
        check(h[0] == h[1], "the bus tap is called with the same sound in the same order");
#endif
    }
    // ---- timing
    if (bench) {
        for (int tracks : {16, 32}) {
            const Project p = makeProject(tracks, 5, false, builtin, vp);
            for (int mode : {1, 2, 2, 1}) {
                const Result a = mixProject(p, mode, 200, 1024, 0.5);
                std::printf("BENCH %d tracks, %d thread(s): wall %.3f s, mixing thread's cpu %.3f s, all threads' cpu %.3f s for 200 blocks of 1024 (%.2f ms/block wall, width %d)\n",
                            tracks, mode, a.seconds, a.mainCpu, a.allCpu, a.seconds / 200 * 1000, a.width);
            }
        }
    }
    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
