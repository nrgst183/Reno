// vegsettings_test: the project settings and event switches the .veg importer reads from the places found by the audit
// (tests/vegas/vegaudit.cpp), against VEGAS's own export of the same projects ($RENO_CMP/info):
//   - Project render quality (video settings block +0x1c: 3 Good, 4 Best) and deinterlace method (+0xf4: 0 none, 1 blend)
//   - an event's resample switch (VideoEventData flags bit 0x200; clear = Disable Resample)
// and that changes to them are written and read back.
//
//   vegsettings_test [cmp folder (default $RENO_CMP)]
#include "../../src/vegas/vegas.h"
#include "../../src/core/json.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../testdata.h"

using namespace reno;

static int passed = 0, failed = 0;
static void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? passed : failed)++;
}
static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
static std::string squash(std::string s)
{
    for (char& c : s) c = c == ' ' ? '_' : (char)std::tolower((unsigned char)c);
    return s;
}
static void findVeg(const std::string& dir, const std::string& name, std::vector<std::string>& found)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string full = dir + "/" + n;
        if (squash(n) == squash(name) + ".veg") found.push_back(full);
        else if (e->d_type == DT_DIR || e->d_type == DT_LNK) findVeg(full, name, found);
    }
    closedir(d);
}

int main(int argc, char** argv)
{
    const std::string cmp = argc > 1 ? argv[1] : testDataDir("RENO_CMP");
    std::vector<std::string> infos;
    if (DIR* d = opendir((cmp + "/info").c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 11 && n.substr(n.size() - 11) == ".vegas.reno") infos.push_back(n.substr(0, n.size() - 11));
        }
        closedir(d);
    }
    int projects = 0, qualities = 0, deints = 0, disabled = 0, disabledVegas = 0;
    std::string firstVeg;
    for (const std::string& base : infos) {
        std::string name = base;
        if (name.compare(0, 5, "real_") == 0) name = name.substr(5);
        std::vector<std::string> cands;
        findVeg(cmp, name, cands);
        if (cands.empty()) continue;
        Project p;
        if (!importVegasProject(cands[0], p).ok) { check(false, base + ": the importer opens it"); continue; }
        Json B;
        if (!Json::parse(slurp(cmp + "/info/" + base + ".vegas.reno"), B, nullptr)) continue;
        ++projects;
        if (firstVeg.empty()) firstVeg = cands[0];
        const Json& sb = B.get("settings");
        if (sb.has("renderQuality")) {
            ++qualities;
            check(p.settings.renderQuality == sb.get("renderQuality").asString(), base + ": render quality " + p.settings.renderQuality + " = VEGAS's " + sb.get("renderQuality").asString());
        }
        if (sb.has("deinterlace")) {
            ++deints;
            check(p.settings.deinterlace == sb.get("deinterlace").asString(), base + ": deinterlace " + p.settings.deinterlace + " = VEGAS's " + sb.get("deinterlace").asString());
        }
        int mine = 0, theirs = 0;
        for (const Track& t : p.tracks) for (const Event& e : t.events) mine += (t.type == TrackType::Video && e.resample == "off") ? 1 : 0;
        for (const Json& tj : B.get("tracks").items())
            for (const Json& ej : tj.get("events").items())
                theirs += ej.get("vegas").get("resampleMode").asString() == "Disable" ? 1 : 0;
        disabled += mine;
        disabledVegas += theirs;
        for (const Track& t : p.tracks) for (const Event& e : t.events) if (t.type == TrackType::Video && !e.keepAspect) { check(false, base + ": no event is stretched on load"); break; }
    }
    if (!projects) { std::printf("SKIP: no VEGAS export (info/real_<project>.vegas.reno) of a project under %s\n", cmp.c_str()); return 0; }
    check(qualities == projects && deints == projects, strfmt("%d projects compared (%d qualities, %d deinterlace methods)", projects, qualities, deints));
    std::printf("resample off: importer %d, VEGAS %d (the export can be another version of the project)\n", disabled, disabledVegas);
    check(disabledVegas == 0 || disabled > 0, "an event VEGAS says has Disable Resample is read as resample off");

    // writing: unchanged is the same bytes; changed is read back changed
    if (!firstVeg.empty()) {
        Project p;
        importVegasProject(firstVeg, p);
        // (that an unchanged project is written back byte for byte is vegroundtrip's check, over all the samples)

        Project w = p;
        w.settings.renderQuality = w.settings.renderQuality == "best" ? "preview" : "best";
        w.settings.deinterlace = w.settings.deinterlace == "blend" ? "interpolate" : "blend";
        int toggled = 0;
        for (Track& t : w.tracks)
            if (t.type == TrackType::Video)
                for (Event& e : t.events) if (toggled < 3) { e.resample = e.resample == "off" ? "project" : "off"; ++toggled; }
        const std::string bytes = writeVegasData(w, "/tmp/vegsettings_b.veg", nullptr);
        std::ofstream("/tmp/vegsettings_b.veg", std::ios::binary) << bytes;
        Project q;
        const bool ok = !bytes.empty() && importVegasProject("/tmp/vegsettings_b.veg", q).ok;
        check(ok, "the changed project is written and read back");
        if (ok) {
            check(q.settings.renderQuality == w.settings.renderQuality, "changed render quality survives: " + q.settings.renderQuality);
            check(q.settings.deinterlace == w.settings.deinterlace, "changed deinterlace survives: " + q.settings.deinterlace);
            std::vector<std::string> a, b;
            for (const Track& t : w.tracks) if (t.type == TrackType::Video) for (const Event& e : t.events) a.push_back(e.resample);
            for (const Track& t : q.tracks) if (t.type == TrackType::Video) for (const Event& e : t.events) b.push_back(e.resample);
            check(toggled > 0 && a == b, strfmt("changed event resample switches survive (%d changed)", toggled));
        }
        std::remove("/tmp/vegsettings_b.veg");
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
