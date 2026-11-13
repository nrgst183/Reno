// The Mixing Console and the Master Bus panel. The console is a row of channel strips as in VEGAS: one per audio track,
// then one per bus, then the
// master (kept in view at the right). Each strip has, from the top: name, effects button, output, sends to buses (track
// strips), pan, mute and
// solo, and a tall fader beside a stereo peak meter. Everything is redrawn from the project on each change, and every
// change made here goes through
// AppState (a drag is one undo step). Meters read Playback's published levels about 30 times a second, hold their peaks
// for a moment, fall back slowly and clear when playback stops.
#include "mixer.h"
#include "recorder.h"
#include "playback.h"
#include "surroundpanner.h"
#include "pluginchooser.h"
#include "fxlibrary.h"
#include "automationctl.h"
#include "pollrate.h"
#include "strconv.h"
#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QSlider>
#include <QElapsedTimer>
#include <QFrame>
#include <QIcon>
#include <QPixmap>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <set>
#include <cmath>

namespace reno {
namespace app {

namespace {


const QColor kConsoleBg(34, 34, 37);
const QColor kStripBg(50, 50, 54);
const QColor kStripEdge(24, 24, 26);
const QColor kStripSelected(38, 66, 96);
const QColor kText(222, 222, 222);
const QColor kDimText(150, 150, 155);
const QColor kGroove(16, 16, 18);
const QColor kTick(105, 105, 110);
const QColor kAccent(0, 122, 204);
const QColor kBusColor(92, 120, 150);
const QColor kFxSendColor(122, 96, 150);
const QColor kInputColor(70, 120, 90);
const QColor kMasterColor(150, 112, 60);

const int kSendH = 18;
const uint64_t kOutputOff = ~(uint64_t)0;     // the output choice 'Off' of an input bus

QColor trackColor(const Track& t, int index)
{
    if (t.color) return QColor((int)((t.color >> 16) & 0xff), (int)((t.color >> 8) & 0xff), (int)(t.color & 0xff));
    static const QColor colors[] = {QColor(118, 104, 204), QColor(60, 140, 214), QColor(46, 166, 156), QColor(122, 180, 64),
                                    QColor(220, 158, 56), QColor(212, 90, 90), QColor(178, 94, 182), QColor(56, 174, 200)};
    return colors[(size_t)index % (sizeof(colors) / sizeof(colors[0]))];
}

// ------------------------------------------------------------------------------------------ fader law
// Where a gain sits along a fader (0 bottom .. 1 top), as VEGAS's console faders: +12 dB at the top, 0 dB two thirds of
// the way up, evenly in dB down to -12 dB (a third of the way up), then squeezed (a sixth as much per dB) down to
// -60 dB, and straight down to silence below that.
struct TaperPoint { double db, pos; };
const TaperPoint kTaper[] = {{12, 1.0}, {6, 0.83}, {0, 0.66}, {-6, 0.49}, {-12, 0.32}, {-18, 0.286},
                             {-24, 0.252}, {-36, 0.184}, {-48, 0.116}, {-60, 0.048}};
const int kTaperN = (int)(sizeof(kTaper) / sizeof(kTaper[0]));
const double kMaxGain = 3.9810717055349722;

double gainToPos(double g)
{
    if (!(g > 0)) return 0.0;
    const double db = 20.0 * std::log10(g);
    if (db >= kTaper[0].db) return 1.0;
    if (db <= kTaper[kTaperN - 1].db) return kTaper[kTaperN - 1].pos * g / 0.001;
    for (int i = 0; i + 1 < kTaperN; ++i)
        if (db >= kTaper[i + 1].db) {
            const double f = (db - kTaper[i + 1].db) / (kTaper[i].db - kTaper[i + 1].db);
            return kTaper[i + 1].pos + f * (kTaper[i].pos - kTaper[i + 1].pos);
        }
    return 0.0;
}

double posToGain(double pos)
{
    if (pos <= 0) return 0.0;
    if (pos >= 1) return kMaxGain;
    if (pos <= kTaper[kTaperN - 1].pos) return 0.001 * pos / kTaper[kTaperN - 1].pos;
    for (int i = 0; i + 1 < kTaperN; ++i)
        if (pos >= kTaper[i + 1].pos) {
            const double f = (pos - kTaper[i + 1].pos) / (kTaper[i].pos - kTaper[i + 1].pos);
            return std::pow(10.0, (kTaper[i + 1].db + f * (kTaper[i].db - kTaper[i + 1].db)) / 20.0);
        }
    return 0.0;
}

QString dbText(double gain, bool unit = false)
{
    if (gain <= 1e-5) return unit ? QString("-Inf dB") : QString("-Inf");
    double db = 20.0 * std::log10(gain);
    if (std::fabs(db) < 0.05) db = 0;
    return QString::number(db, 'f', 1) + (unit ? " dB" : "");
}

QString panText(double pan)
{
    if (std::fabs(pan) < 0.005) return QString("Center");
    return QString("%1% %2").arg((int)std::lround(std::fabs(pan) * 100.0)).arg(pan < 0 ? "L" : "R");
}

// ------------------------------------------------------------------------------------------ meter law
// Meters show -60 dB (or the range chosen with a right-click on a meter, for all of them) .. 0 dB, evenly in dB, as
// VEGAS's console (going over is shown by the peak readout turning red).
double kMeterLo = -60.0;
const double kMeterHi = 0.0;
double meterFrac(double db) { return std::clamp((db - kMeterLo) / (kMeterHi - kMeterLo), 0.0, 1.0); }
double levelDb(float v) { return v > 1e-6f ? 20.0 * std::log10((double)v) : -200.0; }

QString peakText(float v)
{
    if (v <= 1e-6f) return QString("-Inf");
    const double db = levelDb(v);
    return QString::number(db, 'f', std::fabs(db) < 10.0 ? 1 : 0);
}

// A meter's moving parts: the level, a peak that holds for a moment then falls, and whether it has clipped. Two
// channels, or six for a 5.1 master (L R C LFE Ls Rs).
struct MeterState {
    float level[6] = {0, 0, 0, 0, 0, 0}, hold[6] = {0, 0, 0, 0, 0, 0};
    double holdUntil[6] = {0, 0, 0, 0, 0, 0};
    bool clip[6] = {false, false, false, false, false, false};
    bool active = false;
    int channels = 2;

    void feed(float l, float r, double now, double dt, bool holdPeaks = true)
    {
        const float v[2] = {l, r};
        feedN(v, 2, now, dt, holdPeaks);
    }
    void feedN(const float* v, int n, double now, double dt, bool holdPeaks = true)
    {
        if (n != channels) {
            const bool was = active;
            *this = MeterState();
            active = was;
            channels = n == 6 ? 6 : 2;
        }
        const float fall = (float)std::pow(10.0, -20.0 * dt / 20.0);
        for (int k = 0; k < channels; ++k) {
            level[k] = v[k];
            if (v[k] >= hold[k] || !holdPeaks) {
                hold[k] = v[k];
                holdUntil[k] = now + 1.5;
            } else if (now > holdUntil[k]) {
                hold[k] = std::max(v[k], hold[k] * fall);
            }
            if (v[k] > 1.0f) clip[k] = true;
        }
        active = true;
    }
    void reset() { *this = MeterState(); }
};

// Whether two meter states draw the same (levels, held peaks, clip marks, channel count): the meters are fed 30 times a
// second, and a silent channel's meter need not be repainted each time.
bool drawsSame(const MeterState& a, const MeterState& b)
{
    if (a.channels != b.channels || a.active != b.active) return false;
    for (int k = 0; k < a.channels; ++k)
        if (a.level[k] != b.level[k] || a.hold[k] != b.hold[k] || a.clip[k] != b.clip[k]) return false;
    return true;
}

struct Listener {
    int id = 0;
    explicit Listener(std::function<void(int)> fn) { id = AppState::get().addListener(std::move(fn)); }
    ~Listener() { AppState::get().removeListener(id); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
};

// Moves a value the user drags: relative to where the mouse went down (Ctrl: ten times finer, re-anchored when Ctrl goes
// down or up), one undo step for the whole drag.
struct DragValue {
    bool active = false, started = false, fine = false;
    double anchorValue = 0;
    double anchorMouse = 0;

    void press(double value, double mouse)
    {
        active = true;
        started = false;
        fine = false;
        anchorValue = value;
        anchorMouse = mouse;
    }
    // The new value (0..1) for the mouse at 'mouse' along a control 'length' pixels long; false until the mouse has
    // moved far enough to count as a drag.
    bool move(double mouse, double length, bool ctrl, double current, double& out)
    {
        if (!active) return false;
        if (!started) {
            if (std::fabs(mouse - anchorMouse) < 2.0) return false;
            AppState::get().beginDrag();
            started = true;
        }
        if (ctrl != fine) {
            anchorValue = current;
            anchorMouse = mouse;
            fine = ctrl;
        }
        out = std::clamp(anchorValue + (mouse - anchorMouse) / std::max(1.0, length) * (fine ? 0.1 : 1.0), 0.0, 1.0);
        return true;
    }
    void release(const std::string& what)
    {
        if (active && started) AppState::get().endDrag(what);
        active = started = false;
    }
};

// ------------------------------------------------------------------------------------------ fader and meter
// As VEGAS's console: a fader in dB (when 'value' is set) with its ticks, a stereo peak meter at the right with its dB
// scale drawn over it (3, 6, 9 ... dB down), the held peak above the meter (red once over 0 dB; a click clears it) and
// the fader's value (or Muted) underneath. The master shows two faders, linked unless split.
class FaderMeter : public QWidget {
public:
    FaderMeter(bool big, QWidget* parent) : QWidget(parent), big_(big)
    {
        setMinimumHeight(big ? 160 : 140);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }

    std::function<double()> value;
    std::function<void(Project&, double)> apply;
    // A second fader beside the first (the master's right channel, VEGAS's split master faders); moves on its own while
    // splitOn.
    std::function<double()> value2;
    std::function<void(Project&, double)> apply2;
    bool splitOn = false;
    // Two faders drawn even when linked (the master's left and right, which then move together).
    bool twin = false;
    // The channel is muted: the value underneath reads Muted.
    bool muted = false;
    // A fader that is not part of the project (the Preview bus's): moves go here and make no undo step.
    std::function<void(double)> direct;
    std::string undoName = "Volume";
    MeterState meter;
    // What is shown (control regions and options): fader, meter, fader ticks (Show Fader Ticks), meter scale (left
    // out in narrow strips), held peaks.
    bool showFader = true, showMeter = true, ticks = true, meterScale = true, holdPeaks = true;
    std::function<void(const QPoint& global)> onMeterMenu;

