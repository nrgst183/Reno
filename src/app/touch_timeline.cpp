#include "touch_ui.h"
#if RENO_TOUCH_UI  // (the whole file: phones and tablets only)
#include "touch_timeline.h"
#include "appstate.h"
#include "medialoader.h"
#include "playback.h"
#include "touch_style.h"
#include "../engine/timefmt.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStaticText>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <tuple>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>

namespace reno {
namespace app {

using namespace touch;

namespace {

constexpr double kMinPps = 0.5, kMaxPps = 900.0;
constexpr double kSlop = 8;                 // a finger moving less than this is still a tap or a hold
constexpr int kLongPressMs = 420;
constexpr int kAddRowH = 48;
constexpr double kSnapPx = 12;              // how close an edge comes before it snaps
constexpr double kEdgeScrollPx = 44;        // dragging this close to the side scrolls the timeline
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

// The text on an event: its own name, else the file's name without the folder.
QString eventLabel(const Event& e, const MediaItem* m)
{
    if (!e.name.empty()) return QString::fromStdString(e.name);
    if (!m) return QString();
    if (!m->name.empty()) return QString::fromStdString(m->name);
    QString path = QString::fromStdString(m->path);
    const int slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

// Ruler marks: the step between labelled marks that leaves at least 'minPx' between labels.
double rulerStep(double pps, double fps, double minPx)
{
    static const double steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
    const double frame = 1.0 / std::max(1.0, fps);
    for (int frames : {1, 2, 5, 10})
        if (frames * frame < 1.0 && frames * frame * pps >= minPx) return frames * frame;
    for (double s : steps)
        if (s * pps >= minPx) return s;
    return 7200;
}

QString rulerLabel(double t, double step, double fps)
{
    const int64_t whole = (int64_t)std::floor(t + 1e-6);
    const int m = (int)(whole / 60), s = (int)(whole % 60);
    if (step < 1.0) {
        const int f = (int)std::lround((t - whole) * fps);
        return QString("%1:%2.%3").arg(m).arg(s, 2, 10, QChar('0')).arg(f, 2, 10, QChar('0'));
    }
    if (m >= 60) return QString("%1:%2:%3").arg(m / 60).arg(m % 60, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

// The media time between thumbnails at this zoom, rounded to a step that stays the same while scrolling, so the same
// pictures are asked for again (and found in the cache) instead of a new one for every pixel moved.
double thumbGrain(double tileSeconds, double fps)
{
    const double frame = fps > 0 ? 1.0 / fps : 1.0 / 30;
    static const double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60, 120, 300, 600};
    if (tileSeconds <= frame) return frame;
    for (double s : steps)
        if (s >= tileSeconds * 0.5) return std::max(frame, s);
    return 600;
}

}  // namespace

struct TouchTimeline::Impl {
    TouchTimeline* q;
    Playback* pb;
    AppState& app = AppState::get();
    bool tablet = false;
    bool multi = false;                     // Select several
    double pps = 40;
    double scrollY = 0;
    int listener = 0;

    // ---- pictures of the media (thumbnails and waveforms come from the same loader thread as the desktop timeline's)
    std::unique_ptr<tl::Loader> loader;
    std::map<std::string, int> mediaKeys;
    struct Thumb { QImage image; uint64_t used = 0; };
    std::map<tl::ThumbKey, Thumb> thumbs;
    uint64_t paints = 0;
    QTimer* loaderRepaint = nullptr;        // gathers the loader's many small news into one repaint

    // ---- rows, worked out from the project when needed
    struct Row { uint64_t track; bool video; double y, h; };
    std::vector<Row> rows;
    double contentH = 0;
    uint64_t rowsFor = ~0ull;               // AppState::revision() they were made for
    double rowsScroll = kNaN;
    int rowsWidth = -1;

    // ---- what a finger is doing
    enum class Mode { Idle, Pending, Scroll, Pinch, Trim, Move };
    enum class Part { None, Ruler, Header, AddRow, Empty, Event, HandleL, HandleR, Hint };
    struct Hit { Part part = Part::None; uint64_t track = 0, event = 0; };
    struct Finger { int id; QPointF start, pos; };
    Mode mode = Mode::Idle;
    std::vector<Finger> fingers;
    Hit pressHit;
    QElapsedTimer pressClock;
    QTimer* longPress = nullptr;
    qint64 lastTapMs = -100000;
    QPointF lastTapPos;
    // scrolling
    double scrollStartTime = 0, scrollStartY = 0;
    int axis = 0;                           // 0 not chosen yet, 1 across (time), 2 up and down (tracks)
    std::deque<std::pair<qint64, QPointF>> samples;     // recent finger positions, for the flick's speed
    QPointF flingV;                         // pixels per second
    qint64 flingLast = 0;
    // pinching
    double pinchDist = 1, pinchPps = 40, pinchMidY = 0, pinchScrollY = 0;
    // trimming and moving
    struct Orig { uint64_t id; uint64_t track; double start, length, offset; };
    std::vector<Orig> orig;                 // what is changed, as it was when the drag began
    uint64_t grabbed = 0;                   // the event the finger is on
    double grabTime = 0;                    // the time under the finger when the drag began
    double viewAtGrab = 0;
    std::vector<double> snapTimes;
    double snapAt = kNaN;                   // a snapped edge, for the guide line
    QTimer* tick = nullptr;                 // playback following, flicks and edge scrolling
    QPointF lastFingerPos;

    Impl(TouchTimeline* self, Playback* playback) : q(self), pb(playback)
    {
        loaderRepaint = new QTimer(q);
        loaderRepaint->setSingleShot(true);
        loaderRepaint->setInterval(40);
        QObject::connect(loaderRepaint, &QTimer::timeout, q, [this] { q->update(); });
        longPress = new QTimer(q);
        longPress->setSingleShot(true);
        longPress->setInterval(kLongPressMs);
        QObject::connect(longPress, &QTimer::timeout, q, [this] { longPressed(); });
        tick = new QTimer(q);
        tick->setInterval(16);
        tick->setTimerType(Qt::PreciseTimer);
        QObject::connect(tick, &QTimer::timeout, q, [this] { onTick(); });
    }

    void ensureLoader()
    {
        if (loader) return;
        loader.reset(new tl::Loader(app.mediaOpener, [this] { loaderReady(); }));
    }

    void loaderReady()
    {
        if (!loader) return;
        for (tl::Loader::Done& d : loader->take()) {
            Thumb t;
            t.image = std::move(d.image);
            t.used = paints;
            thumbs[d.key] = std::move(t);
        }
        loader->takeDropped();
        loader->takeProbed();
        if (!loaderRepaint->isActive()) loaderRepaint->start();
    }

    int mediaKey(const MediaItem& m)
    {
        const std::string name = m.generated ? "generated:" + std::to_string(m.id) : m.path;
        auto it = mediaKeys.find(name);
        if (it != mediaKeys.end()) return it->second;
        const int k = (int)mediaKeys.size() + 1;
        mediaKeys[name] = k;
        return k;
    }

    const QImage* thumb(const tl::ThumbKey& key, const MediaItem& m, double seconds, int w, int h)
    {
        auto it = thumbs.find(key);
        if (it != thumbs.end()) {
            it->second.used = paints;
            return it->second.image.isNull() ? nullptr : &it->second.image;
        }
        loader->requestThumb(key, m, seconds, w, h);
        // until it comes, the nearest one of the same file and size
        it = thumbs.lower_bound(key);
        if (it != thumbs.end() && it->first.media == key.media && it->first.height == key.height && !it->second.image.isNull())
            return &it->second.image;
        if (it != thumbs.begin()) {
            --it;
            if (it->first.media == key.media && it->first.height == key.height && !it->second.image.isNull()) return &it->second.image;
        }
        return nullptr;
    }

    void trimThumbs()
    {
        if (thumbs.size() < 500) return;
        std::vector<std::pair<uint64_t, tl::ThumbKey>> byUse;
        for (const auto& t : thumbs) byUse.push_back({t.second.used, t.first});
        std::sort(byUse.begin(), byUse.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < byUse.size() / 2; ++k) thumbs.erase(byUse[k].second);
    }

    // ---- geometry
    double eventsLeft() const { return kHeaderW; }
    double playX() const { return kHeaderW + (q->width() - kHeaderW) * 0.5; }
    double viewTime() const { return pb && pb->playing() ? pb->position() : app.cursor(); }
    double xAt(double t) const { return playX() + (t - viewTime()) * pps; }
    double timeAt(double x) const { return viewTime() + (x - playX()) / pps; }

    void layoutRows()
    {
        const uint64_t rev = app.revision();
        if (rev == rowsFor && rowsScroll == scrollY && rowsWidth == q->width()) return;
        rowsFor = rev;
        rowsWidth = q->width();
        rows.clear();
        double y = 0;
        for (const Track& t : app.project().tracks) {
            const bool video = t.type == TrackType::Video;
            const double h = video ? videoTrackHeight(tablet) : audioTrackHeight(tablet);
            rows.push_back({t.id, video, y, h});
            y += h;
        }
        contentH = y + kAddRowH;
        clampScrollY();
        for (Row& r : rows) r.y += kRulerH - scrollY;
        rowsScroll = scrollY;
    }

    void clampScrollY()
    {
        const double room = q->height() - kRulerH;
        scrollY = clampd(scrollY, 0.0, std::max(0.0, contentH - room));
    }

    void setScrollY(double y)
    {
        scrollY = y;
        clampScrollY();
        rowsFor = ~0ull;
        q->update();
    }

    const Row* rowOf(uint64_t track)
    {
        layoutRows();
        for (const Row& r : rows)
            if (r.track == track) return &r;
        return nullptr;
    }

    const Row* rowAtY(double y)
    {
        layoutRows();
        for (const Row& r : rows)
            if (y >= r.y && y < r.y + r.h) return &r;
        return nullptr;
    }

    double addRowY()
    {
        layoutRows();
        return rows.empty() ? kRulerH - scrollY : rows.back().y + rows.back().h;
    }

    QRectF bodyRect(const Row& r, const Event& e) const
    {
        const double x0 = xAt(e.start), x1 = xAt(e.start + e.length);
        return QRectF(x0, r.y + 3, std::max(2.0, x1 - x0), r.h - 6);
    }

    // What a finger at 'pos' is on. Selected events' handles come first: they reach out past the event's edge, and
    // into it a little, so a short event can still be trimmed from either side.
    Hit hitAt(const QPointF& pos)
    {
        layoutRows();
        Hit h;
        if (app.project().tracks.empty() && pos.y() > kRulerH) { h.part = Part::Hint; return h; }
        if (pos.y() < kRulerH) { h.part = Part::Ruler; return h; }
        const double addY = addRowY();
        if (pos.y() >= addY && pos.y() < addY + kAddRowH) { h.part = Part::AddRow; return h; }
        const Row* row = rowAtY(pos.y());
        if (!row) { h.part = Part::Empty; return h; }
        h.track = row->track;
        if (pos.x() < eventsLeft()) { h.part = Part::Header; return h; }
        const Project& p = app.project();
        const Track* t = nullptr;
        for (const Track& tr : p.tracks)
            if (tr.id == row->track) t = &tr;
        if (!t) return h;
        // handles of selected events on this row
        for (const Event& e : t->events) {
            if (!app.selection().count(e.id)) continue;
            const QRectF r = bodyRect(*row, e);
            const double inward = std::min(14.0, r.width() * 0.3);
            if (QRectF(r.left() - kHandleW - 8, r.top() - 6, kHandleW + 8 + inward, r.height() + 12).contains(pos)) {
                h.part = Part::HandleL;
                h.event = e.id;
                return h;
            }
            if (QRectF(r.right() - inward, r.top() - 6, kHandleW + 8 + inward, r.height() + 12).contains(pos)) {
                h.part = Part::HandleR;
                h.event = e.id;
                return h;
            }
        }
        // the topmost (latest starting) event under the finger
        const double tAt = timeAt(pos.x());
        for (auto it = t->events.end(); it != t->events.begin();) {
            --it;
            if (tAt >= it->start && tAt < it->start + it->length) {
                h.part = Part::Event;
                h.event = it->id;
                return h;
            }
        }
        h.part = Part::Empty;
        return h;
    }

    // ---- painting
    void paint(QPainter& p, const QRect& clip)
    {
        ++paints;
        ensureLoader();
        layoutRows();
        const Project& proj = app.project();
        const double w = q->width(), h = q->height();
        p.fillRect(clip, color::bg);
        const double tLeft = timeAt(eventsLeft()), tRight = timeAt(w);
        // the area before the project's start is darker, so it is plain where time 0 is
        const double x0 = xAt(0);
        if (x0 > eventsLeft()) p.fillRect(QRectF(eventsLeft(), kRulerH, x0 - eventsLeft(), h - kRulerH), QColor(12, 12, 13));

        p.save();
        p.setClipRect(QRectF(eventsLeft(), kRulerH, w - eventsLeft(), h - kRulerH));
        for (const Row& r : rows) {
            if (r.y > h || r.y + r.h < kRulerH) continue;
            p.fillRect(QRectF(eventsLeft(), r.y + r.h - 1, w - eventsLeft(), 1), QColor(30, 30, 34));
            const Track* t = nullptr;
            for (const Track& tr : proj.tracks)
                if (tr.id == r.track) { t = &tr; break; }
            if (!t) continue;
            drawTrackEvents(p, r, *t, tLeft, tRight);
        }
        drawAddRow(p);
        p.restore();

        drawHeaders(p);
        drawRuler(p, proj);
        // the guide where a dragged edge snapped
        if (mode == Mode::Trim || mode == Mode::Move) drawDragInfo(p);
        if (std::isfinite(snapAt)) {
            const double sx = xAt(snapAt);
            p.fillRect(QRectF(sx - 0.75, kRulerH, 1.5, h - kRulerH), color::snap);
        }
        // the playhead, over everything
        const double px = std::round(playX());
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(QRectF(px - 1, kRulerH - 6, 2, h - kRulerH + 6), color::playhead);
        QPainterPath cap;
        cap.addRoundedRect(QRectF(px - 6, 2, 12, kRulerH - 6), 3, 3);
        p.fillPath(cap, color::playhead);
        p.setRenderHint(QPainter::Antialiasing, false);
        if (proj.tracks.empty()) drawHint(p);
        trimThumbs();
    }

    void drawTrackEvents(QPainter& p, const Row& r, const Track& t, double tLeft, double tRight)
    {
        const Project& proj = app.project();
        // events are kept in start order; the longest one says how far back to look for one reaching into view
        double longest = 0;
        for (const Event& e : t.events) longest = std::max(longest, e.length);
        const Event* begin = t.events.begin();
        const Event* end = t.events.end();
        const Event* first = std::lower_bound(begin, end, tLeft - longest, [](const Event& e, double v) { return e.start < v; });
        const Event* prev = nullptr;
        for (const Event* it = first; it != end && it->start <= tRight; ++it) {
            if (it->start + it->length < tLeft) { prev = it; continue; }
            drawEvent(p, r, t, *it, proj.findMedia(it->mediaId));
            // where it overlaps the one before: the cross-fade
            if (prev && prev->start + prev->length > it->start) drawCrossfade(p, r, *prev, *it);
            prev = it;
        }
        if (first != begin && first != end) {
            const Event* before = first - 1;
            if (before->start + before->length > first->start) drawCrossfade(p, r, *before, *first);
        }
    }

    QColor bodyColor(const Track& t, const MediaItem* m) const
    {
        if (m && m->generated) return color::text3;
        return t.type == TrackType::Video ? color::video : color::audio;
    }

    QColor edgeColor(const Track& t, const MediaItem* m) const
    {
        if (m && m->generated) return color::text3Edge;
        return t.type == TrackType::Video ? color::videoEdge : color::audioEdge;
    }

    // An event is a plain rectangle with its content clipped to it (a rectangular clip is cheap; a rounded one made
    // painting several times slower), its corners rounded by small cached pictures of the lane's background laid over
    // them, and its edge drawn as straight lines between those corners.
    void drawEvent(QPainter& p, const Row& row, const Track& t, const Event& e, const MediaItem* m)
    {
        const QRectF rf = bodyRect(row, e);
        // whole pixels, so the edges are sharp and the corner pictures meet them exactly
        const QRect r(QPoint((int)std::round(rf.left()), (int)std::round(rf.top())), QPoint((int)std::round(rf.right()) - 1, (int)std::round(rf.bottom()) - 1));
        if (r.width() < 1) return;
        const bool selected = app.selection().count(e.id) != 0;
        if (mode == Mode::Move && selected) {
            // picked up: a shadow under it
            p.fillRect(r.translated(0, 3).adjusted(2, 2, 2, 2), QColor(0, 0, 0, 110));
        }
        p.fillRect(r, bodyColor(t, m));
        const QRect visible = r & QRect((int)eventsLeft(), 0, q->width() - (int)eventsLeft(), q->height());
        p.save();
        p.setClipRect(visible, Qt::IntersectClip);
        if (m && !m->offline) {
            if (t.type == TrackType::Video && !m->generated && (m->info.hasVideo || m->info.isStill)) drawThumbs(p, e, *m, r, r);
            else if (t.type == TrackType::Audio) drawWave(p, e, *m, r);
        }
        drawFades(p, e, r);
        const double visL = std::max((double)r.left(), eventsLeft() + 2.0);
        drawLabel(p, eventLabel(e, m), QRectF(visL + 6, r.top() + 3, r.right() - visL - 10, 18));
        drawBadges(p, e, r, visL);
        if (e.mute) {
            p.fillRect(r, QColor(0, 0, 0, 130));
            if (r.width() > 28) drawIcon(p, t.type == TrackType::Video ? Icon::EyeOff : Icon::Mute, QRectF(r.center().x() - 10, r.center().y() - 10, 20, 20), color::dim);
        }
        p.restore();
        drawFrame(p, r, selected ? color::playhead : edgeColor(t, m), selected ? 2 : 1);
        if (selected) drawHandles(p, r);
        if (e.locked && r.width() > 24) drawIcon(p, Icon::Lock, QRectF(r.right() - 20, r.top() + 4, 14, 14), color::dim);
    }

    // ---- rounded frames without rounded clipping
    struct CornerKey {
        QRgb edge;
        int width;
        int dpr100;
        bool operator<(const CornerKey& o) const { return std::tie(edge, width, dpr100) < std::tie(o.edge, o.width, o.dpr100); }
    };
    std::map<CornerKey, std::array<QPixmap, 4>> corners;

    // The four corners (top left, top right, bottom left, bottom right): the lane's background outside the rounding and
    // the edge's arc, at the screen's scale.
    const std::array<QPixmap, 4>& cornerPictures(const QColor& edge, int width)
    {
        const double dpr = q->devicePixelRatioF();
        const CornerKey key{edge.rgba(), width, (int)std::lround(dpr * 100)};
        auto it = corners.find(key);
        if (it != corners.end()) return it->second;
        std::array<QPixmap, 4> out;
        const int R = kRadius;
        for (int k = 0; k < 4; ++k) {
            QPixmap pm((int)std::ceil(R * dpr), (int)std::ceil(R * dpr));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter c(&pm);
            c.setRenderHint(QPainter::Antialiasing, true);
            // drawn as the top left corner, then mirrored into place
            c.translate(k & 1 ? R : 0, k & 2 ? R : 0);
            c.scale(k & 1 ? -1 : 1, k & 2 ? -1 : 1);
            QPainterPath outside;
            outside.addRect(QRectF(0, 0, R, R));
            QPainterPath round;
            round.addEllipse(QRectF(0, 0, 2.0 * R, 2.0 * R));
            c.fillPath(outside.subtracted(round), color::bg);
            const double half = width / 2.0;
            c.setPen(QPen(edge, width));
            c.drawArc(QRectF(half, half, 2.0 * R - width, 2.0 * R - width), 90 * 16, 90 * 16);
            c.end();
            out[(size_t)k] = pm;
        }
        return corners.emplace(key, out).first->second;
    }

    void drawFrame(QPainter& p, const QRect& r, const QColor& edge, int width)
    {
        const int R = kRadius;
        if (r.width() < 2 * R + 2 || r.height() < 2 * R + 2) {
            // too small to round: a plain edge
            p.fillRect(QRect(r.left(), r.top(), r.width(), width), edge);
            p.fillRect(QRect(r.left(), r.bottom() - width + 1, r.width(), width), edge);
            p.fillRect(QRect(r.left(), r.top(), width, r.height()), edge);
            p.fillRect(QRect(r.right() - width + 1, r.top(), width, r.height()), edge);
            return;
        }
        const std::array<QPixmap, 4>& c = cornerPictures(edge, width);
        const int x1 = r.right() + 1, y1 = r.bottom() + 1;
        p.drawPixmap(QPoint(r.left(), r.top()), c[0]);
        p.drawPixmap(QPoint(x1 - R, r.top()), c[1]);
        p.drawPixmap(QPoint(r.left(), y1 - R), c[2]);
        p.drawPixmap(QPoint(x1 - R, y1 - R), c[3]);
        p.fillRect(QRect(r.left() + R, r.top(), r.width() - 2 * R, width), edge);
        p.fillRect(QRect(r.left() + R, y1 - width, r.width() - 2 * R, width), edge);
        p.fillRect(QRect(r.left(), r.top() + R, width, r.height() - 2 * R), edge);
        p.fillRect(QRect(x1 - width, r.top() + R, width, r.height() - 2 * R), edge);
    }

    // ---- event names, laid out once (QStaticText) and drawn with a shadow so they read over pictures
    struct LabelKey {
        QString text;
        int width;
        bool operator<(const LabelKey& o) const { return std::tie(width, text) < std::tie(o.width, o.text); }
    };
    std::map<LabelKey, QStaticText> labels;

    void drawLabel(QPainter& p, const QString& label, const QRectF& box)
    {
        if (label.isEmpty() || box.width() < 20) return;
        const QFont f = uiFont(tablet ? 13 : 12, true);
        // the width is rounded to 8 pixels so scrolling an event half out of view doesn't lay out its name every frame
        const int w = (int)box.width() / 8 * 8;
        if (w < 16) return;
        auto it = labels.find({label, w});
        if (it == labels.end()) {
            if (labels.size() > 600) labels.clear();
            const QString shown = QFontMetrics(f).elidedText(label, Qt::ElideRight, w);
            QStaticText st(shown);
            st.setTextFormat(Qt::PlainText);
            st.prepare(QTransform(), f);
            it = labels.emplace(LabelKey{label, w}, st).first;
        }
        p.setFont(f);
        const QPointF at(box.left(), box.top() + 1);
        p.setPen(QColor(0, 0, 0, 170));
        p.drawStaticText(at + QPointF(1, 1), it->second);
        p.setPen(color::text);
        p.drawStaticText(at, it->second);
    }

    void drawHandles(QPainter& p, const QRectF& r)
    {
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int side = 0; side < 2; ++side) {
            const QRectF hr = side == 0 ? QRectF(r.left() - kHandleW + 2, r.top(), kHandleW, r.height())
                                        : QRectF(r.right() - 2, r.top(), kHandleW, r.height());
            QPainterPath hp;
            hp.addRoundedRect(hr, 5, 5);
            p.fillPath(hp, color::playhead);
            p.setPen(QPen(QColor(60, 60, 66), 2, Qt::SolidLine, Qt::RoundCap));
            const double cx = hr.center().x(), cy = hr.center().y();
            p.drawLine(QPointF(cx, cy - 8), QPointF(cx, cy + 8));
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    void drawThumbs(QPainter& p, const Event& e, const MediaItem& m, const QRectF& r, const QRectF& content)
    {
        const double tileH = content.height();
        if (tileH < 8) return;
        double aspect = (m.info.width > 0 && m.info.height > 0) ? (double)m.info.width / m.info.height : 16.0 / 9.0;
        aspect = clampd(aspect, 0.4, 3.0);
        const double tileW = std::max(12.0, std::floor(tileH * aspect));
        const double dpr = q->devicePixelRatioF();
        const int reqW = std::max(8, (int)std::lround(tileW * dpr)), reqH = std::max(8, (int)std::lround(tileH * dpr));
        const double dur = m.info.isStill ? 0.0 : m.info.duration;
        const double fps = m.info.fps;
        const double grain = thumbGrain(tileW / pps * std::fabs(e.rate), fps);
        const double lastFrame = dur - (fps > 0 ? 1.0 / fps : 0.04);
        const int key = mediaKey(m);
        const double visL = std::max(content.left(), eventsLeft()), visR = std::min(content.right(), (double)q->width());
        const int64_t first = std::max<int64_t>(0, (int64_t)std::floor((visL - r.left()) / tileW));
        const int64_t last = (int64_t)std::floor((visR - r.left()) / tileW);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int64_t k = first; k <= last; ++k) {
            const QRectF target(r.left() + (double)k * tileW, content.top(), tileW, tileH);
            double mt = 0;
            if (dur > 0) {
                mt = eventMediaTime(e, &m, e.start + ((double)k + 0.5) * tileW / pps);
                mt = std::min(std::max(0.0, std::floor(mt / grain + 0.5) * grain), std::max(0.0, lastFrame));
            }
            tl::ThumbKey tk;
            tk.media = key;
            tk.height = reqH;
            tk.ms = (int64_t)std::llround(mt * 1000.0);
            if (const QImage* img = thumb(tk, m, mt, reqW, reqH)) p.drawImage(target, *img);
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // a shade at the top for the name
        QLinearGradient g(content.topLeft(), QPointF(content.left(), content.top() + 24));
        g.setColorAt(0, QColor(0, 0, 0, 120));
        g.setColorAt(1, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(content.left(), content.top(), content.width(), 24), g);
    }

    void drawWave(QPainter& p, const Event& e, const MediaItem& m, const QRectF& content)
    {
        std::shared_ptr<tl::Peaks> pk = loader->peaks(mediaKey(m), m);
        const double mid = content.center().y(), half = content.height() * 0.42;
        p.fillRect(QRectF(std::max(content.left(), eventsLeft()), mid, std::min(content.right(), (double)q->width()) - std::max(content.left(), eventsLeft()), 1),
                   QColor(color::wave.red(), color::wave.green(), color::wave.blue(), 90));
        if (!pk || pk->levels.empty()) return;
        // the coarsest level that still has a value for every pixel
        const double mediaPerPx = std::fabs(e.rate) / pps;
        size_t level = 0;
        double perPx = tl::kPeakPerSecond * mediaPerPx;
        while (level + 1 < pk->levels.size() && perPx / tl::kPeakStep >= 1.0) {
            perPx /= tl::kPeakStep;
            ++level;
        }
        const double bucketsPerSecond = tl::kPeakPerSecond / std::pow((double)tl::kPeakStep, (double)level);
        const tl::Peaks::Level& lv = pk->levels[level];
        const int64_t ready = pk->ready(level);
        const int x0 = (int)std::floor(std::max(content.left(), eventsLeft())), x1 = (int)std::ceil(std::min(content.right(), (double)q->width()));
        QVector<QLineF> lines;
        lines.reserve(std::max(0, x1 - x0));
        double prevMt = eventMediaTime(e, &m, timeAt(x0));
        for (int x = x0; x < x1; ++x) {
            const double mt = eventMediaTime(e, &m, timeAt(x + 1));
            int64_t a = (int64_t)std::floor(std::min(prevMt, mt) * bucketsPerSecond);
            int64_t b = (int64_t)std::ceil(std::max(prevMt, mt) * bucketsPerSecond);
            prevMt = mt;
            a = std::max<int64_t>(0, a);
            b = std::min<int64_t>(std::min<int64_t>(ready, (int64_t)lv.lo.size()), std::max(b, a + 1));
            if (a >= b) continue;
            int lo = 127, hi = -127;
            for (int64_t k = a; k < b; ++k) {
                lo = std::min<int>(lo, lv.lo[(size_t)k]);
                hi = std::max<int>(hi, lv.hi[(size_t)k]);
            }
            const double g = std::min(2.0, e.gain);
            lines.push_back(QLineF(x + 0.5, mid - hi / 127.0 * half * g, x + 0.5, mid - lo / 127.0 * half * g + 1));
        }
        p.setPen(QPen(color::wave, 1));
        p.drawLines(lines);
    }

    void drawFades(QPainter& p, const Event& e, const QRectF& r)
    {
        const QColor shade(0, 0, 0, 120);
        p.setRenderHint(QPainter::Antialiasing, true);
        if (e.fadeIn > 0) {
            const double fw = std::min(r.width(), e.fadeIn * pps);
            QPainterPath a;
            a.moveTo(r.topLeft());
            a.lineTo(r.left() + fw, r.top());
            a.lineTo(r.bottomLeft());
            a.closeSubpath();
            p.fillPath(a, shade);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1.2));
            p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.left() + fw, r.top()));
        }
        if (e.fadeOut > 0) {
            const double fw = std::min(r.width(), e.fadeOut * pps);
            QPainterPath a;
            a.moveTo(r.topRight());
            a.lineTo(r.right() - fw, r.top());
            a.lineTo(r.bottomRight());
            a.closeSubpath();
            p.fillPath(a, shade);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1.2));
            p.drawLine(QPointF(r.right(), r.bottom()), QPointF(r.right() - fw, r.top()));
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // Small marks at the bottom left: speed, effects, reversed.
    void drawBadges(QPainter& p, const Event& e, const QRectF& r, double visL)
    {
        QStringList marks;
        if (std::fabs(e.rate - 1.0) > 1e-6) marks << QString::number(e.rate, 'g', 3) + QChar(0xD7);
        if (e.reverse) marks << "rev";
        size_t fx = 0;
        for (const FxSlot& s : e.fx)
            if (!s.bypass) ++fx;
        if (fx) marks << (fx > 1 ? QString("fx %1").arg(fx) : QString("fx"));
        if (marks.isEmpty() || r.height() < 34) return;
        p.setFont(uiFont(10, true));
        double x = visL + 5;
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const QString& s : marks) {
            const double tw = p.fontMetrics().horizontalAdvance(s) + 10;
            if (x + tw > r.right() - 4) break;
            const QRectF b(x, r.bottom() - 18, tw, 15);
            QPainterPath bp;
            bp.addRoundedRect(b, 7.5, 7.5);
            p.fillPath(bp, QColor(0, 0, 0, 150));
            p.setPen(color::text);
            p.drawText(b, Qt::AlignCenter, s);
            x += tw + 4;
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    void drawCrossfade(QPainter& p, const Row& row, const Event& a, const Event& b)
    {
        const double from = b.start, to = std::min(a.start + a.length, b.start + b.length);
        if (to <= from) return;
        const QRectF r(xAt(from), row.y + 3, xAt(to) - xAt(from), row.h - 6);
        if (r.width() < 2) return;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(r, QColor(0, 0, 0, 90));
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.4));
        p.drawLine(r.bottomLeft(), r.topRight());
        p.drawLine(r.topLeft(), r.bottomRight());
        if (!b.transition.kind.empty() && r.width() > 22) {
            const QRectF ic(r.center().x() - 10, r.center().y() - 10, 20, 20);
            QPainterPath circle;
            circle.addEllipse(ic.adjusted(-3, -3, 3, 3));
            p.fillPath(circle, color::accent);
            drawIcon(p, Icon::Transition, ic.adjusted(2, 2, -2, -2), Qt::white);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // The "Add track" pill under the tracks, drawn once (a dashed rounded outline is slow to draw every frame).
    QPixmap addRowPicture;
    void drawAddRow(QPainter& p)
    {
        const double y = addRowY();
        if (y > q->height()) return;
        const double dpr = q->devicePixelRatioF();
        const QSize size(140, kAddRowH - 16);
        if (addRowPicture.isNull() || addRowPicture.devicePixelRatio() != dpr) {
            QPixmap pm((int)std::ceil(size.width() * dpr), (int)std::ceil(size.height() * dpr));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter c(&pm);
            c.setRenderHint(QPainter::Antialiasing, true);
            const QRectF r(0.5, 0.5, size.width() - 1, size.height() - 1);
            QPainterPath b;
            b.addRoundedRect(r, r.height() / 2, r.height() / 2);
            c.setPen(QPen(color::line, 1.2, Qt::DashLine));
            c.drawPath(b);
            drawIcon(c, Icon::Add, QRectF(14, r.center().y() - 9, 18, 18), color::dim);
            c.setPen(color::dim);
            c.setFont(uiFont(13));
            c.drawText(r.adjusted(36, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, "Add track");
            c.end();
            addRowPicture = pm;
        }
        p.drawPixmap(QPointF(std::round(playX() - size.width() / 2.0), std::round(y + 8)), addRowPicture);
    }

    void drawHeaders(QPainter& p)
    {
        const double h = q->height();
        p.fillRect(QRectF(0, kRulerH, kHeaderW, h - kRulerH), color::bar);
        p.fillRect(QRectF(kHeaderW - 1, kRulerH, 1, h - kRulerH), color::line);
        const Project& proj = app.project();
        int videoN = 0, audioN = 0;
        p.save();
        p.setClipRect(QRectF(0, kRulerH, kHeaderW, h - kRulerH));
        for (const Row& r : rows) {
            const Track* t = nullptr;
            for (const Track& tr : proj.tracks)
                if (tr.id == r.track) { t = &tr; break; }
            if (!t) continue;
            const int n = r.video ? ++videoN : ++audioN;
            if (r.y > h || r.y + r.h < kRulerH) continue;
            const QRectF cell(0, r.y, kHeaderW - 1, r.h);
            if (app.currentTrack == t->id) p.fillRect(cell, color::raised);
            p.fillRect(QRectF(0, r.y + 4, 3, r.h - 8), r.video ? color::videoEdge : color::audioEdge);
            p.fillRect(QRectF(4, r.y + r.h - 1, kHeaderW - 5, 1), QColor(40, 40, 44));
            const bool off = t->mute;
            const Icon ic = off ? (r.video ? Icon::EyeOff : Icon::Mute) : (r.video ? Icon::Media : Icon::Music);
            const double cy = r.y + r.h * 0.5;
            p.drawPixmap(QPointF(kHeaderW / 2 - 9, std::round(cy - (r.h > 52 ? 16 : 13))), iconPixmap(ic, off ? color::danger : color::dim, 18, q->devicePixelRatioF()));
            p.setPen(color::dim);
            p.setFont(uiFont(11, true));
            p.drawText(QRectF(0, cy + (r.h > 52 ? 4 : 3), kHeaderW, 14), Qt::AlignCenter, (r.video ? "V" : "A") + QString::number(n));
            if (t->solo) {
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setBrush(color::snap);
                p.setPen(Qt::NoPen);
                p.drawEllipse(QPointF(kHeaderW - 9, r.y + 9), 3.5, 3.5);
                p.setRenderHint(QPainter::Antialiasing, false);
            }
        }
        p.restore();
    }

    void drawRuler(QPainter& p, const Project& proj)
    {
        const double w = q->width();
        p.fillRect(QRectF(0, 0, w, kRulerH), color::bar);
        p.fillRect(QRectF(0, kRulerH - 1, w, 1), color::line);
        const double fps = proj.settings.fps();
        // the loop region (time selection)
        if (app.timeSel()) {
            const double a = std::max(eventsLeft(), xAt(app.loopStart)), b = std::min(w, xAt(app.loopEnd));
            if (b > a) p.fillRect(QRectF(a, kRulerH - 6, b - a, 5), color::accent);
        }
        const double step = rulerStep(pps, fps, 74);
        const double minor = step / (step < 1.0 ? 2 : 5);
        p.save();
        p.setClipRect(QRectF(eventsLeft(), 0, w - eventsLeft(), kRulerH));
        const double tL = std::max(0.0, timeAt(eventsLeft())), tR = timeAt(w);
        p.setPen(color::faint);
        for (double t = std::floor(tL / minor) * minor; t <= tR; t += minor) {
            const double x = std::round(xAt(t));
            const bool major = std::fabs(std::remainder(t, step)) < minor * 0.25;
            p.fillRect(QRectF(x, major ? kRulerH - 10 : kRulerH - 5, 1, major ? 9 : 4), major ? color::dim : color::faint);
        }
        p.setFont(uiFont(11));
        p.setPen(color::dim);
        for (double t = std::floor(tL / step) * step; t <= tR + step; t += step) {
            if (t < -1e-9) continue;
            const double x = xAt(t);
            p.drawText(QRectF(x + 4, 2, 90, kRulerH - 12), Qt::AlignLeft | Qt::AlignVCenter, rulerLabel(t, step, fps));
        }
        // markers: small flags
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const Project::Marker& m : proj.markers) {
            const double x = xAt(m.time);
            if (x < eventsLeft() - 10 || x > w + 10) continue;
            if (m.region && m.length > 0) p.fillRect(QRectF(x, 0, m.length * pps, 3), color::snap);
            QPainterPath flag;
            flag.moveTo(x, 4);
            flag.lineTo(x + 8, 4);
            flag.lineTo(x + 8, 12);
            flag.lineTo(x + 4, 15);
            flag.lineTo(x, 12);
            flag.closeSubpath();
            p.fillPath(flag, color::snap);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.restore();
        p.fillRect(QRectF(0, 0, kHeaderW, kRulerH - 1), color::bar);
    }

    // While trimming, the event's new length; while moving, where it starts: in a bubble over the event, so the finger
    // doesn't hide it.
    void drawDragInfo(QPainter& p)
    {
        const Track* t = nullptr;
        const Event* e = app.project().findEvent(grabbed, &t);
        if (!e || !t) return;
        const Row* row = rowOf(t->id);
        if (!row) return;
        const double fps = app.project().settings.fps();
        auto stamp = [fps](double v) {
            const int64_t frames = (int64_t)std::llround(v * fps);
            const int fpsI = std::max(1, (int)std::lround(fps));
            const int64_t secs = frames / fpsI;
            return QString("%1:%2.%3").arg(secs / 60).arg(secs % 60, 2, 10, QChar('0')).arg((int)(frames % fpsI), 2, 10, QChar('0'));
        };
        const QString text = mode == Mode::Trim ? stamp(e->length) : stamp(e->start);
        const QFont f = uiFont(13, true);
        const double w = QFontMetrics(f).horizontalAdvance(text) + 20;
        const double x = mode == Mode::Trim ? (trimLeft ? xAt(e->start) : xAt(e->start + e->length)) : xAt(e->start);
        QRectF b(x - w / 2, row->y - 30, w, 24);
        if (b.top() < kRulerH + 2) b.moveTop(row->y + row->h + 6);
        b.moveLeft(clampd(b.left(), eventsLeft() + 4, q->width() - w - 4));
        p.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath path;
        path.addRoundedRect(b, 12, 12);
        p.fillPath(path, QColor(0, 0, 0, 200));
        p.setPen(color::text);
        p.setFont(f);
        p.drawText(b, Qt::AlignCenter, text);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    void drawHint(QPainter& p)
    {
        const QRectF area(kHeaderW, kRulerH, q->width() - kHeaderW, q->height() - kRulerH);
        const QRectF b(area.center().x() - 120, area.center().y() - 26, 240, 52);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath path;
        path.addRoundedRect(b, 26, 26);
        p.fillPath(path, color::accent);
        drawIcon(p, Icon::Add, QRectF(b.left() + 18, b.center().y() - 11, 22, 22), Qt::white);
        p.setPen(Qt::white);
        p.setFont(uiFont(15, true));
        p.drawText(b.adjusted(46, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, "Add photos and videos");
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // ---- fingers
    void stopPlaying()
    {
        if (!pb || !pb->playing()) return;
        const double t = pb->position();
        pb->pause();
        app.setCursor(t);
    }

    void pressed(int id, const QPointF& pos)
    {
        fingers.push_back({id, pos, pos});
        if (fingers.size() == 2) {
            beginPinch();
            return;
        }
        if (fingers.size() > 2) return;
        stopFling();
        stopPlaying();
        pressHit = hitAt(pos);
        pressClock.start();
        samples.clear();
        samples.push_back({pressClock.elapsed(), pos});
        axis = 0;
        scrollStartTime = app.cursor();
        scrollStartY = scrollY;
        lastFingerPos = pos;
        mode = Mode::Pending;
        // holding still on an event picks it up to move (a selected one also moves with a plain drag)
        if (pressHit.part == Part::Event) longPress->start();
    }

    void moved(int id, const QPointF& pos)
    {
        for (Finger& f : fingers)
            if (f.id == id) f.pos = pos;
        if (mode == Mode::Pinch) {
            pinchMoved();
            return;
        }
        if (fingers.empty() || fingers.front().id != id) return;
        lastFingerPos = pos;
        samples.push_back({pressClock.elapsed(), pos});
        while (samples.size() > 2 && samples.front().first < samples.back().first - 90) samples.pop_front();
        const QPointF d = pos - fingers.front().start;
        if (mode == Mode::Pending) {
            if (std::hypot(d.x(), d.y()) < kSlop) return;
            longPress->stop();
            const bool onSelected = pressHit.part == Part::Event && app.selection().count(pressHit.event);
            if (pressHit.part == Part::HandleL || pressHit.part == Part::HandleR) beginTrim(pressHit.event, pressHit.part == Part::HandleL, fingers.front().start);
            else if (onSelected && std::fabs(d.x()) >= std::fabs(d.y()) * 0.6) beginMove(pressHit.event, fingers.front().start);
            else {
                mode = Mode::Scroll;
                axis = std::fabs(d.x()) >= std::fabs(d.y()) ? 1 : 2;
            }
        }
        switch (mode) {
        case Mode::Scroll:
            if (axis == 1) app.setCursor(std::max(0.0, scrollStartTime - d.x() / pps));
            else setScrollY(scrollStartY - d.y());
            break;
        case Mode::Trim: trimTo(pos); break;
        case Mode::Move: moveTo(pos); break;
        default: break;
        }
        updateTick();
    }

    void released(int id, const QPointF& pos)
    {
        auto it = std::find_if(fingers.begin(), fingers.end(), [id](const Finger& f) { return f.id == id; });
        if (it == fingers.end()) return;
        const bool primary = it == fingers.begin();
        fingers.erase(it);
        if (mode == Mode::Pinch) {
            if (fingers.empty()) mode = Mode::Idle;
            return;
        }
        if (!primary) return;
        longPress->stop();
        switch (mode) {
        case Mode::Pending: tapped(pos); break;
        case Mode::Scroll: startFling(); break;
        case Mode::Trim: app.endDrag("Trim Event"); break;
        case Mode::Move: app.endDrag(orig.size() > 1 ? "Move Events" : "Move Event"); break;
        default: break;
        }
        if (mode == Mode::Trim || mode == Mode::Move) {
            snapAt = kNaN;
            orig.clear();
        }
        mode = Mode::Idle;
        fingers.clear();
        updateTick();
        q->update();
    }

    void cancelled()
    {
        longPress->stop();
        if (mode == Mode::Trim || mode == Mode::Move) app.cancelDrag();
        snapAt = kNaN;
        orig.clear();
        fingers.clear();
        mode = Mode::Idle;
        updateTick();
        q->update();
    }

    void tapped(const QPointF& pos)
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool doubleTap = now - lastTapMs < 300 && std::hypot(pos.x() - lastTapPos.x(), pos.y() - lastTapPos.y()) < 30;
        lastTapMs = doubleTap ? -100000 : now;
        lastTapPos = pos;
        switch (pressHit.part) {
        case Part::Ruler: {
            // a marker's flag near the finger: the playhead goes to the marker itself
            double t = std::max(0.0, timeAt(pos.x()));
            double best = 16.0;
            for (const Project::Marker& m : app.project().markers) {
                const double gap = std::fabs(xAt(m.time) + 4 - pos.x());
                if (gap < best) {
                    best = gap;
                    t = m.time;
                }
            }
            app.setCursor(t);
            break;
        }
        case Part::Header:
            app.currentTrack = pressHit.track;
            app.notify(kSelection);
            if (q->onTrackTapped) q->onTrackTapped(pressHit.track);
            break;
        case Part::AddRow:
            if (q->onAddTrack) q->onAddTrack();
            break;
        case Part::Hint:
            if (q->onEmptyTapped) q->onEmptyTapped();
            break;
        case Part::Event:
        case Part::HandleL:
        case Part::HandleR:
            app.currentTrack = pressHit.track;
            if (multi) {
                std::set<uint64_t> sel = app.selection();
                if (!sel.erase(pressHit.event)) sel.insert(pressHit.event);
                app.setSelection(sel);
            } else if (app.selection().size() != 1 || !app.selection().count(pressHit.event)) app.selectOnly(pressHit.event);
            else app.notify(kSelection);
            if (doubleTap && q->onEventDoubleTapped) q->onEventDoubleTapped(pressHit.event);
            break;
        case Part::Empty:
        case Part::None:
            if (pressHit.track) app.currentTrack = pressHit.track;
            if (multi) break;               // (taps that miss don't drop what was picked)
            if (!app.selection().empty()) app.setSelection({});
            else app.notify(kSelection);
            break;
        }
    }

    void longPressed()
    {
        if (mode != Mode::Pending || pressHit.part != Part::Event || fingers.size() != 1) return;
        app.currentTrack = pressHit.track;
        app.selectOnly(pressHit.event);
        beginMove(pressHit.event, fingers.front().pos);
        q->update();
    }

    // ---- pinch zoom (about the playhead, which stays where it is) and two-finger scrolling of the tracks
    void beginPinch()
    {
        longPress->stop();
        if (mode == Mode::Trim || mode == Mode::Move) app.cancelDrag();
        snapAt = kNaN;
        orig.clear();
        stopFling();
        mode = Mode::Pinch;
        const QPointF a = fingers[0].pos, b = fingers[1].pos;
        pinchDist = std::max(10.0, std::fabs(a.x() - b.x()));
        pinchPps = pps;
        pinchMidY = (a.y() + b.y()) * 0.5;
        pinchScrollY = scrollY;
    }

    void pinchMoved()
    {
        if (fingers.size() < 2) return;
        const QPointF a = fingers[0].pos, b = fingers[1].pos;
        const double dist = std::max(10.0, std::fabs(a.x() - b.x()));
        setPps(pinchPps * dist / pinchDist);
        setScrollY(pinchScrollY - ((a.y() + b.y()) * 0.5 - pinchMidY));
    }

    void setPps(double v)
    {
        const double fitLow = std::max(kMinPps, (q->width() - kHeaderW) * 0.4 / std::max(10.0, app.project().length()));
        pps = clampd(v, std::min(fitLow, 4.0), kMaxPps);
        app.notify(kView);
        q->update();
    }

    // ---- flicks
    void startFling()
    {
        if (samples.size() < 2) return;
        const auto& a = samples.front();
        const auto& b = samples.back();
        // too few moments to tell a speed from, or the finger stopped before it lifted
        if (b.first - a.first < 16 || pressClock.elapsed() - b.first > 60) return;
        const double dt = (b.first - a.first) / 1000.0;
        QPointF v = (b.second - a.second) / dt;
        if (axis == 1) v.setY(0);
        else v.setX(0);
        if (std::hypot(v.x(), v.y()) < 250) return;
        flingV = v * 1.1;
        flingLast = 0;
        updateTick();
    }

    void stopFling()
    {
        flingV = QPointF();
        updateTick();
    }

    void updateTick()
    {
        const bool needed = (pb && pb->playing()) || !flingV.isNull() || mode == Mode::Trim || mode == Mode::Move;
        if (needed && !tick->isActive()) {
            flingLast = 0;
            tick->start();
        } else if (!needed && tick->isActive()) tick->stop();
    }

    void onTick()
    {
        static QElapsedTimer clock;
        if (!clock.isValid()) clock.start();
        const qint64 now = clock.elapsed();
        const double dt = flingLast ? std::min(0.05, (now - flingLast) / 1000.0) : 0.016;
        flingLast = now;
        if (!flingV.isNull()) {
            // the speed falls off as on the platforms' own lists (about a third of a second to slow by 1/e)
            const double decay = std::exp(-dt / 0.33);
            if (flingV.x() != 0) {
                const double t = app.cursor() - flingV.x() * dt / pps;
                if (t <= 0 || t >= app.project().length() + 5) flingV.setX(0);
                app.setCursor(clampd(t, 0.0, app.project().length() + 5));
            }
            if (flingV.y() != 0) {
                const double before = scrollY;
                setScrollY(scrollY - flingV.y() * dt);
                if (scrollY == before) flingV.setY(0);
            }
            flingV *= decay;
            if (std::hypot(flingV.x(), flingV.y()) < 20) flingV = QPointF();
        }
        // a dragged event or edge near the side: the timeline scrolls so it can go further
        if (mode == Mode::Trim || mode == Mode::Move) {
            const double x = lastFingerPos.x();
            double speed = 0;
            if (x < eventsLeft() + kEdgeScrollPx) speed = -(eventsLeft() + kEdgeScrollPx - x);
            else if (x > q->width() - kEdgeScrollPx) speed = x - (q->width() - kEdgeScrollPx);
            if (speed != 0) {
                app.setCursor(std::max(0.0, app.cursor() + speed * 12 * dt / pps));
                if (mode == Mode::Trim) trimTo(lastFingerPos);
                else moveTo(lastFingerPos);
            }
        }
        q->update();
        updateTick();
    }

    // ---- trimming and moving
    // Times a dragged edge snaps to: the playhead, the project's start, markers, and the edges of every event that isn't
    // being dragged.
    void collectSnaps(const std::set<uint64_t>& dragged)
    {
        snapTimes.clear();
        snapTimes.push_back(0);
        snapTimes.push_back(app.cursor());
        const Project& p = app.project();
        for (const Project::Marker& m : p.markers) {
            snapTimes.push_back(m.time);
            if (m.region) snapTimes.push_back(m.time + m.length);
        }
        for (const Track& t : p.tracks)
            for (const Event& e : t.events) {
                if (dragged.count(e.id)) continue;
                snapTimes.push_back(e.start);
                snapTimes.push_back(e.start + e.length);
            }
    }

    // The change that brings one of 'edges' onto a snap time, if one is near enough (else 0, and no guide).
    double snapDelta(const std::vector<double>& edges)
    {
        const double reach = kSnapPx / pps;
        double best = 0, bestGap = reach;
        snapAt = kNaN;
        for (double e : edges)
            for (double s : snapTimes) {
                const double gap = std::fabs(s - e);
                if (gap < bestGap) {
                    bestGap = gap;
                    best = s - e;
                    snapAt = s;
                }
            }
        return best;
    }

    // An event and those grouped with it (a clip's picture and its sound) that share the edge being trimmed, or all of
    // the group for a move.
    std::vector<Orig> withGroup(const std::set<uint64_t>& ids, int edge)
    {
        const Project& p = app.project();
        std::vector<Orig> out;
        std::set<uint64_t> seen;
        auto add = [&](const Event& e, uint64_t track) {
            if (seen.insert(e.id).second) out.push_back({e.id, track, e.start, e.length, e.offset});
        };
        for (const Track& t : p.tracks)
            for (const Event& e : t.events)
                if (ids.count(e.id)) add(e, t.id);
        const std::vector<Orig> picked = out;
        for (const Orig& o : picked) {
            const Event* e = p.findEvent(o.id);
            if (!e || !e->group) continue;
            for (const Track& t : p.tracks)
                for (const Event& g : t.events) {
                    if (g.group != e->group || g.locked) continue;
                    const bool sameEdge = edge < 0 ? std::fabs(g.start - e->start) < 1e-6
                                        : edge > 0 ? std::fabs(g.start + g.length - (e->start + e->length)) < 1e-6 : true;
                    if (sameEdge) add(g, t.id);
                }
        }
        return out;
    }

    void beginTrim(uint64_t eventId, bool left, const QPointF& at)
    {
        const Event* e = app.project().findEvent(eventId);
        if (!e || e->locked) {
            mode = Mode::Scroll;
            axis = 1;
            return;
        }
        orig = withGroup({eventId}, left ? -1 : 1);
        std::set<uint64_t> ids;
        for (const Orig& o : orig) ids.insert(o.id);
        collectSnaps(ids);
        grabbed = eventId;
        grabTime = timeAt(at.x());
        mode = left ? Mode::Trim : Mode::Trim;
        trimLeft = left;
        app.beginDrag();
    }
    bool trimLeft = false;

    void trimTo(const QPointF& pos)
    {
        const Project& p = app.project();
        const double frame = 1.0 / std::max(1.0, p.settings.fps());
        const Orig* main = nullptr;
        for (const Orig& o : orig)
            if (o.id == grabbed) main = &o;
        if (!main) return;
        double delta = timeAt(pos.x()) - grabTime;
        const double edge = trimLeft ? main->start + delta : main->start + main->length + delta;
        delta += snapDelta({edge});
        // limits: at least a frame long, not before the project's start, not before the media's start, and not past
        // the media's end unless the event loops
        for (const Orig& o : orig) {
            const Event* e = app.project().findEvent(o.id);
            if (!e) continue;
            const MediaItem* m = p.findMedia(e->mediaId);
            const double r = std::max(1e-6, std::fabs(e->rate));
            if (trimLeft) {
                delta = std::max(delta, -o.start);
                if (m && !m->info.isStill && !m->generated && !e->reverse) delta = std::max(delta, -o.offset / r);
                delta = std::min(delta, o.length - frame);
            } else {
                delta = std::max(delta, frame - o.length);
                if (m && !e->loop && !m->info.isStill && !m->generated && m->info.duration > 0 && !e->reverse)
                    delta = std::min(delta, (m->info.duration - o.offset) / r - o.length);
            }
        }
        const std::vector<Orig> from = orig;
        const bool left = trimLeft;
        app.dragUpdate([from, delta, left](Project& pr) {
            for (const Orig& o : from) {
                Event* e = pr.findEvent(o.id);
                if (!e) continue;
                if (left) {
                    e->start = o.start + delta;
                    e->length = o.length - delta;
                    e->offset = o.offset + delta * e->rate;
                } else {
                    e->length = o.length + delta;
                }
                e->fadeIn = std::min(e->fadeIn, e->length);
                e->fadeOut = std::min(e->fadeOut, e->length);
            }
            for (Track& t : pr.tracks) t.sortEvents();
        });
    }

    void beginMove(uint64_t eventId, const QPointF& at)
    {
        std::set<uint64_t> ids = app.selection();
        ids.insert(eventId);
        // locked events stay where they are
        for (auto it = ids.begin(); it != ids.end();) {
            const Event* e = app.project().findEvent(*it);
            if (!e || e->locked) it = ids.erase(it);
            else ++it;
        }
        if (!ids.count(eventId)) {
            mode = Mode::Scroll;
            axis = 1;
            return;
        }
        orig = withGroup(ids, 0);
        std::set<uint64_t> all;
        for (const Orig& o : orig) all.insert(o.id);
        collectSnaps(all);
        grabbed = eventId;
        grabTime = timeAt(at.x());
        grabRow = 0;
        if (const Row* r = rowAtY(at.y())) grabRow = r->track;
        mode = Mode::Move;
        app.beginDrag();
        moveTo(at);
    }
    uint64_t grabRow = 0;

    void moveTo(const QPointF& pos)
    {
        const Orig* main = nullptr;
        for (const Orig& o : orig)
            if (o.id == grabbed) main = &o;
        if (!main) return;
        double delta = timeAt(pos.x()) - grabTime;
        delta += snapDelta({main->start + delta, main->start + main->length + delta});
        for (const Orig& o : orig) delta = std::max(delta, -o.start);
        // up or down: to another track of the same kind, the same number of such tracks away for every event picked
        // (the events grouped with them stay on their own tracks)
        int shift = 0;
        const Project& p = app.project();
        std::vector<uint64_t> videoT, audioT;
        for (const Track& t : p.tracks) (t.type == TrackType::Video ? videoT : audioT).push_back(t.id);
        auto indexIn = [](const std::vector<uint64_t>& v, uint64_t id) { return (int)(std::find(v.begin(), v.end(), id) - v.begin()); };
        const Track* grabT = nullptr;
        for (const Track& t : p.tracks)
            if (t.id == main->track) grabT = &t;
        const Row* under = rowAtY(clampd(pos.y(), kRulerH + 1, q->height() - 1));
        if (grabT && under && under->track != main->track) {
            const std::vector<uint64_t>& list = grabT->type == TrackType::Video ? videoT : audioT;
            const int to = indexIn(list, under->track);
            if (to < (int)list.size()) shift = to - indexIn(list, main->track);
        }
        std::set<uint64_t> picked = app.selection();
        picked.insert(grabbed);
        std::map<uint64_t, uint64_t> newTrack;
        bool shiftOk = true;
        for (const Orig& o : orig) {
            if (!picked.count(o.id) || shift == 0) continue;
            const Track* t = nullptr;
            for (const Track& tr : p.tracks)
                if (tr.id == o.track) t = &tr;
            const std::vector<uint64_t>& list = t && t->type == TrackType::Video ? videoT : audioT;
            const int to = indexIn(list, o.track) + shift;
            if (to < 0 || to >= (int)list.size()) shiftOk = false;
            else newTrack[o.id] = list[(size_t)to];
        }
        if (!shiftOk) newTrack.clear();
        const std::vector<Orig> from = orig;
        app.dragUpdate([from, delta, newTrack](Project& pr) {
            for (const Orig& o : from) {
                Track* tr = nullptr;
                Event* e = pr.findEvent(o.id, &tr);
                if (!e) continue;
                e->start = o.start + delta;
                auto nt = newTrack.find(o.id);
                if (nt != newTrack.end() && tr && tr->id != nt->second) {
                    Track* dest = pr.findTrack(nt->second);
                    if (!dest) continue;
                    Event moved = *e;
                    std::vector<Event>& src = tr->events.mut();
                    src.erase(std::remove_if(src.begin(), src.end(), [&](const Event& x) { return x.id == o.id; }), src.end());
                    dest->events.mut().push_back(moved);
                }
            }
            for (Track& t : pr.tracks) t.sortEvents();
        });
    }
};

TouchTimeline::TouchTimeline(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl(this, playback))
{
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(false);
    setFocusPolicy(Qt::NoFocus);
    setMinimumHeight(kRulerH + 60);
    d->listener = AppState::get().addListener([this](int changes) {
        if (changes & (kProject | kSelection | kCursor | kPlayback | kView | kMediaPictures)) update();
        if (changes & kPlayback) d->updateTick();
        if (changes & kProject) d->rowsFor = ~0ull;
    });
}

TouchTimeline::~TouchTimeline()
{
    AppState::get().removeListener(d->listener);
    if (d->mode == Impl::Mode::Trim || d->mode == Impl::Mode::Move) AppState::get().cancelDrag();
}

void TouchTimeline::setTablet(bool tablet)
{
    if (d->tablet == tablet) return;
    d->tablet = tablet;
    d->rowsFor = ~0ull;
    update();
}

double TouchTimeline::pixelsPerSecond() const { return d->pps; }
void TouchTimeline::setPixelsPerSecond(double pps) { d->setPps(pps); }

void TouchTimeline::zoomToFit()
{
    const double len = std::max(1.0, AppState::get().project().length());
    d->setPps((width() - kHeaderW) * 0.45 / len);
}

void TouchTimeline::stopFling() { d->stopFling(); }

void TouchTimeline::setMultiSelect(bool on)
{
    d->multi = on;
    update();
}

bool TouchTimeline::multiSelect() const { return d->multi; }

QRectF TouchTimeline::eventRect(uint64_t eventId) const
{
    const Track* t = nullptr;
    const Event* e = AppState::get().project().findEvent(eventId, &t);
    if (!e || !t) return QRectF();
    const Impl::Row* r = d->rowOf(t->id);
    return r ? d->bodyRect(*r, *e) : QRectF();
}

QRectF TouchTimeline::handleRect(uint64_t eventId, bool left) const
{
    if (!AppState::get().selection().count(eventId)) return QRectF();
    const QRectF r = eventRect(eventId);
    if (r.isEmpty()) return r;
    return left ? QRectF(r.left() - kHandleW + 2, r.top(), kHandleW, r.height()) : QRectF(r.right() - 2, r.top(), kHandleW, r.height());
}

QRectF TouchTimeline::trackRect(uint64_t trackId) const
{
    const Impl::Row* r = d->rowOf(trackId);
    return r ? QRectF(0, r->y, width(), r->h) : QRectF();
}

double TouchTimeline::playheadX() const { return d->playX(); }
double TouchTimeline::xAtTime(double seconds) const { return d->xAt(seconds); }
double TouchTimeline::timeAtX(double x) const { return d->timeAt(x); }

bool TouchTimeline::event(QEvent* e)
{
    switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        QTouchEvent* te = static_cast<QTouchEvent*>(e);
        for (const QEventPoint& pt : te->points()) {
            switch (pt.state()) {
            case QEventPoint::Pressed: d->pressed(pt.id(), pt.position()); break;
            case QEventPoint::Updated: d->moved(pt.id(), pt.position()); break;
            case QEventPoint::Released: d->released(pt.id(), pt.position()); break;
            default: break;
            }
        }
        e->accept();
        return true;
    }
    case QEvent::TouchCancel:
        d->cancelled();
        e->accept();
        return true;
    default:
        return QWidget::event(e);
    }
}

void TouchTimeline::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    d->paint(p, e->rect());
}

void TouchTimeline::resizeEvent(QResizeEvent*)
{
    d->rowsFor = ~0ull;
    d->clampScrollY();
}

// A mouse is one finger (desktop testing, and tablets with a mouse).
void TouchTimeline::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || e->source() != Qt::MouseEventNotSynthesized) return;
    d->pressed(-1, e->position());
}

