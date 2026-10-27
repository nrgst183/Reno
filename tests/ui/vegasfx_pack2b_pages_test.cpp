// The pages of the second batch of sfppack2 / sfresfilter reproductions (Parametric EQ, ...; src/app/fxpages_pack2.inc):
// each page is made over the real built-in effect, its controls are driven, and a picture of each is saved.
//
//   vegasfx_pack2b_pages_test <output folder>
// (run with QT_QPA_PLATFORM=offscreen)
#include "../../src/app/fxpages.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasgdyn.h"
#include "../../src/audiofx/vegasmbdyn.h"
#include "../../src/audiofx/vegaspgeq.h"
#include "../../src/audiofx/vegaspmeq.h"
#include "../../src/audiofx/vegasresfilter.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QTabWidget>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace reno;
using namespace reno::app;

static int gFail = 0, gPass = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) { ++gPass; std::printf("PASS %s\n", what.c_str()); }
    else { ++gFail; std::printf("FAIL %s\n", what.c_str()); }
}

struct Rig {
    std::unique_ptr<AudioPlugin> p;
    DxHost host;
    DxPage* page = nullptr;
    explicit Rig(const char* uid)
    {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = uid;
        p = builtinAudioCreate(d, nullptr);
        host.range = [this](int i, double* lo, double* hi) { return p->paramRange(i, lo, hi); };
        host.get = [this](int i) {
            double lo = 0, hi = 1;
            p->paramRange(i, &lo, &hi);
            return lo + (double)p->paramGet(i) * (hi - lo);
        };
        host.set = [this](int i, double v) {
            double lo = 0, hi = 1;
            p->paramRange(i, &lo, &hi);
            p->paramSet(i, (float)((v - lo) / (hi - lo)));
        };
        host.name = [this](int i) { return p->paramName(i); };
        host.begin = [](int) {};
        host.end = [] {};
        host.state = [this] { return p->saveState(); };
        host.setState = [this](const std::string& st, const std::string&) { p->loadState(st); };
        host.beginEdit = [](const std::string&) {};
        page = makeBuiltinPage(nullptr, uid, host);
    }
};

// (the page reads a setting from a float fraction of its range: close to, not exactly, the whole numbers)
static double value(Rig& r, int i) { return std::round(r.host.get(i) * 100) / 100; }

static void parametricEq(const QString& out)
{
    Rig r("reno.parametriceq");
    check(r.page != nullptr, "Parametric EQ has a page");
    if (!r.page) return;
    r.page->show();
    r.page->grab().save(out + "/parametriceq_default.png");
    auto state = [&] {
        vegpm::Settings s;
        vegpm::decode(r.p->saveState(), &s);
        return s;
    };
    QComboBox* style = r.page->findChild<QComboBox*>("dxParam0");
    QSlider* freq = r.page->findChild<QSlider*>("dxParam1");
    QSlider* width = r.page->findChild<QSlider*>("dxParam2");
    QSlider* amount = r.page->findChild<QSlider*>("dxParam3");
    QSlider* gain = r.page->findChild<QSlider*>("dxParam4");
    QComboBox* acc = r.page->findChild<QComboBox*>("dxParam5");
    check(style && freq && width && amount && gain && acc, "  style, frequency, width, amount, output gain and accuracy controls");
    if (!(style && freq && width && amount && gain && acc)) return;
    check(style->count() == 4 && acc->count() == 3, "  4 filter styles and 3 accuracies");
    check(state().style == 2 && state().freq == 650 && state().width == 10 && state().amount == -40 && state().outGain == 0 && state().accuracy == 1, "  it shows VEGAS's defaults");
    style->setCurrentIndex(3);
    check(state().style == 3, "  the style box sets the filter style");
    amount->setValue(amount->maximum());
    check(state().amount == 200, "  the amount fader's top is +20 dB");
    gain->setValue(gain->minimum());
    check(state().outGain == -600, "  the output gain fader's bottom is -60 dB");
    QLineEdit* fbox = r.page->findChild<QLineEdit*>("dxParam1Edit");
    check(fbox != nullptr, "  the frequency has an edit box");
    if (fbox) {
        fbox->setText("1,500");
        emit fbox->editingFinished();
        check(state().freq == 1500, "  \"1,500\" in the frequency box");
    }
    QLineEdit* wbox = r.page->findChild<QLineEdit*>("dxParam2Edit");
    if (wbox) {
        wbox->setText("2.5");
        emit wbox->editingFinished();
        check(state().width == 25, "  2.5 in the width box is 25 tenths");
    }
    acc->setCurrentIndex(2);
    check(state().accuracy == 2 && r.p->latencySamples() == 2048, "  the accuracy box sets the accuracy");
    r.page->grab().save(out + "/parametriceq_edited.png");
    r.p->setProgram(4);
    r.page->refresh();
    check(style->currentIndex() == 2 && acc->currentIndex() == 1, "  a preset (\"Phone line effect\") shows in the page");
    r.page->grab().save(out + "/parametriceq_preset.png");
    delete r.page;
}