    void feedN(const float* v, int n, double now, double dt)
    {
        const MeterState before = meter;
        meter.feedN(v, n, now, dt, holdPeaks);
        meterFed(before);
    }
    void feed(float l, float r, double now, double dt)
    {
        const MeterState before = meter;
        meter.feed(l, r, now, dt, holdPeaks);
        meterFed(before);
    }
    void clearMeter()
    {
        if (!meter.active) return;
        meter.reset();
        setProperty("levelLeft", 0.f);
        setProperty("levelRight", 0.f);
        setProperty("holdLeft", 0.f);
        setProperty("holdRight", 0.f);
        update();
    }
    bool split() const { return splitOn && value2 && apply2; }
    // The project changed: repaints only if the fader shows something else now (the meter repaints itself as sound
    // comes in). A console of many strips otherwise repainted every fader at every step of any drag.
    void projectChanged()
    {
        if (shownKey() != painted_) update();
    }
    QRect capRect(int which = 0) const { return which ? capAt(gainToPos(value2nd()), geo().grooveX2) : capAt(value ? gainToPos(value()) : 0.0, geo().grooveX); }
    int faderY(double gain) const { return yAt(gainToPos(gain)); }

protected:
    QSize sizeHint() const override { return QSize(big_ ? 80 : 50, 300); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        painted_ = shownKey();
        const Geo g = geo();
        setProperty("cap", capRect());
        setProperty("readout", readoutRect());
        setProperty("split", split());
        if (faders() > 1) setProperty("cap2", capRect(1));
        setProperty("faderTop", g.top);
        setProperty("faderBottom", g.bottom);
        QFont small = font();
        small.setPointSizeF(7.2);
        p.setFont(small);
        const QFontMetrics fm(small);

        if (value && showFader) {
            // the ticks: left of a single fader, between the master's two
            const int tx = faders() > 1 ? (g.grooveX + g.grooveX2) / 2 - 2 : g.grooveX - 11;
            for (int i = 0; ticks && i < kTaperN && tx >= 0; ++i) {
                const int y = yAt(kTaper[i].pos);
                p.fillRect(QRect(tx, y, 4, 1), kTick);
            }
            for (int f = 0; f < faders(); ++f) {
                const int gx = f ? g.grooveX2 : g.grooveX;
                p.fillRect(QRect(gx - 1, g.top - 2, 2, g.bottom - g.top + 4), kGroove);
                p.fillRect(QRect(gx + 1, g.top - 2, 1, g.bottom - g.top + 4), QColor(78, 78, 82));
                const QRect cap = capRect(f);
                const bool hot = (drag_.active && dragging_ == f) || hoverCap_ == f;
                p.fillRect(cap, QColor(16, 16, 18));
                p.fillRect(cap.adjusted(1, 1, -1, -1), hot ? QColor(255, 255, 255) : QColor(226, 226, 230));
                const int bar = std::max(2, cap.width() - 6);
                p.fillRect(QRect(cap.left() + (cap.width() - bar) / 2, cap.top() + 3, bar, 2), QColor(30, 30, 32));
                p.fillRect(QRect(cap.left() + (cap.width() - bar) / 2, cap.bottom() - 4, bar, 2), QColor(30, 30, 32));
                QFont vf = small;
                vf.setPointSizeF(7.6);
                p.setFont(vf);
                // the value underneath (the master's two as a pair, "0.0 0.0")
                p.setPen(muted ? QColor(240, 180, 40) : kText);
                if (faders() == 1) {
                    p.drawText(QRect(gx - 30, g.bottom + 5, 60, height() - g.bottom - 5), Qt::AlignHCenter | Qt::AlignVCenter, muted ? QString("Muted") : dbText(value()));
                } else if (f == 0) {
                    const QString pair = muted ? QString("Muted") : dbText(value()) + "  " + dbText(value2nd());
                    p.drawText(QRect(0, g.bottom + 5, std::max(g.meterX, g.grooveX2 + 20), height() - g.bottom - 5), Qt::AlignHCenter | Qt::AlignVCenter, pair);
                }
                p.setFont(small);
            }
        }
        if (!showMeter) return;

        // the meter: dark, a bar per channel side by side, filled from the bottom
        const QRect box(g.meterX, g.mTop, g.meterBox, g.bottom - g.mTop + 1);
        p.fillRect(box, QColor(24, 24, 26));
        const int base = g.bottom;
        QRegion filled;
        for (int k = 0; k < meter.channels; ++k) {
            const QRect bar(g.meterX + k * (g.meterW + g.meterGap), g.mTop, g.meterW, g.bottom - g.mTop + 1);
            const double lv = levelDb(meter.level[k]);
            const int top = meterY(lv);
            if (meter.level[k] > 1e-6f && top < base) {
                QLinearGradient grad(0, base, 0, g.mTop);
                const double span = std::max(1, base - g.mTop);
                auto at = [&](double db) { return std::clamp((base - meterY(db)) / span, 0.0, 1.0); };
                grad.setColorAt(0.0, QColor(30, 236, 90));
                grad.setColorAt(at(-15.0), QColor(40, 240, 80));
                grad.setColorAt(at(-9.0), QColor(200, 240, 40));
                grad.setColorAt(at(-4.0), QColor(250, 236, 30));
                grad.setColorAt(at(-1.5), QColor(250, 140, 30));
                grad.setColorAt(1.0, QColor(240, 40, 30));
                const QRect fill(bar.left(), top, bar.width(), base - top + 1);
                p.fillRect(fill, grad);
                filled += fill;
            }
            if (meter.hold[k] > 1e-6f) {
                const double hd = levelDb(meter.hold[k]);
                const int y = meterY(hd);
                const QColor hc = hd > -3 ? QColor(250, 110, 30) : (hd > -12 ? QColor(230, 240, 60) : QColor(120, 255, 140));
                p.fillRect(QRect(bar.left(), y, bar.width(), 1), hc);
            }
        }
        if (meterScale) {
            // the scale over the meter, 3 dB apart (fewer when they would crowd): light on the dark, dark on the lit
            const double per3 = (double)(g.bottom - g.mTop) * 3.0 / std::max(1.0, kMeterHi - kMeterLo);
            const int every = per3 >= fm.height() - 1 ? 1 : (per3 * 2 >= fm.height() - 1 ? 2 : (per3 * 3 >= fm.height() - 1 ? 3 : 4));
            QFont sf = small;
            sf.setPointSizeF(7.0);
            p.setFont(sf);
            const QFontMetrics sm(sf);
            auto scale = [&](const QColor& c) {
                p.setPen(c);
                int n = 0;
                for (double db = kMeterHi - 3.0; db > kMeterLo + 1.0; db -= 3.0, ++n) {
                    if (n % every) continue;
                    const int y = meterY(db);
                    p.fillRect(QRect(box.right() - 2, y, 2, 1), c);
                    p.drawText(QRect(box.left(), y - sm.height() / 2, box.width() - 4, sm.height()), Qt::AlignRight | Qt::AlignVCenter, QString::number((int)std::lround(-db)));
                }
            };
            scale(QColor(225, 225, 228));
            if (!filled.isEmpty()) {
                p.save();
                p.setClipRegion(filled);
                scale(QColor(16, 40, 20));
                p.restore();
            }
            p.setFont(small);
        }
        const QRect ro = readoutRect();
        bool clipped = false;
        for (int k = 0; k < meter.channels; ++k) clipped = clipped || meter.clip[k];
        if (clipped) p.fillRect(ro, QColor(190, 30, 30));
        float h = 0;
        for (int k = 0; k < meter.channels; ++k) h = std::max(h, meter.hold[k]);
        QFont rf = small;
        rf.setPointSizeF(7.6);
        p.setFont(rf);
        p.setPen(clipped ? QColor(255, 255, 255) : (meter.active ? kText : kDimText));
        p.drawText(ro.adjusted(0, 0, -1, 0), Qt::AlignRight | Qt::AlignVCenter, peakText(h));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const QPoint pos = e->position().toPoint();
        if (e->button() == Qt::RightButton && showMeter && onMeterMenu && pos.x() >= geo().meterX - 2) {
            onMeterMenu(e->globalPosition().toPoint());
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        if (showMeter && readoutRect().adjusted(-2, -2, 2, 2).contains(pos)) {
            for (int k = 0; k < 6; ++k) {
                meter.clip[k] = false;
                meter.hold[k] = 0;
            }
            update();
            return;
        }
        if (!value || !apply || !showFader) return;
        const Geo g = geo();
        for (int f = 0; f < faders(); ++f) {
            const QRect cap = capRect(f);
            if (cap.adjusted(-2, -2, 2, 2).contains(pos)) {
                dragging_ = f;
                drag_.press(gainToPos(f ? value2nd() : value()), -pos.y());
                update();
                return;
            }
        }
        const int pf = pickAt(pos.x());
        if (std::abs(pos.x() - (pf ? g.grooveX2 : g.grooveX)) <= 8 && pos.y() >= g.top - 8 && pos.y() <= g.bottom + 8) {
            const double v = std::clamp((double)(g.bottom - pos.y()) / std::max(1, g.bottom - g.top), 0.0, 1.0);
            dragging_ = pf;
            if (!direct) AppState::get().beginDrag();
            set(v, false);
            drag_.press(v, -pos.y());
            drag_.started = true;
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QPoint pos = e->position().toPoint();
        int h = -1;
        for (int f = 0; f < faders(); ++f)
            if (capRect(f).adjusted(-2, -2, 2, 2).contains(pos)) h = f;
        if (h != hoverCap_) {
            hoverCap_ = h;
            update();
        }
        if (!drag_.active || !value) return;
        const Geo g = geo();
        const bool ctrl = (e->modifiers() & Qt::ControlModifier) != 0;
        double v;
        if (drag_.move(-pos.y(), g.bottom - g.top, ctrl, gainToPos(dragging_ ? value2nd() : value()), v)) set(v, ctrl);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        drag_.release(undoName);
        controlReleased(AppState::get());
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (!value || !apply || !showFader) return;
        const Geo g = geo();
        const QPoint pos = e->position().toPoint();
        const int f = pickAt(pos.x());
        if (std::abs(pos.x() - (f ? g.grooveX2 : g.grooveX)) > 8) return;
        if (direct) {
            direct(1.0);
            update();
            return;
        }
        std::function<void(Project&, double)> fn = f && split() ? apply2 : apply;
        AppState::get().edit(undoName, [&](Project& p) { fn(p, 1.0); });
    }
    void leaveEvent(QEvent*) override
    {
        hoverCap_ = -1;
        update();
    }

private:
    // How many faders are drawn (the master's two), and the second one's value (the first's while they are linked).
    int faders() const { return split() || (twin && value) ? 2 : 1; }
    double value2nd() const { return split() ? value2() : (value ? value() : 0.0); }
    // After the meter took in sound: the values the tests read, and a repaint if what the meter shows changed.
    void meterFed(const MeterState& before)
    {
        setProperty("meterChannels", meter.channels);
        setProperty("levelLeft", meter.level[0]);
        setProperty("levelRight", meter.level[1]);
        setProperty("holdLeft", meter.hold[0]);
        setProperty("holdRight", meter.hold[1]);
        if (!drawsSame(before, meter)) update();
    }
    // What of the project the fader shows: its level(s), whether it is split, and the view options.
    struct ShownKey {
        double v = -1, v2 = -1;
        bool split = false;
        int options = -1;
        bool operator!=(const ShownKey& o) const { return v != o.v || v2 != o.v2 || split != o.split || options != o.options; }
    };
    ShownKey shownKey() const
    {
        ShownKey k;
        k.v = value ? value() : 0.0;
        k.v2 = value2 ? value2() : 0.0;
        k.split = split();
        k.options = (int)showFader | (int)showMeter << 1 | (int)ticks << 2 | (int)meterScale << 3 | (int)holdPeaks << 4 | (int)muted << 5;
        return k;
    }
    ShownKey painted_;
    // Where things are: the fader groove(s) (top .. bottom is the fader's travel), the meter box at the right (its
    // channels' bars meterW wide, meterGap apart) from mTop down to the same bottom.
    struct Geo {
        int grooveX, grooveX2, meterX, meterBox, meterW, meterGap, top, mTop, bottom;
    };
    Geo geo() const
    {
        Geo g;
        const int n = meter.channels, w = width();
        const bool fader = value && showFader;
        g.meterBox = !showMeter ? 0 : fader ? std::clamp(w - 27, 14, 30) : std::clamp(w - 8, 14, 30);
        g.meterX = fader ? w - g.meterBox - 1 : (w - g.meterBox) / 2;
        g.meterGap = n == 6 ? 1 : 0;
        g.meterW = std::max(1, (g.meterBox - (n - 1) * g.meterGap) / n);
        const int room = showMeter ? g.meterX : w;
        if (faders() > 1) {
            g.grooveX = room / 2 - 8;
            g.grooveX2 = room / 2 + 9;
        } else {
            g.grooveX = room / 2;
            g.grooveX2 = g.grooveX;
        }
        g.top = 6;
        g.mTop = 20;
        g.bottom = height() - 20;
        return g;
    }
    int yAt(double pos) const
    {
        const Geo g = geo();
        return g.bottom - (int)std::lround(pos * (g.bottom - g.top));
    }
    int meterY(double db) const
    {
        const Geo g = geo();
        return g.bottom - (int)std::lround(meterFrac(db) * (g.bottom - g.mTop));
    }
    QRect capAt(double pos, int grooveX) const
    {
        const int w = faders() > 1 ? 8 : 12, h = 12;
        return QRect(grooveX - w / 2, yAt(pos) - h / 2, w, h);
    }
    int pickAt(int x) const
    {
        const Geo g = geo();
        return faders() > 1 && std::abs(x - g.grooveX2) < std::abs(x - g.grooveX) ? 1 : 0;
    }
    QRect readoutRect() const
    {
        const Geo g = geo();
        const int w = std::max(g.meterBox + 8, 34);
        return QRect(std::max(0, g.meterX + g.meterBox - w), 4, w, 14);
    }
    void set(double v, bool ctrl)
    {
        double gain = posToGain(v);
        if (!ctrl && std::fabs(v - gainToPos(1.0)) < 0.012) gain = 1.0;
        if (direct) {
            direct(gain);
            update();
            AppState::get().status(undoName + ": " + dbText(gain, true).toStdString());
            return;
        }
        std::function<void(Project&, double)> fn = dragging_ && split() ? apply2 : apply;
        AppState::get().dragUpdate([&](Project& p) { fn(p, gain); });
        AppState::get().status(undoName + ": " + dbText(gain, true).toStdString());
    }

    bool big_;
    int hoverCap_ = -1;
    int dragging_ = 0;
    DragValue drag_;
};

// ------------------------------------------------------------------------------------------ VU / PPM meters
// VEGAS's VU Meters control region: a VU or peak programme meter per channel; all of the console's meters share one kind
// (right-click menu or the mouse wheel changes them all):
//   Traditional VU (-10 .. +2 VU), Extended VU (-30 .. +8), Logarithmic VU (dB, like Sound Forge's peak meters),
//   UK PPM (marks 1 .. 7, 4 dB apart, 4 = 0 dBu), EBU PPM (-12 .. +12, 0 = 0 dBu), DIN PPM (-50 .. +5 dB, 0 = +6 dBu), Nordic PPM (-42 .. +12 dB, 0 = 0 dBu).
// Levels come from the published peaks (about 30 a second) with each meter's ballistics: VUs average over 300 ms
// (Vintage Mode: an analogue needle's slower, overshooting
// response), PPMs rise in 10 ms (DIN: 5 ms) and fall 20 dB in 1.7 s (Nordic), 1.5 s (DIN), or 24 dB in 2.8 s (UK, EBU).
// 0 dBu is taken as -18 dBFS (EBU R68), so 0 VU (+4 dBu) is -14 dBFS.
struct VuOptions {
    enum Type { TraditionalVU = 0, ExtendedVU, LogVU, UkPPM, EbuPPM, DinPPM, NordicPPM, TypeCount };
    int type = TraditionalVU;
    bool labels = true, peaks = true, holdPeaks = false, holdValleys = false, vintage = false;
    static const char* name(int t)
    {
        static const char* const n[TypeCount] = {"Traditional VU", "Extended VU", "Logarithmic VU", "UK PPM", "EBU PPM", "DIN PPM", "Nordic PPM"};
        return t >= 0 && t < TypeCount ? n[t] : "";
    }
    double lo() const { const double v[TypeCount] = {-10, -30, -60, 1, -12, -50, -42}; return v[type]; }
    double hi() const { const double v[TypeCount] = {2, 8, 6, 7, 12, 5, 12}; return v[type]; }
    double fromDbu(double dbu) const
    {
        switch (type) {
        case TraditionalVU: case ExtendedVU: return dbu - 4.0;
        case LogVU: return dbu - 18.0;
        case UkPPM: return 4.0 + dbu / 4.0;
        case EbuPPM: return dbu;
        case DinPPM: return dbu - 6.0;
        default: return dbu;
        }
    }
    bool ppm() const { return type >= UkPPM; }
};
VuOptions& vuOptions()
{
    static VuOptions o;
    return o;
}

class VuMeter : public QWidget {
public:
    explicit VuMeter(QWidget* parent) : QWidget(parent)
    {
        setObjectName("vuMeter");
        setFixedHeight(52);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    std::function<void(const QPoint&)> onMenu;
    std::function<void(int delta)> onWheel;
    bool shown[2] = {true, true};

    void feed(float l, float r, double dt)
    {
        const VuOptions& o = vuOptions();
        const float in[2] = {l, r};
        for (int k = 0; k < 2; ++k) {
            const double db = in[k] > 1e-7f ? 20.0 * std::log10((double)in[k]) : -120.0;
            double& v = value_[k];
            if (!o.ppm()) {
                // VU: the signal's average (peaks x 0.707 for a sine's RMS) with a 300 ms time constant (vintage: a
                // slower needle that overshoots a little)
                const double target = db - 3.0;
                const double tau = o.vintage ? 0.45 : 0.3;
                const double a = 1.0 - std::exp(-std::max(0.0, dt) / tau);
                vel_[k] = o.vintage ? vel_[k] * 0.6 + (target - v) * a * 0.55 : 0.0;
                v = o.vintage ? v + (target - v) * a + vel_[k] : v + (target - v) * a;
            } else {
                const double fall = o.type == DinPPMType() ? 20.0 / 1.5 : o.type == VuOptions::NordicPPM ? 20.0 / 1.7 : 24.0 / 2.8;
                if (db > v) v = db;
                else v = std::max(db, v - fall * std::max(0.0, dt));
            }
            v = std::max(-120.0, std::min(30.0, v));
            if (v > peak_[k]) peak_[k] = v;
            if (v < valley_[k] || valley_[k] > 29) valley_[k] = v;
            if (in[k] > 1.0f) clip_[k] = true;
        }
        active_ = true;
        setProperty("vuLeft", value_[0]);
        setProperty("vuRight", value_[1]);
        update();
    }
    void clearMeter()
    {
        if (!active_) return;
        for (int k = 0; k < 2; ++k) {
            value_[k] = -120.0;
            vel_[k] = 0;
        }
        active_ = false;
        update();
    }
    void resetClip()
    {
        for (int k = 0; k < 2; ++k) {
            clip_[k] = false;
            peak_[k] = -120.0;
            valley_[k] = 30.0;
        }
        update();
    }

protected:
    static int DinPPMType() { return VuOptions::DinPPM; }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const VuOptions& o = vuOptions();
        p.fillRect(rect(), QColor(30, 31, 30));
        QFont f = font();
        f.setPointSizeF(6.2);
        p.setFont(f);
        p.setPen(kDimText);
        p.drawText(QRect(2, 1, width() - 4, 10), Qt::AlignLeft | Qt::AlignVCenter, o.name(o.type));
        const QRect area(4, 13, width() - 8, height() - (o.labels ? 25 : 16));
        const double lo = o.lo(), hi = o.hi();
        auto xOf = [&](double u) { return area.left() + (int)std::lround(std::clamp((u - lo) / (hi - lo), 0.0, 1.0) * area.width()); };
        const int barH = std::max(3, (area.height() - 2) / 2);
        for (int k = 0; k < 2; ++k) {
            const QRect bar(area.left(), area.top() + k * (barH + 2), area.width(), barH);
            p.fillRect(bar, kGroove);
            if (!shown[k]) continue;
            // the reference (0 VU, PPM 4 / 0) and above it in red
            // where the red starts: 0 VU, full scale, the PPMs' highest permitted levels (UK 6, EBU and Nordic +9)
            const double ref = o.type == VuOptions::UkPPM ? 6.0 : (o.type == VuOptions::EbuPPM || o.type == VuOptions::NordicPPM) ? 9.0 : 0.0;
            const double u = o.fromDbu(value_[k] + 18.0);
            const int x = xOf(u), xr = xOf(ref);
            if (active_ && x > bar.left()) {
                p.fillRect(QRect(bar.left(), bar.top(), std::min(x, xr) - bar.left(), bar.height()), QColor(90, 200, 120));
                if (x > xr) p.fillRect(QRect(xr, bar.top(), x - xr, bar.height()), QColor(226, 82, 64));
            }
            if (o.peaks && peak_[k] > -119) p.fillRect(QRect(xOf(o.fromDbu(peak_[k] + 18.0)) - 1, bar.top(), 2, bar.height()), o.holdPeaks ? QColor(255, 220, 90) : QColor(220, 220, 220, 120));
            if (o.holdValleys && valley_[k] < 29) p.fillRect(QRect(xOf(o.fromDbu(valley_[k] + 18.0)), bar.top(), 1, bar.height()), QColor(120, 180, 255));
            if (clip_[k]) p.fillRect(QRect(bar.right() - 3, bar.top(), 4, bar.height()), QColor(255, 40, 30));
        }
        if (o.labels) {
            p.setPen(kTick);
            const int y = area.bottom() + 2;
            const double step = o.type == VuOptions::UkPPM ? 1 : o.type == VuOptions::TraditionalVU ? 2 : o.type == VuOptions::EbuPPM ? 4 : 10;
            for (double u = std::ceil(lo / step) * step; u <= hi + 1e-9; u += step) {
                const int x = xOf(u);
                p.drawLine(x, area.bottom() - 1, x, area.bottom() + 1);
                const QString t = QString::number(u, 'f', 0);
                if (x - 8 >= 0 && x + 8 <= width()) p.drawText(QRect(x - 10, y, 20, 9), Qt::AlignCenter, t);
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::RightButton && onMenu) onMenu(e->globalPosition().toPoint());
        else if (e->button() == Qt::LeftButton) resetClip();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        if (onWheel) onWheel(e->angleDelta().y() > 0 ? -1 : 1);
        e->accept();
    }

private:
    double value_[2] = {-120.0, -120.0}, vel_[2] = {0, 0};
    double peak_[2] = {-120.0, -120.0}, valley_[2] = {30.0, 30.0};
    bool clip_[2] = {false, false};
    bool active_ = false;
};

// ------------------------------------------------------------------------------------------ pan
// A horizontal pan slider with a mark in the middle and the readout above it. Double-click centres it.
class PanSlider : public QWidget {
public:
    explicit PanSlider(QWidget* parent) : QWidget(parent) { setFixedHeight(34); }
    std::function<double()> value;
    std::function<void(Project&, double)> apply;
    std::string undoName = "Pan";

    // As VEGAS's console: a line across with a marker above it at the pan, and the pan written underneath.
    QRect groove() const { return QRect(6, 11, width() - 12, 2); }
    int xOf(double pan) const { return groove().left() + (int)std::lround((pan + 1.0) * 0.5 * groove().width()); }
    // The project changed: repaints only if the pan shown moved.
    void projectChanged()
    {
        if (!paintedOnce_ || (value ? value() : 0.0) != paintedPan_) update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const double pan = value ? value() : 0.0;
        paintedPan_ = pan;
        paintedOnce_ = true;
        setProperty("knobX", xOf(pan));
        setProperty("groove", groove());
        const QRect g = groove();
        p.fillRect(g, kGroove);
        p.fillRect(QRect(g.left(), g.bottom() + 1, g.width(), 1), QColor(78, 78, 82));
        const int x = xOf(pan);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPointF marker[3] = {QPointF(x - 6.5, g.top() - 8.5), QPointF(x + 6.5, g.top() - 8.5), QPointF(x, g.top() - 1.5)};
        p.setPen(QPen(QColor(16, 16, 18), 1));
        p.setBrush(drag_.active ? QColor(255, 255, 255) : QColor(232, 232, 236));
        p.drawPolygon(marker, 3);
        p.setRenderHint(QPainter::Antialiasing, false);
        QFont f = font();
        f.setPointSizeF(7.6);
        p.setFont(f);
        p.setPen(kText);
        p.drawText(QRect(0, 18, width(), 14), Qt::AlignHCenter | Qt::AlignVCenter, panText(pan));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !value || !apply) return;
        const QPoint pos = e->position().toPoint();
        const double cur = (value() + 1.0) * 0.5;
        if (std::abs(pos.x() - xOf(value())) <= 6) {
            drag_.press(cur, pos.x());
        } else {
            const double v = std::clamp((double)(pos.x() - groove().left()) / std::max(1, groove().width()), 0.0, 1.0);
            AppState::get().beginDrag();
            set(v);
            drag_.press(v, pos.x());
            drag_.started = true;
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!drag_.active || !value) return;
        double v;
        if (drag_.move(e->position().x(), groove().width(), (e->modifiers() & Qt::ControlModifier) != 0, (value() + 1.0) * 0.5, v)) set(v);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        drag_.release(undoName);
        controlReleased(AppState::get());
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        if (!apply) return;
        std::function<void(Project&, double)> fn = apply;
        AppState::get().edit(undoName, [&](Project& p) { fn(p, 0.0); });
    }

private:
    void set(double v)
    {
        double pan = v * 2.0 - 1.0;
        if (std::fabs(pan) < 0.02) pan = 0;
        std::function<void(Project&, double)> fn = apply;
        AppState::get().dragUpdate([&](Project& p) { fn(p, pan); });
        AppState::get().status("Pan: " + panText(pan).toStdString());
    }
    DragValue drag_;
    double paintedPan_ = 0;
    bool paintedOnce_ = false;
};

// ------------------------------------------------------------------------------------------ sends
// One of a track's sends: the bus's short name, a small horizontal fader for the level and a Pre/Post switch.
class SendRow : public QWidget {
public:
    SendRow(uint64_t track, uint64_t bus, QWidget* parent) : QWidget(parent), track_(track), bus_(bus)
    {
        setFixedHeight(kSendH);
        setObjectName(QString("send:%1").arg(bus));
    }
    QRect bar() const { return QRect(14, 3, width() - 14 - 33, height() - 6); }
    QRect prePost() const { return QRect(width() - 31, 2, 29, height() - 4); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        setProperty("bar", bar());
        setProperty("prePost", prePost());
        const Project& pr = AppState::get().project();
        const reno::Bus* b = pr.findBus(bus_);
        const Send* s = find(pr);
        const double level = s ? s->level : 0.0;
        const bool pre = s && s->preFader;
        QFont f = font();
        f.setPointSizeF(6.8);
        p.setFont(f);
        QString name = b ? qs(b->name) : QString("?");
        if (name.startsWith("Bus ")) name = name.mid(4);
        p.setPen(kDimText);
        p.drawText(QRect(1, 0, 13, height()), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(name, Qt::ElideRight, 13));
        const QRect r = bar();
        p.fillRect(r, kGroove);
        const int w = (int)std::lround(gainToPos(level) * r.width());
        if (w > 0) p.fillRect(QRect(r.left(), r.top() + 1, w, r.height() - 2), s && level > 0 ? QColor(70, 120, 170) : kTick);
        p.fillRect(QRect(r.left() + (int)std::lround(gainToPos(1.0) * r.width()), r.top(), 1, r.height()), QColor(120, 120, 126));
        p.setPen(kText);
        p.drawText(r.adjusted(2, 0, -2, 0), Qt::AlignCenter, dbText(level));
        const QRect pp = prePost();
        p.fillRect(pp, pre ? QColor(178, 120, 40) : QColor(62, 62, 66));
        p.setPen(QColor(24, 24, 26));
        p.drawRect(pp.adjusted(0, 0, -1, -1));
        p.setPen(pre ? QColor(255, 255, 255) : kDimText);
        p.drawText(pp, Qt::AlignCenter, pre ? "Pre" : "Post");
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pos = e->position().toPoint();
        const uint64_t t = track_, b = bus_;
        if (prePost().contains(pos)) {
            AppState::get().edit("Send Pre/Post", [&](Project& p) {
                if (Send* s = findOrMake(p, t, b)) s->preFader = !s->preFader;
            });
            return;
        }
        if (!bar().adjusted(-3, -3, 3, 3).contains(pos)) return;
        const Send* s = find(AppState::get().project());
        const double cur = gainToPos(s ? s->level : 0.0);
        const int x = bar().left() + (int)std::lround(cur * bar().width());
        if (std::abs(pos.x() - x) <= 5) {
            drag_.press(cur, pos.x());
        } else {
            const double v = std::clamp((double)(pos.x() - bar().left()) / std::max(1, bar().width()), 0.0, 1.0);
            AppState::get().beginDrag();
            set(v, false);
            drag_.press(v, pos.x());
            drag_.started = true;
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!drag_.active) return;
        const Send* s = find(AppState::get().project());
        const bool ctrl = (e->modifiers() & Qt::ControlModifier) != 0;
        double v;
        if (drag_.move(e->position().x(), bar().width(), ctrl, gainToPos(s ? s->level : 0.0), v)) set(v, ctrl);
    }
    void mouseReleaseEvent(QMouseEvent*) override { drag_.release("Send Level"); }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (!bar().adjusted(-3, -3, 3, 3).contains(e->position().toPoint())) return;
        const uint64_t t = track_, b = bus_;
        AppState::get().edit("Send Level", [&](Project& p) {
            if (Send* s = findOrMake(p, t, b)) s->level = 1.0;
        });
    }

private:
    const Send* find(const Project& p) const
    {
        for (const Track& t : p.tracks)
            if (t.id == track_)
                for (const Send& s : t.sends)
                    if (s.bus == bus_) return &s;
        return nullptr;
    }
    static Send* findOrMake(Project& p, uint64_t track, uint64_t bus)
    {
        Track* t = p.findTrack(track);
        if (!t) return nullptr;
        for (Send& s : t->sends)
            if (s.bus == bus) return &s;
        Send s;
        s.bus = bus;
        // VEGAS: a send to a bus is taken before the track's volume, one to an assignable FX chain after it
        const reno::Bus* b = p.findBus(bus);
        s.preFader = !(b && b->assignable);
        t->sends.push_back(s);
        return &t->sends.back();
    }
    void set(double v, bool ctrl)
    {
        double level = posToGain(v);
        if (!ctrl && std::fabs(v - gainToPos(1.0)) < 0.015) level = 1.0;
        const uint64_t t = track_, b = bus_;
        AppState::get().dragUpdate([&](Project& p) {
            if (Send* s = findOrMake(p, t, b)) s->level = level;
        });
        AppState::get().status("Send: " + dbText(level, true).toStdString());
    }
    uint64_t track_, bus_;
    DragValue drag_;
};

// ------------------------------------------------------------------------------------------ the strip's name

class StripHeader : public QWidget {
public:
    explicit StripHeader(QWidget* parent) : QWidget(parent) { setFixedHeight(20); }
    // How the name is shown, as VEGAS's console: a track's number in a box of its colour, a bus's letter large, the
    // master's with its icon.
    enum Style { NumberBox, BusLetter, MasterIcon };
    Style style = NumberBox;
    QString number, name;
    QColor color;
    bool selected = false;
    std::function<void(Qt::KeyboardModifiers)> onClick;
    std::function<void()> onDoubleClick;
    // The fields above were set again: repaints only if they changed since the last paint.
    void fieldsSet()
    {
        if (!paintedOnce_ || number != paintedNumber_ || name != paintedName_ || color != paintedColor_ || selected != paintedSelected_) update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        paintedNumber_ = number;
        paintedName_ = name;
        paintedColor_ = color;
        paintedSelected_ = selected;
        paintedOnce_ = true;
        QFont nf = font();
        nf.setPointSizeF(8.0);
        const QFontMetrics nfm(nf);
        QString label = name;
        int x = 4;
        if (style == NumberBox) {
            QFont bf = nf;
            const int w = std::max(26, QFontMetrics(bf).horizontalAdvance(number) + 12);
            const QRect box(1, 1, w, height() - 2);
            p.fillRect(box, color.isValid() ? color.darker(118) : QColor(80, 80, 86));
            if (selected) p.fillRect(QRect(box.left() + 3, box.top() + 3, 4, 4), QColor(255, 255, 255));
            p.setFont(bf);
            p.setPen(QColor(255, 255, 255));
            p.drawText(box.adjusted(0, 0, -5, 0), Qt::AlignRight | Qt::AlignVCenter, number);
            x = box.right() + 6;
        } else if (style == BusLetter) {
            // "Bus A": the letter large, then "Bus"
            QString big = number;
            const int sp = name.lastIndexOf(' ');
            if (sp > 0 && name.size() - sp - 1 <= 2) {
                big = name.mid(sp + 1);
                label = name.left(sp);
            }
            if (!big.isEmpty()) {
                QFont lf = nf;
                lf.setBold(true);
                lf.setPointSizeF(11.0);
                p.setFont(lf);
                p.setPen(QColor(255, 255, 255));
                const int w = QFontMetrics(lf).horizontalAdvance(big);
                p.drawText(QRect(5, 0, w + 2, height()), Qt::AlignLeft | Qt::AlignVCenter, big);
                x = 5 + w + 8;
            }
        } else {
            // the master's icon: a square in a square
            const QRect icon(5, 4, 12, 12);
            p.setPen(QColor(235, 235, 238));
            p.drawRect(icon.adjusted(0, 0, -1, -1));
            p.drawRect(icon.adjusted(1, 1, -2, -2));
            p.fillRect(icon.adjusted(4, 4, -4, -4), QColor(235, 235, 238));
            x = icon.right() + 8;
        }
        p.setFont(nf);
        p.setPen(kText);
        p.drawText(QRect(x, 0, width() - x - 2, height()), Qt::AlignLeft | Qt::AlignVCenter, nfm.elidedText(label, Qt::ElideRight, width() - x - 2));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && onClick) onClick(e->modifiers());
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && onDoubleClick) onDoubleClick();
    }

private:
    QString paintedNumber_, paintedName_;
    QColor paintedColor_;
    bool paintedSelected_ = false, paintedOnce_ = false;
};

const char* kButtonStyle =
    "QToolButton { background: #3c3c40; border: 1px solid #1c1c1e; border-radius: 2px; color: #dcdcdc; padding: 1px 2px; }"
    "QToolButton:hover { background: #4a4a50; }"
    "QToolButton:pressed { background: #2c2c30; }"
    "QToolButton#mute:checked { background: #c8473b; color: white; border-color: #e06a5e; }"
    "QToolButton#solo:checked { background: #e2b437; color: #1a1a1a; border-color: #f5d26a; }"
    "QToolButton#arm:checked { background: #b02a2a; color: white; border-color: #e06a5e; }"
    "QToolButton#dimOutput:checked { background: #8a5a1a; color: white; border-color: #c88a3a; }"
    "QToolButton[region=\"true\"]:checked { background: #2d4f6e; border-color: #3d84c6; color: white; }"
    "QToolButton#fx[active=\"true\"] { background: #1f5585; color: white; border-color: #3d84c6; }"
    "QToolButton#masterMute:checked { background: #c8473b; color: white; border-color: #e06a5e; }"
    "QToolButton#masterFx[active=\"true\"] { background: #1f5585; color: white; border-color: #3d84c6; }"
    "QToolButton:disabled { color: #66666c; background: #333336; }"
    "QToolButton[tool=\"true\"] { padding: 2px 8px; }"
    "QToolButton[tool=\"true\"]:checked { background: #2d4f6e; border-color: #3d84c6; color: white; }";

// setToolTip sends the widget an event even when the text is the same; the strips set theirs on every project change.
void setToolTipIfChanged(QWidget* w, const QString& tip)
{
    if (w->toolTip() != tip) w->setToolTip(tip);
}

void setActive(QWidget* w, bool on)
{
    if (w->property("active").toBool() == on && w->property("active").isValid()) return;
    w->setProperty("active", on);
    w->style()->unpolish(w);
    w->style()->polish(w);
}

QString chainText(const FxChain& chain)
{
    if (chain.empty()) return QString("Insert FX");
    QString s = qs(chain[0].name.empty() ? std::string("Effect") : chain[0].name);
    if (chain.size() > 1) s += QString(" +%1").arg(chain.size() - 1);
    return s;
}

QString chainTip(const FxChain& chain, const QString& what)
{
    QString s = what;
    if (chain.empty()) return s + ": no effects (click to add)";
    for (const auto& f : chain) s += "\n  " + qs(f.name) + (f.bypass ? "  (bypassed)" : "");
    return s;
}

// Whether bus 'from' would end up feeding bus 'into' (so 'into' must not be sent back into 'from').
bool feeds(const Project& p, uint64_t from, uint64_t into)
{
    uint64_t cur = from;
    for (size_t step = 0; cur && step <= p.buses.size(); ++step) {
        if (cur == into) return true;
        const reno::Bus* b = p.findBus(cur);
        if (!b) return false;
        cur = b->output;
    }
    return false;
}

// ------------------------------------------------------------------------------------------ what the console shows
// VEGAS's Mixing Console view options: channel width (D / N / W), control regions (I Insert FX, S Sends, H I/O, M
// Meters, F Faders), region labels (Shift+L), fader ticks (Shift+T), holding peaks.
struct ConsoleView {
    enum Width { Narrow = 0, Default = 1, Wide = 2 };
    int width = Default;
    bool fx = true, sends = true, io = true, meters = true, faders = true;
    bool vu = false;
    bool labels = false, ticks = true, holdPeaks = true;
    int stripWidth() const { return width == Narrow ? 56 : width == Wide ? 110 : 76; }
};

// ------------------------------------------------------------------------------------------ insert FX slots
// The Insert FX region's list: a row per effect (the first few) with its bypass switch (lit: working; dark: bypassed)
// and name. Click a name to open that effect, the switch to bypass it; an empty row opens the chain (to add one).
const int kFxRows = 3, kFxRowH = 15;

class FxSlots : public QWidget {
public:
    explicit FxSlots(QWidget* parent) : QWidget(parent)
    {
        setObjectName("fxSlots");
        setFixedHeight(kFxRows * kFxRowH);
        setMouseTracking(true);
    }
    FxTarget target;
    std::function<void(uint64_t slotId)> onOpen;

