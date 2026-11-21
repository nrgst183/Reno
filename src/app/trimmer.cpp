// VEGAS's Trimmer: see trimmer.h.
//
// The file is read by readers of the Trimmer's own (never the timeline's): images on a thread that decodes the newest
// request,
// the waveform on another (a few seconds at a time, shown as it arrives), and the sound, while playing, on the sound
// device's.
#include "trimmer.h"
#include "startprogram.h"
#include "toolwin.h"
#include "panels.h"
#include "playback.h"
#include "vprefs.h"
#include "panels_glyph.h"
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QImage>
#include <set>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
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

// The pictures on the Trimmer's buttons. The transport's own (play, pause, ...) are the Video Preview's (drawGlyph);
// the others are drawn here, in VEGAS 18's colors: blue for what puts media on the timeline, yellow for markers,
// green for regions, red for clearing.
enum class TrimIcon {
    Loop, PlayFromStart, Play, Pause, Stop, PrevFrame, NextFrame, Menu,
    SortHistory, RemoveCurrent, ClearHistory, ExternalMonitor,
    MarkIn, MarkOut, AddFromCursor, AddUpToCursor, FitToFill, Overwrite, CreateSubclip, InsertMarker, InsertRegion, SaveMarkers
};

const QColor kAddBlue(122, 183, 208), kMarkYellow(190, 173, 99), kRegionGreen(124, 196, 132), kClearRed(226, 120, 130);
const QColor kOutline(28, 28, 28), kOff(104, 104, 104);

bool standardIcon(TrimIcon i, Glyph* g)
{
    switch (i) {
    case TrimIcon::Loop: *g = Glyph::Loop; return true;
    case TrimIcon::PlayFromStart: *g = Glyph::PlayFromStart; return true;
    case TrimIcon::Play: *g = Glyph::Play; return true;
    case TrimIcon::Pause: *g = Glyph::Pause; return true;
    case TrimIcon::Stop: *g = Glyph::Stop; return true;
    case TrimIcon::PrevFrame: *g = Glyph::PrevFrame; return true;
    case TrimIcon::NextFrame: *g = Glyph::NextFrame; return true;
    case TrimIcon::Menu: *g = Glyph::Menu; return true;
    default: return false;
    }
}

// VEGAS's media frame (the Add and Fit buttons): a box whose sides are pinched in, like a strip of film.
QPainterPath mediaFrame(double cx, double cy)
{
    QPainterPath f;
    f.moveTo(cx - 7.5, cy - 5.5);
    f.lineTo(cx + 7.5, cy - 5.5);
    f.lineTo(cx + 6.0, cy - 3.5);
    f.lineTo(cx + 6.0, cy + 3.5);
    f.lineTo(cx + 7.5, cy + 5.5);
    f.lineTo(cx - 7.5, cy + 5.5);
    f.lineTo(cx - 6.0, cy + 3.5);
    f.lineTo(cx - 6.0, cy - 3.5);
    f.closeSubpath();
    return f;
}

// A marker flag (a square with a point hanging from its left side), as on the Trimmer's marker bar.
QPainterPath markerFlag(double l, double t, double s)
{
    QPainterPath f;
    f.moveTo(l, t);
    f.lineTo(l + s, t);
    f.lineTo(l + s, t + s * 0.75);
    f.lineTo(l + s * 0.3, t + s * 0.75);
    f.lineTo(l, t + s);
    f.closeSubpath();
    return f;
}

