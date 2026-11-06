// reno_cli model: a project as JSON in the schema RenoCoverage.cs (tools/vegas) writes from VEGAS's object model.
// The names are those of VEGAS's scripting API (ScriptPortal.Vegas) with enumerations as their names, so the two sides
// can be compared field by field (tools/vegcov_compare.py). A field is written only when Reno holds it; the compare
// tool lists what only one side has apart from what differs.
#include "modeldump.h"
#include "../vegas/vegas.h"
#include "../vegas/vegorigin.h"
#include "../engine/fxnames.h"
#include "../media/media.h"
#include <cctype>
#include <cstring>
#include <map>
#include "../core/base.h"
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace reno {
MotionKey motionKeyAt(const std::vector<MotionKey>& keys, double t);     // (engine_video.cpp)
namespace {

// VEGAS's Timecode.Nanos are 100 ns units (the manifest from VEGAS says so itself: nanosPerSecond)
constexpr double kNanosPerSecond = 1e7;

int64_t nanos(double seconds) { return (int64_t)std::llround(seconds * kNanosPerSecond); }

Json obj() { return Json::object(); }
Json arr() { return Json::array(); }

std::string baseName(const std::string& path)
{
    const size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

// A subclip's own name (as VEGAS lists it), from its media record: it follows the record's path text and the two numbers
// ahead of it (the same place the take's name is read from below).
std::string subclipNameOf(const VegMediaRecord& rec)
{
    if (!rec.chunk) return std::string();
    const veg::Bytes& m = rec.chunk->data;
    const size_t at = (size_t)m.u32(0x24) + m.u32(0x28) + m.u32(0x2c) + m.u32(0);
    return m.text(at, m.u32(0x40));
}

bool endsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suffix[i])) return false;
    return true;
}

// The key that ties a take to its media, as the script makes it: the file name, "gen:" and the generator's id, a
// subclip's own name, or for an image sequence "first.png - last.png" (VEGAS names the media "first - last")
// A generator's key is "gen:" and the name VEGAS keeps in its path: "{Svfx:<OpenFX id>}" for an OpenFX generator, the class GUID
// in braces for an older DirectX one. Read from the media record when there is one (Reno's own id for a DirectX generator is
// its built-in's), else made the OpenFX way.
std::string generatorKeyOf(const MediaItem& m)
{
    if (const auto* o = vegOriginAs<VegMediaOrigin>(m.veg, VegOrigin::MediaK)) {
        if (o->chunk) {
            const veg::Bytes& b = o->chunk->data;
            const uint32_t hs = b.u32(0), len = b.u32(0x24);
            static const std::string prefix = "META:\\Video Generator\\";
            if (hs >= 0x40 && b.has(hs, len)) {
                const std::string raw = b.text(hs, len);
                const size_t end = raw.rfind(" (");
                if (raw.compare(0, prefix.size(), prefix) == 0 && end != std::string::npos && end > prefix.size())
                    return "gen:" + raw.substr(prefix.size(), end - prefix.size());
            }
        }
    }
    return "gen:{Svfx:" + m.generator.pluginId + "}";
}

std::string mediaKeyOf(const MediaItem& m)
{
    if (m.generated) return generatorKeyOf(m);
    if (m.subclipLength > 0 && !m.name.empty()) return m.name;
    if (m.sequenceFps > 0) {
        const size_t sep = m.name.find(" - ");
        const std::string file = baseName(m.path);
        const size_t dot = file.find_last_of('.');
        const std::string ext = dot == std::string::npos ? std::string() : file.substr(dot);
        if (sep != std::string::npos) return m.name.substr(0, sep) + ext + " - " + m.name.substr(sep + 3) + ext;
    }
    return baseName(m.path);
}

// Reno's curve (Curve) as VEGAS's CurveType name (fades and envelope points)
const char* curveTypeName(int c)
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

// Reno's KeyCurve as VEGAS's VideoKeyframeType name (pan/crop and track motion keys)
const char* keyframeTypeName(int c)
{
    switch (c) {
    case KeyHold: return "Hold";
    case KeySmooth: return "Smooth";
    case KeyFast: return "Fast";
    case KeySlow: return "Slow";
    case KeySharp: return "Sharp";
    default: return "Linear";
    }
}

// Reno's OpenFX key curve (Param::KeyCurve, the numbers kept in "kc") as VEGAS's OFXInterpolationType name
const char* ofxInterpName(int c)
{
    switch (c) {
    case 1: return "Fast";
    case 2: return "Slow";
    case 3: return "Smooth";
    case 4: return "Sharp";
    case 5: return "Hold";
    default: return "Linear";
    }
}

const char* compositeName(const std::string& m)
{
    static const struct { const char* reno; const char* vegas; } t[] = {
        {"srcalpha", "SrcAlpha"}, {"3dsrcalpha", "SrcAlpha3D"}, {"add", "Add"}, {"subtract", "Subtract"}, {"multiply", "Multiply"},
        {"cut", "Cut"}, {"screen", "Screen"}, {"overlay", "Overlay"}, {"hardlight", "HardLight"}, {"dodge", "Dodge"}, {"burn", "Burn"},
        {"darken", "Darken"}, {"lighten", "Lighten"}, {"difference", "Difference"}, {"differencesquared", "DifferenceSquared"},
        {"custom", "Custom"}};
    for (const auto& e : t) if (m == e.reno) return e.vegas;
    return nullptr;
}

