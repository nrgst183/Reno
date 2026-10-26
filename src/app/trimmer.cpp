// VEGAS's Trimmer: see trimmer.h.
//
// The file is read by readers of the Trimmer's own (never the timeline's): images on a thread that decodes the newest
// request,
// the waveform on another (a few seconds at a time, shown as it arrives), and the sound, while playing, on the sound
// device's.
#include "trimmer.h"
#include "toolwin.h"
#include "panels.h"
#include "playback.h"
#include "vprefs.h"
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QImage>
#include <set>
#include <QComboBox>
#include <QDir>
#include <QProcess>
#include <QDrag>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace reno {
namespace app {

using namespace tw;

namespace {

const int kHistoryMax = 25;
const int kWaveRate = 8000;
const int kWaveBuckets = 4000;

class FrameWorker {
public:
    std::function<void(const QImage& img, double t, uint64_t generation)> onFrame;

    FrameWorker() : thread_([this] { run(); }) {}
    ~FrameWorker()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    void setReader(std::unique_ptr<MediaReader> r, uint64_t generation)
    {
        std::lock_guard<std::mutex> lk(m_);
        next_ = std::move(r);
        hasNext_ = true;
        generation_ = generation;
        wanted_ = false;
    }
    void request(double t)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            want_ = t;
            wanted_ = true;
        }
        cv_.notify_one();
    }

private:
    void run()
    {
        std::unique_ptr<MediaReader> reader;
        for (;;) {
            double t;
            uint64_t gen;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return quit_ || wanted_ || hasNext_; });
                if (quit_) return;
                if (hasNext_) {
                    reader = std::move(next_);
                    hasNext_ = false;
                }
                if (!wanted_) continue;
                wanted_ = false;
                t = want_;
                gen = generation_;
            }
            if (!reader || !reader->info().hasVideo) continue;
            ImagePtr img = reader->videoFrame(t, 960, 540);
            if (!img) continue;
            const QImage q = toQImage(*img);
            if (onFrame) onFrame(q, t, gen);
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, wanted_ = false, hasNext_ = false;
    double want_ = 0;
    uint64_t generation_ = 0;
    std::unique_ptr<MediaReader> next_;
    std::thread thread_;
};

struct Wave {
    std::mutex m;
    std::vector<float> peaks;
    std::atomic<bool> done{false}, cancel{false};
    std::thread thread;
    ~Wave()
    {
        cancel = true;
        if (thread.joinable()) thread.join();
    }
};

}

namespace {
class TrimPicture : public QWidget {
public:
    explicit TrimPicture(QWidget* parent) : QWidget(parent)
    {
        setObjectName("trimmerPicture");
        setMinimumSize(160, 90);
    }
    QImage image;
    QString message;
    bool dimmed = false;
    std::function<void(double)> onHover;

protected:
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (onHover && !(e->buttons() & Qt::LeftButton) && width() > 0) onHover(std::min(1.0, std::max(0.0, e->position().x() / width())));
        QWidget::mouseMoveEvent(e);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), gap());
        if (!image.isNull()) {
            const QSize s = image.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect r(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(r, image);
            if (dimmed) p.fillRect(r, QColor(0, 0, 0, 150));
        }
        if (!message.isEmpty()) {
            p.setPen(QColor(150, 150, 150));
            p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, message);
        }
    }
};
}

namespace {
class TrimStrip : public QWidget {
public:
    explicit TrimStrip(QWidget* parent) : QWidget(parent)
    {
        setObjectName("trimmerStrip");
        setMinimumHeight(84);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
    }
    double duration = 0, fps = 30;
    double cursor = 0, in = 0, out = 0;
    std::vector<float> peaks;
    bool hasAudio = false;
    bool dimWave = false;
    std::vector<MediaItem::Marker> marks;
    std::vector<std::pair<double, std::string>> fileMarks;
    std::function<void(double)> onCursor;
    std::function<void(const QPoint&, int)> onMenu;
    std::function<void(double, double)> onSelect;
    std::function<void()> onDragOut;

