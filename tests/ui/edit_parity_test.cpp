// Edit commands that follow VEGAS Pro: Split with a time selection or with nothing selected, Delete / Cut / Copy of a time
// selection (across all tracks, or of the selected events only), the media a reversed event or one with a velocity envelope
// plays after a Split or a Trim, grouped events, locked events, Auto Ripple and Undo.
//
//   edit_parity_test
//
// QT_QPA_PLATFORM=offscreen. Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QSettings>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
std::string num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }

struct Ids {
    uint64_t clip = 0, snd = 0;
    uint64_t v1 = 0, a1 = 0, a2 = 0;
} ids;

uint64_t addEvent(Project& p, uint64_t track, uint64_t media, double start, double length, double offset = 0, uint64_t group = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = offset;
    e.group = group;
    e.loop = false;
    e.name = "e" + std::to_string(e.id);
    p.findTrack(track)->events.push_back(e);
    p.findTrack(track)->sortEvents();
    return e.id;
}

// V1 (picture), A1 and A2 (sound), no events
void build()
{
    st().newProject();
    std::string err;
    ids.clip = st().importMedia("synth:1", &err);
    ids.snd = st().importMedia("synthaudio:2", &err);
    st().edit("Build", [&](Project& p) {
        for (int k = 0; k < 3; ++k) {
            Track t;
            t.id = p.newId();
            t.type = k == 0 ? TrackType::Video : TrackType::Audio;
            t.name = k == 0 ? "V1" : k == 1 ? "A1" : "A2";
            p.tracks.push_back(t);
        }
        ids.v1 = p.tracks[0].id;
        ids.a1 = p.tracks[1].id;
        ids.a2 = p.tracks[2].id;
    });
    st().setSelection(std::set<uint64_t>());
    st().setCursor(0.0);
    st().setLoop(0, 0);
    st().loopOn = false;
}

const Track* trackOf(uint64_t id) { return const_cast<Project&>(st().project()).findTrack(id); }
const Event* ev(uint64_t id) { return const_cast<Project&>(st().project()).findEvent(id); }

size_t count(uint64_t track) { return trackOf(track)->events.size(); }
const Event& nth(uint64_t track, size_t k) { return trackOf(track)->events[k]; }

