// Loudness Meters window; see loudnesswin.h.
#include "loudnesswin.h"
#include "toolwin.h"
#include "pollrate.h"
#include "playback.h"
#include "../engine/loudness.h"
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace reno {
namespace app {

using namespace tw;

namespace {

// The options, kept in settings (Options > Loudness Meters can set them before any window exists).
struct Options {
    bool atsc = false, wide = false, absolute = false, truePeak = true, blocking = false;
    static Options load()
    {
        QSettings s;
        Options o;
        o.atsc = s.value("loudness/mode").toString() == "atsc";
        o.wide = s.value("loudness/scale").toString() == "ebu18";
        o.absolute = s.value("loudness/absolute", false).toBool();
        o.truePeak = s.value("loudness/truePeak", true).toBool();
        o.blocking = s.value("loudness/peakBlocking", false).toBool();
        return o;
    }
    void save() const
    {
        QSettings s;
        s.setValue("loudness/mode", atsc ? "atsc" : "ebu");
        s.setValue("loudness/scale", wide ? "ebu18" : "ebu9");
        s.setValue("loudness/absolute", absolute);
        s.setValue("loudness/truePeak", truePeak);
        s.setValue("loudness/peakBlocking", blocking);
    }
    double target() const { return atsc ? -24.0 : -23.0; }
    double peakMax() const { return atsc ? -2.0 : -1.0; }
};

const QColor kMeterBack(28, 28, 28);

// VEGAS's meters: M, S, I and LRA on the EBU +9 (or +18) scale in LU about the target (LUFS with Absolute), each with
// its value above, and True peaks meters (dBFS) beside them.
class LoudnessBars : public QWidget {
public:
    explicit LoudnessBars(QWidget* parent) : QWidget(parent)
    {
        setObjectName("loudnessBars");
        setMinimumSize(220, 170);
    }
    double values[3] = {-70, -70, -70};     // momentary, short-term, integrated (LUFS)
    double range = 0;                       // LU
    double peaks[2] = {-144, -144}, peakHold[2] = {-144, -144};
    Options opt;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), dark());
        QFont f = font();
        f.setPixelSize(10);
        p.setFont(f);
        const double target = opt.target();
        const double hi = opt.wide ? 18 : 9, lo = opt.wide ? -36 : -18;          // LU about the target
        const int top = 20, bottom = height() - 16;
        auto yAtLu = [&](double lu) {
            const double t = (hi - std::min(hi, std::max(lo, lu))) / (hi - lo);
            return top + t * (bottom - top);
        };
        const int peakW = opt.truePeak ? 64 : 0;
        const int n = 4, gapW = 6;
        const int bw = std::max(12, (width() - peakW - gapW * (n + 1)) / n);
        const char* names[4] = {"M", "S", "I", "LRA"};
        for (int k = 0; k < n; ++k) {
            const int x = gapW + k * (bw + gapW);
            const QRect groove(x, top, bw, bottom - top);
            p.fillRect(groove, kMeterBack);
            double lu, shown;
            if (k < 3) {
                lu = values[k] <= LoudnessMeter::kSilence + 1e-9 ? lo - 1 : values[k] - target;
                shown = opt.absolute ? values[k] : values[k] - target;
            } else {
                // loudness range: a bar of its height from the bottom of the short-term spread
                lu = lo + range;
                shown = range;
            }
            if (lu > lo) {
                const int y = (int)std::lround(yAtLu(lu));
                QLinearGradient g(0, bottom, 0, top);
                g.setColorAt(0, k == 3 ? QColor(120, 140, 150) : QColor(0, 220, 230));
                g.setColorAt(std::max(0.0, std::min(1.0, (0 - lo) / (hi - lo))), k == 3 ? QColor(120, 140, 150) : QColor(0, 220, 230));
                g.setColorAt(1, k == 3 ? QColor(150, 160, 170) : QColor(160, 90, 230));
                p.fillRect(QRect(x, y, bw, bottom - y), g);
            }
            // the scale, inside each meter as VEGAS draws it
            p.setPen(QColor(220, 220, 220, 170));
            const int step = opt.wide ? 6 : 3;
            for (int v = (int)hi; v >= (int)lo; v -= step) {
                const int y = (int)std::lround(yAtLu(v));
                p.drawLine(x + bw - 4, y, x + bw - 1, y);
                p.drawText(QRect(x, y - 6, bw - 5, 12), Qt::AlignRight | Qt::AlignVCenter, QString::number(opt.absolute && k < 3 ? (int)std::lround(v + target) : v));
            }
            p.setPen(QColor(220, 220, 220));
            const bool silent = k < 3 && values[k] <= LoudnessMeter::kSilence + 1e-9;
            p.drawText(QRect(x - 2, 2, bw + 4, 16), Qt::AlignCenter, silent ? QString::fromUtf8("-∞") : QString::number(shown, 'f', 1));
            p.drawText(QRect(x - 4, bottom + 1, bw + 8, 14), Qt::AlignCenter, names[k]);
        }
        if (peakW) {
            // True peaks (dBFS): 3 to -57, left and right
            const int x0 = width() - peakW + 4, pw = (peakW - 12) / 2;
            auto yAtDb = [&](double db) { return top + (3.0 - std::min(3.0, std::max(-57.0, db))) / 60.0 * (bottom - top); };
            for (int c = 0; c < 2; ++c) {
                const int x = x0 + c * (pw + 4);
                p.fillRect(QRect(x, top, pw, bottom - top), kMeterBack);
                if (peaks[c] > -57) {
                    const int y = (int)std::lround(yAtDb(peaks[c]));
                    p.fillRect(QRect(x, y, pw, bottom - y), peaks[c] > opt.peakMax() ? QColor(240, 70, 60) : QColor(40, 220, 60));
                }
                p.setPen(QColor(0, 0, 0, 200));
                for (int v = 3; v >= -57; v -= 3) {
                    const int y = (int)std::lround(yAtDb(v));
                    p.drawLine(x + pw - 3, y, x + pw - 1, y);
                }
                p.setPen(QColor(220, 220, 220));
                p.drawText(QRect(x - 4, 2, pw + 8, 16), Qt::AlignCenter, peakHold[c] <= -143 ? QString::fromUtf8("-∞") : QString::number(peakHold[c], 'f', 1));
            }
            p.drawText(QRect(x0 - 6, bottom + 1, peakW, 14), Qt::AlignCenter, "True peaks (dBFS)");
        }
    }
};

