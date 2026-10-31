// Editing commands of the timeline: sync links, keyboard trimming, slip and slide, ripple.
#include "timeline_internal.h"

namespace reno {
namespace app {

std::set<uint64_t> TimelineWidget::Impl::withGroups(const std::set<uint64_t>& ids) const
{
    std::set<uint64_t> out = ids, groups;
    if (opt.ignoreGrouping) return out;
    const Project& p = app.project();
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (ids.count(e.id) && e.group) groups.insert(e.group);
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (e.group && groups.count(e.group)) out.insert(e.id);
    return out;
}

// Moves an event's start to 't' keeping the media in place (a trim of its left edge); stops a frame from its end and,
// for media that doesn't loop, at the media's start.
void TimelineWidget::Impl::trimStartTo(const Project& p, Event& e, double t, double fr)
{
    t = std::min(t, e.end() - fr);
    double d = t - e.start;
    double offset = e.offset + d * e.rate;
    const double dur = mediaDuration(p, e);
    if (offset < 0 && !(e.loop && dur > 0)) {
        d -= offset / e.rate;
        offset = 0;
    }
    if (e.loop && dur > 0) {
        offset = std::fmod(offset, dur);
        if (offset < 0) offset += dur;
    }
    e.start += d;
    e.length -= d;
    e.offset = offset;
    shiftEventEnvelopes(e, d);
    e.fadeIn = std::min(e.fadeIn, e.length);
    e.fadeOut = std::min(e.fadeOut, std::max(0.0, e.length - e.fadeIn));
}

void TimelineWidget::Impl::trimEndTo(Event& e, double t, double fr)
{
    e.length = std::max(fr, t - e.start);
    e.fadeIn = std::min(e.fadeIn, e.length);
    e.fadeOut = std::min(e.fadeOut, std::max(0.0, e.length - e.fadeIn));
}

void TimelineWidget::Impl::removeSelection(bool cut)
{
    if (!cut && deleteEnvelopePoints()) return;
    if (app.selection().empty()) return;
    std::set<uint64_t> ids = app.selection();
    for (uint64_t id : app.lockedAmong(ids)) ids.erase(id);
    if (!opt.autoRipple) {
        std::map<uint64_t, Ranges> removed;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (ids.count(e.id)) removed[t.id].emplace_back(e.start, e.end());
        const uint64_t before = app.revision();
        if (cut) app.cutSelection();
        else app.deleteSelection();
        if (app.revision() != before && !removed.empty()) rememberRemoval(removed);
        return;
    }
    if (cut) app.copySelection();
    if (ids.empty()) { app.status("The selected events are locked (Edit > Switches > Lock)."); return; }
    std::map<uint64_t, Ranges> removed;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (ids.count(e.id)) removed[t.id].emplace_back(e.start, e.end());
    const RippleMode mode = opt.ripple;
    app.edit(cut ? "Cut" : "Delete", [&](Project& p) {
        for (Track& t : p.tracks)
            t.events.erase(std::remove_if(t.events.begin(), t.events.end(), [&](const Event& e) { return ids.count(e.id) != 0; }), t.events.end());
        rippleClose(p, removed, mode);
    });
}

void TimelineWidget::Impl::pasteInsert()
{
    if (!app.hasClipboard()) return;
    const double at = quantized(app.cursor());
    const std::set<uint64_t> before = app.selection();
    app.pasteAtCursor();
    const std::set<uint64_t> made = app.selection();
    if (made.empty() || made == before) return;
    double first = 1e300, last = -1e300;
    std::set<uint64_t> tracks;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (made.count(e.id)) {
                first = std::min(first, e.start);
                last = std::max(last, e.end());
                tracks.insert(t.id);
            }
    const double len = last - std::min(first, at);
    if (len <= 0) return;
    const Followers f = followersOf(app.project(), tracks, at, made, opt.ripple);
    const bool quantize = opt.quantize;
    app.edit("Paste Insert", [&](Project& p) { f.apply(p, len, quantize); });
}

void TimelineWidget::Impl::trimStart()
{
    const std::set<uint64_t> ids = app.selection();
    if (ids.empty()) return;
    const double c = quantized(app.cursor()), fr = frame();
    std::set<uint64_t> all = ids;
    for (uint64_t id : withGroups(ids)) {
        const Event* e = app.project().findEvent(id);
        if (!e) continue;
        for (uint64_t s : ids)
            if (const Event* o = app.project().findEvent(s))
                if (std::fabs(o->start - e->start) < fr * 0.5) all.insert(id);
    }
    app.edit("Trim Start", [&](Project& p) {
        for (uint64_t id : all)
            if (Event* e = p.findEvent(id))
                if (c < e->end() - fr * 0.5 && !e->locked) {
                    const double was = e->offset;
                    trimStartTo(p, *e, c, fr);
                    shiftTakes(*e, e->offset - was);
                }
    });
}

void TimelineWidget::Impl::trimEnd()
{
    const std::set<uint64_t> ids = app.selection();
    if (ids.empty()) return;
    const double c = quantized(app.cursor()), fr = frame();
    std::set<uint64_t> all = ids;
    for (uint64_t id : withGroups(ids)) {
        const Event* e = app.project().findEvent(id);
        if (!e) continue;
        for (uint64_t s : ids)
            if (const Event* o = app.project().findEvent(s))
                if (std::fabs(o->end() - e->end()) < fr * 0.5) all.insert(id);
    }
    app.edit("Trim End", [&](Project& p) {
        for (uint64_t id : all)
            if (Event* e = p.findEvent(id))
                if (c > e->start + fr * 0.5 && !e->locked) trimEndTo(*e, c, fr);
    });
}

void TimelineWidget::Impl::trimToSelection()
{
    if (!(app.loopEnd > app.loopStart)) { app.status("Make a time selection first: Ctrl+T trims the selected events to it."); return; }
    const std::set<uint64_t> ids = withGroups(app.selection());
    if (ids.empty()) return;
    const double a = app.loopStart, b = app.loopEnd, fr = frame();
    app.edit("Trim", [&](Project& p) {
        for (uint64_t id : ids)
            if (Event* e = p.findEvent(id)) {
                if (e->end() <= a || e->start >= b || e->locked) continue;
                if (e->start < a) {
                    const double was = e->offset;
                    trimStartTo(p, *e, a, fr);
                    shiftTakes(*e, e->offset - was);
                }
                if (e->end() > b) trimEndTo(*e, b, fr);
            }
    });
}

std::map<uint64_t, double> TimelineWidget::Impl::syncMemo(const Project& p)
{
    std::set<uint64_t> targets;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (e.syncLink) targets.insert(e.syncLink);
    std::map<uint64_t, double> out;
    if (targets.empty()) return out;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (targets.count(e.id)) out[e.id] = e.start;
    return out;
}

void TimelineWidget::Impl::followSyncLinks(Project& p, const std::map<uint64_t, double>& before, const std::set<uint64_t>& moving)
{
    if (before.empty()) return;
    std::map<uint64_t, double> now;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (before.count(e.id)) now[e.id] = e.start;
    for (Track& t : p.tracks) {
        bool moved = false;
        for (Event& e : t.events) {
            if (!e.syncLink || moving.count(e.id)) continue;
            auto a = before.find(e.syncLink);
            auto b = now.find(e.syncLink);
            if (a == before.end() || b == now.end() || b->second == a->second) continue;
            e.start = std::max(0.0, e.start + (b->second - a->second));
            moved = true;
        }
        if (moved) t.sortEvents();
    }
}

std::set<uint64_t> TimelineWidget::Impl::syncLinkGroup(uint64_t id) const
{
    std::set<uint64_t> out;
    const Event* e = app.project().findEvent(id);
    if (!e) return out;
    const uint64_t head = e->syncLink ? e->syncLink : id;
    out.insert(head);
    for (const Track& t : app.project().tracks)
        for (const Event& o : t.events)
            if (o.syncLink == head) out.insert(o.id);
    return out.size() > 1 ? out : std::set<uint64_t>();
}

void TimelineWidget::Impl::createSyncLink(uint64_t head)
{
    std::set<uint64_t> sel = app.selection();
    sel.erase(head);
    if (sel.empty()) return;
    app.edit("Create Sync Link", [&](Project& p) {
        Event* h = p.findEvent(head);
        if (!h) return;
        if (sel.count(h->syncLink)) h->syncLink = 0;
        for (uint64_t id : sel)
            if (Event* e = p.findEvent(id)) {
                e->syncLink = head;
                for (Track& t : p.tracks)
                    for (Event& o : t.events)
                        if (o.syncLink == id) o.syncLink = head;
            }
        h->syncLink = 0;
    });
}

void TimelineWidget::Impl::removeSyncLink(uint64_t head)
{
    const std::set<uint64_t> sel = app.selection();
    app.edit("Remove Sync Link", [&](Project& p) {
        for (Track& t : p.tracks)
            for (Event& e : t.events)
                if (e.syncLink == head && (sel.count(e.id) || sel.empty())) e.syncLink = 0;
    });
}

void TimelineWidget::Impl::nudge(int frames, int pixels, int tracks)
{
    const std::set<uint64_t> sel = app.selection();
    if (sel.empty()) return;
    const Project& pr = app.project();
    if (!app.lockedAmong(withGroups(sel)).empty()) { app.status("A locked event cannot be moved (Edit > Switches > Lock)."); return; }
    if (tracks != 0) {
        app.edit("Move Events", [&](Project& p) {
            std::vector<std::pair<uint64_t, size_t>> moves;
            for (size_t k = 0; k < p.tracks.size(); ++k)
                for (const Event& e : p.tracks[k].events) {
                    if (!sel.count(e.id)) continue;
                    std::vector<size_t> same;
                    int ord = -1;
                    for (size_t j = 0; j < p.tracks.size(); ++j)
                        if (p.tracks[j].type == p.tracks[k].type) {
                            if (j == k) ord = (int)same.size();
                            same.push_back(j);
                        }
                    const int to = ord + tracks;
                    if (to >= 0 && to < (int)same.size()) moves.emplace_back(e.id, same[(size_t)to]);
                }
            for (const auto& m : moves) {
                Track* from = nullptr;
                Event* e = p.findEvent(m.first, &from);
                if (!e || !from || &p.tracks[m.second] == from) continue;
                const Event moved = *e;
                from->events.erase(from->events.begin() + (e - from->events.data()));
                p.tracks[m.second].events.push_back(moved);
                p.tracks[m.second].sortEvents();
            }
        });
        return;
    }
    double delta = frames * frame() + pixels / pps;
    if (opt.quantize && pixels != 0) delta = (pixels > 0 ? 1.0 : -1.0) * std::max(frame(), std::fabs(pr.quantize(std::fabs(delta))));
    const std::set<uint64_t> ids = withGroups(sel);
    double first = 1e300;
    for (uint64_t id : ids)
        if (const Event* e = pr.findEvent(id)) first = std::min(first, e->start);
    if (first + delta < 0) delta = -first;
    if (delta == 0) return;
    const bool quantize = opt.quantize;
    app.edit("Move Events", [&](Project& p) {
        const std::map<uint64_t, double> links = syncMemo(p);
        for (uint64_t id : ids)
            if (Event* e = p.findEvent(id)) e->start = std::max(0.0, quantize ? p.quantize(e->start + delta) : e->start + delta);
        followSyncLinks(p, links, ids);
    });
}

void TimelineWidget::Impl::insertMarker()
{
    const double at = quantized(app.cursor());
    for (const auto& m : app.project().markers)
        if (!m.region && std::fabs(m.time - at) < frame() * 0.5) { app.status("There is a marker here already."); return; }
    uint64_t made = 0;
    app.edit("Insert Marker", [&](Project& p) {
        Project::Marker m;
        m.id = made = p.newId();
        m.time = at;
        p.markers.push_back(m);
    });
    nameNewMarker(made);
}

// Preferences > General "Automatically name regions and markers if not playing": the name box opens on the new
// marker or region.
void TimelineWidget::Impl::nameNewMarker(uint64_t id)
{
    if (!id || !vprefs::on("prefs/autoNameMarkers") || (playback && playback->playing())) return;
    if (markerIndex(id) < 0) return;
    QTimer::singleShot(0, q, [this, id] { if (markerIndex(id) >= 0) startRename(Naming::Marker, id); });
}

void TimelineWidget::Impl::insertRegion()
{
    if (!(app.loopEnd > app.loopStart)) { app.status("Make a time selection first: R makes a region of it."); return; }
    const double a = app.loopStart, b = app.loopEnd;
    uint64_t made = 0;
    app.edit("Insert Region", [&](Project& p) {
        Project::Marker m;
        m.id = made = p.newId();
        m.time = a;
        m.length = b - a;
        m.region = true;
        p.markers.push_back(m);
    });
    nameNewMarker(made);
}

void TimelineWidget::Impl::goToMarker(int number)
{
    const std::vector<Project::Marker>& ms = app.project().markers;
    if (number < 1 || number > (int)ms.size()) return;
    const Project::Marker m = ms[(size_t)number - 1];
    if (m.region) app.setLoop(m.time, m.time + m.length);
    keyCursor(m.time);
    scrollToCursor();
}

void TimelineWidget::Impl::cursorToEdge(int dir, EdgeKind kind)
{
    const Project& p = app.project();
    std::vector<double> v;
    switch (kind) {
    case EdgeKind::Markers:
        for (const auto& m : p.markers) {
            v.push_back(m.time);
            if (m.region) v.push_back(m.time + m.length);
        }
        break;
    case EdgeKind::Events:
    case EdgeKind::EditPoints:
    case EdgeKind::Selected:
        for (const Track& t : p.tracks)
            for (const Event& e : t.events) {
                if (kind == EdgeKind::Selected && !app.selection().count(e.id)) continue;
                v.push_back(e.start);
                v.push_back(e.end());
                if (kind == EdgeKind::EditPoints) {
                    if (e.fadeIn > 0) v.push_back(e.start + e.fadeIn);
                    if (e.fadeOut > 0) v.push_back(e.end() - e.fadeOut);
                }
            }
        break;
    }
    const double now = app.cursor(), eps = frame() * 0.25;
    double best = dir > 0 ? 1e300 : -1.0;
    for (double t : v) {
        if (dir > 0 && t > now + eps && t < best) best = t;
        if (dir < 0 && t < now - eps && t > best) best = t;
    }
    if (dir > 0 && best >= 1e299) {
        if (kind == EdgeKind::Selected) return;
        best = std::max(now, p.length());
    }
    if (dir < 0 && best < 0) {
        if (kind == EdgeKind::Selected) return;
        best = 0.0;
    }
    keyCursor(best);
}

void TimelineWidget::Impl::timeSelectSelected(int dir)
{
    std::vector<std::pair<double, double>> evs;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (app.selection().count(e.id)) evs.emplace_back(e.start, e.end());
    std::sort(evs.begin(), evs.end());
    const double now = app.cursor(), eps = frame() * 0.25;
    const std::pair<double, double>* pick = nullptr;
    for (const auto& e : evs) {
        if (dir > 0 && e.first > now + eps) { pick = &e; break; }
        if (dir < 0 && e.first < now - eps) pick = &e;
    }
    if (!pick) return;
    const std::pair<double, double> sel = *pick;
    keyCursor(sel.first);
    app.setLoop(sel.first, sel.second);
}

void TimelineWidget::Impl::renameSelected()
{
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (app.selection().count(e.id)) {
                startRename(Naming::Event, e.id);
                return;
            }
    if (app.currentTrack) startRename(Naming::Track, app.currentTrack);
}

// Ctrl+Shift+Up / Down (VEGAS's Increase / Decrease Track Height): every track a step taller or shorter. The steps
// are a ladder of heights (the default heights of both kinds times 1.25 to a power, plus the least and most a track
// can be); each track goes to the next one up or down from where it is, so tracks of different heights come to match
// and Up then Down comes back. The heights are the tracks' own (Track::height, as VEGAS keeps them in the .veg): one
// undo step.
int TimelineWidget::Impl::heightStep(int h, int dir)
{
    std::vector<int> ladder = {kMinTrackH, kMaxTrackH};
    for (int def : {kVideoH, kAudioH})
        for (int k = -8; k <= 16; ++k) {
            const int v = (int)std::lround(def * std::pow(1.25, k));
            if (v > kMinTrackH && v < kMaxTrackH) ladder.push_back(v);
        }
    std::sort(ladder.begin(), ladder.end());
    ladder.erase(std::unique(ladder.begin(), ladder.end()), ladder.end());
    if (dir > 0) {
        for (int v : ladder)
            if (v > h) return v;
        return std::max(h, ladder.back());
    }
    for (size_t k = ladder.size(); k-- > 0;)
        if (ladder[k] < h) return ladder[k];
    return std::min(h, ladder.front());
}

void TimelineWidget::Impl::changeTrackHeights(int dir)
{
    minimized.clear();
    maximized.clear();
    const Project& p = app.project();
    std::map<uint64_t, int> next;
    for (const Track& t : p.tracks) {
        const int def = t.type == TrackType::Video ? kVideoH : kAudioH;
        const int now = trackHeight(t);
        const int h = heightStep(now, dir);
        const int value = h == def ? 0 : h;
        if (value != t.height) next[t.id] = value;
    }
    if (!next.empty())
        app.edit(dir > 0 ? "Increase Track Height" : "Decrease Track Height", [&](Project& pr) {
            for (Track& t : pr.tracks) {
                auto it = next.find(t.id);
                if (it != next.end()) t.height = it->second;
            }
        });
    layout();
    updateBars();
    q->update();
}

const std::vector<TimelineWidget::Impl::OptionKey>& TimelineWidget::Impl::optionKeys()
{
    static const std::vector<OptionKey> keys = {
        {"quantize", &TimelineOptions::quantize}, {"snapToMarkers", &TimelineOptions::snapToMarkers},
        {"ignoreGrouping", &TimelineOptions::ignoreGrouping}, {"autoRipple", &TimelineOptions::autoRipple},
        {"showWaveforms", &TimelineOptions::showWaveforms}, {"showEventButtons", &TimelineOptions::showEventButtons},
        {"eventHeaders", &TimelineOptions::eventHeaders}, {"showEnvelopes", &TimelineOptions::showEnvelopes},
        {"lockEnvelopes", &TimelineOptions::lockEnvelopes}, {"showBusTracks", &TimelineOptions::showBusTracks},
        {"showFadeLengths", &TimelineOptions::showFadeLengths}, {"snapToAllEvents", &TimelineOptions::snapToAllEvents},
        {"showTakeInfo", &TimelineOptions::showTakeInfo}, {"showMediaMarkers", &TimelineOptions::showMediaMarkers},
        {"edgeFrames", &TimelineOptions::edgeFrames}, {"slipAllTakes", &TimelineOptions::slipAllTakes},
        {"showMarkerLabels", &TimelineOptions::showMarkerLabels}, {"showEventLength", &TimelineOptions::showEventLength}};
    return keys;
}

void TimelineWidget::Impl::loadOptions()
{
    QSettings s;
    for (const OptionKey& k : optionKeys()) opt.*k.field = s.value(QString("timeline/") + k.key, opt.*k.field).toBool();
    const int ripple = s.value("timeline/rippleMode", (int)opt.ripple).toInt();
    if (ripple >= 0 && ripple <= 2) opt.ripple = (RippleMode)ripple;
    const QString grid = s.value("timeline/gridSpacing", QString::fromStdString(opt.gridSpacing)).toString();
    if (!grid.isEmpty()) opt.gridSpacing = grid.toStdString();
    opt.snapToGrid = app.prefs.snapToGrid;
}

void TimelineWidget::Impl::saveOptions()
{
    QSettings s;
    for (const OptionKey& k : optionKeys()) s.setValue(QString("timeline/") + k.key, opt.*k.field);
    s.setValue("timeline/rippleMode", (int)opt.ripple);
    s.setValue("timeline/gridSpacing", QString::fromStdString(opt.gridSpacing));
    if (app.prefs.snapToGrid != opt.snapToGrid) {
        app.prefs.snapToGrid = opt.snapToGrid;
        s.setValue("prefs/snapToGrid", opt.snapToGrid);
    }
}

void TimelineWidget::Impl::pressTrackEdge(const Hit& h)
{
    drag.kind = Drag::TrackHeight;
    std::set<uint64_t> ids = selectedTracks();
    if (!ids.count(h.track) || ids.size() < 2) ids = {h.track};
    for (const Track& t : app.project().tracks)
        if (ids.count(t.id)) drag.heights.push_back({t.id, trackHeight(t)});
}

void TimelineWidget::Impl::updateTrackEdge(const QPointF& pos)
{
    const int dy = (int)std::lround(pos.y() - drag.pressPos.y());
    if (!drag.started) {
        if (dy == 0) return;
        drag.started = true;
        for (const auto& x : drag.heights) {
            minimized.erase(x.first);
            maximized.erase(x.first);
        }
        app.beginDrag();
    }
    const std::vector<std::pair<uint64_t, int>> base = drag.heights;
    app.dragUpdate([&](Project& p) {
        for (const auto& x : base)
            if (Track* t = p.findTrack(x.first)) t->height = clampTrackH(x.second + dy);
    });
    int shown = 0;
    for (const auto& x : base)
        if (x.first == drag.hit.track) shown = clampTrackH(x.second + dy);
    app.status(strfmt("Track height: %d pixels", shown));
}

void TimelineWidget::Impl::clearGroups()
{
    std::set<uint64_t> groups;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (app.selection().count(e.id) && e.group) groups.insert(e.group);
    if (groups.empty()) return;
    app.edit("Clear Group", [&](Project& p) {
        for (Track& t : p.tracks)
            for (Event& e : t.events)
                if (groups.count(e.group)) e.group = 0;
    });
}

void TimelineWidget::Impl::selectGroups()
{
    std::set<uint64_t> ids = app.selection();
    const bool was = opt.ignoreGrouping;
    opt.ignoreGrouping = false;
    ids = withGroups(ids);
    opt.ignoreGrouping = was;
    app.setSelection(ids);
}

const Event* TimelineWidget::Impl::neighbourOf(const Track& t, const Event& e, int side, double fr)
{
    const Event* best = nullptr;
    for (const Event& o : t.events) {
        if (o.id == e.id) continue;
        if (side > 0) {
            if (o.start > e.start + 1e-9 && o.start <= e.end() + fr * 0.5 && o.end() > e.end() + 1e-9)
                if (!best || std::fabs(o.start - e.end()) < std::fabs(best->start - e.end())) best = &o;
        } else {
            if (o.end() < e.end() - 1e-9 && o.end() >= e.start - fr * 0.5 && o.start < e.start - 1e-9)
                if (!best || std::fabs(o.end() - e.start) < std::fabs(best->end() - e.start)) best = &o;
        }
    }
    return best;
}

// Plans an operation on the start (side -1) or end (+1) of the events 'ids' and of the events grouped with them whose
// edge is at the same place (unless grouping is ignored). False, with the reason, if it can't be done: nothing left
// to change, or a locked event in the way.
bool TimelineWidget::Impl::planEdge(const std::vector<uint64_t>& ids, int side, EdgeOp op, bool ignoreGroup, EdgeWork& w, std::string* why) const
{
    const Project& pr = app.project();
    const double fr = frame();
    w = EdgeWork();
    w.op = op;
    w.side = side;
    std::set<uint64_t> seen;
    auto addPart = [&](size_t k, const Event& e) {
        if (!seen.insert(e.id).second) return true;
        if (e.locked) {
            if (why) *why = "That event is locked (Edit > Switches > Lock): it cannot be trimmed.";
            return false;
        }
        w.parts.push_back(moverOf(pr, (int)k, e));
        return true;
    };
    for (uint64_t id : ids) {
        const Track* tr = nullptr;
        const Event* e = pr.findEvent(id, &tr);
        if (!e || !tr) continue;
        if (!addPart((size_t)trackIndex(tr->id), *e)) return false;
        if (e->group && !ignoreGroup && !opt.ignoreGrouping) {
            const double edge = side < 0 ? e->start : e->end();
            for (size_t k = 0; k < pr.tracks.size(); ++k)
                for (const Event& o : pr.tracks[k].events)
                    if (o.id != e->id && o.group == e->group && std::fabs((side < 0 ? o.start : o.end()) - edge) < fr * 0.5)
                        if (!addPart(k, o)) return false;
        }
    }
    if (w.parts.empty()) {
        if (why) *why = "Select an event first.";
        return false;
    }
    if (op == EdgeOp::Slide) {
        for (const Mover& m : w.parts) {
            const Track& t = pr.tracks[(size_t)m.track];
            const Event* e = pr.findEvent(m.id);
            const Event* o = e ? neighbourOf(t, *e, side, fr) : nullptr;
            if (!o || seen.count(o->id)) continue;
            if (o->locked) {
                if (why) *why = "The event next to it is locked: the edit cannot slide.";
                return false;
            }
            seen.insert(o->id);
            w.others.push_back(moverOf(pr, m.track, *o));
        }
    }
    w.ripple = opt.autoRipple && side > 0 && op != EdgeOp::Slide;
    if (w.ripple) {
        std::set<uint64_t> tracks;
        double from = 1e300;
        for (const Mover& m : w.parts) {
            tracks.insert(pr.tracks[(size_t)m.track].id);
            from = std::min(from, m.end());
        }
        w.followers = followersOf(pr, tracks, from, w.ids(), opt.ripple);
    }
    return true;
}

// The change 'd' limited to what the events allow: at least a frame long, not before zero, within the playback rates
// Reno allows, and not before the start of media that doesn't loop.
double TimelineWidget::Impl::clampEdge(const EdgeWork& w, double d) const
{
    const double fr = frame();
    double lo = -1e300, hi = 1e300;
    auto noLoop = [](const Mover& m) { return !(m.loop && m.mediaLength > 0); };
    auto trimLeft = [&](const Mover& m) {
        lo = std::max(lo, -m.start);
        hi = std::min(hi, m.length - fr);
        if (noLoop(m) && m.rate > 0) lo = std::max(lo, -m.offset / m.rate);
    };
    auto trimRight = [&](const Mover& m) { lo = std::max(lo, fr - m.length); };
    for (const Mover& m : w.parts) {
        switch (w.op) {
        case EdgeOp::Trim:
        case EdgeOp::Slide:
            if (w.side < 0) trimLeft(m);
            else trimRight(m);
            break;
        case EdgeOp::SlipTrim:
            if (w.side < 0) {
                lo = std::max(lo, -m.start);
                hi = std::min(hi, m.length - fr);
            } else {
                trimRight(m);
                if (noLoop(m) && m.rate > 0) hi = std::min(hi, m.offset / m.rate);
            }
            break;
        case EdgeOp::Stretch: {
            const double minLen = std::max(fr, m.length * m.rate / kMaxRate), maxLen = m.length * m.rate / kMinRate;
            if (w.side < 0) {
                lo = std::max(lo, std::max(-m.start, m.length - maxLen));
                hi = std::min(hi, m.length - minLen);
            } else {
                lo = std::max(lo, minLen - m.length);
                hi = std::min(hi, maxLen - m.length);
            }
            break;
        }
        }
    }
    for (const Mover& o : w.others) {
        if (w.side > 0) trimLeft(o);
        else trimRight(o);
    }
    if (lo > hi) return 0.0;
    return std::min(std::max(d, lo), hi);
}

double TimelineWidget::Impl::wrapOffset(const Mover& m, double offset)
{
    if (m.loop && m.mediaLength > 0) {
        offset = std::fmod(offset, m.mediaLength);
        if (offset < 0) offset += m.mediaLength;
    }
    return std::max(0.0, offset);
}

void TimelineWidget::Impl::trimLeftBy(Event& e, const Mover& m, double d)
{
    e.start = m.start + d;
    shiftEventEnvelopes(e, d);
    e.length = m.length - d;
    e.offset = wrapOffset(m, m.offset + d * m.rate);
    shiftTakes(e, d * m.rate);
}

void TimelineWidget::Impl::applyEdge(Project& p, const EdgeWork& w, double d) const
{
    const bool slipTakes = opt.slipAllTakes;
    if (w.ripple) w.followers.apply(p, d, opt.quantize);
    for (const Mover& m : w.parts) {
        Event* e = p.findEvent(m.id);
        if (!e) continue;
        switch (w.op) {
        case EdgeOp::Trim:
        case EdgeOp::Slide:
            if (w.side < 0) trimLeftBy(*e, m, d);
            else e->length = m.length + d;
            break;
        case EdgeOp::SlipTrim:
            if (w.side < 0) {
                e->start = m.start + d;
                e->length = m.length - d;
            } else {
                e->length = m.length + d;
                e->offset = wrapOffset(m, m.offset - d * m.rate);
                if (slipTakes) shiftTakes(*e, -d * m.rate);
            }
            break;
        case EdgeOp::Stretch:
            // the same stretch of media fills the new length (grouped video and audio get the same rate so they stay in
            // step; the engine keeps the pitch where the event's stretch method says so)
            if (w.side < 0) {
                e->start = m.start + d;
                e->length = m.length - d;
            } else {
                e->length = m.length + d;
            }
            e->rate = m.rate * m.length / e->length;
            break;
        }
        clampFades(*e);
    }
    for (const Mover& m : w.others) {
        Event* e = p.findEvent(m.id);
        if (!e) continue;
        if (w.side > 0) trimLeftBy(*e, m, d);
        else e->length = m.length + d;
        clampFades(*e);
    }
}

double TimelineWidget::Impl::edgeStep(const std::vector<uint64_t>& ids, int side, EdgeOp op, bool ignoreGroup, double delta)
{
    EdgeWork w;
    std::string why;
    if (!planEdge(ids, side, op, ignoreGroup, w, &why)) {
        app.status(why);
        return -1;
    }
    const double d = clampEdge(w, delta);
    if (std::fabs(d) < 1e-12) return -1;
    static const char* names[] = {"Trim Event", "Stretch Event", "Slip Trim Event", "Slide Edit"};
    app.edit(names[(int)w.op], [&](Project& p) { applyEdge(p, w, d); });
    if (side > 0 && op != EdgeOp::Slide) {
        std::set<uint64_t> tracks;
        double from = 1e300;
        for (const Mover& m : w.parts) {
            tracks.insert(app.project().tracks.size() > (size_t)m.track ? app.project().tracks[(size_t)m.track].id : 0);
            from = std::min(from, m.end());
        }
        if (!w.ripple) rememberEdit(tracks, from, d, w.ids());
    }
    const Mover& g = w.parts[0];
    return side < 0 ? g.start + d : g.end() + d;
}

bool TimelineWidget::Impl::planBody(const std::set<uint64_t>& ids, bool slide, bool ignoreGroup, BodyWork& w, std::string* why) const
{
    const Project& pr = app.project();
    w = BodyWork();
    w.slide = slide;
    std::set<uint64_t> all = ids;
    if (!ignoreGroup) all = withGroups(ids);
    for (size_t k = 0; k < pr.tracks.size(); ++k)
        for (const Event& e : pr.tracks[k].events)
            if (all.count(e.id)) {
                if (e.locked) {
                    if (why) *why = "That event is locked (Edit > Switches > Lock).";
                    return false;
                }
                w.parts.push_back(moverOf(pr, (int)k, e));
            }
    if (w.parts.empty() && why) *why = "Select an event first.";
    return !w.parts.empty();
}

double TimelineWidget::Impl::clampBody(const BodyWork& w, double d) const
{
    double lo = -1e300, hi = 1e300;
    for (const Mover& m : w.parts) {
        const bool noLoop = !(m.loop && m.mediaLength > 0);
        if (w.slide) {
            lo = std::max(lo, -m.start);
            if (noLoop && m.rate > 0) lo = std::max(lo, -m.offset / m.rate);
        } else if (noLoop && m.rate > 0) {
            hi = std::min(hi, m.offset / m.rate);
            if (m.mediaLength > 0) lo = std::max(lo, std::min(0.0, (m.offset + m.length * m.rate - m.mediaLength) / m.rate));
        }
    }
    if (lo > hi) return 0.0;
    return std::min(std::max(d, lo), hi);
}

void TimelineWidget::Impl::applyBody(Project& p, const BodyWork& w, double d) const
{
    for (const Mover& m : w.parts) {
        Event* e = p.findEvent(m.id);
        if (!e) continue;
        if (w.slide) {
            e->start = m.start + d;
            e->offset = wrapOffset(m, m.offset + d * m.rate);
            shiftEventEnvelopes(*e, d);
            shiftTakes(*e, d * m.rate);
        } else {
            e->offset = wrapOffset(m, m.offset - d * m.rate);
            if (opt.slipAllTakes) shiftTakes(*e, -d * m.rate);
        }
    }
}

void TimelineWidget::Impl::setEdge(uint64_t id, int side)
{
    edgeSel = EdgeSel();
    const Event* e = app.project().findEvent(id);
    if (!e) return;
    edgeSel.event = id;
    edgeSel.side = side;
    if (!app.selection().count(id)) app.selectOnly(id);
    keyCursor(side < 0 ? e->start : e->end());
    q->update();
}

void TimelineWidget::Impl::selectEdge(int dir)
{
    const Project& pr = app.project();
    if (edgeSel.expanded && edgeValid()) {
        if (dir < 0) edgeSel.which = edgeSel.which > 0 ? 0 : -1;
        else edgeSel.which = edgeSel.which < 0 ? 0 : 1;
        app.status(edgeSel.which == 0 ? "Expanded edit: both sides of the edit" : edgeSel.which < 0 ? "Expanded edit: the end of the event before the edit"
                                                                                                   : "Expanded edit: the start of the event after the edit");
        q->update();
        return;
    }
    if (edgeValid()) {
        const Track* tr = nullptr;
        const Event* e = pr.findEvent(edgeSel.event, &tr);
        if (!e || !tr) return;
        const double now = edgeSel.side < 0 ? e->start : e->end();
        struct Edge { double t; uint64_t id; int side; };
        std::vector<Edge> edges;
        for (const Event& o : tr->events) {
            edges.push_back(Edge{o.start, o.id, -1});
            edges.push_back(Edge{o.end(), o.id, 1});
        }
        std::stable_sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.t < b.t || (a.t == b.t && a.side > b.side); });
        int at = -1;
        for (size_t k = 0; k < edges.size(); ++k)
            if (edges[k].id == edgeSel.event && edges[k].side == edgeSel.side) at = (int)k;
        (void)now;
        const int next = at + (dir < 0 ? -1 : 1);
        if (at < 0 || next < 0 || next >= (int)edges.size()) return;
        setEdge(edges[(size_t)next].id, edges[(size_t)next].side);
        app.selectOnly(edges[(size_t)next].id);
        return;
    }
    const Event* pick = nullptr;
    bool onCurrent = false;
    for (const Track& t : pr.tracks)
        for (const Event& e : t.events)
            if (app.selection().count(e.id)) {
                const bool cur = t.id == app.currentTrack;
                if (!pick || (cur && !onCurrent) || (cur == onCurrent && e.start < pick->start)) {
                    pick = &e;
                    onCurrent = cur;
                }
            }
    if (pick) {
        setEdge(pick->id, dir < 0 ? -1 : 1);
        app.status(dir < 0 ? "Edge trimming: the event's start (Num 1 / 3 trim a frame, 4 / 6 a pixel; Num 5 or Esc to stop)"
                           : "Edge trimming: the event's end (Num 1 / 3 trim a frame, 4 / 6 a pixel; Num 5 or Esc to stop)");
        return;
    }
    cursorToEdge(dir, EdgeKind::Events);
}

