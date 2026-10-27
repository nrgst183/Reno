// The sound of a project must be the same every time it is mixed: tracks mixed on several threads, runs after runs,
// with another block size. Several audio tracks play the same nested project, each with its own stretch / pitch
// shift (as in a stutter project): the readers of one nested project must not share mixers and their stretcher state.
//
//   nested_audio_det_test <scratch folder>
#include "../../src/engine/engine.h"
#include "../../src/audiofx/audioplugin.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A sweep with a little noise (written as AAC at 44.1 kHz), so that a stretcher started in the wrong place gives other numbers.
static bool writeSweep(const std::string& path, double seconds)
{
    EncodeSettings es;
    es.path = path;
    es.video = false;
    es.audioFamily = "aac";             // a compressed sound at another rate, as the real clips: reading from a seek is not the same as reading on
    es.outSampleRate = 44100;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    const int n = (int)(seconds * 48000);
    std::vector<float> l((size_t)n), r((size_t)n);
    uint32_t seed = 12345;
    double ph = 0;
    for (int k = 0; k < n; ++k) {
        seed = seed * 1664525u + 1013904223u;
        const double noise = ((seed >> 8) / 16777216.0 - 0.5) * 0.1;
        ph += 2 * 3.14159265358979 * (200.0 + 600.0 * k / n) / 48000.0;
        l[(size_t)k] = (float)(0.4 * std::sin(ph) + noise);
        r[(size_t)k] = (float)(0.4 * std::sin(ph * 1.5) - noise);
    }
    return w->writeAudio(l.data(), r.data(), n) && w->finish(nullptr);
}

static FxSlot slotOf(Project& p, const AudioPluginDesc& d)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.audio = d;
    s.name = d.name;
    return s;
}

static Project baseProject()
{
    Project p;
    p.settings.width = 320;
    p.settings.height = 180;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    p.settings.sampleRate = 48000;
    return p;
}

static Track& addTrack(Project& p, TrackType type)
{
    Track t;
    t.id = p.newId();
    t.type = type;
    p.tracks.push_back(t);
    return p.tracks.back();
}

static Event& addEvent(Project& p, Track& t, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    t.events.push_back(e);
    return t.events.back();
}

