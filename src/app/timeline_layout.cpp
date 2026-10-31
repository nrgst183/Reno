// Geometry of the timeline: where tracks, events and envelopes are, the view, snapping.
#include "timeline_internal.h"
#include <QStyle>
#include <QStyleOptionSlider>

namespace reno {
namespace app {

// A track's height: its own if it has one, else the default for its kind (scaled by Ctrl+Shift+Up/Down), or the smallest
// if minimized.
int TimelineWidget::Impl::trackHeight(const Track& t) const
{
    if (minimized.count(t.id)) return kMinTrackH;
    if (maximized.count(t.id)) return clampTrackH(lanesBottom() - kTopH);
    const int base = t.height > 0 ? t.height : (int)std::lround((t.type == TrackType::Video ? kVideoH : kAudioH) * heightScale);
    return clampTrackH(base);
}

void TimelineWidget::Impl::layout()
{
    ++headerGeneration;
    rows.clear();
    int y = 0;
    const Project& p = app.project();
    std::set<uint64_t> groupSeen;
    for (size_t k = 0; k < p.tracks.size(); ++k) {
        Row r;
        r.index = (int)k;
        r.id = p.tracks[k].id;
        r.video = p.tracks[k].type == TrackType::Video;
        r.y = y;
        r.h = trackHeight(p.tracks[k]);
        if (r.video && p.tracks[k].layersExpanded && !p.tracks[k].minimized && r.h >= 40) {
            r.h *= 2;
            r.lm = buildLayers(p.tracks[k]);
        }
        const uint64_t g = p.tracks[k].trackGroup;
        if (g && !groupSeen.insert(g).second && collapsedGroups.count(g)) r.h = 0;
        y += r.h;
        rows.push_back(r);
    }
    busRows.clear();
    if (opt.showBusTracks) {
        auto add = [&](EnvelopeOwner::Kind kind, uint64_t id, const QString& name, bool video) {
            BusRow b;
            b.owner.kind = kind;
            b.owner.id = id;
            b.name = name;
            b.video = video;
            b.y = y;
            b.h = kBusH;
            y += b.h;
            busRows.push_back(b);
        };
        add(EnvelopeOwner::VideoOut, 0, "Video Output", true);
        add(EnvelopeOwner::Master, 0, "Master", false);
        for (const Bus& b : p.buses) add(EnvelopeOwner::Bus, b.id, QString::fromStdString(b.name.empty() ? std::string("Bus") : b.name), false);
    }
    contentH = y + kBelow;
}

// The A roll, B roll and transition roll of an expanded track (VEGAS 18's Expand Track Layers): an event that starts
// where the
// one before ends, or inside it, goes on the other roll than that one; an event on its own goes on the A roll.
std::shared_ptr<TimelineWidget::Impl::LayerMap> TimelineWidget::Impl::buildLayers(const Track& t) const
{
    auto lm = std::make_shared<LayerMap>();
    const double eps = 0.5 / std::max(1.0, fps());
    const Event* prev = nullptr;
    int prevLayer = 0;
    for (const Event& e : t.events) {
        int layer = 0;
        if (prev && e.start < prev->end() + eps) layer = 1 - prevLayer;
        if (prev && e.start < prev->end() - 1e-9) lm->trans.push_back(std::make_pair(e.start, std::min(prev->end(), e.end())));
        lm->of[e.id] = layer;
        prev = &e;
        prevLayer = layer;
    }
    return lm;
}

int TimelineWidget::Impl::busRowAtY(double y) const
{
    for (size_t k = 0; k < busRows.size(); ++k) {
        const int top = kTopH + busRows[k].y - scrollY;
        if (y >= top && y < top + busRows[k].h) return (int)k;
    }
    return -1;
}

int TimelineWidget::Impl::rowAtY(double y) const
{
    for (size_t k = 0; k < rows.size(); ++k)
        if (y >= rowTop(rows[k]) && y < rowTop(rows[k]) + rows[k].h) return (int)k;
    return -1;
}

QRectF TimelineWidget::Impl::eventRect(const Row& r, const Event& e) const
{
    // on whole pixels, the pixel a time falls in (as the ruler's marks and the cursor are drawn), so an event
    // starting at a mark or the cursor starts on that very pixel and events that meet share no pixel; a rectangle
    // with fractional edges would be filled from the nearest pixel edge, up to a pixel off
    const double x0 = std::floor(xAtF(e.start)), x1 = std::floor(xAtF(e.end()));
    if (r.lm) {
        const auto it = r.lm->of.find(e.id);
        const int layer = it == r.lm->of.end() ? 0 : it->second;
        const double hl = layerH(r);
        return QRectF(x0, rowTop(r) + 1.0 + layer * hl, std::max(1.0, x1 - x0), hl - 1.0);
    }
    return QRectF(x0, rowTop(r) + 1.0, std::max(1.0, x1 - x0), r.h - 2.0);
}

QRectF TimelineWidget::Impl::menuButtonRect(const QRectF& ev) const
{
    const double th = eventTitleH(ev);
    if (th > 0) {
        const double sz = std::min((double)kEventButton, th - 1.0);
        return QRectF(ev.right() - 3.0 - sz, ev.top() + (th - sz) / 2.0, sz, sz);
    }
    return QRectF(ev.right() - 4.0 - kEventButton, ev.bottom() - 3.0 - kEventButton, kEventButton, kEventButton);
}

QString TimelineWidget::Impl::gainText(bool video, double gain)
{
    if (video) return QString::fromStdString(strfmt("Opacity is %.0f%%", gainFraction(gain) * 100.0));
    if (gain <= 1e-5) return QString("Gain is -Inf dB");
    return QString::fromStdString(strfmt("Gain is %.1f dB", 20.0 * std::log10(gain)));
}

TimelineWidget::Impl::HeaderRects TimelineWidget::Impl::headerRects(const Row& r) const
{
    HeaderRects h = vegasHeaderRects(r);
    if (!r.video && app.project().settings.surround()) placeSurround(h);
    return h;
}

// A 5.1 project: an audio track's header shows its surround panner as a small square at its right, from the level
// slider's row down (where the pan slider was; VEGAS's 5.1 header has no pan slider); the level slider gets shorter.
// Only on a header tall and wide enough for the square and a usable level slider.
void TimelineWidget::Impl::placeSurround(HeaderRects& h) const
{
    if (h.level.isEmpty()) return;
    const int top = h.level.top() - 2;
    const int side = std::min(40, h.body.bottom() - 2 - top);
    if (side < 16) return;
    const QRect sq(h.body.right() - 6 - side + 1, top, side, side);
    QRect level = h.level;
    if (!h.pan.isEmpty()) level = QRect(h.levelValue.left() + 6, h.pan.top(), sq.left() - 12 - (h.levelValue.left() + 6), h.pan.height());
    else level.setRight(sq.left() - 12);
    if (level.width() < 30) return;
    h.level = level;
    h.surround = sq;
    h.pan = h.panValue = QRect();
}

TimelineWidget::Impl::HeaderRects TimelineWidget::Impl::vegasHeaderRects(const Row& r) const
{
    HeaderRects h;
    const int y = rowTop(r);
    h.all = QRect(0, y, headerW, r.h);
    h.strip = QRect(kHeaderMargin, y + 1, kStripW, r.h - 1);
    h.menu = QRect(kHeaderMargin + 3, y + 4, 17, 15);
    const int bx = kHeaderMargin + kStripW + 1, br = headerW - kHeaderGap;
    h.body = QRect(bx, y + 1, br - bx, r.h - 1);
    if (r.h < kMinTrackH + 4) {
        int l2 = br - 1;
        if (!buttonHidden(r.video, "solo")) { h.solo = QRect(l2 - 20, y + 4, 16, 16); l2 = h.solo.left(); }
        if (!buttonHidden(r.video, "mute")) { h.mute = QRect(l2 - 20, y + 4, 16, 16); l2 = h.mute.left(); }
        h.name = QRect(bx + 4, y + 3, std::max(0, l2 - 4 - (bx + 4)), 17);
        return h;
    }
    int left = br + 1;
    if (!buttonHidden(r.video, "solo")) { h.solo = QRect(left - 24, y + 4, 16, 16); left = h.solo.left(); }
    if (!buttonHidden(r.video, "mute")) { h.mute = QRect(left - 24, y + 4, 16, 16); left = h.mute.left(); }
    const int nameRoom = 56;
    if (!r.video && !buttonHidden(false, "arm") && (left - 24 - (bx + 4) >= nameRoom || app.armedTracks.count(r.id) != 0)) {
        h.arm = QRect(left - 24, y + 4, 16, 16);
        left = h.arm.left();
    }
    if (left - 24 - (bx + 4) >= nameRoom) {
        if (!buttonHidden(r.video, "fx")) {
            h.fx = QRect(left - 24, y + 4, 16, 16);
            left = h.fx.left();
        }
        if (r.video && !buttonHidden(true, "motion") && left - 24 - (bx + 4) >= nameRoom) {
            h.motion = QRect(left - 24, y + 4, 16, 16);
            left = h.motion.left();
        }
        if (!buttonHidden(r.video, "auto") && left - 24 - (bx + 4) >= nameRoom) {
            h.autoBtn = QRect(left - 24, y + 4, 16, 16);
            left = h.autoBtn.left();
        }
    }
    h.name = QRect(bx + 4, y + 3, std::max(0, left - 6 - (bx + 4)), 17);
    if (r.h >= 46) {
        const int ly = y + 27;
        if (r.video) {
            h.level = QRect(bx + 19, ly, br - 20 - (bx + 19), 12);
        } else {
            h.levelValue = QRect(bx + 10, ly, 50, 12);
            h.level = QRect(bx + 67, ly, br - 20 - (bx + 67), 12);
        }
    }
    if (!r.video && r.h >= 66) {
        const int py = y + 49;
        h.panValue = QRect(bx + 10, py, 50, 12);
        h.pan = QRect(bx + 67, py, br - 20 - (bx + 67), 12);
    }
    const Track* tk = trackOf(r);
    if (r.video && tk && tk->automation && r.h >= 66) h.fade = QRect(bx + 19, y + 49, br - 20 - (bx + 19), 12);
    if (!r.video && r.h >= 90) {
        h.phase = QRect(bx + 6, y + 69, 16, 16);
        h.bus = QRect(bx + 28, y + 69, std::max(30, std::min(90, br - bx - 34 - (h.autoBtn.isEmpty() ? 22 : 0))), 16);
    }
    if (h.autoBtn.isEmpty() && r.video && !h.level.isEmpty() && !buttonHidden(true, "auto")) {
        h.autoBtn = QRect(bx + 1, h.level.top() - 2, 16, 16);
        h.level.setLeft(h.level.left() + 6);
        if (!h.fade.isEmpty()) h.fade.setLeft(h.fade.left() + 6);
    }
    if (h.autoBtn.isEmpty() && !buttonHidden(false, "auto") && !h.bus.isEmpty() && h.bus.right() + 22 < br) h.autoBtn = QRect(h.bus.right() + 6, h.bus.top(), 16, 16);
    return h;
}

void TimelineWidget::Impl::setHeaderWidth(int w)
{
    w = clampHeaderW(std::min(w, q->width() - 120));
    if (w == headerW) return;
    const double t = viewStart;
    headerW = w;
    viewStart = t;
    updateBars();
    q->update();
    QSettings().setValue("timeline/headerWidth", headerW);
    if (q->onHeaderWidthChanged) q->onHeaderWidthChanged(headerW);
}

void TimelineWidget::Impl::updateBars()
{
    const int w = q->width(), h = q->height();
    const int visH = std::max(0, h - kTopH - kBar);
    const bool overflow = contentH > visH && visH > 30;
    const int vw = viewW();
    hbar->setGeometry(headerW, h - kBar, std::max(0, w - 3 * kBar - headerW), kBar);
    zoomInButton->setGeometry(w - 3 * kBar, h - kBar, kBar, kBar);
    zoomOutButton->setGeometry(w - 2 * kBar, h - kBar, kBar, kBar);
    zoomToolButton->setGeometry(w - kBar, h - kBar, kBar, kBar);
    vbar->setGeometry(w - kBar, kLoopTop, kBar, std::max(0, h - 3 * kBar - kLoopTop));
    tallerButton->setGeometry(w - kBar, h - 3 * kBar, kBar, kBar);
    markerToolButton->setGeometry(w - kBar, kMarkerTop, kBar, std::min(kBar, kMarkerH));
    shorterButton->setGeometry(w - kBar, h - 2 * kBar, kBar, kBar);
    // The whole the groove stands for: the project's length (not the cursor or a time selection set past it, so
    // moving those doesn't resize the handle), or the view itself when it extends past that (zoomed out, scrolled on,
    // an event dragged past the end). So the handle is always the view's span as a part of that and starts where the
    // view does.
    const double viewSeconds = vw / pps;
    const double projLen = app.project().length();
    double total = std::max(projLen > 0 ? projLen : 30.0 * 1.04, viewStart + viewSeconds);
    if (hbar->isSliderDown() && hbarTotal > 0) total = std::max(hbarTotal, viewStart + viewSeconds);
    // while an end of the scroll box is dragged the whole is kept as it was at the press so the box's end stays under
    // the pointer (barEdgeDrag); after it the bar is as the view is
    if (barZoom.active && !barZoom.vertical) total = std::max(barZoom.total0, viewStart + viewSeconds);
    // VEGAS's scroll box (measured from VEGAS Pro 18): never shorter than
    // kHBarMinBox pixels; the rest of the groove is the time scale. The box is kHBarMinBox + (groove - kHBarMinBox) x
    // view / whole long and starts at (groove - kHBarMinBox) x view start / whole. Qt's bar is made to draw exactly
    // that: its page is chosen so its proportional box comes out that long, and its value runs over the view starts.
    QStyleOptionSlider so;
    so.initFrom(hbar);
    so.orientation = Qt::Horizontal;
    so.minimum = 0; so.maximum = 1; so.pageStep = 1;
    const double groove = std::max(1, hbar->style()->subControlRect(QStyle::CC_ScrollBar, &so, QStyle::SC_ScrollBarGroove, hbar).width());
    const double share = total > 1e-12 ? clampd(viewSeconds / total, 0.0, 1.0) : 1.0;
    const double boxShare = std::min(1.0, (kHBarMinBox + (groove - kHBarMinBox) * share) / groove);
    hbarTotal = total;
    hbarSpan = std::max(0.0, total - viewSeconds);
    QSignalBlocker b1(hbar), b2(vbar);
    if (hbarSpan * pps < 0.5 || boxShare >= 0.9999) {
        hbarSpan = 0;
        hbar->setRange(0, 0);
        hbar->setPageStep(kHBarRange);
        hbar->setValue(0);
    } else {
        const double page = std::min(1.0e9, kHBarRange * boxShare / (1.0 - boxShare));
        hbar->setRange(0, kHBarRange);
        hbar->setPageStep(std::max(1, (int)std::lround(page)));
        hbar->setSingleStep(std::max(1, (int)std::lround(kHBarRange * (viewSeconds / 20.0) / hbarSpan)));
        hbar->setValue((int)std::lround(clampd(viewStart / hbarSpan, 0.0, 1.0) * kHBarRange));
    }
    vbar->setRange(0, overflow ? std::max(0, contentH - visH) : 0);
    vbar->setPageStep(visH);
    vbar->setSingleStep(24);
    scrollY = std::min(std::max(scrollY, 0), vbar->maximum());
    vbar->setValue(scrollY);
    placeNameEdit();
}

void TimelineWidget::Impl::barEdgeDrag(bool vertical, int phase, int edge, double pos, double len)
{
    if (len < 8) return;
    if (!vertical) {
        const double vw = viewW();
        const double minS = vw / maxPps(), maxS = vw / minPps();
        if (phase == 0) {
            barZoom = BarZoom();
            barZoom.active = true;
            barZoom.pos0 = pos;
            barZoom.viewStart0 = viewStart;
            barZoom.viewEnd0 = viewStart + vw / pps;
            barZoom.total0 = hbarTotal;
            barZoom.scale = barZoom.total0 / std::max(1.0, len - kHBarMinBox);
            return;
        }
        if (!barZoom.active) return;
        if (phase == 2) {
            barZoom.active = false;
            updateBars();
            q->update();
            return;
        }
        const double moved = (pos - barZoom.pos0) * barZoom.scale;
        double newStart = barZoom.viewStart0, newS;
        if (edge == 0) {
            const double end = barZoom.viewEnd0;
            newStart = clampd(barZoom.viewStart0 + moved, std::max(0.0, end - maxS), std::max(0.0, end - minS));
            newS = end - newStart;
        } else {
            newS = clampd(barZoom.viewEnd0 + moved - barZoom.viewStart0, minS, maxS);
        }
        pps = clampd(vw / std::max(newS, 1e-9), minPps(), maxPps());
        viewStart = std::max(0.0, newStart);
        updateBars();
        q->update();
        return;
    }
    if (phase == 0) {
        barZoom = BarZoom();
        barZoom.active = true;
        barZoom.vertical = true;
        barZoom.pos0 = pos;
        barZoom.visH = std::max(1, lanesBottom() - kTopH);
        barZoom.contentTop = scrollY;
        barZoom.contentBottom = scrollY + barZoom.visH;
        barZoom.totalH = contentH;
        barZoom.scale = (double)std::max(contentH, barZoom.visH) / len;
        for (const Row& r : rows) barZoom.tracksH += r.h;
        for (const Track& t : app.project().tracks) barZoom.heights[t.id] = trackHeight(t);
        return;
    }
    if (!barZoom.active) return;
    if (phase == 2) {
        barZoom.active = false;
        if (barZoom.changed) app.endDrag("Zoom Track Height");
        updateBars();
        return;
    }
    const double T = barZoom.tracksH;
    if (!(T > 0)) return;
    const double moved = (pos - barZoom.pos0) * barZoom.scale;
    const double span = std::max(1.0, barZoom.visH + (edge == 1 ? moved : -moved));
    const double u = barZoom.visH / span - 1.0;
    const double f = clampd(1.0 + u, 0.1, 40.0);
    if (!barZoom.changed) {
        minimized.clear();
        maximized.clear();
        app.beginDrag();
        barZoom.changed = true;
    }
    const std::map<uint64_t, int>& base = barZoom.heights;
    app.dragUpdate([&](Project& pr) {
        for (Track& t : pr.tracks) {
            auto it = base.find(t.id);
            if (it == base.end()) continue;
            const int def = t.type == TrackType::Video ? kVideoH : kAudioH;
            const int h = clampTrackH((int)std::lround(it->second * f));
            t.height = h == def ? 0 : h;
        }
    });
    layout();
    double T2 = 0;
    for (const Row& r : rows) T2 += r.h;
    const double u2 = T2 / T - 1.0;
    double sy;
    if (edge == 1) sy = barZoom.contentTop + u2 * std::min(barZoom.contentTop, T);
    else sy = barZoom.contentBottom + u2 * std::min(barZoom.contentBottom, T) - barZoom.visH;
    scrollY = (int)std::lround(std::max(0.0, sy));
    updateBars();
    placeNameEdit();
    q->update();
}

void TimelineWidget::Impl::setViewStart(double t)
{
    const double was = viewStart;
    viewStart = std::max(0.0, t);
    updateBars();
    viewMoved(was, scrollY);
}

void TimelineWidget::Impl::setScrollY(int y)
{
    const int was = scrollY;
    scrollY = y;
    updateBars();
    viewMoved(viewStart, was);
}

// The view starts at 't' (playback keeping the cursor in view): put on a whole pixel (on a screen scaled by a fraction,
// a step of whole device pixels: two pixels at 150%, four at 125%) from where it is, so what is drawn can be moved.
void TimelineWidget::Impl::scrollViewTo(double t)
{
    t = std::max(0.0, t);
    int step = 1;
    const double dpr = q->devicePixelRatioF();
    while (step < 8 && std::fabs(step * dpr - std::round(step * dpr)) > 1e-6) ++step;
    const double from = viewStart * pps;
    double px = std::floor(t * pps + 0.5);
    if (step > 1 && std::fabs(from - std::round(from)) < 1e-6) px = std::round(from) + std::round((px - std::round(from)) / step) * step;
    setViewStart(std::max(0.0, px) / pps);
}

// The view was scrolled from (oldStart, oldScrollY) to where it is now, at the same zoom. Scrolled by whole pixels
// one way, what is drawn is moved across (QWidget::scroll) and only what came into view is painted, plus the few
// things that stay put as the view moves (the top bars' numbers and names, event names running past the left edge, a
// strip at each side). Otherwise everything is painted again.
void TimelineWidget::Impl::viewMoved(double oldStart, int oldScrollY)
{
    const double dxF = (oldStart - viewStart) * pps;
    const int dy = oldScrollY - scrollY;
    if (dxF == 0 && dy == 0) return;
    const double dxR = std::floor(dxF + 0.5);
    const bool whole = std::fabs(dxF - dxR) < 1e-6;
    const int dx = (int)dxR;
    const QRect lanes = lanesRect();
    const bool overlays = drop.kind != DropKind::None || !envLabel.isEmpty() || rows.empty() ||
                          ((drag.kind == Drag::RubberBand || drag.kind == Drag::EnvBand || drag.kind == Drag::ZoomRange) && drag.started) ||
                          (trackListFocus && q->hasFocus() && dy != 0);
    const bool denseNow = countDense();
    if (!whole || (dx != 0 && dy != 0) || std::abs(dx) >= lanes.width() - 80 || std::abs(dy) >= lanes.height() - 20 || overlays ||
        denseNow != dense || !q->isVisible()) {
        q->update();
        return;
    }
    if (dx != 0) {
        q->scroll(dx, 0, lanes);
        QRegion more(QRect(headerW, 0, viewW(), kTopH));
        more |= QRect(lanes.left(), lanes.top(), 48, lanes.height());
        // an event running past the right edge is drawn by how much of it shows (narrow, its name elided at the edge,
        // its link mark only with room for it): what was drawn near the right edge and has been moved in from it is
        // drawn again
        const int right = 64 + std::max(0, -dx);
        more |= QRect(lanes.right() + 1 - right, lanes.top(), right, lanes.height());
        // the events that run past the left edge (before or now) keep their names to it: one strip down all the tracks
        // as wide as the widest of those (one piece to paint rather than one per track)
        const Project& pr = app.project();
        const double edges[2] = {oldStart, viewStart};
        double widest = 0;
        for (const Row& r : rows) {
            const int y = rowTop(r);
            if (y + r.h <= lanes.top() || y > lanes.bottom()) continue;
            const std::vector<Event>& ev = pr.tracks[(size_t)r.index].events;
            double reach = 0;
            for (double edge : edges) {
                const std::pair<size_t, size_t> range = eventRange((size_t)r.index, edge, edge);
                for (size_t k = range.first; k < range.second; ++k) {
                    const Event& e = ev[k];
                    if (!(e.start < edge && e.end() > edge)) continue;
                    const double kept = opt.showTakeInfo ? 640.0 : 40.0 + titleWidth(e);
                    reach = std::max(reach, std::min({(e.end() - viewStart) * pps, (double)viewW(), kept}));
                }
            }
            if (reach >= 12.0) widest = std::max(widest, reach);
        }
        if (widest > 0) more |= QRect(lanes.left(), lanes.top(), (int)std::ceil(std::min(widest, 640.0)) + std::abs(dx) + 4, lanes.height());
        q->update(more);
    } else {
        q->scroll(0, dy, QRect(0, lanes.top(), lanesRight(), lanes.height()));
    }
    dense = denseNow;
}

void TimelineWidget::Impl::setZoom(double newPps, double anchorTime, double anchorX)
{
    pps = clampd(newPps, minPps(), maxPps());
    viewStart = std::max(0.0, anchorTime - (anchorX - headerW) / pps);
    updateBars();
    q->update();
}

void TimelineWidget::Impl::zoomBy(double factor)
{
    const double cx = xAtF(app.cursor());
    if (cx >= headerW && cx <= lanesRight()) setZoom(pps * factor, app.cursor(), cx);
    else setZoom(pps * factor, timeAtF(headerW + viewW() * 0.5), headerW + viewW() * 0.5);
}

void TimelineWidget::Impl::zoomLarge(bool in)
{
    if (in && app.loopEnd > app.loopStart) {
        const double len = app.loopEnd - app.loopStart;
        pps = clampd(viewW() / (len * 1.1), minPps(), maxPps());
        setViewStart(app.loopStart - len * 0.05);
        return;
    }
    zoomBy(in ? 4.0 : 0.25);
}

void TimelineWidget::Impl::zoomTracks(int a, int b)
{
    if (a < 0 || b < a || b >= (int)rows.size()) return;
    const Project& p = app.project();
    double sum = 0;
    for (int k = a; k <= b; ++k) sum += rows[(size_t)k].h;
    const double avail = std::max(1, lanesBottom() - kTopH);
    if (!(sum > 0)) return;
    const double f = clampd(avail / sum, 0.1, 40.0);
    minimized.clear();
    maximized.clear();
    std::map<uint64_t, int> next;
    for (const Track& t : p.tracks) {
        const int def = t.type == TrackType::Video ? kVideoH : kAudioH;
        const int h = clampTrackH((int)std::lround(trackHeight(t) * f));
        const int value = h == def ? 0 : h;
        if (value != t.height) next[t.id] = value;
    }
    if (!next.empty())
        app.edit("Zoom Track Height", [&](Project& pr) {
            for (Track& t : pr.tracks) {
                auto it = next.find(t.id);
                if (it != next.end()) t.height = it->second;
            }
        });
    layout();
    updateBars();
    if (a < (int)rows.size()) setScrollY(rows[(size_t)a].y);
    q->update();
}

void TimelineWidget::Impl::zoomToFit()
{
    if (q->width() <= headerW + 40) { fitWanted = true; return; }
    fitWanted = false;
    const double len = app.project().length();
    pps = viewW() / ((len > 0 ? len : 30.0) * 1.04);
    pps = std::min(pps, maxPps());
    viewStart = 0;
    updateBars();
    q->update();
}

void TimelineWidget::Impl::scrollToCursor()
{
    const double x = xAtF(app.cursor());
    const double viewSeconds = viewW() / pps;
    if (x > lanesRight() - 1) setViewStart(app.cursor() - viewSeconds * 0.9);
    else if (x < headerW) setViewStart(app.cursor() - viewSeconds * 0.1);
}

const Event* TimelineWidget::Impl::fadesFrom(const Track& tr, const Event& e)
{
    const Event* best = nullptr;
    for (const Event& o : tr.events) {
        if (o.id == e.id) continue;
        const bool first = o.start < e.start || (o.start == e.start && o.id < e.id);
        if (first && o.end() > e.start && o.end() < e.end())
            if (!best || o.end() > best->end()) best = &o;
    }
    return best;
}

// fadesFrom for every event of a track at once (by index into tr.events; null where none), for drawing a track of
// thousands of events zoomed out, where asking one by one would scan the track thousands of times. Same answers: the
// events before an event (by start, then id) are swept through keeping their ends; the one wanted is the latest end
// inside the event.
void TimelineWidget::Impl::fadesFromAll(const Track& tr, std::vector<const Event*>& out)
{
    const size_t n = tr.events.size();
    out.assign(n, nullptr);
    std::vector<size_t> order(n);
    for (size_t k = 0; k < n; ++k) order[k] = k;
    auto before = [&](size_t a, size_t b) {
        const Event& x = tr.events[a];
        const Event& y = tr.events[b];
        return x.start < y.start || (x.start == y.start && x.id < y.id);
    };
    std::sort(order.begin(), order.end(), before);
    std::set<std::pair<double, size_t>> ends;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && !before(order[i], order[j]) && !before(order[j], order[i])) ++j;
        for (size_t k = i; k < j; ++k) {
            const Event& e = tr.events[order[k]];
            auto it = ends.lower_bound(std::make_pair(e.end(), (size_t)0));
            if (it == ends.begin()) continue;
            --it;
            if (!(it->first > e.start)) continue;
            auto first = ends.lower_bound(std::make_pair(it->first, (size_t)0));
            out[order[k]] = tr.events[first->second].id != e.id ? &tr.events[first->second] : fadesFrom(tr, e);
        }
        for (size_t k = i; k < j; ++k) ends.insert(std::make_pair(tr.events[order[k]].end(), order[k]));
        i = j;
    }
}

