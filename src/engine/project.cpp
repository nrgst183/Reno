#include "project.h"
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <cmath>

namespace reno {

std::string MediaItem::readPath() const { return sequenceFps > 0 && !path.empty() ? imageSequenceSpec(path, sequenceFps) : path; }

void Track::sortEvents()
{
    const auto before = [](const Event& a, const Event& b) { return a.start < b.start; };
    const CowVec<Event>& cev = events;
    if (std::is_sorted(cev.begin(), cev.end(), before)) return;
    std::vector<Event>& v = events.mut();
    std::vector<uint32_t> order(v.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = (uint32_t)k;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return v[a].start < v[b].start; });
    std::vector<Event> sorted;
    sorted.reserve(v.size());
    for (uint32_t k : order) sorted.push_back(std::move(v[k]));
    v.swap(sorted);
}

double Project::length() const
{
    double len = 0;
    for (const auto& t : tracks)
        for (const auto& e : t.events) len = std::max(len, e.end());
    return len;
}

const MediaItem* Project::findMedia(uint64_t id) const
{
    for (const auto& m : media) if (m.id == id) return &m;
    return nullptr;
}
MediaItem* Project::findMedia(uint64_t id)
{
    for (auto& m : media) if (m.id == id) return &m;
    return nullptr;
}
Track* Project::findTrack(uint64_t id)
{
    for (auto& t : tracks) if (t.id == id) return &t;
    return nullptr;
}
Bus* Project::findBus(uint64_t id)
{
    for (auto& b : buses) if (b.id == id) return &b;
    return nullptr;
}
const Bus* Project::findBus(uint64_t id) const
{
    for (const auto& b : buses) if (b.id == id) return &b;
    return nullptr;
}

// An index of the events by id: where each one was when the index was made.
struct Project::EventIndex {
    std::vector<size_t> counts;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> where;
};

namespace {
const size_t kIndexFrom = 192;      // fewer events than this: just look through them
}

static std::shared_ptr<const Project::EventIndex> makeEventIndex(const std::vector<Track>& tracks)
{
    auto ix = std::make_shared<Project::EventIndex>();
    size_t n = 0;
    for (const Track& t : tracks) { ix->counts.push_back(t.events.size()); n += t.events.size(); }
    ix->where.reserve(n * 2);
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const CowVec<Event>& ev = tracks[ti].events;
        for (size_t k = 0; k < ev.size(); ++k) ix->where.emplace(ev[k].id, std::make_pair((uint32_t)ti, (uint32_t)k));
    }
    return ix;
}

// The track and place of the event with this id, or false.
static bool indexedFind(const std::vector<Track>& tracks, std::shared_ptr<const Project::EventIndex>& slot, uint64_t id, size_t& ti, size_t& k)
{
    size_t total = 0;
    for (const Track& t : tracks) total += t.events.size();
    if (total < kIndexFrom) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::shared_ptr<const Project::EventIndex> ix = std::atomic_load(&slot);
        if (ix) {
            auto it = ix->where.find(id);
            if (it != ix->where.end()) {
                ti = it->second.first;
                k = it->second.second;
                if (ti < tracks.size() && k < tracks[ti].events.size() && tracks[ti].events[k].id == id) return true;
            } else {
                // not known: if the tracks hold as many events as when the index was made, it is not here
                bool fresh = ix->counts.size() == tracks.size();
                for (size_t t = 0; fresh && t < tracks.size(); ++t) fresh = ix->counts[t] == tracks[t].events.size();
                if (fresh) return false;
            }
        }
        if (attempt == 0) std::atomic_store(&slot, makeEventIndex(tracks));
    }
    return false;
}

const Event* Project::findEvent(uint64_t id, const Track** track) const
{
    size_t ti = 0, k = 0;
    if (indexedFind(tracks, eventIndex_, id, ti, k)) {
        if (track) *track = &tracks[ti];
        return &tracks[ti].events[k];
    }
    for (const auto& t : tracks) {
        const CowVec<Event>& ev = t.events;
        for (size_t i = 0; i < ev.size(); ++i)
            if (ev[i].id == id) { if (track) *track = &t; return &ev[i]; }
    }
    return nullptr;
}
Event* Project::findEvent(uint64_t id, Track** track)
{
    const Track* t = nullptr;
    const Event* e = static_cast<const Project*>(this)->findEvent(id, &t);
    if (!e) return nullptr;
    Track* mt = const_cast<Track*>(t);
    if (track) *track = mt;
    return &mt->events[(size_t)(e - mt->events.cbegin())];
}

void Project::collectEventIds(std::vector<uint64_t>& out) const
{
    for (const auto& t : tracks)
        for (const Event& e : t.events.cvec()) out.push_back(e.id);
}

void Project::forEachFx(const std::function<void(FxSlot&)>& fn)
{
    for (auto& m : media) if (m.generated) fn(m.generator);
    for (auto& t : tracks) {
        for (auto& f : t.fx) fn(f);
        bool any = false;
        for (const Event& e : static_cast<const Track&>(t).events)
            if (!e.fx.empty() || !e.transition.kind.empty()) { any = true; break; }
        if (!any) continue;
        for (auto& e : t.events) {
            for (auto& f : e.fx) fn(f);
            if (!e.transition.kind.empty()) fn(e.transition);
        }
    }
    for (auto& b : buses)
        for (auto& f : b.fx) fn(f);
    for (auto& f : videoOut) fn(f);
    for (auto& f : audioOut) fn(f);
}

void Project::forEachFx(const std::function<void(const FxSlot&)>& fn) const
{
    // reads only: the events of a const project may be shared with others and are never made unshared here.
    for (const auto& m : media) if (m.generated) fn(m.generator);
    for (const auto& t : tracks) {
        for (const auto& f : t.fx) fn(f);
        for (const Event& e : t.events) {
            for (const auto& f : e.fx) fn(f);
            if (!e.transition.kind.empty()) fn(e.transition);
        }
    }
    for (const auto& b : buses)
        for (const auto& f : b.fx) fn(f);
    for (const auto& f : videoOut) fn(f);
    for (const auto& f : audioOut) fn(f);
}

FxSlot* Project::findFx(uint64_t slotId)
{
    FxSlot* found = nullptr;
    forEachFx([&](FxSlot& s) { if (s.id == slotId) found = &s; });
    return found;
}

bool Project::anySolo(TrackType type) const
{
    for (const auto& t : tracks) if (t.type == type && t.solo) return true;
    return false;
}

double Project::quantize(double t) const
{
    const double fps = settings.fps();
    return std::floor(t * fps + 0.5) / fps;
}

int64_t Project::frameAt(double t) const
{
    return (int64_t)std::floor(t * settings.fps() + 1e-6);
}

double Project::frameTime(int64_t frame) const { return (double)frame / settings.fps(); }

// The area under curveShape(curve, x) from x = 0 to x = X.
static double curveArea(int curve, double X)
{
    X = std::min(1.0, std::max(0.0, X));
    switch (curve) {
    case CurveFast: return X + (1.0 - X) * (1.0 - X) * (1.0 - X) / 3.0 - 1.0 / 3.0;
    case CurveSlow: return X * X * X / 3.0;
    case CurveSmooth: return X * X * X - X * X * X * X / 2.0;
    case CurveHold: return 0.0;
    case CurveSharp: return X * X * (1.5 + X * (X - 2.0));             // the integral of 4x^3 - 6x^2 + 3x
    default: return X * X / 2.0;
    }
}

// The event's speed (VEGAS's velocity envelope, -1..10) at 'u' seconds into the event.
static inline double clampVelocity(double v) { return std::min(10.0, std::max(-1.0, v)); }

double eventVelocityIntegral(const Event& e, double u)
{
    const Envelope* env = findEnvelope(e.envelopes, "velocity");
    if (!env || env->points.empty()) return u;
    const std::vector<EnvPoint>& pts = env->points;
    double sign = 1.0;
    if (u < 0) { u = -u; sign = -1.0; }
    double sum = 0.0, at = 0.0;
    // before the first point the speed is the first point's
    const double first = std::max(0.0, pts.front().time);
    if (u <= first) return sign * u * clampVelocity(pts.front().value);
    sum += first * clampVelocity(pts.front().value);
    at = first;
    for (size_t k = 0; k + 1 < pts.size(); ++k) {
        const EnvPoint& a = pts[k];
        const EnvPoint& b = pts[k + 1];
        if (b.time <= at) continue;
        const double t0 = std::max(at, a.time), t1 = b.time, span = b.time - a.time;
        const double va = clampVelocity(a.value), vb = clampVelocity(b.value);
        const double end = std::min(u, t1);
        if (span > 0 && end > t0) {
            // the area from a.time to end, less the area from a.time to t0 (t0 > a.time only if points overlap)
            const double x1 = (end - a.time) / span, x0 = (t0 - a.time) / span;
            sum += span * ((x1 - x0) * va + (vb - va) * (curveArea(a.curve, x1) - curveArea(a.curve, x0)));
        }
        at = t1;
        if (u <= t1) return sign * sum;
    }
    // after the last point: the last point's speed
    sum += (u - at) * clampVelocity(pts.back().value);
    return sign * sum;
}