    static const int kRuler = 18;
    double timeAt(double x) const
    {
        const double w = std::max(1, width() - 1);
        return duration > 0 ? std::min(duration, std::max(0.0, x / w * duration)) : 0.0;
    }
    double xAt(double t) const { return duration > 0 ? t / duration * (width() - 1) : 0.0; }
    bool hasSelection() const { return out > in; }
    int markAt(double x) const
    {
        for (size_t k = 0; k < marks.size(); ++k)
            if (std::fabs(xAt(marks[k].time) - x) <= 5) return (int)k;
        return -1;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), dark());
        const QRect ruler(0, 0, width(), kRuler), lane(0, kRuler, width(), height() - kRuler);
        p.fillRect(ruler, bar());
        if (hasSelection()) {
            const QRectF sel(QPointF(xAt(in), 0), QPointF(xAt(out), height()));
            p.fillRect(sel, QColor(18, 132, 214, 70));
            p.fillRect(QRectF(sel.left(), 0, sel.width(), kRuler), QColor(18, 132, 214, 160));
        }
        if (duration > 0) {
            static const double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
            double step = steps[0];
            for (double s : steps) {
                step = s;
                if (xAt(s) >= 70) break;
            }
            p.setPen(QColor(170, 170, 170));
            QFont f = font();
            f.setPixelSize(9);
            p.setFont(f);
            for (double t = 0; t <= duration + 1e-9; t += step) {
                const int x = (int)std::lround(xAt(t));
                p.drawLine(x, kRuler - 6, x, kRuler - 1);
                p.drawText(QRect(x + 2, 1, 80, kRuler - 4), Qt::AlignLeft | Qt::AlignVCenter, qs(timecode(t, fps)));
            }
        }
        const int mid = lane.center().y();
        if (!peaks.empty()) {
            p.setPen(dimWave ? QColor(80, 92, 104) : QColor(150, 196, 230));
            const double n = (double)peaks.size();
            const double half = lane.height() / 2.0 - 3;
            for (int x = 0; x < width(); ++x) {
                const size_t a = (size_t)std::floor(x * n / width()), b = std::max(a + 1, (size_t)std::floor((x + 1) * n / width()));
                float pk = 0;
                for (size_t k = a; k < b && k < peaks.size(); ++k) pk = std::max(pk, peaks[k]);
                const int h = (int)std::lround(std::min(1.f, pk) * half);
                p.drawLine(x, mid - h, x, mid + h);
            }
        } else {
            p.setPen(QColor(90, 90, 90));
            p.drawLine(0, mid, width(), mid);
            if (!hasAudio && duration > 0) {
                p.setPen(QColor(120, 120, 120));
                p.drawText(lane, Qt::AlignCenter, "(no sound)");
            }
        }
        for (const auto& fm : fileMarks) {
            const double x = std::floor(xAt(fm.first)) + 0.5;
            p.setPen(QPen(QColor(255, 170, 60, 170), 1, Qt::DotLine));
            p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
        }
        QFont mf = font();
        mf.setPixelSize(9);
        mf.setBold(true);
        p.setFont(mf);
        for (size_t k = 0; k < marks.size(); ++k) {
            const MediaItem::Marker& m = marks[k];
            const double x = std::floor(xAt(m.time)) + 0.5;
            const QColor c = m.length > 0 ? QColor(70, 190, 90) : QColor(255, 170, 60);
            if (m.length > 0) {
                const double x2 = std::floor(xAt(m.time + m.length)) + 0.5;
                p.fillRect(QRectF(x, kRuler, x2 - x, height() - kRuler), QColor(70, 190, 90, 40));
                p.setPen(QPen(c, 1));
                p.drawLine(QPointF(x2, kRuler), QPointF(x2, height()));
            }
            p.setPen(QPen(c, 1));
            p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
            const QString label = m.name.empty() ? QString::number(k + 1) : QString("%1 %2").arg(k + 1).arg(qs(m.name));
            const QRectF flag(x, kRuler, QFontMetrics(mf).horizontalAdvance(label) + 6, 12);
            p.fillRect(flag, c);
            p.setPen(QColor(20, 20, 20));
            p.drawText(flag, Qt::AlignCenter, label);
        }
        const int cx = (int)std::lround(xAt(cursor));
        p.setPen(QColor(255, 255, 255));
        p.drawLine(cx, 0, cx, height());
        p.setPen(QColor(25, 25, 25));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::RightButton) {
            if (onMenu) onMenu(e->globalPosition().toPoint(), markAt(e->position().x()));
            return;
        }
        if (e->button() != Qt::LeftButton || duration <= 0) return;
        press_ = e->position();
        anchor_ = timeAt(e->position().x());
        const bool inside = hasSelection() && anchor_ >= in && anchor_ <= out;
        mode_ = inside ? Mode::MaybeDrag : Mode::MaybeSelect;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || mode_ == Mode::None) return;
        const QPointF d = e->position() - press_;
        if (mode_ == Mode::MaybeDrag && (std::fabs(d.y()) > 8 || !rect().contains(e->position().toPoint()))) {
            mode_ = Mode::None;
            if (onDragOut) onDragOut();
            return;
        }
        if (mode_ == Mode::MaybeDrag && std::fabs(d.x()) > 4) mode_ = Mode::Selecting;
        if (mode_ == Mode::MaybeSelect && std::fabs(d.x()) > 3) mode_ = Mode::Selecting;
        if (mode_ == Mode::Selecting && onSelect) {
            const double t = timeAt(e->position().x());
            onSelect(std::min(anchor_, t), std::max(anchor_, t));
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if ((mode_ == Mode::MaybeSelect || mode_ == Mode::MaybeDrag) && onCursor) onCursor(timeAt(e->position().x()));
        if (mode_ == Mode::MaybeSelect && onSelect) onSelect(0, 0);
        mode_ = Mode::None;
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        if (onSelect && duration > 0) onSelect(0, duration);
    }

private:
    enum class Mode { None, MaybeSelect, Selecting, MaybeDrag };
    Mode mode_ = Mode::None;
    QPointF press_;
    double anchor_ = 0;
};
}  // namespace

struct TrimmerWindow::Impl {
    TrimmerWindow* q = nullptr;
    QComboBox* history = nullptr;
    TrimPicture* picture = nullptr;
    TrimStrip* strip = nullptr;
    QToolButton *play = nullptr, *loop = nullptr;
    QLabel *cursorLbl = nullptr, *inLbl = nullptr, *outLbl = nullptr, *lenLbl = nullptr, *streamLbl = nullptr;
    int streams = 0;
    int stripSize = 0;
    Alive alive = makeAlive();
    std::unique_ptr<Listener> listener;

    QString path;
    uint64_t mediaId = 0;
    MediaInfo info;
    double dur = 0, fps = 30;
    double pos = 0, in = 0, out = 0;
    bool loopOn = false;
    uint64_t generation = 0;
    std::vector<MediaItem::Marker> marks;
    bool marksSaved = true;
    bool autoSave = QSettings().value("trimmer/autoSaveMarkers", false).toBool();
    bool toPreview = QSettings().value("trimmer/videoInPreview", false).toBool();
    QLabel* marksLbl = nullptr;
    bool overwrite = QSettings().value("trimmer/overwrite", false).toBool();
    bool hoverScrub = QSettings().value("trimmer/hoverScrub", true).toBool();
    QToolButton* overwriteBtn = nullptr;
    void overwriteUnder(const std::vector<uint64_t>& made)
    {
        AppState& app = AppState::get();
        std::set<uint64_t> mine(made.begin(), made.end());
        std::vector<std::pair<uint64_t, std::pair<double, double>>> spans;
        for (uint64_t id : made) {
            const Track* t = nullptr;
            if (const Event* e = app.project().findEvent(id, &t))
                if (t) spans.push_back({t->id, {e->start, e->end()}});
        }
        if (spans.empty()) return;
        app.edit("Timeline Overwrite", [&](Project& p) {
            for (const auto& sp : spans) {
                Track* t = p.findTrack(sp.first);
                if (!t) continue;
                const double a = sp.second.first, b = sp.second.second;
                std::vector<Event> keep;
                for (const Event& e : t->events) {
                    if (mine.count(e.id) || e.end() <= a + 1e-9 || e.start >= b - 1e-9 || e.locked) {
                        keep.push_back(e);
                        continue;
                    }
                    if (e.start < a - 1e-9) {
                        Event left = e;
                        left.length = a - e.start;
                        left.fadeOut = std::min(left.fadeOut, left.length);
                        keep.push_back(left);
                    }
                    if (e.end() > b + 1e-9) {
                        Event right = e;
                        right.id = p.newId();
                        right.start = b;
                        right.length = e.end() - b;
                        right.offset = e.offset + (b - e.start) * e.rate;
                        right.fadeIn = 0;
                        right.transition = FxSlot();
                        for (auto& f : right.fx) f.id = p.newId();
                        right.envelopes.clear();
                        keep.push_back(right);
                    }
                }
                t->events = keep;
                t->sortEvents();
            }
        });
    }

    FrameWorker frames;
    std::unique_ptr<Wave> wave;
    QTimer* waveTimer = nullptr;

    bool playing = false;
    std::unique_ptr<AudioOutput> output;
    std::unique_ptr<MediaReader> audioReader;
    std::mutex audioMutex;
    int64_t audioNext = 0;
    std::atomic<int64_t> handed{0};
    int rate = 48000;
    double playFrom = 0, playTo = 0;
    std::chrono::steady_clock::time_point clock0;
    QTimer* timer = nullptr;
    bool fillingHistory = false;

    ~Impl()
    {
        alive->store(false);
        stopPlaying();
    }

