// VEGAS's automation: see automation.h.
#include "automation.h"
#include <algorithm>
#include <cmath>

namespace reno {

const AutomationModeName kAutomationModes[4] = {
    {"off", "Off"}, {"read", "Read"}, {"touch", "Automation Write (Touch)"}, {"latch", "Automation Write (Latch)"}};

double automationValue(const Envelopes& envs, const std::string& kind, uint64_t target, double t, double whenNone)
{
    const Envelope* e = findEnvelope(envs, kind, target);
    return e && !e->points.empty() ? e->valueAt(t, whenNone) : whenNone;
}

static Envelope& envelopeFor(Envelopes& envs, const std::string& kind, uint64_t target)
{
    if (Envelope* e = findEnvelope(envs, kind, target)) return *e;
    Envelope e;
    e.kind = kind;
    e.target = target;
    envs.push_back(e);
    return envs.back();
}

void setEnvelopeAt(Envelopes& envs, const std::string& kind, uint64_t target, double t, double value, double near, int curve)
{
    if (!std::isfinite(t) || !std::isfinite(value)) return;
    t = std::max(0.0, t);
    const bool fresh = findEnvelope(envs, kind, target) == nullptr;
    Envelope& e = envelopeFor(envs, kind, target);
    if (fresh) e.visible = true;
    if (kind == "mute") curve = CurveHold;
    auto& pts = e.points;
    // the point at t (the nearest within reach)
    int best = -1;
    double bestD = near;
    for (size_t i = 0; i < pts.size(); ++i) {
        const double d = std::fabs(pts[i].time - t);
        if (d <= bestD) {
            bestD = d;
            best = (int)i;
        }
    }
    if (best >= 0) {
        pts[(size_t)best].value = value;
        return;
    }
    EnvPoint pt;
    pt.time = t;
    pt.value = value;
    pt.curve = curve;
    auto it = std::upper_bound(pts.begin(), pts.end(), t, [](double v, const EnvPoint& p) { return v < p.time; });
    if (it != pts.begin()) pt.curve = kind == "mute" ? CurveHold : (it - 1)->curve;
    pts.insert(it, pt);
}

void AutomationTake::write(double t, double value, double step)
{
    if (!std::isfinite(t) || !std::isfinite(value)) return;
    t = std::max(0.0, t);
    // playback went back (a loop): what was written from there on is written again
    while (!written.empty() && written.back().time > t + 1e-9) written.pop_back();
    if (!written.empty()) {
        EnvPoint& last = written.back();
        if (t - last.time < step) {
            if (written.size() > 1) last.time = t;
            last.value = value;
            return;
        }
    }
    EnvPoint pt;
    pt.time = t;
    pt.value = value;
    pt.curve = kind == "mute" ? CurveHold : CurveLinear;
    written.push_back(pt);
}

std::vector<EnvPoint> AutomationTake::result() const
{
    if (written.empty()) return base;
    const double a = written.front().time;
    double b = written.back().time;
    if (latch && latchTo > b) b = latchTo;
    // the value the envelope had (before recording) just after the take, for Touch's return
    Envelope old;
    old.points = base;
    const double after = b + 1.0 / 30.0;
    const double back = base.empty() ? whenNone : old.valueAt(after, whenNone);
    std::vector<EnvPoint> out;
    for (const EnvPoint& p : base)
        if (p.time < a - 1e-9) out.push_back(p);
    if (!base.empty() && a > 1e-6) {
        const double before = old.valueAt(a, whenNone);
        if (out.empty() || std::fabs(out.back().time - a) > 1e-6) {
            EnvPoint pb;
            pb.time = std::max(0.0, a - 1e-3);
            pb.value = before;
            pb.curve = CurveLinear;
            if (out.empty() || pb.time > out.back().time + 1e-9) out.push_back(pb);
        }
    }
    for (const EnvPoint& p : written) out.push_back(p);
    if (latch) {
        if (latchTo > written.back().time + 1e-9) {
            EnvPoint pl = written.back();
            pl.time = latchTo;
            out.push_back(pl);
        }
        for (const EnvPoint& p : base)
            if (p.time > b + 1e-9) out.push_back(p);
    } else {
        // Touch: back to the envelope as it was
        EnvPoint pr;
        pr.time = after;
        pr.value = back;
        pr.curve = kind == "mute" ? CurveHold : CurveLinear;
        bool baseAt = false;
        for (const EnvPoint& p : base)
            if (std::fabs(p.time - after) < 1e-6) baseAt = true;
        if (!baseAt) out.push_back(pr);
        for (const EnvPoint& p : base)
            if (p.time > b + 1e-9 && p.time >= after - 1e-6) out.push_back(p);
    }
    std::stable_sort(out.begin(), out.end(), [](const EnvPoint& x, const EnvPoint& y) { return x.time < y.time; });
    return out;
}

void AutomationTake::apply(Envelopes& envs) const
{
    Envelope& e = envelopeFor(envs, kind, target);
    e.points = result();
    if (!hadEnvelope) e.visible = true;
}

double thinTolerance(const std::string& kind)
{
    if (kind == "volume" || kind == "send") return 0.012;
    if (kind == "pan" || kind == "fadecolor") return 0.02;
    if (kind == "velocity") return 0.05;
    return 0.01;
}

int thinEnvelope(Envelope& e, double from, double to, double tolerance)
{
    auto& pts = e.points;
    if (pts.size() < 3) return 0;
    std::vector<char> keep(pts.size(), 1);
    // the range's points by index
    int first = -1, last = -1;
    for (size_t i = 0; i < pts.size(); ++i)
        if (pts[i].time >= from - 1e-9 && pts[i].time <= to + 1e-9) {
            if (first < 0) first = (int)i;
            last = (int)i;
        }
    if (first < 0 || last - first < 2) return 0;
    // keeps a point where the line between the last kept point and the next one misses it by more than the tolerance
    int anchor = first;
    for (int i = first + 1; i < last; ++i) {
        const EnvPoint& a = pts[(size_t)anchor];
        const EnvPoint& p = pts[(size_t)i];
        const EnvPoint& n = pts[(size_t)i + 1];
        bool drop = a.curve == CurveLinear && p.curve == CurveLinear && n.time > a.time;
        if (drop) {
            // every point dropped since the anchor must still lie near the line from the anchor to the next one
            for (int j = anchor + 1; j <= i && drop; ++j) {
                const EnvPoint& q = pts[(size_t)j];
                const double f = (q.time - a.time) / (n.time - a.time);
                const double v = a.value + (n.value - a.value) * f;
                if (std::fabs(v - q.value) > tolerance) drop = false;
            }
        }
        if (drop) keep[(size_t)i] = 0;
        else anchor = i;
    }
    std::vector<EnvPoint> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    const int removed = (int)(pts.size() - out.size());
    pts = out;
    return removed;
}

double autoDuckGain(double levelDb, double floorDb, double peak)
{
    const double threshold = -45.0;
    if (!(levelDb > threshold)) return 1.0;
    double db = std::min(0.0, floorDb) - std::max(0.0, std::min(1.0, peak)) * (levelDb - threshold);
    db = std::max(-96.0, db);
    return std::pow(10.0, db / 20.0);
}

std::vector<EnvPoint> duckPoints(const std::vector<float>& levelDb, double step, double t0, double floorDb, double peak)
{
    std::vector<EnvPoint> out;
    if (levelDb.empty() || !(step > 0)) return out;
    const double holdSec = 0.3, releaseDbPerSec = 40.0;
    double gdb = 0;                 // the gain now, in dB (0: not ducked)
    double hold = 0;
    auto add = [&](double t, double db) {
        EnvPoint pt;
        pt.time = std::max(0.0, t);
        pt.value = std::pow(10.0, db / 20.0);
        pt.curve = CurveLinear;
        out.push_back(pt);
    };
    add(t0, 0);
    for (size_t i = 0; i < levelDb.size(); ++i) {
        const double t = t0 + (double)i * step;
        const double target = 20.0 * std::log10(std::max(1e-9, autoDuckGain(levelDb[i], floorDb, peak)));
        if (target < gdb - 1e-6) {
            gdb = target;
            hold = holdSec;
        } else if (target > gdb + 1e-6) {
            if (hold > 0) hold -= step;
            else gdb = std::min(target, gdb + releaseDbPerSec * step);
        } else if (target < -1e-6) {
            hold = holdSec;
        }
        add(t + step, gdb);
    }
    add(t0 + (double)levelDb.size() * step + 1.0, 0);
    Envelope e;
    e.kind = "duck";
    e.points = out;
    thinEnvelope(e, 0, 1e9, 0.004);
    return e.points;
}

}  // namespace reno