void TimelineWidget::Impl::ensureIndex(bool thorough) const
{
    const Project& p = app.project();
    bool ok = indexValid && indexRevision == app.revision() && evIndex.size() == p.tracks.size();
    if (ok && !thorough) return;
    for (size_t t = 0; ok && t < p.tracks.size(); ++t) ok = evIndex[t].count == p.tracks[t].events.size();
    if (ok) return;
    evIndex.assign(p.tracks.size(), TrackIndex());
    for (size_t t = 0; t < p.tracks.size(); ++t) {
        const std::vector<Event>& ev = p.tracks[t].events;
        TrackIndex& ix = evIndex[t];
        ix.count = ev.size();
        ix.maxEnd.resize(ev.size());
        double m = -1e300;
        for (size_t k = 0; k < ev.size(); ++k) {
            const double end = ev[k].end();
            if (end > m) m = end;
            ix.maxEnd[k] = m;
            if (!(ev[k].start == ev[k].start) || !(end == end) || (k > 0 && ev[k].start < ev[k - 1].start)) ix.sorted = false;
        }
    }
    mediaIndex.clear();
    for (size_t k = 0; k < p.media.size(); ++k) mediaIndex.emplace(p.media[k].id, k);
    mediaKeyOf.clear();
    eventPlaces.clear();
    indexRevision = app.revision();
    indexValid = true;
}

