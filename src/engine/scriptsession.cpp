// One run of a VEGAS script: answers the C interface (scriptapi.h) on a copy of the project. See scriptsession.h.
//
// What each handle kind keeps in its Desc:
//   RK_APP       (handle 1)
//   RK_TRACK     a = track id                  RK_EVENT     a = event id
//   RK_TAKE      a = event id, c = the take's place in the event's takes (AppState's order)
//   RK_MEDIA     a = media id                  RK_STREAM    a = media id, c = VEGAS's MediaType (2 audio, 3 video)
//   RK_MARKER    a = marker id                 RK_FX        a = effect slot id
//   RK_PLUGIN    c = index in ScriptHost::plugins
//   RK_ENVELOPE  a = owner kind (1 track, 2 event, 3 bus, 4 master, 5 video output), b = owner id, s = Reno's kind,
//                c = target (the bus of a "send")
//   RK_ENVPOINT  a = envelope handle, c = index                RK_GROUP     a = the group number (Event::group)
//   RK_PANCROP   a = event id, c = index                       RK_MOTION    a = track id, c = index
//   RK_BUS       a = bus id, b = 0 a bus, 1 the master, 2 the video output (VEGAS's MasterBus and VideoBus)
#include "scriptsession.h"
#include "timefmt.h"
#include "fxnames.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {

namespace {
enum EnvOwnerKind { OwnTrack = 1, OwnEvent = 2, OwnBus = 3, OwnMaster = 4, OwnVideoOut = 5 };
const double kEps = 1e-9;

// Whether index 'i' (from a script's handle) is not one of the 'n' elements of a list.
inline bool outOfRange(int64_t i, size_t n) { return i < 0 || i >= (int64_t)n; }

// the takes of an event in order with the active one among them (as AppState::takesOf / setActiveTake)
std::vector<Take> allTakes(const Event& e)
{
    const int at = std::max(0, std::min(e.activeTake, (int)e.takes.size()));
    std::vector<Take> all(e.takes.begin(), e.takes.begin() + at);
    Take active;
    active.mediaId = e.mediaId;
    active.offset = e.offset;
    active.name = e.name;
    active.reverse = e.reverse;
    all.push_back(active);
    all.insert(all.end(), e.takes.begin() + at, e.takes.end());
    return all;
}
void setAllTakes(Event& e, std::vector<Take> all, int active)
{
    if (all.empty()) return;
    active = std::max(0, std::min(active, (int)all.size() - 1));
    const Take t = all[(size_t)active];
    e.mediaId = t.mediaId;
    e.offset = t.offset;
    e.name = t.name;
    e.reverse = t.reverse;
    all.erase(all.begin() + active);
    e.takes = all;
    e.activeTake = active;
}

// VEGAS's EnvelopeType <-> Reno's envelope kinds
struct EnvType { int vegas; const char* kind; const char* name; double min, max, neutral; };
const EnvType kEnvTypes[] = {
    {0, "volume", "Volume", 0, 4, 1},       {1, "pan", "Pan", -1, 1, 0},
    {29, "composite", "Composite Level", 0, 1, 1}, {30, "fadecolor", "Fade to Color", -1, 1, 0},
    {31, "mute", "Mute", 0, 1, 0},          {32, "motionblur", "Motion Blur Amount", 0, 1, 0},
    {33, "supersample", "Video Supersampling", 1, 8, 1}, {200, "transition", "Transition Progress", 0, 1, 0},
    {202, "velocity", "Velocity", -1, 10, 1},
};
const EnvType* envTypeOfVegas(int t)
{
    for (const auto& e : kEnvTypes) if (e.vegas == t) return &e;
    return nullptr;
}
const EnvType* envTypeOfKind(const std::string& k)
{
    for (const auto& e : kEnvTypes) if (k == e.kind) return &e;
    return nullptr;
}

// which envelopes each owner can have (Envelope's table in project.h)
bool envAllowed(int owner, bool audio, const std::string& kind)
{
    switch (owner) {
    case OwnTrack: return audio ? (kind == "volume" || kind == "pan" || kind == "mute" || kind == "send")
                                : (kind == "composite" || kind == "fadecolor" || kind == "mute");
    case OwnEvent: return audio ? (kind == "volume" || kind == "pan") : (kind == "composite" || kind == "velocity" || kind == "transition");
    case OwnBus: return kind == "volume" || kind == "pan" || kind == "mute";
    case OwnMaster: return kind == "volume" || kind == "mute";
    case OwnVideoOut: return kind == "fadecolor" || kind == "mute" || kind == "motionblur" || kind == "supersample";
    }
    return false;
}

const char* const kComposite[] = {"",        "custom",   "srcalpha", "3dsrcalpha", "add",    "subtract", "multiply", "cut", "screen",
                                  "overlay", "hardlight", "dodge",   "burn",       "darken", "lighten",  "difference", "diffsquared"};

int64_t b2i(bool b) { return b ? 1 : 0; }
}  // namespace

// ------------------------------------------------------------------------------------------------ the C functions
namespace {
ScriptSession* S(void* ctx) { return static_cast<ScriptSession*>(ctx); }
int32_t copyOut(const std::string& v, char* buf, int32_t cap, int32_t* len)
{
    if (len) *len = (int32_t)v.size();
    if (buf && cap > 0) std::memcpy(buf, v.data(), std::min((size_t)cap, v.size()));
    return RS_OK;
}
template <class F> int32_t guarded(void* c, F f)
{
    try {
        return f();
    } catch (const std::exception& e) {
        return S(c)->failWith(std::string("Reno: ") + e.what());
    } catch (...) {
        return S(c)->failWith("Reno: an unexpected error");
    }
}
int32_t cGetI(void* c, uint64_t h, int32_t p, int64_t* o)
{
    return guarded(c, [&] {
        int64_t v = 0;
        const int r = S(c)->getI(h, p, &v);
        if (o) *o = v;
        return r;
    });
}
int32_t cGetD(void* c, uint64_t h, int32_t p, double* o)
{
    return guarded(c, [&] {
        double v = 0;
        const int r = S(c)->getD(h, p, &v);
        if (o) *o = v;
        return r;
    });
}
int32_t cGetS(void* c, uint64_t h, int32_t p, char* buf, int32_t cap, int32_t* len)
{
    if (len) *len = 0;
    return guarded(c, [&] {
        std::string v;
        const int r = S(c)->getS(h, p, &v);
        if (r != RS_OK) return r;
        return (int)copyOut(v, buf, cap, len);
    });
}
int32_t cSetI(void* c, uint64_t h, int32_t p, int64_t v) { return guarded(c, [&] { return S(c)->setI(h, p, v); }); }
int32_t cSetD(void* c, uint64_t h, int32_t p, double v) { return guarded(c, [&] { return S(c)->setD(h, p, v); }); }
int32_t cSetS(void* c, uint64_t h, int32_t p, const char* v)
{
    return guarded(c, [&] { return S(c)->setS(h, p, v ? std::string(v) : std::string()); });
}
int32_t cList(void* c, uint64_t h, int32_t w, uint64_t* out, int32_t cap, int32_t* count)
{
    if (count) *count = 0;
    return guarded(c, [&] {
        std::vector<uint64_t> v;
        const int r = S(c)->list(h, w, &v);
        if (count) *count = (int32_t)v.size();
        if (r == RS_OK && out)
            for (int32_t k = 0; k < cap && k < (int32_t)v.size(); ++k) out[k] = v[(size_t)k];
        return r;
    });
}
int32_t cCall(void* c, uint64_t h, int32_t op, const int64_t* ia, int32_t ni, const double* da, int32_t nd, const char* s, uint64_t* out,
              double* dout)
{
    if (out) *out = 0;
    if (dout) *dout = 0;
    return guarded(c, [&] {
        std::vector<int64_t> i;
        std::vector<double> d;
        if (ia) i.assign(ia, ia + std::max(0, ni));
        if (da) d.assign(da, da + std::max(0, nd));
        uint64_t o = 0;
        double x = 0;
        const int r = S(c)->call(h, op, i, d, s ? std::string(s) : std::string(), &o, &x);
        if (out) *out = o;
        if (dout) *dout = x;
        return r;
    });
}
int32_t cError(void* c, char* buf, int32_t cap, int32_t* len) { return copyOut(S(c)->lastError(), buf, cap, len); }
int32_t cKind(void* c, uint64_t h) { return S(c)->kindOf(h); }
void cFinish(void* c, int32_t failed, const char* msg)
{
    ScriptSession* s = S(c);
    s->finished = true;
    s->failed = failed != 0;
    s->message = msg ? msg : "";
}
}  // namespace

ScriptSession::ScriptSession(const Project& p, ScriptHost host) : project(p), host_(std::move(host))
{
    api_.version = RENO_SCRIPT_API_VERSION;
    api_.size = (int32_t)sizeof(RenoScriptApi);
    api_.ctx = this;
    api_.getI = cGetI;
    api_.getD = cGetD;
    api_.getS = cGetS;
    api_.setI = cSetI;
    api_.setD = cSetD;
    api_.setS = cSetS;
    api_.list = cList;
    api_.call = cCall;
    api_.error = cError;
    api_.kind = cKind;
    api_.finish = cFinish;
    handle({RK_APP, 0, 0, 0, ""});          // handle 1
}

ScriptSession::~ScriptSession() {}

uint64_t ScriptSession::handle(const Desc& d)
{
    auto it = handles_.find(d);
    if (it != handles_.end()) return it->second;
    descs_.push_back(d);
    const uint64_t h = descs_.size();
    handles_[d] = h;
    return h;
}

const ScriptSession::Desc* ScriptSession::desc(uint64_t h) const
{
    return h >= 1 && h <= descs_.size() ? &descs_[(size_t)(h - 1)] : nullptr;
}

int ScriptSession::kindOf(uint64_t h) const
{
    const Desc* d = desc(h);
    return d ? d->kind : RK_NONE;
}

int ScriptSession::fail(const std::string& why)
{
    error_ = why;
    return RS_FAILED;
}

int ScriptSession::unsupported()
{
    error_ = "not supported by Reno";
    return RS_UNSUPPORTED;
}

int ScriptSession::vegasCurve(int c)
{
    switch (c) {
    case CurveFast: return 2;
    case CurveSlow: return -2;
    case CurveSmooth: return 4;
    case CurveSharp: return -4;
    case CurveHold: return 0;
    default: return 1;
    }
}

int ScriptSession::renoCurve(int v)
{
    switch (v) {
    case 2: return CurveFast;
    case -2: return CurveSlow;
    case 4: return CurveSmooth;
    case -4: return CurveSharp;
    case 0: return CurveHold;
    default: return CurveLinear;
    }
}

int ScriptSession::vegasComposite(const std::string& m)
{
    if (m == "differencesquared") return 16;
    for (int k = 1; k < (int)(sizeof(kComposite) / sizeof(kComposite[0])); ++k)
        if (m == kComposite[k]) return k;
    return 2;
}

std::string ScriptSession::renoComposite(int v)
{
    return v >= 1 && v < (int)(sizeof(kComposite) / sizeof(kComposite[0])) ? kComposite[v] : std::string();
}

// ------------------------------------------------------------------------------------------------ lookups
Track* ScriptSession::track(const Desc& d)
{
    Track* t = project.findTrack(d.a);
    if (!t) fail("the track is no longer in the project");
    return t;
}

Event* ScriptSession::event(const Desc& d, Track** tr)
{
    Track* t = nullptr;
    Event* e = project.findEvent(d.a, &t);
    if (!e) fail("the event is no longer in the project");
    if (tr) *tr = t;
    return e;
}

MediaItem* ScriptSession::media(uint64_t id)
{
    MediaItem* m = project.findMedia(id);
    if (!m) fail("the media is no longer in the project");
    return m;
}

Project::Marker* ScriptSession::marker(const Desc& d)
{
    for (auto& m : project.markers)
        if (m.id == d.a) return &m;
    fail("the marker is no longer in the project");
    return nullptr;
}

Bus* ScriptSession::bus(const Desc& d)
{
    Bus* b = project.findBus(d.a);
    if (!b) fail("the bus is no longer in the project");
    return b;
}

FxSlot* ScriptSession::fx(const Desc& d, FxChain** chainOut, int* index)
{
    auto look = [&](FxChain& c) -> FxSlot* {
        for (size_t k = 0; k < c.size(); ++k)
            if (c[k].id == d.a) {
                if (chainOut) *chainOut = &c;
                if (index) *index = (int)k;
                return &c[k];
            }
        return nullptr;
    };
    if (chainOut) *chainOut = nullptr;
    if (index) *index = 0;
    for (Track& t : project.tracks) {
        if (FxSlot* s = look(t.fx)) return s;
        for (Event& e : t.events) {
            if (FxSlot* s = look(e.fx)) return s;
            if (e.transition.id == d.a && !e.transition.kind.empty()) return &e.transition;
        }
    }
    for (Bus& b : project.buses)
        if (FxSlot* s = look(b.fx)) return s;
    if (FxSlot* s = look(project.videoOut)) return s;
    if (FxSlot* s = look(project.audioOut)) return s;
    for (MediaItem& m : project.media)
        if (m.generated && m.generator.id == d.a) return &m.generator;
    fail("the effect is no longer in the project");
    return nullptr;
}

FxChain* ScriptSession::chainOf(const Desc& o)
{
    switch (o.kind) {
    case RK_TRACK: { Track* t = track(o); return t ? &t->fx : nullptr; }
    case RK_EVENT: { Event* e = event(o); return e ? &e->fx : nullptr; }
    case RK_APP: return &project.videoOut;
    case RK_BUS:
        if (o.b == 1) return &project.audioOut;
        if (o.b == 2) return &project.videoOut;
        { Bus* b = bus(o); return b ? &b->fx : nullptr; }
    }
    return nullptr;
}

bool ScriptSession::envOwnerOf(const Desc& o, int* kind, uint64_t* id)
{
    *id = o.a;
    switch (o.kind) {
    case RK_TRACK: *kind = OwnTrack; return true;
    case RK_EVENT: *kind = OwnEvent; return true;
    case RK_APP: *kind = OwnVideoOut; *id = 0; return true;
    case RK_BUS: *kind = o.b == 1 ? OwnMaster : o.b == 2 ? OwnVideoOut : OwnBus; if (o.b) *id = 0; return true;
    }
    return false;
}

Envelopes* ScriptSession::envelopesOf(int owner, uint64_t id)
{
    switch (owner) {
    case OwnTrack: { Track* t = project.findTrack(id); return t ? &t->envelopes : nullptr; }
    case OwnEvent: { Event* e = project.findEvent(id); return e ? &e->envelopes : nullptr; }
    case OwnBus: { Bus* b = project.findBus(id); return b ? &b->envelopes : nullptr; }
    case OwnMaster: return &project.masterEnvelopes;
    case OwnVideoOut: return &project.videoEnvelopes;
    }
    return nullptr;
}

Envelope* ScriptSession::envelope(const Desc& d)
{
    Envelopes* list = envelopesOf((int)d.a, d.b);
    Envelope* e = list ? findEnvelope(*list, d.s, (uint64_t)d.c) : nullptr;
    if (!e) fail("the envelope is no longer there");
    return e;
}

double ScriptSession::mediaLength(const MediaItem& m) const
{
    if (m.subclipLength > 0) return m.subclipLength;
    if (m.info.isStill || m.generated) return host_.stillLength;
    return m.info.duration;
}

// VEGAS's take offset is from the start of the take's media (a subclip's own start); a reversed subclip runs
// backwards from its end.
double ScriptSession::takeOffset(const Event& e, const MediaItem* m) const
{
    if (!m) return e.offset;
    const double base = m->subclipLength > 0 ? m->subclipStart : 0.0;
    if (e.reverse && reversedMedia_.count(m->id)) return std::max(0.0, base + m->subclipLength - e.offset - e.length * e.rate);
    return e.offset - base;
}

void ScriptSession::setTakeOffset(Event& e, const MediaItem* m, double o)
{
    if (!m) { e.offset = std::max(0.0, o); return; }
    const double base = m->subclipLength > 0 ? m->subclipStart : 0.0;
    if (reversedMedia_.count(m->id)) {
        e.reverse = true;
        e.offset = std::max(base, base + m->subclipLength - o - e.length * e.rate);
    } else {
        e.offset = std::max(0.0, base + o);
    }
}

int ScriptSession::markerKind(const Project::Marker& m) const
{
    if (m.kind == "command") return 2;
    if (m.kind == "cdtrack") return 3;
    if (m.kind == "cdindex") return 4;
    return m.region ? 1 : 0;
}

