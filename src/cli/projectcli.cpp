// reno_cli's project commands: a whole project can be made and changed from the command line, the editor's own way
// (AppState's commands, the same plug-ins and their settings), and then rendered with reno_cli as usual.
//
//   reno_cli new FILE [options] [+ COMMAND ...]       a new project (.reno or .veg), then the commands, then saved
//   reno_cli edit FILE COMMAND [+ COMMAND ...]        opens FILE, runs the commands, saves it (or -o OTHER)
//   reno_cli edit FILE --script CMDS.txt              the commands one a line (# comments)
//   reno_cli info FILE [--params]                     what is in a project
//   reno_cli fx-list [TEXT] [--video | --audio | --transitions | --generators]
//   reno_cli fx-params PLUGIN                         a plug-in's settings: names, kinds, ranges, choices, defaults
//
// The commands are in kProjectCliHelp. Effect settings are set through a live instance of the plug-in, as the effects
// window sets them, so plug-ins that change one setting when another changes do so; the project file holds them as
// the editor saves them.
#include "projectcli.h"
#include "../app/appstate.h"
#include "../audiofx/audioplugin.h"
#include "../engine/catalog.h"
#include "../engine/fxruntime.h"
#include "../engine/timefmt.h"
#include "../ofx/ofxhost.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace reno {

using app::AppState;
using app::FxTarget;

const char* kProjectCliHelp =
    "Making and changing projects:\n"
    "  reno_cli new FILE [--size WxH] [--fps R] [--sample-rate HZ] [--channels 2|6] [--quality Q] [--vegas-defaults] [-y]\n"
    "               [+ COMMAND ...]\n"
    "                           a new project (FILE .reno or .veg), then the commands, then saved. Without --size and\n"
    "                           the others: the editor's new-project settings (Preferences), or VEGAS's with --vegas-defaults\n"
    "  reno_cli edit FILE [-o OTHER] COMMAND [ARGS] [+ COMMAND [ARGS] ...]\n"
    "  reno_cli edit FILE [-o OTHER] --script CMDS.txt\n"
    "                           opens FILE, runs the commands in order and saves it (to OTHER with -o). Commands are\n"
    "                           separated by a lone +, or are one a line in CMDS.txt (# starts a comment; \"quotes\" keep\n"
    "                           spaces). If one fails nothing is saved.\n"
    "  reno_cli info FILE [--params]   tracks, events, media and effects (--params: every effect's settings)\n"
    "  reno_cli fx-list [TEXT] [--video | --audio | --transitions | --generators]\n"
    "                           the plug-ins that can be used (whose name or identifier contains TEXT)\n"
    "  reno_cli fx-params PLUGIN       a plug-in's settings: name, kind, range or choices, default\n"
    "  Render the result as usual: reno_cli FILE -o out.mp4\n"
    "\n"
    "Commands (times: seconds, hh:mm:ss.sss, hh:mm:ss;ff or 300f):\n"
    "  settings [--size WxH] [--fps R] [--sample-rate HZ] [--channels 2|6] [--quality draft|preview|good|best]\n"
    "           [--pixel-format 8bit|32bit]\n"
    "  add-track video|audio [--name TEXT] [--at N]        a track (at position N from 1; default: the end)\n"
    "  track N [--name TEXT] [--mute on|off] [--solo on|off] [--volume DB] [--opacity 0..1] [--pan -1..1]\n"
    "  remove-track N\n"
    "  add FILE [--track N] [--at T] [--length T] [--from T] [--video-only | --audio-only] [--name TEXT]\n"
    "                           puts a media file on the timeline as the editor does (a video with sound makes a video\n"
    "                           and an audio event, grouped); --from: where in the media it begins. Prints the events.\n"
    "  generate PLUGIN [--track N] [--at T] [--length T] [SETTING=VALUE ...]\n"
    "                           generated media (Solid Color, Titles & Text ...) on the timeline, with its settings\n"
    "  event E [--start T] [--length T] [--from T] [--name TEXT] [--mute on|off] [--fade-in T] [--fade-out T]\n"
    "          [--rate R] [--pitch SEMITONES]\n"
    "  remove E\n"
    "  fx TARGET PLUGIN [--raw] [SETTING=VALUE ...]       adds an effect at the end of TARGET's chain\n"
    "  set TARGET N [--raw] SETTING=VALUE ...              changes the N-th effect (from 1) of TARGET's chain\n"
    "  remove-fx TARGET N, bypass TARGET N on|off\n"
    "  transition E PLUGIN [SETTING=VALUE ...] | none      the transition where event E overlaps the one before it\n"
    "  marker T [NAME], region START END [NAME]\n"
    "  info [--params]\n"
    "\n"
    "  E, an event: its id (as add and info print it) or TRACK.INDEX (2.1: the first event of track 2)\n"
    "  TARGET: an event (E or event:E), track:N, video-out, master (the audio output), bus:A (or bus:N),\n"
    "          transition:E (the transition into event E; set without N), generator:E (the generated media event E\n"
    "          shows; set without N)\n"
    "  PLUGIN: a name or identifier as fx-list shows it (\"Gaussian Blur\", \"eFX Compressor\"; enough of it to be one)\n"
    "  SETTING=VALUE: a setting by its name or label (fx-params). Numbers; x,y or x,y,z; colours #rrggbb[aa] or r,g,b[,a]\n"
    "          (0..1); on/off; a choice by its label or its number from 0; text. SETTING@T=VALUE sets a keyframe at T\n"
    "          (from the event's start for an event's effects, from the timeline's start otherwise).\n"
    "          Audio effects: VALUE as the plug-in shows it (-20 for \"-20.0 dB\", or the text of a choice); --raw: the\n"
    "          plug-in's own 0..1 value.\n"
    "\n"
    "Examples:\n"
    "  reno_cli new clip.reno --size 1920x1080 --fps 29.97 + add intro.mp4 + add music.wav --at 0 --track 2\n"
    "  reno_cli edit clip.reno fx 1.1 \"Gaussian Blur\" \"Horizontal Range=0.02\" + fx master \"eFX Limiter\" Threshold=-6\n"
    "  reno_cli edit clip.reno generate \"Titles & Text\" --track 1 --at 2 --length 4 \"Text=Hello\" Scale@0=0.5 Scale@2=1.5\n"
    "  reno_cli edit clip.reno set generator:1.2 \"Text color=#ffcc00\" + transition 1.2 \"3D Blinds\" + info\n"
    "  reno_cli clip.reno -o clip.mp4\n";

namespace {

constexpr int kOk = 0, kFailed = 1, kUsage = 2, kCantOpen = 3, kExists = 5;

bool gQuietCli = false;

// (some plug-ins have placeholder settings named "-" or nothing)
bool shownAudioParam(AudioPlugin& pl, int i)
{
    const std::string n = pl.paramName(i);
    return n.find_first_not_of(" -_") != std::string::npos;
}
void say(const std::string& s)
{
    if (gQuietCli) return;
    std::fprintf(stderr, "%s\n", s.c_str());
    std::fflush(stderr);
}
void problem(const std::string& s)
{
    std::fprintf(stderr, "reno_cli: %s\n", s.c_str());
    std::fflush(stderr);
}
// results (ids and lists) go to stdout
void result(const std::string& s)
{
    std::fprintf(stdout, "%s\n", s.c_str());
    std::fflush(stdout);
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// letters and digits only, in lower case: "Horizontal Range" and "horizontalrange" match
std::string squash(const std::string& s)
{
    std::string o;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) o += (char)std::tolower((unsigned char)c);
    return o;
}
bool contains(const std::string& hay, const std::string& needle) { return lower(hay).find(lower(needle)) != std::string::npos; }

std::string fmt(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

bool number(const std::string& t, double* v)
{
    if (t.empty()) return false;
    char* end = nullptr;
    const double x = std::strtod(t.c_str(), &end);
    if (!end || *end) return false;
    *v = x;
    return true;
}
bool wholeNumber(const std::string& t, long* v)
{
    if (t.empty()) return false;
    char* end = nullptr;
    const long x = std::strtol(t.c_str(), &end, 10);
    if (!end || *end) return false;
    *v = x;
    return true;
}
bool onOff(const std::string& t, bool* v)
{
    const std::string s = lower(t);
    if (s == "on" || s == "true" || s == "yes" || s == "1") { *v = true; return true; }
    if (s == "off" || s == "false" || s == "no" || s == "0") { *v = false; return true; }
    return false;
}

// Seconds, hh:mm:ss.sss, hh:mm:ss;ff (at the project's rate) or 300f (frames); as reno_cli's --start.
bool parseTime(const ProjectSettings& ps, const std::string& text, double* seconds)
{
    if (text.empty()) return false;
    char* end = nullptr;
    if (text.back() == 'f' || text.back() == 'F') {
        const double frames = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() - 1 || frames < 0) return false;
        *seconds = frames / ps.fps();
        return true;
    }
    const double v = std::strtod(text.c_str(), &end);
    if (end && *end == 0) {
        if (v < 0) return false;
        *seconds = v;
        return true;
    }
    double shown = 0;
    const std::string format = text.find(';') != std::string::npos ? "timecode" : "time";
    if (!parseTimeText(ps, text, format, &shown) && !parseTimeText(ps, text, "timecode", &shown)) return false;
    *seconds = std::max(0.0, shown - ps.rulerStart);
    return true;
}

// 29.97 -> 30000/1001 (and the other NTSC rates), 25 -> 25/1, 30000/1001 as given
bool parseFps(const std::string& text, int* num, int* den)
{
    const size_t slash = text.find('/');
    if (slash != std::string::npos) {
        long a = 0, b = 0;
        if (!wholeNumber(text.substr(0, slash), &a) || !wholeNumber(text.substr(slash + 1), &b) || a < 1 || b < 1 || a > 1000000 || b > 1000000) return false;
        *num = (int)a;
        *den = (int)b;
        return (double)a / b <= 1000.0;
    }
    double f = 0;
    if (!number(text, &f) || !(f > 0 && f <= 1000)) return false;
    for (int base : {24, 30, 48, 60, 120, 240})
        if (std::fabs(f - base * 1000.0 / 1001.0) < 0.006) {
            *num = base * 1000;
            *den = 1001;
            return true;
        }
    if (std::fabs(f - std::round(f)) < 1e-6) {
        *num = (int)std::lround(f);
        *den = 1;
        return true;
    }
    *num = (int)std::lround(f * 1000.0);
    *den = 1000;
    return true;
}

bool parseSize(const std::string& t, int* w, int* h)
{
    const size_t x = lower(t).find('x');
    long a = 0, b = 0;
    if (x == std::string::npos || !wholeNumber(t.substr(0, x), &a) || !wholeNumber(t.substr(x + 1), &b) || a < 16 || b < 16 || a > 16384 || b > 16384) return false;
    *w = (int)a;
    *h = (int)b;
    return true;
}

// A command line split shell-fashion: blanks separate, "double quotes" keep blanks (\" inside them is a quote).
// Backslashes are kept as they are (Windows paths).
std::vector<std::string> splitLine(const std::string& line, bool* ok)
{
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false, have = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuote) {
            if (c == '\\' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (c == '"') inQuote = false;
            else cur += c;
        } else if (c == '"') {
            inQuote = true;
            have = true;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (have || !cur.empty()) out.push_back(cur);
            cur.clear();
            have = false;
        } else if (c == '#' && cur.empty() && !have) {
            break;      // a comment to the end of the line
        } else {
            cur += c;
        }
    }
    if (have || !cur.empty()) out.push_back(cur);
    if (ok) *ok = !inQuote;
    return out;
}