const char* const kSlowMotionId = "com.vegascreativesoftware:slowmotion";
const char* const kTimeWarpId = "de.magix:TimeWarp";        // VEGAS Pro 18's name for the same effect

// VEGAS Pro 23's Slow Motion effect on an event: its slot (null if none, or bypassed), and the settings it holds.
static const FxSlot* slowMotionSlot(const Event& e)
{
    for (const FxSlot& s : e.fx)
        if (s.kind == "ofx" && !s.bypass && (s.pluginId == kSlowMotionId || s.pluginId == kTimeWarpId)) return &s;
    return nullptr;
}
static double slowMotionParam(const FxSlot& s, const char* name, double def)
{
    const Json v = s.state.get("params").get(name).get("v");
    return v.isArray() && v.size() > 0 ? v.at(0).asDouble(def) : def;
}
double eventFxRate(const Event& e)
{
    const FxSlot* s = slowMotionSlot(e);
    if (!s) return 1.0;
    const bool timeWarp = s->pluginId == kTimeWarpId;            // VEGAS Pro 18's version of the effect names its settings differently
    return std::min(100.0, std::max(0.001, slowMotionParam(*s, timeWarp ? "Slow Motion Ratio" : "PlaybackRate", 0.5)));
}
std::string eventFxResample(const Event& e)
{
    const FxSlot* s = slowMotionSlot(e);
    if (!s) return std::string();
    if (s->pluginId == kTimeWarpId)                                                    // Method: Morph (blends frames) or Optical Flow
        return slowMotionParam(*s, "Choice Box Algorithm", 1) < 0.5 ? "force" : "optical";
    if (slowMotionParam(*s, "Blending", 0) >= 0.5) return "off";                    // Nearest Frame
    return slowMotionParam(*s, "Quality", 1) < 0.5 ? "force" : "optical";           // Blend: low quality blends, the rest follow the motion
}

double eventMediaTime(const Event& e, const MediaItem* m, double t)
{
    // How far the media has moved on: the rate times the speed (velocity envelope) summed over the event's time.
    const double u = t - e.start;
    const double run = e.reverse ? eventVelocityIntegral(e, e.length) - eventVelocityIntegral(e, u) : eventVelocityIntegral(e, u);
    double mt = e.offset + run * e.rate * eventFxRate(e);
    const double dur = (m && !m->info.isStill && !m->generated) ? m->info.duration : 0.0;
    if (dur > 0) {
        if (e.loop) {
            mt = std::fmod(mt, dur);
            if (mt < 0) mt += dur;
        } else {
            mt = std::min(std::max(mt, 0.0), dur);
        }
    } else if (mt < 0) mt = 0;
    return mt;
}

int64_t timeToSample(double t, int rate)
{
    // half way rounds up; the hair keeps a time that is half way in VEGAS's 100 ns ticks from coming out just below it in double arithmetic.
    return (int64_t)std::floor(t * rate + 0.5 + 1e-7);
}

double mediaSampleTime(double seconds, const MediaItem& m)
{
    const int sr = m.info.sampleRate;
    if (sr <= 0 || m.generated || m.info.isStill) return seconds;
    return std::floor(seconds * sr + 0.5 + 1e-7) / sr;
}

double eventFade(const Event& e, double t)
{
    double f = 1.0;
    const double in = t - e.start, out = e.end() - t;
    if (e.fadeIn > 0 && in < e.fadeIn) f = std::min(f, curveShape(e.fadeInCurve, std::max(0.0, in / e.fadeIn)));
    if (e.fadeOut > 0 && out < e.fadeOut) f = std::min(f, 1.0 - curveShape(e.fadeOutCurve, 1.0 - std::max(0.0, out / e.fadeOut)));
    return f;
}

DefaultFadeTypes& defaultFadeTypes()
{
    static DefaultFadeTypes d;
    return d;
}

void setDefaultFadeCurves(Event& e, TrackType type)
{
    const int in = type == TrackType::Audio ? defaultFadeTypes().audio : defaultFadeTypes().video;
    const int mirror = in == CurveFast ? CurveSlow : in == CurveSlow ? CurveFast : in;
    e.fadeInCurve = in;
    e.fadeOutCurve = mirror;
    e.fadeInRecipCurve = mirror;
}

PanGains panGains(const std::string& type, double level, double pan)
{
    PanGains g;
    const double p = std::min(1.0, std::max(-1.0, std::isfinite(pan) ? pan : 0.0));
    const float lv = (float)level;
    if (type == "add") {
        // Add Channels: the side panned away from is mixed into the other (linearly), so that fully right both
        // channels play on the right at full level (0 dB in the centre)
        if (p >= 0) { g.ll = (float)(1 - p); g.lr = (float)p; g.rl = 0; g.rr = 1; }
        else { g.rr = (float)(1 + p); g.rl = (float)-p; g.lr = 0; g.ll = 1; }
    } else if (type == "power" || type == "film") {
        // Constant Power: a quarter circle, so the two sides' power adds up to the same everywhere (-3 dB each in the
        // centre).
        const double th = (p + 1) * 0.25 * 3.14159265358979323846;
        g.ll = (float)std::cos(th);
        g.rr = (float)std::sin(th);
        g.rl = g.lr = 0;
    } else {
        // Balance: the side panned towards rises (linearly) from the centre's level to 0 dB, the other falls from it
        // to silence; the centre's level is 0, -3 or -6 dB
        const double c = type == "balance3" ? 0.70794578438413791 : type == "balance6" ? 0.50118723362727224 : 1.0;
        const double toward = c + (1 - c) * std::fabs(p), away = c * (1 - std::fabs(p));
        g.ll = (float)(p > 0 ? away : p < 0 ? toward : c);
        g.rr = (float)(p < 0 ? away : p > 0 ? toward : c);
        g.rl = g.lr = 0;
    }
    g.ll *= lv; g.rl *= lv; g.lr *= lv; g.rr *= lv;
    return g;
}

// These are VEGAS's own formulas, read from VEGAS Pro 18's EnvelopeCurve_GetValueAt.
double curveShape(int curve, double x)
{
    x = std::min(1.0, std::max(0.0, x));
    switch (curve) {
    case CurveFast: return 1.0 - (1.0 - x) * (1.0 - x);            // changes quickly at first
    case CurveSlow: return x * x;                                   // changes slowly at first
    case CurveSmooth: return x * x * (3.0 - 2.0 * x);               // eases in and out
    case CurveSharp: return x * (3.0 + x * (4.0 * x - 6.0));        // 4x^3 - 6x^2 + 3x: quick at both ends, level in the middle
    case CurveHold: return x >= 1.0 ? 1.0 : 0.0;
    default: return x;
    }
}

double keyCurveShape(int keyCurve, double s)
{
    s = std::min(1.0, std::max(0.0, s));
    switch (keyCurve) {
    case KeyHold: return s >= 1.0 ? 1.0 : 0.0;
    case KeySmooth: return curveShape(CurveSmooth, s);
    case KeyFast: return curveShape(CurveFast, s);
    case KeySlow: return curveShape(CurveSlow, s);
    case KeySharp: return curveShape(CurveSharp, s);
    default: return s;
    }
}

// the two orders differ only in where smooth and slow are: VEGAS 2 slow / 4 smooth, Reno 2 smooth / 4 slow.
int keyCurveFromVegas(int t)
{
    switch (t) {
    case 1: return KeyHold;
    case 2: return KeySlow;
    case 3: return KeyFast;
    case 4: return KeySmooth;
    case 5: return KeySharp;
    default: return KeyLinear;
    }
}
int keyCurveToVegas(int c)
{
    switch (c) {
    case KeyHold: return 1;
    case KeySlow: return 2;
    case KeyFast: return 3;
    case KeySmooth: return 4;
    case KeySharp: return 5;
    default: return 0;
    }
}

void smoothPath(double smoothness, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, double f,
                double& x, double& y)
{
    x = x1 + (x2 - x1) * f;
    y = y1 + (y2 - y1) * f;
    const double s = std::min(1.0, std::max(0.0, smoothness / 100.0));
    if (!(s > 0) || !std::isfinite(s)) return;
    const double f2 = f * f, f3 = f2 * f;
    auto cr = [&](double p0, double p1, double p2, double p3) {
        return 0.5 * (2 * p1 + (-p0 + p2) * f + (2 * p0 - 5 * p1 + 4 * p2 - p3) * f2 + (-p0 + 3 * p1 - 3 * p2 + p3) * f3);
    };
    const double cx = cr(x0, x1, x2, x3), cy = cr(y0, y1, y2, y3);
    if (!std::isfinite(cx) || !std::isfinite(cy)) return;
    x += (cx - x) * s;
    y += (cy - y) * s;
}

