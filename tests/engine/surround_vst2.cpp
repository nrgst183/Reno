// A tiny VST2 effect for multichannel_test: a gain with six inputs and outputs that takes VST's 5.1 speaker arrangement
// (or plain stereo with RENO_TESTVST_CHANNELS=2). In 5.1 the LFE also gets a quarter of the front pair, which only
// a plug-in that sees all six channels at once can do.
// Built as a shared library:
//   g++ -shared -fPIC -std=c++17 -D__cdecl= -I third_party/vst2 tests/engine/surround_vst2.cpp -o surroundvst2.so
#include "vst.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum { effOpen = 0, effClose = 1, effGetParamName = 8, effMainsChanged = 12, effSetSpeakerArrangement = 42, effGetEffectName = 45,
       effGetVendorString = 47, effGetSpeakerArrangement = 69 };

struct Plug {
    vst_effect_t fx;                // first: the host's pointer is this struct's address
    float gain = 1.0f;              // the one parameter (0..1 is 0..2)
    int arrangements = 0;           // 5.1 arrangements it was given
    int sixBlocks = 0;              // blocks processed with six channels
    bool surround = false;          // the host asked for 5.1
};
int gLastArrangements = 0, gLastSixBlocks = 0, gInstances = 0;

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t* fx, int32_t op, int32_t, intptr_t value, void* ptr, float)
{
    Plug* p = (Plug*)fx;
    char* s = (char*)ptr;
    switch (op) {
    case effOpen: return 0;
    case effClose: --gInstances; delete p; return 0;
    case effGetParamName: if (s) std::snprintf(s, 32, "Gain"); return 0;
    case effGetEffectName: if (s) std::snprintf(s, 64, fx->num_inputs == 6 ? "Reno Surround VST2" : "Reno Stereo VST2"); return 1;
    case effGetVendorString: if (s) std::snprintf(s, 64, "Reno Tests"); return 1;
    case effSetSpeakerArrangement: {
        const vst_speaker_arrangement_t* in = (const vst_speaker_arrangement_t*)value;
        const vst_speaker_arrangement_t* out = (const vst_speaker_arrangement_t*)ptr;
        if (!in || !out || fx->num_inputs != 6) return 0;
        p->surround = in->channels == 6 && out->channels == 6 && in->type == VST_SPEAKER_ARRANGEMENT_TYPE_5_1;
        if (p->surround) gLastArrangements = ++p->arrangements;
        return p->surround ? 1 : 0;
    }
    default: return 0;
    }
}

void VST_FUNCTION_INTERFACE process(vst_effect_t* fx, const float* const* in, float** out, int32_t n)
{
    Plug* p = (Plug*)fx;
    const float g = p->gain * 2.0f;
    for (int c = 0; c < fx->num_outputs; ++c)
        for (int k = 0; k < n; ++k) out[c][k] = in[c][k] * g;
    if (fx->num_outputs == 6) {
        for (int k = 0; k < n; ++k) out[3][k] += 0.25f * g * (in[0][k] + in[1][k]);
        gLastSixBlocks = ++p->sixBlocks;
    }
}

void VST_FUNCTION_INTERFACE setParam(vst_effect_t* fx, uint32_t i, float v)
{
    if (i == 0) ((Plug*)fx)->gain = v;
}
float VST_FUNCTION_INTERFACE getParam(vst_effect_t* fx, uint32_t i) { return i == 0 ? ((Plug*)fx)->gain : 0.f; }

}  // namespace

extern "C" {

#if defined(_WIN32)
#define SVT_EXPORT __declspec(dllexport)
#else
#define SVT_EXPORT __attribute__((visibility("default")))
#endif

SVT_EXPORT vst_effect_t* VSTPluginMain(vst_host_callback_t)
{
    const char* e = std::getenv("RENO_TESTVST_CHANNELS");
    const int ch = (e && std::atoi(e) == 2) ? 2 : 6;
    Plug* p = new Plug();
    std::memset(&p->fx, 0, sizeof(p->fx));
    p->fx.magic_number = (int32_t)VST_MAGICNUMBER;
    p->fx.control = control;
    p->fx.process = process;
    p->fx.process_float = process;
    p->fx.set_parameter = setParam;
    p->fx.get_parameter = getParam;
    p->fx.num_params = 1;
    p->fx.num_inputs = ch;
    p->fx.num_outputs = ch;
    p->fx.flags = 1 << 4;               // processes in place (replacing)
    p->fx.unique_id = ch == 6 ? 0x52353156 : 0x52535456;
    p->fx.version = 1;
    p->gain = 0.5f;
    ++gInstances;
    return &p->fx;
}

// ---- what the test reads
SVT_EXPORT int surroundvst2_arrangements() { return gLastArrangements; }
SVT_EXPORT int surroundvst2_six_blocks() { return gLastSixBlocks; }
SVT_EXPORT int surroundvst2_instances() { return gInstances; }

}  // extern "C"
