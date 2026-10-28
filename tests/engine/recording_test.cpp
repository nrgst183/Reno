// Tests of the recording core: a Record Input for each armed track (device, channels, mono or stereo, input busses),
// sharing the inputs' sound out to files, planning where recording starts and stops, and what a finished recording does
// (a new event, a take of the selected events, a punch-in with pre-roll and post-roll, a take per loop pass).
//
//   recording_test
#include "../../src/engine/recording.h"
#include <cstdio>
#include <cmath>
#include <map>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}
static bool near(double a, double b, double e = 1e-9) { return std::fabs(a - b) < e; }

static Track& addTrack(Project& p)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    p.tracks.push_back(t);
    return p.tracks.back();
}
static Event& addEvent(Project& p, Track& t, double start, double length, uint64_t media = 1, double offset = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = offset;
    e.name = "old";
    t.events.push_back(e);
    t.sortEvents();
    for (Event& x : t.events)
        if (x.id == e.id) return x;
    return t.events[0];
}

int main()
{
    // ---- sources
    Project p;
    const uint64_t ida = addTrack(p).id, idb = addTrack(p).id, idc = addTrack(p).id, idd = addTrack(p).id;
    p.findTrack(idb)->recordInput.device = "usb";
    p.findTrack(idb)->recordInput.first = 2;
    p.findTrack(idb)->recordInput.channels = 1;
    p.findTrack(idc)->recordInput.device = "usb";
    p.findTrack(idc)->recordInput.first = 2;
    p.findTrack(idc)->recordInput.channels = 1;
    InputBus ib;
    ib.id = p.newId();
    p.inputBuses.push_back(ib);
    p.findTrack(idd)->recordInput.inputBus = ib.id;
    std::set<uint64_t> armed = {ida, idb, idc, idd};
    std::vector<RecordSource> src = planRecordSources(p, armed);
    check(src.size() == 3 && src[0].tracks.size() == 1 && src[1].tracks.size() == 2 && src[1].channels() == 1 && src[2].input.inputBus == ib.id && src[2].channels() == 2,
          "sources: tracks with the same input share a file, the others each have their own", std::to_string(src.size()));
    armed = {idb};
    src = planRecordSources(p, armed);
    check(src.size() == 1 && src[0].tracks[0] == idb, "sources: only armed tracks");
    p.inputBuses.clear();
    armed = {idd};
    src = planRecordSources(p, armed);
    check(src.size() == 1 && src[0].input.inputBus == 0, "sources: an input bus that is gone records from the default input");

    // ---- the router
    p.inputBuses.push_back(ib);
    armed = {ida, idb, idd};
    src = planRecordSources(p, armed);
    RecordRouter router;
    std::map<int, std::vector<float>> gotL, gotR;
    std::map<int, int> hasR;
    router.setup(src, [&](int s, const float* l, const float* r, int n) {
        gotL[s].insert(gotL[s].end(), l, l + n);
        if (r) gotR[s].insert(gotR[s].end(), r, r + n);
        hasR[s] = r != nullptr;
    });
    std::vector<std::pair<std::string, int>> devs = router.devices();
    check(devs.size() == 2 && ((devs[0].first == "" && devs[0].second == 2 && devs[1].first == "usb" && devs[1].second == 3) ||
                               (devs[1].first == "" && devs[1].second == 2 && devs[0].first == "usb" && devs[0].second == 3)),
          "router: the devices to open and the channels each must give");
    // default device: 2 channels 1,2; usb: 4 channels, track b takes channel 3 alone
    std::vector<float> two(8), four(16);
    for (int k = 0; k < 4; ++k) {
        two[(size_t)k * 2] = 0.1f * (k + 1);
        two[(size_t)k * 2 + 1] = -0.1f * (k + 1);
        for (int ch = 0; ch < 4; ++ch) four[(size_t)k * 4 + ch] = (float)(ch + 1) + 0.01f * k;
    }
    router.pushDevice("", two.data(), 2, 4);
    router.pushDevice("usb", four.data(), 4, 4);
    router.pushDevice("elsewhere", four.data(), 4, 4);
    check(gotL.count(0) && gotR.count(0) && near(gotL[0][1], 0.2, 1e-6) && near(gotR[0][1], -0.2, 1e-6) && hasR[0], "router: a stereo source gets two channels");
    int usb = -1;
    for (size_t i = 0; i < src.size(); ++i) if (src[i].input.device == "usb") usb = (int)i;
    check(usb >= 0 && gotL[usb].size() == 4 && !hasR[usb] && near(gotL[usb][0], 3.0, 1e-6) && near(gotL[usb][2], 3.02, 1e-6), "router: a mono source gets its channel alone",
          usb >= 0 && !gotL[usb].empty() ? std::to_string(gotL[usb][0]) : "none");
    check(gotL.size() == 2, "router: a device nobody records from is ignored");
    // a source asking for channels the device lacks
    {
        Project q;
        Track& t = addTrack(q);
        t.recordInput.first = 1;
        t.recordInput.channels = 2;
        RecordRouter r2;
        std::vector<float> l, rr;
        r2.setup(planRecordSources(q, {t.id}), [&](int, const float* x, const float* y, int n) { l.assign(x, x + n); rr.assign(y, y + n); });
        std::vector<float> mono = {0.5f, 0.6f};
        r2.pushDevice("", mono.data(), 1, 2);
        check(l.size() == 2 && l[0] == 0.f && rr[0] == 0.f, "router: channels the device lacks are silent");
    }
    router.pushInputBus(ib.id, two.data(), two.data() + 1, 2);
    int busSrc = -1;
    for (size_t i = 0; i < src.size(); ++i) if (src[i].input.inputBus) busSrc = (int)i;
    check(busSrc >= 0 && gotL[busSrc].size() == 2, "router: an input bus's sound goes to the tracks that record it");

    // ---- where recording starts and stops
    {
        Project q;
        Track& t = addTrack(q);
        Event& e = addEvent(q, t, 4, 2);
        const uint64_t eid = e.id, tid = t.id;
        Track& other = addTrack(q);
        addEvent(q, other, 1, 1);
        RecordSpan s = planRecordSpan(q, {tid}, {}, 1.5, 0, 0);
        check(near(s.start, 1.5) && s.stop <= s.start && !s.intoEvents, "span: nothing selected: from the cursor on until stopped");
        s = planRecordSpan(q, {tid}, {eid}, 4, 0, 0);
        check(near(s.start, 4) && near(s.stop, 6) && s.intoEvents, "span: a selected event: its length");
        s = planRecordSpan(q, {tid}, {eid}, 3, 0, 0);
        check(near(s.start, 3) && near(s.stop, 6), "span: the cursor before the event: a pre-roll");
        s = planRecordSpan(q, {tid}, {eid}, 5, 0, 0);
        check(near(s.start, 4), "span: the cursor inside it: from the event's start");
        s = planRecordSpan(q, {tid}, {eid}, 3, 2, 8);
        check(near(s.start, 3) && near(s.stop, 8) && s.intoEvents, "span: with a time selection the pre-roll and post-roll are its edges");
        s = planRecordSpan(q, {tid}, {}, 0, 2, 8);
        check(near(s.start, 2) && near(s.stop, 8) && !s.intoEvents, "span: a time selection alone: inside it");
        s = planRecordSpan(q, {other.id}, {eid}, 4, 0, 0);
        check(!s.intoEvents, "span: events on tracks that are not armed are not recorded into");
    }

    // ---- a new event
    {
        Project q;
        Track& t = addTrack(q);
        addEvent(q, t, 0, 10, 7);
        const uint64_t tid = t.id;
        RecordCommit rc;
        rc.start = 2;
        rc.lag = 0.25;
        RecordCommit::Item it;
        it.track = tid;
        it.mediaId = 9;
        it.length = 3.25;
        it.name = "Rec";
        rc.items.push_back(it);
        std::vector<uint64_t> made = commitRecording(q, rc);
        const Track* tt = q.findTrack(tid);
        const Event* ne = made.size() == 1 ? q.findEvent(made[0]) : nullptr;
        check(ne && near(ne->start, 2) && near(ne->length, 3.0) && near(ne->offset, 0.25) && ne->mediaId == 9 && tt->events.size() == 3,
              "commit: a new event where recording began, the input's lag skipped, cutting back what it lay over");
        bool before = false, after = false;
        for (const Event& e : tt->events) {
            if (e.mediaId == 7 && near(e.start, 0) && near(e.length, 2)) before = true;
            if (e.mediaId == 7 && near(e.start, 5) && near(e.end(), 10) && near(e.offset, 5)) after = true;
        }
        check(before && after, "commit: the old event is cut before and after it");
    }
    // ---- a take of the selected event
    {
        Project q;
        Track& t = addTrack(q);
        Event& e = addEvent(q, t, 4, 2, 7, 1.0);
        const uint64_t tid = t.id, eid = e.id;
        RecordCommit rc;
        rc.start = 4;
        RecordCommit::Item it;
        it.track = tid;
        it.mediaId = 9;
        it.length = 2.0;
        it.name = "Take";
        it.events = {eid};
        rc.items.push_back(it);
        commitRecording(q, rc);
        const Event* ev = q.findEvent(eid);
        check(ev && ev->mediaId == 9 && near(ev->offset, 0) && ev->takes.size() == 1 && ev->takes[0].mediaId == 7 && ev->activeTake == 1 && near(ev->start, 4) && near(ev->length, 2),
              "commit: a selected event gets the recording as a new take and it is the active one");
        // a second one on top
        rc.items[0].mediaId = 10;
        commitRecording(q, rc);
        ev = q.findEvent(eid);
        check(ev && ev->mediaId == 10 && ev->takes.size() == 2 && ev->activeTake == 2, "commit: another recording is a third take");
    }
    // ---- punch-in: recording from 2 to 8 under an event at 4..6: the take starts 2 s into the file
    {
        Project q;
        Track& t = addTrack(q);
        const uint64_t tid = t.id, eid = addEvent(q, t, 4, 2, 7, 1.0).id;
        const uint64_t eid2 = addEvent(q, q.tracks[0], 10, 1, 7, 0).id;
        RecordCommit rc;
        rc.start = 2;
        rc.lag = 0.5;
        RecordCommit::Item it;
        it.track = tid;
        it.mediaId = 9;
        it.length = 6.5;        // 6 seconds recorded after a half second of lag
        it.name = "Punch";
        it.events = {eid, eid2};
        rc.items.push_back(it);
        commitRecording(q, rc);
        const Event* ev = q.findEvent(eid);
        const Event* ev2 = q.findEvent(eid2);
        check(ev && ev->mediaId == 9 && near(ev->offset, 2.5) && near(ev->length, 2) && ev->takes.size() == 1,
              "commit: punch-in: the take begins where the event does, the pre-roll before it in the file (the event can be pulled out to uncover it)", ev ? std::to_string(ev->offset) : "none");
        check(ev2 && ev2->mediaId == 7 && ev2->takes.empty(), "commit: a selected event the recording did not reach is left alone");
    }
    // ---- loop recording
    {
        Project q;
        Track& t = addTrack(q);
        Event& e = addEvent(q, t, 4, 2, 7, 0);
        const uint64_t tid = t.id, eid = e.id;
        RecordCommit rc;
        rc.start = 4;
        rc.loop = true;
        rc.loopStart = 4;
        rc.loopEnd = 6;
        RecordCommit::Item it;
        it.track = tid;
        it.mediaId = 9;
        it.length = 6.5;       // three passes and half of a fourth
        it.name = "Loop";
        it.events = {eid};
        rc.items.push_back(it);
        commitRecording(q, rc);
        const Event* ev = q.findEvent(eid);
        check(ev && ev->takes.size() == 4 && ev->activeTake == 4 && near(ev->offset, 6.0) && ev->mediaId == 9 && ev->takes[0].mediaId == 7 &&
                  near(ev->takes[1].offset, 0) && near(ev->takes[2].offset, 2) && near(ev->takes[3].offset, 4),
              "commit: loop recording: a take per pass, one file, the last pass active",
              ev ? std::to_string(ev->takes.size()) + " takes, active " + std::to_string(ev->activeTake) : "none");
        // loop recording into an empty track: an event over the loop with a take per pass
        Project r;
        Track& t2 = addTrack(r);
        RecordCommit rc2 = rc;
        rc2.items[0].track = t2.id;
        rc2.items[0].events.clear();
        rc2.start = 3;           // started a second before the loop
        rc2.items[0].length = 5.0;
        std::vector<uint64_t> made = commitRecording(r, rc2);
        const Event* ne = made.size() == 1 ? r.findEvent(made[0]) : nullptr;
        check(ne && near(ne->start, 4) && near(ne->length, 2) && ne->takes.size() == 1 && ne->activeTake == 1 && near(ne->offset, 3.0) && near(ne->takes[0].offset, 1.0),
              "commit: loop recording on an empty track: an event over the loop, a take per pass",
              ne ? std::to_string(ne->offset) + " " + std::to_string(ne->takes.size()) : "none");
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