// ------------------------------------------------------------------------------------------------ the project
class Session {
public:
    Session(AppState& app) : app_(app) {}

    AppState& app_;

    Project& pj() { return const_cast<Project&>(app_.project()); }
    const ProjectSettings& st() { return app_.project().settings; }

    // ---- finding things
    Track* trackAt(const std::string& t, std::string* why)
    {
        long n = 0;
        Project& p = pj();
        if (wholeNumber(t, &n)) {
            if (n >= 1 && n <= (long)p.tracks.size()) return &p.tracks[(size_t)n - 1];
            *why = "there is no track " + t + " (the project has " + std::to_string(p.tracks.size()) + ")";
            return nullptr;
        }
        for (auto& tr : p.tracks)
            if (lower(tr.name) == lower(t)) return &tr;
        *why = "no track is called \"" + t + "\"";
        return nullptr;
    }
    int trackNumber(uint64_t id)
    {
        const Project& p = app_.project();
        for (size_t i = 0; i < p.tracks.size(); ++i)
            if (p.tracks[i].id == id) return (int)i + 1;
        return 0;
    }
    // the events of a track in timeline order
    static std::vector<const Event*> ordered(const Track& t)
    {
        std::vector<const Event*> v;
        for (const auto& e : t.events) v.push_back(&e);
        std::stable_sort(v.begin(), v.end(), [](const Event* a, const Event* b) { return a->start < b->start; });
        return v;
    }
    // "2.1" (track 2's first event) or an event id
    Event* eventAt(const std::string& t0, std::string* why)
    {
        std::string t = t0;
        if (t.rfind("event:", 0) == 0) t = t.substr(6);
        Project& p = pj();
        const size_t dot = t.find('.');
        if (dot != std::string::npos) {
            long ti = 0, ei = 0;
            if (!wholeNumber(t.substr(0, dot), &ti) || !wholeNumber(t.substr(dot + 1), &ei)) { *why = "\"" + t0 + "\" is not an event (an id or TRACK.INDEX)"; return nullptr; }
            if (ti < 1 || ti > (long)p.tracks.size()) { *why = "there is no track " + std::to_string(ti); return nullptr; }
            Track& tr = p.tracks[(size_t)ti - 1];
            const std::vector<const Event*> v = ordered(tr);
            if (ei < 1 || ei > (long)v.size()) { *why = "track " + std::to_string(ti) + " has " + std::to_string(v.size()) + " events, not " + std::to_string(ei); return nullptr; }
            return p.findEvent(v[(size_t)ei - 1]->id);
        }
        long id = 0;
        if (wholeNumber(t, &id) && id > 0)
            if (Event* e = p.findEvent((uint64_t)id)) return e;
        *why = "there is no event " + t0;
        return nullptr;
    }
    std::string eventPlace(uint64_t id)
    {
        const Project& p = app_.project();
        for (size_t i = 0; i < p.tracks.size(); ++i) {
            const std::vector<const Event*> v = ordered(p.tracks[i]);
            for (size_t k = 0; k < v.size(); ++k)
                if (v[k]->id == id) return std::to_string(i + 1) + "." + std::to_string(k + 1);
        }
        return "?";
    }

    struct Target {
        FxTarget t;
        bool video = true;
        bool single = false;        // a transition or a generator: one effect, no chain
        double keyOrigin = 0;       // where keyframe times count from (an event's start)
        std::string name;
    };
    bool target(const std::string& s, Target* out, std::string* why)
    {
        Project& p = pj();
        const std::string l = lower(s);
        if (l == "video-out" || l == "video" || l == "videoout") {
            out->t.kind = FxTarget::VideoOut;
            out->name = "Video Output";
            return true;
        }
        if (l == "master" || l == "audio-out" || l == "audioout" || l == "audio") {
            out->t.kind = FxTarget::AudioOut;
            out->video = false;
            out->name = "Master";
            return true;
        }
        if (l.rfind("track:", 0) == 0) {
            Track* tr = trackAt(s.substr(6), why);
            if (!tr) return false;
            out->t.kind = FxTarget::Track;
            out->t.id = tr->id;
            out->video = tr->type == TrackType::Video;
            out->name = "Track " + std::to_string(trackNumber(tr->id));
            return true;
        }
        if (l.rfind("bus:", 0) == 0) {
            const std::string b = s.substr(4);
            long n = 0;
            for (size_t i = 0; i < p.buses.size(); ++i) {
                const std::string letter(1, (char)('A' + (int)i));
                if (lower(b) == lower(letter) || lower(b) == lower(p.buses[i].name) || (wholeNumber(b, &n) && n == (long)i + 1)) {
                    out->t.kind = FxTarget::Bus;
                    out->t.id = p.buses[i].id;
                    out->video = false;
                    out->name = "Bus " + letter;
                    return true;
                }
            }
            *why = "there is no bus " + b + " (the project has " + std::to_string(p.buses.size()) + ")";
            return false;
        }
        const bool trans = l.rfind("transition:", 0) == 0, gen = l.rfind("generator:", 0) == 0;
        const std::string ev = trans ? s.substr(11) : gen ? s.substr(10) : s;
        Event* e = eventAt(ev, why);
        if (!e) {
            if (!trans && !gen && ev.find(':') == std::string::npos && ev.find('.') == std::string::npos)
                *why = "\"" + s + "\" is not a target (an event, track:N, video-out, master, bus:A, transition:E, generator:E)";
            return false;
        }
        Track* tr = nullptr;
        p.findEvent(e->id, &tr);
        out->video = tr && tr->type == TrackType::Video;
        out->keyOrigin = e->start;
        if (trans) {
            if (!out->video) { *why = "only video events have transitions"; return false; }
            out->t.kind = FxTarget::Transition;
            out->t.id = e->id;
            out->single = true;
            out->name = "transition into event " + eventPlace(e->id);
            return true;
        }
        if (gen) {
            const MediaItem* m = p.findMedia(e->mediaId);
            if (!m || !m->generated) { *why = "event " + ev + " does not show generated media"; return false; }
            out->t.kind = FxTarget::Generator;
            out->t.id = m->id;
            out->single = true;
            out->keyOrigin = e->start;
            out->name = "generated media \"" + m->name + "\"";
            return true;
        }
        out->t.kind = FxTarget::Event;
        out->t.id = e->id;
        out->name = "event " + eventPlace(e->id);
        return true;
    }

