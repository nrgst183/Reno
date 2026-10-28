// Tests of the Mixing Console's engine side: the Master Bus's separate left and right faders, the Surround Master's
// channel faders, VEGAS Pro 23's input busses (live sound from the sound card through insert effects, sends, fader and
// pan to a bus, the master or nowhere; left out of a normal render) and the project file for them.
//
//   console_test
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}
static std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }

struct Out {
    std::vector<float> l, r;
    explicit Out(int n = 0) : l((size_t)n), r((size_t)n) {}
};

static double peak(const std::vector<float>& v, size_t from = 0)
{
    double m = 0;
    for (size_t k = from; k < v.size(); ++k) m = std::max(m, (double)std::fabs(v[k]));
    return m;
}

// A fake sound card: input 0 is a constant 0.5 on the left and 0.25 on the right, input 1 a constant 0.1 on both.
static bool fakeInput(const InputBus& ib, float* l, float* r, int n)
{
    for (int k = 0; k < n; ++k) {
        if (ib.device == "mic") { l[k] = 0.5f; r[k] = ib.channels == 1 ? 0.5f : 0.25f; }
        else { l[k] = r[k] = 0.1f; }
    }
    return true;
}

static InputBus makeInputBus(Project& p, const std::string& device)
{
    InputBus b;
    b.id = p.newId();
    b.device = device;
    p.inputBuses.push_back(b);
    return b;
}

static Out render(AudioEngine& eng, const Project& p, int n = 4800)
{
    Out o(n);
    eng.render(p, 0, n, o.l.data(), o.r.data(), true);
    return o;
}