const char* rulerFormatName(const std::string& f)
{
    static const struct { const char* reno; const char* vegas; } t[] = {
        {"samples", "Samples"}, {"time", "Time"}, {"seconds", "Seconds"}, {"timecode", "TimeAndFrames"}, {"frames", "AbsoluteFrames"},
        {"measures", "MeasuresAndBeats"}, {"feet16", "FeetAndFrames16mm"}, {"feet35", "FeetAndFrames35mm"}, {"smpteIVTC", "SmpteFilmSyncIVTC"},
        {"smpte24", "SmpteFilmSync"}, {"smpteEBU", "SmpteEBU"}, {"smpteNonDrop", "SmpteNonDrop"}, {"smpteDrop", "SmpteDrop"},
        {"smpte30", "Smpte30"}, {"cdtime", "AudioCDTime"}, {"nanoseconds", "Nanoseconds"}, {"microseconds", "Microseconds"}};
    for (const auto& e : t) if (f == e.reno) return e.vegas;
    return nullptr;
}

std::string qualityName(const std::string& q)
{
    if (q == "draft") return "Draft";
    if (q == "preview") return "Preview";
    if (q == "best") return "Best";
    return "Good";
}

Json xy(double x, double y)
{
    Json j = obj();
    j["x"] = x;
    j["y"] = y;
    return j;
}

// ---------------------------------------------------------------- effects
Json valueArray(const Json& e)
{
    Json v = arr();
    for (size_t i = 0; i < e.get("v").size(); ++i) v.push(e.get("v").at(i).asDouble());
    return v;
}

std::string upperText(std::string s)
{
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// The names of VEGAS's older video effects that Reno does not host (kept in the file but not in the chain), by class GUID.
const char* olderFxName(const std::string& guid)
{
    static const struct { const char* guid; const char* name; } t[] = {
        {"F4205AB4-E8C1-468F-9AD2-4FEB8DF04538", "VEGAS Deform"},
        {"D9A65BA3-0D06-11D2-9AAC-00A0C99B12C5", "VEGAS Film Effects"},
        {"B78CBE2C-9292-4C7C-9682-5D8A71CC984B", "VEGAS Film Grain"},
        {"300F2F5F-D7C7-48BE-B796-90FC740B654E", "VEGAS Gradient Map"},
        {"0065ED0E-1232-42BF-A455-133867EE9A4A", "VEGAS Soft Contrast"},
        {"C3062BE8-E69D-11D1-9AA9-00A0C99B12C5", "VEGAS Threshold"}};
    for (const auto& e : t) if (guid == e.guid) return e.name;
    return nullptr;
}

// An effect VEGAS keeps as a record of its older kind (no OpenFX parameters; the API answers IsOFX with an error for these, which
// the manifest has as null): its identity is the class GUID, Reno's own identifier for it is a different thing.
Json olderEffectModel(const std::string& name, const VegOlderFx& f)
{
    Json j = obj();
    j["name"] = name;
    j["uniqueId"] = f.classGuid;
    j["classId"] = f.classGuid;
    j["isAudio"] = false;
    j["isVideo"] = true;
    j["isOFX"] = false;
    j["bypass"] = f.bypass;
    j["applyBeforePanCrop"] = f.beforePanCrop;
    if (!f.preset.empty()) j["preset"] = f.preset;
    return j;
}

// A VST2 plug-in's CLSID as VEGAS prints it, from the 32 hex digits the project keeps (the first three fields are stored
// little-endian, so they are flipped back).
std::string clsidText(const std::string& hex32)
{
    if (hex32.size() != 32) return std::string();
    std::string h = hex32;
    auto flip = [&](size_t at, size_t bytes) { for (size_t i = 0; i < bytes / 2; ++i) std::swap_ranges(h.begin() + at + 2 * i, h.begin() + at + 2 * i + 2, h.begin() + at + 2 * (bytes - 1 - i)); };
    flip(0, 4);
    flip(8, 2);
    flip(12, 2);
    return upperText(h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12));
}

Json effectModel(const FxSlot& s, double fps)
{
    Json j = obj();
    if (s.kind == "audio") {
        j["isOFX"] = false;
        j["isAudio"] = true;
        j["isVideo"] = false;
        std::string name = s.audio.name.empty() ? s.name : s.audio.name;
        // Reno's own versions of VEGAS's DirectX track effects are labelled "VEGAS Track EQ"...; the plug-in is named "Track EQ"
        if (s.audio.uid.compare(0, 6, "dshow:") == 0 && name.compare(0, 6, "VEGAS ") == 0) name = name.substr(6);
        j["name"] = name;
        j["format"] = s.audio.format;
        j["uid"] = s.audio.uid;
        // VEGAS's unique id / class id: a DirectX plug-in's CLSID, a VST2 plug-in's CLSID as the project keeps it
        std::string guid;
        const size_t b = s.audio.uid.find('{'), e = s.audio.uid.find('}');
        if (s.audio.uid.compare(0, 6, "dshow:") == 0 && b != std::string::npos && e != std::string::npos && e > b) guid = upperText(s.audio.uid.substr(b + 1, e - b - 1));
        else if (s.audio.format == "VST2") guid = clsidText(s.audio.vegasClsid);
        if (!guid.empty()) {
            j["uniqueId"] = guid;
            j["classId"] = guid;
        }
        j["bypass"] = s.bypass;
        return j;
    }
    VegOlderFx older;
    const std::string label = fxSlotLabel(s);
    if (vegOlderFxOf(s, older)) {
        // Reno hosts a few of the older effects and transitions as its own OpenFX ones (Swirl, Wave, 3D Blinds...), whose parameters it
        // keeps in its own terms: not VEGAS's parameters, so under another name
        Json o = olderEffectModel(label, older);
        o["bypass"] = s.bypass;
        o["applyBeforePanCrop"] = s.beforePanCrop;
        o["renoPluginId"] = s.pluginId;
        return o;
    }
    j["isOFX"] = true;
    j["isAudio"] = false;
    j["isVideo"] = true;
    j["name"] = label;
    j["uniqueId"] = s.pluginId;
    j["classId"] = "00000000-0000-0000-0000-000000000000";      // every OpenFX effect has the empty class id
    if (!s.preset.empty()) j["preset"] = s.preset;
    j["bypass"] = s.bypass;
    j["applyBeforePanCrop"] = s.beforePanCrop;
    Json ps = obj();
    for (const auto& m : s.state.get("params").members()) {
        const Json& e = m.second;
        Json p = obj();
        if (e.has("s")) {
            p["value"] = e.get("s").asString();
        } else {
            // a parameter without a value (a group, a page, a button) is not one VEGAS persists: the manifest lists it by name in paramsOther
            if (e.get("v").size() == 0 && e.get("k").size() == 0) continue;
            p["value"] = valueArray(e);
            if (e.has("k")) {
                Json ks = arr();
                // curves of the keys: [[frame, curve], ...] for those that are not linear
                for (size_t k = 0; k < e.get("k").size(); ++k) {
                    const Json& key = e.get("k").at(k);
                    Json one = obj();
                    one["timeNanos"] = nanos(fps > 0 ? key.at(0).asDouble() / fps : 0);
                    Json v = arr();
                    for (size_t i = 1; i < key.size(); ++i) v.push(key.at(i).asDouble());
                    one["value"] = v;
                    int curve = 0;
                    for (size_t c = 0; c < e.get("kc").size(); ++c)
                        if (std::fabs(e.get("kc").at(c).at(0).asDouble() - key.at(0).asDouble()) < 1e-9) curve = e.get("kc").at(c).at(1).asInt();
                    one["interpolation"] = ofxInterpName(curve);
                    ks.push(one);
                }
                p["keyframes"] = ks;
            }
        }
        ps[m.first] = p;
    }
    j["params"] = ps;
    return j;
}