static MediaItem mediaOf(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    std::unique_ptr<MediaReader> r = openMediaOrProject(path, nullptr);
    if (r) m.info = r->info();
    else m.offline = true;
    return m;
}

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// The hash of the first 'seconds' of the project's sound, mixed in blocks of 'block' samples by a new engine.
static uint64_t mixHash(const Project& p, double seconds, int block, bool parallel, bool finalRender, bool* silent)
{
    FxRuntime fx;
    fx.prepare(p);
    MediaPool pool;
    AudioEngine ae(pool, fx);
    ae.parallelMix = parallel;
    std::unique_ptr<FinalRenderScope> scope;
    if (finalRender) scope.reset(new FinalRenderScope());
    const int64_t total = (int64_t)(seconds * p.settings.sampleRate);
    std::vector<float> l((size_t)block), r((size_t)block);
    uint64_t h = 1469598103934665603ull;
    bool any = false;
    for (int64_t at = 0; at < total; at += block) {
        const int n = (int)std::min<int64_t>(block, total - at);
        ae.render(p, at, n, l.data(), r.data(), true);
        for (int k = 0; k < n; ++k) any = any || l[(size_t)k] != 0.f;
        h = fnv(h, l.data(), (size_t)n * sizeof(float));
        h = fnv(h, r.data(), (size_t)n * sizeof(float));
    }
    if (silent) *silent = !any;
    return h;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: nested_audio_det_test <scratch folder>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string sweep = pathJoin(dir, "det_sweep.m4a"), inner = pathJoin(dir, "det_inner.reno");
    std::string err;
    const std::vector<AudioPluginDesc> builtin = builtinAudioPlugins();
    check(!builtin.empty(), "built-in effects exist");
    check(writeSweep(sweep, 3.0), "the test sound is written");
    {
        // the nested project: the sweep, sped up a little (a stretched event inside the nested project)
        Project p = baseProject();
        MediaItem m = mediaOf(p, sweep);
        p.media.push_back(m);
        const double starts[] = {0.713, 0.599, 0.433};
        for (int i = 0; i < 3; ++i) {
            Track& t = addTrack(p, TrackType::Audio);
            Event& e = addEvent(p, t, m.id, starts[i], 1.3);
            e.offset = 0.5 + 0.1 * i;
            e.rate = 1.1837;
            if (i == 1) e.stretchMethod = "classic";
            // effects with a memory (delays, modulation): their state follows the sound
            if (!builtin.empty()) e.fx.push_back(slotOf(p, builtin[(size_t)(i + 1) % builtin.size()]));
        }
        check(p.save(inner, &err), "the nested project is saved", err);
    }
    Project outer = baseProject();
    {
        MediaItem m = mediaOf(outer, inner);
        check(!m.offline && m.info.hasAudio, "the nested project opens with sound");
        outer.media.push_back(m);
        // five audio tracks, each playing the nested project for about 0.7 s, stretched and pitched in different ways
        const double pitches[] = {6.0, -8.0, -14.0, 3.0, 0.0};
        for (int i = 0; i < 5; ++i) {
            Track& t = addTrack(outer, TrackType::Audio);
            Event& e = addEvent(outer, t, m.id, 0, 0.7);
            e.rate = 1.1284;
            e.pitch = pitches[i];
            e.keepPitch = true;
            e.stretchMethod = "elastique";
            e.stretchMode = i % 2 ? "efficient" : "pro";
        }
    }

    bool silent = false;
    const uint64_t ref = mixHash(outer, 0.8, 1024, true, true, &silent);
    check(!silent, "the mix is not silent");
    int same = 0, runs = 20;
    for (int i = 0; i < runs; ++i) same += mixHash(outer, 0.8, 1024, true, true, nullptr) == ref;
    check(same == runs, "the same sound on every run (threads)", strfmt("%d of %d runs equal", same, runs));
    same = 0;
    for (int i = 0; i < runs; ++i) same += mixHash(outer, 0.8, 1024, true, false, nullptr) == ref;
    check(same == runs, "the same sound on every run, not a final render", strfmt("%d of %d runs equal", same, runs));
    same = 0;
    for (int i = 0; i < 3; ++i) same += mixHash(outer, 0.8, 1024, false, true, nullptr) == ref;
    check(same == 3, "the same sound mixed on one thread", strfmt("%d of 3 runs equal", same));
    check(mixHash(outer, 0.8, 4096, true, true, nullptr) == mixHash(outer, 0.8, 4096, false, true, nullptr), "another block size: threads or not, the same sound");

    // Two readers of one nested project, read side by side, each hear the nested sound as a reader alone would.
    {
        std::unique_ptr<MediaReader> a = openMediaOrProject(inner, &err), b = openMediaOrProject(inner, &err), solo = openMediaOrProject(inner, &err);
        check(a && b && solo, "the nested project opens three times", err);
        if (a && b && solo) {
            const int block = 1024, blocks = 40;
            std::vector<float> al(block), ar(block), bl(block), br(block), sl(block), sr(block);
            int equal = 0;
            double peak = 0;
            for (int k = 0; k < blocks; ++k) {
                a->audio((int64_t)k * block, block, 48000, al.data(), ar.data());
                b->audio((int64_t)k * block, block, 48000, bl.data(), br.data());
                if (std::memcmp(al.data(), bl.data(), sizeof(float) * block) == 0 && std::memcmp(ar.data(), br.data(), sizeof(float) * block) == 0) ++equal;
                for (float v : al) peak = std::max(peak, (double)std::fabs(v));
            }
            check(peak > 0.01, "the nested sound is not silent");
            check(equal == blocks, "two readers of one nested project give the same sound", strfmt("%d of %d blocks equal", equal, blocks));
            // a reader alone, from the start, gives what the first of the two gave
            uint64_t ha = 1469598103934665603ull, hs = ha;
            for (int k = 0; k < blocks; ++k) {
                solo->audio((int64_t)k * block, block, 48000, sl.data(), sr.data());
                hs = fnv(hs, sl.data(), sizeof(float) * block);
            }
            a.reset();
            b.reset();
            std::unique_ptr<MediaReader> again = openMediaOrProject(inner, &err);
            for (int k = 0; k < blocks && again; ++k) {
                again->audio((int64_t)k * block, block, 48000, al.data(), ar.data());
                ha = fnv(ha, al.data(), sizeof(float) * block);
            }
            check(ha == hs, "a reader opened after others have gone gives the same sound as the first");
        }
    }

    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