// Num 5: into expanded edit mode at the selected edge (or the edit nearest the cursor on the current track), and out of
// it again.
void TimelineWidget::Impl::toggleExpandedEdit()
{
    if (edgeSel.expanded) {
        exitEditMode();
        return;
    }
    const Project& pr = app.project();
    const double fr = frame();
    const Event* a = nullptr;
    const Event* b = nullptr;
    if (edgeValid()) {
        const Track* tr = nullptr;
        const Event* e = pr.findEvent(edgeSel.event, &tr);
        const Event* o = (e && tr) ? neighbourOf(*tr, *e, edgeSel.side, fr) : nullptr;
        if (!o) {
            exitEditMode();
            return;
        }
        a = edgeSel.side > 0 ? e : o;
        b = edgeSel.side > 0 ? o : e;
    } else {
        const Track* tr = nullptr;
        for (const Track& t : pr.tracks)
            if (t.id == app.currentTrack) tr = &t;
        if (!tr) return;
        double best = 1e300;
        for (const Event& e : tr->events)
            if (const Event* o = neighbourOf(*tr, e, 1, fr)) {
                const double dist = std::fabs(e.end() - app.cursor());
                if (dist < best) {
                    best = dist;
                    a = &e;
                    b = o;
                }
            }
        if (!a) {
            app.status("Expanded edit mode works on an edit: two events that meet or overlap on the current track.");
            return;
        }
    }
    edgeSel = EdgeSel();
    edgeSel.event = a->id;
    edgeSel.other = b->id;
    edgeSel.side = 1;
    edgeSel.expanded = true;
    std::set<uint64_t> sel;
    sel.insert(a->id);
    sel.insert(b->id);
    app.setSelection(sel);
    keyCursor(a->end());
    app.status("Expanded edit mode: Num 1 / 3 slide the edit (7 / 9 one side of it); Num 5 to stop");
    q->update();
}

