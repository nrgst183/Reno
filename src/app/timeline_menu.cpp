// Right-click menus of the timeline.
#include "recorder.h"
#include "timeline_internal.h"

namespace reno {
namespace app {

QAction* TimelineWidget::Impl::add(QMenu& menu, const QString& text, std::function<void()> fn, bool enabled)
{
    QAction* a = menu.addAction(text);
    a->setEnabled(enabled);
    QObject::connect(a, &QAction::triggered, &menu, [fn] { fn(); });
    return a;
}

void TimelineWidget::Impl::insertTrack(TrackType type, int row)
{
    int index = -1;
    if (row >= 0 && (size_t)row < rows.size() && rows[(size_t)row].video == (type == TrackType::Video)) index = rows[(size_t)row].index;
    app.currentTrack = app.addTrack(type, index);
    app.notify(kSelection);
}

void TimelineWidget::Impl::contextMenu(QContextMenuEvent* ev)
{
    if (drag.kind != Drag::None) return;
    if (swallowRight || groupBypass) return;
    // a right-drag (or the right click it turned out to be) has already shown its menu; Windows sends this after the
    // button is released
    if (ev->reason() == QContextMenuEvent::Mouse && QDateTime::currentMSecsSinceEpoch() - rightMenuAt < 400) return;
    if (ev->reason() == QContextMenuEvent::Keyboard) {
        for (const Row& r : rows) {
            const Track* tr = trackOf(r);
            if (!tr) continue;
            for (const Event& e : tr->events)
                if (app.selection().count(e.id)) {
                    const QRectF er = eventRect(r, e);
                    Hit h;
                    h.part = Part::EventBody;
                    h.track = r.id;
                    h.event = e.id;
                    const QPointF at(clampd(er.left() + 8.0, headerW, lanesRight() - 8.0), er.center().y());
                    if (showMenu(h, q->mapToGlobal(at.toPoint()))) ev->accept();
                    return;
                }
        }
        if (const Row* r = rowOf(app.currentTrack)) {
            Hit h;
            h.part = Part::HeaderBody;
            h.track = r->id;
            if (showMenu(h, q->mapToGlobal(headerRects(*r).menu.bottomLeft()))) ev->accept();
        }
        return;
    }
    if (showMenu(hitTest(QPointF(ev->pos())), ev->globalPos(), timeAtF(ev->pos().x()))) ev->accept();
}

std::map<double, int> TimelineWidget::Impl::trackKeyTimes(const Track& t) const
{
    std::map<double, int> out;
    const double tol = 0.5 / std::max(1.0, app.project().settings.fps());
    auto add = [&](double time, int curve) {
        for (const auto& kv : out)
            if (std::fabs(kv.first - time) <= tol) return;
        out[time] = curve;
    };
    for (const MotionKey& k : t.motion) add(k.time, k.curve);
    for (const MotionKey& k : t.parentMotion) add(k.time, k.curve);
    for (const GlowKey& k : t.shadow2d) add(k.time, k.curve);
    for (const GlowKey& k : t.glow2d) add(k.time, k.curve);
    return out;
}

void TimelineWidget::Impl::paintTrackKeys(QPainter& p, const Row& r, const Track& t, const QRect& c)
{
    if (!showsTrackKeys(r, t)) return;
    const double y = trackKeyY(r);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRectF(c.left(), y - kTrackKeyBand / 2.0, c.width(), kTrackKeyBand), QColor(0, 0, 0, 70));
    for (const auto& kv : trackKeyTimes(t)) {
        const double x = xAtF(kv.first);
        if (x < c.left() - 8 || x > c.right() + 8) continue;
        QColor col(200, 200, 204);
        switch (kv.second) {
        case KeyFast: col = QColor(184, 255, 184); break;
        case KeySlow: col = QColor(204, 196, 128); break;
        case KeySmooth: col = QColor(196, 196, 255); break;
        case KeySharp: col = QColor(240, 136, 204); break;
        case KeyHold: col = QColor(160, 0, 0); break;
        default: break;
        }
        const bool held = drag.kind == Drag::TrackKeys && drag.hit.track == r.id && std::fabs(drag.hit.keyTime - kv.first) < 1e-9;
        const double k = 4.5;
        const QPointF d[4] = {QPointF(x, y - k), QPointF(x + k, y), QPointF(x, y + k), QPointF(x - k, y)};
        p.setPen(QPen(held ? QColor(255, 255, 255) : QColor(20, 20, 20), 1));
        p.setBrush(col);
        p.drawPolygon(d, 4);
    }
    p.restore();
}

bool TimelineWidget::Impl::hitTrackKey(const QPointF& pos, const Row& r, const Track& t, Hit& h) const
{
    if (!showsTrackKeys(r, t) || std::fabs(pos.y() - trackKeyY(r)) > 6.0) return false;
    double best = 7.0, at = -1;
    for (const auto& kv : trackKeyTimes(t)) {
        const double d = std::fabs(xAtF(kv.first) - pos.x());
        if (d < best) { best = d; at = kv.first; }
    }
    if (at < 0) return false;
    h.part = Part::TrackKey;
    h.keyTime = at;
    return true;
}

// A track keyframe drag: every kind of keyframe at the time held goes to the time under the mouse (to a frame, never
// onto another keyframe of the same kind).
void TimelineWidget::Impl::dragTrackKeys(const QPointF& pos)
{
    const uint64_t id = drag.hit.track;
    const double from = drag.hit.keyTime;
    const Project& pr = app.project();
    double to = std::max(0.0, pr.quantize(timeAtF(pos.x())));
    const double tol = 0.5 / std::max(1.0, pr.settings.fps());
    app.dragUpdate([&](Project& p) {
        Track* t = p.findTrack(id);
        if (!t) return;
        auto move = [&](auto& keys) {
            for (const auto& k : keys)
                if (std::fabs(k.time - from) > tol && std::fabs(k.time - to) <= tol) return;
            for (auto& k : keys)
                if (std::fabs(k.time - from) <= tol) k.time = to;
            std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        };
        move(t->motion);
        move(t->parentMotion);
        move(t->shadow2d);
        move(t->glow2d);
    });
    app.status("Track keyframe: " + timecode(to, pr.settings.fps()));
}

void TimelineWidget::Impl::trackKeyMenu(QMenu& menu, const Hit& h)
{
    const uint64_t id = h.track;
    const double at = h.keyTime;
    const double tol = 0.5 / std::max(1.0, app.project().settings.fps());
    auto each = [id, at, tol](Project& p, const std::function<void(int& curve)>& fn, bool erase) {
        Track* t = p.findTrack(id);
        if (!t) return;
        auto go = [&](auto& keys) {
            if (erase) {
                keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const auto& k) { return std::fabs(k.time - at) <= tol; }), keys.end());
                return;
            }
            for (auto& k : keys)
                if (std::fabs(k.time - at) <= tol) fn(k.curve);
        };
        go(t->motion);
        go(t->parentMotion);
        go(t->shadow2d);
        go(t->glow2d);
    };
    int cur = -1;
    if (const Track* t = const_cast<Project&>(app.project()).findTrack(id)) {
        const auto times = trackKeyTimes(*t);
        for (const auto& kv : times)
            if (std::fabs(kv.first - at) <= tol) cur = kv.second;
    }
    const std::pair<const char*, int> types[] = {{"Hold", KeyHold}, {"Linear", KeyLinear}, {"Fast", KeyFast}, {"Slow", KeySlow}, {"Smooth", KeySmooth}, {"Sharp", KeySharp}};
    for (const auto& ty : types) {
        QAction* a = menu.addAction(ty.first);
        a->setCheckable(true);
        a->setChecked(cur == ty.second);
        const int c = ty.second;
        QObject::connect(a, &QAction::triggered, &menu, [this, each, c] {
            app.edit("Keyframe Interpolation", [&](Project& p) { each(p, [c](int& curve) { curve = c; }, false); });
        });
    }
    menu.addSeparator();
    QObject::connect(menu.addAction("Delete"), &QAction::triggered, &menu, [this, each] {
        app.edit("Delete Track Keyframe", [&](Project& p) { each(p, [](int&) {}, true); });
    });
    QObject::connect(menu.addAction("Track Motion..."), &QAction::triggered, &menu, [this, id, at] {
        app.setCursor(at);
        if (q->onTrackMotion) q->onTrackMotion(id);
    });
}

