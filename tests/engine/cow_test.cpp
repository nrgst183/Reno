// cow_test: a Track's events are shared between copies of a project until one changes them (CowVec), and Project::findEvent
// finds events through an index. Checks the sharing, the independence of copies after a change, and that findEvent gives
// the same answer as looking through the tracks - after moves, inserts, erases, sorts and replaced events.
//   cow_test
#include "../../src/engine/project.h"
#include <algorithm>
#include <cstdio>
#include <random>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
}

static Project make(int tracks, int perTrack)
{
    Project p;
    for (int t = 0; t < tracks; ++t) {
        Track tr;
        tr.id = p.newId();
        tr.type = t % 2 ? TrackType::Audio : TrackType::Video;
        for (int k = 0; k < perTrack; ++k) {
            Event e;
            e.id = p.newId();
            e.start = k * 1.5;
            e.length = 1.0;
            tr.events.push_back(e);
        }
        p.tracks.push_back(tr);
    }
    return p;
}

// findEvent against a plain search
static bool agrees(const Project& p, uint64_t id)
{
    const Event* want = nullptr;
    const Track* wantTrack = nullptr;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events.cvec())
            if (e.id == id && !want) { want = &e; wantTrack = &t; }
    const Track* gotTrack = nullptr;
    const Event* got = p.findEvent(id, &gotTrack);
    return got == want && gotTrack == wantTrack;
}

int main()
{
    // ---- sharing
    {
        Project a = make(4, 300);
        Project b = a;
        check(a.tracks[0].events.sameContents(b.tracks[0].events), "a copy shares the events");
        check(a.tracks[0].events.shared(), "shared() says so");
        const Event* before = &static_cast<const Project&>(a).tracks[0].events[5];
        // reading through the const side does not unshare
        double sum = 0;
        for (const Track& t : static_cast<const Project&>(b).tracks) for (const Event& e : t.events) sum += e.start;
        check(a.tracks[0].events.sameContents(b.tracks[0].events) && sum > 0, "reading does not unshare");
        // changing one event unshares only its track
        a.findEvent(a.tracks[2].events[10].id)->gain = 0.25;
        check(!a.tracks[2].events.sameContents(b.tracks[2].events), "the changed track is its own");
        check(a.tracks[0].events.sameContents(b.tracks[0].events) && a.tracks[1].events.sameContents(b.tracks[1].events) &&
              a.tracks[3].events.sameContents(b.tracks[3].events), "the other tracks are still shared");
        check(b.tracks[2].events[10].gain == 1.0 && a.tracks[2].events[10].gain == 0.25, "the copy is not changed");
        (void)before;
        // push_back, erase, insert on one copy leave the other alone
        Project c = a;
        c.tracks[1].events.push_back(Event());
        c.tracks[0].events.erase(c.tracks[0].events.begin());
        c.tracks[3].events.insert(c.tracks[3].events.begin() + 2, Event());
        check(a.tracks[1].events.size() == 300 && a.tracks[0].events.size() == 300 && a.tracks[3].events.size() == 300, "edits of a copy leave the original");
        check(c.tracks[1].events.size() == 301 && c.tracks[0].events.size() == 299 && c.tracks[3].events.size() == 301, "the copy took the edits");
        // forEachFx on a const project does not unshare
        const Project& cb = b;
        int n = 0;
        cb.forEachFx([&](const FxSlot&) { ++n; });
        check(a.tracks[0].events.sameContents(b.tracks[0].events), "const forEachFx does not unshare");
        // non-const forEachFx: tracks with no effect on their events stay shared
        Project d = a;
        d.tracks[1].events.mut()[3].fx.push_back(FxSlot());
        d.tracks[1].events.mut()[3].fx[0].id = 999999;
        Project e = d;
        int seen = 0;
        e.forEachFx([&](FxSlot&) { ++seen; });
        check(seen == 1 && e.tracks[0].events.sameContents(d.tracks[0].events) && e.tracks[1].events.sameContents(d.tracks[1].events) == false, "forEachFx unshares only the track with an effect",
              strfmt("seen %d", seen));
        check(e.findFx(999999) != nullptr, "findFx finds it");
    }
    // ---- findEvent
    {
        Project p = make(6, 400);
        std::mt19937 rng(5);
        std::vector<uint64_t> ids;
        for (const Track& t : p.tracks) for (const Event& e : t.events.cvec()) ids.push_back(e.id);
        bool all = true;
        for (int k = 0; k < 200; ++k) all = all && agrees(p, ids[rng() % ids.size()]);
        check(all, "findEvent: every id found where a plain search finds it");
        check(p.findEvent(123456789) == nullptr && agrees(p, 123456789), "findEvent: an id that is not there");
        // an event moved to another track
        Event moved = p.tracks[0].events[7];
        p.tracks[0].events.erase(p.tracks[0].events.begin() + 7);
        p.tracks[3].events.push_back(moved);
        bool ok = agrees(p, moved.id);
        for (int k = 0; k < 50; ++k) ok = ok && agrees(p, ids[rng() % ids.size()]);
        check(ok, "findEvent after an event moved to another track");
        // sorted differently (start times changed, then sorted)
        for (Event& e : p.tracks[2].events) e.start = -e.start;
        p.tracks[2].sortEvents();
        ok = true;
        for (const Event& e : p.tracks[2].events.cvec()) ok = ok && agrees(p, e.id);
        check(ok, "findEvent after a track was sorted");
        // an event replaced in place by another with a new id (same count)
        Event fresh = p.tracks[1].events[9];
        const uint64_t oldId = fresh.id;
        fresh.id = 777777;
        p.tracks[1].events[9] = fresh;
        check(p.findEvent(777777) != nullptr && agrees(p, 777777) && p.findEvent(oldId) == nullptr && agrees(p, oldId), "findEvent after an event was replaced in place");
        // inserted events
        for (int k = 0; k < 30; ++k) { Event e; e.id = 900000 + (uint64_t)k; e.start = k; p.tracks[4].events.push_back(e); }
        ok = true;
        for (int k = 0; k < 30; ++k) ok = ok && p.findEvent(900000 + (uint64_t)k) && agrees(p, 900000 + (uint64_t)k);
        check(ok, "findEvent finds events that were added");
        // a copy and the original answer for their own contents
        Project q = p;
        q.tracks[4].events.erase(q.tracks[4].events.begin() + 400, q.tracks[4].events.begin() + 410);
        ok = !q.findEvent(900000) && p.findEvent(900000) && agrees(q, 900015) && agrees(p, 900000);
        check(ok, "findEvent in a copy and in its original");
        // the pointer is into the project's own events (changing it changes this project only)
        Project r = p;
        r.findEvent(900020)->gain = 0.125;
        check(p.findEvent(900020)->gain == 1.0 && r.findEvent(900020)->gain == 0.125, "a change through findEvent reaches only its own project");
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
