// vegmatrix_write_test: Reno's .veg writer against the files VEGAS made from the Settings Matrix script
// (tools/scripts/Reno Settings Matrix.cs). Each case takes the file the manifest names as the base, reads it, makes the
// same change in the Reno project, writes it, and compares the records that change (bus records, video track and
// video event settings, the group list) byte for byte with the same records of VEGAS's file. It also reads VEGAS's
// file and checks the setting comes out in the Reno project.
//
//   vegmatrix_write_test [matrix folder (default $RENO_MATRIX)]
//
// The folder is only read; it is not in the repository, and the test skips when it is not there.
//
// The bus cases start from 0447_T2.BusTrack=added_bus.veg, not from 02_base_with_bus.veg: VEGAS gives the added bus its
// name in another text of the record when it has just made it than after a file is opened and saved again, which the
// variants were, so only 0447 (a track sent to the bus, nothing else changed) is a base that differs from them in the
// one setting.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>
#include <string>
#include <vector>
#include "../testdata.h"

using namespace reno;

static int g_pass = 0, g_fail = 0;
static bool check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    return ok;
}

static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// the contents of every chunk (not list) with this id, in file order
static void collect(const veg::Chunk& c, const char* id, std::vector<std::string>& out)
{
    if (!c.isList && c.id == id) out.push_back(std::string((const char*)c.data.data(), c.data.size()));
    for (const auto& k : c.kids) collect(k, id, out);
}
static std::vector<std::string> chunksOf(const std::string& bytes, const char* id)
{
    std::vector<std::string> out;
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    collect(t.root, id, out);
    return out;
}
static uint32_t word(const std::string& s, size_t off)
{
    uint32_t v = 0;
    if (off + 4 <= s.size()) std::memcpy(&v, s.data() + off, 4);
    return v;
}
// the bus records of one kind (2 master, 3 audio bus)
static std::vector<std::string> busesOf(const std::string& bytes, uint32_t type)
{
    std::vector<std::string> out;
    for (auto& b : chunksOf(bytes, veg::G::Bus))
        if (word(b, 0x0c) == type) out.push_back(b);
    return out;
}

static std::string g_dir;

static bool load(const std::string& name, Project& p)
{
    VegasImportResult r = importVegasProject(g_dir + "/" + name, p);
    return check(r.ok, name + " is read", r.error);
}

// base + edit, written, against VEGAS's file; 'pick' says which records to compare
using Pick = std::function<std::vector<std::string>(const std::string&)>;
static void rebuild(const std::string& label, const std::string& base, const std::string& variant, const std::function<void(Project&)>& edit,
                    const Pick& pick, const std::function<void(const Project&)>& readBack = nullptr)
{
    Project p;
    if (!load(base, p)) return;
    edit(p);
    std::vector<std::string> warnings;
    const std::string ours = writeVegasData(p, g_dir + "/" + variant, &warnings);
    const std::string theirs = slurp(g_dir + "/" + variant);
    if (theirs.empty()) { check(false, label + ": " + variant + " is there"); return; }
    const auto a = pick(ours), b = pick(theirs);
    bool same = !a.empty() && a.size() == b.size();
    std::string detail = strfmt("%zu records of ours, %zu of VEGAS's", a.size(), b.size());
    for (size_t i = 0; same && i < a.size(); ++i) {
        if (a[i] == b[i]) continue;
        same = false;
        size_t k = 0;
        while (k < a[i].size() && k < b[i].size() && a[i][k] == b[i][k]) ++k;
        detail = strfmt("record %zu: sizes %zu / %zu, first difference at 0x%zx (ours %02x, VEGAS %02x)", i, a[i].size(), b[i].size(), k,
                        k < a[i].size() ? (unsigned char)a[i][k] : 0, k < b[i].size() ? (unsigned char)b[i][k] : 0);
    }
    check(same, label + ": the records are VEGAS's, byte for byte", detail);
    for (const auto& w : warnings)
        if (w.find("Bus") != std::string::npos || w.find("track") != std::string::npos) check(w.find("not saved") == std::string::npos, label + ": no warning of lost settings", w);
    Project q;
    if (load(variant, q) && readBack) readBack(q);
}

