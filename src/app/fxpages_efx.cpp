// The effects window's page for Reno's eFX effects (src/audiofx/efx.h), laid out as VEGAS shows the MAGIX eFX windows: a bar with
// the effect's on/off switch, the preset list, A/B compare and Reset; a dark display with input and output level meters (and gain
// reduction, clip or modulation, as the effect has them); the effect's name with the In and Out knobs beside it; the side-chain
// switches under the display; a row of the main knobs at the bottom. The spanner swaps the knob row for the settings the
// window has no room for. Everything is drawn here; nothing is taken from the MAGIX plug-ins.
//
// Knobs: drag up or down (Shift: finer), the wheel, or a double-click for the setting's default. Each change is one undo step
// (a drag is one step). Settings are the plug-in's 0..1 values; the text under a knob is the plug-in's own wording (efx::mapText).
#include "fxpages.h"
#include "../audiofx/efx.h"
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <map>

namespace reno {
namespace app {

namespace {

using efx::ParamDef;

const double kPi = 3.14159265358979323846;

enum class Third { None, Gr24, Gr48, GrClip, Mod };
enum class Ring { FromMin, FromMax, FromMiddle };

struct KnobSpec {
    int param;
    const char* label;
    Ring ring = Ring::FromMin;
};

struct Spec {
    uint32_t id;
    const char* title;
    QColor accent;
    std::vector<KnobSpec> knobs;
    std::vector<KnobSpec> more;         // the spanner's row
    Third third = Third::None;
    bool mode = false;                  // setting 1 is a list of modes (a menu at the right)
    bool filter = false;                // side-chain filter type (15), listen (10) and internal / external (16) under the display
    int softClip = -1;                  // a round switch under the Out knob (the Compressor's Soft Clip)
    bool gateSwitches = false;          // Gate: soft knee (11), hold (17)
    int listenBy = -1;                  // a listen switch beside this knob's name (De-Esser: 10 by the frequency)
};

constexpr uint32_t fourCC(const char* s) { return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3]; }

const QColor kLime(160, 210, 40), kCyan(40, 182, 222), kOrange(240, 150, 50), kBlue(96, 150, 255);

const std::vector<Spec>& specs()
{
    static const std::vector<Spec> all = [] {
        std::vector<Spec> v;
        auto add = [&](const char* id, const char* title, QColor accent, std::vector<KnobSpec> knobs, std::vector<KnobSpec> more) -> Spec& {
            Spec s;
            s.id = fourCC(id);
            s.title = title;
            s.accent = accent;
            s.knobs = std::move(knobs);
            s.more = std::move(more);
            v.push_back(s);
            return v.back();
        };
        add("MX2A", "LIMITER", kLime, {{5, "threshold", Ring::FromMax}, {6, "release"}, {7, "clip gain"}},
            {{20, "attack"}, {21, "adaptive rel."}, {22, "peak / rms"}, {24, "input lowcut"}, {25, "sc lowcut"}, {26, "clip shape"}})
            .third = Third::GrClip;
        {
            Spec& s = add("MX27", "COMPRESSOR", kLime, {{5, "sc filter"}, {6, "threshold", Ring::FromMax}, {7, "ratio"}, {8, "attack"}, {9, "release"}},
                          {{20, "adaptive rel."}, {21, "auto makeup"}, {22, "mix"}});
            s.third = Third::Gr24;
            s.filter = true;
            s.softClip = 11;
        }
        add("MX23", "REVERB", kBlue, {{5, "size"}, {6, "time"}, {7, "damping"}, {8, "modulation"}, {9, "mix"}},
            {{20, "predelay"}, {21, "diffusion"}, {22, "low cut"}})
            .mode = true;
        add("MX2B", "TUBE STAGE", kOrange, {{5, "pre eq", Ring::FromMiddle}, {6, "gain"}, {7, "post eq", Ring::FromMiddle}},
            {{15, "stages"}, {16, "class a / ab"}, {20, "tube bias", Ring::FromMiddle}});
        {
            Spec& s = add("MX22", "PHASER", kCyan, {{5, "rate"}, {6, "depth"}, {7, "phase"}, {8, "feedback"}, {9, "mix"}}, {{20, "center freq"}});
            s.mode = true;
            s.third = Third::Mod;
        }
        {
            Spec& s = add("MX21", "CHORUS FLANGER", kCyan, {{5, "rate"}, {6, "depth"}, {7, "phase"}, {8, "feedback", Ring::FromMiddle}, {9, "mix"}},
                          {{20, "low cut"}});
            s.mode = true;
            s.third = Third::Mod;
        }
        {
            Spec& s = add("MX28", "DE-ESSER", kLime, {{5, "freq"}, {6, "reduction"}}, {{20, "filter q"}});
            s.third = Third::Gr24;
            s.listenBy = 5;
        }
        {
            Spec& s = add("MX29", "GATE", kLime, {{5, "sc filter"}, {6, "threshold"}, {7, "range"}, {8, "attack"}, {9, "release"}},
                          {{20, "look ahead"}, {21, "hp filter"}});
            s.third = Third::Gr48;
            s.filter = true;
            s.gateSwitches = true;
        }
        add("MX24", "STEREO DELAY", kCyan, {{5, "delay l"}, {6, "delay r"}, {7, "damping"}, {8, "feedback", Ring::FromMiddle}, {9, "mix"}},
            {{10, "sync"}, {20, "fb lowcut"}, {21, "noise"}, {22, "bbd stages"}, {23, "compander"}, {24, "clock drift"}})
            .mode = true;
        add("MX32", "TREMOLO PAN", kCyan, {{5, "rate"}, {6, "shape"}, {7, "phase"}, {8, "depth"}}, {{10, "sync"}}).third = Third::Mod;
        add("MX30", "VOCAL STRIP", kLime, {{5, "highpass"}, {6, "gate"}, {7, "deesser"}, {8, "compression"}, {9, "tone", Ring::FromMiddle}},
            {{20, "deess freq"}, {21, "auto makeup"}, {22, "comp attack"}, {23, "comp release"}});
        return v;
    }();
    return all;
}

const Spec* specFor(uint32_t id)
{
    for (const Spec& s : specs())
        if (s.id == id) return &s;
    return nullptr;
}

std::vector<ParamDef> defsFor(uint32_t id)
{
    switch (id) {
    case fourCC("MX2A"): return efx::limiterDefs();
    case fourCC("MX27"): return efx::compressorDefs();
    case fourCC("MX23"): return efx::reverbDefs();
    case fourCC("MX2B"): return efx::tubeStageDefs();
    case fourCC("MX22"): return efx::phaserDefs();
    case fourCC("MX21"): return efx::chorusFlangerDefs();
    case fourCC("MX28"): return efx::deEsserDefs();
    case fourCC("MX29"): return efx::gateDefs();
    case fourCC("MX24"): return efx::stereoDelayDefs();
    case fourCC("MX32"): return efx::tremoloPanDefs();
    case fourCC("MX30"): return efx::vocalStripDefs();
    default: return {};
    }
}

// the 0..1 value that picks choice k of a list setting (the middle of its stretch)
float choiceValue(const ParamDef& d, int k)
{
    if (d.kind != efx::Kind::Steps || d.nlabels < 2) return k ? 1.f : 0.f;
    if (k <= 0) return 0.f;
    if (k >= d.nlabels - 1) return 1.f;
    return (float)((d.thresholds[k - 1] + d.thresholds[k]) * 0.5);
}

int choiceOf(const ParamDef& d, float v) { return (int)std::lround(efx::mapValue(d, v)); }

// where a level in dB sits along an input / output meter (0..1): 10 dB a step down to -10, then -6, -3, 0, +3, +6
double levelPos(double db)
{
    static const double pts[][2] = {{-70, 0}, {-10, 0.6}, {-6, 0.69}, {-3, 0.77}, {0, 0.85}, {3, 0.925}, {6, 1.0}};
    if (db <= pts[0][0]) return 0;
    for (int k = 1; k < 7; ++k)
        if (db <= pts[k][0]) return pts[k - 1][1] + (db - pts[k - 1][0]) / (pts[k][0] - pts[k - 1][0]) * (pts[k][1] - pts[k - 1][1]);
    return 1;
}

// where a reduction (dB, positive) sits along a gain-reduction meter, from the right: full scale 'top' dB
double reductionPos(double db, double top)
{
    // the top half of the scale is linear to top/2, the rest spread out (24: 24 18 12 9 6 3; 48: 48 36 24 18 12 6)
    if (db <= 0) return 0;
    if (db >= top) return 1;
    if (db >= top / 2) return 0.5 + (db - top / 2) / (top / 2) * 0.5;
    return db / (top / 2) * 0.5;
}

class EfxPageImpl : public DxPage {
public:
    EfxPageImpl(QWidget* parent, const Spec& spec, std::vector<ParamDef> defs, const EfxHost& host)
        : DxPage(parent), s_(spec), defs_(std::move(defs)), h_(host)
    {
        setObjectName("efxPage");
        setFixedSize(kW, kH);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        const bool limiter = spec.third == Third::GrClip;
        displayH_ = limiter ? 252 : 242;
        meters_ = new QTimer(this);
        meters_->setInterval(33);
        QObject::connect(meters_, &QTimer::timeout, this, [this] { pollMeters(); });
        if (h_.state) slot_[0] = slot_[1] = h_.state();
        layout();
    }

