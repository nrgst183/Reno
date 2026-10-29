// Tests of plug-in delay compensation in the mixer (AudioBlockMixer, engine_audio.cpp): an eFX Limiter (reports a
// latency) on an event, track, bus or master against an inverted plain copy of the same sound: they must cancel.
//
//   pdc_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/efx.h"
#include "../../src/core/base.h"
#include "../../src/engine/engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}
static std::string fmt(const char* f, double a = 0, double b = 0, double c = 0)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

static const double kPi = 3.14159265358979323846;
static const int kRate = 48000;

// "synth:<n>": 10 s of a 220 Hz x n tone at half volume
class Synth : public MediaReader {
public:
    MediaInfo mi;
    double freq = 220;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        const int64_t end = (int64_t)std::llround(mi.duration * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const float v = n >= 0 && n < end ? 0.5f * (float)std::sin(2.0 * kPi * freq * (double)n / sampleRate) : 0.f;
            left[k] = v;
            right[k] = v;
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> synthOpen(const std::string& path, std::string*)
{
    std::unique_ptr<Synth> r(new Synth());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 10;
    r->mi.sampleRate = kRate;
    r->mi.channels = 2;
    r->freq = 220.0 * std::atoi(path.c_str() + 6);
    return std::unique_ptr<MediaReader>(r.release());
}

static uint32_t idOf(const char* s) { return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3]; }

// an eFX Limiter slot: output 0 dB, so that at half volume nothing is limited and it passes the sound as it is
static FxSlot limiterSlot(Project& p, const char* id = "MX2A")
{
    static std::string state;
    if (state.empty()) {
        AudioPluginDesc d;
        d.format = "VST2";
        d.uid = std::to_string(idOf(id));
        std::string err;
        std::unique_ptr<AudioPlugin> pl = createAudioPlugin(d, &err);
        if (pl) {
            pl->activate(kRate, 512);
            pl->paramSet(4, 1.f);
            state = pl->saveState();
        }
    }
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.name = "eFX Limiter";
    s.audio.format = "VST2";
    s.audio.uid = std::to_string(idOf(id));
    s.audio.name = "eFX Limiter";
    s.state = Json(state);
    return s;
}

static uint64_t addMedia(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = path;
    std::string e;
    m.info = synthOpen(path, &e)->info();
    p.media.push_back(m);
    return m.id;
}
static Track& addTrack(Project& p)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    p.tracks.push_back(t);
    return p.tracks.back();
}
static Event& addEvent(Track& t, Project& p, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    t.events.push_back(e);
    return t.events.back();
}
static Bus& addBus(Project& p)
{
    Bus b;
    b.id = p.newId();
    b.name = "Bus";
    p.buses.push_back(b);
    return p.buses.back();
}

struct Out {
    std::vector<float> l, r;
};

// the sound from sample 'from' for 'n' samples, in blocks of 'block' (a fresh engine: nothing carried over)
static Out render(const Project& p, int64_t from, int n, int block)
{
    FxRuntime fx;
    MediaPool pool;
    pool.setOpener(synthOpen);
    AudioEngine audio(pool, fx);
    fx.prepare(p);
    Out o;
    o.l.assign((size_t)n, 0.f);
    o.r.assign((size_t)n, 0.f);
    for (int at = 0; at < n; at += block) audio.render(p, from + at, std::min(block, n - at), o.l.data() + at, o.r.data() + at, true);
    return o;
}

static double peak(const Out& o, int a, int b)
{
    double m = 0;
    for (int k = a; k < b; ++k) m = std::max(m, (double)std::max(std::fabs(o.l[(size_t)k]), std::fabs(o.r[(size_t)k])));
    return m;
}
static double worstDiff(const Out& a, const Out& b, int from, int to)
{
    double m = 0;
    for (int k = from; k < to; ++k) m = std::max(m, (double)std::max(std::fabs(a.l[(size_t)k] - b.l[(size_t)k]), std::fabs(a.r[(size_t)k] - b.r[(size_t)k])));
    return m;
}

