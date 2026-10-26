// Tests of mixer envelopes (track, event, bus, master volume / pan / mute, sends, curves, fades, effect automation)
// with made-up media. Projects without envelopes must sound unchanged.
//
//   audio_envelopes_test [a VST2 effect]
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

// Made-up media (all 10 s long):
//   "dc:"           every sample 0.5 on both sides: what comes out is 0.5 x the gain applied
//   "lr:"           0.5 on the left, 0.25 on the right
//   "synth:<n>"     a 220 x n Hz tone at half volume
class Synth : public MediaReader {
public:
    MediaInfo mi;
    int kind = 0;               // 0 tone, 1 dc, 2 lr
    double freq = 220;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        const int64_t end = (int64_t)std::llround(mi.duration * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            float a = 0, b = 0;
            if (n >= 0 && n < end) {
                if (kind == 1) a = b = 0.5f;
                else if (kind == 2) { a = 0.5f; b = 0.25f; }
                else a = b = 0.5f * (float)std::sin(2.0 * kPi * freq * (double)n / sampleRate);
            }
            left[k] = a;
            right[k] = b;
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
    else if (path == "dc:") r->kind = 1;
    else if (path == "lr:") r->kind = 2;
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

static uint64_t addTrack(Project& p)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    p.tracks.push_back(t);
    return t.id;
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

static Envelope env(const std::string& kind, std::vector<EnvPoint> pts, uint64_t target = 0)
{
    Envelope e;
    e.kind = kind;
    e.target = target;
    e.points = std::move(pts);
    return e;
}

static EnvPoint pt(double t, double v, int curve = CurveLinear)
{
    EnvPoint p;
    p.time = t;
    p.value = v;
    p.curve = curve;
    return p;
}

// The mixer before buses and envelopes (copied from tests/engine/audio_buses_test.cpp): projects without
// envelopes must still sound exactly like this.
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
    FxRuntime fx;
    MediaPool pool;
    pool.setOpener(synthOpen);
    AudioEngine audio(pool, fx);
    const int rate = 48000;
    const double eps = 1e-5;

    // the mix at one moment (left, right), from a short block starting there
    auto at = [&](const Project& p, double t, float* L = nullptr, float* R = nullptr) {
        float l[16], r[16];
        audio.render(p, (int64_t)std::llround(t * rate), 16, l, r, true);
        if (L) *L = l[0];
        if (R) *R = r[0];
        return l[0];
    };
    // a longer stretch
    auto stretch = [&](const Project& p, double from, double to, std::vector<float>& l, std::vector<float>& r) {
        const int n = (int)std::llround((to - from) * rate);
        l.assign((size_t)n, 0.f);
        r.assign((size_t)n, 0.f);
        audio.render(p, (int64_t)std::llround(from * rate), n, l.data(), r.data(), true);
    };
    auto maxStep = [](const std::vector<float>& s) {
        float m = 0;
        for (size_t k = 1; k < s.size(); ++k) m = std::max(m, std::fabs(s[k] - s[k - 1]));
        return m;
    };

    // ---------------------------------------------------------------- without envelopes: unchanged
    {
        Project p;
        p.nextId = 10000;
        const uint64_t m1 = addMedia(p, "synth:1"), m2 = addMedia(p, "synth:2"), m3 = addMedia(p, "synth:3");
        const uint64_t a = addTrack(p), b = addTrack(p);
        addEvent(p, a, m1, 0.3, 3.0, 0.7);
        const uint64_t e2 = addEvent(p, a, m2, 2.5, 2.0);
        p.findEvent(e2)->fadeIn = 0.4;
        p.findEvent(e2)->gain = 0.8;
        const uint64_t e3 = addEvent(p, b, m3, 1.0, 4.0, 9.0);
        p.findEvent(e3)->rate = 1.37;
        p.findEvent(e3)->fadeOut = 1.2;
        p.findTrack(a)->pan = -0.3;
        p.findTrack(a)->level = 0.7;
        p.findTrack(b)->pan = 0.6;
        p.findTrack(b)->level = 1.3;
        p.masterVolume = 0.9;
        std::vector<float> L(20000), R(20000), RL(20000), RR(20000);
        double worst = 0;
        auto compare = [&]() {
            for (int64_t s : {0, 30000, 110000, 150001, 230000}) {
                audio.render(p, s, 20000, L.data(), R.data(), true);
                referenceRender(pool, p, s, 20000, RL.data(), RR.data());
                for (int k = 0; k < 20000; ++k) worst = std::max(worst, (double)std::max(std::fabs(L[k] - RL[k]), std::fabs(R[k] - RR[k])));
            }
        };
        compare();
        check(worst == 0.0, "a project without envelopes sounds exactly as before (bit for bit)", strfmt("worst difference %g", worst));
        // envelopes with no points change nothing either
        worst = 0;
        p.findTrack(a)->envelopes.push_back(env("volume", {}));
        p.findTrack(b)->envelopes.push_back(env("pan", {}));
        p.findEvent(e2)->envelopes.push_back(env("volume", {}));
        p.masterEnvelopes.push_back(env("volume", {}));
        compare();
        check(worst == 0.0, "envelopes without points change nothing", strfmt("worst difference %g", worst));
        // nor do hidden envelopes stop working, or curve settings on linear fades change a cross-fade
        Envelope hidden = env("volume", {pt(0, 0.5)});
        hidden.visible = false;
        p.findTrack(a)->envelopes = {hidden};
        float l1 = 0;
        at(p, 0.6012, &l1);
        p.findTrack(a)->envelopes.clear();
        float l0 = 0;
        at(p, 0.6012, &l0);
        check(std::fabs(l0) > 0.01f && std::fabs(l1 - l0 * 0.5f) < 1e-6, "a hidden envelope still applies", strfmt("%g vs %g", l1, l0));
    }

    // ---------------------------------------------------------------- track volume
    Project p;
    p.nextId = 20000;
    const uint64_t dc = addMedia(p, "dc:");
    const uint64_t lr = addMedia(p, "lr:");
    const uint64_t t1 = addTrack(p);
    const uint64_t ev = addEvent(p, t1, dc, 0.0, 10.0);
    auto trk = [&]() { return p.findTrack(t1); };
    {
        trk()->envelopes = {env("volume", {pt(1.0, 1.0), pt(2.0, 0.25), pt(3.0, 2.0)})};
        check(std::fabs(at(p, 0.5) - 0.5f) < eps, "before the first point: the first point's value", strfmt("%g", at(p, 0.5)));
        check(std::fabs(at(p, 1.0) - 0.5f) < eps && std::fabs(at(p, 2.0) - 0.125f) < eps && std::fabs(at(p, 3.0) - 1.0f) < eps, "at the points: their values",
              strfmt("%g %g %g", at(p, 1.0), at(p, 2.0), at(p, 3.0)));
        check(std::fabs(at(p, 1.5) - 0.5f * 0.625f) < eps, "half way along a linear segment: half way in gain", strfmt("%g", at(p, 1.5)));
        check(std::fabs(at(p, 5.0) - 1.0f) < eps, "after the last point: the last point's value (+6 dB)", strfmt("%g", at(p, 5.0)));
        trk()->level = 0.5;
        check(std::fabs(at(p, 3.5) - 0.5f) < eps, "the envelope multiplies the track's volume slider (trim)", strfmt("%g", at(p, 3.5)));
        trk()->level = 1.0;
        // a ramp is smooth: no step bigger than the ramp's own slope
        std::vector<float> l, r;
        stretch(p, 0.9, 1.999, l, r);
        const float slope = (float)(0.5 * 0.75 / rate);
        check(maxStep(l) <= slope * 1.01f + 1e-7f, "a ramp changes every sample (no zipper steps)", strfmt("largest step %g, slope %g", maxStep(l), slope));
        // the same whatever the block size
        std::vector<float> one(48000), pieces(48000), rr(48000);
        audio.render(p, 48000, 48000, one.data(), rr.data(), true);
        for (int s = 0, k = 0; s < 48000; s += k) {
            k = std::min(48000 - s, 37 + (s % 1500));
            audio.render(p, 48000 + s, k, pieces.data() + s, rr.data() + s, true);
        }
        double d = 0;
        for (int k = 0; k < 48000; ++k) d = std::max(d, (double)std::fabs(one[(size_t)k] - pieces[(size_t)k]));
        check(d == 0.0, "envelopes come out the same in blocks of any size", strfmt("%g", d));
    }
    // curves
    {
        const struct { int c; const char* n; } curves[] = {{CurveFast, "fast"}, {CurveSlow, "slow"}, {CurveSmooth, "smooth"}, {CurveSharp, "sharp"}};
        for (const auto& c : curves) {
            trk()->envelopes = {env("volume", {pt(1.0, 0.0, c.c), pt(2.0, 1.0)})};
            bool ok = true;
            std::string say;
            for (double x : {0.25, 0.5, 0.75}) {
                const float want = 0.5f * (float)curveShape(c.c, x);
                const float got = at(p, 1.0 + x);
                if (std::fabs(got - want) > eps) ok = false;
                say += strfmt("%.4f/%.4f ", got, want);
            }
            check(ok, strfmt("a %s segment follows its curve", c.n), say);
        }
        trk()->envelopes = {env("volume", {pt(1.0, 0.0, CurveFast), pt(2.0, 1.0)})};
        check(at(p, 1.5) > 0.5f * 0.6f, "fast rises quickly at first");
        trk()->envelopes = {env("volume", {pt(1.0, 0.0, CurveSlow), pt(2.0, 1.0)})};
        check(at(p, 1.5) < 0.5f * 0.4f, "slow rises slowly at first");
        // hold: the value stays until the next point, then steps
        trk()->envelopes = {env("volume", {pt(1.0, 1.0, CurveHold), pt(2.0, 0.2)})};
        check(std::fabs(at(p, 1.999) - 0.5f) < eps && std::fabs(at(p, 2.0) - 0.1f) < eps, "a hold point keeps its value up to the next point, then steps",
              strfmt("%g then %g", at(p, 1.999), at(p, 2.0)));
        std::vector<float> l, r;
        stretch(p, 1.9, 2.1, l, r);
        check(std::fabs(maxStep(l) - 0.4f) < 1e-5f, "(one step, of the whole change)", strfmt("%g", maxStep(l)));
        trk()->envelopes.clear();
    }
    // mute
    {
        trk()->envelopes = {env("mute", {pt(0.0, 0.0), pt(2.0, 1.0), pt(4.0, 0.0)})};
        check(std::fabs(at(p, 1.0) - 0.5f) < eps && at(p, 2.5) == 0.f && std::fabs(at(p, 4.5) - 0.5f) < eps && std::fabs(at(p, 1.999) - 0.5f) < eps,
              "a mute envelope silences the track where it is on (and always holds)", strfmt("%g %g %g", at(p, 1.0), at(p, 2.5), at(p, 4.5)));
        trk()->envelopes.clear();
    }
    // pan: the same pan law as the track's pan slider
    {
        float l = 0, r = 0, l2 = 0, r2 = 0;
        trk()->envelopes = {env("pan", {pt(0.0, -1.0), pt(4.0, 1.0)})};
        at(p, 0.0, &l, &r);
        check(std::fabs(l - 0.5f) < eps && r == 0.f, "a pan envelope at full left", strfmt("%g / %g", l, r));
        at(p, 3.0, &l, &r);
        trk()->envelopes.clear();
        trk()->pan = 0.5;
        at(p, 3.0, &l2, &r2);
        check(l == l2 && r == r2, "a pan envelope at 50% R sounds exactly like the pan slider there", strfmt("%g/%g vs %g/%g", l, r, l2, r2));
        trk()->pan = -0.25;
        trk()->envelopes = {env("pan", {pt(0.0, 0.5)})};
        at(p, 3.0, &l, &r);
        trk()->envelopes.clear();
        trk()->pan = 0.25;
        at(p, 3.0, &l2, &r2);
        check(l == l2 && r == r2, "the pan slider is added to the pan envelope (trim)", strfmt("%g/%g vs %g/%g", l, r, l2, r2));
        trk()->pan = 0;
    }

    // ---------------------------------------------------------------- event envelopes, channels, fades
    {
        Event* e = p.findEvent(ev);
        e->start = 1.0;
        e->length = 4.0;
        e->envelopes = {env("volume", {pt(0.0, 0.0), pt(1.0, 1.0)})};
        check(std::fabs(at(p, 1.5) - 0.25f) < eps && std::fabs(at(p, 2.5) - 0.5f) < eps, "an event's volume envelope counts its times from the event's start",
              strfmt("%g %g", at(p, 1.5), at(p, 2.5)));
        p.findEvent(ev)->start = 3.0;
        check(std::fabs(at(p, 3.5) - 0.25f) < eps, "and moves with the event", strfmt("%g", at(p, 3.5)));
        p.findEvent(ev)->gain = 0.5;
        check(std::fabs(at(p, 4.5) - 0.25f) < eps, "it multiplies the event's own gain", strfmt("%g", at(p, 4.5)));
        p.findEvent(ev)->gain = 1.0;
        trk()->level = 0.5;
        check(std::fabs(at(p, 3.5) - 0.125f) < eps, "and the track's volume", strfmt("%g", at(p, 3.5)));
        trk()->level = 1.0;
        p.findEvent(ev)->envelopes = {env("pan", {pt(0.0, -1.0), pt(2.0, 1.0)})};
        float l = 0, r = 0;
        at(p, 3.0, &l, &r);
        check(std::fabs(l - 0.5f) < eps && r == 0.f, "an event's pan envelope", strfmt("%g / %g", l, r));
        at(p, 4.5, &l, &r);
        check(std::fabs(l - 0.25f) < eps && std::fabs(r - 0.5f) < eps, "with the same pan law", strfmt("%g / %g", l, r));
        p.findEvent(ev)->envelopes.clear();

        // channel mapping
        p.findEvent(ev)->mediaId = lr;
        const struct { const char* ch; float l, r; } maps[] = {
            {"both", 0.5f, 0.25f}, {"left", 0.5f, 0.5f}, {"right", 0.25f, 0.25f}, {"swap", 0.25f, 0.5f}, {"combine", 0.375f, 0.375f}};
        for (const auto& m : maps) {
            p.findEvent(ev)->channels = m.ch;
            at(p, 4.0, &l, &r);
            check(std::fabs(l - m.l) < eps && std::fabs(r - m.r) < eps, strfmt("channels: %s", m.ch), strfmt("%g / %g", l, r));
        }
        p.findEvent(ev)->channels = "both";
        p.findEvent(ev)->mediaId = dc;

        // fade curves
        p.findEvent(ev)->fadeIn = 1.0;
        p.findEvent(ev)->fadeInCurve = CurveFast;
        check(std::fabs(at(p, 3.5) - 0.5f * (float)curveShape(CurveFast, 0.5)) < eps, "a fade in follows its curve (fast)", strfmt("%g", at(p, 3.5)));
        p.findEvent(ev)->fadeInCurve = CurveSmooth;
        check(std::fabs(at(p, 3.25) - 0.5f * (float)curveShape(CurveSmooth, 0.25)) < eps, "(smooth)", strfmt("%g", at(p, 3.25)));
        p.findEvent(ev)->fadeIn = 0;
        p.findEvent(ev)->fadeOut = 2.0;
        p.findEvent(ev)->fadeOutCurve = CurveSlow;
        check(std::fabs(at(p, 6.0) - 0.5f * (float)(1.0 - curveShape(CurveSlow, 0.5))) < eps, "a fade out follows its curve (slow)", strfmt("%g", at(p, 6.0)));
        p.findEvent(ev)->fadeOut = 0;
        p.findEvent(ev)->fadeOutCurve = CurveLinear;
        p.findEvent(ev)->fadeInCurve = CurveLinear;
        // a cross-fade: the later event fades in by its fade-in curve, the earlier out by its fade-out curve
        const uint64_t ev2 = addEvent(p, t1, dc, 6.0, 3.0);
        check(std::fabs(at(p, 6.5) - 0.5f) < eps, "a linear cross-fade of two equal sounds adds up to the whole", strfmt("%g", at(p, 6.5)));
        p.findEvent(ev2)->fadeInCurve = CurveSlow;
        p.findEvent(ev)->fadeOutCurve = CurveFast;
        const float want = 0.5f * (float)(curveShape(CurveSlow, 0.25) + 1.0 - curveShape(CurveFast, 0.25));
        check(std::fabs(at(p, 6.25) - want) < eps, "a cross-fade follows the events' fade curves", strfmt("%g (want %g)", at(p, 6.25), want));
        Track* t = trk();
        t->events.erase(std::remove_if(t->events.begin(), t->events.end(), [&](const Event& o) { return o.id == ev2; }), t->events.end());
        p.findEvent(ev)->fadeOutCurve = CurveLinear;
        p.findEvent(ev)->start = 0.0;
        p.findEvent(ev)->length = 10.0;
    }

    // ---------------------------------------------------------------- sends, buses, master
    {
        const uint64_t bus = addBus(p, "A");
        Send s;
        s.bus = bus;
        s.level = 1.0;
        s.preFader = true;
        trk()->sends = {s};
        trk()->level = 0.0;             // only the (pre-fader) send is heard
        trk()->envelopes = {env("send", {pt(1.0, 0.0), pt(3.0, 1.0)}, bus)};
        check(at(p, 0.5) == 0.f && std::fabs(at(p, 2.0) - 0.25f) < eps && std::fabs(at(p, 3.5) - 0.5f) < eps, "a send envelope shapes the send to its bus",
              strfmt("%g %g %g", at(p, 0.5), at(p, 2.0), at(p, 3.5)));
        trk()->sends[0].level = 0.5;
        check(std::fabs(at(p, 2.0) - 0.125f) < eps, "and multiplies the send's level", strfmt("%g", at(p, 2.0)));
        trk()->envelopes.push_back(env("mute", {pt(0.0, 0.0), pt(2.5, 1.0)}));
        check(at(p, 2.75) == 0.f, "a mute envelope silences pre-fader sends too", strfmt("%g", at(p, 2.75)));
        trk()->envelopes.pop_back();
        // a send envelope to a bus the track has no send for: a post-fader send shaped by the envelope
        trk()->sends.clear();
        trk()->level = 1.0;
        p.findBus(bus)->pan = 1.0;      // the bus on the right only...
        trk()->pan = -1.0;              // ...the track on the left only
        float l = 0, r = 0;
        trk()->envelopes = {env("send", {pt(1.0, 0.0), pt(3.0, 1.0)}, bus)};
        trk()->pan = 0.0;
        trk()->envelopes.push_back(env("pan", {pt(0.0, 0.0)}));
        at(p, 2.0, &l, &r);
        check(std::fabs(l - 0.5f) < eps && std::fabs(r - 0.75f) < eps, "a send envelope with no send of its own is a post-fader send at 0 dB",
              strfmt("%g / %g", l, r));
        trk()->envelopes.clear();
        p.findBus(bus)->pan = 0.0;
        // bus envelopes
        trk()->output = bus;
        p.findBus(bus)->envelopes = {env("volume", {pt(1.0, 1.0), pt(2.0, 0.0)})};
        check(std::fabs(at(p, 1.5) - 0.25f) < eps, "a bus's volume envelope", strfmt("%g", at(p, 1.5)));
        p.findBus(bus)->envelopes = {env("pan", {pt(0.0, 1.0)})};
        at(p, 1.5, &l, &r);
        check(l == 0.f && std::fabs(r - 0.5f) < eps, "a bus's pan envelope", strfmt("%g / %g", l, r));
        p.findBus(bus)->envelopes = {env("mute", {pt(1.0, 0.0), pt(2.0, 1.0)})};
        check(std::fabs(at(p, 1.5) - 0.5f) < eps && at(p, 2.5) == 0.f, "a bus's mute envelope");
        p.findBus(bus)->envelopes.clear();
        trk()->output = 0;
        // the master
        p.masterEnvelopes = {env("volume", {pt(1.0, 1.0), pt(2.0, 0.5)})};
        p.masterVolume = 0.5;
        check(std::fabs(at(p, 1.5) - 0.5f * 0.75f * 0.5f) < eps, "the master's volume envelope (times the master fader)", strfmt("%g", at(p, 1.5)));
        p.masterVolume = 1.0;
        p.masterEnvelopes = {env("mute", {pt(1.0, 0.0), pt(2.0, 1.0)})};
        check(std::fabs(at(p, 1.5) - 0.5f) < eps && at(p, 2.5) == 0.f && audio.peakLeft == 0.f, "the master's mute envelope (and its meter)");
        p.masterEnvelopes.clear();
    }

    // ---------------------------------------------------------------- effect automation
    if (!vst.empty()) {
        std::string err;
        std::vector<AudioPluginDesc> d = describeAudioPluginFile(vst, &err);
        if (d.empty()) check(false, "the VST effect is recognised", err);
        else {
            FxSlot fs;
            fs.id = p.newId();
            fs.kind = "audio";
            fs.audio = d[0];
            fs.name = d[0].name;
            trk()->fx.push_back(fs);
            fx.prepare(p);
            AudioPlugin* pl = fx.audio(fs.id);
            check(pl != nullptr && pl->paramCount() > 0, "the effect is made and has settings", fx.error(fs.id));
            if (pl && pl->paramCount() > 0) {
                const std::string name = pl->paramName(0);
                Envelope a = env("fx", {pt(1.0, 0.1), pt(3.0, 0.9)}, fs.id);
                a.param = name;
                trk()->envelopes = {a};
                std::vector<float> l(64), r(64);
                audio.render(p, 2 * rate, 64, l.data(), r.data(), true);
                const float v1 = pl->paramGet(0);
                audio.render(p, (int64_t)(2.5 * rate), 64, l.data(), r.data(), true);
                const float v2 = pl->paramGet(0);
                check(std::fabs(v1 - 0.5f) < 0.01f && std::fabs(v2 - 0.7f) < 0.01f, "an fx envelope moves the plug-in's setting \"" + name + "\" as it plays",
                      strfmt("%.3f then %.3f", v1, v2));
                trk()->envelopes[0].param = "0";
                audio.render(p, 1 * rate, 64, l.data(), r.data(), true);
                check(std::fabs(pl->paramGet(0) - 0.1f) < 0.01f, "(a setting can be named by its number too)", strfmt("%.3f", pl->paramGet(0)));
                trk()->envelopes[0].param = "no such setting";
                audio.render(p, 3 * rate, 64, l.data(), r.data(), true);
                check(std::fabs(pl->paramGet(0) - 0.1f) < 0.01f, "an fx envelope for a setting that is not there does nothing");
            }
            trk()->fx.clear();
            trk()->envelopes.clear();
            fx.clear();
        }
    }

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
