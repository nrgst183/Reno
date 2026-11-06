// edit_split_test: the model's edit arithmetic that Split, Trim, Insert Time and TrackEvent.Split share (project.cpp:
// splitEventAt, offsetAfterStartMove, offsetAfterEndMove). Whatever an edit does, the media an event shows at a moment of
// the timeline that it still covers must not change: this checks that for plain, stretched, reversed and
// velocity-enveloped events, and that the halves of a split have VEGAS's fades, keys and ids.
//
//   edit_split_test
#include "../../src/engine/project.h"
#include <cmath>
#include <cstdio>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
}
static std::string num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.6f", v); return b; }

static Event make(Project& p, double start, double length, double offset, double rate, bool reverse, bool velocity)
{
    Event e;
    e.id = p.newId();
    e.mediaId = 1;
    e.start = start;
    e.length = length;
    e.offset = offset;
    e.rate = rate;
    e.reverse = reverse;
    e.loop = false;
    if (velocity) {
        Envelope v;
        v.kind = "velocity";
        v.points.push_back({0.0, 1.0, CurveLinear});
        v.points.push_back({length * 0.5, 2.5, CurveSmooth});
        v.points.push_back({length, 0.5, CurveLinear});
        e.envelopes.push_back(v);
    }
    return e;
}

// Media time shown at timeline time t (no media item: not wrapped or clamped).
static double shown(const Event& e, double t) { return eventMediaTime(e, nullptr, t); }

static bool sameMedia(const Event& orig, const Event& half, int samples, std::string* why)
{
    for (int k = 0; k < samples; ++k) {
        const double t = half.start + (half.length * (k + 0.5)) / samples;
        const double a = shown(orig, t), b = shown(half, t);
        if (std::fabs(a - b) > 1e-9) { *why = "at " + num(t) + ": " + num(a) + " vs " + num(b); return false; }
    }
    return true;
}