void TouchTimeline::mouseMoveEvent(QMouseEvent* e)
{
    if (e->source() != Qt::MouseEventNotSynthesized || !(e->buttons() & Qt::LeftButton)) return;
    d->moved(-1, e->position());
}

void TouchTimeline::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || e->source() != Qt::MouseEventNotSynthesized) return;
    d->released(-1, e->position());
}

void TouchTimeline::mouseDoubleClickEvent(QMouseEvent* e)
{
    // Qt sends a double-click in place of the second press
    mousePressEvent(e);
}

void TouchTimeline::wheelEvent(QWheelEvent* e)
{
    const QPoint a = e->angleDelta();
    if (e->modifiers() & Qt::ControlModifier) d->setPps(d->pps * std::pow(1.0015, a.y()));
    else if (std::abs(a.x()) > std::abs(a.y()) || (e->modifiers() & Qt::ShiftModifier)) {
        const int dx = std::abs(a.x()) > std::abs(a.y()) ? a.x() : a.y();
        AppState::get().setCursor(std::max(0.0, AppState::get().cursor() - dx * 0.5 / d->pps));
    } else d->setScrollY(d->scrollY - a.y() * 0.5);
    e->accept();
}

}  // namespace app
}  // namespace reno
#endif  // RENO_TOUCH_UI