    void refresh() override { update(); }

protected:
    void showEvent(QShowEvent*) override { meters_->start(); }
    void hideEvent(QHideEvent*) override { meters_->stop(); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paintBody(p);
        paintBar(p);
        paintDisplay(p);
        paintRight(p);
        paintStrip(p);
        paintKnobRow(p);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const QPointF at = e->position();
        if (e->button() != Qt::LeftButton) return;
        for (const Hit& hit : hits_) {
            if (!hit.r.contains(at)) continue;
            press(hit, at);
            return;
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (drag_ < 0) {
            const Hit* hit = hitAt(e->position());
            setCursor(hit && hit->kind == Knob ? Qt::SizeVerCursor : hit ? Qt::PointingHandCursor : Qt::ArrowCursor);
            return;
        }
        const double scale = (e->modifiers() & Qt::ShiftModifier) ? 1000.0 : 200.0;
        const double dy = dragFrom_.y() - e->position().y();
        if (std::fabs(dy) > 1e-9) {
            // (Shift pressed or let go mid-drag: start again from here so the value does not jump)
            const bool fine = (e->modifiers() & Qt::ShiftModifier) != 0;
            if (fine != fine_) {
                fine_ = fine;
                dragFrom_ = e->position();
                dragStart_ = value(drag_);
                return;
            }
            setValue(drag_, (float)std::max(0.0, std::min(1.0, dragStart_ + dy / scale)));
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (drag_ >= 0) {
            drag_ = -1;
            if (h_.end) h_.end();
            update();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        const Hit* hit = hitAt(e->position());
        if (!hit || hit->kind != Knob) {
            if (hit) press(*hit, e->position());
            return;
        }
        setValue(hit->param, defs_[(size_t)hit->param].def);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        const Hit* hit = hitAt(e->position());
        if (!hit || hit->kind != Knob) {
            e->ignore();
            return;
        }
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.002 : 0.01;
        const int notches = e->angleDelta().y() / 120;
        if (!notches) return;
        setValue(hit->param, (float)std::max(0.0, std::min(1.0, value(hit->param) + notches * step)));
        e->accept();
    }

    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ToolTip) {
            QHelpEvent* he = static_cast<QHelpEvent*>(e);
            const Hit* hit = hitAt(he->pos());
            if (hit && !hit->tip.isEmpty()) QToolTip::showText(he->globalPos(), hit->tip, this);
            else QToolTip::hideText();
            return true;
        }
        return DxPage::event(e);
    }

private:
    static constexpr int kW = 524, kH = 436;
    enum Kind { Active, PresetBox, PresetPrev, PresetNext, PresetSave, SlotA, CopyAB, SlotB, Reset, Help, Spanner, Knob, Toggle, FilterType, ModeMenu };
    struct Hit {
        Kind kind;
        QRectF r;
        int param = -1;
        QString tip;
        QPointF centre;
        double radius = 0;
        Ring ring = Ring::FromMin;
        QString label;
        bool small = false;
        int choice = -1;
    };

    // ---------------------------------------------------------------------------------------------- values
    float value(int i) const { return h_.get ? h_.get(i) : 0.f; }
    QString text(int i) const { return QString::fromStdString(efx::mapText(defs_[(size_t)i], value(i))); }
    bool isOn(int i) const
    {
        const ParamDef& d = defs_[(size_t)i];
        return d.kind == efx::Kind::Steps ? efx::mapValue(d, value(i)) >= 0.5 : value(i) >= 0.5f;
    }
    void setValue(int i, float v)
    {
        if (h_.set) h_.set(i, v);
        update();
    }
    void step(const std::string& what, const std::function<void()>& fn)
    {
        if (h_.beginEdit) h_.beginEdit(what);
        fn();
        if (h_.end) h_.end();
        update();
    }
    bool atDefaults() const
    {
        for (int k = 0; k < (int)defs_.size(); ++k)
            if (defs_[(size_t)k].kind != efx::Kind::Unused && k != efx::kVersion && std::fabs(value(k) - defs_[(size_t)k].def) > 1e-6f) return false;
        return true;
    }

    const Hit* hitAt(const QPointF& at) const
    {
        for (const Hit& hit : hits_)
            if (hit.r.contains(at)) return &hit;
        return nullptr;
    }

    // ---------------------------------------------------------------------------------------------- where things are
    QRectF display() const { return QRectF(12, 34, 352, displayH_); }
    // the In and Out knobs' height (higher where a mode list sits under them)
    double inOutY() const { return display().top() + (s_.mode ? 128 : 182); }
    double panelTop() const { return 322; }

    void layout()
    {
        hits_.clear();
        auto add = [&](Kind k, QRectF r, QString tip, int param = -1) {
            Hit hit;
            hit.kind = k;
            hit.r = r;
            hit.tip = tip;
            hit.param = param;
            hits_.push_back(hit);
            return &hits_.back();
        };
        hits_.reserve(64);
        add(Active, QRectF(4, 4, 32, 20), "Turn the effect on or off", efx::kActive);
        add(PresetBox, QRectF(40, 4, 268, 20), "Presets");
        add(PresetPrev, QRectF(308, 4, 16, 20), "Previous preset");
        add(PresetNext, QRectF(324, 4, 16, 20), "Next preset");
        add(PresetSave, QRectF(342, 4, 20, 20), "Save the settings as a preset");
        add(SlotA, QRectF(372, 4, 20, 20), "Settings A (click to compare with B)");
        add(CopyAB, QRectF(393, 4, 24, 20), "Copy these settings to the other slot");
        add(SlotB, QRectF(418, 4, 20, 20), "Settings B (click to compare with A)");
        add(Reset, QRectF(444, 4, 44, 20), "Set every setting back to its default");
        add(Help, QRectF(492, 4, 26, 20), "About this effect");
        add(Spanner, QRectF(492, 30, 26, 22), "More settings");
        // In and Out
        const double ky = inOutY();
        for (int k = 0; k < 2; ++k) {
            Hit* hit = add(Knob, QRectF(380 + k * 64, ky - 20, 48, 40), k ? "Output" : "Input", k ? 4 : 3);
            hit->centre = QPointF(404 + k * 64, ky);
            hit->radius = 15;
            hit->small = true;
            hit->label = k ? "OUT" : "IN";
        }
        if (s_.mode) add(ModeMenu, QRectF(380, display().top() + 206, 132, 22), "Mode", 1);
        if (s_.softClip >= 0) add(Toggle, QRectF(458, 286, 26, 26), QString::fromLatin1(defs_[(size_t)s_.softClip].name), s_.softClip);
        if (s_.filter) {
            for (int k = 0; k < 4; ++k) {
                Hit* hit = add(FilterType, QRectF(14 + k * 20, 284, 20, 16), QString("Side chain filter: %1").arg(filterName(k)), 15);
                hit->choice = k;
            }
            add(Toggle, QRectF(96, 290, 22, 22), "Listen to the side chain", 10);
            add(Toggle, QRectF(140, 300, 52, 18), "Side chain: internal or external", 16);
        }
        if (s_.gateSwitches) {
            add(Toggle, QRectF(226, 300, 30, 18), "Soft knee", 11);
            add(Toggle, QRectF(276, 300, 52, 18), "Hold: short or long", 17);
        }
        // the knob row (or the spanner's)
        const std::vector<KnobSpec>& row = more_ ? s_.more : s_.knobs;
        const int n = (int)row.size();
        const double gap = n >= 5 ? 96 : n == 4 ? 110 : n == 3 ? 105 : 125;
        const double first = kW / 2.0 - gap * (n - 1) / 2.0;
        for (int k = 0; k < n; ++k) {
            const KnobSpec& ks = row[(size_t)k];
            const ParamDef& d = defs_[(size_t)ks.param];
            const double cx = first + gap * k, cy = panelTop() + 56;
            const bool sw = d.kind == efx::Kind::Steps && d.nlabels == 2;
            Hit* hit = add(sw ? Toggle : Knob, sw ? QRectF(cx - 26, cy - 9, 52, 18) : QRectF(cx - 36, cy - 34, 72, 68), QString::fromLatin1(d.name), ks.param);
            hit->centre = QPointF(cx, cy);
            hit->radius = 24;
            hit->ring = ks.ring;
            hit->label = ks.label;
            if (s_.listenBy == ks.param && !more_) {
                QFontMetricsF fm(labelFont());
                const double lw = fm.horizontalAdvance(hit->label);
                Hit* l = add(Toggle, QRectF(cx + lw / 2 + 2, panelTop() + 6, 18, 16), "Listen to what is taken out", 10);
                l->small = true;
            }
        }
    }

    static QString filterName(int k)
    {
        static const char* const names[] = {"off", "low pass", "band pass", "high pass"};
        return names[k];
    }

    // ---------------------------------------------------------------------------------------------- clicks
    void press(const Hit& hit, const QPointF& at)
    {
        switch (hit.kind) {
        case Knob:
            drag_ = hit.param;
            dragFrom_ = at;
            dragStart_ = value(hit.param);
            fine_ = false;
            if (h_.begin) h_.begin(hit.param);
            break;
        case Active:
        case Toggle: {
            const int i = hit.param;
            const ParamDef& d = defs_[(size_t)i];
            const bool on = isOn(i);
            step(std::string(on ? "Turn off \"" : "Turn on \"") + d.name + "\"", [&] { setValue(i, on ? choiceValue(d, 0) : choiceValue(d, 1)); });
            break;
        }
        case FilterType:
            step("Change \"SC Filter Type\"", [&] { setValue(hit.param, (float)hit.choice / 3.f); });
            break;
        case ModeMenu: modeMenu(hit); break;
        case PresetBox: presetMenu(hit); break;
        case PresetPrev:
        case PresetNext: stepPreset(hit.kind == PresetNext ? 1 : -1); break;
        case PresetSave:
            if (h_.presetSave) h_.presetSave();
            update();
            break;
        case SlotA:
        case SlotB: pickSlot(hit.kind == SlotA ? 0 : 1); break;
        case CopyAB:
            if (h_.state) slot_[1 - cur_] = h_.state();
            update();
            break;
        case Reset:
            step("Reset", [&] {
                for (int k = 0; k < (int)defs_.size(); ++k)
                    if (k != efx::kVersion && defs_[(size_t)k].kind != efx::Kind::Unused && k != efx::kActive) setValue(k, defs_[(size_t)k].def);
            });
            break;
        case Help:
            if (h_.help) h_.help();
            break;
        case Spanner:
            more_ = !more_;
            layout();
            update();
            break;
        }
    }

    void modeMenu(const Hit& hit)
    {
        const ParamDef& d = defs_[(size_t)hit.param];
        QMenu m(this);
        const int cur = choiceOf(d, value(hit.param));
        for (int k = 0; k < d.nlabels; ++k) {
            QAction* a = m.addAction(QString::fromLatin1(d.labels[k]));
            a->setCheckable(true);
            a->setChecked(k == cur);
            a->setData(k);
        }
        QAction* a = m.exec(mapToGlobal(hit.r.bottomLeft().toPoint()));
        if (!a) return;
        const int k = a->data().toInt();
        step("Change \"Mode\"", [&] { setValue(hit.param, choiceValue(d, k)); });
    }

    // the window's preset list (its own Preset box): "Default" first, then the user's presets
    void presetMenu(const Hit& hit)
    {
        QMenu m(this);
        QAction* def = m.addAction("Default");
        def->setData(-1);
        const QStringList names = h_.presetNames ? h_.presetNames() : QStringList();
        const int cur = h_.presetCurrent ? h_.presetCurrent() : -1;
        bool sep = false;
        for (int k = 0; k < names.size(); ++k) {
            if (names[k].isEmpty()) continue;
            if (!sep) m.addSeparator();
            sep = true;
            QAction* a = m.addAction(names[k]);
            a->setData(k);
            a->setCheckable(true);
            a->setChecked(k == cur);
        }
        QAction* a = m.exec(mapToGlobal(hit.r.bottomLeft().toPoint()));
        if (!a) return;
        pickPreset(a->data().toInt());
    }

    void pickPreset(int k)
    {
        if (k < 0) {
            step("Preset Default", [&] {
                for (int i = 0; i < (int)defs_.size(); ++i)
                    if (i != efx::kVersion && defs_[(size_t)i].kind != efx::Kind::Unused) setValue(i, defs_[(size_t)i].def);
            });
            if (h_.presetPick) h_.presetPick(-1);
        } else if (h_.presetPick) {
            h_.presetPick(k);
        }
        update();
    }

    void stepPreset(int dir)
    {
        const QStringList names = h_.presetNames ? h_.presetNames() : QStringList();
        std::vector<int> order = {-1};
        for (int k = 0; k < names.size(); ++k)
            if (!names[k].isEmpty()) order.push_back(k);
        const int cur = h_.presetCurrent ? h_.presetCurrent() : -1;
        int at = 0;
        for (size_t k = 0; k < order.size(); ++k)
            if (order[k] == cur) at = (int)k;
        const int next = (at + dir + (int)order.size()) % (int)order.size();
        pickPreset(order[(size_t)next]);
    }

    void pickSlot(int which)
    {
        if (which == cur_ || !h_.state || !h_.setState) {
            cur_ = which;
            update();
            return;
        }
        slot_[cur_] = h_.state();
        cur_ = which;
        h_.setState(slot_[cur_], which ? "Settings B" : "Settings A");
        update();
    }

    // ---------------------------------------------------------------------------------------------- meters
    void pollMeters()
    {
        if (!h_.meter) return;
        bool changed = false;
        for (int k = 0; k < 7; ++k) {
            const float v = h_.meter(k);
            float& shown = shown_[k];
            float next;
            if (k == efx::EfxPlugin::MeterMod) next = v;
            else if (k == efx::EfxPlugin::MeterGain || k == efx::EfxPlugin::MeterClip) {
                // reduction in dB, falling back slowly
                const float db = v > 0.f ? (float)std::max(0.0, -20.0 * std::log10(std::max(1e-6f, v))) : 0.f;
                next = std::max(db, shown - 0.8f);
            } else {
                next = std::max(v, shown * 0.82f);
                if (next < 1e-5f) next = 0.f;
                if (v > hold_[k] || ++holdAge_[k] > 45) { hold_[k] = v; holdAge_[k] = 0; }
            }
            if (next != shown) {
                shown = next;
                changed = true;
            }
        }
        if (changed) update(display().toAlignedRect().adjusted(-2, -2, 2, 2));
    }

    // ---------------------------------------------------------------------------------------------- drawing
    QFont labelFont() const
    {
        QFont f = font();
        f.setPixelSize(13);
        return f;
    }
    QFont smallFont() const
    {
        QFont f = font();
        f.setPixelSize(9);
        return f;
    }

    void paintBody(QPainter& p)
    {
        p.fillRect(rect(), QColor(58, 62, 66));
        // the knob panel: lighter, with a soft edge at the top
        QRectF panel(0, panelTop(), kW, kH - panelTop());
        p.fillRect(panel, QColor(84, 89, 94));
        p.fillRect(QRectF(0, panelTop(), kW, 1), QColor(110, 116, 122));
    }

    void button(QPainter& p, const QRectF& r, const QString& text, bool lit, bool raised = true)
    {
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        if (lit) {
            g.setColorAt(0, QColor(110, 170, 225));
            g.setColorAt(1, QColor(70, 130, 195));
        } else {
            g.setColorAt(0, raised ? QColor(96, 101, 106) : QColor(48, 51, 54));
            g.setColorAt(1, raised ? QColor(70, 74, 78) : QColor(40, 43, 46));
        }
        p.setPen(QPen(QColor(30, 32, 34), 1));
        p.setBrush(g);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        if (!text.isEmpty()) {
            QFont f = font();
            f.setPixelSize(11);
            f.setBold(true);
            p.setFont(f);
            p.setPen(lit ? QColor(20, 30, 45) : QColor(225, 228, 230));
            p.drawText(r, Qt::AlignCenter, text);
        }
    }

    void paintBar(QPainter& p)
    {
        for (const Hit& hit : hits_) {
            const QRectF& r = hit.r;
            switch (hit.kind) {
            case Active: {
                const bool on = isOn(efx::kActive);
                button(p, r, QString(), false, on);
                QPainterPath path;
                path.moveTo(r.left() + 7, r.bottom() - 6);
                path.lineTo(r.left() + 13, r.bottom() - 6);
                path.lineTo(r.left() + 19, r.top() + 6);
                path.lineTo(r.right() - 7, r.top() + 6);
                p.setPen(QPen(on ? QColor(235, 238, 240) : QColor(120, 124, 128), 1.6));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
                break;
            }
            case PresetBox: {
                p.setPen(QPen(QColor(24, 26, 28), 1));
                p.setBrush(QColor(84, 89, 94));
                p.drawRect(QRectF(r.left(), r.top(), 322, r.height()).adjusted(0.5, 0.5, -0.5, -0.5));
                QFont f = font();
                f.setPixelSize(12);
                f.setBold(true);
                p.setFont(f);
                p.setPen(QColor(236, 238, 240));
                p.drawText(r.adjusted(6, 0, -18, 0), Qt::AlignVCenter | Qt::AlignLeft, presetText());
                arrow(p, QPointF(r.right() - 9, r.center().y()), 2);
                break;
            }
            case PresetPrev: arrow(p, r.center(), 1); break;
            case PresetNext: arrow(p, r.center(), 0); break;
            case PresetSave: disk(p, r.center()); break;
            case SlotA: button(p, r, "A", cur_ == 0); break;
            case SlotB: button(p, r, "B", cur_ == 1); break;
            case CopyAB: {
                button(p, r, QString(), false);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(225, 228, 230));
                p.drawEllipse(QPointF(r.left() + 7, r.center().y()), 2, 2);
                arrow(p, QPointF(r.right() - 8, r.center().y()), 0);
                break;
            }
            case Reset: button(p, r, "RESET", false); break;
            case Help: button(p, r, "?", false); break;
            case Spanner: spanner(p, r.center(), more_); break;
            default: break;
            }
        }
    }

