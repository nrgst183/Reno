// Tests of multichannel media and plug-ins:
//   - a 5.1 file's channels: names, layout, picking one channel or a pair (MediaReader::audioPick), seeking;
//   - events that play a channel or pair of a file (Event::mediaChannel / mediaChannels), saved and read back;
//   - plug-ins on a 5.1 master that take six channels as one stream (VST3 kSpeakerArr51, VST2 with six in / out),
//     and the fall-back to a pair at a time.
//
//   multichannel_test <output folder> [RenoTest.vst3] [surroundvst2.so]
// (RenoTest.vst3: tests/vst3/testplug.cpp with RENO_TESTPLUG_SURROUND=1; surroundvst2.so: tests/engine/surround_vst2.cpp.)
#include "../../src/engine/engine.h"
#include "../../src/core/dynlib.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

static const double kPi = 3.14159265358979323846;
static const int kRate = 48000;
// the tone on each channel of the test file (L R C LFE Ls Rs)
static const double kFreq[6] = {300, 500, 700, 60, 1100, 1300};

// The strength of one frequency in a block of sound (amplitude of the best-fitting sine).
static double tone(const float* s, int n, double freq, int rate = kRate)
{
    double c = 0, d = 0;
    for (int k = 0; k < n; ++k) {
        const double a = 2.0 * kPi * freq * k / rate;
        c += s[k] * std::cos(a);
        d += s[k] * std::sin(a);
    }
    return 2.0 * std::sqrt(c * c + d * d) / std::max(1, n);
}