std::vector<Project::Marker*> ScriptSession::markersOfKind(int kind)
{
    std::vector<Project::Marker*> out;
    for (auto& m : project.markers)
        if (markerKind(m) == kind) out.push_back(&m);
    std::stable_sort(out.begin(), out.end(), [](const Project::Marker* a, const Project::Marker* b) { return a->time < b->time; });
    return out;
}

std::vector<uint64_t> ScriptSession::groupIds() const
{
    std::set<uint64_t> ids(emptyGroups_.begin(), emptyGroups_.end());
    for (const Track& t : project.tracks)
        for (const Event& e : t.events)
            if (e.group) ids.insert(e.group);
    return std::vector<uint64_t>(ids.begin(), ids.end());
}

// ------------------------------------------------------------------------------------------------ properties
int ScriptSession::getI(uint64_t h, int prop, int64_t* out)
{
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if ((d.kind == RK_OFXPARAM || d.kind == RK_OFXKEY) && prop != RP_IS_VALID) return ofxGetI(d, prop, out);
    if (prop == RP_IS_VALID) {
        error_.clear();
        switch (d.kind) {
        case RK_TRACK: *out = b2i(project.findTrack(d.a) != nullptr); break;
        case RK_EVENT: *out = b2i(project.findEvent(d.a) != nullptr); break;
        case RK_MEDIA: case RK_STREAM: *out = b2i(project.findMedia(d.a) != nullptr); break;
        case RK_MARKER: *out = b2i(marker(d) != nullptr); break;
        case RK_FX: *out = b2i(fx(d) != nullptr); break;
        case RK_OFXPARAM: { FxSlot* sl = nullptr; *out = b2i(ofxParam(d, &sl) != nullptr); break; }
        case RK_OFXKEY: {
            FxSlot* sl = nullptr;
            const ScriptOfxParam* p = ofxParam(d, &sl);
            *out = b2i(p && d.b < ofxKeyTimes(ofxRead(*sl, *p), *p).size());
            break;
        }
        case RK_ENVELOPE: *out = b2i(envelope(d) != nullptr); break;
        case RK_TAKE: { const Event* e = project.findEvent(d.a); *out = b2i(e && d.c >= 0 && d.c <= (int64_t)e->takes.size()); break; }
        case RK_ENVPOINT: { const Desc* ed = desc(d.a); Envelope* e = ed ? envelope(*ed) : nullptr; *out = b2i(e && d.c >= 0 && d.c < (int64_t)e->points.size()); break; }
        case RK_GROUP: { const auto g = groupIds(); *out = b2i(std::find(g.begin(), g.end(), d.a) != g.end()); break; }
        case RK_BUS: *out = b2i(d.b != 0 || project.findBus(d.a) != nullptr); break;
        default: *out = 1;
        }
        error_.clear();
        return RS_OK;
    }
    switch (d.kind) {
    case RK_APP: {
        const ProjectSettings& st = project.settings;
        switch (prop) {
        case RP_LOOP_MODE: *out = b2i(loopOn); return RS_OK;
        case RP_IS_MODIFIED: *out = b2i(changed); return RS_OK;
        case RP_WIDTH: *out = st.width; return RS_OK;
        case RP_HEIGHT: *out = st.height; return RS_OK;
        case RP_FIELD_ORDER: *out = st.fieldOrder == "upper" ? 1 : st.fieldOrder == "lower" ? 2 : 0; return RS_OK;
        case RP_PIXEL_FORMAT: *out = st.pixelFormat == "32bit" ? (st.pixelRange == "full" ? 2 : 1) : 0; return RS_OK;
        case RP_RENDER_QUALITY: *out = st.renderQuality == "draft" ? 1 : st.renderQuality == "preview" ? 2 : st.renderQuality == "best" ? 4 : 3; return RS_OK;
        case RP_MOTION_BLUR: {
            static const char* const n[] = {"gaussian", "pyramid", "box", "asymmetricgaussian", "asymmetricpyramid", "asymmetricbox"};
            *out = 0;
            for (int k = 0; k < 6; ++k) if (st.motionBlur == n[k]) *out = k;
            return RS_OK;
        }
        case RP_DEINTERLACE: *out = st.deinterlace == "blend" ? 1 : st.deinterlace == "interpolate" ? 2 : 0; return RS_OK;
        case RP_OUTPUT_ROTATION: *out = st.outputRotation; return RS_OK;
        case RP_OUTPUT_360: *out = st.output360 ? 0 : 1; return RS_OK;
        case RP_SAMPLE_RATE: *out = st.sampleRate; return RS_OK;
        case RP_BIT_DEPTH: *out = st.audioBits; return RS_OK;
        case RP_BUS_MODE: *out = st.surround() ? 2 : 1; return RS_OK;
        case RP_RULER_FORMAT: {
            *out = timeFormatInfo(st.rulerFormat).scriptValue;
            return RS_OK;
        }
        case RP_BEATS_PER_MEASURE: *out = st.beatsPerMeasure; return RS_OK;
        case RP_BYPASS_FX: *out = b2i(bypassFx); return RS_OK;
        case RP_IS_PLAYING: *out = 0; return RS_OK;
        case RP_UNDO_CANCEL: *out = b2i(cancelled); return RS_OK;
        case RP_MAIN_WINDOW: *out = host_.mainWindow; return RS_OK;
        }
        return unsupported();
    }
    case RK_TRACK: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        switch (prop) {
        case RP_INDEX:
            for (size_t k = 0; k < project.tracks.size(); ++k)
                if (project.tracks[k].id == t->id) *out = (int64_t)k;
            return RS_OK;
        case RP_MEDIA_TYPE: *out = t->type == TrackType::Audio ? 2 : 3; return RS_OK;
        case RP_MUTE: *out = b2i(t->mute); return RS_OK;
        case RP_SOLO: *out = b2i(t->solo); return RS_OK;
        case RP_SELECTED: *out = b2i(selectedTracks.count(t->id) != 0); return RS_OK;
        case RP_COMPOSITE_MODE: *out = vegasComposite(t->composite); return RS_OK;
        case RP_PARENT_COMPOSITE_MODE: *out = vegasComposite(t->parentComposite); return RS_OK;
        case RP_NESTING_LEVEL: *out = t->compositeLevel; return RS_OK;
        case RP_TOP_FADE_COLOR: *out = t->fadeTop; return RS_OK;
        case RP_BOTTOM_FADE_COLOR: *out = t->fadeBottom; return RS_OK;
        case RP_TRACK_BUS: *out = t->output ? (int64_t)handle({RK_BUS, t->output, 0, 0, ""}) : 0; return RS_OK;
        }
        return unsupported();
    }
    case RK_EVENT: return eventProp(d, prop, out, nullptr, nullptr, nullptr, nullptr, nullptr);
    case RK_TAKE: {
        Event* e = event(d);
        if (!e) return RS_FAILED;
        const auto all = allTakes(*e);
        if (outOfRange(d.c, all.size())) return fail("the take is no longer there");
        const Take& k = all[(size_t)d.c];
        const MediaItem* m = project.findMedia(k.mediaId);
        switch (prop) {
        case RP_INDEX: *out = d.c; return RS_OK;
        case RP_IS_ACTIVE: *out = b2i(d.c == e->activeTake); return RS_OK;
        case RP_MEDIA: *out = m ? (int64_t)mediaHandle(m->id) : 0; return RS_OK;
        case RP_STREAM: {
            Track* tr = nullptr;
            project.findEvent(e->id, &tr);
            *out = m ? (int64_t)handle({RK_STREAM, m->id, 0, tr && tr->type == TrackType::Audio ? 2 : 3, ""}) : 0;
            return RS_OK;
        }
        }
        return unsupported();
    }
    case RK_MEDIA: case RK_STREAM: {
        MediaItem* m = media(d.a);
        if (!m) return RS_FAILED;
        if (d.kind == RK_STREAM) {
            const bool video = d.c == 3;
            switch (prop) {
            case RP_MEDIA_TYPE: *out = d.c; return RS_OK;
            case RP_INDEX: *out = 0; return RS_OK;
            case RP_MEDIA: *out = (int64_t)mediaHandle(m->id); return RS_OK;
            case RP_STREAM_WIDTH: *out = video ? m->info.width : 0; return RS_OK;
            case RP_STREAM_HEIGHT: *out = video ? m->info.height : 0; return RS_OK;
            case RP_STREAM_CHANNELS: *out = video ? 0 : m->info.channels; return RS_OK;
            case RP_STREAM_SAMPLE_RATE: *out = video ? 0 : m->info.sampleRate; return RS_OK;
            case RP_STREAM_BITS: *out = video ? m->info.bitDepth * 3 : 16; return RS_OK;
            case RP_STREAM_FIELD_ORDER: *out = m->fieldOrder() == "upper" ? 1 : m->fieldOrder() == "lower" ? 2 : 0; return RS_OK;
            case RP_IS_OFFLINE: *out = b2i(m->offline); return RS_OK;
            }
            return unsupported();
        }
        switch (prop) {
        case RP_HAS_VIDEO: *out = b2i(m->info.hasVideo || m->generated); return RS_OK;
        case RP_HAS_AUDIO: *out = b2i(m->info.hasAudio); return RS_OK;
        case RP_IS_GENERATED: *out = b2i(m->generated); return RS_OK;
        case RP_IS_SUBCLIP: *out = b2i(m->subclipLength > 0); return RS_OK;
        case RP_IS_OFFLINE: *out = b2i(m->offline); return RS_OK;
        case RP_IS_REVERSED: *out = b2i(reversedMedia_.count(m->id) != 0); return RS_OK;
        case RP_MEDIA_ID: *out = (int64_t)m->id; return RS_OK;
        case RP_GENERATOR: *out = m->generated ? (int64_t)handle({RK_FX, m->generator.id, 0, 0, ""}) : 0; return RS_OK;
        case RP_USE_COUNT: {
            int64_t n = 0;
            for (const Track& t : project.tracks)
                for (const Event& e : t.events) {
                    if (e.mediaId == m->id) ++n;
                    for (const Take& k : e.takes) if (k.mediaId == m->id) ++n;
                }
            *out = n;
            return RS_OK;
        }
        }
        return unsupported();
    }
    case RK_MARKER: {
        Project::Marker* m = marker(d);
        if (!m) return RS_FAILED;
        switch (prop) {
        case RP_MARKER_KIND: *out = markerKind(*m); return RS_OK;
        case RP_INDEX: {
            const auto l = markersOfKind(markerKind(*m));
            for (size_t k = 0; k < l.size(); ++k) if (l[k] == m) *out = (int64_t)k;
            return RS_OK;
        }
        }
        return unsupported();
    }
    case RK_FX: {
        int index = 0;
        FxChain* chain = nullptr;
        FxSlot* s = fx(d, &chain, &index);
        if (!s) return RS_FAILED;
        switch (prop) {
        case RP_INDEX: *out = index; return RS_OK;
        case RP_BYPASS: *out = b2i(s->bypass); return RS_OK;
        case RP_BEFORE_PANCROP: *out = b2i(s->beforePanCrop); return RS_OK;
        case RP_IS_OFX: *out = b2i(s->kind == "ofx"); return RS_OK;
        case RP_PLUGIN:
            *out = 0;
            for (size_t k = 0; k < host_.plugins.size(); ++k) {
                const ScriptPlugin& p = host_.plugins[k];
                if ((s->kind == "ofx" && p.type != ScriptPlugin::Audio && p.id == s->pluginId) ||
                    (s->kind == "audio" && p.type == ScriptPlugin::Audio && p.audio.key() == s->audio.key())) {
                    *out = (int64_t)handle({RK_PLUGIN, 0, 0, (int64_t)k, ""});
                    break;
                }
            }
            return RS_OK;
        }
        return unsupported();
    }
    case RK_PLUGIN: {
        if (outOfRange(d.c, host_.plugins.size())) return fail("no such plug-in");
        const ScriptPlugin& p = host_.plugins[(size_t)d.c];
        switch (prop) {
        case RP_PLUGIN_TYPE: *out = p.type; return RS_OK;
        case RP_IS_VIDEO: *out = b2i(p.type != ScriptPlugin::Audio); return RS_OK;
        case RP_IS_AUDIO: *out = b2i(p.type == ScriptPlugin::Audio); return RS_OK;
        case RP_IS_OFX: *out = b2i(p.type != ScriptPlugin::Audio); return RS_OK;
        case RP_INDEX: *out = d.c; return RS_OK;
        }
        return unsupported();
    }
    case RK_ENVELOPE: {
        Envelope* e = envelope(d);
        if (!e) return RS_FAILED;
        switch (prop) {
        case RP_ENV_TYPE: {
            if (e->kind == "send") {
                int64_t t = -1;
                for (size_t k = 0; k < project.buses.size(); ++k) if (project.buses[k].id == e->target) t = 3 + (int64_t)k;
                *out = t;
                return RS_OK;
            }
            const EnvType* et = envTypeOfKind(e->kind);
            *out = et ? et->vegas : -1;
            return RS_OK;
        }
        case RP_INDEX: {
            Envelopes* l = envelopesOf((int)d.a, d.b);
            for (size_t k = 0; l && k < l->size(); ++k) if (&(*l)[k] == e) *out = (int64_t)k;
            return RS_OK;
        }
        }
        return unsupported();
    }
    case RK_ENVPOINT: {
        const Desc* ed = desc(d.a);
        Envelope* e = ed ? envelope(*ed) : nullptr;
        if (!e) return RS_FAILED;
        if (outOfRange(d.c, e->points.size())) return fail("the envelope point is no longer there");
        switch (prop) {
        case RP_INDEX: *out = d.c; return RS_OK;
        case RP_CURVE: *out = vegasCurve(e->points[(size_t)d.c].curve); return RS_OK;
        case RP_ENV_TYPE: return getI(d.a, RP_ENV_TYPE, out);
        }
        return unsupported();
    }
    case RK_PANCROP: case RK_MOTION: {
        int curve = 0;
        if (d.kind == RK_PANCROP) {
            Event* e = event(d);
            if (!e) return RS_FAILED;
            if (d.c >= 0 && d.c < (int64_t)e->panCrop.size()) curve = e->panCrop[(size_t)d.c].curve;
            else if (!(d.c == 0 && e->panCrop.empty())) return fail("the pan/crop key is no longer there");
        } else {
            Track* t = track(d);
            if (!t) return RS_FAILED;
            if (outOfRange(d.c, t->motion.size())) return fail("the track motion key is no longer there");
            curve = t->motion[(size_t)d.c].curve;
        }
        switch (prop) {
        case RP_KEY_TYPE: *out = keyCurveToVegas(curve); return RS_OK;
        case RP_INDEX: *out = d.c; return RS_OK;
        }
        return unsupported();
    }
    case RK_BUS: {
        if (d.b == 1) {
            if (prop == RP_MUTE) { *out = b2i(project.masterMute); return RS_OK; }
            if (prop == RP_INDEX) { *out = -1; return RS_OK; }
            if (prop == RP_MEDIA_TYPE) { *out = 2; return RS_OK; }
            return unsupported();
        }
        if (d.b == 2) {
            if (prop == RP_INDEX) { *out = -1; return RS_OK; }
            if (prop == RP_MEDIA_TYPE) { *out = 3; return RS_OK; }
            return unsupported();
        }
        Bus* b = bus(d);
        if (!b) return RS_FAILED;
        switch (prop) {
        case RP_MUTE: *out = b2i(b->mute); return RS_OK;
        case RP_SOLO: *out = b2i(b->solo); return RS_OK;
        case RP_MEDIA_TYPE: *out = 2; return RS_OK;
        case RP_INDEX:
            for (size_t k = 0; k < project.buses.size(); ++k) if (project.buses[k].id == b->id) *out = (int64_t)k;
            return RS_OK;
        }
        return unsupported();
    }
    case RK_GROUP:
        if (prop == RP_INDEX) {
            const auto g = groupIds();
            *out = std::find(g.begin(), g.end(), d.a) - g.begin();
            return RS_OK;
        }
        return unsupported();
    }
    return unsupported();
}