    QString presetText() const
    {
        const QStringList names = h_.presetNames ? h_.presetNames() : QStringList();
        const int cur = h_.presetCurrent ? h_.presetCurrent() : -1;
        if (cur >= 0 && cur < names.size() && !names[cur].isEmpty()) return names[cur];
        return atDefaults() ? QString("Default") : QString("*Default");
    }

    // 0 right, 1 left, 2 down
    void arrow(QPainter& p, QPointF c, int dir)
    {
        QPolygonF t;
        if (dir == 0) t << QPointF(c.x() - 3, c.y() - 5) << QPointF(c.x() + 3, c.y()) << QPointF(c.x() - 3, c.y() + 5);
        else if (dir == 1) t << QPointF(c.x() + 3, c.y() - 5) << QPointF(c.x() - 3, c.y()) << QPointF(c.x() + 3, c.y() + 5);
        else t << QPointF(c.x() - 4, c.y() - 2) << QPointF(c.x() + 4, c.y() - 2) << QPointF(c.x(), c.y() + 3);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(225, 228, 230));
        p.drawPolygon(t);
    }

    void disk(QPainter& p, QPointF c)
    {
        const QRectF r(c.x() - 7, c.y() - 7, 14, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(225, 228, 230));
        p.drawRoundedRect(r, 1.5, 1.5);
        p.setBrush(QColor(58, 62, 66));
        p.drawRect(QRectF(r.left() + 3, r.top() + 1.5, 8, 4.5));
        p.drawRect(QRectF(r.left() + 3, r.top() + 8, 8, 5));
        p.setBrush(QColor(225, 228, 230));
        p.drawRect(QRectF(r.left() + 7.5, r.top() + 2.2, 2, 3));
    }

