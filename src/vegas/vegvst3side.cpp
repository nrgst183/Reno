// See vegvst3side.h: VST3 effects kept beside a .veg, because VEGAS Pro has no way to store them in one.
#include "vegvst3side.h"
#include "../core/base.h"
#include "../core/json.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>

namespace reno {

namespace {

const char* const kMagic = "reno-vst3-side";

bool isVst3(const FxSlot& s) { return s.kind == "audio" && s.audio.format == "VST3"; }

Json slotJson(const FxSlot& s, size_t index)
{
    Json j = Json::object();
    j["index"] = (double)index;                 // its place in the chain it was in, VST3 slots counted
    j["name"] = s.name;
    j["bypass"] = s.bypass;
    if (s.bypassAutomation) j["bypassAutomation"] = true;
    j["audio"] = s.audio.toJson();
    j["state"] = s.state;
    return j;
}

FxSlot slotOf(const Json& j, Project& p)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.name = j.get("name").asString();
    s.bypass = j.get("bypass").asBool();
    s.bypassAutomation = j.get("bypassAutomation").asBool();
    s.audio = AudioPluginDesc::fromJson(j.get("audio"));
    s.state = j.get("state");
    return s;
}

// One chain's VST3 slots as a JSON array of {index, ...}; null if it has none.
bool chainSlots(const FxChain& c, Json& out)
{
    out = Json::array();
    for (size_t i = 0; i < c.size(); ++i)
        if (isVst3(c[i])) out.push(slotJson(c[i], i));
    return out.size() > 0;
}

// Puts slots back into a chain at the places they had (they come in ascending order, so each lands after the ones before).
int restore(FxChain& c, const Json& slots, Project& p)
{
    int n = 0;
    for (const Json& js : slots.items()) {
        FxSlot s = slotOf(js, p);
        size_t at = (size_t)std::max(0, js.get("index").asInt());
        if (at > c.size()) at = c.size();
        c.insert(c.begin() + (std::ptrdiff_t)at, s);
        ++n;
    }
    return n;
}

}  // namespace

int countVst3(const Project& p)
{
    auto n = [](const FxChain& c) { int k = 0; for (const auto& s : c) k += isVst3(s) ? 1 : 0; return k; };
    int k = n(p.audioOut);
    for (const auto& t : p.tracks) { k += n(t.fx); for (const auto& e : t.events) k += n(e.fx); }
    for (const auto& b : p.buses) k += n(b.fx);
    return k;
}

std::string vst3SidePath(const std::string& vegPath) { return vegPath + ".reno-vst3"; }

int writeVst3Side(const Project& p, const std::string& vegPath, std::vector<std::string>* warnings)
{
    Json root = Json::object();
    root["format"] = kMagic;
    root["version"] = 1;
    int count = 0;

    Json tracks = Json::array();
    for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& t = p.tracks[ti];
        Json jt = Json::object();
        Json s;
        bool any = false;
        jt["track"] = (double)ti;
        jt["type"] = t.type == TrackType::Video ? "video" : "audio";
        if (chainSlots(t.fx, s)) { jt["fx"] = s; count += (int)s.size(); any = true; }
        Json evs = Json::array();
        for (size_t ei = 0; ei < t.events.size(); ++ei) {
            const Event& e = t.events[ei];
            if (!chainSlots(e.fx, s)) continue;
            Json je = Json::object();
            je["event"] = (double)ei;
            je["start"] = e.start;
            je["length"] = e.length;
            je["fx"] = s;
            evs.push(je);
            count += (int)s.size();
        }
        if (evs.size() > 0) { jt["events"] = evs; any = true; }
        if (any) tracks.push(jt);
    }
    if (tracks.size() > 0) root["tracks"] = tracks;

    Json buses = Json::array();
    for (size_t bi = 0; bi < p.buses.size(); ++bi) {
        Json s;
        if (!chainSlots(p.buses[bi].fx, s)) continue;
        Json jb = Json::object();
        jb["bus"] = (double)bi;
        jb["fx"] = s;
        buses.push(jb);
        count += (int)s.size();
    }
    if (buses.size() > 0) root["buses"] = buses;
    Json m;
    if (chainSlots(p.audioOut, m)) { root["master"] = m; count += (int)m.size(); }

    namespace fs = std::filesystem;
    const std::string side = vst3SidePath(vegPath);
    std::error_code ec;
    if (count == 0) {
        // nothing to keep: an old file would bring back effects that were removed
        fs::remove(fs::u8path(side), ec);
        return 0;
    }
    if (!writeFile(side, root.dump(1))) {
        if (warnings)
            warnings->push_back("The VST3 audio effects could not be kept: the file " + side + " could not be written. They are not in the .veg "
                                "(VEGAS has no VST3 support) and will be missing when the project is opened again.");
        return 0;
    }
    return count;
}