int main()
{
    setEfxPreferBuiltin(true);
    int latency = 0;
    {
        AudioPluginDesc d;
        d.format = "VST2";
        d.uid = std::to_string(idOf("MX2A"));
        std::string err;
        std::unique_ptr<AudioPlugin> pl = createAudioPlugin(d, &err);
        if (pl) pl->activate(kRate, 512);
        latency = pl ? pl->latencySamples() : 0;
        check(latency > 10, "the eFX Limiter reports a latency", fmt("%.0f samples", latency));
    }

    // the sound, once through the effect's path and once plain: first the effect's own length, to see that it is not nothing
    struct Case {
        const char* name;
        int kind;       // 0 event chain, 1 track chain, 2 bus chain, 3 two tracks into a latent bus, 4 master chain, 5 track chain + bus chain, 6 two events with different latencies
    };
    const Case cases[] = {{"on an event", 0}, {"on a track", 1}, {"on a bus", 2}, {"a bus that takes a track with effects and a plain one", 3},
                          {"on the master", 4}, {"on a track and on the bus after it", 5}, {"on one event and two of them on another", 6}};
    for (const Case& c : cases) {
        Project p;
        p.nextId = 1000;
        const uint64_t m = addMedia(p, "synth:1");
        Track& a = addTrack(p);
        Track& b = addTrack(p);
        const uint64_t aId = a.id, bId = b.id;
        (void)aId; (void)bId;
        Event& ea = addEvent(p.tracks[0], p, m, 0.5, 4.0);
        Event& eb = addEvent(p.tracks[1], p, m, 0.5, 4.0);
        (void)eb;
        p.tracks[1].invertPhase = true;
        switch (c.kind) {
        case 0: ea.fx.push_back(limiterSlot(p)); break;
        case 1: p.tracks[0].fx.push_back(limiterSlot(p)); break;
        case 2: {
            Bus& bus = addBus(p);
            bus.fx.push_back(limiterSlot(p));
            p.tracks[0].output = bus.id;
            break;
        }
        case 3: {
            Bus& bus = addBus(p);
            p.tracks[0].output = bus.id;
            p.tracks[1].output = bus.id;
            p.tracks[0].fx.push_back(limiterSlot(p));
            break;
        }
        case 4: p.audioOut.push_back(limiterSlot(p)); break;
        case 5: {
            Bus& bus = addBus(p);
            bus.fx.push_back(limiterSlot(p));
            p.tracks[0].output = bus.id;
            p.tracks[0].fx.push_back(limiterSlot(p));
            break;
        }
        case 6:
            // one limiter on the first track's event, two on the second's, and nothing on the plain one
            ea.fx.push_back(limiterSlot(p));
            p.tracks[1].invertPhase = false;
            p.tracks[1].events[0].fx.push_back(limiterSlot(p));
            p.tracks[1].events[0].fx.push_back(limiterSlot(p));
            break;
        }
        const std::string label = std::string(c.name);
        if (c.kind == 6) {
            // compared with the same project without effects
            Project q = p;
            for (auto& t : q.tracks) {
                t.fx.clear();
                for (auto& e : t.events) e.fx.clear();
            }
            const Out with = render(p, 0, 5 * kRate, 1000);
            const Out plain = render(q, 0, 5 * kRate, 1000);
            const double worst = worstDiff(with, plain, 0, 5 * kRate);
            check(worst < 1e-4, label + ": events with different latencies each sound where they should", fmt("%.2g", worst));
            const Out big = render(p, 0, 5 * kRate, 4800);
            check(worstDiff(with, big, 0, 5 * kRate) == 0.0, label + ": whatever the block size");
            continue;
        }
        const Out o = render(p, 0, 5 * kRate, 1000);
        const double pk = peak(o, 0, 5 * kRate);
        check(pk < 1e-4, label + ": the sound and its upside-down copy cancel", fmt("peak %.2g", pk));
        // alone, it is what the same project without effects gives (the limiter passes this sound as it is), and it starts and
        // stops where the event does
        {
            Project q = p, plain = p;
            q.tracks[1].mute = true;
            plain.tracks[1].mute = true;
            for (auto& t : plain.tracks) {
                t.fx.clear();
                for (auto& e : t.events) e.fx.clear();
            }
            for (auto& bs : plain.buses) bs.fx.clear();
            plain.audioOut.clear();
            const Out solo = render(q, 0, 5 * kRate, 1000);
            const Out ref = render(plain, 0, 5 * kRate, 1000);
            const double worst = worstDiff(solo, ref, 0, 5 * kRate);
            check(worst < 1e-4, label + ": alone, it is where the same sound without effects is", fmt("worst %.2g", worst));
            check(peak(solo, 0, (int)(0.5 * kRate) - 2) == 0.0 && peak(solo, (int)(4.5 * kRate) + 2, 5 * kRate) == 0.0 && peak(solo, 3 * kRate, 3 * kRate + 400) > 0.3,
                  label + ": and nothing sounds outside the event");
        }
        // block sizes
        bool same = true;
        for (int block : {1, 7, 64, 480, 4096, 20000}) {
            // (a stretch only: block size 1 for a whole second is slow enough)
            const int span = block == 1 ? 3000 : 2 * kRate;
            const int from = (int)(0.45 * kRate);
            const Out x = render(p, from, span, block);
            const Out y = render(p, from, span, 1000);
            if (worstDiff(x, y, 0, span) != 0.0) {
                same = false;
                std::printf("  block %d differs by %g\n", block, worstDiff(x, y, 0, span));
            }
        }
        check(same, label + ": the same whatever the block size (bit for bit)");
        // seeks and loops: a block rendered after a jump is what continuous playback gave there
        {
            FxRuntime fx;
            MediaPool pool;
            pool.setOpener(synthOpen);
            AudioEngine audio(pool, fx);
            fx.prepare(p);
            const int blk = 1000;
            std::vector<float> cl((size_t)5 * kRate), cr((size_t)5 * kRate);
            for (int at = 0; at < 5 * kRate; at += blk) audio.render(p, at, blk, cl.data() + at, cr.data() + at, true);
            Project q = p;
            q.tracks[1].mute = true;
            const Out ref = render(q, 0, 5 * kRate, blk);
            FxRuntime fx2;
            MediaPool pool2;
            pool2.setOpener(synthOpen);
            AudioEngine eng(pool2, fx2);
            fx2.prepare(q);
            double worst = 0;
            // jumps forward, back (a loop) and into the middle of the sound
            const int64_t jumps[] = {120000, 24000, 150000, 160000, 30000, 30000 + 1000, 0, 100500, 90000};
            std::vector<float> l(2000), r(2000);
            for (int64_t at : jumps) {
                eng.render(q, at, 2000, l.data(), r.data(), true);
                for (int k = 0; k < 2000; ++k) worst = std::max(worst, (double)std::max(std::fabs(l[(size_t)k] - ref.l[(size_t)(at + k)]), std::fabs(r[(size_t)k] - ref.r[(size_t)(at + k)])));
            }
            check(worst < 1e-5, label + ": after seeks and loops it is what playing through gives", fmt("worst %.2g", worst));
        }
    }

    // ---- no latency: nothing moves
    {
        Project p;
        p.nextId = 1000;
        const uint64_t m = addMedia(p, "synth:1");
        addTrack(p);
        addTrack(p);
        addEvent(p.tracks[0], p, m, 0.5, 4.0);
        addEvent(p.tracks[1], p, m, 1.0, 2.0);
        p.tracks[1].pan = -0.5;
        const Out o = render(p, 0, 5 * kRate, 777);
        const int at = (int)(0.5 * kRate);
        check(o.l[(size_t)at - 1] == 0.f && o.l[(size_t)at + 100] != 0.f, "without a latent plug-in the sound is where it was");
    }

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
