// Motion Tracking: see motiontracking.h.
#include "motiontracking.h"
#include "toolwin.h"
#include "playback.h"
#include "../engine/tracker.h"
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QStandardItemModel>
#include <QToolButton>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>

namespace reno {
namespace app {

using namespace tw;

namespace {
// images are followed at most this big: VEGAS's Precision (Fast, Balanced, Accurate)
const int kWorkSizes[3] = {320, 640, 960};

// The image with the box and the path over it; drags make or move the box.
class TrackView : public QWidget {
public:
    explicit TrackView(QWidget* parent) : QWidget(parent)
    {
        setObjectName("trackingPicture");
        setMinimumSize(240, 135);
        setMouseTracking(true);
    }
    QImage image;
    bool box = false;
    QRectF boxF;
    std::vector<QPointF> path;
    bool vectors = false;
    QString message;
    std::function<void()> onBoxChanged;

    QRectF target() const
    {
        if (image.isNull()) return QRectF();
        const QSizeF s = QSizeF(image.size()).scaled(size(), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
    }
    QPointF toView(QPointF f) const
    {
        const QRectF t = target();
        return QPointF(t.left() + f.x() * t.width(), t.top() + f.y() * t.height());
    }
    QPointF toFrac(QPointF v) const
    {
        const QRectF t = target();
        if (t.isEmpty()) return QPointF();
        return QPointF(std::min(1.0, std::max(0.0, (v.x() - t.left()) / t.width())), std::min(1.0, std::max(0.0, (v.y() - t.top()) / t.height())));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), gap());
        if (image.isNull()) {
            p.setPen(QColor(150, 150, 150));
            p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, message);
            return;
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(target(), image);
        p.setRenderHint(QPainter::Antialiasing, true);
        if (path.size() > 1) {
            p.setPen(QPen(QColor(255, 210, 60), 1.5));
            for (size_t k = 1; k < path.size(); ++k) p.drawLine(toView(path[k - 1]), toView(path[k]));
        }
        if (vectors && path.size() > 1) {
            p.setPen(QPen(QColor(120, 255, 140), 1.2));
            const size_t step = std::max<size_t>(1, path.size() / 60);
            for (size_t k = step; k < path.size(); k += step) {
                const QPointF a = toView(path[k - step]), b = toView(path[k]);
                const QPointF d = (b - a) * 3.0;
                const QPointF tip = a + d;
                p.drawLine(a, tip);
                const double len = std::hypot(d.x(), d.y());
                if (len > 4) {
                    const QPointF u = d / len, n(-u.y(), u.x());
                    p.drawLine(tip, tip - u * 4 + n * 3);
                    p.drawLine(tip, tip - u * 4 - n * 3);
                }
            }
        }
        if (box) {
            const QRectF b(toView(boxF.topLeft()), toView(boxF.bottomRight()));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0), 3));
            p.drawRect(b);
            p.setPen(QPen(QColor(60, 220, 255), 1.5));
            p.drawRect(b);
            p.drawLine(b.center() - QPointF(5, 0), b.center() + QPointF(5, 0));
            p.drawLine(b.center() - QPointF(0, 5), b.center() + QPointF(0, 5));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || image.isNull()) return;
        const QPointF f = toFrac(e->position());
        moving_ = box && boxF.contains(f);
        anchor_ = f;
        start_ = boxF;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || image.isNull()) return;
        const QPointF f = toFrac(e->position());
        if (moving_) {
            QRectF b = start_.translated(f - anchor_);
            b.moveLeft(std::min(1.0 - b.width(), std::max(0.0, b.left())));
            b.moveTop(std::min(1.0 - b.height(), std::max(0.0, b.top())));
            boxF = b;
        } else {
            boxF = QRectF(anchor_, f).normalized();
            box = boxF.width() > 0.01 && boxF.height() > 0.01;
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (onBoxChanged) onBoxChanged();
    }

private:
    bool moving_ = false;
    QPointF anchor_;
    QRectF start_;
};