QString luText(double v, const char* unit)
{
    if (v <= LoudnessMeter::kSilence + 1e-9) return QString::fromUtf8("-∞ %1").arg(unit);
    return QString("%1 %2").arg(v, 0, 'f', 1).arg(unit);
}

// A small round indicator, lit red when over the target.
class Indicator : public QWidget {
public:
    explicit Indicator(QWidget* parent) : QWidget(parent) { setFixedSize(12, 12); }
    bool on = false;
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor(200, 200, 200));
        p.setBrush(on ? QColor(240, 80, 80) : QColor(60, 60, 60));
        p.drawEllipse(QRectF(1.5, 1.5, 9, 9));
    }
};

LoudnessWindow* gLoudness = nullptr;        // for Options > Loudness Meters

}  // namespace

struct LoudnessWindow::Impl {
    Playback* pb = nullptr;
    mutable std::mutex m;
    LoudnessMeter meter;
    int rate = 48000;
    bool listening = false;
    bool resetOnPlay = true;
    bool wasPlaying = false;
    Options opt;
    double customTarget = 0;            // target from setTarget that is neither mode's (0: none)
    LoudnessBars* bars = nullptr;
    QLabel *title = nullptr, *mLbl = nullptr, *sLbl = nullptr, *iLbl = nullptr, *lraLbl = nullptr, *timeLbl = nullptr;
    QLabel *tpName = nullptr;
    Indicator *iOver = nullptr, *tpOver = nullptr;
    QTimer* timer = nullptr;
    PollRate* pollRate = nullptr;          // slows the timer when nothing is measured
    double lastSeconds = -1;           // how much sound the meter had at the last look
    bool moved = false;
    std::unique_ptr<Listener> listener;
    Alive alive = makeAlive();

