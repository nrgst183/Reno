// Tests of 5.1 surround projects: the panning law, routes, LFE and its low-pass filter, fold-down to stereo, buses and
// master in 5.1, keyframes, the project file, and rendering 5.1 files (WAVE, FLAC, AAC, AC-3) read back with six
// channels. A stereo project must still render exactly as it did without 5.1 (hash of its samples).
//
//   surround_test [output folder] [a VST2 effect]
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const double kPi = 3.14159265358979323846;

// (the stereo project and its hash: shared with the program that took the hash from the engine before 5.1)
#include "surround_golden.inc"
static const uint64_t kStereoGold = 10885264225381470114ull;

// Made-up media: "tone:<hz>" 10 s of a tone at half volume, the same on both sides; "lr:<hz>:<hz>" a different tone on each side.
class Tone : public MediaReader {
public:
    MediaInfo mi;
    double fl = 440, fr = 440;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const bool in = n >= 0 && n < (int64_t)(10.0 * sampleRate);
            left[k] = in ? (float)(0.5 * std::sin(2.0 * kPi * fl * (double)n / sampleRate)) : 0.f;
            right[k] = in ? (float)(0.5 * std::sin(2.0 * kPi * fr * (double)n / sampleRate)) : 0.f;
        }
        return true;
    }
};
static std::unique_ptr<MediaReader> toneOpen(const std::string& path, std::string* error)
{
    std::unique_ptr<Tone> r(new Tone());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 10;
    r->mi.sampleRate = 48000;
    r->mi.channels = 2;
    if (path.compare(0, 5, "tone:") == 0) r->fl = r->fr = std::atof(path.c_str() + 5);
    else if (path.compare(0, 3, "lr:") == 0) std::sscanf(path.c_str() + 3, "%lf:%lf", &r->fl, &r->fr);
    else { if (error) *error = "unknown"; return nullptr; }
    return std::unique_ptr<MediaReader>(r.release());
}

static uint64_t addMedia(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = path;
    std::string e;
    m.info = toneOpen(path, &e)->info();
    p.media.push_back(m);
    return m.id;
}
static Track& addTrack(Project& p, uint64_t media, double len = 2.0)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = 0;
    e.length = len;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p.tracks.back();
}
static Project surroundProject()
{
    Project p;
    p.settings.audioChannels = 6;
    return p;
}

// Renders the project's six channels for [s, s + n).
struct Six {
    std::vector<float> c[6];
    float* ptr[6];
    explicit Six(int n) { for (int k = 0; k < 6; ++k) { c[k].assign((size_t)n, 0.f); ptr[k] = c[k].data(); } }
};
static Six renderSix(const Project& p, int64_t s, int n, std::vector<AudioEngine::StripPeak>* peaks = nullptr)
{
    MediaPool pool;
    pool.setOpener(toneOpen);
    FxRuntime fx;
    AudioEngine eng(pool, fx);
    Six out(n);
    eng.renderChannels(p, s, n, out.ptr, true);
    if (peaks) eng.stripPeaks(*peaks);
    return out;
}
static double rms(const float* s, int n)
{
    double e = 0;
    for (int k = 0; k < n; ++k) e += (double)s[k] * s[k];
    return n ? std::sqrt(e / n) : 0;
}
static double rms(const std::vector<float>& s) { return rms(s.data(), (int)s.size()); }
// The strength of one frequency in a block of sound (amplitude of the best-fitting sine).
static double tone(const float* s, int n, double freq, int rate)
{
    double re = 0, im = 0;
    for (int k = 0; k < n; ++k) {
        re += s[k] * std::cos(2.0 * kPi * freq * (double)k / rate);
        im += s[k] * std::sin(2.0 * kPi * freq * (double)k / rate);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double)n;
}
static std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
static const char* kNames[6] = {"L", "R", "C", "LFE", "Ls", "Rs"};

