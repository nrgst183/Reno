// Envelope keys and dragging media, effects and transitions onto the timeline.
#include "timeline_internal.h"

namespace reno {
namespace app {

EnvelopeRef TimelineWidget::Impl::trackEnvelopeRef(const std::string& kind) const
{
    EnvelopeRef r;
    const Row* row = rowOf(app.currentTrack);
    if (!row) return r;
    r.owner.kind = EnvelopeOwner::Track;
    r.owner.id = row->id;
    if (row->video) r.kind = kind == "volume" ? "composite" : std::string();
    else r.kind = kind;
    return r;
}

void TimelineWidget::Impl::toggleTrackEnvelope(const std::string& kind)
{
    const EnvelopeRef r = trackEnvelopeRef(kind);
    if (!r.valid()) {
        app.status("Select a track first" + std::string(kind == "pan" ? " (an audio track: video tracks have no pan)." : "."));
        return;
    }
    app.toggleEnvelope(r);
    if (const Envelope* e = AppState::findEnvelope(app.project(), r))
        if (e->visible) frontEnv = r;
    q->update();
}

void TimelineWidget::Impl::cycleEnvelope(int dir)
{
    EnvelopeOwner o;
    o.kind = EnvelopeOwner::Track;
    o.id = app.currentTrack;
    const Envelopes* list = AppState::envelopesOf(app.project(), o);
    if (!list || list->empty()) {
        app.status("The selected track has no envelopes (V and P put them on).");
        return;
    }
    int at = -1;
    for (size_t k = 0; k < list->size(); ++k) {
        const Envelope& e = (*list)[k];
        if (frontEnv.owner == o && frontEnv.kind == e.kind && frontEnv.target == e.target && frontEnv.param == e.param) at = (int)k;
    }
    const int n = (int)list->size();
    const int next = at < 0 ? (dir > 0 ? 0 : n - 1) : ((at + dir) % n + n) % n;
    const Envelope& e = (*list)[(size_t)next];
    EnvelopeRef r;
    r.owner = o;
    r.kind = e.kind;
    r.target = e.target;
    r.param = e.param;
    frontEnv = r;
    if (!e.visible) app.showEnvelope(r, true);
    std::string name = envelopeName(e.kind).toStdString();
    if (e.kind == "fx") name += ": " + e.param;
    else if (e.kind == "send")
        if (const Bus* b = app.project().findBus(e.target)) name += " to " + b->name;
    app.status("Envelope: " + name);
    q->update();
}

void TimelineWidget::Impl::fillEnvelopeMenu(QMenu* menu, bool video)
{
    menu->clear();
    const Row* row = rowOf(app.currentTrack);
    if (!row || row->video != video) {
        menu->addAction(video ? "(select a video track)" : "(select an audio track)")->setEnabled(false);
        return;
    }
    EnvelopeOwner o;
    o.kind = EnvelopeOwner::Track;
    o.id = row->id;
    addEnvelopeChoices(menu, o);
}

DropKind TimelineWidget::Impl::dropKindOf(const QMimeData* md)
{
    if (!md) return DropKind::None;
    if (md->hasFormat(kMimeMedia)) return DropKind::Media;
    if (md->hasFormat(kMimeVideoFx)) return DropKind::VideoFx;
    if (md->hasFormat(kMimeAudioFx)) return DropKind::AudioFx;
    if (md->hasFormat(kMimeGenerator)) return DropKind::Generator;
    if (md->hasFormat(kMimeTransition)) return DropKind::Transition;
    if (md->hasUrls())
        for (const QUrl& u : md->urls())
            if (u.isLocalFile()) return DropKind::Files;
    return DropKind::None;
}

// Where AppState::addMediaToTimeline will put a piece of media (same logic, so the outline matches): the track
// under the mouse if suitable and free, else the first free track of the kind, else a new one.
std::vector<Ghost> TimelineWidget::Impl::planMedia(const MediaItem& m, double time, uint64_t hint) const
{
    std::vector<Ghost> out;
    const Project& p = app.project();
    const double start = std::max(0.0, p.quantize(time));
    double len = m.info.duration;
    if (m.generated) len = app.prefs.generatedLength;
    else if (m.info.isStill || len <= 0) len = app.prefs.stillLength;
    auto isFree = [&](const Track& t) {
        for (const Event& e : t.events)
            if (e.start < start + len - 1e-9 && e.end() > start + 1e-9) return false;
        return true;
    };
    auto place = [&](TrackType type) {
        Ghost g;
        g.video = type == TrackType::Video;
        g.start = start;
        g.length = len;
        for (const Track& t : p.tracks)
            if (t.id == hint && t.type == type && isFree(t)) g.track = t.id;
        if (!g.track)
            for (const Track& t : p.tracks)
                if (t.type == type && isFree(t)) { g.track = t.id; break; }
        out.push_back(g);
    };
    if (m.info.hasVideo) place(TrackType::Video);
    if (m.info.hasAudio) place(TrackType::Audio);
    return out;
}

double TimelineWidget::Impl::mediaLength(const MediaItem& m) const
{
    if (m.generated) return app.prefs.generatedLength;
    if (m.info.isStill || m.info.duration <= 0) return app.prefs.stillLength;
    return m.info.duration;
}

void TimelineWidget::Impl::planDrop(const QMimeData* md, const QPointF& pos, Qt::KeyboardModifiers mods)
{
    drop = DropState();
    snapLine = -1;
    drop.kind = dropKindOf(md);
    if (drop.kind == DropKind::None) return;
    const char* format = drop.kind == DropKind::Media ? kMimeMedia : drop.kind == DropKind::VideoFx ? kMimeVideoFx
                       : drop.kind == DropKind::AudioFx ? kMimeAudioFx : drop.kind == DropKind::Generator ? kMimeGenerator
                       : drop.kind == DropKind::Transition ? kMimeTransition : nullptr;
    if (format) drop.data = md->data(format).toStdString();
    const Hit h = hitTest(pos);
    drop.track = h.track;
    const Project& pr = app.project();
    const Track* tr = nullptr;
    for (const Track& t : pr.tracks) if (t.id == h.track) tr = &t;
    const bool inLanes = pos.x() >= headerW && pos.x() < lanesRight() && pos.y() >= kTopH && pos.y() < lanesBottom();
    switch (drop.kind) {
    case DropKind::Media:
    case DropKind::Files:
    case DropKind::Generator: {
        if (!inLanes) return;
        if (drop.kind == DropKind::Generator && !(tr && tr->type == TrackType::Video)) return;
        if ((mods & Qt::ControlModifier) && h.onEvent() && drop.kind != DropKind::Generator) {
            const Event* e = pr.findEvent(h.event);
            if (!e) return;
            drop.takes = true;
            drop.event = h.event;
            Ghost g;
            g.track = h.track;
            g.video = tr && tr->type == TrackType::Video;
            g.start = e->start;
            g.length = e->length;
            drop.ghosts.push_back(g);
            if (drop.kind == DropKind::Media) {
                const QByteArray list = md->hasFormat(kMimeMediaListFormat) ? md->data(kMimeMediaListFormat) : QByteArray(drop.data.c_str());
                for (const QString& s : QString::fromUtf8(list).split(QRegularExpression("[\\s,;]+"), Qt::SkipEmptyParts))
                    if (pr.findMedia(s.toULongLong())) drop.media.push_back(s.toULongLong());
            }
            drop.ok = drop.kind == DropKind::Files || !drop.media.empty();
            return;
        }
        double line = -1;
        drop.time = snapped(snapTargets(std::set<uint64_t>(), true), timeAtF(pos.x()), mods, &line);
        showSnap(line, drop.time);
        if (drop.kind == DropKind::Media) {
            // one media id, or several (placed one after another; in man2, all files selected in Project Media, the
            // clicked one first)
            double t = drop.time;
            const QByteArray list = md->hasFormat(kMimeMediaListFormat) ? md->data(kMimeMediaListFormat) : QByteArray(drop.data.c_str());
            for (const QString& s : QString::fromUtf8(list).split(QRegularExpression("[\\s,;]+"), Qt::SkipEmptyParts)) {
                const MediaItem* m = pr.findMedia(s.toULongLong());
                if (!m || std::find(drop.media.begin(), drop.media.end(), m->id) != drop.media.end()) continue;
                drop.media.push_back(m->id);
                for (const Ghost& g : planMedia(*m, t, drop.track)) drop.ghosts.push_back(g);
                t += mediaLength(*m) - autoOverlap(mediaLength(*m));
            }
            drop.ok = !drop.media.empty();
        } else if (drop.kind == DropKind::Generator) {
            MediaItem m;
            m.generated = true;
            m.info.hasVideo = true;
            drop.ghosts = planMedia(m, drop.time, drop.track);
            drop.ok = true;
        } else {
            drop.ok = true;
        }
        break;
    }
    case DropKind::VideoFx:
        if (tr && tr->type == TrackType::Video) {
            if (h.onEvent()) {
                drop.event = h.event;
                drop.ok = true;
            } else if (h.onHeader()) {
                drop.onHeader = true;
                drop.ok = true;
            }
        }
        break;
    case DropKind::AudioFx:
        if (tr && tr->type == TrackType::Audio && (h.onHeader() || inLanes)) {
            drop.onHeader = h.onHeader();
            drop.ok = true;
        }
        break;
    case DropKind::Transition:
        if (tr && tr->type == TrackType::Video && inLanes) {
            const double t = timeAtF(pos.x());
            for (const Event& e : tr->events)
                if (const Event* a = fadesFrom(*tr, e))
                    if (t >= e.start && t <= a->end()) drop.event = e.id;
            if (!drop.event && h.onEvent())
                for (const Event& e : tr->events)
                    if (e.id == h.event && fadesFrom(*tr, e)) drop.event = e.id;
            drop.ok = drop.event != 0;
        }
        break;
    default: break;
    }
}

// Files added one after another (each one's events in 'each'). With Automatically overlap multiple selected media on,
// each is moved back to overlap the previous one by the cut-to-overlap amount (added end to end first so they land on
// the same tracks).
void TimelineWidget::Impl::overlapAdded(const std::vector<std::vector<uint64_t>>& each)
{
    if (each.size() < 2 || !app.prefs.autoOverlapMedia || !app.autoCrossfades()) return;
    std::map<uint64_t, double> shift;
    double total = 0;
    for (size_t k = 1; k < each.size(); ++k) {
        double len = 1e300;
        for (uint64_t e : each[k - 1])
            if (const Event* ev = app.project().findEvent(e)) len = std::min(len, ev->length);
        if (len >= 1e299) continue;
        total += autoOverlap(len);
        for (uint64_t e : each[k]) shift[e] = total;
    }
    if (shift.empty()) return;
    app.edit("Overlap", [&](Project& p) {
        for (const auto& kv : shift)
            if (Event* e = p.findEvent(kv.first)) e->start = std::max(0.0, p.quantize(e->start - kv.second));
    });
}

std::vector<uint64_t> TimelineWidget::Impl::dropMediaIds(const DropState& dr, const std::vector<QUrl>& urls)
{
    std::vector<uint64_t> media = dr.media;
    for (const QUrl& u : urls) {
        if (!u.isLocalFile()) continue;
        std::string err;
        if (const uint64_t m = app.importMedia(u.toLocalFile().toStdString(), &err)) media.push_back(m);
        else app.status("Could not open " + u.toLocalFile().toStdString() + (err.empty() ? std::string() : ": " + err));
    }
    return media;
}

// VEGAS's ways of adding several files (right-drag menu): one after another on a track (mode 0, Add Across Time),
// side by side
// on separate tracks (1, Add Across Tracks), or as takes of one event (2, Add As Takes); video and audio, video only
// (streams
// 1) or audio only (2). Dropped on an event, Add As Takes adds them to it (the first becomes the active take). One
// undo step;
// returns the events made (or given takes).
std::set<uint64_t> TimelineWidget::Impl::addMediaAs(const std::vector<uint64_t>& media, double time, uint64_t track, int mode, int streams, uint64_t ontoEvent)
{
    std::set<uint64_t> made;
    if (media.empty()) return made;
    const size_t steps = app.undoCount();
    auto addOne = [&](uint64_t m, double t, uint64_t hint) {
        if (streams) {
            AppState::MediaRange r;
            r.mediaId = m;
            r.streams = streams;
            app.nextAddRange = r;
        }
        return app.addMediaToTimeline(m, t, hint);
    };
    const char* what = mode == 0 ? "Add Across Time" : mode == 1 ? "Add Across Tracks" : "Add As Takes";
    if (mode == 2) {
        std::vector<uint64_t> into;
        std::vector<uint64_t> rest = media;
        if (ontoEvent && app.project().findEvent(ontoEvent)) {
            into.push_back(ontoEvent);
            for (uint64_t g : withGroups(std::set<uint64_t>{ontoEvent}))
                if (g != ontoEvent) into.push_back(g);
        } else {
            for (uint64_t e : addOne(media.front(), time, track)) into.push_back(e);
            rest.erase(rest.begin());
        }
        for (uint64_t e : into) {
            const Event* ev = app.project().findEvent(e);
            if (!ev) continue;
            const size_t before = AppState::takesOf(*ev).size();
            const Track* tr = nullptr;
            app.project().findEvent(e, &tr);
            if (streams && tr && ((streams == 1) != (tr->type == TrackType::Video))) continue;
            if (app.addTakes(e, rest) > 0 && ontoEvent) app.chooseTake(std::set<uint64_t>{e}, (int)before, "Choose Take");
            made.insert(e);
        }
    } else {
        double t = time;
        std::vector<std::vector<uint64_t>> each;
        for (uint64_t m : media) {
            const MediaItem* mi = app.project().findMedia(m);
            if (!mi) continue;
            const double len = mediaLength(*mi);
            each.push_back(addOne(m, t, track));
            for (uint64_t e : each.back()) made.insert(e);
            if (mode == 0) t += len;
        }
        if (mode == 0) overlapAdded(each);
    }
    const size_t now = app.undoCount();
    if (now > steps + 1) app.mergeUndo(now - steps, what);
    else if (now == steps + 1) app.mergeUndo(1, what);
    if (!made.empty()) app.setSelection(made);
    return made;
}

void TimelineWidget::Impl::rightDropMenu(const DropState& dr, const std::vector<QUrl>& urls, const QPointF& pos, uint64_t ontoEvent)
{
    QMenu m(q);
    struct Pick { int mode, streams; };
    std::map<QAction*, Pick> picks;
    auto modes = [&](QMenu* into, int streams) {
        picks[into->addAction("Add Across &Time")] = Pick{0, streams};
        picks[into->addAction("Add Across T&racks")] = Pick{1, streams};
        picks[into->addAction("Add As T&akes")] = Pick{2, streams};
    };
    modes(&m, 0);
    m.addSeparator();
    modes(m.addMenu("&Video Only"), 1);
    modes(m.addMenu("A&udio Only"), 2);
    m.addSeparator();
    m.addAction("Cance&l");
    QAction* got = q->rightDragMenu ? q->rightDragMenu(&m) : m.exec(q->mapToGlobal(pos.toPoint()));
    rightMenuAt = QDateTime::currentMSecsSinceEpoch();
    auto it = picks.find(got);
    if (it == picks.end()) return;
    const std::vector<uint64_t> media = dropMediaIds(dr, urls);
    addMediaAs(media, dr.time, dr.track, it->second.mode, it->second.streams, it->second.mode == 2 ? ontoEvent : 0);
}

void TimelineWidget::Impl::dragEnter(QDragEnterEvent* ev)
{
    dropRight = (ev->buttons() & Qt::RightButton) != 0;
    planDrop(ev->mimeData(), ev->position(), ev->modifiers());
    if (drop.kind == DropKind::None) { ev->ignore(); return; }
    if (ev->possibleActions() & Qt::CopyAction) ev->setDropAction(Qt::CopyAction);
    ev->accept();
    q->update();
}

void TimelineWidget::Impl::dragMove(QDragMoveEvent* ev)
{
    if (ev->buttons() != Qt::NoButton) dropRight = (ev->buttons() & Qt::RightButton) != 0;
    planDrop(ev->mimeData(), ev->position(), ev->modifiers());
    if (drop.ok) {
        if (ev->possibleActions() & Qt::CopyAction) ev->setDropAction(Qt::CopyAction);
        ev->accept();
    } else {
        ev->ignore();
    }
    q->update();
}

void TimelineWidget::Impl::dragLeave()
{
    drop = DropState();
    snapLine = -1;
    q->update();
}

void TimelineWidget::Impl::dropped(QDropEvent* ev)
{
    planDrop(ev->mimeData(), ev->position(), ev->modifiers());
    const DropState dr = drop;
    std::vector<QUrl> urls;
    if (dr.kind == DropKind::Files)
        for (const QUrl& u : ev->mimeData()->urls()) urls.push_back(u);
    drop = DropState();
    snapLine = -1;
    q->update();
    if (!dr.ok) { ev->ignore(); return; }
    if (ev->possibleActions() & Qt::CopyAction) ev->setDropAction(Qt::CopyAction);
    ev->accept();
    std::string id, label;
    splitPair(dr.data, id, label);
    std::set<uint64_t> made;
    if (dropRight && (dr.kind == DropKind::Media || dr.kind == DropKind::Files)) {
        dropRight = false;
        const Hit h = hitTest(ev->position());
        rightDropMenu(dr, urls, ev->position(), h.onEvent() ? h.event : 0);
        q->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (dr.takes) {
        const std::vector<uint64_t> media = dropMediaIds(dr, urls);
        int added = 0;
        for (uint64_t e : withGroups(std::set<uint64_t>{dr.event})) added += app.addTakes(e, media);
        app.status(added ? strfmt("%d take%s added (T and Shift+T switch between them).", added, added == 1 ? "" : "s")
                         : std::string("That media has nothing the event can show (pictures for a video event, sound for an audio one)."));
        q->setFocus(Qt::OtherFocusReason);
        return;
    }
    switch (dr.kind) {
    case DropKind::Media: {
        double t = dr.time;
        std::vector<std::vector<uint64_t>> each;
        const size_t steps = app.undoCount();
        for (uint64_t m : dr.media) {
            const MediaItem* mi = app.project().findMedia(m);
            if (!mi) continue;
            const double len = mediaLength(*mi);
            each.push_back(app.addMediaToTimeline(m, t, dr.track));
            for (uint64_t e : each.back()) made.insert(e);
            t += len;
        }
        overlapAdded(each);
        if (app.undoCount() > steps + 1) app.mergeUndo(app.undoCount() - steps, "Add to Timeline");
        break;
    }
    case DropKind::Files: {
        double t = dr.time;
        std::vector<std::vector<uint64_t>> eachFile;
        for (const QUrl& u : urls) {
            if (!u.isLocalFile()) continue;
            const std::string path = u.toLocalFile().toStdString();
            std::string err;
            const uint64_t m = app.importMedia(path, &err);
            const MediaItem* mi = m ? app.project().findMedia(m) : nullptr;
            if (!mi) {
                app.status("Could not open " + path + (err.empty() ? std::string() : ": " + err));
                continue;
            }
            const double len = mediaLength(*mi);
            eachFile.push_back(app.addMediaToTimeline(m, t, dr.track));
            for (uint64_t e : eachFile.back()) made.insert(e);
            t += len;
        }
        overlapAdded(eachFile);
        break;
    }
    case DropKind::Generator: {
        const uint64_t m = isFxPackageData(dr.data) ? makeGeneratedMediaFromData(dr.data) : app.addGeneratedMedia(id, label);
        if (m)
            for (uint64_t e : app.addMediaToTimeline(m, dr.time, dr.track)) made.insert(e);
        break;
    }
    case DropKind::VideoFx:
    case DropKind::AudioFx: {
        FxTarget t;
        t.kind = dr.event ? FxTarget::Event : FxTarget::Track;
        t.id = dr.event ? dr.event : dr.track;
        uint64_t slot = 0;
        if (isFxPackageData(dr.data)) {
            slot = addFxData(t, dr.data);
        } else if (dr.kind == DropKind::VideoFx) {
            slot = app.addVideoFx(t, id, label);
        } else {
            Json j;
            if (!Json::parse(dr.data, j)) { app.status("That audio effect could not be read."); return; }
            slot = app.addAudioFx(t, AudioPluginDesc::fromJson(j));
        }
        if (slot && q->onOpenFx) q->onOpenFx(t, slot);
        break;
    }
    case DropKind::Transition:
        if (isFxPackageData(dr.data)) {
            FxTarget t;
            t.kind = FxTarget::Transition;
            t.id = dr.event;
            addFxData(t, dr.data);
        } else {
            app.setTransition(dr.event, id, label);
        }
        break;
    default: break;
    }
    if (!made.empty()) app.setSelection(made);
    q->setFocus(Qt::OtherFocusReason);
}

}  // namespace app
}  // namespace reno