void drawTrimIcon(QPainter& p, TrimIcon icon, const QRectF& r, bool enabled)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const double cx = r.center().x(), cy = r.center().y();
    const QColor light = enabled ? QColor(238, 238, 238) : kOff;
    auto fillOutlined = [&](const QPainterPath& path, const QColor& c) {
        p.setPen(QPen(kOutline, 1.0));
        p.setBrush(enabled ? c : kOff);
        p.drawPath(path);
    };
    auto arrow = [&](double x0, double x1, double y, const QColor& c) {   // a line ending in a head (x1 the tip)
        const double dir = x1 > x0 ? 1 : -1;
        p.setPen(QPen(c, 1.6));
        p.drawLine(QPointF(x0, y), QPointF(x1 - dir * 2.5, y));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPainterPath h;
        h.moveTo(x1, y);
        h.lineTo(x1 - dir * 3.5, y - 3.0);
        h.lineTo(x1 - dir * 3.5, y + 3.0);
        h.closeSubpath();
        p.drawPath(h);
    };
    const QColor ink(25, 25, 25);
    switch (icon) {
    case TrimIcon::SortHistory: {
        QFont f = p.font();
        f.setPixelSize(8);
        f.setBold(true);
        p.setFont(f);
        p.setPen(light);
        p.drawText(QRectF(cx - 9, cy - 8, 18, 9), Qt::AlignCenter, "ABC");
        p.setPen(Qt::NoPen);
        p.setBrush(light);
        QPainterPath h;
        h.moveTo(cx - 3.5, cy + 2.0);
        h.lineTo(cx + 3.5, cy + 2.0);
        h.lineTo(cx, cy + 6.5);
        h.closeSubpath();
        p.drawPath(h);
        break;
    }
    case TrimIcon::RemoveCurrent: {
        p.setPen(QPen(kOutline, 1.0));
        p.setBrush(light);
        p.drawRect(QRectF(cx - 5.5, cy - 7.0, 12.0, 13.0));
        p.setPen(QPen(ink, 1.2));
        for (int k = 0; k < 3; ++k) p.drawLine(QPointF(cx + 0.5, cy - 4.0 + k * 3.0), QPointF(cx + 4.5, cy - 4.0 + k * 3.0));
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawRect(QRectF(cx - 3.5, cy - 5.0, 3.0, 3.5));
        p.setPen(QPen(enabled ? kClearRed : kOff, 2.0));
        p.drawLine(QPointF(cx - 7.5, cy + 2.0), QPointF(cx - 2.5, cy + 7.0));
        p.drawLine(QPointF(cx - 7.5, cy + 7.0), QPointF(cx - 2.5, cy + 2.0));
        break;
    }
    case TrimIcon::ClearHistory: {
        QPen pen(enabled ? kClearRed : kOff, 2.4);
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        p.drawLine(QPointF(cx - 6, cy - 6), QPointF(cx + 6, cy + 6));
        p.drawLine(QPointF(cx - 6, cy + 6), QPointF(cx + 6, cy - 6));
        break;
    }
    case TrimIcon::ExternalMonitor: {
        const QColor screen = enabled ? kAddBlue : QColor(70, 70, 70);
        for (int k = 0; k < 2; ++k) {
            const double ox = k == 0 ? 1.5 : -3.5, oy = k == 0 ? -6.5 : -2.0;
            p.setPen(QPen(kOutline, 1.0));
            p.setBrush(light);
            p.drawRect(QRectF(cx + ox - 1.0, cy + oy, 9.5, 7.5));
            p.setPen(Qt::NoPen);
            p.setBrush(screen);
            p.drawRect(QRectF(cx + ox + 0.5, cy + oy + 1.5, 6.5, 4.5));
            p.setBrush(light);
            p.drawRect(QRectF(cx + ox - 2.0, cy + oy + 7.5, 11.5, 1.6));
        }
        break;
    }
    case TrimIcon::MarkIn:
    case TrimIcon::MarkOut: {
        QPainterPath t;
        const bool in = icon == TrimIcon::MarkIn;
        t.moveTo(cx - 5.5, cy - 5.5);
        t.lineTo(cx + 5.5, cy - 5.5);
        t.lineTo(in ? cx - 5.5 : cx + 5.5, cy + 5.5);
        t.closeSubpath();
        fillOutlined(t, kMarkYellow);
        break;
    }
    case TrimIcon::AddFromCursor:
    case TrimIcon::AddUpToCursor:
    case TrimIcon::FitToFill:
    case TrimIcon::Overwrite:
    case TrimIcon::CreateSubclip: {
        fillOutlined(mediaFrame(cx, cy), kAddBlue);
        const QColor mark = enabled ? ink : QColor(60, 60, 60);
        if (icon == TrimIcon::AddFromCursor) {
            p.setPen(QPen(mark, 1.4));
            p.drawLine(QPointF(cx - 4.0, cy - 3.0), QPointF(cx - 4.0, cy + 3.0));
            arrow(cx - 3.5, cx + 4.5, cy, mark);
        } else if (icon == TrimIcon::AddUpToCursor) {
            p.setPen(QPen(mark, 1.4));
            p.drawLine(QPointF(cx + 4.0, cy - 3.0), QPointF(cx + 4.0, cy + 3.0));
            arrow(cx - 4.5, cx + 3.3, cy, mark);
        } else if (icon == TrimIcon::FitToFill) {
            arrow(cx, cx - 4.5, cy, mark);
            arrow(cx, cx + 4.5, cy, mark);
        } else if (icon == TrimIcon::Overwrite) {
            p.setPen(Qt::NoPen);
            p.setBrush(mark);
            QPainterPath h;
            h.moveTo(cx - 3.5, cy - 3.0);
            h.lineTo(cx + 3.5, cy - 3.0);
            h.lineTo(cx, cy + 1.0);
            h.closeSubpath();
            p.drawPath(h);
            p.drawRect(QRectF(cx - 4.0, cy + 2.0, 8.0, 1.6));
        } else {
            p.setPen(QPen(mark, 1.6));
            p.drawLine(QPointF(cx - 3.5, cy), QPointF(cx + 3.5, cy));
            p.drawLine(QPointF(cx, cy - 3.5), QPointF(cx, cy + 3.5));
        }
        break;
    }
    case TrimIcon::InsertMarker: fillOutlined(markerFlag(cx - 6.0, cy - 6.5, 12.0), kMarkYellow); break;
    case TrimIcon::InsertRegion: {
        QPainterPath g;
        g.moveTo(cx - 7.0, cy - 6.5);
        g.lineTo(cx + 7.0, cy - 6.5);
        g.lineTo(cx + 7.0, cy + 6.5);
        g.lineTo(cx + 4.5, cy + 3.5);
        g.lineTo(cx + 4.5, cy - 2.5);
        g.lineTo(cx - 4.5, cy - 2.5);
        g.lineTo(cx - 4.5, cy + 3.5);
        g.lineTo(cx - 7.0, cy + 6.5);
        g.closeSubpath();
        fillOutlined(g, kRegionGreen);
        break;
    }
    case TrimIcon::SaveMarkers: {
        QPainterPath disk;
        disk.addRect(QRectF(cx - 7.0, cy - 4.0, 11.0, 11.0));
        disk.addRect(QRectF(cx - 4.5, cy - 4.0, 6.0, 3.5));
        disk.addRect(QRectF(cx - 4.5, cy + 2.5, 6.0, 4.5));
        disk.setFillRule(Qt::OddEvenFill);
        fillOutlined(disk, QColor(150, 202, 240));
        fillOutlined(markerFlag(cx + 0.5, cy - 7.5, 7.0), kMarkYellow);
        break;
    }
    default: break;
    }
    p.restore();
}