    double frameLen() const { return 1.0 / (fps > 0 ? fps : 30.0); }
    bool selection() const { return out > in; }

    void updateLabels()
    {
        cursorLbl->setText(qs(timecode(pos, fps)));
        inLbl->setText(selection() ? qs(timecode(in, fps)) : QString("--"));
        outLbl->setText(selection() ? qs(timecode(out, fps)) : QString("--"));
        lenLbl->setText(qs(timecode(selection() ? out - in : dur, fps)));
        strip->cursor = pos;
        strip->in = in;
        strip->out = out;
        strip->update();
        const bool both = info.hasVideo && info.hasAudio;
        const int st = both ? streams : 0;
        streamLbl->setText(!both ? QString() : st == 1 ? QString("Video only") : st == 2 ? QString("Audio only") : QString("Video and audio"));
        picture->dimmed = st == 2;
        strip->dimWave = st == 1;
        picture->update();
        strip->marks = marks;
        strip->fileMarks = info.markers;
        int nm = 0, nr = 0;
        for (const MediaItem::Marker& m : marks) (m.length > 0 ? nr : nm)++;
        marksLbl->setText(marks.empty() ? QString() : QString("%1 marker%2, %3 region%4%5").arg(nm).arg(nm == 1 ? "" : "s").arg(nr).arg(nr == 1 ? "" : "s")
                                                                             .arg(marksSaved ? QString() : QString(" (not saved: S)")));
        play->setText(playing ? QString::fromUtf8("■") : QString::fromUtf8("▶"));
        play->setToolTip(playing ? "Stop (Space)" : "Play (Space)");
    }

    void requestFrame()
    {
        if (info.hasVideo) frames.request(info.isStill ? 0.0 : pos);
    }

    void startWaveform()
    {
        wave.reset();
        strip->peaks.clear();
        strip->hasAudio = info.hasAudio;
        if (!info.hasAudio || dur <= 0) return;
        wave.reset(new Wave());
        Wave* w = wave.get();
        const std::string p = ss(path);
        const double d = dur;
        w->thread = std::thread([w, p, d] {
            std::string err;
            std::unique_ptr<MediaReader> r = openReader(p, &err);
            if (!r || !r->info().hasAudio) {
                w->done = true;
                return;
            }
            const int64_t total = (int64_t)std::ceil(d * kWaveRate);
            const int buckets = (int)std::max<int64_t>(1, std::min<int64_t>(kWaveBuckets, total / 64));
            const double per = (double)total / buckets;
            {
                std::lock_guard<std::mutex> lk(w->m);
                w->peaks.reserve((size_t)buckets);
            }
            std::vector<float> l(4096), rr(4096);
            int64_t at = 0;
            int bucket = 0;
            float peak = 0;
            while (at < total && !w->cancel) {
                const int n = (int)std::min<int64_t>(4096, total - at);
                r->audio(at, n, kWaveRate, l.data(), rr.data());
                for (int k = 0; k < n; ++k) {
                    peak = std::max(peak, std::max(std::fabs(l[(size_t)k]), std::fabs(rr[(size_t)k])));
                    if (at + k + 1 >= (int64_t)std::llround((bucket + 1) * per)) {
                        std::lock_guard<std::mutex> lk(w->m);
                        w->peaks.push_back(peak);
                        peak = 0;
                        ++bucket;
                    }
                }
                at += n;
            }
            w->done = true;
        });
        waveTimer->start();
    }

    void pullWave()
    {
        if (!wave) {
            waveTimer->stop();
            return;
        }
        {
            std::lock_guard<std::mutex> lk(wave->m);
            strip->peaks = wave->peaks;
        }
        strip->update();
        if (wave->done) {
            std::lock_guard<std::mutex> lk(wave->m);
            strip->peaks = wave->peaks;
            waveTimer->stop();
        }
    }

    QStringList historyList() const { return QSettings().value("trimmer/history").toStringList(); }
    void setHistoryList(const QStringList& l)
    {
        QSettings().setValue("trimmer/history", l);
        fillHistory();
    }
    void remember(const QString& p)
    {
        QStringList l = historyList();
        l.removeAll(p);
        l.prepend(p);
        while (l.size() > kHistoryMax) l.removeLast();
        setHistoryList(l);
    }
    void fillHistory()
    {
        fillingHistory = true;
        history->clear();
        const bool folderToo = vprefs::on("prefs/trimmerHistoryFileFirst");
        for (const QString& p : historyList()) {
            const QFileInfo fi(p);
            history->addItem(folderToo ? fi.fileName() + " - " + QDir::toNativeSeparators(fi.absolutePath()) : fi.fileName(), p);
        }
        const int i = history->findData(path);
        history->setCurrentIndex(i);
        fillingHistory = false;
    }

    void startPlaying()
    {
        if (playing || dur <= 0) return;
        if (q->onWillPlay) q->onWillPlay();
        playFrom = pos;
        playTo = selection() ? out : dur;
        if (selection() && (pos < in || pos >= out - 1e-6)) playFrom = in;
        if (!selection() && pos >= dur - frameLen()) playFrom = 0;
        pos = playFrom;
        playing = true;
        handed = 0;
        clock0 = std::chrono::steady_clock::now();
        if (info.hasAudio && !info.isStill) {
            std::string err;
            audioReader = openReader(ss(path), &err);
            rate = AppState::get().project().settings.sampleRate > 0 ? AppState::get().project().settings.sampleRate : 48000;
            audioNext = (int64_t)std::llround(playFrom * rate);
            output = createAudioOutput();
            auto pull = [this](float* l, float* r, int n) {
                std::lock_guard<std::mutex> lk(audioMutex);
                if (audioReader) audioReader->audio(audioNext, n, rate, l, r);
                else {
                    std::fill(l, l + n, 0.f);
                    std::fill(r, r + n, 0.f);
                }
                audioNext += n;
                handed += n;
            };
            if (!audioReader || !output->start(rate, pull)) {
                output = createSilentOutput();
                output->start(rate, pull);
            }
        }
        timer->start();
        updateLabels();
    }

    void stopPlaying()
    {
        if (!playing) return;
        playing = false;
        if (timer) timer->stop();
        if (output) output->stop();
        output.reset();
        std::lock_guard<std::mutex> lk(audioMutex);
        audioReader.reset();
    }

    double playPosition() const
    {
        if (output) {
            const double heard = (double)handed.load() / rate - output->latencySeconds();
            return playFrom + std::max(0.0, heard);
        }
        return playFrom + std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count();
    }

    void tick()
    {
        if (!playing) return;
        double t = playPosition();
        if (t >= playTo) {
            if (loopOn) {
                stopPlaying();
                pos = selection() ? in : 0.0;
                startPlaying();
                return;
            }
            stopPlaying();
            t = playTo;
        }
        pos = std::min(dur, t);
        requestFrame();
        updateLabels();
    }

