// editparity_replay: redoes the editing operations that tools/vegas/RenoEdits.cs did in VEGAS, in Reno, and writes what
// Reno ends up with in the same JSON form, so tools/vegas/editparity_compare.py can compare the two.
//
//   editparity_replay <folder with the scenario folders VEGAS made> <output folder>
//   editparity_replay --selftest <output folder>
//
// For every <scenario>/result.json (with its before.veg) this opens before.veg in Reno (the media files are not needed: the
// two test files are made up when Reno asks for them), does the listed operations in order and writes
// <output>/<scenario>/result.json with the snapshots of the same names ("before", "mid", "after").
//   - "api" operations (TrackEvent.Split ...) run through Reno's scripting engine (ScriptSession), the code a script gets;
//   - "ui" operations (Edit.Split, Edit.Trim.Start, Edit.Delete ...) run through the program's own edit commands
//     (AppState and the timeline), with the cursor, time selection and selection the operation lists.
// --selftest makes a project of Reno's own, writes its before.veg and a result.json in VEGAS's form for a few operations,
// replays them and checks the plumbing. Needs QT_QPA_PLATFORM=offscreen and the extra include folders of keys_test.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/engine/scriptsession.h"
#include "../../src/engine/scriptapi.h"
#include "../../src/core/json.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace reno;
using namespace reno::app;

namespace {

AppState& st() { return AppState::get(); }

int64_t ticks(double s) { return (int64_t)std::llround(s * 1e7); }
double secs(const Json& j) { return j.asDouble() / 1e7; }

// ---- made-up media: any .wav / .avi path is a 20 s file (the two test files RenoEdits.cs writes)
class FakeMedia : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t, int frames, int, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) l[k] = r[k] = 0.f;
        return true;
    }
};

std::unique_ptr<MediaReader> openFake(const std::string& path, std::string* error)
{
    std::string low = path;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    const bool wav = low.size() > 4 && low.compare(low.size() - 4, 4, ".wav") == 0;
    // (RenoEdits.cs falls back to a PNG sequence "media_sequence\seq_00000.png" when VEGAS refuses the AVI: 600 pictures, 30 fps)
    const bool avi = (low.size() > 4 && low.compare(low.size() - 4, 4, ".avi") == 0) ||
                     (low.find("media_sequence") != std::string::npos && low.size() > 4 && low.compare(low.size() - 4, 4, ".png") == 0);
    if (!wav && !avi) return openMediaOrProject(path, error);
    std::unique_ptr<FakeMedia> r(new FakeMedia());
    r->mi.path = path;
    r->mi.hasAudio = wav;
    r->mi.hasVideo = avi;
    r->mi.width = 64;
    r->mi.height = 48;
    r->mi.fps = 30;
    r->mi.duration = 20;
    r->mi.sampleRate = 48000;
    r->mi.channels = 2;
    return std::unique_ptr<MediaReader>(r.release());
}

// ---- the dump (VEGAS's names for curves and envelope kinds)
const char* curveName(int c)
{
    switch (c) {
    case CurveFast: return "Fast";
    case CurveSlow: return "Slow";
    case CurveSmooth: return "Smooth";
    case CurveSharp: return "Sharp";
    case CurveHold: return "None";
    default: return "Linear";
    }
}

const char* envName(const std::string& kind)
{
    if (kind == "volume") return "Volume";
    if (kind == "pan") return "Pan";
    if (kind == "composite") return "Composite";
    if (kind == "velocity") return "Velocity";
    if (kind == "mute") return "Mute";
    if (kind == "fadecolor") return "FadeToColor";
    return "Unknown";
}

Json dumpEnvelopes(const Envelopes& list)
{
    Json out = Json::array();
    for (const Envelope& env : list) {
        Json e = Json::object();
        e["type"] = envName(env.kind);
        Json pts = Json::array();
        for (const EnvPoint& p : env.points) {
            Json o = Json::object();
            o["x"] = ticks(p.time);
            o["y"] = p.value;
            o["curve"] = curveName(p.curve);
            pts.push(o);
        }
        e["points"] = pts;
        out.push(e);
    }
    return out;
}

