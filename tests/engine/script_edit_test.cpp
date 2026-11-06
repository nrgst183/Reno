// script_edit_test: the editing calls of the script engine against what VEGAS Pro 18 did in tools/vegas/RenoEdits.cs (the rules
// were read from its results, see docs/NOTES.md "Editing against VEGAS"): TrackEvent.Split outside an event or on a locked one,
// AdjustStartLength on a looping event and with fades, AdjustPlaybackRate and the rate setter, Split of looping events and fades.
//
//   script_edit_test
#include "../../src/engine/scriptsession.h"
#include "../../src/engine/scriptapi.h"
#include <cmath>
#include <cstdio>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
}
static std::string num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

struct Rig {
    Project p;
    ScriptHost host;
    uint64_t track = 0, media = 0;
    Rig()
    {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        track = t.id;
        p.tracks.push_back(t);
        MediaItem m;
        m.id = p.newId();
        m.path = "/x/tone.wav";
        m.info.duration = 20.0;
        m.info.hasAudio = true;
        media = m.id;
        p.media.push_back(m);
    }
    uint64_t add(double start, double length, double offset, double rate = 1.0, bool loop = true)
    {
        Event e;
        e.id = p.newId();
        e.mediaId = media;
        e.start = start;
        e.length = length;
        e.offset = offset;
        e.rate = rate;
        e.loop = loop;
        p.findTrack(track)->events.push_back(e);
        p.findTrack(track)->sortEvents();
        return e.id;
    }
};

static std::vector<uint64_t> events(ScriptSession& s, uint64_t track)
{
    std::vector<uint64_t> tr, ev;
    s.list(1, RL_TRACKS, &tr);
    for (uint64_t t : tr) { (void)t; }
    s.list(tr.empty() ? 0 : tr[0], RL_EVENTS, &ev);
    (void)track;
    return ev;
}

static int op(ScriptSession& s, uint64_t h, int code, const std::vector<int64_t>& ia, const std::vector<double>& da, uint64_t* out = nullptr)
{
    uint64_t o = 0;
    double d = 0;
    const int rc = s.call(h, code, ia, da, std::string(), &o, &d);
    if (out) *out = o;
    return rc;
}

