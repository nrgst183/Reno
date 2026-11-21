// Project Media's Main Timeline storyboard (see panels_storyboard.h).
#include "panels_storyboard.h"
#include "panels_internal.h"
#include "playback.h"
#include "strconv.h"
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSettings>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace reno {
namespace app {

namespace {

const QSize kCellThumb(104, 58);
const int kCellW = 116, kCellH = 98, kTimeH = 18;
const char* const kMimeMediaIds = "application/x-reno-media-list";     // (Project Media's several-items drag)

// A picture of a media file at a time, made on a thread of its own.
struct FrameJob {
    std::string key, path;
    double time = 0;
    bool video = false;
    MediaPool::Opener opener;
};

class FrameWorker {
public:
    typedef std::function<void(const std::string&, const QImage&)> Deliver;
    explicit FrameWorker(Deliver deliver) : deliver_(std::move(deliver))
    {
        thread_ = std::thread([this] {
            lowerThreadPriority();
            run();
        });
    }
    ~FrameWorker()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void add(const FrameJob& job)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.push_back(job);
        }
        cv_.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            FrameJob job;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
                if (quit_) return;
                job = queue_.front();
                queue_.pop_front();
            }
            QImage out;
            if (job.video) {
                std::string err;
                std::unique_ptr<MediaReader> reader;
                try {
                    reader = job.opener ? job.opener(job.path, &err) : openMediaOrProject(job.path, &err);
                    if (reader) {
                        const double t = reader->info().isStill ? 0.0 : std::max(0.0, std::min(job.time, reader->info().duration - 0.001));
                        ImagePtr img = reader->videoFrame(t, kCellThumb.width() * 2, kCellThumb.height() * 2);
                        if (img && img->w > 0 && img->h > 0)
                            out = toQImage(*img).scaled(kCellThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                } catch (...) {
                }
            }
            deliver_(job.key, out);
        }
    }
    Deliver deliver_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<FrameJob> queue_;
    bool quit_ = false;
    std::thread thread_;
};

// The squares VEGAS shows where a picture is transparent (its generated media).
void drawChecker(QPainter& p, const QRect& r)
{
    p.save();
    p.setClipRect(r);
    const int s = 8;
    for (int y = r.top(); y < r.bottom(); y += s)
        for (int x = r.left(); x < r.right(); x += s)
            p.fillRect(QRect(x, y, s, s), ((x - r.left()) / s + (y - r.top()) / s) % 2 ? QColor(204, 204, 204) : QColor(238, 238, 238));
    p.restore();
}

void drawNoteIcon(QPainter& p, const QRectF& r)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(80, 160, 110));
    const QPointF m = r.center();
    p.drawEllipse(QPointF(m.x() - 5, m.y() + 8), 6, 4.5);
    p.drawRect(QRectF(m.x() - 0.5, m.y() - 14, 2.2, 22));
    p.restore();
}

struct Cell {
    uint64_t event = 0, media = 0;
    uint64_t track = 0;
    double start = 0, length = 0;
    QString name, thumbKey;
    bool video = false, generated = false, offline = false;
};