    FxSlot* singleSlot(const Target& t)
    {
        Project& p = pj();
        if (t.t.kind == FxTarget::Transition) {
            Event* e = p.findEvent(t.t.id);
            return e && !e->transition.kind.empty() ? &e->transition : nullptr;
        }
        if (t.t.kind == FxTarget::Generator) {
            for (auto& m : p.media)
                if (m.id == t.t.id) return &m.generator;
        }
        return nullptr;
    }
    FxSlot* slotAt(const Target& t, const std::string& n, std::string* why)
    {
        if (t.single) {
            FxSlot* s = singleSlot(t);
            if (!s) *why = t.name + " has no effect";
            return s;
        }
        FxChain* c = app_.chainOf(pj(), t.t);
        long k = 0;
        if (!c) { *why = t.name + " has no effects chain"; return nullptr; }
        if (!wholeNumber(n, &k) || k < 1 || k > (long)c->size()) {
            *why = t.name + " has " + std::to_string(c->size()) + " effect" + (c->size() == 1 ? "" : "s") + ", not \"" + n + "\"";
            return nullptr;
        }
        return &(*c)[(size_t)k - 1];
    }

    // ---- plug-ins
    enum class Want { Filter, Transition, Generator };
    static std::string videoMatches(const std::vector<const VideoFxEntry*>& v)
    {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 12; ++i) s += "\n  " + v[i]->label + "  (" + v[i]->id + ")";
        if (v.size() > 12) s += "\n  ... " + std::to_string(v.size() - 12) + " more";
        return s;
    }
    const VideoFxEntry* videoPlugin(const std::string& name, Want want, std::string* why)
    {
        static std::vector<VideoFxEntry> all;
        all = Catalog::get().video();
        std::vector<const VideoFxEntry*> ok;
        for (const auto& v : all) {
            if (!v.ok) continue;
            if ((want == Want::Filter && v.filter) || (want == Want::Transition && v.transition) || (want == Want::Generator && v.generator)) ok.push_back(&v);
        }
        for (const VideoFxEntry* v : ok) if (lower(v->id) == lower(name)) return v;
        std::vector<const VideoFxEntry*> same;
        for (const VideoFxEntry* v : ok) if (squash(v->label) == squash(name)) same.push_back(v);
        // a name VEGAS and a plug-in file both have: the one Reno uses (the built-in when it prefers its own)
        if (same.size() > 1) {
            std::vector<const VideoFxEntry*> b;
            for (const VideoFxEntry* v : same) if (v->builtin == ofx::Host::get().config.preferBuiltinVegasFx) b.push_back(v);
            if (b.size() == 1) return b[0];
        }
        if (same.size() == 1) return same[0];
        std::vector<const VideoFxEntry*> part;
        for (const VideoFxEntry* v : (same.empty() ? ok : same))
            if (same.size() > 1 || contains(v->label, name) || contains(v->id, name)) part.push_back(v);
        if (part.size() == 1) return part[0];
        const char* what = want == Want::Transition ? "transition" : want == Want::Generator ? "generator" : "video effect";
        if (part.empty()) *why = std::string("no ") + what + " is called \"" + name + "\" (reno_cli fx-list lists them)";
        else *why = "\"" + name + "\" could be " + std::to_string(part.size()) + " " + what + "s; say which:" + videoMatches(part);
        return nullptr;
    }
    bool audioPlugin(const std::string& name, AudioPluginDesc* out, std::string* why)
    {
        std::vector<AudioFxEntry> all = Catalog::get().audio();
        std::vector<const AudioFxEntry*> ok;
        for (const auto& a : all) if (a.ok && !a.desc.isInstrument) ok.push_back(&a);
        std::vector<const AudioFxEntry*> same, part;
        for (const AudioFxEntry* a : ok) {
            if (lower(a->desc.key()) == lower(name) || lower(a->desc.uid) == lower(name)) { *out = a->desc; return true; }
            if (squash(a->desc.name) == squash(name)) same.push_back(a);
            else if (contains(a->desc.name, name)) part.push_back(a);
        }
        const std::vector<const AudioFxEntry*>& pick = same.empty() ? part : same;
        if (pick.size() == 1) { *out = pick[0]->desc; return true; }
        if (pick.empty()) { *why = "no audio effect is called \"" + name + "\" (reno_cli fx-list --audio lists them)"; return false; }
        std::string s = "\"" + name + "\" could be " + std::to_string(pick.size()) + " audio effects; say which (or give its identifier):";
        for (size_t i = 0; i < pick.size() && i < 12; ++i) s += "\n  " + pick[i]->desc.name + "  (" + pick[i]->desc.key() + ")";
        *why = s;
        return false;
    }

    // ---- settings of an effect
    // A live instance: made now if need be (as the editor makes them when the project changes)
    ofx::Instance* videoInstance(const FxSlot& s, std::string* why)
    {
        ofx::Instance* inst = app_.fx.video(s.id);
        if (!inst) {
            app_.fx.prepare(app_.project());
            inst = app_.fx.video(s.id);
        }
        if (!inst) *why = "the effect \"" + s.name + "\" could not be started" + (app_.fx.error(s.id).empty() ? "" : ": " + app_.fx.error(s.id));
        return inst;
    }
    AudioPlugin* audioInstance(const FxSlot& s, std::string* why)
    {
        AudioPlugin* pl = app_.fx.audio(s.id);
        if (!pl) {
            app_.fx.prepare(app_.project());
            pl = app_.fx.audio(s.id);
        }
        if (!pl) *why = "the effect \"" + s.name + "\" could not be started" + (app_.fx.error(s.id).empty() ? "" : ": " + app_.fx.error(s.id));
        return pl;
    }

    static ofx::Param* findParam(ofx::Instance& inst, const std::string& name, std::string* why)
    {
        std::vector<ofx::Param*> exact, part;
        for (const auto& up : inst.params.params) {
            ofx::Param* p = up.get();
            if (!p->hasValue()) continue;
            if (p->name == name) return p;
            if (squash(p->name) == squash(name) || squash(p->label()) == squash(name)) exact.push_back(p);
            else if (squash(name).size() >= 3 && (squash(p->label()).find(squash(name)) != std::string::npos || squash(p->name).find(squash(name)) != std::string::npos)) part.push_back(p);
        }
        const std::vector<ofx::Param*>& pick = exact.empty() ? part : exact;
        if (pick.size() == 1) return pick[0];
        if (pick.empty()) *why = "the effect has no setting \"" + name + "\" (reno_cli fx-params lists them)";
        else {
            std::string s = "\"" + name + "\" could be " + std::to_string(pick.size()) + " settings:";
            for (ofx::Param* p : pick) s += " \"" + p->label() + "\" (" + p->name + ")";
            *why = s;
        }
        return nullptr;
    }

    // a value for 'p' from text
    static bool videoValue(ofx::Param& p, const std::string& text, std::vector<double>* v, std::string* why)
    {
        using K = ofx::ParamKind;
        v->assign((size_t)p.dim, 0.0);
        if (p.kind == K::Bool) {
            bool b = false;
            if (!onOff(text, &b)) { *why = p.label() + " is on or off, not \"" + text + "\""; return false; }
            (*v)[0] = b ? 1 : 0;
            return true;
        }
        if (p.kind == K::Choice) {
            const std::vector<std::string> ch = p.choices();
            for (size_t i = 0; i < ch.size(); ++i)
                if (squash(ch[i]) == squash(text)) { (*v)[0] = (double)i; return true; }
            long n = 0;
            if (wholeNumber(text, &n) && n >= 0 && n < (long)ch.size()) { (*v)[0] = (double)n; return true; }
            std::string s = p.label() + " is one of:";
            for (size_t i = 0; i < ch.size(); ++i) s += " " + std::to_string(i) + " \"" + ch[i] + "\"";
            *why = s;
            return false;
        }
        std::vector<std::string> parts;
        if ((p.kind == K::RGB || p.kind == K::RGBA) && !text.empty() && text[0] == '#') {
            const std::string h = text.substr(1);
            if ((h.size() != 6 && h.size() != 8) || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                *why = "\"" + text + "\" is not a colour (#rrggbb or #rrggbbaa)";
                return false;
            }
            for (size_t i = 0; i < h.size() / 2 && i < (size_t)p.dim; ++i) (*v)[i] = std::strtol(h.substr(i * 2, 2).c_str(), nullptr, 16) / 255.0;
            if (h.size() == 6 && p.dim == 4) (*v)[3] = 1.0;
            return true;
        }
        std::stringstream ss(text);
        std::string one;
        while (std::getline(ss, one, ',')) parts.push_back(one);
        if (parts.empty() || (parts.size() != (size_t)p.dim && !(parts.size() == 1) && !(p.kind == K::RGBA && parts.size() == 3))) {
            *why = p.label() + " takes " + std::to_string(p.dim) + " number" + (p.dim == 1 ? "" : "s") + (p.dim > 1 ? " (x,y" + std::string(p.dim > 2 ? ",z" : "") + ")" : "") + ", not \"" + text + "\"";
            return false;
        }
        for (size_t i = 0; i < (size_t)p.dim; ++i) {
            const std::string& t = parts[std::min(i, parts.size() - 1)];
            double x = 0;
            if (!number(t, &x)) { *why = "\"" + t + "\" is not a number (for " + p.label() + ")"; return false; }
            (*v)[i] = x;
        }
        if (p.kind == K::RGBA && parts.size() == 3) (*v)[3] = 1.0;
        if (p.kind == K::RGBA && parts.size() == 1) (*v)[3] = 1.0;
        return true;
    }

    // "name=value" or "name@T=value": the setting, the value and the time (-1: none)
    struct Assign { std::string name, value, at; };
    static bool assignment(const std::string& a, Assign* out, std::string* why)
    {
        const size_t eq = a.find('=');
        if (eq == std::string::npos || eq == 0) { *why = "\"" + a + "\" is not SETTING=VALUE"; return false; }
        std::string lhs = a.substr(0, eq);
        out->value = a.substr(eq + 1);
        const size_t at = lhs.rfind('@');
        if (at != std::string::npos) {
            out->at = lhs.substr(at + 1);
            lhs = lhs.substr(0, at);
        }
        out->name = lhs;
        return true;
    }

    bool setVideo(const FxSlot& s, const Target& t, const std::vector<std::string>& sets, std::string* why)
    {
        if (sets.empty()) return true;
        ofx::Instance* inst = videoInstance(s, why);
        if (!inst) return false;
        for (const std::string& a : sets) {
            Assign as;
            if (!assignment(a, &as, why)) return false;
            ofx::Param* p = findParam(*inst, as.name, why);
            if (!p) return false;
            double frames = 0;
            const bool key = !as.at.empty();
            if (key) {
                double sec = 0;
                if (!parseTime(st(), as.at, &sec)) { *why = "\"" + as.at + "\" is not a time"; return false; }
                frames = sec * st().fps();
                const double r = std::floor(frames + 0.5);
                if (std::fabs(frames - r) < 1e-4) frames = r;
            }
            (void)t;
            if (p->isText()) {
                inst->userSetString(p, as.value, frames, key);
            } else {
                std::vector<double> v;
                if (!videoValue(*p, as.value, &v, why)) return false;
                inst->userSetValue(p, v, frames, key);
            }
        }
        app_.fx.captureStates(pj());
        return true;
    }

    // the plug-in's own 0..1 value showing 'text' ("-20", "-20 dB", "Off")
    static bool audioValue(AudioPlugin& pl, int i, const std::string& text, float* out, std::string* why)
    {
        const float was = pl.paramGet(i);
        const std::string want = squash(text);
        double wantNum = 0;
        char* end = nullptr;
        wantNum = std::strtod(text.c_str(), &end);
        const bool numeric = end != text.c_str();
        float best = -1.f;
        double bestDist = 1e300;
        const int steps = 400;
        std::vector<double> nums((size_t)steps + 1, NAN);
        for (int k = 0; k <= steps; ++k) {
            const float x = (float)k / steps;
            pl.paramSet(i, x);
            const std::string shown = pl.paramText(i);
            if (!want.empty() && squash(shown) == want) { best = x; bestDist = 0; break; }
            char* e2 = nullptr;
            const double n = std::strtod(shown.c_str(), &e2);
            if (e2 != shown.c_str()) nums[(size_t)k] = n;
            if (numeric && e2 != shown.c_str() && std::fabs(n - wantNum) < bestDist) { bestDist = std::fabs(n - wantNum); best = x; }
        }
        // a number outside what the setting shows: refused rather than taken as the nearest end
        double lo = INFINITY, hi = -INFINITY;
        for (double n : nums) if (!std::isnan(n) && std::isfinite(n)) { lo = std::min(lo, n); hi = std::max(hi, n); }
        if (numeric && bestDist > 0 && lo <= hi) {
            const double tol = std::max(1e-6, (hi - lo) * 0.005);
            if (wantNum < lo - tol || wantNum > hi + tol) {
                pl.paramSet(i, was);
                *why = pl.paramName(i) + " goes from " + fmt(lo) + " to " + fmt(hi) + ", not " + text;
                return false;
            }
        }
        if (best >= 0.f && bestDist > 0 && numeric) {
            // closer between the neighbouring steps (bisection where the shown number goes one way)
            const int k = (int)std::lround(best * steps);
            for (int side : {-1, 1}) {
                const int k2 = k + side;
                if (k2 < 0 || k2 > steps || std::isnan(nums[(size_t)k2]) || std::isnan(nums[(size_t)k])) continue;
                const double a = nums[(size_t)std::min(k, k2)], b = nums[(size_t)std::max(k, k2)];
                if (!((wantNum - a) * (wantNum - b) <= 0)) continue;
                float lo = (float)std::min(k, k2) / steps, hi = (float)std::max(k, k2) / steps;
                const bool up = b >= a;
                for (int it = 0; it < 30; ++it) {
                    const float mid = 0.5f * (lo + hi);
                    pl.paramSet(i, mid);
                    const double n = std::strtod(pl.paramText(i).c_str(), nullptr);
                    if ((n < wantNum) == up) lo = mid; else hi = mid;
                }
                best = 0.5f * (lo + hi);
                break;
            }
        }
        pl.paramSet(i, was);
        if (best < 0.f) {
            pl.paramSet(i, 0.f);
            const std::string a = pl.paramText(i);
            pl.paramSet(i, 1.f);
            const std::string b = pl.paramText(i);
            pl.paramSet(i, was);
            *why = pl.paramName(i) + " goes from \"" + a + "\" to \"" + b + "\"; \"" + text + "\" is not one of its values (or use --raw with 0..1)";
            return false;
        }
        *out = best;
        return true;
    }

    bool setAudio(const FxSlot& s, const std::vector<std::string>& sets, bool raw, std::string* why)
    {
        if (sets.empty()) return true;
        AudioPlugin* pl = audioInstance(s, why);
        if (!pl) return false;
        for (const std::string& a : sets) {
            Assign as;
            if (!assignment(a, &as, why)) return false;
            if (!as.at.empty()) { *why = "audio effects' settings have no keyframes (use the track's FX envelopes in the editor)"; return false; }
            int idx = -1;
            std::vector<int> part;
            for (int i = 0; i < pl->paramCount(); ++i) {
                if (squash(pl->paramName(i)) == squash(as.name)) { idx = i; break; }
                if (squash(as.name).size() >= 3 && squash(pl->paramName(i)).find(squash(as.name)) != std::string::npos) part.push_back(i);
            }
            if (idx < 0 && part.size() == 1) idx = part[0];
            long n = 0;
            if (idx < 0 && wholeNumber(as.name, &n) && n >= 1 && n <= pl->paramCount()) idx = (int)n - 1;
            if (idx < 0) {
                *why = part.size() > 1 ? "\"" + as.name + "\" could be several settings of " + s.name : s.name + " has no setting \"" + as.name + "\" (reno_cli fx-params lists them)";
                return false;
            }
            float v = 0.f;
            if (raw) {
                double x = 0;
                if (!number(as.value, &x) || x < 0 || x > 1) { *why = "--raw values are 0..1, not \"" + as.value + "\""; return false; }
                v = (float)x;
            } else if (!audioValue(*pl, idx, as.value, &v, why)) {
                return false;
            }
            pl->paramSet(idx, v);
        }
        app_.fx.captureStates(pj());
        return true;
    }

    bool setSlot(const Target& t, uint64_t slotId, const std::vector<std::string>& sets, bool raw, std::string* why)
    {
        const FxSlot* s = nullptr;
        if (t.single) s = singleSlot(t);
        else if (const FxChain* c = app_.chainOf(pj(), t.t))
            for (const FxSlot& x : *c) if (x.id == slotId) s = &x;
        if (!s) { *why = "the effect is gone"; return false; }
        const FxSlot copy = *s;
        return copy.kind == "ofx" ? setVideo(copy, t, sets, why) : setAudio(copy, sets, raw, why);
    }

    // ---- printing
    std::string videoParamText(const ofx::Param& p, double time)
    {
        using K = ofx::ParamKind;
        if (p.isText()) return "\"" + p.str + "\"";
        const std::vector<double> v = p.keys.empty() ? p.value : p.valueAt(time);
        if (p.kind == K::Bool) return v.empty() || v[0] == 0 ? "off" : "on";
        if (p.kind == K::Choice) {
            const std::vector<std::string> ch = p.choices();
            const int i = v.empty() ? 0 : (int)std::lround(v[0]);
            return i >= 0 && i < (int)ch.size() ? "\"" + ch[(size_t)i] + "\"" : std::to_string(i);
        }
        auto join = [](const std::vector<double>& x) {
            std::string j;
            for (size_t i = 0; i < x.size(); ++i) j += (i ? "," : "") + fmt(x[i]);
            return j;
        };
        if (p.keys.empty()) return join(v);
        // keyframes: VALUE@SECONDS, as they are given to set
        std::string s = "keyframes";
        const double fps = st().fps();
        for (const auto& k : p.keys) s += " " + join(k.second) + "@" + fmt(k.first / fps) + "s";
        return s;
    }

    // a slot's name (a .veg names effects only by identifier: then the plug-in's)
    static std::string slotLabel(const FxSlot& s)
    {
        if (!s.name.empty()) return s.name;
        if (s.kind == "audio") return s.audio.name.empty() ? s.audio.uid : s.audio.name;
        for (const auto& v : Catalog::get().video())
            if (v.id == s.pluginId) return v.label;
        return s.pluginId;
    }

    void printChain(const FxChain& c, const std::string& indent, bool params)
    {
        int n = 0;
        for (const FxSlot& s : c) {
            ++n;
            printSlot(s, indent + "fx " + std::to_string(n) + ": ", indent, params);
        }
    }
    void printSlot(const FxSlot& s, const std::string& head, const std::string& indent, bool params)
    {
        const std::string id = s.kind == "ofx" ? s.pluginId : s.audio.key();
        result(head + slotLabel(s) + "  (" + id + ")" + (s.bypass ? "  [bypassed]" : ""));
        if (!params) return;
        std::string why;
        if (s.kind == "ofx") {
            ofx::Instance* inst = videoInstance(s, &why);
            if (!inst) { result(indent + "    (" + why + ")"); return; }
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            for (const auto& up : inst->params.params)
                if (up->hasValue() && !up->secret()) result(indent + "    " + up->label() + " = " + videoParamText(*up, 0));
        } else {
            AudioPlugin* pl = audioInstance(s, &why);
            if (!pl) { result(indent + "    (" + why + ")"); return; }
            for (int i = 0; i < pl->paramCount(); ++i)
                if (shownAudioParam(*pl, i)) result(indent + "    " + pl->paramName(i) + " = " + pl->paramText(i));
        }
    }

    void printInfo(bool params)
    {
        const Project& p = app_.project();
        const ProjectSettings& s = p.settings;
        result("Project: " + (app_.path().empty() ? std::string("(not saved)") : app_.path()));
        result("  " + std::to_string(s.width) + "x" + std::to_string(s.height) + ", " + fmt(s.fps()) + " fps (" + std::to_string(s.fpsNum) + "/" +
               std::to_string(s.fpsDen) + "), " + std::to_string(s.sampleRate) + " Hz, " + (s.audioChannels == 6 ? "5.1" : "stereo") + ", render quality " +
               s.renderQuality + ", " + s.pixelFormat + ", length " + fmt(p.length()) + " s");
        if (!p.media.empty()) {
            result("Media:");
            for (const auto& m : p.media)
                result("  " + std::to_string(m.id) + ": " + m.name + (m.generated ? "  (generated: " + m.generator.pluginId + ")" : "  " + m.path) + (m.offline ? "  [offline]" : ""));
        }
        for (size_t i = 0; i < p.tracks.size(); ++i) {
            const Track& t = p.tracks[i];
            std::string head = "Track " + std::to_string(i + 1) + " (" + (t.type == TrackType::Video ? "video" : "audio") + ")";
            if (!t.name.empty()) head += " \"" + t.name + "\"";
            if (t.type == TrackType::Audio && std::fabs(t.level - 1.0) > 1e-9)
                head += " volume " + (t.level > 0 ? fmt(20.0 * std::log10(t.level)) + " dB" : std::string("-inf dB"));
            if (t.type == TrackType::Video && std::fabs(t.level - 1.0) > 1e-9) head += " opacity " + fmt(t.level);
            if (t.type == TrackType::Audio && std::fabs(t.pan) > 1e-9) head += " pan " + fmt(t.pan);
            if (t.mute) head += " [muted]";
            if (t.solo) head += " [solo]";
            result(head);
            printChain(t.fx, "  ", params);
            const std::vector<const Event*> v = ordered(t);
            for (size_t k = 0; k < v.size(); ++k) {
                const Event& e = *v[k];
                const MediaItem* m = p.findMedia(e.mediaId);
                std::string line = "  event " + std::to_string(i + 1) + "." + std::to_string(k + 1) + " (id " + std::to_string(e.id) + "): " + fmt(e.start) + " to " +
                                   fmt(e.end()) + " s";
                if (e.offset > 0) line += ", from " + fmt(e.offset) + " s";
                line += ", " + (m ? m->name : std::string("no media"));
                if (!e.name.empty() && (!m || e.name != m->name)) line += " \"" + e.name + "\"";
                if (e.mute) line += " [muted]";
                if (e.group) line += " (group " + std::to_string(e.group) + ")";
                result(line);
                if (!e.transition.kind.empty()) printSlot(e.transition, "    transition: ", "    ", params);
                if (m && m->generated) printSlot(m->generator, "    generator: ", "    ", params);
                printChain(e.fx, "    ", params);
            }
        }
        if (!p.videoOut.empty()) { result("Video Output:"); printChain(p.videoOut, "  ", params); }
        if (!p.audioOut.empty()) { result("Master:"); printChain(p.audioOut, "  ", params); }
        for (size_t i = 0; i < p.buses.size(); ++i) {
            result("Bus " + std::string(1, (char)('A' + (int)i)) + (p.buses[i].name.empty() ? "" : " \"" + p.buses[i].name + "\"") + ":");
            printChain(p.buses[i].fx, "  ", params);
        }
        for (const auto& mk : p.markers)
            result(std::string(mk.region ? "Region " : "Marker ") + fmt(mk.time) + (mk.region ? " to " + fmt(mk.time + mk.length) : "") + " s" +
                   (mk.name.empty() ? "" : " \"" + mk.name + "\""));
    }

    // ---- the commands
    // options of a command: --name VALUE pairs and the rest; 'flags' are options without a value
    struct Opts {
        std::map<std::string, std::string> v;
        std::vector<std::string> rest;
        bool has(const std::string& k) const { return v.count(k) != 0; }
        std::string get(const std::string& k) const { auto i = v.find(k); return i == v.end() ? std::string() : i->second; }
    };
    static bool options(const std::vector<std::string>& a, size_t from, const std::vector<std::string>& valued, const std::vector<std::string>& flags, Opts* o, std::string* why)
    {
        for (size_t i = from; i < a.size(); ++i) {
            const std::string& x = a[i];
            if (x.size() > 2 && x.compare(0, 2, "--") == 0) {
                if (std::find(flags.begin(), flags.end(), x) != flags.end()) { o->v[x] = "1"; continue; }
                if (std::find(valued.begin(), valued.end(), x) != valued.end()) {
                    if (i + 1 >= a.size()) { *why = x + " needs a value"; return false; }
                    o->v[x] = a[++i];
                    continue;
                }
                *why = "unknown option " + x;
                return false;
            }
            o->rest.push_back(x);
        }
        return true;
    }
    bool timeOpt(const Opts& o, const std::string& k, double* t, std::string* why)
    {
        if (!o.has(k)) return true;
        if (!parseTime(st(), o.get(k), t)) { *why = k + ": \"" + o.get(k) + "\" is not a time"; return false; }
        return true;
    }

    bool settingsCmd(const Opts& o, std::string* why)
    {
        ProjectSettings s = st();
        if (o.has("--size") && !parseSize(o.get("--size"), &s.width, &s.height)) { *why = "--size: \"" + o.get("--size") + "\" is not WxH"; return false; }
        if (o.has("--fps") && !parseFps(o.get("--fps"), &s.fpsNum, &s.fpsDen)) { *why = "--fps: \"" + o.get("--fps") + "\" is not a frame rate"; return false; }
        if (o.has("--sample-rate")) {
            long r = 0;
            if (!wholeNumber(o.get("--sample-rate"), &r) || r < 8000 || r > 384000) { *why = "--sample-rate: 8000 to 384000"; return false; }
            s.sampleRate = (int)r;
        }
        if (o.has("--channels")) {
            const std::string c = o.get("--channels");
            if (c != "2" && c != "6") { *why = "--channels: 2 (stereo) or 6 (5.1)"; return false; }
            s.audioChannels = c == "6" ? 6 : 2;
        }
        if (o.has("--quality")) {
            const std::string q = lower(o.get("--quality"));
            if (q != "draft" && q != "preview" && q != "good" && q != "best") { *why = "--quality: draft, preview, good or best"; return false; }
            s.renderQuality = q;
        }
        if (o.has("--pixel-format")) {
            const std::string f = lower(o.get("--pixel-format"));
            if (f != "8bit" && f != "32bit") { *why = "--pixel-format: 8bit or 32bit"; return false; }
            s.pixelFormat = f;
        }
        app_.edit("Project Properties", [&](Project& p) { p.settings = s; });
        return true;
    }

    // --track N given: the events of that track's kind go on it even where they overlap others (as when an event is
    // dragged over another to make a cross-fade); the editor would have looked for a free track
    void putOnTrack(const std::vector<uint64_t>& made, uint64_t trackId)
    {
        app_.edit("Add to Timeline", [&](Project& p) {
            Track* want = p.findTrack(trackId);
            if (!want) return;
            for (uint64_t id : made) {
                Track* now = nullptr;
                Event* e = p.findEvent(id, &now);
                if (!e || !now || now == want || now->type != want->type) continue;
                const Event moved = *e;
                now->events.erase(std::remove_if(now->events.begin(), now->events.end(), [&](const Event& x) { return x.id == id; }), now->events.end());
                want = p.findTrack(trackId);
                want->events.push_back(moved);
            }
        });
    }

    bool run(const std::vector<std::string>& a, std::string* why)
    {
        if (a.empty()) return true;
        const std::string cmd = lower(a[0]);
        Opts o;
        auto opts = [&](const std::vector<std::string>& valued, const std::vector<std::string>& flags = {}) { return options(a, 1, valued, flags, &o, why); };

        if (cmd == "info") {
            if (!opts({}, {"--params"})) return false;
            printInfo(o.has("--params"));
            return true;
        }
        if (cmd == "settings") {
            if (!opts({"--size", "--fps", "--sample-rate", "--channels", "--quality", "--pixel-format"})) return false;
            if (!o.rest.empty()) { *why = "settings: unexpected \"" + o.rest[0] + "\""; return false; }
            return settingsCmd(o, why);
        }
        if (cmd == "add-track") {
            if (!opts({"--name", "--at"})) return false;
            if (o.rest.size() != 1 || (lower(o.rest[0]) != "video" && lower(o.rest[0]) != "audio")) { *why = "add-track video|audio"; return false; }
            long at = 0;
            int index = -1;
            if (o.has("--at")) {
                if (!wholeNumber(o.get("--at"), &at) || at < 1 || at > (long)app_.project().tracks.size() + 1) { *why = "--at: a track position from 1"; return false; }
                index = (int)at - 1;
            }
            const uint64_t id = app_.addTrack(lower(o.rest[0]) == "video" ? TrackType::Video : TrackType::Audio, index);
            if (!id) { *why = "the track could not be added"; return false; }
            if (o.has("--name")) app_.edit("Rename Track", [&](Project& p) { if (Track* t = p.findTrack(id)) t->name = o.get("--name"); });
            result("track " + std::to_string(trackNumber(id)));
            return true;
        }
        if (cmd == "track") {
            if (!opts({"--name", "--mute", "--solo", "--volume", "--opacity", "--pan"})) return false;
            if (o.rest.size() != 1) { *why = "track N [options]"; return false; }
            Track* tr = trackAt(o.rest[0], why);
            if (!tr) return false;
            const uint64_t id = tr->id;
            const bool video = tr->type == TrackType::Video;
            bool mute = tr->mute, solo = tr->solo;
            double level = tr->level, pan = tr->pan;
            if (o.has("--mute") && !onOff(o.get("--mute"), &mute)) { *why = "--mute on|off"; return false; }
            if (o.has("--solo") && !onOff(o.get("--solo"), &solo)) { *why = "--solo on|off"; return false; }
            if (o.has("--volume")) {
                double db = 0;
                if (video) { *why = "--volume is for audio tracks (--opacity for video)"; return false; }
                if (!number(o.get("--volume"), &db) || db > 12) { *why = "--volume: dB, at most 12"; return false; }
                level = std::pow(10.0, db / 20.0);
            }
            if (o.has("--opacity")) {
                if (!video) { *why = "--opacity is for video tracks (--volume for audio)"; return false; }
                if (!number(o.get("--opacity"), &level) || level < 0 || level > 1) { *why = "--opacity: 0..1"; return false; }
            }
            if (o.has("--pan")) {
                if (video) { *why = "--pan is for audio tracks"; return false; }
                if (!number(o.get("--pan"), &pan) || pan < -1 || pan > 1) { *why = "--pan: -1 (left) .. 1 (right)"; return false; }
            }
            app_.edit("Track", [&](Project& p) {
                Track* t = p.findTrack(id);
                if (!t) return;
                if (o.has("--name")) t->name = o.get("--name");
                t->mute = mute;
                t->solo = solo;
                t->level = level;
                t->pan = pan;
            });
            return true;
        }
        if (cmd == "remove-track") {
            if (!opts({})) return false;
            if (o.rest.size() != 1) { *why = "remove-track N"; return false; }
            Track* tr = trackAt(o.rest[0], why);
            if (!tr) return false;
            app_.removeTrack(tr->id);
            return true;
        }
        if (cmd == "add") {
            if (!opts({"--track", "--at", "--length", "--from", "--name"}, {"--video-only", "--audio-only"})) return false;
            if (o.rest.size() != 1) { *why = "add FILE [options]"; return false; }
            std::error_code ec;
            const std::string path = std::filesystem::absolute(std::filesystem::u8path(o.rest[0]), ec).u8string();
            double at = 0, length = -1, from = 0;
            if (!timeOpt(o, "--at", &at, why) || !timeOpt(o, "--length", &length, why) || !timeOpt(o, "--from", &from, why)) return false;
            uint64_t hint = 0;
            if (o.has("--track")) {
                Track* tr = trackAt(o.get("--track"), why);
                if (!tr) return false;
                hint = tr->id;
            }
            std::string err;
            const uint64_t mid = app_.importMedia(path, &err);
            if (!mid) { *why = "cannot open " + o.rest[0] + (err.empty() ? "" : ": " + err); return false; }
            const MediaItem m = *app_.project().findMedia(mid);
            if (o.has("--video-only") && !m.info.hasVideo) { *why = o.rest[0] + " has no picture"; return false; }
            if (o.has("--audio-only") && !m.info.hasAudio) { *why = o.rest[0] + " has no sound"; return false; }
            const bool still = m.info.isStill;
            if (!still && (o.has("--from") || o.has("--length") || o.has("--video-only") || o.has("--audio-only"))) {
                AppState::MediaRange r;
                r.mediaId = mid;
                r.in = from;
                r.out = length > 0 ? from + length : std::max(from + 1e-3, m.info.duration);
                r.streams = o.has("--video-only") ? 1 : o.has("--audio-only") ? 2 : 0;
                if (r.in >= m.info.duration && m.info.duration > 0) { *why = "--from is past the end of the media (" + fmt(m.info.duration) + " s)"; return false; }
                app_.nextAddRange = r;
            }
            const std::vector<uint64_t> made = app_.addMediaToTimeline(mid, at, hint);
            if (made.empty()) { *why = "nothing was added"; return false; }
            if (hint) putOnTrack(made, hint);
            if ((still && length > 0) || o.has("--name")) {
                app_.edit("Event", [&](Project& p) {
                    for (uint64_t id : made)
                        if (Event* e = p.findEvent(id)) {
                            if (still && length > 0) e->length = length;
                            if (o.has("--name")) e->name = o.get("--name");
                        }
                });
            }
            for (uint64_t id : made) result("event " + eventPlace(id) + " (id " + std::to_string(id) + ")");
            return true;
        }
        if (cmd == "generate") {
            if (!opts({"--track", "--at", "--length"})) return false;
            if (o.rest.empty()) { *why = "generate PLUGIN [options] [SETTING=VALUE ...]"; return false; }
            const VideoFxEntry* v = videoPlugin(o.rest[0], Want::Generator, why);
            if (!v) return false;
            double at = 0, length = -1;
            if (!timeOpt(o, "--at", &at, why) || !timeOpt(o, "--length", &length, why)) return false;
            uint64_t hint = 0;
            if (o.has("--track")) {
                Track* tr = trackAt(o.get("--track"), why);
                if (!tr) return false;
                if (tr->type != TrackType::Video) { *why = "track " + o.get("--track") + " is not a video track"; return false; }
                hint = tr->id;
            }
            const uint64_t mid = app_.addGeneratedMedia(v->id, v->label);
            const std::vector<uint64_t> made = app_.addMediaToTimeline(mid, at, hint);
            if (made.empty()) { *why = "nothing was added"; return false; }
            if (hint) putOnTrack(made, hint);
            if (length > 0) app_.edit("Event", [&](Project& p) { for (uint64_t id : made) if (Event* e = p.findEvent(id)) e->length = length; });
            Target t;
            t.t.kind = FxTarget::Generator;
            t.t.id = mid;
            t.single = true;
            if (const Event* e = app_.project().findEvent(made[0])) t.keyOrigin = e->start;
            std::vector<std::string> sets(o.rest.begin() + 1, o.rest.end());
            if (!setSlot(t, 0, sets, false, why)) return false;
            for (uint64_t id : made) result("event " + eventPlace(id) + " (id " + std::to_string(id) + ")");
            return true;
        }
        if (cmd == "event") {
            if (!opts({"--start", "--length", "--from", "--name", "--mute", "--fade-in", "--fade-out", "--rate", "--pitch"})) return false;
            if (o.rest.size() != 1) { *why = "event E [options]"; return false; }
            Event* e = eventAt(o.rest[0], why);
            if (!e) return false;
            const uint64_t id = e->id;
            double start = e->start, length = e->length, from = e->offset, fin = e->fadeIn, fout = e->fadeOut, rate = e->rate, pitch = e->pitch;
            bool mute = e->mute;
            if (!timeOpt(o, "--start", &start, why) || !timeOpt(o, "--length", &length, why) || !timeOpt(o, "--from", &from, why) || !timeOpt(o, "--fade-in", &fin, why) ||
                !timeOpt(o, "--fade-out", &fout, why))
                return false;
            if (o.has("--mute") && !onOff(o.get("--mute"), &mute)) { *why = "--mute on|off"; return false; }
            if (o.has("--rate") && (!number(o.get("--rate"), &rate) || rate == 0 || std::fabs(rate) > 10)) { *why = "--rate: a speed, e.g. 2 (twice as fast), -1 (backwards)"; return false; }
            if (o.has("--pitch") && (!number(o.get("--pitch"), &pitch) || std::fabs(pitch) > 48)) { *why = "--pitch: semitones, -48..48"; return false; }
            if (length <= 0) { *why = "--length must be more than 0"; return false; }
            app_.edit("Event", [&](Project& p) {
                Event* x = p.findEvent(id);
                if (!x) return;
                x->start = start;
                x->length = length;
                x->offset = from;
                x->fadeIn = std::min(fin, length);
                x->fadeOut = std::min(fout, length);
                x->mute = mute;
                x->rate = rate;
                x->pitch = pitch;
                if (o.has("--name")) x->name = o.get("--name");
            });
            return true;
        }
        if (cmd == "remove") {
            if (!opts({})) return false;
            if (o.rest.size() != 1) { *why = "remove E"; return false; }
            Event* e = eventAt(o.rest[0], why);
            if (!e) return false;
            const uint64_t id = e->id;
            app_.edit("Delete", [&](Project& p) {
                for (auto& t : p.tracks)
                    t.events.erase(std::remove_if(t.events.begin(), t.events.end(), [&](const Event& x) { return x.id == id; }), t.events.end());
            });
            return true;
        }
        if (cmd == "fx") {
            if (!opts({}, {"--raw"})) return false;
            if (o.rest.size() < 2) { *why = "fx TARGET PLUGIN [SETTING=VALUE ...]"; return false; }
            Target t;
            if (!target(o.rest[0], &t, why)) return false;
            if (t.single) { *why = "fx adds to a chain; for " + t.name + " use transition or set"; return false; }
            uint64_t slot = 0;
            if (t.video) {
                const VideoFxEntry* v = videoPlugin(o.rest[1], Want::Filter, why);
                AudioPluginDesc other;
                std::string ignore;
                if (!v && why->rfind("no ", 0) == 0 && audioPlugin(o.rest[1], &other, &ignore)) *why = other.name + " is an audio effect, and " + t.name + " is video";
                if (!v) return false;
                slot = app_.addVideoFx(t.t, v->id, v->label);
            } else {
                AudioPluginDesc d;
                if (!audioPlugin(o.rest[1], &d, why)) {
                    std::string ignore;
                    if (why->rfind("no ", 0) == 0)
                        if (const VideoFxEntry* v = videoPlugin(o.rest[1], Want::Filter, &ignore)) *why = v->label + " is a video effect, and " + t.name + " is audio";
                    return false;
                }
                slot = app_.addAudioFx(t.t, d);
            }
            if (!slot) { *why = "the effect could not be added to " + t.name; return false; }
            std::vector<std::string> sets(o.rest.begin() + 2, o.rest.end());
            std::string startWhy;
            const FxSlot* s = nullptr;
            if (const FxChain* c = app_.chainOf(pj(), t.t)) for (const FxSlot& x : *c) if (x.id == slot) s = &x;
            if (s && s->kind == "ofx" ? !videoInstance(*s, &startWhy) : (s && !audioInstance(*s, &startWhy))) {
                *why = startWhy;
                return false;
            }
            if (!setSlot(t, slot, sets, o.has("--raw"), why)) return false;
            const FxChain* c = app_.chainOf(pj(), t.t);
            result(t.name + " fx " + std::to_string(c ? c->size() : 0) + ": " + (s ? slotLabel(*s) : std::string()));
            return true;
        }
        if (cmd == "set") {
            if (!opts({}, {"--raw"})) return false;
            if (o.rest.empty()) { *why = "set TARGET N SETTING=VALUE ..."; return false; }
            Target t;
            if (!target(o.rest[0], &t, why)) return false;
            size_t first = 1;
            uint64_t slotId = 0;
            if (!t.single) {
                if (o.rest.size() < 2) { *why = "set TARGET N SETTING=VALUE ... (N: the effect's place in the chain, from 1)"; return false; }
                FxSlot* s = slotAt(t, o.rest[1], why);
                if (!s) return false;
                slotId = s->id;
                first = 2;
            } else if (!singleSlot(t)) {
                *why = t.name + " has no effect";
                return false;
            }
            std::vector<std::string> sets(o.rest.begin() + (std::ptrdiff_t)first, o.rest.end());
            if (sets.empty()) { *why = "set: no SETTING=VALUE given"; return false; }
            return setSlot(t, slotId, sets, o.has("--raw"), why);
        }
        if (cmd == "remove-fx" || cmd == "bypass") {
            if (!opts({})) return false;
            const size_t need = cmd == "bypass" ? 3 : 2;
            if (o.rest.size() != need) { *why = cmd == "bypass" ? "bypass TARGET N on|off" : "remove-fx TARGET N"; return false; }
            Target t;
            if (!target(o.rest[0], &t, why)) return false;
            if (t.single) { *why = cmd + " is for effect chains"; return false; }
            FxSlot* s = slotAt(t, o.rest[1], why);
            if (!s) return false;
            if (cmd == "remove-fx") app_.removeFx(t.t, s->id);
            else {
                bool b = false;
                if (!onOff(o.rest[2], &b)) { *why = "bypass TARGET N on|off"; return false; }
                app_.setFxBypass(t.t, s->id, b);
            }
            return true;
        }
        if (cmd == "transition") {
            if (!opts({})) return false;
            if (o.rest.size() < 2) { *why = "transition E PLUGIN [SETTING=VALUE ...] | none"; return false; }
            Event* e = eventAt(o.rest[0], why);
            if (!e) return false;
            const uint64_t id = e->id;
            if (lower(o.rest[1]) == "none") { app_.setTransition(id, std::string(), std::string()); return true; }
            Target t;
            if (!target("transition:" + std::to_string(id), &t, why)) return false;
            const VideoFxEntry* v = videoPlugin(o.rest[1], Want::Transition, why);
            if (!v) return false;
            app_.setTransition(id, v->id, v->label);
            std::vector<std::string> sets(o.rest.begin() + 2, o.rest.end());
            return setSlot(t, 0, sets, false, why);
        }
        if (cmd == "marker" || cmd == "region") {
            if (!opts({})) return false;
            const bool region = cmd == "region";
            if (o.rest.size() < (region ? 2u : 1u) || o.rest.size() > (region ? 3u : 2u)) { *why = region ? "region START END [NAME]" : "marker T [NAME]"; return false; }
            double a = 0, b = 0;
            if (!parseTime(st(), o.rest[0], &a)) { *why = "\"" + o.rest[0] + "\" is not a time"; return false; }
            if (region && (!parseTime(st(), o.rest[1], &b) || b <= a)) { *why = "region: END must be a time after START"; return false; }
            const std::string name = o.rest.size() > (region ? 2u : 1u) ? o.rest.back() : std::string();
            app_.edit(region ? "Insert Region" : "Insert Marker", [&](Project& p) {
                Project::Marker mk;
                mk.id = p.newId();
                mk.time = a;
                mk.length = region ? b - a : 0;
                mk.region = region;
                mk.name = name;
                p.markers.push_back(mk);
                std::stable_sort(p.markers.begin(), p.markers.end(), [](const Project::Marker& x, const Project::Marker& y) { return x.time < y.time; });
            });
            return true;
        }
        *why = "unknown command \"" + a[0] + "\" (reno_cli --help lists them)";
        return false;
    }
};