// VEGAS's motion track indicator: the event's length, green where frames have motion data, and the cursor.
class TrackIndicator : public QWidget {
public:
    explicit TrackIndicator(QWidget* parent) : QWidget(parent)
    {
        setObjectName("trackIndicator");
        setFixedHeight(10);
    }
    int64_t first = 0, last = -1, cursor = -1;
    std::vector<int64_t> frames;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(40, 40, 44));
        if (last < first) return;
        const double n = (double)(last - first + 1);
        auto x = [&](int64_t f) { return (int)std::lround((f - first) / n * width()); };
        for (int64_t f : frames) p.fillRect(QRect(x(f), 1, std::max(1, x(f + 1) - x(f)), height() - 2), QColor(70, 200, 90));
        if (cursor >= first && cursor <= last) p.fillRect(QRect(x(cursor), 0, 1, height()), QColor(255, 255, 255));
    }
};
}

struct MotionTrackingWindow::Impl {
    MotionTrackingWindow* q = nullptr;
    Playback* pb = nullptr;
    MediaPool pool;
    std::unique_ptr<VideoEngine> engine;
    TrackView* view = nullptr;
    TrackIndicator* indicator = nullptr;
    QLabel* status = nullptr;
    QLabel* clipName = nullptr;
    QComboBox* precisionBox = nullptr;
    int workSize = kWorkSizes[1];
    QComboBox* applyTo = nullptr;
    QDoubleSpinBox* zoom = nullptr;
    std::map<int64_t, Point> points;
    uint64_t eventId = 0;
    int maxSteps = 1 << 30;
    std::unique_ptr<Listener> listener;

    const Event* ev(const Track** tr = nullptr) const
    {
        const Project& p = AppState::get().project();
        return eventId ? p.findEvent(eventId, tr) : nullptr;
    }

    void pickEvent()
    {
        AppState& app = AppState::get();
        uint64_t now = 0;
        for (uint64_t id : app.selection()) {
            FxTarget t;
            t.kind = FxTarget::Event;
            t.id = id;
            if (app.targetIsVideo(t)) { now = id; break; }
        }
        if (now != eventId) {
            eventId = now;
            points.clear();
            view->box = false;
        }
    }

    ImagePtr pictureAt(double t)
    {
        const Project& p = AppState::get().project();
        const Event* e = ev();
        if (!e || t < e->start - 1e-9 || t >= e->end()) return nullptr;
        return engine->eventPicture(p, *e, t, workSize, workSize);
    }

    void show()
    {
        pickEvent();
        AppState& app = AppState::get();
        const Event* e = ev();
        view->image = QImage();
        if (!e) view->message = "Select a video event to track something in it.";
        else {
            ImagePtr img = pictureAt(app.project().frameTime(app.project().frameAt(app.cursor())));
            if (img) view->image = toQImage(*img);
            else view->message = "Move the cursor over the event (its media has no picture there).";
        }
        view->path.clear();
        for (const auto& kv : points) view->path.push_back(QPointF(kv.second.x, kv.second.y));
        clipName->setText(e ? QString("Clip: %1").arg(qs(e->name.empty() ? std::string("(no name)") : e->name)) : QString("Clip: (no video event selected)"));
        indicator->frames.clear();
        for (const auto& kv : points) indicator->frames.push_back(kv.first);
        if (e) {
            const Project& pr = app.project();
            indicator->first = pr.frameAt(e->start + 1e-9);
            indicator->last = pr.frameAt(e->end() - 1e-9);
            indicator->cursor = pr.frameAt(app.cursor());
        } else {
            indicator->last = indicator->first - 1;
        }
        indicator->update();
        if (!points.empty()) {
            const int64_t f = app.project().frameAt(app.cursor());
            auto it = points.find(f);
            if (it != points.end()) view->boxF.moveCenter(QPointF(it->second.x, it->second.y));
        }
        status->setText(points.empty() ? (view->box ? QString("Track Forward or Backward to follow the box.") : QString("Drag a box round what to follow."))
                                       : QString("%1 frames tracked, %2 .. %3").arg(points.size())
                                             .arg(qs(timecode(points.begin()->second.time, app.project().settings.fps())))
                                             .arg(qs(timecode(points.rbegin()->second.time, app.project().settings.fps()))));
        fillApply();
        view->update();
    }