void TimelineWidget::Impl::exitEditMode()
{
    if (!edgeSel.event) return;
    edgeSel = EdgeSel();
    app.status(std::string());
    q->update();
}

void TimelineWidget::Impl::keypadEdit(int frames, int pixels, Qt::KeyboardModifiers mods)
{
    const bool ctrl = (mods & Qt::ControlModifier) != 0, alt = (mods & Qt::AltModifier) != 0, shift = (mods & Qt::ShiftModifier) != 0;
    const Project& pr = app.project();
    double delta = frames * frame() + pixels / pps;
    if (opt.quantize && pixels != 0) delta = (pixels > 0 ? 1.0 : -1.0) * std::max(frame(), std::fabs(pr.quantize(std::fabs(pixels / pps))));
    if (edgeValid()) {
        uint64_t id = edgeSel.event;
        int side = edgeSel.side;
        EdgeOp op = ctrl && alt ? EdgeOp::Slide : ctrl ? EdgeOp::Stretch : alt ? EdgeOp::SlipTrim : EdgeOp::Trim;
        if (edgeSel.expanded) {
            if (edgeSel.which < 0) side = 1;
            else if (edgeSel.which > 0) { id = edgeSel.other; side = -1; }
            else { side = 1; if (!ctrl && !alt) op = EdgeOp::Slide; }
        }
        const double at = edgeStep(std::vector<uint64_t>{id}, side, op, shift, delta);
        if (at >= 0) keyCursor(at);
        return;
    }
    const std::set<uint64_t> sel = app.selection();
    if (sel.empty()) return;
    if (ctrl && alt) {
        BodyWork w;
        std::string why;
        if (!planBody(sel, true, shift, w, &why)) { app.status(why); return; }
        const double d = clampBody(w, delta);
        if (d != 0) app.edit("Slide Events", [&](Project& p) { applyBody(p, w, d); });
    } else if (ctrl) {
        edgeStep(std::vector<uint64_t>(sel.begin(), sel.end()), 1, EdgeOp::Stretch, shift, delta);
    } else if (alt) {
        BodyWork w;
        std::string why;
        if (!planBody(sel, false, shift, w, &why)) { app.status(why); return; }
        const double d = clampBody(w, delta);
        if (d != 0) app.edit("Slip Events", [&](Project& p) { applyBody(p, w, d); });
    } else {
        const bool was = opt.ignoreGrouping;
        if (shift) opt.ignoreGrouping = true;
        nudge(frames, pixels, 0);
        opt.ignoreGrouping = was;
    }
}