// ------------------------------------------------------------------------------------------------ listing plug-ins
int fxList(const std::vector<std::string>& args)
{
    std::string text;
    bool video = true, audio = true, filters = true, transitions = true, generators = true;
    for (const std::string& a : args) {
        if (a == "--video") { audio = false; transitions = generators = false; }
        else if (a == "--audio") { video = false; }
        else if (a == "--transitions") { audio = false; filters = generators = false; }
        else if (a == "--generators") { audio = false; filters = transitions = false; }
        else if (a.size() > 1 && a[0] == '-') { problem("fx-list: unknown option " + a); return kUsage; }
        else text = a;
    }
    int n = 0;
    if (video) {
        std::vector<VideoFxEntry> all = Catalog::get().video();
        std::stable_sort(all.begin(), all.end(), [](const VideoFxEntry& a, const VideoFxEntry& b) { return lower(a.label) < lower(b.label); });
        for (const auto& v : all) {
            if (!v.ok) continue;
            const char* kind = v.transition ? "transition" : v.generator ? "generator" : v.filter ? "video" : nullptr;
            if (!kind) continue;
            if ((v.transition && !transitions) || (v.generator && !generators) || (v.filter && !v.transition && !v.generator && !filters)) continue;
            if (!text.empty() && !contains(v.label, text) && !contains(v.id, text) && !contains(v.group, text)) continue;
            result(std::string(kind) + "\t" + v.label + "\t" + v.id + (v.builtin ? "\t(built-in)" : ""));
            ++n;
        }
    }
    if (audio) {
        std::vector<AudioFxEntry> all = Catalog::get().audio();
        std::stable_sort(all.begin(), all.end(), [](const AudioFxEntry& a, const AudioFxEntry& b) { return lower(a.desc.name) < lower(b.desc.name); });
        for (const auto& a : all) {
            if (!a.ok || a.desc.isInstrument) continue;
            if (!text.empty() && !contains(a.desc.name, text) && !contains(a.desc.vendor, text)) continue;
            result("audio\t" + a.desc.name + "\t" + a.desc.key() + "\t(" + (a.desc.vendor.empty() ? a.desc.format : a.desc.vendor) + ")");
            ++n;
        }
    }
    if (n == 0) say("no plug-ins" + (text.empty() ? std::string() : " match \"" + text + "\""));
    return kOk;
}

