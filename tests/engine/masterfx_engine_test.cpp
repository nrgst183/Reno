// Tests of VST plug-ins on the master bus while the sound threads use them (build under ASan + UBSan).
//
//   masterfx_engine_test <strictvst.so> <strictvst6.so> <strictvstu.so>
//
// The three are tests/ui/strictvst.cpp built as it is, with -DSTRICTVST_SURROUND and with -DSTRICTVST_UNTIDY: VST2
// effects that count every time the host breaks VST's rules (sound through a plug-in that is switched off or being
// changed, two threads in it at once, calls from the wrong thread). The test changes a master effect while playback
// and a render use it (sample rate, stereo / 5.1, add / remove, state save / load), and also uses a plug-in whose
// reserved fields hold garbage.
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "../../src/core/dynlib.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace reno;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::printf("PASS %s\n", what.c_str()); }
    else { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    std::fflush(stdout);
}

namespace {

// The counters of a strict plug-in (the file is loaded once; the host's instances share it).
struct Strict {
    DynLib lib;
    int (*violations)() = nullptr;
    int (*processed)() = nullptr;
    const char* (*last)() = nullptr;
    int (*rate)() = nullptr;
    const char* (*directory)() = nullptr;
    bool open(const std::string& path)
    {
        if (!lib.open(path, nullptr)) return false;
        violations = (int (*)())lib.symbol("strictvst_violations");
        processed = (int (*)())lib.symbol("strictvst_processed");
        last = (const char* (*)())lib.symbol("strictvst_last");
        rate = (int (*)())lib.symbol("strictvst_rate");
        directory = (const char* (*)())lib.symbol("strictvst_directory");
        return violations && processed && last && rate && directory;
    }
    int bad() const { return violations(); }
    std::string why() const { return last(); }
};

class Tone : public MediaReader {
public:
    MediaInfo mi;
    Tone()
    {
        mi.hasAudio = true;
        mi.duration = 30;
        mi.sampleRate = 48000;
        mi.channels = 2;
    }
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t at, int frames, int, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) l[k] = r[k] = 0.2f * (float)std::sin(0.05 * (double)(at + k));
        return true;
    }
};

AudioPluginDesc findDesc(const std::string& name)
{
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.name == name) return e.desc;
    return AudioPluginDesc();
}

FxSlot slotFor(Project& p, const AudioPluginDesc& d)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.audio = d;
    s.name = d.name;
    return s;
}

Project baseProject()
{
    Project p;
    p.settings.sampleRate = 48000;
    MediaItem m;
    m.id = p.newId();
    m.path = "tone";
    m.info = Tone().mi;
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 30;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

// What AppState does: the live project, a snapshot of it for the threads that render.
struct Live {
    std::mutex m;
    std::shared_ptr<const Project> snap;
    void set(const Project& p)
    {
        std::lock_guard<std::mutex> lk(m);
        snap = std::make_shared<const Project>(p);
    }
    std::shared_ptr<const Project> get()
    {
        std::lock_guard<std::mutex> lk(m);
        return snap;
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: masterfx_engine_test strictvst.so strictvst6.so strictvstu.so\n"); return 2; }
    Strict stereo, surround, untidy;
    check(stereo.open(argv[1]) && surround.open(argv[2]) && untidy.open(argv[3]), "the strict test plug-ins load");
    Catalog::get().scanInProcess(std::vector<std::string>(), {argv[1], argv[2], argv[3]});
    const AudioPluginDesc dStereo = findDesc("Strict Stereo"), dSurround = findDesc("Strict Surround"), dUntidy = findDesc("Strict Untidy");
    check(!dStereo.path.empty() && !dSurround.path.empty() && !dUntidy.path.empty(), "the catalog has them");

    // ---- a plug-in that asks the host things before the host has its effect, its reserved fields rubbish
    {
        std::string err;
        std::unique_ptr<AudioPlugin> pl = createAudioPlugin(dUntidy, &err);
        check(pl != nullptr, "an untidy plug-in is made (the host does not read its rubbish as a pointer) " + err);
        check(untidy.rate() > 1000, strfmt("...and was told a sample rate while being made (%d)", untidy.rate()));
        check(std::string(untidy.directory()) == pathDir(argv[3]), "...and its folder (audioMasterGetDirectory): " + std::string(untidy.directory()));
        if (pl) {
            check(pl->activate(44100, FxRuntime::kAudioBlock), "...and starts");
            std::vector<float> l(4096, 0.1f), r(4096, 0.1f);
            pl->process(l.data(), r.data(), 4096, 0, true);
            check(untidy.bad() == 0, "...and is used by the rules: " + untidy.why());
        }
    }

    // ---- the master's effects while playback renders all the time and a render (Render As) runs too
    MediaPool pool;
    pool.setOpener([](const std::string&, std::string*) { return std::unique_ptr<MediaReader>(new Tone()); });
    FxRuntime fx;
    Project p = baseProject();
    Live live;
    fx.prepare(p);
    live.set(p);
    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    auto player = [&](bool six) {
        AudioEngine audio(pool, fx);
        std::vector<float> ch[6];
        for (auto& c : ch) c.assign(1024, 0.f);
        float* io[6];
        for (int c = 0; c < 6; ++c) io[c] = ch[c].data();
        int64_t at = 0;
        while (!stop) {
            std::shared_ptr<const Project> s = live.get();
            if (six && s->settings.surround()) audio.renderChannels(*s, at, 1024, io, true);
            else audio.render(*s, at, 1024, io[0], io[1], true);
            at += 1024;
            ++blocks;
        }
    };
    std::thread playback(player, true), render(player, false);

    int changes = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    int step = 0;
    while (std::chrono::steady_clock::now() < until) {
        ++step;
        switch (step % 6) {
        case 0:     // a VST2 picked in the chooser for the empty master, OK
            p.audioOut.clear();
            p.audioOut.push_back(slotFor(p, dStereo));
            break;
        case 1:     // another
            p.audioOut.push_back(slotFor(p, step % 12 == 1 ? dSurround : dStereo));
            break;
        case 2:     // the project's sample rate (the plug-ins are started again at it)
            p.settings.sampleRate = p.settings.sampleRate == 48000 ? 44100 : 48000;
            break;
        case 3:     // 5.1 and back (six channels asked of each; pairs for the ones that cannot)
            p.settings.audioChannels = p.settings.audioChannels == 6 ? 2 : 6;
            break;
        case 4:     // their settings saved (undo, save) and given back
            fx.captureStates(p);
            for (FxSlot& s : p.audioOut)
                if (AudioPlugin* pl = fx.audio(s.id))
                    if (s.state.isString()) pl->loadState(s.state.asString());
            break;
        case 5: {   // the master's chain emptied (undo), its instances let go
            p.audioOut.clear();
            std::set<uint64_t> keep;
            p.forEachFx([&](FxSlot& s) { keep.insert(s.id); });
            fx.releaseExcept(keep);
            break;
        }
        }
        fx.prepare(p);
        live.set(p);
        ++changes;
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    stop = true;
    playback.join();
    render.join();
    check(blocks > 100 && stereo.processed() > 0, strfmt("sound went through the master's plug-ins (%d blocks, %d changes)", blocks.load(), changes));
    check(stereo.bad() == 0, "stereo master effects: the host kept VST's rules - " + stereo.why());
    check(surround.bad() == 0, "5.1 master effects: the host kept VST's rules - " + surround.why());
    fx.clear();
    check(stereo.bad() == 0 && surround.bad() == 0, "...and when they were closed");
    std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
