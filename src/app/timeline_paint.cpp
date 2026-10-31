// Painting the timeline: what a repaint covers, the ruler, tracks, events and envelopes.
#include "timeline_internal.h"

namespace reno {
namespace app {

// An event's place, with room for what is drawn just outside it (VEGAS's fade handles, outlines); empty if it isn't on a
// track in view.
QRect TimelineWidget::Impl::eventArea(const Row& row, const Event& e) const
{
    const QRectF r = eventRect(row, e).adjusted(-8, -6, 8, 2);
    const QRectF clip = QRectF(lanesRect()).intersected(r);
    return clip.isEmpty() ? QRect() : clip.toAlignedRect();
}

QRect TimelineWidget::Impl::eventArea(uint64_t id) const
{
    // found through a map of where each event is, built once for the project as it is (a selection change can touch
    // hundreds of events, and a project has thousands)
    ensureIndex();
    const Project& p = app.project();
    if (eventPlaces.empty())
        for (size_t t = 0; t < p.tracks.size(); ++t)
            for (size_t k = 0; k < p.tracks[t].events.size(); ++k) eventPlaces.emplace(p.tracks[t].events[k].id, std::make_pair(t, k));
    auto it = eventPlaces.find(id);
    if (it == eventPlaces.end() || it->second.first >= p.tracks.size() || it->second.second >= p.tracks[it->second.first].events.size() ||
        p.tracks[it->second.first].events[it->second.second].id != id)
        return QRect();
    const Track& tr = p.tracks[it->second.first];
    const Row* row = rowOf(tr.id);
    return row ? eventArea(*row, tr.events[it->second.second]) : QRect();
}

bool TimelineWidget::Impl::sameEnvelopes(const Envelopes& a, const Envelopes& b)
{
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        const Envelope& x = a[k];
        const Envelope& y = b[k];
        if (x.kind != y.kind || x.target != y.target || x.param != y.param || x.visible != y.visible || x.locked != y.locked || x.points.size() != y.points.size())
            return false;
        for (size_t j = 0; j < x.points.size(); ++j)
            if (x.points[j].time != y.points[j].time || x.points[j].value != y.points[j].value || x.points[j].curve != y.points[j].curve) return false;
    }
    return true;
}

bool TimelineWidget::Impl::sameChain(const FxChain& a, const FxChain& b)
{
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k)
        if (a[k].id != b[k].id || a[k].name != b[k].name || a[k].bypass != b[k].bypass || a[k].pluginId != b[k].pluginId) return false;
    return true;
}

bool TimelineWidget::Impl::sameLook(const Event& a, const Event& b)
{
    if (a.id != b.id || a.mediaId != b.mediaId || a.start != b.start || a.length != b.length || a.offset != b.offset || a.rate != b.rate ||
        a.fadeIn != b.fadeIn || a.fadeOut != b.fadeOut || a.gain != b.gain || a.normalize != b.normalize || a.normalizeGain != b.normalizeGain || a.mute != b.mute || a.loop != b.loop || a.group != b.group ||
        a.reverse != b.reverse || a.name != b.name || a.locked != b.locked || a.fadeInCurve != b.fadeInCurve || a.fadeOutCurve != b.fadeOutCurve ||
        a.fadeInRecipCurve != b.fadeInRecipCurve || a.activeTake != b.activeTake || a.takes.size() != b.takes.size() ||
        a.panCrop.empty() != b.panCrop.empty() || a.mediaChannel != b.mediaChannel || a.mediaChannels != b.mediaChannels ||
        a.transition.id != b.transition.id || a.transition.kind != b.transition.kind || a.transition.name != b.transition.name)
        return false;
    for (size_t k = 0; k < a.takes.size(); ++k)
        if (a.takes[k].mediaId != b.takes[k].mediaId || a.takes[k].offset != b.takes[k].offset || a.takes[k].name != b.takes[k].name ||
            a.takes[k].reverse != b.takes[k].reverse)
            return false;
    return sameChain(a.fx, b.fx) && sameEnvelopes(a.envelopes, b.envelopes);
}

bool TimelineWidget::Impl::sameHeader(const Track& a, const Track& b)
{
    return a.id == b.id && a.type == b.type && a.name == b.name && a.level == b.level && a.pan == b.pan && a.panType == b.panType &&
           sameChain(a.fx, b.fx) && a.motion.empty() == b.motion.empty() && a.composite == b.composite && a.compositeLevel == b.compositeLevel &&
           a.parentComposite == b.parentComposite && a.parentMotion.empty() == b.parentMotion.empty() && a.adjustment == b.adjustment &&
           a.surround.x == b.surround.x && a.surround.y == b.surround.y && a.surround.route == b.surround.route &&
           a.surround.keys.size() == b.surround.keys.size() && a.output == b.output && a.sends.size() == b.sends.size() &&
           a.shadow2d.empty() == b.shadow2d.empty() && a.glow2d.empty() == b.glow2d.empty() && a.fadeTop == b.fadeTop && a.fadeBottom == b.fadeBottom;
}

// While a mouse drag changes the project step by step: what has to be repainted for the change from the project as last
// noted (false: everything, because something changed that can't be put in a few places).
bool TimelineWidget::Impl::projectChangeRegion(QRegion& out)
{
    std::shared_ptr<const Project> before = notedProject;
    notedProject = app.snapshot();
    std::vector<Row> rowsBefore;
    rowsBefore.swap(notedRows);
    notedRows = rows;
    if (drag.kind == Drag::None && !releasing) return false;
    if (!before) return false;
    const Project& a = *before;
    const Project& b = app.project();
    if (a.tracks.size() != b.tracks.size() || rowsBefore.size() != rows.size()) return false;
    for (size_t k = 0; k < rows.size(); ++k)
        if (rowsBefore[k].id != rows[k].id || rowsBefore[k].y != rows[k].y || rowsBefore[k].h != rows[k].h) return false;
    if (a.settings.fpsNum != b.settings.fpsNum || a.settings.fpsDen != b.settings.fpsDen || a.settings.rulerFormat != b.settings.rulerFormat ||
        a.settings.rulerStart != b.settings.rulerStart || a.settings.tempo != b.settings.tempo || a.settings.beatsPerMeasure != b.settings.beatsPerMeasure ||
        a.settings.audioChannels != b.settings.audioChannels || a.autoCrossfades != b.autoCrossfades || a.media.size() != b.media.size())
        return false;
    for (size_t k = 0; k < a.media.size(); ++k) {
        const MediaItem& x = a.media[k];
        const MediaItem& y = b.media[k];
        if (x.id != y.id || x.path != y.path || x.name != y.name || x.offline != y.offline || x.generated != y.generated ||
            x.info.duration != y.info.duration || x.info.markers.size() != y.info.markers.size() || x.markers.size() != y.markers.size())
            return false;
    }
    if (opt.showBusTracks) {
        if (a.buses.size() != b.buses.size() || a.masterVolume != b.masterVolume) return false;
        for (const BusRow& br : busRows) {
            const Envelopes* x = AppState::envelopesOf(a, br.owner);
            const Envelopes* y = AppState::envelopesOf(b, br.owner);
            if ((x == nullptr) != (y == nullptr) || (x && !sameEnvelopes(*x, *y))) return false;
        }
        for (size_t k = 0; k < a.buses.size(); ++k)
            if (a.buses[k].id != b.buses[k].id || a.buses[k].name != b.buses[k].name || a.buses[k].level != b.buses[k].level) return false;
    }
    if (a.markers.size() != b.markers.size()) return false;
    int changes = 0;
    for (size_t k = 0; k < a.markers.size(); ++k) {
        const Project::Marker& x = a.markers[k];
        const Project::Marker& y = b.markers[k];
        if (x.id == y.id && x.time == y.time && x.length == y.length && x.name == y.name && x.region == y.region && x.kind == y.kind) continue;
        if (++changes > 16) return false;
        out |= QRect(headerW, 0, viewW(), kTopH);
        for (const Project::Marker* m : {&x, &y}) {
            out |= timeStrip(m->time);
            if (m->region) out |= timeStrip(m->time + m->length);
        }
    }
    changes = 0;
    for (size_t t = 0; t < a.tracks.size(); ++t) {
        const Track& x = a.tracks[t];
        const Track& y = b.tracks[t];
        if (x.id != y.id || x.type != y.type || x.mute != y.mute || x.solo != y.solo || x.color != y.color || x.trackGroup != y.trackGroup ||
            x.height != y.height)
            return false;
        const Row& row = rows[t];
        if (!sameHeader(x, y)) out |= QRect(0, rowTop(row), headerW, row.h);
        if (!sameEnvelopes(x.envelopes, y.envelopes)) out |= laneArea(row);
        const std::vector<Event>& ex = x.events;
        const std::vector<Event>& ey = y.events;
        bool aligned = ex.size() == ey.size();
        for (size_t k = 0; aligned && k < ex.size(); ++k) aligned = ex[k].id == ey[k].id;
        if (aligned) {
            for (size_t k = 0; k < ex.size(); ++k)
                if (!sameLook(ex[k], ey[k])) {
                    if (++changes > 300) return false;
                    out |= eventArea(row, ex[k]);
                    out |= eventArea(row, ey[k]);
                }
            continue;
        }
        std::unordered_map<uint64_t, const Event*> old;
        for (const Event& e : ex) old.emplace(e.id, &e);
        for (const Event& e : ey) {
            auto it = old.find(e.id);
            if (it != old.end() && sameLook(*it->second, e)) {
                old.erase(it);
                continue;
            }
            if (++changes > 300) return false;
            out |= eventArea(row, e);
            if (it != old.end()) {
                out |= eventArea(row, *it->second);
                old.erase(it);
            }
        }
        for (const auto& kv : old) {
            if (++changes > 300) return false;
            out |= eventArea(row, *kv.second);
        }
    }
    const bool wasDense = dense;
    if (countDense() != wasDense) return false;
    return true;
}

bool TimelineWidget::Impl::countDense() const
{
    const Project& pr = app.project();
    const QRect all = lanesRect();
    size_t inView = 0;
    const double from = timeAtF(all.left() - 20.0), to = timeAtF(all.right() + 20.0);
    for (const Row& r : rows) {
        const int y = rowTop(r);
        if (y + r.h <= all.top() || y > all.bottom()) continue;
        const std::vector<Event>& ev = pr.tracks[(size_t)r.index].events;
        const std::pair<size_t, size_t> range = eventRange((size_t)r.index, from, to);
        for (size_t k = range.first; k < range.second && inView <= 200; ++k)
            if (ev[k].end() >= from && ev[k].start <= to) ++inView;
        if (inView > 200) break;
    }
    return inView > 200;
}

// The selection (and the current and selected tracks, the armed ones) changed: the events whose selection changed, the
// header column and the lanes of the tracks that became or stopped being current (false: everything).
bool TimelineWidget::Impl::selectionChangeRegion(QRegion& out)
{
    const std::set<uint64_t>& now = app.selection();
    std::vector<uint64_t> diff;
    std::set_symmetric_difference(notedSelection.begin(), notedSelection.end(), now.begin(), now.end(), std::back_inserter(diff));
    const bool tracksChanged = notedCurrent != app.currentTrack || notedArmed != app.armedTracks;
    const uint64_t oldCurrent = notedCurrent;
    notedSelection = now;
    notedCurrent = app.currentTrack;
    notedArmed = app.armedTracks;
    if (diff.size() > 400) return false;
    for (uint64_t id : diff) out |= eventArea(id);
    if (tracksChanged) {
        out |= headerColumn();
        for (uint64_t id : {oldCurrent, app.currentTrack})
            if (const Row* r = id ? rowOf(id) : nullptr) out |= laneArea(*r);
    }
    return true;
}