double Envelope::valueAt(double t, double whenEmpty) const
{
    if (points.empty()) return whenEmpty;
    if (t <= points.front().time) return points.front().value;
    if (t >= points.back().time) return points.back().value;
    // the last point at or before t
    size_t lo = 0, hi = points.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (points[mid].time <= t) lo = mid; else hi = mid;
    }
    const EnvPoint& a = points[lo];
    const EnvPoint& b = points[lo + 1];
    const double span = b.time - a.time;
    if (!(span > 0)) return b.value;
    const int c = kind == "mute" ? CurveHold : a.curve;
    return a.value + (b.value - a.value) * curveShape(c, (t - a.time) / span);
}

const Envelope* findEnvelope(const Envelopes& list, const std::string& kind, uint64_t target)
{
    for (const auto& e : list)
        if (e.kind == kind && e.target == target) return &e;
    return nullptr;
}

Envelope* findEnvelope(Envelopes& list, const std::string& kind, uint64_t target)
{
    for (auto& e : list)
        if (e.kind == kind && e.target == target) return &e;
    return nullptr;
}

double envelopeValue(const Envelopes& list, const std::string& kind, double t, double whenNone, uint64_t target)
{
    const Envelope* e = findEnvelope(list, kind, target);
    return e ? e->valueAt(t, whenNone) : whenNone;
}

// A track's automated setting at t: its envelope of that kind, unless it has none (with points) or the track's
// automation is Off - then the automation value (Track::autoLevel...).
const Envelope* trackEnvelope(const Track& tr, const std::string& kind, uint64_t target)
{
    if (tr.automationOff()) return nullptr;
    const Envelope* e = findEnvelope(tr.envelopes, kind, target);
    return (e && !e->points.empty()) ? e : nullptr;
}

double trackAutomationValue(const Track& tr, const std::string& kind, double t)
{
    double whenNone = 0.0;
    if (kind == "volume" || kind == "composite") whenNone = tr.autoLevel;
    else if (kind == "pan") whenNone = tr.autoPan;
    else if (kind == "fadecolor") whenNone = tr.autoFade;
    else if (kind == "mute") whenNone = tr.autoMute ? 1.0 : 0.0;
    const Envelope* e = trackEnvelope(tr, kind);
    return e ? e->valueAt(t, whenNone) : whenNone;
}

// ------------------------------------------------------------------------------------------ JSON
static Json fxToJson(const FxSlot& s)
{
    Json j = Json::object();
    j["id"] = s.id;
    j["kind"] = s.kind;
    j["name"] = s.name;
    if (s.kind == "ofx" || s.kind == "builtin") j["plugin"] = s.pluginId;
    else j["audio"] = s.audio.toJson();
    j["bypass"] = s.bypass;
    if (s.beforePanCrop) j["beforePanCrop"] = true;
    if (s.afterComposite) j["afterComposite"] = true;
    if (s.bypassAutomation) j["bypassAutomation"] = true;
    j["state"] = s.state;
    return j;
}

static FxSlot fxFromJson(const Json& j)
{
    FxSlot s;
    s.id = j.get("id").asUInt64();
    s.kind = j.get("kind").asString();
    s.name = j.get("name").asString();
    s.pluginId = j.get("plugin").asString();
    if (j.has("audio")) s.audio = AudioPluginDesc::fromJson(j.get("audio"));
    s.bypass = j.get("bypass").asBool();
    s.beforePanCrop = j.get("beforePanCrop").asBool();
    s.afterComposite = j.get("afterComposite").asBool();
    s.bypassAutomation = j.get("bypassAutomation").asBool();
    s.state = j.get("state");
    return s;
}

static Json chainToJson(const FxChain& c)
{
    Json a = Json::array();
    for (const auto& s : c) a.push(fxToJson(s));
    return a;
}

static FxChain chainFromJson(const Json& j)
{
    FxChain c;
    for (const auto& s : j.items()) c.push_back(fxFromJson(s));
    return c;
}

static Json panCropToJson(const std::vector<PanCropKey>& ks)
{
    Json a = Json::array();
    for (const auto& k : ks) {
        Json j = Json::array();
        for (double v : {k.time, k.cx, k.cy, k.w, k.h, k.rotation, (double)k.curve}) j.push(v);
        if (k.smoothness != 0 || k.pivotX != 0 || k.pivotY != 0)
            for (double v : {k.smoothness, k.pivotX, k.pivotY}) j.push(v);
        a.push(j);
    }
    return a;
}
static std::vector<PanCropKey> panCropFromJson(const Json& a)
{
    std::vector<PanCropKey> ks;
    for (const auto& j : a.items()) {
        PanCropKey k;
        k.time = j.at(0).asDouble(); k.cx = j.at(1).asDouble(0.5); k.cy = j.at(2).asDouble(0.5);
        k.w = j.at(3).asDouble(1.0); k.h = j.at(4).asDouble(1.0); k.rotation = j.at(5).asDouble(); k.curve = j.at(6).asInt();
        k.smoothness = std::min(100.0, std::max(0.0, j.at(7).asDouble()));
        k.pivotX = j.at(8).asDouble();
        k.pivotY = j.at(9).asDouble();
        if (!std::isfinite(k.smoothness)) k.smoothness = 0;
        if (!std::isfinite(k.pivotX) || !std::isfinite(k.pivotY)) k.pivotX = k.pivotY = 0;
        ks.push_back(k);
    }
    return ks;
}
// ---- Event Pan/Crop's mask
std::vector<MaskPath> maskAt(const std::vector<MaskKey>& keys, double t)
{
    if (keys.empty()) return {};
    std::vector<const MaskKey*> order;
    for (const MaskKey& k : keys) order.push_back(&k);
    std::stable_sort(order.begin(), order.end(), [](const MaskKey* a, const MaskKey* b) { return a->time < b->time; });
    if (t <= order.front()->time) return order.front()->paths;
    if (t >= order.back()->time) return order.back()->paths;
    for (size_t k = 0; k + 1 < order.size(); ++k) {
        const MaskKey& a = *order[k];
        const MaskKey& b = *order[k + 1];
        if (t >= b.time) continue;
        const double span = b.time - a.time;
        double f = span > 0 ? std::min(1.0, std::max(0.0, (t - a.time) / span)) : 1.0;
        f = keyCurveShape(a.curve, f);
        std::vector<MaskPath> out = a.paths;
        if (a.paths.size() != b.paths.size()) return out;
        auto mix = [f](double x, double y) { return x + (y - x) * f; };
        for (size_t i = 0; i < out.size(); ++i) {
            const MaskPath& pb = b.paths[i];
            MaskPath& o = out[i];
            o.feather = mix(o.feather, pb.feather);
            o.opacity = mix(o.opacity, pb.opacity);
            if (o.points.size() != pb.points.size()) continue;
            for (size_t j = 0; j < o.points.size(); ++j) {
                MaskPoint& q = o.points[j];
                const MaskPoint& r = pb.points[j];
                q.x = mix(q.x, r.x); q.y = mix(q.y, r.y);
                q.inX = mix(q.inX, r.inX); q.inY = mix(q.inY, r.inY);
                q.outX = mix(q.outX, r.outX); q.outY = mix(q.outY, r.outY);
            }
        }
        return out;
    }
    return order.back()->paths;
}

bool maskActive(const Event& e)
{
    if (!e.maskOn) return false;
    for (const MaskKey& k : e.mask)
        for (const MaskPath& p : k.paths)
            if (p.enabled && p.closed && p.points.size() >= 3) return true;
    return false;
}

static Json maskToJson(const Event& e)
{
    Json j = Json::object();
    j["on"] = e.maskOn;
    if (e.maskApplyToFx) j["applyToFx"] = true;
    Json ks = Json::array();
    for (const MaskKey& k : e.mask) {
        Json jk = Json::object();
        jk["t"] = k.time;
        if (k.curve) jk["curve"] = k.curve;
        Json ps = Json::array();
        for (const MaskPath& p : k.paths) {
            Json jp = Json::object();
            Json pts = Json::array();
            for (const MaskPoint& q : p.points) {
                Json a = Json::array();
                for (double v : {q.x, q.y, q.inX, q.inY, q.outX, q.outY}) a.push(v);
                pts.push(a);
            }
            jp["points"] = pts;
            if (!p.closed) jp["open"] = true;
            if (p.negative) jp["negative"] = true;
            if (!p.enabled) jp["off"] = true;
            if (!p.antiAlias) jp["aliased"] = true;
            jp["featherType"] = p.featherType;
            jp["feather"] = p.feather;
            jp["opacity"] = p.opacity;
            ps.push(jp);
        }
        jk["paths"] = ps;
        ks.push(jk);
    }
    j["keys"] = ks;
    return j;
}
static void maskFromJson(const Json& j, Event& e)
{
    e.maskOn = j.get("on").asBool();
    e.maskApplyToFx = j.get("applyToFx").asBool();
    e.mask.clear();
    for (const auto& jk : j.get("keys").items()) {
        MaskKey k;
        k.time = jk.get("t").asDouble();
        k.curve = std::clamp((int)jk.get("curve").asInt(), 0, 5);
        for (const auto& jp : jk.get("paths").items()) {
            MaskPath p;
            for (const auto& a : jp.get("points").items()) {
                MaskPoint q;
                q.x = a.at(0).asDouble(0.5); q.y = a.at(1).asDouble(0.5);
                q.inX = a.at(2).asDouble(); q.inY = a.at(3).asDouble();
                q.outX = a.at(4).asDouble(); q.outY = a.at(5).asDouble();
                if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.inX) && std::isfinite(q.inY) && std::isfinite(q.outX) &&
                    std::isfinite(q.outY))
                    p.points.push_back(q);
            }
            p.closed = !jp.get("open").asBool();
            p.negative = jp.get("negative").asBool();
            p.enabled = !jp.get("off").asBool();
            p.antiAlias = !jp.get("aliased").asBool();
            p.featherType = std::clamp((int)jp.get("featherType").asInt(2), 0, 3);
            p.feather = std::min(100.0, std::max(0.0, jp.get("feather").asDouble()));
            p.opacity = std::min(1.0, std::max(0.0, jp.get("opacity").asDouble(1.0)));
            k.paths.push_back(p);
        }
        e.mask.push_back(k);
    }
}