void TimelineWidget::Impl::addEnvelopeChoices(QMenu* sub, const EnvelopeOwner& o)
{
    const std::vector<EnvelopeChoice> cs = app.envelopeChoices(o);
    QMenu* fxMenu = nullptr;
    bool sends = false;
    for (const EnvelopeChoice& c : cs) {
        QMenu* into = sub;
        if (c.ref.kind == "fx") {
            if (!fxMenu) {
                sub->addSeparator();
                fxMenu = sub->addMenu("FX Automation");
            }
            into = fxMenu;
        } else if (c.ref.kind == "send" && !sends) {
            sends = true;
            sub->addSeparator();
        }
        const bool has = AppState::findEnvelope(app.project(), c.ref) != nullptr;
        QAction* a = into->addAction(QString::fromStdString(c.name));
        a->setCheckable(true);
        a->setChecked(has);
        const EnvelopeRef ref = c.ref;
        QObject::connect(a, &QAction::triggered, sub, [this, ref, has] {
            if (has) app.removeEnvelope(ref);
            else {
                app.insertEnvelope(ref);
                frontEnv = ref;
            }
            q->update();
        });
    }
    if (cs.empty()) sub->addAction("(none)")->setEnabled(false);
}

void TimelineWidget::Impl::envelopeMenu(QMenu& menu, const Hit& h, double t)
{
    const EnvelopeRef ref = h.env;
    const Envelope* e = AppState::findEnvelope(app.project(), ref);
    if (!e) return;
    frontEnv = ref;
    // what it acts on: the selected points if the one clicked is among them, else the one clicked (on a line, the point
    // the segment starts from)
    std::set<int> pts;
    if (h.part == Part::EnvPoint) {
        if (selEnv == ref && selPoints.count(h.point)) pts = selPoints;
        else {
            pts = {h.point};
            selEnv = ref;
            selPoints = pts;
        }
    } else {
        pts = {std::max(0, h.point)};
    }
    const int n = (int)e->points.size();
    auto change = [this, ref](const std::string& what, std::function<void(Envelope&)> fn) {
        app.edit(what, [&](Project& p) { if (Envelope* x = AppState::findEnvelope(p, ref)) fn(*x); });
    };
    menu.addSection(envelopeName(ref.kind) + (ref.kind == "fx" ? QString(": %1").arg(QString::fromStdString(ref.param)) : QString()));
    if (ref.kind != "mute") {
        const int now = e->points[(size_t)std::min(*pts.begin(), n - 1)].curve;
        for (int c = 0; c <= CurveHold; ++c) {
            QAction* a = menu.addAction(curveIcon(c, false), c == CurveHold ? QString(kCurveNames[c]) : QString("%1 Fade").arg(kCurveNames[c]));
            a->setCheckable(true);
            a->setChecked(c == now);
            QObject::connect(a, &QAction::triggered, &menu, [change, pts, c] {
                change("Envelope Curve", [&](Envelope& x) { for (int i : pts) if (i < (int)x.points.size()) x.points[(size_t)i].curve = c; });
            });
        }
        menu.addSeparator();
    }
    if (h.part == Part::EnvLine) add(menu, "Add Point", [this, ref, t] { addEnvelopePoint(ref, t, Qt::NoModifier); });
    if (h.part == Part::EnvPoint)
        add(menu, "Delete", [this, ref, pts] {
            selEnv = ref;
            selPoints = pts;
            deleteEnvelopePoints();
        }, (int)pts.size() < n);
    QMenu* setTo = ref.kind == "velocity" ? &menu : menu.addMenu("Set To");
    for (const auto& pr : envPresets(ref.kind)) {
        const double v = pr.second;
        QObject::connect(setTo->addAction(pr.first), &QAction::triggered, &menu, [change, pts, v] {
            change("Set Envelope Points", [&](Envelope& x) { for (int i : pts) if (i < (int)x.points.size()) x.points[(size_t)i].value = v; });
        });
    }
    add(menu, "Reset All", [change, ref] {
        const double d = AppState::envelopeDefault(ref.kind);
        change("Reset Envelope", [&](Envelope& x) { for (EnvPoint& pt : x.points) pt.value = d; });
    });
    add(menu, "Flip All Points", [change, ref] {
        const std::string kind = ref.kind;
        change("Flip Envelope", [&](Envelope& x) { for (EnvPoint& pt : x.points) pt.value = fracToEnv(kind, 1.0 - envToFrac(kind, pt.value)); });
    });
    const bool inSel = !envCommandPoints(ref).empty() && app.loopEnd > app.loopStart;
    add(menu, "Flip Selected Points", [this, ref] { flipEnvelopePoints(ref); }, inSel && ref.kind != "mute");
    add(menu, "Thin All Points", [this, ref] { thinEnvelope(ref, false); }, n > 2 && ref.kind != "mute");
    add(menu, "Thin Selected Points", [this, ref] { thinEnvelope(ref, true); }, inSel && envCommandPoints(ref).size() > 2 && ref.kind != "mute");
    if (ref.kind == "fadecolor") {
        QMenu* colors = menu.addMenu("Set Colors");
        for (int top = 1; top >= 0; --top)
            add(*colors, top ? "Top Color..." : "Bottom Color...", [this, ref, top] { pickFadeColor(ref.owner, top != 0); });
    }
    menu.addSeparator();
    add(menu, "Hide", [this, ref] { app.showEnvelope(ref, false); });
    add(menu, "Remove Envelope", [this, ref] { app.removeEnvelope(ref); });
}

void TimelineWidget::Impl::pickFadeColor(const EnvelopeOwner& o, bool top)
{
    const Project& pr = app.project();
    uint32_t now = 0;
    if (o.kind == EnvelopeOwner::Track) {
        for (const Track& t : pr.tracks) if (t.id == o.id) now = top ? t.fadeTop : t.fadeBottom;
    } else {
        now = top ? pr.videoFadeTop : pr.videoFadeBottom;
    }
    const QColor c = QColorDialog::getColor(QColor((int)((now >> 16) & 0xff), (int)((now >> 8) & 0xff), (int)(now & 0xff)), q,
                                            top ? "Fade to Color: Top Color" : "Fade to Color: Bottom Color");
    if (!c.isValid()) return;
    const uint32_t v = 0xFF000000u | ((uint32_t)c.red() << 16) | ((uint32_t)c.green() << 8) | (uint32_t)c.blue();
    app.edit("Fade Colors", [&](Project& p) {
        if (o.kind == EnvelopeOwner::Track) {
            if (Track* t = p.findTrack(o.id)) (top ? t->fadeTop : t->fadeBottom) = v;
        } else {
            (top ? p.videoFadeTop : p.videoFadeBottom) = v;
        }
    });
}

// The other effects windows an event can have, as in VEGAS: its media's generator (the Media Generator window) and,
// where it overlaps the event before it, the transition there (the Transition window).
void TimelineWidget::Impl::fxWindowsMenu(QMenu& menu, uint64_t id)
{
    const Project& p = app.project();
    const Track* tr = nullptr;
    const Event* e = p.findEvent(id, &tr);
    if (!e || !tr || tr->type != TrackType::Video) return;
    if (const MediaItem* m = p.findMedia(e->mediaId))
        if (m->generated) {
            const uint64_t media = m->id;
            add(menu, "Media Generator...", [this, media] {
                FxTarget t;
                t.kind = FxTarget::Generator;
                t.id = media;
                if (q->onOpenFx) q->onOpenFx(t, 0);
            });
        }
    const Event* before = neighbourOf(*tr, *e, -1, 1.0 / std::max(1.0, fps()));
    if (before && before->end() > e->start + 1e-9) {
        add(menu, e->transition.kind.empty() ? "Transition (Cross-Fade)..." : "Transition Properties...", [this, id] {
            FxTarget t;
            t.kind = FxTarget::Transition;
            t.id = id;
            if (q->onOpenFx) q->onOpenFx(t, 0);
        });
    }
}

