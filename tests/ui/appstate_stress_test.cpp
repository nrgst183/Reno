// Editing commands of AppState on a stutter project (thousands of tiny events cut from one clip, as an editor makes by
// cutting a clip into a rhythm): each command must give the right result and stay fast. Prints timings.
//
//   appstate_stress_test <output folder>
#include "../../src/app/appstate.h"
#include "../../src/app/timeline_internal.h"
#include "uitestlib.h"
#include <QApplication>
#include <chrono>
#include <cstdio>
#include <functional>
#include <set>

using namespace reno;
using namespace reno::app;
using uitest::check;

static double nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static const Track* trackOf(uint64_t id)
{
    for (const Track& t : AppState::get().project().tracks) if (t.id == id) return &t;
    return nullptr;
}

// 'n' consecutive pieces of one clip on the first video track, each 'len' long, the next one carrying on where the last stopped
static void build(int n, double len, uint64_t* trackOut, uint64_t* mediaOut)
{
    AppState& st = AppState::get();
    st.newProject();
    std::string err;
    const uint64_t media = st.importMedia("synth:1", &err);
    const uint64_t tid = st.addTrack(TrackType::Video);
    st.edit("stutter", [&](Project& p) {
        Track* t = p.findTrack(tid);
        *trackOut = tid;
        for (int k = 0; k < n; ++k) {
            Event e;
            e.id = p.newId();
            e.mediaId = media;
            e.start = k * len;
            e.length = len;
            e.offset = k * len;
            e.name = "piece";
            t->events.push_back(e);
        }
    });
    *mediaOut = media;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    (void)argc;
    uitest::setup({}, {});
    AppState& st = AppState::get();
    const int n = 4000;
    uint64_t track = 0, media = 0;

    // Event Heal joins every piece that carries on from the one before: 4000 pieces become one event
    {
        build(n, 0.01, &track, &media);
        std::set<uint64_t> all;
        for (const Event& e : trackOf(track)->events) all.insert(e.id);
        const double t0 = nowMs();
        const int joins = st.healEvents(all);
        const double ms = nowMs() - t0;
        const Track* t = trackOf(track);
        std::printf("heal %d pieces: %.1f ms\n", n, ms);
        check(joins == n - 1 && t->events.size() == 1, "heal joins all pieces", "joins " + std::to_string(joins) + " left " + std::to_string(t->events.size()));
        check(t->events.size() == 1 && std::fabs(t->events[0].length - n * 0.01) < 1e-6, "healed length");
        check(ms < 2000, "heal time", std::to_string(ms));
        st.undo();
        check(trackOf(track)->events.size() == (size_t)n, "heal undone");
    }
    // delete every second piece, selected
    {
        build(n, 0.01, &track, &media);
        std::set<uint64_t> sel;
        int k = 0;
        for (const Event& e : trackOf(track)->events) if (k++ % 2) sel.insert(e.id);
        st.setSelection(sel);
        const double t0 = nowMs();
        st.deleteSelection();
        std::printf("delete %zu selected: %.1f ms\n", sel.size(), nowMs() - t0);
        check(trackOf(track)->events.size() == (size_t)n / 2, "delete half");
        // close gaps before the rest
        std::set<uint64_t> rest;
        for (const Event& e : trackOf(track)->events) rest.insert(e.id);
        const double t1 = nowMs();
        const int moved = st.closeGaps(rest);
        std::printf("close gaps %zu: %.1f ms\n", rest.size(), nowMs() - t1);
        check(moved == n / 2 - 1, "close gaps moves all but the first", std::to_string(moved));
    }
    // a time selection over most of the project taken out
    {
        build(n, 0.01, &track, &media);
        st.setSelection({});
        st.setLoop(0.005, n * 0.01 - 0.005);
        const double t0 = nowMs();
        st.deleteSelection();
        std::printf("delete time selection over %d pieces: %.1f ms\n", n, nowMs() - t0);
        check(trackOf(track)->events.size() == 2, "time selection delete leaves two ends", std::to_string(trackOf(track)->events.size()));
        st.setLoop(0, 0);
    }
    // split at the cursor with nothing selected, then undo
    {
        build(n, 0.01, &track, &media);
        st.setSelection({});
        st.setCursor(0.0055);
        const double t0 = nowMs();
        st.splitAtCursor();
        std::printf("split: %.1f ms\n", nowMs() - t0);
        check(trackOf(track)->events.size() == (size_t)n + 1, "split makes one more");
    }
    // the commands that work on everything selected, timed
    {
        build(n, 0.01, &track, &media);
        std::set<uint64_t> all;
        for (const Event& e : trackOf(track)->events) all.insert(e.id);
        auto timed = [&](const char* name, const std::function<void()>& fn) {
            const double t0 = nowMs();
            fn();
            const double ms = nowMs() - t0;
            std::printf("%s (%d events): %.1f ms\n", name, n, ms);
            check(ms < 500, std::string(name) + " is quick", std::to_string(ms));
        };
        st.setSelection(all);
        timed("group", [&] { st.groupSelection(true); });
        timed("lock", [&] { st.setLocked(all, true); });
        timed("unlock", [&] { st.setLocked(all, false); });
        timed("copy", [&] { st.copySelection(); });
        st.setCursor(100.0);
        timed("paste", [&] { st.pasteAtCursor(); });
        check(trackOf(track)->events.size() == (size_t)2 * n, "paste added all");
        timed("undo", [&] { st.undo(); });
        timed("redo", [&] { st.redo(); });
        st.setSelection(all);
        timed("cut", [&] { st.cutSelection(); });
        timed("insert time", [&] { st.insertTime(1.0, 2.0); });
        timed("duplicate track", [&] { st.duplicateTrack(track); });
        timed("undo all", [&] { st.undoAll(); });
    }
    // Auto ripple after taking out every second piece: what is left closes up, each piece by the pieces taken out before it
    {
        build(n, 0.01, &track, &media);
        Project p = st.project();
        std::map<uint64_t, Ranges> removed;
        std::vector<uint64_t> gone;
        Track* t = p.findTrack(track);
        for (size_t k = 1; k < t->events.size(); k += 2) {
            removed[track].emplace_back(t->events[k].start, t->events[k].end());
            gone.push_back(t->events[k].id);
        }
        t->events.erase(std::remove_if(t->events.begin(), t->events.end(), [&](const Event& e) {
            return std::find(gone.begin(), gone.end(), e.id) != gone.end(); }), t->events.end());
        const double t0 = nowMs();
        rippleClose(p, removed, RippleMode::AffectedTracks);
        std::printf("ripple close (checked, not a hot path) over %zu removed pieces: %.1f ms\n", gone.size(), nowMs() - t0);
        bool ok = true;
        for (size_t k = 0; k < t->events.size(); ++k) ok = ok && std::fabs(t->events[k].start - (double)k * 0.01) < 1e-9;
        check(ok && t->events.size() == (size_t)n / 2, "ripple closes the gaps exactly");
    }
    return uitest::summary();
}
