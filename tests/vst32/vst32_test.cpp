// Tests the 32-bit VST2 bridge on Linux: Reno's bridged plug-in object against reno_vst32 (the same server source, built
// 64-bit) running a Linux VST2 test plug-in (bridgevst.cpp). Usage: vst32_test <reno_vst32> <bridgevst.so> [Auto-Tune_Evo_VST.dll]
// Built and run by tests/vst32/run.sh.
#include "../../src/audiofx/audioplugin.h"
#include "../../src/core/base.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <signal.h>
#include <unistd.h>

using namespace reno;

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if (cond) { ++gPass; } else { ++gFail; std::printf("FAIL line %d: %s  ", __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string gPlug;

static std::unique_ptr<AudioPlugin> direct()
{
    AudioPluginDesc d;
    d.format = "VST2";
    d.path = gPlug;
    std::string e;
    auto p = vst2Create(d, &e);
    if (!p) std::printf("direct: %s\n", e.c_str());
    return p;
}
static std::unique_ptr<AudioPlugin> bridged(std::string* err = nullptr)
{
    AudioPluginDesc d;
    d.format = "VST2";
    d.path = gPlug;
    std::string e;
    auto p = vst32Create(d, &e);
    if (!p && !err) std::printf("bridged: %s\n", e.c_str());
    if (err) *err = e;
    return p;
}

static std::vector<float> noise(size_t n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.f, 1.f);
    std::vector<float> v(n);
    for (auto& x : v) x = d(rng);
    return v;
}

// pids of the reno_vst32 processes that are children of this one
static std::vector<int> servers()
{
    std::vector<int> out;
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "pgrep -P %d reno_vst32", (int)getpid());
    FILE* f = popen(cmd, "r");
    int pid;
    while (f && std::fscanf(f, "%d", &pid) == 1) out.push_back(pid);
    if (f) pclose(f);
    return out;
}