QAction* TimelineWidget::Impl::command(QMenu& menu, const char* name, const QString& text)
{
    QAction* a = q->commandAction ? q->commandAction(name) : nullptr;
    if (!a) return nullptr;
    if (text.isEmpty()) {
        menu.addAction(a);
        return a;
    }
    // under the context menu's own name; does what the main window's command does. Key: the action's own, or the one its
    // text names after a tab
    const QStringList parts = a->text().split('\t');
    const QString key = !a->shortcut().isEmpty() ? a->shortcut().toString(QKeySequence::NativeText) : (parts.size() > 1 ? parts[1] : QString());
    QAction* b = menu.addAction(text + (key.isEmpty() ? QString() : "\t" + key));
    b->setEnabled(a->isEnabled());
    b->setCheckable(a->isCheckable());
    b->setChecked(a->isChecked());
    QObject::connect(b, &QAction::triggered, &menu, [a] { a->trigger(); });
    return b;
}

QAction* TimelineWidget::Impl::toggleItem(QMenu& menu, const QString& text, bool on, std::function<void()> fn, bool enabled)
{
    QAction* a = add(menu, text, std::move(fn), enabled);
    a->setCheckable(true);
    a->setChecked(on);
    return a;
}

// A bus track's menu (VEGAS's POPUP_IDX_BUSTRACK_HEADER / VIDEOBUSTRACK_HEADER): its envelopes, switches, the video
// output's fade colors; and (Reno) its effects.
void TimelineWidget::Impl::busMenu(QMenu& menu, const EnvelopeOwner& o)
{
    addEnvelopeChoices(menu.addMenu("Insert/Remove Envelope"), o);
    menu.addSeparator();
    if (o.kind == EnvelopeOwner::Bus) {
        if (const Bus* b = app.project().findBus(o.id)) {
            QMenu* sw = menu.addMenu("Switches");
            const bool mute = b->mute, solo = b->solo;
            const uint64_t id = o.id;
            toggleItem(*sw, "Mute", mute, [this, id, mute] { app.edit(mute ? "Unmute Bus" : "Mute Bus", [&](Project& p) { if (Bus* x = p.findBus(id)) x->mute = !mute; }); });
            toggleItem(*sw, "Solo", solo, [this, id, solo] { app.edit("Solo Bus", [&](Project& p) { if (Bus* x = p.findBus(id)) x->solo = !solo; }); });
        }
    } else if (o.kind == EnvelopeOwner::Master) {
        QMenu* sw = menu.addMenu("Switches");
        const bool mute = app.project().masterMute;
        toggleItem(*sw, "Mute", mute, [this, mute] { app.edit(mute ? "Unmute Master" : "Mute Master", [&](Project& p) { p.masterMute = !mute; }); });
    } else if (o.kind == EnvelopeOwner::VideoOut) {
        QMenu* colors = menu.addMenu("Fade Colors");
        add(*colors, "Top...", [this, o] { pickFadeColor(o, true); });
        add(*colors, "Bottom...", [this, o] { pickFadeColor(o, false); });
    }
    if (!q->onOpenFx) return;
    FxTarget t;
    if (o.kind == EnvelopeOwner::Master) t.kind = FxTarget::AudioOut;
    else if (o.kind == EnvelopeOwner::VideoOut) t.kind = FxTarget::VideoOut;
    else {
        t.kind = FxTarget::Bus;
        t.id = o.id;
    }
    menu.addSeparator();
    add(menu, o.kind == EnvelopeOwner::VideoOut ? "Video Output FX..." : (o.kind == EnvelopeOwner::Master ? "Master Bus FX..." : "Bus FX..."), [this, t] {
        if (q->onOpenFx) q->onOpenFx(t, 0);
    });
}

// The menu for what was right-clicked (or for the track or event whose menu button was clicked). False if there is none
// for that place.
bool TimelineWidget::Impl::showMenu(const Hit& h, const QPoint& globalPos, double time)
{
    QMenu menu(q);
    if (!fillMenu(menu, h, time)) return false;
    q->update();
    menu.exec(globalPos);
    return true;
}

bool TimelineWidget::Impl::fillMenu(QMenu& menu, const Hit& hitIn, double time)
{
    Hit h = hitIn;
    if (h.part == Part::HeaderEdge) h.part = Part::HeaderBody;
    if (h.track) setCurrentTrack(h.track);
    if (q->onBeforeMenu) q->onBeforeMenu();
    if (h.onEnvelope()) {
        envelopeMenu(menu, h, time >= 0 ? time : app.cursor());
    } else if (h.part == Part::BusHeader || h.part == Part::BusLane) {
        busMenu(menu, h.bus);
    } else if (h.onEvent()) {
        if (!app.selection().count(h.event)) selectGroupOf(h.event);
        if (!eventMenu(menu, h, time)) return false;
    } else if (h.part == Part::HeaderPan && h.track && panTypeMenu(menu, h.track)) {
    } else if (h.part == Part::HeaderBus && h.track) {
        busChoiceMenu(menu, h.track);
    } else if (h.part == Part::TrackKey && h.track) {
        trackKeyMenu(menu, h);
    } else if (h.part == Part::HeaderFx && h.track) {
        FxTarget t;
        t.kind = FxTarget::Track;
        t.id = h.track;
        fillChainMenu(&menu, t);
    } else if (h.onHeader() && h.part != Part::HeaderGap && h.track) {
        trackMenu(menu, h, false, time);
    } else if (h.part == Part::Lane && h.track) {
        trackMenu(menu, h, true, time);
    } else if ((h.onHeader() && h.part != Part::HeaderGap) || h.part == Part::Lane) {
        const int row = h.row;
        command(menu, "open", "Open...");
        add(menu, "Insert Audio Track\tCtrl+Q", [this, row] { insertTrack(TrackType::Audio, row); });
        add(menu, "Insert Video Track\tCtrl+Shift+Q", [this, row] { insertTrack(TrackType::Video, row); });
        if (q->commandAction && q->commandAction("preferences")) {
            menu.addSeparator();
            command(menu, "preferences", "Preferences...");
        }
    } else if (h.part == Part::MarkerTag || h.part == Part::RegionEndTag) {
        if (!markerMenu(menu, h)) return false;
    } else if (h.part == Part::LoopStrip) {
        add(menu, "Insert Region\tR", [this] { insertRegion(); }, app.loopEnd > app.loopStart);
        menu.addSeparator();
        command(menu, "loop", "Loop Playback");
        menu.addSeparator();
        add(menu, "Zoom Selection", [this] { zoomLarge(true); }, app.loopEnd > app.loopStart);
        add(menu, "Set Selection to View", [this] { q->setSelectionToView(); });
        add(menu, "Set Selection to Project", [this] { q->setSelectionToProject(); });
        add(menu, "Select Loop Region\tShift+Q", [this] { selectLoopRegion(); });
        menu.addSeparator();
        command(menu, "renderToNewTrack", "Render to New Track...");
        command(menu, "selectivePrerender", "Selectively Prerender Video...");
    } else if (h.part == Part::Corner) {
        timeDisplayMenu(menu);
    } else if (h.part == Part::MarkerBar) {
        command(menu, "loop", "Loop Playback");
        menu.addSeparator();
        add(menu, "Set Selection to View", [this] { q->setSelectionToView(); });
        add(menu, "Set Selection to Project", [this] { q->setSelectionToProject(); });
        add(menu, "Set Selection to All", [this] { q->setSelectionToProject(); });
        add(menu, "Select Loop Region\tShift+Q", [this] { selectLoopRegion(); });
        menu.addSeparator();
        QMenu* mr = menu.addMenu("Markers/Regions");
        add(*mr, "Insert Marker\tM", [this] { insertMarker(); });
        add(*mr, "Insert Region\tR", [this] { insertRegion(); }, app.loopEnd > app.loopStart);
        mr->addSeparator();
        add(*mr, "Delete All", [this] { deleteMarkers(false); }, !app.project().markers.empty());
        add(*mr, "Delete All in Selection", [this] { deleteMarkers(true); }, !app.project().markers.empty() && app.loopEnd > app.loopStart);
        menu.addSeparator();
        command(menu, "quantize", "Quantize to Frames");
        menu.addSeparator();
        command(menu, "snapping", "Enable Snapping");
        command(menu, "snapGrid", "Snap to Grid");
        command(menu, "snapMarkers", "Snap to Markers");
        command(menu, "snapAllEvents", "Snap to All Events");
        menu.addSeparator();
        gridSpacingItems(*menu.addMenu("Grid Spacing"));
        menu.addSeparator();
        command(menu, "selectivePrerender", "Selectively Prerender Video...");
        command(menu, "cleanPrerender", "Clean Up Prerendered Video...");
    } else if (h.part == Part::Ruler) {
        // VEGAS's Time Format menu (how the ruler, time display and readouts count; a project setting), and Set Time at
        // Cursor (moves the ruler's start so the cursor reads the time typed)
        QMenu* fmt = menu.addMenu("Time Format");
        timeFormatItems(*fmt, true, true);
    } else {
        return false;
    }
    return !menu.actions().isEmpty();
}