    void spanner(QPainter& p, QPointF c, bool lit)
    {
        p.save();
        p.translate(c);
        p.rotate(-45);
        const QColor col = lit ? s_.accent : QColor(200, 204, 208);
        p.setPen(QPen(col, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(0, -2), QPointF(0, 8));
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        QPainterPath head;
        head.addEllipse(QPointF(0, -5), 5, 5);
        QPainterPath notch;
        notch.addRect(QRectF(-1.8, -11, 3.6, 6));
        p.drawPath(head.subtracted(notch));
        p.restore();
    }

    void scale(QPainter& p, double x0, double w, double y, const std::vector<std::pair<double, const char*>>& marks, bool below = false)
    {
        p.setFont(smallFont());
        for (const auto& m : marks) {
            const double x = x0 + m.first * w;
            p.setPen(QColor(128, 134, 140));
            p.drawLine(QPointF(x, y + (below ? 2 : -9)), QPointF(x, y + (below ? 8 : -2)));
            p.setPen(QColor(150, 156, 162));
            p.drawText(QRectF(x + 2, y + (below ? 0 : -12), 30, 12), Qt::AlignLeft | Qt::AlignVCenter, m.second);
        }
    }

    // a bar of little lit segments; 'fromRight' fills from the right end
    void bar(QPainter& p, const QRectF& r, double fill, double redFrom, bool fromRight, QColor lit, QColor dim, QColor redLit, QColor redDim, double markAt)
    {
        const double seg = 3.0;
        const int n = (int)(r.width() / seg);
        for (int k = 0; k < n; ++k) {
            const double x = r.left() + k * seg;
            const double f = (k + 0.5) / n;
            const bool red = f >= redFrom;
            const bool on = fromRight ? (1 - f) <= fill : f <= fill;
            QColor c = on ? (red ? redLit : lit) : (red ? redDim : dim);
            p.fillRect(QRectF(x, r.top(), seg - 1, r.height()), c);
        }
        if (markAt >= 0) {
            const double x = r.left() + std::min(1.0, std::max(0.0, markAt)) * (n * seg - 2);
            p.fillRect(QRectF(x, r.top() - 1, 2, r.height() + 2), lit.lighter(130));
        }
    }

    void paintDisplay(QPainter& p)
    {
        const QRectF d = display();
        p.setPen(QPen(QColor(18, 19, 21), 2));
        p.setBrush(QColor(30, 33, 36));
        p.drawRoundedRect(d, 9, 9);
        p.setPen(QPen(QColor(70, 74, 78), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(d.adjusted(-1.5, -1.5, 1.5, 1.5), 10, 10);

        const double x0 = d.left() + 20, w = d.width() - 70;
        const QColor gl(176, 208, 50), gd(44, 54, 20), rl(255, 80, 50), rd(60, 26, 22);
        const std::vector<std::pair<double, const char*>> levels = {
            {levelPos(-70), "70"}, {levelPos(-60), "60"}, {levelPos(-50), "50"}, {levelPos(-40), "40"}, {levelPos(-30), "30"}, {levelPos(-20), "20"},
            {levelPos(-10), "10"}, {levelPos(-6), "6"},   {levelPos(-3), "3"},   {levelPos(0), "0"},    {levelPos(3), "3"},    {levelPos(6), "6"}};
        auto labelLR = [&](double y, const char* name) {
            p.setFont(smallFont());
            p.setPen(QColor(205, 210, 214));
            QFont f = font();
            f.setPixelSize(11);
            p.setFont(f);
            p.drawText(QRectF(x0 + w + 6, y - 2, 12, 12), Qt::AlignCenter, "L");
            p.drawText(QRectF(x0 + w + 6, y + 10, 12, 12), Qt::AlignCenter, "R");
            // (the Limiter has its peak readings there: the name goes above the letters)
            if (s_.third == Third::GrClip) p.drawText(QRectF(x0 + w + 20, y - 15, 32, 14), Qt::AlignLeft | Qt::AlignVCenter, name);
            else p.drawText(QRectF(x0 + w + 20, y + 3, 32, 14), Qt::AlignLeft | Qt::AlignVCenter, name);
        };
        auto pair = [&](double y, int l, int r, const char* name) {
            scale(p, x0, w, y, levels);
            for (int c = 0; c < 2; ++c) {
                const int k = c ? r : l;
                const double db = shown_[k] > 0 ? 20.0 * std::log10(shown_[k]) : -200.0;
                const double hdb = hold_[k] > 0 ? 20.0 * std::log10(hold_[k]) : -200.0;
                bar(p, QRectF(x0, y + c * 12, w, 7), levelPos(db), levelPos(0), false, gl, gd, rl, rd, levelPos(hdb));
                if (s_.third == Third::GrClip) {
                    p.setFont(smallFont());
                    p.setPen(QColor(176, 208, 50));
                    p.drawText(QRectF(x0 + w + 20, y - 2 + c * 12, 30, 12), Qt::AlignLeft | Qt::AlignVCenter,
                               hold_[k] > 1e-5f ? QString::number(hdb, 'f', 1) : QString("-"));
                }
            }
            labelLR(y, name);
        };
        const double top = d.top() + 52;
        pair(top, efx::EfxPlugin::MeterInL, efx::EfxPlugin::MeterInR, "IN");
        pair(top + 57, efx::EfxPlugin::MeterOutL, efx::EfxPlugin::MeterOutR, "OUT");
        if (s_.third == Third::GrClip) {
            p.setFont(smallFont());
            p.setPen(QColor(176, 208, 50));
            p.drawText(QRectF(x0 + w + 20, top + 81, 30, 12), Qt::AlignLeft | Qt::AlignVCenter, "RMS");
        }
        const double y3 = top + 120;
        auto grRow = [&](double y, double topDb, const std::vector<std::pair<double, const char*>>& marks, int meter, const char* name, bool below) {
            if (!below) scale(p, x0, w, y, marks);
            bar(p, QRectF(x0, y, w, 7), reductionPos(shown_[meter], topDb), 2, true, gl, gd, rl, rd, -1);
            p.fillRect(QRectF(x0 + w - 2, y - 1, 2, 9), gl.lighter(130));
            if (below) scale(p, x0, w, y + 7, marks, true);
            QFont f = font();
            f.setPixelSize(11);
            p.setFont(f);
            p.setPen(QColor(205, 210, 214));
            p.drawText(QRectF(x0 + w + 20, y - 3, 40, 14), Qt::AlignLeft | Qt::AlignVCenter, name);
        };
        auto marksFor = [](double topDb, std::vector<std::pair<double, const char*>> m) {
            for (auto& x : m) x.first = 1 - reductionPos(std::atof(x.second), topDb);
            return m;
        };
        switch (s_.third) {
        case Third::Gr24: {
            const auto m = marksFor(24, {{0, "24"}, {0, "18"}, {0, "12"}, {0, "9"}, {0, "6"}, {0, "3"}});
            grRow(y3, 24, m, efx::EfxPlugin::MeterGain, "", false);
            grRow(y3 + 12, 24, {}, efx::EfxPlugin::MeterGain, "", false);
            labelLR(y3, "GR");
            break;
        }
        case Third::Gr48: {
            const auto m = marksFor(48, {{0, "48"}, {0, "36"}, {0, "24"}, {0, "18"}, {0, "12"}, {0, "6"}});
            grRow(y3, 48, m, efx::EfxPlugin::MeterGain, "", false);
            grRow(y3 + 12, 48, {}, efx::EfxPlugin::MeterGain, "", false);
            labelLR(y3, "GR");
            // open / closed lights
            const bool closed = shown_[efx::EfxPlugin::MeterGain] > 1.f;
            const double ly = y3 + 30;
            p.fillRect(QRectF(x0 + w - 42, ly, 7, 7), closed ? QColor(230, 40, 30) : QColor(80, 24, 20));
            p.fillRect(QRectF(x0 + w - 30, ly, 7, 7), closed ? QColor(40, 54, 20) : QColor(140, 200, 40));
            p.setPen(QPen(QColor(150, 156, 162), 1));
            p.drawLine(QPointF(x0 + w - 18, ly), QPointF(x0 + w - 18, ly + 9));
            p.drawLine(QPointF(x0 + w - 18, ly + 9), QPointF(x0 + w - 4, ly + 9));
            p.fillRect(QRectF(x0 + w - 16, ly, 7, 7), QColor(40, 54, 20));
            break;
        }
        case Third::GrClip: {
            const auto m = marksFor(24, {{0, "24"}, {0, "18"}, {0, "12"}, {0, "9"}, {0, "6"}, {0, "3"}});
            grRow(y3, 24, m, efx::EfxPlugin::MeterGain, "GR", false);
            const auto c = marksFor(12, {{0, "12"}, {0, "9"}, {0, "6"}, {0, "4.5"}, {0, "3"}, {0, "1.5"}});
            grRow(y3 + 12, 12, c, efx::EfxPlugin::MeterClip, "CLIP", true);
            break;
        }
        case Third::Mod: {
            const double y = y3 + 22;
            const QRectF r(x0, y, w, 10);
            const double at = shown_[efx::EfxPlugin::MeterMod];
            const double seg = 7.0;
            const int n = (int)(r.width() / seg);
            const int lit = (int)std::lround(at * (n - 1));
            for (int k = 0; k < n; ++k)
                p.fillRect(QRectF(r.left() + k * seg, r.top(), seg - 2, r.height()), k == lit ? QColor(70, 210, 255) : QColor(30, 74, 96));
            QFont f = font();
            f.setPixelSize(11);
            p.setFont(f);
            p.setPen(QColor(205, 210, 214));
            p.drawText(QRectF(x0 + w + 20, y - 2, 40, 14), Qt::AlignLeft | Qt::AlignVCenter, "MOD");
            break;
        }
        case Third::None: break;
        }
    }

    void paintRight(QPainter& p)
    {
        const QRectF d = display();
        // the name
        QFont small = font();
        small.setPixelSize(13);
        small.setBold(true);
        p.setFont(small);
        p.setPen(QColor(232, 235, 238));
        p.drawText(QRectF(380, d.top() + 22, 130, 16), Qt::AlignLeft | Qt::AlignVCenter, "eFX");
        QFont big = font();
        big.setPixelSize(14);
        big.setBold(true);
        big.setLetterSpacing(QFont::PercentageSpacing, 104);
        while (big.pixelSize() > 9 && QFontMetricsF(big).horizontalAdvance(s_.title) > 136) big.setPixelSize(big.pixelSize() - 1);
        p.setFont(big);
        p.setPen(s_.accent);
        p.drawText(QRectF(380, d.top() + 38, 140, 18), Qt::AlignLeft | Qt::AlignVCenter, s_.title);
        // In and Out: names, knobs, readings in a box under them
        const QRectF box(378, inOutY() + 22, 128, 20);
        p.setPen(QPen(QColor(24, 26, 28), 1));
        p.setBrush(QColor(44, 47, 50));
        p.drawRoundedRect(box, 4, 4);
        for (const Hit& hit : hits_) {
            if (hit.kind != Knob || !hit.small) continue;
            QFont f = font();
            f.setPixelSize(12);
            p.setFont(f);
            p.setPen(QColor(220, 224, 228));
            p.drawText(QRectF(hit.centre.x() - 30, hit.centre.y() - 40, 60, 14), Qt::AlignCenter, hit.label);
            knob(p, hit, true);
            f.setPixelSize(11);
            p.setFont(f);
            p.setPen(QColor(220, 224, 228));
            p.drawText(QRectF(hit.centre.x() - 32, box.top(), 64, box.height()), Qt::AlignCenter, text(hit.param));
        }
        if (s_.mode) {
            const ParamDef& d1 = defs_[1];
            for (const Hit& hit : hits_) {
                if (hit.kind != ModeMenu) continue;
                QFont f = font();
                f.setPixelSize(12);
                p.setFont(f);
                p.setPen(QColor(220, 224, 228));
                p.drawText(QRectF(hit.r.left(), hit.r.top() - 16, 80, 14), Qt::AlignLeft | Qt::AlignVCenter, "mode");
                button(p, hit.r, QString(), false);
                f.setPixelSize(12);
                p.setFont(f);
                p.setPen(QColor(232, 235, 238));
                p.drawText(hit.r.adjusted(8, 0, -16, 0), Qt::AlignCenter, QString::fromLatin1(d1.labels[choiceOf(d1, value(1))]));
                arrow(p, QPointF(hit.r.right() - 10, hit.r.center().y()), 2);
            }
        }
    }

    void paintStrip(QPainter& p)
    {
        for (const Hit& hit : hits_) {
            if (hit.kind == FilterType) {
                const int cur = (int)std::lround(value(15) * 3.0);
                const bool on = hit.choice == cur;
                filterIcon(p, hit.r, hit.choice, on ? s_.accent : QColor(200, 204, 208));
                if (hit.choice == 3) {
                    // the slider under the four shapes, its knob at the chosen one
                    const QRectF track(14, 304, 80, 14);
                    p.setPen(QPen(QColor(24, 26, 28), 1));
                    p.setBrush(QColor(36, 38, 41));
                    p.drawRoundedRect(track, 7, 7);
                    const double x = track.left() + 10 + cur * 20;
                    roundKnob(p, QPointF(x, track.center().y()), 7);
                }
                continue;
            }
            if (hit.kind != Toggle || hit.param < 0) continue;
            if (hit.r.top() >= panelTop()) continue;           // the knob row draws its own
            const int i = hit.param;
            const bool on = isOn(i);
            if (i == 10) {
                // listen: a round button with a speaker beside it
                speaker(p, QPointF(hit.r.right() + 8, hit.r.center().y()), on ? s_.accent : QColor(200, 204, 208));
                roundKnob(p, hit.r.center(), 8, on ? s_.accent : QColor());
                continue;
            }
            if (i == s_.softClip) {
                textAbove(p, hit.r, "soft clip", 18);
                roundKnob(p, hit.r.center(), 8, on ? s_.accent : QColor());
                // a line from it up to the Out reading
                p.setPen(QPen(QColor(150, 156, 162), 1));
                const double bx = 500, by = inOutY() + 42;
                p.drawLine(QPointF(hit.r.right(), hit.r.center().y()), QPointF(bx, hit.r.center().y()));
                p.drawLine(QPointF(bx, hit.r.center().y()), QPointF(bx, by));
                continue;
            }
            const char* name = i == 16 ? "sidechain" : i == 11 ? "soft knee" : i == 17 ? "hold" : "";
            textAbove(p, hit.r, name, 16);
            if (i == 11) {
                roundKnob(p, hit.r.center(), 8, on ? s_.accent : QColor());
                continue;
            }
            pill(p, hit.r, on, i == 17 ? "short" : "int", i == 17 ? "long" : "ext");
        }
    }

    void textAbove(QPainter& p, const QRectF& r, const char* text, double up)
    {
        QFont f = font();
        f.setPixelSize(12);
        p.setFont(f);
        p.setPen(QColor(220, 224, 228));
        p.drawText(QRectF(r.center().x() - 40, r.top() - up, 80, 14), Qt::AlignCenter, text);
    }

    // a two-way switch: a dark pill with the knob at one end, the two names either side
    void pill(QPainter& p, const QRectF& r, bool right, const char* left, const char* rightName)
    {
        const QRectF track(r.center().x() - 13, r.top() + 2, 26, 14);
        p.setPen(QPen(QColor(24, 26, 28), 1));
        p.setBrush(QColor(36, 38, 41));
        p.drawRoundedRect(track, 7, 7);
        roundKnob(p, QPointF(right ? track.right() - 7 : track.left() + 7, track.center().y()), 6.5);
        QFont f = font();
        f.setPixelSize(11);
        p.setFont(f);
        p.setPen(QColor(205, 210, 214));
        p.drawText(QRectF(track.left() - 34, track.top(), 32, 14), Qt::AlignRight | Qt::AlignVCenter, left);
        p.drawText(QRectF(track.right() + 3, track.top(), 32, 14), Qt::AlignLeft | Qt::AlignVCenter, rightName);
    }

    void roundKnob(QPainter& p, QPointF c, double r, QColor ring = QColor())
    {
        if (ring.isValid()) {
            p.setPen(QPen(ring, 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, r + 2, r + 2);
        }
        QRadialGradient g(c - QPointF(r * 0.35, r * 0.4), r * 1.6);
        g.setColorAt(0, QColor(150, 155, 160));
        g.setColorAt(1, QColor(52, 55, 58));
        p.setPen(QPen(QColor(20, 22, 24), 1));
        p.setBrush(g);
        p.drawEllipse(c, r, r);
    }

    void speaker(QPainter& p, QPointF c, QColor col)
    {
        QPolygonF s;
        s << QPointF(c.x() - 4, c.y() - 2) << QPointF(c.x() - 2, c.y() - 2) << QPointF(c.x() + 2, c.y() - 5) << QPointF(c.x() + 2, c.y() + 5)
          << QPointF(c.x() - 2, c.y() + 2) << QPointF(c.x() - 4, c.y() + 2);
        p.setPen(QPen(col, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(s);
    }

    void filterIcon(QPainter& p, const QRectF& r, int k, QColor col)
    {
        const double l = r.left() + 3, rt = r.right() - 3, mid = r.center().y(), top = r.top() + 3, bot = r.bottom() - 3;
        QPainterPath path;
        switch (k) {
        case 0: path.moveTo(l, mid); path.lineTo(rt, mid); break;
        case 1: path.moveTo(l, bot); path.lineTo(l + 4, bot); path.cubicTo(l + 7, bot, l + 7, top, rt - 4, top); path.lineTo(rt, top); break;
        case 2: path.moveTo(l, bot); path.cubicTo(l + 5, bot, r.center().x() - 2, top, r.center().x(), top); path.cubicTo(r.center().x() + 2, top, rt - 5, bot, rt, bot); break;
        default: path.moveTo(l, top); path.lineTo(l + 4, top); path.cubicTo(l + 7, top, l + 7, bot, rt - 4, bot); path.lineTo(rt, bot); break;
        }
        p.setPen(QPen(col, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    // a knob: a ring of little lights round a dark cap with a white line (In and Out: a small light cap and no ring)
    void knob(QPainter& p, const Hit& hit, bool small)
    {
        const double v = value(hit.param);
        const QPointF c = hit.centre;
        const double a0 = 225.0, sweep = 270.0;
        const double ang = (a0 - sweep * v) * kPi / 180.0;
        if (!small) {
            const int n = 27;
            for (int k = 0; k < n; ++k) {
                const double f = (double)k / (n - 1);
                bool on;
                switch (hit.ring) {
                case Ring::FromMax: on = f >= v - 1e-6; break;
                case Ring::FromMiddle: on = v >= 0.5 ? (f >= 0.5 - 1e-6 && f <= v + 1e-6) : (f <= 0.5 + 1e-6 && f >= v - 1e-6); break;
                default: on = f <= v + 1e-6; break;
                }
                if (hit.ring == Ring::FromMax && v >= 0.999) on = false;
                const double a = (a0 - sweep * f) * kPi / 180.0;
                const QPointF dir(std::cos(a), -std::sin(a));
                p.setPen(QPen(on ? s_.accent : QColor(40, 43, 46), 3.2, Qt::SolidLine, Qt::FlatCap));
                p.drawLine(c + dir * 28.0, c + dir * 33.0);
            }
        }
        const double r = small ? 13 : 23;
        // shadow, then the cap
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 70));
        p.drawEllipse(c + QPointF(1.5, 2.5), r, r);
        QRadialGradient g(c - QPointF(r * 0.3, r * 0.45), r * 1.5);
        if (small) {
            g.setColorAt(0, QColor(236, 238, 240));
            g.setColorAt(1, QColor(130, 134, 138));
        } else {
            g.setColorAt(0, QColor(112, 117, 122));
            g.setColorAt(0.6, QColor(62, 66, 70));
            g.setColorAt(1, QColor(38, 41, 44));
        }
        p.setPen(QPen(QColor(22, 24, 26), 1.2));
        p.setBrush(g);
        p.drawEllipse(c, r, r);
        const QPointF dir(std::cos(ang), -std::sin(ang));
        p.setPen(QPen(small ? QColor(40, 42, 44) : QColor(236, 238, 240), small ? 2.4 : 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + dir * (r * 0.25), c + dir * (r * 0.9));
    }

    void paintKnobRow(QPainter& p)
    {
        for (const Hit& hit : hits_) {
            if (hit.r.top() < panelTop() || hit.param < 0) continue;
            if (hit.small && hit.kind == Toggle) {
                // the De-Esser's listen switch beside its frequency
                roundKnob(p, hit.r.center(), 6, isOn(hit.param) ? s_.accent : QColor());
                speaker(p, QPointF(hit.r.right() + 8, hit.r.center().y()), isOn(hit.param) ? s_.accent : QColor(200, 204, 208));
                continue;
            }
            QFont f = labelFont();
            p.setFont(f);
            p.setPen(QColor(232, 235, 238));
            p.drawText(QRectF(hit.centre.x() - 50, panelTop() + 6, 100, 16), Qt::AlignCenter, hit.label);
            if (hit.kind == Toggle) {
                const ParamDef& d = defs_[(size_t)hit.param];
                pill(p, hit.r, isOn(hit.param), d.labels[0], d.labels[1]);
                continue;
            }
            knob(p, hit, false);
            QFont t = font();
            t.setPixelSize(11);
            p.setFont(t);
            p.setPen(QColor(232, 235, 238));
            p.drawText(QRectF(hit.centre.x() - 50, panelTop() + 94, 100, 14), Qt::AlignCenter, text(hit.param));
        }
    }

    const Spec& s_;
    std::vector<ParamDef> defs_;
    EfxHost h_;
    std::vector<Hit> hits_;
    QTimer* meters_ = nullptr;
    float shown_[7] = {0, 0, 0, 0, 0, 0, 0.5f};
    float hold_[7] = {0};
    int holdAge_[7] = {0};
    double displayH_ = 242;
    int drag_ = -1;
    QPointF dragFrom_;
    double dragStart_ = 0;
    bool fine_ = false;
    bool more_ = false;
    std::string slot_[2];
    int cur_ = 0;
};

}  // namespace

bool hasEfxPage(uint32_t vstId) { return specFor(vstId) != nullptr; }

DxPage* makeEfxPage(QWidget* parent, uint32_t vstId, const EfxHost& host)
{
    const Spec* s = specFor(vstId);
    if (!s) return nullptr;
    std::vector<ParamDef> defs = defsFor(vstId);
    if ((int)defs.size() < efx::kParams) return nullptr;
    return new EfxPageImpl(parent, *s, std::move(defs), host);
}

}  // namespace app
}  // namespace reno
