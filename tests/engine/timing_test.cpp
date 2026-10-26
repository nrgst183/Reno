// Tests of the time maths that place events, pictures and sound where VEGAS puts them.
//
//   timing_test <folder to write test files into>
//
// - sample grid: an event plays the timeline samples nearest its start and end (timeToSample) and reads media from
//   its offset rounded to a file sample (mediaSampleTime);
// - mixer: events meeting between two samples hand over at the nearest sample;
// - AAC in MP4: the sound starts 2112 samples into the decoded data (VEGAS's encoder delay), in WAV it does not;
// - ruler: numbered marks divide a second's frames at any frame rate (165, 25, 24 fps) and are whole frames;
// - .veg times: 100 ns ticks read as seconds and written back as the same ticks.
#include "../../src/engine/engine.h"
#include "../../src/engine/timefmt.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A sound file made up for the mixer: sample n of the file is n + 1 (as a value, divided by 1e6), on both sides, so
// what the mixer puts at a timeline sample says exactly which sample of the file it took.
class CountReader : public MediaReader {
public:
    MediaInfo mi;
    CountReader()
    {
        mi.hasAudio = true;
        mi.sampleRate = 48000;
        mi.channels = 2;
        mi.duration = 100.0;
    }
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const float v = (n < 0 || sampleRate != mi.sampleRate) ? 0.f : (float)((double)(n + 1) / 1e6);
            left[k] = right[k] = v;
        }
        return true;
    }
};

static Project countProject(uint64_t& mediaId)
{
    Project p;
    p.settings.sampleRate = 48000;
    p.settings.fpsNum = 165;
    p.settings.fpsDen = 1;
    MediaItem m;
    m.id = p.newId();
    m.path = "count:1";
    m.name = "count";
    m.info = CountReader().mi;
    p.media.push_back(m);
    mediaId = m.id;
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    t.panType = "add";              // (0 dB on both sides in the centre)
    p.tracks.push_back(t);
    return p;
}

static Event soundEvent(Project& p, uint64_t media, double start, double length, double offset)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = offset;
    e.keepPitch = false;
    setDefaultFadeCurves(e, TrackType::Audio);
    return e;
}

// The file sample the mix took at each timeline sample (-1: nothing there).
static std::vector<int64_t> mixed(const Project& p, int64_t from, int n)
{
    MediaPool pool;
    pool.setOpener([](const std::string& path, std::string* err) -> std::unique_ptr<MediaReader> {
        if (path.compare(0, 6, "count:") == 0) return std::unique_ptr<MediaReader>(new CountReader());
        return openMedia(path, err);
    });
    FxRuntime fx;
    AudioEngine audio(pool, fx);
    std::vector<float> l((size_t)n), r((size_t)n);
    audio.render(p, from, n, l.data(), r.data(), false);
    std::vector<int64_t> out((size_t)n);
    for (int k = 0; k < n; ++k) {
        const double v = l[(size_t)k];
        out[(size_t)k] = std::fabs(v) < 1e-9 ? -1 : (int64_t)std::llround(v * 1e6) - 1;
    }
    return out;
}

