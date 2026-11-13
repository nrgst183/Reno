// The effects window's page for Reno's eFX effects (src/app/fxpages_efx.cpp): shown instead of the list of sliders, its knobs
// drag, reset and step through undo, the on / off switch, Reset, A / B compare and the spanner's row; and the meters the
// effects measure (efx::EfxPlugin::meterTake) as the sound passes.
//
//   efxpage_test [output folder]
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/playback.h"
#include "../../src/audiofx/efx.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AudioPlugin* plugin(uint64_t slot) { return AppState::get().fx.audio(slot); }
float val(uint64_t slot, int i) { AudioPlugin* p = plugin(slot); return p ? p->paramGet(i) : -1.f; }

uint64_t addEfx(uint64_t id, const char* uid, const char* name)
{
    AppState::get().edit("Add " + std::string(name), [&](Project& p) {
        FxSlot s;
        s.id = id;
        s.kind = "audio";
        s.name = name;
        s.audio.format = "VST2";
        s.audio.uid = uid;
        s.audio.name = name;
        p.audioOut.push_back(s);
    });
    return id;
}

// the page's knob centres are not widgets: these are where fxpages_efx.cpp puts them (a 524 x 436 page)
QPoint rowKnob(int index, int count)
{
    const double gap = count >= 5 ? 96 : count == 4 ? 110 : count == 3 ? 105 : 125;
    return QPoint((int)std::lround(262 - gap * (count - 1) / 2.0 + gap * index), 322 + 56);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("efxpage_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("efxpage_out");
    QDir().mkpath(out);
    uitest::setup({}, {});

    // ------------------------------------------------------------------ the meters
    {
        std::string err;
        AudioPluginDesc d;
        d.format = "VST2";
        d.uid = "1297625655";           // 'MX27', the Compressor
        std::unique_ptr<AudioPlugin> p = efxCreate(d, &err);
        check(p && p->meterCount() == efx::EfxPlugin::MeterCount, "an eFX effect has meters for its page");
        if (p) {
            p->activate(48000, 512);
            p->paramSet(6, 0.5f);       // threshold -30 dB
            std::vector<float> l(512), r(512);
            for (int k = 0; k < 512; ++k) l[k] = r[k] = 0.5f * (float)std::sin(k * 0.05);
            const std::vector<float> l0 = l;
            for (int b = 0; b < 8; ++b) {
                for (int k = 0; k < 512; ++k) l[k] = r[k] = 0.5f * (float)std::sin((b * 512 + k) * 0.05);
                p->process(l.data(), r.data(), 512, (int64_t)b * 512, true);
            }
            const float inL = p->meterTake(efx::EfxPlugin::MeterInL), outL = p->meterTake(efx::EfxPlugin::MeterOutL);
            const float gr = p->meterTake(efx::EfxPlugin::MeterGain);
            check(std::fabs(inL - 0.5f) < 0.01f, "the input meter reads the peak going in", strfmt("%.3f", inL));
            check(outL > 0.f && outL < inL, "the output meter the (compressed) peak coming out", strfmt("%.3f", outL));
            check(gr > 0.f && gr < 0.5f, "the gain meter the smallest gain the compressor applied (well below 1 at -30 dB)", strfmt("%.3f", gr));
            check(p->meterTake(efx::EfxPlugin::MeterInL) == 0.f && p->meterTake(efx::EfxPlugin::MeterGain) == 1.f, "a reading is taken once (silence: 0, no reduction: 1)");
        }
    }

    AppState& app = AppState::get();
    app.newProject();
    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    w->resize(780, 700);
    w->show();
    uitest::settle();
    FxTarget aout;
    aout.kind = FxTarget::AudioOut;

    // ------------------------------------------------------------------ the Compressor's page
    const uint64_t comp = addEfx(9201, "1297625655", "eFX Compressor");
    w->showTarget(aout, comp);
    uitest::settle(400);
    QWidget* page = w->findChild<QWidget*>("efxPage");
    check(page && page->isVisible() && page->size() == QSize(524, 436), "an eFX effect shows its page, not the list of sliders");
    check(!w->findChild<QWidget*>("fxAudioRow"), "  (no rows of sliders)");
    uitest::shot(w, out + "/compressor.png");
    if (page) {
        // threshold: the second of five knobs, dragged down
        const QPoint th = rowKnob(1, 5);
        const float t0 = val(comp, 6);
        uitest::mouseDrag(page, th, th + QPoint(0, 60), 8);
        uitest::settle();
        const float t1 = val(comp, 6);
        check(std::fabs(t1 - (t0 - 0.3f)) < 0.02f, "dragging a knob down 60 pixels turns it down (200 pixels the whole way)", strfmt("%.3f -> %.3f", t0, t1));
        check(app.undoName() == "Change \"Threshold\"", "  as one undo step", app.undoName());
        app.undo();
        uitest::settle();
        check(std::fabs(val(comp, 6) - t0) < 1e-6f, "  undo takes it back");
        app.redo();
        uitest::mouseDoubleClick(page, th);
        uitest::settle();
        check(std::fabs(val(comp, 6) - 1.f) < 1e-6f, "a double-click sets the default (0 dB)");
        // the wheel
        uitest::wheel(page, th, -240);
        uitest::settle();
        check(std::fabs(val(comp, 6) - 0.98f) < 1e-4f, "the wheel steps a hundredth a notch", strfmt("%.4f", val(comp, 6)));
        // the on / off switch at the top left
        uitest::mouseClick(page, QPoint(20, 14));
        uitest::settle();
        check(val(comp, 0) < 0.5f, "the switch at the top left turns the effect off (Active)");
        uitest::mouseClick(page, QPoint(20, 14));
        uitest::settle();
        check(val(comp, 0) > 0.5f, "  and on again");
        // A / B
        uitest::mouseClick(page, QPoint(428, 14));      // B (a copy of A to start with)
        uitest::settle();
        uitest::mouseDrag(page, rowKnob(2, 5), rowKnob(2, 5) + QPoint(0, -40), 6);    // ratio up in B
        uitest::settle();
        const float ratioB = val(comp, 7);
        uitest::mouseClick(page, QPoint(382, 14));      // back to A
        uitest::settle();
        check(std::fabs(val(comp, 7) - 0.5f) < 1e-6f && ratioB > 0.6f, "A and B keep their own settings (B's ratio changed, A's not)",
              strfmt("A %.3f B %.3f", val(comp, 7), ratioB));
        uitest::mouseClick(page, QPoint(428, 14));
        uitest::settle();
        check(std::fabs(val(comp, 7) - ratioB) < 1e-6f, "  and B comes back as it was");
        // Reset
        uitest::mouseClick(page, QPoint(466, 14));
        uitest::settle();
        check(std::fabs(val(comp, 7) - 0.5f) < 1e-6f && std::fabs(val(comp, 6) - 1.f) < 1e-6f, "RESET sets every setting back to its default");
        check(app.undoName() == "Reset", "  as one undo step", app.undoName());
        // the side chain's filter shapes under the display
        uitest::mouseClick(page, QPoint(14 + 3 * 20 + 10, 292));
        uitest::settle();
        check(std::fabs(val(comp, 15) - 1.f) < 1e-6f, "the fourth filter shape sets SC Filter Type to its last place (1)");
        // the spanner: the settings the knob row has no room for
        uitest::mouseClick(page, QPoint(505, 41));
        uitest::settle();
        uitest::shot(w, out + "/compressor_more.png");
        const QPoint mix = rowKnob(2, 3);
        const float m0 = val(comp, 22);
        uitest::mouseDrag(page, mix, mix + QPoint(0, 40), 6);
        uitest::settle();
        check(val(comp, 22) < m0 - 0.1f, "the spanner swaps in the other settings (Mix is the third)", strfmt("%.3f -> %.3f", m0, val(comp, 22)));
    }

    // ------------------------------------------------------------------ every eFX effect has a page
    const struct { const char* uid; const char* name; } all[] = {
        {"1297625665", "eFX Limiter"}, {"1297625651", "eFX Reverb"},     {"1297625666", "eFX TubeStage"}, {"1297625650", "eFX Phaser"},
        {"1297625649", "eFX ChorusFlanger"}, {"1297625656", "eFX DeEsser"}, {"1297625657", "eFX Gate"}, {"1297625652", "eFX StereoDelay"},
        {"1297625906", "eFX TremoloPan"}, {"1297625904", "eFX VocalStrip"},
    };
    uint64_t id = 9300;
    for (const auto& e : all) {
        const uint64_t s = addEfx(++id, e.uid, e.name);
        w->showTarget(aout, s);
        uitest::settle(300);
        bool shown = false;
        for (QWidget* pg : w->findChildren<QWidget*>("efxPage")) shown = shown || pg->isVisible();
        check(shown, std::string(e.name) + " has its page");
        uitest::shot(w, out + "/" + QString::fromLatin1(e.name).remove("eFX ").toLower() + ".png");
    }
    return uitest::summary();
}
