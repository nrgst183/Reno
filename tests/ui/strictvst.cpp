// A VST2 effect that checks the host keeps VST's rules, for masterfx_test. It counts every time the host:
//   - processes sound while the plug-in is switched off (before effMainsChanged(1), after effMainsChanged(0)),
//   - processes sound while another thread is switching it on or off, setting its sample rate, block size, speaker arrangement
//     or settings (a chunk), choosing a program, or closing it,
//   - hands it more samples at once than the block size it was given, or a null buffer,
//   - uses it after effClose,
//   - calls its window or switches it on or off from a thread other than the one that made it (the UI thread; on Windows a
//     plug-in's window belongs to the thread that made it).
// Real plug-ins crash when these happen (mostly on Windows); this one only counts, so the test can say what the host did. The steps
// that change things take a little while (a short sleep) so a host that lets sound through at the same time is caught. Each
// instance has its own state.
//
// Built three times by the tests' build lines: as it is (stereo: it refuses VST's 5.1 speaker arrangement), with
// -DSTRICTVST_SURROUND (it takes six channels in and out when asked for 5.1, as surround plug-ins do), and with -DSTRICTVST_UNTIDY
// (its effect structure is not cleared before it is filled in, so the host's reserved fields hold garbage as in plug-ins that
// don't use Steinberg's SDK, and it asks the host for the sample rate, the time and its folder while being made, before the host
// has its effect):
//   g++ -shared -fPIC -std=c++17 -D__cdecl= -I third_party/vst2 tests/ui/strictvst.cpp -o strictvst.so
#include "vst.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14, effEditClose = 15,
    effEditIdle = 19, effGetChunk = 23, effSetChunk = 24, effSetSpeakerArrangement = 42, effGetEffectName = 45,
    effGetVendorString = 47, effGetProductString = 48, effStartProcess = 71, effStopProcess = 72
};
enum { amVersion = 1, amGetTime = 7, amIOChanged = 13, amGetSampleRate = 16, amGetBlockSize = 17, amGetDirectory = 41 };

std::atomic<int> gViolations{0};
std::atomic<int> gProcessed{0};        // blocks processed, all instances
std::atomic<int> gLive{0};             // instances made and not closed
std::mutex gTextMutex;
std::string gLast;
std::string gDirectory;                // what the host said its folder is (STRICTVST_UNTIDY)
std::atomic<int> gRate{0};             // ...and the sample rate, asked while being made

void violation(const char* what)
{
    ++gViolations;
    std::lock_guard<std::mutex> lk(gTextMutex);
    gLast = what;
    std::fprintf(stderr, "strictvst: host broke a rule: %s\n", what);
}

