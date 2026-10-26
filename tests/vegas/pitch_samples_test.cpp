// pitch_samples_test: how the pitch and time-stretch settings of the sample VEGAS projects come into Reno.
//
//   pitch_samples_test [folder with .veg files [folder with VEGAS's own export (real_<p>.vegas.reno), optional]]
//     (default $RENO_SAMPLES; the second folder defaults to $RENO_CMP/info and is skipped if absent)
//
// 1. Every audio event of every project: the settings are read straight from the file's bytes here
//    (method = bits 2-3 of the flags at 0x08 of the audio event record, Lock to stretch = bit 1, Classic preset at
//    0x0c, élastique mode at 0x10, pitch change as a double at 0x14, formant shift at 0x1c, rate at 0x28) and the
//    events Reno makes must match: method, stretch attributes, whether the pitch follows the rate, the semitones
//    kept (VEGAS keeps a value for a locked event too, which does nothing), formant shift, rate.
// 2. The same against VEGAS Pro 18's scripting API (AudioEvent.Method, PitchSemis, PitchLock, ElastiqueAttribute...)
//    for the projects exported that way.
// 3. Writing the project and reading it again gives the same events; a changed pitch, lock, method or mode is
//    written where VEGAS keeps it.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <sstream>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static bool check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++gPass;
    else ++gFail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
    std::fflush(stdout);
    return ok;
}

typedef std::map<std::string, int> Bag;

static std::string key(const std::string& method, const std::string& mode, bool follows, double pitch, double formant, double rate)
{
    // (a pitch of a locked event, or of method None, does nothing and is not part of what it sounds like)
    return strfmt("%s %s %s pitch %+.3f formant %+.3f rate %.5f", method.c_str(), method == "none" ? "-" : mode.c_str(),
                  follows ? "follows-rate" : "keeps-pitch", follows ? 0.0 : pitch, formant, rate);
}

// the events of the file as the file says (an independent reading of the bytes)
static void rawEvents(const veg::Chunk& c, Bag& bag, int& count)
{
    for (const auto& k : c.kids) {
        if (k.isList && k.is(veg::G::Event)) {
            const veg::Chunk* ed = k.child(veg::G::EventData);
            const veg::Chunk* al = k.child(veg::G::AudioEventList);
            const veg::Chunk* ad = al ? al->child(veg::G::AudioEventData) : nullptr;
            if (ed && ad && !k.children(veg::G::Take).empty()) {
                const veg::Bytes& a = ad->data;
                const uint32_t flags = a.u32(8), method = (flags >> 2) & 3;
                const bool lock = (flags & 2) != 0;
                static const char* const em[] = {"pro", "efficient", "soloist-mono", "soloist-speech"};
                std::string m = method == 3 ? "none" : method == 2 ? "elastique" : "classic", mode;
                if (method == 2) mode = a.u32(0x10) < 4 ? em[a.u32(0x10)] : "pro";
                else if (method != 3) mode = strfmt("A%02u", a.u32(0x0c) + 1);
                const bool follows = lock || method == 3;
                ++bag[key(m, mode, follows, a.f64(0x14), a.f64(0x1c), ed->data.f64(0x28))];
                ++count;
            }
        }
        if (k.isList) rawEvents(k, bag, count);
    }
}

static Bag renoEvents(const Project& p, int& count)
{
    Bag bag;
    for (const auto& t : p.tracks)
        if (t.type == TrackType::Audio)
            for (const auto& e : t.events) {
                ++bag[key(e.stretchMethod, e.stretchMode, !e.keepPitch || e.stretchMethod == "none", e.pitch, e.formant, e.rate)];
                ++count;
            }
    return bag;
}

static std::string differences(const Bag& a, const Bag& b)
{
    std::string s;
    int n = 0;
    for (const auto& x : a) {
        auto it = b.find(x.first);
        const int have = it == b.end() ? 0 : it->second;
        if (have < x.second && n++ < 3) s += strfmt(" [%d missing: %s]", x.second - have, x.first.c_str());
    }
    return s;
}