// An event's properties (get and set in one place: si/sx/ss non-null means set).
int ScriptSession::eventProp(const Desc& d, int prop, int64_t* oi, double* ox, std::string* os, const int64_t* si, const double* sx,
                             const std::string* ss)
{
    Track* tr = nullptr;
    Event* e = event(d, &tr);
    if (!e || !tr) return RS_FAILED;
    const bool audio = tr->type == TrackType::Audio;
    const bool set = si || sx || ss;
    const MediaItem* m = project.findMedia(e->mediaId);
    auto setI = [&](auto& field) { if (si) { field = (*si != 0); touch(); } else *oi = b2i(field); return RS_OK; };
    auto numD = [&](double& field) { if (sx) { field = *sx; touch(); } else *ox = field; return RS_OK; };
    if (ss || os) {
        if (prop != RP_NAME) return unsupported();
        if (ss) { e->name = *ss; touch(); } else *os = e->name;
        return RS_OK;
    }
    if (ox || sx) {
        // numbers
        if (sx && !std::isfinite(*sx)) return fail("not a number");
        switch (prop) {
        case RP_START:
            if (!sx) { *ox = e->start; return RS_OK; }
            if (e->locked) return fail("the event is locked");
            e->start = std::max(0.0, *sx);
            tr->sortEvents();
            touch();
            return RS_OK;
        case RP_LENGTH:
            if (!sx) { *ox = e->length; return RS_OK; }
            if (*sx <= 0) return fail("an event's length must be more than 0");
            e->length = *sx;
            touch();
            return RS_OK;
        case RP_END:
            if (!sx) { *ox = e->end(); return RS_OK; }
            if (*sx <= e->start) return fail("an event must end after it starts");
            e->length = *sx - e->start;
            touch();
            return RS_OK;
        case RP_RATE:
            if (!sx) { *ox = e->rate; return RS_OK; }
            if (*sx <= 0) return fail("the playback rate must be more than 0");
            e->rate = *sx;
            touch();
            return RS_OK;
        case RP_FADE_IN:
            if (sx) { e->fadeIn = std::max(0.0, std::min(*sx, e->length)); touch(); } else *ox = e->fadeIn;
            return RS_OK;
        case RP_FADE_OUT:
            if (sx) { e->fadeOut = std::max(0.0, std::min(*sx, e->length)); touch(); } else *ox = e->fadeOut;
            return RS_OK;
        case RP_LEVEL:
            if (sx) { e->gain = std::max(0.0, *sx); touch(); } else *ox = e->gain;
            return RS_OK;
        case RP_UNDERSAMPLE:
            if (audio) return unsupported();
            if (sx) { e->undersample = std::max(0.01, std::min(1.0, *sx)); touch(); } else *ox = e->undersample;
            return RS_OK;
        case RP_NORMALIZE_GAIN: if (!audio) return unsupported(); return numD(e->normalizeGain);
        case RP_PITCH_SEMIS: if (!audio) return unsupported(); return numD(e->pitch);
        case RP_FORMANT_SHIFT: if (!audio) return unsupported(); return numD(e->formant);
        case RP_OFFSET:
            if (!sx) { *ox = takeOffset(*e, m); return RS_OK; }
            setTakeOffset(*e, m, *sx);
            touch();
            return RS_OK;
        }
        return unsupported();
    }
    // integers and handles
    switch (prop) {
    case RP_INDEX:
        if (set) return unsupported();
        for (size_t k = 0; k < tr->events.size(); ++k) if (tr->events[k].id == e->id) *oi = (int64_t)k;
        return RS_OK;
    case RP_MEDIA_TYPE: if (set) return unsupported(); *oi = audio ? 2 : 3; return RS_OK;
    case RP_MUTE: return setI(e->mute);
    case RP_LOCKED: return setI(e->locked);
    case RP_LOOP: return setI(e->loop);
    case RP_REVERSE: return setI(e->reverse);
    case RP_SELECTED:
        if (!si) { *oi = b2i(selection.count(e->id) != 0); return RS_OK; }
        if (*si) selection.insert(e->id); else selection.erase(e->id);
        viewChanged = true;
        return RS_OK;
    case RP_MAINTAIN_ASPECT: if (audio) return unsupported(); return setI(e->keepAspect);
    case RP_REDUCE_INTERLACE: if (audio) return unsupported(); return setI(e->reduceFlicker);
    case RP_INVERT_PHASE: if (!audio) return unsupported(); return setI(e->invertPhase);
    case RP_FORMANT_LOCK: if (!audio) return unsupported(); return setI(e->preserveFormants);
    case RP_PITCH_LOCK:
        // VEGAS's PitchLock ("Lock to stretch"): the pitch changes with the rate - Reno's keepPitch is the opposite
        if (!audio) return unsupported();
        if (!si) { *oi = b2i(!e->keepPitch); return RS_OK; }
        e->keepPitch = *si == 0;
        touch();
        return RS_OK;
    case RP_NORMALIZE:
        if (!audio) return unsupported();
        if (!si) { *oi = b2i(e->normalize); return RS_OK; }
        e->normalize = *si != 0;
        if (e->normalize && host_.eventPeak) {
            const double peak = host_.eventPeak(project, *e);
            e->normalizeGain = peak > 1e-9 ? std::pow(10.0, -0.1 / 20.0) / peak : 1.0;
        }
        touch();
        return RS_OK;
    case RP_RESAMPLE_MODE:
        if (audio) return unsupported();
        if (!si) { *oi = e->resample == "force" ? 1 : e->resample == "off" ? 2 : 0; return RS_OK; }
        e->resample = *si == 1 ? "force" : *si == 2 ? "off" : "project";
        touch();
        return RS_OK;
    case RP_CHANNELS: {
        if (!audio) return unsupported();
        // VEGAS's ChannelRemapping: 1000 none, 1001 disable left (= right only), 1002 disable right, 1003 mute left,
        // 1004 mute right, 1005 mono, 1006 swap.
        if (!si) {
            const std::string& c = e->channels;
            *oi = c == "left" ? 1002 : c == "right" ? 1001 : c == "swap" ? 1006 : c == "combine" ? 1005 : 1000;
            return RS_OK;
        }
        switch (*si) {
        case 1000: e->channels = "both"; break;
        case 1001: case 1003: e->channels = "right"; break;
        case 1002: case 1004: e->channels = "left"; break;
        case 1005: e->channels = "combine"; break;
        case 1006: e->channels = "swap"; break;
        default: return fail("not one of VEGAS's channel remappings");
        }
        touch();
        return RS_OK;
    }
    case RP_STRETCH_METHOD:
        if (!audio) return unsupported();
        // VEGAS's TimeStretchPitchShift: 0 Classic, 1 ACID-style, 2 élastique, 3 None
        if (!si) { *oi = e->stretchMethod == "classic" ? 0 : e->stretchMethod == "none" ? 3 : 2; return RS_OK; }
        if (*si == 0 || *si == 1) {
            if (e->stretchMethod != "classic") e->stretchMode = "A01";
            e->stretchMethod = "classic";
        } else if (*si == 2) {
            if (e->stretchMethod != "elastique") e->stretchMode = "pro";
            e->stretchMethod = "elastique";
        } else if (*si == 3) {
            e->stretchMethod = "none";
        } else {
            return fail("not one of VEGAS's stretch methods");
        }
        touch();
        return RS_OK;
    case RP_CLASSIC_ATTR:
        if (!audio) return unsupported();
        if (!si) {
            int n = 0;
            if (e->stretchMethod == "classic" && e->stretchMode.size() == 3) n = std::atoi(e->stretchMode.c_str() + 1) - 1;
            *oi = std::max(0, std::min(18, n));
            return RS_OK;
        }
        if (*si < 0 || *si > 18) return fail("not one of VEGAS's classic stretch presets");
        e->stretchMethod = "classic";
        e->stretchMode = strfmt("A%02d", (int)*si + 1);
        touch();
        return RS_OK;
    case RP_ELASTIQUE_ATTR: {
        if (!audio) return unsupported();
        static const char* const modes[] = {"pro", "efficient", "soloist-mono", "soloist-speech"};
        if (!si) {
            *oi = 0;
            for (int k = 0; k < 4; ++k) if (e->stretchMode == modes[k]) *oi = k;
            return RS_OK;
        }
        if (*si < 0 || *si > 3) return fail("not one of VEGAS's élastique modes");
        e->stretchMethod = "elastique";
        e->stretchMode = modes[*si];
        touch();
        return RS_OK;
    }
    case RP_FADE_IN_CURVE:
        if (si) { e->fadeInCurve = renoCurve((int)*si); touch(); } else *oi = vegasCurve(e->fadeInCurve);
        return RS_OK;
    case RP_FADE_OUT_CURVE:
        if (si) { e->fadeOutCurve = renoCurve((int)*si); touch(); } else *oi = vegasCurve(e->fadeOutCurve);
        return RS_OK;
    case RP_FADE_IN_RECIP_CURVE: {
        auto mirror = [](int c) { return c == CurveFast ? (int)CurveSlow : c == CurveSlow ? (int)CurveFast : c; };
        if (si) { e->fadeInRecipCurve = renoCurve((int)*si); touch(); }
        else *oi = vegasCurve(e->fadeInRecipCurve >= 0 ? e->fadeInRecipCurve : mirror(e->fadeInCurve));
        return RS_OK;
    }
    case RP_TRACK: {
        if (!si) { *oi = (int64_t)trackHandle(tr->id); return RS_OK; }
        const Desc* td = desc((uint64_t)*si);
        if (!td || td->kind != RK_TRACK) return fail("not a track");
        Track* to = track(*td);
        if (!to) return RS_FAILED;
        if (to->type != tr->type) return fail("an event can only be moved to a track of the same type");
        if (to->id == tr->id) return RS_OK;
        if (e->locked) return fail("the event is locked");
        Event copy = *e;
        tr->events.erase(std::remove_if(tr->events.begin(), tr->events.end(), [&](const Event& x) { return x.id == copy.id; }), tr->events.end());
        to->events.push_back(copy);
        to->sortEvents();
        touch();
        return RS_OK;
    }
    case RP_GROUP:
        if (set) return unsupported();
        *oi = e->group ? (int64_t)handle({RK_GROUP, e->group, 0, 0, ""}) : 0;
        return RS_OK;
    case RP_ACTIVE_TAKE: {
        if (!si) {
            if (!e->mediaId && e->takes.empty()) { *oi = 0; return RS_OK; }
            *oi = (int64_t)handle({RK_TAKE, e->id, 0, e->activeTake, ""});
            return RS_OK;
        }
        const Desc* kd = desc((uint64_t)*si);
        if (!kd || kd->kind != RK_TAKE || kd->a != e->id) return fail("the take is not one of this event's takes");
        const auto all = allTakes(*e);
        if (outOfRange(kd->c, all.size())) return fail("the take is no longer there");
        setAllTakes(*e, all, (int)kd->c);
        touch();
        return RS_OK;
    }
    case RP_TRANSITION:
        if (set) return unsupported();
        *oi = (!audio && !e->transition.kind.empty()) ? (int64_t)handle({RK_FX, e->transition.id, 0, 0, ""}) : 0;
        return RS_OK;
    case RP_MEDIA:
        if (set) return unsupported();
        *oi = m ? (int64_t)mediaHandle(m->id) : 0;
        return RS_OK;
    case RP_MEDIA_WIDTH: case RP_MEDIA_HEIGHT: {
        if (set) return unsupported();
        int w = m ? m->info.width : 0, h = m ? m->info.height : 0;
        if (w <= 0 || h <= 0) { w = project.settings.width; h = project.settings.height; }
        *oi = prop == RP_MEDIA_WIDTH ? w : h;
        return RS_OK;
    }
    }
    return unsupported();
}

int ScriptSession::getD(uint64_t h, int prop, double* out)
{
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if (d.kind == RK_OFXPARAM || d.kind == RK_OFXKEY) return ofxGetD(d, prop, out);
    if (d.kind == RK_FX && prop == RP_OFX_CURRENT_TIME) {
        if (!fx(d)) return RS_FAILED;
        *out = ofxTime_[d.a];
        return RS_OK;
    }
    switch (d.kind) {
    case RK_APP: {
        const ProjectSettings& st = project.settings;
        switch (prop) {
        case RP_CURSOR: *out = cursor; return RS_OK;
        case RP_SEL_START: *out = selStart; return RS_OK;
        case RP_SEL_LENGTH: *out = selLength; return RS_OK;
        case RP_PROJECT_LENGTH: *out = project.length(); return RS_OK;
        case RP_FRAME_RATE: *out = st.fps(); return RS_OK;
        case RP_PIXEL_ASPECT: *out = st.pixelAspect; return RS_OK;
        case RP_RULER_START: *out = st.rulerStart; return RS_OK;
        case RP_TEMPO: *out = st.tempo; return RS_OK;
        }
        return unsupported();
    }
    case RK_TRACK: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        switch (prop) {
        case RP_LEVEL: *out = t->level; return RS_OK;
        case RP_PAN: if (t->type != TrackType::Audio) return unsupported(); *out = t->pan; return RS_OK;
        case RP_LENGTH: {
            double end = 0;
            for (const Event& e : t->events) end = std::max(end, e.end());
            *out = end;
            return RS_OK;
        }
        }
        return unsupported();
    }
    case RK_EVENT: return eventProp(d, prop, nullptr, out, nullptr, nullptr, nullptr, nullptr);
    case RK_TAKE: {
        Event* e = event(d);
        if (!e) return RS_FAILED;
        const auto all = allTakes(*e);
        if (outOfRange(d.c, all.size())) return fail("the take is no longer there");
        const Take& k = all[(size_t)d.c];
        const MediaItem* m = project.findMedia(k.mediaId);
        Event probe = *e;
        probe.offset = k.offset;
        probe.reverse = k.reverse;
        switch (prop) {
        case RP_OFFSET: *out = takeOffset(probe, m); return RS_OK;
        case RP_LENGTH: *out = e->length; return RS_OK;
        case RP_AVAILABLE_LENGTH: *out = m ? std::max(0.0, mediaLength(*m) - takeOffset(probe, m)) : 0.0; return RS_OK;
        }
        return unsupported();
    }
    case RK_MEDIA: case RK_STREAM: {
        MediaItem* m = media(d.a);
        if (!m) return RS_FAILED;
        switch (prop) {
        case RP_LENGTH: *out = mediaLength(*m); return RS_OK;
        case RP_SUBCLIP_START: *out = m->subclipStart; return RS_OK;
        case RP_STREAM_FPS: *out = d.kind == RK_STREAM && d.c == 3 ? (m->info.fps > 0 ? m->info.fps : project.settings.fps()) : 0.0; return RS_OK;
        case RP_STREAM_PIXEL_ASPECT: *out = 1.0; return RS_OK;
        }
        return unsupported();
    }
    case RK_MARKER: {
        Project::Marker* m = marker(d);
        if (!m) return RS_FAILED;
        switch (prop) {
        case RP_START: *out = m->time; return RS_OK;
        case RP_LENGTH: *out = m->region ? m->length : 0.0; return RS_OK;
        case RP_END: *out = m->time + (m->region ? m->length : 0.0); return RS_OK;
        }
        return unsupported();
    }
    case RK_ENVELOPE: {
        Envelope* e = envelope(d);
        if (!e) return RS_FAILED;
        const EnvType* et = envTypeOfKind(e->kind);
        const double mn = et ? et->min : 0, mx = et ? et->max : 4, nt = et ? et->neutral : 1;
        switch (prop) {
        case RP_ENV_MIN: *out = mn; return RS_OK;
        case RP_ENV_MAX: *out = mx; return RS_OK;
        case RP_ENV_NEUTRAL: *out = nt; return RS_OK;
        }
        return unsupported();
    }
    case RK_ENVPOINT: {
        const Desc* ed = desc(d.a);
        Envelope* e = ed ? envelope(*ed) : nullptr;
        if (!e) return RS_FAILED;
        if (outOfRange(d.c, e->points.size())) return fail("the envelope point is no longer there");
        switch (prop) {
        case RP_X: *out = e->points[(size_t)d.c].time; return RS_OK;
        case RP_Y: *out = e->points[(size_t)d.c].value; return RS_OK;
        }
        return unsupported();
    }
    case RK_PANCROP: {
        Event* e = event(d);
        if (!e) return RS_FAILED;
        PanCropKey k;
        if (d.c >= 0 && d.c < (int64_t)e->panCrop.size()) k = e->panCrop[(size_t)d.c];
        else if (!(d.c == 0 && e->panCrop.empty())) return fail("the pan/crop key is no longer there");
        switch (prop) {
        case RP_KEY_TIME: *out = k.time; return RS_OK;
        case RP_KEY_X: *out = k.cx; return RS_OK;
        case RP_KEY_Y: *out = k.cy; return RS_OK;
        case RP_KEY_W: *out = k.w; return RS_OK;
        case RP_KEY_H: *out = k.h; return RS_OK;
        case RP_KEY_ROTATION: *out = k.rotation; return RS_OK;
        case RP_KEY_SMOOTHNESS: *out = 0; return RS_OK;
        }
        return unsupported();
    }
    case RK_MOTION: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        if (outOfRange(d.c, t->motion.size())) return fail("the track motion key is no longer there");
        const MotionKey& k = t->motion[(size_t)d.c];
        switch (prop) {
        case RP_KEY_TIME: *out = k.time; return RS_OK;
        case RP_KEY_X: *out = k.x; return RS_OK;
        case RP_KEY_Y: *out = k.y; return RS_OK;
        case RP_KEY_W: *out = k.w; return RS_OK;
        case RP_KEY_H: *out = k.h; return RS_OK;
        case RP_KEY_ROTATION: *out = k.rotation; return RS_OK;
        case RP_KEY_SMOOTHNESS: *out = 0; return RS_OK;
        }
        return unsupported();
    }
    case RK_BUS: {
        if (d.b == 1) {
            if (prop == RP_LEVEL) { *out = project.masterVolume; return RS_OK; }
            if (prop == RP_PAN) { *out = project.masterPan; return RS_OK; }
            return unsupported();
        }
        if (d.b == 2) return unsupported();
        Bus* b = bus(d);
        if (!b) return RS_FAILED;
        if (prop == RP_LEVEL) { *out = b->level; return RS_OK; }
        if (prop == RP_PAN) { *out = b->pan; return RS_OK; }
        return unsupported();
    }
    }
    return unsupported();
}