// A button of the Trimmer's bars: flat, as the Video Preview's (GlyphButton), with one of the pictures above.
class TrimButton : public QAbstractButton {
public:
    TrimButton(TrimIcon icon, const char* name, const QString& tip, QWidget* parent) : QAbstractButton(parent), icon_(icon)
    {
        setObjectName(name);
        setFixedSize(24, 22);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(tip);
        setAccessibleName(tip);
    }
    TrimIcon icon() const { return icon_; }
    void setPlaying(bool on)
    {
        if (playing_ == on) return;
        playing_ = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        QColor bg(0, 0, 0, 0), edge(0, 0, 0, 0);
        if (isChecked()) bg = edge = QColor(18, 176, 255);
        else if (isDown()) bg = edge = QColor(46, 143, 214);
        else if (underMouse() && isEnabled()) {
            bg = QColor(90, 90, 90);
            edge = QColor(110, 110, 110);
        }
        p.setPen(QPen(edge, 1.0));
        p.setBrush(bg);
        p.drawRect(r);
        Glyph g;
        if (standardIcon(icon_, &g)) {
            QColor fg = isEnabled() ? QColor(238, 238, 238) : kOff;
            if (g == Glyph::Play && playing_) fg = QColor(120, 220, 140);
            if (isEnabled())
                for (const QPointF& o : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) drawGlyph(p, g, r.translated(o), kOutline);
            drawGlyph(p, g, r, fg);
        } else {
            drawTrimIcon(p, icon_, r, isEnabled());
        }
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    TrimIcon icon_;
    bool playing_ = false;
};

// One of the three time boxes under the transport (VEGAS: selection start, end and length, each after a small
// picture; with no selection the first shows the cursor and the length is the file's, dimmed). A double-click types a
// time into it.
class TrimReadout : public QLabel {
public:
    enum Kind { Start, End, Length };
    TrimReadout(Kind k, const char* name, const QString& tip, QWidget* parent) : QLabel(parent), kind(k)
    {
        setObjectName(name);
        setToolTip(tip);
        setIndent(22);
        setMinimumWidth(112);
        setFixedHeight(20);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    Kind kind;
    bool showsCursor = false;   // Start with no selection: the cursor's picture
    bool dim = false;
    std::function<void(const QString&)> onEdit;
    void setShown(const QString& t, bool dimmed)
    {
        if (t == text() && dimmed == dim) return;
        dim = dimmed;
        QPalette pal = palette();
        pal.setColor(QPalette::WindowText, dimmed ? QColor(128, 128, 128) : QColor(220, 220, 220));
        setPalette(pal);
        setText(t);
    }
    QLineEdit* editor() const { return edit_; }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QLabel::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double cx = 9.0, cy = height() / 2.0;
        p.setPen(QPen(kOutline, 1.0));
        if (kind == Start && showsCursor) {
            // the cursor's head: a white tab with a stem
            p.setBrush(QColor(238, 238, 238));
            p.drawRoundedRect(QRectF(cx - 3.5, cy - 7.0, 7.0, 8.0), 1.5, 1.5);
            p.drawRect(QRectF(cx - 1.0, cy + 1.0, 2.0, 6.0));
            p.setPen(Qt::NoPen);
            p.setBrush(kOutline);
            p.drawRect(QRectF(cx - 1.5, cy - 5.0, 1.0, 2.5));
            p.drawRect(QRectF(cx + 0.5, cy - 5.0, 1.0, 2.5));
        } else if (kind == Start || kind == End) {
            QPainterPath t;
            t.moveTo(cx - 5.0, cy - 5.0);
            t.lineTo(cx + 5.0, cy - 5.0);
            t.lineTo(kind == Start ? cx - 5.0 : cx + 5.0, cy + 5.0);
            t.closeSubpath();
            p.setBrush(kMarkYellow);
            p.drawPath(t);
        } else {
            // the selection's length: yellow in-out ends over a white bracket
            p.setBrush(kMarkYellow);
            p.drawRect(QRectF(cx - 7.0, cy - 6.0, 14.0, 9.0));
            QPainterPath b;
            b.moveTo(cx - 6.0, cy + 5.5);
            b.lineTo(cx - 6.0, cy + 1.5);
            b.lineTo(cx - 2.5, cy - 3.5);
            b.lineTo(cx + 2.5, cy - 3.5);
            b.lineTo(cx + 6.0, cy + 1.5);
            b.lineTo(cx + 6.0, cy + 5.5);
            b.closeSubpath();
            p.setPen(QPen(QColor(238, 238, 238), 1.5));
            p.setBrush(QColor(70, 70, 70));
            p.drawPath(b);
        }
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        if (!onEdit || edit_) return;
        edit_ = new QLineEdit(this);
        edit_->setObjectName("trimmerTimeEdit");
        edit_->setGeometry(indent() - 3, 0, width() - indent() + 3, height());
        edit_->setText(text());
        edit_->selectAll();
        edit_->show();
        edit_->setFocus();
        QObject::connect(edit_, &QLineEdit::editingFinished, this, [this] {
            QLineEdit* e = edit_;
            if (!e) return;
            edit_ = nullptr;     // (hiding it takes the focus away, which finishes the editing again)
            const QString t = e->text();
            e->hide();
            e->deleteLater();
            if (onEdit) onEdit(t);
        });
    }

private:
    QLineEdit* edit_ = nullptr;
};

// A time typed into a box: hh:mm:ss:ff (or fewer fields, from the right: ss:ff, mm:ss:ff; ';' ',' '.' work too), or
// plain seconds. -1 if it can't be read.
double parseTrimmerTime(const QString& text, double fps)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return -1;
    const QStringList parts = t.split(QRegularExpression("[:;,]"));
    bool ok = false;
    if (parts.size() == 1) {
        const double s = t.toDouble(&ok);
        return ok && s >= 0 ? s : -1;
    }
    if (parts.size() > 4) return -1;
    double v[4] = {0, 0, 0, 0};
    for (int k = 0; k < parts.size(); ++k) {
        v[4 - parts.size() + k] = parts[k].toDouble(&ok);
        if (!ok || v[4 - parts.size() + k] < 0) return -1;
    }
    const double nominal = std::round(fps > 0 ? fps : 30.0);
    return v[0] * 3600 + v[1] * 60 + v[2] + v[3] / nominal;
}