    void fillApply()
    {
        const QString keep = applyTo->currentData().toString();
        applyTo->clear();
        const Project& p = AppState::get().project();
        int n = 0;
        for (const Track& t : p.tracks) {
            ++n;
            if (t.type != TrackType::Video) continue;
            applyTo->addItem(QString("Track Motion of track %1%2").arg(n).arg(t.name.empty() ? QString() : " (" + qs(t.name) + ")"),
                             QString("track:%1").arg((qulonglong)t.id));
        }
        applyTo->addItem("Pan/Crop of the event: keep it in the middle", "follow");
        applyTo->addItem("Pan/Crop of the event: hold it still (stabilize)", "stabilize");
        const int i = applyTo->findData(keep);
        if (i >= 0) applyTo->setCurrentIndex(i);
    }
};

MotionTrackingWindow::MotionTrackingWindow(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->q = this;
    d->pb = playback;
    setObjectName("motionTracking");
    d->pool.setOpener([](const std::string& path, std::string* err) { return openReader(path, err); });
    d->engine.reset(new VideoEngine(d->pool, AppState::get().fx));
    Impl* impl = d.get();
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    // VEGAS 18's panel: clip name; Precision, Mode, Show Motion Vectors; the image with the box; the motion track
    // indicator and status line; the five track buttons; where the motion goes.
    d->clipName = new QLabel(this);
    d->clipName->setObjectName("trackClipName");
    v->addWidget(d->clipName);
    auto* opts = new QHBoxLayout();
    d->precisionBox = new QComboBox(this);
    d->precisionBox->setObjectName("trackPrecision");
    d->precisionBox->addItems({"Fast", "Balanced", "Accurate"});
    d->precisionBox->setCurrentIndex(1);
    d->precisionBox->setToolTip("Precision: Fast tracks smaller images (quicker), Accurate larger ones (slower)");
    QComboBox* mode = new QComboBox(this);
    mode->setObjectName("trackMode");
    mode->addItems({"Location", "Rotation & Location", "Scale & Location", "Shape & Location", "Perspective"});
    // Reno follows where the box goes; other kinds of movement are listed greyed as not available
    if (auto* model = qobject_cast<QStandardItemModel*>(mode->model()))
        for (int i = 1; i < model->rowCount(); ++i) {
            model->item(i)->setEnabled(false);
            model->item(i)->setToolTip("Not available: Reno follows the position (Location) only");
        }
    mode->setToolTip("Mode: the kind of movement looked for (Reno: Location)");
    QCheckBox* vectors = new QCheckBox("Show Motion Vectors", this);
    vectors->setObjectName("trackVectors");
    opts->addWidget(new QLabel("Precision:", this));
    opts->addWidget(d->precisionBox);
    opts->addWidget(new QLabel("Mode:", this));
    opts->addWidget(mode);
    opts->addWidget(vectors);
    opts->addStretch(1);
    v->addLayout(opts);
    d->view = new TrackView(this);
    v->addWidget(d->view, 1);
    d->indicator = new TrackIndicator(this);
    v->addWidget(d->indicator);
    d->status = new QLabel(this);
    d->status->setObjectName("trackStatus");
    v->addWidget(d->status);
    auto* top = new QHBoxLayout();
    // track buttons as VEGAS numbers them: (1) backward, (2) a frame backward, (3) both directions, (4) a frame forward, (5) forward
    QPushButton* back = new QPushButton(QString::fromUtf8("\xe2\x97\x80\xe2\x97\x80"), this);
    back->setObjectName("trackBackward");
    back->setToolTip("Track backward from the cursor to the start of the event (or time selection), or until the box is lost");
    QPushButton* backOne = new QPushButton(QString::fromUtf8("\xe2\x97\x80|"), this);
    backOne->setObjectName("trackBackOne");
    backOne->setToolTip("Track one frame backward");
    QPushButton* both = new QPushButton(QString::fromUtf8("\xe2\x97\x80 \xe2\x96\xb6"), this);
    both->setObjectName("trackBoth");
    both->setToolTip("Track both directions from the cursor");
    QPushButton* fwdOne = new QPushButton(QString::fromUtf8("|\xe2\x96\xb6"), this);
    fwdOne->setObjectName("trackForwardOne");
    fwdOne->setToolTip("Track one frame forward");
    QPushButton* fwd = new QPushButton(QString::fromUtf8("\xe2\x96\xb6\xe2\x96\xb6"), this);
    fwd->setObjectName("trackForward");
    fwd->setToolTip("Track forward from the cursor to the end of the event (or time selection), or until the box is lost");
    QPushButton* clear = new QPushButton("Clear", this);
    clear->setObjectName("trackClear");
    for (QPushButton* b : {back, backOne, both, fwdOne, fwd}) top->addWidget(b);
    top->addSpacing(8);
    top->addWidget(clear);
    top->addStretch(1);
    // the motion transfer menu: the events, their Pan/Crop or their effects' position settings, the tracks' Track Motion
    QToolButton* transfer = new QToolButton(this);
    transfer->setObjectName("trackTransfer");
    transfer->setText("Transfer Motion");
    transfer->setToolTip("Motion transfer: choose the event, effect and position setting that should follow the tracked motion");
    transfer->setPopupMode(QToolButton::InstantPopup);
    QMenu* transferMenu = new QMenu(transfer);
    transfer->setMenu(transferMenu);
    top->addWidget(transfer);
    v->addLayout(top);
    auto* bottom = new QHBoxLayout();
    d->applyTo = new QComboBox(this);
    d->applyTo->setObjectName("trackApplyTo");
    d->zoom = new QDoubleSpinBox(this);
    d->zoom->setObjectName("trackZoom");
    d->zoom->setRange(0.1, 1.0);
    d->zoom->setSingleStep(0.05);
    d->zoom->setValue(0.85);
    d->zoom->setToolTip("Pan/Crop: how much of the media the box shows (smaller is zoomed in, leaving room to move)");
    QPushButton* apply = new QPushButton("Apply", this);
    apply->setObjectName("trackApply");
    bottom->addWidget(new QLabel("Apply to:", this));
    bottom->addWidget(d->applyTo, 1);
    bottom->addWidget(new QLabel("Size:", this));
    bottom->addWidget(d->zoom);
    bottom->addWidget(apply);
    v->addLayout(bottom);
    auto run = [this](int dir) {
        QProgressDialog progress("Tracking...", "Stop", 0, 1000, window());
        progress.setWindowTitle("Motion Tracking");
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(400);
        bool lost = false;
        const int n = track(dir, [&](double f) {
            progress.setValue((int)(f * 1000));
            QCoreApplication::processEvents();
            return !progress.wasCanceled();
        }, &lost);
        progress.setValue(1000);
        AppState::get().status(lost ? strfmt("Motion Tracking: lost after %d frames.", n) : strfmt("Motion Tracking: %d frames followed.", n));
    };
    QObject::connect(fwd, &QPushButton::clicked, this, [run] { run(1); });
    QObject::connect(back, &QPushButton::clicked, this, [run] { run(-1); });
    QObject::connect(fwdOne, &QPushButton::clicked, this, [this] { trackOneFrame(1); });
    QObject::connect(backOne, &QPushButton::clicked, this, [this] { trackOneFrame(-1); });
    QObject::connect(both, &QPushButton::clicked, this, [this] {
        QProgressDialog progress("Tracking...", "Stop", 0, 1000, window());
        progress.setWindowTitle("Motion Tracking");
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(400);
        const int n = trackBoth([&](double f) {
            progress.setValue((int)(f * 1000));
            QCoreApplication::processEvents();
            return !progress.wasCanceled();
        });
        progress.setValue(1000);
        AppState::get().status(strfmt("Motion Tracking: %d frames followed.", n));
    });
    QObject::connect(d->precisionBox, &QComboBox::activated, this, [this](int i) { setPrecision(i); });
    QObject::connect(vectors, &QCheckBox::toggled, this, [impl](bool on) {
        impl->view->vectors = on;
        impl->view->update();
    });
    QObject::connect(transferMenu, &QMenu::aboutToShow, this, [this, transferMenu] {
        transferMenu->clear();
        const Project& p = AppState::get().project();
        const std::vector<uint64_t> evs = transferEvents();
        if (evs.empty() || points().size() < 2) {
            transferMenu->addAction("(track something first)")->setEnabled(false);
            return;
        }
        for (size_t i = 0; i < evs.size(); ++i) {
            const Track* t = nullptr;
            const Event* e = p.findEvent(evs[i], &t);
            if (!e) continue;
            QMenu* sub = transferMenu->addMenu(qs(e->name.empty() ? std::string("(event)") : e->name) + (i == 0 ? QString("  (tracked)") : QString()));
            if (i == 0) {
                QObject::connect(sub->addAction("Pan/Crop: keep it in the middle"), &QAction::triggered, this, [this] { applyToPanCrop(false, d->zoom->value()); });
                QObject::connect(sub->addAction("Pan/Crop: hold it still (stabilize)"), &QAction::triggered, this, [this] { applyToPanCrop(true, d->zoom->value()); });
            }
            const std::vector<FxTarget2D> ps = positionSettings(evs[i]);
            if (!ps.empty() && i == 0) sub->addSeparator();
            for (const FxTarget2D& t2 : ps) {
                const uint64_t ev = evs[i], slot = t2.slot;
                const std::string param = t2.param;
                QObject::connect(sub->addAction(qs(t2.label)), &QAction::triggered, this, [this, ev, slot, param] { applyToFxParam(ev, slot, param); });
            }
            if (sub->isEmpty()) sub->addAction("(no effect with a position setting)")->setEnabled(false);
        }
        transferMenu->addSeparator();
        int n = 0;
        for (const Track& t : p.tracks) {
            ++n;
            if (t.type != TrackType::Video) continue;
            const uint64_t id = t.id;
            QObject::connect(transferMenu->addAction(QString("Track Motion of track %1%2").arg(n).arg(t.name.empty() ? QString() : " (" + qs(t.name) + ")")),
                             &QAction::triggered, this, [this, id] { applyToTrackMotion(id); });
        }
    });
    QObject::connect(clear, &QPushButton::clicked, this, [this] { clearPoints(); });
    QObject::connect(apply, &QPushButton::clicked, this, [this, impl] {
        const QString what = impl->applyTo->currentData().toString();
        if (what.startsWith("track:")) applyToTrackMotion(what.mid(6).toULongLong());
        else applyToPanCrop(what == "stabilize", impl->zoom->value());
    });
    d->view->onBoxChanged = [impl] {
        impl->points.clear();
        impl->show();
    };
    d->listener.reset(new Listener([this, impl](int changes) {
        if (!isVisible()) return;
        if (changes & (kSelection | kCursor | kProject)) impl->show();
    }));
    d->show();
}