    const FxChain* chain() const
    {
        const Project& p = AppState::get().project();
        if (target.kind == FxTarget::AudioOut) return &p.audioOut;
        if (target.kind == FxTarget::Bus) {
            const reno::Bus* b = p.findBus(target.id);
            return b ? &b->fx : nullptr;
        }
        for (const auto& t : p.tracks)
            if (t.id == target.id) return &t.fx;
        return nullptr;
    }
    QRect rowRect(int k) const { return QRect(0, k * kFxRowH, width(), kFxRowH - 1); }
    QRect switchRect(int k) const { return QRect(2, k * kFxRowH + 2, 10, kFxRowH - 5); }
    // The project changed: repaints only if the effects listed (names, bypass switches) changed.
    void projectChanged()
    {
        if (!paintedOnce_ || shownKey() != painted_) update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        painted_ = shownKey();
        paintedOnce_ = true;
        const FxChain* c = chain();
        const int n = c ? (int)c->size() : 0;
        QFont f = font();
        f.setPointSizeF(6.8);
        p.setFont(f);
        const QFontMetrics fm(f);
        for (int k = 0; k < kFxRows; ++k) {
            const QRect r = rowRect(k);
            p.fillRect(r, k < n ? QColor(40, 44, 50) : QColor(38, 38, 41));
            if (k >= n) continue;
            const bool more = k == kFxRows - 1 && n > kFxRows;
            const FxSlot& s = (*c)[(size_t)k];
            if (more) {
                p.setPen(kDimText);
                p.drawText(r.adjusted(4, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter, QString("+%1 more...").arg(n - kFxRows + 1));
                continue;
            }
            const QRect sw = switchRect(k);
            p.fillRect(sw, s.bypass ? QColor(70, 70, 74) : QColor(60, 190, 90));
            p.setPen(QColor(16, 16, 18));
            p.drawRect(sw.adjusted(0, 0, -1, -1));
            p.setPen(s.bypass ? QColor(236, 84, 72) : kText);
            const QString name = qs(s.name.empty() ? std::string("Effect") : s.name);
            p.drawText(r.adjusted(16, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(name, Qt::ElideRight, r.width() - 18));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const QPoint at = e->position().toPoint();
        const int k = at.y() / kFxRowH;
        const FxChain* c = chain();
        const int n = c ? (int)c->size() : 0;
        if (k < 0 || k >= kFxRows) return;
        const bool effectRow = k < n && !(k == kFxRows - 1 && n > kFxRows);
        if (e->button() == Qt::RightButton) {
            QMenu m(this);
            if (effectRow) effectMenu(&m, (*c)[(size_t)k]);
            else fillChainMenu(&m, target);
            exec(&m, e->globalPosition().toPoint());
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        if (!effectRow) {
            QMenu m(this);
            m.setObjectName("addInsertFxMenu");
            QAction* chainItem = m.addAction("Plug-In Chain...");
            QObject::connect(chainItem, &QAction::triggered, this, [this] { if (onOpen) onOpen(0); });
            m.addSeparator();
            const FxTarget t = target;
            fillAudioPluginMenu(&m, [t](const AudioPluginDesc& d) { AppState::get().addAudioFx(t, d); });
            exec(&m, e->globalPosition().toPoint());
            return;
        }
        const FxSlot& s = (*c)[(size_t)k];
        if (switchRect(k).adjusted(-2, -2, 2, 2).contains(at)) {
            AppState::get().setFxBypass(target, s.id, !s.bypass);
            update();
            return;
        }
        if (onOpen) onOpen(s.id);
    }

public:
    static std::function<QAction*(QMenu*)>& menuHook()
    {
        static std::function<QAction*(QMenu*)> h;
        return h;
    }
    static void exec(QMenu* m, const QPoint& at)
    {
        if (menuHook()) {
            if (QAction* a = menuHook()(m)) a->trigger();
            return;
        }
        m->exec(at);
    }

    // VEGAS's right-click menu of an insert effect's button: Show, Bypass and Remove <plug-in>, its Presets (current one
    // with a bullet) and the other plug-ins to put in its place.
    void effectMenu(QMenu* m, const FxSlot& slot)
    {
        AppState& app = AppState::get();
        const QString name = qs(slot.name.empty() ? std::string("Effect") : slot.name);
        const uint64_t id = slot.id;
        const FxTarget t = target;
        QAction* show = m->addAction("Show " + name);
        QObject::connect(show, &QAction::triggered, this, [this, id] { if (onOpen) onOpen(id); });
        QAction* by = m->addAction("Bypass " + name);
        by->setCheckable(true);
        by->setChecked(slot.bypass);
        QObject::connect(by, &QAction::triggered, this, [t, id](bool on) { AppState::get().setFxBypass(t, id, on); });
        QAction* rm = m->addAction("Remove " + name);
        QObject::connect(rm, &QAction::triggered, this, [t, id] { AppState::get().removeFx(t, id); });
        QMenu* presets = m->addMenu("Presets");
        AudioPlugin* pl = app.fx.audio(id);
        if (pl) {
            const int n = std::min(pl->programCount(), 512), cur = pl->currentProgram();
            for (int k = 0; k < n; ++k) {
                std::string pn = pl->programName(k);
                if (pn.empty()) pn = strfmt("Program %d", k + 1);
                QAction* a = presets->addAction((k == cur ? QString::fromUtf8("\xe2\x80\xa2 ") : QString("   ")) + qs(pn));
                QObject::connect(a, &QAction::triggered, this, [id, k, pn] {
                    AppState& ap = AppState::get();
                    if (AudioPlugin* x = ap.fx.audio(id)) ap.fxEditStep(id, "Preset " + pn, [&] { x->setProgram(k); });
                });
            }
            for (const AudioPreset& up : audioPresetsFor(slot.audio.key(), slot.audio.name)) {
                QAction* a = presets->addAction("   " + qs(up.name));
                QObject::connect(a, &QAction::triggered, this, [id, up] {
                    AppState& ap = AppState::get();
                    if (AudioPlugin* x = ap.fx.audio(id)) ap.fxEditStep(id, "Preset " + up.name, [&] { x->loadState(up.state); });
                });
            }
        }
        if (presets->isEmpty()) presets->addAction("(none)")->setEnabled(false);
        m->addSeparator();
        fillAudioPluginMenu(m, [t, id](const AudioPluginDesc& d) {
            AppState::get().edit("Replace Plug-In", [&](Project& p) {
                FxChain* c = AppState::get().chainOf(p, t);
                if (!c) return;
                for (FxSlot& x : *c)
                    if (x.id == id) {
                        FxSlot n;
                        n.id = p.newId();
                        n.kind = "audio";
                        n.audio = d;
                        n.name = d.name;
                        x = n;
                        break;
                    }
            });
        });
    }
private:
    // What the rows show: how many effects there are, and the first rows' names and bypass switches.
    struct ShownKey {
        size_t count = 0;
        std::vector<std::pair<std::string, bool>> rows;
        bool operator!=(const ShownKey& o) const { return count != o.count || rows != o.rows; }
    };
    ShownKey shownKey() const
    {
        ShownKey k;
        if (const FxChain* c = chain()) {
            k.count = c->size();
            for (size_t i = 0; i < c->size() && i < (size_t)kFxRows; ++i) k.rows.emplace_back((*c)[i].name, (*c)[i].bypass);
        }
        return k;
    }
    ShownKey painted_;
    bool paintedOnce_ = false;
};

// ------------------------------------------------------------------------------------------ strip buttons
// VEGAS's console draws a strip's switches as small round icons in a column beside the fader; a switch that is on gets
// a blue square behind it. (The buttons keep their text, Pre / Post and the like, for menus and tests; it is not drawn.)
class IconButton : public QToolButton {
public:
    enum Glyph { Mute, Solo, Fx, Phase, Arm, Monitor, PrePost, Lock };
    IconButton(Glyph glyph, QWidget* parent) : QToolButton(parent), glyph_(glyph)
    {
        setFixedSize(18, 18);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        // (Pre / Post and the master's lock show their state by the drawing itself)
        const bool on = isChecked() && glyph_ != PrePost && glyph_ != Lock;
        if (on) {
            p.setPen(QColor(18, 176, 255));
            p.setBrush(QColor(18, 176, 255, 110));
            p.drawRoundedRect(r, 3, 3);
        } else if (underMouse() && isEnabled()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 34));
            p.drawRoundedRect(r, 3, 3);
        }
        if (!isEnabled()) p.setOpacity(0.4);
        const QRectF c(2.5, 2.5, 13, 13);
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(6.8);
        auto ring = [&](const QColor& fill) {
            p.setPen(QPen(QColor(30, 22, 24), 1));
            p.setBrush(fill);
            p.drawEllipse(c);
        };
        auto letter = [&](const QString& t, const QColor& col) {
            p.setFont(f);
            p.setPen(col);
            p.drawText(c.adjusted(-1, 0, 0, 0), Qt::AlignCenter, t);
        };
        switch (glyph_) {
        case Mute: {
            ring(isChecked() ? QColor(240, 96, 110) : QColor(222, 128, 138));
            letter("M", QColor(70, 16, 26));
            // the small no-entry sign at the top right
            const QRectF s(10.5, 1.5, 6, 6);
            p.setPen(QPen(QColor(40, 30, 32), 1));
            p.setBrush(QColor(245, 245, 245));
            p.drawEllipse(s);
            p.drawLine(QPointF(s.left() + 1.2, s.bottom() - 1.2), QPointF(s.right() - 1.2, s.top() + 1.2));
            break;
        }
        case Solo:
            ring(isChecked() ? QColor(240, 200, 70) : QColor(196, 168, 92));
            letter("S", QColor(50, 36, 6));
            p.setPen(QPen(QColor(250, 250, 250), 1.6));
            p.drawLine(QPointF(15.2, 2.0), QPointF(15.2, 6.0));
            p.drawPoint(QPointF(15.2, 8.0));
            break;
        case Fx: {
            QFont xf("DejaVu Serif");
            xf.setItalic(true);
            xf.setPointSizeF(8.5);
            p.setFont(xf);
            p.setPen(QColor(190, 216, 244));
            p.drawText(QRectF(0, 0, 14, 17), Qt::AlignCenter, "fx");
            if (property("active").toBool()) {
                // effects in the chain: a small page at the bottom right
                const QRectF pg(10.5, 8.5, 6, 7);
                p.setPen(QPen(QColor(30, 30, 34), 1));
                p.setBrush(QColor(240, 240, 244));
                p.drawRect(pg);
                p.drawLine(QPointF(pg.left() + 1.5, pg.top() + 2.5), QPointF(pg.right() - 1.5, pg.top() + 2.5));
                p.drawLine(QPointF(pg.left() + 1.5, pg.top() + 4.5), QPointF(pg.right() - 1.5, pg.top() + 4.5));
            }
            break;
        }
        case Phase: {
            const QRectF b(1.5, 4.5, 15, 9);
            p.setPen(QPen(QColor(200, 226, 245), 1));
            p.setBrush(isChecked() ? QColor(70, 150, 220) : QColor(60, 104, 140));
            p.drawRoundedRect(b, 1.5, 1.5);
            QPainterPath w;
            w.moveTo(b.left() + 2, b.bottom() - 2);
            w.cubicTo(b.center().x() - 1, b.bottom() - 2, b.center().x() + 1, b.top() + 2, b.right() - 2, b.top() + 2);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(245, 245, 250), 1.4));
            p.drawPath(w);
            break;
        }
        case Arm: {
            const QColor body = isChecked() ? QColor(240, 60, 60) : QColor(226, 140, 150);
            p.setPen(QPen(QColor(40, 26, 30), 1));
            p.setBrush(body);
            p.drawRoundedRect(QRectF(6, 1.5, 6, 9), 3, 3);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(220, 220, 224), 1.2));
            p.drawArc(QRectF(4, 5, 10, 8), 180 * 16, 180 * 16);
            p.drawLine(QPointF(9, 13), QPointF(9, 15.5));
            p.drawLine(QPointF(6.5, 15.5), QPointF(11.5, 15.5));
            break;
        }
        case Monitor:
            p.setPen(QPen(QColor(180, 214, 240), 1.2));
            p.setBrush(isChecked() ? QColor(110, 190, 245) : QColor(44, 70, 92));
            p.drawRoundedRect(QRectF(2, 6, 14, 6), 3, 3);
            break;
        case PrePost: {
            // "fx" and a small fader, in the order the sound meets them (Post: the fader first)
            const bool post = isChecked();
            QFont xf("DejaVu Serif");
            xf.setItalic(true);
            xf.setPointSizeF(6.5);
            p.setFont(xf);
            p.setPen(QColor(190, 216, 244));
            const qreal fxX = post ? 6 : 0, faderX = post ? 3 : 14;
            p.drawText(QRectF(fxX, 0, 11, 17), Qt::AlignCenter, "fx");
            p.setPen(QPen(QColor(220, 220, 224), 1));
            p.drawLine(QPointF(faderX, 3), QPointF(faderX, 15));
            p.setBrush(QColor(240, 240, 244));
            p.setPen(QPen(QColor(30, 30, 34), 1));
            p.drawRect(QRectF(faderX - 2, 6.5, 4, 5));
            break;
        }
        case Lock: {
            // linked (closed) or split (open)
            const bool open = isChecked();
            p.setPen(QPen(QColor(225, 225, 230), 1.4));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(open ? 8 : 5.5, 2.5, 7, 9), 0, 180 * 16);
            if (!open) {
                p.drawLine(QPointF(5.5, 7), QPointF(5.5, 9));
                p.drawLine(QPointF(12.5, 7), QPointF(12.5, 9));
            } else {
                p.drawLine(QPointF(15, 7), QPointF(15, 9));
            }
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(225, 225, 230));
            p.drawRoundedRect(QRectF(4, 8.5, 10, 7), 1, 1);
            break;
        }
        }
    }

private:
    Glyph glyph_;
};

// The automation row under the I/O: VEGAS's Automation Settings button (a cog, lit while the controls follow their
// envelopes) with the automation mode written beside it and a menu arrow.
class AutomationButton : public QToolButton {
public:
    explicit AutomationButton(QWidget* parent) : QToolButton(parent)
    {
        setFixedHeight(20);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
    }
    QString mode;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) p.fillRect(rect(), QColor(255, 255, 255, 22));
        // the cog: a ring of teeth around a disc, the mode's first letter in it
        const QPointF c(10, height() / 2.0);
        const QColor tooth = isChecked() ? QColor(236, 150, 160) : QColor(170, 170, 176);
        p.setPen(Qt::NoPen);
        p.setBrush(tooth);
        for (int k = 0; k < 8; ++k) {
            const double a = k * M_PI / 4.0;
            p.drawEllipse(QPointF(c.x() + 6.2 * std::cos(a), c.y() + 6.2 * std::sin(a)), 1.6, 1.6);
        }
        p.setBrush(isChecked() ? QColor(250, 236, 238) : QColor(210, 210, 214));
        p.setPen(QPen(QColor(40, 30, 34), 1));
        p.drawEllipse(c, 5.2, 5.2);
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(6.2);
        p.setFont(f);
        p.setPen(isChecked() ? QColor(170, 30, 50) : QColor(60, 60, 66));
        p.drawText(QRectF(c.x() - 5, c.y() - 5, 10, 10), Qt::AlignCenter, mode.left(1));
        p.setRenderHint(QPainter::Antialiasing, false);
        QFont tf = font();
        tf.setPointSizeF(8.0);
        p.setFont(tf);
        p.setPen(kText);
        const QRect text(20, 0, width() - 34, height());
        p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(tf).elidedText(mode, Qt::ElideRight, text.width()));
        // the menu arrow
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 220, 224));
        const qreal ax = width() - 9, ay = height() / 2.0 - 1;
        const QPointF arrow[3] = {QPointF(ax - 3.5, ay - 1.5), QPointF(ax + 3.5, ay - 1.5), QPointF(ax, ay + 2.5)};
        p.drawPolygon(arrow, 3);
    }
};

// A strip's output choice drawn as VEGAS's: just the name, centred (the list opens on a click).
class OutputCombo : public QComboBox {
public:
    explicit OutputCombo(QWidget* parent) : QComboBox(parent) { setAttribute(Qt::WA_Hover, true); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        if (underMouse()) p.fillRect(rect(), QColor(255, 255, 255, 22));
        p.setPen(kText);
        p.setFont(font());
        p.drawText(rect().adjusted(2, 0, -2, 0), Qt::AlignCenter, QFontMetrics(font()).elidedText(currentText(), Qt::ElideRight, width() - 4));
    }
};

// A line of text in a strip (an input device, the master's sound device): centred, no frame.
class StripText : public QToolButton {
public:
    explicit StripText(QWidget* parent) : QToolButton(parent) { setAttribute(Qt::WA_Hover, true); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        if (underMouse() && menu()) p.fillRect(rect(), QColor(255, 255, 255, 22));
        p.setPen(kText);
        p.setFont(font());
        p.drawText(rect().adjusted(2, 0, -2, 0), Qt::AlignCenter, QFontMetrics(font()).elidedText(text(), Qt::ElideRight, width() - 4));
    }
};

// ------------------------------------------------------------------------------------------ a strip
class Strip : public QWidget {
public:
    enum Kind { TrackStrip, BusStrip, MasterStrip, InputStrip, PreviewStrip };

    Strip(Kind kind, uint64_t id, Playback* pb, QWidget* parent) : QWidget(parent), kind_(kind), id_(id), pb_(pb)
    {
        setObjectName(kind == TrackStrip ? QString("strip:track:%1").arg(id) : kind == BusStrip ? QString("strip:bus:%1").arg(id)
                      : kind == InputStrip ? QString("strip:input:%1").arg(id) : kind == PreviewStrip ? QString("strip:preview") : QString("strip:master"));
        setFixedWidth(ConsoleView().stripWidth());
        setAttribute(Qt::WA_StyledBackground, false);
        // As VEGAS's console, from the top: the name, the regions shown (Insert FX, Sends, I/O, VU meters), the faders
        // region (automation, pan, then the switches in a column beside the fader and meter) and the name again in a
        // box at the bottom.
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(1, 0, 2, 2);
        v->setSpacing(0);
        header = new StripHeader(this);
        header->setObjectName("header");
        header->style = kind == BusStrip ? StripHeader::BusLetter : kind == MasterStrip || kind == PreviewStrip ? StripHeader::MasterIcon : StripHeader::NumberBox;
        v->addWidget(header);
        auto line = [&] {
            QWidget* l = new QWidget(this);
            l->setFixedHeight(2);
            l->setAutoFillBackground(true);
            QPalette lp = l->palette();
            lp.setColor(QPalette::Window, kStripEdge);
            l->setPalette(lp);
            v->addWidget(l);
            return l;
        };
        line();

        QFont small = font();
        small.setPointSizeF(8.0);
        auto region = [&](const char* name, const QString& label) {
            QLabel* l = new QLabel(label, this);
            l->setObjectName(QString("label:%1").arg(name));
            QFont lf = font();
            lf.setPointSizeF(6.5);
            l->setFont(lf);
            l->setStyleSheet("QLabel { color: #8a8a90; padding-left: 3px; }");
            l->setFixedHeight(11);
            l->hide();
            v->addWidget(l);
            labels.push_back(l);
            QWidget* box = new QWidget(this);
            box->setObjectName(QString("region:%1").arg(name));
            auto* bl = new QVBoxLayout(box);
            bl->setContentsMargins(2, 1, 1, 1);
            bl->setSpacing(1);
            v->addWidget(box);
            return box;
        };

        fxBox = region("fx", "Insert FX");
        fxSlots = new FxSlots(fxBox);
        fxSlots->target.kind = kind == TrackStrip ? FxTarget::Track : kind == BusStrip ? FxTarget::Bus : kind == InputStrip ? FxTarget::InputBus : FxTarget::AudioOut;
        fxSlots->target.id = kind == MasterStrip || kind == PreviewStrip ? 0 : id;
        fxBox->layout()->addWidget(fxSlots);

        sendBox = region("sends", "Sends");
        sends = new QWidget(sendBox);
        sends->setObjectName("sends");
        sendLayout = new QVBoxLayout(sends);
        sendLayout->setContentsMargins(0, 0, 0, 0);
        sendLayout->setSpacing(2);
        sendBox->layout()->addWidget(sends);

        // I/O: where the sound comes from (a track's Record Input, an input bus's input; blank for a bus or the master)
        // and where it goes (the master: its sound device)
        ioBox = region("io", "I/O");
        static_cast<QVBoxLayout*>(ioBox->layout())->setSpacing(0);
        if (kind == InputStrip || kind == TrackStrip) {
            source = new StripText(ioBox);
            source->setObjectName("inputSource");
            source->setFont(small);
            source->setFixedHeight(20);
            source->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            source->setPopupMode(QToolButton::InstantPopup);
            source->setToolTip(kind == InputStrip ? "Input: the sound card input this input bus takes its sound from"
                                                   : "Record Input: the sound card input this track records from");
            source->setMenu(new QMenu(source));
            ioBox->layout()->addWidget(source);
        } else {
            QWidget* blank = new QWidget(ioBox);
            blank->setFixedHeight(20);
            ioBox->layout()->addWidget(blank);
        }
        if (kind == MasterStrip) {
            device = new QLabel(ioBox);
            device->setObjectName("device");
            device->setFont(small);
            device->setFixedHeight(20);
            device->setAlignment(Qt::AlignCenter);
            device->setStyleSheet("QLabel { background: transparent; border: none; color: #dedede; }");
            const QString dev = pb ? qs(pb->audioDeviceName()) : QString();
            device->setToolTip(QString("The master goes to: %1").arg(dev.isEmpty() ? QString("the sound device") : dev));
            ioBox->layout()->addWidget(device);
        } else {
            output = new OutputCombo(ioBox);
            output->setObjectName("output");
            output->setFont(small);
            output->setFixedHeight(20);
            output->setToolTip("Where this strip's sound goes");
            output->setFocusPolicy(Qt::NoFocus);
            ioBox->layout()->addWidget(output);
        }
        ioLine = line();

        vuBox = region("vu", "VU Meters");
        vu = new VuMeter(vuBox);
        vuBox->layout()->addWidget(vu);

        faderBox = region("faders", "Faders");
        // the automation row: Automation Settings and the mode (Off, Read, Touch, Latch), as the track header's button;
        // kept as a blank row where there is none (the master, input buses, the Preview bus) so the faders line up
        if (kind == TrackStrip || kind == BusStrip) {
            automation = new AutomationButton(faderBox);
            automation->setObjectName("automation");
            automation->setText("A");
            automation->setCheckable(true);
            automation->setPopupMode(QToolButton::InstantPopup);
            automation->setToolTip("Automation Settings: fader, pan and mute follow and write their envelopes");
            faderBox->layout()->addWidget(automation);
        } else {
            QWidget* blank = new QWidget(faderBox);
            blank->setFixedHeight(20);
            faderBox->layout()->addWidget(blank);
        }
        pan = new PanSlider(faderBox);
        pan->setObjectName("pan");
        faderBox->layout()->addWidget(pan);
        // (a 5.1 project: the small surround panner instead, for whatever goes to the master; double-click opens the window)
        surround = new SurroundField(true, faderBox);
        surround->setToolTip("Surround pan: drag the point; double-click for the Surround Panner window (Ctrl+Alt+3)");
        static_cast<QVBoxLayout*>(faderBox->layout())->addWidget(surround, 0, Qt::AlignHCenter);
        surround->hide();
        // (the Preview bus has no pan: its row stays, empty, so its fader lines up with the others)
        panSpace = new QWidget(faderBox);
        panSpace->setFixedHeight(34);
        panSpace->hide();
        faderBox->layout()->addWidget(panSpace);

        // the switches, in a column beside the fader
        switches = new QWidget(this);
        switches->setObjectName("switches");
        switches->setFixedWidth(19);
        auto* sw = new QVBoxLayout(switches);
        sw->setContentsMargins(0, 2, 1, 0);
        sw->setSpacing(4);
        mute = new IconButton(IconButton::Mute, switches);
        mute->setObjectName("mute");
        mute->setText("M");
        mute->setCheckable(true);
        mute->setToolTip("Mute");
        sw->addWidget(mute);
        solo = new IconButton(IconButton::Solo, switches);
        solo->setObjectName("solo");
        solo->setText("S");
        solo->setCheckable(true);
        solo->setToolTip("Solo");
        sw->addWidget(solo);
        if (kind == MasterStrip || kind == PreviewStrip) {
            // the master has no solo; the space is kept so the switches line up
            QSizePolicy sp = solo->sizePolicy();
            sp.setRetainSizeWhenHidden(true);
            solo->setSizePolicy(sp);
            solo->hide();
        }
        // the effects chain (the page on it: there are effects); a click opens it (the Preview bus has none)
        fx = new IconButton(IconButton::Fx, switches);
        fx->setObjectName("fx");
        sw->addWidget(fx);
        if (kind == PreviewStrip) fx->hide();
        if (kind == TrackStrip) {
            phase = new IconButton(IconButton::Phase, switches);
            phase->setObjectName("invertPhase");
            phase->setText(QString::fromUtf8("\xc3\x98"));
            phase->setCheckable(true);
            phase->setToolTip("Invert Track Phase (also applies to the selected channels)");
            sw->addWidget(phase);
            arm = new IconButton(IconButton::Arm, switches);
            arm->setObjectName("arm");
            arm->setText(QString::fromUtf8("\xe2\x97\x8f"));
            arm->setCheckable(true);
            arm->setToolTip("Arm for Record");
            sw->addWidget(arm);
            monitor = new IconButton(IconButton::Monitor, switches);
            monitor->setObjectName("inputMonitor");
            monitor->setCheckable(true);
            monitor->setPopupMode(QToolButton::InstantPopup);
            monitor->setMenu(new QMenu(monitor));
            monitor->setToolTip("Input Monitoring: hear the record input while armed (Off, On, Auto)");
            sw->addWidget(monitor);
        } else if (kind == BusStrip || kind == MasterStrip) {
            // VEGAS's Pre/Post Fader Insert FX: the bus's (master's) effects before or after its fader
            postFx = new IconButton(IconButton::PrePost, switches);
            postFx->setObjectName("prePostFx");
            postFx->setCheckable(true);
            postFx->setToolTip("Pre/Post Fader Insert FX: effects before the fader (Pre) or after it (Post)");
            sw->addWidget(postFx);
        }
        sw->addStretch(1);
        if (kind == MasterStrip) {
            // the master's left and right faders: linked (the lock) or split
            link = new IconButton(IconButton::Lock, switches);
            link->setObjectName("masterSplit");
            link->setCheckable(true);
            link->setText("L/R");
            link->setToolTip("Split the master fader into a left and a right fader (locked: they move together)");
            sw->addWidget(link);
            sw->addSpacing(22);
        }
        fx->setFont(small);

        auto* lower = new QHBoxLayout();
        lower->setContentsMargins(3, 0, 0, 0);
        lower->setSpacing(1);
        lower->addWidget(switches);
        fader = new FaderMeter(false, this);
        fader->setObjectName("fader");
        fader->twin = kind == MasterStrip;
        lower->addWidget(fader, 1);
        v->addLayout(lower, 1);
        filler = new QWidget(this);
        filler->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        filler->hide();
        v->addWidget(filler, 1);

        // the name again, in a box at the bottom (a bus's or the master's; a track's own name, blank until it has one)
        nameBox = new QLabel(this);
        nameBox->setObjectName("nameBox");
        nameBox->setFont(small);
        nameBox->setFixedHeight(18);
        nameBox->setStyleSheet("QLabel { background: #232326; border: 1px solid #141416; color: #dedede; padding-left: 3px; }");
        v->addSpacing(2);
        v->addWidget(nameBox);

        wire();
    }