// The transport's buttons in VEGAS's order (key, menu text): Edit Visible Button Set chooses which are on the bar, the
// hamburger menu lists the others.
struct TransportEntry {
    const char* key;
    const char* text;
};
const TransportEntry kTransport[] = {
    {"loop", "Loop Playback"}, {"playFromStart", "Play From Start"}, {"play", "Play"}, {"pause", "Pause"}, {"stop", "Stop"},
    {"previousFrame", "Previous Frame"}, {"nextFrame", "Next Frame"}, {"markIn", "Mark In"}, {"markOut", "Mark Out"},
    {"addFromCursor", "Add to Timeline from Cursor"}, {"addUpToCursor", "Add to Timeline up to Cursor"}, {"fitToFill", "Fit to Fill"},
    {"overwrite", "Enable Timeline Overwrite"}, {"createSubclip", "Create Subclip"}, {"insertMarker", "Insert Marker"},
    {"insertRegion", "Insert Region"}, {"saveMarkers", "Save Markers/Regions"}};
// As VEGAS has it at first (the manual's "Trimmer Transport Tool Bar").
const char* const kDefaultTransport[] = {"play", "pause", "stop", "addFromCursor", "fitToFill"};
}  // namespace

struct TrimmerWindow::Impl {
    TrimmerWindow* q = nullptr;
    QComboBox* history = nullptr;
    TrimPicture* picture = nullptr;
    TrimStrip* strip = nullptr;
    TrimButton *play = nullptr, *loop = nullptr;
    // the transport's buttons by key (kTransport), the bar they are on and its hamburger
    std::vector<std::pair<QString, TrimButton*>> transport;
    QStringList transportSet;
    QWidget* transportBar = nullptr;
    TrimButton* transportMenu = nullptr;
    TrimButton* externalMonitorBtn = nullptr;
    std::function<void(bool)> externalMonitorHook;
    std::function<void()> subclipHook;
    TrimButton* button(const char* key) const
    {
        for (const auto& b : transport)
            if (b.first == key) return b.second;
        return nullptr;
    }
    TrimReadout *startBox = nullptr, *endBox = nullptr, *lengthBox = nullptr;
    QLabel* streamLbl = nullptr;
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
    bool autoSave = vprefs::on("trimmer/autoSaveMarkers");
    bool toPreview = QSettings().value("trimmer/videoInPreview", false).toBool();
    QLabel* marksLbl = nullptr;
    bool overwrite = QSettings().value("trimmer/overwrite", false).toBool();
    bool hoverScrub = QSettings().value("trimmer/hoverScrub", true).toBool();
    TrimButton* overwriteBtn = nullptr;
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
    double returnTo = 0;    // where the cursor was when Play was pressed: Stop goes back there
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