static Json motionToJson(const std::vector<MotionKey>& ks)
{
    Json a = Json::array();
    for (const auto& k : ks) {
        Json j = Json::array();
        for (double v : {k.time, k.x, k.y, k.w, k.h, k.rotation, (double)k.curve}) j.push(v);
        if (k.has3d() || k.smoothness != 0)
            for (double v : {k.z, k.d, k.rotX, k.rotY, k.oriX, k.oriY, k.oriZ, k.offX, k.offY, k.offZ}) j.push(v);
        if (k.smoothness != 0) j.push(k.smoothness);
        a.push(j);
    }
    return a;
}
static std::vector<MotionKey> motionFromJson(const Json& a)
{
    std::vector<MotionKey> ks;
    for (const auto& j : a.items()) {
        MotionKey k;
        k.time = j.at(0).asDouble(); k.x = j.at(1).asDouble(0.5); k.y = j.at(2).asDouble(0.5);
        k.w = j.at(3).asDouble(1.0); k.h = j.at(4).asDouble(1.0); k.rotation = j.at(5).asDouble(); k.curve = j.at(6).asInt();
        k.z = j.at(7).asDouble(); k.d = j.at(8).asDouble(1.0); k.rotX = j.at(9).asDouble(); k.rotY = j.at(10).asDouble();
        k.oriX = j.at(11).asDouble(); k.oriY = j.at(12).asDouble(); k.oriZ = j.at(13).asDouble();
        k.offX = j.at(14).asDouble(); k.offY = j.at(15).asDouble(); k.offZ = j.at(16).asDouble();
        k.smoothness = std::min(100.0, std::max(0.0, j.at(17).asDouble()));
        if (!std::isfinite(k.smoothness)) k.smoothness = 0;
        ks.push_back(k);
    }
    return ks;
}

static Json glowToJson(const std::vector<GlowKey>& ks)
{
    Json a = Json::array();
    for (const auto& k : ks) {
        Json j = Json::array();
        for (double v : {k.time, k.x, k.y, k.w, k.h, k.blur, k.intensity, (double)k.color, (double)k.curve}) j.push(v);
        a.push(j);
    }
    return a;
}
static std::vector<GlowKey> glowFromJson(const Json& a)
{
    std::vector<GlowKey> ks;
    for (const auto& j : a.items()) {
        GlowKey k;
        k.time = j.at(0).asDouble(); k.x = j.at(1).asDouble(); k.y = j.at(2).asDouble();
        k.w = j.at(3).asDouble(1.0); k.h = j.at(4).asDouble(1.0); k.blur = j.at(5).asDouble(0.2); k.intensity = j.at(6).asDouble(0.5);
        k.color = (uint32_t)j.at(7).asDouble((double)0xFF000000u); k.curve = j.at(8).asInt();
        ks.push_back(k);
    }
    return ks;
}

static Json envelopesToJson(const Envelopes& list)
{
    Json a = Json::array();
    for (const auto& e : list) {
        Json j = Json::object();
        j["kind"] = e.kind;
        if (e.target) j["target"] = e.target;
        if (!e.param.empty()) j["param"] = e.param;
        j["visible"] = e.visible;
        j["locked"] = e.locked;
        Json pts = Json::array();
        for (const auto& p : e.points) {
            Json jp = Json::array();
            jp.push(p.time);
            jp.push(p.value);
            jp.push(p.curve);
            pts.push(jp);
        }
        j["points"] = pts;
        a.push(j);
    }
    return a;
}

static Envelopes envelopesFromJson(const Json& a)
{
    Envelopes list;
    for (const auto& j : a.items()) {
        Envelope e;
        e.kind = j.get("kind").asString();
        e.target = j.get("target").asUInt64();
        e.param = j.get("param").asString();
        e.visible = j.get("visible").asBool(true);
        e.locked = j.get("locked").asBool();
        for (const auto& jp : j.get("points").items()) {
            EnvPoint p;
            p.time = jp.at(0).asDouble();
            p.value = jp.at(1).asDouble();
            p.curve = jp.at(2).asInt();
            e.points.push_back(p);
        }
        std::stable_sort(e.points.begin(), e.points.end(), [](const EnvPoint& x, const EnvPoint& y) { return x.time < y.time; });
        if (!e.kind.empty()) list.push_back(e);
    }
    return list;
}

Json surroundToJson(const SurroundPan& s)
{
    Json j = Json::object();
    j["x"] = s.x;
    j["y"] = s.y;
    j["center"] = s.center;
    j["smoothness"] = s.smoothness;
    j["lfe"] = s.lfe;
    j["lfeLevel"] = s.lfeLevel;
    j["route"] = s.route;
    j["motion"] = s.motion;
    j["speakerMutes"] = (int)s.speakerMutes;
    if (!s.keys.empty()) {
        Json a = Json::array();
        for (const SurroundKey& k : s.keys) {
            Json jk = Json::array();
            jk.push(k.time);
            jk.push(k.x);
            jk.push(k.y);
            jk.push(k.center);
            jk.push(k.curve);
            a.push(jk);
        }
        j["keys"] = a;
    }
    return j;
}

SurroundPan surroundFromJson(const Json& j)
{
    SurroundPan s;
    if (!j.isObject()) return s;
    auto clamp = [](double v, double lo, double hi) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : lo; };
    s.x = clamp(j.get("x").asDouble(0), -1, 1);
    s.y = clamp(j.get("y").asDouble(0), -1, 1);
    s.center = clamp(j.get("center").asDouble(1), 0, 1);
    s.smoothness = clamp(j.get("smoothness").asDouble(1), 0, 1);
    s.lfe = j.get("lfe").asBool();
    s.lfeLevel = clamp(j.get("lfeLevel").asDouble(1), 0, 4);
    s.route = j.get("route").asString("surround");
    if (s.route != "front" && s.route != "center" && s.route != "lfe" && s.route != "rear") s.route = "surround";
    s.motion = j.get("motion").asString("free");
    if (s.motion != "x" && s.motion != "y") s.motion = "free";
    s.speakerMutes = (unsigned)j.get("speakerMutes").asInt(0) & 63u;
    for (const Json& jk : j.get("keys").items()) {
        if (!jk.isArray() || jk.size() < 4) continue;
        SurroundKey k;
        k.time = jk.at(0).asDouble();
        k.x = clamp(jk.at(1).asDouble(), -1, 1);
        k.y = clamp(jk.at(2).asDouble(), -1, 1);
        k.center = clamp(jk.at(3).asDouble(1), 0, 1);
        k.curve = jk.size() > 4 ? jk.at(4).asInt() : 0;
        if (std::isfinite(k.time)) s.keys.push_back(k);
    }
    std::stable_sort(s.keys.begin(), s.keys.end(), [](const SurroundKey& a, const SurroundKey& b) { return a.time < b.time; });
    return s;
}