int ScriptSession::getS(uint64_t h, int prop, std::string* out)
{
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if (d.kind == RK_OFXPARAM || d.kind == RK_OFXKEY) return ofxGetS(d, prop, out);
    if (d.kind == RK_FX && (prop == RP_OFX_PLUGIN_LABEL || prop == RP_OFX_PLUGIN_GROUP || prop == RP_OFX_PLUGIN_PATH)) {
        FxSlot* s = fx(d);
        if (!s) return RS_FAILED;
        std::string err;
        const ScriptOfxDesc* od = ofxDesc(*s, &err);
        if (!od) *out = prop == RP_OFX_PLUGIN_LABEL ? fxSlotLabel(*s) : std::string();
        else *out = prop == RP_OFX_PLUGIN_LABEL ? od->label : prop == RP_OFX_PLUGIN_GROUP ? od->group : od->path;
        return RS_OK;
    }
    switch (d.kind) {
    case RK_APP: {
        const ProjectSettings& st = project.settings;
        switch (prop) {
        case RP_VERSION: *out = host_.version; return RS_OK;
        case RP_APP_NAME: *out = host_.appName; return RS_OK;
        case RP_INSTALL_DIR: *out = host_.installDir; return RS_OK;
        case RP_APPDATA_DIR: *out = host_.appDataDir; return RS_OK;
        case RP_TEMP_DIR: *out = host_.tempDir; return RS_OK;
        case RP_SCRIPT_FILE: *out = host_.scriptFile; return RS_OK;
        case RP_SCRIPT_ARGS: *out = host_.scriptArgs; return RS_OK;
        case RP_PROJECT_PATH: *out = host_.projectPath; return RS_OK;
        case RP_TITLE: *out = st.title; return RS_OK;
        case RP_ARTIST: *out = st.artist; return RS_OK;
        case RP_ENGINEER: *out = st.engineer; return RS_OK;
        case RP_COPYRIGHT: *out = st.copyright; return RS_OK;
        case RP_COMMENTS: *out = st.comments; return RS_OK;
        case RP_UNDO_LABEL: *out = undoLabel; return RS_OK;
        }
        return unsupported();
    }
    case RK_TRACK: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        if (prop == RP_NAME) { *out = t->name; return RS_OK; }
        return unsupported();
    }
    case RK_EVENT: return eventProp(d, prop, nullptr, nullptr, out, nullptr, nullptr, nullptr);
    case RK_TAKE: {
        Event* e = event(d);
        if (!e) return RS_FAILED;
        const auto all = allTakes(*e);
        if (outOfRange(d.c, all.size())) return fail("the take is no longer there");
        const MediaItem* m = project.findMedia(all[(size_t)d.c].mediaId);
        if (prop == RP_NAME) { *out = all[(size_t)d.c].name; return RS_OK; }
        if (prop == RP_PATH) { *out = m ? m->path : std::string(); return RS_OK; }
        return unsupported();
    }
    case RK_MEDIA: case RK_STREAM: {
        MediaItem* m = media(d.a);
        if (!m) return RS_FAILED;
        switch (prop) {
        case RP_PATH: *out = m->path; return RS_OK;
        case RP_NAME: *out = m->name; return RS_OK;
        case RP_COMMENT: *out = m->comment; return RS_OK;
        case RP_STREAM_FORMAT: *out = d.c == 3 ? m->info.videoCodec : m->info.audioCodec; return RS_OK;
        }
        return unsupported();
    }
    case RK_MARKER: {
        Project::Marker* m = marker(d);
        if (!m) return RS_FAILED;
        if (prop == RP_NAME) { *out = m->name; return RS_OK; }
        if (prop == RP_COMMAND_TYPE) { *out = m->command; return RS_OK; }
        return unsupported();
    }
    case RK_FX: {
        FxSlot* s = fx(d);
        if (!s) return RS_FAILED;
        if (prop == RP_NAME) { *out = s->name; return RS_OK; }
        if (prop == RP_UNIQUE_ID) { *out = s->kind == "audio" ? s->audio.key() : "{Svfx:" + s->pluginId + "}"; return RS_OK; }
        return unsupported();
    }
    case RK_PLUGIN: {
        if (outOfRange(d.c, host_.plugins.size())) return fail("no such plug-in");
        const ScriptPlugin& p = host_.plugins[(size_t)d.c];
        switch (prop) {
        case RP_NAME: *out = p.name; return RS_OK;
        case RP_GROUP_NAME: *out = p.group; return RS_OK;
        case RP_UNIQUE_ID: *out = p.type == ScriptPlugin::Audio ? p.id : "{Svfx:" + p.id + "}"; return RS_OK;
        case RP_CLASS_ID: {
            // the DirectX CLSID or VST3 class id if it is one, else a GUID made from the id (stable between runs)
            std::string u = p.type == ScriptPlugin::Audio ? p.audio.uid : std::string();
            std::string hex;
            for (char c : u) if (std::isxdigit((unsigned char)c)) hex += (char)std::tolower((unsigned char)c);
            if (hex.size() != 32) {
                uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull;
                for (char c : p.id) { h1 = (h1 ^ (unsigned char)c) * 1099511628211ull; h2 = (h2 ^ (unsigned char)c) * 6364136223846793005ull + 1; }
                hex = strfmt("%016llx%016llx", (unsigned long long)h1, (unsigned long long)h2);
            }
            *out = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
            return RS_OK;
        }
        }
        return unsupported();
    }
    case RK_ENVELOPE: {
        Envelope* e = envelope(d);
        if (!e) return RS_FAILED;
        if (prop != RP_NAME) return unsupported();
        if (e->kind == "send") {
            const Bus* b = project.findBus(e->target);
            *out = b ? b->name : "Bus";
        } else {
            const EnvType* et = envTypeOfKind(e->kind);
            *out = et ? et->name : e->kind;
        }
        return RS_OK;
    }
    case RK_BUS: {
        if (prop != RP_NAME) return unsupported();
        if (d.b == 1) { *out = "Master"; return RS_OK; }
        if (d.b == 2) { *out = "Video Output"; return RS_OK; }
        Bus* b = bus(d);
        if (!b) return RS_FAILED;
        *out = b->name;
        return RS_OK;
    }
    }
    return unsupported();
}

int ScriptSession::setI(uint64_t h, int prop, int64_t v)
{
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if (d.kind == RK_OFXPARAM || d.kind == RK_OFXKEY) return ofxSetI(d, prop, v);
    switch (d.kind) {
    case RK_APP: {
        ProjectSettings& st = project.settings;
        switch (prop) {
        case RP_LOOP_MODE: loopOn = v != 0; viewChanged = true; return RS_OK;
        case RP_BYPASS_FX: bypassFx = v != 0; viewChanged = true; return RS_OK;
        case RP_UNDO_CANCEL: cancelled = v != 0; return RS_OK;
        case RP_WIDTH: if (v < 16 || v > 16384) return fail("the frame width must be 16 to 16384"); st.width = (int)v; touch(); return RS_OK;
        case RP_HEIGHT: if (v < 16 || v > 16384) return fail("the frame height must be 16 to 16384"); st.height = (int)v; touch(); return RS_OK;
        case RP_FIELD_ORDER: st.fieldOrder = v == 1 ? "upper" : v == 2 ? "lower" : "none"; touch(); return RS_OK;
        case RP_PIXEL_FORMAT: st.pixelFormat = v == 0 ? "8bit" : "32bit"; if (v == 2) st.pixelRange = "full"; else if (v == 1) st.pixelRange = "video"; touch(); return RS_OK;
        case RP_RENDER_QUALITY: st.renderQuality = v == 1 ? "draft" : v == 2 ? "preview" : v == 4 ? "best" : "good"; touch(); return RS_OK;
        case RP_MOTION_BLUR: {
            static const char* const n[] = {"gaussian", "pyramid", "box", "asymmetricgaussian", "asymmetricpyramid", "asymmetricbox"};
            if (v < 0 || v > 5) return fail("not one of VEGAS's motion blur types");
            st.motionBlur = n[v];
            touch();
            return RS_OK;
        }
        case RP_DEINTERLACE: st.deinterlace = v == 1 ? "blend" : v == 2 ? "interpolate" : "none"; touch(); return RS_OK;
        case RP_OUTPUT_360: st.output360 = v == 0; touch(); return RS_OK;
        case RP_OUTPUT_ROTATION:
            if (v != 0 && v != 90 && v != 180 && v != 270) return fail("the output rotation must be 0, 90, 180 or 270");
            st.outputRotation = (int)v;
            touch();
            return RS_OK;
        case RP_SAMPLE_RATE: if (v < 8000 || v > 384000) return fail("the sample rate must be 8000 to 384000"); st.sampleRate = (int)v; touch(); return RS_OK;
        case RP_BIT_DEPTH: if (v != 8 && v != 16 && v != 24 && v != 32) return fail("the bit depth must be 8, 16, 24 or 32"); st.audioBits = (int)v; touch(); return RS_OK;
        case RP_BUS_MODE: st.audioChannels = v == 2 ? 6 : 2; touch(); return RS_OK;
        case RP_RULER_FORMAT:
            st.rulerFormat = timeFormatFromScript((int)v);
            touch();
            return RS_OK;
        case RP_BEATS_PER_MEASURE: if (v < 1 || v > 64) return fail("beats per measure must be 1 to 64"); st.beatsPerMeasure = (int)v; touch(); return RS_OK;
        }
        return unsupported();
    }
    case RK_TRACK: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        switch (prop) {
        case RP_MUTE: t->mute = v != 0; touch(); return RS_OK;
        case RP_SOLO: t->solo = v != 0; touch(); return RS_OK;
        case RP_SELECTED:
            if (v) selectedTracks.insert(t->id); else selectedTracks.erase(t->id);
            viewChanged = true;
            return RS_OK;
        case RP_COMPOSITE_MODE: case RP_PARENT_COMPOSITE_MODE: {
            if (t->type != TrackType::Video) return unsupported();
            const std::string m = renoComposite((int)v);
            if (m.empty()) return fail("not one of VEGAS's compositing modes");
            if (m == "custom") return fail("the Custom compositing mode is not available to scripts");
            (prop == RP_COMPOSITE_MODE ? t->composite : t->parentComposite) = m;
            touch();
            return RS_OK;
        }
        case RP_NESTING_LEVEL:
            if (t->type != TrackType::Video) return unsupported();
            t->compositeLevel = (int)std::max<int64_t>(0, std::min<int64_t>(v, 8));
            touch();
            return RS_OK;
        case RP_TOP_FADE_COLOR: if (t->type != TrackType::Video) return unsupported(); t->fadeTop = (uint32_t)v; touch(); return RS_OK;
        case RP_BOTTOM_FADE_COLOR: if (t->type != TrackType::Video) return unsupported(); t->fadeBottom = (uint32_t)v; touch(); return RS_OK;
        case RP_TRACK_BUS: {
            if (t->type != TrackType::Audio) return unsupported();
            if (v == 0) { t->output = 0; touch(); return RS_OK; }
            const Desc* bd = desc((uint64_t)v);
            if (!bd || bd->kind != RK_BUS) return fail("not a bus");
            if (bd->b == 1) { t->output = 0; touch(); return RS_OK; }
            Bus* b = bus(*bd);
            if (!b) return RS_FAILED;
            t->output = b->id;
            touch();
            return RS_OK;
        }
        }
        return unsupported();
    }
    case RK_EVENT: return eventProp(d, prop, nullptr, nullptr, nullptr, &v, nullptr, nullptr);
    case RK_FX: {
        FxSlot* s = fx(d);
        if (!s) return RS_FAILED;
        if (prop == RP_BYPASS) { s->bypass = v != 0; touch(); return RS_OK; }
        if (prop == RP_BEFORE_PANCROP) { s->beforePanCrop = v != 0; touch(); return RS_OK; }
        return unsupported();
    }
    case RK_ENVPOINT: {
        const Desc* ed = desc(d.a);
        Envelope* e = ed ? envelope(*ed) : nullptr;
        if (!e) return RS_FAILED;
        if (outOfRange(d.c, e->points.size())) return fail("the envelope point is no longer there");
        if (prop != RP_CURVE) return unsupported();
        e->points[(size_t)d.c].curve = renoCurve((int)v);
        touch();
        return RS_OK;
    }
    case RK_PANCROP: case RK_MOTION: {
        if (prop != RP_KEY_TYPE) return unsupported();
        const int curve = keyCurveFromVegas((int)v);       // VEGAS's VideoKeyframeType
        if (d.kind == RK_PANCROP) {
            Event* e = event(d);
            if (!e) return RS_FAILED;
            if (e->panCrop.empty() && d.c == 0) e->panCrop.push_back(PanCropKey());
            if (outOfRange(d.c, e->panCrop.size())) return fail("the pan/crop key is no longer there");
            e->panCrop[(size_t)d.c].curve = curve;
        } else {
            Track* t = track(d);
            if (!t) return RS_FAILED;
            if (outOfRange(d.c, t->motion.size())) return fail("the track motion key is no longer there");
            t->motion[(size_t)d.c].curve = curve;
        }
        touch();
        return RS_OK;
    }
    case RK_BUS: {
        if (d.b == 1) {
            if (prop == RP_MUTE) { project.masterMute = v != 0; touch(); return RS_OK; }
            return unsupported();
        }
        if (d.b == 2) return unsupported();
        Bus* b = bus(d);
        if (!b) return RS_FAILED;
        if (prop == RP_MUTE) { b->mute = v != 0; touch(); return RS_OK; }
        if (prop == RP_SOLO) { b->solo = v != 0; touch(); return RS_OK; }
        return unsupported();
    }
    }
    return unsupported();
}

