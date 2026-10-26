// Tests of nested projects' audio proxies (engine.h setAudioProxyOptions): the project's sound is rendered in the
// background to "<project>.sfap0" and read from there until the project or a file it uses changes. Building can
// be cancelled, forced, and is off by default.
//
//   audioproxy_test <scratch folder>
#include "../../src/engine/engine.h"
#include "../../src/media/vegasfiles.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>

using namespace reno;

static int gPass = 0, gFail = 0;
static bool check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) {
        ++gPass;
        std::printf("PASS %s\n", name.c_str());
    } else {
        ++gFail;
        std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    }
    std::fflush(stdout);
    return ok;
}

static bool writeTone(const std::string& path, double seconds, double amp)
{
    EncodeSettings es;
    es.path = path;
    es.video = false;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    const int n = (int)(seconds * 48000);
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int k = 0; k < n; ++k) l[(size_t)k] = r[(size_t)k] = (float)(amp * std::sin(2 * 3.14159265358979 * 440.0 * k / 48000.0));
    return w->writeAudio(l.data(), r.data(), n) && w->finish(nullptr);
}

static MediaItem mediaItem(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    std::unique_ptr<MediaReader> r = openMediaOrProject(path, nullptr);
    if (r) m.info = r->info();
    else m.offline = true;
    return m;
}

// A project that plays 'media' from 0 for 'seconds' at 'gain' (and 'rate').
static Project playing(const std::string& media, double seconds, double gain, double rate = 1.0)
{
    Project p;
    p.settings.sampleRate = 48000;
    MediaItem m = mediaItem(p, media);
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.start = 0;
    e.length = seconds;
    e.rate = rate;
    e.gain = gain;
    e.loop = true;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

static std::vector<float> readAll(const std::string& path, int64_t from, int frames, int rate = 48000)
{
    std::vector<float> l((size_t)frames), r((size_t)frames);
    std::unique_ptr<MediaReader> rd = openMediaOrProject(path, nullptr);
    if (rd) rd->audio(from, frames, rate, l.data(), r.data());
    return l;
}

static double rms(const std::vector<float>& a)
{
    double e = 0;
    for (float v : a) e += (double)v * v;
    return a.empty() ? 0.0 : std::sqrt(e / a.size());
}

static double maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0;
    for (size_t k = 0; k < std::min(a.size(), b.size()); ++k) d = std::max(d, (double)std::fabs(a[k] - b[k]));
    return d;
}