void TimelineWidget::Impl::freezeFrameAtCursor(uint64_t id)
{
    const Event* e = app.project().findEvent(id);
    if (!e) return;
    const double at = app.cursor() - e->start;
    if (at <= 1e-6 || at >= e->length - 1e-6) return;
    app.edit("Freeze Frame at Cursor", [&](Project& p) {
        Event* o = p.findEvent(id);
        if (!o) return;
        Envelope* v = findEnvelope(o->envelopes, "velocity");
        if (!v) {
            Envelope n;
            n.kind = "velocity";
            o->envelopes.push_back(n);
            v = &o->envelopes.back();
        }
        const double before = v->valueAt(at, 1.0);
        v->points.erase(std::remove_if(v->points.begin(), v->points.end(), [&](const EnvPoint& pt) { return pt.time >= at - 1e-9; }), v->points.end());
        if (v->points.empty()) v->points.push_back(EnvPoint{0.0, 1.0, CurveHold});
        else if (std::fabs(before - v->points.back().value) > 1e-9 || v->points.back().curve != CurveHold) {
            v->points.push_back(EnvPoint{std::max(v->points.back().time, at - 1.0 / std::max(1.0, p.settings.fps())), before, CurveHold});
        }
        v->points.push_back(EnvPoint{at, 0.0, CurveHold});
        v->visible = true;
    });
}

// The event grouped with 'id' that plays the same media file in the other kind of track and is out of step with it (0:
// none). Reversed events, and events at another rate or with a velocity envelope, are not compared.
uint64_t TimelineWidget::Impl::syncPartner(uint64_t id) const
{
    const Project& p = app.project();
    const Track* tr = nullptr;
    const Event* e = p.findEvent(id, &tr);
    if (!e || !tr || !e->group || e->reverse || findEnvelope(e->envelopes, "velocity")) return 0;
    const MediaItem* m = p.findMedia(e->mediaId);
    if (!m || m->path.empty()) return 0;
    for (const Track& t : p.tracks) {
        if (t.type == tr->type) continue;
        for (const Event& o : t.events) {
            if (o.group != e->group || o.reverse || std::fabs(o.rate - e->rate) > 1e-9 || findEnvelope(o.envelopes, "velocity")) continue;
            const MediaItem* om = p.findMedia(o.mediaId);
            if (!om || om->path != m->path) continue;
            const double drift = (e->offset - e->start * e->rate) - (o.offset - o.start * o.rate);
            if (std::fabs(drift) > 0.5 / std::max(1000.0, (double)p.settings.sampleRate)) return o.id;
        }
    }
    return 0;
}

void TimelineWidget::Impl::synchronize(uint64_t id, uint64_t partner, bool byMoving)
{
    if (!partner) return;
    app.edit(byMoving ? "Synchronize by Moving" : "Synchronize by Slipping", [&](Project& p) {
        Event* e = p.findEvent(id);
        const Event* o = p.findEvent(partner);
        if (!e || !o || e->rate <= 0) return;
        if (byMoving) e->start = std::max(0.0, o->start + (e->offset - o->offset) / e->rate);
        else e->offset = std::max(0.0, o->offset + (e->start - o->start) * e->rate);
    });
}

void TimelineWidget::Impl::timeFormatItems(QMenu& m, bool frameRates, bool setTime)
{
    if (frameRates) {
        add(m, "Use Media Frame Rate", [] {}, false)->setToolTip("The timeline counts frames at the project's frame rate");
        toggleItem(m, "Use Project Frame Rate", true, [] {}, false);
        m.addSeparator();
    }
    const std::string now = app.project().settings.rulerFormat;
    for (const TimeFormatInfo& f : timeFormats()) {
        const std::string id = f.id;
        toggleItem(m, QString::fromUtf8(f.menuText), now == id, [this, id] { setTimeFormat(id); });
    }
    if (setTime) {
        m.addSeparator();
        add(m, "Set Time at Cursor", [this] { QTimer::singleShot(0, q, [this] { q->setTimeAtCursor(); }); }, now != "cdtime");
    }
}

QString TimelineWidget::Impl::timeDisplayText() const
{
    const TimeDisplaySource src = timeDisplaySource();
    if (src != TimeDisplaySource::Cursor && midiTimeDisplayText()) return QString::fromStdString(midiTimeDisplayText()(src));
    return QString::fromStdString(rulerTime(app.project(), app.cursor()));
}

void TimelineWidget::Impl::timeDisplayMenu(QMenu& menu)
{
    // the time at the cursor, or the MIDI timecode coming in or going out, or the MIDI clock going out (from Options >
    // Timecode)
    const TimeDisplaySource now = timeDisplaySource();
    const bool midi = (bool)midiTimeDisplayText();
    const struct { const char* text; TimeDisplaySource src; } sources[] = {
        {"Time at Cursor", TimeDisplaySource::Cursor}, {"MIDI Timecode In", TimeDisplaySource::MtcIn},
        {"MIDI Timecode Out", TimeDisplaySource::MtcOut}, {"MIDI Clock Out", TimeDisplaySource::ClockOut}};
    for (const auto& s : sources) {
        const TimeDisplaySource src = s.src;
        QAction* a = toggleItem(menu, s.text, now == src, [this, src] {
            timeDisplaySource() = src;
            q->update(QRect(0, 0, headerW, kTopH));
        });
        if (a && src != TimeDisplaySource::Cursor && !midi) a->setEnabled(false);
    }
    menu.addSeparator();
    timeFormatItems(*menu.addMenu("Time Format"), false, true);
    menu.addSeparator();
    auto colorMenu = [this](QMenu* m, bool text) {
        add(*m, "Default", [this, text] { setTimeDisplayColor(text, QColor()); });
        add(*m, "Custom...", [this, text] {
            const QColor now = text ? (timeText.isValid() ? timeText : kTimeText) : (timeBack.isValid() ? timeBack : kPanel);
            const QColor c = QColorDialog::getColor(now, q, text ? "Text Color" : "Background Color");
            if (c.isValid()) setTimeDisplayColor(text, c);
        });
    };
    colorMenu(menu.addMenu("Text Color"), true);
    colorMenu(menu.addMenu("Background Color"), false);
}

void TimelineWidget::Impl::setTimeDisplayColor(bool text, const QColor& c)
{
    (text ? timeText : timeBack) = c;
    QSettings().setValue(text ? "timeline/timeDisplayText" : "timeline/timeDisplayBackground", c.isValid() ? c.name() : QString());
    q->update(QRect(0, 0, headerW, kTopH));
}

void TimelineWidget::Impl::gridSpacingItems(QMenu& m)
{
    const struct { const char* name; const char* text; } items[] = {
        {"gridRulerMarks", "Ruler Marks"}, {"-", ""}, {"gridMinutes", "Minutes"}, {"gridSeconds", "Seconds"},
        {"gridMilliseconds", "Milliseconds"}, {"gridFrames", "Frames"}, {"-", ""}, {"gridMeasures", "Measures"},
        {"gridQuarter", "Quarter Notes"}, {"gridQuarter3", "Quarter Note Triplets"}, {"grid8th", "8th Notes"},
        {"grid8th3", "8th Note Triplets"}, {"grid16th", "16th Notes"}, {"grid16th3", "16th Note Triplets"},
        {"grid32nd", "32nd Notes"}, {"grid32nd3", "32nd Note Triplets"}};
    const char* ids[] = {"ruler", "", "minutes", "seconds", "milliseconds", "frames", "", "measures", "quarter", "quarter3",
                         "8th", "8th3", "16th", "16th3", "32nd", "32nd3"};
    int k = 0;
    for (const auto& it : items) {
        const std::string id = ids[k++];
        if (it.name[0] == '-') { m.addSeparator(); continue; }
        if (command(m, it.name, it.text)) continue;
        toggleItem(m, it.text, opt.gridSpacing == id, [this, id] { opt.gridSpacing = id; q->optionsChanged(); });
    }
}