    Kind kind() const { return kind_; }
    uint64_t id() const { return id_; }
    QString key() const { return objectName(); }

    StripHeader* header = nullptr;
    FxSlots* fxSlots = nullptr;
    QToolButton *fx = nullptr, *mute = nullptr, *solo = nullptr, *arm = nullptr, *automation = nullptr, *phase = nullptr, *postFx = nullptr;
    QToolButton *source = nullptr, *link = nullptr, *monitor = nullptr;
    QComboBox* output = nullptr;
    QLabel *device = nullptr, *nameBox = nullptr;
    QWidget *fxBox = nullptr, *sendBox = nullptr, *ioBox = nullptr, *faderBox = nullptr, *filler = nullptr, *vuBox = nullptr;
    QWidget *switches = nullptr, *panSpace = nullptr, *ioLine = nullptr;
    VuMeter* vu = nullptr;
    std::vector<QLabel*> labels;
    QWidget* sends = nullptr;
    QVBoxLayout* sendLayout = nullptr;
    PanSlider* pan = nullptr;
    SurroundField* surround = nullptr;
    std::function<void(uint64_t)> onOpenSurround;
    FaderMeter* fader = nullptr;
    std::function<void(const FxTarget&, uint64_t slotId)> onFx;
    std::function<void(Strip*, Qt::KeyboardModifiers)> onSelect;
    std::function<void(Strip*)> onRename;
    std::function<void(uint64_t)> onArm;
    std::function<std::vector<uint64_t>()> onSelectedTracks;

    static void setPostFx(Kind kind, uint64_t id, bool post)
    {
        AppState::get().edit(post ? "Post Fader Insert FX" : "Pre Fader Insert FX", [&](Project& p) {
            if (kind == MasterStrip) p.masterFxPostFader = post;
            else if (reno::Bus* b = p.findBus(id)) b->fxPostFader = post;
        });
    }

    void applyView(const ConsoleView& view)
    {
        const int w = view.stripWidth();
        setFixedWidth(w);
        fxBox->setVisible(view.fx);
        sendBox->setVisible(view.sends);
        ioBox->setVisible(view.io);
        ioLine->setVisible(view.io);
        vuBox->setVisible(view.vu);
        faderBox->setVisible(view.faders);
        switches->setVisible(view.faders);
        const bool shown[5] = {view.fx, view.sends, view.io, view.vu, view.faders};
        for (size_t k = 0; k < labels.size() && k < 5; ++k) labels[k]->setVisible(view.labels && shown[k]);
        fader->showFader = view.faders;
        fader->showMeter = view.meters;
        fader->ticks = view.ticks;
        fader->meterScale = view.width != ConsoleView::Narrow;
        fader->holdPeaks = view.holdPeaks;
        fader->setVisible(view.faders || view.meters);
        filler->setVisible(!(view.faders || view.meters));
        if (arm) arm->setVisible(view.width != ConsoleView::Narrow);
        if (device) {
            const QString dev = pb_ ? qs(pb_->audioDeviceName()) : QString();
            device->setText(QFontMetrics(device->font()).elidedText(dev.isEmpty() ? QString("Output") : dev, Qt::ElideRight, w - 14));
        }
        if (kind_ == PreviewStrip) {
            // the Preview bus has no effects, sends, output or pan: it is only a fader and meters (their rows stay,
            // empty, so the fader lines up with the others)
            for (QWidget* w : {(QWidget*)fxSlots, (QWidget*)output}) {
                QSizePolicy sp = w->sizePolicy();
                sp.setRetainSizeWhenHidden(true);
                w->setSizePolicy(sp);
                w->hide();
            }
        }
        fader->update();
    }

    void makeSends(const Project& p)
    {
        const int nb = (int)p.buses.size();
        if (kind_ == TrackStrip)
            for (const auto& b : p.buses) sendLayout->addWidget(new SendRow(id_, b.id, sends));
        sends->setFixedHeight(nb > 0 ? nb * kSendH + (nb - 1) * 2 : 0);
        sends->setVisible(nb > 0);
    }

    bool assignable(const Project& p) const
    {
        const reno::Bus* b = kind_ == BusStrip ? p.findBus(id_) : nullptr;
        return b && b->assignable;
    }

    QString name(const Project& p) const
    {
        if (kind_ == TrackStrip) {
            for (const auto& t : p.tracks)
                if (t.id == id_) return t.name.empty() ? QString("Audio") : qs(t.name);
            return QString();
        }
        if (kind_ == BusStrip) {
            const reno::Bus* b = p.findBus(id_);
            return b ? qs(b->name) : QString();
        }
        if (kind_ == InputStrip) {
            const reno::InputBus* b = p.findInputBus(id_);
            return b ? qs(b->name) : QString();
        }
        if (kind_ == PreviewStrip) return "Preview";
        return "Master";
    }

    void refresh(const Project& p, int trackIndex)
    {
        const FxChain* chain = nullptr;
        bool muted = false, soloed = false;
        if (kind_ == TrackStrip) {
            const Track* t = nullptr;
            for (const auto& x : p.tracks)
                if (x.id == id_) t = &x;
            if (!t) return;
            header->number = QString::number(trackIndex + 1);
            header->color = trackColor(*t, trackIndex);
            chain = &t->fx;
            muted = t->mute;
            soloed = t->solo;
            fillOutputs(p, t->output, 0);
            source->setText(QFontMetrics(source->font()).elidedText(recordInputText(p, t->recordInput), Qt::ElideRight, width() - 18));
            setToolTipIfChanged(source, "Record Input: " + recordInputText(p, t->recordInput) + (t->recordInput.monitor != "off" ? QString(" (monitoring %1)").arg(qs(t->recordInput.monitor)) : QString()));
            if (phase) {
                const QSignalBlocker pb(phase);
                phase->setChecked(t->invertPhase);
            }
            if (monitor) {
                const QSignalBlocker mb(monitor);
                monitor->setChecked(t->recordInput.monitor != "off");
                setToolTipIfChanged(monitor, QString("Input Monitoring: %1").arg(t->recordInput.monitor == "on" ? "On" : t->recordInput.monitor == "auto" ? "Auto" : "Off"));
            }
        } else if (kind_ == PreviewStrip) {
            static const FxChain none;
            header->number.clear();
            header->color = kMasterColor;
            chain = &none;
            muted = previewBus().mute.load();
        } else if (kind_ == InputStrip) {
            const reno::InputBus* b = p.findInputBus(id_);
            if (!b) return;
            header->number = "IN";
            header->color = kInputColor;
            chain = &b->fx;
            muted = b->mute;
            soloed = b->solo;
            fillOutputs(p, b->output, 0, b->outputOff);
            RecordInput ri;
            ri.device = b->device;
            ri.first = b->first;
            ri.channels = b->channels;
            source->setText(QFontMetrics(source->font()).elidedText(recordInputText(p, ri), Qt::ElideRight, width() - 18));
            setToolTipIfChanged(source, "Input: " + recordInputText(p, ri));
        } else if (kind_ == BusStrip) {
            const reno::Bus* b = p.findBus(id_);
            if (!b) return;
            header->number = b->assignable ? QString("FX") : QString();
            header->color = b->assignable ? kFxSendColor : kBusColor;
            chain = &b->fx;
            muted = b->mute;
            soloed = b->solo;
            fillOutputs(p, b->output, id_);
            if (postFx) {
                const QSignalBlocker pb(postFx);
                postFx->setChecked(b->fxPostFader);
                postFx->setText(b->fxPostFader ? "Post" : "Pre");
            }
        } else {
            header->number.clear();
            header->color = kMasterColor;
            chain = &p.audioOut;
            muted = p.masterMute;
            if (link) {
                const QSignalBlocker lb(link);
                link->setChecked(p.masterVolumeR >= 0);
                fader->splitOn = p.masterVolumeR >= 0;
                fader->update();
            }
            if (postFx) {
                const QSignalBlocker pb(postFx);
                postFx->setChecked(p.masterFxPostFader);
                postFx->setText(p.masterFxPostFader ? "Post" : "Pre");
            }
        }
        {
            // 5.1: a track or bus going to the master is placed by its surround panner (one going to a bus keeps its
            // stereo pan: buses are stereo); the master has no pan
            uint64_t out = 0;
            if (kind_ == TrackStrip) {
                for (const auto& t : p.tracks)
                    if (t.id == id_) out = t.output;
            } else if (kind_ == BusStrip) {
                if (const reno::Bus* b = p.findBus(id_)) out = b->output;
            } else if (kind_ == InputStrip) {
                if (const reno::InputBus* b = p.findInputBus(id_)) out = b->outputOff ? 1 : b->output;
            }
            const bool sur = p.settings.surround();
            surround->setVisible(sur && kind_ != MasterStrip && kind_ != PreviewStrip && out == 0);
            pan->setVisible(kind_ != PreviewStrip && (!sur || (kind_ != MasterStrip && out != 0)));
            panSpace->setVisible(pan->isHidden() && surround->isHidden());
            surround->update();
        }
        header->name = name(p);
        setToolTipIfChanged(header, header->name);
        header->fieldsSet();
        if (header->selected != paintedSelected_) update();
        {
            // the box at the bottom: the channel's own name (a track's is blank until it is given one)
            QString own = header->name;
            if (kind_ == TrackStrip)
                for (const auto& t : p.tracks)
                    if (t.id == id_) own = qs(t.name);
            const QString shown = QFontMetrics(nameBox->font()).elidedText(own, Qt::ElideRight, width() - 12);
            if (nameBox->text() != shown) nameBox->setText(shown);
        }
        if (fader->muted != muted) {
            fader->muted = muted;
            fader->update();
        }
        fx->setText(QFontMetrics(fx->font()).elidedText(chainText(*chain), Qt::ElideRight, width() - 18));
        setToolTipIfChanged(fx, chainTip(*chain, kind_ == MasterStrip ? QString("Master Bus FX") : kind_ == BusStrip ? QString(assignable(p) ? "Assignable FX" : "Bus FX") : kind_ == InputStrip ? QString("Input Bus FX") : QString("Track FX")) +
                       "\n(right-click: Enable All, Bypass All, Delete All" + (kind_ == TrackStrip || kind_ == InputStrip ? QString(")") : QString(", Post Volume)")));
        setActive(fx, !chain->empty());
        fxSlots->projectChanged();
        {
            const QSignalBlocker b1(mute), b2(solo);
            mute->setChecked(muted);
            solo->setChecked(soloed);
        }
        if (arm) {
            const QSignalBlocker b3(arm);
            arm->setChecked(AppState::get().armedTracks.count(id_) != 0);
        }
        if (automation) {
            const EnvelopeOwner o{kind_ == TrackStrip ? EnvelopeOwner::Track : EnvelopeOwner::Bus, id_};
            const QSignalBlocker b4(automation);
            automation->setChecked(automationControlsOn(p, o));
            // (VEGAS writes the mode short: Write (Touch) is Touch)
            QString mode = automationModeName(automationModeOf(p, o));
            const int open = mode.indexOf('('), close = mode.lastIndexOf(')');
            if (open >= 0 && close > open) mode = mode.mid(open + 1, close - open - 1);
            auto* ab = static_cast<AutomationButton*>(automation);
            if (ab->mode != mode) {
                ab->mode = mode;
                ab->update();
            }
            setToolTipIfChanged(automation, QString("Automation Settings (%1)").arg(automationModeName(automationModeOf(p, o))));
        }
        pan->projectChanged();
        fader->projectChanged();
        for (QWidget* s : sends->findChildren<QWidget*>()) s->update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        // a selected channel: the whole strip blue, as VEGAS
        paintedSelected_ = header->selected;
        p.fillRect(rect(), header->selected ? kStripSelected : kStripBg);
        p.fillRect(QRect(width() - 1, 0, 1, height()), kStripEdge);
        p.fillRect(QRect(0, 0, 1, height()), header->selected ? kStripSelected.lighter(115) : QColor(64, 64, 69));
    }

private:
    void fillOutputs(const Project& p, uint64_t current, uint64_t self, bool off = false)
    {
        if (!output) return;
        const QSignalBlocker block(output);
        std::vector<std::pair<QString, uint64_t>> items;
        items.push_back(std::make_pair(QString("Master"), (uint64_t)0));
        for (const auto& b : p.buses) {
            if (b.id == self || (self && feeds(p, b.id, self))) continue;
            if (b.assignable) continue;
            items.push_back(std::make_pair(qs(b.name), b.id));
        }
        if (kind_ == InputStrip) items.push_back(std::make_pair(QString("Off"), kOutputOff));
        // refilled only when the choices change, so an open menu is not disturbed by a fader moving elsewhere
        bool same = output->count() == (int)items.size();
        for (int i = 0; same && i < output->count(); ++i)
            same = output->itemText(i) == items[(size_t)i].first && output->itemData(i).toULongLong() == items[(size_t)i].second;
        if (!same) {
            output->clear();
            for (const auto& it : items) output->addItem(it.first, QVariant((qulonglong)it.second));
        }
        int at = 0;
        for (int i = 0; i < output->count(); ++i)
            if (output->itemData(i).toULongLong() == (off ? kOutputOff : current)) at = i;
        output->setCurrentIndex(at);
    }

