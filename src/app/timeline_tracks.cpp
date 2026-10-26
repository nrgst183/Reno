// Track list commands of the timeline: auto-scroll, track cut/copy/paste, multicamera.
#include "timeline_internal.h"

namespace reno {
namespace app {

void TimelineWidget::Impl::beginWheelScroll(const QPointF& at)
{
    wheelScrolling = true;
    wheelScrollFrom = wheelScrollTo = at;
    if (!wheelScrollTimer) {
        wheelScrollTimer = new QTimer(q);
        wheelScrollTimer->setInterval(40);
        QObject::connect(wheelScrollTimer, &QTimer::timeout, q, [this] { wheelScrollTick(); });
    }
    wheelScrollTimer->start();
    q->setCursor(Qt::SizeAllCursor);
}

void TimelineWidget::Impl::wheelScrollTick()
{
    if (!wheelScrolling) return;
    const QPointF d = wheelScrollTo - wheelScrollFrom;
    const double dx = std::fabs(d.x()) < 6.0 ? 0.0 : d.x() - (d.x() > 0 ? 6.0 : -6.0);
    const double dy = std::fabs(d.y()) < 6.0 ? 0.0 : d.y() - (d.y() > 0 ? 6.0 : -6.0);
    if (dx != 0) setViewStart(std::max(0.0, viewStart + dx * 0.25 / pps));
    if (dy != 0) setScrollY(scrollY + (int)std::lround(dy * 0.25));
}

void TimelineWidget::Impl::endWheelScroll()
{
    wheelScrolling = false;
    if (wheelScrollTimer) wheelScrollTimer->stop();
    q->unsetCursor();
}

void TimelineWidget::Impl::toggleTrackList()
{
    if (listWidthBefore > 0) {
        setHeaderWidth(listWidthBefore);
        listWidthBefore = 0;
    } else {
        listWidthBefore = headerW;
        setHeaderWidth(kHeaderMin);
    }
}

std::set<uint64_t> TimelineWidget::Impl::selectedTracks() const
{
    std::set<uint64_t> out;
    const Project& p = app.project();
    for (uint64_t id : selTracks)
        if (const_cast<Project&>(p).findTrack(id)) out.insert(id);
    if (app.currentTrack) out.insert(app.currentTrack);
    return out;
}

std::vector<uint64_t> TimelineWidget::Impl::selectedTracksInOrder() const
{
    const std::set<uint64_t> sel = selectedTracks();
    std::vector<uint64_t> out;
    for (const Track& t : app.project().tracks)
        if (sel.count(t.id)) out.push_back(t.id);
    return out;
}

void TimelineWidget::Impl::copyTracks(bool cut)
{
    const std::vector<uint64_t> ids = selectedTracksInOrder();
    if (ids.empty()) return;
    Project copy = app.project();
    app.fx.captureStates(copy);
    trackClip.clear();
    for (uint64_t id : ids)
        if (const Track* t = copy.findTrack(id)) trackClip.push_back(*t);
    if (cut) deleteTracks(ids, "Cut Tracks");
    else app.status(strfmt("%d track%s copied.", (int)trackClip.size(), trackClip.size() == 1 ? "" : "s"));
}

bool TimelineWidget::Impl::confirmTrackDelete(const std::vector<uint64_t>& ids)
{
    if (!vprefs::on("prefs/warnDeleteTrack")) return true;
    bool events = false;
    for (uint64_t id : ids)
        for (const Track& t : app.project().tracks)
            if (t.id == id) events = events || !t.events.empty();
    if (!events) return true;
    return QMessageBox::question(q, "Delete Track", ids.size() == 1 ? "The track you are deleting holds at least one event. Delete it anyway?"
                                                                    : "A track you are deleting holds at least one event. Delete them anyway?",
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes;
}

void TimelineWidget::Impl::deleteTracks(const std::vector<uint64_t>& ids, const char* what)
{
    if (ids.empty()) return;
    const std::set<uint64_t> gone(ids.begin(), ids.end());
    app.edit(what, [&](Project& p) {
        p.tracks.erase(std::remove_if(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return gone.count(t.id) != 0; }), p.tracks.end());
    });
    selTracks.clear();
    if (gone.count(app.currentTrack)) {
        app.currentTrack = app.project().tracks.empty() ? 0 : app.project().tracks.front().id;
        app.notify(kSelection);
    }
}

void TimelineWidget::Impl::pasteTracks(uint64_t after)
{
    if (trackClip.empty()) return;
    std::vector<uint64_t> made;
    app.edit(trackClip.size() == 1 ? "Paste Track" : "Paste Tracks", [&](Project& p) {
        size_t at = 0;
        for (size_t k = 0; k < p.tracks.size(); ++k)
            if (p.tracks[k].id == after) at = k + 1;
        std::map<uint64_t, uint64_t> groups;
        for (const Track& src : trackClip) {
            Track t = src;
            t.id = p.newId();
            t.veg.reset();
            std::map<uint64_t, uint64_t> slotMap;
            for (auto& fx : t.fx) { const uint64_t n = p.newId(); slotMap[fx.id] = n; fx.id = n; fx.veg.reset(); }
            t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [&](const Send& sd) { return !p.findBus(sd.bus); }), t.sends.end());
            t.events.erase(std::remove_if(t.events.begin(), t.events.end(), [&](const Event& e) { return e.mediaId && !p.findMedia(e.mediaId); }), t.events.end());
            for (Event& e : t.events) {
                e.id = p.newId();
                e.veg.reset();
                for (auto& fx : e.fx) { const uint64_t n = p.newId(); slotMap[fx.id] = n; fx.id = n; fx.veg.reset(); }
                if (!e.transition.kind.empty()) e.transition.id = p.newId();
                if (e.group) {
                    auto it = groups.find(e.group);
                    if (it == groups.end()) it = groups.emplace(e.group, p.newId()).first;
                    e.group = it->second;
                }
                for (Envelope& env : e.envelopes) { env.veg.reset(); if (env.kind == "fx" && slotMap.count(env.target)) env.target = slotMap[env.target]; }
            }
            for (Envelope& env : t.envelopes) { env.veg.reset(); if (env.kind == "fx" && slotMap.count(env.target)) env.target = slotMap[env.target]; }
            p.tracks.insert(p.tracks.begin() + (std::ptrdiff_t)std::min(at, p.tracks.size()), t);
            made.push_back(t.id);
            ++at;
        }
    });
    if (!made.empty()) {
        selTracks = std::set<uint64_t>(made.begin(), made.end());
        app.currentTrack = made.front();
        app.notify(kSelection);
    }
}