// A chain: the effects Reno hosts, with the ones it keeps without hosting (older kinds) put back where the file has them.
// 'media' (events only): media effects are not the event's, they go with the media (media[].effects).
Json chainModel(const FxChain& c, double fps, const std::vector<VegOlderFx>& hidden = std::vector<VegOlderFx>(), std::vector<Json>* mediaFx = nullptr)
{
    Json a = arr();
    size_t h = 0;
    for (size_t i = 0; i <= c.size(); ++i) {
        while (h < hidden.size() && hidden[h].before <= i) {
            const char* nm = olderFxName(hidden[h].classGuid);
            a.push(olderEffectModel(nm ? nm : "", hidden[h]));
            ++h;
        }
        if (i == c.size()) break;
        const auto* o = vegOriginAs<VegFxOrigin>(c[i].veg, VegOrigin::FxK);
        if (o && o->mediaFx) {
            if (mediaFx) mediaFx->push_back(effectModel(c[i], fps));
            continue;
        }
        a.push(effectModel(c[i], fps));
    }
    return a;
}

// ---------------------------------------------------------------- envelopes
std::string envelopeTypeName(const Envelope& e, const Project& p)
{
    if (e.kind == "volume") return "Volume";
    if (e.kind == "pan") return "Pan";
    if (e.kind == "mute") return "Mute";
    if (e.kind == "composite") return "Composite";
    if (e.kind == "fadecolor") return "FadeToColor";
    if (e.kind == "velocity") return "Velocity";
    if (e.kind == "transition") return "TransitionProgress";
    if (e.kind == "motionblur") return "MotionBlurLength";
    if (e.kind == "supersample") return "VideoSupersampling";
    if (e.kind == "send") {
        int idx = -1;
        for (size_t i = 0; i < p.buses.size(); ++i) if (p.buses[i].id == e.target) idx = (int)i;
        if (idx >= 0 && idx < 26) return std::string("Bus") + (char)('A' + idx);
        return "Bus";
    }
    if (e.kind == "fx") return "FX";
    return e.kind;
}

Json envelopesModel(const Envelopes& list, const Project& p)
{
    Json a = arr();
    for (const Envelope& e : list) {
        Json j = obj();
        j["type"] = envelopeTypeName(e, p);
        // VEGAS names an envelope after its type and reports its range (the values its Envelope.Min/Max/Neutral gave in
        // the Coverage run); the same for every owner, so the table is by type
        j["name"] = j.get("type").asString();
        {
            const std::string t = j.get("type").asString();
            double lo = 0, hi = 1, neutral = 1;
            if (t == "Volume" || t.compare(0, 3, "Bus") == 0) hi = 2;
            else if (t == "Pan" || t == "FadeToColor") { lo = -1; neutral = 0; }
            else if (t == "MotionBlurLength") neutral = 0;
            else if (t == "TransitionProgress") neutral = 0.5;
            else if (t == "Velocity") { lo = -1; hi = 10; }
            else if (t == "VideoSupersampling") { lo = 1; hi = 8; }
            j["min"] = lo;
            j["max"] = hi;
            j["neutral"] = neutral;
        }
        Json pts = arr();
        for (const EnvPoint& pt : e.points) {
            Json q = obj();
            q["xNanos"] = nanos(pt.time);
            q["y"] = pt.value;
            q["curve"] = curveTypeName(pt.curve);
            pts.push(q);
        }
        j["points"] = pts;
        a.push(j);
    }
    return a;
}

// ---------------------------------------------------------------- tracks and events
Json colorModel(uint32_t argb)
{
    Json j = arr();
    j.push((int)((argb >> 16) & 255));
    j.push((int)((argb >> 8) & 255));
    j.push((int)(argb & 255));
    j.push((int)((argb >> 24) & 255));
    return j;     // r, g, b, a
}