    void wire()
    {
        const uint64_t id = id_;
        const Kind kind = kind_;
        header->onClick = [this](Qt::KeyboardModifiers m) { if (onSelect) onSelect(this, m); };
        header->onDoubleClick = [this] { if (onRename) onRename(this); };
        fxSlots->onOpen = [this](uint64_t slot) { if (onFx) onFx(fxSlots->target, slot); };
        QObject::connect(fx, &QToolButton::clicked, [this] { if (onFx) onFx(fxSlots->target, 0); });
        // the FX button's right-click menu: Enable / Bypass / Delete All, and for a bus or the master VEGAS's Post Volume
        fx->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(fx, &QWidget::customContextMenuRequested, this, [this, id, kind](const QPoint& at) {
            QMenu m(this);
            m.setObjectName("fxButtonMenu");
            fillChainMenu(&m, fxSlots->target);
            if (kind != TrackStrip && kind != InputStrip) {
                m.addSeparator();
                QAction* post = m.addAction("Post Volume");
                post->setObjectName("postVolume");
                post->setCheckable(true);
                post->setChecked(postFx && postFx->isChecked());
                QObject::connect(post, &QAction::triggered, this, [id, kind](bool on) { setPostFx(kind, id, on); });
            }
            FxSlots::exec(&m, fx->mapToGlobal(at));
        });
        if (postFx) QObject::connect(postFx, &QToolButton::clicked, [id, kind](bool on) { setPostFx(kind, id, on); });
        if (phase)
            QObject::connect(phase, &QToolButton::clicked, [this, id](bool on) {
                std::set<uint64_t> ids = {id};
                if (onSelectedTracks) for (uint64_t x : onSelectedTracks()) ids.insert(x);
                AppState::get().edit("Invert Track Phase", [&](Project& p) {
                    for (uint64_t x : ids)
                        if (Track* t = p.findTrack(x)) t->invertPhase = on;
                });
            });
        if (arm)
            QObject::connect(arm, &QToolButton::clicked, [this, id] {
                if (onArm) onArm(id);
                else {
                    AppState& app = AppState::get();
                    if (app.armedTracks.count(id)) app.armedTracks.erase(id);
                    else app.armedTracks.insert(id);
                }
                const QSignalBlocker b(arm);
                arm->setChecked(AppState::get().armedTracks.count(id) != 0);
            });
        if (output)
            QObject::connect(output, QOverload<int>::of(&QComboBox::activated), [this, id, kind](int index) {
                const uint64_t to = output->itemData(index).toULongLong();
                AppState::get().edit(kind == TrackStrip ? "Track Output" : kind == InputStrip ? "Input Bus Output" : "Bus Output", [&](Project& p) {
                    if (kind == TrackStrip) {
                        if (Track* t = p.findTrack(id)) t->output = to;
                    } else if (kind == InputStrip) {
                        if (reno::InputBus* b = p.findInputBus(id)) {
                            b->outputOff = to == kOutputOff;
                            if (!b->outputOff) b->output = to;
                        }
                    } else if (reno::Bus* b = p.findBus(id)) {
                        b->output = to;
                    }
                });
            });
        QObject::connect(mute, &QToolButton::clicked, [this, id, kind] {
            if (kind == PreviewStrip) {
                previewBus().mute.store(!previewBus().mute.load());
                const QSignalBlocker b(mute);
                mute->setChecked(previewBus().mute.load());
                return;
            }
            AppState::get().edit(kind == TrackStrip ? "Mute Track" : kind == BusStrip ? "Mute Bus" : kind == InputStrip ? "Mute Input Bus" : "Mute Master", [&](Project& p) {
                if (kind == TrackStrip) {
                    const EnvelopeOwner o{EnvelopeOwner::Track, id};
                    if (Track* t = p.findTrack(id)) {
                        const bool now = controlShown(p, o, "mute", 0, t->mute ? 1.0 : 0.0, automationNow()) >= 0.5;
                        if (!controlMoved(p, o, "mute", 0, now ? 0.0 : 1.0)) t->mute = !t->mute;
                    }
                } else if (kind == BusStrip) {
                    if (reno::Bus* b = p.findBus(id)) b->mute = !b->mute;
                } else if (kind == InputStrip) {
                    if (reno::InputBus* b = p.findInputBus(id)) b->mute = !b->mute;
                } else {
                    p.masterMute = !p.masterMute;
                }
            });
        });
        if (automation) {
            auto* menu = new QMenu(automation);
            automation->setMenu(menu);
            QObject::connect(menu, &QMenu::aboutToShow, [menu, id, kind] {
                menu->clear();
                const EnvelopeOwner o{kind == TrackStrip ? EnvelopeOwner::Track : EnvelopeOwner::Bus, id};
                const bool on = automationControlsOn(AppState::get().project(), o);
                const std::string mode = automationModeOf(AppState::get().project(), o);
                QAction* st = menu->addAction("Automation Settings");
                st->setCheckable(true);
                st->setChecked(on);
                QObject::connect(st, &QAction::triggered, menu, [o, on] { setAutomationControls(AppState::get(), {o}, !on); });
                menu->addSeparator();
                for (const AutomationModeName& m : kAutomationModes) {
                    QAction* a = menu->addAction(m.name);
                    a->setCheckable(true);
                    a->setChecked(mode == m.key);
                    const std::string key = m.key;
                    QObject::connect(a, &QAction::triggered, menu, [o, key] { setAutomationMode(AppState::get(), {o}, key); });
                }
            });
        }
        QObject::connect(solo, &QToolButton::clicked, [id, kind] {
            AppState::get().edit(kind == TrackStrip ? "Solo Track" : kind == InputStrip ? "Solo Input Bus" : "Solo Bus", [&](Project& p) {
                if (kind == TrackStrip) {
                    if (Track* t = p.findTrack(id)) t->solo = !t->solo;
                } else if (kind == InputStrip) {
                    if (reno::InputBus* b = p.findInputBus(id)) b->solo = !b->solo;
                } else if (reno::Bus* b = p.findBus(id)) {
                    b->solo = !b->solo;
                }
            });
        });
        // pan and fader read and write the project directly, so they always show what it holds
        if (kind != MasterStrip) {
            surround->get = [id]() { return surroundPanOf(AppState::get().project(), id); };
            surround->set = [id](Project& p, const SurroundPan& sp) {
                if (SurroundPan* to = surroundPanOf(p, id)) *to = sp;
            };
            surround->onOpenWindow = [this, id] {
                if (onOpenSurround) onOpenSurround(id);
            };
            surround->panType = [id]() { return surroundPanTypeOf(AppState::get().project(), id); };
            surround->setPanType = [id](const std::string& type) {
                AppState::get().edit("Pan Type", [&](Project& p) { setSurroundPanType(p, id, type); });
            };
        }
        if (monitor)
            QObject::connect(monitor->menu(), &QMenu::aboutToShow, [this, id] {
                QMenu* m = monitor->menu();
                m->clear();
                const Track* t = nullptr;
                for (const auto& x : AppState::get().project().tracks)
                    if (x.id == id) t = &x;
                if (!t) return;
                static const char* kModes[3][2] = {{"off", "Off"}, {"on", "On"}, {"auto", "Auto"}};
                for (const auto& mode : kModes) {
                    QAction* a = m->addAction(mode[1]);
                    a->setObjectName(QString("monitor:%1").arg(mode[0]));
                    a->setCheckable(true);
                    a->setChecked(t->recordInput.monitor == mode[0]);
                    const std::string key = mode[0];
                    QObject::connect(a, &QAction::triggered, m, [id, key] {
                        AppState::get().edit("Input Monitoring", [&](Project& p) {
                            if (Track* x = p.findTrack(id)) x->recordInput.monitor = key;
                        });
                    });
                }
            });
        if (kind == TrackStrip) {
            QObject::connect(source->menu(), &QMenu::aboutToShow, [this, id] {
                QMenu* m = source->menu();
                m->clear();
                const Track* t = nullptr;
                for (const auto& x : AppState::get().project().tracks)
                    if (x.id == id) t = &x;
                if (!t) return;
                fillRecordInputMenu(m, t->recordInput, true, [id](const RecordInput& r) {
                    AppState::get().edit("Record Input", [&](Project& p) {
                        if (Track* x = p.findTrack(id)) x->recordInput = r;
                    });
                });
            });
            pan->value = [id] {
                for (const auto& t : AppState::get().project().tracks)
                    if (t.id == id) return controlShown(AppState::get().project(), EnvelopeOwner{EnvelopeOwner::Track, id}, "pan", 0, t.pan, automationNow());
                return 0.0;
            };
            pan->apply = [id](Project& p, double v) {
                if (Track* t = p.findTrack(id)) if (!controlMoved(p, EnvelopeOwner{EnvelopeOwner::Track, id}, "pan", 0, v)) t->pan = v;
            };
            pan->undoName = "Track Pan";
            fader->value = [id] {
                for (const auto& t : AppState::get().project().tracks)
                    if (t.id == id) return controlShown(AppState::get().project(), EnvelopeOwner{EnvelopeOwner::Track, id}, "volume", 0, t.level, automationNow());
                return 0.0;
            };
            fader->apply = [id](Project& p, double v) {
                if (Track* t = p.findTrack(id)) if (!controlMoved(p, EnvelopeOwner{EnvelopeOwner::Track, id}, "volume", 0, v)) t->level = v;
            };
            fader->undoName = "Track Volume";
        } else if (kind == BusStrip) {
            pan->value = [id] {
                const reno::Bus* b = AppState::get().project().findBus(id);
                return b ? controlShown(AppState::get().project(), EnvelopeOwner{EnvelopeOwner::Bus, id}, "pan", 0, b->pan, automationNow()) : 0.0;
            };
            pan->apply = [id](Project& p, double v) {
                if (reno::Bus* b = p.findBus(id)) if (!controlMoved(p, EnvelopeOwner{EnvelopeOwner::Bus, id}, "pan", 0, v)) b->pan = v;
            };
            pan->undoName = "Bus Pan";
            fader->value = [id] {
                const reno::Bus* b = AppState::get().project().findBus(id);
                return b ? controlShown(AppState::get().project(), EnvelopeOwner{EnvelopeOwner::Bus, id}, "volume", 0, b->level, automationNow()) : 0.0;
            };
            fader->apply = [id](Project& p, double v) {
                if (reno::Bus* b = p.findBus(id)) if (!controlMoved(p, EnvelopeOwner{EnvelopeOwner::Bus, id}, "volume", 0, v)) b->level = v;
            };
            fader->undoName = "Bus Level";
        } else if (kind == PreviewStrip) {
            pan->value = [] { return 0.0; };
            pan->apply = [](Project&, double) {};
            fader->value = [] { return (double)previewBus().gain.load(); };
            fader->apply = [](Project&, double) {};
            fader->direct = [](double v) { previewBus().gain.store((float)v); };
            fader->undoName = "Preview Volume";
        } else if (kind == InputStrip) {
            pan->value = [id] {
                const reno::InputBus* b = AppState::get().project().findInputBus(id);
                return b ? b->pan : 0.0;
            };
            pan->apply = [id](Project& p, double v) {
                if (reno::InputBus* b = p.findInputBus(id)) b->pan = v;
            };
            pan->undoName = "Input Bus Pan";
            fader->value = [id] {
                const reno::InputBus* b = AppState::get().project().findInputBus(id);
                return b ? b->level : 0.0;
            };
            fader->apply = [id](Project& p, double v) {
                if (reno::InputBus* b = p.findInputBus(id)) b->level = v;
            };
            fader->undoName = "Input Bus Level";
            QObject::connect(source->menu(), &QMenu::aboutToShow, [this, id] {
                QMenu* m = source->menu();
                m->clear();
                const reno::InputBus* b = AppState::get().project().findInputBus(id);
                if (!b) return;
                RecordInput cur;
                cur.device = b->device;
                cur.first = b->first;
                cur.channels = b->channels;
                fillRecordInputMenu(m, cur, false, [id](const RecordInput& r) {
                    AppState::get().edit("Input Bus Input", [&](Project& p) {
                        if (reno::InputBus* x = p.findInputBus(id)) {
                            x->device = r.device;
                            x->first = r.first;
                            x->channels = r.channels;
                        }
                    });
                });
            });
        } else {
            pan->value = [] { return AppState::get().project().masterPan; };
            pan->apply = [](Project& p, double v) { p.masterPan = v; };
            pan->undoName = "Master Pan";
            fader->value = [] { return AppState::get().project().masterVolume; };
            fader->apply = [](Project& p, double v) { p.masterVolume = v; };
            fader->undoName = "Master Volume";
            // the right fader of a split master (masterVolumeR below 0: the left fader's value for both)
            fader->value2 = [] { return AppState::get().project().masterRightVolume(); };
            fader->apply2 = [](Project& p, double v) { p.masterVolumeR = v; };
            QObject::connect(link, &QToolButton::clicked, [](bool on) {
                AppState::get().edit(on ? "Split Master Fader" : "Link Master Faders", [&](Project& p) {
                    p.masterVolumeR = on ? p.masterVolume : -1.0;
                });
            });
        }
    }

    Kind kind_;
    uint64_t id_;
    Playback* pb_;
    bool paintedSelected_ = false;
};

// Feeds the meters of strips (by id) from what Playback publishes, ~30 times a second; clears them when stopped.
template <class Meter = FaderMeter>
class MeterFeed {
public:
    void tick(Playback* pb, const std::vector<std::pair<uint64_t, Meter*>>& meters)
    {
        const double now = clock_.isValid() ? clock_.elapsed() / 1000.0 : 0.0;
        if (!clock_.isValid()) clock_.start();
        const double dt = std::min(0.2, std::max(0.0, now - last_));
        last_ = now;
        if (!pb || !pb->playing()) {
            for (const auto& m : meters) m.second->clearMeter();
            return;
        }
        pb->stripLevels(levels_);
        for (const auto& m : meters) {
            float l = 0, r = 0;
            const AudioEngine::StripPeak* six = nullptr;
            for (const auto& x : levels_)
                if (x.id == m.first) {
                    l = x.left;
                    r = x.right;
                    if (x.channels == 6) six = &x;
                    break;
                }
            if (six) m.second->feedN(six->ch, 6, now, dt);
            else m.second->feed(l, r, now, dt);
        }
    }

private:
    QElapsedTimer clock_;
    double last_ = 0;
    std::vector<AudioEngine::StripPeak> levels_;
};

}

// ================================================================================================ MixerWindow
// The console: a toolbar (Views, Properties and layout, Dim Output, Insert Audio Track, Insert Bus, Delete Bus), at the
// left the Channel List (a tick per strip: shown or hidden) over the View Controls (which kinds of strips and control
// regions are shown, and the channel width), and the strips. VEGAS's view settings are kept between runs (QSettings
// "mixer/...").
struct MixerWindow::Impl {
    MixerWindow* owner = nullptr;
    Playback* pb = nullptr;
    QToolButton *insertBus = nullptr, *deleteBus = nullptr, *insertTrack = nullptr, *dimBtn = nullptr;
    QToolButton *insertAssignable = nullptr, *insertInput = nullptr, *downmixBtn = nullptr;
    QToolButton *showInputs = nullptr, *showPreview = nullptr, *showAll = nullptr, *showTracks = nullptr, *showBuses = nullptr, *showMaster = nullptr, *showAssign = nullptr;
    QToolButton *regionFx = nullptr, *regionSends = nullptr, *regionIO = nullptr, *regionMeters = nullptr, *regionFaders = nullptr, *regionVu = nullptr;
    QElapsedTimer vuClock, previewClock;
    double previewLast = 0;
    QSlider* widthSlider = nullptr;
    QWidget *side = nullptr, *channelPane = nullptr, *viewPane = nullptr, *sideSpace = nullptr;
    QLabel* formatLabel = nullptr;
    QListWidget* channelList = nullptr;
    QScrollArea* scroll = nullptr;
    QWidget* row = nullptr;
    QHBoxLayout* rowLayout = nullptr;
    QWidget* masterBox = nullptr;
    std::vector<Strip*> strips;
    Strip* master = nullptr;
    Strip* preview = nullptr;
    QLabel* empty = nullptr;
    uint64_t selectedBus = 0;
    std::set<QString> selected;
    std::set<QString> hidden;
    QString lastClicked;
    ConsoleView view;
    std::vector<uint64_t> shape;
    bool rebuildQueued = false, filling = false;
    // the project changed while the console was not showing (another tab in front): its strips are updated when it is
    // shown, not for every edit meanwhile (a strip refreshes a dozen widgets; there can be a hundred)
    bool stale = false;
    std::unique_ptr<Listener> listener;
    QTimer* meterTimer = nullptr;
    PollRate* rate = nullptr;       // slows the meter timer when nothing plays
    bool previewMoving = false;
    MeterFeed<> feed;
    std::function<void(const QPoint&)> meterMenuFn;

    static std::vector<uint64_t> shapeOf(const Project& p)
    {
        std::vector<uint64_t> s;
        for (const auto& b : p.inputBuses) s.push_back(b.id);
        s.push_back(0);
        for (const auto& t : p.tracks)
            if (t.type == TrackType::Audio) s.push_back(t.id);
        s.push_back(0);
        for (const auto& b : p.buses) s.push_back(b.id);
        return s;
    }

    std::vector<Strip*> all() const
    {
        std::vector<Strip*> v = strips;
        v.push_back(master);
        v.push_back(preview);
        return v;
    }

    void openFx(const FxTarget& t, uint64_t slot)
    {
        if (owner->onOpenFx) owner->onOpenFx(t, slot);
    }

    void rebuild()
    {
        rebuildQueued = false;
        const Project& p = AppState::get().project();
        for (Strip* s : strips) {
            rowLayout->removeWidget(s);
            s->hide();
            s->deleteLater();
        }
        strips.clear();
        int at = 0;
        for (const auto& b : p.inputBuses) {
            Strip* s = new Strip(Strip::InputStrip, b.id, pb, row);
            setup(s, p);
            rowLayout->insertWidget(at++, s);
            strips.push_back(s);
        }
        for (const auto& t : p.tracks) {
            if (t.type != TrackType::Audio) continue;
            Strip* s = new Strip(Strip::TrackStrip, t.id, pb, row);
            setup(s, p);
            rowLayout->insertWidget(at++, s);
            strips.push_back(s);
        }
        for (const auto& b : p.buses) {
            Strip* s = new Strip(Strip::BusStrip, b.id, pb, row);
            setup(s, p);
            rowLayout->insertWidget(at++, s);
            strips.push_back(s);
        }
        master->makeSends(p);
        shape = shapeOf(p);
        if (selectedBus && !p.findBus(selectedBus) && !p.findInputBus(selectedBus)) selectedBus = 0;
        std::set<QString> keys;
        for (Strip* s : all()) keys.insert(s->key());
        for (auto* set : {&selected, &hidden})
            for (auto it = set->begin(); it != set->end();) it = keys.count(*it) ? std::next(it) : set->erase(it);
        applyView();
        refresh();
    }

    void setup(Strip* s, const Project& p)
    {
        s->makeSends(p);
        s->onFx = [this](const FxTarget& t, uint64_t slot) { openFx(t, slot); };
        s->onSelect = [this](Strip* st, Qt::KeyboardModifiers m) { select(st, m); };
        s->onRename = [this](Strip* st) { rename(st); };
        s->onSelectedTracks = [this] {
            std::vector<uint64_t> v;
            for (Strip* x : strips)
                if (x->kind() == Strip::TrackStrip && selected.count(x->key())) v.push_back(x->id());
            return v;
        };
        s->onOpenSurround = [this](uint64_t id) {
            if (owner->onOpenSurround) owner->onOpenSurround(id);
        };
        s->fader->onMeterMenu = [this](const QPoint& at) { if (meterMenuFn) meterMenuFn(at); };
        wireVu(s);
        s->onArm = [this](uint64_t id) {
            if (owner->onArmTrack) owner->onArmTrack(id);
            else if (AppState::get().armedTracks.count(id)) AppState::get().armedTracks.erase(id);
            else AppState::get().armedTracks.insert(id);
        };
    }

    void wireVu(Strip* s)
    {
        VuMeter* v = s->vu;
        v->onMenu = [this, v](const QPoint& at) {
            QMenu m(owner);
            m.setObjectName("vuMenu");
            fillVuMenu(&m, v);
            FxSlots::exec(&m, at);
        };
        v->onWheel = [this](int dir) { cycleVu(dir); };
    }

