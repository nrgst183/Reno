// A VST2 effect for testing the 32-bit bridge (built for Linux, 64-bit: tests/vst32/run.sh). Besides a sound it has
// everything the bridge carries: settings, programs, a state chunk, an editor that asks for another size and reports a
// change of a setting, a latency - and two settings that make it misbehave on purpose:
//   setting 3 ("Crash")  at 1.0 while processing: the process dies of an access violation
//   setting 4 ("Hang")   at 1.0 while processing: never returns
// Both only once if BRIDGEVST_CRASHFILE / BRIDGEVST_HANGFILE name a file: they act when they can delete it (so a process
// started again after the failure behaves).
//   g++ -shared -fPIC -std=c++17 -D__cdecl= -I third_party/vst2 tests/vst32/bridgevst.cpp -o bridgevst.so
#include "vst.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <unistd.h>

namespace {

enum { effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4, effGetProgramName = 5,
       effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11,
       effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19, effGetChunk = 23,
       effSetChunk = 24, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48, effGetProgramNameIndexed = 29 };
enum { amAutomate = 0, amSizeWindow = 15 };

const int kParams = 5, kPrograms = 4;

struct Test {
    vst_effect_t fx;
    vst_host_callback_t host = nullptr;
    std::atomic<float> param[kParams];
    float programs[kPrograms][kParams];
    char names[kPrograms][32];
    int program = 0;
    double sampleRate = 44100.0;
    int blockSize = 512;
    bool on = false;
    float lp[2] = {0.f, 0.f};                   // the sound's memory: a one-pole low-pass, so that blocks must follow in order
    vst_rect_t rect{0, 0, 100, 200};            // top, left, bottom, right
    bool editorOpen = false;
    int idles = 0;
    std::vector<char> chunk;
    static std::atomic<long> parentSeen;        // the "window" the host gave (for the test to ask the plug-in's side what it saw)
};
std::atomic<long> Test::parentSeen{0};

bool armed(const char* env)
{
    const char* path = std::getenv(env);
    return !path || unlink(path) == 0;
}

void setText(void* ptr, const std::string& s) { if (ptr) { std::snprintf((char*)ptr, 64, "%s", s.c_str()); } }

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt)
{
    Test* t = (Test*)e->effect_internal;
    switch (op) {
    case effSetProgram:
        if (value >= 0 && value < kPrograms) {
            t->program = (int)value;
            for (int i = 0; i < kParams; ++i) t->param[i] = t->programs[t->program][i];
        }
        return 0;
    case effGetProgram: return t->program;
    case effSetProgramName: std::snprintf(t->names[t->program], 32, "%s", (const char*)ptr); return 0;
    case effGetProgramName: setText(ptr, t->names[t->program]); return 1;
    case effGetProgramNameIndexed: if (index < 0 || index >= kPrograms) return 0; setText(ptr, t->names[index]); return 1;
    case effGetParamName: { static const char* n[kParams] = {"Gain", "Smooth", "Mix", "Crash", "Hang"}; setText(ptr, n[index % kParams]); return 1; }
    case effGetParamDisplay: setText(ptr, std::to_string((int)(t->param[index % kParams] * 100.f))); return 1;
    case effGetParamLabel: setText(ptr, "%"); return 1;
    case effSetSampleRate: t->sampleRate = opt; return 0;
    case effSetBlockSize: t->blockSize = (int)value; return 0;
    case effMainsChanged: t->on = value != 0; if (value) t->lp[0] = t->lp[1] = 0.f; return 0;
    case effEditGetRect: *(vst_rect_t**)ptr = &t->rect; return 1;
    case effEditOpen:
        t->editorOpen = true;
        t->idles = 0;
        Test::parentSeen = (long)(intptr_t)ptr;
        // the window wants to be bigger than it first said
        t->rect = vst_rect_t{0, 0, 240, 320};
        if (t->host) t->host(e, amSizeWindow, 320, 240, nullptr, 0.f);
        return 1;
    case effEditClose: t->editorOpen = false; return 0;
    case effEditIdle:
        // the third idle call: "the user moved the Smooth knob in the window"
        if (t->editorOpen && ++t->idles == 3) {
            t->param[1] = 0.77f;
            if (t->host) t->host(e, amAutomate, 1, 0, nullptr, 0.77f);
        }
        return 0;
    case effGetChunk: {
        // a header, the current program's settings
        std::string s = "BRIDGECHUNK";
        s.append((const char*)&t->program, 4);
        for (int i = 0; i < kParams; ++i) { float v = t->param[i]; s.append((const char*)&v, 4); }
        t->chunk.assign(s.begin(), s.end());
        *(void**)ptr = t->chunk.data();
        return (intptr_t)t->chunk.size();
    }
    case effSetChunk: {
        const size_t need = 11 + 4 + 4 * kParams;
        if ((size_t)value < need || std::memcmp(ptr, "BRIDGECHUNK", 11) != 0) return 0;
        const char* p = (const char*)ptr + 11;
        std::memcpy(&t->program, p, 4);
        p += 4;
        for (int i = 0; i < kParams; ++i) { float v; std::memcpy(&v, p, 4); p += 4; t->param[i] = v; }
        return 1;
    }
    case effGetEffectName: case effGetProductString: setText(ptr, "Bridge Test"); return 1;
    case effGetVendorString: setText(ptr, "Reno Tests"); return 1;
    default: return 0;
    }
}