void TimelineWidget::Impl::jog(int dir)
{
    if (edgeValid()) {
        keypadEdit(dir, 0, Qt::NoModifier);
        return;
    }
    const Project& p = app.project();
    if (playback && playback->playing() && jogging) playback->stop();
    keyCursor(p.frameTime(std::max<int64_t>(0, p.frameAt(app.cursor()) + dir)));
    jogging = false;
    if (playback && !playback->playing()) {
        playback->previewCursor(std::max(0.1, 3.0 * frame()));
        jogging = true;
    }
}

void TimelineWidget::Impl::rememberEdit(const std::set<uint64_t>& tracks, double from, double delta, const std::set<uint64_t>& exclude)
{
    lastEdit = LastEdit();
    lastEdit.tracks = tracks;
    lastEdit.from = from;
    lastEdit.delta = delta;
    lastEdit.exclude = exclude;
    lastEdit.revision = app.revision();
}

void TimelineWidget::Impl::rememberRemoval(const std::map<uint64_t, Ranges>& removed)
{
    lastEdit = LastEdit();
    lastEdit.removed = removed;
    lastEdit.revision = app.revision();
}

void TimelineWidget::Impl::postEditRipple(RippleMode mode)
{
    if (!lastEdit.revision || lastEdit.revision != app.revision()) {
        app.status("Post-edit ripple follows an edit straight away: trim, move, delete or paste first (Auto Ripple, Ctrl+L, does it as you go).");
        return;
    }
    const LastEdit e = lastEdit;
    lastEdit = LastEdit();
    if (!e.removed.empty()) {
        app.edit("Post-Edit Ripple", [&](Project& p) { rippleClose(p, e.removed, mode); });
        return;
    }
    if (std::fabs(e.delta) < 1e-9) return;
    const Followers f = followersOf(app.project(), e.tracks, e.from, e.exclude, mode);
    const bool quantize = opt.quantize;
    app.edit("Post-Edit Ripple", [&](Project& p) { f.apply(p, e.delta, quantize); });
}