void TimelineWidget::Impl::deleteMarkers(bool inSelection)
{
    const double a = app.loopStart, b = app.loopEnd;
    app.edit(inSelection ? "Delete Markers in Selection" : "Delete All Markers", [&](Project& p) {
        p.markers.erase(std::remove_if(p.markers.begin(), p.markers.end(), [&](const Project::Marker& m) {
            return !inSelection || (m.time >= a - 1e-9 && m.time <= b + 1e-9);
        }), p.markers.end());
    });
}

bool TimelineWidget::Impl::markerMenu(QMenu& menu, const Hit& h)
{
    const uint64_t id = h.marker;
    const int k = markerIndex(id);
    if (k < 0) return false;
    const Project::Marker m = app.project().markers[(size_t)k];
    const bool endTag = h.part == Part::RegionEndTag;
    auto goTo = [this](double t) { userCursor(t); scrollToCursor(); };
    if (m.region) {
        if (endTag) {
            add(menu, "Go To", [goTo, m] { goTo(m.time + m.length); });
            add(menu, "Go to Start", [goTo, m] { goTo(m.time); });
        } else {
            add(menu, "Go To", [goTo, m] { goTo(m.time); });
            add(menu, "Go to End", [goTo, m] { goTo(m.time + m.length); });
        }
        add(menu, "Select Region", [this, m] { app.setLoop(m.time, m.time + m.length); });
    } else {
        if (m.kind == "command") {
            command(menu, "commandMarker", "Insert Command...");
            add(menu, "Edit...", [this, id] { QTimer::singleShot(0, q, [this, id] { editCommandMarker(id); }); });
        }
        add(menu, "Go To", [goTo, m] { goTo(m.time); });
    }
    if (!endTag) add(menu, "Rename", [this, id] { QTimer::singleShot(0, q, [this, id] { startRename(Naming::Marker, id); }); });
    menu.addSeparator();
    add(menu, "Delete", [this, id, m] {
        app.edit(m.region ? "Delete Region" : "Delete Marker", [&](Project& p) {
            p.markers.erase(std::remove_if(p.markers.begin(), p.markers.end(), [&](const Project::Marker& o) { return o.id == id; }), p.markers.end());
        });
    });
    return true;
}

// Auto Ducking (track list > More > Auto Ducking Controls): this track as the controller (its sound ducks the listeners)
// and as a listener.
void TimelineWidget::Impl::autoDuckingMenu(QMenu& menu, uint64_t id)
{
    const Track* t = nullptr;
    for (const Track& x : app.project().tracks) if (x.id == id) t = &x;
    if (!t) return;
    const bool controller = t->duckController, listener = t->duckListener;
    add(menu, controller ? "Auto Ducking Controls..." : "Auto Ducking Controller...", [this, id] { showAutoDuckingDialog(q, id); });
    toggleItem(menu, "Auto Ducking Listener", listener, [id, listener] { setAutoDuckingListener(id, !listener); });
    if (controller) {
        menu.addSeparator();
        add(menu, "Remove Auto Ducking Controller", [id] { removeAutoDuckingController(id); });
    }
}

// A track's header (lane false) or the empty part of its lane (lane true): VEGAS's POPUP_IDX_AUDIO/VIDEO_TRACK_HEADER
// and _NONDATA, then Reno's own (track height, compositing, Track Motion, Track FX). An audio track's pan slider
// offers VEGAS's pan types.
bool TimelineWidget::Impl::panTypeMenu(QMenu& menu, uint64_t id)
{
    const Track* t = nullptr;
    for (const auto& x : app.project().tracks) if (x.id == id) t = &x;
    if (!t || t->type != TrackType::Audio) return false;
    static const char* const kTypes[][2] = {{"add", "Add Channels (0 dB Center)"}, {"balance0", "Balance (0 dB Center)"},
                                            {"balance3", "Balance (-3 dB Center)"}, {"balance6", "Balance (-6 dB Center)"},
                                            {"power", "Constant Power"}, {"film", "Film"}};
    const std::string now = t->panType.empty() ? std::string("balance0") : t->panType;
    for (const auto& ty : kTypes) {
        const std::string key = ty[0];
        toggleItem(menu, ty[1], now == key, [this, id, key] {
            app.edit("Track Pan Type", [&](Project& p) { if (Track* o = p.findTrack(id)) o->panType = key == "balance0" ? std::string() : key; });
        });
    }
    return true;
}

void TimelineWidget::Impl::busChoiceMenu(QMenu& menu, uint64_t id)
{
    const Track* t = findTrackConst(id);
    if (!t || t->type != TrackType::Audio) return;
    const std::vector<uint64_t> ids = controlTracks(id);
    auto choose = [this, ids](uint64_t bus) {
        app.edit("Track Output", [&](Project& p) {
            for (uint64_t x : ids)
                if (Track* o = p.findTrack(x)) o->output = bus;
        });
    };
    toggleItem(menu, "Master", t->output == 0, [choose] { choose(0); });
    for (const reno::Bus& b : app.project().buses) {
        const uint64_t bus = b.id;
        toggleItem(menu, QString::fromStdString(b.name.empty() ? std::string("Bus") : b.name), t->output == bus, [choose, bus] { choose(bus); });
    }
    if (app.project().buses.empty()) {
        menu.addSeparator();
        add(menu, "(no buses: Insert > Audio Bus)", [] {}, false);
    }
}