static Json settingsToJson(const ProjectSettings& settings)
{
    Json s = Json::object();
    s["width"] = settings.width;
    s["height"] = settings.height;
    s["fpsNum"] = settings.fpsNum;
    s["fpsDen"] = settings.fpsDen;
    s["sampleRate"] = settings.sampleRate;
    s["fieldOrder"] = settings.fieldOrder;
    s["pixelAspect"] = settings.pixelAspect;
    s["pixelFormat"] = settings.pixelFormat;
    s["renderQuality"] = settings.renderQuality;
    s["resample"] = settings.resample;
    s["motionBlur"] = settings.motionBlur;
    s["deinterlace"] = settings.deinterlace;
    s["audioBits"] = settings.audioBits;
    s["rulerFormat"] = settings.rulerFormat;
    s["rulerStart"] = settings.rulerStart;
    s["tempo"] = settings.tempo;
    s["beatsPerMeasure"] = settings.beatsPerMeasure;
    s["title"] = settings.title;
    s["artist"] = settings.artist;
    s["engineer"] = settings.engineer;
    s["copyright"] = settings.copyright;
    s["comments"] = settings.comments;
    if (settings.outputRotation) s["outputRotation"] = settings.outputRotation;
    if (settings.audioChannels != 2) s["audioChannels"] = settings.audioChannels;
    if (!settings.lfeLowpass) s["lfeLowpass"] = false;
    if (settings.lfeCutoff != 120.0) s["lfeCutoff"] = settings.lfeCutoff;
    if (settings.lfeQuality != "best") s["lfeQuality"] = settings.lfeQuality;
    // VEGAS 18's other Project Properties, written where they are not the defaults.
    {
        const ProjectSettings d;
        if (settings.pixelRange != d.pixelRange) s["pixelRange"] = settings.pixelRange;
        if (settings.compositingGamma != d.compositingGamma) s["compositingGamma"] = settings.compositingGamma;
        if (settings.hdrMode != d.hdrMode) s["hdrMode"] = settings.hdrMode;
        if (settings.masterDisplay != d.masterDisplay) {
            const MasterDisplay& m = settings.masterDisplay;
            Json md = Json::object();
            md["preset"] = m.preset;
            md["redX"] = m.redX;
            md["redY"] = m.redY;
            md["greenX"] = m.greenX;
            md["greenY"] = m.greenY;
            md["blueX"] = m.blueX;
            md["blueY"] = m.blueY;
            md["whiteX"] = m.whiteX;
            md["whiteY"] = m.whiteY;
            md["maxNits"] = m.maxNits;
            md["minNits"] = m.minNits;
            md["maxCll"] = m.maxCll;
            md["maxFall"] = m.maxFall;
            s["masterDisplay"] = md;
        }
        if (settings.output360) s["output360"] = true;
        if (settings.acesVersion != d.acesVersion) s["acesVersion"] = settings.acesVersion;
        if (settings.acesColorSpace != d.acesColorSpace) s["acesColorSpace"] = settings.acesColorSpace;
        if (settings.viewTransform != d.viewTransform) s["viewTransform"] = settings.viewTransform;
        if (settings.lookTransform != d.lookTransform) s["lookTransform"] = settings.lookTransform;
        if (!settings.adjustSourceMedia) s["adjustSourceMedia"] = false;
        if (settings.resampleQuality != d.resampleQuality) s["resampleQuality"] = settings.resampleQuality;
        if (settings.noteGetsBeat != d.noteGetsBeat) s["noteGetsBeat"] = settings.noteGetsBeat;
        if (!settings.cdUpc.empty()) s["cdUpc"] = settings.cdUpc;
        if (settings.cdFirstTrack != d.cdFirstTrack) s["cdFirstTrack"] = settings.cdFirstTrack;
    }
    if (settings.stereo()) s["stereoMode"] = settings.stereoMode;
    if (settings.stereoSwap) s["stereoSwap"] = true;
    if (settings.crosstalk != 0) s["crosstalk"] = settings.crosstalk;
    if (!settings.crosstalkInRender) s["crosstalkInRender"] = false;
    return s;
}

static Json busToJson(const Bus& b)
{
    Json jb = Json::object();
    jb["id"] = b.id;
    jb["name"] = b.name;
    jb["level"] = b.level;
    jb["pan"] = b.pan;
    jb["mute"] = b.mute;
    jb["solo"] = b.solo;
    jb["output"] = b.output;
    if (b.assignable) jb["assignable"] = true;
    if (b.fxPostFader) jb["fxPostFader"] = true;
    jb["fx"] = chainToJson(b.fx);
    if (!b.envelopes.empty()) jb["envelopes"] = envelopesToJson(b.envelopes);
    if (!b.surround.isDefault()) jb["surround"] = surroundToJson(b.surround);
    if (b.automation) jb["automation"] = true;
    if (b.automationMode != "read") jb["automationMode"] = b.automationMode;
    return jb;
}

static Json markerToJson(const Project::Marker& m)
{
    Json jm = Json::object();
    jm["id"] = m.id;
    jm["time"] = m.time;
    jm["length"] = m.length;
    jm["name"] = m.name;
    jm["region"] = m.region;
    if (!m.kind.empty()) jm["kind"] = m.kind;
    if (!m.command.empty()) jm["command"] = m.command;
    if (!m.comment.empty()) jm["comment"] = m.comment;
    return jm;
}

static Json mediaItemToJson(const MediaItem& m)
{
    Json e = Json::object();
    e["id"] = m.id;
    e["path"] = m.path;
    e["name"] = m.name;
    e["generated"] = m.generated;
    if (m.generated) e["generator"] = fxToJson(m.generator);
    Json i = Json::object();
    i["video"] = m.info.hasVideo;
    i["audio"] = m.info.hasAudio;
    i["still"] = m.info.isStill;
    i["width"] = m.info.width;
    i["height"] = m.info.height;
    i["fps"] = m.info.fps;
    i["duration"] = m.info.duration;
    i["sampleRate"] = m.info.sampleRate;
    if (m.info.audioDelaySamples) i["audioDelay"] = m.info.audioDelaySamples;
    i["channels"] = m.info.channels;
    if (!m.info.channelNames.empty()) {
        Json names = Json::array();
        for (const auto& n : m.info.channelNames) names.push(Json(n));
        i["channelNames"] = names;
    }
    if (!m.info.channelLayout.empty()) i["channelLayout"] = m.info.channelLayout;
    if (std::fabs(m.info.pixelAspect - 1.0) > 1e-6) i["pixelAspect"] = m.info.pixelAspect;      // 
    if (m.info.rotation) i["rotation"] = m.info.rotation;
    if (m.info.fieldOrder != "none") i["fieldOrder"] = m.info.fieldOrder;
    e["info"] = i;
    if (!m.fieldOrderOverride.empty()) e["fieldOrderOverride"] = m.fieldOrderOverride;
    if (m.pixelAspectOverride > 0) e["pixelAspectOverride"] = m.pixelAspectOverride;
    if (m.rotationOverride) e["rotationOverride"] = m.rotationOverride;
    if (!m.bin.empty()) e["bin"] = m.bin;
    if (!m.tags.empty()) {
        Json tags = Json::array();
        for (const auto& t : m.tags) tags.push(Json(t));
        e["tags"] = tags;
    }
    if (!m.comment.empty()) e["comment"] = m.comment;
    if (m.sequenceFps > 0) e["sequenceFps"] = m.sequenceFps;
    if (!m.alphaMode.empty()) e["alphaMode"] = m.alphaMode;
    if (!m.stereoMode.empty() && m.stereoMode != "off") e["stereoMode"] = m.stereoMode;
    if (m.stereoSwap) e["stereoSwap"] = true;
    if (m.stereoPair) e["stereoPair"] = m.stereoPair;
    if (!m.colorSpace.empty()) e["colorSpace"] = m.colorSpace;
    if (m.subclipLength > 0) {
        e["subclipStart"] = m.subclipStart;
        e["subclipLength"] = m.subclipLength;
    }
    if (!m.redEye.empty()) {
        Json re = Json::array();
        for (const MediaItem::RedEyeArea& a : m.redEye) {
            Json one = Json::array();
            for (double v : {a.x, a.y, a.w, a.h}) one.push(v);
            re.push(one);
        }
        e["redEye"] = re;
    }
    if (!m.markers.empty()) {
        Json mk = Json::array();
        for (const MediaItem::Marker& k : m.markers) {
            Json one = Json::array();
            one.push(k.time);
            one.push(k.length);
            one.push(k.name);
            mk.push(one);
        }
        e["markers"] = mk;
    }
    return e;
}

// One event as Reno's project file stores it.
static Json eventToJson(const Event& e)
{
    Json je = Json::object();
    je["id"] = e.id;
    je["media"] = e.mediaId;
    je["start"] = e.start;
    je["length"] = e.length;
    je["offset"] = e.offset;
    je["rate"] = e.rate;
    je["fadeIn"] = e.fadeIn;
    je["fadeOut"] = e.fadeOut;
    je["gain"] = e.gain;
    je["mute"] = e.mute;
    je["loop"] = e.loop;
    je["group"] = e.group;
    je["name"] = e.name;
    je["reverse"] = e.reverse;
    je["pitch"] = e.pitch;
    je["keepPitch"] = e.keepPitch;
    je["stretchMethod"] = e.stretchMethod;
    je["stretchMode"] = e.stretchMode;
    if (e.formant != 0) je["formant"] = e.formant;
    if (e.preserveFormants) je["preserveFormants"] = true;
    if (e.invertPhase) je["invertPhase"] = true;
    if (e.snapOffset != 0) je["snapOffset"] = e.snapOffset;
    if (e.adjustment) je["adjustment"] = true;
    if (e.syncLink) je["syncLink"] = e.syncLink;
    je["keepAspect"] = e.keepAspect;
    if (e.stretchToFill) je["stretchToFill"] = true;
    if (e.autoNormalize) je["autoNormalize"] = true;
    if (e.normalize) {
        je["normalize"] = true;
        je["normalizeGain"] = e.normalizeGain;
    }
    if (e.reduceFlicker) je["reduceFlicker"] = true;
    if (!e.resample.empty() && e.resample != "project") je["resample"] = e.resample;
    je["fadeInCurve"] = e.fadeInCurve;
    je["fadeOutCurve"] = e.fadeOutCurve;
    if (e.fadeInRecipCurve >= 0) je["fadeInRecipCurve"] = e.fadeInRecipCurve;
    je["channels"] = e.channels;
    if (e.mediaChannel >= 0) {
        je["mediaChannel"] = e.mediaChannel;
        je["mediaChannels"] = e.mediaChannels;
    }
    je["undersample"] = e.undersample;
    if (e.locked) je["locked"] = true;
    if (!e.takes.empty()) {
        Json tk = Json::array();
        for (const auto& k : e.takes) {
            Json jk = Json::object();
            jk["media"] = k.mediaId;
            jk["offset"] = k.offset;
            jk["name"] = k.name;
            if (k.reverse) jk["reverse"] = true;
            tk.push(jk);
        }
        je["takes"] = tk;
        if (e.activeTake) je["activeTake"] = e.activeTake;
    }
    if (!e.envelopes.empty()) je["envelopes"] = envelopesToJson(e.envelopes);
    if (!e.panCrop.empty()) je["panCrop"] = panCropToJson(e.panCrop);
    if (e.maskOn || !e.mask.empty()) je["mask"] = maskToJson(e);
    je["fx"] = chainToJson(e.fx);
    if (!e.transition.kind.empty()) je["transition"] = fxToJson(e.transition);
    return je;
}