    void resetLocked()
    {
        rate = AppState::get().project().settings.sampleRate > 0 ? AppState::get().project().settings.sampleRate : 48000;
        meter.reset(rate);
        meter.setPeakBlocking(opt.blocking);
    }

    void listen(bool on)
    {
        if (on == listening || !pb) return;
        listening = on;
        if (on) {
            Alive a = alive;
            pb->setOutputTap([this, a](const float* l, const float* r, int n) {
                if (!*a) return;
                std::lock_guard<std::mutex> lk(m);
                meter.process(l, r, n);
            });
        } else {
            pb->setOutputTap(Playback::OutputTap());
        }
    }

    double target() const { return customTarget != 0 ? customTarget : opt.target(); }

    void show()
    {
        Reading r;
        double peaks[2], holds[2];
        {
            std::lock_guard<std::mutex> lk(m);
            r.momentary = meter.momentary();
            r.shortTerm = meter.shortTerm();
            r.integrated = meter.integrated();
            r.range = meter.range();
            r.truePeak = meter.truePeak();
            r.seconds = meter.seconds();
            for (int c = 0; c < 2; ++c) {
                peaks[c] = meter.recentTruePeak(c);
                holds[c] = meter.channelTruePeak(c);
            }
        }
        moved = r.seconds != lastSeconds;
        lastSeconds = r.seconds;
        bars->opt = opt;
        bars->values[0] = r.momentary;
        bars->values[1] = r.shortTerm;
        bars->values[2] = r.integrated;
        bars->range = r.range;
        for (int c = 0; c < 2; ++c) {
            bars->peaks[c] = peaks[c];
            bars->peakHold[c] = holds[c];
        }
        bars->update();
        const double t = target();
        const char* unit = opt.absolute ? "LUFS" : "LU";
        auto val = [&](double v) { return luText(v <= LoudnessMeter::kSilence + 1e-9 ? v : (opt.absolute ? v : v - t), unit); };
        mLbl->setText(val(r.momentary));
        sLbl->setText(val(r.shortTerm));
        iLbl->setText(val(r.integrated));
        lraLbl->setText(QString("%1 LU").arg(r.range, 0, 'f', 1));
        iOver->on = r.integrated > LoudnessMeter::kSilence + 1e-9 && r.integrated > t + (opt.atsc ? 2.0 : 1.0);
        tpOver->on = r.truePeak > opt.peakMax();
        iOver->update();
        tpOver->update();
        tpName->setVisible(opt.truePeak);
        tpOver->setVisible(opt.truePeak);
        title->setText(opt.atsc ? "ATSC A/85" : "EBU R128");
        timeLbl->setText(qs(timecode(r.seconds, AppState::get().project().settings.fps())));
    }
    void optionsChanged()
    {
        opt.save();
        {
            std::lock_guard<std::mutex> lk(m);
            meter.setPeakBlocking(opt.blocking);
        }
        show();
    }
};

