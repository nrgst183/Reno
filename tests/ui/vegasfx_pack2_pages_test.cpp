// The pages of Reno's reproductions of VEGAS's sfppack2 / sfresfilter effects (src/app/fxpages_pack2.inc): each page is
// made over the real built-in effect, its controls are driven, and a picture of each is saved.
//
//   vegasfx_pack2_pages_test <output folder>
// (run with QT_QPA_PLATFORM=offscreen)
#include "../../src/app/fxcurvegraph.h"
#include "../../src/app/fxpages.h"
#include "../../src/audiofx/vegasgraphiceq.h"
#include "../../src/audiofx/audioplugin.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabWidget>
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

static void noiseGate(const QString& out)
{
    Rig r("reno.noisegate");
    check(r.page != nullptr, "Noise Gate has a page");
    if (!r.page) return;
    r.page->show();
    r.page->grab().save(out + "/noisegate_default.png");
    QSlider* thr = r.page->findChild<QSlider*>("dxParam2");
    QSlider* att = r.page->findChild<QSlider*>("dxParam0");
    QSlider* rel = r.page->findChild<QSlider*>("dxParam1");
    check(thr && att && rel, "  threshold, attack and release sliders");
    if (!thr || !att || !rel) return;
    check(std::fabs(value(r, 2) + 20.0) < 0.06 && value(r, 0) == 3 && value(r, 1) == 100, "  they show VEGAS's defaults");
    thr->setValue(thr->minimum());
    check(value(r, 2) <= -79.9, "  the threshold fader at its bottom is -Inf.");
    QLineEdit* relBox = r.page->findChild<QLineEdit*>("dxParam1Edit");
    check(relBox != nullptr, "  the release has an edit box");
    if (relBox) {
        relBox->setText("2,500");
        emit relBox->editingFinished();
        check(value(r, 1) == 2500, "  \"2,500\" in the release box");
        check(relBox->text() == "2,500", "  is shown with its comma");
    }
    att->setValue(att->maximum());
    check(value(r, 0) == 500, "  the attack slider at its end is 500 ms");
    r.p->setProgram(4);
    r.page->refresh();
    check(value(r, 0) == 10 && value(r, 1) == 500 && std::fabs(value(r, 2) + 60) < 0.06, "  a preset shows in the page");
    r.page->grab().save(out + "/noisegate_preset.png");
    delete r.page;
}

// a mouse event on the graph at data (x, y)
static void mouse(CurveGraph* g, QEvent::Type t, int x, int y, Qt::MouseButton b = Qt::LeftButton)
{
    const QPointF at = g->at(x, y);
    QMouseEvent e(t, at, g->mapToGlobal(at), b, t == QEvent::MouseButtonRelease ? Qt::NoButton : b, Qt::NoModifier);
    QApplication::sendEvent(g, &e);
}