MotionTrackingWindow::~MotionTrackingWindow() {}

uint64_t MotionTrackingWindow::trackedEvent() const { return d->eventId; }

void MotionTrackingWindow::setBox(double cx, double cy, double w, double h)
{
    d->pickEvent();
    d->view->boxF = QRectF(cx - w / 2, cy - h / 2, w, h);
    d->view->box = w > 0 && h > 0;
    d->points.clear();
    d->show();
}

bool MotionTrackingWindow::hasBox() const { return d->view->box; }

int MotionTrackingWindow::track(int direction, const std::function<bool(double)>& progress, bool* lost)
{
    if (lost) *lost = false;
    d->pickEvent();
    const Event* e = d->ev();
    if (!e || !d->view->box) return 0;
    AppState& app = AppState::get();
    const Project& p = app.project();
    const Event evCopy = *e;
    int64_t f = p.frameAt(app.cursor());
    // from the cursor to the event's end or start (or the time selection's, if within the event)
    double a = evCopy.start, b = evCopy.end();
    if (app.loopEnd > app.loopStart) {
        a = std::max(a, app.loopStart);
        b = std::min(b, app.loopEnd);
    }
    const int64_t first = p.frameAt(a + 1e-9), last = p.frameAt(b - 1e-9);
    if (f < first || f > last) return 0;
    ImagePtr img = d->pictureAt(p.frameTime(f));
    if (!img) return 0;
    double scale = 1;
    GrayImage g = toGray(*img, 0, &scale);
    const QRectF bx = d->view->boxF;
    BoxTracker tr;
    if (!tr.start(g, bx.center().x() * g.w, bx.center().y() * g.h, bx.width() * g.w, bx.height() * g.h)) return 0;
    Point p0;
    p0.frame = f;
    p0.time = p.frameTime(f);
    p0.x = bx.center().x();
    p0.y = bx.center().y();
    d->points[f] = p0;
    double cx = p0.x * g.w, cy = p0.y * g.h;
    int n = 0;
    const int64_t total = direction > 0 ? last - f : f - first;
    for (int64_t k = f + direction; direction > 0 ? k <= last : k >= first; k += direction) {
        if (n >= d->maxSteps) break;
        if (progress && !progress(total > 0 ? (double)std::llabs(k - f) / total : 1.0)) break;
        ImagePtr next = d->pictureAt(p.frameTime(k));
        if (!next) break;
        const GrayImage gn = toGray(*next, 0);
        if (gn.w != g.w || gn.h != g.h) break;
        if (!tr.step(gn, cx, cy)) {
            if (lost) *lost = true;
            break;
        }
        Point pt;
        pt.frame = k;
        pt.time = p.frameTime(k);
        pt.x = cx / g.w;
        pt.y = cy / g.h;
        d->points[k] = pt;
        ++n;
    }
    d->show();
    return n;
}