uint64_t TimelineWidget::Impl::fxErrorSignature() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    auto chain = [&](const FxChain& c) {
        for (const FxSlot& sl : c)
            if (!app.fx.error(sl.id).empty()) mix(sl.id);
    };
    for (const Track& t : app.project().tracks) {
        chain(t.fx);
        for (const Event& e : t.events) {
            chain(e.fx);
            if (e.transition.id && !app.fx.error(e.transition.id).empty()) mix(e.transition.id);
        }
    }
    return h;
}

QRectF TimelineWidget::Impl::envLabelBox() const
{
    const double w = smallFm.horizontalAdvance(envLabel) + 10.0, h = smallFm.height() + 4.0;
    QRectF box(envLabelAt.x() + 10.0, envLabelAt.y() - h - 6.0, w, h);
    if (box.right() > lanesRight() - 2) box.moveRight(envLabelAt.x() - 10.0);
    if (box.top() < kTopH + 2) box.moveTop(envLabelAt.y() + 8.0);
    return box;
}

TimelineWidget::Impl::Overlays TimelineWidget::Impl::currentOverlays() const
{
    Overlays o;
    o.snap = snapLine;
    if (!envLabel.isEmpty()) o.label = envLabelBox().adjusted(-2, -2, 2, 2).toAlignedRect();
    if ((drag.kind == Drag::RubberBand || drag.kind == Drag::EnvBand) && drag.started)
        o.band = QRectF(drag.pressPos, drag.lastPos).normalized().adjusted(-2, -2, 2, 2).toAlignedRect();
    else if (drag.kind == Drag::ZoomRange && drag.started)
        o.band = QRectF(drag.pressPos, drag.lastPos).normalized().adjusted(-2, -2, 2, 2).united(QRectF(headerW, kTopH, std::max(0, lanesRight() - headerW), std::max(0, lanesBottom() - kTopH))).toAlignedRect();
    if (drag.kind == Drag::Level || drag.kind == Drag::Pan || drag.kind == Drag::Fade || drag.kind == Drag::SurroundPoint) o.sliderTrack = drag.hit.track;
    o.gapHeld = drag.kind == Drag::HeaderWidth;
    o.dragEnv = drag.env;
    o.front = frontEnv;
    o.selEnv = selEnv;
    o.selPoints = selPoints;
    o.edgeEvent = edgeSel.event;
    o.edgeOther = edgeSel.other;
    o.edgeSide = edgeSel.side;
    o.edgeWhich = edgeSel.which;
    o.edgeExpanded = edgeSel.expanded;
    o.listFocus = trackListFocus && q->hasFocus();
    o.selTracks = selTracks;
    return o;
}

void TimelineWidget::Impl::overlaysChanged()
{
    const Overlays now = currentOverlays();
    const Overlays& was = notedOverlays;
    QRegion r;
    const QRect lanes = lanesRect();
    if (was.snap != now.snap)
        for (double t : {was.snap, now.snap})
            if (t >= 0) r |= QRect((int)std::floor(xAtF(clampd(t, -1e7, 1e7))) - 1, lanes.top(), 3, lanes.height()).intersected(lanes);
    if (was.label != now.label) r |= QRegion(was.label) | now.label;
    if (was.band != now.band) r |= QRegion(was.band) | now.band;
    if (was.sliderTrack != now.sliderTrack)
        for (uint64_t id : {was.sliderTrack, now.sliderTrack})
            if (const Row* row = id ? rowOf(id) : nullptr) r |= QRect(0, rowTop(*row), headerW, row->h);
    if (was.gapHeld != now.gapHeld) r |= QRect(headerW - kHeaderGap, kTopH, kHeaderGap, std::max(0, lanesBottom() - kTopH));
    auto lane = [&](const EnvelopeRef& ref) {
        if (!ref.valid()) return;
        EnvLane L;
        if (envLaneOf(ref, &L)) r |= L.clip.adjusted(-8, -8, 8, 8).toAlignedRect().intersected(lanes);
    };
    if (was.dragEnv != now.dragEnv || was.front != now.front) {
        lane(was.dragEnv);
        lane(now.dragEnv);
        lane(was.front);
        lane(now.front);
    }
    if (was.selEnv != now.selEnv || was.selPoints != now.selPoints) {
        lane(was.selEnv);
        lane(now.selEnv);
    }
    if (was.edgeEvent != now.edgeEvent || was.edgeOther != now.edgeOther || was.edgeSide != now.edgeSide || was.edgeWhich != now.edgeWhich ||
        was.edgeExpanded != now.edgeExpanded)
        for (uint64_t id : {was.edgeEvent, was.edgeOther, now.edgeEvent, now.edgeOther})
            if (id) r |= eventArea(id);
    if (was.listFocus != now.listFocus || was.selTracks != now.selTracks) r |= headerColumn();
    notedOverlays = now;
    if (!r.isEmpty()) q->update(r);
}

bool TimelineWidget::Impl::mediaReread()
{
    bool again = false;
    std::map<uint64_t, std::pair<bool, double>> now;
    for (const MediaItem& m : app.project().media) {
        const std::pair<bool, double> v(m.offline, m.info.duration);
        now[m.id] = v;
        auto it = mediaSeen.find(m.id);
        if (it != mediaSeen.end() && ((it->second.first && !m.offline) || it->second.second != v.second)) again = true;
    }
    mediaSeen.swap(now);
    return again;
}

void TimelineWidget::Impl::changed(int changes)
{
    if (app.loopEnd > app.loopStart) {
        lastLoopA = app.loopStart;
        lastLoopB = app.loopEnd;
    }
    if ((changes & kProject) && edgeSel.event && !edgeValid()) edgeSel = EdgeSel();
    if (changes & (kProject | kFile)) indexValid = false;
    if (changes & (kProject | kSelection | kFxParams | kCatalog | kPrefs | kFile)) ++headerGeneration;
    if (changes & kProject) {
        layout();
        validateEnvSelection();
        if (naming != Naming::None && !namedStillThere()) endRename(false);
        updateBars();
    }
    // What has to be repainted: during a drag only what the drag changed; for a selection change only the events (and
    // tracks) whose selection changed; for a change of an effect's settings only if an effect's trouble shown on the
    // timeline changed. (Starting and stopping playback changes nothing that is drawn.)
    QRegion part;
    bool cursorOnly = false;
    bool all = (changes & kCatalog) != 0;
    if ((changes & kProject) && !projectChangeRegion(part)) all = true;
    if ((changes & kSelection) && !selectionChangeRegion(part)) all = true;
    if (changes & (kFxParams | kProject)) {
        const uint64_t sig = fxErrorSignature();
        if ((changes & kFxParams) && sig != notedFxErrors) all = true;
        notedFxErrors = sig;
    }
    if ((changes & kFile) && app.path() != loadedPath) {
        loadedPath = app.path();
        if (loader) loader->forgetFiles();
        minimized.clear();
        for (const Track& t : app.project().tracks)
            if (t.minimized) minimized.insert(t.id);
        layout();
        all = true;
    }
    if ((changes & kProject) && mediaReread() && loader) {
        // Project Media's Refresh found a file that couldn't be read before, or one that has changed: its thumbnails and
        // waveform are rebuilt (the reader that failed, or read the old file, is not used again)
        loader.reset(new Loader(app.mediaOpener, [this] { loaderReady(); }));
        thumbs.clear();
        all = true;
    }
    if (changes & kMediaPictures) {
        for (auto& t : thumbs)
            if (projectMediaKeys.count(t.first.media)) t.second.stale = true;
        for (auto& ns : nestedStamps) {
            const NestedStamp now = nestedStampOf(ns.second.path);
            if (now.size != ns.second.size || now.time != ns.second.time) {
                if (loader) loader->forgetMedia(ns.first, ns.second.path);
                ns.second.size = now.size;
                ns.second.time = now.time;
            }
        }
        all = true;
    }
    if (changes & kPrefs) {
        {
            const int pt = std::max(0, std::min(32, vprefs::number("prefs/timelineFontPt")));
            QFont want = pt > 0 ? QFont(QApplication::font()) : QFont();
            if (pt > 0) want.setPointSizeF(pt);
            if (pt != timelineFontSet) {
                timelineFontSet = pt;
                q->setFont(want);
                all = true;
            }
        }
        if (opt.snapToGrid != app.prefs.snapToGrid) {
            opt.snapToGrid = app.prefs.snapToGrid;
            if (q->onOptionsChanged) q->onOptionsChanged();
        }
    }
    if ((changes & kCursor) && headersFollowCursor()) part |= QRect(0, kTopH, headerW, std::max(0, lanesBottom() - kTopH));
    if (changes & kCursor) {
        // keep the cursor in view while playing, as Preferences say: turn the page when it runs off the right (VEGAS's
        // default), or keep it in the middle once it gets there, or leave the view alone
        if (playback && playback->playing() && drag.kind == Drag::None && app.prefs.scrollMode != "none") {
            const double x = xAtF(app.cursor());
            const double viewSeconds = viewW() / pps;
            double to = 0;
            bool scroll = false;
            if (app.prefs.scrollMode == "smooth") {
                scroll = x > headerW + viewW() * 0.5 + 0.5 || x < headerW;
                to = app.cursor() - viewSeconds * 0.5;
            } else {
                scroll = x > lanesRight() - 1 || x < headerW;
                to = app.cursor() - viewSeconds * 0.02;
            }
            if (scroll) {
                scrollViewTo(std::max(0.0, to));
            }
        }
        if (app.loopStart != paintedLoopA || app.loopEnd != paintedLoopB || app.loopOn != paintedLoopOn) {
            const bool had = paintedLoopB > paintedLoopA, has = app.loopEnd > app.loopStart;
            part |= QRect(headerW, 0, viewW(), kTopH);
            auto span = [&](double a, double b) {
                const double x0 = clampd(xAtF(std::min(a, b)), headerW - 10.0, lanesRight() + 10.0);
                const double x1 = clampd(xAtF(std::max(a, b)), headerW - 10.0, lanesRight() + 10.0);
                part |= QRect((int)std::floor(x0) - 3, kTopH, (int)std::ceil(x1 - x0) + 7, std::max(0, lanesBottom() - kTopH)).intersected(lanesRect());
            };
            if (had && has) {
                span(paintedLoopA, app.loopStart);
                span(paintedLoopB, app.loopEnd);
            } else {
                if (had) span(paintedLoopA, paintedLoopB);
                if (has) span(app.loopStart, app.loopEnd);
            }
            if (app.loopOn != paintedLoopOn)
                for (double t : {paintedLoopA, paintedLoopB, app.loopStart, app.loopEnd}) span(t, t);
        }
        if (!all) {
            const int h = q->height();
            cursorOnly = part.isEmpty();
            part |= QRect((int)std::floor(xAtF(paintedCursor)) - 8, 0, 17, h);
            part |= QRect((int)std::floor(xAtF(app.cursor())) - 8, 0, 17, h);
            part |= QRect(0, 0, headerW, kTopH);
        }
    }
    if (all) q->update();
    else if (!part.isEmpty()) {
        if (cursorOnly) q->updateCursor(part);
        else q->update(part);
    }
    overlaysChanged();
}

