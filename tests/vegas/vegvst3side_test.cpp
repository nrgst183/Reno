// vegvst3side_test: VST3 audio effects beside a .veg (VEGAS Pro 18 has no VST3 host, so the .veg cannot hold them).
//
//   vegvst3side_test [folder with .veg files] [--scratch folder]       (default $RENO_SAMPLES; only read)
//
// A sample project gets VST3 effects on a track, on an event, on a bus and on the master, among its other effects.
//  1. The .veg written is byte for byte the one written without the VST3 effects (VEGAS never sees them).
//  2. After Save and Open every VST3 effect is back where it was (chain, place in the chain), with its plug-in
//     description, name, bypass and state exactly as they were - also for a plug-in that is not installed.
//  3. Saving the reopened project again changes neither the .veg nor the side file; the effects are not duplicated.
//  4. Take them all out and save: the side file goes away. A side file for a track or event that is gone is reported,
//     a damaged one is reported, and neither stops the project from opening.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegvst3side.h"
#include "../../src/core/base.h"
#include <cstdio>
#include <filesystem>
#include "../testdata.h"

using namespace reno;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
static bool check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " - ", detail.c_str()); std::fflush(stdout); }
    return ok;
}

static FxSlot vst3(Project& p, const char* name, const char* state, bool bypass)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.name = name;
    s.bypass = bypass;
    s.audio.format = "VST3";
    s.audio.path = "C:/Program Files/Common Files/VST3/NotInstalled.vst3";      // no such plug-in here: Reno cannot load it
    s.audio.uid = "ABCDEF0123456789ABCDEF0123456789";
    s.audio.name = name;
    s.state = std::string(state);
    return s;
}

static int count3(const FxChain& c) { int n = 0; for (const auto& s : c) n += s.audio.format == "VST3" ? 1 : 0; return n; }
static int total3(const Project& p)
{
    int n = count3(p.audioOut);
    for (const auto& t : p.tracks) { n += count3(t.fx); for (const auto& e : t.events) n += count3(e.fx); }
    for (const auto& b : p.buses) n += count3(b.fx);
    return n;
}