// The events of track 't' (an index into the project's tracks) that can reach into [from, to]: indices [first, last)
// into its events, in their order. Those in that range that end before 'from' still have to be passed over (an
// earlier, longer event keeps the range open).
std::pair<size_t, size_t> TimelineWidget::Impl::eventRange(size_t t, double from, double to) const
{
    ensureIndex();
    const std::vector<Event>& ev = app.project().tracks[t].events;
    const TrackIndex& ix = evIndex[t];
    if (!ix.sorted) return {0, ev.size()};
    const size_t a = (size_t)(std::lower_bound(ix.maxEnd.begin(), ix.maxEnd.end(), from) - ix.maxEnd.begin());
    const size_t b = (size_t)(std::upper_bound(ev.begin(), ev.end(), to, [](double v, const Event& e) { return v < e.start; }) - ev.begin());
    return {a, std::max(a, b)};
}

const MediaItem* TimelineWidget::Impl::mediaOf(uint64_t id) const
{
    ensureIndex();
    const Project& p = app.project();
    auto it = mediaIndex.find(id);
    if (it != mediaIndex.end() && it->second < p.media.size() && p.media[it->second].id == id) return &p.media[it->second];
    return it == mediaIndex.end() ? nullptr : p.findMedia(id);
}

const Event* TimelineWidget::Impl::fadeSource(size_t t, size_t k) const
{
    ensureIndex();
    const Track& tr = app.project().tracks[t];
    const Event& e = tr.events[k];
    TrackIndex& ix = evIndex[t];
    if (ix.sorted) {
        const size_t a = (size_t)(std::upper_bound(ix.maxEnd.begin(), ix.maxEnd.end(), e.start) - ix.maxEnd.begin());
        const size_t b = (size_t)(std::upper_bound(tr.events.begin(), tr.events.end(), e.start, [](double v, const Event& o) { return v < o.start; }) -
                                  tr.events.begin());
        if (b <= a + 64) {
            const Event* best = nullptr;
            for (size_t j = a; j < b; ++j) {
                const Event& o = tr.events[j];
                if (o.id == e.id) continue;
                const bool first = o.start < e.start || (o.start == e.start && o.id < e.id);
                if (first && o.end() > e.start && o.end() < e.end())
                    if (!best || o.end() > best->end()) best = &o;
            }
            return best;
        }
    }
    if (!ix.fadesMade) {
        std::vector<const Event*> all;
        fadesFromAll(tr, all);
        ix.fades.assign(all.size(), -1);
        for (size_t j = 0; j < all.size(); ++j)
            if (all[j]) ix.fades[j] = (int)(all[j] - tr.events.data());
        ix.fadesMade = true;
    }
    return k < ix.fades.size() && ix.fades[k] >= 0 ? &tr.events[(size_t)ix.fades[k]] : nullptr;
}