using MediaFxMap = std::map<uint64_t, std::vector<Json>>;

Json eventModel(const Event& e, const Track& t, const Project& p, const std::vector<uint64_t>& groupIds, double fps, MediaFxMap& mediaFx)
{
    Json j = obj();
    const bool video = t.type == TrackType::Video;
    j["type"] = video ? "video" : "audio";
    j["name"] = e.name;
    j["startNanos"] = nanos(e.start);
    j["lengthNanos"] = nanos(e.length);
    j["playbackRate"] = e.rate;
    j["mute"] = e.mute;
    j["locked"] = e.locked;
    j["loop"] = e.loop;
    j["snapOffsetNanos"] = nanos(e.snapOffset);
    int g = -1;
    if (e.group) for (size_t i = 0; i < groupIds.size(); ++i) if (groupIds[i] == e.group) g = (int)i;
    j["group"] = g;

    // takes in VEGAS's order, the active one marked
    Json takes = arr();
    // Subclips are not kept as media of their own (the reader plays the subclip's file with the start added to the
    // offset); VEGAS names a subclip's take media by the subclip's own name and counts the take's offset from its
    // start, in the event's time. For an event as read, both are taken from the file's take records.
    struct RawTake { bool subclip = false; std::string key; double offset = 0; };
    std::vector<RawTake> raw;
    if (const auto* eo = vegOriginAs<VegEventOrigin>(e.veg, VegOrigin::EventK)) {
        const auto* po = vegOriginAs<VegProjectOrigin>(p.veg, VegOrigin::ProjectK);
        std::vector<const veg::Chunk*> chunks;
        if (eo->chunk) chunks = eo->chunk->children(veg::G::Take);
        bool allOk = po && chunks.size() == eo->takesRead.size() && chunks.size() == e.takes.size() + 1 && e.mediaId == eo->asRead.mediaId &&
                     e.offset == eo->asRead.offset && e.takes.size() == eo->asRead.takes.size();
        for (size_t i = 0; allOk && i < eo->takesOk.size(); ++i) allOk = allOk && eo->takesOk[i];
        if (allOk) {
            for (const veg::Chunk* c : chunks) {
                RawTake r;
                r.offset = (double)c->data.i64(0x10) / 1e7;
                for (const auto& rec : po->media)
                    if (rec.vegId == c->data.u32(0x24) && rec.kind == VegMediaRecord::Subclip && rec.chunk) {
                        r.subclip = true;
                        r.key = subclipNameOf(rec);
                    }
                raw.push_back(r);
            }
        }
    }
    int dumped = 0;
    auto addTake = [&](uint64_t mediaId, double offset, const std::string& name, bool active, bool reverse) {
        const MediaItem* m = p.findMedia(mediaId);
        Json k = obj();
        const RawTake* rt = (size_t)dumped < raw.size() ? &raw[(size_t)dumped] : nullptr;
        ++dumped;
        k["mediaKey"] = rt && rt->subclip ? rt->key : (m ? mediaKeyOf(*m) : std::string());
        // the offset: the file's value, else the media position over the rate (the reader multiplies the file's by it)
        (void)reverse;
        const double off = rt && rt->subclip ? rt->offset : offset / (e.rate > 0 ? e.rate : 1.0);
        // the take's name: its own, else (as VEGAS does) the media's file name without the extension
        std::string nm = name;
        if (nm.empty() && m && m->sequenceFps > 0 && m->name.find(" - ") != std::string::npos) {
            nm = m->name;
        } else if (nm.empty() && m && !m->generated) {
            nm = baseName(m->path);
            const size_t dot = nm.find_last_of('.');
            if (dot != std::string::npos && dot > 0) nm = nm.substr(0, dot);
        }
        k["name"] = nm;
        k["offsetNanos"] = nanos(off);
        k["isActive"] = active;
        takes.push(k);
    };
    const int before = std::min<int>(e.activeTake, (int)e.takes.size());
    // the active take's own name was read from the file (the event's name is another thing in VEGAS)
    std::string activeName = e.name;
    if (const auto* o = vegOriginAs<VegEventOrigin>(e.veg, VegOrigin::EventK))
        if (o->activeTake < o->takesRead.size() && e.name == o->asRead.name) activeName = o->takesRead[o->activeTake].name;
    for (int i = 0; i < before; ++i) addTake(e.takes[i].mediaId, e.takes[i].offset, e.takes[i].name, false, e.takes[i].reverse);
    addTake(e.mediaId, e.offset, activeName, true, e.reverse);
    for (size_t i = before; i < e.takes.size(); ++i) addTake(e.takes[i].mediaId, e.takes[i].offset, e.takes[i].name, false, e.takes[i].reverse);
    j["takes"] = takes;
    j["activeTake"] = before;

    Json fin = obj();
    // Where this event starts inside an earlier one that ends before it does, the shared stretch IS the cross-fade, and VEGAS
    // reports it as the fade-in length whatever shorter length the file keeps (163 of 163 such overlaps in the Coverage run;
    // one set 0.5 s over a 2 s overlap and read 2 s). Reno plays the shared stretch as the cross-fade either way.
    double fadeInLen = e.fadeIn;
    {
        double best = -1;
        for (const Event& o : t.events) {
            if (o.id == e.id || !(o.start < e.start) || !(o.end() < e.end()) || !(o.end() > e.start)) continue;
            if (o.start > best) { best = o.start; fadeInLen = o.end() - e.start; }
        }
    }
    fin["lengthNanos"] = nanos(fadeInLen);
    fin["curve"] = curveTypeName(e.fadeInCurve);
    // VEGAS's Fade.Gain of both fades is the event's own level (setting one changed the other and the event's: the
    // Coverage run set 0.25 and 0.5 and read 0.5 twice)
    fin["gain"] = e.gain;
    fin["reciprocalCurve"] = curveTypeName(effectiveFadeInRecipCurve(t, e));
    if (video && !e.transition.kind.empty()) fin["transition"] = effectModel(e.transition, fps);
    else fin["transition"] = Json();
    j["fadeIn"] = fin;
    Json fout = obj();
    fout["lengthNanos"] = nanos(e.fadeOut);
    fout["curve"] = curveTypeName(e.fadeOutCurve);
    fout["gain"] = e.gain;
    j["fadeOut"] = fout;

    j["envelopes"] = envelopesModel(e.envelopes, p);
    {
        std::vector<Json> mfx;
        j["effects"] = chainModel(e.fx, fps, vegHiddenFxOf(e), &mfx);
        // the media's own effects are listed once, with the media
        if (!mfx.empty() && !mediaFx.count(e.mediaId)) mediaFx[e.mediaId] = mfx;
    }

    if (video) {
        Json v = obj();
        v["undersampleRate"] = e.undersample;
        v["maintainAspectRatio"] = e.keepAspect;
        v["reduceInterlace"] = e.reduceFlicker;
        v["stretchToFill"] = e.stretchToFill;   // (not VEGAS's ScaleToFill: that one is true by default and where it is kept is not known)
        v["resampleMode"] = e.resample == "force" ? "Force" : e.resample == "off" ? "Disable" : "Smart";
        // pan/crop in Reno's own terms (fractions of the media's picture; the compare tool converts VEGAS's corners)
        Json keys = arr();
        for (const PanCropKey& k : e.panCrop) {
            Json q = obj();
            q["positionNanos"] = nanos(k.time);
            q["cx"] = k.cx;
            q["cy"] = k.cy;
            q["w"] = k.w;
            q["h"] = k.h;
            q["rotation"] = k.rotation;
            q["smoothness"] = k.smoothness;
            q["type"] = keyframeTypeName(k.curve);
            keys.push(q);
        }
        v["panCropNative"] = keys;
        j["video"] = v;
    } else {
        Json a = obj();
        a["normalize"] = e.normalize;
        a["normalizeGain"] = e.normalizeGain;
        a["invertPhase"] = e.invertPhase;
        const std::string& ch = e.channels;
        a["channels"] = ch == "swap" ? "Swap" : ch == "combine" ? "Mono" : ch == "muteleft" ? "MuteLeft" : ch == "muteright" ? "MuteRight" : ch == "left" ? "DisableRight" : ch == "right" ? "DisableLeft" : "None";
        a["method"] = e.stretchMethod == "classic" ? "Classic" : e.stretchMethod == "none" ? "None" : "Elastique";
        // VEGAS's PitchLock is the "lock pitch to stretch" switch (flag 0x02): the pitch follows the rate, which is the
        // opposite of Reno's keepPitch (the pitch stays when the rate changes)
        if (e.stretchMethod != "none") {
            a["pitchLock"] = !e.keepPitch;
            a["pitchSemis"] = e.pitch;
        }
        if (e.stretchMethod == "classic") a["classicAttribute"] = e.stretchMode;
        else if (e.stretchMethod == "elastique")
            a["elastiqueAttribute"] = e.stretchMode == "efficient" ? "Efficient" : e.stretchMode == "soloist-mono" ? "Soloist_Monophonic"
                                    : e.stretchMode == "soloist-speech" ? "Soloist_Speech" : "Pro";
        // VEGAS reads the formant settings only for the Pro élastique mode (they threw for the others in the Coverage run)
        if (e.stretchMethod == "elastique" && e.stretchMode != "efficient" && e.stretchMode != "soloist-mono" && e.stretchMode != "soloist-speech") {
            a["formantLock"] = e.preserveFormants;
            a["formantShift"] = e.formant;
        }
        j["audio"] = a;
    }
    return j;
}