void TimelineWidget::Impl::trackMenu(QMenu& menu, const Hit& h, bool lane, double time)
{
    const uint64_t id = h.track;
    const int row = h.row;
    const Row* r = rowOf(id);
    const Track* t = r ? trackOf(*r) : nullptr;
    if (!r || !t) return;
    const bool video = r->video;
    const bool hasSel = !app.selection().empty();
    if (lane) {
        if (time >= 0 && app.loopEnd > app.loopStart && time >= app.loopStart && time <= app.loopEnd) {
            add(menu, "Insert Empty Event", [this] { q->insertEmptyEvent(); });
            if (t && t->type == TrackType::Video) add(menu, "Insert Adjustment Event", [this] { q->insertAdjustmentEvent(); });
            add(menu, "Zoom Selection", [this] { zoomLarge(true); });
            menu.addSeparator();
        }
        add(menu, "Paste\tCtrl+V", [this] { q->paste(); }, app.hasClipboard());
        menu.addSeparator();
        add(menu, "Delete\tDelete", [this] { removeSelection(false); }, hasSel);
        add(menu, "Trim\tCtrl+T", [this] { q->trimToTimeSelection(); }, hasSel && app.loopEnd > app.loopStart);
        add(menu, "Trim Start\tAlt+[", [this] { q->trimStartToCursor(); }, hasSel);
        add(menu, "Trim End\tAlt+]", [this] { q->trimEndToCursor(); }, hasSel);
        add(menu, "Split\tS", [this] { app.splitAtCursor(); });
        add(menu, "Event Heal", [this] { app.healEvents(app.selection()); }, hasSel);
        add(menu, "Close Gaps", [this] { app.closeGaps(app.selection()); }, hasSel);
        menu.addSeparator();
        add(menu, "Select All After Cursor", [this, id] { q->selectAllAfterCursor(id); });
        menu.addSeparator();
    } else {
        add(menu, "Rename", [this, id] { QTimer::singleShot(0, q, [this, id] { startRename(Naming::Track, id); }); });
        menu.addSeparator();
    }
    if (video) add(menu, "Insert Video Track\tCtrl+Shift+Q", [this, row] { insertTrack(TrackType::Video, row); });
    else add(menu, "Insert Audio Track\tCtrl+Q", [this, row] { insertTrack(TrackType::Audio, row); });
    add(menu, "Duplicate Track", [this, id] { q->duplicateTrack(id); });
    add(menu, "Delete Track", [this, id] { if (confirmTrackDelete({id})) app.removeTrack(id); });
    if (video && !lane) add(menu, "Pair as Stereoscopic 3D Subclips", [] {}, false);
    menu.addSeparator();
    if (!lane) {
        add(menu, "Cut", [this] { copyTracks(true); });
        add(menu, "Copy", [this] { copyTracks(false); });
        add(menu, "Paste", [this, id] { pasteTracks(id); }, !trackClip.empty());
        add(menu, "Delete", [this] { if (confirmTrackDelete(selectedTracksInOrder())) deleteTracks(selectedTracksInOrder(), "Delete Tracks"); });
        menu.addSeparator();
    }
    if (!lane && !video && q->onArmTrack) {
        toggleItem(menu, "Arm for Record", app.armedTracks.count(id) != 0, [this, id] { if (q->onArmTrack) q->onArmTrack(id); });
    }
    if (!lane && !video) {
        QMenu* ri = menu.addMenu("Record Input");
        ri->setObjectName("recordInputMenu");
        fillRecordInputMenu(ri, t->recordInput, true, [this, id](const RecordInput& r) {
            app.edit("Record Input", [&](Project& p) { if (Track* o = p.findTrack(id)) o->recordInput = r; });
        });
    }
    if (!lane && !video) panTypeMenu(*menu.addMenu("Pan Type"), id);
    if (!lane && !video) autoDuckingMenu(*menu.addMenu("Auto Ducking"), id);
    {
        EnvelopeOwner o;
        o.kind = EnvelopeOwner::Track;
        o.id = id;
        addEnvelopeChoices(menu.addMenu("Insert/Remove Envelope"), o);
    }
    menu.addSeparator();
    if (!lane) {
        QMenu* sw = menu.addMenu("Switches");
        const bool muted = t->mute, soloed = t->solo;
        toggleItem(*sw, "Mute\tZ", muted, [this, id, muted] {
            app.edit(muted ? "Unmute Track" : "Mute Track", [&](Project& p) { if (Track* o = p.findTrack(id)) o->mute = !muted; });
        });
        toggleItem(*sw, "Solo\tX", soloed, [this, id, soloed] {
            app.edit("Solo Track", [&](Project& p) { if (Track* o = p.findTrack(id)) o->solo = !soloed; });
        });
    }
    if (video) {
        QMenu* colors = menu.addMenu("Fade Colors");
        add(*colors, "Top...", [this, id] { q->fadeColorDialog(id, true); });
        add(*colors, "Bottom...", [this, id] { q->fadeColorDialog(id, false); });
    }
    if (lane) return;
    if (video) {
        // VEGAS 18's Expand Track Layers: the track's events on an A roll, a B roll and a transition roll (again:
        // collapsed)
        const bool expanded = t->layersExpanded;
        add(menu, expanded ? "Collapse Track Layers" : "Expand Track Layers", [this, id, expanded] { setLayersExpanded(id, !expanded); });
    }
    menu.addSeparator();
    if (q->onTrackDefaults) q->onTrackDefaults(&menu, id);
    menu.addSeparator();
    {
        QMenu* tg = menu.addMenu("Track Group");
        const uint64_t g = t->trackGroup;
        add(*tg, "Group Selected Tracks", [this] { groupTracks(true); }, selectedTracks().size() > 1);
        add(*tg, "Ungroup Selected Tracks", [this] { groupTracks(false); }, g != 0);
        tg->addSeparator();
        add(*tg, "Collapse", [this] { collapseTrackGroup(true); }, g != 0 && !collapsedGroups.count(g));
        add(*tg, "Expand", [this] { collapseTrackGroup(false); }, g != 0 && collapsedGroups.count(g) != 0);
        tg->addSeparator();
        add(*tg, "Mute Track Group\tCtrl+Alt+Z", [this] { muteSoloTrackGroup(false, false); }, g != 0);
        add(*tg, "Solo Track Group\tCtrl+Alt+X", [this] { muteSoloTrackGroup(true, false); }, g != 0);
    }
    {
        QMenu* colors = menu.addMenu("Track Display Color");
        for (int k = 0; k < 8; ++k) {
            const QColor c = defaultTrackColor(k);
            QPixmap swatch(14, 14);
            swatch.fill(c);
            QAction* a = colors->addAction(QIcon(swatch), QString("Color %1").arg(k + 1));
            const uint32_t value = ((uint32_t)c.red() << 16) | ((uint32_t)c.green() << 8) | (uint32_t)c.blue();
            QObject::connect(a, &QAction::triggered, &menu, [this, id, value] {
                app.edit("Track Color", [&](Project& p) { if (Track* o = p.findTrack(id)) o->color = value; });
            });
        }
        colors->addSeparator();
        add(*colors, "Default", [this, id] { app.edit("Track Color", [&](Project& p) { if (Track* o = p.findTrack(id)) o->color = 0; }); });
    }
    menu.addSeparator();
    const bool small = minimized.count(id) != 0, big = maximized.count(id) != 0;
    add(menu, small ? "Restore Track Height" : "Minimize Track Height", [this, id] { toggleMinimized(id); });
    add(menu, big ? "Restore Track Height" : "Maximize Track Height", [this, id] { toggleMaximized(id); });
    automationMenu(*menu.addMenu("Automation"), id);
    if (q->onTrackMenu) q->onTrackMenu(&menu, id);
    if (video && q->onTrackMotion) add(menu, "Track Motion...", [this, id] { if (q->onTrackMotion) q->onTrackMotion(id); });
    add(menu, video ? "Video Track FX..." : "Track FX...", [this, id] {
        if (!q->onOpenFx) return;
        FxTarget ft;
        ft.kind = FxTarget::Track;
        ft.id = id;
        q->onOpenFx(ft, 0);
    });
}

void TimelineWidget::Impl::eventSwitchesMenu(QMenu& menu, const Event& e, bool video, const std::set<uint64_t>& sel)
{
    QMenu* switches = menu.addMenu("Switches");
    auto toggle = [&](QMenu* m, const QString& text, bool on, const std::string& what, std::function<void(Event&, bool)> set) {
        toggleItem(*m, text, on, [this, sel, on, what, set] {
            app.edit(what, [&](Project& p) {
                for (uint64_t s : sel) if (Event* o = p.findEvent(s)) set(*o, !on);
            });
        });
    };
    toggle(switches, "Mute", e.mute, e.mute ? "Unmute Events" : "Mute Events", [](Event& o, bool v) { o.mute = v; });
    toggle(switches, "Lock", e.locked, e.locked ? "Unlock Events" : "Lock Events", [](Event& o, bool v) { o.locked = v; });
    toggle(switches, "Loop", e.loop, "Loop Events", [](Event& o, bool v) { o.loop = v; });
    if (video) {
        switches->addSeparator();
        toggle(switches, "Maintain Aspect Ratio", e.keepAspect, "Maintain Aspect Ratio", [](Event& o, bool v) { o.keepAspect = v; });
        toggle(switches, "Reduce Interlace Flicker", e.reduceFlicker, "Reduce Interlace Flicker", [](Event& o, bool v) { o.reduceFlicker = v; });
        switches->addSeparator();
        const struct { const char* text; const char* mode; } res[] = {
            {"Use Project Resample Mode", "project"}, {"Force Resample", "force"}, {"Optical Flow", "optical"}, {"Disable Resample", "off"}};
        const std::string now = (e.resample == "force" || e.resample == "off" || e.resample == "optical") ? e.resample : std::string("project");
        for (const auto& r : res) {
            const std::string mode = r.mode;
            toggleItem(*switches, r.text, now == mode, [this, sel, mode] {
                app.edit(mode == "force" ? "Force Resample" : mode == "off" ? "Disable Resample" : mode == "optical" ? "Optical Flow Resample" : "Use Project Resample Mode", [&](Project& p) {
                    for (uint64_t s : sel) {
                        Track* t = nullptr;
                        if (Event* o = p.findEvent(s, &t))
                            if (t && t->type == TrackType::Video) o->resample = mode;
                    }
                });
            });
        }
    } else {
        toggle(switches, "Invert Phase", e.invertPhase, "Invert Phase", [](Event& o, bool v) { o.invertPhase = v; });
        const bool norm = e.normalize;
        toggleItem(*switches, "Normalize", norm, [this, sel, norm] { app.setNormalize(sel, !norm); });
        toggleItem(*switches, "Auto Normalize", e.autoNormalize, [this, sel, an = e.autoNormalize] { app.setAutoNormalize(sel, !an); });
    }
}