int main()
{
    for (int variant = 0; variant < 8; ++variant) {
        const bool reverse = variant & 1, velocity = variant & 2, fast = variant & 4;
        const std::string tag = std::string(reverse ? "reversed " : "") + (velocity ? "velocity " : "") + (fast ? "rate 2 " : "") + "event";
        Project p;
        Event orig = make(p, 10.0, 6.0, 3.0, fast ? 2.0 : 1.0, reverse, velocity);
        orig.fadeIn = 0.5;
        orig.fadeOut = 0.75;
        orig.fx.resize(1);
        orig.fx[0].id = p.newId();
        PanCropKey pk;
        pk.time = 4.0;
        orig.panCrop.push_back(pk);
        Event left = orig;
        Event right = splitEventAt(p, left, 12.5);
        std::string why;
        check(right.id != 0 && right.id != orig.id, tag + ": split gives the right half a new id");
        check(std::fabs(left.start - 10.0) < 1e-12 && std::fabs(left.length - 2.5) < 1e-12, tag + ": left half keeps its start and ends at the split", num(left.length));
        check(std::fabs(right.start - 12.5) < 1e-12 && std::fabs(right.length - 3.5) < 1e-12, tag + ": right half starts at the split and ends where the event did", num(right.length));
        check(sameMedia(orig, left, 40, &why), tag + ": left half plays the same media as the event did", why);
        check(sameMedia(orig, right, 40, &why), tag + ": right half plays the same media as the event did", why);
        check(left.fadeIn == 0.5 && left.fadeOut == 0 && right.fadeIn == 0 && right.fadeOut == 0.75, tag + ": fade-in stays on the left, fade-out on the right");
        check(right.fx.size() == 1 && right.fx[0].id != orig.fx[0].id, tag + ": the right half's effects have new ids");
        check(right.panCrop.size() == 1 && std::fabs(right.panCrop[0].time - (4.0 - 2.5)) < 1e-12, tag + ": Pan/Crop keys stay put on the timeline", num(right.panCrop[0].time));
        if (velocity) {
            const Envelope* v = findEnvelope(right.envelopes, "velocity");
            check(v && std::fabs(v->points[1].time - (3.0 - 2.5)) < 1e-12, tag + ": envelope points stay put on the timeline");
        }
        // outside the event: nothing happens
        Event whole = orig;
        check(splitEventAt(p, whole, 10.0).id == 0 && splitEventAt(p, whole, 16.0).id == 0 && splitEventAt(p, whole, 9.0).id == 0 &&
                  std::fabs(whole.length - 6.0) < 1e-12, tag + ": a split at or outside the ends does nothing");

        // trimming the start: the picture left stays where it was (the caller moves envelope points, as the app does)
        Event s = orig;
        const double d = 1.75;
        s.offset = offsetAfterStartMove(orig, d);
        s.start += d;
        s.length -= d;
        for (Envelope& env : s.envelopes) for (EnvPoint& pt : env.points) pt.time -= d;
        check(sameMedia(orig, s, 40, &why), tag + ": trimming the start keeps the media where it was on the timeline", why);
        // trimming the end (shorter and longer)
        for (double dl : {-2.0, 1.5}) {
            Event t = orig;
            t.offset = offsetAfterEndMove(orig, dl);
            t.length += dl;
            check(dl < 0 ? sameMedia(orig, t, 40, &why) : sameMedia(t, orig, 40, &why), tag + ": trimming the end by " + num(dl) + " keeps the media where it was", why);
        }
    }

    // a take that is itself reversed keeps its place; the others move with the event
    {
        Project p;
        Event e = make(p, 0, 4.0, 1.0, 1.0, false, false);
        Take a, b;
        a.offset = 5.0;
        b.offset = 7.0;
        b.reverse = true;
        e.takes = {a, b};
        Event r = splitEventAt(p, e, 1.0);
        check(std::fabs(r.takes[0].offset - 6.0) < 1e-12 && std::fabs(r.takes[1].offset - 7.0) < 1e-12, "takes: the right half's takes move with the media (a reversed take stays)", num(r.takes[0].offset));
    }

    // fades longer than the half they end up on are cut to it
    {
        Project p;
        Event e = make(p, 1.0, 10.0, 0.0, 1.0, false, false);
        e.fadeIn = 2.0;
        e.fadeOut = 2.0;
        Event a = e;
        Event ar = splitEventAt(p, a, 2.0);         // 1 s into the 2 s fade-in
        check(std::fabs(a.fadeIn - 1.0) < 1e-12 && a.fadeOut == 0 && ar.fadeIn == 0 && std::fabs(ar.fadeOut - 2.0) < 1e-12, "split inside the fade-in: the left half's fade-in is cut to its length", num(a.fadeIn));
        Event b = e;
        Event br = splitEventAt(p, b, 10.0);        // 1 s before the end of the 2 s fade-out
        check(std::fabs(br.fadeOut - 1.0) < 1e-12 && std::fabs(b.fadeIn - 2.0) < 1e-12 && b.fadeOut == 0, "split inside the fade-out: the right half's fade-out is cut to its length", num(br.fadeOut));
        Event c = e;
        c.length = 4.0;
        Event cr = splitEventAt(p, c, 3.0);         // fades as long as half the event: nothing to cut
        check(std::fabs(c.fadeIn - 2.0) < 1e-12 && std::fabs(cr.fadeOut - 2.0) < 1e-12, "split in the middle of an event with fades half its length keeps them");
    }

    // Looping events (VEGAS: split_audio_loop, loop_split_* in RenoEdits.cs). A 20 s file looped over a longer event: the half that
    // starts past a loop point starts at the media position modulo the file's length, and it shows the same media as before.
    {
        Project p;
        MediaItem m;
        m.id = 1;
        m.info.duration = 20.0;
        p.media.push_back(m);
        struct Case { double offset, at; double wantRight; const char* what; };
        const Case cases[] = {
            {0.0, 12.0, 12.0, "inside the first pass"},
            {0.0, 25.0, 5.0, "past the loop point (VEGAS: the right half's take starts 5 s in)"},
            {0.0, 20.0, 0.0, "exactly at the loop point"},
            {0.0, 45.0, 5.0, "past the second loop point"},
            {5.0, 10.0, 15.0, "take starting 5 s in, before the loop point"},
            {5.0, 15.0, 0.0, "take starting 5 s in, exactly at the loop point"},
            {5.0, 20.0, 5.0, "take starting 5 s in, past the loop point"},
            {5.0, 35.0, 0.0, "take starting 5 s in, at the second loop point"},
        };
        for (const Case& c : cases) {
            Event e = make(p, 2.0, 50.0, c.offset, 1.0, false, false);
            e.loop = true;
            const Event orig = e;
            Event right = splitEventAt(p, e, 2.0 + c.at);
            const MediaItem* mi = p.findMedia(1);
            check(right.id != 0 && std::fabs(right.offset - c.wantRight) < 1e-9, std::string("loop split ") + c.what, num(right.offset));
            check(std::fabs(e.offset - c.offset) < 1e-12 && std::fabs(e.length - c.at) < 1e-12, std::string("loop split ") + c.what + ": the left half keeps its offset and ends at the split");
            bool same = true;
            for (int k = 0; k < 60 && same; ++k) {
                const double t = 2.0 + 50.0 * (k + 0.37) / 60.0;
                const Event& h = t < 2.0 + c.at ? e : right;
                same = std::fabs(eventMediaTime(orig, mi, t) - eventMediaTime(h, mi, t)) < 1e-9;
            }
            check(same, std::string("loop split ") + c.what + ": both halves show the media the event showed");
        }
        // a rate: the offset is media time (VEGAS lists it divided by the rate)
        Event f = make(p, 0.0, 30.0, 0.0, 2.0, false, false);
        f.loop = true;
        const Event fo = f;
        Event fr = splitEventAt(p, f, 17.0);       // 34 s of media: 14 s into the second pass
        check(std::fabs(fr.offset - 14.0) < 1e-9 && std::fabs(eventMediaTime(fo, p.findMedia(1), 20.0) - eventMediaTime(fr, p.findMedia(1), 20.0)) < 1e-9,
              "loop split: rate 2, right half's offset wrapped in media time", num(fr.offset));
        // the same event with Loop off: nothing is wrapped (the media simply ends)
        Event n = make(p, 0.0, 30.0, 0.0, 1.0, false, false);
        n.loop = false;
        Event nr = splitEventAt(p, n, 25.0);
        check(std::fabs(nr.offset - 25.0) < 1e-12, "loop off: the right half's offset is not wrapped", num(nr.offset));
        // reversed: the left half's offset is the one that moves; it is kept inside the media too
        Event rv = make(p, 0.0, 30.0, 0.0, 1.0, true, false);
        rv.loop = true;
        const Event rvo = rv;
        Event rvr = splitEventAt(p, rv, 25.0);
        bool same = true;
        for (int k = 0; k < 60 && same; ++k) {
            const double t = 30.0 * (k + 0.37) / 60.0;
            const Event& h = t < 25.0 ? rv : rvr;
            same = std::fabs(eventMediaTime(rvo, p.findMedia(1), t) - eventMediaTime(h, p.findMedia(1), t)) < 1e-9;
        }
        check(same && rv.offset >= 0 && rv.offset < 20.0, "loop split: reversed looping event keeps its media and the offset inside the file", num(rv.offset));
    }

    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
