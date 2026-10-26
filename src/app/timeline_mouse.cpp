// Mouse handling of the timeline: hit testing, dragging, shuffle, automation controls and the marker tool.
#include "timeline_internal.h"

namespace reno {
namespace app {

void TimelineWidget::Impl::setCurrentTrack(uint64_t id)
{
    if (!id || app.currentTrack == id) return;
    app.currentTrack = id;
    app.notify(kSelection);
}

void TimelineWidget::Impl::userCursor(double t)
{
    if (playback) playback->jumpTo(t);
    else app.setCursor(t);
}

Mover TimelineWidget::Impl::moverOf(const Project& p, int track, const Event& e) const
{
    Mover m;
    m.id = e.id;
    m.track = track;
    m.start = e.start;
    m.length = e.length;
    m.offset = e.offset;
    m.rate = e.rate;
    m.fadeIn = e.fadeIn;
    m.fadeOut = e.fadeOut;
    m.loop = e.loop;
    if (const MediaItem* mi = p.findMedia(e.mediaId))
        if (!mi->info.isStill && !mi->generated) m.mediaLength = mi->info.duration;
    return m;
}

int TimelineWidget::Impl::trackIndex(uint64_t id) const
{
    const Project& p = app.project();
    for (size_t k = 0; k < p.tracks.size(); ++k) if (p.tracks[k].id == id) return (int)k;
    return -1;
}

bool TimelineWidget::Impl::pressRight(QMouseEvent* ev, const QPointF& pos)
{
    if (ev->button() != Qt::RightButton) return false;
    if (drag.kind == Drag::ZoomRange) {
        drag.bandMode = (drag.bandMode + 1) % 3;
        swallowRight = true;
        app.status(drag.bandMode == 0 ? "Free Zoom" : drag.bandMode == 1 ? "Time Zoom" : "Track Height Zoom");
        q->update();
        return true;
    }
    if (drag.kind == Drag::LoopRange && !drag.right && opt.tool == EditTool::Normal && (ev->buttons() & Qt::LeftButton)) {
        // VEGAS's "instant access to the Selection tool from the Normal tool": with the left button held over empty
        // timeline, a right click turns the time selection into the Selection tool's rectangle (more right clicks
        // cycle width only, height only, free).
        app.setLoop(drag.oldLoopA, drag.oldLoopB);
        drag.kind = Drag::RubberBand;
        drag.started = false;
        drag.baseSelection = app.selection();
        drag.bandMode = 0;
        swallowRight = true;
        snapLine = -1;
        updateRubberBand(drag.lastPos, ev->modifiers());
        q->update();
        return true;
    }
    if (drag.kind == Drag::RubberBand) {
        drag.bandMode = (drag.bandMode + 1) % 3;
        swallowRight = true;
        updateRubberBand(drag.lastPos, ev->modifiers());
        app.status(drag.bandMode == 0 ? "Selection: rectangle" : drag.bandMode == 1 ? "Selection: time" : "Selection: tracks");
        q->update();
        return true;
    }
    if (drag.kind != Drag::None) return false;

    const Hit h = hitTest(pos);
    const bool eventTool = opt.tool != EditTool::Envelope && opt.tool != EditTool::Selection && opt.tool != EditTool::Zoom;
    if (h.part == Part::EventGain && h.event && eventTool) {
        drag = DragState();
        drag.pressPos = drag.lastPos = pos;
        drag.lastMods = ev->modifiers();
        drag.pressTime = timeAtF(pos.x());
        drag.hit = h;
        drag.right = true;
        pressGain(h, ev->modifiers(), true);
    } else if (h.part == Part::EventBody && h.event && eventTool && !ev->modifiers()) {
        drag = DragState();
        drag.pressPos = drag.lastPos = pos;
        drag.pressTime = timeAtF(pos.x());
        drag.hit = h;
        drag.kind = Drag::Move;
        drag.right = true;
        if (!app.selection().count(h.event)) selectGroupOf(h.event);
    } else if (h.part == Part::Lane && opt.tool == EditTool::Normal && !app.prefs.rightMouseSelect && !ev->modifiers()) {
        // VEGAS 23: the right button dragged over empty timeline marks the time selection (a click still shows the menu)
        drag = DragState();
        drag.pressPos = drag.lastPos = pos;
        drag.pressTime = timeAtF(pos.x());
        drag.hit = h;
        drag.right = true;
        drag.snaps = snapTargets(std::set<uint64_t>(), false);
        double line = -1;
        drag.anchor = snapped(drag.snaps, drag.pressTime, ev->modifiers(), &line);
        drag.oldLoopA = app.loopStart;
        drag.oldLoopB = app.loopEnd;
        drag.kind = Drag::LoopRange;
    }
    return true;
}

void TimelineWidget::Impl::selectTracksForPress(const Hit& h, Qt::KeyboardModifiers mods)
{
    const bool ctrl = (mods & Qt::ControlModifier) != 0, shift = (mods & Qt::ShiftModifier) != 0;
    bool keepCurrent = false;
    const bool control = h.part == Part::HeaderMute || h.part == Part::HeaderSolo || h.part == Part::HeaderLevel || h.part == Part::HeaderPan ||
                         h.part == Part::HeaderFade || h.part == Part::HeaderAuto || h.part == Part::HeaderPhase || h.part == Part::HeaderBus ||
                         h.part == Part::HeaderFx;
    if (control && h.track && selectedTracks().count(h.track) && selectedTracks().size() > 1) {
        trackListFocus = true;
        keepCurrent = true;
    } else if (h.onHeader() && h.track && h.part != Part::HeaderGap) {
        // a click on a header gives the track list the keyboard (Tab goes back); Ctrl+click and Shift+click select more
        // tracks, as in VEGAS
        trackListFocus = true;
        if (ctrl) {
            std::set<uint64_t> all = selectedTracks();
            if (all.count(h.track) && all.size() > 1) {
                all.erase(h.track);
                selTracks = all;
                if (app.currentTrack == h.track) {
                    app.currentTrack = *all.begin();
                    app.notify(kSelection);
                }
                keepCurrent = true;
            } else {
                all.insert(h.track);
                selTracks = all;
            }
        } else if (shift && app.currentTrack) {
            const int a = trackIndex(app.currentTrack), b = trackIndex(h.track);
            selTracks.clear();
            const Project& pr = app.project();
            for (int k = std::min(a, b); k >= 0 && k <= std::max(a, b) && k < (int)pr.tracks.size(); ++k) selTracks.insert(pr.tracks[(size_t)k].id);
        } else {
            selTracks.clear();
        }
    } else if (h.track && (h.part == Part::Lane || h.onEvent())) {
        trackListFocus = false;
        if (!ctrl && !shift) selTracks.clear();
    }
    if (h.track && !keepCurrent) setCurrentTrack(h.track);
}

void TimelineWidget::Impl::pressRuler(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    const bool ctrl = (mods & Qt::ControlModifier) != 0;
    drag.snaps = snapTargets(std::set<uint64_t>(), !ctrl);
    if (!ctrl) {
        beginScrub(pos, mods);
        return;
    }
    double line = -1;
    drag.kind = Drag::LoopRange;
    drag.oldLoopA = app.loopStart;
    drag.oldLoopB = app.loopEnd;
    drag.anchor = snapped(drag.snaps, drag.pressTime, mods, &line);
}

void TimelineWidget::Impl::pressLoopStrip(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    drag.kind = Drag::LoopRange;
    drag.oldLoopA = app.loopStart;
    drag.oldLoopB = app.loopEnd;
    drag.snaps = snapTargets(std::set<uint64_t>(), true);
    const bool region = app.loopEnd > app.loopStart;
    if (region && std::fabs(pos.x() - xAtF(app.loopStart)) <= 6.0) {
        drag.anchor = app.loopEnd;
        drag.started = true;
    } else if (region && std::fabs(pos.x() - xAtF(app.loopEnd)) <= 6.0) {
        drag.anchor = app.loopStart;
        drag.started = true;
    } else {
        double line = -1;
        drag.anchor = snapped(drag.snaps, drag.pressTime, mods, &line);
    }
}

void TimelineWidget::Impl::pressMarkerTag(const Hit& h, Qt::KeyboardModifiers mods)
{
    const int k = markerIndex(h.marker);
    if (k < 0) return;
    const Project::Marker m = app.project().markers[(size_t)k];
    if (markerTool) chooseMarker(m.id, mods);
    userCursor(h.part == Part::RegionEndTag ? m.time + m.length : m.time);
    drag.markerTime = m.time;
    drag.markerLength = m.length;
    drag.snaps = snapTargets(std::set<uint64_t>(), false, m.id);
    if (!m.region) drag.kind = Drag::Marker;
    else if (mods & Qt::AltModifier) drag.kind = Drag::RegionMove;
    else drag.kind = h.part == Part::RegionEndTag ? Drag::RegionEnd : Drag::RegionStart;
}

void TimelineWidget::Impl::pressLane(const Hit& h, Qt::KeyboardModifiers mods)
{
    bool ctrl = (mods & Qt::ControlModifier) != 0, shift = (mods & Qt::ShiftModifier) != 0;
    if (opt.tool == EditTool::Envelope) {
        if (!ctrl && !shift) selPoints.clear();
        drag.kind = Drag::EnvBand;
        return;
    }
    if (opt.tool == EditTool::Zoom) {
        drag.kind = Drag::ZoomRange;
        return;
    }
    if (opt.tool == EditTool::Selection) {
        std::set<uint64_t> sel = ctrl || shift ? app.selection() : std::set<uint64_t>();
        if (h.event) {
            if (ctrl && sel.count(h.event)) sel.erase(h.event);
            else sel.insert(h.event);
        }
        app.setSelection(sel);
        drag.baseSelection = sel;
        drag.kind = Drag::RubberBand;
        return;
    }
    if (h.part == Part::EventBody && opt.tool == EditTool::Shuffle && !(mods & (Qt::AltModifier | Qt::ControlModifier))) {
        if (!app.selection().count(h.event)) selectGroupOf(h.event);
        drag.kind = Drag::Shuffle;
        return;
    }
    if (h.part == Part::EventBody) {
        mods = toolModifiers(mods, false);
        ctrl = (mods & Qt::ControlModifier) != 0;
        shift = (mods & Qt::ShiftModifier) != 0;
    }
    if (h.part == Part::EventBody && (mods & Qt::AltModifier)) {
        pressSlipOrSlide(h, ctrl, shift);
        return;
    }
    if (h.part == Part::EventBody && !(ctrl && shift)) {
        pressEvent(h, mods);
        return;
    }
    // Ctrl+Shift+drag over events makes a time selection, as a drag over empty space does: nothing selected, the cursor
    // here; a drag from here marks out a loop region (VEGAS's time selection)
    if (!ctrl && !shift) app.setSelection(std::set<uint64_t>());
    drag.snaps = snapTargets(std::set<uint64_t>(), false);
    double line = -1;
    const double t = snapped(drag.snaps, drag.pressTime, mods, &line);
    userCursor(t);
    if (opt.tool == EditTool::Normal && !app.prefs.rightMouseSelect && h.part == Part::Lane) {
        drag.baseSelection = app.selection();
        drag.kind = Drag::RubberBand;
        return;
    }
    drag.kind = Drag::LoopRange;
    drag.oldLoopA = app.loopStart;
    drag.oldLoopB = app.loopEnd;
    drag.anchor = t;
}

// Alt on an event: slip the media inside the event; Ctrl+Alt: slide the event over its media; Alt+Shift: slip trim its
// end; Ctrl+Alt+Shift: split it here and trim the way the mouse goes.
void TimelineWidget::Impl::pressSlipOrSlide(const Hit& h, bool ctrl, bool shift)
{
    if (!app.selection().count(h.event)) selectGroupOf(h.event);
    if (ctrl && shift) {
        pressSplitTrim(h);
        return;
    }
    if (ctrl) {
        const Track* t = nullptr;
        const Event* e = app.project().findEvent(h.event, &t);
        if (e && t) {
            if (const Event* a = fadesFrom(*t, *e))
                if (drag.pressTime <= a->end()) { drag.kind = Drag::RollLeft; return; }
            for (const Event& b : t->events)
                if (fadesFrom(*t, b) == e && drag.pressTime >= b.start) { drag.kind = Drag::RollRight; return; }
        }
    }
    drag.kind = ctrl ? Drag::Slide : shift ? Drag::SlipTrimRight : Drag::Slip;
}

void TimelineWidget::Impl::pressEdge(const Hit& h, Qt::KeyboardModifiers mods)
{
    if (!app.selection().count(h.event)) selectGroupOf(h.event);
    if (h.part == Part::FadeIn) {
        drag.kind = Drag::FadeIn;
        return;
    }
    if (h.part == Part::FadeOut) {
        drag.kind = Drag::FadeOut;
        return;
    }
    // Ctrl: time stretch; Alt: slip trim; Ctrl+Alt: slide the edit; Shift: grouped events stay as they are
    // the Slip, Slide and Time Stretch tools: as if Alt, Ctrl+Alt or Ctrl were held
    mods = toolModifiers(mods, true);
    const bool ctrl = (mods & Qt::ControlModifier) != 0, shift = (mods & Qt::ShiftModifier) != 0;
    const bool left = h.part == Part::EventLeft, alt = (mods & Qt::AltModifier) != 0;
    if (ctrl && alt) drag.kind = left ? Drag::RollLeft : Drag::RollRight;
    else if (alt) drag.kind = left ? Drag::SlipTrimLeft : Drag::SlipTrimRight;
    else if (ctrl) drag.kind = left ? Drag::StretchLeft : Drag::StretchRight;
    else drag.kind = left ? Drag::TrimLeft : Drag::TrimRight;
    drag.ignoreGroup = shift;
}

void TimelineWidget::Impl::pressSnapFlag(const Hit& h)
{
    const Event* e = app.project().findEvent(h.event);
    if (!e) return;
    if (!app.selection().count(h.event)) selectGroupOf(h.event);
    drag.kind = Drag::SnapOffset;
    drag.markerTime = e->snapOffset;
    drag.snaps = snapTargets(std::set<uint64_t>{h.event}, true);
}

void TimelineWidget::Impl::pressMuteSolo(const Hit& h, bool ctrl)
{
    const uint64_t id = h.track;
    const bool solo = h.part == Part::HeaderSolo;
    const Track* ct = findTrackConst(id);
    if (!ct) return;
    if (!solo && ct->automation) {
        // VEGAS's Mute Automation button: the automation's mute (Off, or no mute envelope), the mute envelope at the
        // cursor (Touch / Latch; while playing, at the playing position), nothing in Read
        const bool now = shownAutomation(*ct, "mute") >= 0.5;
        const std::vector<uint64_t> ids = controlTracks(id);
        const double at = recordingWanted(id) ? playback->position() : quantized(app.cursor());
        std::string why;
        app.edit("Mute Automation", [&](Project& p) { why = applyAutomation(p, ids, "mute", now ? 0.0 : 1.0, at, frame()); });
        if (!why.empty()) app.status(why);
        return;
    }
    const bool on = solo ? ct->solo : ct->mute;
    const std::vector<uint64_t> ids = controlTracks(id);
    app.edit(solo ? "Solo Track" : "Mute Track", [&](Project& p) {
        if (ctrl) {
            // VEGAS's Ctrl+click: on a track that is off, it alone (the others out of the mute / solo group); on one
            // that is on, every one of them off
            for (Track& t : p.tracks) {
                bool& sw = solo ? t.solo : t.mute;
                sw = !on && t.id == id;
            }
            return;
        }
        for (uint64_t x : ids)
            if (Track* t = p.findTrack(x)) {
                if (solo) t->solo = !on;
                else t->mute = !on;
            }
    });
}

void TimelineWidget::Impl::pressInvertPhase(const Hit& h)
{
    const Track* ct = findTrackConst(h.track);
    if (!ct) return;
    const bool on = !ct->invertPhase;
    const std::vector<uint64_t> ids = controlTracks(h.track);
    app.edit("Invert Track Phase", [&](Project& p) {
        for (uint64_t x : ids)
            if (Track* t = p.findTrack(x)) t->invertPhase = on;
    });
}

void TimelineWidget::Impl::press(QMouseEvent* ev)
{
    const QPointF pos = ev->position();
    q->setFocus(Qt::MouseFocusReason);
    if (pressRight(ev, pos)) return;

    // VEGAS: the right button held and then the left pressed on an event moves that event alone, out of its group (the
    // right-drag the right button began is dropped; its release shows no menu)
    if (ev->button() == Qt::LeftButton && drag.right && !drag.started && (ev->buttons() & Qt::RightButton) && opt.tool == EditTool::Normal) {
        drag = DragState();
        const Hit h0 = hitTest(pos);
        if (h0.part == Part::EventBody) {
            groupBypass = true;
            groupBypassWas = opt.ignoreGrouping;
            opt.ignoreGrouping = true;
            swallowRight = true;
            app.setSelection(std::set<uint64_t>{h0.event});
        }
    }
    if (ev->button() != Qt::LeftButton || drag.kind != Drag::None) return;
    drag = DragState();
    drag.pressPos = drag.lastPos = pos;
    drag.lastMods = ev->modifiers();
    drag.pressTime = timeAtF(pos.x());
    drag.hit = hitTest(pos);
    const Hit h = drag.hit;
    if (markerTool && h.part != Part::MarkerTag && h.part != Part::RegionEndTag && h.part != Part::MarkerBar) setMarkerTool(false);
    if (h.part == Part::HeaderEdge) {
        // a track header's bottom edge: the track is resized (with the other selected tracks if it is one of them: VEGAS
        // resizes all the selected tracks together)
        pressTrackEdge(h);
        return;
    }
    const Qt::KeyboardModifiers mods = ev->modifiers();
    const bool ctrl = (mods & Qt::ControlModifier) != 0;
    expandedAtPress = edgeSel.expanded;
    if (edgeSel.event) edgeSel = EdgeSel();
    selectTracksForPress(h, mods);
    if (!h.onEnvelope() && opt.tool != EditTool::Envelope && !selPoints.empty()) selPoints.clear();
    switch (h.part) {
    case Part::EnvPoint:
    case Part::EnvLine:
        if (h.part == Part::EnvLine && (mods & Qt::ShiftModifier) && !(mods & (Qt::ControlModifier | Qt::AltModifier))) pressEnvBrush(h);
        else pressEnvelope(h, mods);
        break;
    case Part::Ruler:
        pressRuler(pos, mods);
        break;
    case Part::LoopStrip:
        pressLoopStrip(pos, mods);
        break;
    case Part::MarkerBar:
        beginScrub(pos, mods);
        break;
    case Part::MarkerTag:
    case Part::RegionEndTag:
        pressMarkerTag(h, mods);
        break;
    case Part::TrackKey:
        drag.kind = Drag::TrackKeys;
        break;
    case Part::Lane:
    case Part::EventBody:
    case Part::BusLane:
        pressLane(h, mods);
        break;
    case Part::EventLeft:
    case Part::EventRight:
    case Part::FadeIn:
    case Part::FadeOut:
        pressEdge(h, mods);
        break;
    case Part::EventFx:
    case Part::EventCrop:
    case Part::EventMenu:
        if (!app.selection().count(h.event)) selectGroupOf(h.event);
        break;
    case Part::EventGain:
        pressGain(h, mods, false);
        break;
    case Part::EventSnap:
        pressSnapFlag(h);
        break;
    case Part::HeaderMenu: {
        const Row* r = rowOf(h.track);
        const QPoint below = r ? headerRects(*r).menu.bottomLeft() + QPoint(0, 2) : pos.toPoint();
        drag = DragState();
        setCurrentTrack(h.track);
        if (q->onBeforeMenu) q->onBeforeMenu();
        QMenu menu(q);
        trackHamburger(menu, h.track);
        q->update();
        menu.exec(q->mapToGlobal(below));
        return;
    }
    case Part::HeaderGap:
        drag.kind = Drag::HeaderWidth;
        drag.started = true;
        drag.startValue = headerW;
        break;
    case Part::HeaderArm: {
        const uint64_t id = h.track;
        if (q->onArmTrack) q->onArmTrack(id);
        else {
            if (app.armedTracks.count(id)) app.armedTracks.erase(id);
            else app.armedTracks.insert(id);
            app.notify(kSelection);
        }
        break;
    }
    case Part::HeaderMute:
    case Part::HeaderSolo:
        pressMuteSolo(h, ctrl);
        break;
    case Part::HeaderLevel:
    case Part::HeaderPan:
    case Part::HeaderFade:
        beginSlider(h, pos, h.part == Part::HeaderPan);
        break;
    case Part::HeaderAuto: {
        const Row* r = rowOf(h.track);
        const QRect b = r ? headerRects(*r).autoBtn : QRect();
        if (!b.isEmpty() && pos.x() >= b.right() - 6) {
            QMenu m(q);
            automationMenu(m, h.track);
            drag = DragState();
            m.exec(q->mapToGlobal(b.bottomLeft() + QPoint(0, 2)));
            return;
        }
        toggleAutomation(h.track);
        break;
    }
    case Part::HeaderPhase:
        pressInvertPhase(h);
        break;
    case Part::HeaderBus: {
        const Row* r = rowOf(h.track);
        QMenu m(q);
        busChoiceMenu(m, h.track);
        drag = DragState();
        m.exec(q->mapToGlobal(r ? headerRects(*r).bus.bottomLeft() + QPoint(0, 2) : pos.toPoint()));
        return;
    }
    case Part::HeaderSurround:
        if (const Row* r = rowOf(h.track))
            if (const Track* t = trackOf(*r))
                if (t->surround.route == "surround") drag.kind = Drag::SurroundPoint;
        break;
    default:
        break;
    }
    const bool dragsView = drag.kind != Drag::None && drag.kind != Drag::Level && drag.kind != Drag::Pan && drag.kind != Drag::Fade &&
                           drag.kind != Drag::HeaderWidth && drag.kind != Drag::SurroundPoint;
    if (dragsView) autoScroll->start();
    overlaysChanged();
}

// The keys the normal tool would need for what the current tool does with a plain drag (see EditTool), on an event's
// body ('edge' false) or on an edge. Ctrl or Alt held: the keys as they are (they win over the tool).
Qt::KeyboardModifiers TimelineWidget::Impl::toolModifiers(Qt::KeyboardModifiers mods, bool edge) const
{
    if (mods & (Qt::ControlModifier | Qt::AltModifier)) return mods;
    const Qt::KeyboardModifiers shift = mods & Qt::ShiftModifier;
    switch (opt.tool) {
    case EditTool::Slip: return Qt::AltModifier | (edge ? Qt::NoModifier : shift);
    case EditTool::Slide: return Qt::ControlModifier | Qt::AltModifier;
    case EditTool::SplitTrim: return edge ? mods : (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    case EditTool::Stretch: return edge ? (Qt::ControlModifier | shift) : mods;
    default: return mods;
    }
}

bool TimelineWidget::Impl::beginShuffle()
{
    const Track* t = nullptr;
    const Event* e = app.project().findEvent(drag.hit.event, &t);
    if (!e || !t) return false;
    if (e->locked) { app.status("A locked event cannot be shuffled."); return false; }
    drag.shufStart = e->start;
    drag.shufEnd = e->end();
    drag.shufTo = e->start;
    drag.shufTracks.clear();
    drag.shufTracks.insert(t->id);
    for (uint64_t id : withGroups(std::set<uint64_t>{e->id})) {
        const Track* gt = nullptr;
        const Event* g = app.project().findEvent(id, &gt);
        if (g && gt && g->start >= e->start - 1e-9 && g->start < e->end() - 1e-9) drag.shufTracks.insert(gt->id);
    }
    std::set<double> pts;
    pts.insert(e->start);
    for (const Event& o : t->events) {
        if (o.id == e->id) continue;
        for (double x : {o.start, o.end()})
            if (x <= e->start + 1e-9 || x >= e->end() - 1e-9) pts.insert(x);
    }
    drag.shufPoints.assign(pts.begin(), pts.end());
    app.beginDrag();
    drag.started = true;
    return true;
}

// The shuffle of the events of 'tracks' that start in [from, to) to 'place' (an edit point; see beginShuffle). False
// (and nothing changed) if a locked event would have to move.
bool TimelineWidget::Impl::applyShuffle(Project& p, const std::set<uint64_t>& tracks, double from, double to, double place)
{
    const double len = to - from, eps = 1e-9;
    if (len <= 0 || (place > from - eps && place < to + eps)) return true;
    for (int pass = 0; pass < 2; ++pass)
        for (Track& t : p.tracks) {
            if (!tracks.count(t.id)) continue;
            for (Event& e : t.events) {
                double d = 0;
                const bool block = e.start >= from - eps && e.start < to - eps;
                if (place >= to) {
                    if (block) d = place - to;
                    else if (e.start >= to - eps && e.start < place - eps) d = -len;
                } else {
                    if (block) d = place - from;
                    else if (e.start >= place - eps && e.start < from - eps) d = len;
                }
                if (d == 0) continue;
                if (pass == 0 && e.locked) return false;
                if (pass == 1) e.start = std::max(0.0, e.start + d);
            }
            if (pass == 1) t.sortEvents();
        }
    return true;
}

void TimelineWidget::Impl::updateShuffle(const QPointF& pos)
{
    if (drag.shufPoints.empty()) return;
    const double want = drag.shufStart + timeAtF(pos.x()) - drag.pressTime;
    const double len = drag.shufEnd - drag.shufStart;
    double best = drag.shufStart, bestD = 1e300;
    for (double x : drag.shufPoints) {
        const double startThere = x >= drag.shufEnd - 1e-9 ? x - len : x;
        const double dist = std::fabs(startThere - want);
        if (dist < bestD) { bestD = dist; best = x; }
    }
    drag.shufTo = best;
    const std::set<uint64_t> tracks = drag.shufTracks;
    const double from = drag.shufStart, to = drag.shufEnd;
    bool ok = true;
    app.dragUpdate([&](Project& p) { ok = applyShuffle(p, tracks, from, to, best); });
    snapLine = (best < from - 1e-9 || best > to + 1e-9) ? best : -1;
    if (!ok) app.status("Shuffle: a locked event is in the way.");
    else if (snapLine < 0) app.status("Shuffle: drag the event before or after the events beside it.");
    else app.status("Shuffle: to " + timecode(best >= to ? best - (to - from) : best, fps()));
}

std::set<uint64_t> TimelineWidget::Impl::withGroup(uint64_t id) const
{
    std::set<uint64_t> out{id};
    const Event* e = app.project().findEvent(id);
    if (!e || !e->group || opt.ignoreGrouping) return out;
    for (const auto& t : app.project().tracks)
        for (const auto& o : t.events)
            if (o.group == e->group) out.insert(o.id);
    return out;
}

void TimelineWidget::Impl::beginScrub(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    drag.kind = Drag::Scrub;
    drag.started = true;
    drag.snaps = snapTargets(std::set<uint64_t>(), false);
    if (playback && playback->playing()) {
        playback->pause();
        drag.resumePlayback = true;
    }
    scrub(pos, mods);
}

// A press on an event with the normal edit tool: select it (Ctrl adds or removes, Shift adds), put the cursor there, and
// get ready to move it.
void TimelineWidget::Impl::pressEvent(const Hit& h, Qt::KeyboardModifiers mods)
{
    const bool ctrl = (mods & Qt::ControlModifier) != 0, shift = (mods & Qt::ShiftModifier) != 0;
    std::set<uint64_t> sel = app.selection();
    const bool was = sel.count(h.event) != 0;
    bool canMove = true;
    if (ctrl) {
        for (uint64_t g : withGroup(h.event)) {
            if (was) sel.erase(g);
            else sel.insert(g);
        }
        app.setSelection(sel);
        canMove = !was;
    } else if (shift) {
        // VEGAS: Shift+click selects the range from the event clicked last to this one (the events between them in time,
        // on the tracks between them)
        const Track* ta = nullptr;
        const Track* tb = nullptr;
        const Event* a = clickAnchor ? app.project().findEvent(clickAnchor, &ta) : nullptr;
        const Event* b = app.project().findEvent(h.event, &tb);
        for (uint64_t g : withGroup(h.event)) sel.insert(g);
        if (a && b && ta && tb) {
            const int r0 = std::min(trackIndex(ta->id), trackIndex(tb->id)), r1 = std::max(trackIndex(ta->id), trackIndex(tb->id));
            const double t0 = std::min(a->start, b->start), t1 = std::max(a->end(), b->end());
            const Project& pr = app.project();
            for (int k = r0; k >= 0 && k <= r1 && k < (int)pr.tracks.size(); ++k)
                for (const Event& e : pr.tracks[(size_t)k].events)
                    if (e.start >= t0 - 1e-9 && e.end() <= t1 + 1e-9) sel.insert(e.id);
        }
        app.setSelection(sel);
    } else if (!was) {
        selectGroupOf(h.event);
    } else {
        drag.collapseOnRelease = sel.size() > withGroup(h.event).size();
    }
    if (!shift) clickAnchor = h.event;
    double line = -1;
    userCursor(snapped(snapTargets(std::set<uint64_t>(), false), drag.pressTime, mods, &line));
    if (canMove) drag.kind = Drag::Move;
}

void TimelineWidget::Impl::toggleMinimized(uint64_t trackId)
{
    if (!trackId) return;
    maximized.erase(trackId);
    if (!minimized.erase(trackId)) minimized.insert(trackId);
    layout();
    updateBars();
    q->update();
}

void TimelineWidget::Impl::toggleMaximized(uint64_t trackId)
{
    if (!trackId) return;
    minimized.erase(trackId);
    const bool on = !maximized.erase(trackId);
    if (on) maximized.insert(trackId);
    layout();
    updateBars();
    if (on)
        if (const Row* r = rowOf(trackId)) setScrollY(r->y);
    q->update();
}

const Track* TimelineWidget::Impl::findTrackConst(uint64_t id) const
{
    for (const Track& t : app.project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

// The tracks a header control works on: every selected track of the same kind, when the one clicked is among them
// (VEGAS: "if multiple tracks are selected, all selected tracks are adjusted"), else just it.
std::vector<uint64_t> TimelineWidget::Impl::controlTracks(uint64_t id) const
{
    std::vector<uint64_t> out;
    const Track* t = findTrackConst(id);
    if (!t) return out;
    const std::set<uint64_t> sel = selectedTracks();
    if (sel.count(id) && sel.size() > 1) {
        for (const Track& o : app.project().tracks)
            if (sel.count(o.id) && o.type == t->type) out.push_back(o.id);
    } else {
        out.push_back(id);
    }
    return out;
}

double& TimelineWidget::Impl::autoValueOf(Track& t, const std::string& kind)
{
    if (kind == "pan") return t.autoPan;
    if (kind == "fadecolor") return t.autoFade;
    return t.autoLevel;
}

void TimelineWidget::Impl::setEnvelopeAt(Envelope& e, double at, double v, double frame, const std::string& kind)
{
    for (EnvPoint& pt : e.points)
        if (std::fabs(pt.time - at) <= frame * 0.5) {
            pt.value = v;
            return;
        }
    EnvPoint np;
    np.time = std::max(0.0, at);
    np.value = v;
    size_t k = 0;
    while (k < e.points.size() && e.points[k].time <= at) ++k;
    np.curve = kind == "mute" ? (int)CurveHold : k > 0 ? e.points[k - 1].curve : AppState::envelopeDefaultCurve(kind);
    e.points.insert(e.points.begin() + (std::ptrdiff_t)k, np);
}

Envelope& TimelineWidget::Impl::ensureTrackEnvelope(Track& t, const std::string& kind)
{
    Envelope* e = findEnvelope(t.envelopes, kind);
    if (!e) {
        Envelope ne;
        ne.kind = kind;
        t.envelopes.push_back(ne);
        e = &t.envelopes.back();
    }
    if (e->points.empty()) {
        EnvPoint p0;
        p0.value = kind == "mute" ? (t.autoMute ? 1.0 : 0.0) : autoValueOf(t, kind);
        p0.curve = kind == "mute" ? (int)CurveHold : AppState::envelopeDefaultCurve(kind);
        e->points.push_back(p0);
    }
    e->visible = true;
    return *e;
}

// A control moved in automation mode (not while recording): Off - the automation value; Read - the automation value
// where there is no envelope, else nothing (the envelope sets it); Touch / Latch - the envelope's point at the cursor.
std::string TimelineWidget::Impl::applyAutomation(Project& p, const std::vector<uint64_t>& ids, const std::string& kind, double value, double at, double frame)
{
    std::string why;
    for (uint64_t id : ids) {
        Track* t = p.findTrack(id);
        if (!t || !t->automation) continue;
        const std::string k = (kind == "volume" && t->type == TrackType::Video) ? std::string("composite") : kind;
        const Envelope* e = findEnvelope(t->envelopes, k);
        const bool has = e && !e->points.empty();
        if (t->automationMode == "off" || (!has && t->automationMode == "read")) {
            if (k == "mute") t->autoMute = value >= 0.5;
            else autoValueOf(*t, k) = value;
            continue;
        }
        if (t->automationMode == "read") {
            why = "Automation Read: the envelope sets it (choose Touch or Latch on the Automation Settings button to change it)";
            continue;
        }
        setEnvelopeAt(ensureTrackEnvelope(*t, k), at, value, frame, k);
    }
    return why;
}

bool TimelineWidget::Impl::headersFollowCursor() const
{
    for (const Track& t : app.project().tracks)
        if (t.automation && !t.automationOff())
            for (const Envelope& e : t.envelopes)
                if (e.points.size() > 1 && (e.kind == "volume" || e.kind == "pan" || e.kind == "composite" || e.kind == "fadecolor" || e.kind == "mute"))
                    return true;
    return false;
}

void TimelineWidget::Impl::toggleAutomation(uint64_t id)
{
    const Track* t = findTrackConst(id);
    if (!t) return;
    const bool on = !t->automation;
    const std::vector<uint64_t> ids = controlTracks(id);
    app.edit(on ? "Automation Settings On" : "Automation Settings Off", [&](Project& p) {
        for (uint64_t x : ids)
            if (Track* o = p.findTrack(x)) o->automation = on;
    });
    layout();
    q->update();
}

void TimelineWidget::Impl::setAutomationMode(uint64_t id, const std::string& mode)
{
    const std::vector<uint64_t> ids = controlTracks(id);
    const double at = app.cursor();
    app.edit("Automation Mode", [&](Project& p) {
        for (uint64_t x : ids)
            if (Track* o = p.findTrack(x)) {
                if (mode == "off" && !o->automationOff()) {
                    const bool video = o->type == TrackType::Video;
                    o->autoLevel = trackAutomationValue(*o, video ? "composite" : "volume", at);
                    if (!video) o->autoPan = trackAutomationValue(*o, "pan", at);
                    else o->autoFade = trackAutomationValue(*o, "fadecolor", at);
                    o->autoMute = trackAutomationValue(*o, "mute", at) >= 0.5;
                }
                o->automationMode = mode;
                o->automation = true;
            }
    });
    layout();
    q->update();
}

void TimelineWidget::Impl::automationMenu(QMenu& m, uint64_t id)
{
    const Track* t = findTrackConst(id);
    if (!t) return;
    toggleItem(m, "Activate Automation Controls", t->automation, [this, id] { toggleAutomation(id); });
    m.addSeparator();
    automationModes(m, id);
}

void TimelineWidget::Impl::automationModes(QMenu& m, uint64_t id)
{
    const Track* t = findTrackConst(id);
    if (!t) return;
    static const char* const kModes[][2] = {{"off", "Off"}, {"read", "Read"}, {"touch", "Automation Write (Touch)"}, {"latch", "Automation Write (Latch)"}};
    for (const auto& md : kModes) {
        const std::string mode = md[0];
        toggleItem(m, md[1], t->automation && t->automationMode == mode, [this, id, mode] { setAutomationMode(id, mode); });
    }
}

void TimelineWidget::Impl::setLayersExpanded(uint64_t id, bool on)
{
    app.edit(on ? "Expand Track Layers" : "Collapse Track Layers", [&](Project& p) { if (Track* o = p.findTrack(id)) o->layersExpanded = on; });
    layout();
    updateBars();
    q->update();
}

void TimelineWidget::Impl::trackHamburger(QMenu& menu, uint64_t id)
{
    const Track* t = findTrackConst(id);
    const Row* r = rowOf(id);
    if (!t || !r) return;
    const bool video = t->type == TrackType::Video;
    auto openFxOf = [this, id] {
        if (!q->onOpenFx) return;
        FxTarget ft;
        ft.kind = FxTarget::Track;
        ft.id = id;
        q->onOpenFx(ft, 0);
    };
    if (video) {
        const bool on = t->bypassMotionBlur;
        toggleItem(menu, "Bypass Motion Blur", on, [this, id, on] {
            app.edit("Bypass Motion Blur", [&](Project& p) { if (Track* o = p.findTrack(id)) o->bypassMotionBlur = !on; });
        });
        add(menu, "Track Motion", [this, id] { if (q->onTrackMotion) q->onTrackMotion(id); }, (bool)q->onTrackMotion);
    } else {
        if (q->onArmTrack) toggleItem(menu, "Arm for Record", app.armedTracks.count(id) != 0, [this, id] { if (q->onArmTrack) q->onArmTrack(id); });
        const bool inv = t->invertPhase;
        toggleItem(menu, "Invert Track Phase", inv, [this, id, inv] {
            app.edit("Invert Phase", [&](Project& p) { if (Track* o = p.findTrack(id)) o->invertPhase = !inv; });
        });
    }
    add(menu, "Track FX", openFxOf);
    toggleItem(menu, "Activate Automation Controls", t->automation, [this, id] { toggleAutomation(id); });
    automationModes(*menu.addMenu("Automation Settings"), id);
    if (video && q->onTrackCompositing) q->onTrackCompositing(&menu, id);
    const bool small = minimized.count(id) != 0, big = maximized.count(id) != 0;
    add(menu, small ? "Restore" : "Minimize", [this, id] { toggleMinimized(id); });
    add(menu, big ? "Restore" : "Maximize", [this, id] { toggleMaximized(id); });
    menu.addSeparator();
    add(menu, "Edit Visible Button Set...", [this, video] { editVisibleButtons(video); });
}

void TimelineWidget::Impl::setButtonHidden(bool video, const QString& key, bool hidden)
{
    std::set<QString>& set = hiddenButtons[video ? 1 : 0];
    if (hidden) set.insert(key);
    else set.erase(key);
    QStringList l;
    for (const QString& k : set) l << k;
    QSettings().setValue(video ? "timeline/hiddenButtons/video" : "timeline/hiddenButtons/audio", l);
    layout();
    updateBars();
    q->update();
}

std::vector<std::pair<QString, QString>> TimelineWidget::Impl::headerButtonList(bool video)
{
    std::vector<std::pair<QString, QString>> l;
    if (video) l.push_back({"motion", "Track Motion"});
    else l.push_back({"arm", "Arm for Record"});
    l.push_back({"fx", "Track FX"});
    l.push_back({"auto", "Activate Automation Controls"});
    l.push_back({"mute", "Mute"});
    l.push_back({"solo", "Solo"});
    return l;
}

void TimelineWidget::Impl::editVisibleButtons(bool video)
{
    QDialog dlg(q);
    dlg.setObjectName("visibleButtonSet");
    dlg.setWindowTitle("Edit Visible Button Set...");
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(video ? "Specify the buttons you want to appear in all video track headers" : "Specify the buttons you want to appear in all audio track headers", &dlg));
    std::vector<std::pair<QString, QCheckBox*>> boxes;
    auto* row = new QHBoxLayout();
    auto* list = new QVBoxLayout();
    for (const auto& b : headerButtonList(video)) {
        auto* c = new QCheckBox(b.second, &dlg);
        c->setObjectName("button_" + b.first);
        c->setChecked(!hiddenButtons[video ? 1 : 0].count(b.first));
        list->addWidget(c);
        boxes.push_back(std::make_pair(b.first, c));
    }
    row->addLayout(list, 1);
    auto* side = new QVBoxLayout();
    QPushButton* showAll = new QPushButton("Show All", &dlg);
    QPushButton* hideAll = new QPushButton("Hide All", &dlg);
    showAll->setObjectName("showAll");
    hideAll->setObjectName("hideAll");
    side->addWidget(showAll);
    side->addWidget(hideAll);
    side->addStretch(1);
    row->addLayout(side);
    v->addLayout(row);
    QObject::connect(showAll, &QPushButton::clicked, &dlg, [boxes] { for (const auto& b : boxes) b.second->setChecked(true); });
    QObject::connect(hideAll, &QPushButton::clicked, &dlg, [boxes] { for (const auto& b : boxes) b.second->setChecked(false); });
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (visibleButtonsHook) {
        visibleButtonsHook(&dlg);
    } else if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    if (dlg.result() != QDialog::Accepted) return;
    for (const auto& b : boxes) setButtonHidden(video, b.first, !b.second->isChecked());
}

// An event header's hamburger menu (VEGAS 18's: the commands of the header's buttons and the display switches for the
// event). Video: Pan/Crop, Event FX, Color Grading, Freeze Frame at Cursor, Playback Rate; both: Mute, Lock, Active
// Take Information, Event Length, Fade Handles, Opacity / Volume Handle; then Edit Visible Button Set.
void TimelineWidget::Impl::eventHamburger(QMenu& menu, uint64_t id)
{
    const Track* tr = nullptr;
    const Event* e = app.project().findEvent(id, &tr);
    if (!e || !tr) return;
    const bool video = tr->type == TrackType::Video;
    const std::set<uint64_t> sel = app.selection();
    if (video) {
        if (q->onPanCrop) add(menu, "Event Pan/Crop", [this, id] { if (q->onPanCrop) q->onPanCrop(id); });
        add(menu, "Event FX", [this, id] { openEventFx(id); });
        command(menu, "colorGrading", "Color Grading");
        const double at = app.cursor() - e->start;
        add(menu, "Freeze Frame at Cursor", [this, id] { freezeFrameAtCursor(id); }, at > 1e-6 && at < e->length - 1e-6 && e->transition.kind.empty());
    } else {
        add(menu, "Event FX", [this, id] { openEventFx(id); });
    }
    {
        QMenu* rates = menu.addMenu("Playback Rate");
        const struct { double rate; const char* text; } choices[] = {{0.25, "25%"}, {0.5, "50%"}, {1.0, "100% (Normal)"}, {2.0, "200%"}, {4.0, "400%"}};
        const std::set<uint64_t> withG = withGroups(sel);
        for (const auto& c : choices) {
            const double r = c.rate;
            toggleItem(*rates, c.text, std::fabs(e->rate - r) < 1e-6, [this, withG, r] {
                app.edit("Playback Rate", [&](Project& p) { for (uint64_t s : withG) if (Event* o = p.findEvent(s)) o->rate = r; });
            });
        }
    }
    menu.addSeparator();
    const bool mute = e->mute, lock = e->locked;
    toggleItem(menu, "Mute", mute, [this, sel, mute] {
        app.edit(mute ? "Unmute Events" : "Mute Events", [&](Project& p) { for (uint64_t s : sel) if (Event* o = p.findEvent(s)) o->mute = !mute; });
    });
    toggleItem(menu, "Lock", lock, [this, sel, lock] {
        app.edit(lock ? "Unlock Events" : "Lock Events", [&](Project& p) { for (uint64_t s : sel) if (Event* o = p.findEvent(s)) o->locked = !lock; });
    });
    menu.addSeparator();
    auto flag = [&](const QString& text, bool TimelineOptions::*field) {
        toggleItem(menu, text, opt.*field, [this, field] {
            opt.*field = !(opt.*field);
            layout();
            q->update();
            if (q->onOptionsChanged) q->onOptionsChanged();
        });
    };
    flag("Active Take Information", &TimelineOptions::showTakeInfo);
    flag("Event Length", &TimelineOptions::showEventLength);
    flag("Fade Handles", &TimelineOptions::showFadeHandles);
    flag(video ? "Opacity Handle" : "Volume Handle", &TimelineOptions::showGainHandle);
    menu.addSeparator();
    toggleItem(menu, "Show Event Buttons", opt.showEventButtons, [this] {
        opt.showEventButtons = !opt.showEventButtons;
        q->update();
        if (q->onOptionsChanged) q->onOptionsChanged();
    });
    menu.addSeparator();
    add(menu, "Edit Visible Button Set...", [this] {
        const bool was = opt.showEventButtons;
        QDialog dlg(q);
        dlg.setObjectName("visibleEventButtonSet");
        dlg.setWindowTitle("Edit Visible Button Set...");
        auto* v = new QVBoxLayout(&dlg);
        v->addWidget(new QLabel("Specify the buttons you want to appear in all event headers", &dlg));
        auto* c = new QCheckBox("Pan/Crop, Event FX and the menu button", &dlg);
        c->setObjectName("button_eventButtons");
        c->setChecked(was);
        v->addWidget(c);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        v->addWidget(bb);
        QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (visibleButtonsHook) visibleButtonsHook(&dlg);
        else dlg.exec();
        if (dlg.result() != QDialog::Accepted) return;
        opt.showEventButtons = c->isChecked();
        q->update();
        if (q->onOptionsChanged) q->onOptionsChanged();
    });
}

void TimelineWidget::Impl::recordValue(const std::vector<uint64_t>& ids, const std::string& kind, double value)
{
    if (!rec.on) {
        rec = AutoRec();
        rec.on = true;
        for (uint64_t id : ids)
            if (const Track* t = findTrackConst(id))
                if (t->automation && (t->automationMode == "touch" || t->automationMode == "latch")) {
                    rec.tracks.push_back(id);
                    if (t->automationMode == "latch") rec.latch = true;
                }
        rec.kind = kind;
        if (!app.dragging()) app.beginDrag();
        recTimer->start();
    }
    rec.value = value;
    recPoint(playback ? playback->position() : app.cursor());
    recApply(false);
}

void TimelineWidget::Impl::recPoint(double t)
{
    EnvPoint pt;
    pt.time = std::max(0.0, t);
    pt.value = rec.value;
    pt.curve = rec.kind == "mute" ? (int)CurveHold : (int)CurveLinear;
    if (!rec.pts.empty() && pt.time < rec.pts.back().time - 1e-6) rec.pts.clear();
    if (!rec.pts.empty() && pt.time - rec.pts.back().time < 0.001) rec.pts.back() = pt;
    else rec.pts.push_back(pt);
}

void TimelineWidget::Impl::recApply(bool thin)
{
    if (rec.pts.empty()) return;
    std::vector<EnvPoint> pts = rec.pts;
    const double t0 = pts.front().time, t1 = pts.back().time;
    const std::vector<uint64_t> ids = rec.tracks;
    const std::string kind = rec.kind;
    app.dragUpdate([&](Project& p) {
        for (uint64_t id : ids) {
            Track* t = p.findTrack(id);
            if (!t) continue;
            const std::string k = (kind == "volume" && t->type == TrackType::Video) ? std::string("composite") : kind;
            Envelope& e = ensureTrackEnvelope(*t, k);
            std::vector<EnvPoint> out;
            for (const EnvPoint& b : e.points)
                if (b.time < t0 - 1e-9 || b.time > t1 + 1e-9) out.push_back(b);
            const size_t first = out.size();
            out.insert(out.end(), pts.begin(), pts.end());
            std::stable_sort(out.begin(), out.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
            if (thin) {
                size_t a = first, b = first;
                for (size_t i = 0; i < out.size(); ++i) {
                    if (out[i].time < t0 - 1e-9) a = i + 1;
                    if (out[i].time <= t1 + 1e-9) b = i;
                }
                if (b > a + 1) out = thinRun(k, out, a, b, 0.004);
            }
            e.points = out;
        }
    });
}

void TimelineWidget::Impl::recTick()
{
    if (!rec.on) {
        recTimer->stop();
        return;
    }
    if (!playback || !playback->playing()) {
        finishRecording();
        return;
    }
    const bool held = drag.kind == Drag::Level || drag.kind == Drag::Pan || drag.kind == Drag::Fade;
    if (held || rec.latch) {
        recPoint(playback->position());
        recApply(false);
    }
}

void TimelineWidget::Impl::finishRecording()
{
    if (!rec.on) return;
    recApply(app.prefs.thinAutomation);
    rec = AutoRec();
    recTimer->stop();
    app.endDrag("Record Automation");
    q->update();
}

void TimelineWidget::Impl::beginSlider(const Hit& h, const QPointF& pos, bool pan)
{
    const Row* r = rowOf(h.track);
    const Track* tr = r ? trackOf(*r) : nullptr;
    if (!tr) return;
    const HeaderRects hr = headerRects(*r);
    const bool fade = h.part == Part::HeaderFade;
    const QRect& s = fade ? hr.fade : pan ? hr.pan : hr.level;
    double value = pan ? (tr->pan + 1.0) * 0.5 : (r->video ? clampd(tr->level, 0.0, 1.0) : volumeToSlider(tr->level));
    if (tr->automation) {
        if (fade) value = (shownAutomation(*tr, "fadecolor") + 1.0) * 0.5;
        else if (pan) value = (shownAutomation(*tr, "pan") + 1.0) * 0.5;
        else value = r->video ? clampd(shownAutomation(*tr, "volume"), 0.0, 1.0) : volumeToSlider(shownAutomation(*tr, "volume"));
    }
    drag.kind = fade ? Drag::Fade : pan ? Drag::Pan : Drag::Level;
    drag.startValue = value;
    drag.absolute = std::fabs(pos.x() - (s.left() + value * s.width())) > 6.0;
}

void TimelineWidget::Impl::updateSlider(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    const Row* r = rowOf(drag.hit.track);
    if (!r) return;
    if (!drag.started) {
        if (std::fabs(pos.x() - drag.pressPos.x()) < 2.0) return;
        app.beginDrag();
        drag.started = true;
    }
    const bool video = r->video;
    const QRect s = drag.kind == Drag::Fade ? headerRects(*r).fade : drag.kind == Drag::Pan ? headerRects(*r).pan : headerRects(*r).level;
    if (s.width() <= 0) return;
    const double fine = (mods & Qt::ControlModifier) ? 0.125 : 1.0;
    double v = drag.absolute ? (pos.x() - s.left()) / s.width() : drag.startValue + (pos.x() - drag.pressPos.x()) / s.width() * fine;
    v = clampd(v, 0.0, 1.0);
    const uint64_t id = drag.hit.track;
    const std::vector<uint64_t> ids = controlTracks(id);
    const Track* tr = trackOf(*r);
    if (tr && tr->automation) {
        std::string kind = drag.kind == Drag::Fade ? "fadecolor" : drag.kind == Drag::Pan ? "pan" : "volume";
        double value = 0;
        QString text;
        if (drag.kind == Drag::Level) {
            value = video ? v : sliderToVolume(v);
            if (!video && std::fabs(v - volumeToSlider(1.0)) < 0.006) value = 1.0;
            text = QString(video ? "Level: " : "Volume: ") + levelText(video, value);
        } else {
            value = v * 2.0 - 1.0;
            if (std::fabs(value) < 0.02) value = 0;
            text = drag.kind == Drag::Pan ? "Pan: " + panText(value) : QString("Fade: %1%").arg(value * 100.0, 0, 'f', 0);
        }
        if (recordingWanted(id)) {
            recordValue(ids, kind, value);
            app.status("Recording automation - " + text.toStdString());
            return;
        }
        std::string why;
        const double at = quantized(app.cursor()), fr = frame();
        app.dragUpdate([&](Project& p) { why = applyAutomation(p, ids, kind, value, at, fr); });
        app.status(why.empty() ? text.toStdString() : why);
        return;
    }
    if (drag.kind == Drag::Fade) return;
    if (drag.kind == Drag::Pan) {
        double pan = v * 2.0 - 1.0;
        if (std::fabs(pan) < 0.02) pan = 0;
        app.dragUpdate([&](Project& p) {
            Track* g = p.findTrack(id);
            if (!g) return;
            const double delta = pan - g->pan;
            for (uint64_t x : ids)
                if (Track* t = p.findTrack(x)) t->pan = x == id ? pan : clampd(t->pan + delta, -1.0, 1.0);
        });
        app.status("Pan: " + panText(pan).toStdString());
    } else {
        double level = video ? v : sliderToVolume(v);
        if (!video && std::fabs(v - volumeToSlider(1.0)) < 0.006) level = 1.0;
        app.dragUpdate([&](Project& p) {
            Track* g = p.findTrack(id);
            if (!g) return;
            const double factor = g->level > 1e-9 ? level / g->level : 0.0, delta = level - g->level;
            for (uint64_t x : ids)
                if (Track* t = p.findTrack(x)) {
                    if (x == id) t->level = level;
                    else if (video) t->level = clampd(t->level + delta, 0.0, 1.0);
                    else t->level = g->level > 1e-9 ? std::min(t->level * factor, sliderToVolume(1.0)) : level;
                }
        });
        app.status(std::string(video ? "Level: " : "Volume: ") + levelText(video, level).toStdString());
    }
}

// The small surround panner's point follows the mouse (once it has moved): where the mouse is in the room, as in the
// Surround Panner window; with keyframes, the key at the cursor (created if needed).
void TimelineWidget::Impl::updateSurroundPoint(const QPointF& pos)
{
    const Row* r = rowOf(drag.hit.track);
    if (!r) return;
    if (!drag.started) {
        if ((pos - drag.pressPos).manhattanLength() < 2.0) return;
        app.beginDrag();
        drag.started = true;
    }
    const QRectF room = QRectF(headerRects(*r).surround).adjusted(3.5, 3.5, -3.5, -3.5);
    if (room.width() < 2 || room.height() < 2) return;
    const double x = (pos.x() - room.center().x()) / (room.width() * 0.5);
    const double y = (room.center().y() - pos.y()) / (room.height() * 0.5);
    const uint64_t id = drag.hit.track;
    const double cursor = app.cursor(), frame = 1.0 / std::max(1.0, app.project().settings.fps());
    app.dragUpdate([&](Project& p) { if (Track* t = p.findTrack(id)) surroundPlacePoint(t->surround, x, y, cursor, frame); });
    app.status(QString("Surround Pan: %1, %2").arg(clampd(x, -1, 1), 0, 'f', 2).arg(clampd(y, -1, 1), 0, 'f', 2).toStdString());
}

void TimelineWidget::Impl::scrub(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    double line = -1;
    const double x = clampd(pos.x(), headerW, lanesRight());
    const double t = snapped(drag.snaps, timeAtF(x), mods, &line);
    showSnap(line, t);
    app.setCursor(t);
    scrubSound();
}

void TimelineWidget::Impl::scrubSound()
{
    if (!playback || !vprefs::on("prefs/scrubAudio")) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int ms = scrubPieceMs();
    if (now - scrubSoundAt < ms * 8 / 10) return;
    scrubSoundAt = now;
    setOutputGain((float)std::max(0, std::min(400, vprefs::number("prefs/scrubVolume"))) / 100.0f);
    playback->previewCursor(ms / 1000.0);
}

void TimelineWidget::Impl::scrubSoundDone(bool resume)
{
    if (outputGain() == 1.0f) return;
    if (resume && playback) {
        playback->pause();
        setOutputGain(1.0f);
        return;
    }
    QTimer::singleShot(scrubPieceMs() + 100, q, [] { setOutputGain(1.0f); });
}

void TimelineWidget::Impl::updateLoop(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (!drag.started) {
        if (std::fabs(pos.x() - drag.pressPos.x()) < 4.0) return;
        drag.started = true;
    }
    double line = -1;
    const double t = snapped(drag.snaps, timeAtF(pos.x()), mods, &line);
    showSnap(line, t);
    app.setLoop(drag.anchor, t);
}

bool TimelineWidget::Impl::beginMove()
{
    const Project& pr = app.project();
    const std::set<uint64_t> sel = app.selection();
    const Track* grabTrack = nullptr;
    if (!sel.count(drag.hit.event) || !pr.findEvent(drag.hit.event, &grabTrack) || !grabTrack) return false;
    std::set<uint64_t> groups;
    for (const Track& t : pr.tracks)
        for (const Event& e : t.events)
            if (sel.count(e.id) && e.group && !opt.ignoreGrouping) groups.insert(e.group);
    drag.kindTracks.clear();
    for (size_t k = 0; k < pr.tracks.size(); ++k)
        if (pr.tracks[k].type == grabTrack->type) drag.kindTracks.push_back((int)k);
    drag.movers.clear();
    std::set<uint64_t> ids;
    for (const Track& t : pr.tracks)
        for (const Event& e : t.events)
            if ((sel.count(e.id) || (e.group && groups.count(e.group))) && e.locked) {
                app.status("A locked event cannot be moved (Edit > Switches > Lock).");
                return false;
            }
    for (size_t k = 0; k < pr.tracks.size(); ++k)
        for (const Event& e : pr.tracks[k].events)
            if (sel.count(e.id) || (e.group && groups.count(e.group))) {
                Mover m = moverOf(pr, (int)k, e);
                for (size_t o = 0; o < drag.kindTracks.size(); ++o)
                    if (drag.kindTracks[o] == (int)k) m.ordinal = (int)o;
                if (e.id == drag.hit.event) drag.grabbed = drag.movers.size();
                drag.movers.push_back(m);
                ids.insert(e.id);
            }
    drag.snaps = snapTargets(ids, true);
    // Lock Envelopes to Events: the points of the tracks' envelopes that lie under a moved event go with it (unless an
    // envelope is locked to the timeline)
    drag.envFollow.clear();
    if (opt.lockEnvelopes) {
        std::set<std::tuple<uint64_t, size_t, size_t>> seen;
        for (const Mover& m : drag.movers) {
            const Track& t = pr.tracks[(size_t)m.track];
            for (size_t ei = 0; ei < t.envelopes.size(); ++ei) {
                if (t.envelopes[ei].locked) continue;
                const std::vector<EnvPoint>& pts = t.envelopes[ei].points;
                for (size_t pi = 0; pi < pts.size(); ++pi)
                    if (pts[pi].time >= m.start - 1e-9 && pts[pi].time <= m.end() + 1e-9 && seen.insert(std::make_tuple(t.id, ei, pi)).second)
                        drag.envFollow.push_back(DragState::EnvFollow{t.id, ei, pi, pts[pi].time});
            }
        }
    }
    drag.ripple = opt.autoRipple;
    if (drag.ripple) {
        std::set<uint64_t> tracks;
        double from = 1e300;
        for (const Mover& m : drag.movers) {
            tracks.insert(pr.tracks[(size_t)m.track].id);
            from = std::min(from, m.start + 1e-6);
        }
        drag.followers = followersOf(pr, tracks, from, ids, opt.ripple);
    }
    app.beginDrag();
    drag.started = true;
    return true;
}

void TimelineWidget::Impl::updateMove(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    const std::vector<Mover> movers = drag.movers;
    const std::vector<int> kindTracks = drag.kindTracks;
    const Mover& g = movers[drag.grabbed];
    double delta = timeAtF(pos.x()) - drag.pressTime;
    double line = -1;
    if (snapOn(mods)) {
        double best = 1e300;
        for (const Mover& m : movers)
            for (double edge : {m.start + moverSnapOffset(m) + delta, m.end() + delta}) {
                double t = 0;
                if (nearest(drag.snaps, edge, &t) && std::fabs(t - edge) < std::fabs(best)) {
                    best = t - edge;
                    line = t;
                }
            }
        if (line >= 0) delta += best;
    }
    delta = quantized(g.start + delta) - g.start;
    double first = 1e300;
    for (const Mover& m : movers) first = std::min(first, m.start);
    if (first + delta < 0) delta = -first;
    bool onLine = false;
    for (const Mover& m : movers)
        if (std::fabs(m.start + delta - line) < frame() * 0.6 || std::fabs(m.end() + delta - line) < frame() * 0.6) onLine = true;
    snapLine = (line >= 0 && onLine) ? line : -1;

    int shift = 0;
    if (g.ordinal >= 0 && (size_t)g.track < rows.size() && !kindTracks.empty()) {
        const Row& gr = rows[(size_t)g.track];
        const double cy = rowTop(gr) + gr.h * 0.5 + (pos.y() - drag.pressPos.y());
        int target = rowAtY(cy);
        if (target < 0) target = cy < rowTop(rows.front()) ? 0 : (int)rows.size() - 1;
        int ord = 0, bestDist = 1 << 30;
        for (size_t o = 0; o < kindTracks.size(); ++o) {
            const int dist = std::abs(kindTracks[o] - target);
            if (dist < bestDist) { bestDist = dist; ord = (int)o; }
        }
        shift = ord - g.ordinal;
        for (const Mover& m : movers) {
            if (m.ordinal < 0) continue;
            shift = std::max(shift, -m.ordinal);
            shift = std::min(shift, (int)kindTracks.size() - 1 - m.ordinal);
        }
    }
    const bool ripple = drag.ripple && shift == 0, quantize = opt.quantize;
    const Followers followers = drag.followers;
    const std::vector<DragState::EnvFollow> envFollow = shift == 0 ? drag.envFollow : std::vector<DragState::EnvFollow>();
    app.dragUpdate([&](Project& p) {
        const std::map<uint64_t, double> links = syncMemo(p);
        if (ripple) followers.apply(p, delta, quantize);
        std::set<std::pair<uint64_t, size_t>> touched;
        for (const auto& f : envFollow)
            if (Track* t = p.findTrack(f.track))
                if (f.env < t->envelopes.size() && f.point < t->envelopes[f.env].points.size()) {
                    t->envelopes[f.env].points[f.point].time = std::max(0.0, f.time + delta);
                    touched.insert(std::make_pair(f.track, f.env));
                }
        for (const auto& te : touched) {
            std::vector<EnvPoint>& pts = p.findTrack(te.first)->envelopes[te.second].points;
            std::stable_sort(pts.begin(), pts.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
        }
        for (const Mover& m : movers) {
            Track* from = nullptr;
            Event* e = p.findEvent(m.id, &from);
            if (!e || !from) continue;
            e->start = std::max(0.0, quantize ? p.quantize(m.start + delta) : m.start + delta);
            if (m.ordinal >= 0 && shift != 0) {
                const int to = kindTracks[(size_t)(m.ordinal + shift)];
                if ((size_t)to < p.tracks.size() && &p.tracks[(size_t)to] != from) {
                    const Event moved = *e;
                    from->events.erase(from->events.begin() + (e - from->events.data()));
                    p.tracks[(size_t)to].events.push_back(moved);
                }
            }
        }
        std::set<uint64_t> moving;
        for (const Mover& m : movers) moving.insert(m.id);
        followSyncLinks(p, links, moving);
    });
    app.status("Move: " + timecode(g.start + delta, fps()));
}

// An event's gain line was pressed: the drag that follows pulls it up or down. All the selected events of the same kind
// (audio or video) go with it, as in VEGAS; the event pressed joins the selection if it isn't in it.
void TimelineWidget::Impl::pressGain(const Hit& h, Qt::KeyboardModifiers, bool right)
{
    const Project& pr = app.project();
    const Track* tr = nullptr;
    const Event* e = pr.findEvent(h.event, &tr);
    const Row* row = rowOf(h.track);
    if (!e || !tr || !row) return;
    if (!app.selection().count(h.event)) selectGroupOf(h.event);
    drag.kind = Drag::EventGain;
    drag.right = right;
    drag.gainVideo = tr->type == TrackType::Video;
    drag.gainHeight = std::max(20.0, eventRect(*row, *e).height());
    drag.gainDy = 0;
    drag.gainLastY = drag.pressPos.y();
    drag.gains.clear();
    drag.gains.push_back({e->id, e->gain});
    for (const Track& t : pr.tracks) {
        if ((t.type == TrackType::Video) != drag.gainVideo) continue;
        for (const Event& o : t.events)
            if (o.id != e->id && app.selection().count(o.id) && !o.locked) drag.gains.push_back({o.id, o.gain});
    }
}

void TimelineWidget::Impl::updateGain(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (!drag.started) {
        if ((pos - drag.pressPos).manhattanLength() < 3.0) return;
        const Event* e = app.project().findEvent(drag.hit.event);
        if (!e) { drag.kind = Drag::None; return; }
        if (e->locked) {
            app.status("A locked event cannot be changed (Edit > Switches > Lock).");
            drag = DragState();
            return;
        }
        app.beginDrag();
        drag.started = true;
    }
    // The mouse's movement is added up as it goes, so Ctrl (or the right button) can be pressed and released mid-drag
    // without the line jumping: with it, a pixel moves the line an eighth as far.
    const bool fine = (mods & Qt::ControlModifier) != 0 || drag.right;
    drag.gainDy += (pos.y() - drag.gainLastY) / drag.gainHeight * (fine ? 0.125 : 1.0);
    drag.gainLastY = pos.y();
    const std::vector<std::pair<uint64_t, double>> base = drag.gains;
    const bool video = drag.gainVideo;
    const double dy = drag.gainDy;
    const double shown = gainFraction(gainFraction(base[0].second) - dy);
    app.dragUpdate([&](Project& p) {
        for (const auto& b : base)
            if (Event* x = p.findEvent(b.first)) x->gain = gainFraction(gainFraction(b.second) - dy);
    });
    envLabel = gainText(video, shown);
    envLabelAt = pos;
    app.status(envLabel.toStdString());
    q->update();
}

bool TimelineWidget::Impl::beginEdge()
{
    const Project& pr = app.project();
    const Track* tr = nullptr;
    const Event* e = pr.findEvent(drag.hit.event, &tr);
    if (!e || !tr) return false;
    if (e->locked) {
        app.status("A locked event cannot be trimmed (Edit > Switches > Lock).");
        return false;
    }
    drag.movers.clear();
    drag.movers.push_back(moverOf(pr, trackIndex(tr->id), *e));
    const bool fade = drag.kind == Drag::FadeIn || drag.kind == Drag::FadeOut;
    const bool left = drag.kind == Drag::TrimLeft || drag.kind == Drag::StretchLeft;
    if (e->group && !fade && !opt.ignoreGrouping && !drag.ignoreGroup) {
        const double edge = left ? e->start : e->end();
        for (size_t k = 0; k < pr.tracks.size(); ++k)
            for (const Event& o : pr.tracks[k].events)
                if (o.id != e->id && o.group == e->group && std::fabs((left ? o.start : o.end()) - edge) < frame() * 0.5 && !o.locked)
                    drag.movers.push_back(moverOf(pr, (int)k, o));
    }
    std::set<uint64_t> ids;
    for (const Mover& m : drag.movers) ids.insert(m.id);
    drag.snaps = snapTargets(ids, true);
    drag.ripple = opt.autoRipple && drag.kind == Drag::TrimRight;
    if (drag.ripple) {
        std::set<uint64_t> tracks;
        double from = 1e300;
        for (const Mover& m : drag.movers) {
            tracks.insert(pr.tracks[(size_t)m.track].id);
            from = std::min(from, m.end());
        }
        drag.followers = followersOf(pr, tracks, from, ids, opt.ripple);
    }
    app.beginDrag();
    drag.started = true;
    return true;
}

void TimelineWidget::Impl::updateEdge(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    const double fr = frame();
    const std::vector<Mover> parts = drag.movers;
    const Mover& g = parts[0];
    const Drag kind = drag.kind;
    const double raw = timeAtF(pos.x()) - drag.pressTime;
    double line = -1, edge = 0, delta = 0, factor = 1, fade = 0;
    std::string say;
    switch (kind) {
    case Drag::TrimLeft: {
        const double want = snapped(drag.snaps, g.start + raw, mods, &line);
        double lo = -1e300, hi = 1e300;
        for (const Mover& m : parts) {
            lo = std::max(lo, -m.start);
            hi = std::min(hi, m.length - fr);
        }
        delta = std::min(std::max(want - g.start, lo), hi);
        edge = g.start + delta;
        say = "Trim: length " + timecode(g.length - delta, fps());
        break;
    }
    case Drag::TrimRight: {
        const double want = snapped(drag.snaps, g.end() + raw, mods, &line);
        double lo = -1e300;
        for (const Mover& m : parts) lo = std::max(lo, -(m.length - fr));
        delta = std::max(want - g.end(), lo);
        edge = g.end() + delta;
        say = "Trim: length " + timecode(g.length + delta, fps());
        break;
    }
    case Drag::StretchLeft:
    case Drag::StretchRight: {
        const bool left = kind == Drag::StretchLeft;
        const double want = snapped(drag.snaps, (left ? g.start : g.end()) + raw, mods, &line);
        factor = (left ? g.end() - want : want - g.start) / g.length;
        double lo = 0, hi = 1e300;
        for (const Mover& m : parts) {
            lo = std::max(lo, std::max(m.rate / kMaxRate, fr / m.length));
            hi = std::min(hi, m.rate / kMinRate);
            if (left) hi = std::min(hi, m.end() / m.length);
        }
        factor = std::min(std::max(factor, lo), hi);
        edge = left ? g.end() - g.length * factor : g.start + g.length * factor;
        say = strfmt("Stretch: playback rate %.0f%%", g.rate / factor * 100.0);
        break;
    }
    case Drag::FadeIn: {
        const double want = snapped(drag.snaps, g.start + g.fadeIn + raw, mods, &line);
        fade = clampd(want - g.start, 0.0, std::max(0.0, g.length - g.fadeOut));
        edge = g.start + fade;
        say = "Fade In: " + timecode(fade, fps());
        break;
    }
    case Drag::FadeOut: {
        const double want = snapped(drag.snaps, g.end() - g.fadeOut + raw, mods, &line);
        fade = clampd(g.end() - want, 0.0, std::max(0.0, g.length - g.fadeIn));
        edge = g.end() - fade;
        say = "Fade Out: " + timecode(fade, fps());
        break;
    }
    default:
        return;
    }
    showSnap(line, edge);
    const bool ripple = drag.ripple;
    const Followers followers = drag.followers;
    const bool quantize = opt.quantize;
    app.dragUpdate([&](Project& p) {
        if (ripple) followers.apply(p, delta, quantize);
        for (const Mover& m : parts) {
            Event* e = p.findEvent(m.id);
            if (!e) continue;
            switch (kind) {
            case Drag::TrimLeft:
                e->start = m.start + delta;
                shiftEventEnvelopes(*e, delta);
                e->length = m.length - delta;
                e->offset = m.offset + delta * m.rate;
                if (m.loop && m.mediaLength > 0) {
                    e->offset = std::fmod(e->offset, m.mediaLength);
                    if (e->offset < 0) e->offset += m.mediaLength;
                }
                shiftTakes(*e, delta * m.rate);
                break;
            case Drag::TrimRight: e->length = m.length + delta; break;
            case Drag::StretchLeft:
                e->length = m.length * factor;
                e->start = m.end() - e->length;
                e->rate = m.rate / factor;
                break;
            case Drag::StretchRight:
                e->length = m.length * factor;
                e->rate = m.rate / factor;
                break;
            case Drag::FadeIn: e->fadeIn = fade; break;
            case Drag::FadeOut: e->fadeOut = fade; break;
            default: break;
            }
            e->fadeIn = std::min(e->fadeIn, e->length);
            e->fadeOut = std::min(e->fadeOut, std::max(0.0, e->length - e->fadeIn));
        }
    });
    if (kind != Drag::FadeIn && kind != Drag::FadeOut) edgePreview(edge);
    app.status(say);
}

void TimelineWidget::Impl::dragTo(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    switch (drag.kind) {
    case Drag::Scrub: scrub(pos, mods); break;
    case Drag::LoopRange: updateLoop(pos, mods); break;
    case Drag::Move:
        if (!drag.started) {
            if ((pos - drag.pressPos).manhattanLength() < 4.0) return;
            if (!beginMove()) { drag.kind = Drag::None; return; }
        }
        updateMove(pos, mods);
        break;
    case Drag::TrimLeft:
    case Drag::TrimRight:
    case Drag::StretchLeft:
    case Drag::StretchRight:
    case Drag::FadeIn:
    case Drag::FadeOut:
        if (!drag.started) {
            if ((pos - drag.pressPos).manhattanLength() < 3.0) return;
            if (!beginEdge()) { drag.kind = Drag::None; return; }
        }
        updateEdge(pos, mods);
        break;
    case Drag::EventGain: updateGain(pos, mods); break;
    case Drag::Level:
    case Drag::Pan:
    case Drag::Fade: updateSlider(pos, mods); break;
    case Drag::SurroundPoint: updateSurroundPoint(pos); break;
    case Drag::Marker:
    case Drag::RegionStart:
    case Drag::RegionEnd:
    case Drag::RegionMove: updateMarker(pos, mods); break;
    case Drag::RubberBand: updateRubberBand(pos, mods); break;
    case Drag::ZoomRange:
        if (!drag.started && std::fabs(pos.x() - drag.pressPos.x()) < 4.0) return;
        drag.started = true;
        break;
    case Drag::HeaderWidth:
        setHeaderWidth((int)std::lround(drag.startValue + pos.x() - drag.pressPos.x()));
        break;
    case Drag::TrackHeight: updateTrackEdge(pos); break;
    case Drag::TrackKeys:
        if (!drag.started) {
            if (std::fabs(pos.x() - drag.pressPos.x()) < 3.0) return;
            app.beginDrag();
            drag.started = true;
        }
        dragTrackKeys(pos);
        break;
    case Drag::EnvPoints:
    case Drag::EnvSegment: updateEnvelope(pos, mods); break;
    case Drag::EnvBand: updateEnvBand(pos, mods); break;
    case Drag::EnvBrush: updateEnvBrush(pos); break;
    case Drag::SnapOffset: updateSnapOffset(pos, mods); break;
    case Drag::Slip:
    case Drag::Slide:
        if (!drag.started) {
            if ((pos - drag.pressPos).manhattanLength() < 4.0) return;
            if (!beginBody()) { drag.kind = Drag::None; return; }
        }
        updateBody(pos, mods);
        break;
    case Drag::SlipTrimLeft:
    case Drag::SlipTrimRight:
    case Drag::RollLeft:
    case Drag::RollRight:
        if (!drag.started) {
            if ((pos - drag.pressPos).manhattanLength() < 3.0) return;
            if (!beginEdgeOp()) { drag.kind = Drag::None; return; }
        }
        updateEdgeOp(pos, mods);
        break;
    case Drag::Shuffle:
        if (!drag.started) {
            if ((pos - drag.pressPos).manhattanLength() < 4.0) return;
            if (!beginShuffle()) { drag.kind = Drag::None; return; }
        }
        updateShuffle(pos);
        break;
    case Drag::SplitTrim:
        if (std::fabs(pos.x() - drag.pressPos.x()) < 3.0 || !drag.splitRight) return;
        if (pos.x() > drag.pressPos.x()) {
            drag.kind = Drag::TrimLeft;
            drag.hit.event = drag.splitRight;
        } else {
            drag.kind = Drag::TrimRight;
        }
        if (!beginEdge()) { drag.kind = Drag::None; return; }
        updateEdge(pos, mods);
        break;
    default: break;
    }
}

// Video Event Edge Edit Frames: while an edge is dragged the cursor (and so the preview) follows it; it goes back where
// it was when the drag ends.
void TimelineWidget::Impl::edgePreview(double t)
{
    if (!opt.edgeFrames || (playback && playback->playing())) return;
    if (edgeCursor < 0) edgeCursor = app.cursor();
    app.setCursor(std::max(0.0, t));
}

void TimelineWidget::Impl::endEdgePreview()
{
    if (edgeCursor < 0) return;
    const double c = edgeCursor;
    edgeCursor = -1;
    app.setCursor(c);
}

bool TimelineWidget::Impl::beginBody()
{
    std::string why;
    if (!planBody(app.selection(), drag.kind == Drag::Slide, false, dragBody, &why)) {
        app.status(why);
        return false;
    }
    app.beginDrag();
    drag.started = true;
    return true;
}

void TimelineWidget::Impl::updateBody(const QPointF& pos, Qt::KeyboardModifiers)
{
    if (dragBody.parts.empty()) return;
    double d = timeAtF(pos.x()) - drag.pressTime;
    if (opt.quantize) d = app.project().quantize(d);
    d = clampBody(dragBody, d);
    const BodyWork w = dragBody;
    app.dragUpdate([&](Project& p) { applyBody(p, w, d); });
    app.status((w.slide ? "Slide: " : "Slip: ") + timecode(std::fabs(d), fps()) + (d < 0 ? " back" : " on"));
}

bool TimelineWidget::Impl::beginEdgeOp()
{
    const int side = (drag.kind == Drag::SlipTrimLeft || drag.kind == Drag::RollLeft) ? -1 : 1;
    const EdgeOp op = (drag.kind == Drag::RollLeft || drag.kind == Drag::RollRight) ? EdgeOp::Slide : EdgeOp::SlipTrim;
    std::string why;
    if (!planEdge(std::vector<uint64_t>{drag.hit.event}, side, op, drag.ignoreGroup, dragEdge, &why)) {
        app.status(why);
        return false;
    }
    drag.snaps = snapTargets(dragEdge.ids(), true);
    app.beginDrag();
    drag.started = true;
    return true;
}

void TimelineWidget::Impl::updateEdgeOp(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (dragEdge.parts.empty()) return;
    const Mover& g = dragEdge.parts[0];
    const double edge0 = dragEdge.side < 0 ? g.start : g.end();
    double line = -1;
    const double want = snapped(drag.snaps, edge0 + timeAtF(pos.x()) - drag.pressTime, mods, &line);
    const double d = clampEdge(dragEdge, want - edge0);
    showSnap(line, edge0 + d);
    const EdgeWork w = dragEdge;
    app.dragUpdate([&](Project& p) { applyEdge(p, w, d); });
    edgePreview(edge0 + d);
    app.status((w.op == EdgeOp::Slide ? "Slide edit: " : "Slip trim: ") + timecode(edge0 + d, fps()));
}

void TimelineWidget::Impl::pressSplitTrim(const Hit& h)
{
    const double t = quantized(drag.pressTime);
    const std::set<uint64_t> ids = withGroups(std::set<uint64_t>{h.event});
    for (uint64_t id : ids)
        if (const Event* e = app.project().findEvent(id))
            if (e->locked) { app.status("A locked event cannot be split."); return; }
    uint64_t right = 0;
    std::set<uint64_t> sel;
    app.edit("Split", [&](Project& p) {
        app.fx.captureStates(p);
        std::map<uint64_t, uint64_t> groups;
        for (uint64_t id : ids) {
            const uint64_t r = splitIn(p, id, t, &groups);
            if (id == h.event) right = r;
            if (r) sel.insert(r);
            sel.insert(id);
        }
    });
    if (!right) return;
    drag.kind = Drag::SplitTrim;
    drag.splitRight = right;
    drag.hit.part = Part::EventRight;
    app.setSelection(std::set<uint64_t>{h.event});
}

}  // namespace app
}  // namespace reno