void VST_FUNCTION_INTERFACE process(vst_effect_t* e, const float* const* in, float** out, int32_t n)
{
    Test* t = (Test*)e->effect_internal;
    if (t->param[3] >= 0.99f && armed("BRIDGEVST_CRASHFILE")) { volatile int* p = nullptr; *p = 1; }
    if (t->param[4] >= 0.99f && armed("BRIDGEVST_HANGFILE")) for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    const float gain = t->param[0] * 2.f, a = 0.05f + 0.9f * (1.f - t->param[1]), mix = t->param[2];
    for (int c = 0; c < 2; ++c) {
        const float* i = in[c];
        float* o = out[c];
        for (int k = 0; k < n; ++k) {
            t->lp[c] += a * (i[k] * gain - t->lp[c]);
            o[k] = i[k] * (1.f - mix) + t->lp[c] * mix;
        }
    }
}

void VST_FUNCTION_INTERFACE setParam(vst_effect_t* e, uint32_t i, float v) { if (i < kParams) ((Test*)e->effect_internal)->param[i] = v; }
float VST_FUNCTION_INTERFACE getParam(vst_effect_t* e, uint32_t i) { return i < kParams ? ((Test*)e->effect_internal)->param[i].load() : 0.f; }

}  // namespace

extern "C" __attribute__((visibility("default"))) vst_effect_t* VSTPluginMain(vst_host_callback_t host)
{
    Test* t = new Test();       // (never freed: a host may still hold the pointer after effClose)
    std::memset(&t->fx, 0, sizeof(t->fx));
    t->host = host;
    for (int p = 0; p < kPrograms; ++p) {
        std::snprintf(t->names[p], 32, "Program %d", p + 1);
        const float base[kParams] = {0.5f, 0.3f + 0.1f * p, 1.0f, 0.f, 0.f};
        for (int i = 0; i < kParams; ++i) t->programs[p][i] = base[i];
    }
    for (int i = 0; i < kParams; ++i) t->param[i] = t->programs[0][i];
    t->fx.magic_number = (int32_t)VST_MAGICNUMBER;
    t->fx.control = control;
    t->fx.process = process;
    t->fx.process_float = process;
    t->fx.set_parameter = setParam;
    t->fx.get_parameter = getParam;
    t->fx.num_params = kParams;
    t->fx.num_programs = kPrograms;
    t->fx.num_inputs = 2;
    t->fx.num_outputs = 2;
    t->fx.delay = 64;
    t->fx.flags = (1 << 0) | (1 << 4) | (1 << 5);       // has an editor, processes in place, has chunks
    t->fx.unique_id = 0x42524447;                       // 'BRDG'
    t->fx.version = 1;
    t->fx.effect_internal = t;
    return &t->fx;
}