std::vector<MotionTrackingWindow::Point> MotionTrackingWindow::points() const
{
    std::vector<Point> out;
    for (const auto& kv : d->points) out.push_back(kv.second);
    return out;
}

void MotionTrackingWindow::clearPoints()
{
    d->points.clear();
    d->show();
}

bool MotionTrackingWindow::applyToTrackMotion(uint64_t trackId)
{
    const Track* et = nullptr;
    const Event* e = d->ev(&et);
    if (!e || d->points.size() < 2) return false;
    AppState& app = AppState::get();
    const Project& p = app.project();
    const MediaItem* m = p.findMedia(e->mediaId);
    const double W = p.settings.width, H = p.settings.height;
    double mw = m ? m->info.width : 0, mh = m ? m->info.height : 0;
    if (!(mw > 0 && mh > 0)) { mw = W; mh = H; }
    const double s = std::min(W / mw, H / mh), fw = mw * s / W, fh = mh * s / H;
    const double ox = (1 - fw) / 2, oy = (1 - fh) / 2;
    const std::vector<Point> pts = points();
    const double x0 = ox + pts.front().x * fw, y0 = oy + pts.front().y * fh;
    const double t0 = pts.front().time, t1 = pts.back().time;
    bool ok = false;
    app.edit("Motion Tracking", [&](Project& pr) {
        Track* t = pr.findTrack(trackId);
        if (!t || t->type != TrackType::Video) return;
        MotionKey base;
        for (const MotionKey& k : t->motion)
            if (k.time <= t0 + 1e-9) base = k;
        if (base.time > t0 + 1e-9 && !t->motion.empty()) base = t->motion.front();
        std::vector<MotionKey> keep;
        for (const MotionKey& k : t->motion)
            if (k.time < t0 - 1e-9 || k.time > t1 + 1e-9) keep.push_back(k);
        for (const Point& pt : pts) {
            MotionKey k = base;
            k.time = pt.time;
            k.x = base.x + (ox + pt.x * fw - x0);
            k.y = base.y + (oy + pt.y * fh - y0);
            k.curve = 0;
            keep.push_back(k);
        }
        std::sort(keep.begin(), keep.end(), [](const MotionKey& a, const MotionKey& b) { return a.time < b.time; });
        t->motion = keep;
        ok = true;
    });
    if (ok) app.status(strfmt("Motion Tracking: %d Track Motion keyframes made.", (int)pts.size()));
    return ok;
}