uint64_t TimelineWidget::Impl::splitIn(Project& p, uint64_t id, double t, std::map<uint64_t, uint64_t>* groups) const
{
    Track* tr = nullptr;
    Event* e = p.findEvent(id, &tr);
    if (!e || !tr || t <= e->start + 1e-9 || t >= e->end() - 1e-9) return 0;
    Event right = *e;
    right.id = p.newId();
    right.start = t;
    right.length = e->end() - t;
    right.offset = e->offset + (t - e->start) * e->rate;
    shiftTakes(right, (t - e->start) * e->rate);
    shiftEventEnvelopes(right, t - e->start);
    shiftEventKeyframes(right, t - e->start);
    right.fadeIn = 0;
    right.transition = FxSlot();
    for (auto& s : right.fx) s.id = p.newId();
    if (groups && right.group) {
        auto it = groups->find(right.group);
        if (it == groups->end()) it = groups->emplace(right.group, p.newId()).first;
        right.group = it->second;
    }
    e->length = t - e->start;
    e->fadeOut = 0;
    const uint64_t rid = right.id;
    tr->events.push_back(right);
    tr->sortEvents();
    return rid;
}

std::pair<std::string, std::string> TimelineWidget::Impl::transitionNamed(const char* name)
{
    for (const VideoFxEntry& v : Catalog::get().video())
        if (v.transition && v.ok && QString::fromStdString(v.label).contains(QString::fromUtf8(name), Qt::CaseInsensitive)) return std::make_pair(v.id, v.label);
    return std::make_pair(std::string(), std::string());
}