int main()
{
    // Split: outside, at the ends, on a locked event
    {
        Rig r;
        r.add(2, 6, 1);
        r.add(10, 6, 1);
        r.p.findTrack(r.track)->events[1].locked = true;
        ScriptSession s(r.p, r.host);
        const std::vector<uint64_t> ev = events(s, r.track);
        uint64_t out = 7;
        int rc = op(s, ev[0], RO_SPLIT, {}, {0.0}, &out);
        check(rc == RS_OK && out == 0 && s.project.findTrack(r.track)->events.size() == 2, "Split at the start of an event: no error, nothing split");
        rc = op(s, ev[0], RO_SPLIT, {}, {6.0}, &out);
        check(rc == RS_OK && out == 0, "Split at the end of an event: no error");
        rc = op(s, ev[0], RO_SPLIT, {}, {7.0}, &out);
        check(rc == RS_OK && out == 0, "Split past the end of an event: no error");
        rc = op(s, ev[0], RO_SPLIT, {}, {-1.0}, &out);
        check(rc == RS_OK && out == 0 && s.project.findTrack(r.track)->events.size() == 2, "Split before the start of an event: no error");
        rc = op(s, ev[1], RO_SPLIT, {}, {3.0}, &out);
        check(rc == RS_OK && out != 0 && s.project.findTrack(r.track)->events.size() == 3, "Split of a locked event works");
    }
    // Split of a looping event: the take offset of the right half stays inside the media
    {
        Rig r;
        r.add(0, 30, 0);
        r.add(40, 30, 5);
        ScriptSession s(r.p, r.host);
        const std::vector<uint64_t> ev = events(s, r.track);
        op(s, ev[0], RO_SPLIT, {}, {25.0});
        op(s, ev[1], RO_SPLIT, {}, {10.0});
        const auto& evs = s.project.findTrack(r.track)->events;
        check(evs.size() == 4 && near(evs[1].start, 25.0) && near(evs[1].offset, 5.0), "Script Split of a looping event: right half's offset is 5 s (VEGAS split_audio_loop)", num(evs[1].offset));
        check(near(evs[3].start, 50.0) && near(evs[3].offset, 15.0), "... and 15 s when the split is before the loop point of a take that starts at 5 s", num(evs[3].offset));
    }
    // AdjustStartLength: extending the start of a looping event before the media wraps the offset; fades stay inside
    {
        Rig r;
        r.add(4, 8, 0);
        r.add(4, 8, 5, 2.0);
        const uint64_t f = r.add(4, 8, 5);
        r.p.findEvent(f)->fadeIn = 3.0;
        r.p.findEvent(f)->fadeOut = 3.0;
        ScriptSession s(r.p, r.host);
        const std::vector<uint64_t> ev = events(s, r.track);
        op(s, ev[0], RO_ADJUST_START_LENGTH, {1}, {2.0, 10.0});
        op(s, ev[1], RO_ADJUST_START_LENGTH, {1}, {6.0, 6.0});
        op(s, ev[2], RO_ADJUST_START_LENGTH, {1}, {7.0, 3.0});
        const auto& evs = s.project.findTrack(r.track)->events;
        check(near(evs[0].offset, 18.0), "AdjustStartLength 2 s before the start of a looping event: offset 18 s (VEGAS adjust_audio)", num(evs[0].offset));
        // rate 2, offset 5 (media) trimmed 2 s later: 4 s of media more = 9 media seconds
        check(near(evs[1].offset, 9.0), "... and with a playback rate in media time", num(evs[1].offset));
        check(near(evs[2].fadeIn, 3.0) && near(evs[2].fadeOut, 0.0), "AdjustStartLength to the length of the fade-in leaves no fade-out", num(evs[2].fadeOut));
    }
    // playback rate: start and length stay, the take offset stays in timeline time unless adjustStart
    {
        Rig r;
        r.add(2, 8, 1, 1.0, false);
        r.add(2, 8, 1, 1.0, false);
        r.add(2, 8, 1, 1.0, false);
        ScriptSession s(r.p, r.host);
        const std::vector<uint64_t> ev = events(s, r.track);
        op(s, ev[0], RO_ADJUST_RATE, {0}, {2.0});
        op(s, ev[1], RO_ADJUST_RATE, {1}, {2.0});
        s.setD(ev[2], RP_RATE, 0.5);
        const auto& evs = s.project.findTrack(r.track)->events;
        check(near(evs[0].length, 8.0) && near(evs[0].start, 2.0) && near(evs[0].offset / evs[0].rate, 1.0), "AdjustPlaybackRate(2, false): same start and length, same offset in timeline time", num(evs[0].offset));
        check(near(evs[1].length, 8.0) && near(evs[1].start, 2.0) && near(evs[1].offset, 1.0) && near(evs[1].offset / evs[1].rate, 0.5), "AdjustPlaybackRate(2, true): same place in the media", num(evs[1].offset));
        check(near(evs[2].rate, 0.5) && near(evs[2].offset / evs[2].rate, 1.0) && near(evs[2].length, 8.0), "the rate setter keeps the offset in timeline time", num(evs[2].offset));
    }
    // Split inside a fade
    {
        Rig r;
        const uint64_t a = r.add(1, 10, 1);
        r.p.findEvent(a)->fadeIn = 2.0;
        r.p.findEvent(a)->fadeOut = 2.0;
        ScriptSession s(r.p, r.host);
        const std::vector<uint64_t> ev = events(s, r.track);
        op(s, ev[0], RO_SPLIT, {}, {1.0});
        const auto& evs = s.project.findTrack(r.track)->events;
        check(evs.size() == 2 && near(evs[0].fadeIn, 1.0) && near(evs[1].fadeOut, 2.0), "Script Split 1 s into a 2 s fade-in: the left half's fade-in is 1 s", num(evs[0].fadeIn));
    }
    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