void TimelineWidget::Impl::loaderReady()
{
    std::set<int> keys = loader->takeDropped();
    for (const std::string& path : loader->takeProbed()) {
        auto k = mediaKeys.find(path);
        if (k != mediaKeys.end()) keys.insert(k->second);
    }
    // what is repainted for each file whose thumbnails or waveforms came in: its whole events for a file just read or
    // dropped (wholeKeys); else only the stretch of the file that came in (spans, media seconds)
    const std::set<int> wholeKeys = keys;
    std::map<int, std::pair<double, double>> spans;
    auto addSpan = [&spans](int key, double a, double b) {
        auto& sp = spans.emplace(key, std::make_pair(a, b)).first->second;
        sp.first = std::min(sp.first, a);
        sp.second = std::max(sp.second, b);
    };
    for (Loader::Done& item : loader->take()) {
        keys.insert(item.key.media);
        {
            const double w = item.image.width() / std::max(1.0, q->devicePixelRatioF());
            const double pad = 1.5 * std::max(8.0, w) / std::max(1e-9, pps);
            addSpan(item.key.media, item.key.ms / 1000.0 - pad, item.key.ms / 1000.0 + pad);
        }
        Thumb t;
        t.image = std::move(item.image);
        t.used = paintCount;
        thumbs[item.key] = std::move(t);
    }
    // waveforms being read: only the stretch read since the last look is repainted (by the media time it covers), not
    // the whole event; a long file's waveform comes in a step at a time for minutes, and repainting the whole event
    // at every step held up the interface
    for (auto it = shownPeaks.begin(); it != shownPeaks.end();) {
        const std::shared_ptr<Peaks> pk = it->second.peaks.lock();
        if (!pk) {
            it = shownPeaks.erase(it);
            continue;
        }
        const int64_t now = pk->complete.load() ? -1 : pk->filled.load();
        if (now != it->second.seen) {
            const double a = it->second.seen >= 0 ? it->second.seen / kPeakPerSecond - 0.05 : -1e300;
            const double b = now >= 0 ? now / kPeakPerSecond + 0.05 : 1e300;
            addSpan(std::get<0>(it->first), a, b);
            it->second.seen = now;
            keys.insert(std::get<0>(it->first));
        }
        ++it;
    }
    // not without limit: when there are too many, the ones not drawn for longest go, but not those of the files shown
    // in view at the height they are shown at (what is on screen may not have been painted for a while, and a whole
    // repaint would then have to wait for them again)
    if (thumbs.size() > 900) {
        std::set<std::pair<int, int>> shown;
        {
            const Project& pr = app.project();
            const QRect lanes = lanesRect();
            const double from = timeAtF(lanes.left() - 20.0), to = timeAtF(lanes.right() + 20.0);
            const double dpr = q->devicePixelRatioF();
            for (const Row& row : rows) {
                const int y = rowTop(row);
                if (!row.video || y + row.h <= lanes.top() || y > lanes.bottom()) continue;
                const std::vector<Event>& ev = pr.tracks[(size_t)row.index].events;
                const std::pair<size_t, size_t> range = eventRange((size_t)row.index, from, to);
                for (size_t k = range.first; k < range.second && shown.size() < 400; ++k) {
                    const MediaItem* m = mediaOf(ev[k].mediaId);
                    if (!m || m->generated) continue;
                    const QRectF r = eventRect(row, ev[k]);
                    const double tileH = r.height() - eventTitleH(r);
                    shown.insert({mediaKeyFast(*m), std::max(8, (int)std::lround(tileH * dpr))});
                }
            }
        }
        std::vector<std::tuple<int, uint64_t, ThumbKey>> byAge;
        for (const auto& t : thumbs) byAge.emplace_back(shown.count({t.first.media, t.first.height}) ? 1 : 0, t.second.used, t.first);
        std::sort(byAge.begin(), byAge.end());
        for (size_t k = 0; k < 300 && k < byAge.size(); ++k) thumbs.erase(std::get<2>(byAge[k]));
    }
    if (keys.empty()) return;
    QRegion r;
    size_t n = 0;
    const Project& pr = app.project();
    const QRect lanes = lanesRect();
    const double from = timeAtF(lanes.left() - 20.0), to = timeAtF(lanes.right() + 20.0);
    for (const Row& row : rows) {
        const int y = rowTop(row);
        if (y + row.h <= lanes.top() || y > lanes.bottom()) continue;
        const std::vector<Event>& ev = pr.tracks[(size_t)row.index].events;
        const std::pair<size_t, size_t> range = eventRange((size_t)row.index, from, to);
        for (size_t k = range.first; k < range.second; ++k) {
            const Event& e = ev[k];
            if (!(e.end() >= from && e.start <= to)) continue;
            const MediaItem* m = mediaOf(e.mediaId);
            if (!m) continue;
            const int mk = mediaKeyFast(*m);
            if (!keys.count(mk)) continue;
            if (++n > 600) {
                q->update(lanes);
                return;
            }
            QRect area = eventArea(row, e);
            auto ps = spans.find(mk);
            // only some thumbnails or more of its waveform: the part of the event that shows that stretch of the file
            // (an event that plays its file straight; reversed, looped, velocity or effect-timed ones are repainted
            // whole)
            if (ps != spans.end() && !wholeKeys.count(mk) && !e.reverse && !e.loop && e.rate > 0 && eventFxRate(e) == 1.0 &&
                !findEnvelope(e.envelopes, "velocity") && !area.isEmpty()) {
                const double t0 = e.start + (ps->second.first - e.offset) / e.rate, t1 = e.start + (ps->second.second - e.offset) / e.rate;
                const double x0 = std::max<double>(area.left(), xAtF(std::max(t0, e.start)) - 2.0);
                const double x1 = std::min<double>(area.right() + 1, xAtF(std::min(t1, e.end())) + 2.0);
                if (x1 <= x0) continue;
                area = QRect((int)std::floor(x0), area.top(), (int)std::ceil(x1 - x0) + 1, area.height()).intersected(area);
            }
            r |= area;
        }
    }
    if (!r.isEmpty()) q->update(r);
}

int TimelineWidget::Impl::mediaKeyFast(const MediaItem& m)
{
    ensureIndex();
    auto it = mediaKeyOf.find(&m);
    if (it != mediaKeyOf.end()) return it->second;
    const int k = mediaKey(m);
    mediaKeyOf[&m] = k;
    if (!m.generated && isProjectFile(m.path) && projectMediaKeys.insert(k).second) {
        NestedStamp ns = nestedStampOf(m.path);
        ns.path = m.path;
        nestedStamps[k] = ns;
    }
    return k;
}

TimelineWidget::Impl::NestedStamp TimelineWidget::Impl::nestedStampOf(const std::string& path)
{
    const QFileInfo fi(QString::fromStdString(path));
    NestedStamp ns;
    if (fi.exists()) {
        ns.size = fi.size();
        ns.time = fi.lastModified().toMSecsSinceEpoch();
    }
    return ns;
}

int TimelineWidget::Impl::mediaKey(const MediaItem& m)
{
    const std::string name = m.generated ? strfmt("generated:%llu", (unsigned long long)m.id) : m.path;
    auto it = mediaKeys.find(name);
    if (it != mediaKeys.end()) return it->second;
    const int k = (int)mediaKeys.size() + 1;
    mediaKeys[name] = k;
    return k;
}

const QImage* TimelineWidget::Impl::thumb(const ThumbKey& key, const MediaItem& m, double seconds, int w, int h)
{
    auto it = thumbs.find(key);
    if (it != thumbs.end()) {
        it->second.used = paintCount;
        if (it->second.stale) loader->requestThumb(key, m, seconds, w, h);
        return it->second.image.isNull() ? nullptr : &it->second.image;
    }
    loader->requestThumb(key, m, seconds, w, h);
    const Thumb* best = nullptr;
    int64_t bestGap = 0;
    it = thumbs.lower_bound(key);
    if (it != thumbs.end() && it->first.media == key.media && it->first.height == key.height && !it->second.image.isNull()) {
        best = &it->second;
        bestGap = it->first.ms - key.ms;
    }
    if (it != thumbs.begin()) {
        --it;
        if (it->first.media == key.media && it->first.height == key.height && !it->second.image.isNull() &&
            (!best || key.ms - it->first.ms < bestGap))
            best = &it->second;
    }
    return best ? &best->image : nullptr;
}

void TimelineWidget::Impl::paint(QPainter& p, const QRect& clip)
{
    ++paintCount;
    updateFonts();
    ensureIndex(true);
    paintLanes(p, clip);
    paintHeaders(p, clip);
    paintRuler(p, clip);
    paintCursor(p, clip);
    paintCorners(p, clip);
    paintedCursor = app.cursor();
    paintedLoopA = app.loopStart;
    paintedLoopB = app.loopEnd;
    paintedLoopOn = app.loopOn;
}

double TimelineWidget::Impl::rulerLabelWidth() const
{
    if (rulerLabelW <= 0) {
        rulerLabelW = QFontMetrics(rulerFont).horizontalAdvance(QString("00:00:00:00")) + 20.0;
        rulerLabelFps = 0;
    }
    if (rulerLabelFps != fps()) {
        rulerLabelFps = fps();
        const int digits = frameDigits(std::max(1, (int)std::lround(rulerLabelFps)));
        rulerLabelWide = digits > 2 ? QFontMetrics(rulerFont).horizontalAdvance(QString(digits - 2, QChar('0'))) : 0.0;
    }
    return rulerLabelW + rulerLabelWide;
}

// A straight fade line from (xa, ya) to (xb, yb), drawn only between clipL and clipR (events can reach a very long way
// off screen). With 'shade', the space between the line and the level 'shadeTo' is darkened.
void TimelineWidget::Impl::drawRamp(QPainter& p, double xa, double ya, double xb, double yb, double clipL, double clipR, bool shade, double shadeTo)
{
    if (xb - xa < 0.5) {
        if (xa >= clipL && xa <= clipR) p.drawLine(QPointF(xa, ya), QPointF(xa, yb));
        return;
    }
    const double la = std::max(xa, clipL), lb = std::min(xb, clipR);
    if (lb <= la) return;
    const double y0 = ya + (la - xa) / (xb - xa) * (yb - ya), y1 = ya + (lb - xa) / (xb - xa) * (yb - ya);
    if (shade) {
        QPolygonF poly;
        poly << QPointF(la, shadeTo) << QPointF(lb, shadeTo) << QPointF(lb, y1) << QPointF(la, y0);
        const QPen pen = p.pen();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 112));
        p.drawPolygon(poly);
        p.setBrush(Qt::NoBrush);
        p.setPen(pen);
    }
    p.drawLine(QPointF(la, y0), QPointF(lb, y1));
}

// A fade's line from xa to xb with its curve (rising from 'bottom' to 'top' for a fade-in, falling for a fade-out),
// drawn only between clipL and clipR; with 'shade' the part faded away is darkened.
void TimelineWidget::Impl::drawFade(QPainter& p, double xa, double xb, double top, double bottom, int curve, bool in, double clipL, double clipR, bool shade)
{
    if (curve == CurveLinear || xb - xa < 0.5) {
        if (in) drawRamp(p, xa, bottom, xb, top, clipL, clipR, shade, top);
        else drawRamp(p, xa, top, xb, bottom, clipL, clipR, shade, top);
        return;
    }
    const double la = std::max(xa, clipL), lb = std::min(xb, clipR);
    if (lb <= la) return;
    QPolygonF line;
    for (double x = la;; x = std::min(x + 2.0, lb)) {
        const double u = (x - xa) / (xb - xa);
        const double level = in ? curveShape(curve, u) : 1.0 - curveShape(curve, u);
        line << QPointF(x, bottom - level * (bottom - top));
        if (x >= lb) break;
    }
    if (shade) {
        QPolygonF poly = line;
        poly << QPointF(lb, top) << QPointF(la, top);
        const QPen pen = p.pen();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 112));
        p.drawPolygon(poly);
        p.setBrush(Qt::NoBrush);
        p.setPen(pen);
    }
    p.drawPolyline(line);
}

void TimelineWidget::Impl::drawLinkMark(QPainter& p, const QPointF& at, const QColor& color)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, 1.2));
    p.drawRoundedRect(QRectF(at.x(), at.y() - 2.5, 7.0, 5.0), 2.5, 2.5);
    p.drawRoundedRect(QRectF(at.x() + 4.5, at.y() - 2.5, 7.0, 5.0), 2.5, 2.5);
    p.setRenderHint(QPainter::Antialiasing, false);
}