void TimelineWidget::Impl::smartSplit()
{
    const double a = quantized(app.loopStart), b = quantized(app.loopEnd), fr = frame();
    if (!(b > a)) { app.status("Smart Split: select an event and make a time selection over the part to take out."); return; }
    std::vector<uint64_t> ids;
    const std::set<uint64_t> all = withGroups(app.selection());
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (all.count(e.id) && e.start < a - fr * 0.5 && e.end() > b + fr * 0.5) {
                if (e.locked) { app.status("That event is locked (Edit > Switches > Lock)."); return; }
                ids.push_back(e.id);
            }
    if (ids.empty()) { app.status("Smart Split: the time selection must lie inside a selected event."); return; }
    const auto warp = transitionNamed("Warp Flow");
    const bool ripple = opt.autoRipple, quantize = opt.quantize;
    std::set<uint64_t> made;
    app.edit("Smart Split", [&](Project& p) {
        app.fx.captureStates(p);
        const uint64_t group = p.newId();
        std::set<uint64_t> tracks;
        std::set<uint64_t> pieces;
        double removed = 0;
        for (uint64_t id : ids) {
            Track* tr = nullptr;
            Event* e = p.findEvent(id, &tr);
            if (!e || !tr) continue;
            const uint64_t right = splitIn(p, id, b);
            const uint64_t middle = splitIn(p, id, a);
            if (!right || !middle) continue;
            tr = nullptr;
            p.findEvent(id, &tr);
            if (!tr) continue;
            tr->events.erase(std::remove_if(tr->events.begin(), tr->events.end(), [&](const Event& o) { return o.id == middle; }), tr->events.end());
            Event* l = p.findEvent(id);
            Event* r = p.findEvent(right);
            if (!l || !r) continue;
            // the part after slides back over the part before by five frames (Preferences > Editing "Smart split default
            // length (frames)"), and the two cross-fade there
            const double overlap = std::max(0.0, std::min({std::max(1, vprefs::number("prefs/smartSplitFrames")) * fr, l->length - fr, r->length - fr}));
            const double d = (a - overlap) - r->start;
            r->start += d;
            if (tr->type == TrackType::Video && !warp.first.empty()) {
                r->transition = FxSlot();
                r->transition.id = p.newId();
                r->transition.kind = "ofx";
                r->transition.pluginId = warp.first;
                r->transition.name = warp.second;
            }
            l->group = group;
            r->group = group;
            pieces.insert(id);
            pieces.insert(right);
            made.insert(id);
            made.insert(right);
            tracks.insert(tr->id);
            removed = std::max(removed, -d);
            tr->sortEvents();
        }
        if (ripple && removed > 0)
            for (Track& t : p.tracks) {
                if (opt.ripple != RippleMode::AllTracks && !tracks.count(t.id)) continue;
                for (Event& e : t.events)
                    if (!pieces.count(e.id) && !e.locked && e.start >= b - 1e-9) e.start = std::max(0.0, quantize ? p.quantize(e.start - removed) : e.start - removed);
                t.sortEvents();
            }
    });
    app.setSelection(made);
    app.setLoop(0, 0);
    app.status(warp.first.empty() ? "Smart Split: joined with a five-frame cross-fade (VEGAS's Warp Flow transition is not installed)."
                                  : "Smart Split: joined with a five-frame Warp Flow transition.");
}