    uint64_t ensureMedia()
    {
        AppState& app = AppState::get();
        if (mediaId && app.project().findMedia(mediaId)) return mediaId;
        for (const MediaItem& m : app.project().media)
            if (!m.generated && QString::fromStdString(m.path) == path) return mediaId = m.id;
        std::string err;
        mediaId = app.importMedia(ss(path), &err);
        if (!mediaId) app.status("The Trimmer's file could not be added to the project: " + err);
        return mediaId;
    }

    void startDrag()
    {
        const uint64_t id = ensureMedia();
        if (!id) return;
        AppState& app = AppState::get();
        auto* md = new QMimeData();
        md->setData(kMimeMedia, QByteArray::number((qulonglong)id));
        QDrag* drag = new QDrag(q);
        drag->setMimeData(md);
        app.nextAddRange.mediaId = id;
        app.nextAddRange.in = selection() ? in : 0.0;
        app.nextAddRange.out = selection() ? out : dur;
        app.nextAddRange.streams = streams;
        drag->exec(Qt::CopyAction);
        app.nextAddRange = AppState::MediaRange();
    }
};

TrimmerWindow::TrimmerWindow(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->q = this;
    setObjectName("trimmer");
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, dark());
    setPalette(pal);
    Impl* impl = d.get();

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto button = [](QWidget* parentW, const char* name, const QString& text, const QString& tip) {
        auto* b = new QToolButton(parentW);
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRaise(true);
        return b;
    };

    QWidget* top = new QWidget(this);
    top->setAutoFillBackground(true);
    QPalette tp = top->palette();
    tp.setColor(QPalette::Window, tool());
    top->setPalette(tp);
    top->setFixedHeight(29);
    auto* th = new QHBoxLayout(top);
    th->setContentsMargins(4, 2, 4, 2);
    th->setSpacing(3);
    d->history = new QComboBox(top);
    d->history->setObjectName("trimmerHistory");
    d->history->setToolTip("Trimmer history: the files opened in the Trimmer");
    d->history->setFocusPolicy(Qt::NoFocus);
    d->history->setMinimumWidth(160);
    th->addWidget(d->history, 1);
    QToolButton* add = button(top, "trimmerAdd", "Add", "Add to Timeline from Cursor (A): the selection, or the whole file, at the timeline's cursor");
    QToolButton* addEnd = button(top, "trimmerAddEnd", "Add Up To", "Add to Timeline up to Cursor (Shift+A): so that it ends at the timeline's cursor");
    QToolButton* clear = button(top, "trimmerClearHistory", "Clear", "Clear Trimmer History (Ctrl+Shift+H)");
    QToolButton* fit = button(top, "trimmerFitToFill", "Fit to Fill", "Fit to Fill: the selection (or the whole file) stretched to fill the timeline's time selection");
    d->overwriteBtn = button(top, "trimmerOverwrite", "Overwrite", "Enable Timeline Overwrite: what is added replaces what is on the track there");
    d->overwriteBtn->setCheckable(true);
    d->overwriteBtn->setChecked(d->overwrite);
    QToolButton* sortBtn = button(top, "trimmerSortHistory", "Sort", "Sort Trimmer History (H; Ctrl+click: Z to A)");
    QToolButton* removeBtn = button(top, "trimmerRemoveCurrent", "Remove", "Remove Current Media from Trimmer History (Ctrl+H)");
    th->addWidget(add);
    th->addWidget(addEnd);
    th->addWidget(fit);
    th->addWidget(d->overwriteBtn);
    th->addWidget(sortBtn);
    th->addWidget(removeBtn);
    th->addWidget(clear);
    QObject::connect(fit, &QToolButton::clicked, this, [this] { fitToFill(); });
    QObject::connect(d->overwriteBtn, &QToolButton::toggled, this, [this](bool on) { setTimelineOverwrite(on); });
    QObject::connect(sortBtn, &QToolButton::clicked, this, [this] { sortHistory((QApplication::keyboardModifiers() & Qt::ControlModifier) != 0); });
    QObject::connect(removeBtn, &QToolButton::clicked, this, [this] { removeCurrentFromHistory(); });
    v->addWidget(top);

    d->picture = new TrimPicture(this);
    d->picture->message = "Double-click media in Project Media or the Explorer to look at it here.";
    d->picture->setMouseTracking(true);
    d->picture->onHover = [this, impl](double f) {
        if (!impl->hoverScrub || impl->playing || impl->dur <= 0 || impl->info.isStill) return;
        setPosition(f * impl->dur);
    };
    v->addWidget(d->picture, 1);
    d->strip = new TrimStrip(this);
    v->addWidget(d->strip);

    QWidget* tr = new QWidget(this);
    tr->setFixedHeight(28);
    auto* h = new QHBoxLayout(tr);
    h->setContentsMargins(4, 2, 4, 2);
    h->setSpacing(2);
    d->play = button(tr, "trimmerPlay", QString::fromUtf8("▶"), "Play (Space)");
    d->loop = button(tr, "trimmerLoop", QString::fromUtf8("↻"), "Loop Playback");
    d->loop->setCheckable(true);
    QToolButton* prev = button(tr, "trimmerPrevFrame", QString::fromUtf8("◀|"), "Previous Frame (Left)");
    QToolButton* next = button(tr, "trimmerNextFrame", QString::fromUtf8("|▶"), "Next Frame (Right)");
    QToolButton* markIn = button(tr, "trimmerIn", "[", "Set In Point (I)");
    QToolButton* markOut = button(tr, "trimmerOut", "]", "Set Out Point (O)");
    for (QToolButton* b : {d->play, d->loop, prev, next, markIn, markOut}) h->addWidget(b);
    h->addSpacing(8);
    auto label = [&](const char* name, const QString& tip) {
        QLabel* l = new QLabel(tr);
        l->setObjectName(name);
        l->setToolTip(tip);
        l->setMinimumWidth(70);
        return l;
    };
    d->cursorLbl = label("trimmerCursor", "The Trimmer's cursor");
    d->inLbl = label("trimmerInTime", "Start of the selection");
    d->outLbl = label("trimmerOutTime", "End of the selection");
    d->lenLbl = label("trimmerLength", "Length of the selection (or of the file)");
    d->streamLbl = label("trimmerStreams", "What the Add commands put on the timeline (Tab changes it)");
    d->streamLbl->setMinimumWidth(0);
    d->marksLbl = label("trimmerMarkers", "The Trimmer's markers (M) and regions (R); S saves them with the media");
    d->marksLbl->setMinimumWidth(0);
    h->addWidget(d->cursorLbl);
    h->addWidget(d->streamLbl);
    h->addSpacing(8);
    h->addWidget(d->marksLbl);
    h->addStretch(1);
    h->addWidget(new QLabel("In:", tr));
    h->addWidget(d->inLbl);
    h->addWidget(new QLabel("Out:", tr));
    h->addWidget(d->outLbl);
    h->addWidget(new QLabel("Length:", tr));
    h->addWidget(d->lenLbl);
    v->addWidget(tr);

    d->timer = new QTimer(this);
    d->timer->setInterval(15);
    QObject::connect(d->timer, &QTimer::timeout, this, [impl] { impl->tick(); });
    d->waveTimer = new QTimer(this);
    d->waveTimer->setInterval(150);
    QObject::connect(d->waveTimer, &QTimer::timeout, this, [impl] { impl->pullWave(); });

    Alive alive = d->alive;
    d->frames.onFrame = [impl, alive](const QImage& img, double, uint64_t gen) {
        AppState::post([impl, alive, img, gen] {
            if (!*alive || gen != impl->generation) return;
            impl->picture->image = img;
            impl->picture->update();
            if (impl->toPreview && impl->q->onPreviewPicture) impl->q->onPreviewPicture(img);
        });
    };
    d->strip->onMenu = [this, impl](const QPoint& at, int mark) {
        QMenu m(this);
        m.setObjectName("trimmerMenu");
        QMenu* mr = m.addMenu("Markers/Regions");
        QObject::connect(mr->addAction("Insert Marker\tM"), &QAction::triggered, this, [this] { insertMarker(); });
        QAction* region = mr->addAction("Insert Region\tR");
        region->setEnabled(impl->selection());
        QObject::connect(region, &QAction::triggered, this, [this] { insertRegion(); });
        if (mark >= 0) QObject::connect(mr->addAction("Delete Marker"), &QAction::triggered, this, [this, mark] { deleteMarker(mark); });
        mr->addSeparator();
        QAction* save = mr->addAction("Save Markers/Regions\tS");
        save->setEnabled(!impl->path.isEmpty());
        QObject::connect(save, &QAction::triggered, this, [this] { saveMarkers(); });
        QAction* autoSave = mr->addAction("Auto-Save Trimmer Markers/Regions\tShift+S");
        autoSave->setCheckable(true);
        autoSave->setChecked(impl->autoSave);
        QObject::connect(autoSave, &QAction::triggered, this, [this](bool on) { setAutoSaveMarkers(on); });
        m.addSeparator();
        QAction* pv = m.addAction("Show Video in Preview Window\tCtrl+P");
        pv->setCheckable(true);
        pv->setChecked(impl->toPreview);
        QObject::connect(pv, &QAction::triggered, this, [this](bool on) { setVideoInPreview(on); });
        QAction* hover = m.addAction("Enable Hover Scrub");
        hover->setCheckable(true);
        hover->setChecked(impl->hoverScrub);
        QObject::connect(hover, &QAction::triggered, this, [this](bool on) { setHoverScrub(on); });
        m.addSeparator();
        const bool both = impl->info.hasVideo && impl->info.hasAudio;
        QAction* ao = m.addAction("Select Audio Only");
        ao->setCheckable(true);
        ao->setChecked(impl->streams == 2);
        ao->setEnabled(both);
        QObject::connect(ao, &QAction::triggered, this, [this, impl](bool on) { setStreams(on ? AudioOnly : BothStreams); (void)impl; });
        QAction* vo = m.addAction("Select Video Only");
        vo->setCheckable(true);
        vo->setChecked(impl->streams == 1);
        vo->setEnabled(both);
        QObject::connect(vo, &QAction::triggered, this, [this](bool on) { setStreams(on ? VideoOnly : BothStreams); });
        m.addSeparator();
        QObject::connect(m.addAction("Video Height Larger\tCtrl+Shift+Up"), &QAction::triggered, this, [this] { changeStreamHeight(1); });
        QObject::connect(m.addAction("Audio Height Larger\tCtrl+Shift+Down"), &QAction::triggered, this, [this] { changeStreamHeight(-1); });
        QAction* eq = m.addAction("Video and Audio Equal Height");
        QObject::connect(eq, &QAction::triggered, this, [this, impl] {
            impl->stripSize = 0;
            changeStreamHeight(0);
        });
        m.addSeparator();
        QAction* parent = m.addAction("Select Parent Media");
        const MediaItem* cm = AppState::get().project().findMedia(impl->mediaId);
        parent->setEnabled(cm && cm->subclipLength > 0);
        QObject::connect(parent, &QAction::triggered, this, [this] { selectParentMedia(); });
        QObject::connect(m.addAction("Open in Audio Editor\tCtrl+E"), &QAction::triggered, this, [this] { openInAudioEditor(); });
        m.addSeparator();
        QAction* copy = m.addAction("Copy Snapshot to Clipboard");
        copy->setEnabled(!impl->picture->image.isNull());
        QObject::connect(copy, &QAction::triggered, this, [this] { copySnapshot(); });
        QAction* saveShot = m.addAction("Save Snapshot to File...");
        saveShot->setEnabled(!impl->picture->image.isNull());
        QObject::connect(saveShot, &QAction::triggered, this, [this, impl] {
            const QString base = QFileInfo(impl->path).completeBaseName() + QString("_%1.png").arg(QString::fromStdString(timecode(impl->pos, impl->fps)).remove(':').remove(';'));
            const QString f = QFileDialog::getSaveFileName(this, "Save Snapshot to File", QFileInfo(impl->path).dir().filePath(base), "PNG (*.png);;JPEG (*.jpg)");
            if (!f.isEmpty()) saveSnapshot(f);
        });
        m.exec(at);
    };
    d->strip->onCursor = [this](double t) { setPosition(t); setFocus(); };
    d->strip->onSelect = [this](double a, double b) { setSelection(a, b); setFocus(); };
    d->strip->onDragOut = [impl] { impl->startDrag(); };

    QObject::connect(d->play, &QToolButton::clicked, this, [this] { playing() ? stop() : play(); });
    QObject::connect(d->loop, &QToolButton::toggled, this, [this](bool on) { setLoop(on); });
    QObject::connect(prev, &QToolButton::clicked, this, [this] { stepFrames(-1); });
    QObject::connect(next, &QToolButton::clicked, this, [this] { stepFrames(1); });
    QObject::connect(markIn, &QToolButton::clicked, this, [this] { this->markIn(); });
    QObject::connect(markOut, &QToolButton::clicked, this, [this] { this->markOut(); });
    QObject::connect(add, &QToolButton::clicked, this, [this] { addToTimeline(false); });
    QObject::connect(addEnd, &QToolButton::clicked, this, [this] { addToTimeline(true); });
    QObject::connect(clear, &QToolButton::clicked, this, [this] { clearHistory(); });
    QObject::connect(d->history, QOverload<int>::of(&QComboBox::activated), this, [this, impl](int i) {
        if (impl->fillingHistory || i < 0) return;
        const QString p = impl->history->itemData(i).toString();
        if (p != impl->path) openFile(p);
    });
    d->listener.reset(new Listener([impl](int changes) {
        if (!(changes & kProject) || !impl->mediaId) return;
        const MediaItem* m = AppState::get().project().findMedia(impl->mediaId);
        if (!m) {
            impl->mediaId = 0;
            return;
        }
        bool same = m->markers.size() == impl->marks.size();
        for (size_t k = 0; same && k < impl->marks.size(); ++k)
            same = m->markers[k].time == impl->marks[k].time && m->markers[k].length == impl->marks[k].length && m->markers[k].name == impl->marks[k].name;
        if (!same && impl->marksSaved) {
            impl->marks = m->markers;
            impl->updateLabels();
        }
    }));
    d->fillHistory();
    d->updateLabels();
}

