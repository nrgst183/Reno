// fuzz_fakegen: makes the project and manifest.json of a made-up "Reno Plugin Fuzz.cs" run (tools/scripts), for trying
// tools/fuzz_compare.py without VEGAS (tests/cli/fuzz_compare_test.sh drives it).
//
//   fuzz_fakegen <folder> <picture.png> <sound.wav>
//
// The folder gets fuzz_picture.png, fuzz_picture2.png, fuzz_test.wav, fuzz.veg (written by Reno's .veg writer) and
// manifest.json in the script's layout: video tests of Reno's built-in effects (and one of a plug-in nobody has), one
// second each, two transitions, and two audio effects of Reno's, each 3.5 s. The snapshot files and fuzz_audio.wav named
// there are made afterwards (by the dry run).
#include "../../src/app/appstate.h"
#include "../../src/audiofx/audioplugin.h"
#include <QGuiApplication>
#include <QFile>
#include <cstdio>
#include <string>

using namespace reno;
using namespace reno::app;

static const int kFps = 30;

struct VTest {
    std::string kind, plugin, id, variant;
    int startFrame = 0, lengthFrames = 0, transStart = 0;
};

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: fuzz_fakegen <folder> <picture.png> <sound.wav>\n");
        return 2;
    }
    const std::string folder = argv[1];
    int qargc = 1;
    char arg0[] = "fuzz_fakegen";
    char* qargv[] = {arg0, nullptr};
    QGuiApplication qapp(qargc, qargv);
    QGuiApplication::setOrganizationName("Reno");
    QGuiApplication::setApplicationName("RenoFuzzFake");
    QFile::copy(argv[2], QString::fromStdString(folder + "/fuzz_picture.png"));
    QFile::copy(argv[2], QString::fromStdString(folder + "/fuzz_picture2.png"));
    QFile::copy(argv[3], QString::fromStdString(folder + "/fuzz_test.wav"));

    AppState& app = AppState::get();
    app.newProject(true);
    app.edit("Settings", [](Project& p) {
        p.settings.width = 320;
        p.settings.height = 180;
        p.settings.fpsNum = kFps;
        p.settings.fpsDen = 1;
        p.settings.sampleRate = 48000;
    });
    std::string err;
    const uint64_t pic1 = app.importMedia(folder + "/fuzz_picture.png", &err);
    const uint64_t pic2 = app.importMedia(folder + "/fuzz_picture2.png", &err);
    const uint64_t snd = app.importMedia(folder + "/fuzz_test.wav", &err);
    if (!pic1 || !pic2 || !snd) {
        std::fprintf(stderr, "media: %s\n", err.c_str());
        return 1;
    }
    const uint64_t v1 = app.addTrack(TrackType::Video), v2 = app.addTrack(TrackType::Video), a1 = app.addTrack(TrackType::Audio);

    std::vector<VTest> tests;
    std::vector<std::pair<std::string, std::string>> effects = {{"com.vegascreativesoftware:invert", "Invert"},
                                                                {"com.vegascreativesoftware:blackandwhite", "Black and White"},
                                                                {"com.vegascreativesoftware:gaussianblur", "Gaussian Blur"},
                                                                {"com.vegascreativesoftware:brightnessandcontrast", "Brightness and Contrast"},
                                                                {"com.example.fuzz:nobodyhasthis", "Nobody Has This"}};
    int frame = 0;
    for (const auto& fx : effects) {
        VTest t;
        t.kind = "video";
        t.plugin = fx.second;
        t.id = fx.first;
        t.variant = "default";
        t.startFrame = frame;
        t.lengthFrames = 30;
        tests.push_back(t);
        uint64_t ev = 0;
        app.edit("Add event", [&](Project& p) {
            Track* tr = p.findTrack(v1);
            Event e;
            e.id = ev = p.newId();
            e.mediaId = pic1;
            e.start = frame / (double)kFps;
            e.length = 1.0;
            tr->events.push_back(e);
        });
        app.addVideoFx(FxTarget{FxTarget::Event, ev}, fx.first, fx.second);
        frame += 30;
    }
    const char* trans[][2] = {{"com.vegascreativesoftware:linearwipe", "Linear Wipe"}, {"", "Crossfade"}};
    for (const auto& tr : trans) {
        VTest t;
        t.kind = "transition";
        t.plugin = tr[1];
        t.id = tr[0];
        t.variant = "default";
        t.startFrame = frame;
        t.lengthFrames = 60;
        t.transStart = frame + 15;
        tests.push_back(t);
        uint64_t evB = 0;
        const int f0 = frame;
        app.edit("Add events", [&](Project& p) {
            Track* t2 = p.findTrack(v2);
            Event a;
            a.id = p.newId();
            a.mediaId = pic1;
            a.start = f0 / (double)kFps;
            a.length = 45 / (double)kFps;
            t2->events.push_back(a);
            Event b;
            b.id = evB = p.newId();
            b.mediaId = pic2;
            b.start = (f0 + 15) / (double)kFps;
            b.length = 45 / (double)kFps;
            b.fadeIn = 1.0;
            t2->events.push_back(b);
        });
        if (tr[0][0]) app.setTransition(evB, tr[0], tr[1]);
        frame += 60;
    }
    // audio: effects that a .veg can hold - Reno's reproduction of the eFX Limiter (VST2 'MX2A') three times, and a plug-in
    // nobody has - at event level, 3.5 s apart
    auto idOf = [](const char* s) { return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3]; };
    std::vector<VTest> audio;
    double at = 0;
    for (int k = 0; k < 4; ++k) {
        AudioPluginDesc d;
        d.format = "VST2";
        d.uid = std::to_string(idOf(k < 3 ? "MX2A" : "ZZZZ"));
        d.name = k < 3 ? "eFX Limiter" : "Mystery Plugin";
        VTest t;
        t.kind = "audio";
        t.plugin = d.name;
        t.id = d.uid;
        t.variant = k == 1 ? "preset:Second" : "default";
        t.startFrame = (int)(at * kFps + 0.5);
        t.lengthFrames = (int)(3.5 * kFps + 0.5);
        audio.push_back(t);
        uint64_t ev = 0;
        const double start = at;
        app.edit("Add event", [&](Project& p) {
            Track* tr = p.findTrack(a1);
            Event e;
            e.id = ev = p.newId();
            e.mediaId = snd;
            e.start = start;
            e.length = 2.0;
            tr->events.push_back(e);
        });
        app.addAudioFx(FxTarget{FxTarget::Event, ev}, d);
        at += 3.5;
    }
    std::string e2;
    if (!app.save(folder + "/fuzz.veg", &e2)) {
        std::fprintf(stderr, "save: %s\n", e2.c_str());
        return 1;
    }

    // manifest.json in the script's layout
    std::string m = "{\n  \"tool\": \"Reno Plugin Fuzz\", \"format\": 1,\n  \"vegas\": \"fake (fuzz_fakegen)\",\n";
    m += "  \"project\": {\"file\": \"fuzz.veg\", \"width\": 320, \"height\": 180, \"fps\": 30, \"sample_rate\": 48000, \"channels\": 2},\n";
    m += "  \"audio_effect_mode\": \"event\",\n  \"audio_render\": {\"file\": \"fuzz_audio.wav\", \"template\": \"fake\", \"status\": \"ok\"},\n  \"tests\": [\n";
    char buf[1024];
    int id = 0;
    auto fnum = [](double d) { char b[64]; std::snprintf(b, sizeof b, "%.9g", d); return std::string(b); };
    for (const VTest& t : tests) {
        std::string snaps;
        if (t.kind == "video") {
            std::snprintf(buf, sizeof buf, "{\"file\": \"snap_%04d.png\", \"frame\": %d, \"time_s\": %s, \"percent\": 50, \"result\": \"ok\"}", id, t.startFrame + 15,
                          fnum((t.startFrame + 15) / 30.0).c_str());
            snaps = buf;
        } else {
            const int pc[3] = {25, 50, 75};
            for (int k = 0; k < 3; ++k) {
                const int fr = t.transStart + (int)(30 * pc[k] / 100.0 + 0.5);
                std::snprintf(buf, sizeof buf, "%s{\"file\": \"snap_%04d_%d.png\", \"frame\": %d, \"time_s\": %s, \"percent\": %d, \"result\": \"ok\"}", k ? ", " : "", id, pc[k], fr,
                              fnum(fr / 30.0).c_str(), pc[k]);
                snaps += buf;
            }
        }
        std::snprintf(buf, sizeof buf,
                      "    {\"id\": %d, \"kind\": \"%s\", \"plugin\": \"%s\", \"uid\": \"%s\", \"class_id\": \"\", \"ofx\": true, \"variant\": \"%s\", \"status\": \"ok\", \"error\": \"\", \"note\": \"\", "
                      "\"start_s\": %s, \"length_s\": %s, \"start_frame\": %d, \"length_frames\": %d, \"snapshots\": [%s], \"params\": []},\n",
                      id, t.kind.c_str(), t.plugin.c_str(), t.id.c_str(), t.variant.c_str(), fnum(t.startFrame / 30.0).c_str(), fnum(t.lengthFrames / 30.0).c_str(), t.startFrame,
                      t.lengthFrames, snaps.c_str());
        m += buf;
        ++id;
    }
    for (size_t k = 0; k < audio.size(); ++k) {
        const VTest& t = audio[k];
        std::snprintf(buf, sizeof buf,
                      "    {\"id\": %d, \"kind\": \"audio\", \"plugin\": \"%s\", \"uid\": \"%s\", \"class_id\": \"\", \"ofx\": false, \"variant\": \"%s\", \"status\": \"ok\", \"error\": \"\", \"note\": \"\", "
                      "\"start_s\": %s, \"length_s\": 3.5, \"start_frame\": %d, \"length_frames\": %d, \"event_length_s\": 2, \"mode\": \"event\", \"snapshots\": [], \"params\": []}%s\n",
                      id, t.plugin.c_str(), t.id.c_str(), t.variant.c_str(), fnum(t.startFrame / 30.0).c_str(), t.startFrame, t.lengthFrames, k + 1 < audio.size() ? "," : "");
        m += buf;
        ++id;
    }
    m += "  ]\n}\n";
    writeFile(folder + "/manifest.json", m);
    std::printf("fuzz_fakegen: %d tests, %d frames of video, project saved\n", id, frame);
    return 0;
}