void TimelineWidget::Impl::eventTakeMenu(QMenu& menu, const Event& e, uint64_t id, const std::set<uint64_t>& sel)
{
    QMenu* takes = menu.addMenu("Take");
    command(*takes, "takeRename", "Rename Active");
    command(*takes, "takeChoose", "Choose Active...");
    const std::vector<Take> all = AppState::takesOf(e);
    if (!takes->actions().isEmpty()) takes->addSeparator();
    for (size_t k = 0; k < all.size(); ++k) {
        const int index = (int)k;
        toggleItem(*takes, QString("%1  %2").arg(k + 1).arg(QString::fromStdString(all[k].name)), (int)k == std::min(e.activeTake, (int)all.size() - 1),
                   [this, id, index] { app.chooseTake(std::set<uint64_t>{id}, index); });
    }
    takes->addSeparator();
    add(*takes, "Next Take\tT", [this, sel] { app.stepTakes(sel, 1); }, all.size() > 1);
    add(*takes, "Previous Take\tShift+T", [this, sel] { app.stepTakes(sel, -1); }, all.size() > 1);
    takes->addSeparator();
    add(*takes, "Delete Active", [this, id] { app.deleteActiveTake(id); }, all.size() > 1);
    command(*takes, "takeDelete", "Delete...");
}

void TimelineWidget::Impl::eventGroupMenu(QMenu& menu, const std::set<uint64_t>& sel, bool grouped)
{
    QMenu* group = menu.addMenu("Group");
    add(*group, "Create New\tG", [this] { app.groupSelection(true); }, sel.size() > 1);
    group->addSeparator();
    add(*group, "Remove From\tU", [this] { app.groupSelection(false); }, grouped);
    add(*group, "Clear\tCtrl+U", [this] { clearGroups(); }, grouped);
    add(*group, "Select All\tShift+G", [this] { selectGroups(); }, grouped);
    group->addSeparator();
    add(*group, "Cut All", [this] { selectGroups(); removeSelection(true); }, grouped);
    add(*group, "Copy All", [this] { selectGroups(); app.copySelection(); }, grouped);
    add(*group, "Delete All", [this] { selectGroups(); removeSelection(false); }, grouped);
}

void TimelineWidget::Impl::eventSyncLinkMenu(QMenu& menu, const Event& e, uint64_t id, const std::set<uint64_t>& sel)
{
    const bool others = sel.size() > 1 || (sel.size() == 1 && !sel.count(id));
    bool anyLinked = false;
    for (uint64_t s : sel)
        if (const Event* o = app.project().findEvent(s))
            if (o->syncLink == id) anyLinked = true;
    add(menu, "Create Sync Link with Selected Events", [this, id] { createSyncLink(id); }, others);
    add(menu, "Remove Sync Link from Selected Events", [this, id] { removeSyncLink(id); }, anyLinked);
    QMenu* link = menu.addMenu("Sync Link");
    add(*link, "Unlink", [this, id] {
        app.edit("Unlink", [&](Project& p) { if (Event* x = p.findEvent(id)) x->syncLink = 0; });
    }, e.syncLink != 0);
    add(*link, "Select All", [this, id] { app.setSelection(syncLinkGroup(id)); }, !syncLinkGroup(id).empty());
}

void TimelineWidget::Impl::eventSynchronizeMenu(QMenu& menu, uint64_t id)
{
    // VEGAS's Synchronize: an event that has slipped out of step with the event of the same media grouped with it (its
    // video or audio) is put back in step, by moving it or slipping its media
    const uint64_t partner = syncPartner(id);
    QMenu* sync = menu.addMenu("Synchronize");
    add(*sync, "By Moving", [this, id, partner] { synchronize(id, partner, true); }, partner != 0);
    add(*sync, "By Slipping", [this, id, partner] { synchronize(id, partner, false); }, partner != 0);
}

void TimelineWidget::Impl::eventChannelsMenu(QMenu& menu, const Event& e, const std::set<uint64_t>& sel)
{
    QMenu* ch = menu.addMenu("Channels");
    const struct { const char* id; const char* text; } maps[] = {
        {"both", "Both"}, {"left", "Left Only"}, {"right", "Right Only"}, {"combine", "Combine"}, {"swap", "Swap"}};
    const std::string now = e.channels.empty() ? std::string("both") : e.channels;
    for (const auto& m : maps) {
        const std::string v = m.id;
        toggleItem(*ch, m.text, now == m.id, [this, sel, v] {
            app.edit("Channels", [&](Project& p) {
                for (uint64_t s : sel) {
                    Track* t = nullptr;
                    if (Event* o = p.findEvent(s, &t))
                        if (t && t->type == TrackType::Audio) o->channels = v;
                }
            });
        });
    }
}

void TimelineWidget::Impl::eventRateMenu(QMenu& menu, const Event& e, uint64_t id, const std::set<uint64_t>& sel, bool transition)
{
    QMenu* rates = menu.addMenu("Playback Rate");
    const struct { double rate; const char* text; } choices[] = {{0.25, "25%"}, {0.5, "50%"}, {1.0, "100% (Normal)"}, {2.0, "200%"}, {4.0, "400%"}};
    // The event keeps its length and shows more or less of its media; the events grouped with it get the same rate so a
    // clip's video and audio stay in step.
    const std::set<uint64_t> withG = withGroups(sel);
    for (const auto& c : choices) {
        const double r = c.rate;
        toggleItem(*rates, c.text, std::fabs(e.rate - r) < 1e-6, [this, withG, r] {
            app.edit("Playback Rate", [&](Project& p) { for (uint64_t s : withG) if (Event* o = p.findEvent(s)) o->rate = r; });
        });
    }
    if (std::fabs(e.rate - 1.0) > 1e-6) add(menu, "Reset Rate", [this, withG] {
        app.edit("Playback Rate", [&](Project& p) { for (uint64_t s : withG) if (Event* o = p.findEvent(s)) o->rate = 1.0; });
    });
    if (transition) add(menu, "Remove Transition", [this, id] { app.setTransition(id, std::string(), std::string()); });
}

