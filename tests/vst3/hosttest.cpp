// Runs Reno's VST3 host (src/audiofx/vst3host.cpp) against the test plug-in file built from testplug.cpp.
//
//     hosttest <the plug-in file> <a file that is a library but no VST3 plug-in>
//
// The plug-in file has to sit in a bundle: Something.vst3/Contents/<architecture>/Something.vst3
// Prints one line per check and ends with "N checks, M failed"; the exit code is the number that failed.
// See README.txt for how to build both.
#include "../../src/audiofx/audioplugin.h"
#include "../../src/core/dynlib.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace reno;

static int gChecks = 0, gFailed = 0;

static bool check(bool ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const std::string what = vstrfmt(fmt, ap);
    va_end(ap);
    ++gChecks;
    if (!ok) ++gFailed;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    return ok;
}

static bool about(double a, double b) { return std::fabs(a - b) < 1e-6; }

typedef int (*GetFn)(const char*);
typedef int (*PokeFn)(const char*, double);
static GetFn plugGet = nullptr;
static PokeFn plugPoke = nullptr;

// A block of test audio: left is a ramp, right is a different ramp.
static void fill(std::vector<float>& l, std::vector<float>& r, int n)
{
    l.resize((size_t)n);
    r.resize((size_t)n);
    for (int k = 0; k < n; ++k) {
        l[(size_t)k] = 0.001f * (float)(k % 500) + 0.01f;
        r[(size_t)k] = -0.0005f * (float)(k % 300) - 0.02f;
    }
}

// Runs one block through the plug-in and says whether what came out is the test audio times 'gain'.
static bool gives(AudioPlugin& p, double gain, int n = 256, int64_t pos = 0, bool playing = true)
{
    std::vector<float> l, r, l0, r0;
    fill(l, r, n);
    l0 = l;
    r0 = r;
    p.process(l.data(), r.data(), n, pos, playing);
    for (int k = 0; k < n; ++k)
        if (!about(l[(size_t)k], l0[(size_t)k] * gain) || !about(r[(size_t)k], r0[(size_t)k] * gain)) return false;
    return true;
}