// The tracks a command at the cursor works on: those of the selected events (and events grouped with them), else the
// current track.
std::set<uint64_t> TimelineWidget::Impl::commandTracks() const
{
    std::set<uint64_t> out;
    const std::set<uint64_t> ids = withGroups(app.selection());
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (ids.count(e.id)) out.insert(t.id);
    if (out.empty() && app.currentTrack) out.insert(app.currentTrack);
    return out;
}

void TimelineWidget::Impl::convertCutToTransition(int kind)
{
    const double c = app.cursor(), fr = frame(), want = std::max(fr, app.prefs.cutToOverlap);
    const std::set<uint64_t> tracks = commandTracks();
    std::vector<std::pair<uint64_t, uint64_t>> cuts;
    for (const Track& t : app.project().tracks) {
        if (!tracks.count(t.id)) continue;
        for (const Event& e : t.events) {
            if (std::fabs(e.end() - c) > fr * 0.5) continue;
            for (const Event& o : t.events)
                if (o.id != e.id && std::fabs(o.start - e.end()) < fr * 0.5 && !e.locked && !o.locked) cuts.emplace_back(e.id, o.id);
        }
    }
    if (cuts.empty()) { app.status("Put the cursor on a cut between two events (that meet) first."); return; }
    const char* names[] = {"", "Dissolve", "Linear Wipe"};
    const auto fx = kind > 0 ? transitionNamed(names[kind]) : std::make_pair(std::string(), std::string());
    app.edit("Convert Cut to Transition", [&](Project& p) {
        for (const auto& cut : cuts) {
            Track* tr = nullptr;
            Event* a = p.findEvent(cut.first, &tr);
            Event* b = p.findEvent(cut.second);
            if (!a || !b || !tr) continue;
            // Preferences > Editing "Cut-to-overlap conversion" Alignment: Before cut (the overlap ends at the cut:
            // the later event reaches
            // back the whole way), Centered on cut (each half way), After cut (the earlier event reaches on); each as
            // far as its media allows
            const QString align = vprefs::text("prefs/overlapAlign");
            const double back = align == "before" ? 1.0 : align == "after" ? 0.0 : 0.5;
            const Mover ma = moverOf(p, 0, *a), mb = moverOf(p, 0, *b);
            double half = want * back;
            if (!(mb.loop && mb.mediaLength > 0) && mb.rate > 0) half = std::min(half, mb.offset / mb.rate);
            double halfA = want * (1.0 - back);
            if (!(ma.loop && ma.mediaLength > 0) && ma.mediaLength > 0 && ma.rate > 0)
                halfA = std::min(halfA, std::max(0.0, (ma.mediaLength - ma.offset) / ma.rate - ma.length));
            half = std::max(0.0, std::min(half, b->start));
            if (half + halfA < fr * 0.5) continue;
            a->length = ma.length + halfA;
            trimLeftBy(*b, mb, -half);
            if (tr->type == TrackType::Video) {
                b->transition = FxSlot();
                if (!fx.first.empty()) {
                    b->transition.id = p.newId();
                    b->transition.kind = "ofx";
                    b->transition.pluginId = fx.first;
                    b->transition.name = fx.second;
                }
            }
        }
    });
    if (kind > 0 && fx.first.empty()) app.status(std::string("The ") + names[kind] + " transition is not installed: the cut became a cross-fade.");
}