    void select(Strip* st, Qt::KeyboardModifiers m)
    {
        const QString k = st->key();
        if (m & Qt::ControlModifier) {
            if (selected.count(k)) selected.erase(k);
            else selected.insert(k);
        } else if ((m & Qt::ShiftModifier) && !lastClicked.isEmpty()) {
            const std::vector<Strip*> v = all();
            int a = -1, b = -1;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i]->key() == lastClicked) a = (int)i;
                if (v[i] == st) b = (int)i;
            }
            selected.clear();
            if (a >= 0 && b >= 0)
                for (int i = std::min(a, b); i <= std::max(a, b); ++i) selected.insert(v[(size_t)i]->key());
        } else {
            selected.clear();
            selected.insert(k);
        }
        lastClicked = k;
        if (st->kind() == Strip::BusStrip || st->kind() == Strip::InputStrip) selectedBus = st->id();
        else if (!(m & Qt::ControlModifier)) selectedBus = 0;
        owner->setFocus(Qt::MouseFocusReason);
        refresh();
    }

    void rename(Strip* st)
    {
        if (!st || st->kind() == Strip::MasterStrip || st->kind() == Strip::PreviewStrip) return;
        QLineEdit* ed = new QLineEdit(st->header);
        ed->setObjectName("renameEdit");
        ed->setGeometry(st->header->rect().adjusted(2, 2, -2, -14));
        const Project& p = AppState::get().project();
        QString cur = st->name(p);
        if (st->kind() == Strip::TrackStrip) {
            const Track* t = nullptr;
            for (const auto& x : p.tracks)
                if (x.id == st->id()) t = &x;
            cur = t ? qs(t->name) : QString();
        }
        ed->setText(cur);
        ed->selectAll();
        ed->show();
        ed->setFocus();
        const Strip::Kind kind = st->kind();
        const uint64_t id = st->id();
        QPointer<QLineEdit> guard(ed);
        auto done = std::make_shared<bool>(false);
        QObject::connect(ed, &QLineEdit::editingFinished, ed, [this, guard, kind, id, done] {
            if (*done || !guard) return;
            *done = true;
            const std::string name = guard->text().trimmed().toStdString();
            guard->hide();
            QTimer::singleShot(0, guard.data(), [guard] { if (guard) guard->deleteLater(); });
            owner->setFocus();
            if ((kind == Strip::BusStrip || kind == Strip::InputStrip) && name.empty()) return;
            AppState::get().edit(kind == Strip::TrackStrip ? "Rename Track" : kind == Strip::InputStrip ? "Rename Input Bus" : "Rename Bus", [&](Project& pr) {
                if (kind == Strip::TrackStrip) {
                    if (Track* t = pr.findTrack(id)) t->name = name;
                } else if (kind == Strip::InputStrip) {
                    if (reno::InputBus* b = pr.findInputBus(id)) b->name = name;
                } else if (reno::Bus* b = pr.findBus(id)) {
                    b->name = name;
                }
            });
        });
        ed->installEventFilter(owner);
        ed->setProperty("escapeCancels", true);
    }

    bool shownByKind(const Strip* s) const
    {
        if (s->kind() == Strip::TrackStrip) return showTracks->isChecked();
        if (s->kind() == Strip::InputStrip) return showInputs->isChecked();
        if (s->kind() == Strip::PreviewStrip) return showPreview->isChecked();
        if (s->kind() == Strip::BusStrip) return s->assignable(AppState::get().project()) ? showAssign->isChecked() : showBuses->isChecked();
        return showMaster->isChecked();
    }

    void applyView()
    {
        int shown = 0;
        for (Strip* s : strips) {
            const bool on = shownByKind(s) && !hidden.count(s->key());
            s->applyView(view);
            s->setVisible(on);
            if (on) ++shown;
        }
        master->applyView(view);
        preview->applyView(view);
        {
            const bool ms = shownByKind(master) && !hidden.count(master->key());
            const bool ps = shownByKind(preview) && !hidden.count(preview->key());
            master->setVisible(ms);
            preview->setVisible(ps);
            masterBox->setVisible(ms || ps);
        }
        empty->setVisible(shown == 0);
        empty->setText(strips.empty() ? QString("No audio tracks or buses.\nAdd an audio track, or Insert Bus.") : QString("All strips are hidden."));
        rowLayout->activate();
        scroll->setMaximumWidth(row->sizeHint().width() + 2);
        {
            const QSignalBlocker b1(regionFx), b2(regionSends), b3(regionIO), b4(regionMeters), b5(regionFaders), b6(widthSlider), b7(regionVu);
            regionFx->setChecked(view.fx);
            regionSends->setChecked(view.sends);
            regionIO->setChecked(view.io);
            regionMeters->setChecked(view.meters);
            regionFaders->setChecked(view.faders);
            regionVu->setChecked(view.vu);
            widthSlider->setValue(view.width);
        }
        fillChannelList();
        owner->setProperty("channelWidth", view.width);
        owner->setProperty("stripWidth", view.stripWidth());
    }

    void fillChannelList()
    {
        if (!channelList) return;
        filling = true;
        const Project& p = AppState::get().project();
        channelList->clear();
        for (Strip* s : all()) {
            QListWidgetItem* it = new QListWidgetItem(s->name(p), channelList);
            it->setData(Qt::UserRole, s->key());
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            it->setCheckState(hidden.count(s->key()) ? Qt::Unchecked : Qt::Checked);
            if (!shownByKind(s)) it->setForeground(QColor(110, 110, 115));
        }
        filling = false;
    }

    void refresh()
    {
        const Project& p = AppState::get().project();
        if (formatLabel) {
            // the project's audio format, as VEGAS writes it: 48,000 Hz, 16 Bit
            const QString f = QString("%1 Hz, %2 Bit").arg(QLocale(QLocale::English, QLocale::UnitedStates).toString(p.settings.sampleRate)).arg(p.settings.audioBits);
            if (formatLabel->text() != f) formatLabel->setText(f);
        }
        if (shapeOf(p) != shape) {
            // Rebuilt once control is back in the event loop: the change may have come from one of the strips' own
            // widgets (an output menu), which must not be deleted under its own feet.
            if (!rebuildQueued) {
                rebuildQueued = true;
                QTimer::singleShot(0, owner, [this] { rebuild(); });
            }
            return;
        }
        for (Strip* s : strips) {
            int idx = 0;
            for (size_t k = 0; k < p.tracks.size(); ++k)
                if (p.tracks[k].id == s->id()) idx = (int)k;
            s->header->selected = selected.count(s->key()) != 0;
            s->refresh(p, idx);
        }
        master->header->selected = selected.count(master->key()) != 0;
        master->refresh(p, 0);
        preview->header->selected = selected.count(preview->key()) != 0;
        preview->refresh(p, 0);
        if (channelList) {
            const std::vector<Strip*> every = all();
            for (int i = 0; i < channelList->count(); ++i) {
                QListWidgetItem* it = channelList->item(i);
                const QString key = it->data(Qt::UserRole).toString();
                for (Strip* s : every) {
                    if (s->key() != key) continue;
                    const QString name = s->name(p);
                    if (it->text() != name) {
                        filling = true;
                        it->setText(name);
                        filling = false;
                    }
                }
            }
        }
        deleteBus->setEnabled(!p.buses.empty() || p.findInputBus(selectedBus));
        const reno::Bus* sel = p.findBus(selectedBus);
        if (const reno::InputBus* ib = p.findInputBus(selectedBus)) deleteBus->setToolTip(QString("Delete %1 (tracks recording from it record from the default input)").arg(qs(ib->name)));
        else deleteBus->setToolTip(p.buses.empty() ? QString("Delete Bus") : QString("Delete %1 (sound routed to it goes to the master)").arg(qs(sel ? sel->name : p.buses.back().name)));
        QStringList keys;
        for (const QString& k : selected) keys << k;
        owner->setProperty("selectedStrips", keys);
    }

    void tickMeters()
    {
        if (!owner->isVisible()) return;
        std::vector<std::pair<uint64_t, FaderMeter*>> meters;
        for (Strip* s : strips)
            if (s->isVisible()) meters.push_back(std::make_pair(s->id(), s->fader));
        meters.push_back(std::make_pair((uint64_t)0, master->fader));
        feed.tick(pb, meters);
        {
            // the Preview bus's meter: the loudest sound since the last look, playing or not
            const double now = previewClock.isValid() ? previewClock.elapsed() / 1000.0 : 0.0;
            if (!previewClock.isValid()) previewClock.start();
            const double dt = std::min(0.2, std::max(0.0, now - previewLast));
            previewLast = now;
            PreviewBus& pv = previewBus();
            const float pl = pv.peakL.exchange(0.f), pr = pv.peakR.exchange(0.f);
            preview->fader->feed(pl, pr, now, dt);
            previewMoving = pl > 0.f || pr > 0.f;
        }
        if (view.vu) {
            const double dt = vuClock.isValid() ? std::min(0.25, vuClock.restart() / 1000.0) : 0.033;
            if (!vuClock.isValid()) vuClock.start();
            const bool playing = pb && pb->playing();
            for (Strip* s : all()) {
                if (!s->isVisible() && s != master) continue;
                if (playing) s->vu->feed(s->fader->meter.level[0], s->fader->meter.level[1], dt);
                else s->vu->clearMeter();
            }
        }
        if (pb && dimBtn->isChecked() != pb->dimmed()) {
            const QSignalBlocker b(dimBtn);
            dimBtn->setChecked(pb->dimmed());
        }
        if (rate) rate->ticked((pb && pb->playing()) || previewMoving);
    }

    void save() const
    {
        QSettings s;
        s.setValue("mixer/width", view.width);
        s.setValue("mixer/regions", QString("%1%2%3%4%5%6").arg(view.fx).arg(view.sends).arg(view.io).arg(view.meters).arg(view.faders).arg(view.vu));
        const VuOptions& vo = vuOptions();
        s.setValue("mixer/vu", QString("%1 %2%3%4%5%6").arg(vo.type).arg(vo.labels).arg(vo.peaks).arg(vo.holdPeaks).arg(vo.holdValleys).arg(vo.vintage));
        s.setValue("mixer/labels", view.labels);
        s.setValue("mixer/ticks", view.ticks);
        s.setValue("mixer/holdPeaks", view.holdPeaks);
        s.setValue("mixer/meterRange", kMeterLo);
        s.setValue("mixer/channelList", !channelPane->isHidden());
        s.setValue("mixer/viewControls", !viewPane->isHidden());
        s.setValue("mixer/show", QString("%1%2%3%4%5%6").arg(showTracks->isChecked()).arg(showBuses->isChecked()).arg(showMaster->isChecked()).arg(showAssign->isChecked()).arg(showInputs->isChecked()).arg(showPreview->isChecked()));
    }
    void load()
    {
        QSettings s;
        view.width = std::min(2, std::max(0, s.value("mixer/width", (int)ConsoleView::Default).toInt()));
        const QString r = s.value("mixer/regions", "111110").toString();
        if (r.size() >= 5) {
            view.fx = r[0] == '1'; view.sends = r[1] == '1'; view.io = r[2] == '1'; view.meters = r[3] == '1'; view.faders = r[4] == '1';
            view.vu = r.size() >= 6 && r[5] == '1';
        }
        const QStringList vs = s.value("mixer/vu", "0 11000").toString().split(' ');
        if (vs.size() == 2 && vs[1].size() == 5) {
            VuOptions& vo = vuOptions();
            vo.type = std::max(0, std::min((int)VuOptions::TypeCount - 1, vs[0].toInt()));
            vo.labels = vs[1][0] == '1'; vo.peaks = vs[1][1] == '1'; vo.holdPeaks = vs[1][2] == '1'; vo.holdValleys = vs[1][3] == '1'; vo.vintage = vs[1][4] == '1';
        }
        view.vu = s.value("mixer/vu", false).toBool();
        view.labels = s.value("mixer/labels", false).toBool();
        view.ticks = s.value("mixer/ticks", true).toBool();
        view.holdPeaks = s.value("mixer/holdPeaks", true).toBool();
        const double range = s.value("mixer/meterRange", -60.0).toDouble();
        if (range <= -12 && range >= -120) kMeterLo = range;
        channelPane->setVisible(s.value("mixer/channelList", false).toBool());
        viewPane->setVisible(s.value("mixer/viewControls", true).toBool());
        side->setVisible(!channelPane->isHidden() || !viewPane->isHidden());
        sideSpace->setVisible(channelPane->isHidden());
        const QString sh = s.value("mixer/show", "1111").toString();
        if (sh.size() >= 3) {
            const QSignalBlocker b1(showTracks), b2(showBuses), b3(showMaster), b4(showAssign), b5(showInputs), b6(showPreview);
            showInputs->setChecked(sh.size() < 5 || sh[4] == '1');
            showPreview->setChecked(sh.size() >= 6 && sh[5] == '1');
            showTracks->setChecked(sh[0] == '1');
            showBuses->setChecked(sh[1] == '1');
            showMaster->setChecked(sh[2] == '1');
            showAssign->setChecked(sh.size() < 4 || sh[3] == '1');
        }
    }
    void viewChanged()
    {
        applyView();
        save();
    }

    void setPane(QWidget* pane, bool on)
    {
        pane->setVisible(on);
        side->setVisible(!channelPane->isHidden() || !viewPane->isHidden());
        sideSpace->setVisible(channelPane->isHidden());
        owner->setProperty("channelListShown", !channelPane->isHidden());
        owner->setProperty("viewControlsShown", !viewPane->isHidden());
        save();
    }

    void nudgeFaders(int dir)
    {
        std::vector<Strip*> sel;
        for (Strip* s : all())
            if (selected.count(s->key())) sel.push_back(s);
        if (sel.empty()) {
            AppState::get().status("Mixing Console: click a channel's name to select it first");
            return;
        }
        const double step = std::pow(10.0, 0.5 / 20.0);
        auto bump = [&](double g) {
            if (dir > 0) return g <= 0.001 ? 0.001 : std::min(kMaxGain, g * step);
            return g <= 0.001 ? 0.0 : g / step;
        };
        bool track = false, bus = false, mst = false;
        for (Strip* s : sel)
            if (s->kind() == Strip::PreviewStrip) previewBus().gain.store((float)bump(previewBus().gain.load()));
        for (Strip* s : sel) {
            track = track || s->kind() == Strip::TrackStrip;
            bus = bus || s->kind() == Strip::BusStrip || s->kind() == Strip::InputStrip;
            mst = mst || s->kind() == Strip::MasterStrip;
        }
        const char* what = (int)track + (int)bus + (int)mst > 1 ? "Channel Volume" : track ? "Track Volume" : bus ? "Bus Level" : "Master Volume";
        AppState::get().edit(what, [&](Project& p) {
            for (Strip* s : sel) {
                if (s->kind() == Strip::TrackStrip) {
                    if (Track* t = p.findTrack(s->id())) t->level = bump(t->level);
                } else if (s->kind() == Strip::InputStrip) {
                    if (reno::InputBus* b = p.findInputBus(s->id())) b->level = bump(b->level);
                } else if (s->kind() == Strip::BusStrip) {
                    if (reno::Bus* b = p.findBus(s->id())) b->level = bump(b->level);
                } else if (s->kind() == Strip::MasterStrip) {
                    p.masterVolume = bump(p.masterVolume);
                }
            }
        });
    }

    void showAllChannels()
    {
        hidden.clear();
        const QSignalBlocker b1(showTracks), b2(showBuses), b3(showMaster), b4(showAssign), b5(showInputs), b6(showPreview);
        showPreview->setChecked(true);
        showInputs->setChecked(true);
        showTracks->setChecked(true);
        showBuses->setChecked(true);
        showMaster->setChecked(true);
        showAssign->setChecked(true);
        viewChanged();
    }

    void insertAudioTrack()
    {
        AppState::get().addTrack(TrackType::Audio);
    }

    // the window's parts (the constructor's steps)
    QWidget* buildToolBar();
    QWidget* buildSidePane();
    void buildStrips(QHBoxLayout* body);
    // the tool bar's commands
    void fillViewsMenu(QMenu* viewsMenu);
    static void insertNewBus();
    void deleteSelectedBus();
    void insertAssignableChain();

    void fillLayoutMenu(QMenu* m)
    {
        m->clear();
        auto item = [&](QMenu* in, const QString& text, const QString& key, bool checkable, bool checked, std::function<void()> fn, bool enabled = true) {
            QAction* a = in->addAction(key.isEmpty() ? text : text + "\t" + key);
            a->setCheckable(checkable);
            a->setChecked(checked);
            a->setEnabled(enabled);
            QObject::connect(a, &QAction::triggered, owner, [fn] { fn(); });
            return a;
        };
        item(m, "Audio Properties...", "", false, false, [this] { if (owner->onAudioProperties) owner->onAudioProperties(); }, (bool)owner->onAudioProperties);
        m->addSeparator();
        QMenu* chans = m->addMenu("Show Channels");
        item(chans, "Show All Channels", "Q", false, false, [this] { owner->handleKey(Qt::Key_Q, Qt::NoModifier); });
        chans->addSeparator();
        item(chans, "Preview Bus", "P", true, showPreview->isChecked(), [this] { owner->handleKey(Qt::Key_P, Qt::NoModifier); });
        item(chans, "Master Bus", "T", true, showMaster->isChecked(), [this] { owner->handleKey(Qt::Key_T, Qt::NoModifier); });
        item(chans, "Assignable FX Busses", "E", true, showAssign->isChecked(), [this] { owner->handleKey(Qt::Key_E, Qt::NoModifier); });
        item(chans, "Input Busses", "", true, showInputs->isChecked(), [this] { showInputs->setChecked(!showInputs->isChecked()); });
        item(chans, "Audio Busses", "U", true, showBuses->isChecked(), [this] { owner->handleKey(Qt::Key_U, Qt::NoModifier); });
        item(chans, "Audio Tracks", "A", true, showTracks->isChecked(), [this] { owner->handleKey(Qt::Key_A, Qt::NoModifier); });
        QMenu* regs = m->addMenu("Show Control Regions");
        item(regs, "Show All Control Regions", "", false, false, [this] {
            view.fx = view.sends = view.io = view.meters = view.faders = view.vu = true;
            viewChanged();
        });
        regs->addSeparator();
        item(regs, "Faders Control Region", "F", true, view.faders, [this] { owner->handleKey(Qt::Key_F, Qt::NoModifier); });
        item(regs, "Peak Meters Control Region", "M", true, view.meters, [this] { owner->handleKey(Qt::Key_M, Qt::NoModifier); });
        item(regs, "VU Meters Control Region", "", true, view.vu, [this] {
            view.vu = !view.vu;
            viewChanged();
        });
        item(regs, "I/O Control Region", "H", true, view.io, [this] { owner->handleKey(Qt::Key_H, Qt::NoModifier); });
        item(regs, "Sends Control Region", "S", true, view.sends, [this] { owner->handleKey(Qt::Key_S, Qt::NoModifier); });
        item(regs, "Insert FX Control Region", "I", true, view.fx, [this] { owner->handleKey(Qt::Key_I, Qt::NoModifier); });
        item(m, "Show Control Region Labels", "Shift+L", true, view.labels, [this] { owner->handleKey(Qt::Key_L, Qt::ShiftModifier); });
        item(m, "Show Fader Ticks", "Shift+T", true, view.ticks, [this] { owner->handleKey(Qt::Key_T, Qt::ShiftModifier); });
        QMenu* w = m->addMenu("Channel Width");
        item(w, "Narrow", "N", true, view.width == ConsoleView::Narrow, [this] { owner->handleKey(Qt::Key_N, Qt::NoModifier); });
        item(w, "Default", "D", true, view.width == ConsoleView::Default, [this] { owner->handleKey(Qt::Key_D, Qt::NoModifier); });
        item(w, "Wide", "W", true, view.width == ConsoleView::Wide, [this] { owner->handleKey(Qt::Key_W, Qt::NoModifier); });
        QMenu* meters = m->addMenu("Meter Layout");
        fillMeterMenu(meters);
        m->addSeparator();
        item(m, "Hide Selected Channels", "Shift+H", false, false, [this] { owner->handleKey(Qt::Key_H, Qt::ShiftModifier); }, !selected.empty());
        item(m, "Rename Selected Channel", "F2", false, false, [this] { owner->handleKey(Qt::Key_F2, Qt::NoModifier); }, !selected.empty());
    }

    // VEGAS's VU / PPM meter menu (right-click on a VU meter): Show VU/PPM for that channel strip, Reset Clip, the seven
    // kinds of meter, Show Labels, Show Peaks, Hold Peaks, Hold Valleys, Vintage Mode. All but the first go for every VU
    // meter in the console.
    void fillVuMenu(QMenu* m, VuMeter* which)
    {
        VuOptions& o = vuOptions();
        auto refresh = [this] {
            for (Strip* s : all()) s->vu->update();
            save();
        };
        auto check = [&](const QString& text, bool on, std::function<void()> fn) {
            QAction* a = m->addAction(text);
            a->setCheckable(true);
            a->setChecked(on);
            QObject::connect(a, &QAction::triggered, owner, [fn, refresh] { fn(); refresh(); });
            return a;
        };
        if (which) check("Show VU/PPM", which->shown[0] || which->shown[1], [which] { which->shown[0] = which->shown[1] = !(which->shown[0] || which->shown[1]); });
        QAction* rc = m->addAction("Reset Clip");
        QObject::connect(rc, &QAction::triggered, owner, [this] { for (Strip* s : all()) s->vu->resetClip(); });
        m->addSeparator();
        for (int t = 0; t < VuOptions::TypeCount; ++t)
            check(VuOptions::name(t), o.type == t, [t] { vuOptions().type = t; });
        m->addSeparator();
        check("Show Labels", o.labels, [] { vuOptions().labels = !vuOptions().labels; });
        check("Show Peaks", o.peaks, [] { vuOptions().peaks = !vuOptions().peaks; });
        check("Hold Peaks", o.holdPeaks, [] { vuOptions().holdPeaks = !vuOptions().holdPeaks; });
        check("Hold Valleys", o.holdValleys, [] { vuOptions().holdValleys = !vuOptions().holdValleys; });
        check("Vintage Mode", o.vintage, [] { vuOptions().vintage = !vuOptions().vintage; });
    }
    void cycleVu(int dir)
    {
        VuOptions& o = vuOptions();
        o.type = ((o.type + dir) % VuOptions::TypeCount + VuOptions::TypeCount) % VuOptions::TypeCount;
        for (Strip* s : all()) {
            s->vu->resetClip();
            s->vu->update();
        }
        AppState::get().status(std::string("VU meters: ") + o.name(o.type));
        save();
    }

    void fillMeterMenu(QMenu* m)
    {
        QAction* reset = m->addAction("Reset Clip");
        QObject::connect(reset, &QAction::triggered, owner, [this] {
            for (Strip* s : all()) {
                for (int k = 0; k < 6; ++k) {
                    s->fader->meter.clip[k] = false;
                    s->fader->meter.hold[k] = 0;
                }
                s->fader->update();
            }
        });
        QAction* hold = m->addAction("Hold Peaks");
        hold->setCheckable(true);
        hold->setChecked(view.holdPeaks);
        QObject::connect(hold, &QAction::triggered, owner, [this](bool on) {
            view.holdPeaks = on;
            viewChanged();
        });
        m->addSeparator();
        for (int r : {-24, -42, -60, -78, -96}) {
            QAction* a = m->addAction(QString("%1 dB").arg(r));
            a->setCheckable(true);
            a->setChecked(std::fabs(kMeterLo - r) < 0.5);
            QObject::connect(a, &QAction::triggered, owner, [this, r] {
                kMeterLo = r;
                for (Strip* s : all()) s->fader->update();
                save();
            });
        }
    }
};

// A button of the console's tool bar or view controls (checkable ones start checked).
static QToolButton* makeTool(QWidget* in, QBoxLayout* lay, const char* name, const QString& text, const QString& tip, bool checkable)
{
    auto* b = new QToolButton(in);
    b->setObjectName(name);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setChecked(checkable);
    b->setProperty("tool", true);
    b->setFixedHeight(22);
    b->setFocusPolicy(Qt::NoFocus);
    if (lay) lay->addWidget(b);
    return b;
}

// The tool bar's small pictures (drawn here, VEGAS's own are not used): the layout and settings menus, Downmix, Dim,
// and the Insert buttons'.
enum ToolPicture { LayoutPic, CogPic, DownmixPic, DimPic, TrackPic, AssignablePic, BusPic, InputBusPic, ZoomOutPic, ZoomInPic };
static QIcon toolPicture(ToolPicture which)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(16, 16) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink(232, 232, 236);
    p.setPen(QPen(ink, 1.2));
    p.setBrush(Qt::NoBrush);
    switch (which) {
    case LayoutPic:
        p.drawRect(QRectF(1.5, 2.5, 13, 11));
        p.drawLine(QPointF(1.5, 6), QPointF(14.5, 6));
        p.drawLine(QPointF(6, 6), QPointF(6, 13.5));
        p.drawLine(QPointF(10, 6), QPointF(10, 13.5));
        break;
    case CogPic:
        p.setBrush(ink);
        p.setPen(Qt::NoPen);
        for (int k = 0; k < 8; ++k) {
            const double a = k * M_PI / 4.0;
            p.drawEllipse(QPointF(8 + 5.6 * std::cos(a), 8 + 5.6 * std::sin(a)), 1.7, 1.7);
        }
        p.drawEllipse(QPointF(8, 8), 5, 5);
        p.setBrush(QColor(45, 45, 48));
        p.drawEllipse(QPointF(8, 8), 2, 2);
        break;
    case DownmixPic:
        // two arrows meeting at a bar
        p.drawLine(QPointF(8, 2), QPointF(8, 14));
        p.setBrush(ink);
        {
            const QPointF l[3] = {QPointF(1.5, 4), QPointF(1.5, 12), QPointF(6.5, 8)};
            const QPointF r[3] = {QPointF(14.5, 4), QPointF(14.5, 12), QPointF(9.5, 8)};
            p.drawPolygon(l, 3);
            p.drawPolygon(r, 3);
        }
        break;
    case DimPic: {
        // a speaker and an arrow down
        p.setBrush(ink);
        const QPointF spk[6] = {QPointF(1.5, 6), QPointF(4, 6), QPointF(7.5, 2.5), QPointF(7.5, 13.5), QPointF(4, 10), QPointF(1.5, 10)};
        p.drawPolygon(spk, 6);
        p.drawLine(QPointF(12, 3), QPointF(12, 12));
        const QPointF ar[3] = {QPointF(9.5, 10), QPointF(14.5, 10), QPointF(12, 14)};
        p.drawPolygon(ar, 3);
        break;
    }
    case TrackPic:
        p.setPen(QPen(QColor(120, 200, 255), 1.2));
        p.drawRect(QRectF(1.5, 3.5, 13, 9));
        p.drawLine(QPointF(4, 8), QPointF(5.5, 5.5));
        p.drawLine(QPointF(5.5, 5.5), QPointF(7, 10.5));
        p.drawLine(QPointF(7, 10.5), QPointF(8.5, 6));
        p.drawLine(QPointF(8.5, 6), QPointF(10, 9));
        p.drawLine(QPointF(10, 9), QPointF(12, 8));
        break;
    case AssignablePic: {
        QFont f("DejaVu Serif");
        f.setItalic(true);
        f.setPointSizeF(9);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, "fx");
        break;
    }
    case BusPic:
    case InputBusPic:
        p.setPen(QPen(which == BusPic ? QColor(150, 210, 140) : QColor(240, 190, 120), 1.2));
        p.drawLine(QPointF(2, 4), QPointF(14, 4));
        p.drawLine(QPointF(2, 12), QPointF(14, 12));
        p.drawLine(QPointF(5, 4), QPointF(5, 12));
        p.drawLine(QPointF(11, 4), QPointF(11, 12));
        p.drawLine(QPointF(5, 8), QPointF(11, 8));
        break;
    case ZoomOutPic:
    case ZoomInPic:
        p.drawEllipse(QPointF(6.5, 6.5), 4.5, 4.5);
        p.setPen(QPen(ink, 2.0));
        p.drawLine(QPointF(10, 10), QPointF(14.5, 14.5));
        p.setPen(QPen(ink, 1.2));
        p.drawLine(QPointF(4.5, 6.5), QPointF(8.5, 6.5));
        if (which == ZoomInPic) p.drawLine(QPointF(6.5, 4.5), QPointF(6.5, 8.5));
        break;
    }
    return QIcon(pm);
}

// The console's tool bar, as VEGAS's: the project's audio format at the left; at the right the layout (Views) and
// settings (Properties) menus, Downmix and Dim Output, and the Insert buttons.
QWidget* MixerWindow::Impl::buildToolBar()
{
    auto* bar = new QWidget(owner);
    bar->setObjectName("mixerToolbar");
    bar->setAutoFillBackground(true);
    QPalette bp = bar->palette();
    bp.setColor(QPalette::Window, kConsoleBg);
    bar->setPalette(bp);
    bar->setStyleSheet(
        "QToolButton { background: transparent; border: 1px solid transparent; border-radius: 2px; color: #e0e0e0; padding: 1px 3px; }"
        "QToolButton:hover { background: rgba(255, 255, 255, 30); }"
        "QToolButton:checked { background: #12b0ff; border-color: #12b0ff; color: white; }"
        "QToolButton::menu-indicator { image: none; width: 0px; }");
    auto* tb = new QHBoxLayout(bar);
    tb->setContentsMargins(8, 3, 6, 3);
    tb->setSpacing(2);
    // (its buttons are cut off in a narrow place, as VEGAS's are, rather than holding the place - and every window sharing
    // it - at the bar's whole width)
    bar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    bar->setMinimumWidth(0);
    formatLabel = new QLabel(bar);
    formatLabel->setObjectName("audioFormat");
    formatLabel->setStyleSheet("QLabel { color: #e0e0e0; }");
    tb->addWidget(formatLabel);
    tb->addStretch(1);
    // a menu button with its small arrow beside the picture
    auto menuTool = [&](const char* name, const QString& text, const QString& tip, ToolPicture pic) {
        QToolButton* b = makeTool(bar, tb, name, text, tip, false);
        b->setIcon(toolPicture(pic));
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setPopupMode(QToolButton::InstantPopup);
        b->setMenu(new QMenu(b));
        QLabel* arrow = new QLabel(QString::fromUtf8("\xe2\x96\xbe"), bar);
        arrow->setStyleSheet("QLabel { color: #e0e0e0; }");
        tb->addWidget(arrow);
        tb->addSpacing(4);
        return b;
    };
    QToolButton* views = menuTool("views", "Views", "Show or hide the Channel List (Shift+C) and the View Controls (Shift+R)", LayoutPic);
    QMenu* viewsMenu = views->menu();
    QToolButton* layoutBtn = menuTool("properties", "Properties", "Channels and control regions shown, channel width, meters", CogPic);
    QMenu* layoutMenu = layoutBtn->menu();
    QObject::connect(layoutMenu, &QMenu::aboutToShow, owner, [this, layoutMenu] { fillLayoutMenu(layoutMenu); });
    downmixBtn = makeTool(bar, tb, "downmixOutput", "Downmix", "Downmix Output: hear the mix in stereo (a 5.1 project) or in mono", false);
    downmixBtn->setIcon(toolPicture(DownmixPic));
    downmixBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    downmixBtn->setPopupMode(QToolButton::InstantPopup);
    QMenu* downmixMenu = new QMenu(downmixBtn);
    downmixBtn->setMenu(downmixMenu);
    QObject::connect(downmixMenu, &QMenu::aboutToShow, owner, [this, downmixMenu] { reno::app::fillDownmixMenu(downmixMenu, pb); });
    dimBtn = makeTool(bar, tb, "dimOutput", "Dim", "Dim Output: what is heard 20 dB quieter (Ctrl+Shift+F12)", true);
    dimBtn->setChecked(pb && pb->dimmed());
    dimBtn->setIcon(toolPicture(DimPic));
    dimBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->addSpacing(8);
    auto insertTool = [&](const char* name, const QString& text, const QString& tip, ToolPicture pic) {
        QToolButton* b = makeTool(bar, tb, name, text, tip, false);
        b->setIcon(toolPicture(pic));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        return b;
    };
    insertTrack = insertTool("insertAudioTrack", "Insert Audio Track", "Add an audio track (Ctrl+Q)", TrackPic);
    insertAssignable = insertTool("insertAssignableFx", "Insert Assignable FX...", "Add an assignable effects chain for tracks to send to (Insert > Audio Assignable FX)", AssignablePic);
    insertBus = insertTool("insertBus", "Insert Bus", "Add a bus (Bus A, Bus B ...)", BusPic);
    insertInput = insertTool("insertInputBus", "Insert Input Bus", "Add an input bus: sound from a sound card input, mixed with the project (VEGAS Pro 23)", InputBusPic);
    // (VEGAS has no Delete Bus button: a bus is deleted from its strip's menu or with Delete; the button stays for that)
    deleteBus = makeTool(bar, nullptr, "deleteBus", "Delete Bus", "Delete Bus", false);
    deleteBus->hide();
    QObject::connect(viewsMenu, &QMenu::aboutToShow, owner, [this, viewsMenu] { fillViewsMenu(viewsMenu); });
    QObject::connect(insertTrack, &QToolButton::clicked, owner, [this] { insertAudioTrack(); });
    QObject::connect(dimBtn, &QToolButton::toggled, owner, [this](bool on) {
        if (owner->onDim) owner->onDim(on);
        else if (pb) pb->setDim(on);
    });
    QObject::connect(insertBus, &QToolButton::clicked, [] { insertNewBus(); });
    QObject::connect(insertInput, &QToolButton::clicked, [] { reno::app::insertInputBus(); });
    QObject::connect(deleteBus, &QToolButton::clicked, [this] { deleteSelectedBus(); });
    // Insert Assignable FX: a new chain (FX 1, FX 2 ...), then its effects chosen (as an empty FX button does)
    QObject::connect(insertAssignable, &QToolButton::clicked, owner, [this] { insertAssignableChain(); });
    return bar;
}

