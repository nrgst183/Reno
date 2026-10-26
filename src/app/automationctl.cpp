// See automationctl.h.
#include "automationctl.h"
#include "../engine/automation.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace reno {
namespace app {

std::function<bool()> AutomationHooks::playing;
std::function<double()> AutomationHooks::position;

double automationNow()
{
    if (AutomationHooks::playing && AutomationHooks::playing() && AutomationHooks::position) return std::max(0.0, AutomationHooks::position());
    return AppState::get().cursor();
}

namespace {

struct Settings { bool controls = false; std::string mode = "read"; bool exists = false; };

Settings settingsOf(const Project& p, const EnvelopeOwner& o)
{
    Settings s;
    if (o.kind == EnvelopeOwner::Track) {
        for (const Track& t : p.tracks)
            if (t.id == o.id) { s.controls = t.automation; s.mode = t.automationMode; s.exists = true; }
    } else if (o.kind == EnvelopeOwner::Bus) {
        if (const Bus* b = p.findBus(o.id)) { s.controls = b->automation; s.mode = b->automationMode; s.exists = true; }
    }
    return s;
}

// one take being written
struct Take {
    EnvelopeOwner owner;
    AutomationTake take;
    double lastValue = 0;
    double lastTime = 0;
    bool released = false;     // control released (Latch keeps writing)
};
std::vector<Take> gTakes;

// "Smooth and thin automation data" preference: thin the stretch a take wrote.
void thinTake(Project& p, const Take& t)
{
    if (t.take.written.empty()) return;
    Envelopes* envs = AppState::envelopesOf(p, t.owner);
    if (!envs) return;
    for (Envelope& e : *envs)
        if (e.kind == t.take.kind && e.target == t.take.target)
            thinEnvelope(e, t.take.written.front().time, std::max(t.take.written.back().time, t.take.latchTo), thinTolerance(t.take.kind));
}
bool gLatchDrag = false;        // a drag was begun for a Latch take that goes on after release

}  // namespace

bool automationControlsOn(const Project& p, const EnvelopeOwner& o) { return settingsOf(p, o).controls; }
std::string automationModeOf(const Project& p, const EnvelopeOwner& o) { return settingsOf(p, o).mode; }

const char* automationModeName(const std::string& mode)
{
    for (const AutomationModeName& m : kAutomationModes)
        if (mode == m.key) return m.name;
    return "Read";
}

void setAutomationControls(AppState& app, const std::vector<EnvelopeOwner>& owners, bool on)
{
    app.edit(on ? "Automation Settings On" : "Automation Settings Off", [&](Project& p) {
        for (const EnvelopeOwner& o : owners) {
            if (o.kind == EnvelopeOwner::Track) {
                if (Track* t = p.findTrack(o.id)) t->automation = on;
            } else if (o.kind == EnvelopeOwner::Bus) {
                if (Bus* b = p.findBus(o.id)) b->automation = on;
            }
        }
    });
}

void setAutomationMode(AppState& app, const std::vector<EnvelopeOwner>& owners, const std::string& mode)
{
    bool known = false;
    for (const AutomationModeName& m : kAutomationModes) known = known || mode == m.key;
    if (!known) return;
    const double at = automationNow();
    app.edit(std::string("Automation Mode: ") + automationModeName(mode), [&](Project& p) {
        for (const EnvelopeOwner& o : owners) {
            if (o.kind == EnvelopeOwner::Track) {
                Track* t = p.findTrack(o.id);
                if (!t) continue;
                // Off: control settings at the cursor become the automation values
                if (mode == "off" && !t->automationOff()) {
                    const bool video = t->type == TrackType::Video;
                    t->autoLevel = trackAutomationValue(*t, video ? "composite" : "volume", at);
                    if (!video) t->autoPan = trackAutomationValue(*t, "pan", at);
                    else t->autoFade = trackAutomationValue(*t, "fadecolor", at);
                    t->autoMute = trackAutomationValue(*t, "mute", at) >= 0.5;
                }
                t->automationMode = mode;
                t->automation = true;
            } else if (o.kind == EnvelopeOwner::Bus) {
                if (Bus* b = p.findBus(o.id)) {
                    b->automationMode = mode;
                    b->automation = true;
                }
            }
        }
    });
}

ControlUse controlUse(const Project& p, const EnvelopeOwner& o, const std::string& kind, uint64_t target)
{
    const Settings s = settingsOf(p, o);
    if (!s.exists || !s.controls) return ControlUse::Trim;
    const Envelopes* envs = AppState::envelopesOf(p, o);
    const Envelope* e = envs ? findEnvelope(*envs, kind, target) : nullptr;
    const bool has = e && !e->points.empty();
    const bool track = o.kind == EnvelopeOwner::Track;
    if (s.mode == "off") return track ? ControlUse::AutoValue : ControlUse::Trim;
    if (s.mode == "read") return has ? ControlUse::Follow : (track ? ControlUse::AutoValue : ControlUse::Trim);
    return ControlUse::Write;
}

double controlShown(const Project& p, const EnvelopeOwner& o, const std::string& kind, uint64_t target, double trim, double t)
{
    const ControlUse use = controlUse(p, o, kind, target);
    if (use == ControlUse::Trim) return trim;
    if (o.kind == EnvelopeOwner::Track) {
        for (const Track& tr : p.tracks)
            if (tr.id == o.id) return trackAutomationValue(tr, kind, t);
        return trim;
    }
    const Envelopes* envs = AppState::envelopesOf(p, o);
    const Envelope* e = envs ? findEnvelope(*envs, kind, target) : nullptr;
    if (!e || e->points.empty()) return trim;
    return e->valueAt(t, trim);
}

bool controlMoved(Project& p, const EnvelopeOwner& o, const std::string& kind, uint64_t target, double value)
{
    const ControlUse use = controlUse(p, o, kind, target);
    if (use == ControlUse::Trim) return false;
    if (use == ControlUse::Follow) return true;
    if (use == ControlUse::AutoValue) {
        if (Track* t = p.findTrack(o.id)) {
            if (kind == "pan") t->autoPan = value;
            else if (kind == "mute") t->autoMute = value >= 0.5;
            else if (kind == "fadecolor") t->autoFade = value;
            else t->autoLevel = value;
        }
        return true;
    }
    Envelopes* envs = AppState::envelopesOf(p, o);
    if (!envs) return true;
    const double t = automationNow();
    const bool playing = AutomationHooks::playing && AutomationHooks::playing();
    const double fps = std::max(1.0, p.settings.fps());
    if (!playing) {
        // not playing: the point at the cursor takes the value
        setEnvelopeAt(*envs, kind, target, t, value, 0.5 / fps, AppState::envelopeDefaultCurve(kind));
        return true;
    }
    // playing: a take continuing from where playback began
    Take* tk = nullptr;
    for (Take& x : gTakes)
        if (x.owner == o && x.take.kind == kind && x.take.target == target) tk = &x;
    const Settings s = settingsOf(p, o);
    if (!tk) {
        gTakes.emplace_back();
        tk = &gTakes.back();
        tk->owner = o;
        tk->take.kind = kind;
        tk->take.target = target;
        tk->take.latch = s.mode == "latch";
        if (const Envelope* e = findEnvelope(*envs, kind, target)) {
            tk->take.base = e->points;
            tk->take.hadEnvelope = true;
        }
        tk->take.whenNone = AppState::envelopeDefault(kind);
    }
    tk->released = false;
    tk->lastValue = value;
    tk->lastTime = t;
    tk->take.write(t, value, 0.04);
    tk->take.apply(*envs);
    return true;
}

void controlReleased(AppState& app)
{
    const bool playing = AutomationHooks::playing && AutomationHooks::playing();
    std::vector<Take> done;
    for (auto it = gTakes.begin(); it != gTakes.end();) {
        if (it->take.latch && playing) {
            it->released = true;
            ++it;
        } else {
            done.push_back(*it);
            it = gTakes.erase(it);
        }
    }
    // finished Touch take: thin its points in a separate step
    if (!done.empty() && app.prefs.thinAutomation && !app.dragging())
        app.edit("Smooth and Thin Automation", [&](Project& p) { for (const Take& t : done) thinTake(p, t); });
}

void automationTick(AppState& app)
{
    if (gTakes.empty()) return;
    const bool playing = AutomationHooks::playing && AutomationHooks::playing();
    bool any = false;
    for (Take& t : gTakes) any = any || t.released;
    if (!any) {
        if (!playing) gTakes.clear();                   // Touch take still held when playback stops
        return;
    }
    const double now = automationNow();
    if (playing) {
        // Latch: keep writing the last value
        if (!gLatchDrag) {
            if (app.dragging()) return;
            app.beginDrag();
            gLatchDrag = true;
        }
        app.dragUpdate([&](Project& p) {
            for (Take& t : gTakes) {
                if (!t.released) continue;
                t.take.write(now, t.lastValue, 0.04);
                if (Envelopes* envs = AppState::envelopesOf(p, t.owner)) t.take.apply(*envs);
            }
        });
        return;
    }
    // playback stopped: end the Latch takes
    if (gLatchDrag) {
        app.dragUpdate([&](Project& p) {
            for (Take& t : gTakes) {
                if (!t.released) continue;
                t.take.latchTo = std::max(t.lastTime, now);
                if (Envelopes* envs = AppState::envelopesOf(p, t.owner)) t.take.apply(*envs);
                if (app.prefs.thinAutomation) thinTake(p, t);
            }
        });
        app.endDrag("Record Automation");
        gLatchDrag = false;
    }
    gTakes.clear();
}

}  // namespace app
}  // namespace reno