// What the storyboard's cells are made from: the events of the video tracks, and those of the audio tracks that are not
// grouped with a video event (a clip's picture and sound are one cell), by start time, then track order.
std::vector<Cell> cellsOf(const Project& p)
{
    std::set<uint64_t> videoGroups;
    for (const Track& t : p.tracks)
        if (t.type == TrackType::Video)
            for (const Event& e : t.events)
                if (e.group) videoGroups.insert(e.group);
    struct Key { double start; size_t track; Cell c; };
    std::vector<Key> all;
    for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& t = p.tracks[ti];
        for (const Event& e : t.events) {
            if (t.type != TrackType::Video && e.group && videoGroups.count(e.group)) continue;
            Cell c;
            c.event = e.id;
            c.media = e.mediaId;
            c.track = t.id;
            c.start = e.start;
            c.length = e.length;
            c.video = t.type == TrackType::Video;
            const MediaItem* m = p.findMedia(e.mediaId);
            if (m) {
                c.name = qs(m->name.empty() ? pathFileName(m->path) : m->name);
                c.generated = m->generated;
                c.offline = m->offline;
                const double base = m->subclipLength > 0 ? m->subclipStart : 0.0;
                c.thumbKey = QString("%1@%2").arg(m->id).arg(std::llround((base + e.offset) * 1000.0));
            }
            if (!e.name.empty()) c.name = qs(e.name);
            all.push_back({e.start, ti, c});
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Key& a, const Key& b) {
        if (std::fabs(a.start - b.start) > 1e-9) return a.start < b.start;
        return a.track < b.track;
    });
    std::vector<Cell> out;
    out.reserve(all.size());
    for (const Key& k : all) out.push_back(k.c);
    return out;
}

// The timeline as the storyboard sees it (to tell when it has changed).
std::string timelineSignature(const Project& p)
{
    std::string sig;
    auto num = [&sig](const void* v, size_t n) { sig.append(reinterpret_cast<const char*>(v), n); };
    for (const Track& t : p.tracks) {
        num(&t.id, sizeof t.id);
        for (const Event& e : t.events) {
            num(&e.id, sizeof e.id);
            num(&e.mediaId, sizeof e.mediaId);
            num(&e.start, sizeof e.start);
            num(&e.length, sizeof e.length);
            num(&e.offset, sizeof e.offset);
            sig += e.name;
            sig += '\0';
        }
    }
    for (const MediaItem& m : p.media) {
        num(&m.id, sizeof m.id);
        sig += m.name;
        sig += '\0';
    }
    return sig;
}

class Canvas;

}  // namespace

struct StoryboardView::Impl {
    StoryboardView* owner = nullptr;
    Canvas* canvas = nullptr;
    std::vector<Cell> cells;
    std::set<uint64_t> selected;
    uint64_t anchor = 0;
    bool synced = true;
    bool autoSync = false;
    std::string signature;
    int columns = 1, rows = 1;
    std::map<QString, QImage> thumbs;
    std::set<QString> asked;
    std::unique_ptr<FrameWorker> worker;
    std::unique_ptr<ListenerHandle> listener;
    Alive alive = makeAlive();
    // a drag in progress: Alt+drag of cell 'dragFrom' (dropAt: the place it would go), or media over cell 'dropCell'
    int dragFrom = -1, dropAt = -1, dropCell = -1, dropWhere = -1;
    QPoint pressPos;
    int pressCell = -1;
    bool pressAlt = false;

    ~Impl()
    {
        alive->store(false);
        listener.reset();
        worker.reset();
    }

    int cellCount() const { return (int)cells.size(); }
    // Cells drawn: those with events, the rest of the last row empty, and one more empty row.
    int drawnCells() const
    {
        const int n = cellCount();
        const int filled = (n + columns - 1) / columns * columns;
        return std::max(filled, n) + columns;
    }
    QRect cellRect(int k) const
    {
        const int r = k / columns, c = k % columns;
        return QRect(c * kCellW, r * (kCellH + kTimeH) + kTimeH, kCellW, kCellH);
    }
    int cellAt(const QPoint& pos) const
    {
        for (int k = 0; k < drawnCells(); ++k)
            if (cellRect(k).adjusted(0, -kTimeH, 0, 0).contains(pos)) return k;
        return -1;
    }
    void layout();
    void update();

    QImage thumbFor(const Cell& c)
    {
        if (c.thumbKey.isEmpty() || !c.video || c.generated || c.offline) return QImage();
        auto it = thumbs.find(c.thumbKey);
        if (it != thumbs.end()) return it->second;
        if (!asked.count(c.thumbKey)) {
            asked.insert(c.thumbKey);
            const MediaItem* m = AppState::get().project().findMedia(c.media);
            if (m) {
                FrameJob job;
                job.key = c.thumbKey.toStdString();
                job.path = m->path;
                job.video = m->info.hasVideo;
                job.time = c.thumbKey.section('@', 1).toLongLong() / 1000.0;
                job.opener = AppState::get().mediaOpener;
                worker->add(job);
            }
        }
        return QImage();
    }