// The side pane, as VEGAS's: the Channel List (hidden at first) over the View Controls, a column of switches (which
// kinds of channels, then which control regions are shown) and the channel width as a zoom slider at the bottom.
QWidget* MixerWindow::Impl::buildSidePane()
{
    side = new QWidget(owner);
    side->setObjectName("consoleSide");
    side->setFixedWidth(96);
    side->setAutoFillBackground(true);
    QPalette sp = side->palette();
    sp.setColor(QPalette::Window, kConsoleBg);
    side->setPalette(sp);
    side->setStyleSheet(
        "QToolButton { background: transparent; border: none; color: #e0e0e0; padding: 1px 2px; }"
        "QToolButton:hover:!checked { background: rgba(255, 255, 255, 30); }"
        "QToolButton:checked { background: #12b0ff; color: white; }");
    auto* sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(4, 4, 4, 4);
    sideLay->setSpacing(4);
    channelPane = new QWidget(side);
    channelPane->setObjectName("channelListPane");
    auto* cl = new QVBoxLayout(channelPane);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);
    QLabel* clTitle = new QLabel("Channel List", channelPane);
    clTitle->setStyleSheet("color: #a0a0a6;");
    cl->addWidget(clTitle);
    channelList = new QListWidget(channelPane);
    channelList->setObjectName("channelList");
    channelList->setStyleSheet("QListWidget { background: #2a2a2d; border: 1px solid #1c1c1e; color: #dcdcdc; }");
    channelList->setFocusPolicy(Qt::NoFocus);
    cl->addWidget(channelList, 1);
    sideLay->addWidget(channelPane, 1);
    // (with the Channel List hidden, the switches sit low, above the zoom slider, as VEGAS's)
    sideSpace = new QWidget(side);
    sideSpace->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    sideLay->addWidget(sideSpace, 1);

    viewPane = new QWidget(side);
    viewPane->setObjectName("viewControls");
    auto* vg = new QVBoxLayout(viewPane);
    vg->setContentsMargins(0, 0, 0, 0);
    vg->setSpacing(1);
    auto viewTool = [&](const char* name, const QString& text, const QString& tip, bool region) {
        QToolButton* b = makeTool(viewPane, nullptr, name, text, tip, true);
        b->setProperty("tool", QVariant());
        b->setProperty("region", region);
        b->setFixedHeight(20);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        vg->addWidget(b);
        return b;
    };
    showAll = viewTool("showAll", "Show All", "Show every channel (Q)", false);
    showAll->setCheckable(false);
    showTracks = viewTool("showTracks", "Audio Tracks", "Show the audio tracks' strips (A)", false);
    showBuses = viewTool("showBuses", "Audio Busses", "Show the buses' strips (U)", false);
    showInputs = viewTool("showInputBuses", "Input Busses", "Show the input buses' strips", false);
    showInputs->setChecked(true);
    showAssign = viewTool("showAssignable", "Assignable FX", "Show the assignable FX chains' strips (E)", false);
    showMaster = viewTool("showMaster", "Master Bus", "Show the master strip (T)", false);
    showPreview = viewTool("showPreview", "Preview Bus", "Show the Preview bus (P): what the Trimmer and Start Preview play goes through it", false);
    showPreview->setChecked(false);
    vg->addSpacing(16);
    regionFx = viewTool("regionFx", "Insert FX", "The Insert FX region (I)", true);
    regionSends = viewTool("regionSends", "Sends", "The Sends region (S)", true);
    regionIO = viewTool("regionIO", "I/O", "The I/O region (H)", true);
    regionVu = viewTool("regionVu", "VU Meters", "The VU / PPM meters region (right-click a meter for its kind)", true);
    regionMeters = viewTool("regionMeters", "Meters", "The peak meters (M)", true);
    regionFaders = viewTool("regionFaders", "Faders", "Pan, mute, solo, arm and the faders (F)", true);
    vg->addSpacing(6);
    // the channel width (narrow, default, wide) as VEGAS's zoom slider
    auto* zoom = new QHBoxLayout();
    zoom->setSpacing(2);
    QToolButton* zoomOut = makeTool(viewPane, nullptr, "channelNarrower", QString(), "Narrower channels (N)", false);
    zoomOut->setProperty("tool", QVariant());
    zoomOut->setIcon(toolPicture(ZoomOutPic));
    zoomOut->setFixedSize(18, 18);
    widthSlider = new QSlider(Qt::Horizontal, viewPane);
    widthSlider->setObjectName("channelWidth");
    widthSlider->setRange(0, 2);
    widthSlider->setPageStep(1);
    widthSlider->setFocusPolicy(Qt::NoFocus);
    widthSlider->setToolTip("Narrow (N), default (D) or wide (W) channels");
    QToolButton* zoomIn = makeTool(viewPane, nullptr, "channelWider", QString(), "Wider channels (W)", false);
    zoomIn->setProperty("tool", QVariant());
    zoomIn->setIcon(toolPicture(ZoomInPic));
    zoomIn->setFixedSize(18, 18);
    zoom->addWidget(zoomOut);
    zoom->addWidget(widthSlider, 1);
    zoom->addWidget(zoomIn);
    vg->addLayout(zoom);
    sideLay->addWidget(viewPane, 0);
    QObject::connect(zoomOut, &QToolButton::clicked, owner, [this] { widthSlider->setValue(widthSlider->value() - 1); });
    QObject::connect(zoomIn, &QToolButton::clicked, owner, [this] { widthSlider->setValue(widthSlider->value() + 1); });
    for (QToolButton* b : {showTracks, showBuses, showMaster, showAssign, showInputs, showPreview})
        QObject::connect(b, &QToolButton::toggled, [this](bool) { viewChanged(); });
    QObject::connect(showAll, &QToolButton::clicked, owner, [this] { showAllChannels(); });
    auto region = [this](QToolButton* b, bool ConsoleView::*field) {
        QObject::connect(b, &QToolButton::toggled, owner, [this, field](bool on) {
            view.*field = on;
            viewChanged();
        });
    };
    region(regionFx, &ConsoleView::fx);
    region(regionSends, &ConsoleView::sends);
    region(regionIO, &ConsoleView::io);
    region(regionMeters, &ConsoleView::meters);
    region(regionFaders, &ConsoleView::faders);
    region(regionVu, &ConsoleView::vu);
    QObject::connect(widthSlider, &QSlider::valueChanged, owner, [this](int v) {
        view.width = v;
        viewChanged();
    });
    QObject::connect(channelList, &QListWidget::itemChanged, owner, [this](QListWidgetItem* it) {
        if (filling) return;
        const QString k = it->data(Qt::UserRole).toString();
        if (it->checkState() == Qt::Checked) hidden.erase(k);
        else hidden.insert(k);
        applyView();
    });
    return side;
}

// The strips (in a scroll area) and, at the right, the Preview and Master strips.
void MixerWindow::Impl::buildStrips(QHBoxLayout* body)
{
    scroll = new QScrollArea(owner);
    scroll->setObjectName("mixerScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFocusPolicy(Qt::NoFocus);
    row = new QWidget();
    row->setObjectName("mixerStrips");
    row->setAutoFillBackground(true);
    row->setPalette(owner->palette());
    rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(2);
    empty = new QLabel(row);
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet("color: #8a8a90;");
    rowLayout->addWidget(empty, 1);
    rowLayout->addStretch(0);
    scroll->setWidget(row);
    // (as VEGAS's console the master follows the other channels, the space left over is at the right; the strips'
    // scroll area is only as wide as its strips: applyView sets that)
    body->addWidget(scroll, 1000);

    masterBox = new QWidget(owner);
    masterBox->setObjectName("masterBox");
    auto* mb = new QHBoxLayout(masterBox);
    mb->setContentsMargins(0, 2, 2, 2);
    mb->setSpacing(0);
    auto* divider = new QFrame(masterBox);
    divider->setFixedWidth(2);
    divider->setStyleSheet("background: #18181a;");
    mb->addWidget(divider);
    master = new Strip(Strip::MasterStrip, 0, pb, masterBox);
    master->onFx = [this](const FxTarget& t, uint64_t slot) { openFx(t, slot); };
    master->onSelect = [this](Strip* st, Qt::KeyboardModifiers m) { select(st, m); };
    preview = new Strip(Strip::PreviewStrip, 0, pb, masterBox);
    preview->onSelect = [this](Strip* st, Qt::KeyboardModifiers m) { select(st, m); };
    preview->fader->onMeterMenu = [this](const QPoint& at) { if (meterMenuFn) meterMenuFn(at); };
    wireVu(preview);
    mb->addWidget(preview);
    mb->addWidget(master);
    body->addWidget(masterBox);
    body->addStretch(1);
    // when the strips need a scroll bar, the master's strip ends where theirs do, so the faders stay in line
    QObject::connect(scroll->horizontalScrollBar(), &QScrollBar::rangeChanged, [this, mb](int, int max) {
        mb->setContentsMargins(0, 2, 2, 2 + (max > 0 ? scroll->horizontalScrollBar()->sizeHint().height() : 0));
    });
    auto meterMenu = [this](const QPoint& at) {
        QMenu m(owner);
        fillMeterMenu(&m);
        m.exec(at);
    };
    meterMenuFn = meterMenu;
    master->fader->onMeterMenu = meterMenu;
    wireVu(master);
}

// Views: the Channel List and View Controls shown or not.
void MixerWindow::Impl::fillViewsMenu(QMenu* viewsMenu)
{
    viewsMenu->clear();
    QAction* a = viewsMenu->addAction("Channel List\tShift+C");
    a->setCheckable(true);
    a->setChecked(!channelPane->isHidden());
    QObject::connect(a, &QAction::triggered, owner, [this] { owner->handleKey(Qt::Key_C, Qt::ShiftModifier); });
    QAction* b = viewsMenu->addAction("View Controls\tShift+R");
    b->setCheckable(true);
    b->setChecked(!viewPane->isHidden());
    QObject::connect(b, &QAction::triggered, owner, [this] { owner->handleKey(Qt::Key_R, Qt::ShiftModifier); });
}

// Insert Bus: Bus A, Bus B ... (the first name not taken).
void MixerWindow::Impl::insertNewBus()
{
    AppState::get().edit("Insert Bus", [&](Project& p) {
        Bus b;
        b.id = p.newId();
        for (int k = 0; k < 26 * 27 && b.name.empty(); ++k) {
            const std::string name = k < 26 ? strfmt("Bus %c", 'A' + k) : strfmt("Bus %c%c", 'A' + k / 26 - 1, 'A' + k % 26);
            bool used = false;
            for (const auto& o : p.buses) used = used || o.name == name;
            if (!used) b.name = name;
        }
        p.buses.push_back(b);
    });
}

// Delete Bus: the selected bus or input bus (or else the last bus), with every send and output to it.
void MixerWindow::Impl::deleteSelectedBus()
{
    const Project& pr = AppState::get().project();
    if (pr.findInputBus(selectedBus)) {
        const uint64_t id = selectedBus;
        AppState::get().edit("Delete Input Bus", [&](Project& p) {
            for (auto& t : p.tracks)
                if (t.recordInput.inputBus == id) t.recordInput.inputBus = 0;
            p.inputBuses.erase(std::remove_if(p.inputBuses.begin(), p.inputBuses.end(), [&](const InputBus& b) { return b.id == id; }), p.inputBuses.end());
        });
        selectedBus = 0;
        return;
    }
    if (pr.buses.empty()) return;
    const uint64_t id = pr.findBus(selectedBus) ? selectedBus : pr.buses.back().id;
    AppState::get().edit("Delete Bus", [&](Project& p) {
        for (auto& t : p.tracks) {
            if (t.output == id) t.output = 0;
            t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [&](const Send& s) { return s.bus == id; }), t.sends.end());
        }
        for (auto& b : p.buses)
            if (b.output == id) b.output = 0;
        for (auto& b : p.inputBuses) {
            if (b.output == id) b.output = 0;
            b.sends.erase(std::remove_if(b.sends.begin(), b.sends.end(), [&](const Send& x) { return x.bus == id; }), b.sends.end());
        }
        p.buses.erase(std::remove_if(p.buses.begin(), p.buses.end(), [&](const Bus& b) { return b.id == id; }), p.buses.end());
    });
    selectedBus = 0;
}

// Insert Assignable FX: a new chain, its effects then chosen.
void MixerWindow::Impl::insertAssignableChain()
{
    const uint64_t id = reno::app::insertAssignableFx();
    if (id && owner->onOpenFx) {
        FxTarget t;
        t.kind = FxTarget::Bus;
        t.id = id;
        owner->onOpenFx(t, 0);
    }
}

MixerWindow::MixerWindow(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->owner = this;
    d->pb = playback;
    setObjectName("mixer");
    setStyleSheet(kButtonStyle);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::ClickFocus);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, kConsoleBg);
    setPalette(pal);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(d->buildToolBar());
    auto* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(d->buildSidePane());
    d->buildStrips(body);
    root->addLayout(body, 1);

    d->listener.reset(new Listener([this](int changes) {
        if (changes & kProject) {
            if (isVisible()) d->refresh();
            else d->stale = true;
        }
        if (changes & kPlayback) d->tickMeters();
    }));
    d->meterTimer = new QTimer(this);
    d->meterTimer->setInterval(33);
    QObject::connect(d->meterTimer, &QTimer::timeout, [this] { d->tickMeters(); });
    d->meterTimer->start();
    d->rate = new PollRate(this, d->meterTimer, 33);
    d->load();
    setProperty("channelListShown", !d->channelPane->isHidden());
    setProperty("viewControlsShown", !d->viewPane->isHidden());
    d->rebuild();
}

// The Mixing Console's keys, as in VEGAS (Help > Keyboard Shortcuts, "Mixing Console").
static bool consoleKey(int key, Qt::KeyboardModifiers mods)
{
    const Qt::KeyboardModifiers m = mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    switch (key) {
    case Qt::Key_F2: case Qt::Key_D: case Qt::Key_N: case Qt::Key_W: case Qt::Key_Q: case Qt::Key_U: case Qt::Key_A:
    case Qt::Key_E: case Qt::Key_P: case Qt::Key_F: case Qt::Key_I: case Qt::Key_M: case Qt::Key_S: case Qt::Key_Up: case Qt::Key_Down:
        return m == Qt::NoModifier;
    case Qt::Key_H: case Qt::Key_T: return m == Qt::NoModifier || m == Qt::ShiftModifier;
    case Qt::Key_C: case Qt::Key_L: case Qt::Key_R: return m == Qt::ShiftModifier;
    default: return false;
    }
}

bool MixerWindow::handleKey(int key, Qt::KeyboardModifiers mods)
{
    if (key == Qt::Key_Q && (mods & Qt::ControlModifier) && !(mods & (Qt::ShiftModifier | Qt::AltModifier))) {
        d->insertAudioTrack();
        return true;
    }
    if (!consoleKey(key, mods)) return false;
    const bool shift = mods & Qt::ShiftModifier;
    ConsoleView& v = d->view;
    auto toggle = [](QToolButton* b) { b->setChecked(!b->isChecked()); };
    switch (key) {
    case Qt::Key_F2: {
        for (Strip* s : d->all())
            if (d->selected.count(s->key()) && s->kind() != Strip::MasterStrip && s->kind() != Strip::PreviewStrip) {
                d->rename(s);
                break;
            }
        break;
    }
    case Qt::Key_D: v.width = ConsoleView::Default; d->viewChanged(); break;
    case Qt::Key_N: v.width = ConsoleView::Narrow; d->viewChanged(); break;
    case Qt::Key_W: v.width = ConsoleView::Wide; d->viewChanged(); break;
    case Qt::Key_Q: d->showAllChannels(); break;
    case Qt::Key_U: toggle(d->showBuses); break;
    case Qt::Key_A: toggle(d->showTracks); break;
    case Qt::Key_T:
        if (shift) { v.ticks = !v.ticks; d->viewChanged(); }
        else toggle(d->showMaster);
        break;
    case Qt::Key_E: toggle(d->showAssign); break;
    case Qt::Key_P: toggle(d->showPreview); break;
    case Qt::Key_F: v.faders = !v.faders; d->viewChanged(); break;
    case Qt::Key_I: v.fx = !v.fx; d->viewChanged(); break;
    case Qt::Key_M: v.meters = !v.meters; d->viewChanged(); break;
    case Qt::Key_S: v.sends = !v.sends; d->viewChanged(); break;
    case Qt::Key_H:
        if (shift) {
            for (const QString& k : d->selected) d->hidden.insert(k);
            d->selected.clear();
            d->viewChanged();
            d->refresh();
        } else {
            v.io = !v.io;
            d->viewChanged();
        }
        break;
    case Qt::Key_L: v.labels = !v.labels; d->viewChanged(); break;
    case Qt::Key_C: d->setPane(d->channelPane, !!d->channelPane->isHidden()); break;
    case Qt::Key_R: d->setPane(d->viewPane, !!d->viewPane->isHidden()); break;
    case Qt::Key_Up: d->nudgeFaders(1); break;
    case Qt::Key_Down: d->nudgeFaders(-1); break;
    default: return false;
    }
    return true;
}