int fxParams(AppState& app, const std::vector<std::string>& args)
{
    if (args.size() != 1) { problem("fx-params PLUGIN"); return kUsage; }
    app.newProject();
    Session s(app);
    std::string why;
    // a video effect, transition or generator, else an audio effect
    const VideoFxEntry* v = s.videoPlugin(args[0], Session::Want::Filter, &why);
    if (!v) v = s.videoPlugin(args[0], Session::Want::Generator, &why);
    if (!v) v = s.videoPlugin(args[0], Session::Want::Transition, &why);
    if (v) {
        // started where it would be used: a filter on a track, a transition into an event, a generator as media
        uint64_t sid = 0;
        const uint64_t tr = app.addTrack(TrackType::Video, -1);
        if (v->filter) {
            FxTarget t;
            t.kind = FxTarget::Track;
            t.id = tr;
            sid = app.addVideoFx(t, v->id, v->label);
        } else if (v->transition) {
            app.insertEmptyEvent(tr, 0, 2);
            const uint64_t ev = app.insertEmptyEvent(tr, 1, 2);
            app.setTransition(ev, v->id, v->label);
            if (const Event* e = app.project().findEvent(ev)) sid = e->transition.id;
        } else {
            const uint64_t mid = app.addGeneratedMedia(v->id, v->label);
            if (const MediaItem* m = app.project().findMedia(mid)) sid = m->generator.id;
        }
        FxSlot copy;
        copy.id = sid;
        copy.name = v->label;
        copy.kind = "ofx";
        copy.pluginId = v->id;
        ofx::Instance* inst = s.videoInstance(copy, &why);
        if (!inst) { problem(why); return kFailed; }
        result(v->label + "  (" + v->id + ")");
        std::lock_guard<std::recursive_mutex> lk(inst->mutex);
        using K = ofx::ParamKind;
        for (const auto& up : inst->params.params) {
            const ofx::Param& p = *up;
            if (!p.hasValue() || p.secret()) continue;
            std::string kind;
            switch (p.kind) {
            case K::Int: kind = "whole number"; break;
            case K::Double: kind = "number"; break;
            case K::Bool: kind = "on/off"; break;
            case K::Choice: kind = "choice"; break;
            case K::RGBA: kind = "colour r,g,b,a"; break;
            case K::RGB: kind = "colour r,g,b"; break;
            case K::Double2D: case K::Int2D: kind = "x,y"; break;
            case K::Double3D: case K::Int3D: kind = "x,y,z"; break;
            default: kind = "text"; break;
            }
            std::string line = "  " + p.label() + (p.label() != p.name ? "  [" + p.name + "]" : "") + "  " + kind;
            if (p.kind == K::Choice) {
                const std::vector<std::string> ch = p.choices();
                line += ":";
                for (size_t i = 0; i < ch.size(); ++i) line += (i ? ", " : " ") + std::to_string(i) + " \"" + ch[i] + "\"";
            } else if (p.dim > 0 && p.kind != K::Bool) {
                const double lo = p.props.getDouble(kOfxParamPropDisplayMin, 0, p.props.getDouble(kOfxParamPropMin, 0, -1e300));
                const double hi = p.props.getDouble(kOfxParamPropDisplayMax, 0, p.props.getDouble(kOfxParamPropMax, 0, 1e300));
                if (lo > -1e299 && hi < 1e299) line += " " + fmt(lo) + " .. " + fmt(hi);
            }
            line += ", default " + s.videoParamText(p, 0);
            result(line);
        }
        return kOk;
    }
    AudioPluginDesc d;
    std::string whyAudio;
    if (!s.audioPlugin(args[0], &d, &whyAudio)) {
        problem(why.find("could be") != std::string::npos ? why : whyAudio);
        return kFailed;
    }
    FxTarget t;
    t.kind = FxTarget::AudioOut;
    FxSlot copy;
    copy.id = app.addAudioFx(t, d);
    copy.kind = "audio";
    copy.name = d.name;
    AudioPlugin* pl = s.audioInstance(copy, &why);
    if (!pl) { problem(why); return kFailed; }
    result(d.name + "  (" + d.key() + ")");
    for (int i = 0; i < pl->paramCount(); ++i) {
        if (!shownAudioParam(*pl, i)) continue;
        const float was = pl->paramGet(i);
        const std::string def = pl->paramText(i);
        pl->paramSet(i, 0.f);
        const std::string lo = pl->paramText(i);
        pl->paramSet(i, 1.f);
        const std::string hi = pl->paramText(i);
        pl->paramSet(i, was);
        result("  " + pl->paramName(i) + "  \"" + lo + "\" .. \"" + hi + "\", default \"" + def + "\" (" + fmt(was) + ")");
    }
    return kOk;
}