// Writes a 6-channel WAVE file: each channel a tone of its own (kFreq) at 0.4, 4 seconds.
static bool writeSixFile(const std::string& path)
{
    EncodeSettings es;
    es.path = path;
    es.video = false;
    es.audio = true;
    es.sampleRate = kRate;
    es.audioChannels = 6;
    es.audioFamily = "pcm";
    es.audioBits = 24;
    std::string err;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
    if (!w) {
        std::printf("cannot write %s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    const int block = 4800;
    std::vector<float> ch[6];
    for (auto& c : ch) c.resize(block);
    for (int b = 0; b < 40; ++b) {
        for (int c = 0; c < 6; ++c)
            for (int k = 0; k < block; ++k) ch[c][(size_t)k] = (float)(0.4 * std::sin(2.0 * kPi * kFreq[c] * (double)(b * block + k) / kRate));
        const float* p[6] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data(), ch[4].data(), ch[5].data()};
        if (!w->writeAudioChannels(p, 6, block)) return false;
    }
    return w->finish(&err);
}

// Which of the six tones is loudest in a block, and how loud the rest are next to it.
static int loudest(const float* s, int n, double* others = nullptr)
{
    int best = -1;
    double bv = 0, rest = 0;
    for (int c = 0; c < 6; ++c) {
        const double v = tone(s, n, kFreq[c]);
        if (v > bv) {
            rest = std::max(rest, bv);
            bv = v;
            best = c;
        } else {
            rest = std::max(rest, v);
        }
    }
    if (others) *others = bv > 0 ? rest / bv : 1.0;
    return best;
}

static void testReader(const std::string& file)
{
    std::string err;
    std::unique_ptr<MediaReader> r = openMedia(file, &err);
    check(r != nullptr, "reader: the 6-channel file opens", err);
    if (!r) return;
    const MediaInfo& mi = r->info();
    std::string names;
    for (const auto& n : mi.channelNames) names += (names.empty() ? "" : " ") + n;
    check(mi.channels == 6 && names == "L R C LFE Ls Rs", "reader: six channels, named as VEGAS names speakers", names + " (" + mi.channelLayout + ")");
    check(mi.channelLayout.compare(0, 3, "5.1") == 0, "reader: the layout is 5.1", mi.channelLayout);

    const int n = 9600;
    std::vector<float> l((size_t)n), rr((size_t)n);
    // each channel alone, on both sides
    bool allOk = true;
    std::string detail;
    for (int c = 0; c < 6; ++c) {
        const bool ok = r->audioPick(kRate, n, kRate, c, 1, l.data(), rr.data());
        double others = 1;
        const int got = loudest(l.data(), n, &others);
        const double amp = tone(l.data(), n, kFreq[c]);
        const bool same = std::equal(l.begin(), l.end(), rr.begin());
        if (!ok || got != c || others > 0.01 || std::fabs(amp - 0.4) > 0.01 || !same) {
            allOk = false;
            detail += strfmt("ch %d: got %d amp %.3f others %.4f same %d; ", c, got, amp, others, same ? 1 : 0);
        }
    }
    check(allOk, "reader: each channel picked alone is that channel's tone, on both sides, at full level", detail);
    // a pair: Ls and Rs as left and right
    r->audioPick(2 * kRate, n, kRate, 4, 2, l.data(), rr.data());
    check(loudest(l.data(), n) == 4 && loudest(rr.data(), n) == 5 && std::fabs(tone(rr.data(), n, kFreq[5]) - 0.4) < 0.01,
          "reader: a pair (Ls, Rs) comes as left and right");
    // back to the stereo mix: the same as a reader that never picked
    std::unique_ptr<MediaReader> fresh = openMedia(file, &err);
    std::vector<float> fl((size_t)n), fr((size_t)n);
    r->audio(kRate, n, kRate, l.data(), rr.data());
    fresh->audio(kRate, n, kRate, fl.data(), fr.data());
    float d = 0;
    for (int k = 0; k < n; ++k) d = std::max(d, std::max(std::fabs(l[(size_t)k] - fl[(size_t)k]), std::fabs(rr[(size_t)k] - fr[(size_t)k])));
    check(d < 1e-6f && tone(l.data(), n, kFreq[0]) > 0.1 && tone(l.data(), n, kFreq[2]) > 0.05, "reader: audio() after a pick is the stereo mix again",
          "diff " + fmt(d));
    // seeking backwards and reading on with a pick: continuous and in the right place
    r->audioPick(3 * kRate, n, kRate, 2, 1, l.data(), rr.data());
    r->audioPick(kRate / 2, n, kRate, 2, 1, l.data(), rr.data());
    double err2 = 0;
    for (int k = 0; k < n; ++k) {
        const double want = 0.4 * std::sin(2.0 * kPi * kFreq[2] * (double)(kRate / 2 + k) / kRate);
        err2 = std::max(err2, std::fabs(l[(size_t)k] - want));
    }
    check(err2 < 1e-3, "reader: a pick after a seek backwards is sample-accurate", "max error " + fmt(err2));
    // another rate
    r->audioPick(0, 4410, 44100, 3, 1, l.data(), rr.data());
    check(tone(l.data(), 4410, kFreq[3], 44100) > 0.35, "reader: a pick at another sample rate (44.1 kHz) is resampled");
    // nonsense
    const bool past = r->audioPick(0, 100, kRate, 6, 1, l.data(), rr.data());
    float any = 0;
    for (int k = 0; k < 100; ++k) any = std::max(any, std::fabs(l[(size_t)k]));
    check(!past && any == 0.f, "reader: a channel the file does not have is silence (and false)");
    r->audioPick(0, 100, kRate, 5, 2, l.data(), rr.data());     // a "pair" from the last channel: that channel alone
    check(tone(l.data(), 100, kFreq[5]) >= 0 && std::equal(l.begin(), l.begin() + 100, rr.begin()), "reader: a pair from the last channel is that channel alone");
}

static Project oneEvent(const std::string& file, int first, int count, int projectChannels)
{
    Project p;
    p.settings.audioChannels = projectChannels;
    MediaItem m;
    m.id = p.newId();
    m.path = file;
    m.name = "six";
    std::string err;
    std::unique_ptr<MediaReader> r = openMedia(file, &err);
    if (r) m.info = r->info();
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.start = 0;
    e.length = 3.5;
    e.mediaChannel = first;
    e.mediaChannels = count;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

static void testEngine(const std::string& file)
{
    const int n = 9600;
    std::vector<float> l((size_t)n), r((size_t)n);
    {
        Project p = oneEvent(file, 2, 1, 2);
        MediaPool pool;
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        eng.render(p, kRate, n, l.data(), r.data(), true);
        double others = 1;
        check(loudest(l.data(), n, &others) == 2 && loudest(r.data(), n) == 2 && others < 0.01 && std::fabs(tone(l.data(), n, kFreq[2]) - 0.4) < 0.01,
              "engine: an event of channel 3 (C) plays the centre channel alone, on both sides", "others " + fmt(others));
    }
    {
        Project p = oneEvent(file, 0, 2, 2);
        MediaPool pool;
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        eng.render(p, kRate, n, l.data(), r.data(), true);
        check(loudest(l.data(), n) == 0 && loudest(r.data(), n) == 1, "engine: an event of channels 1/2 plays L and R as left and right");
        // Left Only on top of the pick
        p.tracks[0].events[0].channels = "right";
        eng.render(p, kRate, n, l.data(), r.data(), true);
        check(loudest(l.data(), n) == 1 && loudest(r.data(), n) == 1, "engine: the event's Channels (Right Only) applies on top of the pick");
    }
    {
        // reversed: the media read backwards in blocks keeps the pick
        Project p = oneEvent(file, 4, 1, 2);
        p.tracks[0].events[0].reverse = true;
        MediaPool pool;
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        eng.render(p, kRate, n, l.data(), r.data(), true);
        check(loudest(l.data(), n) == 4, "engine: a reversed event plays its picked channel (Ls)");
        // time-stretched, keeping its pitch
        Project q = oneEvent(file, 5, 1, 2);
        Event& e = q.tracks[0].events[0];
        e.rate = 1.5;
        e.keepPitch = true;
        e.length = 2.0;
        AudioEngine eng2(pool, fx);
        for (int b = 0; b < 4; ++b) eng2.render(q, (int64_t)b * n, n, l.data(), r.data(), true);
        check(loudest(l.data(), n) == 5 && tone(l.data(), n, kFreq[5]) > 0.2, "engine: a stretched event (rate 1.5, pitch kept) plays its picked channel (Rs) at its pitch",
              "amp " + fmt(tone(l.data(), n, kFreq[5])));
    }
    {
        // in a 5.1 project, panned to the centre speaker only
        Project p = oneEvent(file, 3, 1, 6);
        p.tracks[0].surround.route = "center";
        MediaPool pool;
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        std::vector<float> c[6];
        float* ptr[6];
        for (int k = 0; k < 6; ++k) {
            c[k].assign((size_t)n, 0.f);
            ptr[k] = c[k].data();
        }
        eng.renderChannels(p, kRate, n, ptr, true);
        check(tone(c[ChC].data(), n, kFreq[3]) > 0.35 && tone(c[ChL].data(), n, kFreq[3]) < 1e-3,
              "engine: in a 5.1 project an event of the LFE channel panned to the centre plays there");
    }
    // the project file keeps the pick and the media's channel names
    {
        Project p = oneEvent(file, 4, 2, 2);
        Project q;
        std::string err;
        const bool ok = q.fromJson(p.toJson(), &err);
        const Event& e = q.tracks.empty() || q.tracks[0].events.empty() ? Event() : q.tracks[0].events[0];
        check(ok && e.mediaChannel == 4 && e.mediaChannels == 2 && !q.media.empty() && q.media[0].info.channelNames.size() == 6 &&
                  q.media[0].info.channelNames[3] == "LFE" && q.media[0].info.channelLayout == p.media[0].info.channelLayout,
              "project file: an event's channels and the media's channel names are saved and read back", err);
        Project plain;
        Event pe;
        Track pt;
        pt.type = TrackType::Audio;
        pt.events.push_back(pe);
        plain.tracks.push_back(pt);
        const Json j = plain.toJson();
        check(j.dump().find("mediaChannel") == std::string::npos, "project file: an ordinary event writes nothing new");
        Json bad = p.toJson();
        Project r;
        r.fromJson(bad, &err);
        check(r.tracks[0].events[0].mediaChannel == 4, "project file: read back");
    }
}

// Render As's AC-3 5.1 templates: a 5.1 project rendered to an .ac3 file (448 Kbps, six channels) and read back.
static void testAc3(const std::string& file, const std::string& dir)
{
    Project p = oneEvent(file, 2, 1, 6);
    p.tracks[0].surround.route = "center";
    EncodeSettings es;
    es.path = dir + "/surround.ac3";
    es.video = false;
    es.audio = true;
    es.sampleRate = kRate;
    es.audioFamily = "ac3";
    es.audioChannels = 6;
    es.audioKbps = 448;
    FxRuntime fx;
    std::string err;
    const bool ok = exportProject(p, fx, es, 0.0, 2.0, [](const ExportProgress&) { return true; }, &err);
    check(ok, "ac3: a 5.1 project renders to an AC-3 file (448 Kbps, 5.1)", err);
    std::unique_ptr<MediaReader> r = ok ? openMedia(es.path, &err) : nullptr;
    check(r && r->info().channels == 6 && r->info().audioCodec == "ac3", "ac3: it reads back as six channels of AC-3",
          r ? strfmt("%d channels, %s", r->info().channels, r->info().audioCodec.c_str()) : err);
    if (!r) return;
    const int n = 9600;
    std::vector<float> c[6];
    float* ptr[6];
    for (int k = 0; k < 6; ++k) {
        c[k].assign((size_t)n, 0.f);
        ptr[k] = c[k].data();
    }
    r->audioChannels(kRate / 2, n, kRate, 6, ptr);
    const double cc = tone(c[ChC].data(), n, kFreq[2]), ll = tone(c[ChL].data(), n, kFreq[2]);
    check(cc > 0.3 && ll < 0.02, "ac3: the centre-only sound is on C", "C " + fmt(cc) + ", L " + fmt(ll));
}

// ------------------------------------------------------------------ plug-ins on a 5.1 master
static Project masterProject(const AudioPluginDesc& d, uint64_t& slot)
{
    Project p;
    p.settings.audioChannels = 6;
    p.settings.lfeLowpass = false;
    // a tone on the front left only (the "front" route puts a stereo sound on L and R; this file's L channel alone, panned left)
    MediaItem m;
    m.id = p.newId();
    m.path = "tone";
    m.info.hasAudio = true;
    m.info.duration = 10;
    m.info.sampleRate = kRate;
    m.info.channels = 2;
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    t.surround.route = "front";
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 5;
    t.events.push_back(e);
    p.tracks.push_back(t);
    FxSlot fs;
    fs.id = slot = p.newId();
    fs.kind = "audio";
    fs.audio = d;
    fs.name = d.name;
    p.audioOut.push_back(fs);
    return p;
}

class Sine : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int rate, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) l[k] = r[k] = (float)(0.4 * std::sin(2.0 * kPi * 440.0 * (double)(start + k) / rate));
        return true;
    }
};
static std::unique_ptr<MediaReader> sineOpen(const std::string& path, std::string*)
{
    std::unique_ptr<Sine> s(new Sine());
    s->mi.path = path;
    s->mi.hasAudio = true;
    s->mi.duration = 10;
    s->mi.sampleRate = kRate;
    s->mi.channels = 2;
    return std::unique_ptr<MediaReader>(s.release());
}