void pause(int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

struct Strict {
    vst_effect_t fx;
    vst_host_callback_t host = nullptr;
    std::thread::id owner;             // the thread that made it
    std::atomic<bool> on{false};       // between effMainsChanged(1) and (0)
    std::atomic<bool> closed{false};
    std::atomic<int> changing{0};      // a call that changes how it processes is going on
    std::atomic<int> processing{0};    // process() is going on
    int block = 0;
    float params[2] = {1.0f, 0.0f};
    vst_rect_t rect = {0, 0, 120, 200};
    std::string chunk = "strict";
    int chans = 2;

    void mustOwn(const char* what)
    {
        if (std::this_thread::get_id() != owner) violation(what);
    }
};

Strict* self(vst_effect_t* e) { return e ? (Strict*)e->effect_internal : nullptr; }  // the plug-in's own pointer

// A call that changes how the plug-in processes: no sound may go through while it runs.
struct Changing {
    Strict* s;
    Changing(Strict* st, const char* what) : s(st)
    {
        s->changing.fetch_add(1);
        if (s->processing.load()) violation(what);
        pause(1500);
    }
    ~Changing()
    {
        if (s->processing.load()) violation("sound went through while the plug-in was being changed");
        s->changing.fetch_sub(1);
    }
};

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt)
{
    Strict* s = self(e);
    if (!s) return 0;
    if (s->closed.load() && op != effClose) violation("used after effClose");
    char* text = (char*)ptr;
    switch (op) {
    case effOpen: return 0;
    case effClose: {
        s->mustOwn("closed from another thread");
        Changing c(s, "closed while processing");
        if (s->on.load()) violation("closed while switched on (no effMainsChanged(0) first)");
        s->closed = true;
        --gLive;
        return 0;
    }
    case effSetProgram: { Changing c(s, "program changed while processing"); return 0; }
    case effGetParamName: if (text && index >= 0 && index < 2) std::snprintf(text, 32, "%s", index == 0 ? "Gain" : "Tilt"); return 0;
    case effGetParamLabel: if (text) text[0] = 0; return 0;
    case effGetParamDisplay: if (text && index >= 0 && index < 2) std::snprintf(text, 32, "%.2f", s->params[index]); return 0;
    case effSetSampleRate: {
        s->mustOwn("sample rate set from another thread");
        if (s->on.load()) violation("sample rate set while switched on");
        Changing c(s, "sample rate set while processing");
        (void)opt;
        return 0;
    }
    case effSetBlockSize: {
        s->mustOwn("block size set from another thread");
        if (s->on.load()) violation("block size set while switched on");
        Changing c(s, "block size set while processing");
        s->block = (int)value;
        return 0;
    }
    case effMainsChanged: {
        s->mustOwn("switched on or off from another thread");
        Changing c(s, "switched on or off while processing");
        s->on = value != 0;
        return 0;
    }
    case effStartProcess: case effStopProcess: return 0;
    case effSetSpeakerArrangement: {
        s->mustOwn("speaker arrangement set from another thread");
        if (s->on.load()) violation("speaker arrangement set while switched on");
        Changing c(s, "speaker arrangement set while processing");
        const vst_speaker_arrangement_t* in = (const vst_speaker_arrangement_t*)value;
        const vst_speaker_arrangement_t* out = (const vst_speaker_arrangement_t*)ptr;
        if (!in || !out) { violation("a speaker arrangement without both sides"); return 0; }
#ifdef STRICTVST_SURROUND
        const int n = (in->channels == 6 && out->channels == 6) ? 6 : 2;
        if (n != s->chans) {
            s->chans = n;
            s->fx.num_inputs = s->fx.num_outputs = n;
            if (s->host) s->host(&s->fx, amIOChanged, 0, 0, nullptr, 0.f);
        }
        return 1;
#else
        return (in->channels == 2 && out->channels == 2) ? 1 : 0;
#endif
    }
    case effEditGetRect: if (ptr) *(vst_rect_t**)ptr = &s->rect; return 1;
    case effEditOpen: s->mustOwn("window opened from another thread"); return 1;
    case effEditClose: s->mustOwn("window closed from another thread"); return 1;
    case effEditIdle: s->mustOwn("window idled from another thread"); return 1;
    case effGetChunk: if (ptr) *(void**)ptr = (void*)s->chunk.data(); return (intptr_t)s->chunk.size();
    case effSetChunk: {
        Changing c(s, "settings (a chunk) loaded while processing");
        if (ptr && value > 0) s->chunk.assign((const char*)ptr, (size_t)value);
        return 1;
    }
#if defined(STRICTVST_UNTIDY)
    case effGetEffectName: if (text) std::snprintf(text, 64, "Strict Untidy"); return 1;
    case effGetProductString: if (text) std::snprintf(text, 64, "Strict Untidy"); return 1;
#elif defined(STRICTVST_SURROUND)
    case effGetEffectName: if (text) std::snprintf(text, 64, "Strict Surround"); return 1;
    case effGetProductString: if (text) std::snprintf(text, 64, "Strict Surround"); return 1;
#else
    case effGetEffectName: if (text) std::snprintf(text, 64, "Strict Stereo"); return 1;
    case effGetProductString: if (text) std::snprintf(text, 64, "Strict Stereo"); return 1;
#endif
    case effGetVendorString: if (text) std::snprintf(text, 64, "Reno Tests"); return 1;
    default: return 0;
    }
}