bool exists(const std::string& f)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(f), ec);
}

// the commands after 'from', split at lone "+"
std::vector<std::vector<std::string>> commandsOf(const std::vector<std::string>& a, size_t from)
{
    std::vector<std::vector<std::string>> out(1);
    for (size_t i = from; i < a.size(); ++i) {
        if (a[i] == "+") { out.emplace_back(); continue; }
        out.back().push_back(a[i]);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::vector<std::string>& c) { return c.empty(); }), out.end());
    return out;
}

bool readScript(const std::string& file, std::vector<std::vector<std::string>>* out, std::string* why)
{
    std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
    if (!in) { *why = "cannot read " + file; return false; }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (n == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line = line.substr(3);
        bool ok = true;
        std::vector<std::string> words = splitLine(line, &ok);
        if (!ok) { *why = file + " line " + std::to_string(n) + ": a quote is not closed"; return false; }
        if (!words.empty()) out->push_back(words);
    }
    return true;
}

int runCommands(Session& s, const std::vector<std::vector<std::string>>& cmds, size_t* failedAt)
{
    for (size_t i = 0; i < cmds.size(); ++i) {
        std::string why;
        if (!s.run(cmds[i], &why)) {
            std::string all;
            for (const std::string& w : cmds[i]) all += (all.empty() ? "" : " ") + w;
            problem(all + ": " + why);
            if (failedAt) *failedAt = i;
            return kFailed;
        }
    }
    return kOk;
}

}  // namespace