// A marker's tag in the marker bar (a region's start tag): a little flag with its number. Markers and regions are
// numbered in the order they are kept, from 1, as the number keys find them.
QRectF TimelineWidget::Impl::markerTag(int index, const Project::Marker& m) const
{
    const QFontMetrics fm(smallFont);
    const double w = fm.horizontalAdvance(QString::number(index + 1)) + 9.0;
    return QRectF(std::floor(xAtF(m.time)), kMarkerTop + 2.0, w, kMarkerH - 4.0);
}

int TimelineWidget::Impl::markerIndex(uint64_t id) const
{
    const std::vector<Project::Marker>& ms = app.project().markers;
    for (size_t k = 0; k < ms.size(); ++k) if (ms[k].id == id) return (int)k;
    return -1;
}

void TimelineWidget::Impl::updateFonts()
{
    if (fontsMade && fontsFor == q->font()) return;
    fontsMade = true;
    fontsFor = q->font();
    rulerLabelW = 0;
    titles.clear();
    ++headerGeneration;
    smallFont = q->font();
    if (smallFont.pixelSize() > 0) smallFont.setPixelSize(std::max(8, (int)std::lround(smallFont.pixelSize() * 0.9)));
    else if (smallFont.pointSizeF() > 0) smallFont.setPointSizeF(std::max(6.5, smallFont.pointSizeF() * 0.9));
    rulerFont = smallFont;
    for (int px = 11; px >= 7; --px) {
        rulerFont.setPixelSize(px);
        if (QFontMetrics(rulerFont).horizontalAdvance(QString("00:00:00:00")) <= 52) break;
    }
    smallFm = QFontMetrics(smallFont);
}