// Waits (a little past a second, so that the files' times differ) and saves 'p' to 'path' again.
static bool saveLater(Project& p, const std::string& path)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    return p.save(path, nullptr);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: audioproxy_test <scratch folder>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string tone = pathJoin(dir, "tone.wav"), inner = pathJoin(dir, "inner.reno"), proxy = vegasfiles::audioProxyPath(inner);
    std::remove(proxy.c_str());
    check(writeTone(tone, 1.0, 0.5), "the test sound is written");
    Project in = playing(tone, 1.0, 1.0);
    check(in.save(inner, nullptr), "the nested project is saved");

    std::atomic<int> started{0}, finished{0}, failed{0}, stopped{0};
    setAudioProxyProgressHook([&](const std::string&, double, int state, const std::string&) {
        if (state == 0) ++started;
        else if (state == 1) ++finished;
        else if (state == 2) ++failed;
        else ++stopped;
    });

    // ---------------------------------------------------------------- off by default: mixed, not read from a proxy
    const std::vector<float> live = readAll(inner, 0, 48000);
    waitForAudioProxies(5);
    check(!fileExists(proxy), "building is off unless turned on (tests, reno_cli): no proxy is made");
    check(std::fabs(rms(live) - 0.5 / std::sqrt(2.0)) < 0.01, "the nested sound is mixed", strfmt("rms %f", rms(live)));

    // ---------------------------------------------------------------- built in the background, then read from
    setAudioProxyOptions(true, true);
    readAll(inner, 0, 4800);                    // (playing it asks for its proxy)
    check(waitForAudioProxies(120), "the proxy is built in the background");
    check(fileExists(proxy) && audioProxyCurrent(inner), "<project>.sfap0 is there and current");
    vegasfiles::AudioProxyInfo info;
    check(vegasfiles::readAudioProxyInfo(proxy, info) && info.frames == 48000 && info.rate == 48000 && info.channels == 2 && info.format == 3 &&
              info.sourceTime == vegasfiles::fileTime(inner),
          "VEGAS's format: 48000 samples (the project's length), float stereo, stamped with the project's time");
    check(started.load() >= 1 && finished.load() == 1 && failed.load() == 0, "its progress is told, and its end",
          strfmt("%d %d %d", started.load(), finished.load(), failed.load()));
    check(maxDiff(readAll(inner, 0, 48000), live) < 1e-6, "read from the proxy, the sound is the mix's, sample for sample");
    {
        std::vector<float> a = readAll(inner, 30000, 500), b(live.begin() + 30000, live.begin() + 30500);
        check(maxDiff(a, b) < 1e-6, "from anywhere at once (random access)");
    }

    // proof it is the proxy that is read: its samples changed in place (the mark kept) are what comes out - in a render too
    {
        std::string d;
        readFile(proxy, d);
        // (no proxy - a failure above - leaves nothing to change: not written past the end)
        for (int64_t k = 0; k < 48000 * 2 && (size_t)(info.dataOffset + k * 4 + 4) <= d.size(); ++k) {
            const float v = 0.125f;
            std::memcpy(&d[(size_t)(info.dataOffset + k * 4)], &v, 4);
        }
        writeFile(proxy, d);
        setAudioProxyOptions(true, true);       // (readers look again at once)
        const std::vector<float> got = readAll(inner, 0, 48000);
        check(std::fabs(got[100] - 0.125f) < 1e-6 && std::fabs(got[40000] - 0.125f) < 1e-6, "the sound comes from the proxy file");
        Project outer = playing(inner, 1.0, 1.0);
        MediaPool pool;
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        std::vector<float> l(4800), r(4800);
        FinalRenderScope final;
        eng.render(outer, 20000, 4800, l.data(), r.data(), true);
        check(std::fabs(l[100] - 0.125f) < 1e-3, "a render of the project using it reads the proxy too (as VEGAS does)", strfmt("%f", l[100]));
        // played twice as fast (another sample rate): resampled from the proxy
        Project fast = playing(inner, 0.5, 1.0, 2.0);
        AudioEngine eng2(pool, fx);
        eng2.render(fast, 10000, 1000, l.data(), r.data(), true);
        check(std::fabs(l[500] - 0.125f) < 1e-3, "and when the event plays it at another speed", strfmt("%f", l[500]));
    }

    // ---------------------------------------------------------------- stale: the project saved again
    finished = 0;
    in.tracks[0].events[0].gain = 0.5;
    check(saveLater(in, inner), "the nested project is changed and saved again");
    check(!audioProxyCurrent(inner), "its proxy is stale at once (VEGAS's mark: the project's time of last change)");
    const std::vector<float> half = readAll(inner, 0, 48000);
    check(std::fabs(rms(half) - rms(live) * 0.5) < 1e-3, "meanwhile its sound is mixed (as changed)", strfmt("%f %f", rms(half), rms(live)));
    check(waitForAudioProxies(120) && audioProxyCurrent(inner) && finished.load() == 1, "and the proxy is built again");
    check(maxDiff(readAll(inner, 0, 48000), half) < 1e-6, "from the project as it is now");

    // ---------------------------------------------------------------- stale: a file it uses changed (Reno's check on top of VEGAS's)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(writeTone(tone, 1.0, 0.25), "the nested project's sound file is changed");
    check(!audioProxyCurrent(inner), "its proxy is stale (VEGAS would not notice: its mark is the project file's time only)");
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));     // (readers look at what the files use every 3 s)
    readAll(inner, 0, 4800);
    check(waitForAudioProxies(120) && audioProxyCurrent(inner), "and built again");
    {
        const std::vector<float> q = readAll(inner, 0, 48000);
        check(std::fabs(rms(q) - rms(live) * 0.25) < 1e-3, "with the new sound", strfmt("%f", rms(q)));
    }

    // ---------------------------------------------------------------- the parent's view
    {
        Project outer = playing(inner, 1.0, 1.0);
        check(nestedProjectsNeedingProxies(outer).empty(), "nothing to rebuild while every nested proxy is current");
        std::remove(proxy.c_str());
        const std::vector<std::string> need = nestedProjectsNeedingProxies(outer);
        check(need.size() == 1 && pathFileName(need[0]) == "inner.reno", "a missing proxy is listed for Rebuild Offline Nested Audio");
    }

    // ---------------------------------------------------------------- cancelled
    {
        const std::string longer = pathJoin(dir, "long.reno"), lp = vegasfiles::audioProxyPath(longer);
        std::remove(lp.c_str());
        Project lg = playing(tone, 600.0, 1.0);             // ten minutes of the looped sound: long enough to stop half way
        check(lg.save(longer, nullptr), "a long nested project is saved");
        stopped = 0;
        buildAudioProxy(longer);
        for (int k = 0; k < 200 && started.load() == 0; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stopAudioProxyBuilds(true, true);
        waitForAudioProxies(30);
        check(!fileExists(lp) && !fileExists(lp + ".part") && stopped.load() == 1, "cancelled half way: nothing is left behind",
              strfmt("stopped %d", stopped.load()));
        readAll(longer, 0, 4800);
        waitForAudioProxies(5);
        check(!fileExists(lp + ".part") && !fileExists(lp), "a cancelled one is not built again by itself (its sound is mixed)");
        // forced (Rebuild Offline Nested Audio): built after all - and the build of the next one is not held up
        Project small = playing(tone, 0.5, 1.0);
        const std::string sp = pathJoin(dir, "small.reno");
        small.save(sp, nullptr);
        buildAudioProxy(longer, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stopAudioProxyBuilds(true);         // (another project opened: stopped, but not held off)
        buildAudioProxy(sp);
        check(waitForAudioProxies(120) && audioProxyCurrent(sp), "a build stopped for another project's sake leaves the next one free");
    }

    // ---------------------------------------------------------------- a folder that cannot be written: the proxy is kept in memory
    {
        const std::string tone2 = pathJoin(dir, "tone2.wav"), memp = pathJoin(dir, "mem.reno"), mproxy = vegasfiles::audioProxyPath(memp);
        std::remove(mproxy.c_str());
        check(writeTone(tone2, 1.0, 0.5), "memory proxy: a sound file is written");
        Project mp = playing(tone2, 1.0, 1.0);
        check(mp.save(memp, nullptr), "memory proxy: the nested project is saved");
        // (a folder in the place of the part file: nothing can be written there, whoever asks)
        std::filesystem::create_directories(mproxy + ".part");
        setAudioProxyOptions(true, true);
        const std::vector<float> before = readAll(memp, 0, 48000);
        check(waitForAudioProxies(120), "memory proxy: the build is over");
        check(!fileExists(mproxy), "memory proxy: no file could be made");
        check(nestedProjectsNeedingProxies(playing(memp, 1.0, 1.0)).empty(), "memory proxy: and it is not asked for again");
        // the sound file changes without its time (a second's difference VEGAS and Reno would not see): what is played is the proxy's
        const auto t0 = std::filesystem::last_write_time(tone2);
        check(writeTone(tone2, 1.0, 0.25), "memory proxy: the sound file is changed behind its back");
        std::filesystem::last_write_time(tone2, t0);
        const std::vector<float> after = readAll(memp, 0, 48000);
        check(maxDiff(after, before) < 1e-6 && rms(after) > 0.3, "memory proxy: the sound comes from memory", strfmt("%f %f", rms(after), rms(before)));
        Project outer = playing(memp, 1.0, 1.0);
        MediaPool pool;
        FxRuntime fx;
        AudioEngine eng(pool, fx);
        std::vector<float> l(4800), r(4800);
        FinalRenderScope final;
        eng.render(outer, 20000, 4800, l.data(), r.data(), true);
        check(std::fabs(l[100] - before[20100]) < 1e-5, "memory proxy: a render of the project using it plays it too");
        std::filesystem::remove_all(mproxy + ".part");
    }

    // ---------------------------------------------------------------- not a project, nonsense
    buildAudioProxy(tone);
    buildAudioProxy(pathJoin(dir, "missing.veg"), true);
    check(waitForAudioProxies(30) && !fileExists(vegasfiles::audioProxyPath(tone)), "media files and missing projects get no proxy");

    setAudioProxyOptions(true, false);
    setAudioProxyProgressHook(nullptr);
    stopAudioProxyBuilds(true);
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