// An event's menu (VEGAS's POPUP_IDX_VIDEO_EVENT / POPUP_IDX_AUDIO_EVENT), then Reno's own (playback rate, a
// transition's removal).
bool TimelineWidget::Impl::eventMenu(QMenu& menu, const Hit& h, double time)
{
    const Track* tr = nullptr;
    const Event* e = app.project().findEvent(h.event, &tr);
    if (!e || !tr) return false;
    const uint64_t id = h.event;
    const bool video = tr->type == TrackType::Video;
    const bool transition = !e->transition.kind.empty();
    const std::set<uint64_t> sel = app.selection();
    bool grouped = false;
    for (uint64_t s : sel)
        if (const Event* o = app.project().findEvent(s))
            if (o->group) grouped = true;
    if (time >= 0) addFadeTypes(menu, *tr, *e, time);
    command(menu, "openInTrimmer", "Open in Trimmer");
    if (const MediaItem* em = app.project().findMedia(e->mediaId))
        if (em->subclipLength > 0) command(menu, "openParentInTrimmer", "Open Parent Media in Trimmer");
    if (!video && q->onAudioEditor) add(menu, "Open in Audio Editor\tCtrl+E", [this, id] { if (q->onAudioEditor) q->onAudioEditor(id); });
    if (!video) command(menu, "audioEditorCopy", "Open Copy in Audio Editor");
    if (q->onSelectInMedia && e->mediaId) {
        const uint64_t mid = e->mediaId;
        add(menu, "Select in Project Media List", [this, mid] { if (q->onSelectInMedia) q->onSelectInMedia(mid); });
    }
    command(menu, "nestCreate", "Create Nested Timeline");
    command(menu, "nestOpen", "Open Nested Timeline");
    menu.addSeparator();
    if (video) {
        if (q->onPanCrop) add(menu, "Video Event Pan/Crop...", [this, id] { if (q->onPanCrop) q->onPanCrop(id); });
        add(menu, "Video Event FX...", [this, id] { openEventFx(id); });
        if (!command(menu, "mediaFx", "Media FX...")) add(menu, "Media FX...", [] {}, false);
        add(menu, "Edit in VEGAS Effects", [] {}, false);
        command(menu, "motionTracking", "Motion Tracking");
        command(menu, "redEye", "Red Eye Reduction...");
        fxWindowsMenu(menu, id);
    } else {
        add(menu, "Audio Event FX...", [this, id] { openEventFx(id); });
        command(menu, "nonRealTimeFx", "Apply Non-Real-Time Event FX...");
    }
    menu.addSeparator();
    add(menu, "Cut\tCtrl+X", [this] { removeSelection(true); });
    add(menu, "Copy\tCtrl+C", [this] { app.copySelection(); });
    add(menu, "Paste Event Attributes", [this] { app.pasteEventAttributes(app.selection(), AppState::AttrAll); }, app.hasClipboard());
    if (!command(menu, "selectivePaste", "Selectively Paste Event Attributes..."))
        add(menu, "Selectively Paste Event Attributes...", [] {}, false);
    menu.addSeparator();
    add(menu, "Delete\tDelete", [this] { removeSelection(false); });
    add(menu, "Trim\tCtrl+T", [this] { q->trimToTimeSelection(); }, app.loopEnd > app.loopStart);
    add(menu, "Trim Start\tAlt+[", [this] { q->trimStartToCursor(); });
    add(menu, "Trim End\tAlt+]", [this] { q->trimEndToCursor(); });
    add(menu, "Split\tS", [this] { app.splitAtCursor(); });
    if (video) add(menu, "Smart Split\tAlt+S", [this] { smartSplit(); }, app.loopEnd > app.loopStart);
    add(menu, "Event Heal", [this] { app.healEvents(app.selection()); });
    add(menu, "Close Gaps", [this] { app.closeGaps(app.selection()); });
    menu.addSeparator();
    command(menu, "addMissingStream", "Add Missing Streams for Selected Event");
    command(menu, "eventSubclip", "Create Subclip");
    {
        const bool reversed = e->reverse;
        toggleItem(menu, "Reverse", reversed, [this, sel, reversed] {
            app.edit("Reverse Events", [&](Project& p) { for (uint64_t s : sel) if (Event* o = p.findEvent(s)) o->reverse = !reversed; });
        });
    }
    menu.addSeparator();
    add(menu, "Select Events to End", [this] { q->selectEventsToEnd(); });
    add(menu, "Select All After Cursor", [this] { q->selectAllAfterCursor(); });
    {
        EnvelopeOwner o;
        o.kind = EnvelopeOwner::Event;
        o.id = id;
        QMenu* env = menu.addMenu("Insert/Remove Envelope");
        addEnvelopeChoices(env, o);
        if (video) {
            // VEGAS's Freeze Frame at Cursor: a velocity envelope that plays as before up to the cursor and holds the
            // frame there to the event's end
            const double at = app.cursor() - e->start;
            add(*env, "Freeze Frame at Cursor", [this, id] { freezeFrameAtCursor(id); }, at > 1e-6 && at < e->length - 1e-6 && !transition);
        }
    }
    menu.addSeparator();
    eventSwitchesMenu(menu, *e, video, sel);
    eventTakeMenu(menu, *e, id, sel);
    eventGroupMenu(menu, sel, grouped);
    eventSyncLinkMenu(menu, *e, id, sel);
    eventSynchronizeMenu(menu, id);
    if (!video) eventChannelsMenu(menu, *e, sel);
    eventRateMenu(menu, *e, id, sel, transition);
    if (!video) {
        const Track* evTrack = nullptr;
        const Event* ev = app.project().findEvent(id, &evTrack);
        if (ev && ev->mediaId) {
            menu.addSeparator();
            const uint64_t mid = ev->mediaId;
            add(menu, "Beat Detection...", [this, mid] { showBeatDetectionDialog(q, mid); });
            if (evTrack && evTrack->duckController)
                add(menu, "Re-Analyze Auto Ducking", [this, id] {
                    std::string err;
                    if (!reanalyzeAutoDucking(id, &err)) app.status("Auto Ducking: " + err);
                });
        }
    }
    if (q->onEventProperties) {
        menu.addSeparator();
        add(menu, "Properties...", [this, id] { if (q->onEventProperties) q->onEventProperties(id); });
    }
    return true;
}

void TimelineWidget::Impl::openEventFx(uint64_t id)
{
    if (!q->onOpenFx) return;
    FxTarget t;
    t.kind = FxTarget::Event;
    t.id = id;
    q->onOpenFx(t, 0);
}

void TimelineWidget::Impl::addFadeTypes(QMenu& menu, const Track& tr, const Event& e, double time)
{
    const uint64_t id = e.id;
    auto mirror = [](int c) { return c == CurveFast ? (int)CurveSlow : c == CurveSlow ? (int)CurveFast : c; };
    auto curves = [&](const QString& title, std::function<void(int)> set, int now, bool falling, bool cross) {
        QMenu* m = menu.addMenu(title);
        for (int c = 0; c < CurveHold; ++c) {
            QAction* a = m->addAction(curveIcon(c, falling, cross ? mirror(c) : -1), kCurveNames[c]);
            a->setCheckable(true);
            a->setChecked(c == now);
            QObject::connect(a, &QAction::triggered, &menu, [set, c] { set(c); });
        }
    };
    uint64_t outId = 0, inId = 0;
    if (const Event* a = fadesFrom(tr, e))
        if (time >= e.start && time <= a->end()) { outId = a->id; inId = e.id; }
    if (!inId)
        for (const Event& o : tr.events)
            if (const Event* a = fadesFrom(tr, o))
                if (a->id == e.id && time >= o.start && time <= e.end()) { outId = e.id; inId = o.id; }
    if (inId) {
        const Event* in = app.project().findEvent(inId);
        curves("Fade Type", [this, inId, outId, mirror](int c) {
            app.edit("Cross-Fade Type", [&](Project& p) {
                if (Event* x = p.findEvent(inId)) {
                    x->fadeInCurve = c;
                    x->fadeInRecipCurve = mirror(c);
                }
                if (Event* x = p.findEvent(outId)) x->fadeOutCurve = mirror(c);
            });
        }, in ? in->fadeInCurve : 0, false, true);
        if (tr.type == TrackType::Video) {
            QMenu* tm = menu.addMenu("Transition");
            add(*tm, "Insert Other...", [this, inId] {
                FxTarget t;
                t.kind = FxTarget::Transition;
                t.id = inId;
                uint64_t slot = 0;
                if (choosePluginsFor(q, t, &slot) && q->onOpenFx) q->onOpenFx(t, slot);
            });
            tm->addSeparator();
            std::vector<std::pair<std::string, std::string>> all;
            for (const VideoFxEntry& v : Catalog::get().video())
                if (v.transition && v.ok) all.emplace_back(v.label, v.id);
            std::sort(all.begin(), all.end());
            const std::string now = in ? in->transition.pluginId : std::string();
            for (const auto& tv : all) {
                const std::string id = tv.second, label = tv.first;
                toggleItem(*tm, QString::fromStdString(label), id == now, [this, inId, id, label] { app.setTransition(inId, id, label); });
            }
            if (all.empty()) add(*tm, "(no transitions found)", [] {}, false);
        }
        menu.addSeparator();
        return;
    }
    if (e.fadeIn > 0 && time <= e.start + e.fadeIn) {
        curves("Fade Type", [this, id](int c) { app.edit("Fade In Type", [&](Project& p) { if (Event* x = p.findEvent(id)) x->fadeInCurve = c; }); },
               e.fadeInCurve, false, false);
        menu.addSeparator();
    } else if (e.fadeOut > 0 && time >= e.end() - e.fadeOut) {
        curves("Fade Type", [this, id](int c) { app.edit("Fade Out Type", [&](Project& p) { if (Event* x = p.findEvent(id)) x->fadeOutCurve = c; }); },
               e.fadeOutCurve, true, false);
        menu.addSeparator();
    }
}

}  // namespace app
}  // namespace reno