static void paragraphicEq(const QString& out)
{
    Rig r("reno.paragraphiceq");
    check(r.page != nullptr, "Paragraphic EQ has a page");
    if (!r.page) return;
    r.page->show();
    r.page->grab().save(out + "/paragraphiceq_default.png");
    auto state = [&] {
        vegpg::Settings s;
        vegpg::decode(r.p->saveState(), &s);
        return s;
    };
    bool all = true;
    for (int i = 0; i < 14; ++i) all = all && r.page->findChild<QSlider*>(QString("dxParam%1").arg(i)) != nullptr;
    for (int i = 16; i < 20; ++i) all = all && r.page->findChild<QSlider*>(QString("dxParam%1").arg(i)) != nullptr;
    QCheckBox* lo = r.page->findChild<QCheckBox*>("dxParam14");
    QCheckBox* hi = r.page->findChild<QCheckBox*>("dxParam15");
    check(all && lo && hi, "  dry, wet, 4x(gain, width, frequency), 2 shelf boxes, frequencies and gains");
    if (!(all && lo && hi)) return;
    check(state().dry == -250 && state().wet == 0 && state().freq[2] == 1000 && !state().lowOn, "  it shows VEGAS's defaults");
    QSlider* g1 = r.page->findChild<QSlider*>("dxParam2");
    g1->setValue(g1->maximum());
    check(state().gain[0] == 250, "  band 1 gain top is +25 dB");
    QLineEdit* fb = r.page->findChild<QLineEdit*>("dxParam11Edit");
    if (fb) {
        fb->setText("2,500");
        emit fb->editingFinished();
        check(state().freq[1] == 2500, "  \"2,500\" in band 2's frequency box");
    }
    lo->setChecked(true);
    check(state().lowOn == 1, "  the low shelf box turns it on");
    r.page->grab().save(out + "/paragraphiceq_edited.png");
    r.p->setProgram(2);
    r.page->refresh();
    check(state().gain[2] == -150 && state().freq[2] == 1000, "  a preset (\"15 dB notch\") shows in the page");
    r.page->grab().save(out + "/paragraphiceq_preset.png");
    delete r.page;
}