int ScriptSession::setD(uint64_t h, int prop, double v)
{
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if (!std::isfinite(v)) return fail("not a number");
    if (d.kind == RK_OFXPARAM || d.kind == RK_OFXKEY) return ofxSetD(d, prop, v);
    if (d.kind == RK_FX && prop == RP_OFX_CURRENT_TIME) {
        if (!fx(d)) return RS_FAILED;
        ofxTime_[d.a] = std::max(0.0, v);
        return RS_OK;
    }
    switch (d.kind) {
    case RK_APP: {
        ProjectSettings& st = project.settings;
        switch (prop) {
        case RP_CURSOR: cursor = std::max(0.0, v); viewChanged = true; return RS_OK;
        case RP_SEL_START: selStart = std::max(0.0, v); viewChanged = true; return RS_OK;
        case RP_SEL_LENGTH: selLength = v; viewChanged = true; return RS_OK;
        case RP_FRAME_RATE: {
            if (v < 0.5 || v > 1000) return fail("the frame rate must be 0.5 to 1000");
            // VEGAS's NTSC rates are 30000/1001 etc.; others are kept to a thousandth
            int num = (int)std::lround(v * 1000), den = 1000;
            for (int base : {24, 30, 48, 60, 120, 240}) {
                if (std::fabs(v - base * 1000.0 / 1001.0) < 0.001) { num = base * 1000; den = 1001; }
            }
            if (den == 1000 && std::fabs(v - std::round(v)) < 1e-6) { num = (int)std::lround(v); den = 1; }
            st.fpsNum = num;
            st.fpsDen = den;
            touch();
            return RS_OK;
        }
        case RP_PIXEL_ASPECT: if (v <= 0.1 || v > 10) return fail("the pixel aspect ratio must be 0.1 to 10"); st.pixelAspect = v; touch(); return RS_OK;
        case RP_RULER_START: st.rulerStart = v; touch(); return RS_OK;
        case RP_TEMPO: if (v <= 0 || v > 1000) return fail("the tempo must be more than 0"); st.tempo = v; touch(); return RS_OK;
        }
        return unsupported();
    }
    case RK_TRACK: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        if (prop == RP_LEVEL) { t->level = std::max(0.0, t->type == TrackType::Video ? std::min(1.0, v) : v); touch(); return RS_OK; }
        if (prop == RP_PAN && t->type == TrackType::Audio) { t->pan = std::max(-1.0, std::min(1.0, v)); touch(); return RS_OK; }
        return unsupported();
    }
    case RK_EVENT: return eventProp(d, prop, nullptr, nullptr, nullptr, nullptr, &v, nullptr);
    case RK_TAKE: {
        if (prop != RP_OFFSET) return unsupported();
        Event* e = event(d);
        if (!e) return RS_FAILED;
        auto all = allTakes(*e);
        if (outOfRange(d.c, all.size())) return fail("the take is no longer there");
        Event probe = *e;
        const MediaItem* m = project.findMedia(all[(size_t)d.c].mediaId);
        setTakeOffset(probe, m, v);
        all[(size_t)d.c].offset = probe.offset;
        all[(size_t)d.c].reverse = probe.reverse;
        setAllTakes(*e, all, e->activeTake);
        touch();
        return RS_OK;
    }
    case RK_MARKER: {
        Project::Marker* m = marker(d);
        if (!m) return RS_FAILED;
        switch (prop) {
        case RP_START: m->time = std::max(0.0, v); touch(); return RS_OK;
        case RP_LENGTH: if (!m->region) return unsupported(); m->length = std::max(0.0, v); touch(); return RS_OK;
        case RP_END: if (!m->region) return unsupported(); m->length = std::max(0.0, v - m->time); touch(); return RS_OK;
        }
        return unsupported();
    }
    case RK_ENVPOINT: {
        const Desc* ed = desc(d.a);
        Envelope* e = ed ? envelope(*ed) : nullptr;
        if (!e) return RS_FAILED;
        if (outOfRange(d.c, e->points.size())) return fail("the envelope point is no longer there");
        EnvPoint& p = e->points[(size_t)d.c];
        if (prop == RP_Y) {
            const EnvType* et = envTypeOfKind(e->kind);
            p.value = et ? std::max(et->min, std::min(et->max, v)) : std::max(0.0, std::min(4.0, v));
            touch();
            return RS_OK;
        }
        if (prop == RP_X) {
            // the points stay in time order; a point is not moved past its neighbours, as in VEGAS's envelope editing.
            const double lo = d.c > 0 ? e->points[(size_t)d.c - 1].time : -1e300;
            const double hi = d.c + 1 < (int64_t)e->points.size() ? e->points[(size_t)d.c + 1].time : 1e300;
            p.time = std::max(lo, std::min(hi, v));
            touch();
            return RS_OK;
        }
        return unsupported();
    }
    case RK_PANCROP: {
        Event* e = event(d);
        if (!e) return RS_FAILED;
        if (e->panCrop.empty() && d.c == 0) e->panCrop.push_back(PanCropKey());
        if (outOfRange(d.c, e->panCrop.size())) return fail("the pan/crop key is no longer there");
        PanCropKey& k = e->panCrop[(size_t)d.c];
        switch (prop) {
        case RP_KEY_TIME: k.time = std::max(0.0, v); break;
        case RP_KEY_X: k.cx = v; break;
        case RP_KEY_Y: k.cy = v; break;
        case RP_KEY_W: if (v <= 0) return fail("the pan/crop box must have a size"); k.w = v; break;
        case RP_KEY_H: if (v <= 0) return fail("the pan/crop box must have a size"); k.h = v; break;
        case RP_KEY_ROTATION: k.rotation = v; break;
        case RP_KEY_SMOOTHNESS: return RS_OK;
        default: return unsupported();
        }
        if (prop == RP_KEY_TIME)
            std::stable_sort(e->panCrop.begin(), e->panCrop.end(), [](const PanCropKey& a, const PanCropKey& b) { return a.time < b.time; });
        touch();
        return RS_OK;
    }
    case RK_MOTION: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        if (outOfRange(d.c, t->motion.size())) return fail("the track motion key is no longer there");
        MotionKey& k = t->motion[(size_t)d.c];
        switch (prop) {
        case RP_KEY_TIME: k.time = std::max(0.0, v); break;
        case RP_KEY_X: k.x = v; break;
        case RP_KEY_Y: k.y = v; break;
        case RP_KEY_W: if (v <= 0) return fail("the track motion box must have a size"); k.w = v; break;
        case RP_KEY_H: if (v <= 0) return fail("the track motion box must have a size"); k.h = v; break;
        case RP_KEY_ROTATION: k.rotation = v; break;
        case RP_KEY_SMOOTHNESS: return RS_OK;
        default: return unsupported();
        }
        if (prop == RP_KEY_TIME)
            std::stable_sort(t->motion.begin(), t->motion.end(), [](const MotionKey& a, const MotionKey& b) { return a.time < b.time; });
        touch();
        return RS_OK;
    }
    case RK_BUS: {
        if (d.b == 1) {
            if (prop == RP_LEVEL) { project.masterVolume = std::max(0.0, v); touch(); return RS_OK; }
            if (prop == RP_PAN) { project.masterPan = std::max(-1.0, std::min(1.0, v)); touch(); return RS_OK; }
            return unsupported();
        }
        if (d.b == 2) return unsupported();
        Bus* b = bus(d);
        if (!b) return RS_FAILED;
        if (prop == RP_LEVEL) { b->level = std::max(0.0, v); touch(); return RS_OK; }
        if (prop == RP_PAN) { b->pan = std::max(-1.0, std::min(1.0, v)); touch(); return RS_OK; }
        return unsupported();
    }
    }
    return unsupported();
}

int ScriptSession::setS(uint64_t h, int prop, const std::string& v)
{
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if (d.kind == RK_OFXPARAM || d.kind == RK_OFXKEY) return ofxSetS(d, prop, v);
    switch (d.kind) {
    case RK_APP: {
        if (prop == RP_UNDO_LABEL) {
            if (undoLabel.empty()) undoLabel = v;
            return RS_OK;
        }
        ProjectSettings& st = project.settings;
        std::string* f = prop == RP_TITLE ? &st.title : prop == RP_ARTIST ? &st.artist : prop == RP_ENGINEER ? &st.engineer
                       : prop == RP_COPYRIGHT ? &st.copyright : prop == RP_COMMENTS ? &st.comments : nullptr;
        if (!f) return unsupported();
        *f = v;
        touch();
        return RS_OK;
    }
    case RK_TRACK: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        if (prop != RP_NAME) return unsupported();
        t->name = v;
        touch();
        return RS_OK;
    }
    case RK_EVENT: return eventProp(d, prop, nullptr, nullptr, nullptr, nullptr, nullptr, &v);
    case RK_TAKE: {
        if (prop != RP_NAME) return unsupported();
        Event* e = event(d);
        if (!e) return RS_FAILED;
        auto all = allTakes(*e);
        if (outOfRange(d.c, all.size())) return fail("the take is no longer there");
        all[(size_t)d.c].name = v;
        setAllTakes(*e, all, e->activeTake);
        touch();
        return RS_OK;
    }
    case RK_MEDIA: {
        MediaItem* m = media(d.a);
        if (!m) return RS_FAILED;
        if (prop == RP_COMMENT) { m->comment = v; touch(); return RS_OK; }
        return unsupported();
    }
    case RK_MARKER: {
        Project::Marker* m = marker(d);
        if (!m) return RS_FAILED;
        if (prop == RP_NAME) { m->name = v; touch(); return RS_OK; }
        if (prop == RP_COMMAND_TYPE && m->kind == "command") { m->command = v; touch(); return RS_OK; }
        return unsupported();
    }
    case RK_BUS: {
        if (prop != RP_NAME || d.b) return unsupported();
        Bus* b = bus(d);
        if (!b) return RS_FAILED;
        b->name = v;
        touch();
        return RS_OK;
    }
    }
    return unsupported();
}

// ------------------------------------------------------------------------------------------------ lists
int ScriptSession::list(uint64_t h, int which, std::vector<uint64_t>* out)
{
    out->clear();
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if (which == RL_OFX_PARAMS || which == RL_OFX_KEYS) return ofxList(d, which, out);
    switch (which) {
    case RL_TRACKS:
        if (d.kind != RK_APP) return unsupported();
        for (const Track& t : project.tracks) out->push_back(trackHandle(t.id));
        return RS_OK;
    case RL_MEDIA:
        if (d.kind != RK_APP) return unsupported();
        for (const MediaItem& m : project.media) out->push_back(mediaHandle(m.id));
        return RS_OK;
    case RL_SELECTED_MEDIA:
        return d.kind == RK_APP ? RS_OK : unsupported();
    case RL_MARKERS: case RL_REGIONS: case RL_COMMAND_MARKERS: case RL_CD_TRACKS: case RL_CD_INDICES: {
        if (d.kind != RK_APP) return unsupported();
        const int kind = which == RL_MARKERS ? 0 : which == RL_REGIONS ? 1 : which == RL_COMMAND_MARKERS ? 2 : which == RL_CD_TRACKS ? 3 : 4;
        for (Project::Marker* m : markersOfKind(kind)) out->push_back(handle({RK_MARKER, m->id, 0, 0, ""}));
        return RS_OK;
    }
    case RL_GROUPS:
        if (d.kind != RK_APP) return unsupported();
        for (uint64_t g : groupIds()) out->push_back(handle({RK_GROUP, g, 0, 0, ""}));
        return RS_OK;
    case RL_VIDEO_FX: case RL_AUDIO_FX: case RL_TRANSITIONS: case RL_GENERATORS: {
        if (d.kind != RK_APP) return unsupported();
        const int type = which == RL_VIDEO_FX ? ScriptPlugin::VideoFilter : which == RL_AUDIO_FX ? ScriptPlugin::Audio
                       : which == RL_TRANSITIONS ? ScriptPlugin::Transition : ScriptPlugin::Generator;
        for (size_t k = 0; k < host_.plugins.size(); ++k)
            if (host_.plugins[k].type == type) out->push_back(handle({RK_PLUGIN, 0, 0, (int64_t)k, ""}));
        return RS_OK;
    }
    case RL_SPECIAL_BUSES:
        if (d.kind != RK_APP) return unsupported();
        out->push_back(handle({RK_BUS, 0, 1, 0, ""}));
        out->push_back(handle({RK_BUS, 0, 2, 0, ""}));
        return RS_OK;
    case RL_BUSES:
        if (d.kind != RK_APP) return unsupported();
        for (const Bus& b : project.buses) out->push_back(handle({RK_BUS, b.id, 0, 0, ""}));
        return RS_OK;
    case RL_EVENTS:
        if (d.kind == RK_TRACK) {
            Track* t = track(d);
            if (!t) return RS_FAILED;
            for (const Event& e : t->events) out->push_back(eventHandle(e.id));
            return RS_OK;
        }
        if (d.kind == RK_GROUP) {
            for (const Track& t : project.tracks)
                for (const Event& e : t.events)
                    if (e.group == d.a) out->push_back(eventHandle(e.id));
            return RS_OK;
        }
        return unsupported();
    case RL_EFFECTS: case RL_MASTER_EFFECTS: {
        Desc o = d;
        if (which == RL_MASTER_EFFECTS) { if (d.kind != RK_APP) return unsupported(); o = {RK_BUS, 0, 1, 0, ""}; }
        if (o.kind == RK_MEDIA) return unsupported();
        FxChain* c = chainOf(o);
        if (!c) return error_.empty() ? unsupported() : RS_FAILED;
        for (const FxSlot& s : *c) out->push_back(handle({RK_FX, s.id, 0, 0, ""}));
        return RS_OK;
    }
    case RL_ENVELOPES: case RL_MASTER_ENVELOPES: {
        Desc o = d;
        if (which == RL_MASTER_ENVELOPES) { if (d.kind != RK_APP) return unsupported(); o = {RK_BUS, 0, 1, 0, ""}; }
        int ok = 0;
        uint64_t oid = 0;
        if (!envOwnerOf(o, &ok, &oid)) return unsupported();
        Envelopes* l = envelopesOf(ok, oid);
        if (!l) return fail("the envelopes' owner is no longer in the project");
        for (const Envelope& e : *l) {
            if (e.kind == "fx") continue;
            out->push_back(handle({RK_ENVELOPE, (uint64_t)ok, oid, (int64_t)e.target, e.kind}));
        }
        return RS_OK;
    }
    case RL_TAKES: {
        if (d.kind != RK_EVENT) return unsupported();
        Event* e = event(d);
        if (!e) return RS_FAILED;
        if (!e->mediaId && e->takes.empty()) return RS_OK;      // an empty event has no takes
        const size_t n = allTakes(*e).size();
        for (size_t k = 0; k < n; ++k) out->push_back(handle({RK_TAKE, e->id, 0, (int64_t)k, ""}));
        return RS_OK;
    }
    case RL_STREAMS: {
        if (d.kind != RK_MEDIA) return unsupported();
        MediaItem* m = media(d.a);
        if (!m) return RS_FAILED;
        if (m->info.hasVideo || m->generated) out->push_back(handle({RK_STREAM, m->id, 0, 3, ""}));
        if (m->info.hasAudio) out->push_back(handle({RK_STREAM, m->id, 0, 2, ""}));
        return RS_OK;
    }
    case RL_POINTS: {
        if (d.kind != RK_ENVELOPE) return unsupported();
        Envelope* e = envelope(d);
        if (!e) return RS_FAILED;
        for (size_t k = 0; k < e->points.size(); ++k) out->push_back(handle({RK_ENVPOINT, h, 0, (int64_t)k, ""}));
        return RS_OK;
    }
    case RL_PANCROP: {
        if (d.kind != RK_EVENT) return unsupported();
        Event* e = event(d);
        if (!e) return RS_FAILED;
        const size_t n = std::max<size_t>(1, e->panCrop.size());
        for (size_t k = 0; k < n; ++k) out->push_back(handle({RK_PANCROP, e->id, 0, (int64_t)k, ""}));
        return RS_OK;
    }
    case RL_MOTION: {
        if (d.kind != RK_TRACK) return unsupported();
        Track* t = track(d);
        if (!t) return RS_FAILED;
        for (size_t k = 0; k < t->motion.size(); ++k) out->push_back(handle({RK_MOTION, t->id, 0, (int64_t)k, ""}));
        return RS_OK;
    }
    }
    return unsupported();
}

// ------------------------------------------------------------------------------------------------ operations
uint64_t ScriptSession::addFx(FxChain& chain, int pluginIndex, int at, bool video)
{
    const ScriptPlugin& p = host_.plugins[(size_t)pluginIndex];
    FxSlot s;
    s.id = project.newId();
    s.name = p.name;
    if (p.type == ScriptPlugin::Audio) {
        s.kind = "audio";
        s.audio = p.audio;
    } else {
        s.kind = "ofx";
        s.pluginId = p.id;
    }
    (void)video;
    if (at < 0 || at > (int)chain.size()) chain.push_back(s);
    else chain.insert(chain.begin() + at, s);
    touch();
    return handle({RK_FX, s.id, 0, 0, ""});
}