// One track, with its events, as Reno's project file stores it.
static Json trackToJson(const Track& t)
{
    Json jt = Json::object();
    jt["id"] = t.id;
    jt["type"] = t.type == TrackType::Video ? "video" : "audio";
    jt["name"] = t.name;
    jt["mute"] = t.mute;
    jt["solo"] = t.solo;
    jt["level"] = t.level;
    jt["pan"] = t.pan;
    if (!t.panType.empty()) jt["panType"] = t.panType;
    if (!t.surround.isDefault()) jt["surround"] = surroundToJson(t.surround);
    jt["fx"] = chainToJson(t.fx);
    jt["output"] = t.output;
    Json sends = Json::array();
    for (const auto& sd : t.sends) {
        Json js = Json::object();
        js["bus"] = sd.bus;
        js["level"] = sd.level;
        js["preFader"] = sd.preFader;
        sends.push(js);
    }
    jt["sends"] = sends;
    if (!t.motion.empty()) jt["motion"] = motionToJson(t.motion);
    jt["composite"] = t.composite;
    jt["color"] = (double)t.color;
    jt["height"] = t.height;
    if (!t.envelopes.empty()) jt["envelopes"] = envelopesToJson(t.envelopes);
    jt["fadeTop"] = (double)t.fadeTop;
    jt["fadeBottom"] = (double)t.fadeBottom;
    jt["compositeLevel"] = t.compositeLevel;
    if (t.trackGroup) jt["trackGroup"] = t.trackGroup;
    if (t.adjustment) jt["adjustment"] = true;
    if (t.parentComposite != "srcalpha") jt["parentComposite"] = t.parentComposite;
    if (!t.parentMotion.empty()) jt["parentMotion"] = motionToJson(t.parentMotion);
    if (t.minimized) jt["minimized"] = true;
    if (t.layersExpanded) jt["layersExpanded"] = true;
    if (t.bypassMotionBlur) jt["bypassMotionBlur"] = true;
    if (t.invertPhase) jt["invertPhase"] = true;
    if (t.automation) jt["automation"] = true;
    if (t.automationMode != "read") jt["automationMode"] = t.automationMode;
    if (t.autoLevel != 1.0) jt["autoLevel"] = t.autoLevel;
    if (t.autoPan != 0.0) jt["autoPan"] = t.autoPan;
    if (t.autoFade != 0.0) jt["autoFade"] = t.autoFade;
    if (t.autoMute) jt["autoMute"] = true;
    if (t.duckController) jt["duckController"] = true;
    if (t.duckListener) jt["duckListener"] = true;
    if (t.duckFloor != -12.0) jt["duckFloor"] = t.duckFloor;
    if (t.duckPeak != 0) jt["duckPeak"] = t.duckPeak;
    if (!t.shadow2d.empty()) jt["shadow2d"] = glowToJson(t.shadow2d);
    if (!t.glow2d.empty()) jt["glow2d"] = glowToJson(t.glow2d);
    Json es = Json::array();
    for (const auto& e : t.events) es.push(eventToJson(e));
    jt["events"] = es;
    return jt;
}

Json Project::toJson() const
{
    Json j = Json::object();
    j["reno"] = 1;
    j["settings"] = settingsToJson(settings);
    j["masterVolume"] = masterVolume;
    j["masterPan"] = masterPan;
    j["masterMute"] = masterMute;
    if (masterFxPostFader) j["masterFxPostFader"] = true;
    Json bs = Json::array();
    for (const auto& b : buses) bs.push(busToJson(b));
    j["buses"] = bs;
    if (!masterEnvelopes.empty()) j["masterEnvelopes"] = envelopesToJson(masterEnvelopes);
    if (!videoEnvelopes.empty()) j["videoEnvelopes"] = envelopesToJson(videoEnvelopes);
    j["videoFadeTop"] = (double)videoFadeTop;
    j["videoFadeBottom"] = (double)videoFadeBottom;
    Json mk = Json::array();
    for (const auto& m : markers) mk.push(markerToJson(m));
    j["markers"] = mk;
    j["nextId"] = nextId;
    Json ms = Json::array();
    for (const auto& m : media) ms.push(mediaItemToJson(m));
    j["media"] = ms;
    if (!mediaBins.empty()) {
        Json bins = Json::array();
        for (const auto& b : mediaBins) bins.push(Json(b));
        j["mediaBins"] = bins;
    }
    if (!smartBins.empty()) {
        Json bins = Json::array();
        for (const auto& b : smartBins) {
            Json o = Json::object();
            o["name"] = b.first;
            o["search"] = b.second;
            bins.push(o);
        }
        j["smartBins"] = bins;
    }
    Json ts = Json::array();
    for (const auto& t : tracks) ts.push(trackToJson(t));
    j["tracks"] = ts;
    j["videoOut"] = chainToJson(videoOut);
    j["audioOut"] = chainToJson(audioOut);
    return j;
}

// Reads the project settings; every value is kept in a sane range, whatever a hand-edited file says.
static void settingsFromJson(const Json& s, ProjectSettings& st)
{
    st.width = std::max(16, s.get("width").asInt(1920));
    st.height = std::max(16, s.get("height").asInt(1080));
    st.fpsNum = std::max(1, s.get("fpsNum").asInt(30));
    st.fpsDen = std::max(1, s.get("fpsDen").asInt(1));
    st.sampleRate = std::max(8000, s.get("sampleRate").asInt(48000));
    st.fieldOrder = s.get("fieldOrder").asString(st.fieldOrder);
    st.pixelAspect = s.get("pixelAspect").asDouble(1.0);
    st.pixelFormat = s.get("pixelFormat").asString(st.pixelFormat);
    // (an old key) "32bitfull" was 32-bit floating point (full range) composited in linear light
    const bool oldFull = st.pixelFormat == "32bitfull";
    if (oldFull) st.pixelFormat = "32bit";
    st.renderQuality = s.get("renderQuality").asString(st.renderQuality);
    st.resample = s.get("resample").asString(st.resample);
    st.motionBlur = s.get("motionBlur").asString(st.motionBlur);
    st.deinterlace = s.get("deinterlace").asString(st.deinterlace);
    //
    st.pixelRange = s.get("pixelRange").asString(st.pixelRange) == "video" ? "video" : "full";
    st.compositingGamma = s.get("compositingGamma").asDouble(st.compositingGamma);
    st.hdrMode = s.get("hdrMode").asString(st.hdrMode);
    if (s.has("masterDisplay")) {
        const Json& md = s.get("masterDisplay");
        MasterDisplay& m = st.masterDisplay;
        auto unit = [&md](const char* k, double def) { return std::max(0.0, std::min(1.0, md.get(k).asDouble(def))); };
        m.preset = md.get("preset").asString(m.preset);
        m.redX = unit("redX", m.redX);
        m.redY = unit("redY", m.redY);
        m.greenX = unit("greenX", m.greenX);
        m.greenY = unit("greenY", m.greenY);
        m.blueX = unit("blueX", m.blueX);
        m.blueY = unit("blueY", m.blueY);
        m.whiteX = unit("whiteX", m.whiteX);
        m.whiteY = unit("whiteY", m.whiteY);
        m.maxNits = std::max(0.0001, std::min(10000.0, md.get("maxNits").asDouble(m.maxNits)));
        m.minNits = std::max(0.0, std::min(m.maxNits, md.get("minNits").asDouble(m.minNits)));
        m.maxCll = std::clamp((int)md.get("maxCll").asInt(m.maxCll), 0, 65535);
        m.maxFall = std::clamp((int)md.get("maxFall").asInt(m.maxFall), 0, 65535);
    }
    st.output360 = s.get("output360").asBool(st.output360);
    st.acesVersion = s.get("acesVersion").asString(st.acesVersion);
    st.acesColorSpace = s.get("acesColorSpace").asString(st.acesColorSpace);
    st.viewTransform = s.get("viewTransform").asString(st.viewTransform);
    st.lookTransform = s.get("lookTransform").asString(st.lookTransform);
    if (st.acesVersion != "0.7" && st.acesVersion != "1.0" && st.acesVersion != "1.1") st.acesVersion = "1.1";
    if (oldFull) {
        st.pixelRange = "full";
        st.compositingGamma = 1.0;
    }
    st.adjustSourceMedia = s.get("adjustSourceMedia").asBool(st.adjustSourceMedia);
    st.resampleQuality = s.get("resampleQuality").asString(st.resampleQuality);
    st.noteGetsBeat = s.get("noteGetsBeat").asInt(st.noteGetsBeat);
    if (st.noteGetsBeat < 1 || st.noteGetsBeat > 32) st.noteGetsBeat = 4;
    st.cdUpc = s.get("cdUpc").asString(st.cdUpc);
    st.cdFirstTrack = std::max(1, std::min(99, s.get("cdFirstTrack").asInt(st.cdFirstTrack)));
    st.audioBits = s.get("audioBits").asInt(16);
    st.rulerFormat = s.get("rulerFormat").asString(st.rulerFormat);
    st.rulerStart = s.get("rulerStart").asDouble(0);
    st.tempo = s.get("tempo").asDouble(120);
    st.beatsPerMeasure = s.get("beatsPerMeasure").asInt(4);
    st.title = s.get("title").asString();
    st.artist = s.get("artist").asString();
    st.engineer = s.get("engineer").asString();
    st.copyright = s.get("copyright").asString();
    st.comments = s.get("comments").asString();
    st.outputRotation = s.get("outputRotation").asInt(0);
    st.audioChannels = s.get("audioChannels").asInt(2) == 6 ? 6 : 2;
    st.lfeLowpass = s.get("lfeLowpass").asBool(true);
    st.lfeCutoff = std::min(20000.0, std::max(10.0, s.get("lfeCutoff").asDouble(120.0)));
    st.lfeQuality = s.get("lfeQuality").asString("best");
    st.stereoMode = s.get("stereoMode").asString("off");
    st.stereoSwap = s.get("stereoSwap").asBool();
    st.crosstalk = std::min(1.0, std::max(0.0, s.get("crosstalk").asDouble(0.0)));
    st.crosstalkInRender = s.get("crosstalkInRender").asBool(true);
}