static void graphicDynamics(const QString& out)
{
    Rig r("reno.graphicdynamics");
    check(r.page != nullptr, "Graphic Dynamics has a page");
    if (!r.page) return;
    r.page->show();
    r.page->grab().save(out + "/graphicdynamics_default.png");
    auto state = [&] {
        veggd::Settings s;
        veggd::decode(r.p->saveState(), &s);
        return s;
    };
    QSlider* gain = r.page->findChild<QSlider*>("dxParam4");
    QSlider* att = r.page->findChild<QSlider*>("dxParam0");
    QSlider* rel = r.page->findChild<QSlider*>("dxParam1");
    QCheckBox* ag = r.page->findChild<QCheckBox*>("dxParam2");
    QCheckBox* sy = r.page->findChild<QCheckBox*>("dxParam3");
    QSlider* thr = r.page->findChild<QSlider*>("dxThreshold");
    QSlider* rat = r.page->findChild<QSlider*>("dxRatio");
    QPushButton* reset = r.page->findChild<QPushButton*>("dxResetGraph");
    QWidget* graph = r.page->findChild<QWidget*>("dxGraph");
    check(gain && att && rel && ag && sy && thr && rat && reset && graph, "  output gain, attack, release, two check boxes, threshold, ratio, Reset Graph and the graph");
    if (!(gain && att && rel && ag && sy && thr && rat && reset && graph)) return;
    check(state().attack == 10 && state().release == 5000 && state().sync == 1 && !state().autoGain && state().count == 3, "  it shows VEGAS's defaults");
    gain->setValue(gain->maximum());
    check(state().outGain == 250, "  the output gain fader's top is +25 dB");
    ag->setChecked(true);
    sy->setChecked(false);
    check(state().autoGain == 1 && state().sync == 0, "  the check boxes");
    QLineEdit* abox = r.page->findChild<QLineEdit*>("dxParam0Edit");
    if (abox) {
        abox->setText("12.5");
        emit abox->editingFinished();
        check(state().attack == 125, "  12.5 in the attack box is 125 tenths of a ms");
    }
    rat->setValue(30);
    check(state().pt[2][1] == -160 && state().ratio == 30, "  the ratio slider (3.0:1) puts the last point at -16 dB");
    thr->setValue(-300);
    check(state().pt[1][0] == -300 && state().threshold == -300, "  the threshold slider moves the knee");
    QLabel* tr = r.page->findChild<QLabel*>("dxThresholdReadout");
    QLabel* rr = r.page->findChild<QLabel*>("dxRatioReadout");
    check(tr && rr && tr->text() == "-30.0 dB" && rr->text() == "3.0 :1", "  and their readouts");
    // a click on the line adds a point, a drag moves it
    {
        QPoint a(graph->width() / 2, graph->height() / 2);
        const int before = state().count;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(a), graph->mapToGlobal(QPointF(a)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(graph, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(a), graph->mapToGlobal(QPointF(a)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(graph, &release);
        check(state().count >= before, "  the graph takes mouse clicks");
    }
    r.page->grab().save(out + "/graphicdynamics_edited.png");
    reset->click();
    check(state().count == 3 && state().pt[1][0] == -210 && state().threshold == -210 && state().ratio == 10, "  Reset Graph");
    r.p->setProgram(9);
    r.page->refresh();
    check(state().count == 7 && thr->value() == -240, "  a preset (\"Soft knee compressor/gate\") shows in the page");
    r.page->grab().save(out + "/graphicdynamics_preset.png");
    delete r.page;
}

static void multiBandDynamics(const QString& out)
{
    Rig r("reno.multibanddynamics");
    check(r.page != nullptr, "Multi-Band Dynamics has a page");
    if (!r.page) return;
    r.page->show();
    r.page->grab().save(out + "/multibanddynamics_default.png");
    auto state = [&] {
        vegmb::Settings s;
        vegmb::decode(r.p->saveState(), &s);
        return s;
    };
    QTabWidget* tabs = r.page->findChild<QTabWidget*>("dxMbTabs");
    check(tabs && tabs->count() == 2 && tabs->tabText(0) == "Bands 1 && 2", "  two tabs, Bands 1 & 2 and Bands 3 & 4");
    int missing = 0;
    for (int b = 0; b < 4; ++b)
        for (int k = 0; k < 10; ++k)
            if (!r.page->findChild<QWidget*>(QString("dxParam%1").arg(b * 10 + k))) ++missing;
    check(missing == 0 && r.page->findChild<QWidget*>("dxParam40") && r.page->findChild<QWidget*>("dxParam41"), "  every band's ten controls, Sync channels and the output gain");
    if (missing) return;
    check(state().freq[2] == 4000 && state().bypass[0] == 0 && state().bypass[1] == 1 && state().sync == 1, "  it shows VEGAS's defaults");
    QSlider* thr = r.page->findChild<QSlider*>("dxParam0");
    QSlider* gain = r.page->findChild<QSlider*>("dxParam2");
    QSlider* out2 = r.page->findChild<QSlider*>("dxParam41");
    QComboBox* type = r.page->findChild<QComboBox*>("dxParam25");
    QCheckBox* byp = r.page->findChild<QCheckBox*>("dxParam28");
    QCheckBox* solo = r.page->findChild<QCheckBox*>("dxParam29");
    QCheckBox* sync = r.page->findChild<QCheckBox*>("dxParam40");
    QSlider* width = r.page->findChild<QSlider*>("dxParam7");
    QSlider* freq = r.page->findChild<QSlider*>("dxParam6");
    if (!(thr && gain && out2 && type && byp && solo && sync && width && freq)) { check(false, "  controls found"); return; }
    thr->setValue(thr->minimum());
    check(state().threshold[0] == -600, "  the threshold fader's bottom is -60 dB");
    gain->setValue(gain->maximum());
    out2->setValue(out2->minimum());
    check(state().gain[0] == 250 && state().outGain == -250, "  gain and output gain faders");
    type->setCurrentIndex(2);
    check(state().type[2] == 2, "  band 3's type");
    byp->setChecked(false);
    solo->setChecked(true);
    sync->setChecked(false);
    check(state().bypass[2] == 0 && state().solo[2] == 1 && state().sync == 0, "  band 3's bypass and solo and sync channels");
    freq->setValue(0);
    check(state().freq[0] == 50, "  the frequency slider's left end is 50 Hz");
    QLineEdit* abox = r.page->findChild<QLineEdit*>("dxParam3Edit");
    if (abox) {
        abox->setText("12.5");
        emit abox->editingFinished();
        check(state().attack[0] == 125, "  12.5 in the attack box is 125 tenths of a ms");
    }
    QSlider* w2 = r.page->findChild<QSlider*>("dxParam17");
    check(w2 && w2->isEnabled(), "  band 2's width is on for a band-notch");
    QSlider* w3 = r.page->findChild<QSlider*>("dxParam27");
    check(w3 && !w3->isEnabled(), "  and off for a high-shelf (band 3)");
    r.page->grab().save(out + "/multibanddynamics_edited.png");
    tabs->setCurrentIndex(1);
    r.page->grab().save(out + "/multibanddynamics_tab2.png");
    r.p->setProgram(2);
    r.page->refresh();
    check(state().freq[3] == 1633 && state().bypass[3] == 0, "  a preset (\"Anti-DTMF talk-off\") shows in the page");
    r.page->grab().save(out + "/multibanddynamics_preset.png");
    delete r.page;
}

static void resonantFilter(const QString& out)
{
    Rig r("reno.resonantfilter");
    check(r.page != nullptr, "Resonant Filter has a page");
    if (!r.page) return;
    r.page->show();
    r.page->grab().save(out + "/resonantfilter_default.png");
    auto state = [&] {
        vegrf::Settings s;
        vegrf::decode(r.p->saveState(), &s);
        return s;
    };
    QSlider* freq = r.page->findChild<QSlider*>("dxParam0");
    QSlider* res = r.page->findChild<QSlider*>("dxParam1");
    QSlider* wet = r.page->findChild<QSlider*>("dxParam5");
    QRadioButton* lp = r.page->findChild<QRadioButton*>("dxParam2_0");
    QRadioButton* bp = r.page->findChild<QRadioButton*>("dxParam2_1");
    QRadioButton* hp = r.page->findChild<QRadioButton*>("dxParam2_2");
    QRadioButton* second = r.page->findChild<QRadioButton*>("dxParam3_0");
    QRadioButton* fourth = r.page->findChild<QRadioButton*>("dxParam3_1");
    check(freq && res && wet && lp && bp && hp && second && fourth, "  frequency, resonance and wet / dry, three filter types and two orders");
    if (!(freq && res && wet && lp && bp && hp && second && fourth)) return;
    check(state().freq == 20000.f && state().res == 0.f && state().type == 0.f && state().order == 0.f && state().wet == 100.f && lp->isChecked() && second->isChecked(),
          "  it shows VEGAS's defaults: 20000 Hz, low pass, second order, fully wet");
    check(freq->minimum() == 0 && freq->maximum() == 19980 && res->maximum() == 1000 && wet->maximum() == 1000, "  the sliders have a step for each Hz and each tenth of a percent");
    freq->setValue(0);
    check(state().freq == 20.f, "  the frequency slider's left end is 20 Hz");
    freq->setValue(980);
    check(state().freq == 1000.f, "  and it is straight: 980 steps on is 1000 Hz");
    res->setValue(res->maximum());
    wet->setValue(250);
    check(state().res == 100.f && state().wet == 25.f, "  resonance 100 % at the right end, wet / dry 25 % at 250 steps");
    bp->setChecked(true);
    fourth->setChecked(true);
    check(state().type == 1.f && state().order == 1.f, "  band pass and fourth order");
    hp->setChecked(true);
    check(state().type == 2.f, "  high pass");
    QLineEdit* fbox = r.page->findChild<QLineEdit*>("dxParam0Edit");
    QLineEdit* rbox = r.page->findChild<QLineEdit*>("dxParam1Edit");
    QLineEdit* wbox = r.page->findChild<QLineEdit*>("dxParam5Edit");
    check(fbox && rbox && wbox, "  every slider has a box");
    if (fbox && rbox && wbox) {
        fbox->setText("1,234");
        emit fbox->editingFinished();
        rbox->setText("33.3");
        emit rbox->editingFinished();
        wbox->setText("66.6");
        emit wbox->editingFinished();
        check(state().freq == 1234.f && state().res == 33.3f && state().wet == 66.6f, "  1,234 Hz, 33.3 % and 66.6 % typed in the boxes");
        check(fbox->text() == "1,234" && rbox->text() == "33.3" && wbox->text() == "66.6", "  and shown the same way (Hz whole, percent in tenths)");
    }
    r.page->grab().save(out + "/resonantfilter_edited.png");
    r.p->setProgram(2);
    r.page->refresh();
    check(bp->isChecked() && fourth->isChecked() && state().freq == 135.f && state().res == 90.f, "  a preset (\"Bass Hum\") shows in the page");
    r.page->grab().save(out + "/resonantfilter_preset.png");
    delete r.page;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : ".";
    parametricEq(out);
    paragraphicEq(out);
    graphicDynamics(out);
    multiBandDynamics(out);
    resonantFilter(out);
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