int ScriptSession::call(uint64_t h, int op, const std::vector<int64_t>& ia, const std::vector<double>& da, const std::string& s, uint64_t* out,
                        double* dout)
{
    *out = 0;
    *dout = 0;
    const Desc* dp = desc(h);
    if (!dp) return fail("no such object");
    const Desc d = *dp;
    if (d.kind == RK_OFXPARAM || d.kind == RK_OFXKEY) return ofxCall(d, op, ia, da, s, out, dout);
    auto I = [&](size_t k, int64_t def = 0) { return k < ia.size() ? ia[k] : def; };
    auto D = [&](size_t k, double def = 0) { return k < da.size() && std::isfinite(da[k]) ? da[k] : def; };
    auto pluginArg = [&](int64_t hh) -> int {
        const Desc* pd = desc((uint64_t)hh);
        if (!pd || pd->kind != RK_PLUGIN || outOfRange(pd->c, host_.plugins.size())) return -1;
        return (int)pd->c;
    };

    // operations on any owner of effects or envelopes
    if (op == RO_ADD_FX || op == RO_REMOVE_FX || op == RO_ADD_MASTER_FX) {
        Desc o = d;
        if (op == RO_ADD_MASTER_FX) { if (d.kind != RK_APP) return unsupported(); o = {RK_BUS, 0, 1, 0, ""}; }
        if (o.kind != RK_TRACK && o.kind != RK_EVENT && o.kind != RK_BUS && o.kind != RK_APP) return unsupported();
        FxChain* c = chainOf(o);
        if (!c) return RS_FAILED;
        bool video = true;
        if (o.kind == RK_TRACK) { Track* t = track(o); video = t && t->type == TrackType::Video; }
        if (o.kind == RK_EVENT) { Track* t = nullptr; event(o, &t); video = t && t->type == TrackType::Video; }
        if (o.kind == RK_BUS) video = o.b == 2;
        if (op == RO_REMOVE_FX) {
            const Desc* fd = desc((uint64_t)I(0));
            if (!fd || fd->kind != RK_FX) return fail("not an effect");
            const size_t before = c->size();
            c->erase(std::remove_if(c->begin(), c->end(), [&](const FxSlot& x) { return x.id == fd->a; }), c->end());
            if (c->size() == before) return fail("the effect is not in this chain");
            touch();
            return RS_OK;
        }
        const int pi = pluginArg(I(0));
        if (pi < 0) return fail("not a plug-in");
        const ScriptPlugin& p = host_.plugins[(size_t)pi];
        if (video != (p.type != ScriptPlugin::Audio)) return fail(video ? "an audio plug-in cannot go on video" : "a video plug-in cannot go on audio");
        if (p.type == ScriptPlugin::Transition || p.type == ScriptPlugin::Generator)
            return fail("\"" + p.name + "\" is a " + (p.type == ScriptPlugin::Transition ? "transition" : "generator") + ", not an effect");
        *out = addFx(*c, pi, (int)I(1, -1), video);
        return RS_OK;
    }
    if (op == RO_ADD_ENVELOPE || op == RO_REMOVE_ENVELOPE || op == RO_ADD_MASTER_ENVELOPE) {
        Desc o = d;
        if (op == RO_ADD_MASTER_ENVELOPE) { if (d.kind != RK_APP) return unsupported(); o = {RK_BUS, 0, 1, 0, ""}; }
        int ok = 0;
        uint64_t oid = 0;
        if (!envOwnerOf(o, &ok, &oid)) return unsupported();
        Envelopes* l = envelopesOf(ok, oid);
        if (!l) return fail("the envelopes' owner is no longer in the project");
        if (op == RO_REMOVE_ENVELOPE) {
            const Desc* ed = desc((uint64_t)I(0));
            if (!ed || ed->kind != RK_ENVELOPE || (int)ed->a != ok || ed->b != oid) return fail("the envelope is not this object's");
            const size_t before = l->size();
            l->erase(std::remove_if(l->begin(), l->end(), [&](const Envelope& e) { return e.kind == ed->s && e.target == (uint64_t)ed->c; }), l->end());
            if (l->size() == before) return fail("the envelope is no longer there");
            touch();
            return RS_OK;
        }
        bool audio = ok == OwnBus || ok == OwnMaster;
        double eventLength = 0;
        if (ok == OwnTrack) { Track* t = project.findTrack(oid); audio = t && t->type == TrackType::Audio; }
        if (ok == OwnEvent) {
            Track* t = nullptr;
            Event* e = project.findEvent(oid, &t);
            audio = t && t->type == TrackType::Audio;
            eventLength = e ? e->length : 0;
        }
        const int vt = (int)I(0, -1);
        std::string kind;
        uint64_t target = 0;
        if (vt >= 3 && vt <= 28) {
            if (vt - 3 >= (int)project.buses.size()) return fail("there is no such bus in the project");
            kind = "send";
            target = project.buses[(size_t)(vt - 3)].id;
        } else if (const EnvType* et = envTypeOfVegas(vt)) {
            kind = et->kind;
        } else {
            return fail(strfmt("Reno cannot make envelopes of type %d (VEGAS's EnvelopeType)", vt));
        }
        if (!envAllowed(ok, audio, kind)) return fail("this envelope cannot go there (VEGAS's CanAddEnvelope is false)");
        Envelope* e = findEnvelope(*l, kind, target);
        if (!e) {
            Envelope ne;
            ne.kind = kind;
            ne.target = target;
            const EnvType* et = envTypeOfKind(kind);
            EnvPoint p0;
            p0.time = 0;
            p0.value = kind == "send" ? 1.0 : et ? et->neutral : 0;
            if (kind == "mute" || kind == "fadecolor" || kind == "motionblur" || kind == "transition") p0.value = 0;
            if (kind == "mute") p0.curve = CurveHold;
            ne.points.push_back(p0);
            if (kind == "transition" && eventLength > 0) {
                // VEGAS's Transition Progress goes from 0 to 100% along the event.
                EnvPoint p1;
                p1.time = eventLength;
                p1.value = 1;
                ne.points.push_back(p1);
            }
            l->push_back(ne);
            touch();
        } else if (!e->visible) {
            e->visible = true;
            touch();
        }
        *out = handle({RK_ENVELOPE, (uint64_t)ok, oid, (int64_t)target, kind});
        return RS_OK;
    }

    switch (d.kind) {
    case RK_APP:
        switch (op) {
        case RO_ADD_TRACK: {
            const int type = (int)I(0, 3);
            if (type != 2 && type != 3) return fail("a track is video or audio");
            Track t;
            t.id = project.newId();
            t.type = type == 2 ? TrackType::Audio : TrackType::Video;
            if (t.type == TrackType::Audio) t.panType = "add";
            t.name = s;
            int at = (int)I(1, -1);
            if (at < 0 || at > (int)project.tracks.size()) at = (int)project.tracks.size();
            project.tracks.insert(project.tracks.begin() + at, t);
            touch();
            *out = trackHandle(t.id);
            return RS_OK;
        }
        case RO_REMOVE_TRACK: {
            const Desc* td = desc((uint64_t)I(0));
            if (!td || td->kind != RK_TRACK) return fail("not a track");
            const size_t before = project.tracks.size();
            project.tracks.erase(std::remove_if(project.tracks.begin(), project.tracks.end(), [&](const Track& t) { return t.id == td->a; }),
                                 project.tracks.end());
            if (project.tracks.size() == before) return fail("the track is no longer in the project");
            selectedTracks.erase(td->a);
            touch();
            return RS_OK;
        }
        case RO_MOVE_TRACK: {
            const Desc* td = desc((uint64_t)I(0));
            if (!td || td->kind != RK_TRACK) return fail("not a track");
            auto it = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const Track& t) { return t.id == td->a; });
            if (it == project.tracks.end()) return fail("the track is no longer in the project");
            Track t = *it;
            project.tracks.erase(it);
            int at = (int)I(1, 0);
            at = std::max(0, std::min(at, (int)project.tracks.size()));
            project.tracks.insert(project.tracks.begin() + at, t);
            touch();
            return RS_OK;
        }
        case RO_IMPORT_MEDIA: case RO_FIND_MEDIA: {
            for (const MediaItem& m : project.media)
                if (!m.generated && m.subclipLength <= 0 && m.path == s) { *out = mediaHandle(m.id); return RS_OK; }
            if (op == RO_FIND_MEDIA) return RS_OK;
            if (s.empty()) return fail("no file name");
            MediaInfo info;
            std::string err;
            if (!host_.probe || !host_.probe(s, &info, &err)) return fail("the media file could not be opened: " + s + (err.empty() ? "" : " (" + err + ")"));
            MediaItem m;
            m.id = project.newId();
            m.path = s;
            m.name = pathFileName(s);
            m.info = info;
            project.media.push_back(m);
            touch();
            *out = mediaHandle(m.id);
            return RS_OK;
        }
        case RO_ADD_SUBCLIP: {
            const size_t nl = s.find('\n');
            const std::string path = s.substr(0, nl), name = nl == std::string::npos ? std::string() : s.substr(nl + 1);
            uint64_t parentId = 0;
            for (const MediaItem& m : project.media)
                if (!m.generated && m.path == path) { parentId = m.id; break; }
            if (!parentId) {
                uint64_t ph = 0;
                double x = 0;
                const int r = call(1, RO_IMPORT_MEDIA, {}, {}, path, &ph, &x);
                if (r != RS_OK) return r;
                parentId = desc(ph)->a;
            }
            MediaItem m = *project.findMedia(parentId);
            const double full = m.subclipLength > 0 ? m.subclipLength : m.info.duration;
            const double from = std::max(0.0, D(0)) + (m.subclipLength > 0 ? m.subclipStart : 0.0);
            double len = D(1);
            if (full > 0) len = std::min(len, std::max(0.0, (m.subclipLength > 0 ? m.subclipStart : 0.0) + full - from));
            if (!(len > 0)) return fail("a subclip must have a length within its media");
            m.id = project.newId();
            m.subclipStart = from;
            m.subclipLength = len;
            m.name = name.empty() ? m.name : name;
            m.veg = nullptr;
            project.media.push_back(m);
            if (I(0)) reversedMedia_.insert(m.id);
            touch();
            *out = mediaHandle(m.id);
            return RS_OK;
        }
        case RO_REMOVE_MEDIA: {
            const Desc* md = desc((uint64_t)I(0));
            if (!md || md->kind != RK_MEDIA) return fail("not media");
            for (const Track& t : project.tracks)
                for (const Event& e : t.events) {
                    bool used = e.mediaId == md->a;
                    for (const Take& k : e.takes) used = used || k.mediaId == md->a;
                    if (used) return fail("the media is used by events: remove them first");
                }
            const size_t before = project.media.size();
            project.media.erase(std::remove_if(project.media.begin(), project.media.end(), [&](const MediaItem& m) { return m.id == md->a; }),
                                project.media.end());
            if (project.media.size() == before) return fail("the media is no longer in the project");
            touch();
            return RS_OK;
        }
        case RO_REMOVE_UNUSED_MEDIA: {
            std::set<uint64_t> used;
            for (const Track& t : project.tracks)
                for (const Event& e : t.events) {
                    used.insert(e.mediaId);
                    for (const Take& k : e.takes) used.insert(k.mediaId);
                }
            const size_t before = project.media.size();
            project.media.erase(std::remove_if(project.media.begin(), project.media.end(), [&](const MediaItem& m) { return !used.count(m.id); }),
                                project.media.end());
            if (project.media.size() != before) touch();
            return RS_OK;
        }
        case RO_GENERATED_MEDIA: {
            const int pi = pluginArg(I(0));
            if (pi < 0 || host_.plugins[(size_t)pi].type != ScriptPlugin::Generator) return fail("not a generator plug-in (Vegas.Generators)");
            const ScriptPlugin& p = host_.plugins[(size_t)pi];
            MediaItem m;
            m.id = project.newId();
            m.name = p.name;
            m.generated = true;
            m.generator.id = project.newId();
            m.generator.kind = "ofx";
            m.generator.pluginId = p.id;
            m.generator.name = p.name;
            m.info.hasVideo = true;
            m.info.width = project.settings.width;
            m.info.height = project.settings.height;
            m.info.fps = project.settings.fps();
            project.media.push_back(m);
            touch();
            *out = mediaHandle(m.id);
            return RS_OK;
        }
        case RO_ADD_MARKER: {
            const int kind = (int)I(0);
            if (kind < 0 || kind > 4) return fail("not a kind of marker");
            Project::Marker m;
            m.id = project.newId();
            m.time = std::max(0.0, D(0));
            m.region = kind == 1 || kind == 3;
            m.length = m.region ? std::max(0.0, D(1)) : 0.0;
            m.kind = kind == 2 ? "command" : kind == 3 ? "cdtrack" : kind == 4 ? "cdindex" : "";
            if (kind == 2) {
                const size_t nl = s.find('\n');
                m.command = s.substr(0, nl);
                m.name = nl == std::string::npos ? std::string() : s.substr(nl + 1);
            } else {
                m.name = s;
            }
            // VEGAS refuses a second marker at the same place (of the same kind)
            for (const Project::Marker* o : markersOfKind(kind))
                if (std::fabs(o->time - m.time) < 1e-7 && (kind != 1 || std::fabs(o->length - m.length) < 1e-7))
                    return fail("there is already a marker at that position");
            project.markers.push_back(m);
            touch();
            *out = handle({RK_MARKER, m.id, 0, 0, ""});
            return RS_OK;
        }
        case RO_REMOVE_MARKER: {
            const Desc* md = desc((uint64_t)I(0));
            if (!md || md->kind != RK_MARKER) return fail("not a marker");
            const size_t before = project.markers.size();
            project.markers.erase(std::remove_if(project.markers.begin(), project.markers.end(), [&](const Project::Marker& m) { return m.id == md->a; }),
                                  project.markers.end());
            if (project.markers.size() == before) return fail("the marker is no longer in the project");
            touch();
            return RS_OK;
        }
        case RO_NEW_GROUP: {
            const uint64_t g = project.newId();
            emptyGroups_.insert(g);
            *out = handle({RK_GROUP, g, 0, 0, ""});
            return RS_OK;
        }
        case RO_REMOVE_GROUP: {
            const Desc* gd = desc((uint64_t)I(0));
            if (!gd || gd->kind != RK_GROUP) return fail("not a group");
            emptyGroups_.erase(gd->a);
            for (Track& t : project.tracks)
                for (Event& e : t.events)
                    if (e.group == gd->a) { e.group = 0; touch(); }
            return RS_OK;
        }
        case RO_NEW_PROJECT: {
            Project p;
            p.settings = host_.newSettings;
            p.nextId = std::max(project.nextId, p.nextId);
            project = p;
            host_.projectPath.clear();
            selection.clear();
            selectedTracks.clear();
            emptyGroups_.clear();
            reversedMedia_.clear();
            cursor = selStart = selLength = 0;
            startedNew = true;
            openedPath.clear();
            touch();
            viewChanged = true;
            return RS_OK;
        }
        case RO_OPEN_PROJECT: {
            if (!host_.open) return fail("opening projects from a script is not available");
            Project p;
            std::string err;
            if (!host_.open(s, &p, &err)) return fail("the project could not be opened: " + err);
            p.nextId = std::max(project.nextId, p.nextId);
            project = p;
            host_.projectPath = s;
            selection.clear();
            selectedTracks.clear();
            emptyGroups_.clear();
            reversedMedia_.clear();
            cursor = selStart = selLength = 0;
            openedPath = s;
            startedNew = false;
            changed = false;
            viewChanged = true;
            return RS_OK;
        }
        case RO_SAVE_PROJECT: {
            const std::string path = s.empty() ? host_.projectPath : s;
            if (path.empty()) return fail("the project has no file name yet");
            if (!host_.save) return fail("saving from a script is not available");
            std::string err;
            if (!host_.save(project, path, &err)) return fail("the project could not be saved: " + err);
            host_.projectPath = path;
            savedPaths.push_back(path);
            return RS_OK;
        }
        case RO_LOG:
            output.push_back(s);
            if (host_.log) host_.log(s);
            return RS_OK;
        case RO_UPDATE_UI:
            return RS_OK;
        case RO_INSERT_TIME: {
            const double at = D(0), len = D(1);
            if (!(len > 0) || at < 0) return fail("insert time needs a place and a length");
            for (Track& t : project.tracks) {
                std::vector<Event> added;
                for (Event& e : t.events) {
                    if (e.start >= at - kEps) {
                        e.start += len;
                    } else if (e.end() > at + kEps) {
                        Event right = e;
                        right.id = project.newId();
                        right.start = at + len;
                        right.length = e.end() - at;
                        right.offset = e.offset + (at - e.start) * e.rate;
                        for (Take& k : right.takes) k.offset += (at - e.start) * e.rate;
                        for (Envelope& env : right.envelopes) for (EnvPoint& p : env.points) p.time -= at - e.start;
                        right.fadeIn = 0;
                        right.transition = FxSlot();
                        for (auto& x : right.fx) x.id = project.newId();
                        right.group = 0;
                        e.length = at - e.start;
                        e.fadeOut = 0;
                        added.push_back(right);
                    }
                }
                for (Event& e : added) t.events.push_back(e);
                t.sortEvents();
                for (Envelope& env : t.envelopes) for (EnvPoint& p : env.points) if (p.time >= at) p.time += len;
                for (MotionKey& k : t.motion) if (k.time >= at) k.time += len;
            }
            for (Bus& b : project.buses) for (Envelope& env : b.envelopes) for (EnvPoint& p : env.points) if (p.time >= at) p.time += len;
            for (Envelope& env : project.masterEnvelopes) for (EnvPoint& p : env.points) if (p.time >= at) p.time += len;
            for (Envelope& env : project.videoEnvelopes) for (EnvPoint& p : env.points) if (p.time >= at) p.time += len;
            for (auto& m : project.markers) if (m.time >= at) m.time += len;
            touch();
            return RS_OK;
        }
        }
        return unsupported();

    case RK_TRACK: {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        switch (op) {
        case RO_ADD_EVENT: {
            const double start = std::max(0.0, D(0)), len = D(1, 1.0);
            if (!(len > 0)) return fail("an event's length must be more than 0");
            Event e;
            e.id = project.newId();
            e.start = start;
            e.length = len;
            e.name = s;
            e.loop = host_.loopEvents;
            setDefaultFadeCurves(e, t->type);
            t->events.push_back(e);
            t->sortEvents();
            touch();
            *out = eventHandle(e.id);
            return RS_OK;
        }
        case RO_REMOVE_EVENT: {
            const Desc* ed = desc((uint64_t)I(0));
            if (!ed || ed->kind != RK_EVENT) return fail("not an event");
            auto it = std::find_if(t->events.begin(), t->events.end(), [&](const Event& e) { return e.id == ed->a; });
            if (it == t->events.end()) return fail("the event is not on this track");
            if (it->locked) return fail("the event is locked");
            t->events.erase(it);
            selection.erase(ed->a);
            touch();
            return RS_OK;
        }
        case RO_ADD_MOTION_KEY: {
            if (t->type != TrackType::Video) return unsupported();
            const double at = std::max(0.0, D(0));
            for (size_t k = 0; k < t->motion.size(); ++k)
                if (std::fabs(t->motion[k].time - at) < 1e-7) { *out = handle({RK_MOTION, t->id, 0, (int64_t)k, ""}); return RS_OK; }
            // a new key takes the motion as it is at that time (the key before it, or the whole frame)
            MotionKey nk;
            for (const MotionKey& k : t->motion) if (k.time <= at) nk = k;
            if (t->motion.empty() == false && t->motion.front().time > at) nk = t->motion.front();
            nk.time = at;
            t->motion.push_back(nk);
            std::stable_sort(t->motion.begin(), t->motion.end(), [](const MotionKey& a, const MotionKey& b) { return a.time < b.time; });
            for (size_t k = 0; k < t->motion.size(); ++k)
                if (std::fabs(t->motion[k].time - at) < 1e-12) *out = handle({RK_MOTION, t->id, 0, (int64_t)k, ""});
            touch();
            return RS_OK;
        }
        case RO_REMOVE_MOTION_KEY: {
            const Desc* kd = desc((uint64_t)I(0));
            if (!kd || kd->kind != RK_MOTION || kd->a != t->id || outOfRange(kd->c, t->motion.size())) return fail("not one of this track's keys");
            t->motion.erase(t->motion.begin() + kd->c);
            touch();
            return RS_OK;
        }
        }
        return unsupported();
    }

    case RK_EVENT: {
        Track* tr = nullptr;
        Event* e = event(d, &tr);
        if (!e || !tr) return RS_FAILED;
        const bool video = tr->type == TrackType::Video;
        switch (op) {
        case RO_SPLIT: {
            const double at = D(0, -1);
            if (!(at > kEps && at < e->length - kEps)) return fail("the split must be inside the event (0 < offset < its length)");
            if (e->locked) return fail("the event is locked");
            Event right = *e;
            right.id = project.newId();
            right.start = e->start + at;
            right.length = e->length - at;
            right.offset = e->reverse ? e->offset : e->offset + at * e->rate;
            for (Take& k : right.takes) if (!k.reverse) k.offset += at * e->rate;
            for (Envelope& env : right.envelopes) for (EnvPoint& p : env.points) p.time -= at;
            for (PanCropKey& k : right.panCrop) k.time -= at;
            right.fadeIn = 0;
            right.transition = FxSlot();
            for (auto& x : right.fx) x.id = project.newId();
            right.veg = nullptr;
            if (e->reverse) e->offset += right.length * e->rate;
            e->length = at;
            e->fadeOut = 0;
            // the right part is in the same group as the left.
            if (selection.count(e->id)) selection.insert(right.id);
            const uint64_t rid = right.id;
            tr->events.push_back(right);
            tr->sortEvents();
            touch();
            *out = eventHandle(rid);
            return RS_OK;
        }
        case RO_COPY: {
            const Desc* td = desc((uint64_t)I(0));
            if (!td || td->kind != RK_TRACK) return fail("not a track");
            Track* to = track(*td);
            if (!to) return RS_FAILED;
            if (to->type != tr->type) return fail("an event can only be copied to a track of the same type");
            Event c = *e;
            c.id = project.newId();
            c.start = std::max(0.0, D(0, e->start));
            c.group = 0;
            c.locked = false;
            c.veg = nullptr;
            for (auto& x : c.fx) x.id = project.newId();
            if (!c.transition.kind.empty()) c.transition.id = project.newId();
            const uint64_t cid = c.id;
            to->events.push_back(c);
            to->sortEvents();
            touch();
            *out = eventHandle(cid);
            return RS_OK;
        }
        case RO_ADD_TAKE: case RO_REMOVE_TAKE: case RO_CLEAR_TAKES:
            return takeOp(*e, op, ia, s, out);
        case RO_ADJUST_START_LENGTH: {
            const double start = D(0, e->start), len = D(1, e->length);
            if (!(len > 0)) return fail("an event's length must be more than 0");
            if (e->locked) return fail("the event is locked");
            const double delta = start - e->start;
            if (I(0)) {
                e->offset += delta * e->rate;
                for (Take& k : e->takes) k.offset += delta * e->rate;
                for (Envelope& env : e->envelopes) for (EnvPoint& p : env.points) p.time -= delta;
                for (PanCropKey& k : e->panCrop) k.time -= delta;
            }
            e->start = std::max(0.0, start);
            e->length = len;
            tr->sortEvents();
            touch();
            return RS_OK;
        }
        case RO_ADJUST_RATE: {
            const double rate = D(0, 1.0);
            if (!(rate > 0)) return fail("the playback rate must be more than 0");
            if (e->locked) return fail("the event is locked");
            // the event keeps playing the same stretch of its media: its length changes with the rate
            const double end = e->end();
            e->length = e->length * e->rate / rate;
            e->rate = rate;
            if (I(0)) e->start = std::max(0.0, end - e->length);
            tr->sortEvents();
            touch();
            return RS_OK;
        }
        case RO_ADD_PANCROP_KEY: {
            if (!video) return unsupported();
            const double at = std::max(0.0, D(0));
            if (e->panCrop.empty()) e->panCrop.push_back(PanCropKey());
            for (size_t k = 0; k < e->panCrop.size(); ++k)
                if (std::fabs(e->panCrop[k].time - at) < 1e-7) { *out = handle({RK_PANCROP, e->id, 0, (int64_t)k, ""}); return RS_OK; }
            PanCropKey nk = e->panCrop.front();
            for (const PanCropKey& k : e->panCrop) if (k.time <= at) nk = k;
            nk.time = at;
            e->panCrop.push_back(nk);
            std::stable_sort(e->panCrop.begin(), e->panCrop.end(), [](const PanCropKey& a, const PanCropKey& b) { return a.time < b.time; });
            for (size_t k = 0; k < e->panCrop.size(); ++k)
                if (std::fabs(e->panCrop[k].time - at) < 1e-12) *out = handle({RK_PANCROP, e->id, 0, (int64_t)k, ""});
            touch();
            return RS_OK;
        }
        case RO_REMOVE_PANCROP_KEY: {
            const Desc* kd = desc((uint64_t)I(0));
            if (!kd || kd->kind != RK_PANCROP || kd->a != e->id) return fail("not one of this event's keys");
            if (outOfRange(kd->c, e->panCrop.size())) return fail("the key is no longer there");
            if (e->panCrop.size() == 1) return fail("an event's pan/crop always has one key");
            e->panCrop.erase(e->panCrop.begin() + kd->c);
            touch();
            return RS_OK;
        }
        case RO_CLEAR_PANCROP:
            if (!video) return unsupported();
            if (!e->panCrop.empty()) { e->panCrop.clear(); touch(); }
            return RS_OK;
        case RO_SET_TRANSITION: {
            if (!video) return unsupported();
            if (I(0) == 0) { e->transition = FxSlot(); touch(); return RS_OK; }
            const int pi = pluginArg(I(0));
            if (pi < 0) return fail("not a plug-in");
            const ScriptPlugin& p = host_.plugins[(size_t)pi];
            if (p.type != ScriptPlugin::Transition) return fail("\"" + p.name + "\" is not a transition (Vegas.Transitions)");
            e->transition = FxSlot();
            e->transition.id = project.newId();
            e->transition.kind = "ofx";
            e->transition.pluginId = p.id;
            e->transition.name = p.name;
            touch();
            *out = handle({RK_FX, e->transition.id, 0, 0, ""});
            return RS_OK;
        }
        }
        return unsupported();
    }

    case RK_GROUP: {
        const Desc* ed = desc((uint64_t)I(0));
        if (!ed || ed->kind != RK_EVENT) return fail("not an event");
        Event* e = event(*ed);
        if (!e) return RS_FAILED;
        if (op == RO_GROUP_ADD) {
            e->group = d.a;
            emptyGroups_.erase(d.a);
            touch();
            return RS_OK;
        }
        if (op == RO_GROUP_REMOVE) {
            if (e->group != d.a) return fail("the event is not in this group");
            e->group = 0;
            touch();
            return RS_OK;
        }
        return unsupported();
    }

    case RK_ENVELOPE: {
        Envelope* env = envelope(d);
        if (!env) return RS_FAILED;
        const EnvType* et = envTypeOfKind(env->kind);
        switch (op) {
        case RO_ADD_POINT: {
            const double x = D(0);
            double y = D(1);
            y = et ? std::max(et->min, std::min(et->max, y)) : std::max(0.0, std::min(4.0, y));
            const int curve = renoCurve((int)I(0, 1));
            size_t at = env->points.size();
            for (size_t k = 0; k < env->points.size(); ++k) {
                if (std::fabs(env->points[k].time - x) < 1e-7) {
                    env->points[k].value = y;
                    env->points[k].curve = curve;
                    touch();
                    *out = handle({RK_ENVPOINT, h, 0, (int64_t)k, ""});
                    return RS_OK;
                }
                if (env->points[k].time > x) { at = k; break; }
            }
            EnvPoint p;
            p.time = x;
            p.value = y;
            p.curve = curve;
            env->points.insert(env->points.begin() + (long)at, p);
            touch();
            *out = handle({RK_ENVPOINT, h, 0, (int64_t)at, ""});
            return RS_OK;
        }
        case RO_REMOVE_POINT: {
            const Desc* pd = desc((uint64_t)I(0));
            if (!pd || pd->kind != RK_ENVPOINT || pd->a != h) return fail("not one of this envelope's points");
            if (outOfRange(pd->c, env->points.size())) return fail("the point is no longer there");
            env->points.erase(env->points.begin() + pd->c);
            touch();
            return RS_OK;
        }
        case RO_CLEAR_POINTS:
            // VEGAS keeps an envelope's first point
            if (env->points.size() > 1) { env->points.erase(env->points.begin() + 1, env->points.end()); touch(); }
            return RS_OK;
        case RO_VALUE_AT:
            *dout = env->valueAt(D(0), et ? et->neutral : 1.0);
            return RS_OK;
        }
        return unsupported();
    }
    }
    return unsupported();
}