int main()
{
    MediaPool pool;
    FxRuntime fx;
    // ---- the Master Bus's two faders
    {
        Project p;
        InputBus ib = makeInputBus(p, "mic");
        (void)ib;
        AudioEngine eng(pool, fx);
        eng.liveInput = fakeInput;
        Out o = render(eng, p);
        check(std::fabs(o.l[100] - 0.5f) < 1e-6f && std::fabs(o.r[100] - 0.25f) < 1e-6f, "input bus: the sound card's input reaches the master", fmt(o.l[100]) + " " + fmt(o.r[100]));
        p.masterVolume = 0.5;
        o = render(eng, p);
        check(std::fabs(o.l[100] - 0.25f) < 1e-6f && std::fabs(o.r[100] - 0.125f) < 1e-6f, "master: locked faders (right follows the left)");
        p.masterVolumeR = 1.0;
        o = render(eng, p);
        check(std::fabs(o.l[100] - 0.25f) < 1e-6f && std::fabs(o.r[100] - 0.25f) < 1e-6f, "master: the right fader on its own", fmt(o.r[100]));
        p.masterPan = -0.5;
        o = render(eng, p);
        check(std::fabs(o.l[100] - 0.25f) < 1e-6f && std::fabs(o.r[100] - 0.125f) < 1e-6f, "master: the pan applies to both faders");
        p.masterPan = 0;
        Envelope e;
        e.kind = "volume";
        EnvPoint a, b;
        a.time = 0; a.value = 1;
        b.time = 100; b.value = 1;
        e.points = {a, b};
        p.masterEnvelopes.push_back(e);
        p.masterVolume = 1.0;
        p.masterVolumeR = 0.5;
        o = render(eng, p);
        check(std::fabs(o.l[100] - 0.5f) < 1e-5f && std::fabs(o.r[100] - 0.125f) < 1e-5f, "master: the volume envelope works with separate faders", fmt(o.l[100]) + " " + fmt(o.r[100]));
        p.masterEnvelopes.clear();
        p.masterMute = true;
        o = render(eng, p);
        check(peak(o.l) == 0 && peak(o.r) == 0, "master: mute silences both");
        Project q;
        p.masterMute = false;
        std::string err;
        const bool ok = q.fromJson(p.toJson(), &err);
        check(ok && q.masterVolumeR == 0.5 && q.masterVolume == 1.0, "project file: the right master fader is kept");
        Project plain;
        check(!plain.toJson().has("masterVolumeR") && !plain.toJson().has("inputBuses"), "project file: a project without them has nothing new in it");
    }
    // ---- input busses
    {
        Project p;
        InputBus ib = makeInputBus(p, "mic");
        AudioEngine eng(pool, fx);
        Out o = render(eng, p);
        check(peak(o.l) == 0, "input bus: without a live input (a normal render) it is left out");
        eng.liveInput = fakeInput;
        p.inputBuses[0].level = 0.5;
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.25f) < 1e-6f && std::fabs(o.r[10] - 0.125f) < 1e-6f, "input bus: its fader", fmt(o.l[10]));
        p.inputBuses[0].pan = 1.0;
        o = render(eng, p);
        check(std::fabs(o.l[10]) < 1e-6f && std::fabs(o.r[10] - 0.125f) < 1e-6f, "input bus: its pan");
        p.inputBuses[0].pan = 0;
        p.inputBuses[0].mute = true;
        o = render(eng, p);
        check(peak(o.l) == 0, "input bus: mute");
        p.inputBuses[0].mute = false;
        p.inputBuses[0].outputOff = true;
        std::vector<float> tapL;
        eng.inputTap = [&](uint64_t id, const float* l, const float*, int n) { if (id == ib.id) tapL.assign(l, l + n); };
        o = render(eng, p);
        check(peak(o.l) == 0 && !tapL.empty() && std::fabs(tapL[5] - 0.5f) < 1e-6f, "input bus: Output Off is not heard but can be recorded (the tap is before the fader)", fmt(tapL.empty() ? 0 : tapL[5]));
        const AudioEngine::StripPeak* pk = nullptr;
        std::vector<AudioEngine::StripPeak> peaks;
        eng.stripPeaks(peaks);
        for (const auto& x : peaks) if (x.id == ib.id) pk = &x;
        check(pk && pk->left > 0.24f, "input bus: it has a meter", pk ? fmt(pk->left) : "none");
        p.inputBuses[0].outputOff = false;
        eng.inputTap = nullptr;
        // routed to a bus with its own fader
        Bus b;
        b.id = p.newId();
        b.level = 0.5;
        p.buses.push_back(b);
        p.inputBuses[0].level = 1.0;
        p.inputBuses[0].output = b.id;
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.25f) < 1e-6f, "input bus: output to a bus goes through that bus's fader", fmt(o.l[10]));
        // sends to a bus
        p.inputBuses[0].output = 0;
        p.inputBuses[0].outputOff = true;
        Send sd;
        sd.bus = b.id;
        sd.level = 0.5;
        p.inputBuses[0].sends.push_back(sd);
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.125f) < 1e-6f, "input bus: a send reaches the bus (even with its output off)", fmt(o.l[10]));
        // solo: a soloed input bus plays, others do not
        InputBus other = makeInputBus(p, "line");
        p.inputBuses[0].sends.clear();
        p.inputBuses[0].outputOff = false;
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.6f) < 1e-6f, "input bus: two input busses add", fmt(o.l[10]));
        p.inputBuses[1].solo = true;
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.1f) < 1e-6f, "input bus: solo plays only the soloed one", fmt(o.l[10]));
        (void)other;
        // mono input
        p.inputBuses[1].solo = false;
        p.inputBuses[0].channels = 1;
        p.inputBuses[1].mute = true;
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.5f) < 1e-6f && std::fabs(o.r[10] - 0.5f) < 1e-6f, "input bus: a mono input is on both sides");
        // 5.1 projects: the bus is placed by its surround panner
        Project s;
        s.settings.audioChannels = 6;
        InputBus sb = makeInputBus(s, "line");
        s.inputBuses[0].surround.route = "center";
        AudioEngine e2(pool, fx);
        e2.liveInput = fakeInput;
        std::vector<std::vector<float>> six(6, std::vector<float>(4800));
        float* ptr[6];
        for (int c = 0; c < 6; ++c) ptr[c] = six[(size_t)c].data();
        e2.renderChannels(s, 0, 4800, ptr, true);
        check(std::fabs(six[ChC][10] - 0.1f) < 1e-5f && peak(six[ChL]) == 0 && peak(six[ChRs]) == 0, "input bus: in a 5.1 project it is placed by its surround panner", fmt(six[ChC][10]));
        s.masterTrim[ChC] = 0.5;
        e2.renderChannels(s, 0, 4800, ptr, true);
        check(std::fabs(six[ChC][10] - 0.05f) < 1e-5f, "Surround Master: a channel's own fader", fmt(six[ChC][10]));
        (void)sb;
    }
    // ---- the project file
    {
        Project p;
        InputBus ib = makeInputBus(p, "usb-mic");
        p.inputBuses[0].name = "Talkback";
        p.inputBuses[0].first = 2;
        p.inputBuses[0].channels = 1;
        p.inputBuses[0].level = 0.7;
        p.inputBuses[0].pan = -0.2;
        p.inputBuses[0].solo = true;
        p.inputBuses[0].outputOff = true;
        p.inputBuses[0].panType = "film";
        Send sd;
        sd.bus = 77;
        sd.level = 0.3;
        sd.preFader = true;
        p.inputBuses[0].sends.push_back(sd);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        t.recordInput.device = "usb-mic";
        t.recordInput.first = 1;
        t.recordInput.channels = 1;
        t.recordInput.monitor = "auto";
        Track t2;
        t2.id = p.newId();
        t2.type = TrackType::Audio;
        t2.recordInput.inputBus = ib.id;
        p.tracks.push_back(t);
        p.tracks.push_back(t2);
        Project q;
        std::string err;
        const bool ok = q.fromJson(p.toJson(), &err);
        const InputBus* r = q.inputBuses.empty() ? nullptr : &q.inputBuses[0];
        check(ok && r && r->name == "Talkback" && r->device == "usb-mic" && r->first == 2 && r->channels == 1 && r->level == 0.7 && r->pan == -0.2 && r->solo &&
                  r->outputOff && r->panType == "film" && r->sends.size() == 1 && r->sends[0].bus == 77 && r->sends[0].preFader,
              "project file: an input bus keeps everything");
        check(ok && q.tracks.size() == 2 && q.tracks[0].recordInput.device == "usb-mic" && q.tracks[0].recordInput.first == 1 && q.tracks[0].recordInput.channels == 1 &&
                  q.tracks[0].recordInput.monitor == "auto" && q.tracks[1].recordInput.inputBus == ib.id,
              "project file: a track keeps its Record Input");
        check(q.newId() > ib.id, "project file: ids of input busses are not handed out again");
        RecordInput a, b;
        b.first = 1;
        check(a.sameSource(a) && !a.sameSource(b) && a.isDefault() && !b.isDefault(), "record input: same source / default");
    }
    // ---- Input Monitoring
    {
        Project p;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        t.panType = "balance0";
        t.recordInput.monitor = "on";
        p.tracks.push_back(t);
        AudioEngine eng(pool, fx);
        int asked = 0;
        eng.monitorInput = [&](const Track& tr, int64_t, float* l, float* r, int n) {
            ++asked;
            if (tr.recordInput.device == "silent") return false;
            for (int k = 0; k < n; ++k) l[k] = r[k] = 0.4f;
            return true;
        };
        Out o = render(eng, p);
        check(asked > 0 && std::fabs(o.l[10] - 0.4f) < 1e-5f && std::fabs(o.r[10] - 0.4f) < 1e-5f, "monitoring: On puts the live input through the track", fmt(o.l[10]));
        p.tracks[0].level = 0.5;
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.2f) < 1e-5f, "monitoring: through the track's fader", fmt(o.l[10]));
        p.tracks[0].recordInput.monitor = "auto";
        o = render(eng, p);
        check(std::fabs(o.l[10] - 0.2f) < 1e-5f, "monitoring: Auto is heard too (the recorder decides when)");
        p.tracks[0].recordInput.monitor = "off";
        o = render(eng, p);
        check(peak(o.l) == 0, "monitoring: Off is silent");
        p.tracks[0].recordInput.monitor = "on";
        p.tracks[0].mute = true;
        o = render(eng, p);
        check(peak(o.l) == 0, "monitoring: a muted track is silent");
        p.tracks[0].mute = false;
        p.tracks[0].recordInput.device = "silent";
        o = render(eng, p);
        check(peak(o.l) == 0, "monitoring: no live sound, no output");
        p.tracks[0].recordInput.device.clear();
        FxChain& chain = p.tracks[0].fx;
        (void)chain;
        AudioEngine plain(pool, fx);
        o = render(plain, p);
        check(peak(o.l) == 0, "monitoring: a render without the hook (a normal render) has none");
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
