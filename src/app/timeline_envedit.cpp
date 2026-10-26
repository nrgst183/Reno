// Editing envelopes with the mouse, and the envelope commands.
#include "timeline_internal.h"

namespace reno {
namespace app {

// A press on an envelope's point or line: the point (and the others selected with it, or, with a time selection around
// it, all the points of that envelope in the time selection) or the segment gets ready to move.
void TimelineWidget::Impl::pressEnvelope(const Hit& h, Qt::KeyboardModifiers mods)
{
    EnvLane L;
    if (!envLaneOf(h.env, &L)) return;
    frontEnv = h.env;
    drag.env = h.env;
    drag.envBase = L.env->points;
    drag.envOrigin = L.origin;
    drag.envArea = L.area;
    drag.envFrom = (L.from > -1e299 ? L.from : 0.0) - L.origin;
    drag.envTo = L.to < 1e299 ? L.to - L.origin : 1e300;
    const int n = (int)L.env->points.size();
    // VEGAS's Time selection envelope fades: a drag inside the time selection moves all the envelope there, with fades
    // in and out of it (Preferences > Editing). Alt (moving points in time) and Shift work as before.
    if (app.prefs.timeSelEnvFades && app.loopEnd > app.loopStart && !(mods & (Qt::ShiftModifier | Qt::AltModifier)) && drag.pressTime >= app.loopStart &&
        drag.pressTime <= app.loopEnd) {
        pressEnvelopeInSelection(L, h);
        return;
    }
    if (h.part == Part::EnvPoint) {
        const bool shift = (mods & Qt::ShiftModifier) != 0;
        if (selEnv != h.env) selPoints.clear();
        selEnv = h.env;
        if (shift) {
            if (!selPoints.erase(h.point)) selPoints.insert(h.point);
        } else if (!selPoints.count(h.point)) {
            selPoints = {h.point};
            const double t = L.origin + L.env->points[(size_t)h.point].time;
            if (app.loopEnd > app.loopStart && t >= app.loopStart - 1e-9 && t <= app.loopEnd + 1e-9)
                for (int i = 0; i < n; ++i) {
                    const double ti = L.origin + L.env->points[(size_t)i].time;
                    if (ti >= app.loopStart - 1e-9 && ti <= app.loopEnd + 1e-9) selPoints.insert(i);
                }
        }
        drag.envMoving.assign(selPoints.begin(), selPoints.end());
        if (!selPoints.count(h.point)) drag.envMoving.clear();
        drag.grabbed = (size_t)h.point;
        drag.kind = Drag::EnvPoints;
        const EnvPoint& pt = L.env->points[(size_t)h.point];
        envLabel = envValueText(h.env.kind, pt.value);
        envLabelAt = envPointPos(L, (size_t)h.point);
    } else {
        const int a = h.point;
        drag.envMoving.clear();
        if (a < 0) drag.envMoving.push_back(0);
        else {
            drag.envMoving.push_back(a);
            if (a + 1 < n) drag.envMoving.push_back(a + 1);
        }
        selEnv = h.env;
        selPoints = std::set<int>(drag.envMoving.begin(), drag.envMoving.end());
        drag.grabbed = (size_t)drag.envMoving[0];
        drag.kind = Drag::EnvSegment;
    }
    drag.snaps = snapTargets(std::set<uint64_t>(), true);
    q->update();
}

void TimelineWidget::Impl::updateSnapOffset(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (!drag.started) {
        if (std::fabs(pos.x() - drag.pressPos.x()) < 2.0) return;
        app.beginDrag();
        drag.started = true;
    }
    const Event* e = app.project().findEvent(drag.hit.event);
    if (!e) return;
    const double start = e->start, len = e->length;
    const uint64_t id = e->id;
    double line = -1;
    const double at = snapped(drag.snaps, start + drag.markerTime + timeAtF(pos.x()) - drag.pressTime, mods, &line);
    const double off = clampd(at - start, 0.0, len);
    showSnap(line, start + off);
    app.dragUpdate([&](Project& p) { if (Event* x = p.findEvent(id)) x->snapOffset = off; });
    app.status("Snap offset: " + timecode(off, fps()));
    q->setToolTip(QString::fromStdString("Snap offset: " + timecode(off, fps())));
}

// The press of pressEnvelope inside the time selection with Time selection envelope fades on: points are put where the
// fades start and end (as the envelope is there now), and the ones inside the fades move up and down.
void TimelineWidget::Impl::pressEnvelopeInSelection(const EnvLane& L, const Hit& h)
{
    const double s0 = app.loopStart - L.origin, s1 = app.loopEnd - L.origin;
    const double len = std::max(0.0, app.prefs.timeSelEnvFadeLength);
    const std::string& al = app.prefs.timeSelEnvFadeAlign;
    double outA = s0 - len, inA = s0, inB = s1, outB = s1 + len;
    if (al == "centered") { outA = s0 - len / 2; inA = s0 + len / 2; inB = s1 - len / 2; outB = s1 + len / 2; }
    else if (al == "inside") { outA = s0; inA = s0 + len; inB = s1 - len; outB = s1; }
    if (inA > inB) inA = inB = (inA + inB) / 2;
    const double lo = drag.envFrom, hi = drag.envTo;
    outA = clampd(outA, lo, hi); inA = clampd(inA, lo, hi); inB = clampd(inB, lo, hi); outB = clampd(outB, lo, hi);
    std::vector<EnvPoint> pts = L.env->points;
    const double dflt = AppState::envelopeDefault(h.env.kind);
    const std::string kind = h.env.kind;
    auto ensure = [&](double t) {
        for (const EnvPoint& pt : pts)
            if (std::fabs(pt.time - t) < 1e-6) return;
        Envelope tmp;
        tmp.points = pts;
        EnvPoint np;
        np.time = t;
        np.value = tmp.valueAt(t, dflt);
        size_t at = 0;
        while (at < pts.size() && pts[at].time <= t) ++at;
        np.curve = kind == "mute" ? (int)CurveHold : at > 0 ? pts[at - 1].curve : AppState::envelopeDefaultCurve(kind);
        pts.insert(pts.begin() + (std::ptrdiff_t)at, np);
    };
    if (outA < inA - 1e-6) ensure(outA);
    if (outB > inB + 1e-6) ensure(outB);
    ensure(inA);
    ensure(inB);
    drag.envBase = pts;
    drag.envMoving.clear();
    size_t nearest = 0;
    double best = 1e300;
    const double pt = drag.pressTime - L.origin;
    for (size_t i = 0; i < pts.size(); ++i)
        if (pts[i].time >= inA - 1e-9 && pts[i].time <= inB + 1e-9) {
            drag.envMoving.push_back((int)i);
            if (std::fabs(pts[i].time - pt) < best) { best = std::fabs(pts[i].time - pt); nearest = i; }
        }
    drag.grabbed = nearest;
    drag.kind = Drag::EnvSegment;
    selEnv = h.env;
    selPoints = std::set<int>(drag.envMoving.begin(), drag.envMoving.end());
    drag.snaps.clear();
    q->update();
}

void TimelineWidget::Impl::updateEnvelope(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (!drag.started) {
        if ((pos - drag.pressPos).manhattanLength() < 3.0) return;
        if (drag.envMoving.empty()) return;
        app.beginDrag();
        drag.started = true;
    }
    const bool ctrl = (mods & Qt::ControlModifier) != 0, alt = (mods & Qt::AltModifier) != 0;
    const EnvelopeRef ref = drag.env;
    const std::vector<EnvPoint> base = drag.envBase;
    const std::vector<int> moving = drag.envMoving;
    const size_t grabbed = std::min(drag.grabbed, base.size() - 1);
    double df = (drag.pressPos.y() - pos.y()) / std::max(1.0, drag.envArea.height());
    double dt = 0;
    snapLine = -1;
    if (drag.kind == Drag::EnvPoints) {
        // as in VEGAS: Ctrl changes the value finely and leaves the time; Alt changes the time only; Ctrl+Alt the value
        // only, in normal steps
        if (!ctrl) dt = timeAtF(pos.x()) - drag.pressTime;
        if (alt && !ctrl) df = 0;
    }
    if (ctrl && !alt) df *= 0.125;
    if (dt != 0) {
        const double g = drag.envOrigin + base[grabbed].time;
        double line = -1;
        const double want = snapped(drag.snaps, g + dt, mods, &line);
        dt = want - g;
        std::set<int> mv(moving.begin(), moving.end());
        for (int i : moving) {
            double lo = drag.envFrom, hi = drag.envTo;
            for (int j = i - 1; j >= 0; --j)
                if (!mv.count(j)) { lo = std::max(lo, base[(size_t)j].time); break; }
            for (int j = i + 1; j < (int)base.size(); ++j)
                if (!mv.count(j)) { hi = std::min(hi, base[(size_t)j].time); break; }
            const double t = base[(size_t)i].time;
            dt = std::max(dt, std::min(lo, t) - t);
            dt = std::min(dt, std::max(hi, t) - t);
        }
        showSnap(line, g + dt);
    }
    const std::string kind = ref.kind;
    auto valueOf = [&](const EnvPoint& b) { return df == 0 ? b.value : fracToEnv(kind, envToFrac(kind, b.value) + df); };
    app.dragUpdate([&](Project& p) {
        Envelope* e = AppState::findEnvelope(p, ref);
        if (!e) return;
        if (e->points.size() != base.size()) e->points = base;
        for (int i : moving) {
            EnvPoint& pt = e->points[(size_t)i];
            pt.time = base[(size_t)i].time + dt;
            pt.value = valueOf(base[(size_t)i]);
        }
    });
    const double v = valueOf(base[grabbed]);
    const double at = drag.envOrigin + base[grabbed].time + dt;
    envLabel = envValueText(kind, v);
    envLabelAt = QPointF(xAtF(at), drag.envArea.bottom() - envToFrac(kind, v) * drag.envArea.height());
    app.status((envelopeName(kind) + ": " + envLabel).toStdString() + (drag.kind == Drag::EnvPoints ? "  at " + rulerTime(app.project(), at) : std::string()));
    q->update();
}

void TimelineWidget::Impl::updateEnvBand(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (!drag.started) {
        if ((pos - drag.pressPos).manhattanLength() < 4.0) return;
        drag.started = true;
        if (!(mods & (Qt::ControlModifier | Qt::ShiftModifier))) selPoints.clear();
        drag.baseSelection.clear();
        for (int i : selPoints) drag.baseSelection.insert((uint64_t)i);
    }
    const QRectF band = QRectF(drag.pressPos, pos).normalized();
    const std::vector<EnvLane> lanes = envLanes();
    const EnvLane* use = nullptr;
    for (const EnvLane& L : lanes)
        if (L.clip.contains(drag.pressPos)) use = &L;
    if (use) {
        if (selEnv != use->ref) drag.baseSelection.clear();
        selEnv = use->ref;
        frontEnv = use->ref;
        selPoints.clear();
        for (uint64_t i : drag.baseSelection) selPoints.insert((int)i);
        for (size_t i = 0; i < use->env->points.size(); ++i)
            if (pointShown(*use, i) && band.contains(envPointPos(*use, i))) selPoints.insert((int)i);
    }
}

void TimelineWidget::Impl::addEnvelopePoint(const EnvelopeRef& ref, double timelineT, Qt::KeyboardModifiers mods)
{
    EnvLane L;
    if (!envLaneOf(ref, &L)) return;
    double line = -1;
    double t = snapped(snapTargets(std::set<uint64_t>(), true), timelineT, mods, &line);
    t = clampd(t, L.from > -1e299 ? L.from : 0.0, L.to) - L.origin;
    const double dflt = AppState::envelopeDefault(ref.kind);
    int at = 0;
    app.edit("Add Envelope Point", [&](Project& p) {
        Envelope* e = AppState::findEnvelope(p, ref);
        if (!e) return;
        EnvPoint np;
        np.time = t;
        np.value = e->valueAt(t, dflt);
        at = 0;
        while (at < (int)e->points.size() && e->points[(size_t)at].time <= t) ++at;
        np.curve = at > 0 ? e->points[(size_t)at - 1].curve : AppState::envelopeDefaultCurve(ref.kind);
        e->points.insert(e->points.begin() + at, np);
    });
    selEnv = ref;
    selPoints = {at};
    frontEnv = ref;
    q->update();
}

bool TimelineWidget::Impl::deleteEnvelopePoints()
{
    validateEnvSelection();
    if (!selEnv.valid() || selPoints.empty()) return false;
    const EnvelopeRef ref = selEnv;
    std::set<int> gone = selPoints;
    const Envelope* e = AppState::findEnvelope(app.project(), ref);
    if (gone.size() >= e->points.size()) gone.erase(gone.begin());
    selPoints.clear();
    if (gone.empty()) return true;
    app.edit("Delete Envelope Points", [&](Project& p) {
        Envelope* x = AppState::findEnvelope(p, ref);
        if (!x) return;
        for (auto it = gone.rbegin(); it != gone.rend(); ++it)
            if (*it < (int)x->points.size()) x->points.erase(x->points.begin() + *it);
    });
    return true;
}

void TimelineWidget::Impl::validateEnvSelection()
{
    if (!selEnv.valid()) return;
    const Envelope* e = AppState::findEnvelope(app.project(), selEnv);
    if (!e) {
        selEnv = EnvelopeRef();
        selPoints.clear();
        return;
    }
    for (auto it = selPoints.begin(); it != selPoints.end();)
        if (*it >= (int)e->points.size()) it = selPoints.erase(it);
        else ++it;
}

std::set<int> TimelineWidget::Impl::envCommandPoints(const EnvelopeRef& ref) const
{
    std::set<int> out;
    const Envelope* e = AppState::findEnvelope(app.project(), ref);
    if (!e) return out;
    EnvLane L;
    double origin = 0;
    if (envLaneOf(ref, &L)) origin = L.origin;
    if (app.loopEnd > app.loopStart) {
        for (size_t i = 0; i < e->points.size(); ++i) {
            const double t = origin + e->points[i].time;
            if (t >= app.loopStart - 1e-9 && t <= app.loopEnd + 1e-9) out.insert((int)i);
        }
        return out;
    }
    if (selEnv == ref)
        for (int i : selPoints)
            if (i >= 0 && i < (int)e->points.size()) out.insert(i);
    return out;
}

// Thins points [a, b] of an envelope (Douglas-Peucker on time and height): a point goes when the line between the points
// kept either side of it passes within 'tol' of the envelope's height (0..1) at it. 'a' and 'b' stay.
std::vector<EnvPoint> TimelineWidget::Impl::thinRun(const std::string& kind, const std::vector<EnvPoint>& pts, size_t a, size_t b, double tol)
{
    std::vector<char> keep(pts.size(), 1);
    if (b <= a + 1 || kind == "mute") return pts;
    std::vector<std::pair<size_t, size_t>> stack{{a, b}};
    for (size_t i = a + 1; i < b; ++i) keep[i] = 0;
    while (!stack.empty()) {
        const auto [i0, i1] = stack.back();
        stack.pop_back();
        if (i1 <= i0 + 1) continue;
        Envelope two;
        two.points = {pts[i0], pts[i1]};
        double worst = -1;
        size_t at = i0;
        for (size_t i = i0 + 1; i < i1; ++i) {
            const double d = std::fabs(envToFrac(kind, pts[i].value) - envToFrac(kind, two.valueAt(pts[i].time, pts[i].value)));
            if (d > worst) { worst = d; at = i; }
        }
        if (worst > tol) {
            keep[at] = 1;
            stack.push_back({i0, at});
            stack.push_back({at, i1});
        }
    }
    std::vector<EnvPoint> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

void TimelineWidget::Impl::thinEnvelope(const EnvelopeRef& ref, bool selectedOnly)
{
    const Envelope* e = AppState::findEnvelope(app.project(), ref);
    if (!e || e->points.size() < 3) return;
    size_t a = 0, b = e->points.size() - 1;
    if (selectedOnly) {
        const std::set<int> pts = envCommandPoints(ref);
        if (pts.size() < 3) return;
        a = (size_t)*pts.begin();
        b = (size_t)*pts.rbegin();
    }
    const std::string kind = ref.kind;
    app.edit(selectedOnly ? "Thin Selected Points" : "Thin All Points", [&](Project& p) {
        if (Envelope* x = AppState::findEnvelope(p, ref)) x->points = thinRun(kind, x->points, a, std::min(b, x->points.size() - 1), 0.01);
    });
    selPoints.clear();
    q->update();
}

void TimelineWidget::Impl::flipEnvelopePoints(const EnvelopeRef& ref)
{
    const std::set<int> pts = envCommandPoints(ref);
    if (pts.empty()) return;
    const std::string kind = ref.kind;
    app.edit("Flip Envelope", [&](Project& p) {
        if (Envelope* x = AppState::findEnvelope(p, ref))
            for (int i : pts)
                if (i < (int)x->points.size()) x->points[(size_t)i].value = fracToEnv(kind, 1.0 - envToFrac(kind, x->points[(size_t)i].value));
    });
}

// The envelope brush (VEGAS's Shift+drag over an envelope's line): the points under the stretch drawn over are replaced
// by what was drawn, one point per pixel column.
void TimelineWidget::Impl::pressEnvBrush(const Hit& h)
{
    EnvLane L;
    if (!envLaneOf(h.env, &L)) return;
    frontEnv = h.env;
    selEnv = h.env;
    selPoints.clear();
    drag.env = h.env;
    drag.envBase = L.env->points;
    drag.envOrigin = L.origin;
    drag.envArea = L.area;
    drag.envFrom = (L.from > -1e299 ? L.from : 0.0) - L.origin;
    drag.envTo = L.to < 1e299 ? L.to - L.origin : 1e300;
    drag.brush.clear();
    drag.brushPps = pps;
    drag.kind = Drag::EnvBrush;
}

std::vector<EnvPoint> TimelineWidget::Impl::brushedPoints(bool thin) const
{
    const std::string kind = drag.env.kind;
    std::vector<EnvPoint> out;
    if (drag.brush.empty()) return drag.envBase;
    const double t0 = drag.brush.begin()->first / drag.brushPps - drag.envOrigin;
    const double t1 = drag.brush.rbegin()->first / drag.brushPps - drag.envOrigin;
    const int curve = drag.envBase.empty() ? AppState::envelopeDefaultCurve(kind) : CurveLinear;
    for (const EnvPoint& b : drag.envBase)
        if (b.time < t0 - 1e-9) out.push_back(b);
    const size_t first = out.size();
    for (const auto& c : drag.brush) {
        EnvPoint np;
        np.time = clampd(c.first / drag.brushPps - drag.envOrigin, drag.envFrom, drag.envTo);
        np.value = fracToEnv(kind, c.second);
        np.curve = kind == "mute" ? (int)CurveHold : curve;
        if (out.size() > first && np.time <= out.back().time + 1e-9) out.back() = np;
        else out.push_back(np);
    }
    const size_t last = out.size() - 1;
    for (const EnvPoint& b : drag.envBase)
        if (b.time > t1 + 1e-9) out.push_back(b);
    if (thin && last > first + 1) out = thinRun(kind, out, first, last, std::max(0.004, 1.5 / std::max(1.0, drag.envArea.height())));
    return out;
}

void TimelineWidget::Impl::updateEnvBrush(const QPointF& pos)
{
    if (!drag.started) {
        if ((pos - drag.pressPos).manhattanLength() < 2.0) return;
        app.beginDrag();
        drag.started = true;
        drag.brush[(long long)std::llround(timeAtF(drag.pressPos.x()) * drag.brushPps)] =
            clampd((drag.envArea.bottom() - drag.pressPos.y()) / std::max(1.0, drag.envArea.height()), 0.0, 1.0);
    }
    const double f = clampd((drag.envArea.bottom() - pos.y()) / std::max(1.0, drag.envArea.height()), 0.0, 1.0);
    const long long col = (long long)std::llround(timeAtF(pos.x()) * drag.brushPps);
    const long long prev = drag.lastBrushCol;
    if (drag.brush.count(prev) && prev != col) {
        const double pf = drag.brush[prev];
        const long long step = col > prev ? 1 : -1;
        for (long long c = prev + step; c != col; c += step)
            drag.brush[c] = pf + (f - pf) * (double)(c - prev) / (double)(col - prev);
    }
    drag.brush[col] = f;
    drag.lastBrushCol = col;
    const std::vector<EnvPoint> pts = brushedPoints(false);
    const EnvelopeRef ref = drag.env;
    app.dragUpdate([&](Project& p) { if (Envelope* e = AppState::findEnvelope(p, ref)) e->points = pts; });
    envLabel = envValueText(ref.kind, fracToEnv(ref.kind, f));
    envLabelAt = pos;
    q->update();
}

void TimelineWidget::Impl::endEnvBrush(const DragState& was)
{
    if (!was.started) return;
    DragState keep = drag;
    drag = was;
    const std::vector<EnvPoint> pts = brushedPoints(app.prefs.thinAutomation);
    drag = keep;
    const EnvelopeRef ref = was.env;
    app.dragUpdate([&](Project& p) { if (Envelope* e = AppState::findEnvelope(p, ref)) e->points = pts; });
    app.endDrag("Draw Envelope");
}

bool TimelineWidget::Impl::copyEnvelopePoints()
{
    if (opt.tool != EditTool::Envelope) return false;
    validateEnvSelection();
    const EnvelopeRef ref = selEnv.valid() ? selEnv : frontEnv;
    const Envelope* e = AppState::findEnvelope(app.project(), ref);
    if (!e) return false;
    const std::set<int> pts = envCommandPoints(ref);
    if (pts.empty()) return false;
    EnvLane L;
    const double origin = envLaneOf(ref, &L) ? L.origin : 0.0;
    envClip = EnvClip();
    envClip.kind = ref.kind;
    const double from = app.loopEnd > app.loopStart ? app.loopStart - origin : e->points[(size_t)*pts.begin()].time;
    for (int i : pts) {
        EnvPoint pt = e->points[(size_t)i];
        pt.time -= from;
        envClip.points.push_back(pt);
    }
    envClip.span = app.loopEnd > app.loopStart ? app.loopEnd - app.loopStart : envClip.points.back().time;
    app.status(QString("%1 envelope point(s) copied").arg(envClip.points.size()).toStdString());
    return true;
}

// Edit > Paste with the Envelope tool: the copied points into the selected envelope at the cursor (the points they cover
// are replaced). Another kind of envelope takes them at the same heights.
bool TimelineWidget::Impl::pasteEnvelopePoints()
{
    if (opt.tool != EditTool::Envelope || envClip.points.empty()) return false;
    validateEnvSelection();
    const EnvelopeRef ref = selEnv.valid() ? selEnv : frontEnv;
    EnvLane L;
    if (!AppState::findEnvelope(app.project(), ref) || !envLaneOf(ref, &L)) return false;
    const double at = quantized(app.cursor()) - L.origin;
    const double lo = L.from > -1e299 ? L.from - L.origin : 0.0, hi = L.to < 1e299 ? L.to - L.origin : 1e300;
    const EnvClip clip = envClip;
    const std::string kind = ref.kind;
    app.edit("Paste Envelope Points", [&](Project& p) {
        Envelope* e = AppState::findEnvelope(p, ref);
        if (!e) return;
        std::vector<EnvPoint> out;
        for (const EnvPoint& b : e->points)
            if (b.time < at - 1e-9 || b.time > at + clip.span + 1e-9) out.push_back(b);
        for (EnvPoint pt : clip.points) {
            pt.time = at + pt.time;
            if (pt.time < lo - 1e-9 || pt.time > hi + 1e-9) continue;
            if (kind != clip.kind) pt.value = fracToEnv(kind, envToFrac(clip.kind, pt.value));
            if (kind == "mute") pt.curve = CurveHold;
            out.push_back(pt);
        }
        std::stable_sort(out.begin(), out.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
        if (!out.empty()) e->points = out;
    });
    q->update();
    return true;
}

void TimelineWidget::Impl::setMarkerTool(bool on)
{
    markerTool = on;
    if (!on) selMarkers.clear();
    markerToolButton->update();
    app.status(on ? "Marker Tool: click markers (Ctrl+click, Shift+click, Left / Right, Shift+Left / Right), drag them together, Delete deletes them."
                  : std::string());
    q->update();
}

// A marker tag clicked with the tool on: Ctrl adds or removes it, Shift selects the markers between the last one chosen
// and it, a plain click selects it (keeping the others if it was already selected, to drag them).
void TimelineWidget::Impl::chooseMarker(uint64_t id, Qt::KeyboardModifiers mods)
{
    const int k = markerIndex(id);
    if (k < 0) return;
    const Project& pr = app.project();
    const Project::Marker& m = pr.markers[(size_t)k];
    for (auto it = selMarkers.begin(); it != selMarkers.end();) {
        const int j = markerIndex(*it);
        if (j < 0 || markerBar(pr.markers[(size_t)j]) != markerBar(m)) it = selMarkers.erase(it);
        else ++it;
    }
    if (mods & Qt::ControlModifier) {
        if (!selMarkers.erase(id)) selMarkers.insert(id);
    } else if ((mods & Qt::ShiftModifier) && markerAnchor && markerIndex(markerAnchor) >= 0) {
        const double a = pr.markers[(size_t)markerIndex(markerAnchor)].time, b = m.time;
        for (const Project::Marker& o : pr.markers)
            if (markerBar(o) == markerBar(m) && o.time >= std::min(a, b) - 1e-9 && o.time <= std::max(a, b) + 1e-9) selMarkers.insert(o.id);
    } else if (!selMarkers.count(id)) {
        selMarkers = {id};
    }
    if (!(mods & Qt::ShiftModifier)) markerAnchor = id;
    q->update();
}

bool TimelineWidget::Impl::markerToolKey(int dir, bool extend)
{
    const Project& pr = app.project();
    int bar = 0;
    double from = app.cursor();
    if (markerAnchor && markerIndex(markerAnchor) >= 0) {
        const Project::Marker& a = pr.markers[(size_t)markerIndex(markerAnchor)];
        bar = markerBar(a);
        from = a.time;
    }
    const Project::Marker* best = nullptr;
    for (const Project::Marker& o : pr.markers) {
        if (markerBar(o) != bar || o.id == markerAnchor) continue;
        if (dir > 0 ? o.time <= from + 1e-9 : o.time >= from - 1e-9) continue;
        if (!best || (dir > 0 ? o.time < best->time : o.time > best->time)) best = &o;
    }
    if (!best) return true;
    if (extend) selMarkers.insert(best->id);
    else selMarkers = {best->id};
    markerAnchor = best->id;
    userCursor(best->time);
    scrollToCursor();
    q->update();
    return true;
}

void TimelineWidget::Impl::deleteChosenMarkers()
{
    if (selMarkers.empty()) return;
    const std::set<uint64_t> gone = selMarkers;
    selMarkers.clear();
    app.edit(gone.size() > 1 ? "Delete Markers" : "Delete Marker", [&](Project& p) {
        p.markers.erase(std::remove_if(p.markers.begin(), p.markers.end(), [&](const Project::Marker& o) { return gone.count(o.id) != 0; }), p.markers.end());
    });
}

void TimelineWidget::Impl::updateMarker(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (!drag.started) {
        if (std::fabs(pos.x() - drag.pressPos.x()) < 3.0) return;
        app.beginDrag();
        drag.started = true;
    }
    const uint64_t id = drag.hit.marker;
    const double raw = timeAtF(pos.x()) - drag.pressTime;
    const double a0 = drag.markerTime, len0 = drag.markerLength, fr = opt.quantize ? frame() : 1e-3;
    double line = -1, edge = 0, at = a0, len = len0;
    switch (drag.kind) {
    case Drag::Marker:
        at = edge = snapped(drag.snaps, a0 + raw, mods, &line);
        break;
    case Drag::RegionStart:
        at = edge = std::min(snapped(drag.snaps, a0 + raw, mods, &line), a0 + len0 - fr);
        len = a0 + len0 - at;
        break;
    case Drag::RegionEnd:
        edge = std::max(snapped(drag.snaps, a0 + len0 + raw, mods, &line), a0 + fr);
        len = edge - a0;
        break;
    default: {
        double ls = -1, le = -1;
        const double s0 = a0 + raw;
        const double sa = snapped(drag.snaps, s0, mods, &ls), se = snapped(drag.snaps, s0 + len0, mods, &le);
        if (le >= 0 && (ls < 0 || std::fabs(se - (s0 + len0)) < std::fabs(sa - s0))) {
            at = se - len0;
            line = le;
            edge = se;
        } else {
            at = sa;
            line = ls;
            edge = sa;
        }
        at = std::max(0.0, at);
        break;
    }
    }
    showSnap(line, edge);
    const bool together = markerTool && selMarkers.count(id) && selMarkers.size() > 1 && (drag.kind == Drag::Marker || drag.kind == Drag::RegionMove);
    app.dragUpdate([&](Project& p) {
        double d = at - a0;
        if (together)
            for (const auto& m : p.markers)
                if (selMarkers.count(m.id) && m.id != id) d = std::max(d, -m.time);
        for (auto& m : p.markers) {
            if (m.id == id) {
                m.time = std::max(0.0, together ? a0 + d : at);
                m.length = len;
            } else if (together && selMarkers.count(m.id)) {
                m.time = std::max(0.0, m.time + d);
            }
        }
    });
    app.status((drag.kind == Drag::Marker ? "Marker: " : "Region: ") + rulerTime(app.project(), at) +
               (drag.kind == Drag::Marker ? std::string() : " - " + rulerTime(app.project(), at + len)));
}

void TimelineWidget::Impl::updateRubberBand(const QPointF& pos, Qt::KeyboardModifiers)
{
    if (!drag.started) {
        if ((pos - drag.pressPos).manhattanLength() < 4.0) return;
        drag.started = true;
    }
    QRectF band = QRectF(drag.pressPos, pos).normalized();
    // VEGAS's Selection tool: a right click while dragging cycles between rectangle, time and track selection; time:
    // every track over the stretch of time (which becomes the time selection); track: the whole tracks
    if (drag.bandMode == 1) {
        band.setTop(kTopH);
        band.setBottom(lanesBottom());
        app.setLoop(std::max(0.0, timeAtF(band.left())), std::max(0.0, timeAtF(band.right())));
    } else if (drag.bandMode == 2) {
        band.setLeft(headerW);
        band.setRight(1e9);
    }
    std::set<uint64_t> sel = drag.baseSelection;
    const Project& pr = app.project();
    for (const Row& r : rows) {
        if (rowTop(r) > band.bottom() + 1 || rowTop(r) + r.h < band.top() - 1) continue;
        if (drag.bandMode == 2) {
            for (const Event& ev : pr.tracks[(size_t)r.index].events) sel.insert(ev.id);
            continue;
        }
        const Track& t = pr.tracks[(size_t)r.index];
        const std::pair<size_t, size_t> range = eventRange((size_t)r.index, timeAtF(band.left() - 2.0), timeAtF(band.right() + 2.0));
        for (size_t k = range.first; k < range.second; ++k)
            if (eventRect(r, t.events[k]).intersects(band)) sel.insert(t.events[k].id);
    }
    app.setSelection(sel);
}

// Where something is drawn differently while the mouse is over it (lit up): repainted when the mouse enters or leaves
// it, instead of the whole timeline.
QRegion TimelineWidget::Impl::hotRegion(const Hit& h) const
{
    switch (h.part) {
    case Part::None:
    case Part::Corner:
    case Part::Ruler:
    case Part::LoopStrip:
    case Part::MarkerBar:
    case Part::Lane:
    case Part::BusLane:
    case Part::EventBody:
    case Part::EventLeft:
    case Part::EventRight: return QRegion();
    case Part::MarkerTag:
    case Part::RegionEndTag: return QRegion(QRect(headerW, 0, viewW(), kTopH));
    case Part::FadeIn:
    case Part::FadeOut:
    case Part::EventGain:
    case Part::EventFx:
    case Part::EventCrop:
    case Part::EventMenu: return QRegion(eventArea(h.event));
    case Part::HeaderGap: return QRegion(QRect(headerW - kHeaderGap, kTopH, kHeaderGap, std::max(0, lanesBottom() - kTopH)));
    case Part::HeaderEdge: return QRegion();
    case Part::BusHeader:
        if (const BusRow* b = busRowOf(h.bus)) return QRegion(QRect(0, busTop(*b), headerW, b->h));
        return QRegion(q->rect());
    case Part::EnvPoint:
    case Part::EnvLine: {
        EnvLane L;
        if (envLaneOf(h.env, &L)) return QRegion(L.clip.adjusted(-8, -8, 8, 8).toAlignedRect().intersected(lanesRect()));
        return QRegion(q->rect());
    }
    default:
        if (h.onHeader() || h.part == Part::HeaderArm) {
            if (const Row* r = rowOf(h.track)) return QRegion(QRect(0, rowTop(*r), headerW, r->h));
        }
        return QRegion(q->rect());
    }
}

void TimelineWidget::Impl::move(QMouseEvent* ev)
{
    const QPointF pos = ev->position();
    drag.lastPos = pos;
    drag.lastMods = ev->modifiers();
    if ((ev->buttons() & Qt::LeftButton) || (drag.right && (ev->buttons() & Qt::RightButton))) {
        dragTo(pos, ev->modifiers());
        return;
    }
    const Hit h = hitTest(pos);
    if (!(h == hover)) {
        q->update(hotRegion(hover) | hotRegion(h));
        hover = h;
    }
    setPointer(h, ev->modifiers());
}

void TimelineWidget::Impl::setPointer(const Hit& h, Qt::KeyboardModifiers mods)
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    const bool otherTool = (opt.tool == EditTool::Selection || opt.tool == EditTool::Zoom || opt.tool == EditTool::Envelope) &&
                           (h.part == Part::Lane || h.part == Part::EventBody || h.part == Part::BusLane);
    switch (h.part) {
    case Part::EventLeft:
    case Part::EventRight:
    case Part::FadeIn:
    case Part::FadeOut:
    case Part::HeaderLevel:
    case Part::HeaderPan:
    case Part::RegionEndTag: shape = Qt::SizeHorCursor; break;
    case Part::EventFx:
    case Part::EventCrop:
    case Part::HeaderGap: shape = Qt::SplitHCursor; break;
    case Part::HeaderEdge: shape = Qt::SplitVCursor; break;
    case Part::HeaderSurround: shape = Qt::CrossCursor; break;
    case Part::EventMenu:
    case Part::HeaderArm:
    case Part::HeaderMute:
    case Part::HeaderSolo:
    case Part::HeaderFx:
    case Part::HeaderMotion:
    case Part::HeaderMenu:
    case Part::TrackKey:
    case Part::MarkerTag: shape = Qt::PointingHandCursor; break;
    case Part::EnvPoint: shape = Qt::SizeAllCursor; break;
    case Part::EnvLine: shape = Qt::SizeVerCursor; break;
    default: break;
    }
    if (otherTool) shape = Qt::CrossCursor;
    if (h.part == Part::EventBody) {
        if (opt.tool == EditTool::Shuffle) shape = Qt::OpenHandCursor;
        else if (opt.tool == EditTool::Slip || opt.tool == EditTool::Slide) shape = Qt::SizeHorCursor;
        else if (opt.tool == EditTool::SplitTrim) shape = Qt::SplitHCursor;
    }
    // VEGAS-style pointers (cursors.h) where they say more than Qt's: trim, stretch and slip-trim on an edge, the fade
    // handles, slip, the envelopes, the Zoom / Selection / Envelope tools
    const bool ctrl = (mods & Qt::ControlModifier) != 0, alt = (mods & Qt::AltModifier) != 0;
    int own = -1;
    switch (h.part) {
    case Part::EventLeft:
    case Part::EventRight: {
        const bool left = h.part == Part::EventLeft;
        if (ctrl && alt) break;
        own = (int)(alt ? (left ? EditCursor::SlipTrimLeft : EditCursor::SlipTrimRight)
                        : ctrl ? (left ? EditCursor::StretchLeft : EditCursor::StretchRight) : (left ? EditCursor::TrimLeft : EditCursor::TrimRight));
        break;
    }
    case Part::FadeIn: own = (int)EditCursor::FadeIn; break;
    case Part::FadeOut: own = (int)EditCursor::FadeOut; break;
    case Part::EnvPoint: own = (int)EditCursor::EnvPoint; break;
    case Part::EnvLine: own = (int)EditCursor::EnvLine; break;
    case Part::EventGain: own = (int)EditCursor::EnvLine; break;
    case Part::EventFx:
    case Part::EventCrop: shape = Qt::PointingHandCursor; break;
    default: break;
    }
    if (h.part == Part::EventBody && alt && !ctrl && opt.tool == EditTool::Normal) own = (int)EditCursor::Slip;
    if (h.part == Part::EventBody && opt.tool == EditTool::Slip) own = (int)EditCursor::Slip;
    if (otherTool)
        own = (int)(opt.tool == EditTool::Zoom ? EditCursor::Zoom : opt.tool == EditTool::Envelope ? EditCursor::EnvelopeTool : EditCursor::SelectTool);
    if (own >= 0) {
        if (ownCursor != own || q->cursor().shape() != Qt::BitmapCursor) {
            ownCursor = own;
            q->setCursor(editCursor((EditCursor)own));
        }
        return;
    }
    if (ownCursor >= 0 || q->cursor().shape() != shape) {
        ownCursor = -1;
        q->setCursor(shape);
    }
}

void TimelineWidget::Impl::release(QMouseEvent* ev)
{
    if (ev->button() == Qt::RightButton && swallowRight) {
        swallowRight = false;
        rightMenuAt = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    if (ev->button() == Qt::RightButton && drag.right) {
        releaseRight(ev);
        return;
    }
    if (ev->button() != Qt::LeftButton) return;
    if (groupBypass) {
        QTimer::singleShot(0, q, [this] {
            if (!groupBypass) return;
            groupBypass = false;
            opt.ignoreGrouping = groupBypassWas;
        });
    }
    autoScroll->stop();
    const DragState was = drag;
    drag = DragState();
    snapLine = -1;
    envLabel.clear();
    releasing = true;
    struct Done { bool& flag; ~Done() { flag = false; } } done{releasing};
    const Row* row = rowOf(was.hit.track);
    const bool video = row && row->video;
    // Preferences > Editing "Collapse loop region when no time selection is present": a click in the timeline (no time
    // selection made) clears the loop region too
    if (was.kind == Drag::LoopRange && !was.started && was.hit.part != Part::LoopStrip && vprefs::on("prefs/collapseLoop") && app.loopEnd > app.loopStart)
        app.setLoop(0, 0);
    switch (was.kind) {
    case Drag::TrackHeight:
        if (was.started) app.endDrag(was.heights.size() > 1 ? "Track Heights" : "Track Height");
        break;
    case Drag::Move:
        if (was.started) {
            app.endDrag("Move Events");
            if (!was.ripple && !was.movers.empty()) {
                const Mover& g = was.movers[std::min(was.grabbed, was.movers.size() - 1)];
                const Track* t = nullptr;
                const Event* e = app.project().findEvent(g.id, &t);
                std::set<uint64_t> tracks, ids;
                double from = 1e300;
                for (const Mover& m : was.movers) {
                    if ((size_t)m.track < app.project().tracks.size()) tracks.insert(app.project().tracks[(size_t)m.track].id);
                    ids.insert(m.id);
                    from = std::min(from, m.end());
                }
                if (e && t && tracks.count(t->id)) rememberEdit(tracks, from, e->start - g.start, ids);
            }
        } else if (was.collapseOnRelease) selectGroupOf(was.hit.event);
        break;
    case Drag::TrimLeft:
    case Drag::TrimRight:
    case Drag::StretchLeft:
    case Drag::StretchRight:
        app.endDrag(was.kind == Drag::TrimLeft || was.kind == Drag::TrimRight ? "Trim Event" : "Stretch Event");
        endEdgePreview();
        if ((was.kind == Drag::TrimRight || was.kind == Drag::StretchRight) && !was.ripple && !was.movers.empty())
            if (const Event* e = app.project().findEvent(was.movers[0].id)) {
                std::set<uint64_t> tracks, ids;
                for (const Mover& m : was.movers) {
                    if ((size_t)m.track < app.project().tracks.size()) tracks.insert(app.project().tracks[(size_t)m.track].id);
                    ids.insert(m.id);
                }
                rememberEdit(tracks, was.movers[0].end(), e->length - was.movers[0].length, ids);
            }
        break;
    case Drag::Shuffle:
        if (was.started) app.endDrag("Shuffle Events");
        break;
    case Drag::Slip: app.endDrag("Slip Event"); break;
    case Drag::Slide: app.endDrag("Slide Event"); break;
    case Drag::SlipTrimLeft:
    case Drag::SlipTrimRight:
        app.endDrag("Slip Trim Event");
        endEdgePreview();
        break;
    case Drag::RollLeft:
    case Drag::RollRight:
        app.endDrag("Slide Edit");
        endEdgePreview();
        break;
    case Drag::FadeIn: app.endDrag("Fade In"); break;
    case Drag::FadeOut: app.endDrag("Fade Out"); break;
    case Drag::EnvPoints:
        if (was.started) app.endDrag("Move Envelope Points");
        break;
    case Drag::EnvSegment:
        if (was.started) app.endDrag("Move Envelope Segment");
        break;
    case Drag::EnvBrush: endEnvBrush(was); break;
    case Drag::SnapOffset:
        if (was.started) app.endDrag("Event Snap Offset");
        break;
    case Drag::EventGain:
        if (was.started) app.endDrag(was.gainVideo ? "Event Opacity" : "Event Gain");
        break;
    case Drag::Level:
    case Drag::Pan:
    case Drag::Fade:
        if (rec.on) {
            if (!rec.latch) finishRecording();
            break;
        }
        app.endDrag(was.kind == Drag::Fade ? "Track Fade" : was.kind == Drag::Pan ? "Track Pan" : video ? "Track Opacity" : "Track Volume");
        break;
    case Drag::TrackKeys:
        if (was.started) app.endDrag("Move Track Keyframe");
        break;
    case Drag::SurroundPoint:
        if (was.started) app.endDrag("Surround Pan Position");
        break;
    case Drag::Scrub:
        scrubSoundDone(was.resumePlayback);
        if (was.resumePlayback && playback) playback->play();
        break;
    case Drag::Marker:
        if (was.started) app.endDrag("Move Marker");
        break;
    case Drag::RegionStart:
    case Drag::RegionEnd:
    case Drag::RegionMove:
        if (was.started) app.endDrag("Move Region");
        break;
    case Drag::ZoomRange:
        if (was.started) {
            // the dragged area fills the view: its time span (Free Zoom and Time Zoom) and its tracks (Free Zoom and
            // Track Height Zoom)
            const double a = timeAtF(std::min(was.pressPos.x(), ev->position().x())), b = timeAtF(std::max(was.pressPos.x(), ev->position().x()));
            if (was.bandMode != 2 && b - a > 1e-6) {
                pps = clampd(viewW() / (b - a), minPps(), maxPps());
                setViewStart(a);
            }
            if (was.bandMode != 1) {
                const double y0 = std::min(was.pressPos.y(), ev->position().y()), y1 = std::max(was.pressPos.y(), ev->position().y());
                int ra = -1, rb = -1;
                for (size_t k = 0; k < rows.size(); ++k)
                    if (rows[k].h > 0 && rowTop(rows[k]) + rows[k].h > y0 && rowTop(rows[k]) < y1) {
                        if (ra < 0) ra = (int)k;
                        rb = (int)k;
                    }
                if (ra >= 0) zoomTracks(ra, rb);
            }
        } else {
            zoomToFit();
            if (!rows.empty()) zoomTracks(0, (int)rows.size() - 1);
        }
        break;
    default: break;
    }
    if (was.started && was.kind != Drag::Scrub) app.status(std::string());
    if (was.kind == Drag::None) {
        const Hit h = hitTest(ev->position());
        const bool same = h.part == was.hit.part && h.event == was.hit.event && h.track == was.hit.track;
        if (same && (h.part == Part::EventFx || h.part == Part::HeaderFx) && q->onOpenFx) {
            FxTarget t;
            t.kind = h.part == Part::EventFx ? FxTarget::Event : FxTarget::Track;
            t.id = h.part == Part::EventFx ? h.event : h.track;
            q->onOpenFx(t, 0);
        } else if (same && h.part == Part::EventCrop && q->onPanCrop) {
            q->onPanCrop(h.event);
        } else if (same && h.part == Part::HeaderMotion && q->onTrackMotion) {
            q->onTrackMotion(h.track);
        } else if (same && h.part == Part::EventMenu) {
            const Track* tr = nullptr;
            const Event* e = app.project().findEvent(h.event, &tr);
            const Row* r = (e && tr) ? rowOf(tr->id) : nullptr;
            if (r) {
                if (!app.selection().count(h.event)) selectGroupOf(h.event);
                if (q->onBeforeMenu) q->onBeforeMenu();
                QMenu menu(q);
                eventHamburger(menu, h.event);
                q->update();
                menu.exec(q->mapToGlobal(menuButtonRect(eventRect(*r, *e)).bottomLeft().toPoint() + QPoint(0, 2)));
            }
        }
    }
    QRegion gone = dragVisualsOf(was);
    if (!gone.isEmpty()) q->update(gone);
    overlaysChanged();
}

QRegion TimelineWidget::Impl::dragVisualsOf(const DragState& d) const
{
    QRegion r;
    if (d.kind == Drag::Level || d.kind == Drag::Pan || d.kind == Drag::Fade || d.kind == Drag::SurroundPoint)
        if (const Row* row = rowOf(d.hit.track)) r |= QRect(0, rowTop(*row), headerW, row->h);
    if (d.kind == Drag::HeaderWidth) r |= QRect(headerW - kHeaderGap, kTopH, kHeaderGap, std::max(0, lanesBottom() - kTopH));
    if (d.env.valid()) {
        EnvLane L;
        if (envLaneOf(d.env, &L)) r |= L.clip.adjusted(-8, -8, 8, 8).toAlignedRect().intersected(lanesRect());
    }
    return r;
}

void TimelineWidget::Impl::releaseRight(QMouseEvent* ev)
{
    autoScroll->stop();
    const DragState was = drag;
    drag = DragState();
    snapLine = -1;
    rightMenuAt = QDateTime::currentMSecsSinceEpoch();
    if (!was.started) {
        showMenu(hitTest(ev->position()), ev->globalPosition().toPoint(), timeAtF(ev->position().x()));
        rightMenuAt = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    if (was.kind == Drag::EventGain) {
        app.endDrag(was.gainVideo ? "Event Opacity" : "Event Gain");
        envLabel.clear();
        app.status(std::string());
        q->update();
        return;
    }
    if (was.kind == Drag::LoopRange) {
        app.status(std::string());
        q->update();
        return;
    }
    QMenu m(q);
    QAction* moveHere = m.addAction("&Move Here");
    QAction* copyHere = m.addAction("&Copy Here");
    QAction* shuffle = m.addAction("&Shuffle Events");
    m.addSeparator();
    m.addAction("Cance&l");
    QAction* got = q->rightDragMenu ? q->rightDragMenu(&m) : m.exec(ev->globalPosition().toPoint());
    rightMenuAt = QDateTime::currentMSecsSinceEpoch();
    if (got == moveHere) {
        app.endDrag("Move Events");
        return;
    }
    std::map<uint64_t, std::pair<uint64_t, double>> moved;
    double grabbedStart = 0;
    for (const Mover& mv : was.movers) {
        const Track* t = nullptr;
        if (const Event* e = app.project().findEvent(mv.id, &t)) {
            if (t) moved[mv.id] = {t->id, e->start};
            if (mv.id == was.hit.event) grabbedStart = e->start;
        }
    }
    app.cancelDrag();
    if (got == copyHere) {
        std::set<uint64_t> made;
        app.edit("Copy Events", [&](Project& p) {
            app.fx.captureStates(p);
            std::map<uint64_t, uint64_t> groups;
            std::vector<std::pair<uint64_t, Event>> adds;
            for (const auto& kv : moved) {
                const Event* e = p.findEvent(kv.first);
                if (!e) continue;
                Event c = *e;
                c.id = p.newId();
                c.start = kv.second.second;
                c.veg.reset();
                if (c.group) {
                    if (!groups.count(c.group)) groups[c.group] = p.newId();
                    c.group = groups[c.group];
                }
                for (FxSlot& sl : c.fx) { sl.id = p.newId(); sl.veg.reset(); }
                if (!c.transition.kind.empty()) { c.transition.id = p.newId(); c.transition.veg.reset(); }
                adds.push_back({kv.second.first, c});
            }
            for (auto& a : adds)
                if (Track* t = p.findTrack(a.first)) {
                    made.insert(a.second.id);
                    t->events.push_back(a.second);
                    t->sortEvents();
                }
        });
        if (!made.empty()) app.setSelection(made);
        return;
    }
    if (got == shuffle) {
        const Track* t = nullptr;
        const Event* e = app.project().findEvent(was.hit.event, &t);
        if (!e || !t) return;
        drag.hit = was.hit;
        drag.kind = Drag::Shuffle;
        if (!beginShuffle()) { drag = DragState(); return; }
        const double len = drag.shufEnd - drag.shufStart;
        double best = drag.shufStart, bestD = 1e300;
        for (double x : drag.shufPoints) {
            const double startThere = x >= drag.shufEnd - 1e-9 ? x - len : x;
            if (std::fabs(startThere - grabbedStart) < bestD) { bestD = std::fabs(startThere - grabbedStart); best = x; }
        }
        const std::set<uint64_t> tracks = drag.shufTracks;
        const double from = drag.shufStart, to = drag.shufEnd;
        bool ok = true;
        app.dragUpdate([&](Project& p) { ok = applyShuffle(p, tracks, from, to, best); });
        drag = DragState();
        app.endDrag("Shuffle Events");
        if (!ok) app.status("Shuffle: a locked event is in the way.");
    }
}

bool TimelineWidget::Impl::cancelMouseDrag()
{
    if (drag.kind == Drag::None) return false;
    autoScroll->stop();
    const DragState was = drag;
    drag = DragState();
    snapLine = -1;
    envLabel.clear();
    switch (was.kind) {
    case Drag::EnvBand:
        selPoints.clear();
        for (uint64_t i : was.baseSelection) selPoints.insert((int)i);
        break;
    case Drag::LoopRange: app.setLoop(was.oldLoopA, was.oldLoopB); break;
    case Drag::Scrub:
        scrubSoundDone(was.resumePlayback);
        if (was.resumePlayback && playback) playback->play();
        break;
    case Drag::RubberBand: app.setSelection(was.baseSelection); break;
    case Drag::ZoomRange: break;
    default:
        if (was.started) app.cancelDrag();
        break;
    }
    endEdgePreview();
    app.status(std::string());
    q->update();
    return true;
}

void TimelineWidget::Impl::autoScrollTick()
{
    if (drag.kind == Drag::None || drag.kind == Drag::Level || drag.kind == Drag::Pan || drag.kind == Drag::Fade || drag.kind == Drag::SurroundPoint || drag.kind == Drag::RubberBand ||
        drag.kind == Drag::ZoomRange || drag.kind == Drag::EventGain ||
        drag.kind == Drag::EnvBand || drag.kind == Drag::EnvSegment) {
        autoScroll->stop();
        return;
    }
    if (!drag.started) return;
    const double x = drag.lastPos.x();
    double over = 0;
    if (x > lanesRight() - 12) over = x - (lanesRight() - 12);
    else if (x < headerW + 12 && viewStart > 0) over = x - (headerW + 12);
    if (over == 0) return;
    const double px = (over > 0 ? 1.0 : -1.0) * clampd(std::fabs(over) * 1.5, 6.0, 80.0);
    setViewStart(viewStart + px / pps);
    dragTo(drag.lastPos, drag.lastMods);
    overlaysChanged();
}

void TimelineWidget::Impl::doubleClick(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    if (drag.started) return;
    drag = DragState();
    const Hit h = hitTest(ev->position());
    const uint64_t id = h.track;
    switch (h.part) {
    case Part::EnvLine: addEnvelopePoint(h.env, timeAtF(ev->position().x()), ev->modifiers()); break;
    case Part::TrackKey:
        app.setCursor(h.keyTime);
        if (q->onTrackMotion) q->onTrackMotion(id);
        break;
    case Part::HeaderName: startRename(Naming::Track, id); break;
    case Part::MarkerTag:
    case Part::RegionEndTag: {
        // VEGAS: a double-click on a region's (or CD track's) tag selects it; on a command marker's, its Command
        // Properties; on a marker's, its name is typed
        const int k = markerIndex(h.marker);
        if (k < 0) break;
        const Project::Marker m = app.project().markers[(size_t)k];
        if (m.kind == "command") QTimer::singleShot(0, q, [this, id = m.id] { editCommandMarker(id); });
        else if (m.region || m.length > 0) app.setLoop(m.time, m.time + m.length);
        else if (h.part == Part::MarkerTag) startRename(Naming::Marker, h.marker);
        break;
    }
    case Part::MarkerBar:
        q->setSelectionToProject();
        break;
    case Part::EventLeft:
    case Part::EventRight:
        if (edgeSel.expanded || expandedAtPress) exitEditMode();
        else {
            edgeSel = EdgeSel();
            edgeSel.event = h.event;
            edgeSel.side = h.part == Part::EventLeft ? -1 : 1;
            toggleExpandedEdit();
        }
        break;
    case Part::HeaderLevel: {
        const Row* r = rowOf(id);
        const bool video = r && r->video;
        app.edit(video ? "Track Opacity" : "Track Volume", [&](Project& p) { if (Track* t = p.findTrack(id)) t->level = 1.0; });
        break;
    }
    case Part::HeaderPan:
        app.edit("Track Pan", [&](Project& p) { if (Track* t = p.findTrack(id)) t->pan = 0.0; });
        break;
    case Part::HeaderSurround:
        setCurrentTrack(id);
        if (q->onOpenSurround) q->onOpenSurround(id);
        break;
    case Part::LoopStrip: {
        const double t = timeAtF(ev->position().x());
        double from = -1.0, to = -1.0;
        for (const Project::Marker& m : app.project().markers)
            for (const double mt : {m.time, m.region ? m.time + m.length : m.time}) {
                if (mt <= t && mt > from) from = mt;
                if (mt > t && (to < 0 || mt < to)) to = mt;
            }
        if (from >= 0 && to > from) app.setLoop(from, to);
        break;
    }
    case Part::EventBody:
    case Part::FadeIn:
    case Part::FadeOut:
        if (const Event* e = app.project().findEvent(h.event)) {
            const double a = e->start, b = e->end();
            selectGroupOf(h.event);
            app.setLoop(a, b);
        }
        break;
    default: break;
    }
}

void TimelineWidget::Impl::wheel(QWheelEvent* ev)
{
    int delta = ev->angleDelta().y();
    if (delta == 0) delta = ev->angleDelta().x();
    if (delta == 0) { ev->ignore(); return; }
    const QPointF pos = ev->position();
    const Qt::KeyboardModifiers mods = ev->modifiers();
    const double notches = delta / 120.0;
    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        // VEGAS: Ctrl+Shift+wheel moves the cursor in small steps (with Alt, by frames); in edge-trimming mode it trims
        // the selected edge instead (a pixel, with Alt a frame)
        const int dir = notches > 0 ? 1 : -1;
        const bool alt = (mods & Qt::AltModifier) != 0;
        if (edgeValid()) keypadEdit(alt ? dir : 0, alt ? 0 : dir, Qt::NoModifier);
        else if (alt) keyCursor(app.project().frameTime(std::max<int64_t>(0, app.project().frameAt(app.cursor()) + dir)));
        else keyCursor(stepFrom(app.cursor(), dir));
        ev->accept();
        return;
    }
    if (ev->angleDelta().y() == 0 && !(mods & Qt::ControlModifier)) {
        setViewStart(viewStart - notches * viewW() * 0.12 / pps);
        ev->accept();
        return;
    }
    {
        // over a track's volume or pan slider the wheel moves it (with Ctrl, finely), as in VEGAS; only on the knob
        // itself, elsewhere over the headers the wheel scrolls them
        const Hit h = hitTest(pos);
        bool onKnob = false;
        if (h.track && (h.part == Part::HeaderLevel || h.part == Part::HeaderPan))
            if (const Row* r = rowOf(h.track))
                if (const Track* tr = trackOf(*r)) {
                    const bool pan = h.part == Part::HeaderPan;
                    const QRect sr = pan ? headerRects(*r).pan : headerRects(*r).level;
                    const double value = pan ? (tr->pan + 1.0) * 0.5 : (r->video ? clampd(tr->level, 0.0, 1.0) : volumeToSlider(tr->level));
                    onKnob = std::fabs(pos.x() - (sr.left() + value * sr.width())) <= 6.0;
                }
        if (onKnob) {
            changeLevelOf(std::set<uint64_t>{h.track}, notches > 0 ? 1 : -1, h.part == Part::HeaderPan, (mods & Qt::ControlModifier) != 0);
            ev->accept();
            return;
        }
    }
    if ((mods & Qt::ControlModifier) || (pos.x() < headerW && !(mods & Qt::ShiftModifier))) {
        setScrollY(scrollY - (int)std::lround(notches * 48.0));
    } else if (mods & Qt::ShiftModifier) {
        setViewStart(viewStart - notches * viewW() * 0.12 / pps);
    } else {
        const double x = clampd(pos.x(), headerW, lanesRight());
        setZoom(pps * std::pow(1.25, notches), timeAtF(x), x);
    }
    ev->accept();
}

}  // namespace app
}  // namespace reno