int ScriptSession::takeOp(Event& e, int op, const std::vector<int64_t>& ia, const std::string& s, uint64_t* out)
{
    Track* tr = nullptr;
    project.findEvent(e.id, &tr);
    const bool empty = !e.mediaId && e.takes.empty();
    if (op == RO_CLEAR_TAKES) {
        if (!e.takes.empty()) { e.takes.clear(); e.activeTake = 0; touch(); }
        return RS_OK;
    }
    if (op == RO_REMOVE_TAKE) {
        const Desc* kd = desc(ia.empty() ? 0 : (uint64_t)ia[0]);
        if (!kd || kd->kind != RK_TAKE || kd->a != e.id) return fail("not one of this event's takes");
        auto all = allTakes(e);
        if (outOfRange(kd->c, all.size())) return fail("the take is no longer there");
        if (all.size() == 1) {
            // the only take: the event becomes an empty event (as in VEGAS)
            e.mediaId = 0;
            e.offset = 0;
            e.takes.clear();
            e.activeTake = 0;
            touch();
            return RS_OK;
        }
        int active = e.activeTake;
        all.erase(all.begin() + kd->c);
        if (kd->c < active || active >= (int)all.size()) active = std::max(0, active - 1);
        setAllTakes(e, all, active);
        touch();
        return RS_OK;
    }
    // RO_ADD_TAKE
    const Desc* sd = desc(ia.empty() ? 0 : (uint64_t)ia[0]);
    if (!sd || sd->kind != RK_STREAM) return fail("not a media stream");
    MediaItem* m = media(sd->a);
    if (!m) return RS_FAILED;
    const bool trackAudio = tr && tr->type == TrackType::Audio;
    if ((sd->c == 2) != trackAudio) return fail(trackAudio ? "a video stream cannot be a take of an audio event" : "an audio stream cannot be a take of a video event");
    Take k;
    k.mediaId = m->id;
    k.name = s.empty() ? m->name : s;
    Event probe = e;
    probe.reverse = false;
    setTakeOffset(probe, m, 0);
    k.offset = probe.offset;
    k.reverse = probe.reverse;
    const bool makeActive = ia.size() < 2 || ia[1] != 0;
    if (empty) {
        e.mediaId = k.mediaId;
        e.offset = k.offset;
        e.name = k.name;
        e.reverse = k.reverse;
        e.takes.clear();
        e.activeTake = 0;
        touch();
        *out = handle({RK_TAKE, e.id, 0, 0, ""});
        return RS_OK;
    }
    auto all = allTakes(e);
    all.push_back(k);
    const int idx = (int)all.size() - 1;
    setAllTakes(e, all, makeActive ? idx : e.activeTake);
    touch();
    *out = handle({RK_TAKE, e.id, 0, idx, ""});
    return RS_OK;
}

// ------------------------------------------------------------------------------------------------ OpenFX parameters
// VEGAS's OFXEffect.Parameters.
namespace {
// VEGAS's OFXInterpolationType <-> Reno's key curves (ofx::Param::KeyCurve: 0 Linear, 1 Fast, 2 Slow, 3 Smooth, 4 Sharp, 5 Hold)
int vegasInterp(int c) { return c >= 0 && c <= 5 ? c + 1 : 1; }
int renoInterp(int v) { return v >= 1 && v <= 6 ? v - 1 : 0; }

std::string contextOf(Project& p, uint64_t slotId)
{
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (e.transition.id == slotId && !e.transition.kind.empty()) return "Transition";
    for (const MediaItem& m : p.media)
        if (m.generated && m.generator.id == slotId) return "Generator";
    return "Filter";
}
}  // namespace

double ScriptSession::ofxFps() const
{
    const ProjectSettings& st = project.settings;
    const double f = st.fpsDen > 0 ? (double)st.fpsNum / st.fpsDen : 0;
    return f > 0 ? f : 30.0;
}

const ScriptOfxDesc* ScriptSession::ofxDesc(const FxSlot& s, std::string* error)
{
    if (s.kind != "ofx") { if (error) *error = "not an OpenFX effect"; return nullptr; }
    const std::string ctx = contextOf(project, s.id);
    const std::string key = s.pluginId + "|" + ctx;
    auto it = ofxDescs_.find(key);
    if (it != ofxDescs_.end()) return &it->second;
    ScriptOfxDesc d;
    std::string err;
    if (!host_.describeOfx || !host_.describeOfx(s.pluginId, ctx, &d, &err)) {
        if (error) *error = "the parameters of " + fxSlotLabel(s) + " cannot be read" + (err.empty() ? "" : ": " + err);
        return nullptr;
    }
    return &(ofxDescs_[key] = d);
}

const ScriptOfxParam* ScriptSession::ofxParam(const Desc& d, FxSlot** slot)
{
    Desc fd{RK_FX, d.a, 0, 0, ""};
    FxSlot* s = fx(fd);
    if (!s) return nullptr;
    std::string err;
    const ScriptOfxDesc* od = ofxDesc(*s, &err);
    if (!od) { fail(err); return nullptr; }
    if (outOfRange(d.c, od->params.size())) { fail("no such parameter"); return nullptr; }
    if (slot) *slot = s;
    return &od->params[(size_t)d.c];
}

