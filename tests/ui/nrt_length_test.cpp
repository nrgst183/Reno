// Tools > Audio > Apply Non-Real-Time Event FX with effects that make the sound longer or shorter (Time Stretch, ExpressFX
// Time Stretch, Gapper/Snipper, Vibrato): the new take's file is as long as the effects make the sound (Time Stretch to
// 200 %: twice as long), not cut to the event; the event keeps its length; effects that give back what they are given
// work in the same chain; one undo step.
//
//   nrt_length_test <output folder>
//
// Set HOME to a scratch folder. QT_QPA_PLATFORM=offscreen.
#include "../../src/app/audiotools.h"
#include "../../src/app/fxwindow.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/engine/engine.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AudioPluginDesc builtinAudio(const std::string& uid)
{
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.uid == uid) return e.desc;
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = uid;
    return d;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoNrtLengthTest");
    QApplication::setApplicationName("nrt_length_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: nrt_length_test <output folder>"); return 2; }
    const QString out = args[1];
    QDir(out).removeRecursively();
    QDir().mkpath(out);
    uitest::setup({}, {});
    AppState& app = AppState::get();
    std::string err;

    // a 2 second tone file
    const std::string tone = (out + "/tone.wav").toStdString();
    {
        app.newProject();
        const uint64_t m = app.importMedia("synthaudio:1", &err);
        const std::vector<uint64_t> placed = app.addMediaToTimeline(m, 0.0);
        app.edit("Short", [&](Project& p) { for (uint64_t id : placed) p.findEvent(id)->length = 2.0; });
        check(realTimeRenderTo(tone, &err), "setup: the tone file is made", err);
    }

    // runs 'uid' with the given parameters (0..1 each, by index) and returns the new take's length in seconds (0 on failure)
    auto apply = [&](const std::vector<std::pair<std::string, std::vector<std::pair<int, float>>>>& effects, double* eventLength, size_t* undoSteps) {
        app.newProject();
        const uint64_t m = app.importMedia(tone, &err);
        const std::vector<uint64_t> placed = app.addMediaToTimeline(m, 0.0);
        const uint64_t E = placed.front();
        FxChain chain;
        for (const auto& fx : effects) {
            FxTarget t;
            t.kind = FxTarget::Event;
            t.id = E;
            const AudioPluginDesc d = builtinAudio(fx.first);
            const uint64_t slot = app.addAudioFx(t, d);
            app.fx.prepare(app.project());
            AudioPlugin* pl = app.fx.audio(slot);
            if (!pl) return 0.0;
            for (const auto& pv : fx.second) pl->paramSet(pv.first, pv.second);
            app.edit("Set", [&](Project& p) { app.fx.captureStates(p); });
            for (const FxSlot& s : app.project().findEvent(E)->fx)
                if (s.id == slot) chain.push_back(s);
        }
        const double len0 = app.project().findEvent(E)->length;
        const size_t u0 = app.undoCount();
        NrtOptions o;
        o.folder = out.toStdString();
        if (!applyNonRealTimeFxToEvents({E}, chain, o, &err)) return 0.0;
        const Event* ev = app.project().findEvent(E);
        if (eventLength) *eventLength = ev->length - len0;
        if (undoSteps) *undoSteps = app.undoCount() - u0;
        const MediaItem* tm = app.project().findMedia(AppState::takesOf(*ev).back().mediaId);
        if (!tm) return 0.0;
        return tm->info.duration;
    };

    // Time Stretch to 200 % (Final Length 50..500 %: 200 is (200 - 50) / 450 of the range)
    {
        double dl = 1;
        size_t steps = 0;
        const double d = apply({{"reno.timestretch", {{0, (float)((200.0 - 50.0) / 450.0)}}}}, &dl, &steps);
        check(std::fabs(d - 4.0) < 0.02, "Time Stretch to 200 %: the new take is twice as long", std::to_string(d));
        check(std::fabs(dl) < 1e-9, "the event keeps its length");
        check(steps == 1, "one undo step", std::to_string(steps));
    }
    {
        const double d = apply({{"reno.timestretch", {{0, (float)((50.0 - 50.0) / 450.0)}}}}, nullptr, nullptr);
        check(std::fabs(d - 1.0) < 0.1, "Time Stretch to 50 %: half as long (give or take the engine's frame)", std::to_string(d));
    }
    {
        const double d = apply({{"reno.xptimestretch", {{0, (float)((150.0 - 50.0) / 450.0)}}}}, nullptr, nullptr);
        check(std::fabs(d - 3.0) < 0.1, "ExpressFX Time Stretch to 150 %: one and a half times as long", std::to_string(d));
    }
    {
        // Time Stretch to 200 %, then Distortion (which gives back what it is given): the chain works in order
        const double d = apply({{"reno.timestretch", {{0, (float)((200.0 - 50.0) / 450.0)}}}, {"reno.distortion", {}}}, nullptr, nullptr);
        check(std::fabs(d - 4.0) < 0.02, "Time Stretch then Distortion: twice as long", std::to_string(d));
    }
    {
        // Gapper (the default 20 Hz, 75 ms: the result is 250 % of the original) and Snipper (40 %)
        const double g = apply({{"reno.gapsnip", {}}}, nullptr, nullptr);
        check(g > 4.0 && g < 5.5, "Gapper: longer than the event (about 5 s)", std::to_string(g));
        const double s = apply({{"reno.gapsnip", {{0, 1.0f}}}}, nullptr, nullptr);
        check(s > 0.5 && s < 1.3, "Snipper: shorter than the event (about 0.8 s)", std::to_string(s));
    }
    {
        // Pitch Shift without "Preserve duration" (the default): an octave up is half as long, an octave down twice as long;
        // with the box ticked the length stays
        const double up = apply({{"reno.pitchshift", {{0, 0.62f}}}}, nullptr, nullptr);
        check(std::fabs(up - 1.0) < 0.1, "Pitch Shift an octave up without Preserve duration: half as long", std::to_string(up));
        const double dn = apply({{"reno.pitchshift", {{0, 0.38f}}}}, nullptr, nullptr);
        check(std::fabs(dn - 4.0) < 0.1, "Pitch Shift an octave down: twice as long", std::to_string(dn));
        const double keep = apply({{"reno.pitchshift", {{0, 0.62f}, {4, 1.0f}}}}, nullptr, nullptr);
        check(std::fabs(keep - 2.0) < 0.1, "Pitch Shift with Preserve duration: as long as the event", std::to_string(keep));
    }
    {
        // an effect that gives back what it is given: the file is as long as the event, as before
        const double d = apply({{"reno.distortion", {}}}, nullptr, nullptr);
        check(std::fabs(d - 2.0) < 0.02, "Distortion alone: as long as the event", std::to_string(d));
    }
    return uitest::summary();
}