Json trackModel(const Track& t, int index, const Project& p, const std::vector<uint64_t>& groupIds, double fps, MediaFxMap& mediaFx)
{
    Json j = obj();
    const bool video = t.type == TrackType::Video;
    j["index"] = index;
    j["type"] = video ? "video" : "audio";
    j["name"] = t.name;
    j["mute"] = t.mute;
    j["solo"] = t.solo;
    j["envelopes"] = envelopesModel(t.envelopes, p);
    j["effects"] = chainModel(t.fx, fps, vegHiddenFxOf(t));
    if (video) {
        Json v = obj();
        v["compositeLevel"] = t.level;
        const char* cm = compositeName(t.composite);
        if (cm) v["compositeMode"] = cm;
        v["compositeNestingLevel"] = t.compositeLevel;
        // A track is a compositing child when it is nested (compositeLevel > 0) and a parent when the video track after it is
        // nested deeper; VEGAS gives the parent composite mode "Invalid" for a track that is not a parent.
        const bool child = t.compositeLevel > 0;
        bool parent = false;
        for (size_t k = (size_t)index + 1; k < p.tracks.size(); ++k) {
            if (p.tracks[k].type != TrackType::Video) continue;
            parent = p.tracks[k].compositeLevel > t.compositeLevel;
            break;
        }
        v["isCompositingChild"] = child;
        v["isCompositingParent"] = parent;
        const char* pm = parent ? compositeName(t.parentComposite) : nullptr;
        v["parentCompositeMode"] = pm ? pm : "Invalid";
        v["topFadeColor"] = colorModel(t.fadeTop);
        v["bottomFadeColor"] = colorModel(t.fadeBottom);
        v["bypassMotionBlur"] = t.bypassMotionBlur;
        Json mk = arr();
        for (const MotionKey& k : t.motion) {
            Json q = obj();
            q["positionNanos"] = nanos(k.time);
            q["x"] = k.x;
            q["y"] = k.y;
            q["w"] = k.w;
            q["h"] = k.h;
            q["rotation"] = k.rotation;
            q["smoothness"] = k.smoothness;
            q["type"] = keyframeTypeName(k.curve);
            // 3D Track Motion (the compare tool converts the script's values to these): see MotionKey
            q["z"] = k.z;
            q["d"] = k.d;
            q["rotX"] = k.rotX;
            q["rotY"] = k.rotY;
            q["oriX"] = k.oriX;
            q["oriY"] = k.oriY;
            q["oriZ"] = k.oriZ;
            q["offX"] = k.offX;
            q["offY"] = k.offY;
            q["offZ"] = k.offZ;
            mk.push(q);
        }
        v["motionNative"] = mk;
        v["shadowEnabled"] = !t.shadow2d.empty();
        v["glowEnabled"] = !t.glow2d.empty();
        // Track Motion's shadow and glow in the script's units (pixels from the frame's middle, y up; degrees; blur and
        // intensity in percent). The VEGAS side has keys even while they are off; the compare tool leaves those out.
        {
            const double W = p.settings.width, H = p.settings.height;
            // shadow and glow keys are the motion's place and size at their time, moved and scaled
            auto glow = [&](const std::vector<GlowKey>& keys) {
                Json a = arr();
                for (const GlowKey& g : keys) {
                    const MotionKey m = motionKeyAt(t.motion, g.time);
                    Json q = obj();
                    q["positionNanos"] = nanos(g.time);
                    q["smoothness"] = 0;
                    q["type"] = keyframeTypeName(g.curve);
                    q["x"] = (m.x - 0.5) * W + g.x * W;
                    q["y"] = (0.5 - m.y) * H - g.y * H;
                    q["w"] = m.w * W * g.w;
                    q["h"] = m.h * H * g.h;
                    q["rotationZ"] = m.rotation;
                    q["rotationOffsetX"] = m.offX * W;
                    q["rotationOffsetY"] = -m.offY * W;
                    q["orientationZ"] = m.oriZ;
                    q["blur"] = g.blur * 100.0;
                    q["intensity"] = g.intensity * 100.0;
                    q["color"] = colorModel(g.color);
                    a.push(q);
                }
                return a;
            };
            if (!t.shadow2d.empty()) v["shadow"] = glow(t.shadow2d);
            if (!t.glow2d.empty()) v["glow"] = glow(t.glow2d);
        }
        j["video"] = v;
    } else {
        Json a = obj();
        a["volume"] = t.level;
        a["panX"] = t.pan;
        a["invertPhase"] = t.invertPhase;
        Json bus = obj();
        bus["isMaster"] = t.output == 0;
        int bi = -1;
        for (size_t i = 0; i < p.buses.size(); ++i) if (p.buses[i].id == t.output) bi = (int)i;
        bus["index"] = bi;
        a["busTrack"] = bus;
        j["audio"] = a;
    }
    Json evs = arr();
    for (const Event& e : t.events) evs.push(eventModel(e, t, p, groupIds, fps, mediaFx));
    j["events"] = evs;
    return j;
}