LoudnessWindow::LoudnessWindow(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    setObjectName("loudnessMeters");
    gLoudness = this;
    d->pb = playback;
    d->opt = Options::load();
    d->resetLocked();
    Impl* impl = d.get();
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(6);
    auto* top = new QHBoxLayout();
    // statistics at the left, as VEGAS has them
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    d->title = new QLabel(this);
    d->title->setObjectName("loudnessMode");
    grid->addWidget(d->title, 0, 0, 1, 3);
    auto row = [&](int r, const QString& name, const char* obj, const QString& tip) {
        QLabel* n = new QLabel(name, this);
        n->setToolTip(tip);
        QLabel* val = new QLabel(this);
        val->setObjectName(obj);
        val->setToolTip(tip);
        val->setMinimumWidth(70);
        val->setFrameShape(QFrame::StyledPanel);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(n, r, 0);
        grid->addWidget(val, r, 1);
        return val;
    };
    d->mLbl = row(1, "Momentary", "loudnessMomentary", "The loudness of the last 400 ms");
    d->sLbl = row(2, "Short", "loudnessShortTerm", "The loudness of the last 3 seconds");
    d->iLbl = row(3, "Integrated", "loudnessIntegrated", "Loudness of everything measured since playback started (gated per EBU R 128)");
    d->iOver = new Indicator(this);
    d->iOver->setObjectName("integratedOver");
    d->iOver->setToolTip("Lit when the integrated loudness is over the target");
    grid->addWidget(d->iOver, 3, 2);
    d->lraLbl = row(4, "Loudness Range", "loudnessRange", "How much the short-term loudness varies (LRA, EBU Tech 3342)");
    d->tpName = new QLabel("True Peaks", this);
    grid->addWidget(d->tpName, 5, 0);
    d->tpOver = new Indicator(this);
    d->tpOver->setObjectName("truePeakOver");
    d->tpOver->setToolTip("Lit when a true peak went over the maximum (right-click: Reset Clip)");
    grid->addWidget(d->tpOver, 5, 2);
    d->timeLbl = row(6, "Measured", "loudnessTime", "How much sound has been measured");
    grid->setRowStretch(7, 1);
    top->addLayout(grid);
    d->bars = new LoudnessBars(this);
    top->addWidget(d->bars, 1);
    v->addLayout(top, 1);

    auto* bottom = new QHBoxLayout();
    QPushButton* measure = new QPushButton("Measure", this);
    measure->setObjectName("loudnessMeasure");
    measure->setToolTip("Measure the time selection (or whole project) without playing it");
    QPushButton* reset = new QPushButton("Reset", this);
    reset->setObjectName("loudnessReset");
    reset->setToolTip("Reset Metering Engine");
    bottom->addStretch(1);
    bottom->addWidget(measure);
    bottom->addWidget(reset);
    v->addLayout(bottom);

    QObject::connect(reset, &QPushButton::clicked, this, [this] { this->reset(); });
    QObject::connect(measure, &QPushButton::clicked, this, [this] {
        AppState& app = AppState::get();
        const bool sel = app.loopEnd > app.loopStart;
        this->measure(sel ? app.loopStart : 0.0, sel ? app.loopEnd : app.project().length());
    });

    d->timer = new QTimer(this);
    d->timer->setInterval(100);
    QObject::connect(d->timer, &QTimer::timeout, this, [impl] {
        impl->show();
        if (impl->pollRate) impl->pollRate->ticked(impl->moved || (impl->pb && impl->pb->playing()));
    });
    d->timer->start();
    d->pollRate = new PollRate(this, d->timer, 100);
    d->listener.reset(new Listener([impl](int changes) {
        if (!(changes & kPlayback) || !impl->pb) return;
        const bool now = impl->pb->playing();
        // VEGAS recomputes loudness whenever playing starts
        if (now && !impl->wasPlaying && impl->resetOnPlay && impl->listening) {
            std::lock_guard<std::mutex> lk(impl->m);
            impl->resetLocked();
        }
        impl->wasPlaying = now;
    }));
    d->show();
}

LoudnessWindow::~LoudnessWindow()
{
    if (gLoudness == this) gLoudness = nullptr;
    d->alive->store(false);
    d->listen(false);
}