static void graphicEq(const QString& out)
{
    Rig r("reno.graphiceq");
    check(r.page != nullptr, "Graphic EQ has a page");
    if (!r.page) return;
    r.page->show();
    QTabWidget* tabs = r.page->findChild<QTabWidget*>("dxGeqTabs");
    check(tabs && tabs->count() == 3 && tabs->tabText(0) == "10 Band" && tabs->tabText(1) == "20 Band" && tabs->tabText(2) == "Envelope", "  it has the 10 Band, 20 Band and Envelope views");
    if (!tabs) return;
    auto state = [&] {
        veggeq::Settings s;
        veggeq::decode(r.p->saveState(), &s);
        return s;
    };
    auto band = [&](int b) { return r.page->findChild<QSlider*>(QString("dxGeq20Band%1").arg(b)); };
    auto band10 = [&](int b) { return r.page->findChild<QSlider*>(QString("dxGeq10Band%1").arg(b)); };
    for (int t = 0; t < 3; ++t) {
        tabs->setCurrentIndex(t);
        r.page->grab().save(out + QString("/graphiceq_default_%1.png").arg(t));
    }
    int bands = 0;
    for (int b = 0; b < 20; ++b) bands += band(b) != nullptr;
    int bands10 = 0;
    for (int b = 0; b < 20; ++b) bands10 += band10(b) != nullptr;
    check(bands == 20 && bands10 == 10 && band10(1) && band10(19) && !band10(0), "  20 band faders, and 10 (the odd bands) on the 10 band view");
    // a band fader moves a trapezoid of the envelope and is saved with the settings
    QSlider* b4 = band(4);
    check(b4 != nullptr && b4->minimum() == 0 && b4->maximum() == 720, "  a fader is -Inf. (-48 dB) to 24 dB in tenths");
    if (!b4) return;
    b4->setValue(480 + 60);
    {
        const veggeq::Settings s = state();
        veggeq::Settings want;
        int32_t p[veggeq::kMaxPoints + 8][2] = {};
        for (int k = 0; k < want.count; ++k) p[k][0] = want.pt[k][0], p[k][1] = want.pt[k][1];
        int n = want.count;
        veggeq::bandToPoints(p, &n, 4, 60, 250);
        bool same = s.band[4] == 60 && s.count == n;
        for (int k = 0; k < n && same; ++k) same = s.pt[k][0] == p[k][0] && s.pt[k][1] == p[k][1];
        check(same && n > 2, "  band 5 (80 Hz) at +6 dB makes the plug-in's trapezoid in the envelope");
    }
    // the same envelope is in the graph, and the fader's readout says its level
    tabs->setCurrentIndex(2);
    CurveGraph* g = dynamic_cast<CurveGraph*>(r.page->findChild<QWidget*>("dxGraph"));
    check(g != nullptr && (int)g->pts.size() == state().count, "  the graph shows those points");
    if (!g) return;
    r.page->grab().save(out + "/graphiceq_band.png");
    // dragging a point of the graph
    const int before = state().count, y0 = state().pt[1][1], x0 = state().pt[1][0];
    mouse(g, QEvent::MouseButtonPress, x0, y0);
    mouse(g, QEvent::MouseMove, x0, y0 + 100);
    mouse(g, QEvent::MouseButtonRelease, x0, y0 + 100);
    check(state().count == before && state().pt[1][1] == y0 + 100 && state().pt[1][0] == x0, "  a point can be dragged (up and down, and the saved form follows)");
    // clicking on a line adds a point; double clicking it removes it
    {
        const veggeq::Settings s0 = state();
        const int mx = (s0.pt[0][0] + s0.pt[1][0]) / 2;
        const int my = (s0.pt[0][1] + s0.pt[1][1]) / 2;
        mouse(g, QEvent::MouseButtonPress, mx, my);
        mouse(g, QEvent::MouseButtonRelease, mx, my);
        check(state().count == s0.count + 1, "  a click on the line adds a point");
        const veggeq::Settings s1 = state();
        mouse(g, QEvent::MouseButtonPress, s1.pt[1][0], s1.pt[1][1], Qt::RightButton);
        mouse(g, QEvent::MouseButtonRelease, s1.pt[1][0], s1.pt[1][1], Qt::RightButton);
        check(state().count == s0.count, "  a right click on a point deletes it");
    }
    // the output gain and the accuracy
    QSlider* og = r.page->findChild<QSlider*>("dxParam1");
    QComboBox* acc = r.page->findChild<QComboBox*>("dxParam0");
    check(og != nullptr && acc != nullptr, "  output gain fader and accuracy box");
    if (og && acc) {
        og->setValue(og->maximum());
        check(state().outGain == 200, "  the output gain fader's top is +20 dB");
        acc->setCurrentIndex(2);
        check(state().accuracy == 2 && r.p->latencySamples() == 2048, "  the accuracy box sets the filter size (and the delay)");
    }
    // Reset: the envelope and, on the band views, the faders
    QPushButton* reset = r.page->findChild<QPushButton*>("dxReset");
    check(reset != nullptr, "  a Reset button");
    if (reset) {
        tabs->setCurrentIndex(2);
        reset->click();
        check(state().count == 2 && state().band[4] == 60, "  Reset on the Envelope view resets only the envelope");
        b4->setValue(480 + 60);
        tabs->setCurrentIndex(1);
        reset->click();
        check(state().count == 2 && state().band[4] == 0, "  and on a band view the faders too");
    }
    // presets show
    r.p->setProgram(3);
    r.page->refresh();
    check(g->pts.size() == 6 && g->pts[2].y == 202, "  a preset (\"Boost bass around 80 Hz by 6 dB\") shows in the graph");
    tabs->setCurrentIndex(2);
    r.page->grab().save(out + "/graphiceq_preset.png");
    delete r.page;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : ".";
    noiseGate(out);
    graphicEq(out);
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