// How finely thumbnails are told apart in time, given how much media one of them stands for: coarse enough that
// scrolling and small changes of zoom reuse the same thumbnails, never coarser than what is shown.
double TimelineWidget::Impl::thumbGrain(double tileSeconds, double mediaFps)
{
    const double one = mediaFps > 0 ? 1.0 / mediaFps : 1.0 / 30.0;
    double g = one;
    for (double c : {0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0, 600.0})
        if (c > g && c <= tileSeconds) g = c;
    return g;
}

// Preferences > Video "Show source frame numbers on event thumbnails as": the media's frame number, time (hh:mm:ss.mmm)
// or timecode (at the media's own rate) in the thumbnail's top left corner, as VEGAS writes it.
void TimelineWidget::Impl::drawThumbNumber(QPainter& p, const QRectF& target, double mt, double mediaFps, const std::string& how)
{
    const double fpsM = mediaFps > 0 ? mediaFps : 30.0;
    const long long frame = (long long)std::floor(mt * fpsM + 1e-6);
    QString text;
    if (how == "frames") text = QString::number(frame);
    else if (how == "time") {
        const long long ms = (long long)std::floor(mt * 1000.0 + 1e-6), sec = ms / 1000;
        text = QString::asprintf("%02lld:%02lld:%02lld.%03lld", sec / 3600, (sec / 60) % 60, sec % 60, ms % 1000);
    } else {
        const int nominal = std::max(1, (int)std::lround(fpsM));
        const long long sec = frame / nominal;
        text = QString::asprintf("%02lld:%02lld:%02lld:%0*lld", sec / 3600, (sec / 60) % 60, sec % 60, frameDigits(nominal), frame % nominal);
    }
    const QFontMetrics fm(smallFont);
    const QRectF box(target.left() + 1.0, target.top() + 1.0, std::min(target.width() - 2.0, fm.horizontalAdvance(text) + 6.0), fm.height() + 1.0);
    p.save();
    p.setClipRect(target, Qt::IntersectClip);
    p.fillRect(box, QColor(0, 0, 0, 170));
    p.setFont(smallFont);
    p.setPen(QColor(235, 235, 235));
    p.drawText(box, Qt::AlignCenter, text);
    p.restore();
}