ScriptSession::OfxValue ScriptSession::ofxRead(const FxSlot& s, const ScriptOfxParam& p) const
{
    OfxValue v;
    v.v = p.def;
    v.v.resize((size_t)p.dim, 0.0);
    v.s = p.defText;
    const Json& e = s.state.get("params").get(p.name);
    if (!e.isObject()) return v;
    if (p.dim > 0) {
        const Json& a = e.get("v");
        for (size_t k = 0; k < (size_t)p.dim && k < a.size(); ++k) v.v[k] = a.at(k).asDouble();
        for (const Json& k : e.get("k").items()) {
            if (k.size() < 1) continue;
            std::vector<double> x((size_t)p.dim, 0.0);
            for (size_t c = 0; c < (size_t)p.dim && c + 1 < k.size(); ++c) x[c] = k.at(c + 1).asDouble();
            v.keys[k.at(0).asDouble()] = x;
        }
        for (const Json& c : e.get("kc").items())
            if (c.size() >= 2 && v.keys.count(c.at(0).asDouble())) v.curves[c.at(0).asDouble()] = c.at(1).asInt();
    } else {
        if (e.has("s")) v.s = e.get("s").asString();
        for (const Json& k : e.get("ks").items())
            if (k.size() >= 2) v.textKeys[k.at(0).asDouble()] = k.at(1).asString();
    }
    return v;
}

void ScriptSession::ofxWrite(FxSlot& s, const ScriptOfxParam& p, const OfxValue& v)
{
    if (!s.state.isObject()) {
        s.state = Json::object();
        s.state["plugin"] = s.pluginId;
    }
    if (!s.state.get("params").isObject()) s.state["params"] = Json::object();
    Json e = Json::object();
    if (p.dim > 0) {
        Json a = Json::array();
        for (double x : v.v) a.push(p.whole ? std::round(x) : x);
        e["v"] = a;
        if (p.whole) e["i"] = true;
        if (!v.keys.empty()) {
            Json ks = Json::array(), kc = Json::array();
            for (const auto& kv : v.keys) {
                Json k = Json::array();
                k.push(kv.first);
                for (double x : kv.second) k.push(p.whole ? std::round(x) : x);
                ks.push(k);
                auto c = v.curves.find(kv.first);
                if (c != v.curves.end() && c->second != 0) {
                    Json one = Json::array();
                    one.push(kv.first);
                    one.push(c->second);
                    kc.push(one);
                }
            }
            e["k"] = ks;
            if (kc.size() > 0) e["kc"] = kc;
        }
    } else {
        e["s"] = v.s;
        if (!v.textKeys.empty()) {
            Json ks = Json::array();
            for (const auto& kv : v.textKeys) {
                Json k = Json::array();
                k.push(kv.first);
                k.push(kv.second);
                ks.push(k);
            }
            e["ks"] = ks;
        }
    }
    s.state["params"][p.name] = e;
    fxStatesChanged.insert(s.id);
    touch();
}

std::vector<double> ScriptSession::ofxKeyTimes(const OfxValue& v, const ScriptOfxParam& p) const
{
    std::vector<double> t;
    if (p.dim > 0) for (const auto& kv : v.keys) t.push_back(kv.first);
    else for (const auto& kv : v.textKeys) t.push_back(kv.first);
    return t;
}

namespace {
// The value at 'frame' of keyed numbers (between two keys: the left key's curve; before the first key, after the last: theirs)
std::vector<double> keyedAt(const std::map<double, std::vector<double>>& keys, const std::map<double, int>& curves, double frame)
{
    auto hi = keys.lower_bound(frame);
    if (hi == keys.begin()) return hi->second;
    if (hi == keys.end()) return std::prev(hi)->second;
    if (std::fabs(hi->first - frame) < 1e-9) return hi->second;
    auto lo = std::prev(hi);
    auto c = curves.find(lo->first);
    const int curve = c == curves.end() ? 0 : c->second;
    if (curve == 5) return lo->second;      // Hold
    const double f = curveShape(curve, (frame - lo->first) / (hi->first - lo->first));
    std::vector<double> out(lo->second.size());
    for (size_t k = 0; k < out.size() && k < hi->second.size(); ++k) out[k] = lo->second[k] + (hi->second[k] - lo->second[k]) * f;
    return out;
}
}  // namespace

int ScriptSession::ofxGetI(const Desc& d, int prop, int64_t* out)
{
    FxSlot* s = nullptr;
    const ScriptOfxParam* p = ofxParam(d, &s);
    if (!p) return RS_FAILED;
    if (d.kind == RK_OFXKEY) {
        const auto times = ofxKeyTimes(ofxRead(*s, *p), *p);
        if (d.b >= times.size()) return fail("the keyframe is no longer there");
        switch (prop) {
        case RP_INDEX: *out = (int64_t)d.b; return RS_OK;
        case RP_OFX_TYPE: *out = p->type; return RS_OK;
        case RP_OFX_INTERP: {
            const OfxValue v = ofxRead(*s, *p);
            auto c = v.curves.find(times[d.b]);
            *out = vegasInterp(c == v.curves.end() ? 0 : c->second);
            return RS_OK;
        }
        }
        return unsupported();
    }
    switch (prop) {
    case RP_INDEX: *out = d.c; return RS_OK;
    case RP_OFX_TYPE: *out = p->type; return RS_OK;
    case RP_OFX_ENABLED: *out = b2i(p->enabled); return RS_OK;
    case RP_OFX_SECRET: *out = b2i(p->secret); return RS_OK;
    case RP_OFX_CAN_ANIMATE: *out = b2i(p->canAnimate); return RS_OK;
    case RP_OFX_PERSIST: *out = b2i(p->persist); return RS_OK;
    case RP_OFX_DIM: *out = p->dim; return RS_OK;
    case RP_OFX_ANIMATED: *out = b2i(!ofxKeyTimes(ofxRead(*s, *p), *p).empty()); return RS_OK;
    case RP_OFX_EFFECT: *out = (int64_t)handle({RK_FX, d.a, 0, 0, ""}); return RS_OK;
    }
    return unsupported();
}

int ScriptSession::ofxGetD(const Desc& d, int prop, double* out)
{
    FxSlot* s = nullptr;
    const ScriptOfxParam* p = ofxParam(d, &s);
    if (!p) return RS_FAILED;
    if (d.kind == RK_OFXKEY && prop == RP_KEY_TIME) {
        const auto times = ofxKeyTimes(ofxRead(*s, *p), *p);
        if (d.b >= times.size()) return fail("the keyframe is no longer there");
        *out = times[d.b] / ofxFps();
        return RS_OK;
    }
    return unsupported();
}

int ScriptSession::ofxGetS(const Desc& d, int prop, std::string* out)
{
    FxSlot* s = nullptr;
    const ScriptOfxParam* p = ofxParam(d, &s);
    if (!p) return RS_FAILED;
    if (d.kind == RK_OFXKEY) {
        if (prop != RP_OFX_TEXT) return unsupported();
        const OfxValue v = ofxRead(*s, *p);
        const auto times = ofxKeyTimes(v, *p);
        if (d.b >= times.size() || p->dim > 0) return fail("not a text keyframe");
        *out = v.textKeys.at(times[d.b]);
        return RS_OK;
    }
    switch (prop) {
    case RP_NAME: *out = p->name; return RS_OK;
    case RP_OFX_LABEL: *out = p->label; return RS_OK;
    case RP_OFX_HINT: *out = p->hint; return RS_OK;
    case RP_OFX_PARENT: *out = p->parent; return RS_OK;
    case RP_OFX_CHOICES: {
        out->clear();
        for (size_t k = 0; k < p->choices.size(); ++k) *out += (k ? "\n" : "") + p->choices[k];
        return RS_OK;
    }
    case RP_OFX_TEXT: {
        if (p->dim > 0) return fail("not a text parameter");
        const OfxValue v = ofxRead(*s, *p);
        if (!v.textKeys.empty()) {
            const double f = ofxTime_[d.a] * ofxFps();
            auto it = v.textKeys.upper_bound(f + 1e-9);
            *out = it == v.textKeys.begin() ? it->second : std::prev(it)->second;
        } else {
            *out = v.s;
        }
        return RS_OK;
    }
    }
    return unsupported();
}

int ScriptSession::ofxSetI(const Desc& d, int prop, int64_t x)
{
    FxSlot* s = nullptr;
    const ScriptOfxParam* p = ofxParam(d, &s);
    if (!p) return RS_FAILED;
    OfxValue v = ofxRead(*s, *p);
    const auto times = ofxKeyTimes(v, *p);
    if (d.kind == RK_OFXKEY) {
        if (prop != RP_OFX_INTERP) return unsupported();
        if (d.b >= times.size()) return fail("the keyframe is no longer there");
        const int c = renoInterp((int)x);
        if (c == 0) v.curves.erase(times[d.b]);
        else v.curves[times[d.b]] = c;
        ofxWrite(*s, *p, v);
        return RS_OK;
    }
    if (prop != RP_OFX_ANIMATED) return unsupported();
    if (x) {
        if (!p->canAnimate) return fail(p->name + " cannot be animated");
        // as VEGAS: a parameter that becomes animated gets a keyframe with its value at the start.
        if (times.empty()) {
            if (p->dim > 0) v.keys[0] = v.v;
            else v.textKeys[0] = v.s;
        }
    } else {
        const double f = ofxTime_[d.a] * ofxFps();
        if (p->dim > 0 && !v.keys.empty()) v.v = keyedAt(v.keys, v.curves, f);
        v.keys.clear();
        v.curves.clear();
        v.textKeys.clear();
    }
    ofxWrite(*s, *p, v);
    return RS_OK;
}

int ScriptSession::ofxSetD(const Desc& d, int prop, double x)
{
    FxSlot* s = nullptr;
    const ScriptOfxParam* p = ofxParam(d, &s);
    if (!p) return RS_FAILED;
    if (d.kind != RK_OFXKEY || prop != RP_KEY_TIME) return unsupported();
    OfxValue v = ofxRead(*s, *p);
    const auto times = ofxKeyTimes(v, *p);
    if (d.b >= times.size()) return fail("the keyframe is no longer there");
    const double from = times[d.b], to = std::max(0.0, x) * ofxFps();
    if (p->dim > 0) {
        auto val = v.keys[from];
        v.keys.erase(from);
        v.keys[to] = val;
        auto c = v.curves.find(from);
        if (c != v.curves.end()) { const int cv = c->second; v.curves.erase(c); v.curves[to] = cv; }
    } else {
        auto val = v.textKeys[from];
        v.textKeys.erase(from);
        v.textKeys[to] = val;
    }
    ofxWrite(*s, *p, v);
    return RS_OK;
}

int ScriptSession::ofxSetS(const Desc& d, int prop, const std::string& x)
{
    FxSlot* s = nullptr;
    const ScriptOfxParam* p = ofxParam(d, &s);
    if (!p) return RS_FAILED;
    if (prop != RP_OFX_TEXT) return unsupported();
    if (p->dim > 0) return fail("not a text parameter");
    OfxValue v = ofxRead(*s, *p);
    const auto times = ofxKeyTimes(v, *p);
    if (d.kind == RK_OFXKEY) {
        if (d.b >= times.size()) return fail("the keyframe is no longer there");
        v.textKeys[times[d.b]] = x;
    } else if (!v.textKeys.empty()) {
        v.textKeys[ofxTime_[d.a] * ofxFps()] = x;
    } else {
        v.s = x;
    }
    ofxWrite(*s, *p, v);
    return RS_OK;
}

int ScriptSession::ofxList(const Desc& d, int which, std::vector<uint64_t>* out)
{
    if (which == RL_OFX_PARAMS && d.kind == RK_FX) {
        FxSlot* s = fx(d);
        if (!s) return RS_FAILED;
        std::string err;
        const ScriptOfxDesc* od = ofxDesc(*s, &err);
        if (!od) return fail(err);
        for (size_t k = 0; k < od->params.size(); ++k) out->push_back(handle({RK_OFXPARAM, d.a, 0, (int64_t)k, ""}));
        return RS_OK;
    }
    if (which == RL_OFX_KEYS && d.kind == RK_OFXPARAM) {
        FxSlot* s = nullptr;
        const ScriptOfxParam* p = ofxParam(d, &s);
        if (!p) return RS_FAILED;
        const size_t n = ofxKeyTimes(ofxRead(*s, *p), *p).size();
        for (size_t k = 0; k < n; ++k) out->push_back(handle({RK_OFXKEY, d.a, (uint64_t)k, d.c, ""}));
        return RS_OK;
    }
    return unsupported();
}

int ScriptSession::ofxCall(const Desc& d, int op, const std::vector<int64_t>& ia, const std::vector<double>& da, const std::string& str,
                           uint64_t* out, double* dout)
{
    FxSlot* s = nullptr;
    const ScriptOfxParam* p = ofxParam(d, &s);
    if (!p) return RS_FAILED;
    OfxValue v = ofxRead(*s, *p);
    const auto times = ofxKeyTimes(v, *p);
    const double fps = ofxFps();
    const double now = ofxTime_[d.a] * fps;
    if (d.kind == RK_OFXKEY && d.b >= times.size()) return fail("the keyframe is no longer there");
    switch (op) {
    case RO_OFX_GET: {
        if (p->dim <= 0) return fail("not a parameter with numbers");
        const size_t comp = ia.empty() ? 0 : (size_t)std::max<int64_t>(0, ia[0]);
        const int what = ia.size() > 1 ? (int)ia[1] : 0;
        if (comp >= (size_t)p->dim) return fail("no such component");
        std::vector<double> x;
        if (d.kind == RK_OFXKEY) x = v.keys[times[d.b]];
        else
            switch (what) {
            case 0: x = v.keys.empty() ? v.v : keyedAt(v.keys, v.curves, now); break;
            case 1: x = p->def; break;
            case 2: x = p->min; break;
            case 3: x = p->max; break;
            case 4: x = p->displayMin; break;
            case 5: x = p->displayMax; break;
            case 6: x = v.keys.empty() ? v.v : keyedAt(v.keys, v.curves, (da.empty() ? 0 : da[0]) * fps); break;
            default: return fail("no such value");
            }
        *dout = comp < x.size() ? x[comp] : 0;
        if (p->whole && what == 0) *dout = std::round(*dout);
        return RS_OK;
    }
    case RO_OFX_SET: {
        if (p->dim <= 0) return fail("not a parameter with numbers");
        if ((int)da.size() < p->dim) return fail("too few numbers");
        std::vector<double> x(da.begin(), da.begin() + p->dim);
        for (double& c : x) {
            if (!std::isfinite(c)) return fail("not a number");
            if (p->whole) c = std::round(c);
        }
        if (d.kind == RK_OFXKEY) v.keys[times[d.b]] = x;
        else if (!v.keys.empty()) v.keys[now] = x;
        else v.v = x;
        ofxWrite(*s, *p, v);
        return RS_OK;
    }
    case RO_OFX_SET_AT: {
        if (d.kind != RK_OFXPARAM) return unsupported();
        if (!p->canAnimate) return fail(p->name + " cannot be animated");
        if (da.empty() || !std::isfinite(da[0])) return fail("no time");
        const double f = std::max(0.0, da[0]) * fps;
        double at = f;
        for (double t : times) if (std::fabs(t - f) < 0.5) at = t;
        if (p->dim > 0) {
            if ((int)da.size() < p->dim + 1) return fail("too few numbers");
            std::vector<double> x(da.begin() + 1, da.begin() + 1 + p->dim);
            for (double& c : x) {
                if (!std::isfinite(c)) return fail("not a number");
                if (p->whole) c = std::round(c);
            }
            v.keys[at] = x;
        } else {
            v.textKeys[at] = str;
        }
        ofxWrite(*s, *p, v);
        const auto after = ofxKeyTimes(v, *p);
        for (size_t k = 0; k < after.size(); ++k)
            if (after[k] == at) *out = handle({RK_OFXKEY, d.a, (uint64_t)k, d.c, ""});
        return RS_OK;
    }
    case RO_OFX_REMOVE_KEY: {
        if (d.kind != RK_OFXPARAM || ia.empty()) return unsupported();
        const Desc* kd = desc((uint64_t)ia[0]);
        if (!kd || kd->kind != RK_OFXKEY || kd->a != d.a || kd->c != d.c || kd->b >= times.size()) return fail("not a keyframe of this parameter");
        const double t = times[kd->b];
        if (p->dim > 0) {
            if (v.keys.size() == 1) v.v = v.keys.begin()->second;
            v.keys.erase(t);
            v.curves.erase(t);
        } else {
            if (v.textKeys.size() == 1) v.s = v.textKeys.begin()->second;
            v.textKeys.erase(t);
        }
        ofxWrite(*s, *p, v);
        return RS_OK;
    }
    }
    return unsupported();
}

}  // namespace reno