static bool waitFor(const std::function<bool()>& f, int ms)
{
    for (int t = 0; t < ms; t += 5) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

// the same work on a plug-in: some blocks of sound with settings changing in between and inside
static std::vector<float> run(AudioPlugin& p, bool automate)
{
    p.activate(48000.0, 512);
    p.paramSet(0, 0.6f);
    p.paramSet(1, 0.25f);
    p.paramSet(2, 0.8f);
    std::vector<float> all;
    const int sizes[] = {512, 1000, 1, 333, 20000, 64, 4096};
    int64_t pos = 0;
    unsigned seed = 1;
    for (int n : sizes) {
        std::vector<float> l = noise((size_t)n, seed++), r = noise((size_t)n, seed++);
        if (automate) {
            for (int at = 0; at < n; at += 64) {
                p.paramSet(0, 0.3f + 0.4f * (float)((at / 64) % 5) / 4.f);
                p.process(l.data() + at, r.data() + at, std::min(64, n - at), pos + at, true);
            }
        } else {
            p.process(l.data(), r.data(), n, pos, true);
        }
        pos += n;
        all.insert(all.end(), l.begin(), l.end());
        all.insert(all.end(), r.begin(), r.end());
    }
    return all;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: vst32_test reno_vst32 bridgevst.so [dll]\n"); return 2; }
    setenv("RENO_VST32_SERVER", argv[1], 1);
    gPlug = argv[2];
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setLogEcho(false);
    std::vector<std::string> logLines;
    setLogSink([&](const std::string& s) { logLines.push_back(s); });

    // ---- what a file is
    {
        PeKind k = inspectPeFile(gPlug);
        CHECK(k.machine == 0 && !k.exportsVst, "a Linux library is not an executable in PE form");
        CHECK(inspectPeFile("/nonexistent/file.dll").machine == 0, "missing file");
        if (argc > 3 && fileExists(argv[3])) {
            k = inspectPeFile(argv[3]);
            CHECK(k.machine == 0x14C, "Auto-Tune Evo is x86: machine 0x%x", k.machine);
            CHECK(k.exportsKnown && k.exportsVst, "it exports VSTPluginMain");
            CHECK(vst32Wanted(argv[3]), "it is run through the bridge");
            std::string e;
            CHECK(vst32Describe(argv[3], &e).empty() && !e.empty(), "(no way to run an x86 DLL here: %s)", e.c_str());
        }
        CHECK(!vst32Wanted(gPlug), "a 64-bit Linux plug-in is loaded directly");
        setenv("RENO_VST32_FORCE", "1", 1);
        CHECK(vst32Wanted(gPlug), "RENO_VST32_FORCE routes everything through the bridge");
        unsetenv("RENO_VST32_FORCE");
    }

    // ---- describe, through the bridge and directly
    {
        auto d = direct();
        std::string e;
        auto descs = vst32Describe(gPlug, &e);
        CHECK(descs.size() == 1, "describe: %s", e.c_str());
        if (descs.size() == 1 && d) {
            const AudioPluginDesc& b = descs[0];
            CHECK(b.name == d->desc.name && b.vendor == d->desc.vendor && b.uid == d->desc.uid, "same identity (%s / %s)", b.name.c_str(), b.uid.c_str());
            CHECK(b.numInputs == 2 && b.numOutputs == 2 && b.hasEditor == d->desc.hasEditor, "channels and editor");
            CHECK(b.bridged32 && b.format == "VST2" && b.path == gPlug, "marked as bridged");
            CHECK(audioPluginKindNote(b) == " (32-bit, bridged)" && audioPluginKindNote(d->desc).empty(), "the note");
            Json j = b.toJson();
            CHECK(AudioPluginDesc::fromJson(j).bridged32, "bridged32 survives the plug-in list");
        }
        CHECK(waitFor([] { return servers().empty(); }, 3000), "the process of describe ended");
        std::string e2;
        CHECK(vst32Describe("/nonexistent/x.so", &e2).empty() && !e2.empty(), "bad file: %s", e2.c_str());
        CHECK(waitFor([] { return servers().empty(); }, 3000), "no process left after a failed open");
    }

    // ---- createAudioPlugin routes by the file (forced here) and the server being missing is an error, not a crash
    {
        setenv("RENO_VST32_FORCE", "1", 1);
        AudioPluginDesc d;
        d.format = "VST2";
        d.path = gPlug;
        auto p = createAudioPlugin(d);
        CHECK(p && p->desc.bridged32, "createAudioPlugin gives a bridged plug-in");
        p.reset();
        setenv("RENO_VST32_SERVER", "/nonexistent/reno_vst32", 1);
        std::string e;
        p = createAudioPlugin(d, &e);
        CHECK(!p && !e.empty(), "server missing: %s", e.c_str());
        setenv("RENO_VST32_SERVER", argv[1], 1);
        unsetenv("RENO_VST32_FORCE");
    }

    // ---- parameters, programs, chunks, presets: like the plug-in loaded directly
    {
        auto d = direct();
        auto b = bridged();
        CHECK(d && b, "both made");
        if (d && b) {
            CHECK(b->paramCount() == d->paramCount() && b->paramCount() == 5, "param count %d", b->paramCount());
            for (int i = 0; i < 5; ++i) {
                CHECK(b->paramName(i) == d->paramName(i), "name %d: %s", i, b->paramName(i).c_str());
                CHECK(b->paramText(i) == d->paramText(i), "text %d: %s vs %s", i, b->paramText(i).c_str(), d->paramText(i).c_str());
                CHECK(b->paramGet(i) == d->paramGet(i), "initial value %d", i);
            }
            CHECK(b->programCount() == 4 && b->programName(2) == "Program 3" && b->currentProgram() == 0, "programs");
            CHECK(b->latencySamples() == 64, "latency %d", b->latencySamples());
            CHECK(b->hasEditor(), "editor");
            b->paramSet(0, 0.9f);
            d->paramSet(0, 0.9f);
            CHECK(b->paramGet(0) == 0.9f, "a setting reads back at once");
            CHECK(waitFor([&] { return b->paramText(0) == d->paramText(0); }, 1000), "the plug-in has it: %s", b->paramText(0).c_str());
            b->paramSet(1, 2.5f);
            d->paramSet(1, 2.5f);
            CHECK(b->paramGet(1) == 1.f, "settings are clamped to 0..1");

            // state
            std::string sb = b->saveState(), sd = d->saveState();
            CHECK(!sb.empty() && sb == sd && sb[0] == 'Q', "chunk equals the direct one (%s)", sb.substr(0, 4).c_str());
            CHECK(b->setProgram(2) && d->setProgram(2), "set program");
            CHECK(b->currentProgram() == 2 && b->paramGet(1) == d->paramGet(1), "program change shows in the settings (%f)", b->paramGet(1));
            CHECK(b->loadState(sb), "load state");
            CHECK(b->paramGet(0) == 0.9f && b->currentProgram() == 0, "state restored: %f", b->paramGet(0));
            CHECK(b->saveState() == sb, "and saved again the same");
            // a chunk saved by the bridged plug-in loads into a direct one (the .veg holds exactly this)
            d->loadState(sb);
            CHECK(d->paramGet(0) == 0.9f, "direct takes the bridged chunk");
            // presets
            b->setProgram(0);
            d->setProgram(0);
            const std::string fb = b->saveVstPresetFile(false), fd = d->saveVstPresetFile(false);
            CHECK(!fb.empty() && fb == fd, ".fxp equal (%zu bytes)", fb.size());
            const std::string bb = b->saveVstPresetFile(true), bd = d->saveVstPresetFile(true);
            CHECK(!bb.empty() && bb == bd, ".fxb equal (%zu bytes)", bb.size());
            std::string perr;
            CHECK(b->loadVstPresetFile(fb, &perr), "load .fxp: %s", perr.c_str());
            CHECK(!b->loadVstPresetFile("junk", &perr) && !perr.empty(), "bad preset says why: %s", perr.c_str());
            CHECK(!b->setChannels(6) && b->channels() == 2, "a stereo plug-in does not take 6 channels");
            CHECK(b->setChannels(2), "stereo is fine");
        }
    }

    // ---- the sound: bit for bit as loaded directly
    for (int automate = 0; automate < 2; ++automate) {
        auto d = direct();
        auto b = bridged();
        if (!d || !b) { CHECK(false, "made"); continue; }
        const std::vector<float> a = run(*d, automate != 0), c = run(*b, automate != 0);
        CHECK(a.size() == c.size() && std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0, "sound identical (automate %d, %zu samples)", automate, a.size());
        float mx = 0;
        for (float x : c) mx = std::max(mx, std::fabs(x));
        CHECK(mx > 0.1f, "and it is not silence (%f)", mx);
        CHECK(b->stoppedReason().empty(), "running");
    }

    // ---- speed of a block's round trip
    {
        auto b = bridged();
        b->activate(48000.0, 512);
        std::vector<float> l = noise(512, 1), r = noise(512, 2);
        const auto t0 = std::chrono::steady_clock::now();
        const int N = 3000;
        for (int i = 0; i < N; ++i) b->process(l.data(), r.data(), 512, (int64_t)i * 512, true);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("round trip of a 512-frame block: %.1f us\n", us);
        CHECK(us < 2000.0, "a block takes %.0f us", us);
    }

    // ---- the editor
    {
        auto b = bridged();
        std::atomic<int> rw{0}, rh{0}, pc{-1};
        b->onEditorResize = [&](int w, int h) { rw = w; rh = h; };
        b->onParamChangedByPlugin = [&](int i) { pc = i; };
        int w = 0, h = 0;
        CHECK(!b->openEditor(nullptr, &w, &h), "no parent window, no editor");
        CHECK(b->openEditor((void*)0x1234, &w, &h), "editor opens");
        CHECK(w == 320 && h == 240, "size the plug-in asked for: %d x %d", w, h);
        CHECK(waitFor([&] { return rw.load() == 320 && rh.load() == 240; }, 2000), "the resize reached Reno");
        CHECK(waitFor([&] { return pc.load() == 1; }, 2000), "a change made in the window reached Reno");
        CHECK(waitFor([&] { return b->paramGet(1) > 0.76f && b->paramGet(1) < 0.78f; }, 2000), "and its value (%f)", b->paramGet(1));
        b->idle();
        b->closeEditor();
        b->closeEditor();
        CHECK(b->openEditor((void*)0x1234, &w, &h), "again");
        b->closeEditor();
        CHECK(b->stoppedReason().empty(), "all well");
    }

    // ---- several at once, each its own process, all gone at the end
    {
        std::vector<std::unique_ptr<AudioPlugin>> v;
        for (int i = 0; i < 3; ++i) v.push_back(bridged());
        CHECK(servers().size() == 3, "three processes: %zu", servers().size());
        v[0]->paramSet(0, 0.1f);
        CHECK(v[1]->paramGet(0) == 0.5f && v[0]->paramGet(0) == 0.1f, "settings are separate");
        v.clear();
        CHECK(waitFor([] { return servers().empty(); }, 3000), "all processes ended with their plug-ins");
    }

    // ---- the process killed from outside: the sound passes through, nothing else happens to Reno
    {
        auto b = bridged();
        b->activate(48000.0, 512);
        std::vector<float> l = noise(512, 3), r = noise(512, 4), l0 = l, r0 = r;
        b->process(l.data(), r.data(), 512, 0, true);
        CHECK(l != l0, "processed while alive");
        CHECK(b->paramName(1) == "Smooth", "name");
        std::vector<int> pids = servers();
        CHECK(pids.size() == 1, "one process");
        for (int p : pids) kill(p, SIGKILL);
        l = l0; r = r0;
        const auto t0 = std::chrono::steady_clock::now();
        b->process(l.data(), r.data(), 512, 512, false);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(l == l0 && r == r0, "passes through");
        CHECK(ms < 1000.0, "noticed at once (%.0f ms)", ms);
        CHECK(waitFor([&] { return !b->stoppedReason().empty(); }, 1000), "says it stopped");
        std::printf("reason: %s\n", b->stoppedReason().c_str());
        CHECK(b->paramGet(0) == 0.5f, "settings still readable");
        b->paramSet(0, 0.7f);
        CHECK(b->paramName(1) == "Smooth", "names are remembered");
        CHECK(b->paramText(1).empty() && !b->loadState("Q:xx") && b->currentProgram() == -1, "calls that need the process fail quietly");
        int w, h;
        CHECK(!b->openEditor((void*)0x1, &w, &h), "no editor");
        b->closeEditor();
        // started again with the next play
        b->process(l.data(), r.data(), 512, 1024, false);
        CHECK(!b->stoppedReason().empty(), "not by a stopped transport");
        b->process(l.data(), r.data(), 512, 1024, true);
        CHECK(waitFor([&] { return b->stoppedReason().empty(); }, 10000), "started again by the next play");
        l = l0; r = r0;
        b->process(l.data(), r.data(), 512, 1536, true);
        CHECK(l != l0, "processing again");
        CHECK(b->paramGet(0) == 0.7f, "with the setting it had (%f)", b->paramGet(0));
    }

    // ---- a crash inside the plug-in (an access violation in its process) and the same restoring the saved state
    {
        const char* flag = "/tmp/bridgevst_crash.flag";
        FILE* f = std::fopen(flag, "w");
        if (f) std::fclose(f);
        setenv("BRIDGEVST_CRASHFILE", flag, 1);
        auto b = bridged();
        b->activate(48000.0, 512);
        b->paramSet(0, 0.8f);
        b->setProgram(1);
        const std::string saved = b->saveState();
        b->loadState(saved);
        std::vector<float> l = noise(512, 3), r = noise(512, 4), l0 = l, r0 = r;
        b->process(l.data(), r.data(), 512, 0, true);
        b->paramSet(3, 1.0f);
        l = l0; r = r0;
        b->process(l.data(), r.data(), 512, 512, true);
        CHECK(l == l0 && r == r0, "the block of the crash passes through");
        CHECK(!b->stoppedReason().empty(), "stopped: %s", b->stoppedReason().c_str());
        b->process(l.data(), r.data(), 512, 1024, false);
        b->process(l.data(), r.data(), 512, 1024, true);
        CHECK(waitFor([&] { return b->stoppedReason().empty(); }, 10000), "started again");
        CHECK(b->currentProgram() == 1, "the program it had: %d", b->currentProgram());
        l = l0; r = r0;
        b->process(l.data(), r.data(), 512, 1536, true);
        CHECK(l != l0 && b->stoppedReason().empty(), "and works");
        unsetenv("BRIDGEVST_CRASHFILE");
    }

    // ---- a hang inside the plug-in: the wait ends, the process is ended
    {
        const char* flag = "/tmp/bridgevst_hang.flag";
        FILE* f = std::fopen(flag, "w");
        if (f) std::fclose(f);
        setenv("BRIDGEVST_HANGFILE", flag, 1);
        auto b = bridged();
        b->activate(48000.0, 512);
        b->paramSet(4, 1.0f);
        std::vector<float> l = noise(512, 3), r = noise(512, 4), l0 = l, r0 = r;
        const auto t0 = std::chrono::steady_clock::now();
        b->process(l.data(), r.data(), 512, 0, true);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(l == l0 && r == r0, "passes through");
        CHECK(s > 3.0 && s < 8.0, "gave up after %.1f s", s);
        CHECK(!b->stoppedReason().empty(), "says so: %s", b->stoppedReason().c_str());
        CHECK(waitFor([] { return servers().empty(); }, 3000), "the hung process was ended");
        unsetenv("BRIDGEVST_HANGFILE");
    }

    // ---- many threads at once (settings from one, sound from another, window calls from a third)
    {
        auto b = bridged();
        b->activate(48000.0, 256);
        std::atomic<bool> stop{false};
        std::thread t1([&] { int i = 0; while (!stop) { b->paramSet(0, (i++ % 100) / 100.f); b->paramGet(1); } });
        std::thread t2([&] { while (!stop) { b->paramText(0); b->paramName(2); b->currentProgram(); } });
        std::vector<float> l = noise(256, 3), r = noise(256, 4);
        for (int i = 0; i < 1500; ++i) b->process(l.data(), r.data(), 256, i * 256, true);
        stop = true;
        t1.join();
        t2.join();
        CHECK(b->stoppedReason().empty(), "still running after the threads");
    }

    CHECK(waitFor([] { return servers().empty(); }, 3000), "no processes left over");
    if (gFail) for (const auto& l : logLines) std::printf("log: %s\n", l.c_str());
    std::printf("%d checks passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
