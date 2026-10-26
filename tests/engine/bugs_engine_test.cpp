// bugs_engine_test: regression tests for engine-level bugs found and fixed in the "bugs" pass (pass 5).
//   bugs_engine_test <scratch folder>
#include "../../src/engine/project.h"
#include "../../src/engine/engine.h"
#include "../../src/core/base.h"
#include "../../src/media/vegasfiles.h"
#include <chrono>
#include <future>
#include <thread>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) ++gPass;
    else ++gFail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}

static Project projectWith(int tracks)
{
    Project p;
    for (int k = 0; k < tracks; ++k) {
        Track t;
        t.id = p.newId();
        t.type = k % 2 ? TrackType::Audio : TrackType::Video;
        t.name = strfmt("Track %d with a name long enough to make the file grow", k + 1);
        p.tracks.push_back(t);
    }
    return p;
}

static void setFileLimit(rlim_t bytes)
{
    struct rlimit r;
    getrlimit(RLIMIT_FSIZE, &r);
    r.rlim_cur = bytes;
    setrlimit(RLIMIT_FSIZE, &r);
}

// Saving a project over the one there must not lose it when the write fails (the disk full): before, the file was cut
// to nothing first and written in place, and a failure only when the buffered end went out was not even told.
static void saveKeepsOldOnFailure(const std::string& dir)
{
    const std::string path = pathJoin(dir, "keep.reno");
    const Project small = projectWith(2);
    std::string err;
    check(small.save(path, &err), "Project::save: a project is saved", err);
    std::string before;
    readFile(path, before);
    std::signal(SIGXFSZ, SIG_IGN);                  // (a write past the limit fails instead of stopping the program)
    for (int tracks : {20, 400}) {                  // (smaller than the stream's buffer, and larger)
        const Project big = projectWith(tracks);
        setFileLimit((rlim_t)before.size() + 64);
        std::string e2;
        const bool ok = big.save(path, &e2);
        setFileLimit(RLIM_INFINITY);
        std::string after;
        readFile(path, after);
        check(!ok, strfmt("Project::save: a write that fails is told (%d tracks)", tracks));
        check(after == before, strfmt("Project::save: the project there is kept when the write fails (%d tracks)", tracks),
              strfmt("%zu bytes before, %zu after", before.size(), after.size()));
        check(!fileExists(path + ".reno-saving"), strfmt("Project::save: nothing is left beside it (%d tracks)", tracks));
    }
    // and an ordinary save over it still replaces it
    const Project two = projectWith(5);
    check(two.save(path, &err), "Project::save: saved again over it", err);
    Project back;
    check(back.load(path, &err) && back.tracks.size() == 5, "Project::save: the new one is there", err);
}

// A VEGAS audio proxy (.sfap0) whose chunk after the samples has a damaged size: reading its header went round and round
// for ever (the size wrapped the position back to the chunk's start), hanging whatever looked at the nested project.
static void damagedProxyDoesNotHang(const std::string& dir)
{
    const std::string path = pathJoin(dir, "damaged.veg.sfap0");
    vegasfiles::AudioProxyWriter w;
    std::string err;
    std::vector<float> l(4800, 0.25f), r(4800, -0.25f);
    const bool made = w.open(path, 48000, &err) && w.write(l.data(), r.data(), 4800) && w.finish(123456789, &err);
    check(made, "a proxy is written", err);
    vegasfiles::AudioProxyInfo info;
    check(vegasfiles::readAudioProxyInfo(path, info) && info.frames == 4800 && info.stamped, "and read back");
    // the chunk after the samples: its size made 2^64 - 7 (the next chunk's place then wraps round to this one)
    std::string d;
    readFile(path, d);
    const int64_t after = (info.dataOffset + 4800 * 8 + 7) & ~(int64_t)7;
    if ((size_t)after + 24 <= d.size()) {
        const uint64_t bad = ~(uint64_t)0 - 6;
        std::memcpy(&d[(size_t)after + 16], &bad, 8);
        writeFile(path, d);
    }
    auto task = std::async(std::launch::async, [path] {
        vegasfiles::AudioProxyInfo i;
        vegasfiles::readAudioProxyInfo(path, i);
        return true;
    });
    const bool done = task.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    check(done, "a proxy with a damaged chunk size is read without hanging");
    if (!done) {
        std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
        std::fflush(stdout);
        std::_Exit(1);
    }
}

// ---------------------------------------------------------------- nested projects saved again
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

// A project that plays 'media' from 0 for 'seconds' at 'gain'.
static Project playing(const std::string& media, double seconds, double gain)
{
    Project p;
    p.settings.sampleRate = 48000;
    MediaItem m;
    m.id = p.newId();
    m.path = media;
    m.name = pathFileName(media);
    if (std::unique_ptr<MediaReader> r = openMediaOrProject(media, nullptr)) m.info = r->info();
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.start = 0;
    e.length = seconds;
    e.gain = gain;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

static double rmsOf(const std::vector<float>& a)
{
    double e = 0;
    for (float v : a) e += (double)v * v;
    return a.empty() ? 0.0 : std::sqrt(e / a.size());
}

// Regression: a nested project saved again (by another window or VEGAS) while the project using it is open must be
// re-read; its readers used to keep the old version.
static void nestedProjectSavedAgain(const std::string& dir)
{
    setAudioProxyOptions(false, false);             // (mixed, not read from a proxy)
    const std::string tone = pathJoin(dir, "nested_tone.wav"), inner = pathJoin(dir, "nested_inner.reno");
    check(writeTone(tone, 2.0, 0.5), "nested: the test sound is written");
    Project in = playing(tone, 2.0, 1.0);
    check(in.save(inner, nullptr), "nested: the nested project is saved");
    const Project outer = playing(inner, 2.0, 1.0);
    MediaPool pool;
    FxRuntime fx;
    AudioEngine eng(pool, fx);
    std::vector<float> l(4800), r(4800);
    eng.render(outer, 24000, 4800, l.data(), r.data(), true);
    const double before = rmsOf(l);
    // saved again, quieter (a little later, so that the file's time differs)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    in.tracks[0].events[0].gain = 0.25;
    check(in.save(inner, nullptr), "nested: saved again, quieter");
    std::this_thread::sleep_for(std::chrono::milliseconds(1700));      // (files are looked at every 1.5 s)
    eng.render(outer, 24000, 4800, l.data(), r.data(), true);
    const double after = rmsOf(l);
    const uint64_t gen = nestedProjectsGeneration();
    check(before > 0.3 && std::fabs(after / before - 0.25) < 0.02, "nested: the project using it plays it as it is now (same readers)",
          strfmt("rms %.4f before, %.4f after", before, after));
    std::this_thread::sleep_for(std::chrono::milliseconds(1700));
    check(nestedProjectsGeneration() == gen, "nested: nothing more changes when nothing more is saved");
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "bugs_engine_out";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    saveKeepsOldOnFailure(dir);
    damagedProxyDoesNotHang(dir);
    nestedProjectSavedAgain(dir);
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