void TimelineWidget::Impl::drawThumbs(QPainter& p, const Event& e, const MediaItem& m, const QRectF& r, const QRectF& content, const QRect& c)
{
    const double tileH = content.height();
    if (tileH < 8) return;
    double aspect = (m.info.width > 0 && m.info.height > 0) ? (double)m.info.width / m.info.height : 16.0 / 9.0;
    aspect = clampd(aspect, 0.4, 4.0);
    const double tileW = std::max(8.0, std::floor(tileH * aspect));
    const double dpr = q->devicePixelRatioF();
    // Preferences > Reno > Waveforms and pictures: Thumbnail quality; draft asks for half the size and lets it be
    // stretched, high for twice
    const double qf = vprefs::thumbScale();
    const int reqW = std::max(8, (int)std::lround(tileW * dpr * qf)), reqH = std::max(8, (int)std::lround(tileH * dpr * qf));
    const double dur = (m.info.isStill || m.generated) ? 0.0 : m.info.duration;
    const double grain = thumbGrain(tileW / pps * e.rate, m.info.fps);
    const int key = mediaKeyFast(m);
    const double fpsM = m.info.fps > 0 ? m.info.fps : 0.0;
    const double lastFrame = dur - (fpsM > 0 ? 1.0 / fpsM : 0.04);
    const std::string& which = app.prefs.videoThumbnails;
    const std::string& numbers = app.prefs.thumbnailNumbers;
    auto tile = [&](const QRectF& target, double mt) {
        if (mt > lastFrame) mt = std::max(0.0, lastFrame);
        ThumbKey tk;
        tk.media = key;
        tk.height = reqH;
        tk.ms = (int64_t)std::llround(mt * 1000.0);
        if (const QImage* img = thumb(tk, m, mt, reqW, reqH)) p.drawImage(target, *img);
        else p.fillRect(target, QColor(18, 18, 20));
        if (numbers != "none" && dur > 0 && tileH >= 14) drawThumbNumber(p, target, mt, fpsM, numbers);
    };
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (which != "all") p.fillRect(content, QColor(34, 34, 38));
    if (which == "none") {
    } else if (which != "all") {
        // VEGAS's Head / Head, Tail / Head, Center, Tail: the first image at the start, the last one at the end, the
        // middle one in the middle (only those that fit side by side)
        const double evW = r.width();
        std::vector<std::pair<double, double>> at;
        const double endT = e.start + e.length;
        at.push_back({r.left() + 1.0, dur > 0 ? eventMediaTime(e, &m, e.start) : 0.0});
        if (which != "head" && evW >= tileW * 2.0)
            at.push_back({r.right() - tileW, dur > 0 ? eventMediaTime(e, &m, std::max(e.start, endT - 1.0 / std::max(1.0, app.project().settings.fps()))) : 0.0});
        if (which == "headCenterTail" && evW >= tileW * 3.0)
            at.push_back({r.center().x() - tileW * 0.5, dur > 0 ? eventMediaTime(e, &m, e.start + e.length * 0.5) : 0.0});
        for (const auto& a : at) {
            const QRectF target(a.first, content.top(), tileW, tileH);
            if (target.right() < content.left() || target.left() > content.right()) continue;
            p.save();
            p.setClipRect(content, Qt::IntersectClip);
            tile(target, a.second);
            p.restore();
        }
    } else {
        const double from = std::max(content.left(), (double)c.left() - 1.0), to = std::min(content.right(), (double)c.right() + 1.0);
        const int64_t first = std::max<int64_t>(std::max<int64_t>(0, (int64_t)std::floor((content.left() - r.left()) / tileW)),
                                                (int64_t)std::floor((from - r.left() - 1.0) / tileW));
        const int64_t last = std::min<int64_t>((int64_t)std::floor((content.right() - r.left()) / tileW), (int64_t)std::floor((to - r.left()) / tileW));
        for (int64_t k = first; k <= last; ++k) {
            const QRectF target(r.left() + (double)k * tileW + 1.0, content.top(), tileW, tileH);
            double mt = 0;
            if (dur > 0) {
                mt = eventMediaTime(e, &m, e.start + (double)k * tileW / pps);
                mt = std::floor(mt / grain + 0.5) * grain;
            }
            tile(target, mt);
        }
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
}

void TimelineWidget::Impl::drawWave(QPainter& p, const Event& e, const MediaItem& m, const QRectF& content, const QColor& color, const QRect& c)
{
    const double mid = std::floor(content.center().y()) + 0.5;
    p.setPen(QColor(color.red(), color.green(), color.blue(), 70));
    p.drawLine(QPointF(content.left(), mid), QPointF(content.right(), mid));
    if (m.generated) return;
    const int key = mediaKeyFast(m);
    const std::shared_ptr<Peaks> pk = loader->peaks(key, m, e.mediaChannel, e.mediaChannels);
    if (!pk) return;
    {
        ShownPeaks& sp = shownPeaks[std::make_tuple(key, e.mediaChannel < 0 ? -1 : e.mediaChannel, e.mediaChannel < 0 ? 0 : e.mediaChannels)];
        if (sp.peaks.expired()) {
            sp.peaks = pk;
            sp.seen = pk->complete.load() ? -1 : pk->filled.load();
        }
    }
    const double dur = loader->duration(m);
    if (dur <= 0) return;
    // the drawn height follows the event's gain and its Normalize switch, as in VEGAS (Shift+Up / Shift+Down zoom the
    // waveforms vertically so quiet sound is easier to see)
    const double level = e.gain * (e.normalize && e.normalizeGain > 0 ? e.normalizeGain : 1.0);
    const double amp = (content.height() * 0.5 - 1.0) * clampd(level, 0.0, 1000.0) * waveZoom / 127.0;
    const double span = e.rate / pps;
    size_t mip = 0;
    double perPx = span * kPeakPerSecond;
    while (mip + 1 < pk->levels.size() && perPx / kPeakStep >= 1.0) {
        ++mip;
        perPx /= kPeakStep;
    }
    double perSecond = kPeakPerSecond;
    for (size_t k = 0; k < mip; ++k) perSecond /= kPeakStep;
    const Peaks::Level& lv = pk->levels[mip];
    const int64_t count = (int64_t)lv.lo.size(), ready = std::min(count, pk->ready(mip));
    if (ready <= 0) return;

    QVector<QLineF> lines;
    const int x0 = (int)std::ceil(std::max(content.left(), (double)c.left() - 1.0)), x1 = (int)std::floor(std::min(content.right(), (double)c.right() + 1.0));
    lines.reserve(std::max(0, x1 - x0 + 1));
    for (int x = x0; x <= x1; ++x) {
        // a reversed event plays its stretch of the media from its end back: the column shows the stretch that ends
        // where a forward one would begin, as the sound is heard and as VEGAS draws it
        double m0 = e.reverse ? e.offset + (e.length - (timeAtF(x) - e.start)) * e.rate - span : e.offset + (timeAtF(x) - e.start) * e.rate;
        if (e.loop) {
            m0 = std::fmod(m0, dur);
            if (m0 < 0) m0 += dur;
        } else if (m0 < 0 || m0 >= dur) continue;
        double lo = 0, hi = 0;
        if (perPx < 1.0) {
            const double at = (m0 + span * 0.5) * perSecond - 0.5;
            const int64_t a = std::max<int64_t>(0, (int64_t)std::floor(at)), b = std::min(a + 1, count - 1);
            if (b >= ready) continue;
            const double f = clampd(at - (double)a, 0.0, 1.0);
            lo = lv.lo[(size_t)a] + (lv.lo[(size_t)b] - lv.lo[(size_t)a]) * f;
            hi = lv.hi[(size_t)a] + (lv.hi[(size_t)b] - lv.hi[(size_t)a]) * f;
        } else {
            const int64_t a = (int64_t)std::floor(m0 * perSecond);
            const int64_t n = std::max<int64_t>(1, (int64_t)std::floor((m0 + span) * perSecond) - a);
            int vlo = 127, vhi = -127;
            for (int64_t j = a; j < a + n; ++j) {
                const int64_t jj = j < count ? j : (e.loop ? j - count : -1);
                if (jj < 0 || jj >= ready) continue;
                vlo = std::min<int>(vlo, lv.lo[(size_t)jj]);
                vhi = std::max<int>(vhi, lv.hi[(size_t)jj]);
            }
            if (vlo > vhi) continue;
            lo = vlo;
            hi = vhi;
        }
        const double ya = std::max(content.top(), mid - hi * amp), yb = std::min(content.bottom(), mid - lo * amp);
        lines.push_back(QLineF(x + 0.5, std::min(ya, mid - 0.5), x + 0.5, std::max(yb, mid + 0.5)));
    }
    p.setPen(color);
    p.drawLines(lines);
}

void TimelineWidget::Impl::drawMenuIcon(QPainter& p, const QRectF& b, const QColor& color)
{
    const double w = std::min(b.width() - 2.0, 13.0), x = b.center().x() - w * 0.5;
    const double gap = std::max(3.0, std::floor(b.height() / 4.0));
    for (int k = -1; k <= 1; ++k) p.fillRect(QRectF(x, std::floor(b.center().y() + k * gap) - 1.0, w, 2.0), color);
}

void TimelineWidget::Impl::drawKeptText(QPainter& p, const QRectF& r, const QString& text, KeptText& k, bool centre)
{
    if (text.isEmpty() || p.pen().style() == Qt::NoPen || r.isEmpty()) return;
    const int flags = centre ? (int)Qt::AlignCenter : (int)(Qt::AlignVCenter | Qt::AlignLeft);
    bool plain = p.layoutDirection() == Qt::LeftToRight;
    for (const QChar c : text)
        if (c == u'\r' || c == u'\n' || c == u'\t' || c == QChar(0x9c)) plain = false;
    if (!plain) {
        p.drawText(r, flags, text);
        return;
    }
    const int dpi = p.device() ? p.device()->logicalDpiY() : 0;
    if (!k.layout || k.text != text || !(k.font == p.font()) || k.dpi != dpi) {
        k.text = text;
        k.font = p.font();
        k.dpi = dpi;
        k.layout.reset(new QTextLayout(text, k.font));
        QTextOption o;
        o.setTextDirection(Qt::LeftToRight);
        o.setAlignment(Qt::AlignLeft);
        k.layout->setTextOption(o);
        k.layout->setCacheEnabled(true);
        k.layout->setFormats(QList<QTextLayout::FormatRange>());
        k.layout->beginLayout();
        QTextLine line = k.layout->createLine();
        if (line.isValid()) {
            line.setLineWidth(0x01000000);
            line.setPosition(QPointF(0., 0.));
            k.height = line.ascent() + line.descent();
            k.width = line.naturalTextWidth();
        }
        k.layout->endLayout();
        if (!line.isValid() || k.layout->lineCount() != 1) {
            k.layout.reset();
            p.drawText(r, flags, text);
            return;
        }
    }
    const QTextLine line = k.layout->lineAt(0);
    const double yoff = (r.height() - k.height) / 2;
    const double boundsX = centre ? (r.width() - k.width) / 2 : 0.0;
    const bool clip = !r.contains(QRectF(r.x() + boundsX, r.y() + yoff, k.width, k.height));
    if (clip) {
        p.save();
        p.setClipRect(r, Qt::IntersectClip);
    }
    line.draw(&p, QPointF(r.x() + (centre ? (r.width() - line.horizontalAdvance()) / 2 : 0.0), r.y() + yoff));
    if (clip) p.restore();
}

double TimelineWidget::Impl::titleWidth(const Event& e)
{
    TitleText& t = titleText(e, mediaOf(e.mediaId), -2);
    if (t.fullWidth < 0) t.fullWidth = smallFm.horizontalAdvance(t.full);
    return t.fullWidth;
}

TimelineWidget::Impl::TitleText& TimelineWidget::Impl::titleText(const Event& e, const MediaItem* m, int width)
{
    const std::string& source = !e.name.empty() ? e.name : (m ? m->name : std::string("?"));
    if (titles.size() > 6000) titles.clear();
    TitleText& t = titles[e.id];
    if (!t.made || t.source != source || t.rate != e.rate || t.reverse != e.reverse) {
        t.made = true;
        t.source = source;
        t.rate = e.rate;
        t.reverse = e.reverse;
        t.full = QString::fromStdString(source);
        if (std::fabs(e.rate - 1.0) > 1e-6) t.full += QString::fromStdString(strfmt("  (%d%%)", (int)std::lround(e.rate * 100.0)));
        if (e.reverse) t.full += QString("  (Reversed)");
        t.width = -1;
        t.fullWidth = -1;
    }
    if (width >= 0 && t.width != width) {
        t.width = width;
        t.elided = smallFm.elidedText(t.full, Qt::ElideRight, width);
    }
    return t;
}

TimelineWidget::Impl::RowLook TimelineWidget::Impl::rowLook(const Row& row) const
{
    RowLook l;
    l.tc = trackColor(app.project().tracks[(size_t)row.index], row.index);
    l.body = row.video ? mix(l.tc, QColor(16, 16, 16), 0.55) : l.tc;
    l.title = mix(l.tc, QColor(0, 0, 0), 0.25);
    l.wave = mix(l.tc, QColor(255, 255, 255), 0.62);
    // Preferences > Display, "Reverse Event Background Color": a light event with a dark waveform instead of a dark one
    // with a light waveform
    if (!row.video && vprefs::on("prefs/reverseEventBg")) {
        l.body = mix(l.tc, QColor(255, 255, 255), 0.55);
        l.wave = mix(l.tc, QColor(0, 0, 0), 0.7);
    }
    return l;
}

// An event too narrow for anything but a sliver of color, that can be drawn together with the slivers next to it
// (zoomed out over a stutter edit, thousands of them: VEGAS too draws them as one strip). 0: not in this paint; 1:
// such a sliver, its rectangle and color given; 2: drawn on its own (drawEvent).
int TimelineWidget::Impl::sliverOf(const Row& row, const Event& e, const QRect& c, const RowLook& look, QRectF* rect, QColor* color) const
{
    const QRectF r = eventRect(row, e);
    if (r.right() < c.left() - 2 || r.left() > c.right() + 2) return 0;
    const QRect lanes = lanesRect();
    const QRectF v(QPointF(std::max(r.left(), lanes.left() - 16.0), r.top()), QPointF(std::min(r.right(), lanes.right() + 16.0), r.bottom()));
    if (!(v.width() < 5.0) || e.mute) return 2;
    const bool selected = app.selection().count(e.id) != 0;
    if (selected) return 2;
    *rect = v;
    *color = row.video ? look.tc : look.body;
    return 1;
}

void TimelineWidget::Impl::drawEvent(QPainter& p, const Row& row, const Event& e, const QRect& c, const RowLook& look)
{
    const QRectF r = eventRect(row, e);
    if (r.right() < c.left() - 2 || r.left() > c.right() + 2) return;
    const MediaItem* m = mediaOf(e.mediaId);
    const bool selected = app.selection().count(e.id) != 0;
    const QColor body = look.body;
    // Only the part in view is drawn: zoomed far in, an event can be a very long way wide. It is the part in the
    // whole view, not just in what this paint covers, so a part painted on its own looks exactly as in a whole
    // repaint (its name elided at the same place and so on).
    const QRect lanes = lanesRect();
    const QRectF v(QPointF(std::max(r.left(), lanes.left() - 16.0), r.top()), QPointF(std::min(r.right(), lanes.right() + 16.0), r.bottom()));
    if (v.width() < 5.0) {
        const qreal opacity = p.opacity();
        if (e.mute) p.setOpacity(0.42);
        p.fillRect(v, row.video ? look.tc : body);
        if (selected) p.fillRect(v, QColor(255, 255, 255, 80));
        if (e.mute) p.setOpacity(opacity);
        return;
    }
    p.save();
    if (e.mute) p.setOpacity(0.42);
    // A narrow event (a stutter's, zoomed out) is drawn plainly: square corners, straight fade lines with nothing
    // shaded, no smoothing. Thousands are drawn at once, and at that size it looks the same.
    const bool narrow = v.width() < 12.0;
    // Its body and outline are drawn only as far as 16 pixels past what this paint covers: the same pixels there, and
    // an event running across the whole view is not outlined from end to end.
    const QRectF vd(QPointF(std::max(v.left(), c.left() - 16.0), v.top()), QPointF(std::min(v.right(), c.right() + 16.0), v.bottom()));
    p.fillRect(vd, body);

    const EventPaint ev{row, e, m, c, look, r, v, vd, selected, narrow};
    const double titleH = eventTitleH(r);
    p.save();
    p.setClipRect(vd.intersected(QRectF(c)));
    drawEventContent(p, ev, titleH);
    drawEventTitle(p, ev, titleH);
    drawFadesAndGain(p, ev);
    drawLoopNotches(p, ev);
    p.restore();
    drawEventOutline(p, ev);
    drawFadeLengths(p, ev);
    drawFadeHandles(p, ev);
    drawEventButtons(p, ev);
    drawEventExtras(p, e, r, v);
    p.restore();
}

void TimelineWidget::Impl::drawEventContent(QPainter& p, const EventPaint& ev, double titleH)
{
    const Event& e = ev.e;
    const MediaItem* m = ev.m;
    const QRectF &r = ev.r, &v = ev.v;
    const QColor& tc = ev.look.tc;
    const QRectF content(v.left(), v.top() + titleH, v.width(), v.height() - titleH);
    if (!m && !e.mediaId) {
        // an event with no media: VEGAS's empty event is drawn plain; VEGAS Pro 23's adjustment event "with special
        // shading" is a hatch of light lines over the track's color, labeled
        if (e.adjustment) {
            p.fillRect(content, mix(tc, QColor(30, 30, 40), 0.55));
            p.fillRect(content, QBrush(QColor(255, 255, 255, 46), Qt::BDiagPattern));
            if (content.width() > 70 && content.height() > 12) {
                p.setFont(smallFont);
                p.setPen(QColor(235, 235, 245));
                p.drawText(content, Qt::AlignCenter, QString("Adjustment"));
            }
        }
    } else if (!m || m->offline) {
        p.fillRect(content, QColor(70, 36, 36));
        p.fillRect(content, QBrush(QColor(255, 255, 255, 28), Qt::DiagCrossPattern));
        if (content.width() > 90) {
            p.setFont(smallFont);
            p.setPen(QColor(240, 205, 205));
            p.drawText(content, Qt::AlignCenter, QString("Media Offline"));
        }
    } else if (ev.row.video) {
        if (m->generated) {
            p.fillRect(content, mix(tc, QColor(20, 20, 20), 0.5));
            p.fillRect(content, QBrush(QColor(255, 255, 255, 40), Qt::BDiagPattern));
        } else {
            drawThumbs(p, e, *m, r, content, ev.c);
            if (std::fabs(e.rate - 1.0) > 1e-6 && content.height() > 10 && !ev.narrow) drawStretchMark(p, ev, content);
        }
    } else if (opt.showWaveforms) {
        drawWave(p, e, *m, content, ev.look.wave, ev.c);
    }
}

void TimelineWidget::Impl::drawStretchMark(QPainter& p, const EventPaint& ev, const QRectF& content)
{
    const QRect& c = ev.c;
    QPolygonF zig;
    const double y0 = content.top() + 2.0, y1 = content.top() + 6.0;
    const double xs = ev.r.left(), to = std::min(content.right(), c.right() + 8.0);
    long long k = (long long)std::ceil((std::max(content.left(), c.left() - 8.0) - xs) / 4.0);
    for (double x = xs + 4.0 * k; x <= to; x += 4.0, ++k) zig << QPointF(x, k % 2 ? y1 : y0);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(0, 0, 0, 160), 2.6));
    p.drawPolyline(zig);
    p.setPen(QPen(QColor(255, 255, 255, 220), 1.1));
    p.drawPolyline(zig);
    p.restore();
}

void TimelineWidget::Impl::drawEventTitle(QPainter& p, const EventPaint& ev, double titleH)
{
    const Event& e = ev.e;
    const QRectF &r = ev.r, &v = ev.v, &vd = ev.vd;
    const QRect& c = ev.c;
    if (titleH > 0) {
        p.fillRect(QRectF(vd.left(), vd.top(), vd.width(), titleH), ev.look.title);
        double tx = std::max(r.left(), (double)headerW) + 5.0;
        double textRight = v.right() - 8.0;
        if (opt.showEventButtons && hasMenuButton(r)) {
            double left = menuButtonRect(r).left();
            if (hasFxButton(r)) left = fxButtonRect(r).left();
            if (hasCropButton(r, ev.row.video)) left = cropButtonRect(r).left();
            textRight = std::min(textRight, left - 4.0);
        }
        if (opt.showEventLength && textRight - tx > 90.0) {
            const QFontMetrics lfm(smallFont);
            const QString lt = QString::fromStdString(timecode(e.length, fps()));
            const double lw = lfm.horizontalAdvance(lt) + 4.0;
            if (textRight - lw > c.left() - 40.0 && tx < c.right() + 2) {
                p.setFont(smallFont);
                p.setPen(QColor(230, 230, 230));
                p.drawText(QRectF(textRight - lw, v.top(), lw, titleH), Qt::AlignRight | Qt::AlignVCenter, lt);
            }
            textRight -= lw + 6.0;
        }
        if (e.group && textRight - tx > 30) {
            if (tx < c.right() + 2 && tx + 14.0 > c.left()) drawLinkMark(p, QPointF(tx, v.top() + titleH * 0.5), QColor(240, 240, 240));
            tx += 15.0;
        }
        const double textW = textRight - tx;
        if (textW > 14 && tx < c.right() + 2) {
            p.setFont(smallFont);
            p.setPen(QColor(240, 240, 240));
            TitleText& tt = titleText(e, ev.m, (int)textW);
            drawKeptText(p, QRectF(tx, v.top(), textW, titleH), tt.elided, tt.kept);
        }
    } else if (e.group && v.width() > 40) {
        const double lx = std::max(r.left(), (double)headerW) + 4.0;
        if (lx - 3.0 < c.right() + 1 && lx + 16.0 > c.left()) {
            p.save();
            outlined(p, [&](const QColor& col) { drawLinkMark(p, QPointF(lx, v.bottom() - 7.0), col); });
            p.restore();
        }
    }
}

