// Pages of Reno's versions of the sfppack3_x64.dll effects (src/app/fxpages_pack3.inc): each page is built on a real
// built-in plug-in, shown, driven like a user would (sliders, boxes, buttons, the graph) and saved as a picture in
// <output folder>. Checks that the controls reach the plug-in's settings and that the page shows them again.
//
//   pack3_pages_test <output folder>
#include "../../src/app/fxpages.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasampmod.h"
#include "../../src/audiofx/vegasflange.h"
#include "../../src/audiofx/vegasvibrato.h"
#include <QApplication>
#include <QMouseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

struct Bound {
    std::unique_ptr<AudioPlugin> plugin;
    DxHost host;
    DxPage* page = nullptr;
    int edits = 0;
    std::string stateAtEdit;
    double real(int i) const
    {
        double lo = 0, hi = 1;
        plugin->paramRange(i, &lo, &hi);
        return lo + (double)plugin->paramGet(i) * (hi - lo);
    }
    void setReal(int i, double v)
    {
        double lo = 0, hi = 1;
        plugin->paramRange(i, &lo, &hi);
        plugin->paramSet(i, (float)((v - lo) / (hi - lo)));
    }
};

static std::unique_ptr<Bound> open(const std::string& uid)
{
    std::unique_ptr<Bound> b(new Bound());
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = uid;
    std::string err;
    b->plugin = builtinAudioCreate(d, &err);
    if (!b->plugin) return nullptr;
    b->plugin->activate(48000, 512);
    Bound* raw = b.get();
    b->host.range = [raw](int i, double* lo, double* hi) { return raw->plugin->paramRange(i, lo, hi); };
    b->host.get = [raw](int i) { return raw->real(i); };
    b->host.set = [raw](int i, double v) { raw->setReal(i, v); ++raw->edits; };
    b->host.name = [raw](int i) { return raw->plugin->paramName(i); };
    b->host.begin = [](int) {};
    b->host.end = [] {};
    b->host.state = [raw] { return raw->plugin->saveState(); };
    b->host.setState = [raw](const std::string& st, const std::string&) { raw->plugin->loadState(st); ++raw->edits; };
    b->host.beginEdit = [](const std::string&) {};
    b->page = makeBuiltinPage(nullptr, uid, b->host);
    if (!b->page) return nullptr;
    b->page->show();
    QApplication::processEvents();
    return b;
}

template <class T> static T* find(Bound& b, const char* name)
{
    return b.page->findChild<T*>(name);
}

static void shot(Bound& b, const QString& dir, const char* name)
{
    b.page->grab().save(dir + "/" + name + ".png");
}

static void smooth(const QString& out)
{
    auto b = open("reno.smooth");
    check(b != nullptr, "Smooth/Enhance: has a page");
    if (!b) return;
    QSlider* s = find<QSlider>(*b, "dxParam0");
    QLabel* v = find<QLabel>(*b, "dxParam0Value");
    check(s && v, "Smooth/Enhance: the Operation slider and its value");
    if (!s || !v) return;
    check(s->minimum() == 0 && s->maximum() == 10, "Smooth/Enhance: the slider has 11 positions (-5 to 5)");
    check(s->value() == 5 && v->text() == "0", "Smooth/Enhance: starts at 0", v->text().toStdString());
    s->setValue(10);
    check(std::fabs(b->real(0) - 5) < 1e-4 && v->text() == "5", "Smooth/Enhance: slider to the right end: Operation 5, shown as 5");
    s->setValue(0);
    check(std::fabs(b->real(0) + 5) < 1e-4 && v->text() == "-5", "Smooth/Enhance: slider to the left end: Operation -5");
    b->plugin->setProgram(1);
    b->page->refresh();
    check(std::fabs(b->real(0) - 3) < 1e-4 && s->value() == 8, "Smooth/Enhance: the preset Boost high frequencies shows as 3");
    shot(*b, out, "pack3_smooth");
}