// ------------------------------------------------------------------------------------------ the law
static void testLaw()
{
    SurroundPan sp;
    // Constant Power: the same loudness everywhere in the room
    double worst = 0;
    for (const char* type : {"power", "film"})
        for (int i = -10; i <= 10; ++i)
            for (int j = -10; j <= 10; ++j) {
                float g[6];
                surroundGains(sp, type, i / 10.0, j / 10.0, 1.0, g);
                double pw = 0;
                for (int k = 0; k < 6; ++k)
                    if (k != ChLFE) pw += (double)g[k] * g[k];
                worst = std::max(worst, std::fabs(pw - 1.0));
            }
    check(worst < 1e-5, "law: Constant Power and Film keep the power over the whole room", "worst error " + fmt(worst));
    auto gains = [&](const char* type, double x, double y, double c, const SurroundPan& s) {
        std::vector<float> v(6);
        surroundGains(s, type, x, y, c, v.data());
        return v;
    };
    auto only = [&](const std::vector<float>& g, int ch) {
        for (int k = 0; k < 6; ++k)
            if (std::fabs(g[k] - (k == ch ? 1.f : 0.f)) > 1e-5f) return false;
        return true;
    };
    for (const char* type : {"", "balance3", "add", "power"}) {
        const std::string t = std::string("law (") + (*type ? type : "balance0") + "): ";
        check(only(gains(type, -1, 1, 1, sp), ChL), t + "front left corner is L alone");
        check(only(gains(type, 1, 1, 1, sp), ChR), t + "front right corner is R alone");
        check(only(gains(type, -1, -1, 1, sp), ChLs), t + "rear left corner is Ls alone");
        check(only(gains(type, 1, -1, 1, sp), ChRs), t + "rear right corner is Rs alone");
        check(only(gains(type, 0, 1, 1, sp), ChC), t + "front centre with Center 0.0 dB is C alone");
    }
    // Center is a gain on the centre channel: 0.0 dB leaves it, a cut or a boost scales it (VEGAS: "boost or attenuate")
    std::vector<float> g = gains("power", 0, 1, 0.5, sp);
    std::vector<float> boost = gains("power", 0, 1, 2.0, sp);
    check(std::fabs(g[ChC] - 0.5f) < 1e-6f && std::fabs(boost[ChC] - 2.0f) < 1e-6f && g[ChL] < 1e-6f, "law: Center is a gain on the centre channel", fmt(g[ChC]) + " " + fmt(boost[ChC]));
    g = gains("power", -1, 1, 0.5, sp);
    check(only(g, ChL), "law: Center leaves the other speakers alone");
    // a muted centre speaker: a phantom centre between left and right, at -3 dB each
    SurroundPan mutedC;
    mutedC.speakerMutes = 1u << ChC;
    g = gains("power", 0, 1, 1, mutedC);
    check(std::fabs(g[ChL] - 0.70711f) < 1e-4f && std::fabs(g[ChR] - 0.70711f) < 1e-4f && g[ChC] == 0.f, "law: a muted centre speaker gives a phantom centre at -3 dB",
          fmt(g[ChL]) + " " + fmt(g[ChR]));
    g = gains("power", 0, 0, 1, mutedC);
    bool quarter = true;
    for (int k : {ChL, ChR, ChLs, ChRs}) quarter = quarter && std::fabs(g[k] - 0.5f) < 1e-5f;
    check(quarter, "law: the middle of the room is all four corners at -6 dB (centre muted)");
    // Add Channels is linear and adds to 1 between two speakers; Balance falls from the centre level to nothing and rises to 0 dB
    g = gains("add", -0.5, -1, 1, sp);
    check(std::fabs(g[ChLs] - 0.75f) < 1e-5f && std::fabs(g[ChRs] - 0.25f) < 1e-5f, "law: Add Channels pans linearly", fmt(g[ChLs]) + " " + fmt(g[ChRs]));
    g = gains("balance3", 0, -1, 1, sp);
    check(std::fabs(g[ChLs] - 0.70795f) < 1e-4f && std::fabs(g[ChRs] - 0.70795f) < 1e-4f, "law: Balance (-3 dB center) is 3 dB down in the middle of the rear");
    g = gains("", -0.5, -1, 1, sp);
    check(std::fabs(g[ChLs] - 1.0f) < 1e-5f && std::fabs(g[ChRs] - 0.5f) < 1e-5f, "law: Balance (0 dB center) keeps the side panned to at 0 dB and fades the other",
          fmt(g[ChLs]) + " " + fmt(g[ChRs]));
    g = gains("power", -0.5, -1, 1, sp);
    check(std::fabs(g[ChLs] - std::cos(kPi / 8)) < 1e-5 && std::fabs(g[ChRs] - std::sin(kPi / 8)) < 1e-5, "law: Constant Power pans on a quarter circle");
    // Film: pairs of adjacent speakers at the cinema's angles. Panned to the front left corner some of it is in Ls (the
    // panner draws L at 45 degrees, it belongs at 30), fully to the centre speaker nothing is in the others, in the middle of the room all five play.
    g = gains("film", -1, 1, 1, sp);
    check(g[ChL] > 0.9f && g[ChLs] > 0.1f && g[ChLs] < 0.5f && g[ChR] < 1e-6f && g[ChC] < 1e-6f, "law: Film puts a little of the front left corner in the rear left",
          "L " + fmt(g[ChL]) + " Ls " + fmt(g[ChLs]));
    check(only(gains("film", 0, 1, 1, sp), ChC), "law: Film panned to the centre speaker plays from it alone");
    g = gains("film", 0, 0, 1, sp);
    bool even = true;
    for (int k : {ChL, ChR, ChC, ChLs, ChRs}) even = even && std::fabs(g[k] - std::sqrt(0.2f)) < 1e-5f;
    check(even, "law: Film in the middle of the room plays from all five speakers");
    g = gains("film", 0, -1, 1, sp);
    check(std::fabs(g[ChLs] - g[ChRs]) < 1e-5f && g[ChL] < 1e-6f && g[ChC] < 1e-6f, "law: Film straight behind is Ls and Rs alike");
    // speaker mutes and the LFE
    SurroundPan m;
    m.speakerMutes = 1u << ChC;
    m.lfe = true;
    m.lfeLevel = 0.5;
    g = gains("power", 0, 1, 1, m);
    check(g[ChC] == 0.f && std::fabs(g[ChLFE] - 0.5f) < 1e-6f, "law: a muted speaker gets nothing; Enable LFE sends at its level");
    // keyframes: held before the first, the curves between, Hold keys hold, the last after
    SurroundPan k;
    SurroundKey k1, k2, k3, k4;
    k1.time = 1.0; k1.x = -1; k1.y = 1; k1.center = 1;
    k2.time = 3.0; k2.x = 1; k2.y = -1; k2.center = 0.5;
    k3.time = 4.0; k3.curve = KeyHold;
    k4.time = 5.0; k4.x = 1; k4.y = 1;
    k.keys = {k1, k2, k3, k4};
    double x, y, c;
    surroundPointAt(k, 0.5, x, y, c);
    bool ok = x == -1 && y == 1 && c == 1;
    surroundPointAt(k, 2.0, x, y, c);
    ok = ok && std::fabs(x) < 1e-9 && std::fabs(y) < 1e-9 && std::fabs(c - 0.75) < 1e-9;
    surroundPointAt(k, 4.5, x, y, c);
    ok = ok && x == 0 && y == 0;
    surroundPointAt(k, 9, x, y, c);
    ok = ok && x == 1 && y == 1;
    check(ok, "law: keyframes - held before the first, straight lines between, hold keys hold, the last after");
    // the keyframe types shape the movement in time (the same shapes as every VEGAS keyframe)
    SurroundPan shaped;
    SurroundKey s1, s2;
    s2.time = 1.0;
    s2.x = 1;
    shaped.keys = {s1, s2};
    double halfLin, halfFast, halfSlow, halfSmooth, halfSharp, dummy;
    for (int curve : {(int)KeyLinear, (int)KeyFast, (int)KeySlow, (int)KeySmooth, (int)KeySharp}) {
        shaped.keys[0].curve = curve;
        surroundPointAt(shaped, 0.25, x, dummy, dummy);
        (curve == KeyLinear ? halfLin : curve == KeyFast ? halfFast : curve == KeySlow ? halfSlow : curve == KeySmooth ? halfSmooth : halfSharp) = x;
    }
    check(std::fabs(halfLin - 0.25) < 1e-9 && halfFast > halfLin && halfSlow < halfLin && halfSmooth < halfLin && halfSharp > halfLin,
          "law: keyframe types Linear, Fast, Slow, Smooth and Sharp shape the move in time",
          fmt(halfLin) + " " + fmt(halfFast) + " " + fmt(halfSlow) + " " + fmt(halfSmooth) + " " + fmt(halfSharp));
    // Smoothness: 0 is a straight line between keys; more bends the path through the keys before and after
    SurroundPan path;
    SurroundKey p1, p2, p3;
    p1.time = 0; p1.x = -1; p1.y = 1;
    p2.time = 1; p2.x = 0; p2.y = 1;
    p3.time = 2; p3.x = 0; p3.y = -1;
    path.keys = {p1, p2, p3};
    double sx, sy, bx, by;
    surroundPointAt(path, 1.5, sx, sy, dummy);
    path.smoothness = 1.0;
    surroundPointAt(path, 1.5, bx, by, dummy);
    check(std::fabs(sx) < 1e-9 && std::fabs(sy - 0.0) < 1e-9 && (std::fabs(bx - sx) > 0.01 || std::fabs(by - sy) > 0.01), "law: Smoothness 0 is a straight line, more bends the path",
          "straight " + fmt(sx) + "," + fmt(sy) + " smooth " + fmt(bx) + "," + fmt(by));
    path.smoothness = 0;
    path.keys[1].smoothness = 1.0;
    surroundPointAt(path, 1.5, bx, by, dummy);
    check(std::fabs(bx - sx) > 0.01 || std::fabs(by - sy) > 0.01, "law: a keyframe's own Smoothness wins over the panner's");
    path.smoothness = 1.0;
    surroundPointAt(path, 1.0, bx, by, dummy);
    check(std::fabs(bx) < 1e-9 && std::fabs(by - 1.0) < 1e-9, "law: a smooth path still passes through its keys");
}