    void rebuild()
    {
        const Project& p = AppState::get().project();
        cells = cellsOf(p);
        signature = timelineSignature(p);
        synced = true;
        std::set<uint64_t> keep;
        for (const Cell& c : cells)
            if (selected.count(c.event)) keep.insert(c.event);
        selected = keep;
        layout();
        update();
    }

    void projectChanged()
    {
        const std::string sig = timelineSignature(AppState::get().project());
        if (sig == signature) return;
        if (autoSync) {
            rebuild();
        } else if (synced) {
            synced = false;
            update();
        }
    }

    // While out of step nothing may change: offer to refresh.
    bool mayChange()
    {
        if (synced) return true;
        bool yes = false;
        if (owner->askRefresh) yes = owner->askRefresh();
        else
            yes = QMessageBox::question(owner->window(), "Storyboard",
                                        "The Main Timeline storyboard does not show the timeline as it is now, so it cannot be changed.\n\n"
                                        "Refresh the storyboard now?") == QMessageBox::Yes;
        if (yes) rebuild();
        return false;
    }

    bool moveCell(int from, int to)
    {
        if (!mayChange()) return false;
        const int n = cellCount();
        if (from < 0 || from >= n || to < 0) return false;
        to = std::min(to, n);
        if (to == from || to == from + 1) return false;
        const Cell moved = cells[(size_t)from];
        const uint64_t target = to < n ? cells[(size_t)to].event : 0;
        bool done = false;
        AppState::get().edit("Rearrange Storyboard", [&](Project& p) {
            const Event* me = p.findEvent(moved.event);
            if (!me) return;
            const uint64_t group = me->group;
            const double s0 = me->start, len = me->length;
            auto inMoved = [&](const Event& e) { return e.id == moved.event || (group && e.group == group); };
            // close the gap: what follows starts where the moved event did
            double next = -1;
            for (const Track& t : p.tracks)
                for (const Event& e : t.events)
                    if (!inMoved(e) && e.start > s0 + 1e-9 && (next < 0 || e.start < next)) next = e.start;
            if (next >= 0) {
                const double gap = next - s0;
                for (Track& t : p.tracks)
                    for (Event& e : t.events.mut())
                        if (!inMoved(e) && e.start > s0 + 1e-9) e.start -= gap;
            }
            // make room at the target's place (now shifted) or at the end
            double at = 0;
            if (target) {
                const Event* te = p.findEvent(target);
                if (!te) return;
                at = te->start;
                for (Track& t : p.tracks)
                    for (Event& e : t.events.mut())
                        if (!inMoved(e) && e.start >= at - 1e-9) e.start += len;
            } else {
                for (const Track& t : p.tracks)
                    for (const Event& e : t.events)
                        if (!inMoved(e)) at = std::max(at, e.start + e.length);
            }
            const double by = at - s0;
            for (Track& t : p.tracks)
                for (Event& e : t.events.mut())
                    if (inMoved(e)) e.start = std::max(0.0, e.start + by);
            for (Track& t : p.tracks) t.sortEvents();
            done = true;
        });
        rebuild();
        if (done) {
            selected = {moved.event};
            anchor = moved.event;
            update();
            if (owner->onSelectionChanged) owner->onSelectionChanged();
        }
        return done;
    }

    // How long a media item is on the timeline when added (as AppState::addMediaToTimeline makes it).
    static double addedLength(const MediaItem& m)
    {
        const AppState& app = AppState::get();
        if (m.generated) return app.prefs.generatedLength;
        if (m.subclipLength > 0 && !m.info.isStill) return m.subclipLength;
        if (m.info.isStill || m.info.duration <= 0) return app.prefs.stillLength;
        return m.info.duration;
    }