LoudnessWindow::Reading LoudnessWindow::reading() const
{
    std::lock_guard<std::mutex> lk(d->m);
    Reading r;
    r.momentary = d->meter.momentary();
    r.shortTerm = d->meter.shortTerm();
    r.integrated = d->meter.integrated();
    r.range = d->meter.range();
    r.truePeak = d->meter.truePeak();
    r.maxMomentary = d->meter.maxMomentary();
    r.maxShortTerm = d->meter.maxShortTerm();
    r.seconds = d->meter.seconds();
    return r;
}

void LoudnessWindow::reset()
{
    {
        std::lock_guard<std::mutex> lk(d->m);
        d->resetLocked();
    }
    d->show();
}

void LoudnessWindow::resetClip()
{
    {
        std::lock_guard<std::mutex> lk(d->m);
        d->meter.resetPeak();
    }
    d->show();
}

bool LoudnessWindow::measure(double from, double to, const std::function<bool(double)>& progress)
{
    AppState& app = AppState::get();
    if (d->pb) d->pb->pause();
    std::shared_ptr<const Project> p = app.snapshot();
    const int rate = p->settings.sampleRate > 0 ? p->settings.sampleRate : 48000;
    MediaPool pool;
    if (app.mediaOpener) pool.setOpener(app.mediaOpener);
    AudioEngine engine(pool, app.fx);
    LoudnessMeter meter(rate);
    meter.setPeakBlocking(d->opt.blocking);
    const int64_t a = (int64_t)std::llround(std::max(0.0, from) * rate), b = (int64_t)std::llround(std::max(from, to) * rate);
    std::vector<float> l(4096), r(4096);
    bool stopped = false;
    for (int64_t at = a; at < b; at += 4096) {
        const int n = (int)std::min<int64_t>(4096, b - at);
        engine.render(*p, at, n, l.data(), r.data(), false);
        meter.process(l.data(), r.data(), n);
        if (progress && ((at - a) / 4096) % 16 == 0 && !progress((double)(at - a) / std::max<int64_t>(1, b - a))) {
            stopped = true;
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lk(d->m);
        d->meter = meter;
        d->rate = rate;
    }
    d->show();
    app.status(strfmt("Loudness of %s: %.1f LUFS integrated, range %.1f LU, true peak %.1f dBTP.", timecode(std::max(0.0, to - from), p->settings.fps()).c_str(),
                      meter.integrated(), meter.range(), meter.truePeak()));
    return !stopped;
}

void LoudnessWindow::setTarget(double lufs)
{
    // the two modes' targets choose the mode; any other is kept as the target alone
    if (std::fabs(lufs + 23) < 1e-6 || std::fabs(lufs + 24) < 1e-6) {
        d->customTarget = 0;
        setAtscMode(std::fabs(lufs + 24) < 1e-6);
    } else {
        d->customTarget = lufs;
        d->show();
    }
}

double LoudnessWindow::target() const { return d->target(); }

void LoudnessWindow::setResetOnPlay(bool on) { d->resetOnPlay = on; }

bool LoudnessWindow::listening() const { return d->listening; }

void LoudnessWindow::setAtscMode(bool atsc) { d->opt.atsc = atsc; d->customTarget = 0; d->optionsChanged(); }
bool LoudnessWindow::atscMode() const { return d->opt.atsc; }
void LoudnessWindow::setWideScale(bool wide) { d->opt.wide = wide; d->optionsChanged(); }
bool LoudnessWindow::wideScale() const { return d->opt.wide; }
void LoudnessWindow::setAbsolute(bool on) { d->opt.absolute = on; d->optionsChanged(); }
bool LoudnessWindow::absolute() const { return d->opt.absolute; }
void LoudnessWindow::setShowTruePeak(bool on) { d->opt.truePeak = on; d->optionsChanged(); }
bool LoudnessWindow::showTruePeak() const { return d->opt.truePeak; }
void LoudnessWindow::setPeakBlocking(bool on) { d->opt.blocking = on; d->optionsChanged(); }
bool LoudnessWindow::peakBlocking() const { return d->opt.blocking; }
bool LoudnessWindow::integratedOver() const { return d->iOver->on; }
bool LoudnessWindow::truePeakOver() const { return d->tpOver->on; }

void LoudnessWindow::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    d->listen(true);
}