TrimmerWindow::~TrimmerWindow() {}

bool TrimmerWindow::openMedia(uint64_t mediaId)
{
    const MediaItem* m = AppState::get().project().findMedia(mediaId);
    if (!m) return false;
    if (m->generated || m->path.empty()) {
        d->picture->message = "Generated media has no file to show in the Trimmer.";
        d->picture->update();
        return false;
    }
    const std::vector<MediaItem::Marker> marks = m->markers;
    const bool ok = openFile(qs(m->path));
    if (ok) {
        d->mediaId = mediaId;
        d->marks = marks;
        d->marksSaved = true;
        d->updateLabels();
    }
    return ok;
}

bool TrimmerWindow::openFile(const QString& path)
{
    d->stopPlaying();
    std::string err;
    std::unique_ptr<MediaReader> r = openReader(ss(path), &err);
    ++d->generation;
    d->picture->image = QImage();
    if (!r || (!r->info().hasVideo && !r->info().hasAudio)) {
        d->picture->message = QString("%1 cannot be read%2").arg(QFileInfo(path).fileName(), err.empty() ? QString(".") : ": " + qs(err));
        d->picture->update();
        d->frames.setReader(nullptr, d->generation);
        d->path.clear();
        d->mediaId = 0;
        d->info = MediaInfo();
        d->dur = 0;
        d->strip->duration = 0;
        d->wave.reset();
        d->strip->peaks.clear();
        d->updateLabels();
        return false;
    }
    d->path = path;
    d->mediaId = 0;
    d->marks.clear();
    d->marksSaved = true;
    for (const MediaItem& m : AppState::get().project().media)
        if (!m.generated && qs(m.path) == path) {
            d->mediaId = m.id;
            d->marks = m.markers;
        }
    d->info = r->info();
    d->dur = d->info.isStill || d->info.duration <= 0 ? AppState::get().prefs.stillLength : d->info.duration;
    d->fps = d->info.fps > 0 ? d->info.fps : AppState::get().project().settings.fps();
    d->pos = 0;
    d->in = d->out = 0;
    d->strip->duration = d->dur;
    d->strip->fps = d->fps;
    d->picture->message = d->info.hasVideo ? QString() : QString("%1\n(sound only)").arg(QFileInfo(path).fileName());
    d->picture->update();
    d->frames.setReader(d->info.hasVideo ? std::move(r) : nullptr, d->generation);
    d->requestFrame();
    d->startWaveform();
    d->remember(path);
    d->updateLabels();
    setWindowTitle(QString("Trimmer - %1").arg(QFileInfo(path).fileName()));
    return true;
}