    bool placeMedia(const std::vector<uint64_t>& ids, int cell, int where)
    {
        AppState& app = AppState::get();
        if (ids.empty() || !mayChange()) return false;
        const int n = cellCount();
        bool any = false;
        if (cell >= 0 && cell < n && where == 1) {
            // onto a cell: its event shows the (first) dropped media instead
            const MediaItem* m = app.project().findMedia(ids.front());
            const uint64_t ev = cells[(size_t)cell].event;
            if (!m) return false;
            const bool hasVideo = m->info.hasVideo, hasAudio = m->info.hasAudio;
            const uint64_t id = m->id;
            app.edit("Replace in Storyboard", [&](Project& p) {
                Track* t = nullptr;
                Event* e = p.findEvent(ev, &t);
                if (!e) return;
                const uint64_t group = e->group;
                for (Track& tr : p.tracks)
                    for (Event& x : tr.events.mut()) {
                        if (x.id != ev && (!group || x.group != group)) continue;
                        if ((tr.type == TrackType::Video && !hasVideo) || (tr.type != TrackType::Video && !hasAudio)) continue;
                        x.mediaId = id;
                        x.offset = 0;
                        x.takes.clear();
                        x.activeTake = 0;
                        any = true;
                    }
            });
        } else {
            // inserted: before the cell, after it, or at the end; what follows moves right to make room
            double at = 0;
            uint64_t track = app.currentTrack;
            if (cell >= 0 && cell < n) {
                const Cell& c = cells[(size_t)cell];
                track = c.track;
                if (where == 0) {
                    at = c.start;
                } else {
                    at = c.start + c.length;
                    if (cell + 1 < n) at = cells[(size_t)cell + 1].start;
                }
            } else {
                for (const Cell& c : cells) at = std::max(at, c.start + c.length);
                if (!cells.empty()) track = cells.back().track;
            }
            for (uint64_t id : ids) {
                const MediaItem* m = app.project().findMedia(id);
                if (!m) continue;
                const double len = addedLength(*m);
                app.edit("Insert into Storyboard", [&](Project& p) {
                    for (Track& t : p.tracks)
                        for (Event& e : t.events.mut())
                            if (e.start >= at - 1e-9) e.start += len;
                });
                const std::vector<uint64_t> made = app.addMediaToTimeline(id, at, track);
                if (!made.empty()) any = true;
                at += len;
            }
        }
        rebuild();
        return any;
    }

    void select(int k, Qt::KeyboardModifiers mods)
    {
        if (k < 0 || k >= cellCount()) {
            if (!(mods & (Qt::ControlModifier | Qt::ShiftModifier))) selected.clear();
        } else {
            const uint64_t ev = cells[(size_t)k].event;
            if (mods & Qt::ShiftModifier) {
                int a = 0;
                for (int i = 0; i < cellCount(); ++i)
                    if (cells[(size_t)i].event == anchor) a = i;
                if (!(mods & Qt::ControlModifier)) selected.clear();
                for (int i = std::min(a, k); i <= std::max(a, k); ++i) selected.insert(cells[(size_t)i].event);
            } else if (mods & Qt::ControlModifier) {
                if (!selected.erase(ev)) selected.insert(ev);
                anchor = ev;
            } else {
                selected = {ev};
                anchor = ev;
            }
        }
        update();
        if (owner->onSelectionChanged) owner->onSelectionChanged();
    }