Json dumpProject()
{
    AppState& app = st();
    const Project& p = app.project();
    Json o = Json::object();
    o["cursor"] = ticks(app.cursor());
    o["selectionStart"] = ticks(app.loopStart);
    o["selectionLength"] = app.loopEnd > app.loopStart ? ticks(app.loopEnd - app.loopStart) : (int64_t)0;
    std::map<uint64_t, int> groups;
    Json tracks = Json::array();
    int ti = 0;
    for (const Track& t : p.tracks) {
        Json jt = Json::object();
        jt["index"] = ti;
        jt["type"] = t.type == TrackType::Video ? "video" : "audio";
        jt["name"] = t.name;
        jt["mute"] = t.mute;
        jt["solo"] = t.solo;
        jt["envelopes"] = dumpEnvelopes(t.envelopes);
        Json evs = Json::array();
        int ei = 0;
        for (const Event& e : t.events) {
            Json je = Json::object();
            je["track"] = ti;
            je["index"] = ei;
            je["kind"] = t.type == TrackType::Video ? "video" : "audio";
            je["name"] = e.name;
            je["start"] = ticks(e.start);
            je["length"] = ticks(e.length);
            je["end"] = ticks(e.end());
            je["playbackRate"] = e.rate;
            je["mute"] = e.mute;
            je["locked"] = e.locked;
            je["loop"] = e.loop;
            je["selected"] = app.selection().count(e.id) != 0;
            int gid = -1;
            if (e.group) {
                auto it = groups.find(e.group);
                if (it == groups.end()) it = groups.emplace(e.group, (int)groups.size()).first;
                gid = it->second;
            }
            je["group"] = gid;
            Json fi = Json::object(), fo = Json::object();
            fi["length"] = ticks(e.fadeIn);
            fi["curve"] = curveName(e.fadeInCurve);
            fi["reciprocalCurve"] = curveName(e.fadeInRecipCurve >= 0 ? e.fadeInRecipCurve : CurveSlow);
            fo["length"] = ticks(e.fadeOut);
            fo["curve"] = curveName(e.fadeOutCurve);
            je["fadeIn"] = fi;
            je["fadeOut"] = fo;
            if (!e.transition.kind.empty()) je["transition"] = e.transition.name;
            Json takes = Json::array();
            const std::vector<Take> all = AppState::takesOf(e);
            for (size_t k = 0; k < all.size(); ++k) {
                Json jk = Json::object();
                jk["name"] = all[k].name;
                jk["active"] = (int)k == e.activeTake;
                // VEGAS's take offset is in timeline time (the media position divided by the rate)
                jk["offset"] = ticks(all[k].offset / (e.rate > 0 ? e.rate : 1.0));
                jk["mediaOffset"] = ticks(all[k].offset);
                jk["reversed"] = (int)k == e.activeTake ? e.reverse : all[k].reverse;
                takes.push(jk);
            }
            je["takes"] = takes;
            je["reversed"] = e.reverse;
            je["envelopes"] = dumpEnvelopes(e.envelopes);
            Json keys = Json::array();
            for (const PanCropKey& k : e.panCrop) {
                Json jk = Json::object();
                jk["position"] = ticks(k.time);
                keys.push(jk);
            }
            je["panCrop"] = keys;
            Json fx = Json::array();
            for (const FxSlot& s : e.fx) {
                Json jf = Json::object();
                jf["name"] = s.name;
                jf["bypass"] = s.bypass;
                fx.push(jf);
            }
            je["effects"] = fx;
            evs.push(je);
            ++ei;
        }
        jt["events"] = evs;
        tracks.push(jt);
        ++ti;
    }
    o["tracks"] = tracks;
    Json marks = Json::array(), regions = Json::array();
    for (const Project::Marker& m : p.markers) {
        Json jm = Json::object();
        jm["position"] = ticks(m.time);
        jm["label"] = m.name;
        if (m.region) { jm["length"] = ticks(m.length); regions.push(jm); }
        else marks.push(jm);
    }
    o["markers"] = marks;
    o["regions"] = regions;
    return o;
}

// ---- finding events by [track, index]
const Event* eventAt(int track, int index)
{
    const Project& p = st().project();
    if (track < 0 || track >= (int)p.tracks.size()) return nullptr;
    const auto& evs = p.tracks[(size_t)track].events;
    if (index < 0 || index >= (int)evs.size()) return nullptr;
    return &evs[(size_t)index];
}