bool MixerWindow::event(QEvent* e)
{
    if (e->type() == QEvent::Show && d->stale) {
        d->stale = false;
        d->refresh();
    }
    // the console's keys come before the main window's shortcuts of the same keys while it has focus
    if (e->type() == QEvent::ShortcutOverride) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        const bool typing = qobject_cast<QLineEdit*>(QApplication::focusWidget()) != nullptr;
        if (!typing && (consoleKey(k->key(), k->modifiers()) || (k->key() == Qt::Key_Q && k->modifiers() == Qt::ControlModifier))) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void MixerWindow::keyPressEvent(QKeyEvent* e)
{
    if (handleKey(e->key(), e->modifiers())) e->accept();
    else QWidget::keyPressEvent(e);
}

bool MixerWindow::eventFilter(QObject* o, QEvent* e)
{
    // Esc while renaming: the name is left as it was
    if (e->type() == QEvent::KeyPress && o->property("escapeCancels").toBool() && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        if (QLineEdit* ed = qobject_cast<QLineEdit*>(o)) {
            ed->blockSignals(true);
            ed->deleteLater();
            setFocus();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

MixerWindow::~MixerWindow() {}

// ================================================================================================ MasterBusWidget
// VEGAS Pro 18's Master Bus window, drawn to its measurements (the window's own tool bar is the main window's): "Master"
// with its box at the top; the effects, automation, mute and solo buttons; then from the left the fader (VEGAS's pair of
// white caps for the left and right channels, locked together), the two meter bars with the scale between them ("- 3 -"
// every 3 dB, 0 dB at the top of the bars and -60 dB at their foot, 7 pixels a dB at VEGAS's default size), the held
// peaks over the bars (nothing until something has played) and each channel's fader level under them.
namespace {

const QColor kMbBg(45, 45, 45), kMbWell(35, 35, 35), kMbText(220, 220, 220), kMbLine(70, 70, 70), kMbFill(132, 132, 132);

// The master fader's law: +12 dB at the top, 0 dB a little above two thirds of the way up (where VEGAS has it), spread
// out round 0 dB, down to silence at the bottom.
const TaperPoint kMasterTaper[] = {{12, 1.0}, {6, 0.83}, {0, 0.648}, {-6, 0.53}, {-12, 0.425}, {-18, 0.335},
                                   {-24, 0.26}, {-36, 0.15}, {-48, 0.07}, {-60, 0.02}};
const int kMasterTaperN = (int)(sizeof(kMasterTaper) / sizeof(kMasterTaper[0]));

double masterGainToPos(double g)
{
    if (!(g > 0)) return 0.0;
    const double db = 20.0 * std::log10(g);
    if (db >= kMasterTaper[0].db) return 1.0;
    if (db <= kMasterTaper[kMasterTaperN - 1].db) return kMasterTaper[kMasterTaperN - 1].pos * g / 0.001;
    for (int i = 0; i + 1 < kMasterTaperN; ++i)
        if (db >= kMasterTaper[i + 1].db) {
            const double f = (db - kMasterTaper[i + 1].db) / (kMasterTaper[i].db - kMasterTaper[i + 1].db);
            return kMasterTaper[i + 1].pos + f * (kMasterTaper[i].pos - kMasterTaper[i + 1].pos);
        }
    return 0.0;
}

double masterPosToGain(double pos)
{
    if (pos <= 0) return 0.0;
    if (pos >= 1) return kMaxGain;
    const TaperPoint& last = kMasterTaper[kMasterTaperN - 1];
    if (pos <= last.pos) return 0.001 * pos / last.pos;
    for (int i = 0; i + 1 < kMasterTaperN; ++i)
        if (pos >= kMasterTaper[i + 1].pos) {
            const double f = (pos - kMasterTaper[i + 1].pos) / (kMasterTaper[i].pos - kMasterTaper[i + 1].pos);
            return std::pow(10.0, (kMasterTaper[i + 1].db + f * (kMasterTaper[i].db - kMasterTaper[i + 1].db)) / 20.0);
        }
    return 0.0;
}

class MasterMeter : public QWidget {
public:
    explicit MasterMeter(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(150);
        setMinimumWidth(92);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }
    std::function<double()> value;
    std::function<void(Project&, double)> apply;
    std::string undoName = "Master Volume";
    MeterState meter;

    static const int kGrooveX = 21, kCapX = 8, kCapW = 25, kCapH = 13, kMeterL = 46, kMeterR = 76, kMeterW = 11, kScaleX = 57, kScaleW = 19;
    int meterTop() const { return 16; }
    int meterBottom() const { return std::max(meterTop() + 60, height() - 17); }
    int grooveTop() const { return meterTop() + 10; }
    int grooveBottom() const { return meterBottom() - 19; }
    int faderY(double gain) const { return grooveBottom() - (int)std::lround(masterGainToPos(gain) * (grooveBottom() - grooveTop())); }
    QRect capRect() const { return QRect(kCapX, faderY(value ? value() : 1.0) - kCapH / 2, kCapW, kCapH); }
    QRect peakRect(int k) const { return QRect((k ? kMeterR : kMeterL) - 7, 1, kMeterW + 14, 14); }
    int meterY(double db) const
    {
        const double f = std::clamp(-db / 60.0, 0.0, 1.0);
        return meterTop() + (int)std::lround(f * (meterBottom() - meterTop()));
    }

    void feed(float l, float r, double now, double dt)
    {
        const MeterState before = meter;
        meter.feed(l, r, now, dt);
        publish();
        if (!drawsSame(before, meter)) update();
    }
    void feedN(const float* v, int n, double now, double dt)
    {
        const MeterState before = meter;
        meter.feedN(v, n, now, dt);
        publish();
        if (!drawsSame(before, meter)) update();
    }
    void clearMeter()
    {
        if (!meter.active) return;
        for (int k = 0; k < 6; ++k) meter.level[k] = 0;
        meter.active = false;
        publish();
        update();
    }

protected:
    QSize sizeHint() const override { return QSize(96, 300); }

    void publish()
    {
        setProperty("levelLeft", meter.level[0]);
        setProperty("levelRight", meter.level[1]);
        setProperty("holdLeft", meter.hold[0]);
        setProperty("holdRight", meter.hold[1]);
        setProperty("meterChannels", meter.channels);
    }

    // The six bars of a 5.1 master where the two bars and their scale are, each channel's name under it, the loudest
    // held peak of each side over them; the fader as in stereo.
    void paintSix(QPainter& p)
    {
        const int top = meterTop(), bottom = meterBottom();
        setProperty("cap", capRect());
        setProperty("faderTop", grooveTop());
        setProperty("faderBottom", grooveBottom());
        setProperty("readout", peakRect(0).united(peakRect(1)));
        QFont f = font();
        f.setPixelSize(8);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int x0 = kMeterL, x1 = kMeterR + kMeterW, w = (x1 - x0 - 5) / 6;
        static const char* names[6] = {"L", "R", "C", "LFE", "Ls", "Rs"};
        for (int k = 0; k < 6; ++k) {
            const QRect bar(x0 + k * (w + 1), top, w, bottom - top + 1);
            p.fillRect(bar, kMbWell);
            if (meter.level[k] > 1e-6f) {
                const int y = meterY(levelDb(meter.level[k]));
                if (y < bottom) {
                    QLinearGradient grad(0, bottom, 0, top);
                    grad.setColorAt(0.0, QColor(0, 170, 60));
                    grad.setColorAt(0.75, QColor(40, 210, 60));
                    grad.setColorAt(0.80, QColor(230, 220, 40));
                    grad.setColorAt(0.94, QColor(250, 170, 30));
                    grad.setColorAt(0.95, QColor(240, 50, 40));
                    grad.setColorAt(1.0, QColor(250, 40, 40));
                    p.fillRect(QRect(bar.left(), y, bar.width(), bottom - y + 1), grad);
                }
            }
            if (meter.hold[k] > 1e-6f) {
                const double hd = levelDb(meter.hold[k]);
                p.fillRect(QRect(bar.left(), meterY(hd), bar.width(), 1), hd > -0.05 ? QColor(255, 60, 50) : QColor(230, 230, 230));
            }
            if (meter.clip[k]) p.fillRect(QRect(bar.left(), top - 3, bar.width(), 2), QColor(240, 40, 40));
            p.setPen(kMbText);
            p.drawText(QRect(bar.left() - 4, bottom + 2, bar.width() + 8, fm.height()), Qt::AlignCenter, k == 3 ? "E" : (k >= 4 ? QString(names[k]).right(1).toLower() : names[k]));
        }
        const int sides[2][4] = {{0, 2, 3, 4}, {1, 2, 3, 5}};
        for (int s = 0; s < 2; ++s) {
            float h = 0;
            bool clip = false;
            for (int c : sides[s]) {
                h = std::max(h, meter.hold[c]);
                clip = clip || meter.clip[c];
            }
            const QRect r = peakRect(s);
            if (clip) p.fillRect(r.adjusted(2, 1, -2, -1), QColor(200, 30, 30));
            p.setPen(clip ? QColor(255, 255, 255) : kMbText);
            if (h > 1e-6f || clip) p.drawText(r, Qt::AlignCenter, peakText(h));   // VEGAS shows nothing until something has played
        }
        const QRect cap = capRect();
        p.fillRect(QRect(kGrooveX, grooveTop(), 1, grooveBottom() - grooveTop() + 1), kMbLine);
        if (cap.bottom() + 1 < grooveBottom()) p.fillRect(QRect(kGrooveX - 1, cap.bottom() + 1, 1, grooveBottom() - cap.bottom() - 1), kMbFill);
        const bool hot = drag_.active || hover_;
        p.fillRect(cap, QColor(35, 35, 35));
        for (int k = 0; k < 2; ++k) {
            const QRect c(cap.left() + 1 + k * 12, cap.top() + 1, 11, cap.height() - 2);
            p.fillRect(c, hot ? QColor(255, 255, 255) : QColor(246, 246, 246));
            p.fillRect(QRect(c.left() + 3, c.top() + 2, 5, 2), QColor(35, 35, 35));
            p.fillRect(QRect(c.left() + 3, c.bottom() - 2, 5, 2), QColor(35, 35, 35));
        }
        p.setPen(kMbText);
        p.drawText(QRect(0, bottom + 2, kMeterL - 2, fm.height()), Qt::AlignCenter, value ? dbText(value()) : QString("0.0"));
    }

    void paintScale(QPainter& p, const QPoint& shift, int top, int bottom, const QFontMetrics& fm) const
    {
        for (int db = 3; db <= 57; db += 3) {
            const int y = meterY(-db) + shift.y();
            if (meterY(-db) > bottom - 3) break;
            p.fillRect(QRect(kScaleX + shift.x(), y, 2, 1), kMbText);
            p.fillRect(QRect(kScaleX + kScaleW - 2 + shift.x(), y, 2, 1), kMbText);
            const double spacing = (bottom - top) * 3.0 / 60.0;
            const int every = spacing >= fm.height() - 1 ? 1 : (2 * spacing >= fm.height() - 1 ? 2 : 4);
            if ((db / 3) % every != 0) continue;
            p.setPen(kMbText);
            p.drawText(QRect(kScaleX + 2 + shift.x(), y - fm.height() / 2 - 1, kScaleW - 4, fm.height() + 2), Qt::AlignCenter, QString::number(db));
        }
    }
    QImage scaleImg_;
    QString scaleKey_;

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), kMbBg);
        if (meter.channels == 6) {
            paintSix(p);
            return;
        }
        const int top = meterTop(), bottom = meterBottom();
        setProperty("cap", capRect());
        setProperty("faderTop", grooveTop());
        setProperty("faderBottom", grooveBottom());
        setProperty("readout", peakRect(0).united(peakRect(1)));
        QFont f = font();
        f.setPixelSize(9);
        p.setFont(f);
        const QFontMetrics fm(f);

        for (int k = 0; k < 2; ++k) {
            const QRect bar(k ? kMeterR : kMeterL, top, kMeterW, bottom - top + 1);
            p.fillRect(bar, kMbWell);
            if (meter.level[k] > 1e-6f) {
                const int y = meterY(levelDb(meter.level[k]));
                if (y < bottom) {
                    QLinearGradient grad(0, bottom, 0, top);
                    grad.setColorAt(0.0, QColor(0, 170, 60));
                    grad.setColorAt(0.75, QColor(40, 210, 60));
                    grad.setColorAt(0.80, QColor(230, 220, 40));
                    grad.setColorAt(0.94, QColor(250, 170, 30));
                    grad.setColorAt(0.95, QColor(240, 50, 40));
                    grad.setColorAt(1.0, QColor(250, 40, 40));
                    p.fillRect(QRect(bar.left(), y, bar.width(), bottom - y + 1), grad);
                }
            }
            if (meter.hold[k] > 1e-6f) {
                const double hd = levelDb(meter.hold[k]);
                p.fillRect(QRect(bar.left(), meterY(hd), bar.width(), 1), hd > -0.05 ? QColor(255, 60, 50) : QColor(230, 230, 230));
            }
        }
        const qreal dpr = devicePixelRatioF();
        if (dpr == std::floor(dpr) && dpr >= 1.0 && dpr <= 4.0) {
            const QRect area(kScaleX, top - 6, kScaleW, bottom - top + 15);
            const QString key = QString("%1 %2 %3 %4 %5 %6").arg(top).arg(bottom).arg(f.key(), kMbBg.name(), kMbText.name()).arg(dpr);
            if (scaleImg_.isNull() || scaleKey_ != key) {
                QImage img(area.size() * (int)dpr, QImage::Format_RGB32);
                img.setDevicePixelRatio(dpr);
                img.fill(kMbBg);
                QPainter ip(&img);
                ip.setFont(f);
                paintScale(ip, QPoint(-area.left(), -area.top()), top, bottom, fm);
                ip.end();
                scaleImg_ = img;
                scaleKey_ = key;
            }
            p.drawImage(area.topLeft(), scaleImg_);
        } else {
            paintScale(p, QPoint(0, 0), top, bottom, fm);
        }
        for (int k = 0; k < 2; ++k) {
            const bool clip = meter.clip[k];
            const QRect r = peakRect(k);
            if (clip) p.fillRect(r.adjusted(2, 1, -2, -1), QColor(200, 30, 30));
            p.setPen(clip ? QColor(255, 255, 255) : kMbText);
            if (meter.hold[k] > 1e-6f || clip) p.drawText(r, Qt::AlignCenter, peakText(meter.hold[k]));   // nothing until something has played
        }

        const QRect cap = capRect();
        p.fillRect(QRect(kGrooveX, grooveTop(), 1, grooveBottom() - grooveTop() + 1), kMbLine);
        if (cap.bottom() + 1 < grooveBottom()) p.fillRect(QRect(kGrooveX - 1, cap.bottom() + 1, 1, grooveBottom() - cap.bottom() - 1), kMbFill);
        const bool hot = drag_.active || hover_;
        p.fillRect(cap, QColor(35, 35, 35));
        for (int k = 0; k < 2; ++k) {
            const QRect c(cap.left() + 1 + k * 12, cap.top() + 1, 11, cap.height() - 2);
            p.fillRect(c, hot ? QColor(255, 255, 255) : QColor(246, 246, 246));
            p.fillRect(QRect(c.left() + 3, c.top() + 2, 5, 2), QColor(35, 35, 35));
            p.fillRect(QRect(c.left() + 3, c.bottom() - 2, 5, 2), QColor(35, 35, 35));
        }
        const int ly = bottom + 3;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(240, 240, 240));
        p.drawRect(QRect(kGrooveX - 4, ly + 4, 9, 6));
        p.setBrush(Qt::NoBrush);
        p.setPen(QColor(240, 240, 240));
        p.drawArc(QRect(kGrooveX - 3, ly, 6, 8), 0, 180 * 16);
        p.drawLine(kGrooveX - 3, ly + 4, kGrooveX - 3, ly + 3);
        p.drawLine(kGrooveX + 3, ly + 4, kGrooveX + 3, ly + 3);
        p.fillRect(QRect(kGrooveX, ly + 6, 1, 2), QColor(35, 35, 35));
        p.setPen(kMbText);
        const QString v = value ? dbText(value()) : QString("0.0");
        for (int k = 0; k < 2; ++k) p.drawText(QRect((k ? kMeterR : kMeterL) - 8, ly, kMeterW + 16, fm.height()), Qt::AlignCenter, v);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pos = e->position().toPoint();
        if (peakRect(0).contains(pos) || peakRect(1).contains(pos)) {
            for (int k = 0; k < 6; ++k) {
                meter.clip[k] = false;
                meter.hold[k] = 0;
            }
            publish();
            update();
            return;
        }
        if (!value || !apply) return;
        if (capRect().adjusted(-2, -2, 2, 2).contains(pos)) {
            drag_.press(masterGainToPos(value()), -pos.y());
        } else if (pos.x() < kMeterL - 4 && pos.y() >= grooveTop() - 8 && pos.y() <= grooveBottom() + 8) {
            const double v = std::clamp((double)(grooveBottom() - pos.y()) / std::max(1, grooveBottom() - grooveTop()), 0.0, 1.0);
            AppState::get().beginDrag();
            set(v, false);
            drag_.press(v, -pos.y());
            drag_.started = true;
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QPoint pos = e->position().toPoint();
        const bool h = capRect().adjusted(-2, -2, 2, 2).contains(pos);
        if (h != hover_) {
            hover_ = h;
            update();
        }
        if (!drag_.active || !value) return;
        const bool ctrl = (e->modifiers() & Qt::ControlModifier) != 0;
        double v;
        if (drag_.move(-pos.y(), grooveBottom() - grooveTop(), ctrl, masterGainToPos(value()), v)) set(v, ctrl);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        drag_.release(undoName);
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (!value || !apply || e->position().x() >= kMeterL - 4) return;
        std::function<void(Project&, double)> fn = apply;
        AppState::get().edit(undoName, [&](Project& p) { fn(p, 1.0); });
    }
    void leaveEvent(QEvent*) override
    {
        hover_ = false;
        update();
    }

private:
    void set(double v, bool ctrl)
    {
        double gain = masterPosToGain(v);
        if (!ctrl && std::fabs(v - masterGainToPos(1.0)) < 0.012) gain = 1.0;
        std::function<void(Project&, double)> fn = apply;
        AppState::get().dragUpdate([&](Project& p) { fn(p, gain); });
        AppState::get().status(undoName + ": " + dbText(gain, true).toStdString());
    }
    bool hover_ = false;
    DragValue drag_;
};

enum class MbIcon { Box, Fx, Automation, Mute, Solo };
QIcon mbIcon(MbIcon which, bool on)
{
    QPixmap pm(32, 32);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor white(240, 240, 240), dark(35, 35, 35);
    QFont f;
    f.setPixelSize(9);
    f.setBold(true);
    switch (which) {
    case MbIcon::Box:
        p.fillRect(QRectF(1, 1, 14, 14), white);
        p.fillRect(QRectF(3, 3, 10, 10), dark);
        p.fillRect(QRectF(5.5, 5.5, 5, 5), white);
        break;
    case MbIcon::Fx: {
        QFont ff("serif");
        ff.setPixelSize(13);
        ff.setItalic(true);
        p.setFont(ff);
        p.setPen(on ? QColor(80, 220, 90) : white);
        p.drawText(QRectF(0, -1, 16, 16), Qt::AlignCenter, "fx");
        break;
    }
    case MbIcon::Automation: {
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        for (int k = 0; k < 8; ++k) {
            const double a = k * 3.14159265358979 / 4.0;
            p.drawEllipse(QPointF(8 + 6 * std::cos(a), 8 + 6 * std::sin(a)), 1.6, 1.6);
        }
        p.setBrush(QColor(226, 140, 150));
        p.drawEllipse(QPointF(8, 8), 4.6, 4.6);
        p.setFont(f);
        p.setPen(dark);
        p.drawText(QRectF(3, 3, 10, 10), Qt::AlignCenter, "R");
        break;
    }
    case MbIcon::Mute:
    case MbIcon::Solo: {
        const bool mute = which == MbIcon::Mute;
        QColor disc = mute ? QColor(232, 144, 156) : QColor(196, 168, 92);
        if (on) disc = mute ? QColor(255, 80, 90) : QColor(255, 210, 60);
        p.setPen(Qt::NoPen);
        p.setBrush(disc);
        p.drawEllipse(QPointF(7.5, 8.5), 6.6, 6.6);
        p.setFont(f);
        p.setPen(dark);
        p.drawText(QRectF(1, 2, 13, 13), Qt::AlignCenter, mute ? "M" : "S");
        p.setPen(QPen(white, 1.4));
        if (mute) {
            p.setBrush(dark);
            p.drawEllipse(QPointF(12.0, 4.2), 2.4, 2.4);
        } else {
            p.drawLine(QPointF(14.2, 1.5), QPointF(14.2, 6.0));
            p.drawPoint(QPointF(14.2, 8.0));
        }
        break;
    }
    }
    return QIcon(pm);
}

}

struct MasterBusWidget::Impl {
    MasterBusWidget* owner = nullptr;
    Playback* pb = nullptr;
    QToolButton *fx = nullptr, *automation = nullptr, *mute = nullptr, *solo = nullptr;
    QLabel* title = nullptr;
    MasterMeter* meters = nullptr;
    std::unique_ptr<Listener> listener;
    QTimer* meterTimer = nullptr;
    PollRate* rate = nullptr;
    MeterFeed<MasterMeter> feed;

    // Called on every project change (each step of a drag): the icons are drawn and set only when what they show
    // changes, since setting an icon re-lays out and repaints its button.
    int shownFx = -1, shownMute = -1;      // -1: not set yet
    double shownVolume = -1;
    void refresh()
    {
        const Project& p = AppState::get().project();
        const bool hasFx = !p.audioOut.empty();
        setToolTipIfChanged(fx, chainTip(p.audioOut, "Master Bus FX"));
        if (shownFx != (int)hasFx) {
            fx->setIcon(mbIcon(MbIcon::Fx, hasFx));
            shownFx = hasFx;
        }
        setActive(fx, hasFx);
        const QSignalBlocker b(mute);
        mute->setChecked(p.masterMute);
        if (shownMute != (int)p.masterMute) {
            mute->setIcon(mbIcon(MbIcon::Mute, p.masterMute));
            shownMute = p.masterMute;
        }
        // the fader's level is the only thing of the project the meters show
        if (p.masterVolume != shownVolume) {
            shownVolume = p.masterVolume;
            meters->update();
        }
    }
    void tickMeters()
    {
        if (!owner->isVisible()) return;
        std::vector<std::pair<uint64_t, MasterMeter*>> list;
        list.push_back(std::make_pair((uint64_t)0, meters));
        feed.tick(pb, list);
        if (rate) rate->ticked(pb && pb->playing());
    }
};

MasterBusWidget::MasterBusWidget(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->owner = this;
    d->pb = playback;
    setObjectName("masterBus");
    setStyleSheet("QToolButton { background: transparent; border: 1px solid transparent; border-radius: 0px; padding: 0px; }"
                  "QToolButton:hover { background: #464646; border-color: #5a5a5a; }"
                  "QToolButton:pressed { background: #232323; }"
                  "QToolButton:checked { border-color: #12b0ff; }"
                  "QToolButton:disabled { background: transparent; }");
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, kMbBg);
    setPalette(pal);
    setMinimumWidth(96);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* top = new QHBoxLayout();
    top->setContentsMargins(8, 4, 4, 0);
    top->setSpacing(0);
    QLabel* box = new QLabel(this);
    box->setPixmap(mbIcon(MbIcon::Box, false).pixmap(16, 16));
    box->setFixedSize(16, 16);
    d->title = new QLabel("Master", this);
    d->title->setObjectName("masterTitle");
    d->title->setStyleSheet("color: #dcdcdc;");
    top->addWidget(box);
    top->addSpacing(8);
    top->addWidget(d->title, 1);
    root->addLayout(top);
    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(4, 4, 4, 0);
    buttons->setSpacing(2);
    auto button = [&](const char* name, MbIcon icon, const QString& tip) {
        QToolButton* b = new QToolButton(this);
        b->setObjectName(name);
        b->setIcon(mbIcon(icon, false));
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(18, 18);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        buttons->addWidget(b);
        return b;
    };
    d->fx = button("masterFx", MbIcon::Fx, "Master Bus FX");
    d->automation = button("masterAutomation", MbIcon::Automation, "Automation Settings: not available yet");
    d->mute = button("masterMute", MbIcon::Mute, "Mute the master");
    d->solo = button("masterSolo", MbIcon::Solo, "Solo: the master is always heard");
    d->mute->setCheckable(true);
    // these two look as they do in VEGAS; a click says why they do nothing here
    QObject::connect(d->automation, &QToolButton::clicked, [] { AppState::get().status("Automation settings are not available yet."); });
    QObject::connect(d->solo, &QToolButton::clicked, [] { AppState::get().status("The master is always heard: it has nothing to be soloed against."); });
    buttons->addStretch(1);
    root->addLayout(buttons);

    d->meters = new MasterMeter(this);
    d->meters->setObjectName("masterFader");
    d->meters->value = [] { return AppState::get().project().masterVolume; };
    d->meters->apply = [](Project& p, double v) { p.masterVolume = v; };
    d->meters->setToolTip("Master volume: drag the caps (Ctrl: finer), double-click for 0 dB; click the peak values to clear");
    root->addWidget(d->meters, 1);

    QObject::connect(d->fx, &QToolButton::clicked, [this] {
        if (!onOpenFx) return;
        FxTarget t;
        t.kind = FxTarget::AudioOut;
        onOpenFx(t, 0);
    });
    // VEGAS's right-click on the Master FX button: Post Volume (the effects after the master's volume), and the chain's
    // Enable / Bypass / Delete All
    d->fx->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(d->fx, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QMenu m(this);
        m.setObjectName("masterFxMenu");
        QAction* pre = m.addAction("Pre FX");
        QAction* post = m.addAction("Post Volume");
        post->setObjectName("postVolume");
        const bool isPost = AppState::get().project().masterFxPostFader;
        pre->setCheckable(true);
        pre->setChecked(!isPost);
        post->setCheckable(true);
        post->setChecked(isPost);
        QObject::connect(pre, &QAction::triggered, this, [] { Strip::setPostFx(Strip::MasterStrip, 0, false); });
        QObject::connect(post, &QAction::triggered, this, [] { Strip::setPostFx(Strip::MasterStrip, 0, true); });
        m.addSeparator();
        FxTarget t;
        t.kind = FxTarget::AudioOut;
        fillChainMenu(&m, t);
        FxSlots::exec(&m, d->fx->mapToGlobal(at));
    });
    QObject::connect(d->mute, &QToolButton::clicked, [] {
        AppState::get().edit("Mute Master", [](Project& p) { p.masterMute = !p.masterMute; });
    });
    d->listener.reset(new Listener([this](int changes) {
        if (changes & kProject) d->refresh();
        if (changes & kPlayback) d->tickMeters();
    }));
    d->meterTimer = new QTimer(this);
    d->meterTimer->setInterval(33);
    QObject::connect(d->meterTimer, &QTimer::timeout, [this] { d->tickMeters(); });
    d->meterTimer->start();
    d->rate = new PollRate(this, d->meterTimer, 33);
    d->refresh();
}

MasterBusWidget::~MasterBusWidget() {}

std::function<QAction*(QMenu*)>& mixerMenuHookForTests() { return FxSlots::menuHook(); }

uint64_t insertAssignableFx()
{
    uint64_t id = 0;
    AppState::get().edit("Insert Assignable FX", [&](Project& p) {
        Bus b;
        b.id = id = p.newId();
        b.assignable = true;
        for (int k = 1; k < 1000 && b.name.empty(); ++k) {
            const std::string name = strfmt("FX %d", k);
            bool used = false;
            for (const auto& o : p.buses) used = used || o.name == name;
            if (!used) b.name = name;
        }
        p.buses.push_back(b);
    });
    return id;
}

uint64_t insertInputBus()
{
    uint64_t id = 0;
    AppState::get().edit("Insert Input Bus", [&](Project& p) {
        if (p.inputBuses.size() >= 26) return;
        InputBus b;
        b.id = id = p.newId();
        for (int k = 0; k < 26 && b.name.empty(); ++k) {
            const std::string name = strfmt("Input Bus %c", 'A' + k);
            bool used = false;
            for (const auto& o : p.inputBuses) used = used || o.name == name;
            if (!used) b.name = name;
        }
        p.inputBuses.push_back(b);
    });
    return id;
}

void fillDownmixMenu(QMenu* m, Playback* pb)
{
    m->clear();
    AppState& app = AppState::get();
    const bool six = app.project().settings.surround();
    const bool mono = pb && pb->monoOutput();
    const bool stereo = !mono && (!six || app.prefs.downmixOutput);
    auto item = [&](const char* name, const QString& text, bool on, bool enabled, std::function<void()> fn) {
        QAction* a = m->addAction(text);
        a->setObjectName(name);
        a->setCheckable(true);
        a->setChecked(on);
        a->setEnabled(enabled);
        QObject::connect(a, &QAction::triggered, m, [fn] { fn(); });
    };
    item("downmix51", "5.1 Surround Output", six && !mono && !app.prefs.downmixOutput, six, [pb] {
        AppState& ap = AppState::get();
        if (pb) pb->setMonoOutput(false);
        ap.prefs.downmixOutput = false;
        ap.prefs.save();
        ap.status("Downmix Output: 5.1 surround (from the next time playing starts).");
    });
    item("downmixStereo", "Stereo Output", stereo, true, [pb, six] {
        AppState& ap = AppState::get();
        if (pb) pb->setMonoOutput(false);
        if (six) {
            ap.prefs.downmixOutput = true;
            ap.prefs.save();
        }
        ap.status(six ? "Downmix Output: stereo (from the next time playing starts)." : "Downmix Output: stereo.");
    });
    item("downmixMono", "Mono Output", mono, pb != nullptr, [pb] {
        if (pb) pb->setMonoOutput(true);
        AppState::get().status("Downmix Output: mono - left and right are heard mixed together.");
    });
}

}
}