    static std::vector<uint64_t> mediaFrom(const QMimeData* md)
    {
        std::vector<uint64_t> ids;
        if (!md) return ids;
        if (md->hasFormat(kMimeMediaIds) || md->hasFormat(kMimeMedia)) {
            const QByteArray list = md->hasFormat(kMimeMediaIds) ? md->data(kMimeMediaIds) : md->data(kMimeMedia);
            for (const QByteArray& part : list.split(' ')) {
                bool ok = false;
                const uint64_t id = part.trimmed().toULongLong(&ok);
                if (ok && id) ids.push_back(id);
            }
        }
        return ids;
    }
    static QStringList filesFrom(const QMimeData* md)
    {
        QStringList files;
        if (md && md->hasUrls())
            for (const QUrl& u : md->urls())
                if (u.isLocalFile()) files << u.toLocalFile();
        return files;
    }
    // Where a drop at 'pos' goes: the cell and 0 before it, 1 onto it, 2 after it.
    void dropTarget(const QPoint& pos, int& cell, int& where) const
    {
        cell = cellAt(pos);
        where = 0;
        if (cell < 0) {
            cell = cellCount();
            return;
        }
        if (cell >= cellCount()) {
            cell = cellCount();
            return;
        }
        const QRect r = cellRect(cell);
        const int x = pos.x() - r.left();
        where = x < r.width() / 4 ? 0 : x > r.width() * 3 / 4 ? 2 : 1;
    }
};