std::set<uint64_t> selectionFrom(const Json& list)
{
    std::set<uint64_t> ids;
    for (const Json& pr : list.items())
        if (pr.size() == 2)
            if (const Event* e = eventAt(pr.at(0).asInt(), pr.at(1).asInt())) ids.insert(e->id);
    return ids;
}

// the cursor, time selection and selection an operation lists (what it does not list stays as it was)
void applyView(const Json& a)
{
    AppState& app = st();
    if (a.has("selected")) app.setSelection(selectionFrom(a.get("selected")));
    if (a.has("cursor")) app.setCursor(secs(a.get("cursor")));
    if (a.has("selectionStart")) {
        const double s = secs(a.get("selectionStart")), l = secs(a.get("selectionLength"));
        app.setLoop(s, s + l);
    }
    if (a.has("track")) {
        const int t = a.get("track").asInt();
        if (t >= 0 && t < (int)app.project().tracks.size()) app.currentTrack = app.project().tracks[(size_t)t].id;
    }
}

// "Cut, copy, and delete grouped events" (on by default): the groups of the selected events are selected too
void withGroups()
{
    AppState& app = st();
    std::set<uint64_t> sel = app.selection(), groups;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (sel.count(e.id) && e.group) groups.insert(e.group);
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (e.group && groups.count(e.group)) sel.insert(e.id);
    app.setSelection(sel);
}

std::string uiOp(TimelineWidget& tl, const std::string& op, const Json& a)
{
    AppState& app = st();
    applyView(a);
    if (op == "Edit.Split") app.splitAtCursor();
    else if (op == "Edit.Trim.Start") tl.trimStartToCursor();
    else if (op == "Edit.Trim.End") tl.trimEndToCursor();
    else if (op == "Edit.Trim") tl.trimToTimeSelection();
    else if (op == "Edit.Delete") { withGroups(); tl.deleteSelection(); }
    else if (op == "Edit.Cut") { withGroups(); tl.cutSelection(); }
    else if (op == "Edit.Copy") { withGroups(); app.copySelection(); }
    else if (op == "Edit.Paste") tl.paste();
    else if (op == "Edit.PasteInsert") tl.pasteInsert();
    else if (op == "Edit.Ripple.AffectedTracks") tl.postEditRipple(RippleMode::AffectedTracks);
    else if (op == "Edit.Ripple.All") tl.postEditRipple(RippleMode::AllTracks);
    else if (op == "Edit.EventHeal") app.healEvents(app.selection());
    else if (op == "Edit.CloseGaps") app.closeGaps(app.selection());
    else return "the replay does not know the command " + op;
    return std::string();
}

// ---- API operations through Reno's scripting engine
std::string apiOp(const std::string& op, const Json& a)
{
    AppState& app = st();
    ScriptHost host;
    ScriptSession s(app.project(), host);
    s.selection = app.selection();
    s.cursor = app.cursor();
    s.selStart = app.loopStart;
    s.selLength = std::max(0.0, app.loopEnd - app.loopStart);
    std::vector<uint64_t> tracks;
    s.list(1, RL_TRACKS, &tracks);
    const int ti = a.get("track").asInt(), ei = a.get("index").asInt();
    if (ti < 0 || ti >= (int)tracks.size()) return "no such track";
    std::vector<uint64_t> evs;
    s.list(tracks[(size_t)ti], RL_EVENTS, &evs);
    if (ei < 0 || ei >= (int)evs.size()) return "no such event";
    const uint64_t eh = evs[(size_t)ei];
    uint64_t out = 0;
    double d = 0;
    int rc = RS_OK;
    auto call = [&](int code, const std::vector<int64_t>& ia, const std::vector<double>& da) {
        return s.call(eh, code, ia, da, std::string(), &out, &d);
    };
    double startNow = 0, lengthNow = 0;
    s.getD(eh, RP_START, &startNow);
    s.getD(eh, RP_LENGTH, &lengthNow);
    if (op == "split") rc = call(RO_SPLIT, {}, {secs(a.get("offset"))});
    else if (op == "adjustStartLength")
        rc = call(RO_ADJUST_START_LENGTH, {a.get("adjustTakes").asBool() ? 1 : 0}, {secs(a.get("start")), secs(a.get("length"))});
    else if (op == "setLength") rc = s.setD(eh, RP_LENGTH, secs(a.get("length")));
    else if (op == "setStart") rc = s.setD(eh, RP_START, secs(a.get("start")));
    else if (op == "setEnd") rc = s.setD(eh, RP_END, secs(a.get("end")));
    else if (op == "setLoop") rc = s.setI(eh, RP_LOOP, a.get("loop").asBool() ? 1 : 0);
    else if (op == "setPlaybackRate") rc = s.setD(eh, RP_RATE, a.get("rate").asDouble());
    else if (op == "adjustPlaybackRate") rc = call(RO_ADJUST_RATE, {a.get("adjustStart").asBool() ? 1 : 0}, {a.get("rate").asDouble()});
    else if (op == "copy") {
        const int to = a.get("toTrack").asInt();
        if (to < 0 || to >= (int)tracks.size()) return "no such track";
        rc = call(RO_COPY, {(int64_t)tracks[(size_t)to]}, {secs(a.get("start"))});
    } else if (op == "remove") rc = s.call(tracks[(size_t)ti], RO_REMOVE_EVENT, {(int64_t)eh}, std::vector<double>(), std::string(), &out, &d);
    else return "the replay does not know the API operation " + op;
    if (rc != RS_OK) return s.message.empty() ? "the script engine refused (" + std::to_string(rc) + ")" : s.message;
    const Project result = s.project;
    app.edit("Script", [&](Project& p) { p = result; });
    return std::string();
}