void TimelineWidget::Impl::changeLevelOf(const std::set<uint64_t>& ids, int dir, bool pan, bool fine)
{
    if (ids.empty()) return;
    const double step = fine ? 0.1 : 1.0;
    std::string say;
    app.edit(pan ? "Track Pan" : "Track Volume", [&](Project& p) {
        for (Track& t : p.tracks) {
            if (!ids.count(t.id)) continue;
            if (pan) {
                if (t.type != TrackType::Audio) continue;
                t.pan = clampd(std::round((t.pan + dir * 0.01 * step) * 1000.0) / 1000.0, -1.0, 1.0);
                say = "Pan: " + panText(t.pan).toStdString();
            } else if (t.type == TrackType::Video) {
                t.level = clampd(std::round((t.level + dir * 0.01 * step) * 1000.0) / 1000.0, 0.0, 1.0);
                say = "Level: " + levelText(true, t.level).toStdString();
            } else {
                const double db = t.level > 1e-6 ? 20.0 * std::log10(t.level) : -60.5;
                const double next = std::min(12.0, std::round((db + dir * 0.5 * step) * 20.0) / 20.0);
                t.level = next <= -60.0 ? 0.0 : std::pow(10.0, next / 20.0);
                say = "Volume: " + levelText(false, t.level).toStdString();
            }
        }
    });
    app.status(say);
}