namespace {

class Canvas : public QWidget {
public:
    Canvas(StoryboardView::Impl* d, QWidget* parent) : QWidget(parent), d_(d)
    {
        setAcceptDrops(true);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        // the background: the window's, or VEGAS's red while out of step with the timeline
        p.fillRect(rect(), d_->synced ? QColor(45, 45, 48) : QColor(168, 90, 80));
        QFont small = font();
        small.setPixelSize(10);
        QFont nameFont = font();
        nameFont.setPixelSize(10);
        const double fps = AppState::get().project().settings.fps() > 0 ? AppState::get().project().settings.fps() : 30.0;
        const int drawn = d_->drawnCells();
        for (int k = 0; k < drawn; ++k) {
            const QRect r = d_->cellRect(k);
            if (k >= d_->cellCount()) {
                p.fillRect(r.adjusted(1, 1, -1, -1), QColor(68, 68, 70));
                p.setPen(QColor(30, 30, 32));
                p.drawRect(r.adjusted(0, 0, -1, -1));
                continue;
            }
            const Cell& c = d_->cells[(size_t)k];
            const bool sel = d_->selected.count(c.event) > 0;
            p.setFont(small);
            p.setPen(QColor(220, 220, 220));
            p.drawText(QRect(r.left() + 4, r.top() - kTimeH, r.width() - 4, kTimeH), Qt::AlignLeft | Qt::AlignVCenter, qs(timecode(c.start, fps)));
            p.fillRect(r.adjusted(1, 1, -1, -1), sel ? QColor(51, 153, 255) : QColor(250, 250, 250));
            p.setPen(QColor(30, 30, 32));
            p.drawRect(r.adjusted(0, 0, -1, -1));
            const QRect pic(r.left() + (r.width() - kCellThumb.width()) / 2, r.top() + 6, kCellThumb.width(), kCellThumb.height());
            if (c.generated) {
                drawChecker(p, pic);
            } else if (!c.video) {
                p.fillRect(pic, QColor(40, 40, 44));
                drawNoteIcon(p, pic);
            } else {
                const QImage img = d_->thumbFor(c);
                p.fillRect(pic, QColor(40, 40, 44));
                if (!img.isNull())
                    p.drawImage(QPoint(pic.left() + (pic.width() - img.width()) / 2, pic.top() + (pic.height() - img.height()) / 2), img);
                if (c.offline) {
                    p.setPen(QPen(QColor(200, 70, 60), 3));
                    p.drawLine(pic.center() + QPoint(-12, -12), pic.center() + QPoint(12, 12));
                    p.drawLine(pic.center() + QPoint(12, -12), pic.center() + QPoint(-12, 12));
                }
            }
            p.setFont(nameFont);
            p.setPen(sel ? QColor(255, 255, 255) : QColor(150, 150, 150));
            p.drawText(QRect(r.left() + 3, pic.bottom() + 3, r.width() - 6, r.bottom() - pic.bottom() - 4),
                       Qt::AlignHCenter | Qt::AlignTop | Qt::TextWrapAnywhere, c.name);
        }
        // where an Alt+drag or a drop would put it
        int markCell = -1, where = 0;
        if (d_->dragFrom >= 0 && d_->dropAt >= 0) {
            markCell = d_->dropAt;
        } else if (d_->dropCell >= 0) {
            markCell = d_->dropCell;
            where = d_->dropWhere;
        }
        if (markCell >= 0) {
            const QRect r = d_->cellRect(std::min(markCell, d_->drawnCells() - 1));
            if (where == 1) {
                p.setPen(QPen(QColor(51, 153, 255), 3));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(2, 2, -2, -2));
            } else {
                const int x = where == 2 ? r.right() : r.left();
                p.fillRect(QRect(x - 2, r.top(), 4, r.height()), QColor(255, 255, 255));
                p.fillRect(QRect(x - 1, r.top(), 2, r.height()), QColor(51, 153, 255));
            }
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int k = d_->cellAt(e->position().toPoint());
        d_->pressPos = e->position().toPoint();
        d_->pressCell = k >= 0 && k < d_->cellCount() ? k : -1;
        d_->pressAlt = (e->modifiers() & Qt::AltModifier) != 0;
        if (d_->pressAlt && d_->pressCell >= 0) return;    // an Alt+drag starts
        d_->select(k, e->modifiers());
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || d_->pressCell < 0) return;
        const QPoint pos = e->position().toPoint();
        if ((pos - d_->pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        if (d_->pressAlt) {
            d_->dragFrom = d_->pressCell;
            int cell = d_->cellAt(pos);
            if (cell < 0 || cell > d_->cellCount()) cell = d_->cellCount();
            else if (cell < d_->cellCount() && pos.x() - d_->cellRect(cell).left() > kCellW / 2) ++cell;
            d_->dropAt = cell;
            setCursor(Qt::DragMoveCursor);
            update();
            return;
        }
        // a plain drag carries the cell's media, as a drag from Project Media does
        const Cell c = d_->cells[(size_t)d_->pressCell];
        d_->pressCell = -1;
        auto* md = new QMimeData();
        md->setData(kMimeMedia, QByteArray::number((qulonglong)c.media));
        QDrag* drag = new QDrag(this);
        drag->setMimeData(md);
        drag->exec(Qt::CopyAction);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int from = d_->dragFrom, to = d_->dropAt;
        d_->dragFrom = d_->dropAt = -1;
        d_->pressCell = -1;
        unsetCursor();
        if (from >= 0 && to >= 0) {
            // (an Alt+drop on the cell itself or just after it leaves the order as it is)
            d_->owner->moveCell(from, to);
        } else if (d_->pressAlt) {
            d_->select(d_->cellAt(e->position().toPoint()), Qt::NoModifier);
        }
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        // the cursor goes to the event's start, and the event is selected on the timeline
        const int k = d_->cellAt(e->position().toPoint());
        if (k < 0 || k >= d_->cellCount()) return;
        AppState& app = AppState::get();
        app.setSelection({d_->cells[(size_t)k].event});
        app.setCursor(d_->cells[(size_t)k].start);
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->source() == this) { e->ignore(); return; }
        const QMimeData* md = e->mimeData();
        if (!StoryboardView::Impl::mediaFrom(md).empty() || !StoryboardView::Impl::filesFrom(md).isEmpty()) e->acceptProposedAction();
        else e->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (e->source() == this) { e->ignore(); return; }
        d_->dropTarget(e->position().toPoint(), d_->dropCell, d_->dropWhere);
        e->acceptProposedAction();
        update();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override
    {
        d_->dropCell = d_->dropWhere = -1;
        update();
    }
    void dropEvent(QDropEvent* e) override
    {
        int cell = 0, where = 0;
        d_->dropTarget(e->position().toPoint(), cell, where);
        d_->dropCell = d_->dropWhere = -1;
        std::vector<uint64_t> ids = StoryboardView::Impl::mediaFrom(e->mimeData());
        const QStringList files = StoryboardView::Impl::filesFrom(e->mimeData());
        if (ids.empty() && !files.isEmpty()) {
            if (!d_->synced) {
                d_->mayChange();
                e->ignore();
                return;
            }
            for (const QString& f : files) {
                std::string err;
                if (const uint64_t id = AppState::get().importMedia(f.toStdString(), &err)) ids.push_back(id);
            }
        }
        if (ids.empty()) { e->ignore(); return; }
        e->acceptProposedAction();
        d_->placeMedia(ids, cell, where);
    }

private:
    StoryboardView::Impl* d_;
};

}  // namespace

void StoryboardView::Impl::layout()
{
    const int w = std::max(kCellW, owner->viewport()->width());
    columns = std::max(1, w / kCellW);
    rows = (drawnCells() + columns - 1) / columns;
    canvas->resize(w, rows * (kCellH + kTimeH) + 4);
}

void StoryboardView::Impl::update() { canvas->update(); }

StoryboardView::StoryboardView(QWidget* parent) : QScrollArea(parent), d(new Impl())
{
    d->owner = this;
    Impl* impl = d.get();
    setObjectName("storyboard");
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    d->canvas = new Canvas(impl, this);
    d->canvas->setObjectName("storyboardCells");
    setWidget(d->canvas);
    d->autoSync = QSettings().value("media/storyboardAutoSync", false).toBool();
    d->worker.reset(new FrameWorker([impl](const std::string& key, const QImage& img) {
        const Alive alive = impl->alive;
        AppState::post([alive, impl, key, img] {
            if (!alive->load()) return;
            impl->thumbs[QString::fromStdString(key)] = img;
            impl->update();
        });
    }));
    d->listener.reset(new ListenerHandle([impl, serial = std::make_shared<uint64_t>(AppState::get().projectSerial())](int changes) {
        // (another project is shown as it is; a change to this one leaves the storyboard out of step)
        if ((changes & kFile) && AppState::get().projectSerial() != *serial) {
            *serial = AppState::get().projectSerial();
            impl->rebuild();
        } else if (changes & kProject) {
            impl->projectChanged();
        }
    }));
    d->rebuild();
}

StoryboardView::~StoryboardView() {}

void StoryboardView::resizeEvent(QResizeEvent* e)
{
    QScrollArea::resizeEvent(e);
    d->layout();
}

void StoryboardView::refresh() { d->rebuild(); }
bool StoryboardView::inSync() const { return d->synced; }
void StoryboardView::setAutoSync(bool on)
{
    d->autoSync = on;
    QSettings().setValue("media/storyboardAutoSync", on);
    if (on && !d->synced) d->rebuild();
}
bool StoryboardView::autoSync() const { return d->autoSync; }
std::vector<uint64_t> StoryboardView::events() const
{
    std::vector<uint64_t> out;
    for (const Cell& c : d->cells) out.push_back(c.event);
    return out;
}
std::vector<uint64_t> StoryboardView::selectedEvents() const
{
    std::vector<uint64_t> out;
    for (const Cell& c : d->cells)
        if (d->selected.count(c.event)) out.push_back(c.event);
    return out;
}
bool StoryboardView::moveCell(int from, int to) { return d->moveCell(from, to); }
bool StoryboardView::placeMedia(const std::vector<uint64_t>& mediaIds, int cell, int where) { return d->placeMedia(mediaIds, cell, where); }
QWidget* StoryboardView::canvas() const { return d->canvas; }
QRect StoryboardView::cellRect(int cell) const { return d->cellRect(cell); }

}  // namespace app
}  // namespace reno
