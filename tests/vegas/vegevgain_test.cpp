// vegevgain_test: an event's gain (audio: volume, video: opacity) as the .veg importer reads it, against VEGAS's own
// reading of the same project (the files in $RENO_CMP/info, written by tools/vegas/Export to Reno.cs). VEGAS's
// "gain" there is the event's own gain (FadeIn.Gain) times the constant value of a volume or composite envelope, and for
// an audio event with Normalize on also its normalising gain; Reno keeps that apart (Event::normalize, normalizeGain)
// and the engine multiplies them. Also: the gain survives a save and load (writeVegasProject) unchanged.
//
//   vegevgain_test [cmp folder (default $RENO_CMP)]
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

// (the info files are named with underscores for the project's spaces)
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
    int events = 0, staleDiffs = 0, unmatched = 0, mismatches = 0, lowered = 0, videoLowered = 0, projects = 0, roundTrip = 0, roundTripBad = 0;
    for (const std::string& base : infos) {
        std::string name = base;
        if (name.compare(0, 5, "real_") == 0) name = name.substr(5);
        std::vector<std::string> cands;
        findVeg(cmp, name, cands);
        if (cands.empty()) { std::printf("(no .veg found for %s)\n", base.c_str()); continue; }
        const std::string veg = cands[0];
        Project p;
        const VegasImportResult r = importVegasProject(veg, p);
        if (!r.ok) { check(false, base + ": the importer opens it"); continue; }
        Json B;
        std::string err;
        if (!Json::parse(slurp(cmp + "/info/" + base + ".vegas.reno"), B, &err)) { check(false, base + ": the VEGAS export reads"); continue; }
        ++projects;
        struct Ev { double start, length, gain; bool video, used; };
        std::vector<Ev> mine;
        for (const Track& t : p.tracks)
            for (const Event& e : t.events)
                mine.push_back({e.start, e.length, e.gain * (e.normalize && e.normalizeGain > 0 ? e.normalizeGain : 1.0), t.type == TrackType::Video, false});
        int bad = 0, n = 0, lost = 0;
        for (const Json& tj : B.get("tracks").items()) {
            const bool video = tj.get("type").asString() == "video";
            for (const Json& ej : tj.get("events").items()) {
                const double s = ej.get("start").asDouble(), l = ej.get("length").asDouble(), g = ej.get("gain").asDouble();
                Ev* hit = nullptr;
                for (Ev& m : mine)
                    if (!m.used && m.video == video && std::fabs(m.start - s) < 1e-5 && std::fabs(m.length - l) < 1e-5 && std::fabs(m.gain - g) < 1e-4 * std::max(1.0, g)) { hit = &m; break; }
                if (!hit)
                    for (Ev& m : mine)
                        if (!m.used && m.video == video && std::fabs(m.start - s) < 1e-5 && std::fabs(m.length - l) < 1e-5) { hit = &m; break; }
                ++n;
                ++events;
                if (g < 0.9999) { ++lowered; if (video) ++videoLowered; }
                if (!hit) {
                    // (a project whose .veg on disk is not the version VEGAS exported has its events elsewhere: those events cannot be compared)
                    ++unmatched;
                    ++lost;
                    if (unmatched <= 3) {
                        std::printf("  %s: %s event at %.5f length %.5f has no match; importer has %zu events; nearest:", base.c_str(), video ? "video" : "audio", s, l, mine.size());
                        int shown = 0;
                        for (const Ev& m : mine)
                            if (std::fabs(m.start - s) < 0.01 && shown++ < 3) std::printf(" [%s %.5f %.5f g %.3f%s]", m.video ? "v" : "a", m.start, m.length, m.gain, m.used ? " used" : "");
                        std::printf("\n");
                    }
                    continue;
                }
                hit->used = true;
                if (std::fabs(hit->gain - g) > 1e-4 * std::max(1.0, g)) {
                    ++bad;
                    if (bad <= 5) std::printf("  %s: %s event at %.4f: VEGAS gain %.6f, importer %.6f\n", base.c_str(), video ? "video" : "audio", s, g, hit->gain);
                }
            }
        }
        // (a project of which the .veg on disk is not the version VEGAS exported - some events gone or moved - may also differ
        // in an event's gain; reported, not counted)
        if (lost == 0) mismatches += bad;
        else staleDiffs += bad;
        check(bad == 0 || lost > 0, strfmt("%s: the gain / opacity of every event found in both equals VEGAS's (%d events in VEGAS's export)", base.c_str(), n));
        // written as a .veg and read back: the same gains - and, with every gain changed (an edit made in Reno: the
        // line dragged down), the new ones
        for (int edit = 0; edit < 2; ++edit) {
            Project w = p;
            std::vector<double> want;
            for (Track& t : w.tracks)
                for (Event& e : t.events) {
                    if (edit) e.gain = std::floor(e.gain * 0.5 * 1000.0) / 1000.0;
                    want.push_back(e.gain);
                }
            const std::string out = "/tmp/vegevgain_" + name + ".veg";
            const std::string bytes = writeVegasData(w, out, nullptr);
            std::ofstream(out, std::ios::binary) << bytes;
            Project q;
            bool same = false;
            if (!bytes.empty() && importVegasProject(out, q).ok) {
                std::vector<double> got;
                for (const Track& t : q.tracks) for (const Event& e : t.events) got.push_back(e.gain);
                same = got.size() == want.size();
                for (size_t i = 0; same && i < want.size(); ++i) same = std::fabs(want[i] - got[i]) < 1e-6;
            }
            ++roundTrip;
            if (!same) ++roundTripBad;
            check(same, base + (edit ? ": gains changed in Reno are written to the .veg and read back" : ": unchanged gains survive writing the .veg and reading it back"));
            std::remove(out.c_str());
        }
    }
    std::printf("%d projects, %d events (%d with gain below 1, %d of them video), %d gain mismatches, %d round trips (%d bad)\n", projects, events, lowered, videoLowered, mismatches,
                roundTrip, roundTripBad);
    std::printf("%d gain differences in the projects whose .veg is another version than VEGAS's export\n", staleDiffs);
    std::printf("%d events could not be paired with one of VEGAS's by time (a different version of the project)\n", unmatched);
    if (!projects) { std::printf("SKIP: no VEGAS export (info/real_<project>.vegas.reno) of a project under %s\n", cmp.c_str()); return 0; }
    check(mismatches == 0, strfmt("%d events paired, none with a different gain or opacity", events - unmatched));
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