QString TrimmerWindow::currentPath() const { return d->path; }
uint64_t TrimmerWindow::currentMedia() const { return d->mediaId; }
double TrimmerWindow::duration() const { return d->dur; }
double TrimmerWindow::position() const { return d->pos; }

void TrimmerWindow::setPosition(double seconds)
{
    if (d->playing) d->stopPlaying();
    d->pos = std::min(d->dur, std::max(0.0, seconds));
    d->requestFrame();
    d->updateLabels();
}

void TrimmerWindow::setSelection(double in, double out)
{
    in = std::min(d->dur, std::max(0.0, in));
    out = std::min(d->dur, std::max(0.0, out));
    if (out < in) std::swap(in, out);
    if (out - in < 1e-6) in = out = 0;
    d->in = in;
    d->out = out;
    d->updateLabels();
}

double TrimmerWindow::selectionIn() const { return d->in; }
double TrimmerWindow::selectionOut() const { return d->out; }

void TrimmerWindow::markIn()
{
    const double out = d->out > d->pos ? d->out : d->dur;
    setSelection(d->pos, out);
}

void TrimmerWindow::markOut()
{
    const double in = d->out > d->in && d->in < d->pos ? d->in : 0.0;
    setSelection(in, d->pos);
}

void TrimmerWindow::stepFrames(int n)
{
    const double f = d->frameLen();
    const double frame = std::floor(d->pos / f + 0.5);
    setPosition((frame + n) * f);
}

std::vector<uint64_t> TrimmerWindow::addToTimeline(bool endAtCursor)
{
    if (d->path.isEmpty() || d->dur <= 0) return {};
    const uint64_t id = d->ensureMedia();
    if (!id) return {};
    AppState& app = AppState::get();
    double in = d->selection() ? d->in : 0.0, out = d->selection() ? d->out : d->dur;
    double at = endAtCursor ? std::max(0.0, app.cursor() - (out - in)) : app.cursor();
    // VEGAS's three-point editing: no selection in the Trimmer and a time selection on the timeline gives as much of the
    // file from the Trimmer's cursor as the time selection is long
    if (!d->selection() && app.loopEnd > app.loopStart && !d->info.isStill) {
        in = std::min(d->pos, std::max(0.0, d->dur - d->frameLen()));
        out = std::min(d->dur, in + (app.loopEnd - app.loopStart));
        at = endAtCursor ? std::max(0.0, app.loopEnd - (out - in)) : app.loopStart;
    }
    app.nextAddRange.mediaId = id;
    app.nextAddRange.in = in;
    app.nextAddRange.out = out;
    app.nextAddRange.streams = d->streams;
    const size_t steps = app.undoCount();
    std::vector<uint64_t> made = app.addMediaToTimeline(id, at, app.currentTrack);
    app.nextAddRange = AppState::MediaRange();
    if (d->overwrite && !made.empty()) {
        d->overwriteUnder(made);
        if (app.undoCount() > steps + 1) app.mergeUndo(app.undoCount() - steps, "Add to Timeline");
    }
    if (!endAtCursor && !made.empty())
        if (const Event* e = app.project().findEvent(made.front())) app.setCursor(e->end());
    return made;
}

bool TrimmerWindow::selectTimelineLength(bool beforeCursor)
{
    AppState& app = AppState::get();
    const double len = app.loopEnd - app.loopStart;
    if (len <= 1e-9 || d->dur <= 0) {
        app.status("Make a time selection on the timeline first: T selects as much of the Trimmer's file");
        return false;
    }
    double a = beforeCursor ? d->pos - len : d->pos;
    a = std::max(0.0, std::min(a, d->dur - len));
    setSelection(a, std::min(d->dur, a + len));
    return true;
}

TrimmerWindow::Streams TrimmerWindow::streams() const { return (Streams)d->streams; }