int TimelineWidget::Impl::segmentAt(const Envelope& e, double t)
{
    int k = -1;
    for (size_t i = 0; i < e.points.size(); ++i)
        if (e.points[i].time <= t + 1e-12) k = (int)i;
    return k;
}

std::vector<TimelineWidget::Impl::EnvLane> TimelineWidget::Impl::envLanes() const
{
    std::vector<EnvLane> out;
    if (!opt.showEnvelopes) return out;
    const Project& pr = app.project();
    const QRectF lanes(lanesRect());
    auto addList = [&](const Envelopes& list, const EnvelopeOwner& owner, const QRectF& area, const QRectF& clip, double origin, double from, double to) {
        for (const Envelope& e : list) {
            if (!e.visible || e.points.empty()) continue;
            EnvLane L;
            L.ref.owner = owner;
            L.ref.kind = e.kind;
            L.ref.target = e.target;
            L.ref.param = e.param;
            L.env = &e;
            L.area = area;
            L.clip = clip.intersected(lanes);
            L.origin = origin;
            L.from = from;
            L.to = to;
            if (!L.clip.isEmpty()) out.push_back(L);
        }
    };
    for (const Row& r : rows) {
        const Track& tr = pr.tracks[(size_t)r.index];
        if (tr.envelopes.empty()) continue;
        EnvelopeOwner o;
        o.kind = EnvelopeOwner::Track;
        o.id = tr.id;
        const QRectF clip(headerW, rowTop(r), viewW(), r.h - 1);
        addList(tr.envelopes, o, envArea(clip), clip, 0.0, -1e300, 1e300);
    }
    for (const BusRow& b : busRows) {
        const Envelopes* list = AppState::envelopesOf(pr, b.owner);
        if (!list || list->empty()) continue;
        const QRectF clip(headerW, busTop(b), viewW(), b.h - 1);
        addList(*list, b.owner, envArea(clip), clip, 0.0, -1e300, 1e300);
    }
    const double vf = timeAtF(headerW - 20.0), vt = timeAtF(lanesRight() + 20.0);
    for (const Row& r : rows) {
        const Track& tr = pr.tracks[(size_t)r.index];
        const std::pair<size_t, size_t> range = eventRange((size_t)r.index, vf, vt);
        for (size_t k = range.first; k < range.second; ++k) {
            const Event& e = tr.events[k];
            if (e.envelopes.empty() || e.end() < vf || e.start > vt) continue;
            const QRectF er = eventRect(r, e);
            const double titleH = eventTitleH(er);
            const QRectF content(er.left(), er.top() + titleH, er.width(), er.height() - titleH);
            EnvelopeOwner o;
            o.kind = EnvelopeOwner::Event;
            o.id = e.id;
            addList(e.envelopes, o, envArea(content), content, e.start, e.start, e.end());
        }
    }
    std::stable_partition(out.begin(), out.end(), [&](const EnvLane& L) { return L.ref != frontEnv; });
    return out;
}