bool MotionTrackingWindow::applyToPanCrop(bool stabilize, double zoom)
{
    const Event* e = d->ev();
    if (!e || d->points.size() < 2) return false;
    AppState& app = AppState::get();
    const std::vector<Point> pts = points();
    const uint64_t id = e->id;
    zoom = std::min(1.0, std::max(0.05, zoom));
    bool ok = false;
    app.edit(stabilize ? "Motion Tracking: Stabilize" : "Motion Tracking: Follow", [&](Project& pr) {
        Event* ev = pr.findEvent(id);
        if (!ev) return;
        const double t0 = pts.front().time - ev->start, t1 = pts.back().time - ev->start;
        std::vector<PanCropKey> keep;
        for (const PanCropKey& k : ev->panCrop)
            if (k.time < t0 - 1e-9 || k.time > t1 + 1e-9) keep.push_back(k);
        for (const Point& pt : pts) {
            PanCropKey k;
            k.time = pt.time - ev->start;
            k.w = k.h = zoom;
            if (stabilize) {
                k.cx = 0.5 + (pt.x - pts.front().x);
                k.cy = 0.5 + (pt.y - pts.front().y);
            } else {
                k.cx = pt.x;
                k.cy = pt.y;
            }
            keep.push_back(k);
        }
        std::sort(keep.begin(), keep.end(), [](const PanCropKey& a, const PanCropKey& b) { return a.time < b.time; });
        ev->panCrop = keep;
        ok = true;
    });
    if (ok) app.status(strfmt("Motion Tracking: %d Pan/Crop keyframes made.", (int)pts.size()));
    return ok;
}