    void projectChanged(int changes);
    void updateLabels()
    {
        // as VEGAS: with a selection its start, end and length; without, the cursor and the file's length (dimmed)
        const bool open = !path.isEmpty();
        startBox->showsCursor = !selection();
        startBox->setShown(open || selection() ? qs(timecode(selection() ? in : pos, fps)) : QString(), false);
        startBox->update();
        endBox->setShown(selection() ? qs(timecode(out, fps)) : QString(), false);
        lengthBox->setShown(open ? qs(timecode(selection() ? out - in : dur, fps)) : QString(), !selection());
        strip->cursor = pos;
        strip->in = in;
        strip->out = out;
        strip->update();
        const bool both = info.hasVideo && info.hasAudio;
        const int st = both ? streams : 0;
        // (a few words left of the time boxes: which stream the Add commands take when it isn't both, and markers not saved)
        streamLbl->setText(!both ? QString() : st == 1 ? QString("Video only") : st == 2 ? QString("Audio only") : QString());
        picture->dimmed = st == 2;
        strip->dimWave = st == 1;
        picture->update();
        strip->marks = marks;
        strip->fileMarks = info.markers;
        int nm = 0, nr = 0;
        for (const MediaItem::Marker& m : marks) (m.length > 0 ? nr : nm)++;
        marksLbl->setText(marks.empty() || marksSaved ? QString() : QString("Not saved"));
        marksLbl->setToolTip(QString("The Trimmer's markers (M) and regions (R): %1 marker%2, %3 region%4%5").arg(nm).arg(nm == 1 ? "" : "s").arg(nr)
                                 .arg(nr == 1 ? "" : "s").arg(marksSaved ? QString() : QString(", not saved yet (S saves them with the media)")));
        play->setPlaying(playing);
        updateButtons();
    }
    // What the buttons can do now (the playing ones always look ready, as in VEGAS): the Add and marker buttons need a
    // file, Fit to Fill a time selection on the timeline too, Save Markers/Regions markers not yet saved.
    void updateButtons()
    {
        const bool open = !path.isEmpty() && dur > 0;
        auto enable = [](QWidget* w, bool on) {
            if (w && w->isEnabled() != on) w->setEnabled(on);
        };
        for (const char* k : {"addFromCursor", "addUpToCursor", "insertMarker", "insertRegion"}) enable(button(k), open);
        enable(button("fitToFill"), open && AppState::get().timeSel());
        enable(button("saveMarkers"), open && !marksSaved);
        enable(button("createSubclip"), open && selection() && subclipHook != nullptr);
        enable(externalMonitorBtn, externalMonitorHook != nullptr);
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
                previewBus().process(l, r, n);
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

// The bar on top, as in VEGAS 18: the Trimmer history (the files opened), then Sort Trimmer History, Remove Current
// Media from Trimmer History, Clear Trimmer History and Trimmer on External Monitor.
QWidget* TrimmerWindow::buildToolBar()
{
    QWidget* top = new QWidget(this);
    top->setAutoFillBackground(true);
    QPalette tp = top->palette();
    tp.setColor(QPalette::Window, tool());
    top->setPalette(tp);
    top->setFixedHeight(29);
    auto* th = new QHBoxLayout(top);
    th->setContentsMargins(4, 2, 4, 2);
    th->setSpacing(1);
    d->history = new QComboBox(top);
    d->history->setObjectName("trimmerHistory");
    d->history->setToolTip("Trimmer history: the files opened in the Trimmer");
    d->history->setFocusPolicy(Qt::NoFocus);
    d->history->setMinimumWidth(160);
    th->addWidget(d->history, 1);
    th->addSpacing(4);
    auto* sortBtn = new TrimButton(TrimIcon::SortHistory, "trimmerSortHistory", "Sort Trimmer History (H; Ctrl+click: Z to A)", top);
    auto* removeBtn = new TrimButton(TrimIcon::RemoveCurrent, "trimmerRemoveCurrent", "Remove Current Media from Trimmer History (Ctrl+H)", top);
    auto* clear = new TrimButton(TrimIcon::ClearHistory, "trimmerClearHistory", "Clear Trimmer History (Ctrl+Shift+H)", top);
    d->externalMonitorBtn = new TrimButton(TrimIcon::ExternalMonitor, "trimmerExternalMonitor", "Trimmer on External Monitor", top);
    d->externalMonitorBtn->setCheckable(true);
    for (QWidget* b : {(QWidget*)sortBtn, (QWidget*)removeBtn, (QWidget*)clear, (QWidget*)d->externalMonitorBtn}) th->addWidget(b);
    QObject::connect(sortBtn, &QAbstractButton::clicked, this, [this] { sortHistory((QApplication::keyboardModifiers() & Qt::ControlModifier) != 0); });
    QObject::connect(removeBtn, &QAbstractButton::clicked, this, [this] { removeCurrentFromHistory(); });
    QObject::connect(clear, &QAbstractButton::clicked, this, [this] { clearHistory(); });
    QObject::connect(d->externalMonitorBtn, &QAbstractButton::toggled, this, [this](bool on) {
        if (d->externalMonitorHook) d->externalMonitorHook(on);
    });
    QObject::connect(d->history, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (d->fillingHistory || i < 0) return;
        const QString p = d->history->itemData(i).toString();
        if (p != d->path) openFile(p);
    });
    return top;
}

// Under the strip, as in VEGAS 18: the Trimmer Transport tool bar in the middle (the buttons Edit Visible Button Set
// chooses, then the hamburger with the others), and below it the three time boxes.
QWidget* TrimmerWindow::buildTransportBar()
{
    QWidget* under = new QWidget(this);
    under->setAutoFillBackground(true);
    QPalette up = under->palette();
    up.setColor(QPalette::Window, tool());
    under->setPalette(up);
    auto* v = new QVBoxLayout(under);
    v->setContentsMargins(4, 6, 4, 5);
    v->setSpacing(7);

    d->transportBar = new QWidget(under);
    d->transportBar->setObjectName("trimmerTransport");
    v->addWidget(d->transportBar);
    struct Made {
        const char* key;
        TrimIcon icon;
        const char* name;
        const char* tip;
    };
    const Made made[] = {
        {"loop", TrimIcon::Loop, "trimmerLoop", "Loop Playback"},
        {"playFromStart", TrimIcon::PlayFromStart, "trimmerPlayFromStart", "Play From Start"},
        {"play", TrimIcon::Play, "trimmerPlay", "Play (Space)"},
        {"pause", TrimIcon::Pause, "trimmerPause", "Pause"},
        {"stop", TrimIcon::Stop, "trimmerStop", "Stop (Space while playing)"},
        {"previousFrame", TrimIcon::PrevFrame, "trimmerPrevFrame", "Previous Frame (Left)"},
        {"nextFrame", TrimIcon::NextFrame, "trimmerNextFrame", "Next Frame (Right)"},
        {"markIn", TrimIcon::MarkIn, "trimmerIn", "Marks the beginning of the Trimmer selection. (I)"},
        {"markOut", TrimIcon::MarkOut, "trimmerOut", "Marks the end of the Trimmer selection. (O)"},
        {"addFromCursor", TrimIcon::AddFromCursor, "trimmerAdd",
         "Add to Timeline from Cursor (A): creates a new event on the timeline using the Trimmer selection, beginning at the cursor position"},
        {"addUpToCursor", TrimIcon::AddUpToCursor, "trimmerAddEnd",
         "Add to Timeline up to Cursor (Shift+A): creates a new event on the timeline using the Trimmer selection, ending at the cursor position"},
        {"fitToFill", TrimIcon::FitToFill, "trimmerFitToFill",
         "Fit to Fill: adds the Trimmer selection to the timeline, stretching or compressing it as needed to fill the timeline selection"},
        {"overwrite", TrimIcon::Overwrite, "trimmerOverwrite",
         "Enable Timeline Overwrite: replaces the contents of the timeline when creating events from the Trimmer"},
        {"createSubclip", TrimIcon::CreateSubclip, "trimmerCreateSubclip", "Create Subclip: a new clip in Project Media from the Trimmer selection"},
        {"insertMarker", TrimIcon::InsertMarker, "trimmerInsertMarker", "Insert Marker (M)"},
        {"insertRegion", TrimIcon::InsertRegion, "trimmerInsertRegion", "Insert Region (R)"},
        {"saveMarkers", TrimIcon::SaveMarkers, "trimmerSaveMarkers", "Save Markers/Regions (S): keeps the Trimmer's markers and regions with the media"},
    };
    for (const Made& m : made) {
        auto* b = new TrimButton(m.icon, m.name, m.tip, d->transportBar);
        d->transport.push_back(std::make_pair(QString(m.key), b));
    }
    d->play = d->button("play");
    d->loop = d->button("loop");
    d->loop->setCheckable(true);
    d->overwriteBtn = d->button("overwrite");
    d->overwriteBtn->setCheckable(true);
    d->overwriteBtn->setChecked(d->overwrite);
    d->transportMenu = new TrimButton(TrimIcon::Menu, "trimmerTransportMenu", "More buttons; Edit Visible Button Set...", d->transportBar);
    auto on = [this](const char* key, std::function<void()> fn) { QObject::connect(d->button(key), &QAbstractButton::clicked, this, [fn] { fn(); }); };
    QObject::connect(d->loop, &QAbstractButton::toggled, this, [this](bool b) { setLoop(b); });
    on("playFromStart", [this] { playFromStart(); });
    on("play", [this] { play(); });
    on("pause", [this] { pause(); });
    on("stop", [this] { stop(); });
    on("previousFrame", [this] { stepFrames(-1); });
    on("nextFrame", [this] { stepFrames(1); });
    on("markIn", [this] { markIn(); });
    on("markOut", [this] { markOut(); });
    on("addFromCursor", [this] { addToTimeline(false); });
    on("addUpToCursor", [this] { addToTimeline(true); });
    on("fitToFill", [this] { fitToFill(); });
    QObject::connect(d->overwriteBtn, &QAbstractButton::toggled, this, [this](bool b) { setTimelineOverwrite(b); });
    on("createSubclip", [this] {
        if (d->subclipHook) d->subclipHook();
    });
    on("insertMarker", [this] { insertMarker(); });
    on("insertRegion", [this] { insertRegion(); });
    on("saveMarkers", [this] { saveMarkers(); });
    QObject::connect(d->transportMenu, &QAbstractButton::clicked, this, [this] {
        QMenu* m = transportMenu();
        m->exec(d->transportMenu->mapToGlobal(QPoint(0, d->transportMenu->height())));
        m->deleteLater();
    });
    QStringList set = QSettings().value("trimmer/transportButtons").toStringList();
    if (!QSettings().contains("trimmer/transportButtons"))
        for (const char* k : kDefaultTransport) set << k;
    d->transportSet = set;
    arrangeTransport();

    // the times: selection start (or the cursor), end and length
    auto* times = new QHBoxLayout();
    times->setSpacing(10);
    QWidget* info = new QWidget(under);
    auto* ih = new QHBoxLayout(info);
    ih->setContentsMargins(0, 0, 0, 0);
    ih->setSpacing(8);
    d->streamLbl = new QLabel(info);
    d->streamLbl->setObjectName("trimmerStreams");
    d->streamLbl->setToolTip("What the Add commands put on the timeline (Tab changes it)");
    d->marksLbl = new QLabel(info);
    d->marksLbl->setObjectName("trimmerMarkers");
    d->marksLbl->setToolTip("The Trimmer's markers (M) and regions (R); S saves them with the media");
    for (QLabel* l : {d->streamLbl, d->marksLbl}) {
        QPalette lp = l->palette();
        lp.setColor(QPalette::WindowText, QColor(150, 150, 150));
        l->setPalette(lp);
        ih->addWidget(l);
    }
    ih->addStretch(1);
    info->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);    // (the boxes stay in the middle)
    times->addWidget(info, 1);
    d->startBox = new TrimReadout(TrimReadout::Start, "trimmerInTime", "Start of the selection (the cursor when nothing is selected); double-click to type a time", under);
    d->endBox = new TrimReadout(TrimReadout::End, "trimmerOutTime", "End of the selection; double-click to type a time", under);
    d->lengthBox = new TrimReadout(TrimReadout::Length, "trimmerLength", "Length of the selection (dimmed: of the whole file); double-click to type a length", under);
    for (TrimReadout* r : {d->startBox, d->endBox, d->lengthBox}) times->addWidget(r);
    QWidget* right = new QWidget(under);
    times->addWidget(right, 1);
    v->addLayout(times);
    Impl* impl = d.get();
    d->startBox->onEdit = [this, impl](const QString& t) {
        const double s = parseTrimmerTime(t, impl->fps);
        if (s < 0) return impl->updateLabels();
        if (impl->selection()) setSelection(s, std::max(s, impl->out));
        else setPosition(s);
    };
    d->endBox->onEdit = [this, impl](const QString& t) {
        const double s = parseTrimmerTime(t, impl->fps);
        if (s < 0) return impl->updateLabels();
        setSelection(impl->selection() ? std::min(impl->in, s) : std::min(impl->pos, s), s);
    };
    d->lengthBox->onEdit = [this, impl](const QString& t) {
        const double s = parseTrimmerTime(t, impl->fps);
        if (s < 0) return impl->updateLabels();
        const double a = impl->selection() ? impl->in : impl->pos;
        setSelection(a, a + s);
    };
    return under;
}