void TrimmerWindow::setStreams(Streams s)
{
    d->streams = std::min(2, std::max(0, (int)s));
    d->updateLabels();
}

void TrimmerWindow::cycleStreams()
{
    if (!(d->info.hasVideo && d->info.hasAudio)) return;
    setStreams((Streams)((d->streams + 1) % 3));
}

void TrimmerWindow::changeStreamHeight(int dir)
{
    d->stripSize = dir == 0 ? 0 : std::min(1, std::max(-1, d->stripSize - (dir > 0 ? 1 : -1)));
    const int h = d->stripSize < 0 ? 48 : d->stripSize == 0 ? 84 : std::max(84, height() / 2);
    d->strip->setMinimumHeight(h);
    d->strip->setMaximumHeight(d->stripSize > 0 ? QWIDGETSIZE_MAX : h);
    d->strip->updateGeometry();
    d->strip->update();
    setProperty("stripSize", d->stripSize);
}

bool TrimmerWindow::openInAudioEditor()
{
    AppState& app = AppState::get();
    if (d->path.isEmpty() || !QFileInfo::exists(d->path)) {
        app.status("The Trimmer has no file that can be opened in an audio editor.");
        return false;
    }
    if (app.prefs.audioEditor.empty()) {
        app.status("Choose the audio editor in Options > Preferences > Editing first.");
        return false;
    }
    const QString editor = QString::fromStdString(app.prefs.audioEditor);
    if (!QProcess::startDetached(editor, QStringList() << QDir::toNativeSeparators(d->path))) {
        app.status("Could not start the audio editor " + app.prefs.audioEditor + " (Options > Preferences > Editing sets it).");
        return false;
    }
    app.status("Opened " + ss(QFileInfo(d->path).fileName()) + " in " + ss(QFileInfo(editor).fileName()) + ".");
    return true;
}

std::vector<MediaItem::Marker> TrimmerWindow::markers() const { return d->marks; }

void TrimmerWindow::insertMarker()
{
    if (d->path.isEmpty() || d->dur <= 0) return;
    MediaItem::Marker m;
    m.time = d->pos;
    d->marks.push_back(m);
    std::stable_sort(d->marks.begin(), d->marks.end(), [](const MediaItem::Marker& a, const MediaItem::Marker& b) { return a.time < b.time; });
    d->marksSaved = false;
    if (d->autoSave) saveMarkers();
    d->updateLabels();
}

bool TrimmerWindow::insertRegion()
{
    if (d->path.isEmpty() || !d->selection()) {
        AppState::get().status("Select part of the Trimmer's file first: R makes a region of the selection");
        return false;
    }
    MediaItem::Marker m;
    m.time = d->in;
    m.length = d->out - d->in;
    d->marks.push_back(m);
    std::stable_sort(d->marks.begin(), d->marks.end(), [](const MediaItem::Marker& a, const MediaItem::Marker& b) { return a.time < b.time; });
    d->marksSaved = false;
    if (d->autoSave) saveMarkers();
    d->updateLabels();
    return true;
}

void TrimmerWindow::deleteMarker(int index)
{
    if (index < 0 || index >= (int)d->marks.size()) return;
    d->marks.erase(d->marks.begin() + index);
    d->marksSaved = false;
    if (d->autoSave) saveMarkers();
    d->updateLabels();
}

bool TrimmerWindow::saveMarkers()
{
    if (d->path.isEmpty()) return false;
    const uint64_t id = d->ensureMedia();
    if (!id) return false;
    const std::vector<MediaItem::Marker> marks = d->marks;
    AppState& app = AppState::get();
    const MediaItem* now = app.project().findMedia(id);
    bool same = now && now->markers.size() == marks.size();
    for (size_t k = 0; same && k < marks.size(); ++k)
        same = now->markers[k].time == marks[k].time && now->markers[k].length == marks[k].length && now->markers[k].name == marks[k].name;
    if (!same) app.edit("Save Trimmer Markers/Regions", [id, marks](Project& p) { if (MediaItem* m = p.findMedia(id)) m->markers = marks; });
    d->marksSaved = true;
    d->updateLabels();
    return true;
}

bool TrimmerWindow::markersSaved() const { return d->marksSaved; }

void TrimmerWindow::setAutoSaveMarkers(bool on)
{
    d->autoSave = on;
    QSettings().setValue("trimmer/autoSaveMarkers", on);
    AppState::get().status(on ? "Auto-Save Trimmer Markers/Regions: on (each marker or region is saved with the media as it is made)"
                              : "Auto-Save Trimmer Markers/Regions: off (S saves them)");
    if (on && !d->marksSaved) saveMarkers();
}

bool TrimmerWindow::autoSaveMarkers() const { return d->autoSave; }

void TrimmerWindow::setVideoInPreview(bool on)
{
    d->toPreview = on;
    QSettings().setValue("trimmer/videoInPreview", on);
    if (onPreviewPicture) onPreviewPicture(on ? d->picture->image : QImage());
}

bool TrimmerWindow::videoInPreview() const { return d->toPreview; }

void TrimmerWindow::play() { d->startPlaying(); }

void TrimmerWindow::stop()
{
    d->stopPlaying();
    d->updateLabels();
}

bool TrimmerWindow::playing() const { return d->playing; }

void TrimmerWindow::setLoop(bool on)
{
    d->loopOn = on;
    if (d->loop->isChecked() != on) d->loop->setChecked(on);
}

QStringList TrimmerWindow::history() const { return d->historyList(); }

void TrimmerWindow::removeCurrentFromHistory()
{
    QStringList l = d->historyList();
    l.removeAll(d->path);
    d->setHistoryList(l);
}

void TrimmerWindow::clearHistory() { d->setHistoryList(QStringList()); }

void TrimmerWindow::sortHistory() { sortHistory(false); }

void TrimmerWindow::sortHistory(bool reverse)
{
    QStringList l = d->historyList();
    std::sort(l.begin(), l.end(), [reverse](const QString& a, const QString& b) {
        const int c = QFileInfo(a).fileName().compare(QFileInfo(b).fileName(), Qt::CaseInsensitive);
        return reverse ? c > 0 : c < 0;
    });
    d->setHistoryList(l);
}