// A "ui" operation the script could not get VEGAS to run ("viaApi": true) was done by RenoEdits.cs through the scripting API
// (ApiEquivalent: Split / TrimStart / TrimEnd / Trim / Delete on the selected events, or on the ones under the cursor when none is
// selected). The same is done here, so what the two programs' APIs do is compared rather than VEGAS's API against Reno's command.
std::string standInOp(const std::string& op, const Json& a)
{
    AppState& app = st();
    applyView(a);
    const double cur = app.cursor();
    const double ss = app.loopStart, sl = std::max(0.0, app.loopEnd - app.loopStart);
    auto targets = [&](double at) {
        std::vector<std::pair<int, int>> sel, under;
        const Project& p = app.project();
        for (size_t t = 0; t < p.tracks.size(); ++t)
            for (size_t k = 0; k < p.tracks[t].events.size(); ++k) {
                const Event& e = p.tracks[t].events[k];
                if (app.selection().count(e.id)) sel.push_back({(int)t, (int)k});
                else if (!e.locked && e.start < at - 1e-9 && e.end() > at + 1e-9) under.push_back({(int)t, (int)k});
            }
        std::vector<std::pair<int, int>> r = sel.empty() ? under : sel;
        std::sort(r.begin(), r.end(), [](const std::pair<int, int>& x, const std::pair<int, int>& y) { return x.first != y.first ? x.first < y.first : x.second > y.second; });
        return r;
    };
    auto args = [&](int t, int k) { Json j = Json::object(); j["track"] = t; j["index"] = k; return j; };
    if (op == "Edit.Split") {
        std::vector<double> points;
        if (sl > 0) { points.push_back(ss); points.push_back(ss + sl); } else points.push_back(cur);
        for (double pt : points)
            for (const auto& tk : targets(pt)) {
                const Event& e = app.project().tracks[(size_t)tk.first].events[(size_t)tk.second];
                if (!(pt > e.start && pt < e.end())) continue;
                Json j = args(tk.first, tk.second);
                j["offset"] = ticks(pt - e.start);
                const std::string f = apiOp("split", j);
                if (!f.empty()) return f;
            }
        return std::string();
    }
    if (op == "Edit.Trim.Start" || op == "Edit.Trim.End" || (op == "Edit.Trim" && sl > 0)) {
        const double at = op == "Edit.Trim" ? ss : cur;
        for (const auto& tk : targets(at)) {
            const Event e = app.project().tracks[(size_t)tk.first].events[(size_t)tk.second];
            double a0 = 0, b0 = 0;
            if (op == "Edit.Trim") { a0 = std::max(e.start, ss); b0 = std::min(e.end(), ss + sl); if (!(b0 > a0)) continue; }
            else {
                if (!(cur > e.start && cur < e.end())) continue;
                if (op == "Edit.Trim.Start") { a0 = cur; b0 = e.end(); } else { a0 = e.start; b0 = cur; }
            }
            Json j = args(tk.first, tk.second);
            j["start"] = ticks(a0);
            j["length"] = ticks(b0 - a0);
            j["adjustTakes"] = true;
            const std::string f = apiOp("adjustStartLength", j);
            if (!f.empty()) return f;
        }
        return std::string();
    }
    if (op == "Edit.Delete" && sl <= 0) {
        for (const auto& tk : targets(cur)) {
            const Event& e = app.project().tracks[(size_t)tk.first].events[(size_t)tk.second];
            if (!app.selection().count(e.id)) continue;
            const std::string f = apiOp("remove", args(tk.first, tk.second));
            if (!f.empty()) return f;
        }
        return std::string();
    }
    return "no API stand-in for " + op;
}