static const AudioPluginDesc* findByName(const std::vector<AudioPluginDesc>& list, const std::string& start)
{
    for (const auto& d : list)
        if (d.name.compare(0, start.size(), start) == 0) return &d;
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: hosttest <plug-in file> <library that is no VST3 plug-in>\n");
        return 2;
    }
    const std::string path = argv[1], notPlugin = argv[2];
    const char* levelText = std::getenv("RENO_TESTPLUG_FACTORY");
    int level = levelText ? std::atoi(levelText) : 3;
    if (level < 1 || level > 3) level = 3;
    std::printf("plug-in file: %s\nfactory level: %d\n", path.c_str(), level);
    std::string error;

    // ---------------------------------------------------------------- finding and describing
    const std::string bundle = pathDir(pathDir(pathDir(path)));         // Something.vst3
    {
        const std::vector<std::string> files = findAudioPluginFiles({pathDir(bundle)});
        bool found = false;
        for (const auto& f : files) found = found || f == path;
        check(found, "findAudioPluginFiles finds the file inside its bundle (%d file(s) found)", (int)files.size());
    }
    const std::vector<AudioPluginDesc> descs = describeAudioPluginFile(path, &error);
    check(descs.size() == 3, "describe finds 3 effects and leaves the controller class out (found %d) %s", (int)descs.size(), error.c_str());
    const AudioPluginDesc* gainDesc = findByName(descs, "Reno Test Gain");
    const AudioPluginDesc* monoDesc = findByName(descs, "Reno Test Mono");
    const AudioPluginDesc* toneDesc = findByName(descs, "Reno Test T");
    if (!check(gainDesc && monoDesc && toneDesc, "describe names all three")) {
        std::printf("cannot go on\n");
        return 1;
    }
    check(gainDesc->format == "VST3" && gainDesc->path == path, "describe: format and path");
    check(gainDesc->uid == "52454E4F54455354000000014741494E", "describe: class id as 32 hexadecimal digits (%s)", gainDesc->uid.c_str());
    check(gainDesc->vendor == "Reno Tests", "describe: vendor from the factory when the class names none (%s)", gainDesc->vendor.c_str());
    check(monoDesc->vendor == (level >= 2 ? "Mono Maker" : "Reno Tests"), "describe: vendor of a class that names one (%s)", monoDesc->vendor.c_str());
    check(!gainDesc->isInstrument && toneDesc->isInstrument == (level >= 2), "describe: instrument or effect");
    check(toneDesc->name == (level >= 3 ? "Reno Test T\xC3\xB6ne \xF0\x9F\x8E\xB5" : "Reno Test Tone"), "describe: name (UTF-16 to UTF-8 at level 3) (%s)", toneDesc->name.c_str());
    {
        const std::vector<AudioPluginDesc> again = describeAudioPluginFile(bundle, &error);
        check(again.size() == 3, "describe given the bundle folder instead of the file");
    }
    {
        // (each call first, then the check: the text of the error is only there once the call is done)
        error.clear();
        bool none = describeAudioPluginFile("/no/such/place/Nothing.vst3", &error).empty();
        check(none && !error.empty(), "describe: a file that is not there (\"%s\")", error.c_str());
        error.clear();
        none = describeAudioPluginFile(notPlugin, &error).empty();
        check(none && error == "is not a VST3 plug-in", "describe: a library that is no VST3 plug-in (\"%s\")", error.c_str());
        AudioPluginDesc bad = *gainDesc;
        bad.uid = "not a class id";
        error.clear();
        none = createAudioPlugin(bad, &error) == nullptr;
        check(none && !error.empty(), "create: a class id that cannot be read (\"%s\")", error.c_str());
        bad.uid = "00112233445566778899AABBCCDDEEFF";
        error.clear();
        none = createAudioPlugin(bad, &error) == nullptr;
        check(none && !error.empty(), "create: a class the file does not have (\"%s\")", error.c_str());
        bad.uid = "52454E4F5445535400000001FF4354524C";       // (34 digits)
        none = createAudioPlugin(bad, &error) == nullptr;
        check(none, "create: a class id of the wrong length");
        bad.uid = "52454E4F54455354000000FF4354524C";         // the controller class: not an effect
        error.clear();
        none = createAudioPlugin(bad, &error) == nullptr;
        check(none && !error.empty(), "create: the controller class on its own (\"%s\")", error.c_str());
    }

    // the plug-in's own counts (the same library, opened a second time)
    DynLib lib;
    if (!lib.open(path, &error)) {
        std::printf("FAIL cannot open the plug-in file myself: %s\n", error.c_str());
        return 1;
    }
    plugGet = (GetFn)lib.symbol("RenoTestPlugGet");
    plugPoke = (PokeFn)lib.symbol("RenoTestPlugPoke");
    if (!plugGet || !plugPoke) {
        std::printf("FAIL the plug-in file has no test functions\n");
        return 1;
    }
    check(plugGet("starts") == 1 && plugGet("factories") == 1, "the file was started once and asked for its factory once (%d, %d)", plugGet("starts"), plugGet("factories"));
    check(plugGet("hostContexts") == (level >= 3 ? 1 : 0), "IPluginFactory3::setHostContext");
    check(plugGet("live") == 0, "nothing is left of the effects that could not be made (%d objects)", plugGet("live"));

    // ---------------------------------------------------------------- the gain effect
    {
        error.clear();
        std::unique_ptr<AudioPlugin> p = createAudioPlugin(*gainDesc, &error);
        if (!check(p != nullptr, "create the gain effect %s", error.c_str())) return 1;
        check(p->desc.numInputs == 2 && p->desc.numOutputs == 2, "gain: stereo in, stereo out (%d, %d)", p->desc.numInputs, p->desc.numOutputs);
        check(plugGet("live") == 2 && plugGet("initialized") == 2, "gain: a component and a controller were made and initialized");
        check(plugGet("connects") == 2 && plugGet("messages") == 1, "gain: the two were connected and a message made by the host went across (%d, %d)",
              plugGet("connects"), plugGet("messages"));
        check(plugGet("componentStates") == 1 && plugGet("handlers") == 1, "gain: the controller got the component's state and a component handler");

        // parameters
        check(p->paramCount() == 3, "gain: 3 parameters (the hidden one is left out): %d", p->paramCount());
        check(p->paramName(0) == "Gain" && p->paramName(1) == "Page" && p->paramName(2) == "Level", "gain: parameter names (%s, %s, %s)",
              p->paramName(0).c_str(), p->paramName(1).c_str(), p->paramName(2).c_str());
        check(about(p->paramGet(0), 0.5), "gain: the gain parameter starts at 0.5 (%g)", p->paramGet(0));
        check(p->paramText(0) == "1.00 x", "gain: value as text with its unit (\"%s\")", p->paramText(0).c_str());
        check(p->paramText(1) == "Page 1", "gain: value as text without a unit (\"%s\")", p->paramText(1).c_str());
        check(p->paramText(2) == "0.000", "gain: value as text when the plug-in gives none (\"%s\")", p->paramText(2).c_str());
        p->paramSet(99, 0.5f);
        p->paramSet(-1, 0.5f);
        check(p->paramName(-1).empty() && p->paramName(3).empty() && p->paramText(99).empty() && p->paramGet(99) == 0.f, "gain: parameters that are not there");

        // audio
        {
            std::vector<float> l, r;
            fill(l, r, 64);
            const std::vector<float> l0 = l;
            p->process(l.data(), r.data(), 64, 0, true);
            check(l == l0 && plugGet("blocks") == 0, "gain: process before activate leaves the audio alone");
        }
        check(p->activate(48000.0, 256), "gain: activate");
        check(plugGet("sidechainOn") == 0, "gain: the side-chain bus was switched off");
        check(gives(*p, 1.0), "gain: audio comes out times 1");
        p->paramSet(0, 0.25f);
        check(about(p->paramGet(0), 0.25), "gain: paramSet shows in paramGet at once");
        check(gives(*p, 0.5) && plugGet("points") == 1, "gain: paramSet changes the gain with the next block (to 0.5)");
        check(gives(*p, 0.5) && plugGet("points") == 1, "gain: and is sent only once");
        p->paramSet(0, 0.1f);
        p->paramSet(0, 1.0f);
        check(gives(*p, 2.0) && plugGet("points") == 2, "gain: two changes before a block arrive as the newer one (gain 2)");
        p->paramSet(0, 7.f);
        check(about(p->paramGet(0), 1.0), "gain: paramSet keeps values inside 0..1");
        p->paramSet(0, 0.5f);
        const int blocksBefore = plugGet("blocks");
        check(gives(*p, 1.0, 1000, 5000, true), "gain: 1000 frames in one call, block size 256");
        check(plugGet("maxBlock") <= 256 && plugGet("blocks") - blocksBefore == 4, "gain: they went in as 4 blocks of at most 256 (%d, biggest %d)",
              plugGet("blocks") - blocksBefore, plugGet("maxBlock"));
        check(plugGet("position") == 5000 + 768 && plugGet("playing") == 1, "gain: the plug-in was told the position and that it is playing (%d)", plugGet("position"));
        gives(*p, 1.0, 256, 0, false);
        check(plugGet("playing") == 0, "gain: and that it is not");
        {
            std::vector<float> l(256, 0.25f), r(256, -0.125f);
            p->paramSet(0, 1.0f);
            p->process(l.data(), r.data(), 256, 0, true);
            check(about(l[10], 0.5) && about(r[200], -0.25), "gain: left and right stay left and right");
            check(about(p->paramGet(2), 0.0), "gain: a value the processor reports is not shown before idle()");
            p->idle();
            check(about(p->paramGet(2), 0.5), "gain: idle() hands it to the controller (level %g)", p->paramGet(2));
            p->paramSet(2, 0.9f);
            const int pointsBefore = plugGet("points");
            p->process(l.data(), r.data(), 256, 0, true);
            p->idle();
            check(plugGet("points") == pointsBefore, "gain: a read-only parameter cannot be set");
            p->paramSet(0, 0.f);
            std::vector<float> zl(256, 0.25f), zr(256, 0.25f);
            p->process(zl.data(), zr.data(), 256, 0, true);
            bool silent = true;
            for (int k = 0; k < 256; ++k) silent = silent && zl[(size_t)k] == 0.f && zr[(size_t)k] == 0.f;
            check(silent, "gain: output the plug-in marks silent is silent whatever it left in the buffer");
            std::vector<float> none;
            p->process(none.data(), none.data(), 0, 0, true);
            p->process(nullptr, nullptr, 16, 0, true);
        }

        // state
        p->paramSet(0, 0.3f);
        p->paramSet(1, 2.f / 3.f);
        gives(*p, 0.6);
        const std::string state = p->saveState();
        check(state.compare(0, 5, "VST3:") == 0 && state.size() > 12, "gain: saveState gives text (%s)", state.c_str());
        p->paramSet(0, 0.9f);
        p->paramSet(1, 0.f);
        check(gives(*p, 1.8), "gain: (changed to 1.8)");
        p->paramSet(0, 1.0f);                                   // still waiting for a block when the state comes in
        check(p->loadState(state), "gain: loadState");
        check(about(p->paramGet(0), 0.3) && about(p->paramGet(1), 2.0 / 3.0), "gain: loadState puts both halves' values back (%g, %g)", p->paramGet(0), p->paramGet(1));
        check(gives(*p, 0.6), "gain: and the audio is back at 0.6 (a change made before the state was loaded is dropped)");
        check(!p->loadState("") && !p->loadState("rubbish") && !p->loadState("VST3:no-separator"), "gain: loadState refuses text that is not a state");
        check(!p->loadState("VST3:AAAA:-"), "gain: loadState says so when the plug-in refuses the state");
        check(gives(*p, 0.6), "gain: still 0.6 after those");
        {
            std::unique_ptr<AudioPlugin> q = createAudioPlugin(*gainDesc, &error);
            check(q && q->loadState(state) && about(q->paramGet(0), 0.3) && about(q->paramGet(1), 2.0 / 3.0), "gain: a second one takes the first one's state");
            check(q && q->activate(44100.0, 512) && gives(*q, 0.6), "gain: and plays it");
        }
        check(plugGet("live") == 2, "gain: the second one is gone again (%d objects)", plugGet("live"));

        // the window
        int w = 0, h = 0, resizedW = 0, resizedH = 0, resizes = 0;
        std::vector<int> changed;
        p->onEditorResize = [&](int rw, int rh) { resizedW = rw; resizedH = rh; ++resizes; };
        p->onParamChangedByPlugin = [&](int index) { changed.push_back(index); };
        check(p->hasEditor(), "gain: has a window");
        check(plugGet("live") == 2, "gain: asking did not leave a window behind");
        check(!p->openEditor(nullptr, &w, &h), "gain: no window without a parent");
        check(!p->openEditor((void*)(size_t)0xBAD, &w, &h) && plugGet("viewsOpen") == 0 && plugGet("live") == 2, "gain: a window that will not attach is given up cleanly");
        check(p->openEditor((void*)(size_t)0x1234, &w, &h) && w == 320 && h == 200, "gain: the window opens and says its size (%d x %d)", w, h);
        check(plugGet("viewsOpen") == 1, "gain: the plug-in has it attached");
        check(plugPoke("resize", 500) == 1 && resizes == 1 && resizedW == 500 && resizedH == 240, "gain: the window asks for 500 x 240 and the application hears of it (%d x %d)", resizedW, resizedH);
        check(plugGet("onSize") == 1 && plugGet("sizeWidth") == 500, "gain: and the window is then told its new size");
        check(plugPoke("gain", 0.75) == 1 && changed.size() == 1 && changed[0] == 0, "gain: a knob moved in the window is reported with the parameter's index");
        check(about(p->paramGet(0), 0.75) && gives(*p, 1.5), "gain: and reaches the audio with the next block (gain 1.5)");
        changed.clear();
        check(plugPoke("preset", 0.2) == 1 && changed.empty(), "gain: a preset picked in the window is not acted on inside the plug-in's call");
        p->idle();
        check(changed.size() == 3 && about(p->paramGet(0), 0.2), "gain: idle() then reports every parameter as changed (%d)", (int)changed.size());
        check(p->openEditor((void*)(size_t)0x1234, &w, &h) && plugGet("viewsOpen") == 1, "gain: opening the window again closes the first");
        p->closeEditor();
        check(plugGet("viewsOpen") == 0 && plugGet("live") == 2 && plugPoke("resize", 600) == 0, "gain: the window closes and is gone");
        p->closeEditor();

        // activate again, deactivate
        check(p->activate(96000.0, 64) && gives(*p, 1.5, 300), "gain: activate again with another rate and block size");
        p->deactivate();
        {
            std::vector<float> l, r;
            fill(l, r, 64);
            const std::vector<float> l0 = l;
            const int before = plugGet("blocks");
            p->process(l.data(), r.data(), 64, 0, true);
            check(l == l0 && plugGet("blocks") == before, "gain: process after deactivate leaves the audio alone");
        }
        p->deactivate();
        check(p->activate(48000.0, 256) && p->openEditor((void*)(size_t)0x1234, &w, &h), "gain: active and with its window open ...");
        p.reset();
        check(plugGet("live") == 0 && plugGet("viewsOpen") == 0 && plugGet("handlers") == 0, "gain: ... it can be destroyed and nothing is left (%d objects)", plugGet("live"));
        check(plugGet("initialized") == plugGet("terminated") && plugGet("connects") == plugGet("disconnects"),
              "gain: everything initialized was terminated, everything connected disconnected (%d/%d, %d/%d)", plugGet("initialized"), plugGet("terminated"),
              plugGet("connects"), plugGet("disconnects"));
    }

    // ---------------------------------------------------------------- the mono effect
    {
        const int connectsBefore = plugGet("connects");
        error.clear();
        std::unique_ptr<AudioPlugin> p = createAudioPlugin(*monoDesc, &error);
        if (check(p != nullptr, "create the mono effect %s", error.c_str())) {
            check(p->desc.numInputs == 1 && p->desc.numOutputs == 1, "mono: stays mono when asked to be stereo (%d, %d)", p->desc.numInputs, p->desc.numOutputs);
            check(plugGet("connects") == connectsBefore, "mono: a component without IConnectionPoint is not connected");
            check(p->paramCount() == 3 && p->latencySamples() == 7, "mono: parameters and latency (%d, %d)", p->paramCount(), p->latencySamples());
            check(p->activate(48000.0, 128), "mono: activate");
            std::vector<float> l(200, 1.0f), r(200, 0.0f);
            p->process(l.data(), r.data(), 200, 0, true);
            bool ok = true;
            for (int k = 0; k < 200; ++k) ok = ok && about(l[(size_t)k], 0.5) && about(r[(size_t)k], 0.5);
            check(ok, "mono: left and right are mixed going in and the result goes to both");
            p->paramSet(0, 1.0f);
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            p->process(l.data(), r.data(), 200, 0, true);
            check(about(l[0], 1.0) && about(r[199], 1.0), "mono: paramSet reaches it");
            const std::string state = p->saveState();
            p->paramSet(0, 0.f);
            check(p->loadState(state) && about(p->paramGet(0), 1.0), "mono: state out and in");
        }
    }

    // ---------------------------------------------------------------- the instrument (one object, no audio input, no window)
    {
        const int initializedBefore = plugGet("initialized");
        error.clear();
        std::unique_ptr<AudioPlugin> p = createAudioPlugin(*toneDesc, &error);
        if (check(p != nullptr, "create the instrument %s", error.c_str())) {
            check(plugGet("live") == 1 && plugGet("initialized") == initializedBefore + 1, "tone: component and controller are one object, initialized once");
            check(p->desc.numInputs == 0 && p->desc.numOutputs == 2 && p->desc.isInstrument == (level >= 2), "tone: no input, stereo out (%d, %d)", p->desc.numInputs, p->desc.numOutputs);
            int w = 0, h = 0;
            check(!p->hasEditor() && !p->openEditor((void*)(size_t)0x1234, &w, &h), "tone: has no window");
            check(p->activate(48000.0, 256), "tone: activate");
            std::vector<float> l(300, 0.9f), r(300, -0.9f);
            p->process(l.data(), r.data(), 300, 0, true);
            check(about(l[0], 0.25) && about(r[299], 0.25), "tone: what it makes replaces the audio");
            p->paramSet(0, 1.0f);
            p->process(l.data(), r.data(), 300, 0, true);
            check(about(l[0], 0.5) && about(r[299], 0.5), "tone: paramSet reaches it");
            const std::string state = p->saveState();
            p->paramSet(0, 0.25f);
            p->process(l.data(), r.data(), 300, 0, true);
            check(p->loadState(state) && about(p->paramGet(0), 1.0), "tone: state out and in");
            p->process(l.data(), r.data(), 300, 0, true);
            check(about(l[0], 0.5), "tone: and the audio follows");
        }
    }
    check(plugGet("live") == 0, "nothing is left of the mono effect and the instrument (%d objects)", plugGet("live"));

    // ---------------------------------------------------------------- two threads
    {
        std::unique_ptr<AudioPlugin> p = createAudioPlugin(*gainDesc, &error);
        if (check(p && p->activate(48000.0, 128), "threads: create and activate")) {
            std::atomic<bool> stop{false};
            std::atomic<int> blocks{0};
            std::atomic<int> badSamples{0};
            std::thread audio([&]() {
                std::vector<float> l(512), r(512);
                int64_t pos = 0;
                while (!stop) {
                    std::fill(l.begin(), l.end(), 0.25f);
                    std::fill(r.begin(), r.end(), 0.25f);
                    p->process(l.data(), r.data(), 512, pos, true);
                    // whatever the gain was, every sample is 0.25 times something in 0..2
                    for (int k = 0; k < 512; ++k)
                        if (!(l[(size_t)k] >= 0.f && l[(size_t)k] <= 0.5001f)) ++badSamples;
                    pos += 512;
                    ++blocks;
                }
            });
            const std::string state = p->saveState();
            int w = 0, h = 0;
            for (int k = 0; k < 400; ++k) {
                p->paramSet(0, (float)(k % 11) / 10.f);
                p->paramSet(1, (float)(k % 4) / 3.f);
                if (k % 7 == 0) p->saveState();
                if (k % 13 == 0) p->loadState(state);
                if (k % 50 == 0) p->activate(k % 100 == 0 ? 48000.0 : 44100.0, 64 + k % 200);
                if (k % 90 == 45) p->deactivate();
                if (k % 90 == 46) p->activate(48000.0, 256);
                if (k % 40 == 0) p->openEditor((void*)(size_t)0x1234, &w, &h);
                if (k % 40 == 20) p->closeEditor();
                if (k % 9 == 0) plugPoke("gain", 0.5);
                p->idle();
                p->paramGet(2);
                p->latencySamples();
                std::this_thread::yield();
            }
            while (blocks < 50) std::this_thread::yield();
            stop = true;
            audio.join();
            check(badSamples == 0, "threads: audio stayed sane while everything else was going on (%d blocks)", blocks.load());
        }
    }

    // ---------------------------------------------------------------- the end
    check(plugGet("live") == 0, "at the end no plug-in object is left (%d)", plugGet("live"));
    check(plugGet("initialized") == plugGet("terminated"), "everything initialized was terminated (%d, %d)", plugGet("initialized"), plugGet("terminated"));
    check(plugGet("connects") == plugGet("disconnects") && plugGet("handlers") == 0 && plugGet("viewsOpen") == 0, "nothing is still connected, handled or open");
    check(plugGet("starts") == 1 && plugGet("factories") == 1, "the file was still only started once");
    check(plugGet("errors") == 0, "the plug-in saw the host do nothing wrong (%d wrong)", plugGet("errors"));

    std::printf("\n%d checks, %d failed\n", gChecks, gFailed);
    return gFailed;
}