// ------------------------------------------------------------------------------------------ the mixer in 5.1
static void testMixer()
{
    const int n = 48000;
    // a mono tone at the front left: only L
    {
        Project p = surroundProject();
        Track& t = addTrack(p, addMedia(p, "tone:440"));
        t.surround.x = -1;
        t.surround.y = 1;
        Six s = renderSix(p, 0, n);
        bool ok = std::fabs(rms(s.c[ChL]) - 0.5 / std::sqrt(2.0)) < 1e-3;
        for (int k = 1; k < 6; ++k) ok = ok && rms(s.c[k]) < 1e-6;
        check(ok, "mixer: a track panned to the front left plays from L alone", "L rms " + fmt(rms(s.c[ChL])));
    }
    // the middle of the room, phantom centre: four speakers at -6 dB, the sum's power the same as the source's
    {
        Project p = surroundProject();
        Track& t = addTrack(p, addMedia(p, "tone:440"));
        t.panType = "power";
        t.surround.speakerMutes = 1u << ChC;
        Six s = renderSix(p, 0, n);
        double pw = 0;
        for (int k = 0; k < 6; ++k) pw += rms(s.c[k]) * rms(s.c[k]);
        check(std::fabs(rms(s.c[ChLs]) - 0.25 / std::sqrt(2.0)) < 1e-3 && std::fabs(pw - 0.125) < 1e-3, "mixer: centred track is spread at constant power",
              "power " + fmt(pw));
    }
    // a stereo source keeps its sides apart in the middle and folds to one side at the edge
    {
        Project p = surroundProject();
        Track& t = addTrack(p, addMedia(p, "lr:300:700"));
        t.panType = "power";
        t.surround.y = 1;
        t.surround.speakerMutes = 1u << ChC;
        Six s = renderSix(p, 0, n);
        const double l300 = tone(s.ptr[ChL], n, 300, 48000), l700 = tone(s.ptr[ChL], n, 700, 48000);
        const double r700 = tone(s.ptr[ChR], n, 700, 48000), r300 = tone(s.ptr[ChR], n, 300, 48000);
        check(l300 > 0.3 && l700 < 0.01 && r700 > 0.3 && r300 < 0.01, "mixer: a stereo track keeps left on the left, right on the right",
              "L: " + fmt(l300) + "/" + fmt(l700) + "  R: " + fmt(r700) + "/" + fmt(r300));
        p.tracks[0].surround.x = -1;
        s = renderSix(p, 0, n);
        check(tone(s.ptr[ChL], n, 300, 48000) > 0.2 && tone(s.ptr[ChL], n, 700, 48000) > 0.2 && rms(s.c[ChR]) < 1e-6,
              "mixer: panned hard left, both sides of a stereo track play from the left");
    }
    // the routes
    {
        Project p = surroundProject();
        Track& a = addTrack(p, addMedia(p, "lr:300:500"));
        a.surround.route = "front";
        Track& b = addTrack(p, addMedia(p, "tone:700"));
        b.surround.route = "center";
        Track& c = addTrack(p, addMedia(p, "lr:900:1100"));
        c.surround.route = "rear";
        Track& d = addTrack(p, addMedia(p, "tone:50"));
        d.surround.route = "lfe";
        Six s = renderSix(p, 0, n);
        const double want[6][2] = {{300, 0.5}, {500, 0.5}, {700, 0.5}, {50, 0.5}, {900, 0.5}, {1100, 0.5}};
        bool ok = true;
        std::string det;
        for (int k = 0; k < 6; ++k) {
            const double a1 = tone(s.ptr[k] + 4800, n - 4800, want[k][0], 48000);
            det += std::string(kNames[k]) + " " + fmt(a1) + " ";
            ok = ok && std::fabs(a1 - want[k][1]) < (k == ChLFE ? 0.02 : 0.002);
        }
        check(ok, "mixer: Front Only, Center Only, Rear Only and LFE Only put the sound on those speakers at full level", det);
    }
    // the LFE low-pass: 50 Hz passes, 1 kHz does not; switched off, both pass
    {
        Project p = surroundProject();
        Track& t = addTrack(p, addMedia(p, "tone:1000"));
        t.surround.route = "lfe";
        Six s = renderSix(p, 0, n);
        const double hi = rms(s.ptr[ChLFE] + 4800, n - 4800);
        p.settings.lfeLowpass = false;
        Six s2 = renderSix(p, 0, n);
        const double open = rms(s2.ptr[ChLFE] + 4800, n - 4800);
        p.settings.lfeLowpass = true;
        p.settings.lfeQuality = "fast";
        Six s3 = renderSix(p, 0, n);
        const double fast = rms(s3.ptr[ChLFE] + 4800, n - 4800);
        check(hi < 1e-4 && std::fabs(open - 0.5 / std::sqrt(2.0)) < 1e-3 && fast > hi && fast < 0.01, "mixer: the LFE low-pass filter (120 Hz) stops 1 kHz",
              "best " + fmt(hi) + ", fast " + fmt(fast) + ", off " + fmt(open));
        // and it runs on smoothly from one block to the next
        MediaPool pool;
        pool.setOpener(toneOpen);
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        Project q = surroundProject();
        Track& u = addTrack(q, addMedia(q, "tone:60"));
        u.surround.route = "lfe";
        Six whole(4800), parts(4800);
        { AudioEngine e2(pool, fx); e2.renderChannels(q, 0, 4800, whole.ptr, true); }
        for (int at = 0; at < 4800; at += 512) {
            float* pp[6];
            for (int c = 0; c < 6; ++c) pp[c] = parts.ptr[c] + at;
            eng.renderChannels(q, at, std::min(512, 4800 - at), pp, true);
        }
        float d = 0;
        for (int k = 0; k < 4800; ++k) d = std::max(d, std::fabs(whole.c[ChLFE][(size_t)k] - parts.c[ChLFE][(size_t)k]));
        check(d < 1e-5f, "mixer: the LFE filter carries on across blocks", "largest difference " + fmt(d));
    }
    // Enable LFE on a panned track: both sides mixed, at the LFE level, besides the panning
    {
        Project p = surroundProject();
        Track& t = addTrack(p, addMedia(p, "tone:60"));
        t.surround.lfe = true;
        t.surround.lfeLevel = 0.5;
        p.settings.lfeLowpass = false;
        Six s = renderSix(p, 0, n);
        check(std::fabs(rms(s.c[ChLFE]) - 0.25 / std::sqrt(2.0)) < 1e-3 && rms(s.c[ChLs]) > 0.05, "mixer: Enable LFE adds the LFE share at its level",
              fmt(rms(s.c[ChLFE])));
    }
    // a bus: tracks mix into it in stereo, the bus is placed by its own surround panner; volume, mute and master level apply
    {
        Project p = surroundProject();
        Bus b;
        b.id = p.newId();
        b.level = 0.5;
        b.surround.x = 1;
        b.surround.y = -1;
        p.buses.push_back(b);
        Track& t = addTrack(p, addMedia(p, "tone:440"));
        t.output = b.id;
        t.surround.x = -1;          // (its own surround panner is not used: it goes to a bus)
        t.surround.y = 1;
        p.masterVolume = 0.5;
        std::vector<AudioEngine::StripPeak> peaks;
        Six s = renderSix(p, 0, n, &peaks);
        bool ok = std::fabs(rms(s.c[ChRs]) - 0.125 / std::sqrt(2.0)) < 1e-3;
        for (int k = 0; k < 6; ++k)
            if (k != ChRs) ok = ok && rms(s.c[k]) < 1e-6;
        check(ok, "mixer: a bus is placed by its surround panner, at its level and the master's", "Rs " + fmt(rms(s.c[ChRs])));
        const AudioEngine::StripPeak* master = nullptr;
        for (const auto& x : peaks)
            if (x.id == 0) master = &x;
        check(master && master->channels == 6 && std::fabs(master->ch[ChRs] - 0.125f) < 2e-3f && master->ch[ChL] == 0.f && master->right > 0.12f,
              "mixer: the master's meter has six channels", master ? "Rs peak " + fmt(master->ch[ChRs]) : "no master peak");
        p.masterMute = true;
        s = renderSix(p, 0, n);
        check(rms(s.c[ChRs]) == 0, "mixer: master mute silences all six");
        p.masterMute = false;
        Envelope e;
        e.kind = "volume";
        e.points = {{0, 0, CurveLinear}};
        p.buses[0].envelopes.push_back(e);
        s = renderSix(p, 0, n);
        check(rms(s.c[ChRs]) == 0, "mixer: a bus's volume envelope works in 5.1");
    }
    // a track moving by keyframes: smooth (no steps between blocks)
    {
        Project p = surroundProject();
        Track& t = addTrack(p, addMedia(p, "tone:200"));
        {
            SurroundKey a, b;
            a.x = -1; a.y = 1;
            b.time = 1.0; b.x = 1; b.y = 1;
            t.surround.keys = {a, b};
        }
        Six s = renderSix(p, 0, n);
        const double first = rms(s.ptr[ChL], 2400), last = rms(s.ptr[ChL] + n - 2400, 2400);
        const double lastR = rms(s.ptr[ChR] + n - 2400, 2400);
        check(first > 0.3 && last < 0.02 && lastR > 0.3, "mixer: surround keyframes move the sound from L to R", fmt(first) + " -> " + fmt(last));
    }
    // the fold-down: render() of a 5.1 project is the standard stereo fold-down of its six channels
    {
        Project p = surroundProject();
        Track& a = addTrack(p, addMedia(p, "tone:700"));
        a.surround.route = "center";
        Track& b = addTrack(p, addMedia(p, "lr:300:500"));
        b.surround.route = "rear";
        Track& c = addTrack(p, addMedia(p, "tone:50"));
        c.surround.route = "lfe";
        MediaPool pool;
        pool.setOpener(toneOpen);
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        std::vector<float> l(n), r(n);
        eng.render(p, 0, n, l.data(), r.data(), true);
        const double c700 = tone(l.data(), n, 700, 48000), ls = tone(l.data(), n, 300, 48000), rs = tone(r.data(), n, 500, 48000);
        const double lfe = tone(l.data(), n, 50, 48000), cross = tone(l.data(), n, 500, 48000);
        check(std::fabs(c700 - 0.3536) < 2e-3 && std::fabs(ls - 0.3536) < 2e-3 && std::fabs(rs - 0.3536) < 2e-3 && lfe < 1e-3 && cross < 1e-3,
              "mixer: the stereo fold-down: C and the surrounds at -3 dB, the LFE left out", "C " + fmt(c700) + " Ls " + fmt(ls) + " Rs " + fmt(rs));
    }
}