int main(int argc, char** argv)
{
    std::string dir = testDataDir("RENO_SAMPLES"), scratch = (fs::temp_directory_path() / "vegvst3side_test").string();
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--scratch" && i + 1 < argc) scratch = argv[++i];
        else dir = argv[i];
    }
    if (testDataMissing(dir)) return 0;
    std::error_code ec;
    fs::create_directories(scratch, ec);
    // a sample with an audio track that has an event, and a bus
    Project base;
    size_t ti = 0, ei = 0;
    bool found = false;
    for (const std::string& f : testVegFiles(dir)) {
        Project c;
        if (!importVegasProject(f, c).ok || c.buses.empty()) continue;
        for (size_t t = 0; !found && t < c.tracks.size(); ++t)
            if (c.tracks[t].type == TrackType::Audio && !c.tracks[t].events.empty()) { ti = t; ei = c.tracks[t].events.size() - 1; found = true; }
        if (found) { base = c; break; }
    }
    if (!found) { std::printf("SKIP: no sample project has an audio event and a bus\n"); return 0; }

    const std::string veg = scratch + "/side.veg", side = vst3SidePath(veg);
    fs::remove(veg, ec);
    fs::remove(side, ec);
    fs::remove(veg + ".bak", ec);

    Project plain = base;
    plain.tracks[ti].events[ei].start += 0;       // (unchanged)
    // the .veg without any VST3 effects
    const std::string plainBytes = writeVegasData(plain, veg, nullptr);
    check(!plainBytes.empty(), "the sample is written");
    check(writeVegasData(plain, veg, nullptr) == plainBytes, "(writing twice gives the same bytes)");

    Project p = base;
    const size_t trackFx0 = p.tracks[ti].fx.size(), evFx0 = p.tracks[ti].events[ei].fx.size();
    p.tracks[ti].fx.insert(p.tracks[ti].fx.begin(), vst3(p, "Track Verb", "VST3:AAAA:-", false));
    p.tracks[ti].fx.push_back(vst3(p, "Track End", "VST3:-:BBBB", true));
    p.tracks[ti].events[ei].fx.push_back(vst3(p, "Event Comp", "VST3:CCCC:DDDD", false));
    p.buses[0].fx.push_back(vst3(p, "Bus EQ", "VST3:EEEE:-", true));
    p.audioOut.push_back(vst3(p, "Master Limiter", "VST3:FFFF:GGGG", false));
    const int expect = total3(p);
    check(expect == 5, "five VST3 effects set up");

    // 1. the .veg is the same as without them
    std::vector<std::string> warnings;
    const std::string withBytes = writeVegasData(p, veg, &warnings);
    check(withBytes == plainBytes, "the .veg written with VST3 effects in the project is the same bytes as without");
    {
        // (which chain makes the difference, when one does)
        auto only = [&](int which) {
            Project x = base;
            if (which == 0) x.tracks[ti].fx.push_back(vst3(x, "T", "VST3:A:-", false));
            if (which == 1) x.tracks[ti].events[ei].fx.push_back(vst3(x, "E", "VST3:A:-", false));
            if (which == 2) x.buses[0].fx.push_back(vst3(x, "B", "VST3:A:-", false));
            if (which == 3) x.audioOut.push_back(vst3(x, "M", "VST3:A:-", false));
            return writeVegasData(x, veg, nullptr) == plainBytes;
        };
        check(only(0), "  track chain alone leaves the .veg as it was");
        check(only(1), "  event chain alone leaves the .veg as it was");
        check(only(2), "  bus chain alone leaves the .veg as it was");
        check(only(3), "  master chain alone leaves the .veg as it was");
    }
    bool said = false;
    for (const auto& w : warnings) said = said || w.find("VST3") != std::string::npos;
    check(said, "the writer says the VST3 effects are not in the .veg");

    // save for real
    const VegasSaveResult sr = saveVegasProject(p, veg);
    check(sr.ok, "saved", sr.error);
    check(fs::exists(side), "the side file is beside the .veg");
    std::string onDisk;
    readFile(veg, onDisk);
    check(onDisk == plainBytes || !onDisk.empty(), "the .veg is on disk");

    // 2. open
    Project q;
    VegasImportResult ir = importVegasProject(veg, q);
    check(ir.ok, "opened", ir.error);
    check(total3(q) == expect, "all VST3 effects are back", strfmt("%d of %d", total3(q), expect));
    bool noted = false;
    for (const auto& n : ir.notes) noted = noted || n.find("VST3") != std::string::npos;
    check(noted, "opening says they were put back");
    if (ti < q.tracks.size() && ei < q.tracks[ti].events.size()) {
        const FxChain& tf = q.tracks[ti].fx;
        check(tf.size() == trackFx0 + 2 && tf.front().name == "Track Verb" && tf.back().name == "Track End", "track chain: first and last places kept");
        check(tf.size() >= 2 && tf.back().bypass && !tf.front().bypass, "bypass kept");
        check(tf.front().state.asString() == "VST3:AAAA:-" && tf.back().state.asString() == "VST3:-:BBBB", "state strings kept exactly");
        check(tf.front().audio.uid == "ABCDEF0123456789ABCDEF0123456789" && tf.front().audio.format == "VST3" &&
              tf.front().audio.path == "C:/Program Files/Common Files/VST3/NotInstalled.vst3", "plug-in description kept");
        const FxChain& ef = q.tracks[ti].events[ei].fx;
        check(ef.size() == evFx0 + 1 && ef.back().name == "Event Comp" && ef.back().state.asString() == "VST3:CCCC:DDDD", "event chain");
    } else {
        check(false, "track and event are there");
    }
    check(!q.buses.empty() && count3(q.buses[0].fx) == 1 && q.buses[0].fx.back().bypass && q.buses[0].fx.back().state.asString() == "VST3:EEEE:-", "bus chain");
    check(count3(q.audioOut) == 1 && q.audioOut.back().state.asString() == "VST3:FFFF:GGGG", "master chain");

    // 3. save again: nothing changes, nothing is duplicated
    std::string side1, side2;
    readFile(side, side1);
    check(saveVegasProject(q, veg).ok, "saved again");
    readFile(side, side2);
    check(side1 == side2 && !side1.empty(), "the side file is the same after a second save");
    Project r;
    check(importVegasProject(veg, r).ok && total3(r) == expect, "opened again with the same effects (no duplicates)");

    // 4. a side file for things that are gone, a damaged one, and none
    {
        Project fewer = r;
        // the event is no longer where it was
        fewer.tracks[ti].events[ei].start += 1000;
        Project g;
        // (read the .veg, then let the side file be read into a project whose event moved)
        g = base;
        g.tracks[ti].events[ei].start += 1000;
        std::vector<std::string> notes;
        const int n = readVst3Side(veg, g, &notes);
        check(n == expect - 1, "an event that moved: its effect is not put back, the others are", strfmt("%d", n));
        bool lost = false;
        for (const auto& s : notes) lost = lost || s.find("could not be placed") != std::string::npos;
        check(lost, "and that is said");
        Project h = base;
        h.tracks.resize(ti);        // the track is gone (anything from it on)
        notes.clear();
        const int m = readVst3Side(veg, h, &notes);
        check(m < expect && !notes.empty(), "a track that is gone: its effects are reported");
    }
    {
        std::string saved;
        readFile(side, saved);
        writeFile(side, "{ not json");
        Project d;
        const VegasImportResult dr = importVegasProject(veg, d);
        bool said2 = false;
        for (const auto& n : dr.notes) said2 = said2 || n.find("could not be read") != std::string::npos;
        check(dr.ok && total3(d) == 0 && said2, "a damaged side file: the project opens without the effects and says so");
        writeFile(side, "[1,2,3]");
        Project d2;
        check(importVegasProject(veg, d2).ok && total3(d2) == 0, "a side file of another shape is ignored");
        writeFile(side, saved);
    }
    {
        Project none = r;
        auto strip = [](FxChain& c) { for (size_t i = c.size(); i-- > 0;) if (c[i].audio.format == "VST3") c.erase(c.begin() + (std::ptrdiff_t)i); };
        strip(none.audioOut);
        for (auto& t : none.tracks) { strip(t.fx); for (auto& e : t.events) strip(e.fx); }
        for (auto& b : none.buses) strip(b.fx);
        check(saveVegasProject(none, veg).ok, "saved without VST3 effects");
        check(!fs::exists(side), "the side file is removed (it would bring back effects that were taken out)");
        Project z;
        check(importVegasProject(veg, z).ok && total3(z) == 0, "opens with none");
    }
    // a project without a side file opens as before
    {
        Project y;
        const VegasImportResult yr = importVegasProject(veg, y);
        bool any = false;
        for (const auto& n : yr.notes) any = any || n.find("VST3") != std::string::npos;
        check(yr.ok && !any, "no side file: no VST3 note");
    }
    fs::remove(veg, ec);
    fs::remove(veg + ".bak", ec);
    fs::remove(side, ec);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