// Puts the buttons of the visible set on the transport bar, in VEGAS's order, centered, with the hamburger last.
void TrimmerWindow::arrangeTransport()
{
    delete d->transportBar->layout();
    auto* h = new QHBoxLayout(d->transportBar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    h->addStretch(1);
    for (const auto& b : d->transport) {
        const bool shown = d->transportSet.contains(b.first);
        b.second->setVisible(shown);
        if (shown) h->addWidget(b.second);
    }
    h->addWidget(d->transportMenu);
    h->addStretch(1);
}

QStringList TrimmerWindow::visibleTransportButtons() const { return d->transportSet; }

void TrimmerWindow::setVisibleTransportButtons(const QStringList& set)
{
    d->transportSet.clear();
    for (const TransportEntry& e : kTransport)
        if (set.contains(e.key)) d->transportSet << e.key;
    QSettings().setValue("trimmer/transportButtons", d->transportSet);
    arrangeTransport();
}

// The hamburger's menu: the buttons not on the bar (as commands; Loop Playback and Enable Timeline Overwrite as
// checks), then Edit Visible Button Set.
QMenu* TrimmerWindow::transportMenu()
{
    auto* m = new QMenu(this);
    m->setObjectName("trimmerTransportMenu");
    for (const TransportEntry& e : kTransport) {
        if (d->transportSet.contains(e.key)) continue;
        TrimButton* b = d->button(e.key);
        QAction* a = m->addAction(e.text);
        a->setObjectName(QString("trimmerMenu_") + e.key);
        a->setEnabled(b->isEnabled());
        a->setCheckable(b->isCheckable());
        a->setChecked(b->isChecked());
        QObject::connect(a, &QAction::triggered, b, [b] { b->click(); });
    }
    m->addSeparator();
    QAction* edit = m->addAction("Edit Visible Button Set...");
    edit->setObjectName("trimmerMenu_editSet");
    QObject::connect(edit, &QAction::triggered, this, [this] { editVisibleTransportButtons(); });
    return m;
}

// Edit Visible Button Set: a check box for each of the transport's buttons.
void TrimmerWindow::editVisibleTransportButtons()
{
    QDialog dlg(this);
    dlg.setObjectName("trimmerButtonSet");
    dlg.setWindowTitle("Edit Visible Button Set");
    auto* v = new QVBoxLayout(&dlg);
    std::vector<std::pair<QString, QCheckBox*>> boxes;
    for (const TransportEntry& e : kTransport) {
        auto* c = new QCheckBox(e.text, &dlg);
        c->setObjectName(QString("button_") + e.key);
        c->setChecked(d->transportSet.contains(e.key));
        v->addWidget(c);
        boxes.push_back(std::make_pair(QString(e.key), c));
    }
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList set;
    for (const auto& b : boxes)
        if (b.second->isChecked()) set << b.first;
    setVisibleTransportButtons(set);
}

void TrimmerWindow::setExternalMonitorHook(std::function<void(bool on)> fn)
{
    d->externalMonitorHook = std::move(fn);
    if (!d->externalMonitorHook) setExternalMonitorShown(false);
    d->updateButtons();
}

void TrimmerWindow::setExternalMonitorShown(bool on)
{
    const QSignalBlocker block(d->externalMonitorBtn);
    d->externalMonitorBtn->setChecked(on);
}

void TrimmerWindow::setCreateSubclipHook(std::function<void()> fn)
{
    d->subclipHook = std::move(fn);
    d->updateButtons();
}

// The strip's right-click menu ('mark': the marker clicked on, -1 none).
void TrimmerWindow::showStripMenu(const QPoint& at, int mark)
{
    Impl* impl = d.get();
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
}

// The project changed: the media shown may be gone, or its markers changed elsewhere (shown here when they had been
// saved with it).
void TrimmerWindow::Impl::projectChanged(int changes)
{
    updateButtons();    // (Fit to Fill follows the timeline's time selection)
    if (!(changes & kProject) || !mediaId) return;
    const MediaItem* m = AppState::get().project().findMedia(mediaId);
    if (!m) {
        mediaId = 0;
        return;
    }
    bool same = m->markers.size() == marks.size();
    for (size_t k = 0; same && k < marks.size(); ++k)
        same = m->markers[k].time == marks[k].time && m->markers[k].length == marks[k].length && m->markers[k].name == marks[k].name;
    if (!same && marksSaved) {
        marks = m->markers;
        updateLabels();
    }
}

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

    v->addWidget(buildToolBar());

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

    v->addWidget(buildTransportBar());

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
    d->strip->onMenu = [this](const QPoint& at, int mark) { showStripMenu(at, mark); };
    d->strip->onCursor = [this](double t) { setPosition(t); setFocus(); };
    d->strip->onSelect = [this](double a, double b) { setSelection(a, b); setFocus(); };
    d->strip->onDragOut = [impl] { impl->startDrag(); };

    d->listener.reset(new Listener([impl](int changes) { impl->projectChanged(changes); }));
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
    if (!startProgram(editor, QStringList() << QDir::toNativeSeparators(d->path))) {
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
    vprefs::set("trimmer/autoSaveMarkers", on);   // through the cache the Preferences dialog reads
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

void TrimmerWindow::play()
{
    if (!d->playing) d->returnTo = d->pos;
    d->startPlaying();
}

void TrimmerWindow::playFromStart()
{
    if (d->playing) d->stopPlaying();
    d->returnTo = d->pos;
    d->pos = d->selection() ? d->in : 0.0;
    d->startPlaying();
}

void TrimmerWindow::pause()
{
    d->stopPlaying();
    d->updateLabels();
}

void TrimmerWindow::stop()
{
    const bool was = d->playing;
    d->stopPlaying();
    if (was) {
        d->pos = std::min(d->dur, std::max(0.0, d->returnTo));
        d->requestFrame();
    }
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