void TimelineWidget::Impl::groupTracks(bool group)
{
    const std::set<uint64_t> ids = selectedTracks();
    if (ids.empty()) return;
    if (group && ids.size() < 2) { app.status("Select two or more tracks first (Ctrl+click their headers)."); return; }
    app.edit(group ? "Group Tracks" : "Ungroup Tracks", [&](Project& p) {
        const uint64_t g = group ? p.newId() : 0;
        for (Track& t : p.tracks)
            if (ids.count(t.id)) t.trackGroup = g;
    });
}

uint64_t TimelineWidget::Impl::currentTrackGroup() const
{
    for (const Track& t : app.project().tracks)
        if (t.id == app.currentTrack) return t.trackGroup;
    return 0;
}

void TimelineWidget::Impl::collapseTrackGroup(bool collapse)
{
    const uint64_t g = currentTrackGroup();
    if (!g) { app.status("The current track is in no track group."); return; }
    if (collapse) collapsedGroups.insert(g);
    else collapsedGroups.erase(g);
    layout();
    updateBars();
    q->update();
}

void TimelineWidget::Impl::muteSoloTrackGroup(bool solo, bool exclusive)
{
    const uint64_t g = currentTrackGroup();
    if (!g) { app.status("The current track is in no track group (Tracks > Track Group > Group Selected Tracks)."); return; }
    bool all = true;
    for (const Track& t : app.project().tracks)
        if (t.trackGroup == g) all = all && (solo ? t.solo : t.mute);
    const bool on = exclusive ? true : !all;
    app.edit(solo ? "Solo Track Group" : "Mute Track Group", [&](Project& p) {
        for (Track& t : p.tracks) {
            bool& flag = solo ? t.solo : t.mute;
            if (t.trackGroup == g) flag = on;
            else if (exclusive) flag = false;
        }
    });
}

void TimelineWidget::Impl::createMulticamTrack()
{
    const Project& pr = app.project();
    std::vector<size_t> cams;
    for (size_t k = 0; k < pr.tracks.size(); ++k) {
        const Track& t = pr.tracks[k];
        if (t.type != TrackType::Video || t.events.empty()) continue;
        bool sel = app.selection().empty();
        for (const Event& e : t.events) sel = sel || app.selection().count(e.id);
        if (sel) {
            for (const Event& e : t.events)
                if (e.locked) { app.status("A locked event is in the way (Edit > Switches > Lock)."); return; }
            cams.push_back(k);
        }
    }
    if (cams.size() < 2) { app.status("Create Multicamera Track: put each camera's video on a track of its own, lined up, and select them (or nothing, for every video track)."); return; }
    std::vector<double> cuts;
    for (size_t k : cams)
        for (const Event& e : pr.tracks[k].events) {
            cuts.push_back(e.start);
            cuts.push_back(e.end());
        }
    std::sort(cuts.begin(), cuts.end());
    const double fr = frame();
    std::vector<Event> made;
    for (size_t c = 0; c + 1 < cuts.size(); ++c) {
        const double a = cuts[c], b = cuts[c + 1];
        if (b - a < fr * 0.5) continue;
        std::vector<const Event*> here;
        for (size_t k : cams)
            for (const Event& e : pr.tracks[k].events)
                if (e.start <= a + 1e-9 && e.end() >= b - 1e-9) { here.push_back(&e); break; }
        if (here.empty()) continue;
        std::vector<Take> takes;
        for (const Event* e : here) {
            Take t;
            t.mediaId = e->mediaId;
            t.offset = e->offset + (a - e->start) * e->rate;
            const MediaItem* m = pr.findMedia(e->mediaId);
            t.name = m ? m->name : e->name;
            takes.push_back(t);
        }
        if (!made.empty() && std::fabs(made.back().end() - a) < 1e-6) {
            Event& last = made.back();
            const std::vector<Take> was = AppState::takesOf(last);
            bool same = was.size() == takes.size();
            for (size_t k = 0; same && k < takes.size(); ++k)
                same = was[k].mediaId == takes[k].mediaId && std::fabs(was[k].offset + last.length * last.rate - takes[k].offset) < 1e-4;
            if (same) {
                last.length = b - last.start;
                continue;
            }
        }
        Event e = *here[0];
        e.start = a;
        e.length = b - a;
        e.offset = takes[0].offset;
        e.name = takes[0].name;
        e.fadeIn = e.fadeOut = 0;
        e.group = 0;
        e.envelopes.clear();
        e.transition = FxSlot();
        e.takes.assign(takes.begin() + 1, takes.end());
        e.activeTake = 0;
        made.push_back(e);
    }
    if (made.empty()) return;
    std::set<uint64_t> ids;
    uint64_t trackId = 0;
    app.edit("Create Multicamera Track", [&](Project& p) {
        app.fx.captureStates(p);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.name = "Multicamera";
        for (Event& e : made) {
            e.id = p.newId();
            for (auto& s : e.fx) s.id = p.newId();
            ids.insert(e.id);
            t.events.push_back(e);
        }
        t.sortEvents();
        std::set<uint64_t> gone;
        for (size_t k : cams) gone.insert(p.tracks[k].id);
        const size_t at = cams[0];
        p.tracks.insert(p.tracks.begin() + (std::ptrdiff_t)at, t);
        p.tracks.erase(std::remove_if(p.tracks.begin(), p.tracks.end(), [&](const Track& o) { return gone.count(o.id) != 0; }), p.tracks.end());
        trackId = t.id;
    });
    app.currentTrack = trackId;
    app.setSelection(ids);
    app.status(strfmt("Multicamera track made from %d cameras: Ctrl+Shift+D turns multicamera editing on, then 1 ... 9 choose a camera.", (int)cams.size()));
}