QWidget* MotionTrackingWindow::picture() const { return d->view; }

int MotionTrackingWindow::trackOneFrame(int direction)
{
    d->maxSteps = 1;
    const int n = track(direction < 0 ? -1 : 1);
    d->maxSteps = 1 << 30;
    if (n > 0) {
        AppState& app = AppState::get();
        const int64_t f = app.project().frameAt(app.cursor()) + (direction < 0 ? -1 : 1);
        app.setCursor(app.project().frameTime(f));
    }
    return n;
}

int MotionTrackingWindow::trackBoth(const std::function<bool(double)>& progress)
{
    AppState& app = AppState::get();
    const double at = app.cursor();
    const QRectF box = d->view->boxF;
    bool stop = false;
    const int a = track(-1, [&](double f) {
        if (progress && !progress(f * 0.5)) stop = true;
        return !stop;
    });
    if (stop) return a;
    app.setCursor(at);
    d->view->boxF = box;
    const int b = track(1, [&](double f) { return !progress || progress(0.5 + f * 0.5); });
    return a + b;
}

void MotionTrackingWindow::setPrecision(int p)
{
    p = std::max(0, std::min(2, p));
    if (d->workSize != kWorkSizes[p]) d->points.clear();
    d->workSize = kWorkSizes[p];
    if (d->precisionBox->currentIndex() != p) d->precisionBox->setCurrentIndex(p);
    d->show();
}

int MotionTrackingWindow::precision() const
{
    for (int k = 0; k < 3; ++k)
        if (d->workSize == kWorkSizes[k]) return k;
    return 1;
}

