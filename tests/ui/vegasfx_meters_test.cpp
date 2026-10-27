// The level meters of Track Compressor and Multi-Band Dynamics: the plug-ins measure what runs through them
// (AudioPlugin::meterTake) and their pages show it (LevelMeter bars fed by a timer).
//
//   vegasfx_meters_test <output folder>      (run with QT_QPA_PLATFORM=offscreen)
#include "../../src/app/fxpages.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasmbdyn.h"
#include <QApplication>
#include <QCheckBox>
#include <QThread>
#include <QWidget>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

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
        p->activate(48000, 4096);
        host.range = [this](int i, double* lo, double* hi) { return p->paramRange(i, lo, hi); };
        host.get = [this](int i) {
            double lo = 0, hi = 1;
            p->paramRange(i, &lo, &hi);
            return lo + (double)p->paramGet(i) * (hi - lo);
        };
        host.set = [](int, double) {};
        host.name = [this](int i) { return p->paramName(i); };
        host.begin = [](int) {};
        host.end = [] {};
        host.state = [this] { return p->saveState(); };
        host.setState = [this](const std::string& st, const std::string&) { p->loadState(st); };
        host.beginEdit = [](const std::string&) {};
        host.meter = [this](int i) { return p->meterTake(i); };
        page = makeBuiltinPage(nullptr, uid, host);
    }
    // 'blocks' blocks of 480 frames of a sine of the given amplitude on the left, half of it on the right
    void play(double amp, int blocks, int64_t* pos)
    {
        std::vector<float> l(480), r(480);
        for (int b = 0; b < blocks; ++b) {
            for (int k = 0; k < 480; ++k) {
                const double s = std::sin(2 * 3.14159265358979 * 1000.0 * (double)(*pos + k) / 48000.0);
                l[(size_t)k] = (float)(amp * s);
                r[(size_t)k] = (float)(amp * 0.5 * s);
            }
            p->process(l.data(), r.data(), 480, *pos, true);
            *pos += 480;
        }
    }
};

static double dbOf(float x) { return x > 0.f ? 20.0 * std::log10((double)x) : -999.0; }

static void trackCompressor(const QString& out)
{
    Rig r("reno.trkcomp");
    check(r.p->meterCount() == 5, "Track Compressor has 5 meters (input L/R, output L/R, reduction)");
    int64_t pos = 0;
    r.p->meterTake(0);
    r.play(0.0, 2, &pos);
    check(r.p->meterTake(0) == 0.f && r.p->meterTake(4) == 1.f, "  silence: no level and no reduction");
    // threshold -30 dB, 4:1: a loud sine is compressed
    r.p->setProgram(0);
    r.p->paramSet(2, (float)((-30.0 - -60.0) / 60.0));
    r.p->paramSet(3, (float)((4.0 - 1.0) / 49.0));
    r.p->paramSet(4, 0.f);
    r.p->paramSet(6, 0.f);                  // no automatic gain compensation
    r.play(0.5, 60, &pos);
    const float inL = r.p->meterTake(0), inR = r.p->meterTake(1), outL = r.p->meterTake(2), red = r.p->meterTake(4);
    check(std::fabs(dbOf(inL) - dbOf(0.5f)) < 0.2 && std::fabs(dbOf(inR) - dbOf(0.25f)) < 0.2, "  input meters read the peaks of the two channels");
    check(outL < inL, "  the output is quieter than the input when compressing");
    check(red < 0.9f && red > 0.f, "  the reduction meter shows gain taken off");
    check(r.p->meterTake(0) == 0.f, "  a meter starts again from nothing after being read");
    check(r.page != nullptr, "  the page is there");
    if (!r.page) return;
    r.page->show();
    r.play(0.5, 60, &pos);
    for (int k = 0; k < 6; ++k) {
        QApplication::processEvents();
        QThread::msleep(50);
        r.play(0.5, 10, &pos);
    }
    QApplication::processEvents();
    QWidget* mi = r.page->findChild<QWidget*>("dxMeterInput");
    QWidget* mo = r.page->findChild<QWidget*>("dxMeterOutput");
    QWidget* mr = r.page->findChild<QWidget*>("dxMeterReduction");
    check(mi && mo && mr, "  the page has the input, output and reduction meters");
    r.page->grab().save(out + "/trackcompressor_meters.png");
    delete r.page;
}

static void multiBand(const QString& out)
{
    Rig r("reno.multibanddynamics");
    check(r.p->meterCount() == 12, "Multi-Band Dynamics has 12 meters (3 for each band)");
    int64_t pos = 0;
    // band 2 (1 kHz, band-notch) on, with a threshold of -40 dB and 10:1
    vegmb::Settings s;
    vegmb::decode(r.p->saveState(), &s);
    s.bypass[1] = 0;
    s.threshold[1] = -400;
    s.amount[1] = 100;
    r.p->loadState(vegmb::encode(s));
    r.play(0.5, 80, &pos);
    float peak = 0.f, gain = 2.f;
    for (int k = 0; k < 12; ++k) {
        const float v = r.p->meterTake(k);
        if (k == 3) peak = v;
        if (k == 5) gain = v;
    }
    check(peak > 0.01f, "  band 2 sees the 1 kHz sine");
    check(gain < 1.f && gain > 0.f, "  band 2's reduction meter shows its compressor working");
    check(r.p->meterTake(3) == 0.f, "  a meter starts again from nothing after being read");
    if (!r.page) { check(false, "  the page is there"); return; }
    r.page->show();
    for (int k = 0; k < 6; ++k) {
        r.play(0.5, 10, &pos);
        QApplication::processEvents();
        QThread::msleep(50);
    }
    QWidget* b2 = r.page->findChild<QWidget*>("dxMeterBand2");
    QWidget* r2 = r.page->findChild<QWidget*>("dxMeterReduction2");
    check(b2 && r2 && r.page->findChild<QWidget*>("dxMeterBand4"), "  the page has the level and reduction meters of each band");
    QCheckBox* en = r.page->findChild<QCheckBox*>("dxMbEnableMeters");
    check(en && en->isChecked(), "  \"Enable meters\" is on as VEGAS's default");
    r.page->grab().save(out + "/multibanddynamics_meters.png");
    if (en) {
        en->setChecked(false);
        vegmb::Settings t;
        vegmb::decode(r.p->saveState(), &t);
        check(t.extra[8] == 0, "  switching it off is kept in the saved form");
    }
    delete r.page;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : ".";
    trackCompressor(out);
    multiBand(out);
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