// The multicamera track: the current track if its events have takes, else the first video track whose do.
const Track* TimelineWidget::Impl::multicamTrack() const
{
    const Track* first = nullptr;
    for (const Track& t : app.project().tracks) {
        if (t.type != TrackType::Video) continue;
        bool takes = false;
        for (const Event& e : t.events) takes = takes || !e.takes.empty();
        if (!takes) continue;
        if (t.id == app.currentTrack) return &t;
        if (!first) first = &t;
    }
    return first;
}

bool TimelineWidget::Impl::multicamCut(int take, bool crossfade)
{
    const Track* tr = multicamTrack();
    if (!tr) { app.status("There is no multicamera track (Tools > Multicamera > Create Multicamera Track)."); return false; }
    const bool playing = playback && playback->playing();
    const double t = quantized(playing ? playback->position() : app.cursor());
    const Event* e = nullptr;
    for (const Event& o : tr->events)
        if (o.start <= t + 1e-9 && o.end() > t + 1e-9) e = &o;
    if (!e) return false;
    const int index = take - 1;
    if (index < 0 || index > (int)e->takes.size()) { app.status(strfmt("There is no camera %d here.", take)); return false; }
    if (e->locked) { app.status("That event is locked."); return false; }
    const uint64_t id = e->id;
    const double fr = frame();
    if (!playing && !crossfade) {
        app.chooseTake(std::set<uint64_t>{id}, index, "Choose Camera");
        return true;
    }
    const double overlap = crossfade ? std::max(fr, app.prefs.cutToOverlap) : 0.0;
    app.edit(crossfade ? "Multicamera Cross-fade" : "Multicamera Cut", [&](Project& p) {
        uint64_t right = (t > e->start + fr * 0.5) ? splitIn(p, id, t) : id;
        if (!right) right = id;
        Event* r = p.findEvent(right);
        if (!r) return;
        AppState::setActiveTake(*r, index);
        if (crossfade && right != id) {
            const Mover m = moverOf(p, 0, *r);
            double back = std::min(overlap, std::max(0.0, t - p.findEvent(id)->start - fr));
            if (m.rate > 0) back = std::min(back, m.offset / m.rate);
            for (const Take& k : r->takes) back = std::min(back, m.rate > 0 ? k.offset / m.rate : back);
            if (back > 0) trimLeftBy(*r, m, -back);
        }
    });
    return true;
}

}  // namespace app
}  // namespace reno