void TimelineWidget::Impl::drawFadesAndGain(QPainter& p, const EventPaint& ev)
{
    const Event& e = ev.e;
    const QRectF &r = ev.r, &v = ev.v;
    const QRect& c = ev.c;
    const bool narrow = ev.narrow;
    p.setRenderHint(QPainter::Antialiasing, !narrow && !dense);
    p.setPen(QPen(QColor(250, 250, 250, 220), narrow ? 1.0 : 1.2));
    const bool gainLine = hasGainLine(r) && !narrow;
    const double gainY = gainLine ? gainLineY(r, e.gain) : r.top();
    if (e.fadeIn > 0)
        drawFade(p, r.left(), std::min(xAtF(e.start + e.fadeIn), r.right()), gainY, r.bottom(), narrow ? CurveLinear : e.fadeInCurve, true, v.left(), v.right(), !narrow);
    if (e.fadeOut > 0)
        drawFade(p, std::max(xAtF(e.end() - e.fadeOut), r.left()), r.right(), gainY, r.bottom(), narrow ? CurveLinear : e.fadeOutCurve, false, v.left(), v.right(), !narrow);
    if (!gainLine) return;
    // the gain line (audio: volume, video: opacity): across the event at the gain's height, with VEGAS's small cyan
    // handle in the middle (shown on a selected event, under the mouse, and wherever the gain is lowered)
    const bool lowered = e.gain < 0.9995;
    const bool gainHot = (hover.event == e.id && hover.part == Part::EventGain) ||
                         (drag.kind == Drag::EventGain && drag.started && std::any_of(drag.gains.begin(), drag.gains.end(), [&](const std::pair<uint64_t, double>& g) { return g.first == e.id; }));
    if (lowered || (gainHot && !(drag.kind == Drag::EventGain && !drag.started))) {
        p.setRenderHint(QPainter::Antialiasing, false);
        const double ly = std::floor(gainY) + 1.0;
        p.setPen(QPen(QColor(255, 255, 255, 240), 2.0, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(v.left() + 1.0, ly), QPointF(v.right() - 1.0, ly));
    }
    if (v.width() >= 40.0 && (lowered || ev.selected || gainHot)) {
        const double cx = std::floor((r.left() + r.right()) * 0.5);
        if (cx + 8.0 >= c.left() && cx - 8.0 <= c.right()) {
            const double ty = std::floor(gainY) + (lowered ? 2.0 : 1.0);
            const double w = gainHot ? 7.0 : 5.5, h = gainHot ? 5.0 : 3.5;
            QPolygonF tab;
            tab << QPointF(cx - w, ty) << QPointF(cx + w, ty) << QPointF(cx + w - h, ty + h) << QPointF(cx - w + h, ty + h);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(QPen(QColor(0, 70, 90), 1.0));
            p.setBrush(gainHot ? QColor(120, 240, 255) : QColor(0, 220, 250));
            p.drawPolygon(tab);
        }
    }
}

void TimelineWidget::Impl::drawLoopNotches(QPainter& p, const EventPaint& ev)
{
    const Event& e = ev.e;
    const MediaItem* m = ev.m;
    const QRectF &r = ev.r, &v = ev.v;
    const QRect& c = ev.c;
    const double dur = (m && !m->info.isStill && !m->generated) ? m->info.duration : 0.0;
    if (!e.loop || dur <= 0 || e.rate <= 0 || dur / e.rate * pps < 8.0) return;
    const double from = std::max(e.start, timeAtF(std::max(v.left(), c.left() - 8.0))), to = std::min(e.end(), timeAtF(v.right()));
    int64_t k = (int64_t)std::floor((e.offset + (from - e.start) * e.rate) / dur) + 1;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(16, 16, 16));
    for (;; ++k) {
        const double t = e.start + ((double)k * dur - e.offset) / e.rate;
        if (t >= to - frame() * 0.5) break;
        const double x = xAtF(t);
        if (x > c.right() + 8.0) break;
        QPolygonF notch;
        notch << QPointF(x - 4.5, r.top()) << QPointF(x + 4.5, r.top()) << QPointF(x, r.top() + 7.0);
        p.drawPolygon(notch);
    }
}

// Selected: lit up, with a light rim; else a dark line where it starts and ends so events that meet are told apart.
void TimelineWidget::Impl::drawEventOutline(QPainter& p, const EventPaint& ev)
{
    const QRectF &r = ev.r, &v = ev.v, &vd = ev.vd;
    const bool selected = ev.selected, narrow = ev.narrow;
    p.setRenderHint(QPainter::Antialiasing, false);
    if (selected) {
        p.fillRect(vd, QColor(255, 255, 255, 58));
        p.setPen(QColor(236, 236, 236));
        p.setBrush(Qt::NoBrush);
        p.drawRect(vd.adjusted(0, 0, -1, -1));
    } else {
        if (r.left() >= v.left() - 0.5) p.fillRect(QRectF(std::floor(r.left()), v.top(), 1.0, v.height()), QColor(0, 0, 0, 120));
        if (r.right() <= v.right() + 0.5) p.fillRect(QRectF(std::floor(r.right()) - 1.0, v.top(), 1.0, v.height()), QColor(0, 0, 0, 120));
    }
    if (!narrow) {
        p.fillRect(QRectF(vd.left(), std::floor(r.top()), vd.width(), 1.0), QColor(216, 216, 216));
    }
}

void TimelineWidget::Impl::drawFadeLengths(QPainter& p, const EventPaint& ev)
{
    if (!opt.showFadeLengths || !ev.selected) return;
    const Event& e = ev.e;
    const QRectF& r = ev.r;
    p.setFont(smallFont);
    const QFontMetrics fm2(smallFont);
    for (int side = 0; side < 2; ++side) {
        const double len = side == 0 ? e.fadeIn : e.fadeOut;
        if (len <= 0) continue;
        const QString text = QString::fromStdString(timecode(len, fps()));
        const double tw = fm2.horizontalAdvance(text) + 6.0;
        const double x = side == 0 ? xAtF(e.start + len) + 3.0 : xAtF(e.end() - len) - 3.0 - tw;
        const QRectF box(x, r.top() + 2.0, tw, fm2.height() + 2.0);
        if (box.left() < r.left() || box.right() > r.right()) continue;
        p.fillRect(box, QColor(0, 0, 0, 150));
        p.setPen(QColor(240, 240, 240));
        p.drawText(box, Qt::AlignCenter, text);
    }
}