std::string spans(uint64_t track)
{
    std::string s;
    for (const Event& e : trackOf(track)->events) s += "[" + num(e.start) + "," + num(e.end()) + ") ";
    return s;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoEditParityTest");
    QApplication::setApplicationName("edit_parity_test");
    QSettings().clear();
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    Playback playback;
    TimelineWidget tl(&playback);
    tl.resize(1400, 520);
    tl.show();
    uitest::settle(100);

    // ---------------------------------------------------------------- Split
    {
        build();
        st().edit("Add", [&](Project& p) {
            addEvent(p, ids.v1, ids.clip, 1.0, 6.0);
            addEvent(p, ids.a1, ids.snd, 1.0, 6.0);
            addEvent(p, ids.a2, ids.snd, 8.0, 1.0);
        });
        st().setCursor(3.0);
        st().splitAtCursor();
        check(count(ids.v1) == 2 && count(ids.a1) == 2 && count(ids.a2) == 1, "Split with nothing selected splits every event under the cursor on all tracks",
              spans(ids.v1) + spans(ids.a1));

        // a locked event is left alone
        st().setSelection(std::set<uint64_t>());
        st().edit("Lock", [&](Project& p) { p.findTrack(ids.a1)->events[1].locked = true; });
        st().setCursor(5.0);
        st().splitAtCursor();
        check(count(ids.v1) == 3 && count(ids.a1) == 2, "a locked event is not split", spans(ids.v1) + spans(ids.a1));
    }
    {
        build();
        st().edit("Add", [&](Project& p) {
            addEvent(p, ids.a1, ids.snd, 1.0, 8.0);
            addEvent(p, ids.a2, ids.snd, 1.0, 8.0);
        });
        const uint64_t e1 = nth(ids.a1, 0).id;
        st().setSelection({e1});
        st().setCursor(4.0);
        st().splitAtCursor();
        check(count(ids.a1) == 2 && count(ids.a2) == 1, "Split with an event selected splits only the selected events");
    }
    {
        build();
        st().edit("Add", [&](Project& p) {
            addEvent(p, ids.a1, ids.snd, 1.0, 8.0);
            addEvent(p, ids.a2, ids.snd, 1.0, 8.0);
            addEvent(p, ids.v1, ids.clip, 5.0, 3.0);
        });
        st().setLoop(4.0, 7.0);
        st().setSelection(std::set<uint64_t>());
        st().splitAtCursor();
        check(count(ids.a1) == 3 && count(ids.a2) == 3 && count(ids.v1) == 2,
              "Split with a time selection and nothing selected splits at both ends on all tracks", spans(ids.a1) + "| " + spans(ids.v1));
        check(near(nth(ids.a1, 0).end(), 4.0) && near(nth(ids.a1, 1).start, 4.0) && near(nth(ids.a1, 1).end(), 7.0) && near(nth(ids.a1, 2).start, 7.0),
              "... the middle piece is exactly the time selection");
        check(near(nth(ids.a1, 1).offset, 3.0) && near(nth(ids.a1, 2).offset, 6.0), "... and each piece keeps its place in the media", num(nth(ids.a1, 1).offset));
        st().undo();
        check(count(ids.a1) == 1 && count(ids.v1) == 1, "Undo takes both cuts back in one step");

        // with one event selected, only that one
        st().setSelection({nth(ids.a2, 0).id});
        st().splitAtCursor();
        check(count(ids.a2) == 3 && count(ids.a1) == 1 && count(ids.v1) == 1, "Split with a time selection and an event selected splits that event at both ends");
    }
    {
        // grouped events split together when both are selected, and the new halves form their own groups
        build();
        st().edit("Add", [&](Project& p) {
            const uint64_t g = p.newId();
            addEvent(p, ids.v1, ids.clip, 1.0, 6.0, 0, g);
            addEvent(p, ids.a1, ids.snd, 1.0, 6.0, 0, g);
        });
        st().setSelection({nth(ids.v1, 0).id, nth(ids.a1, 0).id});
        st().setCursor(4.0);
        st().splitAtCursor();
        const uint64_t g1 = nth(ids.v1, 0).group, g2 = nth(ids.v1, 1).group;
        check(g1 && g2 && g1 != g2 && nth(ids.a1, 0).group == g1 && nth(ids.a1, 1).group == g2, "split halves of a group form a group each: left with left, right with right");
    }
    {
        // the media each half plays: reversed and velocity
        build();
        st().edit("Add", [&](Project& p) {
            const uint64_t r = addEvent(p, ids.a1, ids.snd, 0.0, 6.0, 2.0);
            p.findEvent(r)->reverse = true;
            const uint64_t v = addEvent(p, ids.v1, ids.clip, 0.0, 6.0, 1.0);
            Envelope vel;
            vel.kind = "velocity";
            vel.points.push_back({0.0, 1.0, CurveLinear});
            vel.points.push_back({3.0, 2.5, CurveSmooth});
            vel.points.push_back({6.0, 0.5, CurveLinear});
            p.findEvent(v)->envelopes.push_back(vel);
        });
        for (uint64_t track : {ids.a1, ids.v1}) {
            const Event whole = nth(track, 0);
            st().setSelection({whole.id});
            st().setCursor(2.5);
            st().splitAtCursor();
            const Event &l = nth(track, 0), &r = nth(track, 1);
            double worst = 0;
            for (double t = 0.05; t < 6.0; t += 0.1) {
                const Event& half = t < 2.5 ? l : r;
                worst = std::max(worst, std::fabs(eventMediaTime(whole, nullptr, t) - eventMediaTime(half, nullptr, t)));
            }
            check(worst < 1e-9, std::string("Split: both halves of the ") + (track == ids.a1 ? "reversed" : "velocity") + " event play what the event played", num(worst));
            st().undo();
        }
    }

    // ---------------------------------------------------------------- looping events (VEGAS: RenoEdits split_audio_loop, loop_*)
    {
        build();
        const double dur = st().project().findMedia(ids.snd)->info.duration;
        check(dur > 1.0, "loop: the test sound has a length", num(dur));
        // an event 2.5 files long, from the start of the file, split past the first loop point
        st().edit("Add", [&](Project& p) {
            const uint64_t a = addEvent(p, ids.a1, ids.snd, 0.0, dur * 2.5, 0.0);
            p.findEvent(a)->loop = true;
            const uint64_t b = addEvent(p, ids.a2, ids.snd, 0.0, dur * 2.5, 0.0);
            p.findEvent(b)->loop = false;
        });
        st().setSelection(std::set<uint64_t>());
        st().setCursor(dur * 1.25);
        st().splitAtCursor();
        check(count(ids.a1) == 2 && near(nth(ids.a1, 1).offset, dur * 0.25, 1e-6), "loop: Split past the loop point gives the right half a take offset inside the file",
              num(nth(ids.a1, 1).offset));
        check(count(ids.a2) == 2 && near(nth(ids.a2, 1).offset, dur * 1.25, 1e-6), "loop off: Split past the end of the file keeps the plain offset", num(nth(ids.a2, 1).offset));
        // both halves still show what the event showed
        {
            const MediaItem* m = st().project().findMedia(ids.snd);
            double worst = 0;
            Event whole;
            whole.mediaId = ids.snd; whole.start = 0; whole.length = dur * 2.5; whole.loop = true;
            for (double t = 0.05; t < dur * 2.5; t += dur / 37.0) {
                const Event& half = t < dur * 1.25 ? nth(ids.a1, 0) : nth(ids.a1, 1);
                worst = std::max(worst, std::fabs(eventMediaTime(whole, m, t) - eventMediaTime(half, m, t)));
            }
            check(worst < 1e-9, "loop: the halves of a split looping event play the same media", num(worst));
        }
        st().undo();
        check(count(ids.a1) == 1 && near(nth(ids.a1, 0).offset, 0.0), "loop: Undo takes the split back");

        // Trim Start past the first loop point wraps the offset, with the event's media where it was
        const double before = st().project().findEvent(nth(ids.a1, 0).id)->offset;
        (void)before;
        st().setSelection({nth(ids.a1, 0).id});
        st().setCursor(dur * 1.5);
        tl.trimStartToCursor();
        check(count(ids.a1) == 1 && near(nth(ids.a1, 0).start, dur * 1.5, 1e-6) && near(nth(ids.a1, 0).offset, dur * 0.5, 1e-6),
              "loop: Trim Start past the loop point leaves a take offset inside the file", num(nth(ids.a1, 0).offset));
    }

    // ---------------------------------------------------------------- Delete / Cut / Copy with a time selection
    {
        build();
        st().edit("Add", [&](Project& p) {
            addEvent(p, ids.v1, ids.clip, 2.0, 8.0);
            addEvent(p, ids.a1, ids.snd, 3.0, 6.0);
            addEvent(p, ids.a2, ids.snd, 5.0, 1.0);
            addEvent(p, ids.a2, ids.snd, 8.0, 1.5);
        });
        st().setSelection(std::set<uint64_t>());
        st().setLoop(4.0, 7.0);
        tl.deleteSelection();
        check(count(ids.v1) == 2 && near(nth(ids.v1, 0).end(), 4.0) && near(nth(ids.v1, 1).start, 7.0), "Delete with a time selection takes that time out of the events on every track",
              spans(ids.v1));
        check(count(ids.a1) == 2 && count(ids.a2) == 1 && near(nth(ids.a2, 0).start, 8.0), "... an event wholly inside it goes, one outside it stays", spans(ids.a1) + "| " + spans(ids.a2));
        check(near(nth(ids.v1, 1).offset, 5.0 * 1.0 + 0.0 - 0.0 + 0.0, 1e-6), "... the piece after it still plays the media it did", num(nth(ids.v1, 1).offset));
        st().undo();
        check(count(ids.v1) == 1 && count(ids.a2) == 2, "Undo brings everything back in one step");

        // Auto Ripple closes the gap on every track the edit touched
        tl.options().autoRipple = true;
        tl.options().ripple = RippleMode::AllTracks;
        tl.deleteSelection();
        check(count(ids.v1) == 2 && near(nth(ids.v1, 1).start, 4.0), "with Auto Ripple the rest moves back over the gap", spans(ids.v1));
        tl.options().autoRipple = false;
        st().undo();
    }
    {
        build();
        st().edit("Add", [&](Project& p) {
            addEvent(p, ids.a1, ids.snd, 1.0, 8.0);
            addEvent(p, ids.a2, ids.snd, 1.0, 8.0);
        });
        // selected event only
        st().setSelection({nth(ids.a1, 0).id});
        st().setLoop(3.0, 5.0);
        tl.deleteSelection();
        check(count(ids.a1) == 2 && count(ids.a2) == 1 && near(nth(ids.a1, 0).end(), 3.0) && near(nth(ids.a1, 1).start, 5.0),
              "Delete with an event selected and a time selection takes the time out of that event only", spans(ids.a1) + "| " + spans(ids.a2));
        // a selected event the time selection does not touch goes whole
        st().setSelection({nth(ids.a2, 0).id});
        st().setLoop(30.0, 31.0);
        tl.deleteSelection();
        check(count(ids.a2) == 0, "a selected event outside the time selection is deleted whole");
    }
    {
        build();
        st().edit("Add", [&](Project& p) {
            addEvent(p, ids.a1, ids.snd, 1.0, 8.0, 0.5);
            addEvent(p, ids.a2, ids.snd, 2.0, 2.0);
        });
        st().setSelection(std::set<uint64_t>());
        st().setLoop(3.0, 6.0);
        tl.cutSelection();
        check(count(ids.a1) == 2 && st().hasClipboard(), "Cut with a time selection takes the time out and keeps it on the clipboard", spans(ids.a1));
        st().setLoop(0, 0);
        st().setCursor(12.0);
        st().currentTrack = ids.a1;
        tl.paste();
        const Track* t = trackOf(ids.a1);
        bool found = false;
        for (const Event& e : t->events)
            if (near(e.start, 12.0) && near(e.length, 3.0) && near(e.offset, 3.0 - 1.0 + 0.5)) found = true;
        check(found, "... Paste puts back the piece that was cut (3 s from its place in the media)", spans(ids.a1));
        // Copy leaves the events whole
        st().setSelection(std::set<uint64_t>());
        st().setLoop(1.5, 2.5);
        const size_t before = count(ids.a1);
        st().copySelection();
        check(count(ids.a1) == before && st().hasClipboard(), "Copy with a time selection changes nothing");
        st().setLoop(0, 0);
        st().setCursor(20.0);
        st().currentTrack = ids.a1;
        st().pasteAtCursor();
        bool piece = false;
        for (const Event& e : trackOf(ids.a1)->events)
            if (near(e.start, 20.0) && near(e.length, 1.0)) piece = true;
        check(piece, "... and what it copied is the 1 s inside the time selection", spans(ids.a1));
    }
    {
        // locked events are left alone; a group is cut at the same place
        build();
        st().edit("Add", [&](Project& p) {
            addEvent(p, ids.a1, ids.snd, 1.0, 8.0);
            addEvent(p, ids.a2, ids.snd, 1.0, 8.0);
            p.findTrack(ids.a2)->events[0].locked = true;
        });
        st().setSelection(std::set<uint64_t>());
        st().setLoop(3.0, 5.0);
        tl.deleteSelection();
        check(count(ids.a1) == 2 && count(ids.a2) == 1 && near(nth(ids.a2, 0).length, 8.0), "a locked event is not touched by Delete with a time selection");
    }
    {
        // plain Delete / Cut of selected events with no time selection is as before
        build();
        st().edit("Add", [&](Project& p) { addEvent(p, ids.a1, ids.snd, 1.0, 8.0); });
        st().setLoop(0, 0);
        st().setSelection({nth(ids.a1, 0).id});
        tl.deleteSelection();
        check(count(ids.a1) == 0, "Delete without a time selection removes the selected events");
    }

    // ---------------------------------------------------------------- Trim of reversed events
    {
        build();
        uint64_t rev = 0;
        st().edit("Add", [&](Project& p) {
            rev = addEvent(p, ids.a1, ids.snd, 2.0, 6.0, 1.0);
            p.findEvent(rev)->reverse = true;
        });
        const Event whole = *ev(rev);
        st().setSelection({rev});
        st().setCursor(4.0);
        tl.trimStartToCursor();
        double worst = 0;
        for (double t = 4.05; t < 8.0; t += 0.1) worst = std::max(worst, std::fabs(eventMediaTime(whole, nullptr, t) - eventMediaTime(*ev(rev), nullptr, t)));
        check(near(ev(rev)->start, 4.0) && worst < 1e-9, "Trim Start of a reversed event keeps the media where it was on the timeline", num(worst));
        st().setCursor(6.0);
        tl.trimEndToCursor();
        worst = 0;
        for (double t = 4.05; t < 6.0; t += 0.1) worst = std::max(worst, std::fabs(eventMediaTime(whole, nullptr, t) - eventMediaTime(*ev(rev), nullptr, t)));
        check(near(ev(rev)->end(), 6.0) && worst < 1e-9, "Trim End of a reversed event keeps the media where it was on the timeline", num(worst));
    }

    return uitest::summary();
}