static void clickAt(QWidget* w, QPointF p, Qt::MouseButton b = Qt::LeftButton, bool release = true)
{
    QMouseEvent press(QEvent::MouseButtonPress, p, w->mapToGlobal(p), b, b, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    if (release) {
        QMouseEvent rel(QEvent::MouseButtonRelease, p, w->mapToGlobal(p), b, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(w, &rel);
    }
}

static void vibrato(const QString& out)
{
    auto b = open("reno.vibrato");
    check(b != nullptr, "Vibrato: has a page");
    if (!b) return;
    QSlider* gs = find<QSlider>(*b, "dxParam3");
    QSlider* ss = find<QSlider>(*b, "dxParam0");
    QSlider* fs = find<QSlider>(*b, "dxParam1");
    QLineEdit* fb = find<QLineEdit>(*b, "dxParam1Edit");
    QCheckBox* bl = find<QCheckBox>(*b, "dxParam2");
    QPushButton* reset = find<QPushButton>(*b, "dxReset");
    QWidget* graph = b->page->findChild<QWidget*>("dxGraph");
    QLabel* readout = find<QLabel>(*b, "dxGraphReadout");
    check(gs && ss && fs && fb && bl && reset && graph && readout, "Vibrato: the faders, graph, frequency, blend box and Reset are there");
    if (!(gs && ss && fs && fb && bl && reset && graph && readout)) return;
    check(std::fabs(b->real(0) - 12) < 1e-4 && fb->text() == "1.000" && !bl->isChecked() && std::fabs(b->real(3)) < 1e-4, "Vibrato: starts at VEGAS's defaults",
          fb->text().toStdString());
    check(readout->text() == "12.00 semitones", "Vibrato: the readout says the semitones at rest", readout->text().toStdString());
    ss->setValue(ss->minimum());
    check(std::fabs(b->real(0) - 0.01) < 1e-4, "Vibrato: the semitones fader at the bottom is 0.01");
    gs->setValue(gs->minimum());
    check(b->real(3) <= -79.9, "Vibrato: the gain fader at the bottom is -Inf.");
    fb->setText("5");
    fb->editingFinished();
    QApplication::processEvents();
    check(std::fabs(b->real(1) - 5) < 1e-4 && fs->value() > fs->minimum(), "Vibrato: frequency box 5 Hz");
    fs->setValue(fs->maximum());
    check(std::fabs(b->real(1) - 1000) < 1e-2, "Vibrato: the frequency trackbar at the right end is 1,000 Hz");
    bl->click();
    check(std::fabs(b->real(2) - 1) < 1e-4, "Vibrato: Blend graph edges on");
    // the graph: a click on the line adds a point, dragging moves it, a double click removes it
    b->plugin->setProgram(0);
    b->page->refresh();
    const QSize gsz = graph->size();
    const double left = 35, right = gsz.width() - 6, top = 6, bottom = gsz.height() - 6;
    const double xs = left + (right - left) * 0.5, ym = (top + bottom) / 2;
    clickAt(graph, QPointF(xs, ym));
    vegvib::Settings s;
    check(vegvib::decode(b->plugin->saveState(), &s) && s.count == 3 && std::abs(s.pts[2] - 500) <= 2 && s.pts[3] == 0, "Vibrato: a click on the line adds a point at 500");
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(xs, ym), graph->mapToGlobal(QPointF(xs, ym)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &press);
    const QPointF to(left + (right - left) * 0.25, top);
    QMouseEvent move(QEvent::MouseMove, to, graph->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &move);
    QMouseEvent rel(QEvent::MouseButtonRelease, to, graph->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &rel);
    check(vegvib::decode(b->plugin->saveState(), &s) && s.count == 3 && std::abs(s.pts[2] - 250) <= 2 && s.pts[3] == 100, "Vibrato: dragging the point to 250 and the top gives (250, 100)",
          std::to_string(s.pts[2]) + "," + std::to_string(s.pts[3]));
    shot(*b, out, "pack3_vibrato");
    QMouseEvent dbl(QEvent::MouseButtonDblClick, to, graph->mapToGlobal(to), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &dbl);
    check(vegvib::decode(b->plugin->saveState(), &s) && s.count == 2, "Vibrato: a double click removes it");
    clickAt(graph, QPointF(left, ym), Qt::RightButton);
    check(vegvib::decode(b->plugin->saveState(), &s) && s.count == 2, "Vibrato: an end point stays");
    b->plugin->setProgram(4);
    b->page->refresh();
    vegvib::decode(b->plugin->saveState(), &s);
    check(s.count == 8 && std::fabs(b->real(1) - 9) < 1e-4, "Vibrato: the preset Vibrato 2 shows 8 points and 9 Hz");
    reset->click();
    check(vegvib::decode(b->plugin->saveState(), &s) && s.count == 2 && s.pts[0] == 0 && s.pts[1] == 0 && s.pts[2] == 1000 && s.pts[3] == 0 && std::fabs(b->real(1) - 9) < 1e-4,
          "Vibrato: Reset makes the graph flat and leaves the settings");
    shot(*b, out, "pack3_vibrato2");
}

static void ampmod(const QString& out)
{
    auto b = open("reno.ampmod");
    check(b != nullptr, "Amplitude Modulation: has a page");
    if (!b) return;
    QSlider* dry = find<QSlider>(*b, "dxParam6");
    QSlider* wet = find<QSlider>(*b, "dxParam7");
    QSlider* amp = find<QSlider>(*b, "dxParam0");
    QSlider* fs = find<QSlider>(*b, "dxParam1");
    QLineEdit* fb = find<QLineEdit>(*b, "dxParam1Edit");
    QSlider* ps = find<QSlider>(*b, "dxParam2");
    QCheckBox* lc = find<QCheckBox>(*b, "dxParam3");
    QSlider* ls = find<QSlider>(*b, "dxParam4");
    QCheckBox* bl = find<QCheckBox>(*b, "dxParam5");
    QCheckBox* ts = find<QCheckBox>(*b, "dxParam10");
    QDoubleSpinBox* mult = find<QDoubleSpinBox>(*b, "dxParam9");
    QComboBox* per = find<QComboBox>(*b, "dxParam8");
    QPushButton* reset = find<QPushButton>(*b, "dxReset");
    QWidget* graph = b->page->findChild<QWidget*>("dxGraph");
    QLabel* readout = find<QLabel>(*b, "dxGraphReadout");
    const bool all = dry && wet && amp && fs && fb && ps && lc && ls && bl && ts && mult && per && reset && graph && readout;
    check(all, "Amplitude Modulation: the faders, graph, frequency, pan, low-pass, tempo sync, period and Reset are there");
    if (!all) return;
    check(std::fabs(b->real(0) + 48) < 1e-4 && b->real(6) <= -79.9 && std::fabs(b->real(7)) < 1e-4 && fb->text() == "1.000" && !lc->isChecked() && !bl->isChecked() && !ts->isChecked() &&
              per->currentIndex() == 18 && std::fabs(mult->value() - 1) < 1e-6,
          "Amplitude Modulation: starts at VEGAS's defaults");
    check(readout->text() == "-48.0 dB", "Amplitude Modulation: the readout says the amplitude at rest", readout->text().toStdString());
    dry->setValue(dry->maximum());
    check(std::fabs(b->real(6)) < 1e-4, "Amplitude Modulation: the Dry out fader at the top is 0 dB");
    wet->setValue(wet->maximum());
    check(std::fabs(b->real(7) - 24) < 1e-4, "Amplitude Modulation: the Wet out fader at the top is 24 dB");
    amp->setValue(amp->minimum());
    check(b->real(0) <= -79.9, "Amplitude Modulation: the Amplitude fader at the bottom is -Inf.");
    b->plugin->setProgram(0);
    b->page->refresh();
    fb->setText("5,000");
    fb->editingFinished();
    check(std::fabs(b->real(1) - 5000) < 1e-3, "Amplitude Modulation: frequency box 5,000 Hz");
    ps->setValue(ps->maximum());
    check(std::fabs(b->real(2) - 100) < 1e-4, "Amplitude Modulation: stereo pan to 100");
    check(!ls->isEnabled(), "Amplitude Modulation: the low-pass trackbar waits for its check box");
    lc->click();
    check(ls->isEnabled() && std::fabs(b->real(3) - 1) < 1e-4, "Amplitude Modulation: the low-pass check box turns it on");
    ls->setValue(ls->maximum());
    check(std::fabs(b->real(4) - 10000) < 1e-2, "Amplitude Modulation: the low-pass trackbar at the right end is 10,000 Hz");
    bl->click();
    check(std::fabs(b->real(5) - 1) < 1e-4, "Amplitude Modulation: Blend graph edges on");
    // tempo sync turns the period on and the frequency off
    check(!per->isEnabled() && fs->isEnabled(), "Amplitude Modulation: with tempo sync off the period waits");
    ts->click();
    check(per->isEnabled() && mult->isEnabled() && !fs->isEnabled() && !fb->isEnabled(), "Amplitude Modulation: with tempo sync on the period works and the frequency waits");
    per->setCurrentIndex(13);
    mult->setValue(2.5);
    check(std::fabs(b->real(8) - 13) < 1e-4 && std::fabs(b->real(9) - 2.5) < 1e-4, "Amplitude Modulation: a quarter note times 2.5");
    // the graph: a click on the line adds a point, dragging moves it, a double click removes it
    b->plugin->setProgram(0);
    b->page->refresh();
    const QSize gsz = graph->size();
    const double left = 32, right = gsz.width() - 6, top = 6, bottom = gsz.height() - 6;
    const double xs = left + (right - left) * 0.5, ym = (top + bottom) / 2;
    clickAt(graph, QPointF(xs, ym));
    vegamp::Settings s;
    check(vegamp::decode(b->plugin->saveState(), &s) && s.count == 3 && std::abs(s.pts[2] - 500) <= 2 && s.pts[3] == -50, "Amplitude Modulation: a click on the line adds a point at 500");
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(xs, ym), graph->mapToGlobal(QPointF(xs, ym)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &press);
    const QPointF to(left + (right - left) * 0.25, top);
    QMouseEvent move(QEvent::MouseMove, to, graph->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &move);
    QMouseEvent rel(QEvent::MouseButtonRelease, to, graph->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &rel);
    check(vegamp::decode(b->plugin->saveState(), &s) && s.count == 3 && std::abs(s.pts[2] - 250) <= 2 && s.pts[3] == 0, "Amplitude Modulation: dragging the point to 250 and the top gives (250, 0)",
          std::to_string(s.pts[2]) + "," + std::to_string(s.pts[3]));
    shot(*b, out, "pack3_ampmod");
    QMouseEvent dbl(QEvent::MouseButtonDblClick, to, graph->mapToGlobal(to), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(graph, &dbl);
    check(vegamp::decode(b->plugin->saveState(), &s) && s.count == 2, "Amplitude Modulation: a double click removes it");
    b->plugin->setProgram(2);
    b->page->refresh();
    vegamp::decode(b->plugin->saveState(), &s);
    check(s.count == 12 && std::fabs(b->real(1) - 1) < 1e-4, "Amplitude Modulation: the preset Sine shows 12 points");
    reset->click();
    check(vegamp::decode(b->plugin->saveState(), &s) && s.count == 2 && s.pts[1] == -50 && s.pts[2] == 1000 && s.pts[3] == -50 && std::fabs(b->real(0) + 48) < 1e-4,
          "Amplitude Modulation: Reset makes the graph flat at -50 and leaves the settings");
    b->plugin->setProgram(3);
    b->page->refresh();
    shot(*b, out, "pack3_ampmod2");
}

static void gapper(const QString& out)
{
    auto b = open("reno.gapsnip");
    check(b != nullptr, "Gapper/Snipper: has a page");
    if (!b) return;
    QSlider* fs = find<QSlider>(*b, "dxParam1");
    QLineEdit* fb = find<QLineEdit>(*b, "dxParam1Edit");
    QSlider* ls = find<QSlider>(*b, "dxParam2");
    QLineEdit* lb = find<QLineEdit>(*b, "dxParam2Edit");
    QSlider* ds = find<QSlider>(*b, "dxParam3");
    QLabel* dv = find<QLabel>(*b, "dxParam3Value");
    QLabel* pc = find<QLabel>(*b, "dxPercentOfOriginal");
    check(fs && fb && ls && lb && ds && dv && pc, "Gapper/Snipper: frequency, length, fade and percent controls are there");
    if (!(fs && fb && ls && lb && ds && dv && pc)) return;
    check(fb->text() == "20.0000" && lb->text() == "0.0750" && dv->text() == "15 %" && pc->text() == "250 %", "Gapper/Snipper: starts at the defaults",
          (fb->text() + " " + lb->text() + " " + dv->text() + " " + pc->text()).toStdString());
    fb->setText("100");
    fb->editingFinished();
    QApplication::processEvents();
    check(std::fabs(b->real(1) - 100) < 1e-3 && pc->text() == "850 %", "Gapper/Snipper: frequency box 100 Hz: 850 %", pc->text().toStdString());
    ls->setValue(0);
    check(std::fabs(b->real(2) - 0.0001) < 1e-6, "Gapper/Snipper: length slider at the left end is 0.0001 s");
    ds->setValue(1000);
    check(std::fabs(b->real(3) - 50) < 1e-3 && dv->text() == "50 %", "Gapper/Snipper: fade slider to the right end: 50 %");
    QList<QRadioButton*> rb = b->page->findChildren<QRadioButton*>();
    check(rb.size() == 2 && rb[0]->text().contains("Gapper") && rb[1]->text().contains("Snipper"), "Gapper/Snipper: two effect choices");
    if (rb.size() == 2) {
        rb[1]->click();
        check(std::fabs(b->real(0) - 1) < 1e-4, "Gapper/Snipper: Snipper selected");
        b->plugin->setProgram(3);
        b->page->refresh();
        check(pc->text() == "200 %" && std::fabs(b->real(0)) < 1e-4, "Gapper/Snipper: the preset Distort 1 (gapper) shows 200 %", pc->text().toStdString());
    }
    shot(*b, out, "pack3_gapper");
}

static void flange(const QString& out)
{
    auto b = open("reno.flangewah");
    check(b != nullptr, "Flange/Wah-wah: has a page");
    if (!b) return;
    QSlider* rs = find<QSlider>(*b, "dxParam3");
    QLineEdit* rb = find<QLineEdit>(*b, "dxParam3Edit");
    QSlider* ds = find<QSlider>(*b, "dxParam4");
    QLabel* dv = find<QLabel>(*b, "dxParam4Value");
    QSlider* cs = find<QSlider>(*b, "dxParam1");
    QSlider* qs = find<QSlider>(*b, "dxParam2");
    QLabel* qv = find<QLabel>(*b, "dxParam2Value");
    QDoubleSpinBox* mult = find<QDoubleSpinBox>(*b, "dxParam8");
    QComboBox* per = find<QComboBox>(*b, "dxParam7");
    QCheckBox* ts = find<QCheckBox>(*b, "dxParam9");
    QSlider* dry = find<QSlider>(*b, "dxParam6");
    QSlider* wet = find<QSlider>(*b, "dxParam5");
    check(rs && rb && ds && dv && cs && qs && qv && mult && per && ts && dry && wet, "Flange/Wah-wah: all the controls of the dialog are there");
    if (!(rs && rb && ds && dv && cs && qs && qv && mult && per && ts && dry && wet)) return;
    check(rb->text() == "10.000" && dv->text() == "10%" && qv->text() == "1%", "Flange/Wah-wah: starts at the defaults",
          (rb->text() + " " + dv->text() + " " + qv->text()).toStdString());
    check(!cs->isEnabled() && !qs->isEnabled() && !per->isEnabled(), "Flange/Wah-wah: the flange has no center frequency or resonance, and no period without tempo sync");
    QList<QRadioButton*> rd = b->page->findChildren<QRadioButton*>();
    check(rd.size() == 3 && rd[0]->text() == "Flange" && rd[1]->text() == "Phaser" && rd[2]->text() == "Wah-Wah", "Flange/Wah-wah: three effect choices");
    if (rd.size() == 3) {
        rd[2]->click();
        check(std::fabs(b->real(0) - 2) < 1e-4 && cs->isEnabled() && qs->isEnabled(), "Flange/Wah-wah: Wah-Wah selected, center frequency and resonance work");
    }
    rb->setText("2.5");
    rb->editingFinished();
    QApplication::processEvents();
    check(std::fabs(b->real(3) - 2.5) < 1e-3, "Flange/Wah-wah: rate box 2.5 Hz");
    ds->setValue(1000);
    check(std::fabs(b->real(4) - 100) < 1e-3 && dv->text() == "100%", "Flange/Wah-wah: depth slider to the right end: 100%");
    qs->setValue(0);
    check(std::fabs(b->real(2) - 1) < 1e-3, "Flange/Wah-wah: resonance slider to the left end: 1");
    dry->setValue(1000);
    wet->setValue(0);
    check(std::fabs(b->real(6) - 12) < 1e-3 && b->real(5) <= -79.9, "Flange/Wah-wah: dry out to +12 dB, wet out to -Inf.");
    ts->click();
    check(per->isEnabled() && mult->isEnabled() && b->real(9) > 0.5, "Flange/Wah-wah: tempo sync turns the period on");
    per->setCurrentIndex(13);
    mult->setValue(2.0);
    check(std::fabs(b->real(7) - 13) < 1e-4 && std::fabs(b->real(8) - 2.0) < 1e-4, "Flange/Wah-wah: period Quarter Note x 2");
    vegflange::Settings s;
    check(vegflange::decode(b->plugin->saveState(), &s) && s.effect == 2 && s.rate == 2500 && s.depth == 100 && s.period == 13 && s.multiplier == 2000 && s.tempoSync == 1 &&
              s.dryOut == 120 && s.wetOut == -800,
          "Flange/Wah-wah: the saved form has the page's values");
    b->plugin->setProgram(6);
    b->page->refresh();
    check(std::fabs(b->real(0) - 2) < 1e-4 && std::fabs(b->real(2) - 100) < 1e-3 && std::fabs(b->real(3) - 0.2) < 1e-3, "Flange/Wah-wah: the preset Wah-wah 3 shows");
    shot(*b, out, "pack3_flange");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : QString(".");
    QDir().mkpath(out);
    smooth(out);
    gapper(out);
    vibrato(out);
    ampmod(out);
    flange(out);
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