static Bus busFromJson(const Json& jb)
{
    Bus b;
    b.id = jb.get("id").asUInt64();
    b.name = jb.get("name").asString();
    b.level = jb.get("level").asDouble(1.0);
    b.pan = jb.get("pan").asDouble(0);
    b.mute = jb.get("mute").asBool();
    b.solo = jb.get("solo").asBool();
    b.output = jb.get("output").asUInt64();
    b.assignable = jb.get("assignable").asBool();
    b.fxPostFader = jb.get("fxPostFader").asBool();
    b.fx = chainFromJson(jb.get("fx"));
    b.envelopes = envelopesFromJson(jb.get("envelopes"));
    b.surround = surroundFromJson(jb.get("surround"));
    b.automation = jb.get("automationControls").asBool() || jb.get("automation").asBool();
    b.automationMode = jb.get("automationMode").asString("read");
    if (b.automationMode != "off" && b.automationMode != "touch" && b.automationMode != "latch") b.automationMode = "read";
    return b;
}

static Project::Marker markerFromJson(const Json& jm)
{
    Project::Marker m;
    m.id = jm.get("id").asUInt64();
    m.time = jm.get("time").asDouble();
    m.length = jm.get("length").asDouble();
    m.name = jm.get("name").asString();
    m.region = jm.get("region").asBool();
    m.kind = jm.get("kind").asString();
    m.command = jm.get("command").asString();
    m.comment = jm.get("comment").asString();
    return m;
}

static MediaItem mediaItemFromJson(const Json& e)
{
    MediaItem m;
    m.id = e.get("id").asUInt64();
    m.path = e.get("path").asString();
    m.name = e.get("name").asString();
    m.generated = e.get("generated").asBool();
    if (m.generated) m.generator = fxFromJson(e.get("generator"));
    const Json& i = e.get("info");
    m.info.path = m.path;
    m.info.hasVideo = i.get("video").asBool();
    m.info.hasAudio = i.get("audio").asBool();
    m.info.isStill = i.get("still").asBool();
    m.info.width = i.get("width").asInt();
    m.info.height = i.get("height").asInt();
    m.info.fps = i.get("fps").asDouble();
    m.info.duration = i.get("duration").asDouble();
    m.info.sampleRate = i.get("sampleRate").asInt();
    m.info.audioDelaySamples = i.get("audioDelay").asInt();
    m.info.channels = i.get("channels").asInt();
    for (const auto& n : i.get("channelNames").items()) m.info.channelNames.push_back(n.asString());
    m.info.channelLayout = i.get("channelLayout").asString();
    m.info.pixelAspect = i.get("pixelAspect").asDouble(1.0);
    if (!(m.info.pixelAspect > 0.01 && m.info.pixelAspect < 100.0)) m.info.pixelAspect = 1.0;
    m.info.rotation = i.get("rotation").asInt();
    m.info.fieldOrder = i.get("fieldOrder").asString("none");
    if (m.info.fieldOrder != "upper" && m.info.fieldOrder != "lower") m.info.fieldOrder = "none";
    m.fieldOrderOverride = e.get("fieldOrderOverride").asString();
    if (m.fieldOrderOverride != "none" && m.fieldOrderOverride != "upper" && m.fieldOrderOverride != "lower") m.fieldOrderOverride.clear();
    m.pixelAspectOverride = e.get("pixelAspectOverride").asDouble(0);
    if (!(m.pixelAspectOverride > 0.01 && m.pixelAspectOverride < 100.0)) m.pixelAspectOverride = 0;
    m.rotationOverride = e.get("rotationOverride").asInt();
    if (m.rotationOverride != 90 && m.rotationOverride != 180 && m.rotationOverride != 270) m.rotationOverride = 0;
    m.bin = e.get("bin").asString();
    for (const auto& t : e.get("tags").items()) m.tags.push_back(t.asString());
    m.comment = e.get("comment").asString();
    m.sequenceFps = std::max(0.0, std::min(1000.0, e.get("sequenceFps").asDouble(0)));
    m.alphaMode = e.get("alphaMode").asString();
    m.stereoMode = e.get("stereoMode").asString("off");
    m.stereoSwap = e.get("stereoSwap").asBool();
    m.stereoPair = e.get("stereoPair").asUInt64();
    m.colorSpace = e.get("colorSpace").asString();
    if (m.alphaMode != "none" && m.alphaMode != "straight" && m.alphaMode != "premultiplied") m.alphaMode.clear();
    m.subclipStart = std::max(0.0, e.get("subclipStart").asDouble(0));
    m.subclipLength = std::max(0.0, e.get("subclipLength").asDouble(0));
    for (const auto& a : e.get("redEye").items()) {
        if (a.size() < 4) continue;
        MediaItem::RedEyeArea r;
        r.x = a.at(0).asDouble();
        r.y = a.at(1).asDouble();
        r.w = a.at(2).asDouble();
        r.h = a.at(3).asDouble();
        if (r.w > 0 && r.h > 0 && r.w <= 2 && r.h <= 2 && std::fabs(r.x) <= 2 && std::fabs(r.y) <= 2) m.redEye.push_back(r);
    }
    for (const auto& a : e.get("markers").items()) {
        MediaItem::Marker k;
        k.time = a.at(0).asDouble();
        k.length = a.at(1).asDouble();
        k.name = a.at(2).asString();
        if (std::isfinite(k.time) && std::isfinite(k.length) && k.time >= 0 && k.length >= 0) m.markers.push_back(k);
    }
    return m;
}