// ------------------------------------------------------------------------------------------ the project file
static void testJson()
{
    Project p = surroundProject();
    p.settings.lfeCutoff = 90;
    p.settings.lfeQuality = "good";
    p.settings.lfeLowpass = false;
    Track& t = addTrack(p, addMedia(p, "tone:440"));
    t.surround.x = 0.25;
    t.surround.y = -0.5;
    t.surround.center = 0.3;
    t.surround.smoothness = 0.6;
    t.surround.lfe = true;
    t.surround.lfeLevel = 0.7;
    t.surround.route = "rear";
    t.surround.motion = "x";
    t.surround.speakerMutes = 5;
    {
        SurroundKey k;
        k.time = 1.5; k.x = 0.1; k.y = 0.2; k.center = 0.3; k.curve = KeySmooth; k.smoothness = 0.4;
        t.surround.keys = {k};
    }
    t.panType = "film";
    Bus b;
    b.id = p.newId();
    b.surround.y = 1;
    b.panType = "add";
    p.buses.push_back(b);
    Project q;
    std::string err;
    const bool ok = q.fromJson(p.toJson(), &err);
    const SurroundPan& s = q.tracks.empty() ? SurroundPan() : q.tracks[0].surround;
    check(ok && q.settings.audioChannels == 6 && q.settings.lfeCutoff == 90 && q.settings.lfeQuality == "good" && !q.settings.lfeLowpass &&
              s.x == 0.25 && s.y == -0.5 && s.center == 0.3 && s.smoothness == 0.6 && s.lfe && s.lfeLevel == 0.7 && s.route == "rear" &&
              s.motion == "x" && s.speakerMutes == 5 && s.keys.size() == 1 && s.keys[0].curve == KeySmooth && s.keys[0].smoothness == 0.4 && q.tracks[0].panType == "film" && q.buses.size() == 1 && q.buses[0].surround.y == 1 && q.buses[0].panType == "add",
          "project file: the master bus mode, LFE filter and every surround pan setting are kept");
    Project st;
    Json j = st.toJson();
    check(!j.get("settings").has("audioChannels") && !j.get("settings").has("lfeLowpass"), "project file: a stereo project's file has nothing new in it");
    Json bad = p.toJson();
    bad["settings"]["audioChannels"] = 5;
    bad["tracks"] = Json::array();
    Project r;
    r.fromJson(bad, &err);
    check(r.settings.audioChannels == 2, "project file: a channel count other than 2 or 6 reads as stereo");
}