bool readFile(const std::string& path, std::string* text)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *text = ss.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << text;
    return true;
}

// Replays one scenario folder; returns false if it could not be done at all.
bool replay(TimelineWidget& tl, const std::string& inDir, const std::string& outDir, const std::string& name)
{
    AppState& app = st();
    std::string text, err;
    Json vegasResult;
    if (!readFile(inDir + "/" + name + "/result.json", &text) || !Json::parse(text, vegasResult, &err)) {
        std::printf("%s: no readable result.json (%s)\n", name.c_str(), err.c_str());
        return false;
    }
    Json result = Json::object();
    result["scenario"] = name;
    result["program"] = "Reno";
    Json opsOut = Json::array();
    const std::string before = inDir + "/" + name + "/before.veg";
    if (!app.open(before, &err)) {
        result["error"] = "Reno could not open before.veg: " + err;
        QDir().mkpath(QString::fromStdString(outDir + "/" + name));
        writeFile(outDir + "/" + name + "/result.json", result.dump(1));
        std::printf("%s: %s\n", name.c_str(), err.c_str());
        return false;
    }
    app.setSelection(std::set<uint64_t>());
    app.setCursor(0);
    app.setLoop(0, 0);
    // The files themselves are not here (the .veg names them where VEGAS had them): make them the 20 s files RenoEdits.cs writes,
    // so that loops, trims past the media and the like see the right length.
    {
        std::vector<std::pair<uint64_t, MediaInfo>> made;
        for (const MediaItem& m : app.project().media) {
            std::string e;
            std::unique_ptr<MediaReader> r = openFake(m.path, &e);
            const std::string low = [&] { std::string l = m.path; for (char& c : l) c = (char)std::tolower((unsigned char)c); return l; }();
            const bool ours = low.size() > 4 && (low.compare(low.size() - 4, 4, ".wav") == 0 || low.compare(low.size() - 4, 4, ".avi") == 0 ||
                                                 (low.find("media_sequence") != std::string::npos && low.compare(low.size() - 4, 4, ".png") == 0));
            if (r && ours) made.push_back({m.id, r->info()});
        }
        if (!made.empty())
            app.edit("Replay media", [&](Project& p) {
                for (const auto& mk : made)
                    if (MediaItem* m = p.findMedia(mk.first)) { m->info = mk.second; m->offline = false; }
            });
    }
    if (std::getenv("REPLAY_DEBUG")) for (const MediaItem& m : app.project().media) std::printf("  media %s duration %.4f still %d generated %d offline %d\n", m.path.c_str(), m.info.duration, (int)m.info.isStill, (int)m.generated, (int)m.offline);
    for (const Json& op : vegasResult.get("ops").items()) {
        const std::string kind = op.get("kind").asString(), what = op.get("op").asString();
        Json o = Json::object();
        o["kind"] = kind;
        o["op"] = what;
        std::string failure;
        if (kind == "snapshot") result[what] = dumpProject();
        else if (kind == "ui") failure = op.get("viaApi").asBool(false) ? standInOp(what, op.get("args")) : uiOp(tl, what, op.get("args"));
        else if (kind == "api") failure = apiOp(what, op.get("args"));
        // (what VEGAS refused is not tried: the project is as VEGAS's was)
        if (kind != "snapshot" && !op.get("ok").asBool(true)) failure = "VEGAS refused this operation: " + op.get("error").asString();
        o["ok"] = failure.empty();
        if (!failure.empty()) o["error"] = failure;
        opsOut.push(o);
        uitest::settle(10);
    }
    result["ops"] = opsOut;
    if (!result.has("before")) result["before"] = dumpProject();
    QDir().mkpath(QString::fromStdString(outDir + "/" + name));
    writeFile(outDir + "/" + name + "/result.json", result.dump(1));
    std::printf("%s: replayed %d operations\n", name.c_str(), (int)opsOut.size());
    return true;
}

