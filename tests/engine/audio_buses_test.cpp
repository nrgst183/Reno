// Tests of the mixer's buses, sends, solo and master, its meters, and of reversed, pitched and time-stretched
// events, with made-up media.
//
//   audio_buses_test [a VST2 effect]
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
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

// Made-up media:
//   "synth:<tag>"            10 s of a 220 Hz x tag tone at half volume
//   "tone:<hz>:<seconds>"    a tone at half volume for that long, then silence (the file is 10 s)
//   "ramp:"                  10 s where sample n is (n % 100000) / 100000: every sample says where it came from
class Synth : public MediaReader {
public:
    MediaInfo mi;
    int kind = 0;               // 0 tone, 1 ramp
    double freq = 220, until = 10;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        const int64_t end = (int64_t)std::llround(mi.duration * sampleRate);
        const int64_t stop = (int64_t)std::llround(until * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            float v = 0;
            if (n >= 0 && n < end) {
                if (kind == 1) v = (float)(n % 100000) / 100000.f;
                else if (n < stop) v = 0.5f * (float)std::sin(2.0 * kPi * freq * (double)n / sampleRate);
            }
            left[k] = v;
            right[k] = v;
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> synthOpen(const std::string& path, std::string* error)
{
    std::unique_ptr<Synth> r(new Synth());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 10;
    r->mi.sampleRate = 48000;
    r->mi.channels = 2;
    if (path.compare(0, 6, "synth:") == 0) r->freq = 220.0 * std::atoi(path.c_str() + 6);
    else if (path.compare(0, 5, "tone:") == 0) std::sscanf(path.c_str() + 5, "%lf:%lf", &r->freq, &r->until);
    else if (path == "ramp:") r->kind = 1;
    else { if (error) *error = "unknown"; return nullptr; }
    return std::unique_ptr<MediaReader>(r.release());
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

static uint64_t addEvent(Project& p, uint64_t track, uint64_t media, double start, double length, double offset = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = offset;
    Track* t = p.findTrack(track);
    t->events.push_back(e);
    t->sortEvents();
    return e.id;
}

static uint64_t addBus(Project& p, const std::string& name)
{
    Bus b;
    b.id = p.newId();
    b.name = name;
    p.buses.push_back(b);
    return b.id;
}

// The strength of one frequency in a block of sound (amplitude of the best-fitting sine).
static double tone(const std::vector<float>& s, double freq, int rate)
{
    double re = 0, im = 0;
    for (size_t k = 0; k < s.size(); ++k) {
        re += s[k] * std::cos(2.0 * kPi * freq * (double)k / rate);
        im += s[k] * std::sin(2.0 * kPi * freq * (double)k / rate);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double)s.size();
}

// The frequency of a steady tone, from the time between its first and last upward zero crossings
// (each placed between samples by straight-line interpolation).
static double frequency(const std::vector<float>& s, int rate)
{
    double first = -1, last = -1;
    int count = 0;
    for (size_t k = 1; k < s.size(); ++k)
        if (s[k - 1] < 0 && s[k] >= 0) {
            const double t = (double)(k - 1) + s[k - 1] / (s[k - 1] - s[k]);
            if (first < 0) first = t;
            last = t;
            ++count;
        }
    if (count < 2) return 0;
    return (count - 1) * (double)rate / (last - first);
}

static double rms(const std::vector<float>& s)
{
    double e = 0;
    for (float v : s) e += (double)v * v;
    return s.empty() ? 0 : std::sqrt(e / s.size());
}

static float maxStep(const std::vector<float>& s)
{
    float m = 0;
    for (size_t k = 1; k < s.size(); ++k) m = std::max(m, std::fabs(s[k] - s[k - 1]));
    return m;
}

static const AudioEngine::StripPeak* findPeak(const std::vector<AudioEngine::StripPeak>& v, uint64_t id)
{
    for (const auto& p : v) if (p.id == id) return &p;
    return nullptr;
}

// The mixer before buses (copied from the old engine_audio.cpp): projects without buses must sound
// exactly like this.
static void referenceRender(MediaPool& pool, const Project& p, int64_t start, int frames, float* left, float* right)
{
    std::memset(left, 0, (size_t)frames * sizeof(float));
    std::memset(right, 0, (size_t)frames * sizeof(float));
    const int rate = p.settings.sampleRate;
    const double t0 = (double)start / rate, t1 = (double)(start + frames) / rate;
    std::vector<float> tl_(frames), tr_(frames), el_(frames), er_(frames);
    const bool solo = p.anySolo(TrackType::Audio);
    for (const auto& tr : p.tracks) {
        if (tr.type != TrackType::Audio || tr.mute || (solo && !tr.solo)) continue;
        std::memset(tl_.data(), 0, (size_t)frames * sizeof(float));
        std::memset(tr_.data(), 0, (size_t)frames * sizeof(float));
        bool any = false;
        for (const auto& e : tr.events) {
            if (e.mute || e.end() <= t0 || e.start >= t1 || e.rate <= 0) continue;
            const MediaItem* m = p.findMedia(e.mediaId);
            if (!m || m->generated || m->offline || !m->info.hasAudio) continue;
            int a = (int)std::max<int64_t>(0, (int64_t)std::ceil(e.start * rate - 1e-9) - start);
            int b = (int)std::min<int64_t>(frames, (int64_t)std::ceil(e.end() * rate - 1e-9) - start);
            if (b <= a) continue;
            const int n = b - a;
            const int readRate = std::max(1000, (int)std::llround(rate / e.rate));
            const double mt = e.offset + ((double)(start + a) / rate - e.start) * e.rate;
            int64_t pos = (int64_t)std::llround(mt * readRate);
            const int64_t period = (e.loop && m->info.duration > 0) ? std::max<int64_t>(1, (int64_t)std::llround(m->info.duration * readRate)) : 0;
            std::memset(el_.data(), 0, (size_t)n * sizeof(float));
            std::memset(er_.data(), 0, (size_t)n * sizeof(float));
            int done = 0;
            bool got = false;
            while (done < n) {
                int chunk = n - done;
                int64_t at = pos + done;
                if (period > 0) {
                    at %= period;
                    if (at < 0) at += period;
                    chunk = (int)std::min<int64_t>(chunk, period - at);
                }
                if (pool.audio(e.id + 1000000, *m, at, chunk, readRate, el_.data() + done, er_.data() + done)) got = true;
                done += chunk;
            }
            if (!got) continue;
            any = true;
            bool overlaps = false;
            for (const auto& o : tr.events)
                if (o.id != e.id && !o.mute && o.start < e.end() && o.end() > e.start) overlaps = true;
            const bool fades = overlaps || e.fadeIn > 0 || e.fadeOut > 0;
            const float gain = (float)e.gain;
            for (int k = 0; k < n; ++k) {
                float g = gain;
                if (fades) {
                    const double t = (double)(start + a + k) / rate;
                    g *= (float)eventFade(e, t);
                    if (overlaps) g *= (float)overlapWeight(tr, e, t);
                }
                tl_[(size_t)(a + k)] += el_[(size_t)k] * g;
                tr_[(size_t)(a + k)] += er_[(size_t)k] * g;
            }
        }
        if (!any && tr.fx.empty()) continue;
        const float gl = (float)(tr.level * (tr.pan > 0 ? 1.0 - tr.pan : 1.0));
        const float gr = (float)(tr.level * (tr.pan < 0 ? 1.0 + tr.pan : 1.0));
        for (int k = 0; k < frames; ++k) {
            left[k] += tl_[(size_t)k] * gl;
            right[k] += tr_[(size_t)k] * gr;
        }
    }
    const float mv = (float)p.masterVolume;
    for (int k = 0; k < frames; ++k) {
        left[k] *= mv;
        right[k] *= mv;
    }
}

int main(int argc, char** argv)
{
    const std::string vst = argc > 1 ? argv[1] : "";
    std::string err;
    FxRuntime fx;
    MediaPool pool;
    pool.setOpener(synthOpen);
    AudioEngine audio(pool, fx);
    const int rate = 48000;
    std::vector<float> l(4800), r(4800);

    // ---------------------------------------------------------------- without buses: unchanged
    {
        Project p;
        p.nextId = 10000;
        const uint64_t m1 = addMedia(p, "synth:1"), m2 = addMedia(p, "synth:2"), m3 = addMedia(p, "synth:3");
        const uint64_t a = addTrack(p).id, b = addTrack(p).id, c = addTrack(p).id;
        addEvent(p, a, m1, 0.3, 3.0, 0.7);
        const uint64_t e2 = addEvent(p, a, m2, 2.5, 2.0);
        p.findEvent(e2)->fadeIn = 0.4;
        p.findEvent(e2)->gain = 0.8;
        const uint64_t e3 = addEvent(p, b, m3, 1.0, 4.0, 9.0);              // loops round
        p.findEvent(e3)->rate = 1.37;
        p.findEvent(e3)->fadeOut = 1.2;
        addEvent(p, c, m1, 0.0, 6.0, 2.0);
        p.findTrack(a)->pan = -0.3;
        p.findTrack(a)->level = 0.7;
        p.findTrack(b)->pan = 0.6;
        p.findTrack(b)->level = 1.3;
        p.findTrack(c)->mute = true;
        p.masterVolume = 0.9;
        // routing that names buses which do not exist goes to the master
        p.findTrack(b)->output = 999999;
        std::vector<float> L(20000), R(20000), RL(20000), RR(20000);
        double worst = 0;
        for (int64_t at : {0, 30000, 110000, 150001, 230000}) {
            audio.render(p, at, 20000, L.data(), R.data(), true);
            referenceRender(pool, p, at, 20000, RL.data(), RR.data());
            for (int k = 0; k < 20000; ++k) worst = std::max(worst, (double)std::max(std::fabs(L[k] - RL[k]), std::fabs(R[k] - RR[k])));
        }
        p.findTrack(b)->solo = true;
        audio.render(p, 110000, 20000, L.data(), R.data(), true);
        referenceRender(pool, p, 110000, 20000, RL.data(), RR.data());
        for (int k = 0; k < 20000; ++k) worst = std::max(worst, (double)std::max(std::fabs(L[k] - RL[k]), std::fabs(R[k] - RR[k])));
        check(worst == 0.0, "a project without buses sounds exactly as before (bit for bit)", strfmt("worst difference %g", worst));
    }

    // ---------------------------------------------------------------- buses and sends
    {
        Project p;
        p.nextId = 20000;
        const uint64_t m1 = addMedia(p, "synth:1"), m2 = addMedia(p, "synth:2");     // 220 Hz, 440 Hz
        const uint64_t t1 = addTrack(p).id, t2 = addTrack(p).id;
        addEvent(p, t1, m1, 0, 9);
        addEvent(p, t2, m2, 0, 9);
        const uint64_t busA = addBus(p, "Bus A"), busB = addBus(p, "Bus B");
        auto block = [&](double t) { audio.render(p, (int64_t)std::llround(t * rate), (int)l.size(), l.data(), r.data(), true); };

        p.findTrack(t1)->output = busA;
        p.findBus(busA)->level = 0.5;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.25) < 0.005 && std::fabs(tone(l, 440, rate) - 0.5) < 0.005, "a track routed to a bus goes through the bus's level",
              strfmt("220: %.3f 440: %.3f", tone(l, 220, rate), tone(l, 440, rate)));
        p.findBus(busA)->pan = 1.0;
        block(2.0);
        check(tone(l, 220, rate) < 0.001 && std::fabs(tone(r, 220, rate) - 0.25) < 0.005, "a bus pans", strfmt("%.3f / %.3f", tone(l, 220, rate), tone(r, 220, rate)));
        p.findBus(busA)->pan = 0;
        p.findBus(busA)->mute = true;
        block(2.0);
        check(tone(l, 220, rate) < 0.001 && tone(l, 440, rate) > 0.49, "a muted bus is silent and the others carry on");
        p.findBus(busA)->mute = false;

        // a chain of buses: A into B into the master
        p.findBus(busA)->output = busB;
        p.findBus(busB)->level = 0.5;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.125) < 0.005, "a bus can feed another bus", strfmt("%.3f", tone(l, 220, rate)));
        // a loop of buses: each that feeds back into itself goes to the master instead
        p.findBus(busB)->output = busA;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.25) < 0.005 && std::isfinite(l[0]), "buses that feed back into themselves go to the master", strfmt("%.3f", tone(l, 220, rate)));
        p.findBus(busA)->output = 0;
        p.findBus(busB)->output = 0;
        p.findBus(busB)->level = 1.0;
        p.findBus(busA)->level = 1.0;
        p.findTrack(t1)->output = 0;

        // sends: pre-fader ignore the track's volume and pan, post-fader follow them
        p.findTrack(t2)->mute = true;
        p.findTrack(t1)->level = 0;
        Send s;
        s.bus = busA;
        s.level = 0.5;
        s.preFader = true;
        p.findTrack(t1)->sends.push_back(s);
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.25) < 0.005, "a pre-fader send is heard with the track's fader down", strfmt("%.3f", tone(l, 220, rate)));
        p.findTrack(t1)->sends[0].preFader = false;
        block(2.0);
        check(tone(l, 220, rate) < 0.001, "a post-fader send follows the fader down");
        p.findTrack(t1)->level = 1.0;
        p.findTrack(t1)->pan = 1.0;
        block(2.0);
        check(tone(l, 220, rate) < 0.001 && std::fabs(tone(r, 220, rate) - 0.75) < 0.005, "a post-fader send follows the track's pan, and adds to its direct sound",
              strfmt("%.3f / %.3f", tone(l, 220, rate), tone(r, 220, rate)));
        p.findTrack(t1)->sends[0].preFader = true;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.25) < 0.005 && std::fabs(tone(r, 220, rate) - 0.75) < 0.005, "a pre-fader send does not follow the track's pan",
              strfmt("%.3f / %.3f", tone(l, 220, rate), tone(r, 220, rate)));
        p.findTrack(t1)->pan = 0;
        p.findTrack(t1)->mute = true;
        block(2.0);
        check(rms(l) < 1e-6, "a muted track sends nothing");
        p.findTrack(t1)->mute = false;
        p.findTrack(t2)->mute = false;
        p.findTrack(t1)->sends.clear();

        // meters: every strip, after its fader
        p.findTrack(t1)->output = busA;
        p.findTrack(t1)->level = 0.5;
        p.findBus(busA)->level = 0.5;
        block(2.0);
        std::vector<AudioEngine::StripPeak> pk;
        audio.stripPeaks(pk);
        const AudioEngine::StripPeak *pt1 = findPeak(pk, t1), *pt2 = findPeak(pk, t2), *pa = findPeak(pk, busA), *pb = findPeak(pk, busB), *pm = findPeak(pk, 0);
        check(pt1 && pt2 && pa && pb && pm, "meters for every track, bus and the master", strfmt("%d strips", (int)pk.size()));
        if (pt1 && pt2 && pa && pb && pm)
            check(std::fabs(pt1->left - 0.25f) < 0.01f && std::fabs(pt2->right - 0.5f) < 0.01f && std::fabs(pa->left - 0.125f) < 0.01f && pb->left == 0.f &&
                      std::fabs(pm->left - audio.peakLeft) < 1e-6f && pm->left > 0.5f,
                  "and they read the level after each fader",
                  strfmt("track %.3f, track %.3f, bus A %.3f, bus B %.3f, master %.3f", pt1->left, pt2->right, pa->left, pb->left, pm->left));

        // solo across tracks and buses
        p.findBus(busA)->level = 1.0;
        p.findTrack(t1)->level = 1.0;
        p.findBus(busA)->solo = true;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.5) < 0.005 && tone(l, 440, rate) < 0.001, "soloing a bus keeps the tracks feeding it and silences the rest",
              strfmt("220: %.3f 440: %.3f", tone(l, 220, rate), tone(l, 440, rate)));
        p.findBus(busA)->solo = false;
        p.findTrack(t2)->solo = true;
        block(2.0);
        check(tone(l, 220, rate) < 0.001 && tone(l, 440, rate) > 0.49, "soloing a track silences a bus fed only by other tracks");
        p.findTrack(t2)->solo = false;
        p.findTrack(t1)->solo = true;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.5) < 0.005 && tone(l, 440, rate) < 0.001, "a soloed track is heard through its bus");
        p.findTrack(t1)->solo = false;
        // a track that sends to a soloed bus is heard there but not directly
        p.findTrack(t1)->output = 0;
        Send s2;
        s2.bus = busB;
        s2.level = 1.0;
        p.findTrack(t1)->sends.push_back(s2);
        p.findBus(busB)->pan = 1.0;
        p.findBus(busB)->solo = true;
        block(2.0);
        check(tone(l, 220, rate) < 0.001 && std::fabs(tone(r, 220, rate) - 0.5) < 0.005 && tone(r, 440, rate) < 0.001,
              "soloing a bus keeps tracks' sends to it but not their own output", strfmt("%.3f / %.3f", tone(l, 220, rate), tone(r, 220, rate)));
        p.findBus(busB)->solo = false;
        p.findBus(busB)->pan = 0;
        p.findTrack(t1)->sends.clear();

        // the master
        p.masterPan = 1.0;
        block(2.0);
        check(rms(l) < 1e-6 && rms(r) > 0.3, "the master pans");
        p.masterPan = -0.5;
        block(2.0);
        check(std::fabs(tone(r, 440, rate) - 0.25) < 0.005 && std::fabs(tone(l, 440, rate) - 0.5) < 0.005, "the master pans part way with the tracks' pan law");
        p.masterPan = 0;
        p.masterMute = true;
        block(2.0);
        check(rms(l) < 1e-9 && audio.peakLeft == 0, "the master mutes");
        p.masterMute = false;

        // an effect on a bus
        if (!vst.empty()) {
            std::vector<AudioPluginDesc> d = describeAudioPluginFile(vst, &err);
            if (d.empty()) check(false, "the VST effect is recognised", err);
            else {
                p.findTrack(t2)->mute = true;
                p.findTrack(t1)->output = busA;
                block(2.0);
                const std::vector<float> dry = l;
                FxSlot fs;
                fs.id = p.newId();
                fs.kind = "audio";
                fs.audio = d[0];
                fs.name = d[0].name;
                p.findBus(busA)->fx.push_back(fs);
                fx.prepare(p);
                check(fx.audio(fs.id) != nullptr, "an effect on a bus can be made", fx.error(fs.id));
                for (int k = 0; k < 5; ++k) block(2.0 + k * 0.1);
                block(2.0);
                double diff = 0;
                for (size_t k = 0; k < l.size(); ++k) diff = std::max(diff, (double)std::fabs(l[k] - dry[k]));
                check(diff > 0.001, "and changes the bus's sound", strfmt("difference %.4f", diff));
                p.findBus(busA)->fx[0].bypass = true;
                block(2.0);
                diff = 0;
                for (size_t k = 0; k < l.size(); ++k) diff = std::max(diff, (double)std::fabs(l[k] - dry[k]));
                check(diff < 1e-6, "a bypassed bus effect does nothing");
                // an effect on the master bus
                p.findBus(busA)->fx[0].bypass = false;
                FxSlot ms = fs;
                ms.id = p.newId();
                p.findBus(busA)->fx.clear();
                p.audioOut.push_back(ms);
                fx.prepare(p);
                for (int k = 0; k < 5; ++k) block(2.0 + k * 0.1);
                block(2.0);
                diff = 0;
                for (size_t k = 0; k < l.size(); ++k) diff = std::max(diff, (double)std::fabs(l[k] - dry[k]));
                check(diff > 0.001, "an effect on the master changes the mix", strfmt("difference %.4f", diff));
                p.audioOut.clear();
                fx.clear();
            }
        }
    }

    // ---------------------------------------------------------------- reversed events
    {
        Project p;
        p.nextId = 30000;
        const uint64_t ramp = addMedia(p, "ramp:");
        const uint64_t t = addTrack(p).id;
        const uint64_t e = addEvent(p, t, ramp, 1.0, 3.0, 2.0);
        p.findEvent(e)->reverse = true;
        // the event plays media seconds 2..5 backwards: its first sample is media sample 5 s - 1
        std::vector<float> L(rate), R(rate);
        audio.render(p, rate, rate, L.data(), R.data(), true);
        const int64_t endIdx = 5 * rate;
        int wrong = 0;
        for (int k = 0; k < rate; ++k)
            if (L[(size_t)k] != (float)((endIdx - 1 - k) % 100000) / 100000.f) ++wrong;
        check(wrong == 0, "a reversed event plays its media backwards, sample for sample", strfmt("%d wrong", wrong));
        // the same stretch in blocks of odd sizes, starting at an arbitrary place
        std::vector<float> longL(30000), longR(30000), shortL(30000), shortR(30000);
        audio.render(p, rate * 2 + 123, 30000, longL.data(), longR.data(), true);
        for (int at = 0; at < 30000; at += 441) audio.render(p, rate * 2 + 123 + at, std::min(441, 30000 - at), shortL.data() + at, shortR.data() + at, true);
        double worst = 0;
        for (int k = 0; k < 30000; ++k) worst = std::max(worst, (double)std::fabs(longL[(size_t)k] - shortL[(size_t)k]));
        check(worst == 0, "reversed sound joins up exactly across blocks of any size", strfmt("%g", worst));
        // the last sample of the event is the first of its stretch of media
        audio.render(p, rate * 4 - 10, 10, L.data(), R.data(), true);
        check(L[9] == (float)((2 * rate) % 100000) / 100000.f, "and ends on the first sample of its media", strfmt("%g", L[9]));
        // at double speed (tape-like), backwards
        p.findEvent(e)->rate = 2.0;
        audio.render(p, rate, 1000, L.data(), R.data(), true);
        const int64_t end2 = (int64_t)std::llround((2.0 + 3.0 * 2.0) * 24000);
        wrong = 0;
        for (int k = 0; k < 1000; ++k)
            if (L[(size_t)k] != (float)((end2 - 1 - k) % 100000) / 100000.f) ++wrong;
        check(wrong == 0, "reversed at double speed reads every other sample's worth of media backwards", strfmt("%d wrong", wrong));
        // a tone reversed is still the tone, without clicks
        Project q;
        q.nextId = 40000;
        const uint64_t m2 = addMedia(q, "synth:2");
        const uint64_t qt = addTrack(q).id;
        const uint64_t qe = addEvent(q, qt, m2, 0, 9, 0.5);
        q.findEvent(qe)->reverse = true;
        std::vector<float> s(96000), sr(96000);
        for (int at = 0; at < 96000; at += 1024) audio.render(q, 48000 + at, std::min(1024, 96000 - at), s.data() + at, sr.data() + at, true);
        const double f = frequency(s, rate);
        check(std::fabs(f - 440) < 0.05 && maxStep(s) < 0.5 * 2 * kPi * 440 / rate * 1.05, "a reversed tone keeps its frequency with no clicks",
              strfmt("%.3f Hz, largest step %.4f", f, maxStep(s)));
    }

    // ---------------------------------------------------------------- pitch and keeping pitch
    {
        Project p;
        p.nextId = 50000;
        const uint64_t m2 = addMedia(p, "synth:2");                    // 440 Hz
        const uint64_t t = addTrack(p).id;
        const uint64_t e = addEvent(p, t, m2, 0, 9.5);
        // plays [from, from + n) in playback-sized blocks
        auto play = [&](double from, int n, std::vector<float>& out, int blockSize) {
            out.assign((size_t)n, 0.f);
            std::vector<float> rr((size_t)n);
            const int64_t s0 = (int64_t)std::llround(from * rate);
            for (int at = 0; at < n; at += blockSize) audio.render(p, s0 + at, std::min(blockSize, n - at), out.data() + at, rr.data() + at, true);
        };
        std::vector<float> s;
        struct Case { double pitch, rate; bool keep; double want; const char* what; };
        const Case cases[] = {
            {12, 1, false, 880, "an octave up"},
            {-12, 1, false, 220, "an octave down"},
            {7, 1, false, 440 * std::pow(2.0, 7 / 12.0), "a fifth up"},
            {-5, 1, false, 440 * std::pow(2.0, -5 / 12.0), "five semitones down"},
            {0.5, 1, false, 440 * std::pow(2.0, 0.5 / 12.0), "a quarter tone up"},
            {0, 2, true, 440, "double speed keeping pitch"},
            {0, 0.5, true, 440, "half speed keeping pitch"},
            {0, 1.37, true, 440, "1.37x keeping pitch"},
            {3, 2, true, 440 * std::pow(2.0, 3 / 12.0), "double speed keeping pitch, 3 semitones up"},
            {-12, 2, false, 440, "double speed (tape) then an octave down"},
            {4, 0.8, false, 440 * 0.8 * std::pow(2.0, 4 / 12.0), "0.8x (tape) then 4 semitones up"},
        };
        double worstCents = 0, worstStep = 0;
        for (const Case& c : cases) {
            Event* ev = p.findEvent(e);
            ev->pitch = c.pitch;
            ev->rate = c.rate;
            ev->keepPitch = c.keep;
            ev->length = 4.5 / c.rate;
            play(0.5, 48000, s, 1024);
            const double f = frequency(s, rate);
            const double cents = 1200.0 * std::log2(f / c.want);
            const double step = maxStep(s) / (0.5 * 2 * kPi * c.want / rate);       // 1 = what a clean sine's steepest step is
            const double amp = tone(s, c.want, rate);
            worstCents = std::max(worstCents, std::fabs(cents));
            worstStep = std::max(worstStep, step);
            check(std::fabs(cents) < 1.0 && step < 1.25 && amp > 0.4, strfmt("pitch: %s", c.what),
                  strfmt("%.3f Hz (want %.3f, %.3f cents), level %.3f, steepest step %.2fx a clean sine's", f, c.want, cents, amp, step));
        }
        std::printf("INFO worst pitch error %.3f cents, worst step %.2fx\n", worstCents, worstStep);

        // blocks of any size join up exactly while stretching
        Event* ev = p.findEvent(e);
        ev->pitch = 5;
        ev->rate = 1.0;
        ev->keepPitch = false;
        ev->length = 9;
        std::vector<float> a, b;
        play(1.0, 30000, a, 30000);
        play(1.0, 30000, b, 441);
        double worst = 0;
        for (size_t k = 0; k < a.size(); ++k) worst = std::max(worst, (double)std::fabs(a[k] - b[k]));
        check(worst < 1e-6, "pitched sound joins up exactly across blocks of any size", strfmt("%g", worst));

        // reversed and pitched
        ev->reverse = true;
        play(1.0, 48000, s, 777);
        const double fr = frequency(s, rate), want = 440 * std::pow(2.0, 5 / 12.0);
        check(std::fabs(1200.0 * std::log2(fr / want)) < 1.0, "a reversed event can be pitched too", strfmt("%.3f Hz (want %.3f)", fr, want));
        ev->reverse = false;

        // keeping pitch: the tone lasts for 4 s of media, so at double speed it is over 2 s into the event
        Project q;
        q.nextId = 60000;
        const uint64_t tm = addMedia(q, "tone:440:4");
        const uint64_t qt = addTrack(q).id;
        const uint64_t qe = addEvent(q, qt, tm, 0, 4);
        q.findEvent(qe)->rate = 2.0;
        q.findEvent(qe)->keepPitch = true;
        q.findEvent(qe)->loop = false;
        std::vector<float> all((size_t)rate * 4), allR((size_t)rate * 4);
        for (int at = 0; at < rate * 4; at += 1024) audio.render(q, at, std::min(1024, rate * 4 - at), all.data() + at, allR.data() + at, true);
        std::vector<float> before(all.begin() + (size_t)(rate * 1.4), all.begin() + (size_t)(rate * 1.9));
        std::vector<float> after(all.begin() + (size_t)(rate * 2.1), all.begin() + (size_t)(rate * 2.6));
        check(std::fabs(frequency(before, rate) - 440) < 0.3 && rms(before) > 0.3 && rms(after) < 0.01,
              "keeping pitch at double speed: same tone, half the length",
              strfmt("%.2f Hz, before %.3f after %.4f", frequency(before, rate), rms(before), rms(after)));
        q.findEvent(qe)->rate = 0.5;
        q.findEvent(qe)->length = 9.5;
        std::vector<float> slow((size_t)rate * 10), slowR((size_t)rate * 10);
        for (int at = 0; at < rate * 10; at += 1024) audio.render(q, at, std::min(1024, rate * 10 - at), slow.data() + at, slowR.data() + at, true);
        std::vector<float> s1(slow.begin() + (size_t)(rate * 7.2), slow.begin() + (size_t)(rate * 7.7));
        std::vector<float> s2(slow.begin() + (size_t)(rate * 8.3), slow.begin() + (size_t)(rate * 8.8));
        check(std::fabs(frequency(s1, rate) - 440) < 0.3 && rms(s1) > 0.3 && rms(s2) < 0.01, "keeping pitch at half speed: same tone, twice the length",
              strfmt("%.2f Hz, before %.3f after %.4f", frequency(s1, rate), rms(s1), rms(s2)));

        // a jump (a seek) starts the stretcher again cleanly
        q.findEvent(qe)->rate = 1.5;
        q.findEvent(qe)->length = 2.5;
        std::vector<float> j1(4800), j2(4800);
        audio.render(q, 30000, 4800, j1.data(), j2.data(), true);
        audio.render(q, 70000, 4800, j1.data(), j2.data(), true);
        check(std::fabs(frequency(j1, rate) - 440) < 1.0 && rms(j1) > 0.3, "after a jump the stretched sound picks up straight away", strfmt("%.2f Hz", frequency(j1, rate)));
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