// ------------------------------------------------------------------------------------------ rendering 5.1 files
static Project routesProject()
{
    Project p = surroundProject();
    p.settings.lfeLowpass = false;
    const char* media[4] = {"lr:300:500", "tone:700", "tone:90", "lr:900:1100"};
    const char* routes[4] = {"front", "center", "lfe", "rear"};
    for (int i = 0; i < 4; ++i) {
        Track& t = addTrack(p, addMedia(p, media[i]), 1.0);
        t.surround.route = routes[i];
    }
    return p;
}

static void testRender(const std::string& dir)
{
    struct Case { const char* name; const char* file; const char* family; double tol; };
    const Case cases[] = {
        {"PCM in WAVE", "surround.wav", "pcm", 0.002},
        {"FLAC in Matroska", "surround_flac.mkv", "flac", 0.002},
        {"AAC in MP4", "surround_aac.mp4", "aac", 0.05},
        {"AC-3 in Matroska", "surround_ac3.mkv", "ac3", 0.05},
    };
    const double freq[6] = {300, 500, 700, 90, 900, 1100};
    for (const Case& c : cases) {
        Project p = routesProject();
        FxRuntime fx;
        EncodeSettings es;
        es.path = dir + "/" + c.file;
        es.video = false;
        es.audio = true;
        es.audioFamily = c.family;
        es.audioChannels = 6;
        es.audioKbps = 448;
        std::string err, desc;
        const bool ok = exportProject(p, fx, es, 0.0, 1.0, nullptr, &err, toneOpen, &desc);
        check(ok, std::string("render: ") + c.name + " written", ok ? desc : err);
        if (!ok) continue;
        std::unique_ptr<MediaReader> r = openMedia(es.path, &err);
        check(r && r->info().channels == 6, std::string("render: ") + c.name + " has six channels",
              r ? std::to_string(r->info().channels) + " channels, " + r->info().audioCodec : err);
        if (!r) continue;
        const int n = 48000;
        Six s(n);
        const bool got = r->audioChannels(0, n, 48000, 6, s.ptr);
        bool right = got;
        std::string det;
        for (int k = 0; k < 6; ++k) {
            const int a = 4800, m = 38400;
            const double own = tone(s.ptr[k] + a, m, freq[k], 48000);
            double other = 0;
            for (int q = 0; q < 6; ++q)
                if (q != k) other = std::max(other, tone(s.ptr[k] + a, m, freq[q], 48000));
            det += std::string(kNames[k]) + " " + fmt(own) + "/" + fmt(other) + " ";
            right = right && std::fabs(own - 0.5) < c.tol + (k == ChLFE ? 0.05 : 0.0) && other < 0.02;
        }
        check(right, std::string("render: ") + c.name + " read back: each channel holds its own sound", det);
        // reading on from where it stopped gives the same as reading that part afresh
        if (c.family == std::string("pcm")) {
            Six a(1000), b(1000);
            r->audioChannels(20000, 1000, 48000, 6, a.ptr);
            r->audioChannels(21000, 1000, 48000, 6, a.ptr);     // (on in order)
            std::unique_ptr<MediaReader> r2 = openMedia(es.path, &err);
            r2->audioChannels(21000, 1000, 48000, 6, b.ptr);
            float d = 0;
            for (int ch = 0; ch < 6; ++ch)
                for (int k = 0; k < 1000; ++k) d = std::max(d, std::fabs(a.c[ch][(size_t)k] - b.c[ch][(size_t)k]));
            check(d == 0.f, "render: the six-channel reader reads on and seeks alike");
            // and the ordinary stereo read of the 5.1 file is still its fold-down
            std::vector<float> l(n), rr(n);
            r2->audio(0, n, 48000, l.data(), rr.data());
            check(tone(l.data() + 4800, 38400, 300, 48000) > 0.2 && tone(rr.data() + 4800, 38400, 1100, 48000) > 0.1, "render: a 5.1 file still reads as stereo");
        }
    }
    // a stereo project rendered to 5.1: its left and right, silence in the rest
    {
        Project p;
        addTrack(p, addMedia(p, "lr:300:500"), 1.0);
        FxRuntime fx;
        EncodeSettings es;
        es.path = dir + "/stereo_as_51.wav";
        es.video = false;
        es.audioFamily = "pcm";
        es.audioChannels = 6;
        std::string err;
        const bool ok = exportProject(p, fx, es, 0.0, 1.0, nullptr, &err, toneOpen);
        std::unique_ptr<MediaReader> r = ok ? openMedia(es.path, &err) : nullptr;
        bool good = r && r->info().channels == 6;
        if (good) {
            Six s(24000);
            r->audioChannels(0, 24000, 48000, 6, s.ptr);
            good = tone(s.ptr[0], 24000, 300, 48000) > 0.45 && tone(s.ptr[1], 24000, 500, 48000) > 0.45 && rms(s.c[2]) < 1e-4 && rms(s.c[5]) < 1e-4;
        }
        check(good, "render: a stereo project rendered as 5.1 has its sound in L and R only", err);
    }
    // a 5.1 project rendered to a stereo file: the fold-down
    {
        Project p = routesProject();
        p.masterVolume = 0.5;           // (the fold-down of all six at full level would clip a 16-bit file)
        FxRuntime fx;
        EncodeSettings es;
        es.path = dir + "/surround_as_stereo.wav";
        es.video = false;
        es.audioFamily = "pcm";
        std::string err;
        const bool ok = exportProject(p, fx, es, 0.0, 1.0, nullptr, &err, toneOpen);
        std::unique_ptr<MediaReader> r = ok ? openMedia(es.path, &err) : nullptr;
        bool good = r && r->info().channels == 2;
        std::string det = r ? std::to_string(r->info().channels) + " channels" : err;
        if (good) {
            std::vector<float> l(24000), rr(24000);
            r->audio(0, 24000, 48000, l.data(), rr.data());
            const double c = tone(l.data(), 24000, 700, 48000), rs = tone(rr.data(), 24000, 1100, 48000), lfe = tone(l.data(), 24000, 90, 48000);
            det = "C " + fmt(c) + " Rs " + fmt(rs) + " LFE " + fmt(lfe);
            good = std::fabs(c - 0.1768) < 0.002 && std::fabs(rs - 0.1768) < 0.002 && lfe < 0.002;
        }
        check(good, "render: a 5.1 project rendered to a stereo file is folded down", det);
    }
}

