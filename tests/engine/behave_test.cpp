// Tests of behaviour copied from VEGAS: default fade curves, a cross-fade's curve on the later event, Quickfade
// Audio Edits, and the track pan types (Add Channels, Balance, Constant Power, Film). Made-up media.
//
//   behave_test [folder with the VEGAS sample projects (default $RENO_SAMPLES)]
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include "../../src/vegas/vegas.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}
static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

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

// (each project its own ids: the media pool keeps its readers by event id)
static uint64_t gNextId = 1;
static void fresh(Project& p) { p.nextId = gNextId; gNextId += 1000; }

int main(int argc, char** argv)
{
    const std::string samples = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    FxRuntime fx;
    MediaPool pool;
    pool.setOpener(synthOpen);
    AudioEngine audio(pool, fx);
    const int rate = 48000;
    auto at = [&](const Project& p, double t, float* L = nullptr, float* R = nullptr) {
        float l[4], r[4];
        audio.render(p, (int64_t)std::llround(t * rate), 4, l, r, true);
        if (L) *L = l[0];
        if (R) *R = r[0];
        return l[0];
    };

    // ---- VEGAS's curve formulas (EnvelopeCurve_GetValueAt in vegas180k.dll)
    {
        bool ok = true;
        std::string say;
        for (double x : {0.1, 0.25, 0.5, 0.8}) {
            const double fast = 1 - (1 - x) * (1 - x), slow = x * x, smooth = 3 * x * x - 2 * x * x * x, sharp = 4 * x * x * x - 6 * x * x + 3 * x;
            if (!near(curveShape(CurveFast, x), fast, 1e-12) || !near(curveShape(CurveSlow, x), slow, 1e-12) ||
                !near(curveShape(CurveSmooth, x), smooth, 1e-12) || !near(curveShape(CurveSharp, x), sharp, 1e-12) ||
                !near(curveShape(CurveLinear, x), x, 1e-12)) { ok = false; say += std::to_string(x) + " "; }
        }
        check(ok, "curve shapes are VEGAS's (Fast, Slow, Smooth, Sharp = 4x^3 - 6x^2 + 3x)", say);
        check(near(curveShape(CurveSharp, 0.5), 0.5, 1e-12) && near(curveShape(CurveHold, 0.5), 0.0, 1e-12) && near(curveShape(CurveHold, 1.0), 1.0, 1e-12),
              "Sharp passes the middle level; Hold steps at the end");
        // Sharp is level in the middle (VEGAS's), not steep like the inverse of Smooth
        const double slope = (curveShape(CurveSharp, 0.51) - curveShape(CurveSharp, 0.49)) / 0.02;
        check(slope < 0.01, "Sharp is flat at the middle", std::to_string(slope));
    }

    // ---- the fade curves of a new event (VEGAS: 2 / -2 / reciprocal -2 for sound, 4 / 4 / 4 for pictures)
    {
        Event a, v;
        setDefaultFadeCurves(a, TrackType::Audio);
        setDefaultFadeCurves(v, TrackType::Video);
        check(a.fadeInCurve == CurveFast && a.fadeOutCurve == CurveSlow && a.fadeInRecipCurve == CurveSlow, "new sound event: Fast in, Slow out");
        check(v.fadeInCurve == CurveSmooth && v.fadeOutCurve == CurveSmooth && v.fadeInRecipCurve == CurveSmooth, "new picture event: Smooth in and out");
        // a default video cross-fade adds up to the whole picture all the way through
        Track tr;
        tr.type = TrackType::Video;
        Event A = v, B = v;
        A.id = 1; A.start = 0; A.length = 2;
        B.id = 2; B.start = 1; B.length = 2;
        tr.events = {A, B};
        double worst = 0;
        for (double t = 1.0; t < 2.0; t += 0.05) worst = std::max(worst, std::fabs(overlapWeight(tr, A, t) + overlapWeight(tr, B, t) - 1.0));
        check(worst < 1e-9, "default picture cross-fade sums to 1", std::to_string(worst));
        // and a default sound cross-fade keeps the loudness up (Fast in + Slow out: no dip in the middle)
        Event a1 = a, a2 = a;
        a1.id = 1; a1.start = 0; a1.length = 2;
        a2.id = 2; a2.start = 1; a2.length = 2;
        tr.events = {a1, a2};
        const double mid = overlapWeight(tr, a1, 1.5) + overlapWeight(tr, a2, 1.5);
        check(mid > 1.0, "default sound cross-fade does not dip", std::to_string(mid));
    }

    // ---- a cross-fade's out-curve kept on the later event (FadeIn.ReciprocalCurve)
    {
        Project p;
        fresh(p);
        const uint64_t dc = addMedia(p, "dc:");
        const uint64_t tid = addTrack(p);
        const uint64_t ea = addEvent(p, tid, dc, 0, 2);
        const uint64_t eb = addEvent(p, tid, dc, 1, 2);
        p.findEvent(ea)->fadeOutCurve = CurveSmooth;
        p.findEvent(eb)->fadeInCurve = CurveSlow;
        const double before = at(p, 1.5);
        check(near(before, 0.5 * (0.25 + 0.5)), "no reciprocal curve: the earlier event's own fade-out curve", std::to_string(before));
        p.findEvent(eb)->fadeInRecipCurve = CurveFast;
        const double after = at(p, 1.5);
        check(near(after, 0.5 * (0.25 + 0.25)), "reciprocal curve shapes the earlier event's side", std::to_string(after));
        // saved and read back
        Project q;
        q.fromJson(p.toJson());
        check(q.findEvent(eb) && q.findEvent(eb)->fadeInRecipCurve == CurveFast && q.findEvent(ea)->fadeInRecipCurve == -1, "reciprocal curve kept in Reno's JSON");
    }

    // ---- Quickfade Audio Edits
    {
        Project p;
        fresh(p);
        const uint64_t dc = addMedia(p, "dc:");
        const uint64_t tid = addTrack(p);
        const uint64_t e1 = addEvent(p, tid, dc, 0, 1);
        addEvent(p, tid, dc, 1, 1);     // butted up against it (a split)
        check(near(at(p, 0.005), 0.5), "quick fades off (Project default): the edge is abrupt");
        p.quickFade = 0.010;
        check(near(at(p, 0.0), 0.0, 1e-3), "quick fade: silent at the very start", std::to_string(at(p, 0.0)));
        check(near(at(p, 0.005), 0.25, 1e-3), "quick fade: half way 5 ms in", std::to_string(at(p, 0.005)));
        check(near(at(p, 0.02), 0.5), "quick fade: full level after 10 ms");
        check(near(at(p, 0.995), 0.25, 1e-3) && near(at(p, 1.005), 0.25, 1e-3), "quick fades on both sides of a split (VEGAS's small dip)",
              std::to_string(at(p, 0.995)) + " " + std::to_string(at(p, 1.005)));
        // an edge with a fade of its own keeps just that fade
        p.findEvent(e1)->fadeIn = 0.5;
        check(near(at(p, 0.005), 0.5 * 0.01, 1e-4), "an event's own fade-in replaces the quick fade", std::to_string(at(p, 0.005)));
        // a cross-fade is left alone
        Project c;
        fresh(c);
        const uint64_t dc2 = addMedia(c, "dc:");
        const uint64_t t2 = addTrack(c);
        addEvent(c, t2, dc2, 0, 1.5);
        addEvent(c, t2, dc2, 1, 1.5);
        const double plain = at(c, 1.002), plainEnd = at(c, 1.498);
        c.quickFade = 0.010;
        check(near(at(c, 1.002), plain) && near(at(c, 1.498), plainEnd), "no quick fade inside a cross-fade");
        // a very short event: at most half of it
        Project s;
        fresh(s);
        const uint64_t dc3 = addMedia(s, "dc:");
        const uint64_t t3 = addTrack(s);
        addEvent(s, t3, dc3, 0, 0.004);
        s.quickFade = 0.010;
        check(near(at(s, 0.002), 0.5, 2e-2), "a 4 ms event: the quick fades take 2 ms each", std::to_string(at(s, 0.002)));
    }

    // ---- pan types
    {
        PanGains g = panGains("", 1.0, 0.5);
        check(near(g.ll, 0.5) && near(g.rr, 1.0) && g.rl == 0 && g.lr == 0, "Balance (0 dB center) is Reno's old law");
        g = panGains("balance3", 1.0, 0.0);
        check(near(g.ll, 0.70795) && near(g.rr, 0.70795), "Balance (-3 dB center): -3 dB in the middle");
        g = panGains("balance3", 1.0, 1.0);
        check(near(g.ll, 0.0) && near(g.rr, 1.0), "Balance (-3 dB center): 0 dB fully to one side");
        g = panGains("balance6", 2.0, 0.0);
        check(near(g.ll, 1.00237) && near(g.rr, 1.00237), "Balance (-6 dB center) times the level");
        g = panGains("add", 1.0, 1.0);
        check(near(g.ll, 0) && near(g.lr, 1) && near(g.rr, 1) && near(g.rl, 0), "Add Channels: fully right, both channels on the right");
        g = panGains("power", 1.0, 0.0);
        check(near(g.ll * g.ll + g.rr * g.rr, 1.0) && near(g.ll, g.rr), "Constant Power: equal power in the middle");
        g = panGains("power", 1.0, -0.4);
        check(near(g.ll * g.ll + g.rr * g.rr, 1.0) && g.ll > g.rr, "Constant Power: the same power off centre");

        Project p;
        fresh(p);
        const uint64_t lr = addMedia(p, "lr:");     // left 0.5, right 0.25
        const uint64_t tid = addTrack(p);
        addEvent(p, tid, lr, 0, 5);
        Track* t = p.findTrack(tid);
        t->pan = 0.5;
        float L = 0, R = 0;
        at(p, 1.0, &L, &R);
        check(near(L, 0.25) && near(R, 0.25), "the default pan type sounds as before");
        t->panType = "add";
        t->pan = 1.0;
        at(p, 1.0, &L, &R);
        check(near(L, 0.0) && near(R, 0.75), "Add Channels track panned right: both on the right", std::to_string(L) + " " + std::to_string(R));
        t->panType = "balance3";
        t->pan = 0.0;
        at(p, 1.0, &L, &R);
        check(near(L, 0.5 * 0.70795) && near(R, 0.25 * 0.70795), "Balance (-3 dB) track in the middle");
        // with a pan envelope (added to the slider)
        t->panType = "add";
        Envelope pe;
        pe.kind = "pan";
        EnvPoint pt;
        pt.time = 0;
        pt.value = -1.0;
        pe.points.push_back(pt);
        t->envelopes.push_back(pe);
        at(p, 1.0, &L, &R);
        check(near(L, 0.75) && near(R, 0.0), "Add Channels with a pan envelope fully left", std::to_string(L) + " " + std::to_string(R));
        t->envelopes.clear();
        // a pre-fader send takes the sound before the pan, a post-fader one after it
        const uint64_t bus = addBus(p, "Bus A");
        t->pan = 1.0;
        t->level = 1.0;
        Send sd;
        sd.bus = bus;
        sd.level = 1.0;
        sd.preFader = true;
        t->sends.push_back(sd);
        t->mute = false;
        t->output = bus;        // (all of it to the bus as well: the bus hears pan(x) + x)
        at(p, 1.0, &L, &R);
        check(near(L, 0.5) && near(R, 0.75 + 0.25), "pre-fader send unpanned, the output panned", std::to_string(L) + " " + std::to_string(R));
        t->sends[0].preFader = false;
        at(p, 1.0, &L, &R);
        check(near(L, 0.0) && near(R, 1.5), "post-fader send panned too", std::to_string(L) + " " + std::to_string(R));
        Project q;
        q.fromJson(p.toJson());
        check(q.findTrack(tid) && q.findTrack(tid)->panType == "add", "pan type kept in Reno's JSON");
    }

    // ---- the sample projects (every .veg under the samples folder): what VEGAS wrote reads as valid fade curves and pan types
    {
        const std::vector<std::string> files = testVegFiles(samples);
        if (files.empty()) std::printf("SKIP no sample projects (.veg) under %s\n", samples.c_str());
        int read = 0, badCurve = 0, badPan = 0, events = 0, recip = 0;
        for (const auto& f : files) {
            Project p;
            fresh(p);
            if (!importVegasProject(f, p).ok) continue;
            ++read;
            for (const auto& t : p.tracks) {
                if (t.type == TrackType::Audio && t.panType.empty()) ++badPan;
                for (const auto& e : t.events) {
                    ++events;
                    const int cs[] = {e.fadeInCurve, e.fadeOutCurve};
                    for (int c : cs) if (c < CurveLinear || c > CurveHold) ++badCurve;
                    // FadeIn.ReciprocalCurve: -1 = not set, else one of the curves
                    if (e.fadeInRecipCurve != -1 && (e.fadeInRecipCurve < CurveLinear || e.fadeInRecipCurve > CurveHold)) ++badCurve;
                    if (e.fadeInRecipCurve >= 0) ++recip;
                }
            }
        }
        if (read) {
            check(badCurve == 0, "samples: every fade curve (and reciprocal curve) read is a known curve",
                  std::to_string(events) + " events in " + std::to_string(read) + " projects, " + std::to_string(recip) + " with a reciprocal curve");
            check(badPan == 0, "samples: every audio track has a pan type");
        }
    }

    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
