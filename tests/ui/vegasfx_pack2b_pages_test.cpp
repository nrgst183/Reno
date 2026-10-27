// The pages of the second batch of sfppack2 / sfresfilter reproductions (Parametric EQ, ...; src/app/fxpages_pack2.inc):
// each page is made over the real built-in effect, its controls are driven, and a picture of each is saved.
//
//   vegasfx_pack2b_pages_test <output folder>
// (run with QT_QPA_PLATFORM=offscreen)
#include "../../src/app/fxpages.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegaspmeq.h"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QSlider>
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

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : ".";
    parametricEq(out);
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