// Renders a second of the master; the LFE's 440 Hz (which only a six-channel plug-in puts there) and L's.
static void renderMaster(Project& p, FxRuntime& fx, double& lfe, double& left)
{
    MediaPool pool;
    pool.setOpener(sineOpen);
    AudioEngine eng(pool, fx);
    const int n = 4800;
    std::vector<float> c[6];
    float* ptr[6];
    for (int k = 0; k < 6; ++k) {
        c[k].assign((size_t)n, 0.f);
        ptr[k] = c[k].data();
    }
    for (int b = 0; b < 4; ++b) eng.renderChannels(p, (int64_t)b * n, n, ptr, true);
    lfe = tone(c[ChLFE].data(), n, 440.0);
    left = tone(c[ChL].data(), n, 440.0);
}

static AudioPluginDesc findPlugin(const std::string& file, const std::string& name)
{
    std::string err;
    for (const AudioPluginDesc& d : describeAudioPluginFile(file, &err))
        if (d.name == name) return d;
    std::printf("(no %s in %s: %s)\n", name.c_str(), file.c_str(), err.c_str());
    return AudioPluginDesc();
}

static void testPlugins(const std::string& vst3, const std::string& vst2)
{
    if (!vst3.empty()) {
        setenv("RENO_TESTPLUG_SURROUND", "1", 1);
        const AudioPluginDesc d = findPlugin(vst3, "Reno Test Surround");
        check(!d.name.empty(), "vst3: the test plug-in offers its surround effect");
        if (!d.name.empty()) {
            uint64_t slot = 0;
            Project p = masterProject(d, slot);
            FxRuntime fx;
            fx.prepare(p);
            AudioPlugin* pl = fx.audio(slot);
            check(pl && pl->channels() == 6 && !fx.audioExtra(slot, 1) && !fx.audioExtra(slot, 2),
                  "vst3: on a 5.1 master the plug-in takes kSpeakerArr51 and is one instance (no extra pairs)", fx.error(slot));
            double lfe = 0, left = 0;
            renderMaster(p, fx, lfe, left);
            // front route: 0.4 on L and R; the plug-in's LFE gets 0.25 * (L + R) = 0.2
            check(std::fabs(left - 0.4) < 0.01 && std::fabs(lfe - 0.2) < 0.01, "vst3: the six channels go through it as one stream (its LFE hears L and R)",
                  "L " + fmt(left) + ", LFE " + fmt(lfe));
            // back to stereo: the plug-in is stereo again
            p.settings.audioChannels = 2;
            fx.prepare(p);
            check(pl->channels() == 2, "vst3: in a stereo project the plug-in is put back to stereo");
            MediaPool pool;
            pool.setOpener(sineOpen);
            AudioEngine eng(pool, fx);
            std::vector<float> l(4800), r(4800);
            eng.render(p, 0, 4800, l.data(), r.data(), true);
            check(std::fabs(tone(l.data(), 4800, 440.0) - 0.4) < 0.01, "vst3: ... and processes stereo as before");
            // and 5.1 again
            p.settings.audioChannels = 6;
            fx.prepare(p);
            check(pl->channels() == 6, "vst3: 5.1 again: six channels again");
        }
        // a stereo-only effect (the Gain) on a 5.1 master: a pair at a time
        const AudioPluginDesc g = findPlugin(vst3, "Reno Test Gain");
        if (!g.name.empty()) {
            uint64_t slot = 0;
            Project p = masterProject(g, slot);
            FxRuntime fx;
            fx.prepare(p);
            AudioPlugin* pl = fx.audio(slot);
            check(pl && pl->channels() == 2 && fx.audioExtra(slot, 1) && fx.audioExtra(slot, 2),
                  "vst3: a stereo-only plug-in refuses 5.1 and gets an instance for each pair (as before)");
            double lfe = 0, left = 0;
            renderMaster(p, fx, lfe, left);
            check(std::fabs(left - 0.4) < 0.01 && lfe < 1e-4, "vst3: ... and its LFE stays silent", "L " + fmt(left) + ", LFE " + fmt(lfe));
        }
    }
    if (!vst2.empty()) {
        unsetenv("RENO_TESTVST_CHANNELS");
        const AudioPluginDesc d = findPlugin(vst2, "Reno Surround VST2");
        check(!d.name.empty() && d.numInputs == 6 && d.numOutputs == 6, "vst2: the 6-in / 6-out test plug-in is recognised");
        if (!d.name.empty()) {
            uint64_t slot = 0;
            Project p = masterProject(d, slot);
            FxRuntime fx;
            fx.prepare(p);
            AudioPlugin* pl = fx.audio(slot);
            DynLib lib;
            std::string err;
            lib.open(vst2, &err);
            typedef int (*Count)();
            Count arr = (Count)lib.symbol("surroundvst2_arrangements");
            Count blocks = (Count)lib.symbol("surroundvst2_six_blocks");
            check(pl && pl->channels() == 6 && !fx.audioExtra(slot, 1) && arr && arr() >= 1,
                  "vst2: a 6-in / 6-out plug-in is given VST's 5.1 speaker arrangement and is one instance", arr ? strfmt("arrangements %d", arr()) : "");
            double lfe = 0, left = 0;
            renderMaster(p, fx, lfe, left);
            check(std::fabs(left - 0.4) < 0.01 && std::fabs(lfe - 0.2) < 0.01 && blocks && blocks() > 0,
                  "vst2: the six channels go through it as one stream", "L " + fmt(left) + ", LFE " + fmt(lfe));
            lib.leak();
        }
        setenv("RENO_TESTVST_CHANNELS", "2", 1);
        const AudioPluginDesc s = findPlugin(vst2, "Reno Stereo VST2");
        if (!s.name.empty()) {
            uint64_t slot = 0;
            Project p = masterProject(s, slot);
            FxRuntime fx;
            fx.prepare(p);
            AudioPlugin* pl = fx.audio(slot);
            check(pl && pl->channels() == 2 && fx.audioExtra(slot, 1) && fx.audioExtra(slot, 2), "vst2: a stereo plug-in falls back to a pair at a time");
            double lfe = 0, left = 0;
            renderMaster(p, fx, lfe, left);
            check(std::fabs(left - 0.4) < 0.01 && lfe < 1e-4, "vst2: ... the LFE pair passes through its own instance untouched");
        }
        unsetenv("RENO_TESTVST_CHANNELS");
    }
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string file = dir + "/six_channels.wav";
    check(writeSixFile(file), "a 6-channel test file is written");
    testReader(file);
    testEngine(file);
    testAc3(file, dir);
    testPlugins(argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "");
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