Json markerModel(const Project::Marker& m)
{
    Json j = obj();
    j["positionNanos"] = nanos(m.time);
    j["label"] = m.name;
    if (m.region || m.kind == "cdtrack") j["lengthNanos"] = nanos(m.length);
    if (m.kind == "command") {
        // Reno keeps the marker's text in 'name' (the command's parameter), the command's kind in 'command' and the
        // comment apart; VEGAS's CommandMarker has CommandType, CommandParameter, and a Label that is the comment.
        j["label"] = m.comment;
        j["commandType"] = m.command;
        j["commandParameter"] = m.name;
    }
    return j;
}

}  // namespace

Json projectModelJson(const Project& p)
{
    Json root = obj();
    root["schema"] = "reno-model/1";
    root["source"] = "reno";
    root["nanosPerSecond"] = kNanosPerSecond;
    const ProjectSettings& s = p.settings;
    const double fps = s.fps();

    Json proj = obj();
    Json v = obj();
    // VEGAS's Video.Width/Height give the picture as rendered: with a quarter-turn output rotation they are swapped
    // against the size the file holds (Coverage run: a 1080x1920 project turned clockwise says 1920x1080, a 640x360
    // one turned counterclockwise says 360x640; upside down changes nothing).
    const bool quarter = s.outputRotation == 90 || s.outputRotation == 270;
    v["width"] = quarter ? s.height : s.width;
    v["height"] = quarter ? s.width : s.height;
    v["frameRate"] = fps;
    v["fieldOrder"] = s.fieldOrder == "upper" ? "UpperFieldFirst" : s.fieldOrder == "lower" ? "LowerFieldFirst" : "ProgressiveScan";
    v["pixelAspectRatio"] = s.pixelAspect;
    v["renderQuality"] = qualityName(s.renderQuality);
    {
        const std::string& mb = s.motionBlur;
        v["motionBlurType"] = mb == "pyramid" ? "Pyramid" : mb == "box" ? "Box" : mb == "asymgaussian" ? "AsymmetricGaussian"
                            : mb == "asympyramid" ? "AsymmetricPyramid" : mb == "asymbox" ? "AsymmetricBox" : "Gaussian";
    }
    v["deinterlaceMethod"] = s.deinterlace == "blend" ? "BlendFields" : s.deinterlace == "interpolate" ? "InterpolateFields" : "None";
    v["pixelFormat"] = s.pixelFormat == "32bit" ? (s.pixelRange == "video" ? "Float32Bit" : "Float32BitFullRange") : "Int8Bit";
    v["outputRotation"] = s.outputRotation == 90 ? "QuarterTurnClockwise" : s.outputRotation == 180 ? "Inverted"
                        : s.outputRotation == 270 ? "QuarterTurnCounterclockwise" : "None";
    v["hdrMode"] = s.hdrMode == "hdr10" ? "HDR10" : s.hdrMode == "hlg" ? "HLG" : "Off";
    v["t360Output"] = s.output360 ? "On" : "Off";
    proj["video"] = v;
    Json a = obj();
    a["sampleRate"] = s.sampleRate;
    a["bitDepth"] = s.audioBits;
    a["masterBusMode"] = s.audioChannels == 6 ? "Surround" : "Stereo";
    a["resampleQuality"] = s.resampleQuality == "best" ? "Best" : s.resampleQuality == "good" ? "Good" : "Preview";
    a["lfeLowpassFilterEnabled"] = s.lfeLowpass;
    a["lfeLowpassFilterCutoffFrequency"] = s.lfeCutoff;
    proj["audio"] = a;
    Json r = obj();
    if (const char* rf = rulerFormatName(s.rulerFormat)) r["format"] = rf;
    r["startTimeNanos"] = nanos(s.rulerStart);
    r["beatsPerMinute"] = s.tempo;
    r["beatsPerMeasure"] = s.beatsPerMeasure;
    r["beatValue"] = s.noteGetsBeat;
    proj["ruler"] = r;
    Json sum = obj();
    sum["title"] = s.title;
    sum["artist"] = s.artist;
    sum["engineer"] = s.engineer;
    sum["copyright"] = s.copyright;
    sum["comments"] = s.comments;
    proj["summary"] = sum;
    Json cd = obj();
    cd["upc"] = s.cdUpc;
    cd["firstTrack"] = s.cdFirstTrack;
    proj["audioCD"] = cd;
    proj["lengthNanos"] = nanos(p.length());
    root["project"] = proj;

    MediaFxMap mediaFx;
    // event groups (ids of the non-zero groups in order of first use)
    std::vector<uint64_t> groupIds;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (e.group && std::find(groupIds.begin(), groupIds.end(), e.group) == groupIds.end()) groupIds.push_back(e.group);

    Json tracks = arr();
    for (size_t i = 0; i < p.tracks.size(); ++i) tracks.push(trackModel(p.tracks[i], (int)i, p, groupIds, fps, mediaFx));
    root["tracks"] = tracks;

    // media
    Json media = arr();
    // 'sub': the media is a subclip of m's file (Reno keeps only the file; the file's subclip records say what VEGAS lists)
    auto emitMedia = [&](const MediaItem& m, const VegMediaRecord* sub) {
        Json j = obj();
        if (sub) {
            j["kind"] = "subclip";
            j["key"] = subclipNameOf(*sub);
            j["fileName"] = subclipNameOf(*sub);
        } else if (m.generated) {
            j["kind"] = "generated";
            j["key"] = generatorKeyOf(m);
            VegOlderFx legacy;
            if (vegLegacyGeneratorOf(m, legacy)) {
                // one of the older DirectX generators, which Reno hosts as its own built-in: identity and preset are the file's
                Json g = olderEffectModel(m.generator.name.empty() ? m.name : m.generator.name, legacy);
                g["renoPluginId"] = m.generator.pluginId;
                j["generator"] = g;
            } else {
                j["generator"] = effectModel(m.generator, fps);
            }
        } else {
            j["kind"] = m.sequenceFps > 0 ? "sequence" : (m.subclipLength > 0 ? "subclip" : (endsWithNoCase(m.path, ".veg") ? "nested" : "file"));
            j["key"] = mediaKeyOf(m);
            j["fileName"] = baseName(m.path);
            // VEGAS names an image sequence "first.png - last.png"; Reno keeps "first - last" as the item's name
            if (m.sequenceFps > 0) {
                const size_t dash = m.name.find(" - ");
                const std::string file = baseName(m.path);
                const size_t dot = file.find_last_of('.');
                if (dash != std::string::npos && dot != std::string::npos) j["fileName"] = file + " - " + m.name.substr(dash + 3) + file.substr(dot);
            }
        }
        // The reader leaves a still picture's size to the moment it is first shown (opening every still would slow a project
        // with hundreds); the model states it, so the dump opens the file (a missing one stays 0, as VEGAS would show it offline)
        MediaInfo info = m.info;
        if (!m.generated && !m.offline && info.hasVideo && (info.width <= 0 || info.height <= 0 || (m.sequenceFps > 0 && info.duration <= 0))) {
            try {
                std::string err;
                std::unique_ptr<MediaReader> r = openMedia(m.sequenceFps > 0 ? imageSequenceSpec(m.path, m.sequenceFps) : m.path, &err);
                if (r) {
                    info.width = r->info().width;
                    info.height = r->info().height;
                    if (m.sequenceFps > 0) {
                        info.fps = r->info().fps;
                        info.duration = r->info().duration;
                        info.isStill = false;       // (the reader takes a numbered .png for a still)
                    }
                }
            } catch (...) {
            }
        }
        j["hasVideo"] = info.hasVideo;
        j["hasAudio"] = info.hasAudio;
        if (info.hasVideo) {
            j["width"] = info.width;
            j["height"] = info.height;
        }
        // what the media file itself says (read when the project was opened); VEGAS reports the same for a still as 0
        if (!sub && !m.generated) {
            j["lengthNanos"] = nanos(info.isStill || (m.sequenceFps <= 0 && info.hasVideo && !info.hasAudio && info.fps <= 0) ? 0.0 : info.duration);
            if (info.hasVideo) {
                j["frameRate"] = info.isStill ? 0.0 : info.fps;
                j["pixelAspectRatio"] = info.pixelAspect;
                j["fieldOrder"] = info.fieldOrder == "upper" ? "UpperFieldFirst" : info.fieldOrder == "lower" ? "LowerFieldFirst" : "ProgressiveScan";
            }
        }
        if (info.hasAudio) {
            j["sampleRate"] = info.sampleRate;
            j["channels"] = info.channels;
        }
        if (sub) {
            j["subclipStartNanos"] = nanos(sub->subStart);
            j["subclipLengthNanos"] = nanos(sub->subLen);
            j["subclipReversed"] = sub->subReversed;
        } else if (m.subclipLength > 0) {
            j["subclipStartNanos"] = nanos(m.subclipStart);
            j["subclipLengthNanos"] = nanos(m.subclipLength);
        }
        if (!sub) {
            auto it = mediaFx.find(m.id);
            if (it != mediaFx.end()) {
                Json fx = arr();
                for (const Json& f : it->second) fx.push(f);
                j["effects"] = fx;
            }
        }
        media.push(j);
    };
    // In the file's order when the project was read from one (subclips sit among the files in VEGAS's pool); items made since
    // follow.
    {
        std::vector<uint64_t> done;
        if (const auto* po = vegOriginAs<VegProjectOrigin>(p.veg, VegOrigin::ProjectK)) {
            for (const VegMediaRecord& rec : po->media) {
                const MediaItem* m = rec.renoId ? p.findMedia(rec.renoId) : nullptr;
                if (!m) continue;
                if (rec.kind == VegMediaRecord::Subclip && rec.chunk) {
                    emitMedia(*m, &rec);
                } else if (std::find(done.begin(), done.end(), m->id) == done.end()) {
                    done.push_back(m->id);
                    emitMedia(*m, nullptr);
                }
            }
        }
        for (const MediaItem& m : p.media)
            if (std::find(done.begin(), done.end(), m.id) == done.end()) emitMedia(m, nullptr);
    }
    root["media"] = media;


    Json buses = arr();
    for (size_t i = 0; i < p.buses.size(); ++i) {
        const Bus& b = p.buses[i];
        Json j = obj();
        j["index"] = (int)i;
        j["name"] = b.name;
        j["isFXBus"] = b.assignable;
        j["mute"] = b.mute;
        j["solo"] = b.solo;
        j["effects"] = chainModel(b.fx, fps);
        j["envelopes"] = envelopesModel(b.envelopes, p);
        buses.push(j);
    }
    root["audioBuses"] = buses;
    Json master = obj();
    master["effects"] = chainModel(p.audioOut, fps);
    master["envelopes"] = envelopesModel(p.masterEnvelopes, p);
    root["masterBus"] = master;
    Json vbus = obj();
    vbus["effects"] = chainModel(p.videoOut, fps, vegHiddenVideoOutFxOf(p));
    vbus["envelopes"] = envelopesModel(p.videoEnvelopes, p);
    vbus["topFadeColor"] = colorModel(p.videoFadeTop);
    vbus["bottomFadeColor"] = colorModel(p.videoFadeBottom);
    root["videoBus"] = vbus;

    Json markers = arr(), regions = arr(), cmds = arr(), cdTracks = arr(), cdIdx = arr();
    for (const Project::Marker& m : p.markers) {
        if (m.kind == "command") cmds.push(markerModel(m));
        else if (m.kind == "cdtrack") cdTracks.push(markerModel(m));
        else if (m.kind == "cdindex") cdIdx.push(markerModel(m));
        else if (m.region) regions.push(markerModel(m));
        else markers.push(markerModel(m));
    }
    root["markers"] = markers;
    root["regions"] = regions;
    root["commandMarkers"] = cmds;
    root["cdTracks"] = cdTracks;
    root["cdIndices"] = cdIdx;
    return root;
}

