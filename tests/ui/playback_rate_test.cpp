// Playing at other rates (VEGAS's J/K/L shuttle and Rate slider): the sound follows the rate (varispeed: faster and higher,
// backwards when negative, quiet at the fastest rates), the cursor moves at that rate either way, and the picture follows. The
// sound is taken from a stand-in sound device the test pulls from itself.
//
//   playback_rate_test
// Build: uibuild playback_rate_test tests/ui/playback_rate_test.cpp
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <chrono>
#include <cmath>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

static const double kPi = 3.14159265358979323846;

// "ramp:" 20 s of sound whose value is seconds / 20 on both sides; "tone:" 20 s of a 1 kHz sine at half volume;
// "frames:" 20 s of 64x36 pictures whose blue is the frame number.
class Reader : public MediaReader {
public:
    MediaInfo mi;
    std::string kind;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        (void)maxW;
        (void)maxH;
        if (kind != "frames") return nullptr;
        ImagePtr img = Image::make(64, 36, PixelDepth::Byte, false);
        const int f = std::max(0, std::min(599, (int)std::floor(seconds * 30.0 + 1e-6)));
        for (int y = 0; y < 36; ++y)
            for (int x = 0; x < 64; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = (uint8_t)(f % 256); d[1] = (uint8_t)(f / 256); d[2] = 0; d[3] = 255;
            }
        return img;
    }
    bool audio(int64_t start, int frames, int rate, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) {
            const double t = (double)(start + k) / rate;
            float v = 0.f;
            if (t >= 0 && t < 20.0) v = kind == "ramp" ? (float)(t / 20.0) : (float)(0.5 * std::sin(2 * kPi * 1000.0 * t));
            l[k] = r[k] = v;
        }
        return kind != "frames";
    }
};

static std::unique_ptr<MediaReader> opener(const std::string& path, std::string* error)
{
    std::unique_ptr<Reader> r(new Reader());
    r->kind = path.substr(0, path.find(':'));
    r->mi.path = path;
    r->mi.duration = 20;
    if (r->kind == "frames") {
        r->mi.hasVideo = true;
        r->mi.width = 64;
        r->mi.height = 36;
        r->mi.fps = 30;
    } else if (r->kind == "ramp" || r->kind == "tone") {
        r->mi.hasAudio = true;
        r->mi.sampleRate = 48000;
        r->mi.channels = 2;
    } else {
        if (error) *error = "unknown";
        return nullptr;
    }
    return std::unique_ptr<MediaReader>(r.release());
}

// A sound device that takes sound only when the test asks.
class ManualOutput : public AudioOutput {
public:
    Pull pull;
    bool start(int, Pull p) override { pull = std::move(p); return true; }
    void stop() override { pull = Pull(); }
    std::string name() const override { return "test"; }
    double latency = 0;
    double latencySeconds() const override { return latency; }
};

// Takes 'seconds' of sound from the playback in 5 ms pieces, at about the pace of a sound card.
static void takeSound(ManualOutput* out, double seconds, std::vector<float>& got)
{
    const int piece = 240;
    const int pieces = (int)(seconds * 48000 / piece);
    std::vector<float> l(piece), r(piece);
    for (int k = 0; k < pieces && out->pull; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        QCoreApplication::processEvents();
        if (!out->pull) break;  // playback stopped by itself
        out->pull(l.data(), r.data(), piece);
        got.insert(got.end(), l.begin(), l.end());
    }
}