int readVst3Side(const std::string& vegPath, Project& p, std::vector<std::string>* notes)
{
    std::string text;
    if (!readFile(vst3SidePath(vegPath), text)) return 0;
    Json root;
    std::string err;
    if (!Json::parse(text, root, &err) || root.get("format").asString() != kMagic) {
        if (notes) notes->push_back("The file " + vst3SidePath(vegPath) + " (VST3 effects kept by Reno) could not be read; its effects are not in the project.");
        return 0;
    }
    int restored = 0, lost = 0;
    auto lose = [&](const Json& slots) { lost += (int)slots.size(); };

    for (const Json& jt : root.get("tracks").items()) {
        const int ti = jt.get("track").asInt(-1);
        const bool typeOk = ti >= 0 && ti < (int)p.tracks.size() &&
                            (p.tracks[(size_t)ti].type == TrackType::Video) == (jt.get("type").asString() == "video");
        if (!typeOk) {
            lose(jt.get("fx"));
            for (const Json& je : jt.get("events").items()) lose(je.get("fx"));
            continue;
        }
        Track& t = p.tracks[(size_t)ti];
        if (jt.has("fx")) restored += restore(t.fx, jt.get("fx"), p);
        for (const Json& je : jt.get("events").items()) {
            // the event at the same place in the track if it has the same start and length, else the first with them
            const double st = je.get("start").asDouble(), ln = je.get("length").asDouble();
            const int saved = je.get("event").asInt(-1);
            Event* found = nullptr;
            for (size_t i = 0; i < t.events.size(); ++i) {
                Event& e = t.events[i];
                if (std::fabs(e.start - st) > 1e-6 || std::fabs(e.length - ln) > 1e-6) continue;
                if (!found || (int)i == saved) found = &e;
                if ((int)i == saved) break;
            }
            if (found) restored += restore(found->fx, je.get("fx"), p);
            else lose(je.get("fx"));
        }
    }
    for (const Json& jb : root.get("buses").items()) {
        const int bi = jb.get("bus").asInt(-1);
        if (bi >= 0 && bi < (int)p.buses.size()) restored += restore(p.buses[(size_t)bi].fx, jb.get("fx"), p);
        else lose(jb.get("fx"));
    }
    if (root.has("master")) restored += restore(p.audioOut, root.get("master"), p);

    if (notes) {
        if (restored > 0)
            notes->push_back(strfmt("%d VST3 audio effect(s) were put back from %s (VEGAS Pro cannot keep VST3 effects in a .veg, so VEGAS does not show them).",
                                    restored, std::filesystem::u8path(vst3SidePath(vegPath)).filename().u8string().c_str()));
        if (lost > 0)
            notes->push_back(strfmt("%d VST3 audio effect(s) in %s could not be placed: the track or event they belonged to is not in this project any more.",
                                    lost, std::filesystem::u8path(vst3SidePath(vegPath)).filename().u8string().c_str()));
    }
    return restored;
}

}  // namespace reno