bool TimelineWidget::Impl::envLaneOf(const EnvelopeRef& ref, EnvLane* out) const
{
    for (const EnvLane& L : envLanes())
        if (L.ref == ref) { *out = L; return true; }
    return false;
}

bool TimelineWidget::Impl::hitEnvelope(const QPointF& pos, Hit& h, bool points, bool lines) const
{
    if (!opt.showEnvelopes) return false;
    const std::vector<EnvLane> lanes = envLanes();
    if (points)
        for (size_t k = lanes.size(); k-- > 0;) {
            const EnvLane& L = lanes[k];
            if (!L.clip.adjusted(-5, -5, 5, 5).contains(pos)) continue;
            int best = -1;
            double bestD = 5.5;
            for (size_t i = 0; i < L.env->points.size(); ++i) {
                if (!pointShown(L, i)) continue;
                const QPointF q = envPointPos(L, i);
                const double d = std::max(std::fabs(q.x() - pos.x()), std::fabs(q.y() - pos.y()));
                if (d < bestD) { bestD = d; best = (int)i; }
            }
            if (best >= 0) {
                h.part = Part::EnvPoint;
                h.env = L.ref;
                h.point = best;
                return true;
            }
        }
    if (lines)
        for (size_t k = lanes.size(); k-- > 0;) {
            const EnvLane& L = lanes[k];
            if (!L.clip.contains(pos)) continue;
            const double t = timeAtF(pos.x());
            if (t < L.from || t > L.to) continue;
            if (std::fabs(envY(L, envValueAt(L, t)) - pos.y()) > 4.0) continue;
            h.part = Part::EnvLine;
            h.env = L.ref;
            h.point = segmentAt(*L.env, t - L.origin);
            return true;
        }
    return false;
}