void TimelineWidget::Impl::drawFadeHandles(QPainter& p, const EventPaint& ev)
{
    const Event& e = ev.e;
    const QRectF& r = ev.r;
    const QRect& c = ev.c;
    if (!hasFadeHandles(r)) return;
    p.setRenderHint(QPainter::Antialiasing, !dense);
    const double fi = clampd(xAtF(e.start + e.fadeIn), r.left() + 5.0, r.right() - 5.0);
    const double fo = clampd(xAtF(e.end() - e.fadeOut), r.left() + 5.0, r.right() - 5.0);
    for (int side = 0; side < 2; ++side) {
        const bool hot = hover.event == e.id && hover.part == (side == 0 ? Part::FadeIn : Part::FadeOut);
        const double x = side == 0 ? fi : fo;
        if (x + 7.0 < c.left() || x - 7.0 > c.right()) continue;
        // VEGAS 18's: small and pure blue; a corner of a triangle in the event's corner while there is no fade, a short
        // tab hanging from the top edge where a fade ends (bigger and lighter under the mouse)
        p.setPen(Qt::NoPen);
        p.setBrush(hot ? kSnapLine : kFadeHandle);
        const double fadeLen = side == 0 ? e.fadeIn : e.fadeOut;
        const double t = std::floor(r.top());
        QPolygonF tab;
        if (fadeLen <= 0 && !hot) {
            const double cx = side == 0 ? std::floor(r.left()) : std::floor(r.right());
            const double d = side == 0 ? 1.0 : -1.0;
            tab << QPointF(cx, t) << QPointF(cx + d * 4.0, t) << QPointF(cx, t + 4.0);
        } else {
            const double w = hot ? 6.0 : 4.0, h = hot ? 4.0 : 2.5;
            tab << QPointF(x - w, t) << QPointF(x + w, t) << QPointF(x + w - h, t + h) << QPointF(x - w + h, t + h);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.drawPolygon(tab);
    }
}

void TimelineWidget::Impl::drawEventButtons(QPainter& p, const EventPaint& ev)
{
    if (!opt.showEventButtons) return;
    const Event& e = ev.e;
    const QRectF& r = ev.r;
    const QRect& c = ev.c;
    auto button = [&](const QRectF& b, Part part) {
        if (b.right() < c.left() || b.left() > c.right()) return;
        const bool hot = hover.event == e.id && hover.part == part;
        p.setRenderHint(QPainter::Antialiasing, !dense);
        if (hot) {
            p.setPen(QPen(QColor(255, 255, 255, 200), 1.0));
            p.setBrush(QColor(255, 255, 255, 70));
            p.drawRoundedRect(b.adjusted(-1, -1, 1, 1), 2.5, 2.5);
        }
        if (part == Part::EventCrop) {
            const bool any = !e.panCrop.empty() || maskActive(e);
            outlined(p, [&](const QColor& col) { drawCropIcon(p, b, any && col.red() == 255 ? kFxOn : col); });
        } else if (part == Part::EventFx) {
            bool bad = false;
            for (const FxSlot& s : e.fx)
                if (!app.fx.error(s.id).empty()) bad = true;
            const bool any = !e.fx.empty();
            outlined(p, [&](const QColor& col) { drawFxIcon(p, b, col.red() == 255 ? (bad ? kFxBad : (any ? kFxOn : col)) : col, any); });
        } else {
            outlined(p, [&](const QColor& col) { drawMenuIcon(p, b, col); });
        }
    };
    if (hasCropButton(r, ev.row.video)) button(cropButtonRect(r), Part::EventCrop);
    if (hasFxButton(r)) button(fxButtonRect(r), Part::EventFx);
    if (hasMenuButton(r)) button(menuButtonRect(r), Part::EventMenu);
}

// What the editing adds on an event: the media's own markers, the active take, the lock, and the edge selected for
// keyboard trimming (a red bracket, as VEGAS marks it).
void TimelineWidget::Impl::drawEventExtras(QPainter& p, const Event& e, const QRectF& r, const QRectF& v)
{
    const bool snapFlag = r.width() > 24.0 && (e.snapOffset > 0 || app.selection().count(e.id) != 0);
    const bool linked = e.syncLink && app.selection().count(e.syncLink) != 0;
    bool offStart = false, offEnd = false;
    if (opt.showUnquantized) {
        const Project& qp = app.project();
        offStart = std::fabs(qp.quantize(e.start) - e.start) > 1e-5 / std::max(1.0, fps());
        offEnd = std::fabs(qp.quantize(e.end()) - e.end()) > 1e-5 / std::max(1.0, fps());
    }
    if (!offStart && !offEnd && !opt.showMediaMarkers && !(opt.showEventLength && v.width() > 40) && !(opt.showTakeInfo && v.width() > 50) && !(e.locked && v.width() > 16) &&
        !(edgeSel.event && (edgeSel.event == e.id || (edgeSel.expanded && edgeSel.other == e.id))) && !snapFlag && !linked)
        return;
    p.save();
    p.setClipRect(v);
    const Project& pr = app.project();
    const MediaItem* m = mediaOf(e.mediaId);
    if (offStart || offEnd) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 32, 32));
        if (offStart) p.drawRect(QRectF(std::floor(r.left()), r.top(), 2.0, r.height()));
        if (offEnd) p.drawRect(QRectF(std::floor(r.right()) - 1.0, r.top(), 2.0, r.height()));
    }
    if (opt.showMediaMarkers && m && e.rate > 0)
        for (const auto& mk : m->info.markers) {
            const double t = e.start + (mk.first - e.offset) / e.rate;
            if (t < e.start || t > e.end()) continue;
            const double x = std::floor(xAtF(t)) + 0.5;
            p.setPen(QPen(QColor(255, 170, 60, 200), 1.0, Qt::DotLine));
            p.drawLine(QPointF(x, v.top()), QPointF(x, v.bottom()));
            p.fillRect(QRectF(x - 2.0, v.top(), 5.0, 5.0), QColor(255, 170, 60));
            if (opt.showMarkerLabels && !mk.second.empty()) {
                const QFontMetrics fm(smallFont);
                const QString text = fm.elidedText(QString::fromStdString(mk.second), Qt::ElideRight, 140);
                const QRectF box(x + 3.0, v.top() + 1.0, fm.horizontalAdvance(text) + 6.0, fm.height() + 1.0);
                p.fillRect(box, QColor(60, 40, 10, 190));
                p.setFont(smallFont);
                p.setPen(QColor(255, 200, 120));
                p.drawText(box.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
            }
        }
    if (opt.showMediaMarkers && m && e.rate > 0)
        for (const MediaItem::Marker& mk : m->markers) {
            const double t0 = e.start + (mk.time - e.offset) / e.rate, t1 = t0 + mk.length / e.rate;
            if (t1 < e.start || t0 > e.end()) continue;
            const QColor c = mk.length > 0 ? QColor(70, 190, 90) : QColor(255, 170, 60);
            const double x = std::floor(xAtF(std::max(t0, e.start))) + 0.5;
            if (mk.length > 0) {
                const double x2 = std::floor(xAtF(std::min(t1, e.end()))) + 0.5;
                p.fillRect(QRectF(x, v.top(), x2 - x, v.height()), QColor(70, 190, 90, 35));
                if (t1 <= e.end()) {
                    p.setPen(QPen(c, 1.0, Qt::DotLine));
                    p.drawLine(QPointF(x2, v.top()), QPointF(x2, v.bottom()));
                }
            }
            if (t0 < e.start) continue;
            p.setPen(QPen(c, 1.0, Qt::DotLine));
            p.drawLine(QPointF(x, v.top()), QPointF(x, v.bottom()));
            p.fillRect(QRectF(x - 2.0, v.top(), 5.0, 5.0), c);
            if (opt.showMarkerLabels && !mk.name.empty()) {
                const QFontMetrics fm(smallFont);
                const QString text = fm.elidedText(QString::fromStdString(mk.name), Qt::ElideRight, 140);
                const QRectF box(x + 3.0, v.top() + 1.0, fm.horizontalAdvance(text) + 6.0, fm.height() + 1.0);
                p.fillRect(box, QColor(30, 30, 30, 190));
                p.setFont(smallFont);
                p.setPen(c.lighter(130));
                p.drawText(box.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
            }
        }
    if (opt.showEventLength && v.width() > 40 && eventTitleH(r) <= 0) {
        const QFontMetrics fm(smallFont);
        const QString text = QString::fromStdString(timecode(e.length, fps()));
        const double w = fm.horizontalAdvance(text) + 6.0;
        double right = std::min(r.right(), v.right()) - 2.0;
        if (opt.showEventButtons) {
            const Track* et = nullptr;
            pr.findEvent(e.id, &et);
            if (hasCropButton(r, et && et->type == TrackType::Video)) right = std::min(right, cropButtonRect(r).left() - 3.0);
            else if (hasFxButton(r)) right = std::min(right, fxButtonRect(r).left() - 3.0);
            else if (hasMenuButton(r)) right = std::min(right, menuButtonRect(r).left() - 3.0);
        }
        if (w < right - std::max(r.left(), v.left()) - 20.0) {
            const QRectF box(right - w, v.bottom() - fm.height() - 4.0, w, fm.height() + 2.0);
            p.fillRect(box, QColor(0, 0, 0, 150));
            p.setFont(smallFont);
            p.setPen(QColor(240, 240, 240));
            p.drawText(box, Qt::AlignCenter, text);
        }
    }
    if (opt.showTakeInfo && v.width() > 30 && e.group) {
        // VEGAS's out-of-sync mark: an event of the same file as the video or audio grouped with it but not in step with
        // it: a red bar along the top and how far off it is
        if (const uint64_t partner = syncPartner(e.id))
            if (const Event* o = pr.findEvent(partner)) {
                const double drift = ((e.offset - e.start * e.rate) - (o->offset - o->start * o->rate)) / std::max(1e-9, e.rate);
                p.fillRect(QRectF(v.left(), v.top(), v.width(), 3.0), QColor(230, 40, 40));
                const QString text = QString(drift > 0 ? "-" : "+") + QString::fromStdString(timecode(std::fabs(drift), fps()));
                const QFontMetrics fm(smallFont);
                const QRectF box(std::max(r.left(), (double)headerW) + 3.0, v.top() + 4.0, fm.horizontalAdvance(text) + 6.0, fm.height() + 1.0);
                p.fillRect(box, QColor(150, 20, 20, 200));
                p.setFont(smallFont);
                p.setPen(QColor(255, 235, 235));
                p.drawText(box, Qt::AlignCenter, text);
            }
    }
    if (opt.showTakeInfo && v.width() > 50) {
        const int n = (int)e.takes.size() + 1;
        QString text = QString::fromStdString(e.name);
        if (n > 1) text = QString("Take %1/%2: ").arg(std::min(e.activeTake, n - 1) + 1).arg(n) + text;
        if (std::fabs(e.rate - 1.0) > 1e-6) text += QString(" (%1%)").arg(e.rate * 100.0, 0, 'f', 1);
        const QFontMetrics fm(smallFont);
        const double w = std::min(v.width() - 6.0, (double)fm.horizontalAdvance(text) + 6.0);
        const QRectF box(std::max(r.left(), (double)headerW) + (e.group ? 18.0 : 3.0), v.bottom() - fm.height() - 4.0, w, fm.height() + 2.0);
        p.fillRect(box, QColor(0, 0, 0, 150));
        p.setFont(smallFont);
        p.setPen(QColor(240, 240, 240));
        p.drawText(box.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text, Qt::ElideRight, (int)box.width() - 5));
    }
    if (snapFlag) {
        const double fx = std::floor(xAtF(e.start + clampd(e.snapOffset, 0.0, e.length))) + 0.5;
        QPolygonF tri;
        tri << QPointF(fx, r.bottom() - 7.0) << QPointF(fx + 6.0, r.bottom() - 1.0) << QPointF(fx, r.bottom() - 1.0);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(20, 20, 20), 1.0));
        p.setBrush(QColor(250, 250, 250));
        p.drawPolygon(tri);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    if (linked) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(178, 92, 255), 2.0));
        p.drawRect(v.adjusted(1, 1, -1, -1));
    }
    if (e.locked && v.width() > 16) {
        p.setRenderHint(QPainter::Antialiasing, true);
        const double x = std::min(r.right(), v.right()) - 13.0, y = v.top() + 3.0;
        p.setPen(QPen(QColor(20, 20, 20), 2.6));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(x + 2.0, y, 6.0, 8.0), 0, 180 * 16);
        p.setPen(QPen(QColor(255, 214, 90), 1.3));
        p.drawArc(QRectF(x + 2.0, y, 6.0, 8.0), 0, 180 * 16);
        p.setPen(QPen(QColor(20, 20, 20), 1.0));
        p.setBrush(QColor(255, 214, 90));
        p.drawRect(QRectF(x, y + 4.0, 10.0, 7.0));
    }
    if (edgeSel.event && (edgeSel.event == e.id || (edgeSel.expanded && edgeSel.other == e.id))) {
        std::vector<int> sides;
        if (!edgeSel.expanded) sides.push_back(edgeSel.side);
        else if (e.id == edgeSel.event && edgeSel.which <= 0) sides.push_back(1);
        else if (e.id == edgeSel.other && edgeSel.which >= 0) sides.push_back(-1);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QColor red(235, 40, 40);
        for (int side : sides) {
            const double x = side < 0 ? std::floor(r.left()) : std::floor(r.right()) - 3.0;
            p.fillRect(QRectF(x, v.top(), 3.0, v.height()), red);
            const double tip = side < 0 ? x + 3.0 : x - 5.0;
            p.fillRect(QRectF(tip, v.top(), 5.0, 3.0), red);
            p.fillRect(QRectF(tip, v.bottom() - 3.0, 5.0, 3.0), red);
        }
    }
    p.restore();
}

QColor TimelineWidget::Impl::markerColor(const Project::Marker& m)
{
    if (m.kind == "command") return QColor(196, 150, 236);
    if (m.kind == "cdtrack" || m.kind == "cdindex") return QColor(110, 196, 240);
    return m.region ? kRegionColor : kMarkerColor;
}

void TimelineWidget::Impl::drawCrossFade(QPainter& p, const Row& row, const Event& a, const Event& b, const QRect& c)
{
    const double x0 = xAtF(b.start), x1 = xAtF(a.end());
    if (x1 < c.left() || x0 > c.right() || x1 - x0 < 2.0) return;
    const QRectF rb = eventRect(row, b);
    const QRect lanes = lanesRect();
    const double l = std::max(x0, lanes.left() - 16.0), rr = std::min(x1, lanes.right() + 16.0);
    const bool transition = !b.transition.kind.empty();
    const bool target = drop.ok && drop.kind == DropKind::Transition && drop.event == b.id;
    p.save();
    if (target || transition) p.fillRect(QRectF(l, rb.top(), rr - l, rb.height()), target ? QColor(255, 196, 61, 90) : QColor(150, 96, 210, 70));
    p.setRenderHint(QPainter::Antialiasing, !dense);
    p.setPen(QPen(QColor(36, 44, 255), 1.4));
    const bool narrow = x1 - x0 < 12.0;
    if (narrow) p.setRenderHint(QPainter::Antialiasing, false);
    drawFade(p, x0, x1, rb.top() + 1, rb.bottom() - 1, narrow ? CurveLinear : (b.fadeInRecipCurve >= 0 ? b.fadeInRecipCurve : a.fadeOutCurve), false, l, rr, false);
    drawFade(p, x0, x1, rb.top() + 1, rb.bottom() - 1, narrow ? CurveLinear : b.fadeInCurve, true, l, rr, false);
    if (transition && x1 - x0 > 34.0) {
        const QFontMetrics fm(smallFont);
        const bool bad = !app.fx.error(b.transition.id).empty();
        const QString name = fm.elidedText(QString::fromStdString(b.transition.name), Qt::ElideRight, (int)(x1 - x0 - 12.0));
        const double w = fm.horizontalAdvance(name) + 10.0, h = fm.height() + 3.0;
        const QRectF pill((x0 + x1 - w) * 0.5, rb.center().y() - h * 0.5, w, h);
        p.setPen(Qt::NoPen);
        p.setBrush(bad ? QColor(150, 50, 44, 235) : QColor(16, 16, 18, 215));
        p.drawRoundedRect(pill, 3.0, 3.0);
        p.setFont(smallFont);
        p.setPen(QColor(244, 244, 244));
        p.drawText(pill, Qt::AlignCenter, name);
    }
    p.restore();
}