void TimelineWidget::Impl::convertTransitionToCut()
{
    const double c = app.cursor(), fr = frame();
    const std::set<uint64_t> tracks = commandTracks();
    std::vector<std::pair<uint64_t, uint64_t>> pairs;
    for (const Track& t : app.project().tracks) {
        if (!tracks.count(t.id)) continue;
        for (const Event& b : t.events)
            if (const Event* a = fadesFrom(t, b))
                if (c >= b.start - fr * 0.5 && c <= a->end() + fr * 0.5 && !a->locked && !b.locked) pairs.emplace_back(a->id, b.id);
    }
    if (pairs.empty()) { app.status("Put the cursor in a cross-fade or transition first."); return; }
    const bool quantize = opt.quantize;
    app.edit("Convert Transition to Cut", [&](Project& p) {
        for (const auto& pr : pairs) {
            Event* a = p.findEvent(pr.first);
            Event* b = p.findEvent(pr.second);
            if (!a || !b) continue;
            double m = (b->start + a->end()) * 0.5;
            if (quantize) m = p.quantize(m);
            const Mover mb = moverOf(p, 0, *b);
            a->length = std::max(fr, m - a->start);
            trimLeftBy(*b, mb, m - mb.start);
            b->transition = FxSlot();
            clampFades(*a);
            clampFades(*b);
        }
    });
}

void TimelineWidget::Impl::addMarkerOfKind(const char* kind, double at, double length, const std::string& command, const std::string& name, const char* what)
{
    app.edit(what, [&](Project& p) {
        Project::Marker m;
        m.id = p.newId();
        m.time = at;
        m.length = length;
        m.region = length > 0;
        m.kind = kind;
        m.command = command;
        m.name = name;
        p.markers.push_back(m);
    });
}

void TimelineWidget::Impl::insertCdTrack()
{
    double a = quantized(app.cursor()), b = 0;
    if (app.loopEnd > app.loopStart) {
        a = app.loopStart;
        b = app.loopEnd;
    } else {
        b = std::max(app.project().length(), a + 1.0);
        for (const auto& m : app.project().markers)
            if (m.kind == "cdtrack" && m.time > a + 1e-6) b = std::min(b, m.time);
    }
    if (!(b > a)) return;
    addMarkerOfKind("cdtrack", a, b - a, std::string(), std::string(), "Insert CD Track Region");
}

void TimelineWidget::Impl::insertCdIndex()
{
    const double at = quantized(app.cursor());
    for (const auto& m : app.project().markers)
        if (m.kind == "cdindex" && std::fabs(m.time - at) < frame() * 0.5) { app.status("There is a CD index here already."); return; }
    addMarkerOfKind("cdindex", at, 0, std::string(), std::string(), "Insert CD Track Index");
}

void TimelineWidget::Impl::cursorToCd(int dir, bool tracks)
{
    std::vector<double> v;
    for (const auto& m : app.project().markers) {
        if (m.kind == "cdtrack") {
            v.push_back(m.time);
            if (!tracks) v.push_back(m.time + m.length);
        } else if (!tracks && m.kind == "cdindex") {
            v.push_back(m.time);
        } else if (!tracks && m.kind.empty() && m.region) {
            v.push_back(m.time);
        }
    }
    const double now = app.cursor(), eps = frame() * 0.25;
    double best = dir > 0 ? 1e300 : -1.0;
    for (double t : v) {
        if (dir > 0 && t > now + eps && t < best) best = t;
        if (dir < 0 && t < now - eps && t > best) best = t;
    }
    if (best < 0 || best >= 1e299) { app.status(tracks ? "No more CD tracks that way." : "No more CD indexes or regions that way."); return; }
    keyCursor(best);
}

// VEGAS's Command Properties on a command marker (its tag's Edit..., or a double-click on it)
void TimelineWidget::Impl::editCommandMarker(uint64_t id)
{
    const int k = markerIndex(id);
    if (k < 0) return;
    const Project::Marker m = app.project().markers[(size_t)k];
    if (m.kind != "command") return;
    CommandProps c;
    c.command = m.command;
    c.parameter = m.name;
    c.comment = m.comment;
    c.position = m.time;
    if (!commandPropertiesDialog(q, c)) return;
    app.edit("Edit Command Marker", [&](Project& p) {
        for (Project::Marker& x : p.markers)
            if (x.id == id) {
                x.command = c.command;
                x.name = c.parameter;
                x.comment = c.comment;
                x.time = std::max(0.0, quantized(c.position));
            }
    });
}

void TimelineWidget::Impl::rebuildPeaks()
{
    loader.reset(new Loader(app.mediaOpener, [this] { loaderReady(); }, true));
    thumbs.clear();
    app.status("Rebuilding the audio peaks.");
    q->update();
}

void TimelineWidget::Impl::addTakesToSelected(const std::vector<uint64_t>& media)
{
    int added = 0;
    for (uint64_t id : app.selection()) added += app.addTakes(id, media);
    if (!added) app.status("Nothing to add: select an event, and media with the event's kind of picture or sound.");
}

}  // namespace app
}  // namespace reno