std::vector<uint64_t> MotionTrackingWindow::transferEvents() const
{
    std::vector<uint64_t> out;
    const Event* e = d->ev();
    if (!e) return out;
    out.push_back(e->id);
    const Project& p = AppState::get().project();
    for (const Track& t : p.tracks) {
        if (t.type != TrackType::Video) continue;
        for (const Event& o : t.events)
            if (o.id != e->id && o.start < e->end() && o.end() > e->start) out.push_back(o.id);
    }
    return out;
}

std::vector<MotionTrackingWindow::FxTarget2D> MotionTrackingWindow::positionSettings(uint64_t eventId) const
{
    std::vector<FxTarget2D> out;
    AppState& app = AppState::get();
    const Event* e = app.project().findEvent(eventId);
    if (!e) return out;
    for (const FxSlot& s : e->fx) {
        ofx::Instance* inst = s.kind == "ofx" ? app.fx.video(s.id) : nullptr;
        if (!inst) continue;
        std::unique_lock<std::recursive_mutex> lk(inst->mutex, std::try_to_lock);
        if (!lk.owns_lock()) continue;
        for (const auto& up : inst->params.params) {
            const ofx::Param& prm = *up;
            if (prm.kind != ofx::ParamKind::Double2D || prm.secret()) continue;
            const std::string type = prm.props.getString(kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
            if (type != kOfxParamDoubleTypeXY && type != kOfxParamDoubleTypeXYAbsolute && type != "OfxParamDoubleTypeNormalisedXY" &&
                type != "OfxParamDoubleTypeNormalisedXYAbsolute")
                continue;
            FxTarget2D t;
            t.slot = s.id;
            t.param = prm.name;
            t.label = (s.name.empty() ? std::string("Effect") : s.name) + ": " + prm.label();
            out.push_back(t);
        }
    }
    return out;
}

bool MotionTrackingWindow::applyToFxParam(uint64_t eventId, uint64_t slotId, const std::string& paramName)
{
    const Event* tracked = d->ev();
    if (!tracked || d->points.size() < 2) return false;
    AppState& app = AppState::get();
    const Project& p = app.project();
    const Event* target = p.findEvent(eventId);
    ofx::Instance* inst = app.fx.video(slotId);
    if (!target || !inst) return false;
    ofx::Param* prm = inst->params.find(paramName);
    if (!prm || prm->kind != ofx::ParamKind::Double2D) return false;
    const MediaItem* m = p.findMedia(tracked->mediaId);
    const double W = p.settings.width, H = p.settings.height, par = p.settings.pixelAspect > 0.01 ? p.settings.pixelAspect : 1.0;
    double mw = m ? m->info.width : 0, mh = m ? m->info.height : 0;
    if (!(mw > 0 && mh > 0)) { mw = W; mh = H; }
    const double s = std::min(W / mw, H / mh), fw = mw * s / W, fh = mh * s / H;
    const double ox = (1 - fw) / 2, oy = (1 - fh) / 2;
    // where the setting counts from: fractions of the frame, or its canonical pixels (OpenFX: y up)
    const std::string type = prm->props.getString(kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    const bool normalised = type == "OfxParamDoubleTypeNormalisedXY" || type == "OfxParamDoubleTypeNormalisedXYAbsolute" ||
                            prm->props.getString(kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesCanonical) == kOfxParamCoordinatesNormalised;
    const double fps = p.settings.fps();
    const std::vector<Point> pts = points();
    app.fxEditStep(slotId, "Motion Tracking: " + prm->label(), [&] {
        for (const Point& pt : pts) {
            if (pt.time < target->start - 1e-9 || pt.time > target->end() + 1e-9) continue;
            const double fx = ox + pt.x * fw, fy = oy + pt.y * fh;
            std::vector<double> v = normalised ? std::vector<double>{fx, 1.0 - fy} : std::vector<double>{fx * W * par, (1.0 - fy) * H};
            inst->userSetValue(prm, v, std::floor((pt.time - target->start) * fps + 0.5), true);
        }
    });
    app.status(strfmt("Motion Tracking: %d keyframes of %s.", (int)pts.size(), prm->label().c_str()));
    return true;
}

}
}