decltype(AppState::mediaOpener) g_testOpener;

// ---- --selftest: a project of Reno's own and a few operations written the way RenoEdits.cs writes them
int selftest(TimelineWidget& tl, const std::string& out)
{
    AppState& app = st();
    app.mediaOpener = g_testOpener;
    app.newProject();
    std::string err;
    const uint64_t wav = app.importMedia("synthaudio:1", &err);
    if (!wav) { std::printf("FAIL no media: %s\n", err.c_str()); return 1; }
    const std::string dir = out + "/in/selftest_split";
    QDir().mkpath(QString::fromStdString(dir));
    app.addTrack(TrackType::Audio);
    const std::vector<uint64_t> made = app.addMediaToTimeline(wav, 2.0);
    app.edit("Setup", [&](Project& p) {
        for (Track& t : p.tracks)
            for (Event& e : t.events) { e.length = 8; e.offset = 3; e.fadeIn = 1; e.fadeOut = 2; }
    });
    if (!app.save(dir + "/before.veg", &err)) { std::printf("FAIL cannot save before.veg: %s\n", err.c_str()); return 1; }
    int at = -1;
    for (size_t k = 0; k < app.project().tracks.size(); ++k) if (!app.project().tracks[k].events.empty()) at = (int)k;
    char json[2048];
    std::snprintf(json, sizeof json,
                  "{\"scenario\":\"selftest_split\",\"ops\":["
                  "{\"kind\":\"snapshot\",\"op\":\"before\"},"
                  "{\"kind\":\"api\",\"op\":\"split\",\"args\":{\"track\":%d,\"index\":0,\"offset\":30000000},\"ok\":true},"
                  "{\"kind\":\"ui\",\"op\":\"Edit.Split\",\"args\":{\"cursor\":90000000,\"selected\":[[%d,1]]},\"ok\":true},"
                  "{\"kind\":\"snapshot\",\"op\":\"after\"}]}",
                  at, at);
    writeFile(dir + "/result.json", json);
    if (!replay(tl, out + "/in", out + "/out", "selftest_split")) return 1;
    std::string text;
    Json r;
    if (!readFile(out + "/out/selftest_split/result.json", &text) || !Json::parse(text, r, &err)) { std::printf("FAIL no output\n"); return 1; }
    const Json& evs = r.get("after").get("tracks").at((size_t)at).get("events");
    uitest::check(evs.size() == 3, "selftest: two splits make three events", std::to_string(evs.size()));
    if (evs.size() == 3) {
        uitest::check(evs.at(0).get("length").asInt64() == 30000000 && evs.at(1).get("length").asInt64() == 40000000 &&
                          evs.at(2).get("length").asInt64() == 10000000,
                      "selftest: the halves are 3 s, 4 s and 1 s long");
        uitest::check(evs.at(0).get("end").asInt64() == evs.at(1).get("start").asInt64(), "selftest: the halves meet");
        uitest::check(evs.at(0).get("fadeOut").get("length").asInt64() == 0 && evs.at(2).get("fadeIn").get("length").asInt64() == 0,
                      "selftest: no fade at the cut");
    }
    return uitest::summary();
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoEditParity");
    QApplication::setApplicationName("editparity_replay");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) {
        std::printf("usage: editparity_replay <folder of scenario folders> <output folder>\n       editparity_replay --selftest <output folder>\n");
        return 2;
    }
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    g_testOpener = st().mediaOpener;
    st().mediaOpener = openFake;
    st().newProject();
    Playback playback;
    TimelineWidget tl(&playback);
    tl.resize(1400, 520);
    tl.show();
    uitest::settle(100);
    if (args[1] == "--selftest") return selftest(tl, args[2].toStdString());
    const std::string in = args[1].toStdString(), out = args[2].toStdString();
    QDir().mkpath(QString::fromStdString(out));
    int failed = 0, done = 0;
    for (const QFileInfo& fi : QDir(args[1]).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!QFile::exists(fi.absoluteFilePath() + "/result.json")) continue;
        ++done;
        if (!replay(tl, in, out, fi.fileName().toStdString())) ++failed;
    }
    std::printf("%d scenarios, %d could not be replayed\n", done, failed);
    return failed ? 1 : 0;
}