std::vector<uint64_t> TrimmerWindow::fitToFill()
{
    AppState& app = AppState::get();
    if (d->path.isEmpty() || d->dur <= 0) return {};
    const double len = app.loopEnd - app.loopStart;
    if (len <= 1e-9) {
        app.status("Fit to Fill: make a time selection on the timeline for the Trimmer's selection to fill.");
        return {};
    }
    const uint64_t id = d->ensureMedia();
    if (!id) return {};
    const double in = d->selection() ? d->in : 0.0, out = d->selection() ? d->out : d->dur;
    const size_t steps = app.undoCount();
    app.nextAddRange.mediaId = id;
    app.nextAddRange.in = in;
    app.nextAddRange.out = out;
    app.nextAddRange.streams = d->streams;
    std::vector<uint64_t> made = app.addMediaToTimeline(id, app.loopStart, app.currentTrack);
    app.nextAddRange = AppState::MediaRange();
    if (made.empty()) return made;
    const double rate = std::min(100.0, std::max(0.01, (out - in) / len));
    app.edit("Fit to Fill", [&](Project& p) {
        for (uint64_t e : made)
            if (Event* ev = p.findEvent(e)) {
                ev->length = len;
                ev->rate = rate;
                ev->fadeIn = std::min(ev->fadeIn, len);
                ev->fadeOut = std::min(ev->fadeOut, len);
            }
    });
    if (d->overwrite) d->overwriteUnder(made);
    if (app.undoCount() > steps + 1) app.mergeUndo(app.undoCount() - steps, "Fit to Fill");
    app.setCursor(app.loopEnd);
    return made;
}

void TrimmerWindow::setTimelineOverwrite(bool on)
{
    d->overwrite = on;
    QSettings().setValue("trimmer/overwrite", on);
    if (d->overwriteBtn && d->overwriteBtn->isChecked() != on) d->overwriteBtn->setChecked(on);
}

bool TrimmerWindow::timelineOverwrite() const { return d->overwrite; }

bool TrimmerWindow::openParentMedia(uint64_t mediaId)
{
    const MediaItem* m = AppState::get().project().findMedia(mediaId);
    if (!m || m->path.empty()) return false;
    const double a = m->subclipStart, len = m->subclipLength;
    uint64_t parent = 0;
    for (const MediaItem& o : AppState::get().project().media)
        if (o.path == m->path && o.subclipLength <= 0 && !o.generated) parent = o.id;
    const bool ok = parent ? openMedia(parent) : openFile(qs(m->path));
    if (ok && len > 0) {
        setSelection(a, std::min(d->dur, a + len));
        setPosition(a);
    }
    return ok;
}

bool TrimmerWindow::selectParentMedia()
{
    const MediaItem* m = AppState::get().project().findMedia(d->mediaId);
    if (!m || m->subclipLength <= 0) return false;
    return openParentMedia(m->id);
}

bool TrimmerWindow::copySnapshot()
{
    if (d->picture->image.isNull()) return false;
    QApplication::clipboard()->setImage(d->picture->image);
    AppState::get().status("The Trimmer's frame was copied to the clipboard.");
    return true;
}

bool TrimmerWindow::saveSnapshot(const QString& path)
{
    AppState& app = AppState::get();
    if (d->picture->image.isNull() || path.isEmpty()) return false;
    if (!d->picture->image.save(path)) {
        app.status("The snapshot could not be saved to " + ss(path) + ".");
        return false;
    }
    std::string err;
    if (!app.importMedia(ss(path), &err)) app.status("The snapshot was saved but could not be added to Project Media" + (err.empty() ? std::string() : ": " + err));
    return true;
}

void TrimmerWindow::setHoverScrub(bool on)
{
    d->hoverScrub = on;
    QSettings().setValue("trimmer/hoverScrub", on);
}

bool TrimmerWindow::hoverScrub() const { return d->hoverScrub; }

QImage TrimmerWindow::picture() const { return d->picture->image; }
bool TrimmerWindow::waveformReady() const { return d->wave && d->wave->done && !d->waveTimer->isActive(); }
std::vector<float> TrimmerWindow::waveform() const { return d->strip->peaks; }
QWidget* TrimmerWindow::strip() const { return d->strip; }

static bool trimmerKey(const QKeyEvent* k)
{
    const Qt::KeyboardModifiers m = k->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
    switch (k->key()) {
    case Qt::Key_Space: case Qt::Key_I: case Qt::Key_O: case Qt::Key_Left: case Qt::Key_Right:
    case Qt::Key_Home: case Qt::Key_End: case Qt::Key_Return: case Qt::Key_Enter:
        return m == Qt::NoModifier;
    case Qt::Key_A: case Qt::Key_T: return m == Qt::NoModifier || m == Qt::ShiftModifier;
    case Qt::Key_H: return m == Qt::NoModifier || m == Qt::ControlModifier || m == (Qt::ControlModifier | Qt::ShiftModifier);
    case Qt::Key_Tab: return m == Qt::NoModifier;
    case Qt::Key_Up: case Qt::Key_Down: return m == (Qt::ControlModifier | Qt::ShiftModifier);
    case Qt::Key_E: case Qt::Key_P: return m == Qt::ControlModifier;
    case Qt::Key_M: case Qt::Key_R: return m == Qt::NoModifier;
    case Qt::Key_S: return m == Qt::NoModifier || m == Qt::ShiftModifier;
    default: return false;
    }
}

bool TrimmerWindow::event(QEvent* e)
{
    if (e->type() == QEvent::ShortcutOverride && trimmerKey(static_cast<QKeyEvent*>(e))) {
        e->accept();
        return true;
    }
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Tab && trimmerKey(static_cast<QKeyEvent*>(e))) {
        keyPressEvent(static_cast<QKeyEvent*>(e));
        return true;
    }
    return QWidget::event(e);
}

void TrimmerWindow::keyPressEvent(QKeyEvent* e)
{
    if (!trimmerKey(e)) return QWidget::keyPressEvent(e);
    const bool shift = e->modifiers() & Qt::ShiftModifier, ctrl = e->modifiers() & Qt::ControlModifier;
    switch (e->key()) {
    case Qt::Key_Space: playing() ? stop() : play(); break;
    case Qt::Key_I: markIn(); break;
    case Qt::Key_O: markOut(); break;
    case Qt::Key_Left: stepFrames(-1); break;
    case Qt::Key_Right: stepFrames(1); break;
    case Qt::Key_Home: setPosition(0); break;
    case Qt::Key_End: setPosition(d->dur); break;
    case Qt::Key_Return: case Qt::Key_Enter: addToTimeline(false); break;
    case Qt::Key_A: addToTimeline(shift); break;
    case Qt::Key_T: selectTimelineLength(shift); break;
    case Qt::Key_Tab: cycleStreams(); break;
    case Qt::Key_Up: changeStreamHeight(1); break;
    case Qt::Key_Down: changeStreamHeight(-1); break;
    case Qt::Key_E: openInAudioEditor(); break;
    case Qt::Key_P: setVideoInPreview(!videoInPreview()); break;
    case Qt::Key_M: insertMarker(); break;
    case Qt::Key_R: insertRegion(); break;
    case Qt::Key_S:
        if (shift) setAutoSaveMarkers(!autoSaveMarkers());
        else saveMarkers();
        break;
    case Qt::Key_H:
        if (ctrl && shift) clearHistory();
        else if (ctrl) removeCurrentFromHistory();
        else sortHistory();
        break;
    default: break;
    }
    e->accept();
}

}  // namespace app
}  // namespace reno