static int64_t addClip(const std::string& path, TrackType type)
{
    AppState& app = AppState::get();
    uint64_t id = 0;
    app.edit("test", [&](Project& p) {
        MediaItem m;
        m.id = p.newId();
        m.path = path;
        m.name = path;
        std::unique_ptr<MediaReader> rd = opener(path, nullptr);
        m.info = rd->info();
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = type;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.start = 0;
        e.length = 20;
        t.events.push_back(e);
        p.tracks.push_back(t);
        id = m.id;
    });
    return (int64_t)id;
}

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    AppState& app = AppState::get();
    app.mediaOpener = opener;
    app.edit("settings", [](Project& p) {
        p.settings.sampleRate = 48000;
        p.settings.width = 64;
        p.settings.height = 36;
    });
    Playback pb;
    ManualOutput* out = new ManualOutput();
    pb.setAudioOutput(std::unique_ptr<AudioOutput>(out));

    check(Playback::nextShuttleRate(0, 1) == 1 && Playback::nextShuttleRate(1, 1) == 1.5 && Playback::nextShuttleRate(1.5, 1) == 2 &&
              Playback::nextShuttleRate(2, 1) == 4 && Playback::nextShuttleRate(4, 1) == 10 && Playback::nextShuttleRate(10, 1) == 20 &&
              Playback::nextShuttleRate(20, 1) == 20,
          "L steps 1, 1.5, 2, 4, 10, 20 and stays at 20");
    check(Playback::nextShuttleRate(0, -1) == -1 && Playback::nextShuttleRate(-1, -1) == -1.5 && Playback::nextShuttleRate(2, -1) == -1 &&
              Playback::nextShuttleRate(-4, 1) == 1 && Playback::nextShuttleRate(0.25, 1) == 1,
          "J the same backwards; against the way it is going, either starts at 1");

    // ------------------------------------------------------------------ the sound follows the rate
    addClip("ramp:", TrackType::Audio);
    auto slope = [](const std::vector<float>& s, size_t from, size_t to) {
        // the ramp's change per second of output: (seconds of file) / 20 per output second
        return (double)(s[to] - s[from]) / ((double)(to - from) / 48000.0) * 20.0;
    };
    struct Case { double rate; const char* what; };
    for (const Case& c : {Case{1.0, "at normal speed"}, Case{2.0, "at 2x"}, Case{0.5, "at half speed"}, Case{-1.0, "backwards"}, Case{-1.5, "backwards at 1.5x"}, Case{4.0, "at 4x"}}) {
        app.setCursor(10.0);
        pb.playAt(c.rate);
        std::vector<float> got;
        takeSound(out, 0.6, got);
        const double cursor = pb.position();
        pb.pause();
        if (got.size() < 48000 / 2) {
            check(false, std::string("sound comes ") + c.what, "too little");
            continue;
        }
        const double s = slope(got, 4800, got.size() - 1);
        check(std::fabs(s - c.rate) < 0.03 * std::max(1.0, std::fabs(c.rate)), strfmt("the sound plays %s (the file goes by at %.2f x)", c.what, c.rate),
              strfmt("%.3f", s));
        // and it is where the cursor says
        const double heardAt = got.back() * 20.0;
        check(std::fabs(heardAt - cursor) < 0.25 * std::max(1.0, std::fabs(c.rate)), strfmt("%s the sound is where the cursor is", c.what),
              strfmt("sound %.3f s, cursor %.3f s", heardAt, cursor));
    }
    // the fastest rates are quiet
    app.setCursor(5.0);
    pb.playAt(10.0);
    std::vector<float> fast;
    takeSound(out, 0.3, fast);
    double loud = 0;
    for (float v : fast) loud = std::max(loud, (double)std::fabs(v));
    const double at = app.cursor();
    pb.pause();
    check(loud == 0.0 && at > 6.5, "at 10x the sound is muted (as in VEGAS) and the cursor races on", strfmt("loudest %.3f, cursor %.2f", loud, at));

    // backwards to the start: stops there
    app.setCursor(0.3);
    pb.playAt(-2.0);
    std::vector<float> tail;
    takeSound(out, 0.5, tail);
    for (int k = 0; k < 20 && pb.playing(); ++k) uitest::settle(20);
    check(!pb.playing() && app.cursor() < 0.01, "playing backwards stops at the start", strfmt("%.3f", app.cursor()));

    // a loop region played backwards goes round it
    app.setLoop(4.0, 5.0);
    app.loopOn = true;
    app.setCursor(4.2);
    pb.playAt(-2.0);
    std::vector<float> loop;
    takeSound(out, 0.6, loop);
    double lo = 1e9, hi = -1e9;
    for (size_t k = 4800; k < loop.size(); ++k) {
        lo = std::min(lo, (double)loop[k] * 20.0);
        hi = std::max(hi, (double)loop[k] * 20.0);
    }
    pb.pause();
    check(lo > 3.98 && hi < 5.02 && hi - lo > 0.8, "backwards round a loop region: the sound stays inside it", strfmt("%.3f .. %.3f s", lo, hi));
    app.loopOn = false;
    app.setLoop(0, 0);

    // a short loop region at normal speed with a sound card 100 ms behind: the cursor follows what is heard (never before the loop's start just after going round, nor past its end)
    {
        out->latency = 0.1;
        app.setLoop(6.0, 6.25);
        app.loopOn = true;
        app.setCursor(6.0);
        pb.play();
        const int piece = 240;
        std::vector<float> l(piece), r(piece), heard;
        double worstOutside = 0, worstOff = 0;
        for (int k = 0; k < 300 && out->pull; ++k) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            out->pull(l.data(), r.data(), piece);
            heard.insert(heard.end(), l.begin(), l.end());
            const double pos = pb.position();
            if (pos < 6.0 - 1e-6) worstOutside = std::max(worstOutside, 6.0 - pos);
            if (pos > 6.25 + 1e-6) worstOutside = std::max(worstOutside, pos - 6.25);
            // what the speakers play now: the sample handed over 'latency' ago
            const long at = (long)heard.size() - (long)(0.1 * 48000);
            if (at > 0 && k > 30) {
                // (at the loop's end 6.25 and its start 6.0 are the same moment)
                double d = std::fabs(pos - heard[(size_t)at - 1] * 20.0);
                d = std::min(d, std::fabs(d - 0.25));
                worstOff = std::max(worstOff, d);
            }
        }
        pb.pause();
        check(worstOutside < 1e-6, "a short loop region: the cursor stays inside it", strfmt("%.4f s outside", worstOutside));
        check(worstOff < 0.002, "and is at the sample being heard", strfmt("%.4f s off", worstOff));
        out->latency = 0;
        app.loopOn = false;
        app.setLoop(0, 0);
    }

    // pitch: a 1 kHz tone at 2x is 2 kHz
    app.edit("tone", [](Project& p) { p.tracks.clear(); });
    addClip("tone:", TrackType::Audio);
    for (double rate : {2.0, -1.0, 0.5}) {
        app.setCursor(8.0);
        pb.playAt(rate);
        std::vector<float> got;
        takeSound(out, 0.5, got);
        pb.pause();
        int crossings = 0;
        for (size_t k = 4801; k < got.size(); ++k)
            if ((got[k - 1] < 0) != (got[k] < 0)) ++crossings;
        const double hz = crossings / 2.0 / ((got.size() - 4801) / 48000.0);
        check(std::fabs(hz - 1000.0 * std::fabs(rate)) < 25.0 * std::fabs(rate), strfmt("a 1 kHz tone played at %.1f x sounds at %.0f Hz", rate, 1000.0 * std::fabs(rate)),
              strfmt("%.1f Hz", hz));
    }

    // ------------------------------------------------------------------ the picture follows
    app.edit("frames", [](Project& p) { p.tracks.clear(); });
    addClip("frames:", TrackType::Video);
    double shownAt = -1;
    int shownFrame = -1;
    pb.onFrame = [&](const QImage& img, double seconds, double) {
        shownAt = seconds;
        shownFrame = qBlue(img.pixel(10, 10)) + 256 * qGreen(img.pixel(10, 10));
    };
    app.setCursor(12.0);
    pb.playAt(-1.0);
    std::vector<float> none;
    takeSound(out, 0.5, none);
    uitest::settle(100);
    pb.pause();
    uitest::settle(200);
    check(shownAt > 11.0 && shownAt < 11.9 && shownFrame == (int)std::floor(shownAt * 30.0 + 1e-6),
          "playing backwards, the picture shown is the frame at the cursor (which went back)", strfmt("%.3f s, frame %d", shownAt, shownFrame));

    return uitest::summary();
}