void TimelineWidget::Impl::paintGrid(QPainter& p, const QRect& c, int tracksBottom)
{
    if (tracksBottom <= kTopH) return;
    const double step = gridStep();
    QVector<QPointF> dots;
    for (int64_t k = std::max<int64_t>(0, (int64_t)std::floor(timeAtF(c.left()) / step));; ++k) {
        const double x = std::floor(xAtF((double)k * step));
        if (x > c.right()) break;
        if (x < c.left()) continue;
        for (int y = std::max(c.top(), kTopH) | 1; y < std::min(tracksBottom, c.bottom() + 1); y += 2) dots.push_back(QPointF(x + 0.5, y + 0.5));
    }
    p.setPen(QColor(118, 118, 118));
    p.drawPoints(dots.constData(), dots.size());
}

void TimelineWidget::Impl::paintLayerLanes(QPainter& p, const Row& r, int y, const QRect& c)
{
    const double hl = layerH(r);
    const double top = y + 1.0;
    for (int k = 1; k <= 2; ++k) p.fillRect(QRectF(c.left(), top + k * hl - 1.0, c.width(), 1.0), QColor(255, 255, 255, 38));
    for (const auto& t : r.lm->trans) {
        const double x0 = std::floor(xAtF(t.first)), x1 = std::floor(xAtF(t.second));
        if (x1 < c.left() - 2 || x0 > c.right() + 2) continue;
        p.fillRect(QRectF(x0, top + 2.0 * hl, std::max(1.0, x1 - x0), r.h - 2.0 - 2.0 * hl - 1.0), QColor(96, 140, 210, 170));
    }
    p.setFont(smallFont);
    p.setPen(QColor(255, 255, 255, 80));
    const char* names[3] = {"A roll", "B roll", "Transition roll"};
    for (int k = 0; k < 3; ++k)
        p.drawText(QRectF(c.left() + 4.0, top + k * hl, 110.0, k < 2 ? hl : r.h - 2.0 - 2.0 * hl), Qt::AlignLeft | Qt::AlignVCenter, names[k]);
}

void TimelineWidget::Impl::paintLanes(QPainter& p, const QRect& clip)
{
    const QRect c = lanesRect().intersected(clip);
    if (c.isEmpty()) return;
    p.save();
    p.setClipRect(c);
    p.fillRect(c, kBelowBg);
    const Project& pr = app.project();
    for (const Row& r : rows) {
        const int y = rowTop(r);
        if (y + r.h <= c.top() || y > c.bottom()) continue;
        p.fillRect(QRect(c.left(), y, c.width(), r.h - 1), kLane);
        p.fillRect(QRect(c.left(), y + r.h - 1, c.width(), 1), kLine);
        if (r.lm) paintLayerLanes(p, r, y, c);
    }
    for (const BusRow& b : busRows) {
        const int y = busTop(b);
        if (y + b.h <= c.top() || y > c.bottom()) continue;
        p.fillRect(QRect(c.left(), y, c.width(), b.h - 1), kBusLane);
        p.fillRect(QRect(c.left(), y + b.h - 1, c.width(), 1), kLine);
    }
    const int tracksBottom = kTopH + contentH - kBelow - scrollY;
    dense = countDense();
    paintGrid(p, c, tracksBottom);
    if (app.loopEnd > app.loopStart) {
        const double x0 = clampd(xAtF(app.loopStart), c.left() - 2.0, c.right() + 2.0), x1 = clampd(xAtF(app.loopEnd), c.left() - 2.0, c.right() + 2.0);
        p.fillRect(QRectF(x0, kTopH, x1 - x0, tracksBottom - kTopH), kSelectionWash);
    }
    for (const Row& r : rows) {
        const int y = rowTop(r);
        if (y + r.h <= c.top() || y > c.bottom()) continue;
        const Track& tr = pr.tracks[(size_t)r.index];
        const double from = timeAtF(c.left() - 20.0), to = timeAtF(c.right() + 20.0);
        const std::pair<size_t, size_t> range = eventRange((size_t)r.index, from, to);
        const RowLook look = rowLook(r);
        QRectF run;
        QColor runColor;
        bool inRun = false;
        for (size_t k = range.first; k < range.second; ++k) {
            const Event& e = tr.events[k];
            if (!(e.end() >= from && e.start <= to)) continue;
            QRectF sr;
            QColor sc;
            const int kind = sliverOf(r, e, c, look, &sr, &sc);
            if (kind == 0) continue;
            if (kind == 1) {
                if (inRun && sc == runColor && sr.left() <= run.right() && sr.top() == run.top() && sr.bottom() == run.bottom()) {
                    if (sr.right() > run.right()) run.setRight(sr.right());
                    continue;
                }
                if (inRun) p.fillRect(run, runColor);
                run = sr;
                runColor = sc;
                inRun = true;
                continue;
            }
            if (inRun) p.fillRect(run, runColor);
            inRun = false;
            drawEvent(p, r, e, c, look);
        }
        if (inRun) p.fillRect(run, runColor);
        for (size_t k = range.first; k < range.second; ++k) {
            const Event& e = tr.events[k];
            if (e.end() >= from && e.start <= to)
                if (pr.autoCrossfades || !e.transition.kind.empty())
                    // an X can be drawn only where the overlap is two pixels wide, and the overlap is inside the event:
                    // narrower events needn't be asked what they fade from
                    if (e.length * pps >= 1.99)
                        if (const Event* a = fadeSource((size_t)r.index, k)) drawCrossFade(p, r, *a, e, c);
        }
        if (tr.mute || (pr.anySolo(tr.type) && !tr.solo)) p.fillRect(QRect(c.left(), y, c.width(), r.h - 1), QColor(30, 30, 30, 140));
        paintTrackKeys(p, r, tr, c);
    }
    for (const EnvLane& L : envLanes()) drawEnvelope(p, L, c);
    if (app.loopEnd > app.loopStart) {
        const QColor edge = app.loopOn ? QColor(kLoopOn.red(), kLoopOn.green(), kLoopOn.blue(), 150) : QColor(255, 255, 255, 90);
        p.fillRect(QRectF(std::floor(xAtF(app.loopStart)), kTopH, 1.0, tracksBottom - kTopH), edge);
        p.fillRect(QRectF(std::floor(xAtF(app.loopEnd)), kTopH, 1.0, tracksBottom - kTopH), edge);
    }
    // markers and regions: dotted lines down through the tracks (Preferences > General "Always draw marker lines";
    // cleared, only while snapping is on)
    for (const auto& m : pr.markers) {
        if (!vprefs::on("prefs/alwaysMarkerLines") && !app.snapping) break;
        const QColor col = markerColor(m);
        QPen pen(QColor(col.red(), col.green(), col.blue(), 190), 1.0, Qt::DotLine);
        p.setPen(pen);
        for (double t : {m.time, m.time + m.length}) {
            const double x = std::floor(xAtF(t)) + 0.5;
            if (x >= c.left() - 1 && x <= c.right() + 1) p.drawLine(QPointF(x, kTopH), QPointF(x, tracksBottom));
            if (!m.region) break;
        }
    }
    if (rows.empty() && drop.kind == DropKind::None) {
        p.setFont(q->font());
        p.setPen(kDimText);
        p.drawText(lanesRect().adjusted(20, 20, -20, -20), Qt::AlignCenter, "Drag media here to start, or right-click to insert a track.");
    }
    paintDrop(p, c, tracksBottom);
    if (snapLine >= 0) {
        QColor sc = kSnapLine;
        for (const QString& x : vprefs::list("prefs/snapColors"))
            if (x.startsWith("indicator=") && QColor(x.mid(10)).isValid()) sc = QColor(x.mid(10));
        if (opt.quantize && std::fabs(app.project().quantize(snapLine) - snapLine) > 1e-6) {
            p.setPen(QPen(sc, 1.0, Qt::DashLine));
            const double x = std::floor(xAtF(snapLine)) + 0.5;
            p.drawLine(QPointF(x, c.top()), QPointF(x, c.bottom()));
        } else {
            p.fillRect(QRectF(std::floor(xAtF(snapLine)), c.top(), 1.0, c.height()), sc);
        }
    }
    drawEnvelopeLabel(p);
    if ((drag.kind == Drag::RubberBand || drag.kind == Drag::EnvBand) && drag.started) {
        p.setPen(QPen(QColor(255, 255, 255, 220), 1.0, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 30));
        p.drawRect(QRectF(drag.pressPos, drag.lastPos).normalized());
    } else if (drag.kind == Drag::ZoomRange && drag.started) {
        QRectF z = QRectF(drag.pressPos, drag.lastPos).normalized();
        if (drag.bandMode == 1) { z.setTop(kTopH); z.setBottom(tracksBottom); }
        else if (drag.bandMode == 2) { z.setLeft(headerW); z.setRight(lanesRight()); }
        p.fillRect(z, QColor(70, 170, 255, 50));
        p.setPen(QPen(QColor(255, 255, 255, 220), 1.0, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(z);
    }
    p.restore();
}

void TimelineWidget::Impl::paintDrop(QPainter& p, const QRect& c, int tracksBottom)
{
    if (drop.kind == DropKind::None || !drop.ok) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPen dashed(kDrop, 1.6, Qt::DashLine);
    const QColor wash(255, 196, 61, 44);
    for (const Ghost& g : drop.ghosts) {
        const double x = xAtF(g.start), w = std::max(3.0, g.length * pps);
        QRectF r;
        QString label;
        if (const Row* row = rowOf(g.track)) r = QRectF(x, rowTop(*row) + 1.0, w, row->h - 3.0);
        else if (g.video) {
            r = QRectF(x, kTopH + 2.0, w, 16.0);
            label = "New Video Track";
        } else {
            r = QRectF(x, tracksBottom + 3.0, w, kBelow - 8.0);
            label = "New Audio Track";
        }
        r = QRectF(QPointF(std::max(r.left(), c.left() - 8.0), r.top()), QPointF(std::min(r.right(), c.right() + 8.0), r.bottom()));
        if (r.width() <= 0) continue;
        p.setPen(dashed);
        p.setBrush(wash);
        p.drawRect(r);
        if (!label.isEmpty() && r.width() > 110) {
            p.setFont(smallFont);
            p.setPen(QColor(250, 240, 210));
            p.drawText(r.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
        }
    }
    if (drop.kind == DropKind::Files) {
        p.setPen(dashed);
        const double x = std::floor(xAtF(drop.time)) + 0.5;
        p.drawLine(QPointF(x, c.top()), QPointF(x, c.bottom()));
    }
    if (drop.event && drop.kind == DropKind::VideoFx) {
        const Track* tr = nullptr;
        if (const Event* e = app.project().findEvent(drop.event, &tr))
            if (const Row* row = rowOf(tr->id)) {
                p.setPen(QPen(kDrop, 2.0));
                p.setBrush(wash);
                p.drawRect(eventRect(*row, *e).adjusted(1, 1, -1, -1));
            }
    }
    if (!drop.event && drop.track && (drop.kind == DropKind::AudioFx || drop.kind == DropKind::VideoFx)) {
        if (const Row* row = rowOf(drop.track)) {
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(QPen(kDrop, 2.0));
            p.setBrush(QColor(255, 196, 61, 20));
            p.drawRect(QRect(c.left() + 1, rowTop(*row) + 1, c.width() - 2, row->h - 3));
        }
    }
    p.restore();
}

}  // namespace app
}  // namespace reno