void TimelineWidget::Impl::drawEnvelope(QPainter& p, const EnvLane& L, const QRect& c)
{
    if (L.clip.intersected(QRectF(c).adjusted(-6, -6, 6, 6)).isEmpty()) return;
    // the line is computed over the whole view, not just what this paint covers, so a part painted on its own looks
    // exactly as in a whole repaint
    const QRectF clip = L.clip.intersected(QRectF(lanesRect()).adjusted(-6, -6, 6, 6));
    p.save();
    p.setClipRect(L.clip.adjusted(0, -1, 0, 1).intersected(QRectF(lanesRect())));
    p.setRenderHint(QPainter::Antialiasing, true);
    const bool front = L.ref == frontEnv || hover.env == L.ref || drag.env == L.ref;
    QColor col = envelopeColor(L.ref.kind);
    if (L.ref.owner.kind == EnvelopeOwner::Track)
        if (const Track* ot = findTrackConst(L.ref.owner.id))
            if (ot->automationOff()) col = mix(col, QColor(70, 70, 70), 0.65);
    const double xa = std::max(clip.left(), xAtF(L.from)), xb = std::min(clip.right(), xAtF(L.to));
    const std::vector<EnvPoint>& pts = L.env->points;
    const double dflt = AppState::envelopeDefault(L.ref.kind);
    if (xb > xa) {
        QPolygonF line;
        size_t pi = 0;
        while (pi < pts.size() && xAtF(L.origin + pts[pi].time) < xa) ++pi;
        // sampled every 2 pixels on a grid that keeps to the timeline rather than to the view's edge, so a line moved
        // across by a scroll matches what is drawn beside it
        const double grid = xAtF(0.0);
        auto nextX = [&](double x) { return std::min(grid + 2.0 * (std::floor((x - grid) / 2.0 + 1e-9) + 1.0), xb); };
        for (double x = xa;; x = nextX(x)) {
            for (; pi < pts.size() && xAtF(L.origin + pts[pi].time) <= x; ++pi) {
                const double px = xAtF(L.origin + pts[pi].time);
                line << QPointF(px, envY(L, L.env->valueAt(pts[pi].time - 1e-9, dflt))) << QPointF(px, envY(L, pts[pi].value));
            }
            line << QPointF(x, envY(L, envValueAt(L, timeAtF(x))));
            if (x >= xb) break;
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 140), front ? 3.8 : 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
        p.setPen(QPen(front ? col.lighter(125) : col, front ? 2.0 : 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    for (size_t i = 0; i < pts.size(); ++i) {
        if (!pointShown(L, i)) continue;
        const QPointF q = envPointPos(L, i);
        if (q.x() < clip.left() - 5 || q.x() > clip.right() + 5) continue;
        const bool sel = selEnv == L.ref && selPoints.count((int)i);
        const bool hot = hover.part == Part::EnvPoint && hover.env == L.ref && hover.point == (int)i;
        const QRectF box(std::floor(q.x()) - 3.0, std::floor(q.y()) - 3.0, 7.0, 7.0);
        p.fillRect(box, QColor(10, 10, 10));
        p.fillRect(box.adjusted(1, 1, -1, -1), sel ? QColor(255, 255, 255) : (hot ? col.lighter(160) : col));
    }
    p.restore();
}

void TimelineWidget::Impl::drawEnvelopeLabel(QPainter& p)
{
    if (envLabel.isEmpty()) return;
    const QRectF box = envLabelBox();
    p.save();
    p.setPen(QColor(20, 20, 20));
    p.setBrush(QColor(255, 255, 225));
    p.drawRect(box);
    p.setFont(smallFont);
    p.drawText(box, Qt::AlignCenter, envLabel);
    p.restore();
}

Hit TimelineWidget::Impl::hitTest(const QPointF& pos) const
{
    Hit h;
    const double x = pos.x(), y = pos.y();
    if (x < 0 || y < 0 || x >= q->width() || y >= lanesBottom()) return h;
    if (y < kTopH) {
        if (x < headerW) h.part = Part::Corner;
        else if (x < lanesRight()) {
            if (y >= kRulerTop) h.part = Part::Ruler;
            else if (y >= kLoopTop) h.part = Part::LoopStrip;
            else {
                h.part = Part::MarkerBar;
                const std::vector<Project::Marker>& ms = app.project().markers;
                for (size_t k = ms.size(); k-- > 0;) {
                    if (ms[k].region && regionEndTag(ms[k]).contains(pos)) { h.part = Part::RegionEndTag; h.marker = ms[k].id; break; }
                    if (markerTag((int)k, ms[k]).contains(pos)) { h.part = Part::MarkerTag; h.marker = ms[k].id; break; }
                }
            }
        }
        return h;
    }
    if (x >= lanesRight()) return h;
    h.row = rowAtY(y);
    if (h.row >= 0) h.track = rows[(size_t)h.row].id;
    if (x >= headerW - kHeaderGap && x < headerW) {
        h.part = Part::HeaderGap;
        return h;
    }
    if (x < headerW - kHeaderGap) {
        for (size_t k = 0; k < rows.size(); ++k) {
            if (rows[k].h <= 0) continue;
            const int bottom = rowTop(rows[k]) + rows[k].h;
            if (y >= bottom - kTrackEdgePx && y < bottom + 2) {
                h.part = Part::HeaderEdge;
                h.row = (int)k;
                h.track = rows[k].id;
                return h;
            }
        }
    }
    if (h.row < 0) {
        const int b = busRowAtY(y);
        if (b >= 0) {
            h.bus = busRows[(size_t)b].owner;
            if (x < headerW) {
                h.part = Part::BusHeader;
                return h;
            }
            Hit eh = h;
            if (hitEnvelope(pos, eh, true, true)) return eh;
            h.part = Part::BusLane;
            return h;
        }
    }
    if (x < headerW) {
        if (h.row < 0) return h;
        const Row& r = rows[(size_t)h.row];
        const HeaderRects hr = headerRects(r);
        const QPoint p = pos.toPoint();
        h.part = Part::HeaderBody;
        if (hr.mute.contains(p)) h.part = Part::HeaderMute;
        else if (hr.solo.contains(p)) h.part = Part::HeaderSolo;
        else if (hr.fx.contains(p)) h.part = Part::HeaderFx;
        else if (r.video && hr.motion.contains(p)) h.part = Part::HeaderMotion;
        else if (!r.video && hr.arm.contains(p)) h.part = Part::HeaderArm;
        else if (!hr.autoBtn.isEmpty() && hr.autoBtn.contains(p)) h.part = Part::HeaderAuto;
        else if (!hr.phase.isEmpty() && hr.phase.contains(p)) h.part = Part::HeaderPhase;
        else if (!hr.bus.isEmpty() && hr.bus.contains(p)) h.part = Part::HeaderBus;
        else if (!hr.fade.isEmpty() && hr.fade.adjusted(-7, -2, 7, 2).contains(p)) h.part = Part::HeaderFade;
        else if (hr.menu.contains(p)) h.part = Part::HeaderMenu;
        else if (hr.name.contains(p)) h.part = Part::HeaderName;
        else if (!hr.level.isEmpty() && hr.level.adjusted(-7, -2, 7, 2).contains(p)) h.part = Part::HeaderLevel;
        else if (!hr.pan.isEmpty() && hr.pan.adjusted(-7, -2, 7, 2).contains(p)) h.part = Part::HeaderPan;
        else if (!hr.surround.isEmpty() && hr.surround.contains(p)) h.part = Part::HeaderSurround;
        return h;
    }
    h.part = Part::Lane;
    if (h.row < 0) return h;
    const Row& r = rows[(size_t)h.row];
    const Track* tr = trackOf(r);
    if (!tr) return h;
    if (hitTrackKey(pos, r, *tr, h)) return h;
    {
        Hit eh = h;
        if (hitEnvelope(pos, eh, true, false)) return eh;
    }
    // First the parts that are taken hold of: fade handles, edges, the FX button, whichever is nearest. An edge counts
    // even where another event lies over it (there would be no other way to get at it), but not with the envelope tool.
    double best = 1e9;
    uint64_t gainEvent = 0;
    for (size_t k = opt.tool == EditTool::Envelope ? 0 : tr->events.size(); k-- > 0;) {
        const Event& e = tr->events[k];
        const QRectF er = eventRect(r, e);
        if (x < er.left() - kEdgePx - 2 || x > er.right() + kEdgePx + 2 || y < er.top() || y > er.bottom()) continue;
        const bool inside = x >= er.left() && x <= er.right();
        auto consider = [&](Part part, double distance) {
            const double score = distance + (inside ? 0.0 : 0.5);
            if (score < best) { best = score; h.part = part; h.event = e.id; }
        };
        if (opt.showEventButtons) {
            if (hasMenuButton(er) && menuButtonRect(er).contains(pos)) consider(Part::EventMenu, -1.0);
            if (hasFxButton(er) && fxButtonRect(er).contains(pos)) consider(Part::EventFx, -1.0);
            if (hasCropButton(er, r.video) && cropButtonRect(er).contains(pos)) consider(Part::EventCrop, -1.0);
        }
        if (hasFadeHandles(er) && y < er.top() + kHandlePx) {
            const double fi = std::floor(xAtF(e.start + e.fadeIn)), fo = std::floor(xAtF(e.end() - e.fadeOut));
            if (x >= fi - 5 && x <= fi + (e.fadeIn > 0 ? 5 : 8)) consider(Part::FadeIn, std::fabs(x - fi) - 0.25);
            if (x <= fo + 5 && x >= fo - (e.fadeOut > 0 ? 5 : 8)) consider(Part::FadeOut, std::fabs(x - fo) - 0.25);
        }
        if (!gainEvent && inside && hasGainLine(er) && x > er.left() + 6.0 && x < er.right() - 6.0 && std::fabs(y - gainLineY(er, e.gain)) <= kGainGrabPx)
            gainEvent = e.id;
        if (inside && er.width() > 24.0 && y > er.bottom() - 8.0 && e.length > 0) {
            const double fx = std::floor(xAtF(e.start + clampd(e.snapOffset, 0.0, e.length)));
            if (x >= fx - 1.0 && x <= fx + 7.0) consider(Part::EventSnap, -0.5);
        }
        const double inner = std::min(kEdgePx, er.width() / 3.0);
        if (x >= er.left() - kEdgePx && x <= er.left() + inner) consider(Part::EventLeft, std::fabs(x - er.left()));
        if (x <= er.right() + kEdgePx && x >= er.right() - inner) consider(Part::EventRight, std::fabs(x - er.right()));
    }
    if (h.event) return h;
    {
        Hit eh = h;
        if (hitEnvelope(pos, eh, false, true)) return eh;
    }
    if (gainEvent) {
        h.part = Part::EventGain;
        h.event = gainEvent;
        return h;
    }
    for (size_t k = tr->events.size(); k-- > 0;) {
        const Event& e = tr->events[k];
        if (eventRect(r, e).contains(pos)) {
            h.part = Part::EventBody;
            h.event = e.id;
            return h;
        }
    }
    return h;
}

// The times that things snap to: zero, the cursor, the ends of the loop region, the edges of every event not being
// dragged, the markers and region ends (Snap to Markers) and the ruler's numbered marks in view (Snap to Grid).
std::vector<double> TimelineWidget::Impl::snapTargets(const std::set<uint64_t>& moving, bool withCursor, uint64_t movingMarker) const
{
    std::vector<double> v;
    v.push_back(0.0);
    if (withCursor) v.push_back(app.cursor());
    if (app.loopEnd > app.loopStart) {
        v.push_back(app.loopStart);
        v.push_back(app.loopEnd);
    }
    std::set<uint64_t> own;
    if (!opt.snapToAllEvents) {
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (moving.count(e.id)) own.insert(t.id);
        if (own.empty()) own.insert(app.currentTrack);
    }
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (!moving.count(e.id) && (opt.snapToAllEvents || own.count(t.id))) {
                v.push_back(e.start);
                v.push_back(e.end());
            }
    if (opt.snapToMarkers)
        for (const auto& m : app.project().markers)
            if (m.id != movingMarker) {
                v.push_back(m.time);
                if (m.region) v.push_back(m.time + m.length);
            }
    // the media markers on the events (Beat Detection's, saved from the Trimmer, the file's own) are snap objects too
    // while shown (View > Event Media Markers)
    if (opt.snapToMarkers && opt.showMediaMarkers)
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events) {
                if (moving.count(e.id) || e.rate <= 0) continue;
                const MediaItem* m = mediaOf(e.mediaId);
                if (!m) continue;
                auto add = [&](double mediaTime) {
                    const double at = e.start + (mediaTime - e.offset) / e.rate;
                    if (at >= e.start && at <= e.end()) v.push_back(at);
                };
                for (const auto& mk : m->info.markers) add(mk.first);
                for (const MediaItem::Marker& mk : m->markers) add(mk.time);
            }
    if (opt.snapToGrid) {
        const double step = gridStep(2.0);
        const double from = std::max(0.0, viewStart - viewW() / pps), to = viewStart + 2.0 * viewW() / pps;
        for (int64_t k = (int64_t)std::floor(from / step); (double)k * step <= to; ++k) v.push_back((double)k * step);
    }
    std::sort(v.begin(), v.end());
    return v;
}

// The grid's spacing in seconds (Options > Grid Spacing), at least 'minPx' pixels apart at this zoom (a fine grid zoomed
// out goes by whole multiples of itself so it is never drawn as a grey wash).
double TimelineWidget::Impl::gridStep(double minPx) const
{
    const double f = fps();
    if (opt.gridSpacing == "ruler" || opt.gridSpacing.empty()) return (double)rulerStep(pps, f, rulerLabelWidth()) / f;
    const ProjectSettings& s = app.project().settings;
    const double beat = 60.0 / (s.tempo > 0 ? s.tempo : 120.0);
    const std::string& g = opt.gridSpacing;
    double step = g == "minutes" ? 60.0 : g == "seconds" ? 1.0 : g == "milliseconds" ? 0.001 : g == "frames" ? 1.0 / f
                : g == "measures" ? beat * std::max(1, s.beatsPerMeasure) : g == "quarter" ? beat : g == "quarter3" ? beat * 2.0 / 3.0
                : g == "8th" ? beat / 2.0 : g == "8th3" ? beat / 3.0 : g == "16th" ? beat / 4.0 : g == "16th3" ? beat / 6.0
                : g == "32nd" ? beat / 8.0 : g == "32nd3" ? beat / 12.0 : 0.0;
    if (!(step > 0)) return (double)rulerStep(pps, f, rulerLabelWidth()) / f;
    const double base = step;
    for (int k = 2; step * pps < minPx && k < 1000000; k = k < 10 ? k + (k < 5 ? 3 : 5) : k * 10) step = base * k;
    return step;
}

double TimelineWidget::Impl::quantized(double t) const
{
    if (!opt.quantize) return t;
    if (app.prefs.noQuantizeAudioOnly && drag.kind != Drag::None && dragIsAudioOnly()) return t;
    return app.project().quantize(t);
}

bool TimelineWidget::Impl::dragIsAudioOnly() const
{
    if (drag.audioOnly < 0) {
        drag.audioOnly = drag.movers.empty() ? 0 : 1;
        const Project& p = app.project();
        for (const Mover& m : drag.movers)
            if (m.track >= 0 && m.track < (int)p.tracks.size() && p.tracks[(size_t)m.track].type != TrackType::Audio) drag.audioOnly = 0;
    }
    return drag.audioOnly == 1;
}

bool TimelineWidget::Impl::nearest(const std::vector<double>& targets, double t, double* out) const
{
    const double reach = std::max(1, std::min(64, vprefs::number("prefs/snapDistance"))) / pps;
    auto it = std::lower_bound(targets.begin(), targets.end(), t);
    bool found = false;
    double best = reach;
    if (it != targets.end() && *it - t <= best) { best = *it - t; *out = *it; found = true; }
    if (it != targets.begin() && t - *(it - 1) <= best) { *out = *(it - 1); found = true; }
    return found;
}

double TimelineWidget::Impl::snapped(const std::vector<double>& targets, double t, Qt::KeyboardModifiers mods, double* line) const
{
    *line = -1;
    double target = 0;
    if (snapOn(mods) && nearest(targets, t, &target)) {
        t = target;
        *line = target;
    }
    return std::max(0.0, quantized(t));
}

}  // namespace app
}  // namespace reno