// An effect on the master of a 5.1 project: every pair of channels goes through an instance of it (the first through
// the slot's own, the others through FxRuntime::audioExtra), so the same sound on L, C and Ls comes out the same.
static void testMasterFx(const std::string& vst)
{
    std::string err;
    std::vector<AudioPluginDesc> d = describeAudioPluginFile(vst, &err);
    if (d.empty()) {
        check(false, "master fx: the VST effect is recognised", err);
        return;
    }
    Project p = surroundProject();
    p.settings.lfeLowpass = false;
    const char* routes[5] = {"front", "center", "lfe", "rear", "front"};
    for (int i = 0; i < 4; ++i) {
        Track& t = addTrack(p, addMedia(p, "tone:330"));
        t.surround.route = routes[i];
    }
    FxSlot fs;
    fs.id = p.newId();
    fs.kind = "audio";
    fs.audio = d[0];
    fs.name = d[0].name;
    p.audioOut.push_back(fs);
    MediaPool pool;
    pool.setOpener(toneOpen);
    FxRuntime fx;
    fx.prepare(p);
    check(fx.audio(fs.id) && fx.audioExtra(fs.id, 1) && fx.audioExtra(fs.id, 2) && !fx.audioExtra(fs.id, 3),
          "master fx: a 5.1 master's effect gets an instance for each pair of channels", fx.error(fs.id));
    // the extra instances follow the first one's settings
    AudioPlugin* main = fx.audio(fs.id);
    if (main && main->paramCount() > 0) main->paramSet(0, main->paramGet(0) > 0.5f ? 0.2f : 0.8f);
    AudioEngine eng(pool, fx);
    const int n = 4800;
    Six s(n);
    for (int k = 0; k < 5; ++k) eng.renderChannels(p, (int64_t)k * n, n, s.ptr, true);
    float dLC = 0, dLLs = 0, change = 0;
    for (int k = 0; k < n; ++k) {
        dLC = std::max(dLC, std::fabs(s.c[ChL][(size_t)k] - s.c[ChC][(size_t)k]));
        dLLs = std::max(dLLs, std::fabs(s.c[ChL][(size_t)k] - s.c[ChLs][(size_t)k]));
        const double t = (double)(4 * n + k) / 48000.0;
        change = std::max(change, std::fabs(s.c[ChL][(size_t)k] - (float)(0.5 * std::sin(2.0 * kPi * 330.0 * t))));
    }
    check(change > 0.001 && dLC < 1e-5f && dLLs < 1e-5f, "master fx: the effect changes all three pairs alike, at the same settings",
          "changed " + fmt(change) + ", L-C " + fmt(dLC) + ", L-Ls " + fmt(dLLs));
    p.audioOut[0].bypass = true;
    eng.renderChannels(p, 5 * n, n, s.ptr, true);
    float dry = 0;
    for (int k = 0; k < n; ++k) {
        const double t = (double)(5 * n + k) / 48000.0;
        dry = std::max(dry, std::fabs(s.c[ChC][(size_t)k] - (float)(0.5 * std::sin(2.0 * kPi * 330.0 * t))));
    }
    check(dry < 1e-5f, "master fx: bypassed, all six pass as they are");
    (void)routes;
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    {
        FxRuntime fx;
        const uint64_t h = goldHash(goldProject(), fx);
        check(h == kStereoGold, "stereo: a stereo project renders bit for bit as before 5.1 was added", std::to_string(h));
    }
    testLaw();
    testMixer();
    testJson();
    testRender(dir);
    if (argc > 2) testMasterFx(argv[2]);
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