bool isProjectCliCommand(const std::string& cmd) { return cmd == "new" || cmd == "edit" || cmd == "info" || cmd == "fx-list" || cmd == "fx-params"; }

int runProjectCli(const std::vector<std::string>& args, AppState& app, bool quiet)
{
    gQuietCli = quiet;
    if (args.empty()) return kUsage;
    const std::string cmd = args[0];
    if (cmd == "fx-list") return fxList(std::vector<std::string>(args.begin() + 1, args.end()));
    if (cmd == "fx-params") return fxParams(app, std::vector<std::string>(args.begin() + 1, args.end()));
    if (args.size() < 2) { problem(cmd + ": no project file given"); return kUsage; }
    const std::string file = args[1];
    Session s(app);
    std::string why;

    if (cmd == "info") {
        std::string err;
        if (!app.open(file, &err)) { problem("cannot open " + file + (err.empty() ? "" : ": " + err)); return kCantOpen; }
        std::vector<std::string> c{"info"};
        for (size_t i = 2; i < args.size(); ++i) c.push_back(args[i]);
        return s.run(c, &why) ? kOk : (problem(why), kUsage);
    }

    // new / edit: their own options before the first command
    std::string output = file, script;
    bool overwrite = false, vegasDefaults = false;
    std::vector<std::string> settingsArgs{"settings"};
    size_t i = 2;
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-y" || a == "--overwrite") overwrite = true;
        else if ((a == "-o" || a == "--output") && i + 1 < args.size()) output = args[++i];
        else if (a == "--script" && i + 1 < args.size()) script = args[++i];
        else if (cmd == "new" && a == "--vegas-defaults") vegasDefaults = true;
        else if (cmd == "new" && (a == "--size" || a == "--fps" || a == "--sample-rate" || a == "--channels" || a == "--quality" || a == "--pixel-format") && i + 1 < args.size()) {
            settingsArgs.push_back(a);
            settingsArgs.push_back(args[++i]);
        } else break;
    }
    std::vector<std::vector<std::string>> cmds;
    if (!script.empty() && !readScript(script, &cmds, &why)) { problem(why); return kUsage; }
    for (auto& c : commandsOf(args, i)) cmds.push_back(c);
    const std::string ext = lower(std::filesystem::u8path(output).extension().u8string());
    if (ext != ".reno" && ext != ".veg") { problem(output + ": a project is saved as .reno or .veg"); return kUsage; }

    if (cmd == "new") {
        if (exists(output) && !overwrite) { problem(output + " exists (use -y to replace it)"); return kExists; }
        app.newProject(vegasDefaults);
        if (settingsArgs.size() > 1) cmds.insert(cmds.begin(), settingsArgs);
    } else {
        if (cmds.empty()) { problem("edit " + file + ": no commands given"); return kUsage; }
        std::string err;
        if (!app.open(file, &err)) { problem("cannot open " + file + (err.empty() ? "" : ": " + err)); return kCantOpen; }
        if (output != file && exists(output) && !overwrite) { problem(output + " exists (use -y to replace it)"); return kExists; }
    }
    const int code = runCommands(s, cmds, nullptr);
    if (code != kOk) {
        problem(output + " was not saved");
        return code;
    }
    std::string err;
    std::error_code ec;
    const std::string absOut = std::filesystem::absolute(std::filesystem::u8path(output), ec).u8string();
    if (!app.save(ec ? output : absOut, &err)) { problem("cannot save " + output + (err.empty() ? "" : ": " + err)); return kCantOpen; }
    for (const std::string& w : app.saveWarnings()) say("warning: " + w);
    say("Saved " + output);
    return kOk;
}

}  // namespace reno