void VST_FUNCTION_INTERFACE process(vst_effect_t* e, const float* const* in, float** out, int32_t n)
{
    Strict* s = self(e);
    if (!s) return;
    s->processing.fetch_add(1);
    if (s->closed.load()) violation("processed after effClose");
    if (!s->on.load()) violation("processed while switched off");
    if (s->changing.load()) violation("processed while another thread was changing the plug-in");
    if (s->processing.load() > 1) violation("processed on two threads at once");
    if (n < 0 || (s->block > 0 && n > s->block)) violation("handed more samples than the block size");
    const int nIn = s->fx.num_inputs, nOut = s->fx.num_outputs;
    bool ok = in && out;
    for (int c = 0; ok && c < nIn; ++c) ok = in[c] != nullptr;
    for (int c = 0; ok && c < nOut; ++c) ok = out[c] != nullptr;
    if (!ok) violation("a null buffer");
    else
        for (int c = 0; c < nOut; ++c)
            for (int k = 0; k < n; ++k) out[c][k] = (c < nIn ? in[c][k] : 0.f) * s->params[0];
    pause(200);
    ++gProcessed;
    s->processing.fetch_sub(1);
}

void VST_FUNCTION_INTERFACE setParam(vst_effect_t* e, uint32_t i, float v)
{
    if (Strict* s = self(e))
        if (i < 2) s->params[i] = v;
}

float VST_FUNCTION_INTERFACE getParam(vst_effect_t* e, uint32_t i)
{
    Strict* s = self(e);
    return (s && i < 2) ? s->params[i] : 0.f;
}

}  // namespace

extern "C" {

#if defined(_WIN32)
#define SVST_EXPORT __declspec(dllexport)
#else
#define SVST_EXPORT __attribute__((visibility("default")))
#endif

SVST_EXPORT vst_effect_t* VSTPluginMain(vst_host_callback_t host)
{
    Strict* s = new Strict();  // never freed: a host may still hold the pointer after effClose
    s->host = host;
    s->owner = std::this_thread::get_id();
#ifdef STRICTVST_UNTIDY
    std::memset(&s->fx, 0xA5, sizeof(s->fx));  // garbage everywhere the plug-in doesn't fill in
#else
    std::memset(&s->fx, 0, sizeof(s->fx));
#endif
    s->fx.magic_number = (int32_t)VST_MAGICNUMBER;
    s->fx.control = control;
    s->fx.process = process;
    s->fx.process_float = process;
    s->fx.set_parameter = setParam;
    s->fx.get_parameter = getParam;
    s->fx.num_programs = 0;
    s->fx.num_params = 2;
    s->fx.delay = 0;
    s->fx.num_inputs = 2;
    s->fx.num_outputs = 2;
    s->fx.flags = 1 | (1 << 4) | (1 << 5);     // has an editor, processes in place, settings as chunks
#if defined(STRICTVST_UNTIDY)
    s->fx.unique_id = 0x53747255;
#elif defined(STRICTVST_SURROUND)
    s->fx.unique_id = 0x53747253;
#else
    s->fx.unique_id = 0x53747232;
#endif
    s->fx.version = 1;
    s->fx.effect_internal = s;
    ++gLive;
#ifdef STRICTVST_UNTIDY
    // asked before the host has the effect: plug-ins do this, with the effect they are about to hand over
    if (host) {
        if (host(&s->fx, amVersion, 0, 0, nullptr, 0.f) <= 0) violation("no VST version");
        gRate = (int)host(&s->fx, amGetSampleRate, 0, 0, nullptr, 0.f);
        host(&s->fx, amGetBlockSize, 0, 0, nullptr, 0.f);
        host(&s->fx, amGetTime, 0, 0, nullptr, 0.f);
        const char* dir = (const char*)host(&s->fx, amGetDirectory, 0, 0, nullptr, 0.f);
        std::lock_guard<std::mutex> lk(gTextMutex);
        gDirectory = dir ? dir : "";
    }
#endif
    return &s->fx;
}

SVST_EXPORT int strictvst_violations() { return gViolations.load(); }
SVST_EXPORT int strictvst_processed() { return gProcessed.load(); }
SVST_EXPORT int strictvst_live() { return gLive.load(); }
SVST_EXPORT int strictvst_rate() { return gRate.load(); }
SVST_EXPORT const char* strictvst_directory()
{
    static std::string copy;
    std::lock_guard<std::mutex> lk(gTextMutex);
    copy = gDirectory;
    return copy.c_str();
}
SVST_EXPORT const char* strictvst_last()
{
    static std::string copy;
    std::lock_guard<std::mutex> lk(gTextMutex);
    copy = gLast;
    return copy.c_str();
}

}  // extern "C"