void LoudnessWindow::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    d->listen(false);
}

void LoudnessWindow::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu(this);
    menu.setObjectName("loudnessMenu");
    fillLoudnessMenu(&menu, this);
    menu.exec(e->globalPos());
}

void fillLoudnessMenu(QMenu* menu, LoudnessWindow* w)
{
    if (!w) w = gLoudness;
    menu->clear();
    const Options o = Options::load();
    auto toggle = [menu](const QString& text, const char* name, bool on, std::function<void()> fn) {
        QAction* a = menu->addAction(text);
        a->setObjectName(name);
        a->setCheckable(true);
        a->setChecked(on);
        QObject::connect(a, &QAction::triggered, menu, [fn] { fn(); });
        return a;
    };
    // each sets the option through the window if there is one, else in settings
    auto set = [w](std::function<void(LoudnessWindow*)> viaWindow, std::function<void(Options&)> viaSettings) {
        if (w) viaWindow(w);
        else {
            Options x = Options::load();
            viaSettings(x);
            x.save();
        }
    };
    toggle("EBU R 128 Mode", "loudnessEbu", !o.atsc, [set] { set([](LoudnessWindow* x) { x->setAtscMode(false); }, [](Options& x) { x.atsc = false; }); });
    toggle("ATSC A 85 Mode", "loudnessAtsc", o.atsc, [set] { set([](LoudnessWindow* x) { x->setAtscMode(true); }, [](Options& x) { x.atsc = true; }); });
    menu->addSeparator();
    QMenu* scale = menu->addMenu("Loudness Scale");
    {
        QAction* s9 = scale->addAction("EBU +9");
        s9->setObjectName("loudnessScale9");
        s9->setCheckable(true);
        s9->setChecked(!o.wide);
        QObject::connect(s9, &QAction::triggered, menu, [set] { set([](LoudnessWindow* x) { x->setWideScale(false); }, [](Options& x) { x.wide = false; }); });
        QAction* s18 = scale->addAction("EBU +18");
        s18->setObjectName("loudnessScale18");
        s18->setCheckable(true);
        s18->setChecked(o.wide);
        QObject::connect(s18, &QAction::triggered, menu, [set] { set([](LoudnessWindow* x) { x->setWideScale(true); }, [](Options& x) { x.wide = true; }); });
    }
    toggle(o.atsc ? "Absolute (-24 LUFS)" : "Absolute (-23 LUFS)", "loudnessAbsolute", o.absolute,
           [set, o] { set([o](LoudnessWindow* x) { x->setAbsolute(!o.absolute); }, [o](Options& x) { x.absolute = !o.absolute; }); });
    menu->addSeparator();
    toggle("Show True Peak Meter", "loudnessShowTruePeak", o.truePeak,
           [set, o] { set([o](LoudnessWindow* x) { x->setShowTruePeak(!o.truePeak); }, [o](Options& x) { x.truePeak = !o.truePeak; }); });
    toggle("True Peak Blocking Filter", "loudnessBlocking", o.blocking,
           [set, o] { set([o](LoudnessWindow* x) { x->setPeakBlocking(!o.blocking); }, [o](Options& x) { x.blocking = !o.blocking; }); });
    menu->addSeparator();
    QAction* clip = menu->addAction("Reset Clip");
    clip->setObjectName("loudnessResetClip");
    clip->setEnabled(w != nullptr);
    QObject::connect(clip, &QAction::triggered, menu, [w] { if (w) w->resetClip(); });
    QAction* engine = menu->addAction("Reset Metering Engine");
    engine->setObjectName("loudnessResetEngine");
    engine->setEnabled(w != nullptr);
    QObject::connect(engine, &QAction::triggered, menu, [w] { if (w) w->reset(); });
}

}  // namespace app
}  // namespace reno