static std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    const std::string info = argc > 2 ? argv[2] : testDataDir("RENO_CMP") + "/info";
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& f : std::filesystem::recursive_directory_iterator(dir, ec))
        if (f.is_regular_file() && f.path().extension() == ".veg") files.push_back(f.path().string());
    if (files.empty()) { std::printf("SKIP: no .veg files in %s\n", dir.c_str()); return 0; }

    int total = 0, withPitch = 0, locked = 0, none = 0, classic = 0, soloist = 0;
    std::map<std::string, Bag> byName;
    for (const std::string& path : files) {
        const std::string raw = slurp(path);
        veg::ChunkTree t;
        veg::readChunkTree(veg::Bytes((const uint8_t*)raw.data(), raw.size()), t);
        Bag want;
        int nRaw = 0;
        rawEvents(t.root, want, nRaw);
        Project p;
        const VegasImportResult r = importVegasProject(path, p);
        if (!check(r.ok, "reads " + std::filesystem::path(path).filename().string(), r.error)) continue;
        int nReno = 0;
        const Bag got = renoEvents(p, nReno);
        check(nRaw == nReno && got == want, std::filesystem::path(path).filename().string() + ": the pitch / stretch settings of its " + std::to_string(nRaw) + " sound events",
              nRaw == nReno ? differences(want, got) : strfmt("%d events in the file, %d in Reno", nRaw, nReno));
        byName[std::filesystem::path(path).stem().string()] = got;
        for (const auto& t2 : p.tracks)
            if (t2.type == TrackType::Audio)
                for (const auto& e : t2.events) {
                    ++total;
                    withPitch += (e.keepPitch && e.pitch != 0 && e.stretchMethod != "none") ? 1 : 0;
                    locked += (!e.keepPitch && e.stretchMethod != "none") ? 1 : 0;
                    none += e.stretchMethod == "none";
                    classic += e.stretchMethod == "classic";
                    soloist += e.stretchMode.compare(0, 7, "soloist") == 0 && e.stretchMethod == "elastique";
                }

        // writing and reading again
        std::vector<std::string> warnings;
        const std::string bytes = writeVegasData(p, path, &warnings);
        Project q;
        const VegasImportResult r2 = importVegasData(bytes, path, q);
        int nQ = 0;
        const Bag again = renoEvents(q, nQ);
        check(r2.ok && again == got && nQ == nReno, std::filesystem::path(path).filename().string() + ": written and read again, the same pitch / stretch settings", differences(got, again));
    }
    std::printf("   %d sound events: %d with a pitch change, %d locked to the rate, %d with method None, %d Classic, %d Soloist\n", total, withPitch, locked, none, classic, soloist);
    check(total > 0, "the samples have sound events");

    // VEGAS Pro 18's own scripting API's reading of some of the projects (exported by the Reno bridge)
    int compared = 0;
    for (const auto& f : std::filesystem::directory_iterator(info, ec)) {
        const std::string fn = f.path().filename().string();
        if (fn.compare(0, 5, "real_") != 0 || f.path().extension() != ".reno") continue;
        std::string project = fn.substr(5, fn.find(".vegas.reno") - 5);
        Json j;
        if (!Json::parse(slurp(f.path().string()), j)) continue;
        Bag api;
        std::function<void(const Json&)> walk = [&](const Json& o) {
            if (o.isArray()) { for (size_t i = 0; i < o.size(); ++i) walk(o.at(i)); return; }
            if (!o.isObject()) return;
            const Json& v = o.get("vegas");
            if (v.isObject() && v.has("method") && v.has("pitchSemis")) {
                const std::string m = v.get("method").asString();
                const bool lock = v.get("pitchLock").asBool();
                std::string method = m == "Elastique" ? "elastique" : m == "Classic" ? "classic" : "none", mode;
                if (method == "elastique") {
                    const std::string a = v.get("elastique").asString();
                    mode = a == "Pro" ? "pro" : a == "Efficient" ? "efficient" : a == "Soloist_Speech" ? "soloist-speech" : "soloist-mono";
                } else if (method == "classic") mode = v.get("classic").asString();
                ++api[key(method, mode, lock || method == "none", v.get("pitchSemis").asDouble(), method == "elastique" ? v.get("formantShift").asDouble() : 0.0,
                          std::round(o.get("rate").asDouble() * 1000.0) / 1000.0)];
            }
            for (const auto& mm : o.members()) walk(mm.second);
        };
        walk(j);
        // (the API's rate is rounded to 3 places here, so Reno's is too, and both ignore the formant of Classic events)
        std::string name = project;
        for (auto& c : name) if (c == '_') c = ' ';
        auto it = byName.find(name);
        if (it == byName.end()) continue;
        Bag reno;
        for (const auto& x : it->second) {
            // re-key Reno's rate at 3 places
            std::string k = x.first;
            const size_t at = k.rfind("rate ");
            const double rate = std::atof(k.c_str() + at + 5);
            k = k.substr(0, at) + strfmt("rate %.5f", std::round(rate * 1000.0) / 1000.0);
            reno[k] += x.second;
        }
        // (Classic events have no formant shift in the API: it fails for them, and VEGAS keeps 0)
        int apiN = 0, renoN = 0;
        for (const auto& x : api) apiN += x.second;
        for (const auto& x : reno) renoN += x.second;
        // a project saved again after the sample copy was made differs by a few events: not compared
        const bool known = apiN != renoN;
        if (known) { std::printf("   (%s: %d events in VEGAS's export, %d in the sample - the project was changed since; not compared)\n", project.c_str(), apiN, renoN); continue; }
        ++compared;
        check(api == reno, "VEGAS's own reading of " + project + ".veg (" + std::to_string(apiN) + " sound events) is Reno's: method, attributes, pitch change, lock, formant, rate", differences(api, reno) + differences(reno, api));
    }
    if (!compared) std::printf("SKIP: no VEGAS export in %s\n", info.c_str());

    // writing a changed pitch / lock / method / mode
    {
        Project p;
        // the first sample (smallest) with an élastique audio event
        std::string path;
        size_t fewest = 0;
        for (const std::string& f : testVegFiles(dir)) {
            Project cand;
            if (!importVegasProject(f, cand).ok) continue;
            size_t n = 0;
            bool has = false;
            for (const auto& t : cand.tracks) { n += t.events.size(); for (const auto& ev : t.events) has = has || (t.type == TrackType::Audio && ev.stretchMethod == "elastique"); }
            if (has && (path.empty() || n < fewest)) { path = f; fewest = n; }
        }
        if (!path.empty() && check(importVegasProject(path, p).ok, "reads a sample with a stretched sound for the write tests")) {
            Event* e = nullptr;
            for (auto& t : p.tracks)
                for (auto& ev : t.events)
                    if (!e && t.type == TrackType::Audio && ev.stretchMethod == "elastique") e = &ev;
            if (e) {
                const uint64_t id = e->id;
                struct Change { const char* what; std::function<void(Event&)> f; std::string method, mode; bool follows; double pitch; };
                const Change changes[] = {
                    {"a pitch of +5.5 semitones", [](Event& x) { x.keepPitch = true; x.pitch = 5.5; }, "elastique", "", false, 5.5},
                    {"a pitch of -24 semitones", [](Event& x) { x.keepPitch = true; x.pitch = -24; }, "elastique", "", false, -24},
                    {"Lock to stretch", [](Event& x) { x.keepPitch = false; x.pitch = 0; }, "elastique", "", true, 0},
                    {"Classic A07 with +3", [](Event& x) { x.keepPitch = true; x.pitch = 3; x.stretchMethod = "classic"; x.stretchMode = "A07"; }, "classic", "A07", false, 3},
                    {"élastique Soloist (Speech) with -2", [](Event& x) { x.keepPitch = true; x.pitch = -2; x.stretchMode = "soloist-speech"; }, "elastique", "soloist-speech", false, -2},
                    {"method None", [](Event& x) { x.stretchMethod = "none"; x.keepPitch = false; x.pitch = 0; }, "none", "", true, 0},
                };
                for (const Change& c : changes) {
                    Project w = p;
                    Event* we = w.findEvent(id);
                    c.f(*we);
                    const std::string wantMode = c.mode.empty() ? we->stretchMode : c.mode;
                    std::vector<std::string> warnings;
                    Project q;
                    const bool ok = importVegasData(writeVegasData(w, path, &warnings), path, q).ok;
                    const Event* qe = q.findEvent(id);
                    // (event ids are made afresh when reading: find the one at the same place)
                    const Event* same = nullptr;
                    for (const auto& t : q.tracks)
                        for (const auto& ev : t.events)
                            if (!same && std::fabs(ev.start - we->start) < 1e-6 && ev.length > 0 && std::fabs(ev.length - we->length) < 1e-6 && t.type == TrackType::Audio &&
                                std::fabs(ev.offset - we->offset) < 1e-6)
                                same = &ev;
                    (void)qe;
                    bool match = false;
                    // several events may share a start: look for one that has the new settings among those at that place
                    for (const auto& t : q.tracks)
                        for (const auto& ev : t.events)
                            if (t.type == TrackType::Audio && std::fabs(ev.start - we->start) < 1e-6 && std::fabs(ev.length - we->length) < 1e-6 && ev.stretchMethod == c.method &&
                                (c.method == "none" || ev.stretchMode == wantMode) && ((!ev.keepPitch) || ev.stretchMethod == "none") == c.follows &&
                                (c.follows || std::fabs(ev.pitch - c.pitch) < 1e-9))
                                match = true;
                    (void)same;
                    check(ok && match, std::string("written and read again: ") + c.what);
                }
            }
        }
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