static std::string list(const std::vector<int64_t>& v, size_t a, size_t b)
{
    std::string s;
    for (size_t k = a; k < b && k < v.size(); ++k) s += strfmt("%s%lld", s.empty() ? "" : " ", (long long)v[k]);
    return s;
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";

    // ------------------------------------------------------------------ the sample grid
    check(timeToSample(0.0, 48000) == 0 && timeToSample(1054.31 / 48000, 48000) == 1054 && timeToSample(2189.72 / 48000, 48000) == 2190,
          "timeToSample: the nearest sample (1054.31 -> 1054, 2189.72 -> 2190: VEGAS's own mixes)");
    check(timeToSample(2879.0784 / 48000, 48000) == 2879, "an event ending at 2879.08 samples plays up to sample 2878 (VEGAS)");
    check(timeToSample(5e-7, 1000000) == 1, "half way rounds up (0.5 us at 1 MHz)");
    {
        MediaItem m;
        m.info.hasAudio = true;
        m.info.sampleRate = 44100;
        // 52.3427801 s at 44.1 kHz is sample 2308316.60: VEGAS starts that event at 2308317
        check(std::fabs(mediaSampleTime(52.3427801, m) * 44100 - 2308317) < 1e-6, "mediaSampleTime: the offset on the file's own samples");
        m.info.sampleRate = 0;
        check(mediaSampleTime(1.23456789, m) == 1.23456789, "and as it is when the file's rate is not known");
    }

    // ------------------------------------------------------------------ the mixer on that grid
    {
        uint64_t mid = 0;
        Project p = countProject(mid);
        // two events meeting at 1054.31 samples, the second starting 1000 samples into the file
        const double b = 1054.31 / 48000;
        p.tracks[0].events.push_back(soundEvent(p, mid, 0.0, b, 0.0));
        p.tracks[0].events.push_back(soundEvent(p, mid, b, 0.01, 1000.0 / 48000));
        p.autoCrossfades = false;
        p.quickFade = 0;
        const std::vector<int64_t> v = mixed(p, 1050, 10);
        check(v[3] == 1053 && v[4] == 1000 && v[5] == 1001, "events meeting at 1054.31 samples hand over at sample 1054, the second from its offset's sample",
              list(v, 0, 10));
    }
    {
        uint64_t mid = 0;
        Project p = countProject(mid);
        // starting at 100.6 samples with an offset of 500.4 samples: sample 101 plays file sample 500, 102 plays 501
        p.tracks[0].events.push_back(soundEvent(p, mid, 100.6 / 48000, 0.01, 500.4 / 48000));
        p.quickFade = 0;
        const std::vector<int64_t> v = mixed(p, 98, 8);
        check(v[2] == -1 && v[3] == 500 && v[4] == 501, "an event at 100.6 samples starts at sample 101 with its offset's sample (500.4 -> 500)",
              list(v, 0, 8));
    }
    {
        // a 44.1 kHz file in a 48 kHz project: the offset is rounded at 44.1 kHz (14.9971085 s = 661372.48 -> 661372)
        MediaItem m;
        m.info.sampleRate = 44100;
        const double t = mediaSampleTime(14.9971085, m);
        check(std::fabs(t * 44100 - 661372) < 1e-6 && std::fabs(t - 14.9971085) < 0.5 / 44100, "a 44.1 kHz file's offset on its own grid");
    }

    // ------------------------------------------------------------------ AAC's delay (VEGAS's 2112 samples)
    {
        // a burst at sample 20000 of what is written, in an MP4 (AAC) and a WAV
        const int rate = 44100, n = 44100, at = 20000;
        std::vector<float> l((size_t)n, 0.f), r((size_t)n, 0.f);
        for (int k = 0; k < 64; ++k) l[(size_t)(at + k)] = r[(size_t)(at + k)] = 0.8f * (float)std::sin(k * 0.7);
        auto write = [&](const std::string& path, const std::string& family) {
            EncodeSettings es;
            es.path = path;
            es.video = false;
            es.audio = true;
            es.sampleRate = rate;
            es.audioFamily = family;
            std::string err;
            std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
            if (!w) return false;
            w->writeAudio(l.data(), r.data(), n);
            return w->finish(&err);
        };
        auto burstAt = [&](const std::string& path, int* delay) -> int64_t {
            std::string err;
            std::unique_ptr<MediaReader> rd = openMedia(path, &err);
            if (!rd) return -999999;
            if (delay) *delay = rd->info().audioDelaySamples;
            std::vector<float> a((size_t)n), b((size_t)n);
            rd->audio(0, n, rate, a.data(), b.data());
            // (where the burst's energy is, on average: the codec smears its loudest sample about)
            double sum = 0, w = 0;
            for (int k = 0; k < n; ++k) {
                const double e = (double)a[(size_t)k] * a[(size_t)k];
                sum += e * k;
                w += e;
            }
            return w > 0 ? (int64_t)std::llround(sum / w) : -1;
        };
        const std::string mp4 = pathJoin(dir, "timing_aac.mp4"), wav = pathJoin(dir, "timing_pcm.wav");
        const bool wroteMp4 = write(mp4, "aac"), wroteWav = write(wav, "pcm");
        check(wroteMp4 && wroteWav, "an AAC .mp4 and a .wav with a burst at sample 20000 are written");
        int dMp4 = -1, dWav = -1;
        const int64_t pm = burstAt(mp4, &dMp4), pw = burstAt(wav, &dWav);
        // where the burst is in the WAV, it is 2112 samples earlier in the MP4
        check(dWav == 0 && dMp4 == 2112, "the MP4's AAC has VEGAS's 2112-sample delay, the WAV none", strfmt("%d / %d", dMp4, dWav));
        check(pw >= at && pw < at + 64 && std::llabs((pm + 2112) - pw) <= 4, "the burst plays 2112 samples earlier from the AAC (as in VEGAS)",
              strfmt("wav %lld, mp4 %lld", (long long)pw, (long long)pm));
        // a 5.1 read of it (audioChannels) leaves the same samples out
        {
            std::string err;
            std::unique_ptr<MediaReader> rd = openMedia(mp4, &err);
            std::vector<std::vector<float>> ch(6, std::vector<float>((size_t)n));
            float* out[6];
            for (int c = 0; c < 6; ++c) out[c] = ch[(size_t)c].data();
            if (rd) rd->audioChannels(0, n, rate, 6, out);
            double sum = 0, w = 0;
            for (int k = 0; k < n; ++k)
                for (int c = 0; c < 6; ++c) {
                    const double e = (double)ch[(size_t)c][(size_t)k] * ch[(size_t)c][(size_t)k];
                    sum += e * k;
                    w += e;
                }
            const int64_t best = w > 0 ? (int64_t)std::llround(sum / w) : -1;
            check(rd && std::llabs(best - pm) <= 4, "and read as 5.1, the same", strfmt("%lld / %lld", (long long)best, (long long)pm));
        }
    }

    // ------------------------------------------------------------------ the ruler
    {
        // every step below a second divides the second's frames
        bool ok = true;
        std::string bad;
        for (double fps : {24.0, 25.0, 30.0, 30000.0 / 1001, 50.0, 60.0, 120.0, 140.0, 165.0, 24000.0 / 1001})
            for (double pps : {5.0, 20.0, 60.0, 150.0, 400.0, 1000.0, 3000.0, 10000.0, 40000.0}) {
                const int64_t s = rulerStepFrames(pps, fps, 70.0);
                const int64_t nominal = std::llround(fps);
                if (s < nominal && nominal % s != 0) { ok = false; bad += strfmt(" %g fps %g pps: %lld;", fps, pps, (long long)s); }
                if (s >= nominal && s % nominal != 0) { ok = false; bad += strfmt(" %g fps %g pps: %lld;", fps, pps, (long long)s); }
            }
        check(ok, "the ruler's steps divide a second's frames (or are whole seconds) at every frame rate", bad);
        // the steps it had at 30, 60 and 120 fps stay
        check(rulerStepFrames(3000.0, 30.0, 70.0) == 1 && rulerStepFrames(1000.0, 30.0, 70.0) == 5 && rulerStepFrames(500.0, 30.0, 70.0) == 5 &&
                  rulerStepFrames(250.0, 30.0, 70.0) == 10 && rulerStepFrames(150.0, 30.0, 70.0) == 15 && rulerStepFrames(1000.0, 120.0, 70.0) == 10,
              "30 and 120 fps keep their steps (1, 5, 10, 15 frames; 10 at 120 fps)");
        // 165 fps: marks at 17:00, 17:33, 17:66 ... (never 17:95 and 17:105)
        ProjectSettings s;
        s.fpsNum = 165;
        s.fpsDen = 1;
        const int64_t step = rulerStepFrames(800.0, 165.0, 70.0);
        std::string labels;
        for (int64_t f = 17 * 165; f <= 18 * 165; f += step) labels += formatTime(s, (double)f / 165.0, "timecode") + " ";
        check(step == 15 && labels.find("00:00:17:150 00:00:18:000") != std::string::npos, "165 fps: numbered every 15 frames, 17:150 then 18:000", labels);
        // the frame numbers are whole frames at 165 and at 29.97 fps, at the mark's exact time
        bool whole = true;
        for (int64_t f = 0; f < 165 * 120; f += 15)
            if (formatTime(s, (double)f / 165.0, "frames") != strfmt("%lld", (long long)f)) whole = false;
        s.fpsNum = 30000;
        s.fpsDen = 1001;
        for (int64_t f = 0; f < 30 * 600; f += 5)
            if (formatTime(s, (double)f * 1001.0 / 30000.0, "frames") != strfmt("%lld", (long long)f)) whole = false;
        check(whole, "the numbers are the marks' own frames (165 fps; 29.97 fps over ten minutes)");
    }

    // ------------------------------------------------------------------ frames at 140 / 165 fps and NTSC rates
    {
        Project p;
        p.settings.fpsNum = 165;
        p.settings.fpsDen = 1;
        bool ok = true;
        for (int64_t f = 0; f < 165 * 3600; f += 997)
            if (p.frameAt(p.frameTime(f)) != f) ok = false;
        p.settings.fpsNum = 30000;
        p.settings.fpsDen = 1001;
        for (int64_t f = 0; f < 30 * 3600 * 3; f += 1013)
            if (p.frameAt(p.frameTime(f)) != f) ok = false;
        // a time just short of a frame's start (one 100 ns tick) is still the frame before
        p.settings.fpsNum = 165;
        p.settings.fpsDen = 1;
        const double t = std::floor(p.frameTime(2805) * 1e7 - 1) / 1e7;
        check(ok && p.frameAt(t) == 2804, "frameAt / frameTime agree over hours at 165 and 29.97 fps");
    }


    // ------------------------------------------------------------------ Time & Frames at 100 fps and over
    {
        // the frame part has the digits of the counter's last frame: 120 fps counts 000..119, so the text never
        // changes width (00:02:06:102 then 00:02:07:006, not 00:02:07:06)
        ProjectSettings s;
        s.fpsNum = 120;
        s.fpsDen = 1;
        check(formatTime(s, 126.0 + 102.0 / 120.0, "timecode") == "00:02:06:102" && formatTime(s, 127.0 + 6.0 / 120.0, "timecode") == "00:02:07:006",
              "120 fps: 00:02:06:102 and 00:02:07:006", formatTime(s, 127.0 + 6.0 / 120.0, "timecode"));
        size_t width = 0;
        bool same = true;
        for (int64_t f = 0; f < 120 * 300; f += 7) {
            const std::string t = formatTime(s, (double)f / 120.0, "timecode");
            if (!width) width = t.size();
            if (t.size() != width) same = false;
        }
        check(same && width == 12, "120 fps: every time is written 12 characters wide");
        check(frameDigits(24) == 2 && frameDigits(30) == 2 && frameDigits(60) == 2 && frameDigits(100) == 2 && frameDigits(101) == 3 &&
                  frameDigits(120) == 3 && frameDigits(165) == 3 && frameDigits(1000) == 3 && frameDigits(1001) == 4,
              "frameDigits: 2 up to 100 fps, 3 from 101");
        // at 100 fps the last frame is 99: still two digits
        s.fpsNum = 100;
        check(formatTime(s, 1.0 + 99.0 / 100.0, "timecode") == "00:00:01:99" && formatTime(s, 2.0 + 5.0 / 100.0, "timecode") == "00:00:02:05", "100 fps: two digits");
        // the other formats are not changed (SMPTE counters go to 30 at most)
        s.fpsNum = 30000;
        s.fpsDen = 1001;
        check(formatTime(s, 1.0 * 1001.0 / 1000.0 + 5.0 * 1001.0 / 30000.0, "smpteNonDrop") == "00:00:01:05" &&
                  formatTime(s, 0.5, "time") == "00:00:00.500",
              "SMPTE and Time keep their widths", formatTime(s, 1.0 * 1001.0 / 1000.0 + 5.0 * 1001.0 / 30000.0, "smpteNonDrop"));
        // typed times: both the padded and the short frame part are read
        s.fpsNum = 120;
        s.fpsDen = 1;
        double a = -1, b = -1, c = -1;
        const bool pa = parseTimeText(s, "00:02:07:006", "timecode", &a), pb = parseTimeText(s, "00:02:07:06", "timecode", &b),
                   pc = parseTimeText(s, "00:02:06:102", "timecode", &c);
        check(pa && pb && pc && std::fabs(a - (127.0 + 6.0 / 120.0)) < 1e-9 && std::fabs(b - a) < 1e-9 && std::fabs(c - (126.0 + 102.0 / 120.0)) < 1e-9,
              "120 fps: 00:02:07:006 and 00:02:07:06 both read as frame 6 of second 127");
        // written and read back: the same time, at 120 fps and 29.97 / 59.94
        bool round = true;
        std::string bad;
        for (int num : {120, 30000, 60000, 240, 165}) {
            s.fpsNum = num;
            s.fpsDen = num >= 30000 ? 1001 : 1;
            const double fps = s.fps();
            for (int64_t f = 0; f < (int64_t)(fps * 3600 * 2); f += 4111) {
                const double t = (double)f / fps;
                double back = -1;
                const std::string txt = formatTime(s, t, "timecode");
                if (!parseTimeText(s, txt, "timecode", &back) || std::llround(back * fps) != f) {
                    round = false;
                    bad += strfmt(" %d/%d frame %lld '%s';", s.fpsNum, s.fpsDen, (long long)f, txt.c_str());
                }
            }
        }
        check(round, "Time & Frames written and read back is the same frame (120, 240, 165, 29.97, 59.94 fps, two hours)", bad);
        // a time exactly on a frame shows that frame (never the one before), a time just before it the one before
        // (floored, not rounded: 0.6 of a frame in is still that frame), and the text agrees with Project::frameAt
        bool onFrame = true;
        bad.clear();
        for (int num : {120, 30000, 60000, 24000, 25}) {
            Project p;
            p.settings.fpsNum = num;
            p.settings.fpsDen = num >= 24000 ? 1001 : 1;
            const double fps = p.settings.fps();
            const int nominal = (int)std::lround(fps);
            for (int64_t f = 0; f < (int64_t)(fps * 3600 * 3); f += 997) {
                const double t = p.frameTime(f);
                auto frameOf = [&](const std::string& txt) {
                    int h = 0, m = 0, sec = 0, ff = 0;
                    std::sscanf(txt.c_str(), "%d:%d:%d:%d", &h, &m, &sec, &ff);
                    return ((int64_t)(h * 3600 + m * 60 + sec)) * nominal + ff;
                };
                const int64_t at = frameOf(formatTime(p.settings, t, "timecode"));
                const int64_t later = frameOf(formatTime(p.settings, t + 0.6 / fps, "timecode"));
                const int64_t before = f > 0 ? frameOf(formatTime(p.settings, t - 0.001 / fps, "timecode")) : -1;
                if (at != f || later != f || (f > 0 && before != f - 1) || p.frameAt(t) != at) {
                    onFrame = false;
                    bad += strfmt(" %d fps frame %lld: %lld %lld %lld;", num, (long long)f, (long long)at, (long long)later, (long long)before);
                    if (bad.size() > 300) break;
                }
            }
        }
        check(onFrame, "a time on or within a frame shows that frame, just before it the previous one (120, 29.97, 59.94, 23.976, 25 fps)", bad);
    }

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
