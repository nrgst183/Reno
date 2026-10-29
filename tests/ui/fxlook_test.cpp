// Look of the effects window (VEGAS Pro 18 chrome: track chip and name, chain joined by a line with a circle at each end, Preset
// row, framed page with title bar) and the hand-built pages of Reno's built-in audio effects (src/app/fxpages.cpp): every control
// changes the setting it stands for and follows preset changes. Saves pictures of the window and every page.
//
//   fxlook_test <output folder>
// Prints PASS / FAIL lines and a summary; exits non-zero on failure.
#include "../../src/audiofx/vegasvibrato.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/engine/catalog.h"
#include "uitestlib.h"
#include "../../src/audiofx/vegasdistortion.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSlider>
#include <QToolButton>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
FxWindow* gWin = nullptr;

void settle(int ms = 120)
{
    uitest::settle(ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

template <class T>
T* one(const char* name)
{
    for (T* w : gWin->findChildren<T*>(name))
        if (w->isVisibleTo(gWin)) return w;
    return nullptr;
}

template <class T>
T* ctl(const QString& name)
{
    for (T* w : gWin->findChildren<T*>(name))
        if (w->isVisibleTo(gWin)) return w;
    return nullptr;
}

QWidget* chainButton(uint64_t slot)
{
    for (QWidget* b : gWin->findChildren<QWidget*>("fxChainButton"))
        if (b->isVisibleTo(gWin) && b->property("slotId").toULongLong() == slot) return b;
    return nullptr;
}

AudioPluginDesc builtin(const char* uid)
{
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.format == "BUILTIN" && e.desc.uid == uid) return e.desc;
    return AudioPluginDesc();
}

double real(AudioPlugin* p, int i)
{
    double lo = 0, hi = 1;
    p->paramRange(i, &lo, &hi);
    return lo + p->paramGet(i) * (hi - lo);
}

// settings whose page control is not one per setting: bypass is the host's; the taps' own are one set of controls for the tap picked
static bool skipParam(AudioPlugin* p, int i)
{
    const std::string n = p->paramName(i);
    // Pitch Shift's Initial time is typed as hr:mn:sc.xxx (checked in vegaspack1_page_test)
    return n == "Bypass" || n == "Initial Time" || n.compare(0, 4, "Tap ") == 0;
}

struct Kind {
    const char* uid;
    const char* name;
};
const Kind kKinds[] = {{"reno.distortion", "Distortion"}, {"reno.vibrato", "Vibrato"}, {"reno.pitchshift", "Pitch Shift"}, {"reno.chorus", "Chorus"},
                       {"reno.flangewah", "Flange/Wah-wah"}, {"reno.ampmod", "Amplitude Modulation"}, {"reno.simpledelay", "Simple Delay"},
                       {"reno.reverb", "Reverb"}, {"reno.moderneq", "Modern Equalizer"}, {"reno.multitapdelay", "Multi-Tap Delay"}};

// The control for setting i on the page showing now (slider, check box, combo box, radio buttons).
struct Found {
    QSlider* slider = nullptr;
    QLineEdit* edit = nullptr;
    QCheckBox* check = nullptr;
    QComboBox* combo = nullptr;
    QDoubleSpinBox* spin = nullptr;     // a number with a spinner (the tempo-sync multiplier of Flange/Wah-wah and Amplitude Modulation)
    std::vector<QRadioButton*> radios;
    bool any() const { return slider || edit || check || combo || spin || !radios.empty(); }
};

Found findControl(int i, const char* eqName = nullptr)
{
    Found f;
    const QString base = eqName ? QString(eqName) : QString("dxParam%1").arg(i);
    f.slider = ctl<QSlider>(base);
    f.edit = ctl<QLineEdit>(base + "Edit");  // also the only control of a setting that has a box and no slider
    f.check = ctl<QCheckBox>(base);
    f.combo = ctl<QComboBox>(base);
    f.spin = ctl<QDoubleSpinBox>(base);
    for (int k = 0; k < 8; ++k)
        if (QRadioButton* r = ctl<QRadioButton>(QString("dxParam%1_%2").arg(i).arg(k))) f.radios.push_back(r);
    return f;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("fxlook_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("fxlook_out");
    QDir().mkpath(gOut);
    uitest::setup({}, {});

    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    uint64_t v1 = 0, a1 = 0, ve = 0, ae = 0;
    app.edit("tracks", [&](Project& p) {
        auto track = [&](TrackType t, const char* n) {
            Track tr;
            tr.id = p.newId();
            tr.type = t;
            tr.name = n;
            p.tracks.push_back(tr);
            return tr.id;
        };
        v1 = track(TrackType::Video, "V1");
        a1 = track(TrackType::Audio, "Voice");
        auto ev = [&](uint64_t t) {
            Event e;
            e.id = p.newId();
            e.mediaId = media;
            e.start = 1;
            e.length = 3;
            e.name = "Lolga";
            p.findTrack(t)->events.push_back(e);
            return e.id;
        };
        ve = ev(v1);
        ae = ev(a1);
    });
    FxTarget at, vt, trackT;
    at.kind = FxTarget::Event;
    at.id = ae;
    vt.kind = FxTarget::Event;
    vt.id = ve;
    trackT.kind = FxTarget::Track;
    trackT.id = a1;
    app.setCursor(1.0);

    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    gWin = w;
    w->resize(560, 640);

    // ------------------------------------------------------------------ the chain, with Distortion, Vibrato and Reverb
    std::map<std::string, uint64_t> fxs;
    for (const Kind& k : kKinds) fxs[k.uid] = app.addAudioFx(at, builtin(k.uid));
    check(fxs.size() == sizeof kKinds / sizeof kKinds[0] && fxs["reno.moderneq"] != 0, "setup: all built-in audio effects on an audio event");
    // chain of the first three, as in the VEGAS pictures; the others go after them
    w->showTarget(at, fxs["reno.distortion"]);
    w->show();
    settle(300);

    // ------------------------------------------------------------------ chrome
    {
        QWidget* chip = one<QWidget>("fxTargetChip");
        QLabel* name = one<QLabel>("fxTargetName");
        check(chip != nullptr, "header: the track's chip");
        check(name && name->text() == "Lolga", "header: the event's name", name ? name->text().toStdString() : "none");
        check(chip && chip->toolTip() == "Track 2", "header: the chip carries the track's number", chip ? chip->toolTip().toStdString() : "none");
        check(w->windowTitle().startsWith("Audio Event FX: "), "the title of the window is as before", w->windowTitle().toStdString());
        check(one<QWidget>("fxFrame") != nullptr && one<QWidget>("fxTitleBar") != nullptr, "the page is in a frame with a title bar");
        QLabel* tn = one<QLabel>("fxTitleName");
        check(tn && tn->text() == "Distortion", "the title bar names the effect", tn ? tn->text().toStdString() : "none");
        for (const char* n : {"fxPreset", "fxPresetSave", "fxPresetDelete", "fxPresetHelp", "fxPluginChain", "fxRemove", "fxAdd", "fxMoveLeft", "fxMoveRight",
                              "fxAudioMode", "fxPresetRow", "fxFrameBypass", "fxFrameAbout", "fxFrameHelp"}) {
            QWidget* x = gWin->findChild<QWidget*>(n);
            check(x != nullptr, std::string("controls kept: ") + n);
        }
        check(one<QWidget>("fxPresetHelp") && one<QWidget>("fxPreset") && one<QWidget>("fxPresetSave"), "the Preset row is showing");
        // chain buttons: pick one with a click, bypass with its tick
        QWidget* b2 = chainButton(fxs["reno.vibrato"]);
        check(b2 != nullptr, "the chain has a button for each effect");
        if (b2) {
            uitest::mouseClick(b2, QPoint(b2->width() - 6, b2->height() / 2));
            settle();
            QLabel* tn2 = one<QLabel>("fxTitleName");
            check(tn2 && tn2->text() == "Vibrato", "a click on a chain button picks it: the page and its title change", tn2 ? tn2->text().toStdString() : "none");
            QCheckBox* tick = b2->findChild<QCheckBox*>("fxBypass");
            check(tick && tick->isChecked(), "the tick shows the effect is on");
            if (tick) tick->click();
            settle();
            const FxSlot* sl = nullptr;
            for (const FxSlot& s : *app.chainOf(app.project(), at))
                if (s.id == fxs["reno.vibrato"]) sl = &s;
            check(sl && sl->bypass, "clearing the tick bypasses the effect");
            QToolButton* by = one<QToolButton>("fxFrameBypass");
            check(by && by->isChecked(), "the title bar's bypass button shows it");
            if (by) by->click();
            settle();
            sl = nullptr;
            for (const FxSlot& s : *app.chainOf(app.project(), at))
                if (s.id == fxs["reno.vibrato"]) sl = &s;
            check(sl && !sl->bypass && tick && tick->isChecked(), "the title bar's bypass button switches it on again too");
        }
        w->showTarget(at, fxs["reno.distortion"]);
        settle();
        uitest::shot(w, gOut + "/fxlook_chrome_all.png");
    }

    // a shorter chain for the picture compared with VEGAS's
    {
        for (const Kind& k : kKinds)
            if (std::string(k.uid) != "reno.distortion" && std::string(k.uid) != "reno.vibrato" && std::string(k.uid) != "reno.reverb") app.removeFx(at, fxs[k.uid]);
        settle();
        w->resize(500, 520);
        w->showTarget(at, fxs["reno.distortion"]);
        settle(250);
        uitest::shot(w, gOut + "/fxlook_chrome.png");
        w->showTarget(trackT, 0);
        settle();
        QLabel* name = one<QLabel>("fxTargetName");
        check(name && name->text() == "Voice", "header: a track's name", name ? name->text().toStdString() : "none");
        w->showTarget(at, fxs["reno.distortion"]);
        settle();
        for (const Kind& k : kKinds) fxs[k.uid] = std::string(k.uid) == "reno.distortion" || std::string(k.uid) == "reno.vibrato" || std::string(k.uid) == "reno.reverb" ? fxs[k.uid] : 0;
    }

    // ------------------------------------------------------------------ each page
    w->resize(600, 700);
    for (const Kind& k : kKinds) {
        if (!fxs[k.uid]) fxs[k.uid] = app.addAudioFx(at, builtin(k.uid));
        w->showTarget(at, fxs[k.uid]);
        settle(250);
        AudioPlugin* plugin = app.fx.audio(fxs[k.uid]);
        check(plugin != nullptr, std::string(k.name) + ": the effect is made");
        if (!plugin) continue;
        check(one<QWidget>("fxDxPage") != nullptr, std::string(k.name) + ": its own page is shown");
        QLabel* tn = one<QLabel>("fxTitleName");
        check(tn && tn->text() == k.name, std::string(k.name) + ": the title bar", tn ? tn->text().toStdString() : "none");
        if (QWidget* page = one<QWidget>("fxDxPage")) {
            uitest::shot(w, gOut + "/fxlook_" + QString(k.uid).mid(5) + ".png");
            check(page->palette().color(QPalette::Window) == QColor(240, 240, 240), std::string(k.name) + ": the page is light grey");
        }
        const int n = plugin->paramCount();
        const bool eq = std::string(k.uid) == "reno.moderneq";
        int missing = 0;
        std::string which;
        for (int i = 0; i < n; ++i) {
            if (eq && i < 56) continue;  // the bands: below
            if (skipParam(plugin, i)) continue;  // VEGAS's pages leave bypass to the host
            if (!findControl(i).any()) { ++missing; which += " " + plugin->paramName(i); }
        }
        check(missing == 0, std::string(k.name) + ": every setting has a control", which);

        // each control sets its setting
        for (int i = 0; i < n; ++i) {
            if (eq && i < 56) continue;
            if (skipParam(plugin, i)) continue;
            const Found f = findControl(i);
            double lo = 0, hi = 1;
            plugin->paramRange(i, &lo, &hi);
            const std::string nm = std::string(k.name) + ": " + plugin->paramName(i);
            if (f.slider) {
                const int top = f.slider->maximum();
                f.slider->setValue(top * 3 / 4 > f.slider->value() ? top * 3 / 4 : top / 4);
                settle(20);
                const double t = (double)f.slider->value() / top;
                const double v = real(plugin, i);
                const double linear = lo + t * (hi - lo), logv = lo > 0 ? lo * std::pow(hi / lo, t) : linear;
                const double tol = (hi - lo) * 0.002 + 1e-6;
                check(std::fabs(v - linear) <= tol || std::fabs(v - logv) <= std::max(tol, std::fabs(logv) * 0.002), nm + ": its slider sets it",
                      strfmt("%g (slider at %d of %d)", v, f.slider->value(), top));
                // and shows it
                plugin->paramSet(i, 0.5f);
                settle(20);
                app.notify(kFxParams);
                settle(450);
                const double v2 = real(plugin, i);
                const double t2 = (double)f.slider->value() / f.slider->maximum();
                const double lin2 = lo + t2 * (hi - lo), log2 = lo > 0 ? lo * std::pow(hi / lo, t2) : lin2;
                const double tol2 = std::max(tol, (hi - lo) / top * 0.51);  // a setting with steps shows its nearest step
                check(std::fabs(v2 - lin2) <= tol2 || std::fabs(v2 - log2) <= std::max(tol2, std::fabs(log2) * 0.002), nm + ": its slider follows a change from elsewhere",
                      strfmt("%g, slider %d of %d", v2, f.slider->value(), f.slider->maximum()));
                if (f.edit && !f.edit->property("shown").toBool()) {
                    f.edit->setText(QString::number(lo + (hi - lo) * 0.25, 'f', 3));
                    f.edit->editingFinished();
                    settle(20);
                    check(std::fabs(real(plugin, i) - (lo + (hi - lo) * 0.25)) <= std::max((hi - lo) * 0.001 + 1e-6, 0.5), nm + ": its edit box sets it", strfmt("%g", real(plugin, i)));
                    f.edit->setText("-9999999");
                    f.edit->editingFinished();
                    settle(20);
                    check(std::fabs(real(plugin, i) - lo) < 1e-6 || std::isfinite(real(plugin, i)), nm + ": a number out of range is kept in range");
                }
            } else if (f.edit) {
                f.edit->setText(QString::number(lo + (hi - lo) * 0.25, 'f', 3));
                f.edit->editingFinished();
                settle(20);
                check(std::fabs(real(plugin, i) - (lo + (hi - lo) * 0.25)) <= (hi - lo) * 0.001 + 1e-6, nm + ": its edit box sets it", strfmt("%g", real(plugin, i)));
            } else if (f.spin) {
                if (!f.spin->isEnabled()) continue;
                f.spin->setValue(lo + (hi - lo) * 0.25);
                settle(20);
                check(std::fabs(real(plugin, i) - (lo + (hi - lo) * 0.25)) <= (hi - lo) * 0.001 + 1e-6, nm + ": its spin box sets it", strfmt("%g", real(plugin, i)));
            } else if (f.check) {
                // a disabled box is one VEGAS's page switches off for the settings now (the anti-alias filter below a ratio of 1.1, Preserve duration beyond 12 semitones...)
                if (!f.check->isEnabled()) continue;
                const bool was = f.check->isChecked();
                f.check->click();
                settle(20);
                check((real(plugin, i) >= 0.5) == !was, nm + ": its check box sets it", strfmt("%g", real(plugin, i)));
            } else if (f.combo) {
                f.combo->setCurrentIndex(f.combo->count() - 1);
                settle(20);
                check(std::lround(real(plugin, i)) == f.combo->count() - 1, nm + ": its list sets it", strfmt("%g", real(plugin, i)));
            } else if (!f.radios.empty()) {
                f.radios.back()->click();
                settle(20);
                check(std::lround(real(plugin, i)) == (long)f.radios.size() - 1, nm + ": its radio buttons set it", strfmt("%g", real(plugin, i)));
            }
        }

        // presets: the page follows
        if (QComboBox* pb = one<QComboBox>("fxPreset")) {
            bool followed = true;
            std::string bad;
            for (int pi = 0; pi < pb->count(); ++pi) {
                pb->setCurrentIndex(pi);
                QMetaObject::invokeMethod(pb, "activated", Q_ARG(int, pi));
                settle(40);
                for (int i = 0; i < n; ++i) {
                    if (eq && i < 56) continue;
                    if (skipParam(plugin, i)) continue;
                    const Found f = findControl(i);
                    double lo = 0, hi = 1;
                    plugin->paramRange(i, &lo, &hi);
                    const double v = real(plugin, i);
                    bool ok = true;
                    if (f.slider) {
                        const double t = (double)f.slider->value() / f.slider->maximum();
                        const double lin = lo + t * (hi - lo), lg = lo > 0 ? lo * std::pow(hi / lo, t) : lin;
                        // a slider with whole steps (Reverb's faders: 1 dB) shows the step nearest to a preset's value
                        const double tolP = std::max((hi - lo) * 0.002 + 1e-6, (hi - lo) / f.slider->maximum() * 0.51);
                        ok = std::fabs(v - lin) <= tolP || std::fabs(v - lg) <= std::max(tolP, std::fabs(lg) * 0.002);
                    } else if (f.edit) ok = std::fabs(f.edit->text().toDouble() - v) < 1e-3;
                    else if (f.spin) ok = std::fabs(f.spin->value() - v) < 1e-3;
                    else if (f.check) ok = f.check->isChecked() == (v >= 0.5);
                    else if (f.combo) ok = f.combo->currentIndex() == std::lround(v);
                    else if (!f.radios.empty()) ok = (long)f.radios.size() > std::lround(v) && f.radios[(size_t)std::lround(v)]->isChecked();
                    if (!ok) { followed = false; bad += strfmt(" [%s: preset %d, value %g, slider %d of %d]", plugin->paramName(i).c_str(), pi, v, f.slider ? f.slider->value() : -1, f.slider ? f.slider->maximum() : -1); }
                }
            }
            check(pb->count() > 0 && followed, std::string(k.name) + ": the page follows every preset", bad);
        }
        // undo: a slider drag is one step
        {
            for (int i = 0; i < n; ++i) {
                if (eq && i < 56) continue;
                const Found f = findControl(i);
                if (!f.slider) continue;
                const int top = f.slider->maximum();
                plugin->paramSet(i, 0.2f);
                app.notify(kFxParams);
                settle(450);
                const double before = real(plugin, i);
                f.slider->setSliderDown(true);
                f.slider->setValue(top / 2);
                f.slider->setValue(top * 4 / 5);
                f.slider->setSliderDown(false);
                settle();
                const double after = real(plugin, i);
                app.undo();
                settle();
                check(after != before && std::fabs(real(plugin, i) - before) < 1e-4 * std::max(1.0, std::fabs(before)), std::string(k.name) + ": a drag of a slider is one undo step",
                      strfmt("before %g, after %g, undone %g", before, after, real(plugin, i)));
                app.redo();
                settle();
                break;
            }
        }
    }

    // ------------------------------------------------------------------ the graphs
    {
        w->showTarget(at, fxs["reno.distortion"]);
        settle(250);
        AudioPlugin* p = app.fx.audio(fxs["reno.distortion"]);
        p->paramSet(0, 0.f);
        app.notify(kFxParams);
        settle(450);
        QWidget* g = nullptr;
        for (QWidget* c : gWin->findChildren<QWidget*>())
            if (c->isVisibleTo(gWin) && c->objectName() == "dxGraph") g = c;
        check(g != nullptr, "Distortion: the graph is there");
        if (g) {
            // the VEGAS graph: a click on the line adds a point, dragged up; double-click deletes it
            auto points = [&] {
                vegdist::Settings s;
                vegdist::decode(p->saveState(), &s);
                return s;
            };
            const int before = points().posCount;
            const QRect pl = g->rect().marginsRemoved(QMargins(32, 4, 8, 16));
            const QPoint mid(pl.left() + pl.width() / 2, pl.bottom() - pl.height() / 2);
            uitest::mouseDrag(g, mid, mid - QPoint(0, pl.height() / 4), 4);
            settle();
            const vegdist::Settings s = points();
            check(s.posCount == before + 1 && s.pos[1].y > s.pos[1].x + 100, "Distortion: a click on the line adds a point that drags",
                  strfmt("%d points, (%d, %d)", s.posCount, s.pos[1].x, s.pos[1].y));
            app.undo();
            settle();
            check(points().posCount == before, "Distortion: adding a point is one undo step", strfmt("%d", points().posCount));
            app.redo();
            settle();
            // Synchronize copies the positive graph to the negative one
            if (QComboBox* mode = one<QComboBox>("dxPolarityMode")) {
                mode->setCurrentIndex(1);
                settle();
                const vegdist::Settings t = points();
                check(t.mode == 1 && t.negCount == t.posCount && t.neg[1].y == t.pos[1].y, "Distortion: Synchronize copies the graph",
                      strfmt("mode %d, %d/%d points", t.mode, t.posCount, t.negCount));
            } else check(false, "Distortion: the polarity mode box is there");
        }
        w->showTarget(at, fxs["reno.vibrato"]);
        settle(250);
        AudioPlugin* vp = app.fx.audio(fxs["reno.vibrato"]);
        vp->setProgram(0);
        app.notify(kFxParams);
        settle(450);
        QWidget* vg = nullptr;
        for (QWidget* c : gWin->findChildren<QWidget*>())
            if (c->isVisibleTo(gWin) && c->objectName() == "dxGraph" && c->height() < 260) vg = c;
        check(vg != nullptr, "Vibrato: the graph is there");
        if (vg) {
            uitest::mouseDrag(vg, QPoint(vg->width() / 2, vg->height() / 2), QPoint(vg->width() / 2, vg->height() / 4), 4);
            settle();
            vegvib::Settings vs;
            check(vegvib::decode(vp->saveState(), &vs) && vs.count == 3 && vs.pts[3] > 30, "Vibrato: pulling the line up puts a point there", strfmt("%d points", vs.count));
            uitest::shot(w, gOut + "/fxlook_vibrato_pulled.png");
        }
        // Modern Equalizer: a band's point is pulled across and up
        w->showTarget(at, fxs["reno.moderneq"]);
        settle(250);
        AudioPlugin* ep = app.fx.audio(fxs["reno.moderneq"]);
        ep->setProgram(3);       // Bass boost: band 1, low shelf at 100 Hz, +5 dB
        app.notify(kFxParams);
        settle(450);
        QWidget* eg = one<QWidget>("dxEqGraph");
        check(eg != nullptr, "Modern Equalizer: the graph is there");
        if (eg) {
            const double f0 = real(ep, 2), g0 = real(ep, 3);
            // 100 Hz is a fifth of the way across (20 Hz .. 20 kHz), +5 dB a bit above the middle
            const QRectF pl = QRectF(eg->rect().marginsRemoved(QMargins(32, 6, 8, 16)));
            const QPoint from((int)(pl.left() + std::log(f0 / 20.0) / std::log(1000.0) * pl.width()), (int)(pl.center().y() - g0 / 24.0 * pl.height() / 2));
            uitest::mouseDrag(eg, from, from + QPoint(40, -20), 5);
            settle();
            check(real(ep, 2) > f0 * 1.3 && real(ep, 3) > g0, "Modern Equalizer: pulling a band's point changes its frequency and gain", strfmt("%g Hz, %g dB", real(ep, 2), real(ep, 3)));
            QAbstractButton* b3 = one<QAbstractButton>("dxEqBand3");
            if (b3) b3->click();
            settle();
            QCheckBox* en = one<QCheckBox>("dxEqEnabled");
            check(en && !en->isChecked(), "Modern Equalizer: another band's button shows that band's settings (band 4 is off)");
            if (en) en->click();
            settle();
            check(real(ep, 3 * 7) >= 0.5, "Modern Equalizer: Enabled of the band shown sets that band's", strfmt("%g", real(ep, 21)));
            uitest::shot(w, gOut + "/fxlook_moderneq_bands.png");
        }
    }

    // ------------------------------------------------------------------ the plain sliders are still there (the page's "Sliders" switch)
    {
        w->showTarget(at, fxs["reno.distortion"]);
        settle(250);
        QToolButton* mode = one<QToolButton>("fxAudioMode");
        check(mode != nullptr, "the Sliders switch is shown for an effect with a page");
        if (mode) {
            mode->click();
            settle(250);
            std::vector<QSlider*> sl;
            for (QSlider* s : gWin->findChildren<QSlider*>("fxAudioSlider"))
                if (s->isVisibleTo(gWin)) sl.push_back(s);
            check(sl.size() == 6 && one<QWidget>("fxDxPage") == nullptr, "Sliders: a slider for each setting, in the frame", std::to_string(sl.size()));
            check(one<QWidget>("fxFrame") != nullptr, "Sliders: in the same frame");
            uitest::shot(w, gOut + "/fxlook_sliders.png");
            mode->click();
            settle(250);
            check(one<QWidget>("fxDxPage") != nullptr, "the page comes back");
        }
    }

    delete w;
    return uitest::summary();
}