static Event eventFromJson(const Json& je)
{
    Event e;
    e.id = je.get("id").asUInt64();
    e.mediaId = je.get("media").asUInt64();
    e.start = je.get("start").asDouble();
    e.length = std::max(0.0, je.get("length").asDouble());
    e.offset = je.get("offset").asDouble();
    e.rate = je.get("rate").asDouble(1.0);
    if (!(e.rate > 0.001)) e.rate = 1.0;
    e.fadeIn = je.get("fadeIn").asDouble();
    e.fadeOut = je.get("fadeOut").asDouble();
    e.gain = je.get("gain").asDouble(1.0);
    e.mute = je.get("mute").asBool();
    e.loop = je.get("loop").asBool(true);
    e.group = je.get("group").asUInt64();
    e.name = je.get("name").asString();
    e.reverse = je.get("reverse").asBool();
    e.pitch = je.get("pitch").asDouble(0);
    e.keepPitch = je.get("keepPitch").asBool();
    e.stretchMethod = je.get("stretchMethod").asString("elastique");
    e.stretchMode = je.get("stretchMode").asString("pro");
    e.formant = je.get("formant").asDouble(0);
    e.preserveFormants = je.get("preserveFormants").asBool();
    e.invertPhase = je.get("invertPhase").asBool();
    e.snapOffset = std::max(0.0, je.get("snapOffset").asDouble(0.0));
    e.adjustment = je.get("adjustment").asBool();
    e.syncLink = je.get("syncLink").asUInt64();
    e.keepAspect = je.get("keepAspect").asBool(true);
    e.stretchToFill = je.get("stretchToFill").asBool(false);
    e.normalize = je.get("normalize").asBool();
    e.autoNormalize = je.get("autoNormalize").asBool();
    e.normalizeGain = je.get("normalizeGain").asDouble(1.0);
    if (!(e.normalizeGain > 0 && e.normalizeGain < 1e4)) e.normalizeGain = 1.0;
    e.reduceFlicker = je.get("reduceFlicker").asBool();
    e.resample = je.get("resample").asString("project");
    if (e.resample != "force" && e.resample != "off" && e.resample != "optical") e.resample = "project";
    e.fadeInCurve = je.get("fadeInCurve").asInt(CurveLinear);
    e.fadeOutCurve = je.get("fadeOutCurve").asInt(CurveLinear);
    e.fadeInRecipCurve = std::min((int)CurveHold, std::max(-1, je.get("fadeInRecipCurve").asInt(-1)));
    e.channels = je.get("channels").asString("both");
    e.mediaChannel = std::max(-1, std::min(63, je.get("mediaChannel").asInt(-1)));
    e.mediaChannels = e.mediaChannel < 0 ? 0 : (je.get("mediaChannels").asInt(1) >= 2 ? 2 : 1);
    e.undersample = je.get("undersample").asDouble(1.0);
    e.locked = je.get("locked").asBool();
    for (const auto& jk : je.get("takes").items()) {
        Take k;
        k.mediaId = jk.get("media").asUInt64();
        k.offset = jk.get("offset").asDouble();
        k.name = jk.get("name").asString();
        k.reverse = jk.get("reverse").asBool();
        e.takes.push_back(k);
    }
    e.activeTake = std::max(0, std::min((int)je.get("activeTake").asInt(), (int)e.takes.size()));
    if (!(e.undersample > 0.01 && e.undersample <= 1.0)) e.undersample = 1.0;
    e.envelopes = envelopesFromJson(je.get("envelopes"));
    e.panCrop = panCropFromJson(je.get("panCrop"));
    if (je.has("mask")) maskFromJson(je.get("mask"), e);
    e.fx = chainFromJson(je.get("fx"));
    if (je.has("transition")) e.transition = fxFromJson(je.get("transition"));
    return e;
}

static Track trackFromJson(const Json& jt)
{
    Track t;
    t.id = jt.get("id").asUInt64();
    t.type = jt.get("type").asString() == "audio" ? TrackType::Audio : TrackType::Video;
    t.name = jt.get("name").asString();
    t.mute = jt.get("mute").asBool();
    t.solo = jt.get("solo").asBool();
    t.level = jt.get("level").asDouble(1.0);
    t.pan = jt.get("pan").asDouble(0.0);
    t.panType = jt.get("panType").asString("");
    t.surround = surroundFromJson(jt.get("surround"));
    t.fx = chainFromJson(jt.get("fx"));
    t.output = jt.get("output").asUInt64();
    for (const auto& js : jt.get("sends").items()) {
        Send sd;
        sd.bus = js.get("bus").asUInt64();
        sd.level = js.get("level").asDouble();
        sd.preFader = js.get("preFader").asBool();
        t.sends.push_back(sd);
    }
    t.motion = motionFromJson(jt.get("motion"));
    t.composite = jt.get("composite").asString("srcalpha");
    t.color = (uint32_t)jt.get("color").asDouble(0);
    t.height = jt.get("height").asInt(0);
    t.envelopes = envelopesFromJson(jt.get("envelopes"));
    t.fadeTop = (uint32_t)jt.get("fadeTop").asDouble((double)0xFFFFFFFFu);
    t.fadeBottom = (uint32_t)jt.get("fadeBottom").asDouble((double)0xFF000000u);
    t.compositeLevel = jt.get("compositeLevel").asInt(0);
    t.trackGroup = jt.get("trackGroup").asUInt64();
    t.adjustment = jt.get("adjustment").asBool();
    t.parentComposite = jt.get("parentComposite").asString("srcalpha");
    t.parentMotion = motionFromJson(jt.get("parentMotion"));
    t.minimized = jt.get("minimized").asBool();
    t.layersExpanded = jt.get("layersExpanded").asBool();
    t.bypassMotionBlur = jt.get("bypassMotionBlur").asBool();
    t.invertPhase = jt.get("invertPhase").asBool();
    t.automation = jt.get("automation").asBool() || jt.get("automationControls").asBool();
    t.automationMode = jt.get("automationMode").asString("read");
    if (t.automationMode != "off" && t.automationMode != "touch" && t.automationMode != "latch") t.automationMode = "read";
    t.autoLevel = jt.get("autoLevel").asDouble(1.0);
    t.autoPan = jt.get("autoPan").asDouble(0.0);
    t.autoFade = jt.get("autoFade").asDouble(0.0);
    t.autoMute = jt.get("autoMute").asBool();
    t.duckController = jt.get("duckController").asBool();
    t.duckListener = jt.get("duckListener").asBool();
    t.duckFloor = jt.get("duckFloor").asDouble(-12.0);
    t.duckPeak = jt.get("duckPeak").asDouble(0);
    t.shadow2d = glowFromJson(jt.get("shadow2d"));
    t.glow2d = glowFromJson(jt.get("glow2d"));
    for (const auto& je : jt.get("events").items()) t.events.push_back(eventFromJson(je));
    t.sortEvents();
    return t;
}

bool Project::fromJson(const Json& j, std::string* error)
{
    if (!j.isObject() || !j.has("tracks")) {
        if (error) *error = "this is not a Reno project";
        return false;
    }
    Project p;
    settingsFromJson(j.get("settings"), p.settings);
    p.masterVolume = j.get("masterVolume").asDouble(1.0);
    p.masterPan = j.get("masterPan").asDouble(0);
    p.masterMute = j.get("masterMute").asBool();
    p.masterFxPostFader = j.get("masterFxPostFader").asBool();
    for (const auto& jb : j.get("buses").items()) p.buses.push_back(busFromJson(jb));
    p.masterEnvelopes = envelopesFromJson(j.get("masterEnvelopes"));
    p.videoEnvelopes = envelopesFromJson(j.get("videoEnvelopes"));
    p.videoFadeTop = (uint32_t)j.get("videoFadeTop").asDouble((double)0xFFFFFFFFu);
    p.videoFadeBottom = (uint32_t)j.get("videoFadeBottom").asDouble((double)0xFF000000u);
    for (const auto& jm : j.get("markers").items()) p.markers.push_back(markerFromJson(jm));
    for (const auto& b : j.get("mediaBins").items())
        if (!b.asString().empty()) p.mediaBins.push_back(b.asString());
    for (const auto& b : j.get("smartBins").items()) p.smartBins.push_back({b.get("name").asString(), b.get("search").asString()});
    for (const auto& e : j.get("media").items()) p.media.push_back(mediaItemFromJson(e));
    for (const auto& jt : j.get("tracks").items()) p.tracks.push_back(trackFromJson(jt));
    p.videoOut = chainFromJson(j.get("videoOut"));
    p.audioOut = chainFromJson(j.get("audioOut"));
    // ids: never hand out one that is already in use, whatever the file says
    uint64_t top = j.get("nextId").asUInt64(1);
    for (const auto& m : p.media) top = std::max(top, m.id + 1);
    for (const auto& b : p.buses) top = std::max(top, b.id + 1);
    for (const auto& m : p.markers) top = std::max(top, m.id + 1);
    for (const auto& t : p.tracks) {
        top = std::max(top, std::max(t.id, t.trackGroup) + 1);
        for (const auto& e : t.events) top = std::max(top, std::max(e.id, e.group) + 1);
    }
    p.forEachFx([&](FxSlot& f) { top = std::max(top, f.id + 1); });
    p.nextId = top;
    *this = p;
    return true;
}

bool Project::save(const std::string& path, std::string* error) const
{
    // Written beside it first and then put in its place.
    const std::string part = path + ".reno-saving";
#ifdef _WIN32
    const std::filesystem::path from(widen(part)), to(widen(path));
#else
    const std::filesystem::path from(part), to(path);
#endif
    std::error_code ec;
    if (!writeFile(part, toJson().dump(1))) {
        std::filesystem::remove(from, ec);
        if (error) *error = "the file could not be written";
        return false;
    }
    std::filesystem::rename(from, to, ec);
    if (ec) {
        std::filesystem::remove(from, ec);
        if (error) *error = "the file could not be written (" + path + " could not be replaced: is it open elsewhere?)";
        return false;
    }
    return true;
}

bool Project::load(const std::string& path, std::string* error)
{
    std::string text;
    if (!readFile(path, text)) {
        if (error) *error = "the file could not be read";
        return false;
    }
    Json j;
    std::string e;
    if (!Json::parse(text, j, &e)) {
        if (error) *error = "the file is damaged (" + e + ")";
        return false;
    }
    return fromJson(j, error);
}

}  // namespace reno