int main(int argc, char** argv)
{
    g_dir = argc > 1 ? argv[1] : testDataDir("RENO_MATRIX");
    if (testDataMissing(g_dir + "/manifest.txt")) return 0;

    const Pick buses3 = [](const std::string& s) { return busesOf(s, 3); };
    const Pick master = [](const std::string& s) { return busesOf(s, 2); };
    const Pick videoEvents = [](const std::string& s) { return chunksOf(s, veg::G::VideoEventData); };
    const Pick videoTracks = [](const std::string& s) { return chunksOf(s, veg::G::VideoTrackData); };

    // ---- buses: mute, solo, pan type
    const std::string busBase = "0447_T2.BusTrack=added_bus.veg";
    rebuild("bus mute", busBase, "0437_Bus2.Mute=true.veg", [](Project& p) { p.buses.at(0).mute = true; }, buses3,
            [](const Project& q) { check(q.buses.size() == 1 && q.buses[0].mute && !q.buses[0].solo, "bus mute is read"); });
    rebuild("bus solo", busBase, "0436_Bus2.Solo=true.veg", [](Project& p) { p.buses.at(0).solo = true; }, buses3,
            [](const Project& q) { check(q.buses.size() == 1 && q.buses[0].solo && !q.buses[0].mute, "bus solo is read"); });
    const struct { const char* file; const char* name; } pans[] = {{"0438_Bus2.PanType=ConstantPower.veg", "power"}, {"0439_Bus2.PanType=Balance.veg", "balance0"},
                                                                  {"0440_Bus2.PanType=Notch3Db.veg", "balance3"}, {"0441_Bus2.PanType=Notch6Db.veg", "balance6"},
                                                                  {"0442_Bus2.PanType=Film.veg", "film"}};
    for (const auto& pt : pans) {
        const std::string name = pt.name;
        rebuild(std::string("bus pan type ") + pt.name, busBase, pt.file, [name](Project& p) { p.buses.at(0).panType = name; }, buses3,
                [name](const Project& q) { check(q.buses.size() == 1 && q.buses[0].panType == name, "bus pan type " + name + " is read"); });
    }
    // ---- the master: mute (its pan type is not kept in the file at all: the three PanType files equal the base)
    rebuild("master mute", "00_base.veg", "0421_MasterBus.Mute=true.veg", [](Project& p) { p.masterMute = true; }, master,
            [](const Project& q) { check(q.masterMute, "master mute is read"); });

    // ---- events: undersample rate, Maintain Aspect Ratio, Reduce Interlace Flicker, resample mode
    const struct { const char* file; int track; const char* what; std::function<void(Event&)> edit; std::function<bool(const Event&)> test; } ev[] = {
        {"0257_T0.E0.UnderSampleRate=0.5.veg", 0, "undersample 0.5", [](Event& e) { e.undersample = 0.5; }, [](const Event& e) { return e.undersample == 0.5; }},
        {"0295_T1.E0.UnderSampleRate=0.5.veg", 1, "undersample 0.5", [](Event& e) { e.undersample = 0.5; }, [](const Event& e) { return e.undersample == 0.5; }},
        {"0258_T0.E0.MaintainAspectRatio=false.veg", 0, "keep aspect off", [](Event& e) { e.keepAspect = false; }, [](const Event& e) { return !e.keepAspect; }},
        {"0296_T1.E0.MaintainAspectRatio=false.veg", 1, "keep aspect off", [](Event& e) { e.keepAspect = false; }, [](const Event& e) { return !e.keepAspect; }},
        {"0259_T0.E0.ReduceInterlace=true.veg", 0, "reduce flicker on", [](Event& e) { e.reduceFlicker = true; }, [](const Event& e) { return e.reduceFlicker; }},
        {"0297_T1.E0.ReduceInterlace=false.veg", 1, "reduce flicker off", [](Event& e) { e.reduceFlicker = false; }, [](const Event& e) { return !e.reduceFlicker; }},
        {"0260_T0.E0.ResampleMode=Force.veg", 0, "resample force", [](Event& e) { e.resample = "force"; }, [](const Event& e) { return e.resample == "force"; }},
        {"0298_T1.E0.ResampleMode=Force.veg", 1, "resample force", [](Event& e) { e.resample = "force"; }, [](const Event& e) { return e.resample == "force"; }},
        {"0261_T0.E0.ResampleMode=Disable.veg", 0, "resample off", [](Event& e) { e.resample = "off"; }, [](const Event& e) { return e.resample == "off"; }},
        {"0299_T1.E0.ResampleMode=Disable.veg", 1, "resample off", [](Event& e) { e.resample = "off"; }, [](const Event& e) { return e.resample == "off"; }},
    };
    for (const auto& c : ev) {
        const int ti = c.track;
        auto edit = c.edit;
        auto test = c.test;
        rebuild(std::string("event ") + c.what + " (track " + std::to_string(ti + 1) + ")", "00_base.veg", c.file,
                [ti, edit](Project& p) { edit(p.tracks.at((size_t)ti).events[0]); }, videoEvents,
                [ti, test, c](const Project& q) { check(test(std::as_const(q).tracks.at((size_t)ti).events.at(0)), std::string("event ") + c.what + " is read"); });
    }

    // ---- parent and child
    rebuild("video track 2 a child of track 1", "00_base.veg", "0235_T1.CompositeNestingLevel=1.veg", [](Project& p) { p.tracks.at(1).compositeLevel = 1; }, videoTracks,
            [](const Project& q) { check(q.tracks.at(0).compositeLevel == 0 && q.tracks.at(1).compositeLevel == 1, "parent and child are read"); });

    // ---- event group (written before this run; checked against VEGAS's own file all the same)
    rebuild("event group", "00_base.veg", "0410_group_A_and_audio.veg",
            [](Project& p) {
                const uint64_t g = p.newId();
                p.tracks.at(0).events[0].group = g;
                p.tracks.at(2).events[0].group = g;
            },
            [](const std::string& s) { return chunksOf(s, veg::G::Group); },
            [](const Project& q) { check(q.tracks.at(0).events.at(0).group != 0 && q.tracks.at(0).events.at(0).group == q.tracks.at(2).events.at(0).group, "the event group is read"); });

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