int runModelCommand(const std::vector<std::string>& args)
{
    std::string in, out, resave;
    bool notes = false;
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        if ((a == "-o" || a == "--output") && k + 1 < args.size()) out = args[++k];
        else if (a == "--resave" && k + 1 < args.size()) resave = args[++k];
        else if (a == "--notes") notes = true;
        else if (!a.empty() && a[0] != '-' && in.empty()) in = a;
        else {
            std::fputs("usage: reno_cli model <project.veg|.reno> [-o out.json] [--resave out.veg] [--notes]\n", stderr);
            return 2;
        }
    }
    if (in.empty()) {
        std::fputs("usage: reno_cli model <project.veg|.reno> [-o out.json] [--resave out.veg] [--notes]\n", stderr);
        return 2;
    }
    Project p;
    std::string error;
    std::vector<std::string> n;
    if (!loadProjectFileWithNotes(in, p, &error, &n)) {
        std::fprintf(stderr, "cannot open %s: %s\n", in.c_str(), error.c_str());
        return 3;
    }
    if (notes)
        for (const std::string& s : n) std::fprintf(stderr, "note: %s\n", s.c_str());
    // describe the project as read, before saving: saving re-points the effects' origins (media effects included)
    const std::string text = projectModelJson(p).dump(1);
    if (!resave.empty()) {
        const VegasSaveResult r = saveVegasProject(p, resave);
        if (!r.ok) {
            std::fprintf(stderr, "cannot save %s: %s\n", resave.c_str(), r.error.c_str());
            return 4;
        }
        for (const std::string& w : r.warnings) std::fprintf(stderr, "save warning: %s\n", w.c_str());
    }
    if (out.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
    } else {
        FILE* f = std::fopen(out.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", out.c_str());
            return 5;
        }
        std::fwrite(text.data(), 1, text.size(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    return 0;
}

}  // namespace reno
